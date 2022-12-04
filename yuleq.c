// yu’egh leQ
//
// Copyright 2022 Yuleq Authors
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// The software is provided "as is", without warranty of any kind,
// express or implied, including but not limited to the warranties of
// merchantability, fitness for a particular purpose and noninfringement.
// In no event shall the authors or copyright holders be liable for any
// claim, damages or other liability, whether in an action of contract,
// tort or otherwise, arising from, out of or in connection with the
// software or the use or other dealings in the software.
//
// Requirements
// - ffmpeg and ffprobe in $PATH
// - portaudio library
//
// Compile
//     gcc -Wall -O2 -lportaudio -lm yuleq.c -o yuleq

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <consoleapi.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <portaudio.h>


#define MAX_TRACKS 10       // max number of input files
#define MAX_LENGTH 600      // max input length in s
#define STEP       50       // loop adjustment step in ms
#define LATENCY    20       // audio buffer size in ms
#define CHUNK_SIZE 0x100000 // slurp chunk size in bytes
#define HELP       "\
syntax: yuleq [options] files...\n\
    -h   show help\n\
    -r   blind test with reference\n\
    -b   blind test without reference\n\
    -l   list audio devices\n\
    -d n audio device index\n\
    -o n output samplerate\n\
    -v   verbose output\n"

#define PANIC(...) do {printf(__VA_ARGS__); exit(1);} while (0)

#ifdef  _WIN32
#undef  min
#undef  max
#define M_PI        3.14159265358979323846
#define popen(c, t) _popen(c, t"r")
#define pclose      _pclose
#define write       _write
#endif

struct arg {
    bool  list_devices;
    bool  blind;
    bool  refblind;
    int   device_index;
    int   device_rate;
    char* files[MAX_TRACKS];
    int   num_files;
    bool  verbose;
};

struct buffer {
    void* buf;
    int   size;
};

struct track {
    float* pcm;        // interleaved channels
    char*  name;       // file name
    int    channels;   // source channels
    int    samplerate; // source samplerate
    int    length;     // total frames in buffer
};

struct player {
    int    track;      // current track
    int    next;       // next track
    int    pos;        // track position
    int    start;      // loop start
    int    end;        // loop end
    int    length;     // total length in samples
    int    channels;   // output channels
    int    samplerate; // output samplerate
    bool   running;    // running flag
    bool   paused;     // true when paused
    float* window;     // fade window coefficients
};


static PaStream*     stream;
static struct arg    arg;
static struct player player;
static struct track  tracks[MAX_TRACKS];


static int min(int a, int b) {
    return a < b ? a : b;
}

static int max(int a, int b) {
    return a > b ? a : b;
}

// true if architecture is big endian
static bool isbig(void) {
    int one = 1;
    return !(*(char*)&one);
}

static void parse_args(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        // file args
        if (argv[i][0] != '-') {
            if (arg.num_files >= MAX_TRACKS) {
                PANIC("too many files\n");
            }
            arg.files[arg.num_files] = argv[i];
            arg.num_files += 1;
            continue;
        }

        char  flag  = argv[i][1];
        char* value = "";

        // get value as -xbar or -x bar
        if (flag && argv[i][2]) {
            value = &argv[i][2];
        } else if (i + 1 < argc) {
            value = argv[i + 1];
        }

        if (flag == 'h') {
            printf(HELP);
            exit(0);
        } else if (flag == 'v') {
            arg.verbose = true;
        } else if (flag == 'b') {
            arg.blind = true;
        } else if (flag == 'r') {
            arg.refblind = true;
        } else if (flag == 'l') {
            arg.list_devices = true;
        } else if (flag == 'd') {
            char* endptr = NULL;
            arg.device_index = strtol(value, &endptr, 10) + 1;
            if (endptr == value) {
                PANIC("invalid device index: '%s'\n", value);
            }
            i += !argv[i][2];
        } else if (flag == 'o') {
            char* endptr = NULL;
            arg.device_rate = strtol(value, &endptr, 10);
            if (endptr == value) {
                PANIC("invalid samplerate: '%s'\n", value);
            }
            i += !argv[i][2];
        } else {
            PANIC("unknown option: %s\n", argv[i]);
        }
    }
}

static void* alloc(void* ptr, size_t size) {
    ptr = realloc(ptr, size);
    if (!ptr) {
        PANIC("out of memory\n");
    }
    return ptr;
}

// generate cross-fade window
static void gen_window(void) {
    int ch     = player.channels;
    int n      = LATENCY * player.samplerate / 1000;
    float* win = alloc(player.window, n * ch * sizeof(float));

    for (int i = 0; i < n; i++) {
        double w = 0.5 + 0.5 * cos(M_PI * i / n);
        for (int c = 0; c < ch; c++) {
            win[i * ch + c] = (float)w;
        }
    }
    player.window = win;
}

// cross-fade out to in using window
static void apply_window(float* out, const float* in) {
    int ch     = player.channels;
    int n      = LATENCY * player.samplerate / 1000;
    float* win = player.window;

    for (int i = 0; i < n * ch; i++) {
        out[i] = win[i] * out[i] + (1.0f - win[i]) * in[i];
    }
}

// audio processing callback
static int process(const void* input, void* output, unsigned long n, const PaStreamCallbackTimeInfo* time, PaStreamCallbackFlags flags, void* data) {
    int    ch  = player.channels;
    float* in  = tracks[player.track].pcm + player.pos * ch;
    float* out = output;

    if (player.paused) {
        memset(out, 0, n * ch * sizeof(float));
        return paContinue;
    }

    memcpy(out, in, n * ch * sizeof(float));

    // track switch windowing
    if (player.track != player.next) {
        in = tracks[player.next].pcm + player.pos * ch;
        apply_window(out, in);
        player.track = player.next;
    }

    player.pos += n;
    // loop windowing
    if (player.pos > player.end) {
        in = tracks[player.track].pcm + player.start * ch;
        apply_window(out, in);
        player.pos = player.start + n;
    }

    return paContinue;
}

static void init_audio(void) {
    int err = Pa_Initialize();
    if (err) {
        PANIC("audio init failed: %s\n", Pa_GetErrorText(err));
    }
}

static void start_stream(void) {
    int device = Pa_GetDefaultOutputDevice();
    if (arg.device_index) {
        device = arg.device_index - 1;
    }

    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    if (!info) {
        PANIC("invalid device index: %d\n", device);
    }

    int ch      = player.channels;
    int sr      = player.samplerate;
    int samples = LATENCY * sr / 1000;

    player.end     = player.length;
    player.running = true;

    PaStreamParameters params = {
        .device           = device,
        .channelCount     = ch,
        .sampleFormat     = paFloat32,
        .suggestedLatency = info->defaultLowOutputLatency,
    };

    int err = Pa_OpenStream(&stream, NULL, &params, sr, samples, 0, process, NULL);
    if (err) {
        PANIC("stream open failed: %s\n", Pa_GetErrorText(err));
    }

    err = Pa_StartStream(stream);
    if (err) {
        PANIC("stream start failed: %s\n", Pa_GetErrorText(err));
    }
}

static void list_devices(void) {
    int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        printf("%2d: %s\n", i, info->name);
    }
}

// run command and capture stdout
static struct buffer slurp(const char* command, ...) {
    char cmd[0x1000] = {0};
    va_list ap = {0};

    va_start(ap, command);
    vsnprintf(cmd, sizeof(cmd) - 1, command, ap);
    va_end(ap);
    if (arg.verbose) {
        printf("%s\n", cmd);
    }

    FILE* f = popen(cmd, "r");
    if (!f) {
        PANIC("command failed: %s\n", cmd);
    }

    char* buf = NULL;
    int   len = 0;
    int   cap = CHUNK_SIZE;
    int   n   = CHUNK_SIZE;

    while (n == CHUNK_SIZE) {
        buf = alloc(buf, cap);
        n   = (int)fread(buf + len, 1, CHUNK_SIZE, f);
        len += n;
        cap += CHUNK_SIZE;
    }
    buf[len] = 0; // ensure zero termination

    if (pclose(f) != 0) {
        PANIC("command failed: %s\n", cmd);
    }

    return (struct buffer){buf, len};
}

// search in s for prefix and return subsequent integer
static int grep_int(const char* s, const char* prefix) {
    char* tmp  = strstr(s, prefix);
    if (!tmp) {
        return 0;
    }
    return atoi(tmp + strlen(prefix));
}

// load track from file into ram
static struct track load_track(char* name) {
    struct track  t = {0};
    struct buffer b = {0};

    // get info from ffprobe
    b = slurp("ffprobe -of flat -show_streams -select_streams a \"%s\"", name);

    t.channels   = grep_int(b.buf, "streams.stream.0.channels=");
    t.samplerate = grep_int(b.buf, "streams.stream.0.sample_rate=\"");
    if (t.channels == 0 || t.samplerate == 0) {
        PANIC("%s: invalid audio file\n", name);
    }

    int duration = grep_int(b.buf, "streams.stream.0.duration=\"");
    if (duration > MAX_LENGTH) {
        PANIC("%s: too long\n", name);
    }
    free(b.buf);

    // get pcm data from ffmpeg
    char* en = isbig() ? "be" : "le";
    int   sr = arg.device_rate;
    if (sr) {
        b = slurp("ffmpeg -i \"%s\" -af aresample=%d:resampler=soxr:precision=33 -f f32%s -", name, sr, en);
    } else {
        b = slurp("ffmpeg -i \"%s\" -f f32%s -", name, en);
    }

    t.length = b.size / sizeof(float) / t.channels;
    t.pcm    = b.buf;
    t.name   = name;
    return t;
}

static void load_tracks(void) {
    if (arg.num_files == 0) {
        PANIC("no input files\n");
    }
    struct player* p  = &player;
    struct track*  t0 = &tracks[0];

    for (int i = 0; i < arg.num_files; i++) {
        struct track* t = &tracks[i];

        *t = load_track(arg.files[i]);

        // first track determines length, channels, rate
        if (t->length != t0->length) {
            printf("%s: length mismatch, got %d, expected %d\n", t->name, t->length, t0->length);
        }
        if (t->channels != t0->channels) {
            PANIC("%s: channel mismatch, got %d, expected %d\n", t->name, t->channels, t0->channels);
        }
        if (t->samplerate != t0->samplerate) {
            PANIC("%s: samplerate mismatch, got %d, expected %d\n", t->name, t->samplerate, t0->samplerate);
        }

        if (i == 0) {
            p->length     = t->length;
            p->channels   = t->channels;
            p->samplerate = arg.device_rate ? arg.device_rate : t->samplerate;
        }

        // apply zero padding to end of buffer
        int samples = LATENCY * player.samplerate / 1000;
        if (t->length < p->length) {
            samples += p->length - t->length;
        }
        int size  = t->length * t->channels * sizeof(float);
        int bytes = samples * t->channels * sizeof(float);
        t->pcm = alloc(t->pcm, size + bytes);
        memset(t->pcm + t->length * t->channels, 0, bytes);
    }
}

static void shuffle_tracks(bool skip_first) {
    srand((unsigned)time(NULL));
    int n = arg.num_files;

    for (int i = (int)skip_first; i < n - 1; i++) {
        int j = i + (int)(rand() / (RAND_MAX + 1.0) * (n - i));
        struct track t = tracks[i];
        tracks[i] = tracks[j];
        tracks[j] = t;
    }
}

#ifdef _WIN32

static void init_terminal(void) {
    DWORD m = 0;
    GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &m);
    m &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT);
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), m);
    GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &m);
    m |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), m);
    write(1, "\33[?25l", 6); // hide cursor
}

static void restore_terminal(void) {
    DWORD m = 0;
    GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &m);
    m |= ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT;
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), m);
    write(1, "\33[?25h\n", 7); // show cursor
    GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &m);
    m &= ~ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), m);
}

static char read_key(void) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (WaitForSingleObject(h, 100) == WAIT_TIMEOUT) {
        return 0;
    }
    DWORD        n    = 0;
    INPUT_RECORD r[1] = {0};
    ReadConsoleInput(h, r, 1, &n);
    if (r[0].EventType != KEY_EVENT || !r[0].Event.KeyEvent.bKeyDown) {
        return 0;
    }
    return r[0].Event.KeyEvent.uChar.AsciiChar;
}

#else // _WIN32

static void init_terminal(void) {
    struct termios a = { 0 };
    tcgetattr(0, &a);
    a.c_lflag &= ~(ICANON | ECHO); // unbuffered, echo off
    a.c_cc[VMIN]  = 0;             // 100 ms read timeout
    a.c_cc[VTIME] = 1;
    tcsetattr(0, TCSANOW, &a);
    write(1, "\33[?25l", 6); // hide cursor
}

static void restore_terminal(void) {
    struct termios a = { 0 };
    tcgetattr(0, &a);
    a.c_lflag |= (ICANON | ECHO); // buffered, echo on
    tcsetattr(0, TCSANOW, &a);
    write(1, "\33[?25h\n", 7); // display cursor
}

static char read_key(void) {
    char ch = 0;
    read(0, &ch, 1);
    return ch;
}

#endif // _WIN32

static void clear_terminal(void) {
    write(1, "\33[H\33[J", 6);
}

static void print_progress(void) {
    char buf[81];

    int pos   = player.pos * 80 / player.length;
    int start = player.start * 80 / player.length;
    int end   = (player.end - 1) * 80 / player.length;

    for (int i = 0; i < 80; i++) {
        if (i == pos) {
            buf[i] = '0' + (player.track + 1) % 10;
        } else if (i == start) {
            buf[i] = '[';
        } else if (i == end) {
            buf[i] = ']';
        } else {
            buf[i] = '-';
        }
    }
    buf[80]='\r';

    write(1, buf, 81);
}

static void print_files(bool reference, bool blind) {
    if (reference) {
        printf("[1] reference\n");
    }
    for (int i = (int)reference; i < arg.num_files; i++) {
        char* name = blind ? "???" : tracks[i].name;
        printf("[%d] %s\n", (i + 1) % 10, name);
    }
}

static void print_info(void) {
    printf("--------------------------------------------------------------------------------\n");
    print_files(arg.refblind, arg.blind || arg.refblind);
    printf("--------------------------------------------------------------------------------\n"
           "[s] start  [x] clear  [i/o] adjust  [q]     quit                     %d channels\n"
           "[d] end    [c] clear  [k/l] adjust  [space] pause                    %d Hz\n",
           player.channels, player.samplerate);
}

// handle ctrl-c
static void signal_handler(int sig) {
    player.running = false;
}

int main(int argc, char** argv) {
    parse_args(argc - 1, argv + 1);
    if (!arg.verbose) {
        fclose(stderr); // mute portaudio / ffmpeg print noise
    }

    init_audio();
    if (arg.list_devices) {
        list_devices();
        exit(0);
    }

    load_tracks();
    if (arg.blind || arg.refblind) {
        shuffle_tracks(arg.refblind);
    }

    gen_window();
    start_stream();

    init_terminal();
    if (!arg.verbose) {
        clear_terminal();
    }
    print_info();
    signal(SIGINT, signal_handler);

    int step = STEP * player.samplerate / 1000;

    while (player.running) {
        char ch = read_key(); // key or 0 on timeout

        switch (ch) {
        case ' ':
            player.paused = !player.paused;
            break;
        case '0':
            ch += 10; // fallthru
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            if (ch - '0' <= arg.num_files) {
                player.next = ch - '0' - 1;
            }
            break;
        case 'c': // clear end
            player.end = player.length;
            break;
        case 'd': // set end
            player.end = player.pos;
            break;
        case 'i': // dec start
            player.start = max(player.start - step, 0);
            break;
        case 'k': // dec end
            player.end = max(player.end - step, player.start);
            break;
        case 'l': // inc end
            player.end = min(player.end + step, player.length);
            break;
        case 'o': // inc start
            player.start = min(player.start + step, player.end);
            break;
        case 'q': // quit
            player.running = false;
            break;
        case 's': // set start
            player.start = player.pos;
            break;
        case 'x': // clear start
            player.start = 0;
            break;
        }

        print_progress();
    }

    restore_terminal();
    if (arg.blind || arg.refblind) {
        print_files(false, false);
    }
}
