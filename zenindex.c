//
// Add specified (or default top) directories to song database.
// May be run any times.
// Currently it only adds songs, but doesn't remove.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <fcntl.h>
#include <stdbool.h>
#include <unistd.h>

#include <rhash.h>  // librhash-dev
#include <fts.h>
//#include <sys/types.h>
#include <sys/stat.h>
//#include <unistd.h>
#include <hiredis.h>

#include "settings.h"


void numToHex (unsigned char* src, int len, char* dst)
{
  static const char table[] = "0123456789abcdef";
  unsigned char* s;
  unsigned char* end = src + len;
  unsigned char* d = dst;

  for (s = src; s < end; s++)
  {
      //unsigned char v = *s;
      //unsigned char b = table[v >> 4];
      //printf ("hi: v=%u, b=%c\n", v, b);
      //*d = b;
      *d = table[*s >> 4];
      d++;
      *d = table[*s & 0x0f];
      //printf ("lo: %d (%p)\n", *d, d);
      d++;
  }
  *d = 0;
}  // numToHex


int hashByPath (char* path, char* result_hexline)
{
    char sample[SAMPLE_SIZE];
    int fd;
    size_t rr;
    unsigned char hash[20];
    int hr;

    fd = open (path, O_RDONLY);
    if (fd == -1) {
        printf ("unable to open %s\n", path);
        return 0;
    }
    rr = read (fd, sample, SAMPLE_SIZE);
    close (fd);
    //printf ("%u bytes read\n", rr);
    if (rr == -1) {
        printf ("unable to read %s\n", path);
        return 0;
    }
    if (rr == 0) {
        printf ("read 0 bytes from %s\n", path);
        return 0;
    }
    hr = rhash_msg (RHASH_SHA1, (const void*)sample, rr, hash);
    if (hr != 0)
        return 0;
    numToHex (hash, 20, result_hexline);
    return 1;
} // hashByPath


char* genreFromPath (char* path, char* buf, int buf_size)
{
    char* slash;
    int genre_size;

    slash = strchr (path, '/');
    if (slash == NULL)
        return NULL;
    genre_size = slash - path;
    if (genre_size > buf_size -1)
        return NULL;
    strncpy (buf, path, genre_size);
    buf[genre_size] = '\0';
    return buf;
} // genreFromPath


// return:
// 0: not added or added to 'all' set only
// 1: added to 'all' set, main hash
// 2: added to 'all' set, main hash and unlisten list
int updateDBFromSingleFile (redisContext * c, FTSENT * pe)
{
    char *ext;
    redisReply *reply, *reply2;
    char hexhash[SONG_HASH_SIZE];
    int hr;
    int exist;
    char genre[16]; int genre_size=16;
    bool add_to_unlisten;

    ext = strrchr(pe->fts_name, '.');
    if (ext == NULL) {
        printf ("no ext: %s\n", pe->fts_name);
        return 0;
    }
    ext = ext+1;
    if (
            strcasecmp (ext, "mp3") != 0 &&
            strcasecmp (ext, "ogg") != 0 && 
            strcasecmp (ext, "ape") != 0 && 
            strcasecmp (ext, "wav") != 0 && 
            strcasecmp (ext, "webm") != 0 && 
            strcasecmp (ext, "m4a") != 0 && 
            strcasecmp (ext, "mkv") != 0 && 
            strcasecmp (ext, "mp4") != 0 && 
            strcasecmp (ext, "avi") != 0 && 
            strcasecmp (ext, "mov") != 0 && 
            strcasecmp (ext, "wma") != 0 && 
            strcasecmp (ext, "mp2") != 0 && 
            strcasecmp (ext, "mpg") != 0 && 
            strcasecmp (ext, "mpeg") != 0 && 
            strcasecmp (ext, "opus") != 0 && 
            strcasecmp (ext, "flac") != 0 
       ) {
        //printf ("skip: %s\n", pe->fts_name);
        return 0;
    }

    if (genreFromPath (pe->fts_accpath, genre, genre_size) == NULL) {
        printf ("can't guess genre of %s\n", pe->fts_accpath);
        return 0;
    }

    reply = redisCommand (c,"HEXISTS path2hash %s", pe->fts_accpath);
    if (!reply)
        return 0;
    exist = reply->integer;
    freeReplyObject(reply);
    if (exist != 0) {
        //printf ("exist: %s\n", pe->fts_name);
        return 0;
    }

    hr = hashByPath (pe->fts_accpath, hexhash);
    if (hr != 1) {
        printf ("can't hash file %s\n", pe->fts_accpath);
        return 0;
    }

    reply = redisCommand (c,"SADD all:%s %s", genre, hexhash);
    freeReplyObject(reply);
    // ignore result

    printf ("add: %s\n", pe->fts_name);
    reply = redisCommand (c,"HSET path2hash %s %s",
         pe->fts_accpath, hexhash); //pe->fts_statp->st_size);
    reply2 = redisCommand (c,"HSET hash2path %s %s",
         hexhash, pe->fts_accpath);
    if (!reply || !reply2)
        return 0;
    freeReplyObject(reply);
    freeReplyObject(reply2);

    reply = redisCommand (c,"LPOS unlisten:%s %s", genre, hexhash);
    if (!reply)
        return 1;
    add_to_unlisten = (reply->type == REDIS_REPLY_NIL || reply->type == REDIS_REPLY_ERROR);
    freeReplyObject(reply);
    if (! add_to_unlisten)
        return 1;

    //printf ("unlisten:%s\n", genre);
    reply = redisCommand (c,"LPUSH unlisten:%s %s", genre, hexhash);
    if (!reply)
        return 1;
    freeReplyObject(reply);

    return 2;
}  // updateDBFromSingleFile


int updateDBFromFiles(redisContext * c, char** tops)
{
    int d_c, f_c;
    FTS *ftsp;
    FTSENT *p, *chp;
    int fts_options = FTS_COMFOLLOW | FTS_LOGICAL | FTS_NOCHDIR;
    int ret = 0;
    int new = 0; int unl = 0;

    if ((ftsp = fts_open(tops, fts_options, NULL)) == NULL) {
        printf ("fts_open fail\n");
        goto out;
    }
    /* Initialize ftsp with as many dirs as possible. */
    chp = fts_children(ftsp, 0);
    if (chp == NULL) {
        goto out;               /* no files to traverse */
    }
    d_c = f_c = 0;
    while ((p = fts_read(ftsp)) != NULL) {
        switch (p->fts_info) {
        case FTS_D:
            //printf("d %s\n", p->fts_path);
            d_c++;
            break;
        case FTS_F:
            //printf("f %s\n", p->fts_path);
            switch (updateDBFromSingleFile(c, p)) {
                case 1: new++;
                case 2: new++; unl++;
            }
            f_c++;
            break;
        default:
            break;
        }
    }
    fts_close(ftsp);
    printf("dirs: %d, files: %d, new: %d, unlisten: %d\n", d_c, f_c, new, unl);
    ret = 1;
out:
    return ret;
}   // updateDBFromFiles


redisContext* initRedis()
{
    redisContext *c;

    struct timeval timeout = { 1, 500000 }; // 1.5 seconds
    c = redisConnectWithTimeout(redis_hostname, redis_port, timeout);
    if (c == NULL || c->err) {
        if (c) {
            printf("Connection error: %s\n", c->errstr);
            redisFree(c);
        } else {
            printf("Connection error: can't allocate redis context\n");
        }
        exit(1);
    }
    return c;
}   // initRedis


void purgeDb(redisContext * c) {
    int n;
    redisReply *reply;

    for (n = 0; n < N_GENRES; n++) {
        printf ("DEL unlisten:%s ", genre_str[n]);
        reply = redisCommand (c,"DEL unlisten:%s", genre_str[n]);
        if (reply) {
            printf ("OK\n");
            freeReplyObject(reply);
        }
        printf ("DEL listen:%s ", genre_str[n]);
        reply = redisCommand (c,"DEL listen:%s", genre_str[n]);
        if (reply) {
            printf ("OK\n");
            freeReplyObject(reply);
        }
        printf ("DEL all:%s ", genre_str[n]);
        reply = redisCommand (c,"DEL all:%s", genre_str[n]);
        if (reply) {
            printf ("OK\n");
            freeReplyObject(reply);
        }
    }
    printf ("DEL path2hash ");
    reply = redisCommand (c,"DEL path2hash");
    if (reply) {
        printf ("OK\n");
        freeReplyObject(reply);
    }
    printf ("DEL hash2path ");
    reply = redisCommand (c,"DEL hash2path");
    if (reply) {
        printf ("OK\n");
        freeReplyObject(reply);
    }
}   // purgeDb


int main (int argc, char **argv) {

    redisContext *c;
    int u;
    char **tops;
    int arg_consumed = 0;

    c = initRedis();

    if (argc > 1) {
        if (strcmp (argv[1], "-p") == 0) {
            purgeDb (c);
            arg_consumed++;
        }
    }
    if (argc - arg_consumed > 1) {
        tops = argv + 1 + arg_consumed;
    } else {
        chdir (recordings_top_dir);
        tops = (char**)genre_str;
    }

    rhash_library_init();
    u = updateDBFromFiles (c, tops);
    redisFree(c);

    return u == 0 ? 1 : 0;
}
