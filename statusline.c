/*
 * Claude Code statusline — C port of statusline.py (byte-for-byte parity).
 *
 * Reads the status JSON on stdin, reads cheap system metrics from /proc and
 * /sys, prints one compact color-coded line (no trailing newline).
 *
 *   Segments:  launch>current dir | model ctx% 5h 7d | BAT RAM CPU DISK IO NET
 *
 * Single self-contained file - no dependencies beyond libc (JSON reader
 * included below).
 *
 * Build:   cc -O2 -Wall -Wextra -std=c11 -static -o statusline-bin statusline.c -lm
 * (or just `make`, which prefers musl-gcc for its ~40x faster process init)
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

static void sb_cat(SB *s, const char *src, size_t n) {   /* raw bytes, no fmt */
    if (n > sizeof s->buf - 1 - s->len) n = sizeof s->buf - 1 - s->len;
    memcpy(s->buf + s->len, src, n);
    s->len += n;
    s->buf[s->len] = 0;
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
static const char *bat_glyph(int pct, bool charging) {
    if (!NERD) return G->bat;
    if (charging) return u8"\U000F0084";   /* md battery-charging */
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

/* JSON reader (defined below) */
static const char *jget(const char *obj, const char *key);
static bool jstr(const char *v, char *dst, size_t cap);
static bool jnum(const char *v, double *out);

/* resets_at -> seconds until reset. Epoch (s or ms, number or numeric
 * string) or ISO8601 (Z / +00:00 assumed UTC). NAN when unparseable.
 * Takes a raw JSON value pointer (jget result).                             */
static double parse_reset(const char *v) {
    double now = (double)time(NULL);
    double n;
    if (jnum(v, &n)) {
        if (n > 1e12) n /= 1000.0;
        return n - now;
    }
    char sv[64];
    if (!jstr(v, sv, sizeof sv)) return NAN;
    char *end = NULL;
    n = strtod(sv, &end);
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

/* ------------------------------------------------------------------ json -- */
/* Minimal zero-allocation JSON reader. A "value" is a const char* pointing
 * at the value's first byte inside the raw stdin buffer; NULL means absent.
 * We only ever extract strings and numbers at known object paths, but the
 * skipper understands the full grammar (arrays, escapes, nesting) so unknown
 * fields never derail us. The whole document is validated first, so partial
 * garbage degrades to "no data" exactly like Python's json.loads.           */
#define JSON_MAX_DEPTH 64

static const char *jws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *jskip_str(const char *p) {   /* p at opening quote */
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p += 2;
        else p++;
    }
    return *p == '"' ? p + 1 : NULL;
}

/* Skip one value of any type; returns pointer past it, or NULL on error. */
static const char *jskip(const char *p, int depth) {
    if (!p || depth > JSON_MAX_DEPTH) return NULL;
    p = jws(p);
    if (*p == '"') return jskip_str(p);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = jws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (close == '}') {                 /* "key": */
                if (*p != '"') return NULL;
                p = jskip_str(p);
                if (!p) return NULL;
                p = jws(p);
                if (*p != ':') return NULL;
                p = jws(p + 1);
            }
            p = jskip(p, depth + 1);
            if (!p) return NULL;
            p = jws(p);
            if (*p == ',') { p = jws(p + 1); continue; }
            if (*p == close) return p + 1;
            return NULL;
        }
    }
    if (!strncmp(p, "true", 4))  return p + 4;
    if (!strncmp(p, "false", 5)) return p + 5;
    if (!strncmp(p, "null", 4))  return p + 4;
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        p++;
        while (*p && strchr("0123456789+-.eE", *p)) p++;
        return p;
    }
    return NULL;
}

/* Find `key` in the object at `obj`; -> value pointer or NULL. First match
 * wins (duplicate keys don't occur in this input). Keys compare raw, which
 * is exact for the plain-ASCII keys we look up.                             */
static const char *jget(const char *obj, const char *key) {
    if (!obj) return NULL;
    const char *p = jws(obj);
    if (*p != '{') return NULL;
    p = jws(p + 1);
    size_t klen = strlen(key);
    while (*p == '"') {
        const char *kstart = p + 1;
        const char *kend = jskip_str(p);
        if (!kend) return NULL;
        bool match = (size_t)(kend - 1 - kstart) == klen &&
                     !memcmp(kstart, key, klen);
        p = jws(kend);
        if (*p != ':') return NULL;
        p = jws(p + 1);
        if (match) return p;
        p = jskip(p, 0);
        if (!p) return NULL;
        p = jws(p);
        if (*p != ',') return NULL;
        p = jws(p + 1);
    }
    return NULL;
}

/* Decode a JSON string value into dst (handles \escapes, \uXXXX incl.
 * surrogate pairs -> UTF-8). False if absent, not a string, or too long.    */
static bool jstr(const char *v, char *dst, size_t cap) {
    if (!v) return false;
    v = jws(v);
    if (*v != '"') return false;
    v++;
    size_t j = 0;
    while (*v && *v != '"') {
        if (j + 5 >= cap) return false;
        if (*v != '\\') { dst[j++] = *v++; continue; }
        v++;
        switch (*v) {
            case '"':  dst[j++] = '"';  v++; break;
            case '\\': dst[j++] = '\\'; v++; break;
            case '/':  dst[j++] = '/';  v++; break;
            case 'b':  dst[j++] = '\b'; v++; break;
            case 'f':  dst[j++] = '\f'; v++; break;
            case 'n':  dst[j++] = '\n'; v++; break;
            case 'r':  dst[j++] = '\r'; v++; break;
            case 't':  dst[j++] = '\t'; v++; break;
            case 'u': {
                unsigned cp;
                if (sscanf(v + 1, "%4x", &cp) != 1) return false;
                v += 5;
                if (cp >= 0xD800 && cp <= 0xDBFF && v[0] == '\\' && v[1] == 'u') {
                    unsigned lo;
                    if (sscanf(v + 2, "%4x", &lo) == 1 &&
                        lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        v += 6;
                    }
                }
                if (cp < 0x80) dst[j++] = (char)cp;
                else if (cp < 0x800) {
                    dst[j++] = (char)(0xC0 | cp >> 6);
                    dst[j++] = (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    dst[j++] = (char)(0xE0 | cp >> 12);
                    dst[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    dst[j++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    dst[j++] = (char)(0xF0 | cp >> 18);
                    dst[j++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    dst[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    dst[j++] = (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: return false;
        }
    }
    if (*v != '"') return false;
    dst[j] = 0;
    return true;
}

/* Numeric value -> double. False for absent / true / false / null / string. */
static bool jnum(const char *v, double *out) {
    if (!v) return false;
    v = jws(v);
    if (!(*v == '-' || (*v >= '0' && *v <= '9'))) return false;
    *out = strtod(v, NULL);
    return true;
}

/* --------------------------------------------------------- /proc, /sys ---- */
/* Raw-syscall file readers: open/read/close into a caller buffer, no stdio.
 * Each stdio FILE costs a malloc (mmap/brk churn + page faults) plus extra
 * fstat/lseek syscalls per stream; a render opens ~10 files, so it adds up. */
static int rdfile(const char *path, char *buf, size_t cap) {  /* whole file */
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t len = 0;
    for (;;) {
        ssize_t n = read(fd, buf + len, cap - 1 - len);
        if (n <= 0) break;
        len += (size_t)n;
        if (len >= cap - 1) break;
    }
    close(fd);
    buf[len] = 0;
    return (int)len;
}
static int rdfirst(const char *path, char *buf, size_t cap) { /* one read() */
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = 0;
    return (int)n;
}
static const char *next_line(const char *p) {
    p = strchr(p, '\n');
    return p ? p + 1 : NULL;
}

/* Battery, with the sysfs name memoized in rate state: the warm path opens
 * capacity/status directly instead of re-scanning the power_supply dir every
 * render (getdents64 x2 + opens). bat: "" = unknown -> scan; "-" = scanned,
 * no battery (skip; a hotplugged battery shows after the state resets).
 * Scan picks the lexicographically first BAT* without probing readability -
 * multi-battery machines with an unreadable first battery would need the
 * old probe, single-battery laptops never do.                               */
struct State;
static bool read_battery(char *bat, size_t batcap, int *pct, bool *charging) {
    char path[352], buf[64];
    if (!strcmp(bat, "-")) return false;
    if (bat[0]) {
        snprintf(path, sizeof path, "/sys/class/power_supply/%s/capacity", bat);
        if (rdfirst(path, buf, sizeof buf) > 0) goto have;
        bat[0] = 0;                               /* stale name -> rescan */
    }
    {
        char best[32] = "";
        DIR *d = opendir("/sys/class/power_supply");
        if (!d) return false;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "BAT", 3) != 0 ||
                strlen(e->d_name) >= sizeof best)
                continue;
            if (!best[0] || strcmp(e->d_name, best) < 0)
                snprintf(best, sizeof best, "%s", e->d_name);
        }
        closedir(d);
        if (!best[0]) { snprintf(bat, batcap, "-"); return false; }
        snprintf(bat, batcap, "%s", best);
        snprintf(path, sizeof path, "/sys/class/power_supply/%s/capacity", bat);
        if (rdfirst(path, buf, sizeof buf) <= 0) return false;
    }
have:;
    char *end;
    long v = strtol(buf, &end, 10);
    if (end == buf) return false;
    *pct = (int)v;
    *charging = false;
    snprintf(path, sizeof path, "/sys/class/power_supply/%s/status", bat);
    if (rdfirst(path, buf, sizeof buf) > 0) {
        buf[strcspn(buf, "\n")] = 0;
        *charging = !strcmp(buf, "Charging") || !strcmp(buf, "Full");
    }
    return true;
}

static bool read_mem(double *used_pct, double *avail_bytes) {
    char buf[4096];   /* whole meminfo fits; needed lines are at the top */
    if (rdfirst("/proc/meminfo", buf, sizeof buf) <= 0) return false;
    long long total = 0, avail = 0;
    for (const char *p = buf; p && (!total || !avail); p = next_line(p)) {
        if (!strncmp(p, "MemTotal:", 9))           total = strtoll(p + 9, NULL, 10) * 1024;
        else if (!strncmp(p, "MemAvailable:", 13)) avail = strtoll(p + 13, NULL, 10) * 1024;
    }
    if (!total) return false;
    *used_pct = (double)(total - avail) / (double)total * 100.0;
    *avail_bytes = (double)avail;
    return true;
}

static bool read_cpu_snapshot(long long *total, long long *idle) {
    char buf[1024];   /* only need line 1; strtoll stops at "\ncpu0" anyway */
    if (rdfirst("/proc/stat", buf, sizeof buf) <= 0) return false;
    if (strncmp(buf, "cpu", 3) != 0) return false;
    long long vals[16]; int n = 0;
    for (const char *p = buf + 3; n < 16; ) {
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
    char buf[16384];   /* fits dozens of interfaces */
    if (rdfile("/proc/net/dev", buf, sizeof buf) <= 0) return false;
    int lineno = 0;
    *rx = *tx = 0;
    for (const char *p = buf; p; p = next_line(p)) {
        if (++lineno <= 2) continue;
        const char *nl = strchr(p, '\n');
        const char *colon = strchr(p, ':');
        if (!colon || (nl && colon > nl)) continue;
        const char *iface = p;
        while (*iface == ' ') iface++;
        char name[32];
        size_t ilen = (size_t)(colon - iface);
        if (ilen == 0 || ilen >= sizeof name) continue;
        memcpy(name, iface, ilen);
        name[ilen] = 0;
        if (skip_iface(name)) continue;
        long long fields[9]; int n = 0;
        for (const char *q = colon + 1; n < 9; ) {
            char *end;
            long long v = strtoll(q, &end, 10);
            if (end == q) break;
            fields[n++] = v; q = end;
        }
        if (n >= 9) { *rx += fields[0]; *tx += fields[8]; }
    }
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
    char buf[16384];
    if (rdfile("/proc/diskstats", buf, sizeof buf) <= 0) return false;
    *rd = *wr = 0;
    /* per line: major minor name reads merged sectors_rd ms writes merged
     * sectors_wr ... - we need sectors_rd (3rd after name) and sectors_wr
     * (7th after name)                                                      */
    for (const char *p = buf; p; p = next_line(p)) {
        char *end;
        const char *q = p;
        strtoll(q, &end, 10); if (end == q) continue; q = end;   /* major */
        strtoll(q, &end, 10); if (end == q) continue; q = end;   /* minor */
        while (*q == ' ') q++;
        const char *ns = q;
        while (*q && *q != ' ' && *q != '\n') q++;
        char name[64];
        size_t nlen = (size_t)(q - ns);
        if (nlen == 0 || nlen >= sizeof name) continue;
        memcpy(name, ns, nlen);
        name[nlen] = 0;
        if (!is_whole_disk(name)) continue;
        long long f[7];
        bool ok = true;
        for (int i = 0; i < 7; i++) {
            f[i] = strtoll(q, &end, 10);
            if (end == q) { ok = false; break; }
            q = end;
        }
        if (!ok) continue;
        *rd += f[2] * 512;
        *wr += f[6] * 512;
    }
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
    char buf[128], *end;
    if (rdfirst("/proc/loadavg", buf, sizeof buf) <= 0) return NAN;
    double one = strtod(buf, &end);
    if (end == buf) return NAN;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    return one / (double)n * 100.0;
}

/* ------------------------------------------------------------ rate state -- */
/* Per-session state so concurrent Claude sessions don't stomp each other's
 * delta snapshots. Flat text, one line:
 *   ts  hc ct ci  hn rx tx  hi ior iow  cpu rxr txr iorr iowr  bat
 * (h* = snapshot-present flags; rates are doubles, "nan" = absent; bat is
 * the memoized battery sysfs name, "-" = none found)                        */
typedef struct State {
    double ts;
    bool has_cpu, has_net, has_io;
    long long ct, ci, rx, tx, ior, iow;
    double r_cpu, r_rx, r_tx, r_ior, r_iow;   /* cached rates, NAN = absent */
    char bat[32];
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

static void state_file(const char *root, char *dst, size_t cap) {
    char sid[128];
    bool has_sid = jstr(jget(root, "session_id"), sid, sizeof sid) && sid[0];
    char clean[65], dir[256];
    size_t j = 0;
    if (has_sid) {
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
    char buf[512];
    if (rdfirst(path, buf, sizeof buf) <= 0) return;
    State t;
    state_init(&t);
    const char *p = buf;
    char *e;
    t.ts = strtod(p, &e); if (e == p) return; p = e;
    long long v[9];                        /* hc ct ci  hn rx tx  hi ior iow */
    for (int i = 0; i < 9; i++) {
        v[i] = strtoll(p, &e, 10); if (e == p) return; p = e;
    }
    double *r[5] = {&t.r_cpu, &t.r_rx, &t.r_tx, &t.r_ior, &t.r_iow};
    for (int i = 0; i < 5; i++) {
        *r[i] = strtod(p, &e); if (e == p) return; p = e;
    }
    t.has_cpu = v[0]; t.ct  = v[1]; t.ci  = v[2];
    t.has_net = v[3]; t.rx  = v[4]; t.tx  = v[5];
    t.has_io  = v[6]; t.ior = v[7]; t.iow = v[8];
    while (*p == ' ' || *p == '\n') p++;   /* optional battery-name token */
    size_t bl = 0;
    while (p[bl] && p[bl] != ' ' && p[bl] != '\n' && bl < sizeof t.bat - 1) bl++;
    if (bl) { memcpy(t.bat, p, bl); t.bat[bl] = 0; }
    *s = t;
}

static void state_save(const char *path, const State *s) {
    char tmp[640];   /* path buffer (600) + ".tmp" */
    char buf[512];
    int len = snprintf(buf, sizeof buf,
            "%.6f %d %lld %lld %d %lld %lld %d %lld %lld %.17g %.17g %.17g %.17g %.17g %s\n",
            s->ts, s->has_cpu, s->ct, s->ci, s->has_net, s->rx, s->tx,
            s->has_io, s->ior, s->iow,
            s->r_cpu, s->r_rx, s->r_tx, s->r_ior, s->r_iow,
            s->bat[0] ? s->bat : "-");
    if (len <= 0 || (size_t)len >= sizeof buf) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    /* O_EXCL: never follow a pre-existing symlink (matters for /dev/shm) */
    int fd = open(tmp, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) { unlink(tmp); fd = open(tmp, O_CREAT | O_EXCL | O_WRONLY, 0600); }
    if (fd < 0) return;
    bool ok = write(fd, buf, (size_t)len) == len;
    close(fd);
    if (ok) rename(tmp, path);
    else unlink(tmp);
}

/* Compute cpu%, net rx/tx B/s, io r/w B/s from deltas vs the prior snapshot,
 * updating *st in place. Returns true when it recomputed (state needs
 * saving), false on the reuse path (cached rates younger than
 * RATE_MIN_INTERVAL - no snapshot reads, no write).                         */
static bool rate_metrics(State *st) {
    double now = now_monotonicish();
    double dt = now - st->ts;

    /* Reuse check FIRST: Claude re-renders on events (keystrokes, tool calls),
     * not just the refresh timer. During bursts the reuse path hits and the
     * three snapshot reads below would be pure waste.                       */
    bool any_cached = !isnan(st->r_cpu) || !isnan(st->r_rx) || !isnan(st->r_tx) ||
                      !isnan(st->r_ior) || !isnan(st->r_iow);
    if (any_cached && dt >= 0 && dt < RATE_MIN_INTERVAL)
        return false;

    long long ct = 0, ci = 0, rx = 0, tx = 0, ior = 0, iow = 0;
    bool has_cpu = read_cpu_snapshot(&ct, &ci);
    bool has_net = read_net_snapshot(&rx, &tx);
    bool has_io  = read_io_snapshot(&ior, &iow);

    State old = *st;                 /* rates carry forward, never blank out */
    if (has_cpu && old.has_cpu && dt > 0) {
        long long dtot = ct - old.ct, didle = ci - old.ci;
        if (dtot > 0) {
            double v = (1.0 - (double)didle / (double)dtot) * 100.0;
            st->r_cpu = v < 0 ? 0 : v > 100 ? 100 : v;
        }
    }
    if (has_net && old.has_net && dt > 0) {
        st->r_rx = fmax(0.0, (double)(rx - old.rx) / dt);
        st->r_tx = fmax(0.0, (double)(tx - old.tx) / dt);
    }
    if (has_io && old.has_io && dt > 0) {
        st->r_ior = fmax(0.0, (double)(ior - old.ior) / dt);
        st->r_iow = fmax(0.0, (double)(iow - old.iow) / dt);
    }
    st->ts = now;
    if (has_cpu) { st->has_cpu = true; st->ct = ct; st->ci = ci; }
    if (has_net) { st->has_net = true; st->rx = rx; st->tx = tx; }
    if (has_io)  { st->has_io  = true; st->ior = ior; st->iow = iow; }
    return true;
}

/* ------------------------------------------------------------------ main -- */
/* Static stdin buffer: .bss pages fault in only as data touches them, and
 * there's no malloc/mmap churn. After the first read() we check whether we
 * already hold a complete JSON document (Claude writes it in one go) and
 * skip the EOF-confirming second read.                                      */
static char RAW[262144];
static size_t read_stdin_raw(void) {
    size_t len = 0;
    for (;;) {
        ssize_t n = read(0, RAW + len, sizeof RAW - 1 - len);
        if (n <= 0) break;
        len += (size_t)n;
        if (len >= sizeof RAW - 1) break;
        RAW[len] = 0;
        const char *p = jws(RAW);
        if (*p == '{') {
            const char *end = jskip(p, 0);
            if (end && *jws(end) == 0) break;     /* complete doc - done */
        }
    }
    RAW[len] = 0;
    return len;
}

/* usage segment: N%<reset>, % colored by use_color, reset dim, no space. */
static bool usage_seg(const char *rl, const char *node, const char *glyph, SB *dst) {
    const char *o = jget(rl, node);
    const char *used = jget(o, "used_percentage");
    if (!used) used = jget(o, "usedPercentage");
    double uv;
    if (!jnum(used, &uv)) return false;
    const char *reset = jget(o, "resets_at");
    if (!reset) reset = jget(o, "resetsAt");
    char label[32];
    fmt_delta(parse_reset(reset), label, sizeof label);
    if (glyph[0]) sb_c(dst, DIM, "%s", glyph);
    sb_c(dst, use_color(uv), "%ld%%", iround(uv));
    sb_c(dst, DIM, "%s", label);
    return true;
}

int main(void) {
    HOME = getenv("HOME");
    if (!HOME) HOME = "";
    const char *nerd_env = getenv("CLAUDE_STATUSLINE_NERD");
    NERD = !(nerd_env && !strcmp(nerd_env, "0"));
    G = NERD ? &G_NERD : &G_PLAIN;

    read_stdin_raw();
    /* Accept the document only if it parses completely (like json.loads). */
    const char *root = NULL;
    {
        const char *p = jws(RAW);
        if (*p == '{') {
            const char *end = jskip(p, 0);
            if (end && *jws(end) == 0) root = p;
        }
    }

    SB dirs = {0}, mid = {0}, sys_ = {0};

    /* --- dirs --- */
    const char *ws = jget(root, "workspace");
    char curbuf[2048], launchbuf[2048], cwdbuf[1024];
    const char *current = NULL, *launch = NULL;
    if (jstr(jget(ws, "current_dir"), curbuf, sizeof curbuf) && curbuf[0])
        current = curbuf;
    if (!current && jstr(jget(root, "cwd"), curbuf, sizeof curbuf) && curbuf[0])
        current = curbuf;
    if (!current) current = getcwd(cwdbuf, sizeof cwdbuf) ? cwdbuf : "";
    if (jstr(jget(ws, "project_dir"), launchbuf, sizeof launchbuf) && launchbuf[0])
        launch = launchbuf;
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
    const char *mdl = jget(root, "model");
    char modelbuf[256];
    const char *model = NULL;
    if (jstr(jget(mdl, "display_name"), modelbuf, sizeof modelbuf) && modelbuf[0])
        model = modelbuf;
    if (!model && jstr(jget(mdl, "id"), modelbuf, sizeof modelbuf) && modelbuf[0])
        model = modelbuf;
    if (!model) model = "?";
    if (G->model[0]) { sb_c(&mid, "\033[35m", "%s", G->model); sb_raw(&mid, " "); }
    sb_c(&mid, BOLD, "%s", model);

    const char *cw = jget(root, "context_window");
    const char *ctx = jget(cw, "used_percentage");
    if (!ctx) ctx = jget(cw, "usedPercentage");
    double ctxv;
    if (jnum(ctx, &ctxv)) {
        sb_raw(&mid, " ");
        if (G->ctx[0]) sb_c(&mid, DIM, "%s", G->ctx);
        sb_c(&mid, pct_color(ctxv), "%ld%%", iround(ctxv));
    }

    const char *rl = jget(root, "rate_limits");
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
    state_load(sfile, &rates);
    bool recomputed = rate_metrics(&rates);
    char oldbat[32];
    memcpy(oldbat, rates.bat, sizeof oldbat);

    bool first = true;
#define SYS_SEP() do { if (!first) sb_raw(&sys_, " "); first = false; } while (0)

    int bpct; bool bchg;
    if (read_battery(rates.bat, sizeof rates.bat, &bpct, &bchg)) {
        SYS_SEP();
        const char *bc = bat_color(bpct, bchg);
        sb_c(&sys_, bc, "%s", bat_glyph(bpct, bchg));
        sb_raw(&sys_, " ");
        sb_c(&sys_, bc, "%d%%", bpct);
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

    /* Persist state when rates were recomputed or the battery name was just
     * (re)discovered; the burst reuse path writes nothing.                  */
    if (recomputed || memcmp(oldbat, rates.bat, sizeof oldbat) != 0)
        state_save(sfile, &rates);

    /* --- assemble and emit in a single write(): segments joined by dim │,
     * no stdout stdio (avoids its lazy-init ioctl/fcntl and writev)         */
    static const char SEG_SEP[] = " " DIM u8"│" RST " ";
    SB out = {0};
    sb_cat(&out, dirs.buf, dirs.len);
    sb_cat(&out, SEG_SEP, sizeof SEG_SEP - 1);
    sb_cat(&out, mid.buf, mid.len);
    if (sys_.len) {
        sb_cat(&out, SEG_SEP, sizeof SEG_SEP - 1);
        sb_cat(&out, sys_.buf, sys_.len);
    }
    size_t off = 0;
    while (off < out.len) {
        ssize_t n = write(1, out.buf + off, out.len - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    return 0;
}
