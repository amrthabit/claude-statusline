/*
 * Claude Code statusline — C port of statusline.py (byte-for-byte parity).
 *
 * Reads the status JSON on stdin, reads cheap system metrics from /proc and
 * /sys, prints one compact color-coded line (no trailing newline).
 *
 *   Segments:  launch>current dir | model ctx% 5h 7d | BAT RAM CPU DISK IO NET
 *
 * Build:   gcc -O2 -Wall -Wextra -std=c11 -o statusline-bin statusline.c vendor/cJSON.c -lm
 * (or just `make`)
 *
 * Thresholds and behavior knobs live in the CONFIG block below.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include "vendor/cJSON.h"

/* ---------------------------------------------------------------- CONFIG -- */
/* Color thresholds (percent). >=WARN -> amber, >=BAD -> red, else green.    */
#define SYS_WARN 70.0   /* RAM / CPU / disk / context */
#define SYS_BAD  80.0
#define USE_WARN 70.0   /* 5h / 7d subscription usage */
#define USE_BAD  80.0

/* Minimum seconds between recomputing rate metrics (cpu/net/io). Kept BELOW
 * the 1s refreshInterval so every tick recomputes (the Python version's 1.0
 * would sit exactly on the threshold and flicker between fresh and stale).  */
#define RATE_MIN_INTERVAL 0.45

/* Throughput thresholds for IO / network (bytes per second). Below WARN uses
 * the terminal's standard color; >=WARN amber; >=BAD red.                   */
#define RATE_WARN (10.0 * 1024 * 1024)  /* 10 MiB/s */
#define RATE_BAD  (30.0 * 1024 * 1024)  /* 30 MiB/s */

#define PATH_MAXLEN 30   /* dir display length before middle-elision */
/* -------------------------------------------------------------------------- */

/* ANSI */
#define RST  "\033[0m"
#define DIM  "\033[2m"
#define BOLD "\033[1m"
#define GRN  "\033[32m"
#define YLW  "\033[33m"
#define RED  "\033[31m"
#define CYN  "\033[36m"

static bool NERD = true;
static const char *HOME = "";

/* Glyphs (Nerd Font v3), with plain-text fallbacks. Selected at runtime. */
typedef struct {
    const char *launch, *cur, *model, *ctx, *t5h, *t7d,
               *ram, *cpu, *disk, *io, *net, *arrow, *bat;
} Glyphs;
static const Glyphs G_NERD = {
    .launch = u8"",      /* fa home        */
    .cur    = u8"",      /* fa folder-open */
    .model  = "",              /* (no icon)      */
    .ctx    = "",              /* (no icon)      */
    .t5h    = "",              /* (no icon)      */
    .t7d    = "",              /* (no icon)      */
    .ram    = u8"\U000F0F58",  /* md memory      */
    .cpu    = u8"",      /* fa microchip   */
    .disk   = u8"\U000F02CA",  /* md harddisk    */
    .io     = u8"",      /* fa exchange    */
    .net    = u8"\U000F0200",  /* md ethernet    */
    .arrow  = u8"",      /* fa angle-right */
    .bat    = "",              /* chosen by level, see bat_glyph */
};
static const Glyphs G_PLAIN = {
    .launch = "L", .cur = "D", .model = "M", .ctx = "ctx",
    .t5h = "5h", .t7d = "7d", .ram = "RAM", .cpu = "CPU",
    .disk = "DSK", .io = "IO", .net = "NET", .arrow = ">", .bat = "BAT",
};
static const Glyphs *G = &G_NERD;

/* fa battery ramp U+F240 (full) .. U+F244 (empty) */
static const char *BAT_RAMP[5] = {
    u8"", u8"", u8"", u8"", u8"",
};

/* ------------------------------------------------------- string building -- */
typedef struct { char buf[8192]; size_t len; } SB;

static void sb_raw(SB *s, const char *fmt, ...) {
    if (s->len >= sizeof s->buf - 1) return;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(s->buf + s->len, sizeof s->buf - s->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        s->len += (size_t)n;
        if (s->len > sizeof s->buf - 1) s->len = sizeof s->buf - 1;
    }
}

/* Python c(s, color): color + text + RESET (reset always appended, even for
 * the empty "standard color" — byte parity with the reference).             */
static void sb_c(SB *s, const char *color, const char *fmt, ...) {
    char tmp[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    sb_raw(s, "%s%s" RST, color, tmp);
}

/* ---------------------------------------------------------------- colors -- */
static const char *pct_color(double p)  { return p >= SYS_BAD ? RED : p >= SYS_WARN ? YLW : GRN; }
static const char *use_color(double p)  { return p >= USE_BAD ? RED : p >= USE_WARN ? YLW : GRN; }
static const char *rate_color(double b) { return b >= RATE_BAD ? RED : b >= RATE_WARN ? YLW : ""; }
static const char *bat_color(int pct, bool charging) {
    /* Inverted vs usage: a LOW battery is bad. */
    if (charging) return GRN;
    if (pct <= 15) return RED;
    if (pct <= 30) return YLW;
    return GRN;
}
static const char *bat_glyph(int pct) {
    if (!NERD) return G->bat;
    int idx = pct >= 88 ? 0 : pct >= 63 ? 1 : pct >= 38 ? 2 : pct >= 13 ? 3 : 4;
    return BAT_RAMP[idx];
}

/* --------------------------------------------------------------- helpers -- */
/* Python round() / int(round(x)) is round-half-even; rint() matches. */
static long iround(double x) { return (long)rint(x); }

/* Bytes -> compact human string (no decimals for big units). */
static void human(double n, char *dst, size_t cap) {
    static const char *UNITS[] = {"B", "K", "M", "G", "T"};
    for (int i = 0; i < 5; i++) {
        if (n < 1024.0 || i == 4) {
            if (i == 0)           snprintf(dst, cap, "%ld%s", (long)n, UNITS[i]);
            else if (n < 10.0)    snprintf(dst, cap, "%.1f%s", n, UNITS[i]);
            else                  snprintf(dst, cap, "%ld%s", iround(n), UNITS[i]);
            return;
        }
        n /= 1024.0;
    }
}

static void gbs(double n, char *dst, size_t cap) { snprintf(dst, cap, "%.1fG", n / 1e9); }

/* ~ substitution + middle elision, ported from shorten().
 * snprintf truncation here is by design: pathological >250-char path parts
 * get clipped for display rather than overflowing.                          */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void shorten(const char *path, char *dst, size_t cap) {
    if (!path || !*path) { dst[0] = 0; return; }
    char p[1024];
    size_t hl = strlen(HOME);
    if (hl && strcmp(path, HOME) == 0) { snprintf(dst, cap, "~"); return; }
    if (hl && strncmp(path, HOME, hl) == 0 && path[hl] == '/')
        snprintf(p, sizeof p, "~%s", path + hl);
    else
        snprintf(p, sizeof p, "%s", path);
    if (strlen(p) <= PATH_MAXLEN) { snprintf(dst, cap, "%s", p); return; }
    /* split on '/': need >3 parts to elide */
    int nparts = 1;
    for (const char *q = p; *q; q++) if (*q == '/') nparts++;
    if (nparts <= 3) { snprintf(dst, cap, "%s", p); return; }
    /* head = first part, tail = last two parts */
    char *slash1 = strchr(p, '/');
    const char *tail = p + strlen(p);
    for (int seen = 0; tail > p; tail--)
        if (tail[-1] == '/' && ++seen == 2) break;
    snprintf(dst, cap, "%.*s/%s/%s", (int)(slash1 - p), p, u8"…", tail);
}
#pragma GCC diagnostic pop

/* Trailing-slash strip + '//' './' collapse — cheap normpath for equality. */
static void normalize(const char *src, char *dst, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < cap; i++) {
        char ch = src[i];
        if (ch == '/') {
            while (src[i + 1] == '/') i++;                       /* //  */
            while (src[i + 1] == '.' &&
                   (src[i + 2] == '/' || src[i + 2] == 0)) {     /* /./ */
                i += src[i + 2] ? 2 : 1;
                while (src[i + 1] == '/') i++;
            }
        }
        dst[j++] = ch;
    }
    while (j > 1 && dst[j - 1] == '/') j--;                      /* trailing */
    dst[j] = 0;
}

/* Seconds -> 2d13h / 3h8m / 8m / <1m / now (NAN -> "?"). */
static void fmt_delta(double seconds, char *dst, size_t cap) {
    if (isnan(seconds)) { snprintf(dst, cap, "?"); return; }
    long long s = (long long)seconds;   /* trunc toward zero, like int() */
    if (s <= 0) { snprintf(dst, cap, "now"); return; }
    long long d = s / 86400; s %= 86400;
    long long h = s / 3600;  s %= 3600;
    long long m = s / 60;
    if (d)      snprintf(dst, cap, "%lldd%lldh", d, h);
    else if (h) snprintf(dst, cap, "%lldh%lldm", h, m);
    else if (m) snprintf(dst, cap, "%lldm", m);
    else        snprintf(dst, cap, "<1m");
}

/* resets_at -> seconds until reset. Epoch (s or ms, number or numeric
 * string) or ISO8601 (Z / +00:00 assumed UTC). NAN when unparseable.        */
static double parse_reset(const cJSON *v) {
    if (!v) return NAN;
    double now = (double)time(NULL);
    if (cJSON_IsNumber(v)) {
        double n = v->valuedouble;
        if (n > 1e12) n /= 1000.0;
        return n - now;
    }
    if (!cJSON_IsString(v) || !v->valuestring) return NAN;
    const char *sv = v->valuestring;
    char *end = NULL;
    double n = strtod(sv, &end);
    if (end && end != sv && *end == 0) {
        if (n > 1e12) n /= 1000.0;
        return n - now;
    }
    struct tm tm = {0};
    end = strptime(sv, "%Y-%m-%dT%H:%M:%S", &tm);
    if (!end) return NAN;
    double frac = 0, off = 0;
    if (*end == '.') { char *e2; frac = strtod(end, &e2); end = e2; }
    if (*end == '+' || *end == '-') {
        int oh = 0, om = 0;
        if (sscanf(end + 1, "%2d:%2d", &oh, &om) >= 1)
            off = (end[0] == '-' ? -1 : 1) * (oh * 3600 + om * 60);
    }
    return (double)timegm(&tm) + frac - off - now;
}

/* -------------------------------------------------------------- json dig -- */
static const cJSON *jget(const cJSON *o, const char *k) {
    return o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
}
static const char *jstr(const cJSON *v) {
    return (v && cJSON_IsString(v) && v->valuestring && v->valuestring[0])
        ? v->valuestring : NULL;
}

/* --------------------------------------------------------- /proc, /sys ---- */
static bool read_battery(int *pct, bool *charging) {
    char best[64] = "", path[320], line[64];   /* 320 > dir prefix + d_name(255) + suffix */
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "BAT", 3) != 0 || strlen(e->d_name) >= sizeof best)
            continue;
        snprintf(path, sizeof path, "/sys/class/power_supply/%s/capacity", e->d_name);
        if (access(path, R_OK) != 0) continue;
        if (!best[0] || strcmp(e->d_name, best) < 0)
            snprintf(best, sizeof best, "%s", e->d_name);
    }
    closedir(d);
    if (!best[0]) return false;
    snprintf(path, sizeof path, "/sys/class/power_supply/%s/capacity", best);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fscanf(f, "%d", pct) == 1;
    fclose(f);
    if (!ok) return false;
    *charging = false;
    snprintf(path, sizeof path, "/sys/class/power_supply/%s/status", best);
    f = fopen(path, "r");
    if (f) {
        if (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            *charging = !strcmp(line, "Charging") || !strcmp(line, "Full");
        }
        fclose(f);
    }
    return true;
}

static bool read_mem(double *used_pct, double *avail_bytes) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return false;
    long long total = 0, avail = 0;
    char line[256];
    while (fgets(line, sizeof line, f) && (!total || !avail)) {
        long long v;
        if (sscanf(line, "MemTotal: %lld", &v) == 1)          total = v * 1024;
        else if (sscanf(line, "MemAvailable: %lld", &v) == 1) avail = v * 1024;
    }
    fclose(f);
    if (!total) return false;
    *used_pct = (double)(total - avail) / (double)total * 100.0;
    *avail_bytes = (double)avail;
    return true;
}

static bool read_cpu_snapshot(long long *total, long long *idle) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return false;
    char line[512];
    bool ok = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if (!ok || strncmp(line, "cpu", 3) != 0) return false;
    long long vals[16]; int n = 0;
    for (char *p = line + 3; n < 16; ) {
        char *end;
        long long v = strtoll(p, &end, 10);
        if (end == p) break;
        vals[n++] = v; p = end;
    }
    if (n < 4) return false;
    *idle = vals[3] + (n > 4 ? vals[4] : 0);   /* idle + iowait */
    *total = 0;
    for (int i = 0; i < n; i++) *total += vals[i];
    return true;
}

static bool skip_iface(const char *name) {
    return !strcmp(name, "lo") ||
           !strncmp(name, "veth", 4) || !strncmp(name, "docker", 6) ||
           !strncmp(name, "br-", 3)  || !strncmp(name, "vnet", 4);
}

static bool read_net_snapshot(long long *rx, long long *tx) {
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return false;
    char line[512];
    int lineno = 0;
    *rx = *tx = 0;
    while (fgets(line, sizeof line, f)) {
        if (++lineno <= 2) continue;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *iface = line;
        while (*iface == ' ') iface++;
        if (skip_iface(iface)) continue;
        long long fields[9]; int n = 0;
        for (char *p = colon + 1; n < 9; ) {
            char *end;
            long long v = strtoll(p, &end, 10);
            if (end == p) break;
            fields[n++] = v; p = end;
        }
        if (n >= 9) { *rx += fields[0]; *tx += fields[8]; }
    }
    fclose(f);
    return true;
}

/* Whole-disk device name matcher (nvmeXnY, sdX, vdX, mmcblkN, xvdX). */
static bool all_of(const char *s, int (*cls)(int)) {
    if (!*s) return false;
    for (; *s; s++) if (!cls((unsigned char)*s)) return false;
    return true;
}
static int is_lower_alpha(int ch) { return ch >= 'a' && ch <= 'z'; }
static int is_digit_c(int ch)     { return ch >= '0' && ch <= '9'; }
static bool is_whole_disk(const char *name) {
    if (!strncmp(name, "nvme", 4)) {
        const char *p = name + 4;
        if (!is_digit_c((unsigned char)*p)) return false;
        while (is_digit_c((unsigned char)*p)) p++;
        if (*p != 'n') return false;
        return all_of(p + 1, is_digit_c);
    }
    if (!strncmp(name, "sd", 2) || !strncmp(name, "vd", 2))
        return all_of(name + 2, is_lower_alpha);
    if (!strncmp(name, "mmcblk", 6))
        return all_of(name + 6, is_digit_c);
    if (!strncmp(name, "xvd", 3))
        return all_of(name + 3, is_lower_alpha);
    return false;
}

static bool read_io_snapshot(long long *rd, long long *wr) {
    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) return false;
    char line[512];
    *rd = *wr = 0;
    while (fgets(line, sizeof line, f)) {
        char name[64];
        long long fld[10];
        /* major minor name reads merged sectors_rd ms writes merged sectors_wr */
        int n = sscanf(line, "%lld %lld %63s %lld %lld %lld %lld %lld %lld %lld",
                       &fld[0], &fld[1], name,
                       &fld[3], &fld[4], &fld[5], &fld[6], &fld[7], &fld[8], &fld[9]);
        if (n < 10 || !is_whole_disk(name)) continue;
        *rd += fld[5] * 512;
        *wr += fld[9] * 512;
    }
    fclose(f);
    return true;
}

static bool read_disk(double *pct, double *avail_bytes) {
    struct statvfs st;
    if (statvfs("/", &st) != 0) return false;
    double total = (double)st.f_blocks * st.f_frsize;
    double freeb = (double)st.f_bfree  * st.f_frsize;
    double avail = (double)st.f_bavail * st.f_frsize;
    double used  = total - freeb;
    double denom = used + avail;
    *pct = denom > 0 ? used / denom * 100.0 : 0.0;
    *avail_bytes = avail;
    return true;
}

static double loadavg_pct(void) {
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return NAN;
    double one;
    bool ok = fscanf(f, "%lf", &one) == 1;
    fclose(f);
    if (!ok) return NAN;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    return one / (double)n * 100.0;
}

/* ------------------------------------------------------------ rate state -- */
/* Per-session state so concurrent Claude sessions don't stomp each other's
 * delta snapshots. Flat text, one line:
 *   ts  hc ct ci  hn rx tx  hi ior iow  cpu rxr txr iorr iowr
 * (h* = snapshot-present flags; rates are doubles, "nan" = absent)          */
typedef struct {
    double ts;
    bool has_cpu, has_net, has_io;
    long long ct, ci, rx, tx, ior, iow;
    double r_cpu, r_rx, r_tx, r_ior, r_iow;   /* cached rates, NAN = absent */
} State;

static void state_init(State *s) {
    memset(s, 0, sizeof *s);
    s->r_cpu = s->r_rx = s->r_tx = s->r_ior = s->r_iow = NAN;
}

static double now_monotonicish(void) {
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);
    return (double)tp.tv_sec + tp.tv_nsec / 1e9;
}

/* State goes to RAM-backed tmpfs, never disk: XDG_RUNTIME_DIR (user-private,
 * auto-wiped at logout), else /dev/shm. ~/.claude/cache is the last resort
 * on exotic systems with neither.                                           */
static void state_dir(char *dst, size_t cap) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt && access(rt, W_OK) == 0) { snprintf(dst, cap, "%s", rt); return; }
    if (access("/dev/shm", W_OK) == 0)      { snprintf(dst, cap, "/dev/shm"); return; }
    snprintf(dst, cap, "%s/.claude/cache", HOME);
}

static void state_file(const cJSON *root, char *dst, size_t cap) {
    const char *sid = jstr(jget(root, "session_id"));
    char clean[65], dir[256];
    size_t j = 0;
    if (sid) {
        for (size_t i = 0; sid[i] && j < 64; i++) {
            char ch = sid[i];
            bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                      (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
            clean[j++] = ok ? ch : '_';
        }
    }
    clean[j] = 0;
    if (!j) snprintf(clean, sizeof clean, "default");
    state_dir(dir, sizeof dir);
    snprintf(dst, cap, "%s/claude-statusline-state-%s.dat", dir, clean);
}

static void state_load(const char *path, State *s) {
    state_init(s);
    FILE *f = fopen(path, "r");
    if (!f) return;
    State t;
    state_init(&t);
    int hc, hn, hi;
    int n = fscanf(f, "%lf %d %lld %lld %d %lld %lld %d %lld %lld %lf %lf %lf %lf %lf",
                   &t.ts, &hc, &t.ct, &t.ci, &hn, &t.rx, &t.tx, &hi, &t.ior, &t.iow,
                   &t.r_cpu, &t.r_rx, &t.r_tx, &t.r_ior, &t.r_iow);
    fclose(f);
    if (n != 15) return;             /* corrupt/old format -> clean re-seed */
    t.has_cpu = hc; t.has_net = hn; t.has_io = hi;
    *s = t;
}

static void state_save(const char *path, const State *s) {
    char tmp[640];   /* path buffer (600) + ".tmp" */
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    /* O_EXCL: never follow a pre-existing symlink (matters for /dev/shm) */
    int fd = open(tmp, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) { unlink(tmp); fd = open(tmp, O_CREAT | O_EXCL | O_WRONLY, 0600); }
    if (fd < 0) return;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); return; }
    fprintf(f, "%.6f %d %lld %lld %d %lld %lld %d %lld %lld %.17g %.17g %.17g %.17g %.17g\n",
            s->ts, s->has_cpu, s->ct, s->ci, s->has_net, s->rx, s->tx,
            s->has_io, s->ior, s->iow,
            s->r_cpu, s->r_rx, s->r_tx, s->r_ior, s->r_iow);
    if (fclose(f) == 0) rename(tmp, path);
}

/* Compute cpu%, net rx/tx B/s, io r/w B/s from deltas vs saved state.
 * Reuses cached rates when the last sample is younger than RATE_MIN_INTERVAL. */
static void rate_metrics(const char *path, State *out_rates) {
    State st;
    state_load(path, &st);
    double now = now_monotonicish();
    double dt = now - st.ts;

    long long ct = 0, ci = 0, rx = 0, tx = 0, ior = 0, iow = 0;
    bool has_cpu = read_cpu_snapshot(&ct, &ci);
    bool has_net = read_net_snapshot(&rx, &tx);
    bool has_io  = read_io_snapshot(&ior, &iow);

    bool any_cached = !isnan(st.r_cpu) || !isnan(st.r_rx) || !isnan(st.r_tx) ||
                      !isnan(st.r_ior) || !isnan(st.r_iow);
    if (any_cached && dt >= 0 && dt < RATE_MIN_INTERVAL) {
        *out_rates = st;             /* reuse; leave file untouched */
        return;
    }

    State ns = st;                   /* rates carry forward, never blank out */
    if (has_cpu && st.has_cpu && dt > 0) {
        long long dtot = ct - st.ct, didle = ci - st.ci;
        if (dtot > 0) {
            double v = (1.0 - (double)didle / (double)dtot) * 100.0;
            ns.r_cpu = v < 0 ? 0 : v > 100 ? 100 : v;
        }
    }
    if (has_net && st.has_net && dt > 0) {
        ns.r_rx = fmax(0.0, (double)(rx - st.rx) / dt);
        ns.r_tx = fmax(0.0, (double)(tx - st.tx) / dt);
    }
    if (has_io && st.has_io && dt > 0) {
        ns.r_ior = fmax(0.0, (double)(ior - st.ior) / dt);
        ns.r_iow = fmax(0.0, (double)(iow - st.iow) / dt);
    }
    ns.ts = now;
    if (has_cpu) { ns.has_cpu = true; ns.ct = ct; ns.ci = ci; }
    if (has_net) { ns.has_net = true; ns.rx = rx; ns.tx = tx; }
    if (has_io)  { ns.has_io  = true; ns.ior = ior; ns.iow = iow; }
    state_save(path, &ns);
    *out_rates = ns;
}

/* ------------------------------------------------------------------ main -- */
static char *read_stdin(void) {
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, stdin)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            if (cap >= 1 << 20) break;      /* 1MB sanity cap */
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
    }
    buf[len] = 0;
    return buf;
}

/* usage segment: N%<reset>, % colored by use_color, reset dim, no space. */
static bool usage_seg(const cJSON *rl, const char *node, const char *glyph, SB *dst) {
    const cJSON *o = jget(rl, node);
    const cJSON *used = jget(o, "used_percentage");
    if (!used) used = jget(o, "usedPercentage");
    if (!cJSON_IsNumber(used)) return false;
    const cJSON *reset = jget(o, "resets_at");
    if (!reset) reset = jget(o, "resetsAt");
    char label[32];
    fmt_delta(parse_reset(reset), label, sizeof label);
    if (glyph[0]) sb_c(dst, DIM, "%s", glyph);
    sb_c(dst, use_color(used->valuedouble), "%ld%%", iround(used->valuedouble));
    sb_c(dst, DIM, "%s", label);
    return true;
}

int main(void) {
    HOME = getenv("HOME");
    if (!HOME) HOME = "";
    const char *nerd_env = getenv("CLAUDE_STATUSLINE_NERD");
    NERD = !(nerd_env && !strcmp(nerd_env, "0"));
    G = NERD ? &G_NERD : &G_PLAIN;

    char *raw = read_stdin();
    cJSON *root = raw ? cJSON_Parse(raw) : NULL;   /* NULL on garbage: fine */

    SB dirs = {0}, mid = {0}, sys_ = {0};

    /* --- dirs --- */
    const cJSON *ws = jget(root, "workspace");
    const char *current = jstr(jget(ws, "current_dir"));
    if (!current) current = jstr(jget(root, "cwd"));
    char cwdbuf[1024];
    if (!current) current = getcwd(cwdbuf, sizeof cwdbuf) ? cwdbuf : "";
    const char *launch = jstr(jget(ws, "project_dir"));
    if (!launch) launch = current;

    char cur_s[256], launch_s[256], n1[1024], n2[1024];
    shorten(current, cur_s, sizeof cur_s);
    normalize(current, n1, sizeof n1);
    normalize(launch, n2, sizeof n2);
    if (strcmp(n1, n2) != 0) {
        shorten(launch, launch_s, sizeof launch_s);
        sb_c(&dirs, DIM, "%s", G->launch);
        sb_raw(&dirs, " ");
        sb_c(&dirs, DIM, "%s", launch_s);
        sb_raw(&dirs, " ");
        sb_c(&dirs, DIM, "%s", G->arrow);
        sb_raw(&dirs, " ");
    }
    sb_c(&dirs, CYN, "%s", G->cur);
    sb_raw(&dirs, " ");
    sb_c(&dirs, BOLD, "%s", cur_s);

    /* --- model + context + usage --- */
    const cJSON *mdl = jget(root, "model");
    const char *model = jstr(jget(mdl, "display_name"));
    if (!model) model = jstr(jget(mdl, "id"));
    if (!model) model = "?";
    if (G->model[0]) { sb_c(&mid, "\033[35m", "%s", G->model); sb_raw(&mid, " "); }
    sb_c(&mid, BOLD, "%s", model);

    const cJSON *cw = jget(root, "context_window");
    const cJSON *ctx = jget(cw, "used_percentage");
    if (!ctx) ctx = jget(cw, "usedPercentage");
    if (cJSON_IsNumber(ctx)) {
        sb_raw(&mid, " ");
        if (G->ctx[0]) sb_c(&mid, DIM, "%s", G->ctx);
        sb_c(&mid, pct_color(ctx->valuedouble), "%ld%%", iround(ctx->valuedouble));
    }

    const cJSON *rl = jget(root, "rate_limits");
    SB seg = {0};
    if (usage_seg(rl, "five_hour", G->t5h, &seg)) {
        sb_raw(&mid, " %s", seg.buf);
    }
    seg.len = 0; seg.buf[0] = 0;
    if (usage_seg(rl, "seven_day", G->t7d, &seg)) {
        sb_raw(&mid, " %s", seg.buf);
    }

    /* --- system metrics --- */
    char sfile[600];
    state_file(root, sfile, sizeof sfile);
    State rates;
    rate_metrics(sfile, &rates);

    bool first = true;
#define SYS_SEP() do { if (!first) sb_raw(&sys_, " "); first = false; } while (0)

    int bpct; bool bchg;
    if (read_battery(&bpct, &bchg)) {
        SYS_SEP();
        const char *bc = bat_color(bpct, bchg);
        sb_c(&sys_, bc, "%s", bat_glyph(bpct));
        sb_raw(&sys_, " ");
        sb_c(&sys_, bc, "%d%%%s", bpct, bchg ? u8"⚡" : "");
    }

    double ram_pct, ram_free;
    if (read_mem(&ram_pct, &ram_free)) {
        SYS_SEP();
        char fb[32]; gbs(ram_free, fb, sizeof fb);
        sb_c(&sys_, DIM, "%s", G->ram);
        sb_raw(&sys_, " ");
        sb_c(&sys_, pct_color(ram_pct), "%ld%%", iround(ram_pct));
        sb_c(&sys_, DIM, "%s", fb);
    }

    double cpu = rates.r_cpu;
    if (isnan(cpu)) cpu = loadavg_pct();   /* first-render fallback */
    if (!isnan(cpu)) {
        SYS_SEP();
        sb_c(&sys_, DIM, "%s", G->cpu);
        sb_raw(&sys_, " ");
        sb_c(&sys_, pct_color(cpu), "%ld%%", iround(cpu));
    }

    double disk_pct, disk_free;
    if (read_disk(&disk_pct, &disk_free)) {
        SYS_SEP();
        char fb[32]; gbs(disk_free, fb, sizeof fb);
        sb_c(&sys_, DIM, "%s", G->disk);
        sb_raw(&sys_, " ");
        sb_c(&sys_, pct_color(disk_pct), "%ld%%", iround(disk_pct));
        sb_c(&sys_, DIM, "%s", fb);
    }

    if (!isnan(rates.r_ior) || !isnan(rates.r_iow)) {
        double ior = isnan(rates.r_ior) ? 0 : rates.r_ior;
        double iow = isnan(rates.r_iow) ? 0 : rates.r_iow;
        char hr[32], hw[32];
        human(ior, hr, sizeof hr);
        human(iow, hw, sizeof hw);
        SYS_SEP();
        sb_c(&sys_, DIM, "%s", G->io);
        sb_raw(&sys_, " ");
        sb_c(&sys_, DIM, "R");
        sb_c(&sys_, rate_color(ior), "%s", hr);
        sb_c(&sys_, DIM, "W");
        sb_c(&sys_, rate_color(iow), "%s", hw);
    }

    if (!isnan(rates.r_rx) || !isnan(rates.r_tx)) {
        double rx = isnan(rates.r_rx) ? 0 : rates.r_rx;
        double tx = isnan(rates.r_tx) ? 0 : rates.r_tx;
        char hr[32], ht[32];
        human(rx, hr, sizeof hr);
        human(tx, ht, sizeof ht);
        SYS_SEP();
        sb_c(&sys_, DIM, "%s", G->net);
        sb_raw(&sys_, " ");
        sb_c(&sys_, rate_color(rx), "%s%s", u8"↓", hr);
        sb_c(&sys_, rate_color(tx), "%s%s", u8"↑", ht);
    }

    /* --- assemble: segments joined by dim │ --- */
    fputs(dirs.buf, stdout);
    fputs(" " DIM u8"│" RST " ", stdout);
    fputs(mid.buf, stdout);
    if (sys_.len) {
        fputs(" " DIM u8"│" RST " ", stdout);
        fputs(sys_.buf, stdout);
    }

    cJSON_Delete(root);
    free(raw);
    return 0;
}
