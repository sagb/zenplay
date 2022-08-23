//////////////////// START OF SETTINGS /////////////////////

// redis database contains pathes relative to this directory.
// second level directories must be genre_str (see below).
const char* recordings_top_dir = "/music";

// each genre has own:
//   distinct control button,
//   redis tables and statistics,
//   own directory under recordings_top_dir.

// number of genres
#define N_GENRES    2
// start with this genre
#define DEFAULT_GENRE  1  // depressive/blue

const char* genre_str[] = {
    "red",
    "blue",
    NULL
};
const char *chipname = "gpiochip0";
const unsigned int button_gpio[N_GENRES] = {
    27,
    17
};
const unsigned int led_gpio[N_GENRES] = {
    6,
    5
};

// enable debug mode:
// to change song to first genre, enter "0" at standard input,
//  second - "1" and so on.
//#define USE_KEYBOARD_INSTEAD_OF_GPIO

// to index recordings,
// hash this first number of bytes
#define SAMPLE_SIZE  0x10000
// to this hash size
#define SONG_HASH_SIZE 41

const char *redis_hostname = "127.0.0.1";
const int redis_port = 6379;

// 1 - most frequently choosen songs are preferred
// 0 - random
const bool popular_mode = 0;
// 1 - order
// 0 - shuffle
const bool order_instead_of_shuffle = 1;

//////////////////// END OF SETTINGS /////////////////////
