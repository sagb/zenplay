#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <sys/time.h>
#include <unistd.h>

// https://github.com/redis/hiredis
// https://redis.io/commands
#include <hiredis.h>

// https://raw.githubusercontent.com/mpv-player/mpv-examples/master/libmpv/simple/simple.c
#include <mpv/client.h>

// https://github.com/starnight/libgpiod-example
#include <gpiod.h>

#include "settings.h"

typedef struct {
    struct gpiod_chip* chip;
    struct gpiod_line* button[N_GENRES];
    struct gpiod_line* led[N_GENRES];
    struct timeval last_press[N_GENRES];
} gpio_t;
#define GPIO_CONSUMER   "zenplay"

// global scalars: the only "state" besides redis.
// use only functions below to change them.

unsigned int total_listen_duration[N_GENRES];
unsigned int total_listen_duration_at_program_start[N_GENRES];
unsigned int total_unlisten_duration[N_GENRES]; // synthetic
unsigned int total_duration[N_GENRES];
unsigned int total_listen_count[N_GENRES];
unsigned int total_unlisten_count[N_GENRES];
unsigned int all_genres_listen_duration;
unsigned int all_genres_listen_count;
unsigned int all_genres_average_duration;

// functions to maintain coordinated values of all vars above

void set_all_genres_average_duration (unsigned int d)
// important intermediate value
{
    int n;
    printf ("all_genres_average_duration: %u\n", d);
    all_genres_average_duration = d;
    for (n=0; n<N_GENRES; n++) {
        total_unlisten_duration[n] = d * total_unlisten_count[n];
        total_duration[n] =
            total_unlisten_duration[n] + total_listen_duration[n];
    }
}

void set_total_listen_duration (int genre, unsigned int d)
{
    int n;
    printf ("total_listen_duration[%s]: %u\n", genre_str[genre], d);
    total_listen_duration[genre] = d;
    total_duration[genre] = d + total_unlisten_duration[genre];
    all_genres_listen_duration = 0; for (n=0; n<N_GENRES; n++)
        all_genres_listen_duration += total_listen_duration[n];
    if (all_genres_listen_count != 0)
        set_all_genres_average_duration (
            all_genres_listen_duration / all_genres_listen_count);
}

void set_total_listen_duration_at_program_start (int genre, unsigned int d)
{
    printf ("total_listen_duration_at_program_start[%s]: %u\n",
            genre_str[genre], d);
    total_listen_duration_at_program_start[genre] = d;
}

void set_total_unlisten_count (int genre, unsigned int c)
{
    printf ("total_unlisten_count[%s]: %u\n", genre_str[genre], c);
    total_unlisten_count[genre] = c;
    total_unlisten_duration[genre] = c * all_genres_average_duration;
    total_duration[genre] = 
        total_listen_duration[genre] + total_unlisten_duration[genre];
}

void decrement_total_unlisten_count (int genre, unsigned int c)
{
    set_total_unlisten_count (genre, total_unlisten_count[genre] - c);
}

void set_total_listen_count (int genre, unsigned int c)
{
    int n;
    printf ("total_listen_count[%s]: %u\n", genre_str[genre], c);
    total_listen_count[genre] = c;
    all_genres_listen_count = 0; for (n=0; n<N_GENRES; n++)
        all_genres_listen_count += total_listen_count[n];
    if (all_genres_listen_count != 0)
        set_all_genres_average_duration (
            all_genres_listen_duration / all_genres_listen_count);
}


// 1. for invalid redis context which we generally don't know how to fix
// 2. (probably wrong) for any inconsistency in redis db
void die (const char* format, ...)
{
    va_list argptr;
    va_start(argptr, format);
    vfprintf(stderr, format, argptr);
    va_end(argptr);
    exit (1);
}


redisContext* initRedis()
{
    redisContext *c;

    struct timeval timeout = { 1, 500000 }; // 1.5 seconds
    c = redisConnectWithTimeout (redis_hostname, redis_port, timeout);
    if (c == NULL || c->err) {
        if (c) {
            die ("init: %s\n", c->errstr);
        } else {
            die ("can't allocate redis context\n");
        }
    }
    return c;
}   // initRedis


// convert redis stream timestamp: <millisecondsTime>-<sequenceNumber>
// to milliseconds
unsigned int tsToMS (char* ts)
{
    char ms[16];
    char* minus;
    unsigned int ms_size;

    minus = strrchr (ts, '-');
    if (minus == NULL)
        return 0;
    ms_size = minus - ts;
    if (ms_size > 15)
        return 0;
    strncpy (ms, ts, ms_size);
    ms[ms_size] = '\0';
    return atoi(ms);
}


int loadScalarsFromDB (redisContext *c, int genre)
{
    redisReply *reply, *reply2;
    unsigned int top;

    reply = redisCommand (c,"XREVRANGE listen:%s + - COUNT 1",
            genre_str[genre]);
    if (!reply)
        die ("last listen: %s\n", c->errstr);
    if (! (reply->type == REDIS_REPLY_ARRAY
            && reply->elements > 0
            && reply->element[0]->type == REDIS_REPLY_ARRAY
            && reply->element[0]->elements > 0
            && reply->element[0]->element[0]->type == REDIS_REPLY_STRING
       )) {
        // init listen:genre log for first time
        reply2 = redisCommand (c,"XADD listen:%s 0-01 song 0",
                genre_str[genre]);
        if (!reply2)
            die ("can't init listen:%s\n", genre_str[genre]);
        top = 0;
        freeReplyObject(reply2);
    } else {
        top = tsToMS (reply->element[0]->element[0]->str);
    }
    freeReplyObject(reply);
    set_total_listen_duration (genre, top);

    reply = redisCommand (c,"XLEN listen:%s", genre_str[genre]);
    if (!reply)
        die ("listen count: %s\n", c->errstr);
    set_total_listen_count (genre, reply->integer - 1);  // 1 is for "0-01"
    freeReplyObject(reply);

    reply = redisCommand (c,"LLEN unlisten:%s", genre_str[genre]);
    if (!reply)
        die ("unlisten count: %s\n", c->errstr);
    set_total_unlisten_count (genre, reply->integer);
    freeReplyObject(reply);

    return 1;
}


int recordListenedSongToDB (redisContext *c, int genre,
        char* songhash, unsigned int duration, bool already_listened)
{
    redisReply *reply;
    unsigned int newtop;

    newtop = total_listen_duration[genre] + duration;
    // keep local vars in sync with redis
    set_total_listen_duration (genre, newtop);

    printf ("incr listen:%s ms to %u, log %s\n",
            genre_str[genre], newtop, songhash);
    reply = redisCommand (c,"XADD listen:%s %u song %s",
            genre_str[genre], newtop, songhash);
    if (!reply)
        die ("record listened: %s\n", c->errstr);
    if (! (reply->type == REDIS_REPLY_STRING))
        die ("record listened: wrong type: %u\n", reply->type);
    if (tsToMS (reply->str) != newtop)
        die ("record listened: unexpected id\n");
    freeReplyObject(reply);

    if (! already_listened) {
        printf ("remove %s from unlisten:%s\n", songhash, genre_str[genre]);
        reply = redisCommand (c,"LREM unlisten:%s 0 %s",
                genre_str[genre], songhash);
        if (!reply)
            die ("remove unlisten: %s\n", c->errstr);
        freeReplyObject(reply);
        // keep local vars in sync with redis
        decrement_total_unlisten_count (genre, 1);
    }

    return 1;
} // recordListenSongToDB


bool recentlyListened (redisContext *c, int genre, char* song)
{
    redisReply *reply;
    int n, m;
    const char* P = "recentlyListened";

    printf ("%s: testing song %s\n", P, song);
    reply = redisCommand (c,"XRANGE listen:%s %u +",
            genre_str[genre],
            total_listen_duration_at_program_start[genre]);

    if (!reply)
        die ("%s: %s\n", P, c->errstr);
    if (reply->type != REDIS_REPLY_ARRAY)
        die ("%s: top reply is not array\n", P);
    for (n = 0; n < reply->elements; n++) {
        if (reply->element[n]->type != REDIS_REPLY_ARRAY)
            die ("%s: 2nd level is not array\n", P);
        if (reply->element[n]->elements < 2)
            die ("%s: 2nd level has less than 2 elements\n", P);
        if (reply->element[n]->element[1]->type != REDIS_REPLY_ARRAY)
            die ("%s: 3rd level is not array\n", P);
        for (m = 0; m < reply->element[n]->element[1]->elements-1; m++) {
            if (strcmp (reply->element[n]->element[1]->element[m]->str,
                        "song") == 0) {
                // next element is song id
                if (strcmp (song,
                        reply->element[n]->element[1]->element[m+1]->str) == 0) {
                    freeReplyObject(reply);
                    printf ("%s: found at position %d from %d\n",
                            P, n, reply->elements);
                    return true;
                    }
            }
        }
    }
    freeReplyObject(reply);
    printf ("%s: not found\n", P);
    return false;
} // recentlyListened


void chooseFromListened (redisContext *c, int genre, unsigned int r,
        char* song, int songsize)
{
    redisReply *reply;
    int n;
    const char* P = "chooseFromListened";

    reply = redisCommand (c,"XRANGE listen:%s %u + COUNT 1",
            genre_str[genre], r);
    if (!reply)
        die ("%s: %s\n", P, c->errstr);
    if (reply->type != REDIS_REPLY_ARRAY)
        die ("%s: top reply is not array\n", P);
    if (reply->elements < 1)
        die ("%s: less that 1 element in top reply\n", P);
    if (reply->element[0]->type != REDIS_REPLY_ARRAY)
        die ("%s: 2nd level is not array\n", P);
    if (reply->element[0]->elements < 2)
        die ("%s: 2nd level has less than 2 elements\n", P);
    if (reply->element[0]->element[1]->type != REDIS_REPLY_ARRAY)
        die ("%s: 3rd level is not array\n", P);
    for (n = 0; n < reply->element[0]->element[1]->elements-1; n++) {
        if (strcmp (reply->element[0]->element[1]->element[n]->str,
                    "song") == 0) {
            // next element is song id
            strncpy (song,
                    reply->element[0]->element[1]->element[n+1]->str,
                    songsize-1);
            song[songsize-1] = '\0';
            freeReplyObject(reply);
            printf ("%s: dur %u from %u\n",
                    P, r, total_listen_duration[genre]);
            return;
        }
    }
    die ("can't %s\n", P);
} // chooseFromListened


void chooseFromUnlistened (redisContext *c, int genre, unsigned int ru,
        char* song, int songsize)
{
    redisReply *reply;
    unsigned int ri;
    const char* P = "chooseFromUnlistened";

    // duration to count
    ri = ru * total_unlisten_count[genre] / total_unlisten_duration[genre];
    printf ("%s: rec %u from %u\n",
            P, ri, total_unlisten_count[genre]);
    reply = redisCommand (c,"LINDEX unlisten:%s %u",
            genre_str[genre], ri);
    if (!reply)
        die ("%s: %s\n", P, c->errstr);
    //printf ("type %u\n", reply->type);
    strncpy (song, reply->str, songsize-1);
    song[songsize-1] = '\0';
    freeReplyObject(reply);
} // chooseFromUnistened


// returns:
// song, is_already_listened
void chooseNext (redisContext *c, int genre,
        char* song, int songsize, bool* is_already_listened)
{
    const char* P = "chooseNext";
    redisReply *reply;
    unsigned int r; // random pointer to duration
    uint64_t td64, ra64, rm64, r64;
    uint32_t td32, ra32;
    int try;

    // generate random pointer r to timeline
    // while trying 10 times to suppress
    // songs played during current program run
    try = 0;
nextTry:
    // 64-bit proportion
    td32 = total_duration[genre];
    ra32 = (uint32_t)random();
    rm64 = 0x80000000;
    r64 = (uint64_t)td32 * (uint64_t)ra32 / rm64;
    r = r64;
    printf ("%s: select r=%u from total_duration[%s]=%u (%f\%)\n",
            P, r, genre_str[genre], total_duration[genre],
            ((double)r64 / (double)(total_duration[genre]) * (double)100.0));
    // here we have r

    if (r < total_listen_duration[genre]) {
        *is_already_listened = true;
        chooseFromListened (c, genre, r, song, songsize);
        if (recentlyListened (c, genre, song) && try < 10) {
            try++;
            goto nextTry;
        }
    }
    else {
        *is_already_listened = false;
        chooseFromUnlistened (c, genre, r - total_listen_duration[genre],
                song, songsize);
    }
} // chooseNext


unsigned int msTime()
{
    struct timeval tv;
    if (gettimeofday (&tv, NULL) == -1)
        die ("gettimeofday");
    return (tv.tv_sec * 1000 + tv.tv_usec / 1000);
}


// poll buttons.
// return in predictable time 10 ms
// '\0': timeout
// '0', '1' etc: genres starting from 0
// (or whatever entered)

char pollButtonsAndConsumeTime (gpio_t* gpio)
{
    const suseconds_t maxwait = 10000; // 10 ms
    const int ignore_repetitive_press = 1500; // 1.5 s
    fd_set rfds;
    struct timeval tv;
    int ms_since_last_press;
    int n, v;

    gettimeofday (&tv, NULL);
    for (n=0; n<N_GENRES; n++) {
        v = gpiod_line_get_value (gpio->button[n]);
        if (v < 0)
            die ("gpiod_line_get_value failed for button '%s'\n",
                    genre_str[n]);
        else if (v == 1) {
            // button pressed
            if (gpio->last_press[n].tv_sec == 0) {
                // first detection
                gpio->last_press[n].tv_sec = tv.tv_sec;
                gpio->last_press[n].tv_usec = tv.tv_usec;
                // immediate return: no sleep, no other buttons
                return ('0' + n);
            } else {
                // already pressed, maybe ignore?
                ms_since_last_press = 
                    (tv.tv_sec - gpio->last_press[n].tv_sec) * 1000 +
                    (tv.tv_usec - gpio->last_press[n].tv_usec) / 1000;
                if (ms_since_last_press < ignore_repetitive_press)
                    continue;
                else {
                    // setup new grace period and return
                    gpio->last_press[n].tv_sec = tv.tv_sec;
                    gpio->last_press[n].tv_usec = tv.tv_usec;
                    return ('0' + n);
                }
            }
        } else if (v == 0) {
            // discard grace period
            if (gpio->last_press[n].tv_sec != 0)
                gpio->last_press[n].tv_sec = gpio->last_press[n].tv_usec = 0;
        }
    }
    FD_ZERO(&rfds);
    tv.tv_sec = 0;
    tv.tv_usec = maxwait;
    select(0, &rfds, NULL, NULL, &tv); // just sleep for 10 ms
    return '\0';
}  // pollButtonsAndConsumeTime()


char pollButtonsAndConsumeTime_kb()
{
    const suseconds_t maxwait = 10000; // 10 ms
    int retval;

    // surrogate "buttons":
    // enter "0", "1" etc to choose genre,
    // or anything to just change song

    fd_set rfds;
    struct timeval tv;
    char in[80]; int inlen = 80;

    FD_ZERO(&rfds);
    FD_SET(0, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = maxwait;
    retval = select(1, &rfds, NULL, NULL, &tv);
    if (retval == -1)
        die("select()");
    else if (retval) {
        printf("reading \"button\" from stdin\n");
        fgets (in, inlen, stdin);
        return in[0];
    }
    return '\0';
}


// return:
//  'p', 'd' or whavever "button" or '\0' (playback finished) as return value,
//  playback duration on ms at supplied pointer.
char playPath (mpv_handle *m, gpio_t* gpio, char* path, unsigned int* duration)
{
    const char* P = "playPath";
    int mc;
    unsigned int start_ms;
    char btn = '\0';

    const char *cmd[] = {"loadfile", path, NULL};
    mc = mpv_command (m, cmd);
    if (mc < 0)
        die ("%s: loadfile fail\n", P);

    start_ms = msTime();
    // mpv_wait_event() second argument has 1 sec granularity -> useless,
    // so we just poll both mpv and buttons.
    while (1) {
        mpv_event *event = mpv_wait_event (m, 0);
        if (event->event_id == MPV_EVENT_NONE) {
            // time to handle buttons
#ifdef USE_KEYBOARD_INSTEAD_OF_GPIO
            btn = pollButtonsAndConsumeTime_kb();
#else
            btn = pollButtonsAndConsumeTime (gpio);
#endif
            if (btn != '\0') {
                printf ("%s: interrupt with '%c'\n", P, btn);
                break;
            }
        } else {
            printf("%s: event: %s\n", P, mpv_event_name (event->event_id));
            if (event->event_id == MPV_EVENT_SHUTDOWN)
                die ("%s: got MPV_EVENT_SHUTDOWN\n", P);
            if (event->event_id == MPV_EVENT_IDLE)
                break;
        }
    }
    *duration = msTime() - start_ms;
    return btn;
} // playPath()


// play and return same things as playPath
char playSong (redisContext *c, mpv_handle *m, gpio_t* gpio, char* song,
        unsigned int* duration)
{
    const char* P = "playSong";
    char btn;
    char path[4096]; const int pathlen = 4096;
    redisReply *reply;

    reply = redisCommand (c,"HGET hash2path %s", song);
    if (!reply)
        die ("%s: %s\n", P, c->errstr);
    if (reply->type != REDIS_REPLY_STRING || reply->len < 1)
        die ("%s: hash %s isn't converted to path\n", P, song);
    if (reply->len > pathlen-1)
        die ("%s: %s: path bigger than %d\n", P, song, pathlen-1);
    strncpy (path, reply->str, pathlen);
    path[pathlen-1] = '\0';
    printf ("%s: '%s'\n", P, path);
    btn = playPath (m, gpio, path, duration);
    printf ("%s: duration %u\n", P, *duration);
    return btn;
}


mpv_handle* initMpv()
{
    mpv_handle *ctx;
    int mc; //int val;
   
    ctx = mpv_create();
    if (!ctx)
        die ("failed creating mpv context\n");
    mc = mpv_set_option_string(ctx, "video", "no");
    //mpv_set_option_string(ctx, "input-vo-keyboard", "yes");
    //val = 1; mc = mpv_set_option(ctx, "osc", MPV_FORMAT_FLAG, &val);
    mc = mpv_initialize(ctx);
    if (mc < 0)
        die ("mpv_initialize\n");
    return ctx;
}  // initMpv()


void initGpio (gpio_t *gpio)
{
    int n;
    gpio->chip = gpiod_chip_open_by_name (chipname);
    if (!gpio->chip)
        die ("gpiod_chip_open_by_name\n");
    for (n=0; n<N_GENRES; n++) {
        gpio->button[n] = gpiod_chip_get_line (gpio->chip, button_gpio[n]);
        if (! gpio->button[n])
            die ("gpiod_chip_get_line for button '%s'\n", genre_str[n]);
        gpio->led[n] = gpiod_chip_get_line (gpio->chip, led_gpio[n]);
        if (! gpio->led[n])
            die ("gpiod_chip_get_line for led '%s'\n", genre_str[n]);
        if (gpiod_line_request_input (gpio->button[n], GPIO_CONSUMER) < 0)
            die ("gpiod_line_request_input for button '%s'\n", genre_str[n]);
        if (gpiod_line_request_output (gpio->led[n], GPIO_CONSUMER, 0) < 0)
            die ("gpiod_line_request_output for led '%s'\n", genre_str[n]);
    }
}  // initGpio()


void closeGpio (gpio_t *gpio)
{
    int n;
    for (n=0; n<N_GENRES; n++) {
        gpiod_line_release (gpio->button[n]);
        gpiod_line_release (gpio->led[n]);
    }
    gpiod_chip_close (gpio->chip);
}  // closeGpio()


void ledOn (gpio_t* gpio, unsigned int genre)
{
    int n, v;

    for (n=0; n<N_GENRES; n++) {
        v = (n == genre) ? 1 : 0;
        gpiod_line_set_value (gpio->led[n], v);
        //if (ret < 0) {  perror(
    }
}  // ledOn


int main (int argc, char **argv) {

    redisContext *c;
    mpv_handle *m;
    gpio_t gpio;
    char song[41];
    unsigned int listen_duration;
    unsigned int genre;
    bool is_already_listened;
    char btn = '\0';
    unsigned int snum;

    m = initMpv();
#ifndef USE_KEYBOARD_INSTEAD_OF_GPIO
    initGpio (&gpio);
#endif

    if (argc > 1) {
        // special mode: just play given songs
        for (snum = 1; snum < argc; snum++) {
            printf ("playing %s\n", argv[snum]);
            btn = playPath (m, &gpio, argv[snum], &listen_duration);
            printf ("listen_duration=%u\n", listen_duration);
        }
        mpv_terminate_destroy (m);
        return 0;
    }

    srandom (time (NULL));
    if (chdir (recordings_top_dir) == -1)
        die ("can't cd to %s\n", recordings_top_dir);
    c = initRedis();
    for (genre = 0; genre < N_GENRES; genre++) {
        loadScalarsFromDB (c, genre);
        set_total_listen_duration_at_program_start (genre,
                total_listen_duration[genre]);
    }
    if (all_genres_average_duration == 0)
        // very first play, just initialized DB
        set_all_genres_average_duration (15000);
    genre = DEFAULT_GENRE;

    while(1) {
        if (btn >= '0' && btn < '0' + N_GENRES)
            genre = btn - '0';
        // or previous genre by default
        chooseNext (c, genre,
               song, sizeof(song), &is_already_listened);
        ledOn (&gpio, genre);
        btn = playSong (c, m, &gpio, song, &listen_duration);
        recordListenedSongToDB (c, genre, song,
               listen_duration, is_already_listened);
        printf ("\n");
    }

    closeGpio (&gpio);
    mpv_terminate_destroy (m);
    redisFree (c);
    return 0;
}
