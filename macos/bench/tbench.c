/*
 * tbench: the part of the benchmark (bench.py) that runs inside the
 * terminal being measured, and the processes' statistics.
 *
 * tbench run OUTFILE [RUNS]
 *     How fast the terminal consumes output. Each workload is written
 *     to stdout, followed by a DA1 query; the time is taken when the
 *     terminal's answer arrives, i.e. once it has processed everything.
 *     Appends "size <cols> <rows> <width> <height>" to OUTFILE, then a
 *     "<workload> <bytes> <seconds>" line per run, then "done".
 *     TBENCH_ONLY=<workload> runs only that one.
 *
 * tbench mark FILE
 *     Writes the time (seconds since the epoch) to FILE, and 3 s later
 *     the window's size to FILE.size, then waits until the terminal
 *     closes: how long the terminal took to start its command, and an
 *     idle window.
 *
 * tbench stat PID
 *     Prints the process' CPU time (ns), its idle and interrupt
 *     wakeups, and its memory footprint (bytes).
 */
#include <errno.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

struct buf { char *p; size_t len, cap; };

static void
put(struct buf *b, const char *s, size_t n)
{
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2;
        b->p = realloc(b->p, b->cap);
        if (b->p == NULL)
            exit(1);
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

static void
putf(struct buf *b, const char *fmt, ...)
{
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    put(b, tmp, (size_t)n);
}

/* xorshift64, seeded for each workload: the same output in every run */
static uint64_t rng;
static uint32_t
rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

static double
now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void
write_all(const char *p, size_t n)
{
    while (n > 0) {
        ssize_t r = write(1, p, n > 65536 ? 65536 : n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            exit(1);
        }
        p += r;
        n -= (size_t)r;
    }
}

/*
 * Waits until the terminal has processed everything written so far:
 * its answer to DA1 is CSI ? <params> c. Anything else (e.g. a key
 * pressed in the window) is ignored.
 */
static void
sync_terminal(void)
{
    write_all("\033[c", 3);

    int state = 0;
    for (;;) {
        char c;
        ssize_t r = read(0, &c, 1);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            exit(1);

        if (c == '\033')
            state = 1;
        else if (state == 1)
            state = c == '[' ? 2 : 0;
        else if (state == 2)
            state = c == '?' ? 3 : 0;
        else if (state == 3) {
            if (c == 'c')
                return;
            if ((c < '0' || c > '9') && c != ';')
                state = 0;
        }
    }
}

#define MiB (1024 * 1024)

/* Lines of printable ASCII, of random lengths */
static void
gen_ascii(struct buf *b, int cols, int rows)
{
    while (b->len < 64 * MiB) {
        int n = 10 + rnd() % (cols - 10);
        for (int i = 0; i < n; i++) {
            char c = 32 + rnd() % 95;
            put(b, &c, 1);
        }
        put(b, "\r\n", 2);
    }
}

/* Lines of non-ASCII text: accents, CJK (double width), emoji, box
 * drawing, and a combining character */
static void
gen_unicode(struct buf *b, int cols, int rows)
{
    static const char *pieces[] = {
        "ä", "ö", "ü", "ß", "€", "é", "ñ", "λ", "Ж", "→", "│", "─", "█",
        "漢", "字", "日", "本", "語", "한", "글", "🚀", "😀", "👍", "e\xcc\x81",
        "a", "b", "c", " ", " ", " ",
    };
    while (b->len < 32 * MiB) {
        int width = 0;
        int n = 10 + rnd() % (cols - 12);
        while (width < n) {
            const char *s = pieces[rnd() % (sizeof(pieces) / sizeof(pieces[0]))];
            put(b, s, strlen(s));
            width += (unsigned char)s[0] >= 0xe3 ? 2 : 1;
        }
        put(b, "\r\n", 2);
    }
}

/* Full screens of cells, each with its own foreground and background
 * color (24-bit) */
static void
gen_dense_cells(struct buf *b, int cols, int rows)
{
    while (b->len < 64 * MiB) {
        put(b, "\033[H", 3);
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                putf(b, "\033[38;2;%u;%u;%u;48;2;%u;%u;%um%c",
                     rnd() & 0xff, rnd() & 0xff, rnd() & 0xff,
                     rnd() & 0xff, rnd() & 0xff, rnd() & 0xff,
                     'A' + rnd() % 26);
            }
        }
    }
    put(b, "\033[0m", 4);
}

/* Short lines, i.e. scrolling (like yes) */
static void
gen_scroll(struct buf *b, int cols, int rows)
{
    while (b->len < 16 * MiB)
        put(b, "y\r\n", 3);
}

/* The same, in a scrolling region (like a pager, or vim) */
static void
gen_scroll_region(struct buf *b, int cols, int rows)
{
    putf(b, "\033[2;%dr\033[%d;1H", rows - 1, rows - 1);
    while (b->len < 16 * MiB)
        put(b, "y\r\n", 3);
    put(b, "\033[r", 3);
}

/* A character at a random position, again and again */
static void
gen_cursor_motion(struct buf *b, int cols, int rows)
{
    while (b->len < 32 * MiB)
        putf(b, "\033[%u;%uH%c", 1 + rnd() % rows, 1 + rnd() % cols, 'A' + rnd() % 26);
}

static int
run(const char *path, int runs)
{
    FILE *out = fopen(path, "a");
    if (out == NULL)
        return 1;

    struct termios orig, raw;
    tcgetattr(0, &orig);
    raw = orig;
    cfmakeraw(&raw);
    tcsetattr(0, TCSANOW, &raw);

    /* Let the window settle (e.g. its initial size) */
    usleep(1500 * 1000);
    sync_terminal();

    struct winsize ws = {0};
    ioctl(1, TIOCGWINSZ, &ws);
    fprintf(out, "size %d %d %d %d\n", ws.ws_col, ws.ws_row, ws.ws_xpixel, ws.ws_ypixel);
    fflush(out);

    static const struct {
        const char *name;
        void (*gen)(struct buf *, int, int);
    } workloads[] = {
        {"ascii", gen_ascii},
        {"unicode", gen_unicode},
        {"dense_cells", gen_dense_cells},
        {"scroll", gen_scroll},
        {"scroll_region", gen_scroll_region},
        {"cursor_motion", gen_cursor_motion},
    };

    const char *only = getenv("TBENCH_ONLY");
    for (size_t w = 0; w < sizeof(workloads) / sizeof(workloads[0]); w++) {
        if (only != NULL && strcmp(only, workloads[w].name) != 0)
            continue;

        rng = 0x2545F4914F6CDD1DULL + w;
        struct buf b = {0};
        workloads[w].gen(&b, ws.ws_col, ws.ws_row);

        for (int i = 0; i < runs; i++) {
            write_all("\033[0m\033[r\033[2J\033[H", 14);
            sync_terminal();

            const double t0 = now();
            write_all(b.p, b.len);
            sync_terminal();
            const double t = now() - t0;

            fprintf(out, "%s %zu %.6f\n", workloads[w].name, b.len, t);
            fflush(out);
        }
        free(b.p);
    }

    write_all("\033[0m\033[r\033[2J\033[H", 14);
    tcsetattr(0, TCSANOW, &orig);
    fprintf(out, "done\n");
    fclose(out);
    return 0;
}

/* Renamed into place, so that it's never read half-written */
static int
write_file(const char *path, const char *text)
{
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL)
        return 1;
    fputs(text, f);
    return fclose(f) != 0 || rename(tmp, path) != 0;
}

static int
mark(const char *path)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    char text[128];
    snprintf(text, sizeof(text), "%lld.%09ld\n", (long long)ts.tv_sec, ts.tv_nsec);
    if (write_file(path, text) != 0)
        return 1;

    sleep(3);
    struct winsize ws = {0};
    ioctl(1, TIOCGWINSZ, &ws);
    char size_path[4096];
    snprintf(size_path, sizeof(size_path), "%s.size", path);
    snprintf(text, sizeof(text), "%d %d %d %d\n", ws.ws_col, ws.ws_row, ws.ws_xpixel, ws.ws_ypixel);
    if (write_file(size_path, text) != 0)
        return 1;

    /* Until the terminal closes (SIGHUP) */
    for (;;)
        pause();
}

static int
stat_process(pid_t pid)
{
    struct rusage_info_v4 ri;
    if (proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&ri) != 0) {
        perror("proc_pid_rusage");
        return 1;
    }

    /* The CPU times are in Mach absolute time units */
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const uint64_t cpu = (ri.ri_user_time + ri.ri_system_time) * tb.numer / tb.denom;

    printf("%llu %llu %llu %llu\n", (unsigned long long)cpu,
           (unsigned long long)ri.ri_pkg_idle_wkups,
           (unsigned long long)ri.ri_interrupt_wkups,
           (unsigned long long)ri.ri_phys_footprint);
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "run") == 0) {
        const int runs = argc > 3 ? atoi(argv[3]) : 3;
        return run(argv[2], runs > 0 ? runs : 1);
    }
    if (argc == 3 && strcmp(argv[1], "mark") == 0)
        return mark(argv[2]);
    if (argc == 3 && strcmp(argv[1], "stat") == 0)
        return stat_process((pid_t)atoi(argv[2]));

    fprintf(stderr,
            "usage: tbench run OUTFILE [RUNS]\n"
            "       tbench mark FILE\n"
            "       tbench stat PID\n");
    return 2;
}
