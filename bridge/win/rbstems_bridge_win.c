/*
 * rekordbox stems cache bridge, Windows
 *
 * The Windows fork of ../rbstems_bridge.c (the macOS bridge): the same cache, the same matching,
 * the same config.ini grammar, marker, bridge-state and log, ported to Win32 (win_compat.h). Read
 * the macOS file's opening comment for how the cache works; what differs here:
 *
 * Stands in for rekordbox's onnxruntime.dll. Every call goes to the real library (Pioneer's,
 * unmodified, kept outside rekordbox's folder: see REAL_PATH), except Run() on the Demucs session
 * (the model file named hdemucs.onnx), whose results are cached on disk as 16-bit FLAC.
 *
 * Cache:  %LOCALAPPDATA%\rbstemsplus\Cache\<model sha256>\<key[0:2]>\<key>.flac and .desc, or, chosen
 *         in the app's Settings, the same layout in "RB Stems Plus Cache" on an external drive.
 * Config: %APPDATA%\rbstemsplus\config.ini   (written by the app; the same keys as on macOS)
 * State:  %LOCALAPPDATA%\rbstemsplus\Logs\bridge-state
 * Log:    %LOCALAPPDATA%\rbstemsplus\Logs\bridge.log (over 5 MB it becomes bridge.log.1)
 * Home:   the account's known folders (SHGetKnownFolderPath), never environment variables.
 * Real:   %ProgramData%\rbstemsplus\ort\onnxruntime.dll, loaded only when nobody but Administrators,
 *         SYSTEM and TrustedInstaller can change it or its two parent folders (the installer sets
 *         those ACLs: ProgramData lets every user create files by default).
 * Space:  the floor is the same: 10% of the disk, at most 50 GB (GetDiskFreeSpaceExW).
 *
 * Paths are UTF-8 inside the bridge and UTF-16 at the Win32 boundary. Files are opened relative to
 * their folder's handle (NtCreateFile), never following a link or junction at the last component.
 */
#define __USE_MINGW_ANSI_STDIO 1
#include "win_compat.h"
#include <FLAC/metadata.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#include <ctype.h>
#include <math.h>
#include <process.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <strings.h>

/* MSVC's SAL annotations the ONNX Runtime header uses on Windows; empty for mingw */
#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#include "onnxruntime_c_api.h"

#define FORMAT "rbstems-v1"
#define EXPORT __declspec(dllexport)

#define DESC_FRAME 4410                 /* summary resolution: 0.1 s */
#define DESC_MAX_FRAMES 512
#define NEAR_MAX 0.15                   /* summary distance (mean |log10 rms| difference) */
#define NEAR_CANDIDATES 3
#define SHIFT_MAX 4096                  /* time shift searched when verifying, in samples */
#define SHIFT_ACCEPT 1024               /* ...and accepted: rekordbox's chunks overlap by 2048 */
#define GAIN_SLACK 0.02
#define M_TOTAL 1.0                     /* dB a near chunk's whole residual may exceed the entry's */
#define M_FRAME 3.0                     /* ...and each frame's */
#define R_FLOOR (-35.0)                 /* a frame residual below this always passes */
#define FRAME_FLOOR 1e-6                /* frames quieter than -60 dBFS (mean square) aren't judged */

/* exported so the bridge can be told from Pioneer's library (the app looks for "rbstems bridge: ") */
EXPORT const char rbstems_marker[] = "rbstems-bridge";

/* rekordbox's own models whose stems this bridge rebuilds and caches (STEMS Engine 0002), and the
 * version of that rebuild. A new rebuild (a fix, a change of maths) takes a new REBUILD_VERSION. */
#define REBUILD_VERSION "1"
#define PIONEER_0002 "435c987855f7ea74ff5f20090541748cbbce5a2e8e501d07b10f4e703453215a"
static const char *const pioneer_models[] = {PIONEER_0002};
/* what this bridge can cache, for the app, which reads it from the file (keep in step with
 * pioneer_models): "rbstems-cache: rebuild=<v> location=1 kept=1 pioneer=<sha>[,<sha>];". location=1: it reads
 * cache_dir and cache_id (a chosen location). New tokens go before pioneer=, whose list runs to ';'. */
__attribute__((used)) static const char rbstems_caps[] = "rbstems-cache: rebuild=" REBUILD_VERSION " location=1 kept=1 pioneer=" PIONEER_0002 ";";
/* dB: rebuilt stems must explain the chunk at least this well (chunks over -60 dBFS, FRAME_FLOOR).
 * Pioneer's model doesn't make its stems add up exactly: on rumble techno (Ignez, SMV015, 171
 * chunks) its worst chunk was -17.9 dB, most below -24; a wrong rebuild gives -6 dB or worse. */
#define REBUILT_RESIDUAL_MAX (-10.0)

static const OrtApiBase *real_base;
static HMODULE real_handle;             /* the real library */
static const OrtApi *R;                 /* the real library's functions */
static OrtApi api;                      /* what rekordbox gets: R with three entries replaced */
static rb_once_t once = RB_ONCE_INIT;
static rb_mutex lock = RB_MUTEX_INIT;
static rb_mutex hash_lock = RB_MUTEX_INIT;
static rb_queue *writer;                /* store_entry only */
static rb_queue *watchdog;              /* GPU runs' deadlines */
static rb_queue *maint;                 /* validate, index loads, prune: never on rekordbox's threads */
#define CACHE_TAIL "\\rbstemsplus\\Cache"
#define CACHE_FOLDER "RB Stems Plus Cache"   /* a chosen location's folder (the app makes it) */
#define MARKER ".rbstemsplus-cache"     /* in it: id=<UUID>, format=rbstems-v1 (the app writes it, once) */
#define MARKER_MAX 4096
#define LOCATION_MAX 700                /* bytes in a chosen location's path */
#define DEEPEST 160                     /* the longest name under the cache: <model>\\<xx>\\._<key>.desc.<pid>.tmp */
static char base_dir[RB_PATH_MAX - DEEPEST]; /* where the stems are ("": none), short enough for every file in it */
static char memo_dir[RB_PATH_MAX - DEEPEST]; /* the model-hash memos: always ~/Library/Caches/rbstemsplus */
static char config_path[RB_PATH_MAX];
static char kept_dir[RB_PATH_MAX];          /* the kept sets' folder ("": none) */
static char log_path[RB_PATH_MAX];
static char state_path[RB_PATH_MAX];       /* bridge-state, for the app's light */
static int chosen;                      /* base_dir is a chosen location, on an external drive */
static char cache_id[37];               /* ...whose marker carries this id */
static int enabled = 1;
static int rekordbox_model = 0;         /* this account caches rekordbox's own (listed) models too */
/* Near hits (a similar chunk's stems, verified) are off unless config.ini says near_hits=1: in real
 * use rekordbox decodes a file the same way every time, so its reloads are exact hits (the owner's
 * log: 72 hits, 0 near hits, 77 candidates rejected, all neighbouring chunks of the same track),
 * and the index costs a load at every launch, memory in rekordbox and reads before every miss. The
 * .desc files are still written, so a future pre-cache (inexact decodes) can turn it on with every
 * entry saved meanwhile. A test build takes RBSTEMS_TEST_NEAR_HITS as the default. */
static int near_on = 0;                /* config.ini near_hits */
static int gpu_on = 1;                  /* config.ini gpu: rekordbox's own Demucs model runs on the GPU (DirectML), on by default */
static int cuda_on = 1;                 /* config.ini cuda: our Demucs v4 model on an NVIDIA GPU through a CUDA ONNX Runtime (ort-cuda folder), if one is installed */
static int gpu_v4_on = 0;              /* config.ini gpu_v4: our Demucs v4 model too (experimental: it exhausts an 8 GB GPU), off by default */
static long long max_bytes = 20LL << 30;
static double max_days = 60;
static long long total_bytes = -1;      /* -1: not counted yet */
static int space_paused = -1;           /* under lock: -1 not measured yet (space_ok) */
static int writer_checked;              /* under lock: this writer batch checked the marker (store_entry) */
static long long hits, near_hits, misses, writes;
static void lose(const char *why);
static int repin(uint64_t old_dev, uint64_t old_ino);

/* ---------------------------------------------------------------- log, config */

#define LOG_MAX (5 << 20)               /* bridge.log over this becomes bridge.log.1 (one kept) */
static rb_mutex log_lock = RB_MUTEX_INIT;

/* snprintf for paths: a path that doesn't fit is "" and -1, never a shorter path (a truncated
 * cache path could name the home folder or ~/Library) */
__attribute__((format(printf, 3, 4)))
static int pathf(char *out, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    if (r >= 0 && (size_t)r < n) return 0;
    out[0] = 0;
    return -1;
}

/* fopen for writing that won't follow a symlink planted at the path */
static FILE *open_w(const char *path, int flags, const char *mode) {
    int fd = rb_open(path, flags | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return NULL;
    char m[4] = {mode[0], 'b', 0};                          /* bytes as written: "\n", never "\r\n" */
    FILE *f = fdopen(fd, m);
    if (!f) close(fd);
    return f;
}

static void logf_(const char *fmt, ...) {
    if (!log_path[0]) return;
    rb_lock(&log_lock);                          /* one rotation, whole lines */
    struct rb_stat st;
    if (rb_stat(log_path, &st) == 0 && st.st_size > LOG_MAX) {
        char old[RB_PATH_MAX + 2];
        if (!pathf(old, sizeof old, "%s.1", log_path)) rb_rename(log_path, old);
    }
    FILE *f = open_w(log_path, O_WRONLY | O_APPEND, "a");
    if (f) {
        time_t t = time(NULL);
        struct tm tmv;
        char ts[32];
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", (localtime_s(&tmv, &t), &tmv));
        fprintf(f, "%s  ", ts);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    rb_unlock(&log_lock);
}

static void mkdirs(const char *path) {
    char tmp[RB_PATH_MAX];
    if (pathf(tmp, sizeof tmp, "%s", path)) return;
    char *p = tmp;
    if (isalpha((unsigned char)tmp[0]) && tmp[1] == ':' && tmp[2] == '\\') p = tmp + 3;   /* not the drive itself */
    for (; *p; p++)
        if (*p == '\\') { *p = 0; rb_mkdir(tmp); *p = '\\'; }
    rb_mkdir(tmp);
}

/* 1 or 0 for a yes/no value, -1 when it's neither */
static int yes_no(const char *v) {
    if (!strcmp(v, "0") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcasecmp(v, "false")) return 0;
    if (!strcmp(v, "1") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcasecmp(v, "true")) return 1;
    return -1;
}

/* config.ini, written by the app. The same grammar as the app's reader (app/Sources/Config.swift);
 * bridge/tests/run.sh gives both readers the same fixtures:
 *  - the whole file at once: at most CONFIG_MAX bytes, valid UTF-8, no NUL. A file that breaks
 *    that, or that can't be read for any reason but not existing, is unreadable: nothing is cached
 *    (the app says so, and won't rewrite it). No file: the defaults, stems saved on this Mac.
 *  - a UTF-8 byte order mark only at offset 0; lines end at "\n" and hold at most CONFIG_LINE_MAX
 *    bytes (a "\r" counts); leading spaces, tabs and "\r" are skipped; '#' starts a comment line.
 *  - ours are the top (before any [section]) and [cache]: the name between '[' and the first ']',
 *    spaces and tabs around it and case ignored. Other sections are someone else's ([watcher] has
 *    an enabled of its own). An over-long section line leaves its scope unknown: not ours.
 *  - key=value: the key is what comes before the first '=', case ignored; the value is quoted
 *    ("..." or '...', ending at its own quote) or ends at the first space, tab, "\r" or '#'. An
 *    over-long line's key still counts, never its value.
 *  - enabled, rekordbox_model, max_gb, max_days: unknown keys are ignored; a bad or out-of-range
 *    value is ignored with a log line, and so is an over-long line.
 *  - cache_dir and cache_id: a chosen location (location_path_ok, uuid_ok). Either key in our
 *    scope means one is chosen, and then both must be there, valid, and the same on every line,
 *    or nothing is cached: a mistake never means this Mac. So does an over-long section line in a
 *    file that has either key anywhere (its scope can't be told). */
#define CONFIG_MAX 65536
#define CONFIG_LINE_MAX 1024

typedef struct {
    int enabled, rekordbox_model, near_hits, gpu, gpu_v4, cuda;
    long long max_bytes;
    double max_days;
    int location;                       /* 0: this Mac; 1: a chosen location; -1: one is set, wrongly */
    char dir[LOCATION_MAX + 1], id[37];
} config;

static int utf8_ok(const unsigned char *s, size_t n) {     /* shortest forms, no surrogates, at most U+10FFFF */
    for (size_t i = 0; i < n;) {
        unsigned c = s[i], cp, min;
        size_t k;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) { k = 1; cp = c & 0x1F; min = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { k = 2; cp = c & 0x0F; min = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { k = 3; cp = c & 0x07; min = 0x10000; }
        else return 0;
        if (n - i <= k) return 0;
        for (size_t j = 1; j <= k; j++) {
            if ((s[i + j] & 0xC0) != 0x80) return 0;
            cp = cp << 6 | (s[i + j] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += k + 1;
    }
    return 1;
}

/* a chosen location's path: a drive letter's absolute path ("E:\\..."), at most LOCATION_MAX
 * bytes, plain components (none empty, "." or "..", none ending in a dot or a space), none of the
 * characters Windows refuses in names (<>:"/|?* and control characters), and ending in our folder's
 * name; at the drive's root is allowed ("E:\\RB Stems Plus Cache"). No "\\\\?\\", no UNC. */
static int location_path_ok(const char *p) {
    size_t n = strlen(p), tail = strlen("\\" CACHE_FOLDER);
    if (!isalpha((unsigned char)p[0]) || p[1] != ':' || p[2] != '\\') return 0;
    if (n > LOCATION_MAX || n < 2 + tail || strcmp(p + n - tail, "\\" CACHE_FOLDER)) return 0;
    for (const char *c = p + 3; *c; c++)
        if ((unsigned char)*c < 0x20 || *c == 0x7F || strchr("<>:\"/|?*", *c)) return 0;
    for (const char *c = p + 2; c;) {                       /* c: at a '\\' */
        const char *e = strchr(c + 1, '\\');
        size_t l = e ? (size_t)(e - c - 1) : strlen(c + 1);
        if (l == 0 || (l == 1 && c[1] == '.') || (l == 2 && c[1] == '.' && c[2] == '.')) return 0;
        if (c[l] == '.' || c[l] == ' ') return 0;          /* the component's last character */
        c = e;
    }
    return 1;
}

static int uuid_ok(const char *s) {                         /* 8-4-4-4-12 hex digits, either case */
    if (strlen(s) != 36) return 0;
    for (int i = 0; i < 36; i++)
        if (i == 8 || i == 13 || i == 18 || i == 23 ? s[i] != '-' : !isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

static int is_hex(const char *s, size_t n) {               /* lowercase, as hex() writes */
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

static int blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* the C locale, for numbers: the app's reader always takes '.' as the decimal mark */
static _locale_t c_loc;
static void c_locale_init(void) { c_loc = _create_locale(LC_ALL, "C"); }
static _locale_t c_locale(void) {
    static rb_once_t o = RB_ONCE_INIT;
    rb_once(&o, c_locale_init);
    return c_loc;
}

/* config.ini's bytes into c (which holds the defaults); -1: unreadable (a NUL, invalid UTF-8) */
static int parse_config(const char *b, size_t n, config *c) {
    if (memchr(b, 0, n) || !utf8_ok((const unsigned char *)b, n)) return -1;
    int ours = 1, long_section = 0, loc_anywhere = 0, loc_lines = 0, bad = 0, have_dir = 0, have_id = 0;
    char val[CONFIG_LINE_MAX + 1];
    size_t i = n >= 3 && !memcmp(b, "\xEF\xBB\xBF", 3) ? 3 : 0;
    while (i < n) {
        const char *s = b + i, *nl = memchr(s, '\n', n - i), *end = nl ? nl : b + n, *p = s;
        size_t len = (size_t)(end - s);
        i += len + 1;
        int over = len > CONFIG_LINE_MAX;
        while (p < end && blank(*p)) p++;
        if (p == end || *p == '#') continue;
        if (over) logf_("config: ignored a line over %d bytes: %.*s", CONFIG_LINE_MAX, (int)(end - p < 40 ? end - p : 40), p);
        if (*p == '[') {                                    /* [section] */
            const char *a = p + 1, *q = over ? NULL : memchr(p, ']', (size_t)(end - p));
            long_section |= over;
            if (q) {
                while (a < q && (*a == ' ' || *a == '\t')) a++;
                while (q > a && (q[-1] == ' ' || q[-1] == '\t')) q--;
            }
            ours = q && q - a == 5 && !strncasecmp(a, "cache", 5);
            continue;
        }
        const char *eq = memchr(p, '=', (size_t)(end - p)), *ke = eq;
        if (!eq) continue;
        while (ke > p && blank(ke[-1])) ke--;
        char key[16];
        size_t kl = (size_t)(ke - p);
        if (kl >= sizeof key) continue;                     /* none of ours is that long */
        for (size_t j = 0; j < kl; j++) key[j] = (char)tolower((unsigned char)p[j]);
        key[kl] = 0;
        int loc = !strcmp(key, "cache_dir") || !strcmp(key, "cache_id");
        loc_anywhere |= loc;
        if (!ours) continue;
        loc_lines += loc;
        if (over) { bad |= loc; continue; }
        const char *v = eq + 1, *ve, *q;
        while (v < end && (*v == ' ' || *v == '\t')) v++;
        if (v < end && (*v == '"' || *v == '\'') && (q = memchr(v + 1, *v, (size_t)(end - v - 1)))) { v++; ve = q; }
        else for (ve = v; ve < end && !blank(*ve) && *ve != '#'; ve++) {}
        memcpy(val, v, (size_t)(ve - v));
        val[ve - v] = 0;
        if (!strcmp(key, "cache_dir")) {
            if (!location_path_ok(val) || (have_dir && strcmp(c->dir, val))) bad = 1;
            else { memcpy(c->dir, val, strlen(val) + 1); have_dir = 1; }
        } else if (!strcmp(key, "cache_id")) {
            if (!uuid_ok(val) || (have_id && strcasecmp(c->id, val))) bad = 1;
            else { memcpy(c->id, val, 37); have_id = 1; }
        } else if (!strcmp(key, "enabled") || !strcmp(key, "rekordbox_model") || !strcmp(key, "near_hits") || !strcmp(key, "gpu") || !strcmp(key, "gpu_v4") || !strcmp(key, "cuda")) {
            int yn = yes_no(val), *dst = !strcmp(key, "enabled") ? &c->enabled : !strcmp(key, "near_hits") ? &c->near_hits : !strcmp(key, "gpu") ? &c->gpu
                                       : !strcmp(key, "gpu_v4") ? &c->gpu_v4 : !strcmp(key, "cuda") ? &c->cuda : &c->rekordbox_model;
            if (yn >= 0) *dst = yn;
            else logf_("config: ignored %s=%.40s (allowed 1 or 0)", key, val);
        } else if (!strcmp(key, "max_gb") || !strcmp(key, "max_days")) {
            int gb = key[4] == 'g';
            double lo = gb ? 0.1 : 1, hi = gb ? 100000 : 36500;
            char *e;
            double x = _strtod_l(val, &e, c_locale());               /* '.' whatever rekordbox's locale */
            if (!*val || isspace((unsigned char)*val) || *e || !(x >= lo && x <= hi)) logf_("config: ignored %s=%.40s (allowed %g to %g)", key, val, lo, hi);
            else if (gb) c->max_bytes = (long long)(x * (1LL << 30));
            else c->max_days = x;
        }
    }
    int unknown = long_section && loc_anywhere;
    c->location = !loc_lines && !unknown ? 0 : bad || !have_dir || !have_id || unknown ? -1 : 1;
    return 0;
}

/* reads config.ini into c; -1 when it exists but can't be read (see above) */
static int read_config(config *c) {
    if (!config_path[0]) return 0;
    int fd = rb_open(config_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct rb_stat st;
    char *b = NULL;
    ssize_t n = -1, r = 0;
    if (rb_fstat(fd, &st) == 0 && RB_ISREG(st.st_mode) && st.st_size <= CONFIG_MAX && (b = malloc(CONFIG_MAX + 1))) {
        size_t got = 0;
        while (got <= CONFIG_MAX && (r = read(fd, b + got, CONFIG_MAX + 1 - got)) > 0) got += (size_t)r;
        n = r < 0 || got > CONFIG_MAX ? -1 : (ssize_t)got;
    }
    close(fd);
    int ok = n >= 0 && parse_config(b, (size_t)n, c) == 0;
    free(b);
    return ok ? 0 : -1;
}

/* ---------------------------------------------------------------- where the stems are */

/* This Mac (base_dir is the default folder, memo_dir) or a chosen location: CACHE_FOLDER on an
 * external drive, whose marker's id is cache_id. A chosen location is checked off rekordbox's
 * threads (validate, the first job on the maintenance queue); until then it is pending and
 * rekordbox's stems pass through. Losing it (lose) is a failure of the location itself, never a
 * file missing below it. While it is unavailable it is checked again every RECHECK_EVERY seconds
 * (recheck: the same check as at launch; with the drive gone only the mount table is read), and
 * used again once it passes: a drive reconnected, repaired or allowed needs no restart. The
 * sessions rekordbox opens at launch are registered either way. The app learns the state from
 * bridge-state. */
enum { LOC_PENDING, LOC_AVAILABLE, LOC_UNAVAILABLE };
static int loc_state = LOC_UNAVAILABLE;     /* under lock; without base_dir never available */
static const char *loc_reason = "";         /* why unavailable: a code the app knows (bridge-state) */
static uint64_t pin_dev;                       /* under lock: base_dir as the last check found it, a */
static uint64_t pin_ino;                       /* cheap same-folder check (the drive's identity is its marker) */
#define MNAMELEN 300
static char mount_dir[MNAMELEN];            /* under lock: the chosen location's volume ("E:\\") */
static uint64_t mount_serial;               /* under lock: ...and its serial number */
#define RECHECK_EVERY 10                    /* seconds between checks of an unavailable chosen location */
static int recheck_armed;                   /* under lock: a recheck is scheduled */

static int loc_now(void) {
    rb_lock(&lock);
    int s = loc_state;
    rb_unlock(&lock);
    return s;
}

static int test_any_volume(void) {          /* a test build may put a chosen location on any volume */
#ifdef RBSTEMS_TEST_REAL_PATH
    return getenv("RBSTEMS_TEST_ANY_VOLUME") != NULL;
#else
    return 0;
#endif
}

/* Test builds only: pauses and failures at chosen moments, for bridge/tests (a drive detached
 * mid-write, mid-prune, mid-index-load; a write failing with a given errno; an open failing once) */
#ifdef RBSTEMS_TEST_REAL_PATH
static void test_pause(const char *name) {
    const char *t = getenv(name);
    if (t) rb_sleep_seconds(atof(t));
}
static int test_errno(const char *name) {
    const char *t = getenv(name);
    return t ? atoi(t) : 0;
}
static _Thread_local int test_open_errno;   /* the next open_base on this thread fails with it */
#else
#define test_pause(name) ((void)0)
#define test_errno(name) 0
#endif

static const char *reason_text(const char *r) {
    return !strcmp(r, "config") ? "config.ini can't be read, or sets it wrongly"
         : !strcmp(r, "disabled") ? "Stems Cache is off (enabled=0), or isn't hooked into this rekordbox"
         : !strcmp(r, "not-connected") ? "its drive isn't connected"
         : !strcmp(r, "folder") ? "its folder is missing, or something on the way to it isn't a real folder"
         : !strcmp(r, "marker") ? "its folder's marker is missing or another folder's"
         : !strcmp(r, "read-only") ? "the drive is read-only"
         : !strcmp(r, "not-local") ? "the drive isn't connected to this PC (a network drive)"
         : !strcmp(r, "no-permission") ? "no permission (another account's folder)"
         : !strcmp(r, "write-failed") ? "writing failed"
         : "the drive doesn't answer (input/output error)";
}

/* when this process started (seconds since 1970): with the pid, the app tells this rekordbox from
 * a later process that got the same pid */
static long process_start(void) { return rb_process_start(); }

/* bridge-state, next to the log: what this rekordbox does with the location. The app's light
 * follows it while this process runs (app/Sources/Location.swift reads it: its keys are a contract). */
static void write_state(void) {
    static rb_mutex m = RB_MUTEX_INIT;
    static const char *names[] = {"pending", "available", "unavailable"};
    char tmp[RB_PATH_MAX + 32];
    if (!state_path[0] || pathf(tmp, sizeof tmp, "%s.%d.tmp", state_path, getpid())) return;
    rb_lock(&m);
    rb_lock(&lock);
    int s = loc_state;
    const char *why = s == LOC_AVAILABLE ? "" : loc_reason;
    rb_unlock(&lock);
    FILE *f = open_w(tmp, O_WRONLY | O_TRUNC, "w");
    if (f) {
        int ok = fprintf(f, "pid=%d\nstart=%ld\nlocation=%s\nstate=%s\nreason=%s\n", getpid(), process_start(),
                         chosen ? base_dir : "", names[s], why) > 0;
        ok = fclose(f) == 0 && ok;
        if (!ok || rb_rename(tmp, state_path) != 0) rb_unlink(tmp);
    }
    rb_unlock(&m);
}

static const char *why_of(int e) {
    return e == ENOENT || e == ENOTDIR || e == ELOOP || e == EXDEV ? "folder"
         : e == EACCES || e == EPERM ? "no-permission"
         : e == EROFS ? "read-only" : "io-error";
}

/* why a chosen location's folder failed: its drive gone, or the folder itself */
static const char *gone_why(int e) {
    char mnt[MNAMELEN];
    uint64_t want, ser = 0;
    rb_lock(&lock);
    rb_strlcpy(mnt, mount_dir, sizeof mnt);
    want = mount_serial;
    rb_unlock(&lock);
    if (!test_any_volume() && (rb_volume_of(mnt, NULL, 0, &ser, NULL, 0, NULL) != 0 || ser != want)) return "not-connected";
    return why_of(e);
}

static int read_all(int fd, void *buf, size_t n) {          /* exactly n bytes, or -1 */
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { if (r == 0) errno = EIO; return -1; }
        got += (size_t)r;
    }
    return 0;
}

/* opens a file to read without blocking on a FIFO or following a link, and only if it is a regular
 * file of at most max bytes (else -1, *err EFTYPE) */
static int checked_open(int dir, const char *name, long long max, long long *size, int *err) {
    int fd = rb_openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { *err = errno; return -1; }
    struct rb_stat st;
    if (rb_fstat(fd, &st) != 0 || !RB_ISREG(st.st_mode) || st.st_size > max) { *err = EFTYPE; close(fd); return -1; }
    if (size) *size = st.st_size;
    return fd;
}

/* whether the folder fd is stored under exactly this name: on a volume that ignores case,
 * rb_openat("ab") also opens "AB", which isn't ours */
static int name_is(int fd, const char *name) { return rb_name_is(fd, name); }

/* <model> or <xx> in dir, made first if `create`: a real folder (no link), on the same volume and
 * named exactly name; -1 with *err otherwise (EXDEV: another volume, or another name's case) */
static int sub_open(int dir, const char *name, int create, uint64_t dev, int *err) {
    static int said;
    if (create && rb_mkdirat(dir, name) != 0 && errno != EEXIST) { *err = errno; return -1; }
    int fd = rb_openat(dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { *err = errno; return -1; }
    struct rb_stat st;
    if (rb_fstat(fd, &st) != 0 || st.st_dev != dev || !RB_ISDIR(st.st_mode) || !name_is(fd, name)) {
        if (!said) { said = 1; logf_("cache: a folder that matches %s only by ignoring case (or on another volume) is left alone", name); }
        *err = EXDEV;
        close(fd);
        return -1;
    }
    return fd;
}

/* base_dir, opened for one operation and closed at its end (short descriptors don't stop an
 * eject). A chosen location must be the folder the last check pinned: failing that is a loss. This
 * Mac's folder is made again when `create` (Clear deletes it). -1 with *err otherwise. */
static int open_base(int create, int *err) {
    if (!base_dir[0]) { *err = ENOENT; return -1; }
    if (!chosen && create) mkdirs(base_dir);
    int fd = rb_open(base_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    *err = fd < 0 ? errno : 0;
#ifdef RBSTEMS_TEST_REAL_PATH
    if (test_open_errno && fd >= 0) { close(fd); fd = -1; *err = test_open_errno; }
    test_open_errno = 0;
#endif
    if (!chosen) return fd;
    struct rb_stat st;
    rb_lock(&lock);
    uint64_t dev = pin_dev;
    uint64_t ino = pin_ino;
    rb_unlock(&lock);
    if (fd < 0) {
        /* a failure of the location itself; a shortage of descriptors or memory in rekordbox is
         * only this lookup's (a miss), never a loss for the run */
        int e = *err;
        if (e == ENOENT || e == ENOTDIR || e == ELOOP || e == ENXIO || e == ENODEV || e == EIO || e == EACCES || e == EPERM || e == EROFS)
            lose(gone_why(e));
    } else if (rb_fstat(fd, &st) != 0 || st.st_dev != dev || st.st_ino != ino) {
        /* a drive connected again has new device numbers: checked at once (repin), and used
         * when the folder opened is the one that check pinned */
        int ok = 0;
        if (repin(dev, ino)) {
            rb_lock(&lock);
            dev = pin_dev;
            ino = pin_ino;
            rb_unlock(&lock);
            ok = rb_fstat(fd, &st) == 0 && st.st_dev == dev && st.st_ino == ino;
        }
        if (!ok) {
            close(fd);
            fd = -1;
            *err = EXDEV;
            lose("folder");
        }
    }
    return fd;
}

/* the prefix folder of an entry, base_dir/<model>/<key[0:2]>, made first if `create` */
static int entry_dir(const char *model, const char *key, int create, int *err) {
    struct rb_stat st;
    char xx[3] = {key[0], key[1], 0};
    int b = open_base(create, err), m = -1, x = -1;
    if (b < 0) return -1;
    if (rb_fstat(b, &st) == 0 && (m = sub_open(b, model, create, st.st_dev, err)) >= 0) x = sub_open(m, xx, create, st.st_dev, err);
    else if (m < 0 && !*err) *err = errno;
    if (m >= 0) close(m);
    close(b);
    return x;
}

/* the marker's text: an id= line equal to cache_id and a format= line equal to FORMAT, and no
 * other value of either */
static int marker_ok(const char *b, size_t n, const char *want) {
    int id = 0, fmt = 0;
    if (memchr(b, 0, n)) return 0;
    for (size_t i = 0; i < n;) {
        const char *s = b + i, *nl = memchr(s, '\n', n - i);
        size_t len = nl ? (size_t)(nl - s) : n - i;
        i += len + 1;
        if (len > 3 && !strncmp(s, "id=", 3)) {
            if (len != 39 || strncasecmp(s + 3, want, 36)) return 0;
            id = 1;
        } else if (len > 7 && !strncmp(s, "format=", 7)) {
            if (len != 7 + strlen(FORMAT) || strncmp(s + 7, FORMAT, strlen(FORMAT))) return 0;
            fmt = 1;
        }
    }
    return id && fmt;
}

/* NULL when base_dir (open as fd) holds our marker, else why not; "" when it couldn't be told
 * for want of descriptors or memory (not the location's fault: no verdict) */
static const char *marker_check(int fd) {
    char buf[MARKER_MAX];
    long long size = 0;
    int err = 0, m = checked_open(fd, MARKER, MARKER_MAX, &size, &err);
    if (m < 0) return err == EACCES || err == EPERM ? "no-permission" : err == EIO || err == ENXIO || err == ENODEV ? "io-error"
                    : err == EMFILE || err == ENFILE || err == ENOMEM || err == EINTR || err == EAGAIN ? "" : "marker";
    int r = read_all(m, buf, (size_t)size);
    close(m);
    if (r) return "io-error";
    return marker_ok(buf, (size_t)size, cache_id) ? NULL : "marker";
}

/* A chosen location is still the folder validate pinned, with our marker (re-read at each prune,
 * each index load and the first write of each writer batch): else it is lost. 0 when it is. */
static int location_recheck(void) {
    if (!chosen) return 0;
    int err, fd = open_base(0, &err);
    if (fd < 0) return -1;
    const char *why = marker_check(fd);
    close(fd);
    if (why && *why) { lose(why); return -1; }
    return why ? -1 : 0;                                    /* "": not now, but not lost */
}

/* Whether the chosen location can be used (NULL, with base_dir's volume serial and file index in
 * *dev and *ino, its volume's root in mnt and serial in *ser), else why not. Its volume: the one
 * GetVolumePathName finds for base_dir, an external drive (removable, or on a USB, 1394 or SD bus)
 * and never the one Windows runs from; a network drive is "not-local". From the root down, every
 * component a real folder (not a link or junction: lstat) on that volume; the folder holds our
 * marker; the volume and the folder are writable. Nothing is ever created here: the app makes the
 * folder and the marker. */
static const char *check_location(uint64_t *pdev, uint64_t *pino, char mnt[MNAMELEN], uint64_t *pser) {
    int any = test_any_volume();
    char fsname[64] = "";
    DWORD vflags = 0;
    uint64_t ser = 0;
    if (rb_volume_of(base_dir, mnt, MNAMELEN, &ser, fsname, sizeof fsname, &vflags)) return "not-connected";
    *pser = ser;
    {
        wchar_t *w = rb_wide(mnt);
        UINT t = w ? GetDriveTypeW(w) : DRIVE_UNKNOWN;
        free(w);
        if (t == DRIVE_REMOTE && !any) return "not-local";
        if (t == DRIVE_NO_ROOT_DIR || t == DRIVE_UNKNOWN) return "not-connected";
    }
    if (!any && (rb_volume_system(mnt) || !rb_volume_external(mnt))) return "not-connected";
    struct rb_stat st;
    char p[sizeof base_dir];
    size_t ml = strlen(mnt);
    if (rb_lstat(mnt, &st) != 0) return errno == ENOENT || errno == ENOTDIR ? "not-connected" : why_of(errno);
    uint64_t dev = st.st_dev;
    rb_strlcpy(p, base_dir, sizeof p);
    if (ml < 3 || _strnicmp(p, mnt, ml)) return "folder";              /* mnt ends in '\\' */
    for (char *s = p + ml;;) {
        char *e = strchr(s, '\\');
        if (e) *e = 0;
        if (rb_lstat(p, &st) != 0) return errno == ENOENT ? "folder" : why_of(errno);
        if (!RB_ISDIR(st.st_mode) || (!any && st.st_dev != dev)) return "folder";
        if (!e) break;
        *e = '\\';
        s = e + 1;
    }
    if (vflags & FILE_READ_ONLY_VOLUME) return "read-only";
    int fd = rb_open(base_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return why_of(errno);
    const char *why = rb_fstat(fd, &st) != 0 ? "io-error" : marker_check(fd);
    if (why && !*why) why = "io-error";                     /* checked once: no descriptors is a failure here */
    *pdev = st.st_dev;
    *pino = st.st_ino;
    int werr = 0;
    if (!why && !rb_dir_writable(fd, &werr)) why = werr == EROFS ? "read-only" : "no-permission";
    close(fd);
    return why;
}

static void log_using(const char *how) {
    char root[MNAMELEN], fsname[64] = "?";
    double fr = 0, tot = 0;
    if (!rb_volume_of(base_dir, root, sizeof root, NULL, fsname, sizeof fsname, NULL) && !rb_space(base_dir, &fr, &tot))
        logf_("cache: %s %s (%s on %s, %.1f GB free of %.1f GB)", how, base_dir, fsname, root, fr / 1e9, tot / 1e9);
    else logf_("cache: %s %s", how, base_dir);
}

static unsigned recheck_every(void) {
#ifdef RBSTEMS_TEST_REAL_PATH
    const char *t = getenv("RBSTEMS_TEST_RECHECK_SECONDS");
    if (t) return (unsigned)atoi(t);
#endif
    return RECHECK_EVERY;
}

/* schedules recheck while the chosen location is unavailable (not while Stems Cache is off) */
static void recheck(void);
static void recheck_job(void *ctx) { (void)ctx; recheck(); }
static void arm_recheck(void) {
    rb_lock(&lock);
    int go = chosen && !recheck_armed && loc_state == LOC_UNAVAILABLE && strcmp(loc_reason, "disabled");
    if (go) recheck_armed = 1;
    rb_unlock(&lock);
    if (go && rb_after(maint, recheck_every(), recheck_job, NULL)) {
        rb_lock(&lock);
        recheck_armed = 0;
        rb_unlock(&lock);
    }
}

static void validate(void) {
    test_pause("RBSTEMS_TEST_VALIDATE_DELAY");
    uint64_t dev = 0;
    uint64_t ino = 0;
    char mnt[MNAMELEN] = "";
    uint64_t ser = 0;
    const char *why = check_location(&dev, &ino, mnt, &ser);
    rb_lock(&lock);
    loc_state = why ? LOC_UNAVAILABLE : LOC_AVAILABLE;
    loc_reason = why ? why : "";
    pin_dev = dev;
    pin_ino = ino;
    rb_strlcpy(mount_dir, mnt, sizeof mount_dir);
    mount_serial = ser;
    rb_unlock(&lock);
    write_state();
    if (why) logf_("cache: %s not available (%s): stems aren't saved or loaded until it is; checked again every %u seconds", base_dir, reason_text(why), recheck_every());
    else log_using("using");
    arm_recheck();
}

static void first_prune(int tries);

/* The chosen location, unavailable, checked again as at launch (maintenance queue). Once it
 * passes it is used again: the space it takes is counted afresh (first_prune), and with near hits
 * on the index is loaded again by the next miss. Logged when it's back, or when the reason changes. */
static void recheck(void) {
    rb_lock(&lock);
    recheck_armed = 0;
    int still = loc_state == LOC_UNAVAILABLE;
    rb_unlock(&lock);
    if (!still) return;
    uint64_t dev = 0;
    uint64_t ino = 0;
    char mnt[MNAMELEN] = "";
    uint64_t ser = 0;
    const char *why = check_location(&dev, &ino, mnt, &ser);
    int back = 0, changed = 0;
    rb_lock(&lock);
    if (loc_state == LOC_UNAVAILABLE) {
        if (!why) {
            loc_state = LOC_AVAILABLE;
            loc_reason = "";
            pin_dev = dev;
            pin_ino = ino;
            rb_strlcpy(mount_dir, mnt, sizeof mount_dir);
            mount_serial = ser;
            total_bytes = -1;
            space_paused = -1;
            writer_checked = 0;                             /* the next write checks the marker again */
            back = 1;
        } else if (strcmp(why, loc_reason)) {
            loc_reason = why;
            changed = 1;
        }
    }
    rb_unlock(&lock);
    if (back) {
        write_state();
        log_using("back: using");
        first_prune(0);
        return;
    }
    if (changed) {
        write_state();
        logf_("cache: %s is still unavailable, now because %s", base_dir, reason_text(why));
    }
    arm_recheck();
}

/* ---------------------------------------------------------------- loading the real library */

/* Pioneer's library, unmodified, outside rekordbox's folder, where only administrators can write:
 * rekordbox's folder then holds Pioneer's files but this one, and uninstalling puts Pioneer's back.
 * Only this path, never the environment: a path from the environment would let any process put
 * code into rekordbox. RBSTEMS_TEST_REAL_PATH exists for bridge/win/tests (build.sh --test)
 * only; a release build refuses it. The release path is under %ProgramData% (FOLDERID_ProgramData),
 * read at load time; REAL_TAIL is what follows it. */
#ifdef RBSTEMS_TEST_REAL_PATH
#define BUILD_NOTE ", TEST BUILD"
#else
#define BUILD_NOTE ""
#endif
#define REAL_TAIL "\\rbstemsplus\\ort\\onnxruntime.dll"
static char real_path[RB_PATH_MAX];
static HMODULE self_module;

static int hooks_ok; /* the real library is the ONNX Runtime 1.18 whose OrtApi layout we patch */

/* the SIDs that may change the real library: Administrators, SYSTEM, TrustedInstaller (and, in a
 * test build, the account running the tests) */
static int trusted_sid(PSID sid) {
    if (!sid || !IsValidSid(sid)) return 0;
    if (IsWellKnownSid(sid, WinBuiltinAdministratorsSid) || IsWellKnownSid(sid, WinLocalSystemSid)) return 1;
    static const char *const ti = "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";   /* NT SERVICE\TrustedInstaller */
    LPSTR str = NULL;
    int ok = 0;
    if (ConvertSidToStringSidA(sid, &str)) { ok = !strcmp(str, ti); LocalFree(str); }
#ifdef RBSTEMS_TEST_REAL_PATH
    if (!ok) {
        HANDLE tok;
        union { TOKEN_USER u; unsigned char b[256]; } tu;
        DWORD got;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
            if (GetTokenInformation(tok, TokenUser, &tu, sizeof tu, &got)) ok = EqualSid(sid, tu.u.User.Sid);
            CloseHandle(tok);
        }
    }
#endif
    return ok;
}

/* Whether only trusted SIDs can change path: its owner is one, and no ACE that applies to the
 * object itself allows anyone else to write, append, delete, change its permissions or owner, or
 * (a folder) add or delete what is in it. A NULL DACL (everyone may do anything) fails. */
static int acl_ok(const char *path) {
    wchar_t *w = rb_wide(path);
    PSID owner = NULL;
    PACL dacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    int ok = 0;
    if (w && GetNamedSecurityInfoW(w, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, NULL, &dacl, NULL, &sd)
                 == ERROR_SUCCESS) {
        ok = trusted_sid(owner) && dacl;
        const ACCESS_MASK writes = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD
                                   | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED;
        for (DWORD i = 0; ok && i < dacl->AceCount; i++) {
            ACE_HEADER *h;
            if (!GetAce(dacl, i, (LPVOID *)&h)) { ok = 0; break; }
            if (h->AceFlags & INHERIT_ONLY_ACE) continue;
            if (h->AceType == ACCESS_ALLOWED_ACE_TYPE) {
                ACCESS_ALLOWED_ACE *a = (ACCESS_ALLOWED_ACE *)h;
                if ((a->Mask & writes) && !trusted_sid((PSID)&a->SidStart)) ok = 0;
            } else if (h->AceType != ACCESS_DENIED_ACE_TYPE) {
                ok = 0;                                     /* callback or object ACEs: not on a plain file */
            }
        }
    }
    if (sd) LocalFree(sd);
    free(w);
    return ok;
}

static int only_root_can_change(const char *path) {
    char real[RB_PATH_MAX], dir[RB_PATH_MAX];
    if (rb_realpath(path, real, sizeof real)) return 1; /* missing: LoadLibrary says so */
    if (!acl_ok(real)) {
        logf_("ERROR real library: %s can be changed by others than administrators: not loaded", real);
        return 0;
    }
    if (pathf(dir, sizeof dir, "%s", real)) return 0;
    for (int level = 0; level < 2; level++) {
        char *slash = strrchr(dir, '\\');
        if (!slash || slash <= dir + 2) break;              /* not the drive's root */
        *slash = 0;
        if (!acl_ok(dir)) {
            logf_("ERROR real library: its folder %s can be changed by others than administrators: not loaded", dir);
            return 0;
        }
    }
    return 1;
}

static HMODULE open_real(const char *path) {
    if (!only_root_can_change(path)) return NULL;
    wchar_t *w = rb_wide(path);
    /* its own dependencies from its folder, rekordbox's folder (DirectML.dll) and System32 */
    HMODULE h = w ? LoadLibraryExW(w, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32) : NULL;
    DWORD e = GetLastError();
    free(w);
    if (!h) { logf_("real library: cannot load %s: error %lu", path, (unsigned long)e); return NULL; }
    if (h == self_module || GetProcAddress(h, "rbstems_marker")) {  /* handed back this bridge */
        logf_("ERROR real library: %s resolved to the bridge itself", path);
        if (h != self_module) FreeLibrary(h);
        return NULL;
    }
    if (!GetProcAddress(h, "OrtGetApiBase")) { logf_("ERROR real library: %s has no OrtGetApiBase", path); FreeLibrary(h); return NULL; }
    return h;
}

/* a known folder as UTF-8; a test build takes RBSTEMS_TEST_HOME instead when it is set ("" = no
 * home): <home>\AppData\Local, <home>\AppData\Roaming, <home>\ProgramData */
static int known_folder(REFKNOWNFOLDERID id, const char *test_tail, char *out, size_t n) {
#ifdef RBSTEMS_TEST_REAL_PATH
    const char *home = getenv("RBSTEMS_TEST_HOME");
    if (home) return home[0] && home[1] == ':' ? pathf(out, n, "%s\\%s", home, test_tail) : -1;
#endif
    (void)test_tail;
    PWSTR w = NULL;
    int r = -1;
    if (SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, NULL, &w) == S_OK) r = rb_utf8(w, -1, out, n);
    CoTaskMemFree(w);
    if (r == 0 && !(isalpha((unsigned char)out[0]) && out[1] == ':' && out[2] == '\\')) r = -1;   /* a drive letter's path only */
    return r;
}

/* the config, log and default cache paths in the account's known folders; a path that doesn't fit
 * is "" (off). Nothing is created here but the log's folder. */
static void set_paths(void) {
    char local[RB_PATH_MAX], roaming[RB_PATH_MAX], dir[RB_PATH_MAX];
    if (known_folder(&FOLDERID_LocalAppData, "AppData\\Local", local, sizeof local)) return;   /* no home: no log, no config, no cache */
    if (!pathf(dir, sizeof dir, "%s\\rbstemsplus\\Logs", local) && !pathf(log_path, sizeof log_path, "%s\\bridge.log", dir)) {
        mkdirs(dir);
        pathf(state_path, sizeof state_path, "%s\\bridge-state", dir);
    }
    if (!known_folder(&FOLDERID_RoamingAppData, "AppData\\Roaming", roaming, sizeof roaming)) {
        pathf(config_path, sizeof config_path, "%s\\rbstemsplus\\config.ini", roaming);
        pathf(kept_dir, sizeof kept_dir, "%s\\rbstemsplus\\kept", roaming);
    }
    size_t len, tail = strlen(CACHE_TAIL);
    if (pathf(memo_dir, sizeof memo_dir, "%s" CACHE_TAIL, local) || (len = strlen(memo_dir)) <= tail
        || strcmp(memo_dir + len - tail, CACHE_TAIL)) {
        memo_dir[0] = 0;
        logf_("ERROR the local app data folder's path is too long for the cache (%zu characters): nothing is cached", strlen(local));
    }
}

/* Where the stems go, from config.ini: this PC's folder, a chosen location (pending until
 * validate has checked it), or nowhere (a config.ini that can't be read or sets the location
 * wrongly: never this PC instead). Then the folders: the default one, which keeps the model-hash
 * memos in either case; a chosen location is never created by the bridge. */
static void set_location(void) {
    config c = {.enabled = 1, .rekordbox_model = 0, .near_hits = 0, .gpu = 1, .gpu_v4 = 0, .cuda = 1, .max_bytes = 20LL << 30, .max_days = 60};
#ifdef RBSTEMS_TEST_REAL_PATH
    if (getenv("RBSTEMS_TEST_NEAR_HITS")) c.near_hits = atoi(getenv("RBSTEMS_TEST_NEAR_HITS")) != 0;
#endif
    int unreadable = read_config(&c);
    enabled = c.enabled;
    rekordbox_model = c.rekordbox_model;
    near_on = c.near_hits;
    gpu_on = c.gpu;
    gpu_v4_on = c.gpu_v4;
    cuda_on = c.cuda;
    max_bytes = c.max_bytes;
    max_days = c.max_days;
    if (!memo_dir[0]) return;                               /* no home for the cache: nothing cached */
    if (unreadable || c.location < 0) {
        loc_reason = "config";
        logf_("ERROR config: %s: nothing is cached until it's fixed (RB Stems Plus > Settings)",
              unreadable ? "config.ini can't be read" : "where stems are saved (cache_dir, cache_id) is set wrongly");
    } else if (c.location) {
        chosen = 1;
        memcpy(base_dir, c.dir, strlen(c.dir) + 1);
        memcpy(cache_id, c.id, 37);
        loc_state = LOC_PENDING;
    } else {
        memcpy(base_dir, memo_dir, sizeof base_dir);
        loc_state = LOC_AVAILABLE;
    }
    mkdirs(memo_dir);
}

static void validate_job(void *ctx) { (void)ctx; validate(); }

static void load_real(void) {
    set_paths();
#ifdef RBSTEMS_TEST_REAL_PATH
    pathf(real_path, sizeof real_path, "%s", RBSTEMS_TEST_REAL_PATH);
#else
    {
        PWSTR w = NULL;
        char pd[RB_PATH_MAX];
        if (SHGetKnownFolderPath(&FOLDERID_ProgramData, KF_FLAG_DONT_VERIFY, NULL, &w) == S_OK && !rb_utf8(w, -1, pd, sizeof pd))
            pathf(real_path, sizeof real_path, "%s" REAL_TAIL, pd);
        CoTaskMemFree(w);
    }
#endif
    HMODULE h = real_path[0] ? open_real(real_path) : NULL;
    if (!h) { logf_("ERROR no real ONNX Runtime library: rekordbox's analysis will fail; reinstall or uninstall"); return; }
    real_handle = h;
    const OrtApiBase *(ORT_API_CALL *get)(void) = (const OrtApiBase *(ORT_API_CALL *)(void))(void *)GetProcAddress(h, "OrtGetApiBase");
    real_base = get();
    const char *ver = real_base ? real_base->GetVersionString() : "";
    hooks_ok = real_base && strncmp(ver, "1.18.", 5) == 0;
    set_location();
    writer = rb_queue_create(0);
    maint = rb_queue_create(1);
    watchdog = rb_queue_create(0);
    if (!writer || !maint || !watchdog) { hooks_ok = 0; logf_("ERROR could not start the bridge's threads: passing everything through"); }
    logf_("bridge loaded (real library %s from %s%s; cache %s%s, enabled=%d, max_gb=%.1f, max_days=%g, rekordbox_model=%d, rebuild %s%s)",
          real_base ? ver : "MISSING", real_path,
          hooks_ok ? "" : ", NOT 1.18.x: passing everything through", base_dir[0] ? base_dir : "NONE",
          !chosen ? "" : enabled && hooks_ok ? " (chosen in Settings: checking it)" : " (chosen in Settings: not used, Stems Cache is off here)", enabled,
          max_bytes / (double)(1LL << 30), max_days, rekordbox_model, REBUILD_VERSION, BUILD_NOTE);
    if (chosen) logf_("cache: the model's checksums are remembered in %s", memo_dir);
    if (!near_on) logf_("cache: near hits off (near_hits=0): only exact hits are served");
    write_state();
    /* a drive is touched only when Stems Cache will use it: off (enabled=0) or passing everything
     * through (not ORT 1.18), the chosen location is never checked, never woken */
    if (chosen && enabled && hooks_ok) rb_async(maint, validate_job, NULL);
    else if (chosen) {
        rb_lock(&lock);
        loc_state = LOC_UNAVAILABLE;
        loc_reason = "disabled";
        rb_unlock(&lock);
        write_state();
    }
}

/* ---------------------------------------------------------------- Demucs sessions */

#define MAX_SESSIONS 16
/* model: what names the session's cache folder and goes into its keys (the model's checksum, or
 * for a rebuilt model rebuilt_id); rebuild: the session's stems are rebuilt from x (rebuild_spec) */
static struct {
    OrtSession *s; char model[65]; int rebuild;             /* model[0] == 0: not cached, only watched */
    int gpu;                                                /* runs on the GPU: watched, with a CPU twin to fall back to */
    int abandoned;                                          /* the GPU stalled or failed: runs go to the twin */
    const OrtEnv *env; wchar_t *path; OrtSessionOptions *copts;   /* what the twin is made from (gpu only) */
    OrtSession *twin;
    char fp[65]; int refs;                                  /* gpu: the model's checksum; sessions sharing this one */
    rb_mutex run_lock;                                      /* gpu: one run at a time (DirectML's thread-safety isn't verified) */
    int use_cuda;                                           /* runs go to the shared CUDA session first; s itself is the CPU fallback */
} sessions[MAX_SESSIONS];

static const char *demucs_model(OrtSession *s, int *rebuild) {
    const char *m = NULL;
    rb_lock(&lock);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].s == s && sessions[i].model[0]) { m = sessions[i].model; *rebuild = sessions[i].rebuild; }
    rb_unlock(&lock);
    return m;
}

static void hex(const unsigned char *d, int n, char *out) {
    for (int i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static int file_sha256_locked(const char *path, char out[65]) {
    /* the checksum is remembered per (size, modification time) in model-<inode>.sha256 */
    struct rb_stat st;
    if (rb_stat(path, &st) != 0) return -1;
    char memo[RB_PATH_MAX], tmp[RB_PATH_MAX], want[128], line[200];
    FILE *f;
    if (!memo_dir[0]) memo[0] = 0;                          /* no cache: no memo, hashed every time */
    else pathf(memo, sizeof memo, "%s\\model-%llu.sha256", memo_dir, (unsigned long long)st.st_ino);
    snprintf(want, sizeof want, "%lld %ld", (long long)st.st_size, (long)st.st_mtime);
    long long size = 0;
    int err, fd = memo[0] ? checked_open(AT_FDCWD, memo, sizeof line - 1, &size, &err) : -1;
    if (fd >= 0) {
        int r = read_all(fd, line, (size_t)size);
        close(fd);
        line[r ? 0 : size] = 0;
        /* only 64 lowercase hex digits: the value becomes a folder's name and goes into every key */
        size_t wl = strlen(want);
        if (!strncmp(line, want, wl) && line[wl] == ' ' && is_hex(line + wl + 1, 64) && strlen(line + wl + 1) >= 64
            && (line[wl + 65] == '\n' || !line[wl + 65])) {
            memcpy(out, line + wl + 1, 64);
            out[64] = 0;
            return 0;
        }
    }
    fd = rb_open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    unsigned char *buf = malloc(1 << 20);              /* per call: concurrent sessions hashed one shared buffer */
    if (!buf) { close(fd); return -1; }
    rb_sha256 c;
    rb_sha_init(&c);
    ssize_t n;
    while ((n = read(fd, buf, 1 << 20)) > 0) rb_sha_update(&c, buf, (size_t)n);
    free(buf);
    close(fd);
    if (n < 0) return -1;
    unsigned char d[32];
    rb_sha_final(d, &c);
    hex(d, 32, out);
    if (memo[0] && !pathf(tmp, sizeof tmp, "%s.%d.tmp", memo, getpid()) && (f = open_w(tmp, O_WRONLY | O_TRUNC, "w"))) {
        int ok = fprintf(f, "%s %s\n", want, out) > 0;
        ok = fclose(f) == 0 && ok;
        if (!ok || rb_rename(tmp, memo) != 0) rb_unlink(tmp);
    }
    return 0;
}

static int file_sha256(const char *path, char out[65]) {
    rb_lock(&hash_lock);                     /* sessions opened together hash once and agree */
    int r = file_sha256_locked(path, out);
    rb_unlock(&hash_lock);
    return r;
}

/* the cache folder (and key prefix) of a model whose stems are rebuilt: still 64 hex digits, so the
 * cleanup's layout() knows it, but never the folder of the model itself, and new for each
 * REBUILD_VERSION */
static void rebuilt_id(const char *fp, char out[65]) {
    unsigned char d[32];
    rb_sha256 c;
    rb_sha_init(&c);
    rb_sha_update(&c, "rbstems-rebuild-" REBUILD_VERSION ":", (size_t)strlen("rbstems-rebuild-" REBUILD_VERSION ":"));
    rb_sha_update(&c, fp, 64);
    rb_sha_final(d, &c);
    hex(d, 32, out);
}

/* ---------------------------------------------------------------- the cache's layout */

/* What a walk of the cache folder finds, by name and depth: only what this file writes, and the
 * AppleDouble companions ("._<name>") exFAT and FAT32 add to each of those. Everything else
 * (another app's files, a symlink, a folder someone made, the marker and its companion) is OTHER
 * and is never read, deleted or entered. Companions are never opened. */
enum { OTHER, MODEL_DIR, PREFIX_DIR, ENTRY_FLAC, ENTRY_DESC, ENTRY_TMP, MEMO_TMP, COMP_DIR, COMP_FLAC, COMP_DESC, COMP_TMP };

static int is_pid_tmp(const char *s) {                      /* ".<pid>.tmp", the whole rest */
    if (*s++ != '.' || !isdigit((unsigned char)*s)) return 0;
    while (isdigit((unsigned char)*s)) s++;
    return !strcmp(s, ".tmp");
}

static int is_memo_tmp(const char *n) {                     /* model-<inode>.sha256.<pid>.tmp */
    if (strncmp(n, "model-", 6) || !isdigit((unsigned char)n[6])) return 0;
    for (n += 6; isdigit((unsigned char)*n); n++) {}
    return !strncmp(n, ".sha256", 7) && is_pid_tmp(n + 7);
}

/* n: a name found at `level` below the cache folder (1: in it; 3: in the prefix folder `parent`),
 * a folder (dir) or a regular file (file), by lstat. Memo temporary files belong to this Mac's
 * folder only (memo_dir): in a chosen location their names are OTHER. */
static int layout(const char *n, int level, int dir, int file, const char *parent) {
    size_t len = strlen(n);
    if (file && n[0] == '.' && n[1] == '_') {               /* a companion: of a folder, an entry or an entry's temp */
        if (level < 3) {
            int k = layout(n + 2, level, 1, 0, parent);
            return k == MODEL_DIR || k == PREFIX_DIR ? COMP_DIR : OTHER;
        }
        int k = layout(n + 2, level, 0, 1, parent);
        return k == ENTRY_FLAC ? COMP_FLAC : k == ENTRY_DESC ? COMP_DESC : k == ENTRY_TMP ? COMP_TMP : OTHER;
    }
    if (level == 1) {                                       /* <model sha256>/, model-<inode>.sha256.<pid>.tmp */
        if (dir) return len == 64 && is_hex(n, 64) ? MODEL_DIR : OTHER;
        return file && !chosen && is_memo_tmp(n) ? MEMO_TMP : OTHER;
    }
    if (level == 2) return dir && len == 2 && is_hex(n, 2) ? PREFIX_DIR : OTHER;   /* <key[0:2]>/ */
    if (level != 3 || !file || len < 69 || !parent || strlen(parent) != 2 || !is_hex(n, 64) || strncmp(n, parent, 2))
        return OTHER;
    if (!strcmp(n + 64, ".flac")) return ENTRY_FLAC;        /* <key>.flac, <key>.desc, either's .<pid>.tmp */
    if (!strcmp(n + 64, ".desc")) return ENTRY_DESC;
    return (!strncmp(n + 64, ".flac", 5) || !strncmp(n + 64, ".desc", 5)) && is_pid_tmp(n + 69) ? ENTRY_TMP : OTHER;
}

/* calls f(ctx, name, lstat) for each name in the folder fd (not "." or ".."); -1 if the folder
 * couldn't be read whole (f returning nonzero stops it there, also -1) */
static int each_name(int fd, rb_name_fn f, void *ctx) { return rb_each_name(fd, f, ctx); }

/* The disk space an entry takes (the size limit counts it): st_blocks of <key>.flac, <key>.desc
 * and, on exFAT and FAT32, their companions. x: its prefix folder. A missing file counts 0. */
static long long entry_size(int x, const char *key) {
    static const char *const forms[] = {"%s.flac", "%s.desc", "._%s.flac", "._%s.desc"};
    long long sum = 0;
    for (int i = 0; i < 4; i++) {
        char name[80];
        struct rb_stat st;
        snprintf(name, sizeof name, forms[i], key);
        if (rb_fstatat(x, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && RB_ISREG(st.st_mode)) sum += (long long)st.st_blocks * 512;
    }
    return sum;
}

/* ---------------------------------------------------------------- chunk summaries */

/* The summary of a chunk: log10 RMS of the mono mix and of its first difference (the high
 * band) over 0.1 s frames. mix is planar [2][L]. Returns the number of frames. */
static int summarize(const float *mix, size_t L, float *out) {
    int n = (int)(L / DESC_FRAME);
    if (n > DESC_MAX_FRAMES) n = DESC_MAX_FRAMES;
    const float *l = mix, *r = mix + L;
    double prev = 0;
    for (int f = 0; f < n; f++) {
        double e0 = 0, e1 = 0;
        for (size_t i = (size_t)f * DESC_FRAME; i < (size_t)(f + 1) * DESC_FRAME; i++) {
            double m = 0.5 * ((double)l[i] + r[i]);
            e0 += m * m;
            e1 += (m - prev) * (m - prev);
            prev = m;
        }
        out[f] = (float)log10(sqrt(e0 / DESC_FRAME) + 1e-6);
        out[n + f] = (float)log10(sqrt(e1 / DESC_FRAME) + 1e-6);
    }
    return n;
}

/* The residual of a chunk against stems (pcm, 8-ch interleaved half level) shifted by d: per
 * channel, per frame and over the whole chunk, 10 log10(resid energy / mix energy). Samples the
 * shifted stems don't cover count as explained (serve() gives them to "other"). mixe receives
 * each frame's mean square (per channel), for the -60 dBFS floor. */
static void residual(const int16_t *pcm, const float *mix, size_t L, long d, int frames,
                     float *frame_db /* [2][frames] */, float *total_db /* [2] */, double *mixe /* [2][frames] */) {
    for (int c = 0; c < 2; c++) {
        const float *m = mix + (size_t)c * L;
        double rt = 0, mt = 0;
        for (int f = 0; f < frames; f++) {
            double re = 0, me = 0;
            for (size_t n = (size_t)f * DESC_FRAME; n < (size_t)(f + 1) * DESC_FRAME; n++) {
                long k = (long)n - d;
                double s = 0;
                if (k >= 0 && k < (long)L) {
                    const int16_t *p = pcm + (size_t)k * 8 + c;
                    s = (p[0] + p[2] + p[4] + p[6]) / 32767.0 * 2.0;
                } else {
                    s = m[n];
                }
                double e = m[n] - s;
                re += e * e;
                me += (double)m[n] * m[n];
            }
            frame_db[c * frames + f] = (float)(10 * log10((re + 1e-12) / (me + 1e-12)));
            if (mixe) mixe[c * frames + f] = me / DESC_FRAME;
            rt += re;
            mt += me;
        }
        total_db[c] = (float)(10 * log10((rt + 1e-12) / (mt + 1e-12)));
    }
}

/* the .desc file: "RBD2", frames, r0, g0, summary [2*frames], total residual [2] (dB, per
 * channel), frame residuals [2][frames] (dB). "RBD1" (no residuals, before 2026-10-06) is read
 * for completeness but never offered as a near candidate; an exact hit rewrites it as RBD2. */
typedef struct { char key[65]; int frames, version; float r0, g0; float *d, *rt, *rf; } summary;
static summary *index_;
static size_t index_n;
static char index_model[65];
static time_t index_time;
static int index_loading;               /* a load is queued or running: reloads are coalesced */
static double reload_every = 30;        /* seconds: max(30, 10 x the last load's time) */
/* the last few summaries add_to_index saw, for the load that was running meanwhile (it may have
 * walked past their files before they were written) */
#define RECENT 32
static summary recent[RECENT];
static char recent_model[RECENT][65];
static unsigned long long recent_seq[RECENT], add_seq;
/* The index's ceiling: entries for max_bytes at INDEX_ENTRY_BYTES each (well under a full chunk's
 * 2.4 MB), at most INDEX_HARD_MAX (about 1.9 KB each in rekordbox's memory). Past it a load keeps
 * a partial index: exact hits and separations aren't affected, only near hits. */
#define INDEX_ENTRY_BYTES (1LL << 20)
#define INDEX_HARD_MAX 50000
static size_t index_ceiling(void) {
    long long n = max_bytes / INDEX_ENTRY_BYTES;
    return n < 1000 ? 1000 : n > INDEX_HARD_MAX ? INDEX_HARD_MAX : (size_t)n;
}

/* A chosen location's entries can be written by anyone who can write the drive, so
 * the bars a .desc sets are capped, and an exact hit must add up to the chunk. Calibrated on the
 * 118 RBD2 entries saved on the owner's Mac (2026-11-01): whole-chunk residual at worst -16.0 dB
 * (median -33.4); |g0 - 1| at most 0.065; frame residuals over -10 dB only in frames under the
 * -60 dBFS floor, which aren't judged. Garbage and corruption are caught, not forgery: stems
 * swapped in a way that keeps their sum still pass. */
#define CHOSEN_RT_MAX (-10.0)
#define CHOSEN_RF_MAX (-10.0)
#define CHOSEN_G0_SLACK 0.15
#define EXACT_RESIDUAL_MAX (-10.0)

static int all_finite(const float *v, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!isfinite(v[i])) return 0;
    return 1;
}

static void free_summary(summary *s) {
    free(s->d);
    free(s->rt);
    free(s->rf);
    s->d = s->rt = s->rf = NULL;
}

static int copy_summary(summary *to, const summary *from) {        /* deep; 0 when done */
    size_t n2 = 2 * (size_t)from->frames;
    *to = *from;
    to->d = malloc(sizeof(float) * n2);
    to->rt = malloc(sizeof(float) * 2);
    to->rf = malloc(sizeof(float) * n2);
    if (!to->d || !to->rt || !to->rf) { free_summary(to); return -1; }
    memcpy(to->d, from->d, sizeof(float) * n2);
    memcpy(to->rt, from->rt, sizeof(float) * 2);
    memcpy(to->rf, from->rf, sizeof(float) * n2);
    return 0;
}

#define DESC_MAX (16 + 8 * (2 * DESC_MAX_FRAMES) + 8)

/* a .desc's bytes; in a chosen location its bars are capped (CHOSEN_*) */
static int parse_desc(const unsigned char *b, size_t n, summary *s) {
    char magic[4];
    s->d = s->rt = s->rf = NULL;
    if (n < 16) return -1;
    memcpy(magic, b, 4);
    memcpy(&s->frames, b + 4, 4);
    memcpy(&s->r0, b + 8, 4);
    memcpy(&s->g0, b + 12, 4);
    if ((memcmp(magic, "RBD1", 4) && memcmp(magic, "RBD2", 4)) || s->frames <= 0 || s->frames > DESC_MAX_FRAMES) return -1;
    s->version = magic[3] - '0';
    size_t n2 = 2 * (size_t)s->frames, want = 16 + 4 * n2 + (s->version == 2 ? 8 + 4 * n2 : 0);
    if (n != want) return -1;
    s->d = malloc(sizeof(float) * n2);
    int ok = s->d != NULL;
    if (ok) memcpy(s->d, b + 16, 4 * n2);
    if (ok && s->version == 2) {
        s->rt = malloc(sizeof(float) * 2);
        s->rf = malloc(sizeof(float) * n2);
        ok = s->rt && s->rf;
        if (ok) {
            memcpy(s->rt, b + 16 + 4 * n2, 8);
            memcpy(s->rf, b + 24 + 4 * n2, 4 * n2);
            ok = all_finite(s->rt, 2) && all_finite(s->rf, n2);
        }
    }
    ok = ok && isfinite(s->r0) && isfinite(s->g0) && all_finite(s->d, n2);
    if (ok && chosen && s->version == 2) {
        for (int c = 0; c < 2; c++) s->rt[c] = fminf(s->rt[c], (float)CHOSEN_RT_MAX);
        for (size_t i = 0; i < n2; i++) s->rf[i] = fminf(s->rf[i], (float)CHOSEN_RF_MAX);
    }
    if (!ok) free_summary(s);
    return ok ? 0 : -1;
}

/* <key>.desc in the prefix folder x */
static int read_desc(int x, const char *key, summary *s) {
    char name[80];
    unsigned char b[DESC_MAX];
    long long size;
    int err, fd;
    snprintf(name, sizeof name, "%s.desc", key);
    if ((fd = checked_open(x, name, DESC_MAX, &size, &err)) < 0) return -1;
    int r = read_all(fd, b, (size_t)size);
    close(fd);
    return r ? -1 : parse_desc(b, (size_t)size, s);
}

static int write_desc(int x, const char *key, const float *d, int frames, float r0, float g0, const float *rt,
                      const float *rf, int *err) {
    char name[80], tmp[96];
    snprintf(name, sizeof name, "%s.desc", key);
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", name, getpid());
    rb_unlinkat(x, tmp, 0);
    int fd = rb_openat(x, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (!f) { *err = errno; if (fd >= 0) { close(fd); rb_unlinkat(x, tmp, 0); } return -1; }
    size_t n = 2 * (size_t)frames;
    int ok = fwrite("RBD2", 1, 4, f) == 4 && fwrite(&frames, 4, 1, f) == 1 && fwrite(&r0, 4, 1, f) == 1
             && fwrite(&g0, 4, 1, f) == 1 && fwrite(d, sizeof(float), n, f) == n
             && fwrite(rt, sizeof(float), 2, f) == 2 && fwrite(rf, sizeof(float), n, f) == n;
    if (!ok) *err = errno;
    if (fclose(f) != 0 && ok) { ok = 0; *err = errno; }
    if (ok && rb_renameat(x, tmp, x, name) != 0) { ok = 0; *err = errno; }
    if (!ok) { rb_unlinkat(x, tmp, 0); return -1; }
    return 0;
}

/* whether a valid summary may be a near-hit candidate: in a chosen location, only one whose
 * level g0 is near 1 (a .desc there can't loosen the level bar; a quiet chunk's g0 is about 0
 * and it is never a near hit anyway). The file itself stays valid: it isn't rewritten. */
static int near_usable(const summary *s) {
    return s->version == 2 && (!chosen || fabs(s->g0 - 1.0) <= CHOSEN_G0_SLACK);
}

static void free_index(summary *ix, size_t n) {
    for (size_t i = 0; i < n; i++) free_summary(&ix[i]);
    free(ix);
}

/* (Re)reads every RBD2 summary of one model's folder, off rekordbox's threads (the maintenance
 * queue), and publishes it while the location is still available; up to the ceiling. The number
 * loaded, or -1 when the location isn't available. Clears index_loading in any case. */
typedef struct { summary *s; size_t n, cap; } bucket;
typedef struct {
    char (*pre)[3];
    size_t np, limit;
    int m;
    uint64_t dev;
    bucket *bk;
    _Atomic size_t taken;
    _Atomic int capped;
} index_walk;
typedef struct { index_walk *w; size_t i; int x; } index_bucket_walk;

static int index_prefix_cb(void *ctx, const char *pn, const struct rb_stat *ps) {
    index_walk *w = ctx;
    if (layout(pn, 2, RB_ISDIR(ps->st_mode), RB_ISREG(ps->st_mode), NULL) == PREFIX_DIR && w->np < 256) memcpy(w->pre[w->np++], pn, 3);
    return 0;
}

static int index_entry_cb(void *ctx, const char *en, const struct rb_stat *es) {
    index_bucket_walk *bw = ctx;
    index_walk *w = bw->w;
    bucket *k = &w->bk[bw->i];
    if (layout(en, 3, RB_ISDIR(es->st_mode), RB_ISREG(es->st_mode), w->pre[bw->i]) != ENTRY_DESC) return 0;
    if (atomic_fetch_add(&w->taken, 1) >= w->limit) { w->capped = 1; return 1; }
    if (k->n == k->cap) {
        summary *t = realloc(k->s, (k->cap ? k->cap * 2 : 64) * sizeof *t);
        if (!t) return 1;                                   /* a partial index: still correct */
        k->s = t;
        k->cap = k->cap ? k->cap * 2 : 64;
    }
    char key[65];
    memcpy(key, en, 64);
    key[64] = 0;
    if (read_desc(bw->x, key, &k->s[k->n]) == 0) {
        if (!near_usable(&k->s[k->n])) free_summary(&k->s[k->n]);
        else { memcpy(k->s[k->n].key, key, 65); k->n++; }
    }
    return 0;
}

static void index_bucket(void *ctx, size_t i) {
    index_walk *w = ctx;
    int e, x = sub_open(w->m, w->pre[i], 0, w->dev, &e);
    if (x < 0) return;
    index_bucket_walk bw = {w, i, x};
    each_name(x, index_entry_cb, &bw);
    close(x);
}

static long load_index(const char *model) {
    summary *ix = NULL;
    size_t n = 0;
    long r = -1;
    double t0 = rb_mono_seconds();
    rb_lock(&lock);
    unsigned long long seq0 = add_seq;
    rb_unlock(&lock);
    index_walk w = {.limit = index_ceiling(), .m = -1};
    int err = 0, b = -1;
    struct rb_stat st;
    if (loc_now() == LOC_AVAILABLE && !location_recheck()) {
        b = open_base(0, &err);
        if (b >= 0 || (!chosen && err == ENOENT)) r = 0;     /* this PC's folder cleared: an empty index */
    }
    /* the prefix folders are read in parallel: on a USB drive each small read waits on the drive,
     * and several at once keep it busy */
    if (b >= 0 && rb_fstat(b, &st) == 0 && (w.m = sub_open(b, model, 0, st.st_dev, &err)) >= 0 && (w.pre = malloc(256 * 3))) {
        w.dev = st.st_dev;
        each_name(w.m, index_prefix_cb, &w);
    }
    test_pause("RBSTEMS_TEST_INDEX_DELAY");                 /* the prefix folders listed, none read yet */
    w.bk = w.np ? calloc(w.np, sizeof *w.bk) : NULL;
    if (w.bk) rb_apply(w.np, index_bucket, &w);
    for (size_t i = 0; w.bk && i < w.np; i++) n += w.bk[i].n;
    if (n && (ix = malloc(n * sizeof *ix))) {
        size_t at = 0;
        for (size_t i = 0; i < w.np; i++) { memcpy(ix + at, w.bk[i].s, w.bk[i].n * sizeof *ix); at += w.bk[i].n; }
    } else if (n) {                                         /* out of memory: no index this time */
        for (size_t i = 0; i < w.np; i++) free_index(w.bk[i].s, w.bk[i].n), w.bk[i].s = NULL;
        n = 0;
    }
    for (size_t i = 0; w.bk && i < w.np; i++) free(w.bk[i].s);
    free(w.bk);
    free(w.pre);
    if (w.m >= 0) close(w.m);
    if (b >= 0) close(b);
    size_t limit = w.limit;
    summary *t = r >= 0 ? realloc(ix, (n + RECENT + 1) * sizeof *ix) : NULL;   /* room for the merge */
    if (t) ix = t;
    double took = rb_mono_seconds() - t0;
    rb_lock(&lock);
    if (r >= 0 && t && loc_state == LOC_AVAILABLE) {
        for (int i = 0; i < RECENT; i++) {                   /* stored while this load ran */
            if (recent_seq[i] <= seq0 || strcmp(recent_model[i], model) || n >= limit) continue;
            size_t j = 0;
            while (j < n && strcmp(ix[j].key, recent[i].key)) j++;
            if (j == n && copy_summary(&ix[n], &recent[i]) == 0) n++;
        }
        summary *old = index_;
        size_t old_n = index_n;
        index_ = ix;
        index_n = n;
        memcpy(index_model, model, 65);
        index_time = time(NULL);
        reload_every = fmax(30, 10 * took);
        ix = old;
        n = old_n;
        r = (long)index_n;
    } else r = -1;
    index_loading = 0;
    rb_unlock(&lock);
    free_index(ix, n);
    if (w.capped) logf_("summaries: stopped at %zu (the most kept for a %.1f GB limit): near hits use those; exact hits are unaffected",
                        limit, max_bytes / (double)(1LL << 30));
    if (took > 2) logf_("summaries: loading took %.1f s; reloaded at most every %.0f s", took, fmax(30, 10 * took));
    return r;
}

/* queues a load of model's summaries on the maintenance queue, unless one is queued already */
static void index_load_job(void *ctx) {
    load_index(ctx);
    free(ctx);
}

static void queue_index_load(const char *model) {
    rb_lock(&lock);
    int go = !index_loading;
    index_loading = 1;
    rb_unlock(&lock);
    char *m = go ? strdup(model) : NULL;
    if (!m || rb_async(maint, index_load_job, m)) {
        free(m);
        if (go) { rb_lock(&lock); index_loading = 0; rb_unlock(&lock); }
    }
}

static void add_to_index(const char *model, const char *key, const float *d, int frames, float r0, float g0,
                         const float *rt, const float *rf) {
    summary src = {.frames = frames, .version = 2, .r0 = r0, .g0 = g0, .d = (float *)d, .rt = (float *)rt, .rf = (float *)rf}, s, r;
    memcpy(src.key, key, 65);
    if (!near_on || !near_usable(&src) || copy_summary(&s, &src)) return;
    int have_r = copy_summary(&r, &src) == 0, placed = 0;
    summary old_s = {0}, old_r = {0};
    rb_lock(&lock);
    if (loc_state == LOC_AVAILABLE && !strcmp(index_model, model)) {
        for (size_t i = 0; i < index_n && !placed; i++)     /* the same entry again (an upgrade): replaced */
            if (!strcmp(index_[i].key, key)) { old_s = index_[i]; index_[i] = s; placed = 1; }
        summary *ix;
        if (!placed && index_n < index_ceiling() && (ix = realloc(index_, (index_n + 1) * sizeof *ix))) {
            index_ = ix;
            index_[index_n++] = s;
            placed = 1;
        }
    }
    if (have_r) {
        int slot = -1;
        for (int i = 0; i < RECENT && slot < 0; i++)
            if (recent_seq[i] && !strcmp(recent[i].key, key) && !strcmp(recent_model[i], model)) slot = i;
        if (slot < 0) slot = (int)(add_seq % RECENT);
        old_r = recent[slot];
        recent[slot] = r;
        memcpy(recent_model[slot], model, 65);
        recent_seq[slot] = ++add_seq;
    }
    rb_unlock(&lock);
    if (!placed) free_summary(&s);
    free_summary(&old_s);
    free_summary(&old_r);
}

/* forgets the index (a lost location); under lock. The caller frees what it returns. */
static summary *forget_index_locked(size_t *n) {
    summary *old = index_;
    *n = index_n;
    index_ = NULL;
    index_n = 0;
    index_model[0] = 0;
    index_time = 0;
    return old;
}

/* the NEAR_CANDIDATES nearest RBD2 summaries of model closer than NEAR_MAX, copied out */
static int nearest(const char *model, const float *d, int frames, summary *out) {
    double best[NEAR_CANDIDATES];
    int found = 0;
    rb_lock(&lock);
    for (size_t i = 0; i < index_n && !strcmp(index_model, model); i++) {
        if (index_[i].frames != frames) continue;
        double dist = 0;
        for (int j = 0; j < 2 * frames; j++) dist += fabs(index_[i].d[j] - d[j]);
        dist /= 2 * frames;
        if (!(dist <= NEAR_MAX)) continue;                 /* NaN never passes */
        int k;
        if (found < NEAR_CANDIDATES) k = found++;
        else if (dist >= best[NEAR_CANDIDATES - 1]) continue;
        else k = NEAR_CANDIDATES - 1;
        while (k > 0 && best[k - 1] > dist) {
            best[k] = best[k - 1];
            out[k] = out[k - 1];
            k--;
        }
        best[k] = dist;
        out[k] = index_[i];                                /* shallow: arrays copied below */
    }
    for (int k = 0; k < found; k++) {                       /* deep copies, valid after the lock */
        size_t n2 = 2 * (size_t)out[k].frames;
        float *rt = malloc(sizeof(float) * 2), *rf = malloc(sizeof(float) * n2);
        if (rt && rf) {
            memcpy(rt, out[k].rt, sizeof(float) * 2);
            memcpy(rf, out[k].rf, sizeof(float) * n2);
        } else {
            free(rt);
            free(rf);
            rt = rf = NULL;
        }
        out[k].d = NULL;
        out[k].rt = rt;
        out[k].rf = rf;
    }
    rb_unlock(&lock);
    return found;
}

/* ---------------------------------------------------------------- verification */

/* mono stems sum (full level) from cached pcm, and mono mix */
static void monos(const int16_t *pcm, const float *mix, size_t L, float *s, float *m) {
    for (size_t i = 0; i < L; i++) {
        double v = 0;
        for (int ch = 0; ch < 8; ch++) v += pcm[i * 8 + ch];
        s[i] = (float)(v / 32767.0 * 2.0 * 0.5);                 /* stems doubled, L+R halved */
        m[i] = 0.5f * (mix[i] + mix[L + i]);
    }
}

/* correlation and level of mix[n] against stems[n - d] (normalised by the whole signals) */
static void corr_at(const float *s, const float *m, size_t L, long d, double ss, double mm, double *r, double *g) {
    double sm = 0;
    for (size_t n = 0; n < L; n++) {
        long k = (long)n - d;
        if (k >= 0 && k < (long)L) sm += (double)m[n] * s[k];
    }
    *r = sm / sqrt(ss * mm + 1e-30);
    *g = sm / (mm + 1e-30);
}

/* the shift d (|d| <= SHIFT_MAX) that best aligns stems with the mix: coarse on 16x averaged
 * signals, then sample-exact around it */
static long best_shift(const float *s, const float *m, size_t L, double ss, double mm) {
    size_t M = L / 16;
    float *a = malloc(M * sizeof(float)), *b = malloc(M * sizeof(float));
    long coarse = 0;
    if (a && b) {
        for (size_t i = 0; i < M; i++) {
            double x = 0, y = 0;
            for (int j = 0; j < 16; j++) { x += s[i * 16 + j]; y += m[i * 16 + j]; }
            a[i] = (float)x;
            b[i] = (float)y;
        }
        double best = -1e300;
        for (long c = -SHIFT_MAX / 16; c <= SHIFT_MAX / 16; c++) {
            double v = 0;
            for (size_t i = 0; i < M; i++) {
                long k = (long)i - c;
                if (k >= 0 && k < (long)M) v += (double)b[i] * a[k];
            }
            if (v > best) { best = v; coarse = c; }
        }
    }
    free(a);
    free(b);
    long d0 = coarse * 16, bestd = d0;
    double bestv = -1e300, r, g;
    for (long d = d0 - 24; d <= d0 + 24; d++) {
        if (d < -SHIFT_MAX || d > SHIFT_MAX) continue;
        corr_at(s, m, L, d, ss, mm, &r, &g);
        if (r > bestv) { bestv = r; bestd = d; }
    }
    return bestd;
}

/* does the requested chunk pass the entry's residual bars? worst: the largest excess in dB */
static int residual_ok(const int16_t *pcm, const float *mix, size_t L, long d, const summary *e, double *worst,
                       double *tot0, double *tot1) {
    int frames = e->frames;
    float *rf = malloc(sizeof(float) * 2 * frames), rt[2];
    double *me = malloc(sizeof(double) * 2 * frames);
    int ok = rf && me;
    *worst = -1e9;
    if (ok) {
        residual(pcm, mix, L, d, frames, rf, rt, me);
        long fs = lround((double)d / DESC_FRAME);           /* the entry's frame the content came from */
        for (int c = 0; c < 2; c++) {
            double ex = rt[c] - (e->rt[c] + M_TOTAL);
            if (ex > *worst) *worst = ex;
            if (ex > 0) ok = 0;
            for (int f = 0; f < frames; f++) {
                if (me[c * frames + f] < FRAME_FLOOR) continue;
                long fo = f - fs;
                if (fo < 0) fo = 0;
                if (fo >= frames) fo = frames - 1;
                double bar = fmax(e->rf[c * frames + fo] + M_FRAME, R_FLOOR);
                double exf = rf[c * frames + f] - bar;
                if (exf > *worst) *worst = exf;
                if (exf > 0) ok = 0;
            }
        }
        *tot0 = rt[0];
        *tot1 = rt[1];
    }
    free(rf);
    free(me);
    return ok;
}

/* ---------------------------------------------------------------- limits */

/* Kept sets: a prepared playlist's entries, kept until a date. The prepare helper writes one file
 * per set into kept_dir (<name>.kept, renamed into place whole), exactly:
 *     rbstemsplus-kept 1\n
 *     until <seconds since 1970, decimal, no leading zero>\n
 *     <model folder> <key>\n          (each 64 lowercase hex digits; any number of lines, 0 too)
 * (the same bytes as on the Mac: upstream 1.3.0's format). A file that differs in any byte, is over
 * KEPT_FILE_MAX, or isn't a plain file (a link) keeps nothing, with a log line; one whose date has
 * passed keeps nothing, silently. Over KEPT_SETS_MAX files (the app makes at most 5) keeps nothing
 * at all: never a choice of which. Names starting with '.' are someone's temporary files. The
 * bridge never writes or deletes them. */
#define KEPT_SETS_MAX 5
#define KEPT_FILE_MAX (8 << 20)                 /* ~64,000 entries: about 1 TB of stems */
#define KEPT_LINE 130                           /* "<64> <64>\n" */
typedef struct { unsigned char model[32], key[32]; } kept_entry;
static void unhex(const char *s, unsigned char *out);

static int by_kept(const void *a, const void *b) { return memcmp(a, b, sizeof(kept_entry)); }

/* a kept file's bytes: 0 with *until and *count (the entries, written to out unless it is NULL),
 * -1 if they aren't exactly the format above */
static int parse_kept(const char *b, size_t n, long long *until, kept_entry *out, size_t *count) {
    static const char head[] = "rbstemsplus-kept 1\nuntil ";
    size_t h = sizeof head - 1, i;
    long long u = 0;
    if (n < h || memcmp(b, head, h)) return -1;
    for (i = h; i < n && i - h < 12 && b[i] >= '0' && b[i] <= '9'; i++) u = u * 10 + (b[i] - '0');
    if (i == h || i >= n || b[i] != '\n' || (b[h] == '0' && i - h > 1)) return -1;
    i++;
    if ((n - i) % KEPT_LINE) return -1;
    size_t k = 0;
    for (; i < n; i += KEPT_LINE, k++) {
        if (!is_hex(b + i, 64) || b[i + 64] != ' ' || !is_hex(b + i + 65, 64) || b[i + 129] != '\n') return -1;
        if (out) { unhex(b + i, out[k].model); unhex(b + i + 65, out[k].key); }
    }
    *until = u;
    *count = k;
    return 0;
}

typedef struct { int d; time_t now; kept_entry *v; size_t n, files, live; } kept_walk;

static int kept_file_cb(void *ctx, const char *name, const struct rb_stat *lst) {
    kept_walk *w = ctx;
    size_t l = strlen(name), c = 0;
    long long until = 0, size = 0;
    if (name[0] == '.' || l <= 5 || strcmp(name + l - 5, ".kept") || ++w->files > KEPT_SETS_MAX) return 0;
    const char *why = NULL;
    char *b = NULL;
    int err = 0, fd = RB_ISREG(lst->st_mode) ? checked_open(w->d, name, KEPT_FILE_MAX, &size, &err) : -1;
    if (fd < 0) why = !RB_ISREG(lst->st_mode) ? "not a plain file" : err == EFTYPE ? "not a plain file or too big" : strerror(err);
    else if (!(b = malloc((size_t)size + 1))) why = "out of memory";
    else if (read_all(fd, b, (size_t)size)) why = "changed while read";
    else if (parse_kept(b, (size_t)size, &until, NULL, &c)) why = "not a kept set";
    if (fd >= 0) close(fd);
    if (!why && until > (long long)w->now && c) {
        kept_entry *t = c <= SIZE_MAX / sizeof *w->v - w->n ? realloc(w->v, (w->n + c) * sizeof *w->v) : NULL;
        if (!t) why = "out of memory";
        else {
            w->v = t;
            parse_kept(b, (size_t)size, &until, w->v + w->n, &c);
            w->n += c;
        }
    }
    if (!why && until > (long long)w->now) w->live++;
    free(b);
    if (why) logf_("kept: %s ignored: %s", name, why);
    return 0;
}

/* the entries of every kept set still in its time, sorted (duplicates are harmless), into *out;
 * returns their number and the sets' in *sets. Nothing (0) without the folder or on any doubt. */
static size_t load_kept(kept_entry **out, size_t *sets, time_t now) {
    kept_walk w = {.now = now};
    *out = NULL;
    *sets = 0;
    if (!kept_dir[0]) return 0;
    w.d = rb_open(kept_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (w.d < 0) {
        if (errno != ENOENT) logf_("kept: the folder can't be read (%s): nothing is kept", strerror(errno));
        return 0;
    }
    int ok = !each_name(w.d, kept_file_cb, &w);
    close(w.d);
    if (!ok || w.files > KEPT_SETS_MAX) {
        logf_(ok ? "kept: %zu sets, over %d: nothing is kept" : "kept: the folder can't be read (%zu sets so far): nothing is kept", w.files, KEPT_SETS_MAX);
        free(w.v);
        return 0;
    }
    if (w.n) qsort(w.v, w.n, sizeof *w.v, by_kept);
    *out = w.v;
    *sets = w.live;
    return w.n;
}

/* one file of an entry as prune finds it: kept small (the model is an index into prune's table of
 * model folders, the key 32 bytes), so a big cache's list stays small too. kept: in a kept set
 * (live entries only) */
typedef struct { unsigned char key[32]; uint32_t model; uint8_t kind, kept; time_t mtime; long long bytes; } pfile;
enum { PF_FLAC, PF_DESC, PF_CFLAC, PF_CDESC };

static int by_key(const void *a, const void *b) {
    const pfile *x = a, *y = b;
    if (x->model != y->model) return x->model < y->model ? -1 : 1;
    int c = memcmp(x->key, y->key, 32);
    return c ? c : x->kind - y->kind;
}

static int by_mtime(const void *a, const void *b) {   /* entries not kept first, least recently used first */
    const pfile *p = a, *q = b;
    if (p->kept != q->kept) return p->kept - q->kept;
    return p->mtime < q->mtime ? -1 : p->mtime > q->mtime;
}

static void unhex(const char *s, unsigned char *out) {     /* 64 lowercase hex digits (is_hex) */
    for (int i = 0; i < 32; i++) {
        int hi = s[2 * i] <= '9' ? s[2 * i] - '0' : s[2 * i] - 'a' + 10, lo = s[2 * i + 1] <= '9' ? s[2 * i + 1] - '0' : s[2 * i + 1] - 'a' + 10;
        out[i] = (unsigned char)(hi << 4 | lo);
    }
}

/* deletes an entry's files (and companions) through its prefix folder; 0 if its FLAC is gone */
static int remove_entry(const char *model, const char *key) {
    static const char *const forms[] = {"%s.flac", "%s.desc", "._%s.flac", "._%s.desc"};
    int err, x = entry_dir(model, key, 0, &err), r = -1;
    if (x < 0) return -1;
    for (int i = 0; i < 4; i++) {
        char name[80];
        snprintf(name, sizeof name, forms[i], key);
        if (rb_unlinkat(x, name, 0) == 0 || errno == ENOENT) { if (!i) r = 0; }
    }
    close(x);
    return r;
}

/* a companion ("._<name>") whose file or folder is gone, left for over an hour: deleted */
static void orphan(int dir, const char *name, const struct rb_stat *st, time_t now) {
    struct rb_stat o;
    if (now - st->st_mtime > 3600 && rb_fstatat(dir, name + 2, &o, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT) rb_unlinkat(dir, name, 0);
}

/* The limits, off rekordbox's threads (maintenance queue): entries unused for max_days go, then
 * over max_bytes the least recently used, down to 90% of it. An entry in a kept set (load_kept)
 * never ages out, and over max_bytes goes only once every other entry has gone. Sizes are the disk space taken
 * (entry_size: companions too); each entry's files are grouped by key first. Only layout()'s
 * names are touched, through folders opened without following links (sub_open). A walk that
 * couldn't read everything (an error, out of memory) evicts nothing and sets no total.
 * Writes go on meanwhile (the writer queue): what they add while the walk runs is kept aside
 * (prune_pending) and added to the walk's total, so no write is lost from the count (one the walk
 * also found is counted twice until the next prune: too much, never too little). Returns whether
 * the walk was complete. */
static int prune_running;               /* under lock */
static long long prune_pending;

typedef struct {
    pfile *pf;
    size_t n, cap, nm;
    char (*models)[65];
    long long temps;
    int complete;
    time_t now;
    uint64_t dev;
    int b, m, x;                        /* the folders being walked */
    size_t model;                       /* index of the model folder being walked */
    const char *prefix;                 /* name of the prefix folder being walked */
} prune_walk;

static int prune_add(prune_walk *w, const char *name, int kind, const struct rb_stat *st) {
    if (w->n == w->cap) {
        pfile *t = realloc(w->pf, (w->cap ? w->cap * 2 : 4096) * sizeof *w->pf);
        if (!t) { w->complete = 0; return 1; }
        w->pf = t;
        w->cap = w->cap ? w->cap * 2 : 4096;
    }
    pfile *f = &w->pf[w->n++];
    unhex(name, f->key);
    f->model = (uint32_t)w->model;
    f->kind = (uint8_t)kind;
    f->mtime = st->st_mtime;
    f->bytes = st->st_blocks * 512;
    return 0;
}

static int prune_top_cb(void *ctx, const char *name, const struct rb_stat *st) {
    prune_walk *w = ctx;
    int k = layout(name, 1, RB_ISDIR(st->st_mode), RB_ISREG(st->st_mode), NULL);
    if (k == MODEL_DIR) {
        if (!(w->nm & (w->nm - 1)) || !w->models) {
            char (*t)[65] = realloc(w->models, (w->nm ? w->nm * 2 : 1) * 65);
            if (!t) { w->complete = 0; return 1; }
            w->models = t;
        }
        memcpy(w->models[w->nm++], name, 65);
    } else if (k == MEMO_TMP && w->now - st->st_mtime > 3600) {
        if (!rb_unlinkat(w->b, name, 0)) w->temps++;
    } else if (k == COMP_DIR) orphan(w->b, name, st, w->now);
    return 0;
}

static int prune_entry_cb(void *ctx, const char *en, const struct rb_stat *es) {
    prune_walk *w = ctx;
    int kind = layout(en, 3, RB_ISDIR(es->st_mode), RB_ISREG(es->st_mode), w->prefix);
    switch (kind) {
    case ENTRY_FLAC: return prune_add(w, en, PF_FLAC, es);
    case ENTRY_DESC: return prune_add(w, en, PF_DESC, es);
    case COMP_FLAC: return prune_add(w, en + 2, PF_CFLAC, es);
    case COMP_DESC: return prune_add(w, en + 2, PF_CDESC, es);
    case COMP_TMP: orphan(w->x, en, es, w->now); return 0;
    case ENTRY_TMP:
        if (w->now - es->st_mtime > 3600 && !rb_unlinkat(w->x, en, 0)) {
            char c[96];
            snprintf(c, sizeof c, "._%s", en);
            rb_unlinkat(w->x, c, 0);
            w->temps++;
        }
        return 0;
    default: return 0;
    }
}

static int prune_prefix_cb(void *ctx, const char *pn, const struct rb_stat *ps) {
    prune_walk *w = ctx;
    int k = layout(pn, 2, RB_ISDIR(ps->st_mode), RB_ISREG(ps->st_mode), NULL), e;
    if (k == COMP_DIR) orphan(w->m, pn, ps, w->now);
    if (k != PREFIX_DIR) return 0;
    if ((w->x = sub_open(w->m, pn, 0, w->dev, &e)) < 0) { if (e != ENOENT && e != EXDEV) w->complete = 0; return !w->complete; }
    w->prefix = pn;
    if (each_name(w->x, prune_entry_cb, w)) w->complete = 0;
    close(w->x);
    w->x = -1;
    return !w->complete;
}

static int prune_memo_cb(void *ctx, const char *name, const struct rb_stat *st) {
    prune_walk *w = ctx;
    if (RB_ISREG(st->st_mode) && is_memo_tmp(name) && w->now - st->st_mtime > 3600 && !rb_unlinkat(w->b, name, 0)) w->temps++;
    return 0;
}

static int prune(void) {
    prune_walk w = {.complete = 1, .now = time(NULL), .b = -1, .m = -1, .x = -1};
    long long total = 0, aged = 0, evicted = 0;
    int err = 0, b = -1;
    struct rb_stat bs;
    if (loc_now() != LOC_AVAILABLE || location_recheck() || (b = open_base(0, &err)) < 0) return !chosen && b < 0 && err == ENOENT;
    if (rb_fstat(b, &bs) != 0) { close(b); return 0; }
    rb_lock(&lock);
    prune_running = 1;
    prune_pending = 0;
    rb_unlock(&lock);
    w.dev = bs.st_dev;
    w.b = b;
    if (each_name(b, prune_top_cb, &w)) w.complete = 0;
    test_pause("RBSTEMS_TEST_PRUNE_DELAY");                 /* the top level walked, the model folders not yet */
    for (size_t i = 0; i < w.nm && w.complete; i++) {
        int m = sub_open(b, w.models[i], 0, w.dev, &err);
        if (m < 0) { if (err != ENOENT && err != EXDEV) w.complete = 0; continue; }
        w.m = m;
        w.model = i;
        if (each_name(m, prune_prefix_cb, &w)) w.complete = 0;
        close(m);
        w.m = -1;
    }
    close(b);
    if (chosen && memo_dir[0]) {                            /* the memos' folder: its stale temporary files only */
        int md = rb_open(memo_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (md >= 0) {
            w.b = md;
            each_name(md, prune_memo_cb, &w);
            close(md);
        }
    }
    pfile *pf = w.pf;
    size_t n = w.n;
    char (*models)[65] = w.models;
    int complete = w.complete;
    time_t now = w.now;
    size_t live = 0, kn = 0, ksets = 0;
    long long kentries = 0, kbytes = 0, kevicted = 0;
    kept_entry *kv = NULL;
    unsigned char (*mbin)[32] = NULL;
    if (complete && (kn = load_kept(&kv, &ksets, now)) && !(mbin = malloc((w.nm ? w.nm : 1) * sizeof *mbin))) {
        logf_("kept: out of memory: nothing is kept");
        kn = 0;
    }
    for (size_t i = 0; i < w.nm && mbin; i++) unhex(models[i], mbin[i]);
    if (complete) {
        qsort(pf, n, sizeof *pf, by_key);
        for (size_t i = 0; i < n;) {                        /* one entry: its files, grouped by key */
            size_t j = i;
            long long bytes = 0;
            time_t flac = -1, newest = 0;
            while (j < n && pf[j].model == pf[i].model && !memcmp(pf[j].key, pf[i].key, 32)) {
                bytes += pf[j].bytes;
                if (pf[j].kind == PF_FLAC) flac = pf[j].mtime;
                if (pf[j].mtime > newest) newest = pf[j].mtime;
                j++;
            }
            char key[65];
            hex(pf[i].key, 32, key);
            uint32_t model = pf[i].model;
            int kept = 0;
            if (kn && flac >= 0) {
                kept_entry probe;
                memcpy(probe.model, mbin[model], 32);
                memcpy(probe.key, pf[i].key, 32);
                kept = bsearch(&probe, kv, kn, sizeof *kv, by_kept) != NULL;
            }
            if (kept) { kentries++; kbytes += bytes; }
            if (flac >= 0 && !kept && now - flac > (time_t)(max_days * 86400)) {
                if (!remove_entry(models[model], key)) aged++;
                else total += bytes;
            } else if (flac < 0 && now - newest > 3600) {
                remove_entry(models[model], key);           /* a summary or companions without the stems */
            } else {
                total += bytes;
                if (flac >= 0) {                            /* the list of live entries reuses pf (live <= i) */
                    memcpy(pf[live].key, pf[i].key, 32);
                    pf[live].model = model;
                    pf[live].mtime = flac;
                    pf[live].bytes = bytes;
                    pf[live].kept = (uint8_t)kept;
                    live++;
                }
            }
            i = j;
        }
        if (total > max_bytes) {
            qsort(pf, live, sizeof *pf, by_mtime);          /* not kept, least recently used, first */
            for (size_t i = 0; i < live && total > max_bytes * 9 / 10; i++) {
                char key[65];
                hex(pf[i].key, 32, key);
                if (!remove_entry(models[pf[i].model], key)) {
                    total -= pf[i].bytes;
                    evicted++;
                    if (pf[i].kept) { kevicted++; kentries--; kbytes -= pf[i].bytes; }
                }
            }
        }
    }
    if (kn || ksets)
        logf_("kept: %zu sets, %lld of their entries in the cache (%.2f GB)", ksets, kentries, kbytes / (double)(1LL << 30));
    if (kevicted) logf_("kept: %lld kept entries removed: the cache was over %.1f GB with every other entry gone", kevicted, max_bytes / (double)(1LL << 30));
    free(kv);
    free(mbin);
    rb_lock(&lock);
    if (complete && loc_state == LOC_AVAILABLE) total_bytes = total + prune_pending;
    prune_running = 0;
    rb_unlock(&lock);
    free(pf);
    free(models);
    logf_("limits: %.2f GB in cache (%lld bytes on disk); removed %lld unused for %g days, %lld over %.1f GB, %lld stale temp files%s",
          total / (double)(1LL << 30), total, aged, max_days, evicted, max_bytes / (double)(1LL << 30), w.temps,
          complete ? "" : " (the folder couldn't be read whole: no eviction this time)");
    return complete;
}

/* The limits at launch, once per load: retried (a minute apart, three times at most) when the walk
 * couldn't read everything, or the limits would go unenforced for the run (no total to grow). */
static void first_prune_job(void *ctx) { first_prune((int)(intptr_t)ctx); }
static void first_prune(int tries) {
    if (prune() || tries >= 3 || loc_now() != LOC_AVAILABLE) return;
    rb_after(maint, 60, first_prune_job, (void *)(intptr_t)(tries + 1));
}

/* queues prune on the maintenance queue, unless it is queued already */
static int prune_queued;                /* under lock */
static void prune_job(void *ctx) {
    (void)ctx;
    rb_lock(&lock);
    prune_queued = 0;
    rb_unlock(&lock);
    prune();
}
static void queue_prune(void) {
    rb_lock(&lock);
    int go = !prune_queued;
    prune_queued = 1;
    rb_unlock(&lock);
    if (go && rb_async(maint, prune_job, NULL)) { rb_lock(&lock); prune_queued = 0; rb_unlock(&lock); }
}

/* ---------------------------------------------------------------- free space */

/* The floor (the user's decision, not a setting): under it no FLAC, .desc or index entry is
 * written. 10% of the size of the cache's disk, at most FREE_FLOOR_MAX; FREE_FLOOR_MAX when the size
 * can't be read. In bytes, GB as Finder counts them (10^9 bytes), like the app's status line, which
 * computes the same floor (cacheFreeFloor in app/Sources/State.swift). */
#define FREE_FLOOR_MAX 50000000000ULL
#define FREE_EVERY 60                   /* seconds a measurement is reused */

/* Free bytes on the cache's volume available to this account (GetDiskFreeSpaceExW); 0 if it can't
 * be read. A test build can fake it (RBSTEMS_TEST_FREE_GB). */
static double free_bytes(const char **how) {
#ifdef RBSTEMS_TEST_REAL_PATH
    const char *t = getenv("RBSTEMS_TEST_FREE_GB");
    if (t) { *how = "test"; return atof(t) * 1e9; }
#endif
    if (!chosen) mkdirs(base_dir);                          /* this PC's: it may have been cleared since loading */
    double b = 0;
    *how = "GetDiskFreeSpaceEx";
    return rb_space(base_dir, &b, NULL) == 0 ? b : 0;
}

/* The size of the cache's volume in bytes; 0 if it can't be read. A test build can fake it
 * (RBSTEMS_TEST_DISK_GB; 0: can't be read). Called after free_bytes, which creates this PC's
 * cache folder. */
static uint64_t disk_bytes(void) {
#ifdef RBSTEMS_TEST_REAL_PATH
    const char *t = getenv("RBSTEMS_TEST_DISK_GB");
    if (t) { double g = atof(t); return g > 0 ? (uint64_t)(g * 1e9 + 0.5) : 0; }
#endif
    double tot = 0;
    return rb_space(base_dir, NULL, &tot) == 0 ? (uint64_t)tot : 0;
}

/* the floor for a disk of `disk` bytes (0: unknown): 10% of it, at most FREE_FLOOR_MAX */
static uint64_t free_floor(uint64_t disk) {
    return disk > 0 && disk / 10 < FREE_FLOOR_MAX ? disk / 10 : FREE_FLOOR_MAX;
}

/* whether a cache write may go ahead: measured at most every FREE_EVERY seconds, logged when
 * saving pauses and when it resumes. Called on the writer queue only; its state is under lock
 * because a lost location resets it. */
static uint64_t space_checked;
static int space_ok(void) {
    uint64_t every = FREE_EVERY, now = (uint64_t)rb_mono_seconds();
#ifdef RBSTEMS_TEST_REAL_PATH
    const char *t = getenv("RBSTEMS_TEST_FREE_SECONDS");
    if (t) every = (uint64_t)atoi(t);
#endif
    rb_lock(&lock);
    int paused = space_paused, fresh = paused >= 0 && now - space_checked < every;
    if (!fresh) space_checked = now;
    rb_unlock(&lock);
    if (fresh) return !paused;
    const char *how;
    double b = free_bytes(&how);
    uint64_t disk = disk_bytes(), fl = free_floor(disk);
    char size[48];
    if (disk) snprintf(size, sizeof size, "disk %.1f GB", disk / 1e9);
    else snprintf(size, sizeof size, "disk size unknown");
    int p = !(b >= (double)fl);
    if (p && paused != 1)
        logf_("cache: saving paused, %.1f GB free on its disk (%s), under its floor of %.1f GB (%s); saved stems still load",
              b / 1e9, how, fl / 1e9, size);
    else if (!p && paused == 1)
        logf_("cache: saving resumed, %.1f GB free on its disk (%s), floor %.1f GB (%s)", b / 1e9, how, fl / 1e9, size);
    else if (paused < 0)
        logf_("cache: %.1f GB free on its disk (%s), floor %.1f GB (%s)", b / 1e9, how, fl / 1e9, size);
    rb_lock(&lock);
    space_paused = p;
    rb_unlock(&lock);
    return !p;
}

/* The chosen location failed (see "where the stems are"): unavailable until a recheck finds it
 * usable again. The index, the total and the space check are forgotten; bridge-state and the log
 * say why. */
static void lose(const char *why) {
    if (!chosen) return;
    size_t n = 0;
    summary *old = NULL;
    rb_lock(&lock);
    int was = loc_state;
    if (was != LOC_UNAVAILABLE) {
        loc_state = LOC_UNAVAILABLE;
        loc_reason = why;
        old = forget_index_locked(&n);
        total_bytes = -1;
        space_paused = -1;
    }
    rb_unlock(&lock);
    if (was == LOC_UNAVAILABLE) return;
    free_index(old, n);
    write_state();
    logf_("cache: %s not available (%s): stems aren't saved or loaded until it is; checked again every %u seconds", base_dir, reason_text(why), recheck_every());
    arm_recheck();
}

/* The chosen location opened as another folder than the pinned one (old_dev, old_ino): its drive
 * connected again since the last use (a new mount has new device numbers), or the folder
 * replaced. Checked at once as at launch (check_location): when it passes, the folder found is
 * pinned and used, its space counted afresh and the index loaded again, as after a recheck.
 * 1 then (or when another thread pinned it meanwhile); 0 and nothing changed otherwise: the
 * caller loses it. Any thread: the space count runs on the maintenance queue. */
static int repin(uint64_t old_dev, uint64_t old_ino) {
    uint64_t dev = 0;
    uint64_t ino = 0;
    char mnt[MNAMELEN] = "";
    uint64_t ser = 0;
    if (check_location(&dev, &ino, mnt, &ser)) return 0;
    size_t n = 0;
    summary *old = NULL;
    int ours = 0, fresh = 0;
    rb_lock(&lock);
    if (loc_state == LOC_AVAILABLE) {
        ours = 1;
        if (pin_dev == old_dev && pin_ino == old_ino) {
            pin_dev = dev;
            pin_ino = ino;
            rb_strlcpy(mount_dir, mnt, sizeof mount_dir);
            mount_serial = ser;
            old = forget_index_locked(&n);
            total_bytes = -1;
            space_paused = -1;
            writer_checked = 0;
            fresh = 1;
        }
    }
    rb_unlock(&lock);
    if (!fresh) return ours;
    free_index(old, n);
    log_using("checked again: using");
    rb_async(maint, first_prune_job, (void *)(intptr_t)0);
    return 1;
}

/* ---------------------------------------------------------------- FLAC */

typedef struct { int16_t *pcm; size_t frames; } write_job;   /* pcm interleaved, 8 channels */

/* <key>.flac in the prefix folder x, through a temporary file renamed when complete; -1 with *err */
static int write_flac(int x, const char *key, write_job *job, int *err) {
    char name[80], tmp[96];
    snprintf(name, sizeof name, "%s.flac", key);
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", name, getpid());
    rb_unlinkat(x, tmp, 0);                                    /* a stale one of this pid */
    int fd = rb_openat(x, tmp, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    FILE *f = fd >= 0 ? fdopen(fd, "w+b") : NULL;           /* w+b: the encoder seeks back for STREAMINFO */
    if (!f) { *err = errno; if (fd >= 0) { close(fd); rb_unlinkat(x, tmp, 0); } return -1; }
    FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
    if (!enc) { fclose(f); rb_unlinkat(x, tmp, 0); *err = ENOMEM; return -1; }
    FLAC__StreamMetadata *vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    FLAC__StreamMetadata_VorbisComment_Entry ent;
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&ent, "RBSTEMS_KEY", key);
    FLAC__metadata_object_vorbiscomment_append_comment(vc, ent, false);
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&ent, "RBSTEMS_FORMAT", FORMAT " drums,bass,other,vocals L/R half-level");
    FLAC__metadata_object_vorbiscomment_append_comment(vc, ent, false);
    FLAC__stream_encoder_set_channels(enc, 8);
    FLAC__stream_encoder_set_bits_per_sample(enc, 16);
    FLAC__stream_encoder_set_sample_rate(enc, 44100);
    FLAC__stream_encoder_set_compression_level(enc, 5);
    FLAC__stream_encoder_set_total_samples_estimate(enc, job->frames);
    FLAC__stream_encoder_set_metadata(enc, &vc, 1);
    /* the encoder owns f from here: finish (or delete) closes it, even after a failed init */
    errno = 0;
    int ok = FLAC__stream_encoder_init_FILE(enc, f, NULL, NULL) == FLAC__STREAM_ENCODER_INIT_STATUS_OK;
    if (ok) {
        enum { BLOCK = 4096 };
        FLAC__int32 buf[BLOCK * 8];
        for (size_t i = 0; ok && i < job->frames; i += BLOCK) {
            size_t n = job->frames - i < BLOCK ? job->frames - i : BLOCK;
            for (size_t j = 0; j < n * 8; j++) buf[j] = job->pcm[i * 8 + j];
            ok = FLAC__stream_encoder_process_interleaved(enc, buf, (unsigned)n);
        }
        ok = FLAC__stream_encoder_finish(enc) && ok;
    }
    *err = ok ? 0 : errno ? errno : EIO;                    /* the write's own errno, as far as it survives */
    test_pause("RBSTEMS_TEST_WRITE_DELAY");                 /* the temp file written, not yet renamed */
    if (ok && test_errno("RBSTEMS_TEST_WRITE_ERRNO")) { ok = 0; *err = test_errno("RBSTEMS_TEST_WRITE_ERRNO"); }
    FLAC__stream_encoder_delete(enc);
    FLAC__metadata_object_delete(vc);
    if (ok && rb_renameat(x, tmp, x, name) != 0) { ok = 0; *err = errno; }
    if (ok) {
        rb_lock(&lock);
        writes++;
        rb_unlock(&lock);
        return 0;
    }
    rb_unlinkat(x, tmp, 0);
    return -1;
}

typedef struct { int16_t *pcm; size_t frames, got; int ok; char key[65]; int key_ok; const unsigned char *b; size_t n, at; } read_ctx;

static FLAC__StreamDecoderReadStatus rd_read(const FLAC__StreamDecoder *d, FLAC__byte buf[], size_t *bytes, void *u) {
    read_ctx *c = u;
    (void)d;
    size_t left = c->n - c->at;
    if (!left) { *bytes = 0; return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM; }
    if (*bytes > left) *bytes = left;
    memcpy(buf, c->b + c->at, *bytes);
    c->at += *bytes;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus rd_write(const FLAC__StreamDecoder *d, const FLAC__Frame *fr,
                                               const FLAC__int32 *const buf[], void *u) {
    read_ctx *c = u;
    (void)d;
    if (fr->header.channels != 8 || fr->header.bits_per_sample != 16) { c->ok = 0; return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT; }
    for (unsigned i = 0; i < fr->header.blocksize; i++) {
        if (c->got >= c->frames) { c->ok = 0; return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT; }
        for (int ch = 0; ch < 8; ch++) c->pcm[c->got * 8 + ch] = (int16_t)buf[ch][i];
        c->got++;
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void rd_meta(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *m, void *u) {
    read_ctx *c = u;
    (void)d;
    if (m->type == FLAC__METADATA_TYPE_STREAMINFO && m->data.stream_info.total_samples != c->frames) c->ok = 0;
    if (m->type == FLAC__METADATA_TYPE_VORBIS_COMMENT)
        for (FLAC__uint32 i = 0; i < m->data.vorbis_comment.num_comments; i++) {
            const FLAC__StreamMetadata_VorbisComment_Entry *e = &m->data.vorbis_comment.comments[i];
            if (e->length == 12 + 64 && !memcmp(e->entry, "RBSTEMS_KEY=", 12) && !memcmp(e->entry + 12, c->key, 64)) c->key_ok = 1;
        }
}

static void rd_error(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus s, void *u) {
    (void)d; (void)s;
    ((read_ctx *)u)->ok = 0;
}

/* decodes an entry's FLAC from memory: exactly `frames` 8-channel 16-bit frames, and the key it
 * names; 0 when it is all there */
static int decode_flac(const unsigned char *b, size_t n, const char *key, int16_t *pcm, size_t frames) {
    read_ctx c = {pcm, frames, 0, 1, {0}, 0, b, n, 0};
    memcpy(c.key, key, 65);
    FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
    if (!d) return -1;
    FLAC__stream_decoder_set_metadata_respond(d, FLAC__METADATA_TYPE_VORBIS_COMMENT);
    int ok = FLAC__stream_decoder_init_stream(d, rd_read, NULL, NULL, NULL, NULL, rd_write, rd_meta, rd_error, &c)
                 == FLAC__STREAM_DECODER_INIT_STATUS_OK
             && FLAC__stream_decoder_process_until_end_of_stream(d);
    FLAC__stream_decoder_finish(d);
    FLAC__stream_decoder_delete(d);
    return ok && c.ok && c.key_ok && c.got == frames ? 0 : -1;
}

/* the most bytes an entry's FLAC of `frames` frames may take: raw 16-bit audio and room for headers */
static long long flac_max(size_t frames) { return (long long)(frames * 16 + (1 << 20)); }

/* Reads an entry's stems (exact or near): its prefix folder (entry_dir), the FLAC opened as
 * checked_open does, read whole, then decoded. 0 when done; *keep (if given) then receives the
 * open file for the hit's "last used" time (futimes). Otherwise -1 with *err: ENOENT and the like
 * for a miss; a failure of the location itself has been dealt with (lose). */
static int read_entry(const char *model, const char *key, int16_t *pcm, size_t frames, int *keep, int *err) {
    char name[80];
    long long size = 0;
    snprintf(name, sizeof name, "%s.flac", key);
    *err = 0;
    int x = entry_dir(model, key, 0, err), fd = -1, r = -1;
    if (x >= 0) {
        fd = checked_open(x, name, flac_max(frames), &size, err);
        close(x);
    }
    if (fd < 0) return -1;
    unsigned char *b = malloc(size ? (size_t)size : 1);
    if (!b) *err = ENOMEM;
    else if (read_all(fd, b, (size_t)size)) *err = errno;
    else if (decode_flac(b, (size_t)size, key, pcm, frames)) *err = EFTYPE;   /* damaged or not ours: a miss */
    else r = 0;
    free(b);
    if (!r && keep) *keep = fd;
    else close(fd);
    return r;
}

/* After a failed read or write of an entry: an I/O error may be the drive going. The location
 * is checked again (lose if it fails); otherwise it was that entry only, a miss. */
static void entry_failed(int err) {
    if (chosen && (err == EIO || err == ENXIO || err == ENODEV)) location_recheck();
}

/* ---------------------------------------------------------------- tensors */

typedef struct { float *data; int64_t dims[8]; size_t ndim, count; } tensor;

static int view(const OrtValue *v, tensor *t) {
    OrtTensorTypeAndShapeInfo *info = NULL;
    if (R->GetTensorTypeAndShape(v, &info)) return -1;
    ONNXTensorElementDataType type;
    int bad = R->GetTensorElementType(info, &type) || type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT
              || R->GetDimensionsCount(info, &t->ndim) || t->ndim > 8 || R->GetDimensions(info, t->dims, t->ndim);
    R->ReleaseTensorTypeAndShapeInfo(info);
    if (bad) return -1;
    t->count = 1;
    for (size_t i = 0; i < t->ndim; i++) t->count *= (size_t)t->dims[i];
    void *p = NULL;
    if (R->GetTensorMutableData((OrtValue *)v, &p)) return -1;
    t->data = p;
    return 0;
}

static int index_of(const char *const *names, size_t n, const char *want) {
    for (size_t i = 0; i < n; i++)
        if (names[i] && !strcmp(names[i], want)) return (int)i;
    return -1;
}

static void mean_std(const tensor *mix, double *mean, double *std) {
    double s = 0, ss = 0;
    for (size_t i = 0; i < mix->count; i++) s += mix->data[i];
    *mean = s / mix->count;
    for (size_t i = 0; i < mix->count; i++) { double d = mix->data[i] - *mean; ss += d * d; }
    *std = sqrt(ss / (mix->count - 1));                     /* unbiased, like torch.std */
}

/* our model (x all zero) returns NaN for constant input: rekordbox gets 0 instead (it then
 * rebuilds mean, which is right for constant input). Returns 1 if x was zero. */
static int sanitize_ours(OrtValue **outputs, int ix, int ixt) {
    tensor tx, txt;
    if (view(outputs[ix], &tx) || view(outputs[ixt], &txt)) return 0;
    for (size_t i = 0; i < tx.count; i++)
        if (tx.data[i] != 0.0f) return 0;
    for (size_t i = 0; i < txt.count; i++)
        if (!isfinite(txt.data[i])) txt.data[i] = 0.0f;
    return 1;
}

/* ---------------------------------------------------------------- rebuilding rekordbox's stems */

#define NFFT 4096
#define HOP 1024
#define BINS 2048                       /* NFFT / 2: the model's bins (the top one is dropped) */

/* The four stems' spectrogram part of one chunk, as rekordbox rebuilds it (Demucs's
 * HDemucs._ispec): x [4 sources][4: L.re L.im R.re R.im][BINS][F] -> out [4][2 channels][L],
 * before the time branch (xt * std + mean) is added. Inverse STFT: NFFT 4096, hop 1024, periodic
 * Hann, "normalized" (x sqrt(NFFT)), centred; the top bin and two empty frames on each side
 * restored, then the chunk's own samples taken out. Checked against torch.istft (max 2.4e-7) and
 * inside rekordbox (a null test: its stems played silence). 0 when done. */
static int rebuild_spec(const float *x, size_t L, size_t F, float *out) {
    const size_t pad = HOP / 2 * 3, start = NFFT / 2 + pad;  /* centre trim, then Demucs's padding */
    if (F != (L + HOP - 1) / HOP) return 1;
    const size_t frames = F + 4, full = NFFT + HOP * (frames - 1);
    if (start + L > full) return 1;
    double *setup = malloc(sizeof(double) * 2 * NFFT);      /* rb_irfft's scratch, per call */
    float *win = malloc(sizeof(float) * NFFT), *env = calloc(full, sizeof(float)), *y = malloc(sizeof(float) * full);
    float *re = malloc(sizeof(float) * BINS), *im = malloc(sizeof(float) * BINS), *frame = malloc(sizeof(float) * NFFT);
    int ok = setup && win && env && y && re && im && frame;
    if (ok) {
        for (int n = 0; n < NFFT; n++) win[n] = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * n / NFFT);
        for (size_t t = 0; t < frames; t++)              /* the window envelope covers every frame, empty ones too */
            for (int n = 0; n < NFFT; n++) env[t * HOP + n] += win[n] * win[n];
        /* rb_irfft returns the plain sum (no 1/NFFT), as vDSP's inverse real FFT does: times
         * sqrt(NFFT) for "normalized", over NFFT for the inverse DFT */
        const float scale = (float)(sqrt((double)NFFT) / NFFT);
        for (int s = 0; s < 4; s++)
            for (int c = 0; c < 2; c++) {
                memset(y, 0, sizeof(float) * full);
                const float *xr = x + (size_t)(s * 4 + 2 * c) * BINS * F, *xi = x + (size_t)(s * 4 + 2 * c + 1) * BINS * F;
                for (size_t f = 0; f < F; f++) {
                    /* packed: re[0] = DC, im[0] = Nyquist (the restored top bin, zero) */
                    for (size_t k = 0; k < BINS; k++) { re[k] = xr[k * F + f]; im[k] = xi[k * F + f]; }
                    im[0] = 0.0f;
                    rb_irfft(re, im, 12, frame, setup);
                    const size_t off = (f + 2) * HOP;
                    for (int n = 0; n < NFFT; n++) y[off + n] += frame[n] * scale * win[n];
                }
                float *o = out + (size_t)(s * 2 + c) * L;
                for (size_t n = 0; n < L; n++) o[n] = y[start + n] / env[start + n];
            }
    }
    free(setup);
    free(win); free(env); free(y); free(re); free(im); free(frame);
    return ok ? 0 : 1;
}

/* rekordbox's own models whose stems are rebuilt (see the top of the file). A test build also
 * takes RBSTEMS_TEST_PIONEER_MODEL. */
static int is_pioneer(const char *fp) {
    for (size_t i = 0; i < sizeof pioneer_models / sizeof *pioneer_models; i++)
        if (!strcmp(fp, pioneer_models[i])) return 1;
#ifdef RBSTEMS_TEST_REAL_PATH
    const char *t = getenv("RBSTEMS_TEST_PIONEER_MODEL");
    if (t && !strcmp(fp, t)) return 1;
#endif
    return 0;
}

/* A rebuilt model's answer as the stems rekordbox makes of it, rebuild_spec(x) + xt * std + mean,
 * quantised the way the cache stores stems (pcm, 8 channels interleaved, half level). Never
 * writes to the model's outputs. Nonzero, and logged, when the answer can't be cached: shapes
 * other than rekordbox's, values that aren't finite, or stems beyond what the cache can hold. */
static int rebuilt_pcm(const tensor *tx, const tensor *txt, const tensor *mag, size_t L, double mean, double std,
                       int16_t *pcm, const char *key) {
    size_t F = mag->ndim == 4 ? (size_t)mag->dims[3] : 0;
    if (mag->ndim != 4 || mag->dims[0] != 1 || mag->dims[1] != 4 || mag->dims[2] != BINS || tx->ndim != 5
        || tx->dims[0] != 1 || tx->dims[1] != 4 || tx->dims[2] != 4 || tx->dims[3] != BINS || (size_t)tx->dims[4] != F
        || F != (L + HOP - 1) / HOP || txt->count != L * 8) {
        logf_("not cached %.12s: unexpected shapes for a rebuilt model (x %zu values, %zu frames, L %zu)", key, tx->count, F, L);
        return -1;
    }
    float *s = malloc(sizeof(float) * 8 * L);
    if (!s || rebuild_spec(tx->data, L, F, s)) {
        free(s);
        logf_("not cached %.12s: the rebuild failed", key);
        return -1;
    }
    size_t clamped = 0;
    int finite = 1;
    for (size_t ch = 0; ch < 8 && finite; ch++)
        for (size_t i = 0; i < L; i++) {
            double v = ((double)s[ch * L + i] + txt->data[ch * L + i] * std + mean) * 0.5 * 32767.0;
            if (!isfinite(v)) { finite = 0; break; }
            if (v > 32767 || v < -32767) { clamped++; v = v > 0 ? 32767 : -32767; }
            pcm[i * 8 + ch] = (int16_t)lrint(v);
        }
    free(s);
    if (!finite) { logf_("not cached %.12s: rebuilt stems aren't finite", key); return -1; }
    if (clamped) { logf_("not cached %.12s: %zu rebuilt samples beyond the cache's range", key, clamped); return -1; }
    return 0;
}

/* ---------------------------------------------------------------- the hooks */

/* Our models, by the SHA-256 of the file: opened with ONNX Runtime's memory pattern off. Measured
 * with rekordbox's ORT 1.18, one 485,100-sample chunk, 4 threads: peak 2.26 GB with it, 1.75 GB
 * without, same speed. Compiled in, never read from a file. The checksum itself may come from the
 * memo in the cache folder, so a forged memo could change another model's memory plan (never its
 * output), or have another model's stems rebuilt and cached: no more than writing wrong entries
 * into the cache folder directly, which the same account can always do, and rebuilt stems that
 * don't add up to the chunk are refused anyway (store_entry). A test build also takes
 * RBSTEMS_TEST_OUR_MODEL. */
static const char *const our_models[] = {
    "0cc50d877629fda906562d08236b32a3e9934e64c4ea8f3ab7fc43f40976e772",
};

static int is_ours(const char *fp) {
    for (size_t i = 0; i < sizeof our_models / sizeof *our_models; i++)
        if (!strcmp(fp, our_models[i])) return 1;
#ifdef RBSTEMS_TEST_REAL_PATH
    const char *t = getenv("RBSTEMS_TEST_OUR_MODEL");
    if (t && !strcmp(fp, t)) return 1;
#endif
    return 0;
}

/* ---------------------------------------------------------------- the GPU (DirectML) */

/* rekordbox for Windows runs its Demucs sessions on the CPU: it adds no execution provider (its
 * log, 2026-11-08). Pioneer's ONNX Runtime is a DirectML build, so with gpu=1 (the default) the
 * bridge adds DirectML to every Demucs session's copy of rekordbox's options, on the adapter with
 * the most dedicated video memory (a laptop's discrete GPU rather than its integrated one). DirectML
 * needs the memory pattern off and sequential execution. Anything that fails falls back to the CPU. */
static int dml_adapter = -1;            /* chosen once: the DXGI adapter index, -1 not chosen yet, -2 none */
static char dml_adapter_name[160];
static int have_nvidia;                 /* a hardware DXGI adapter from NVIDIA (vendor 0x10DE) was seen */

typedef struct { void *vtbl; } rb_com;   /* IDXGIFactory1 / IDXGIAdapter1, called through their vtables */
typedef struct {
    WCHAR Description[128]; UINT VendorId, DeviceId, SubSysId, Revision;
    SIZE_T DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory; LUID AdapterLuid; UINT Flags;
} rb_adapter_desc1;

static void pick_adapter(void) {
    if (dml_adapter != -1) return;
    dml_adapter = -2;
    HMODULE dx = LoadLibraryExW(L"dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    typedef HRESULT (WINAPI *create_t)(const GUID *, void **);
    create_t create = dx ? (create_t)(void *)GetProcAddress(dx, "CreateDXGIFactory1") : NULL;
    static const GUID iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
    rb_com *f = NULL;
    if (!create || create(&iid_factory1, (void **)&f) < 0 || !f) { logf_("GPU: no DXGI (DirectX) here"); return; }
    /* IDXGIFactory1: ... EnumAdapters1 is method 12; IDXGIAdapter1: GetDesc1 is method 10; Release is 2 */
    typedef HRESULT (WINAPI *enum1_t)(rb_com *, UINT, rb_com **);
    typedef HRESULT (WINAPI *desc1_t)(rb_com *, rb_adapter_desc1 *);
    typedef ULONG (WINAPI *release_t)(rb_com *);
    void **fv = *(void ***)f;
    SIZE_T best = 0;
    for (UINT i = 0; i < 16; i++) {
        rb_com *ad = NULL;
        if (((enum1_t)fv[12])(f, i, &ad) < 0 || !ad) break;
        void **av = *(void ***)ad;
        rb_adapter_desc1 d;
        if (((desc1_t)av[10])(ad, &d) >= 0) {
            char name[160];
            if (rb_utf8(d.Description, -1, name, sizeof name)) snprintf(name, sizeof name, "adapter %u", i);
            int software = (d.Flags & 2) != 0;      /* DXGI_ADAPTER_FLAG_SOFTWARE (Microsoft Basic Render Driver) */
            logf_("GPU: adapter %u: %s, %.1f GB of video memory%s", i, name, d.DedicatedVideoMemory / 1073741824.0, software ? " (software)" : "");
            if (!software && d.VendorId == 0x10DE) have_nvidia = 1;
            if (!software && (dml_adapter < 0 || d.DedicatedVideoMemory > best)) {
                dml_adapter = (int)i;
                best = d.DedicatedVideoMemory;
                rb_strlcpy(dml_adapter_name, name, sizeof dml_adapter_name);
            }
        }
        ((release_t)av[2])(ad);
    }
    ((release_t)fv[2])(f);
}

/* adds DirectML to o; 0 when done, with *what describing it */
static int add_dml(OrtSessionOptions *o, const char **what) {
    static char desc[220];
    static OrtStatus *(ORT_API_CALL *dml)(OrtSessionOptions *, int);
    if (!dml) dml = (OrtStatus *(ORT_API_CALL *)(OrtSessionOptions *, int))(void *)GetProcAddress(real_handle, "OrtSessionOptionsAppendExecutionProvider_DML");
    if (!dml) { *what = "this ONNX Runtime has no DirectML"; return -1; }
    pick_adapter();
    if (dml_adapter < 0) { *what = "no GPU found"; return -1; }
    OrtStatus *st = R->SetSessionExecutionMode(o, ORT_SEQUENTIAL);
    if (!st) st = dml(o, dml_adapter);
    if (st) {
        snprintf(desc, sizeof desc, "DirectML refused: %s", R->GetErrorMessage(st));
        R->ReleaseStatus(st);
        *what = desc;
        return -1;
    }
    snprintf(desc, sizeof desc, "on the GPU (DirectML, %s)", dml_adapter_name);
    *what = desc;
    return 0;
}

/* a status whose failure doesn't matter (a best-effort option): freed */
static void ignore_status(OrtStatus *st) { if (st) R->ReleaseStatus(st); }

/* How many CPU threads our model's CPU sessions get. rekordbox's own options leave Demucs v4 at
 * 0.88x real time; the same model measured with rbsp-bench: 4 threads 2.5x, 12 threads 3.4x (an
 * 8 GB RTX 4060 laptop, 2026-11-09). All logical processors, at most 12. */
static int cpu_threads(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#ifdef RBSTEMS_TEST_REAL_PATH
    if (getenv("RBSTEMS_TEST_CPU_THREADS")) n = (DWORD)atoi(getenv("RBSTEMS_TEST_CPU_THREADS"));
#endif
    return n < 1 ? 1 : n > 12 ? 12 : (int)n;
}

/* ---------------------------------------------------------------- the GPU (CUDA)
 *
 * DirectML allocates every intermediate tensor of the graph up front (31.9 GB for Demucs v4: it
 * never finishes on an 8 GB card, GPU-BENCH.md). CUDA's allocator reuses buffers, so the model
 * needs a few GB. Pioneer's ONNX Runtime has no CUDA, so a second, CUDA-enabled ONNX Runtime 1.18
 * (the installer puts it in %ProgramData%\rbstemsplus\ort-cuda, administrators-only like ort) is
 * loaded next to it, for our model only. Its session is a different library's object: rekordbox
 * keeps getting a CPU session of Pioneer's library (s), and each Run copies the inputs across,
 * runs the CUDA session and copies the outputs back. A CUDA failure or a missed deadline turns
 * the CUDA session off and the run goes to s (the CPU, our thread count): rekordbox never waits for it. */
static HMODULE cuda_handle;
static const OrtApi *CR;                /* the CUDA library's own OrtApi */
static OrtEnv *cuda_env;
static int cuda_state;                  /* 0 not tried, 1 loaded, -1 not available */
static rb_mutex cuda_lock = RB_MUTEX_INIT;      /* init and the shared session */
static rb_mutex cuda_run_lock = RB_MUTEX_INIT;  /* one CUDA run at a time */
static OrtSession *cuda_sess;           /* the shared session: rekordbox opens the model twice */
static char cuda_fp[65];
static int cuda_refs;

static int cuda_init(void) {                            /* cuda_lock held */
    if (cuda_state) return cuda_state > 0;
    cuda_state = -1;
    if (!hooks_ok) return 0;
    pick_adapter();
    if (!have_nvidia) { logf_("CUDA: no NVIDIA GPU: Demucs v4 stays on the CPU"); return 0; }
    char path[RB_PATH_MAX] = "";
#ifdef RBSTEMS_TEST_REAL_PATH
    if (getenv("RBSTEMS_TEST_CUDA_PATH")) pathf(path, sizeof path, "%s", getenv("RBSTEMS_TEST_CUDA_PATH"));
#else
    {
        PWSTR w = NULL;
        char pd[RB_PATH_MAX];
        if (SHGetKnownFolderPath(&FOLDERID_ProgramData, KF_FLAG_DONT_VERIFY, NULL, &w) == S_OK && !rb_utf8(w, -1, pd, sizeof pd))
            pathf(path, sizeof path, "%s\\rbstemsplus\\ort-cuda\\onnxruntime.dll", pd);
        CoTaskMemFree(w);
    }
#endif
    wchar_t *wp = path[0] ? rb_wide(path) : NULL;
    int there = wp && GetFileAttributesW(wp) != INVALID_FILE_ATTRIBUTES;
    free(wp);
    if (!there) { logf_("CUDA: no CUDA ONNX Runtime installed (%s): Demucs v4 stays on the CPU", path[0] ? path : "no path"); return 0; }
    HMODULE h = open_real(path);                        /* the same checks as the real library: administrators-only */
    if (!h) return 0;
    const OrtApiBase *(ORT_API_CALL *get)(void) = (const OrtApiBase *(ORT_API_CALL *)(void))(void *)GetProcAddress(h, "OrtGetApiBase");
    const OrtApiBase *base = get ? get() : NULL;
    const char *ver = base ? base->GetVersionString() : "";
    if (!base || strncmp(ver, "1.18.", 5)) { logf_("CUDA: %s is ONNX Runtime %s, not 1.18.x: not used", path, ver); FreeLibrary(h); return 0; }
    /* cuDNN loads its own parts (cudnn_ops_infer64_8.dll ...) by bare name and aborts rekordbox (0xc0000409) when it can't
     * find them: they sit in this folder, which is not on rekordbox's search path. A module already loaded answers a
     * bare-name load, so every DLL of the folder is loaded here by full path (the folder is administrators-only). */
    {
        wchar_t *wdir = rb_wide(path);
        wchar_t *sl = wdir ? wcsrchr(wdir, L'\\') : NULL;
        if (sl) {
            *sl = 0;
            wchar_t pat[RB_PATH_MAX], full[RB_PATH_MAX];
            swprintf(pat, RB_PATH_MAX, L"%ls\\*.dll", wdir);
            WIN32_FIND_DATAW fd;
            HANDLE ff = FindFirstFileW(pat, &fd);
            int n = 0, bad = 0;
            if (ff != INVALID_HANDLE_VALUE) {
                do {
                    if (!_wcsnicmp(fd.cFileName, L"onnxruntime", 11)) continue;           /* ONNX Runtime loads its own providers (preloaded alone, the CUDA one fails to initialise: error 1114) */
                    swprintf(full, RB_PATH_MAX, L"%ls\\%ls", wdir, fd.cFileName);
                    if (LoadLibraryExW(full, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) n++;
                    else { char nm[300]; DWORD e = GetLastError(); if (rb_utf8(fd.cFileName, -1, nm, sizeof nm)) nm[0] = 0; bad++; logf_("CUDA: cannot load %s: error %lu", nm, (unsigned long)e); }
                } while (FindNextFileW(ff, &fd));
                FindClose(ff);
            }
            logf_("CUDA: %d libraries preloaded%s", n, bad ? " (some failed: the CUDA session will probably fail)" : "");
        }
        free(wdir);
    }
    const OrtApi *api = base->GetApi(ORT_API_VERSION);
    OrtEnv *env = NULL;
    OrtStatus *st = api ? api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "rbstems-cuda", &env) : NULL;
    if (!api || st) {
        logf_("CUDA: cannot start %s: %s", path, st ? api->GetErrorMessage(st) : "no OrtApi");
        if (st) api->ReleaseStatus(st);
        FreeLibrary(h);
        return 0;
    }
    cuda_handle = h; CR = api; cuda_env = env;
    cuda_state = 1;
    logf_("CUDA: ONNX Runtime %s loaded from %s", ver, path);
    return 1;
}

/* the shared CUDA session of our model, opened on first use; NULL (with a log line) when it can't be */
static int cuda_acquire(const wchar_t *model_path, const char *fp) {
    int ok = 0;
    rb_lock(&cuda_lock);
    if (cuda_sess && !strcmp(cuda_fp, fp)) { cuda_refs++; ok = 1; }
    else if (!cuda_sess && cuda_init()) {
        OrtSessionOptions *o = NULL;
        OrtCUDAProviderOptionsV2 *p = NULL;
        OrtSession *sess = NULL;
        OrtStatus *st = CR->CreateSessionOptions(&o);
        if (!st) st = CR->CreateCUDAProviderOptions(&p);
        if (!st) {
            /* device 0: DXGI's order isn't CUDA's, so the first NVIDIA GPU. The arena that grows by powers of two took
             * all 8 GB of an RTX 4060 and spilled into shared memory (2x real time); sized to the requests with a 5 GiB
             * cap it peaks at 3.9 GB and runs 20 to 24x real time (rbsp-bench --cuda, 2026-11-09) */
            const char *keys[] = {"device_id", "cudnn_conv_algo_search", "arena_extend_strategy", "gpu_mem_limit"};
            const char *vals[] = {"0", "HEURISTIC", "kSameAsRequested", "5368709120"};
            st = CR->UpdateCUDAProviderOptions(p, keys, vals, 4);
        }
        if (!st) st = CR->SessionOptionsAppendExecutionProvider_CUDA_V2(o, p);
        if (!st) st = CR->SetSessionGraphOptimizationLevel(o, ORT_ENABLE_ALL);
        if (!st) st = CR->CreateSession(cuda_env, model_path, o, &sess);
        if (st) { logf_("CUDA: the session for model %.12s failed (%s): it runs on the CPU", fp, CR->GetErrorMessage(st)); CR->ReleaseStatus(st); }
        else { cuda_sess = sess; memcpy(cuda_fp, fp, 65); cuda_refs = 1; ok = 1; logf_("CUDA: session opened for model %.12s", fp); }
        if (p) CR->ReleaseCUDAProviderOptions(p);
        if (o) CR->ReleaseSessionOptions(o);
    }
    rb_unlock(&cuda_lock);
    return ok;
}

static void cuda_release(void) {
    OrtSession *s = NULL;
    rb_lock(&cuda_lock);
    if (cuda_sess && --cuda_refs <= 0) { s = cuda_sess; cuda_sess = NULL; cuda_refs = 0; }
    rb_unlock(&cuda_lock);
    if (s) { rb_lock(&cuda_run_lock); CR->ReleaseSession(s); rb_unlock(&cuda_run_lock); }
}

typedef struct { OrtRunOptions *ro; int done; int refs; const OrtApi *api; } watch;   /* also used by the GPU watchdog below */
static void watch_unref(watch *w);
static void watch_fire(void *ctx);
static double gpu_deadline(size_t samples);
static OrtValue *new_tensor(const int64_t *dims, size_t ndim);

/* one run on the CUDA session; 0 when the outputs are filled. Float tensors only (our model's). On
 * failure nothing of the outputs is left behind and why[] says what went wrong. */
static int cuda_run(const char *const *in_names, const OrtValue *const *inputs, size_t n_in, const char *const *out_names, size_t n_out,
                    OrtValue **outputs, size_t samples, double *took, char *why, size_t whyn) {
    enum { MAXIO = 8 };
    OrtValue *cin[MAXIO] = {0}, *cout[MAXIO] = {0};
    int created[MAXIO] = {0};
    OrtMemoryInfo *mi = NULL;
    watch *w = NULL;
    int rc = -1;
    OrtStatus *st = NULL;
    if (n_in > MAXIO || n_out > MAXIO) { snprintf(why, whyn, "too many tensors"); return -1; }
    rb_lock(&cuda_lock);
    OrtSession *cs = cuda_sess;
    rb_unlock(&cuda_lock);
    if (!cs) { snprintf(why, whyn, "no CUDA session"); return -1; }
    rb_lock(&cuda_run_lock);
    if ((st = CR->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mi))) goto fail;
    for (size_t k = 0; k < n_in; k++) {
        tensor t;
        if (view(inputs[k], &t)) { snprintf(why, whyn, "input %zu isn't a float tensor", k); goto done; }
        if ((st = CR->CreateTensorWithDataAsOrtValue(mi, t.data, t.count * sizeof(float), t.dims, t.ndim, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &cin[k]))) goto fail;
    }
    w = calloc(1, sizeof *w);
    if (!w || !watchdog || CR->CreateRunOptions(&w->ro)) { free(w); w = NULL; }
    double deadline = gpu_deadline(samples);
    if (w) { w->api = CR; w->refs = 2; if (rb_after(watchdog, deadline, watch_fire, w)) w->refs = 1; }
    double t0 = rb_mono_seconds();
    st = CR->Run(cs, w ? w->ro : NULL, in_names, (const OrtValue *const *)cin, n_in, out_names, n_out, cout);
    *took = rb_mono_seconds() - t0;
    if (w) { rb_lock(&lock); w->done = 1; rb_unlock(&lock); watch_unref(w); w = NULL; }
    if (st) goto fail;
    for (size_t k = 0; k < n_out; k++) {
        OrtTensorTypeAndShapeInfo *info = NULL;
        ONNXTensorElementDataType type;
        int64_t dims[8];
        size_t nd = 0, count = 0;
        void *src = NULL;
        if ((st = CR->GetTensorTypeAndShape(cout[k], &info))) goto fail;
        OrtStatus *s2 = CR->GetTensorElementType(info, &type);
        if (!s2) s2 = CR->GetDimensionsCount(info, &nd);
        if (!s2 && nd <= 8) s2 = CR->GetDimensions(info, dims, nd);
        if (!s2) s2 = CR->GetTensorShapeElementCount(info, &count);
        CR->ReleaseTensorTypeAndShapeInfo(info);
        if (s2) { st = s2; goto fail; }
        if (type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || nd > 8) { snprintf(why, whyn, "output %zu isn't a float tensor", k); goto done; }
        if ((st = CR->GetTensorMutableData(cout[k], &src))) goto fail;
        tensor d;
        if (outputs[k]) {                                       /* rekordbox gave one: it must be the same size */
            if (view(outputs[k], &d) || d.count != count) { snprintf(why, whyn, "output %zu doesn't match", k); goto done; }
        } else {
            if (!(outputs[k] = new_tensor(dims, nd)) || view(outputs[k], &d)) { snprintf(why, whyn, "cannot allocate output %zu", k); goto done; }
            created[k] = 1;
        }
        memcpy(d.data, src, count * sizeof(float));
    }
    rc = 0;
    goto done;
fail:
    snprintf(why, whyn, "%s", st ? CR->GetErrorMessage(st) : "failed");
    if (st) CR->ReleaseStatus(st);
done:
    if (w) { rb_lock(&lock); w->done = 1; rb_unlock(&lock); watch_unref(w); }
    if (rc) for (size_t k = 0; k < n_out; k++) if (created[k]) { R->ReleaseValue(outputs[k]); outputs[k] = NULL; }
    for (size_t k = 0; k < MAXIO; k++) { if (cin[k]) CR->ReleaseValue(cin[k]); if (cout[k]) CR->ReleaseValue(cout[k]); }
    if (mi) CR->ReleaseMemoryInfo(mi);
    rb_unlock(&cuda_run_lock);
    return rc;
}

/* A Demucs session: a copy of rekordbox's options with DirectML added (gpu=1) and, for our model,
 * the memory pattern off (measured: 2.26 GB peak with it, 1.75 GB without, same speed). If the
 * GPU session can't be made, the same without DirectML; if anything about the copy fails,
 * rekordbox's options unchanged. *how says which; *gpu whether it runs on the GPU. */
static OrtStatus *create_demucs(const OrtEnv *env, const ORTCHAR_T *path, const OrtSessionOptions *options,
                                OrtSession **out, const char *fp, const char **how, int *gpu, int no_dml) {
    static char howbuf[300];
    *how = "rekordbox's session options";
    *gpu = 0;
    int ours = is_ours(fp);
    int want_gpu = ours ? gpu_on && gpu_v4_on : gpu_on;     /* ours: only with gpu_v4=1 */
    if (no_dml) want_gpu = 0;                               /* the CUDA session takes the GPU; this one is its CPU fallback */
    if (!options || (!ours && !want_gpu)) return R->CreateSession(env, path, options, out);
    for (int attempt = want_gpu ? 0 : 1; attempt < 2; attempt++) {
        OrtSessionOptions *o = NULL;
        const char *dml_what = "";
        int with_dml = 0;
        OrtStatus *st = R->CloneSessionOptions(options, &o);
#ifdef RBSTEMS_TEST_REAL_PATH
        if (!st && getenv("RBSTEMS_TEST_CLONE_FAIL")) st = R->CreateStatus(ORT_FAIL, "test failure");
#endif
        /* DirectML needs the memory pattern off, whichever model it is */
        if (!st && (ours || attempt == 0)) st = R->DisableMemPattern(o);
        if (!st && ours && attempt == 1) st = R->SetIntraOpNumThreads(o, cpu_threads());   /* on the CPU: use the cores */
        if (!st && attempt == 0) {
            with_dml = add_dml(o, &dml_what) == 0;
            if (!with_dml) {
                logf_("GPU: %s: model %.12s runs on the CPU", dml_what, fp);
                R->ReleaseSessionOptions(o);
                if (!ours) return R->CreateSession(env, path, options, out);
                continue;                                   /* ours: memory pattern off, on the CPU */
            }
        }
        if (!st) st = R->CreateSession(env, path, o, out);  /* the session keeps its own copy of o */
        if (o) R->ReleaseSessionOptions(o);
        if (!st) {
            char thr[40] = "";
            if (ours && !with_dml) snprintf(thr, sizeof thr, ", %d CPU threads", cpu_threads());
            snprintf(howbuf, sizeof howbuf, "%s%s%s%s", ours ? "memory pattern off" : "rekordbox's session options",
                     thr, with_dml ? ", " : "", with_dml ? dml_what : "");
            *how = howbuf;
            *gpu = with_dml;
            return NULL;
        }
        logf_("model %.12s: %s failed (%s)%s", fp, with_dml ? "the GPU session" : "memory pattern off", R->GetErrorMessage(st),
              with_dml ? ": trying the CPU" : ", opening it with rekordbox's session options");
        R->ReleaseStatus(st);
        if (!with_dml && attempt == 1) break;
        if (with_dml && !ours) return R->CreateSession(env, path, options, out);
    }
    return R->CreateSession(env, path, options, out);
}

/* the maintenance job queued when a Demucs session opens: this model's summaries, then (once per
 * load) the limits */
static void session_open_job(void *ctx) {
    char *model = ctx;
    static char loaded[65];                                 /* the maintenance queue's own: once per load */
    static int pruned;
    long n = -1;
    if (!near_on) {                                         /* no index: the limits only */
        rb_lock(&lock);
        index_loading = 0;
        int ok = model && loc_state == LOC_AVAILABLE;
        rb_unlock(&lock);
        if (ok) logf_("summaries not loaded: near hits are off");
        else logf_("summaries not loaded: %s isn't available", base_dir);
        if (!pruned && ok) { pruned = 1; first_prune(0); }
        free(model);
        return;
    }
    if (model && strcmp(loaded, model)) {
        if ((n = load_index(model)) >= 0) memcpy(loaded, model, 65);
    } else {                                                /* another session of a model already loaded */
        rb_lock(&lock);
        index_loading = 0;
        if (model && loc_state == LOC_AVAILABLE) n = (long)index_n;
        rb_unlock(&lock);
    }
    if (n >= 0) logf_("summaries loaded: %ld cached chunks", n);
    else logf_("summaries not loaded: %s isn't available", base_dir);
    if (!pruned && n >= 0) { pruned = 1; first_prune(0); }
    free(model);
}

/* the session options rekordbox added DirectML to (the last few): the Demucs session's line says
 * whether its options were among them, i.e. whether it runs on the GPU */
static const OrtSessionOptions *dml_options[16];
static rb_mutex dml_lock = RB_MUTEX_INIT;
static void remember_dml(const OrtSessionOptions *o) {
    static unsigned next;
    rb_lock(&dml_lock);
    dml_options[next++ % 16] = o;
    rb_unlock(&dml_lock);
}
static int had_dml(const OrtSessionOptions *o) {
    int r = 0;
    rb_lock(&dml_lock);
    for (int i = 0; i < 16; i++) r |= o && dml_options[i] == o;
    rb_unlock(&dml_lock);
    return r;
}

/* whether a model path names rekordbox's Demucs model: its file name is hdemucs.onnx, after a
 * '\' or a '/' (Demucs3.dll builds "<folder>/hdemucs.onnx"), whatever the case */
static int is_demucs_path(const ORTCHAR_T *p) {
    size_t len = p ? wcslen(p) : 0;
    if (len < 13) return 0;
    const wchar_t *t = p + len - 13;
    return (t[0] == L'\\' || t[0] == L'/') && !_wcsicmp(t + 1, L"hdemucs.onnx");
}

static OrtStatus *ORT_API_CALL hook_CreateSession(const OrtEnv *env, const ORTCHAR_T *model_path,
                                                  const OrtSessionOptions *options, OrtSession **out) {
    if (!is_demucs_path(model_path)) return R->CreateSession(env, model_path, options, out);
    char fp[65], path[RB_PATH_MAX];
    const char *how;
    /* hashed first: the checksum picks the options (the memo keeps it cheap) */
    if (rb_utf8(model_path, -1, path, sizeof path) || file_sha256(path, fp)) {
        logf_("ERROR cannot read %s", path[0] ? path : "the model (its path isn't valid UTF-16 or is too long)");
        return R->CreateSession(env, model_path, options, out);
    }
    /* rekordbox opens the same model more than once: on the GPU the copies would each take the model's
     * weights and activations (8 GB cards run out), so they share one session */
    rb_lock(&lock);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].s && sessions[i].gpu && !sessions[i].abandoned && !strcmp(sessions[i].fp, fp)) {
            sessions[i].refs++;
            *out = sessions[i].s;
            rb_unlock(&lock);
            logf_("Demucs session opened: model %.12s, shared with an open session on the GPU", fp);
            return NULL;
        }
    rb_unlock(&lock);
    int on_gpu = 0;
    /* our model on an NVIDIA GPU: a CUDA session (shared), with this session as its CPU fallback */
    int cuda_ok = is_ours(fp) && cuda_on && gpu_on && cuda_acquire(model_path, fp);
    OrtStatus *st = create_demucs(env, model_path, options, out, fp, &how, &on_gpu, cuda_ok);
    if (st && cuda_ok) { cuda_release(); cuda_ok = 0; }
    int cached = !st && enabled && base_dir[0];            /* a chosen location may come back later */
#ifdef RBSTEMS_TEST_REAL_PATH
    if (!st && options && getenv("RBSTEMS_TEST_GPU_AS_CPU") && is_ours(fp) && gpu_v4_on) on_gpu = 1;   /* a stand-in for the GPU */
#endif
    /* rekordbox's own model: rebuilt and cached only where this account turned it on */
    int pioneer = is_pioneer(fp), rebuild = pioneer && rekordbox_model;
    char id[65];
    if (rebuild) rebuilt_id(fp, id);
    else memcpy(id, fp, 65);
    if (!st) logf_("Demucs session opened: model %.12s, %s%s%s%s", fp, how,
                   cuda_ok ? ", on the GPU (CUDA; this CPU session is the fallback)" : on_gpu ? "" : had_dml(options) ? ", on the GPU (rekordbox's DirectML)"
                   : !gpu_on ? ", on the CPU (gpu=0 in config.ini)" : is_ours(fp) && !gpu_v4_on ? ", on the CPU (gpu_v4=0: Demucs v4 on the GPU is experimental)" : ", on the CPU",
                   cached ? "" : ", not cached",
                   !cached || !pioneer ? "" : rebuild ? ", rekordbox's own: stems rebuilt" : ", rekordbox's own: not cached here (rekordbox_model=0)");
    if (st || (!cached && !on_gpu && !cuda_ok)) return st;
    /* a GPU session is watched: what a CPU twin is made from (rekordbox's options, as they were) is kept */
    OrtSessionOptions *copts = NULL;
    if (on_gpu && options && !R->CloneSessionOptions(options, &copts) && is_ours(fp)) { ignore_status(R->DisableMemPattern(copts)); ignore_status(R->SetIntraOpNumThreads(copts, cpu_threads())); }
    wchar_t *wpath = on_gpu ? wcsdup(model_path) : NULL;
    int stored = 0;
    rb_lock(&lock);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (!sessions[i].s) {
            memset(&sessions[i], 0, sizeof sessions[i]);
            sessions[i].s = *out;
            if (cached) { memcpy(sessions[i].model, id, 65); sessions[i].rebuild = rebuild; }
            sessions[i].gpu = on_gpu && copts && wpath;
            memcpy(sessions[i].fp, fp, 65);
            sessions[i].refs = 1;
            sessions[i].use_cuda = cuda_ok;
            sessions[i].env = env; sessions[i].path = wpath; sessions[i].copts = copts;
            stored = 1;
            break;
        }
    rb_unlock(&lock);
    if (!stored) { if (copts) R->ReleaseSessionOptions(copts); free(wpath); if (cuda_ok) cuda_release(); }
    if (!cached) return st;
    /* off rekordbox's threads, after a chosen location's check: this model's summaries, then
     * (once per load) the limits; a location that comes back later is counted by recheck */
    char *model = strdup(id);
    rb_lock(&lock);
    index_loading = 1;                                      /* hook_Run doesn't queue another meanwhile */
    rb_unlock(&lock);
    if (rb_async(maint, session_open_job, model)) {
        free(model);
        rb_lock(&lock);
        index_loading = 0;
        rb_unlock(&lock);
    }
    return st;
}

static void ORT_API_CALL hook_ReleaseSession(OrtSession *s) {
    OrtSession *twin = NULL;
    OrtSessionOptions *copts = NULL;
    wchar_t *wpath = NULL;
    int cuda = 0;
    rb_lock(&lock);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].s == s && sessions[i].refs > 1) { sessions[i].refs--; rb_unlock(&lock); return; }   /* still shared */
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].s == s) {
            twin = sessions[i].twin; copts = sessions[i].copts; wpath = sessions[i].path; cuda = sessions[i].use_cuda;
            memset(&sessions[i], 0, sizeof sessions[i]);
        }
    rb_unlock(&lock);
    if (cuda) cuda_release();
    if (twin) R->ReleaseSession(twin);
    if (copts) R->ReleaseSessionOptions(copts);
    free(wpath);
    R->ReleaseSession(s);
}

static OrtValue *new_tensor(const int64_t *dims, size_t ndim) {
    OrtAllocator *a = NULL;
    OrtValue *v = NULL;
    if (R->GetAllocatorWithDefaultOptions(&a)) return NULL;
    if (R->CreateTensorAsOrtValue(a, dims, ndim, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &v)) return NULL;
    return v;
}

/* fills the outputs from cached pcm, shifted by d samples; samples the shifted stems don't
 * cover go to "other" (the mix there), so the stems still add up to the mix; 0 when done */
static int serve(const int16_t *pcm, const float *mix, size_t L, long d, double mean, double std, const int64_t *xdims,
                 const int64_t *xtdims, int ix, int ixt, OrtValue **outputs) {
    OrtValue *ox = outputs[ix] ? outputs[ix] : new_tensor(xdims, 5);
    OrtValue *oxt = outputs[ixt] ? outputs[ixt] : new_tensor(xtdims, 3);
    tensor tx, txt;
    if (ox && oxt && !view(ox, &tx) && !view(oxt, &txt) && tx.count == (size_t)(xdims[1] * xdims[2] * xdims[3] * xdims[4])
        && txt.count == L * 8) {
        memset(tx.data, 0, tx.count * sizeof(float));
        for (size_t ch = 0; ch < 8; ch++)
            for (size_t i = 0; i < L; i++) {
                long k = (long)i - d;
                double v;
                if (k >= 0 && k < (long)L) v = pcm[(size_t)k * 8 + ch] / 32767.0 * 2.0;
                else v = (ch == 4 || ch == 5) ? mix[(ch - 4) * L + i] : 0.0;     /* other L/R = mix */
                txt.data[ch * L + i] = (float)((v - mean) / std);
            }
        outputs[ix] = ox;
        outputs[ixt] = oxt;
        return 0;
    }
    if (!outputs[ix] && ox) R->ReleaseValue(ox);
    if (!outputs[ixt] && oxt) R->ReleaseValue(oxt);
    return -1;
}

/* The writer queue is bounded: past WRITER_JOBS_MAX jobs or WRITER_BYTES_MAX bytes waiting (each
 * holds a chunk's stems and mix, about 11.6 MB for rekordbox's), new cache work is dropped (the
 * stems still reach rekordbox), logged once until the queue empties. */
#define WRITER_JOBS_MAX 4
#define WRITER_BYTES_MAX (64LL << 20)
static int writer_jobs, writer_full;                        /* under lock */
static long long writer_bytes;

static int writer_admit(long long bytes) {
    rb_lock(&lock);
    int ok = writer_jobs < WRITER_JOBS_MAX && writer_bytes + bytes <= WRITER_BYTES_MAX, say = !ok && !writer_full;
    if (ok) { writer_jobs++; writer_bytes += bytes; }
    else writer_full = 1;
    rb_unlock(&lock);
    if (say) logf_("cache: %d chunks are waiting to be saved: new stems aren't saved until they are", WRITER_JOBS_MAX);
    return ok;
}

static void writer_done(long long bytes) {
    rb_lock(&lock);
    writer_bytes -= bytes;
    if (--writer_jobs == 0) writer_checked = writer_full = 0;   /* the next job starts a new batch */
    rb_unlock(&lock);
}

/* the background work of a new entry or an RBD1 upgrade: summary, r0/g0 and residuals from the
 * stored pcm and the chunk, then (if write_pcm) the FLAC, then the .desc and the index. Rebuilt
 * stems (rebuilt) that don't add up to the chunk within REBUILT_RESIDUAL_MAX on either channel
 * aren't stored: a rebuild that doesn't match the model never reaches the cache. Chunks quieter
 * than FRAME_FLOOR (a track's silent end) aren't judged: there the residual means nothing.
 * The running total changes by the entry's disk space before and after (entry_size), whatever
 * happened; a chosen location's marker is checked at the first write of each batch. */
static void store_entry(const char *model, const char *key, int16_t *pcm, float *mix, size_t L, int write_pcm, int rebuilt) {
#ifdef RBSTEMS_TEST_REAL_PATH
    if (getenv("RBSTEMS_TEST_WRITER_DELAY")) rb_sleep_seconds(atof(getenv("RBSTEMS_TEST_WRITER_DELAY")));
#endif
    if (loc_now() != LOC_AVAILABLE) return;
    rb_lock(&lock);
    int check = chosen && !writer_checked;
    writer_checked = 1;
    rb_unlock(&lock);
    if (check && location_recheck()) return;
    if (!space_ok()) return;                                /* under the floor: nothing written */
    int frames = 0;
    float *d = malloc(sizeof(float) * 2 * DESC_MAX_FRAMES), *s = malloc(L * sizeof(float)), *m = malloc(L * sizeof(float));
    float *rf = malloc(sizeof(float) * 2 * DESC_MAX_FRAMES), rt[2];
    double r = 0, g = 0;
    int ok = d && s && m && rf;
    if (ok) {
        frames = summarize(mix, L, d);
        double sse = 0, mme = 0;
        monos(pcm, mix, L, s, m);
        for (size_t i = 0; i < L; i++) { sse += (double)s[i] * s[i]; mme += (double)m[i] * m[i]; }
        corr_at(s, m, L, 0, sse, mme, &r, &g);
        residual(pcm, mix, L, 0, frames, rf, rt, NULL);
        ok = isfinite(r) && isfinite(g) && all_finite(d, 2 * (size_t)frames) && all_finite(rf, 2 * (size_t)frames);
        double me = 0;
        for (size_t i = 0; i < 2 * L; i++) me += (double)mix[i] * mix[i];
        if (ok && rebuilt && me / (2.0 * L) >= FRAME_FLOOR && !(rt[0] <= REBUILT_RESIDUAL_MAX && rt[1] <= REBUILT_RESIDUAL_MAX)) {
            logf_("not cached %.12s: rebuilt stems don't add up to the chunk (residual L %.1f R %.1f dB, at most %.0f)",
                  key, rt[0], rt[1], REBUILT_RESIDUAL_MAX);
            ok = 0;
        } else if (ok && chosen && me / (2.0 * L) >= FRAME_FLOOR && !(rt[0] <= EXACT_RESIDUAL_MAX && rt[1] <= EXACT_RESIDUAL_MAX)) {
            /* exact_adds_up would refuse it on every hit: written, it would only be rewritten */
            logf_("not cached %.12s: its stems don't add up to the chunk (residual L %.1f R %.1f dB, at most %.0f on another drive)",
                  key, rt[0], rt[1], EXACT_RESIDUAL_MAX);
            ok = 0;
        }
    }
    int err = 0, x = ok ? entry_dir(model, key, 1, &err) : -1;
    if (x >= 0) {
        long long before = entry_size(x, key);
        write_job job = {pcm, L};
        int stored = (!write_pcm || write_flac(x, key, &job, &err) == 0)
                     && write_desc(x, key, d, frames, (float)r, (float)g, rt, rf, &err) == 0;
        long long after = entry_size(x, key);
        close(x);
        int over;
        rb_lock(&lock);
        if (total_bytes >= 0) total_bytes += after - before;
        if (prune_running) prune_pending += after - before;
        over = total_bytes > max_bytes;
        rb_unlock(&lock);
        if (stored) add_to_index(model, key, d, frames, (float)r, (float)g, rt, rf);
        if (over) queue_prune();
        x = stored ? 0 : -1;
    }
    if (ok && x < 0) {
        logf_("ERROR writing %s/%s/%.2s/%s: %s", base_dir, model, key, key, strerror(err));
        if (err == EROFS) lose("read-only");
        else if (chosen && (err == EACCES || err == EPERM)) lose("no-permission");
        else entry_failed(err);
    }
    free(d); free(s); free(m); free(rf);
}

/* whether an entry's .desc is a whole, valid RBD2 (an older or damaged one is rewritten on a hit) */
static int desc_is_rbd2(const char *model, const char *key) {
    summary sm;
    int err, ok = 0, x = entry_dir(model, key, 0, &err);
    if (x < 0) return 0;
    if (read_desc(x, key, &sm) == 0) {
        ok = sm.version == 2;
        free_summary(&sm);
    }
    close(x);
    return ok;
}

/* An exact hit from a chosen location must add up to its chunk (EXACT_RESIDUAL_MAX on each
 * channel), unless the chunk is under FRAME_FLOOR (a track's silent end, as store_entry). */
static int exact_adds_up(const int16_t *pcm, const float *mix, size_t L) {
    double me = 0;
    for (size_t i = 0; i < 2 * L; i++) me += (double)mix[i] * mix[i];
    if (me / (2.0 * L) < FRAME_FLOOR) return 1;
    int frames = (int)(L / DESC_FRAME);
    if (frames > DESC_MAX_FRAMES) frames = DESC_MAX_FRAMES;
    float *rf = malloc(sizeof(float) * 2 * (size_t)frames), rt[2];
    if (!rf) return 0;
    residual(pcm, mix, L, 0, frames, rf, rt, NULL);
    free(rf);
    return rt[0] <= EXACT_RESIDUAL_MAX && rt[1] <= EXACT_RESIDUAL_MAX;
}

/* a chunk for the writer queue (store_entry), which frees it */
typedef struct { char *model, *key; int16_t *pcm; float *mix; size_t L; int write_pcm, rebuilt; long long bytes; } store_job;
static void store_job_run(void *ctx) {
    store_job *j = ctx;
    store_entry(j->model, j->key, j->pcm, j->mix, j->L, j->write_pcm, j->rebuilt);
    free(j->pcm); free(j->mix); free(j->key); free(j->model);
    writer_done(j->bytes);
    free(j);
}

/* queues a store_job; on failure frees everything and gives the writer's place back */
static void queue_store(char *model, char *key, int16_t *pcm, float *mix, size_t L, int write_pcm, int rebuilt, long long bytes) {
    store_job *j = malloc(sizeof *j);
    if (j) *j = (store_job){model, key, pcm, mix, L, write_pcm, rebuilt, bytes};
    if (!j || rb_async(writer, store_job_run, j)) {
        free(j); free(pcm); free(mix); free(key); free(model);
        writer_done(bytes);
    }
}

/* where a chunk's time goes: "hook" is the whole Run call as rekordbox sees it, "model" the model's own
 * Run inside it, "other" the bridge's work (hash, summary, lookup, rebuild), and "between" the time since the
 * previous Run on this thread returned (rekordbox's own work: its STFT/iSTFT, decoding). Averaged every 20
 * misses. (windows-test-5: the model took 0.3 s on the GPU, but chunks arrived every ~1.4 s.) */
static _Thread_local double run_returned;       /* when the last hooked Run on this thread returned */
static struct { double hook, model, between; int n, nb; } run_stats;

/* ---------------------------------------------------------------- the GPU watchdog */

/* A GPU run that stalls (a model that doesn't fit the GPU's memory spills into shared memory and
 * can take minutes per chunk) would leave rekordbox stuck. So a GPU session's Run gets a
 * deadline: past it the run is terminated and answered by a CPU twin of the session, and every
 * later run of that session goes to the twin. */
static rb_mutex twin_lock = RB_MUTEX_INIT;

static void watch_unref(watch *w) {
    rb_lock(&lock);
    int last = --w->refs == 0;
    rb_unlock(&lock);
    if (last) { w->api->ReleaseRunOptions(w->ro); free(w); }
}
static void watch_fire(void *ctx) {
    watch *w = ctx;
    rb_lock(&lock);
    int stop = !w->done;
    rb_unlock(&lock);
    if (stop) ignore_status(w->api->RunOptionsSetTerminate(w->ro));
    watch_unref(w);
}

static double gpu_deadline(size_t samples) {
    double d = 25.0, scaled = 3.0 * (double)samples / 44100.0;   /* the CPU needs ~1.1x the chunk's length for Demucs v4 */
    if (scaled > d) d = scaled;
#ifdef RBSTEMS_TEST_REAL_PATH
    if (getenv("RBSTEMS_TEST_GPU_TIMEOUT_MS")) d = atof(getenv("RBSTEMS_TEST_GPU_TIMEOUT_MS")) / 1000.0;
#endif
    return d;
}

/* every Run of a Demucs session: the GPU ones watched, the others straight through. samples: the
 * chunk's length (0 when unknown) */
static OrtStatus *guarded_run(OrtSession *s, const OrtRunOptions *ro, const char *const *in_names, const OrtValue *const *inputs,
                              size_t n_in, const char *const *out_names, size_t n_out, OrtValue **outputs, size_t samples) {
    int gpu = 0, gone = 0, i, cuda = 0;
    OrtSession *twin = NULL;
    static int cuda_broken, cuda_logged;                    /* a failed CUDA run: every later run is the CPU's */
    rb_lock(&lock);
    for (i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].s == s) { gpu = sessions[i].gpu; gone = sessions[i].abandoned; twin = sessions[i].twin; cuda = sessions[i].use_cuda && !cuda_broken; break; }
    rb_unlock(&lock);
    if (cuda) {
        char why[300] = "";
        double took = 0;
        if (cuda_run(in_names, inputs, n_in, out_names, n_out, outputs, samples, &took, why, sizeof why) == 0) {
            if (cuda_logged < 3) { cuda_logged++; logf_("CUDA run: %.2f s for %zu samples", took, samples); }
            return NULL;
        }
        rb_lock(&lock);
        cuda_broken = 1;
        rb_unlock(&lock);
        logf_("CUDA run failed (%s): this model now runs on the CPU", why);
    }
    rb_mutex *run_lock = gpu ? &sessions[i].run_lock : NULL;
    if (!gpu) return R->Run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs);
    char msg[300] = "";
    if (!gone) {
        rb_lock(run_lock);                                  /* the deadline counts from when this run starts */
        rb_lock(&lock);
        gone = sessions[i].abandoned;                       /* a run that waited: the one before it may have stalled */
        rb_unlock(&lock);
        if (gone) rb_unlock(run_lock);
    }
    if (!gone) {
        watch *w = calloc(1, sizeof *w);
        if (!w || !watchdog || R->CreateRunOptions(&w->ro)) {
            free(w);
            OrtStatus *st = R->Run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs);
            rb_unlock(run_lock);
            return st;
        }
        w->refs = 2;
        w->api = R;
        double deadline = gpu_deadline(samples);
        if (rb_after(watchdog, deadline, watch_fire, w)) w->refs = 1;
#ifdef RBSTEMS_TEST_REAL_PATH
        if (getenv("RBSTEMS_TEST_RUN_DELAY_MS")) rb_sleep_seconds(atof(getenv("RBSTEMS_TEST_RUN_DELAY_MS")) / 1000.0);
#endif
        double t0 = rb_mono_seconds();
        OrtStatus *st = R->Run(s, w->ro, in_names, inputs, n_in, out_names, n_out, outputs);
        double took = rb_mono_seconds() - t0;
        rb_lock(&lock);
        w->done = 1;
        rb_unlock(&lock);
        watch_unref(w);
        if (!st) { rb_unlock(run_lock); return NULL; }
        snprintf(msg, sizeof msg, "%s", R->GetErrorMessage(st));
        R->ReleaseStatus(st);
        rb_lock(&lock);
        for (i = 0; i < MAX_SESSIONS; i++)
            if (sessions[i].s == s) { gone = sessions[i].abandoned; sessions[i].abandoned = 1; }
        rb_unlock(&lock);
        rb_unlock(run_lock);                                /* waiting runs now see abandoned and take the twin */
        if (!gone)
            logf_("GPU run %s after %.1f s (%s): this model now runs on the CPU (the GPU session keeps its memory until rekordbox closes it)",
                  took >= deadline ? "took over its deadline" : "failed", took, msg);
    }
    /* the CPU twin, made on first use */
    rb_lock(&twin_lock);
    rb_lock(&lock);
    for (i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].s == s) { twin = sessions[i].twin; break; }
    const OrtEnv *env = i < MAX_SESSIONS ? sessions[i].env : NULL;
    OrtSessionOptions *copts = i < MAX_SESSIONS ? sessions[i].copts : NULL;
    wchar_t *wpath = i < MAX_SESSIONS && sessions[i].path ? wcsdup(sessions[i].path) : NULL;
    rb_unlock(&lock);
    OrtStatus *st = NULL;
    if (!twin) {
        if (!copts || !wpath) st = R->CreateStatus(ORT_FAIL, "no CPU twin to fall back to");
        else {
            st = R->CreateSession(env, wpath, copts, &twin);
            if (!st) {
                rb_lock(&lock);
                for (i = 0; i < MAX_SESSIONS; i++)
                    if (sessions[i].s == s) { sessions[i].twin = twin; break; }
                int kept = i < MAX_SESSIONS;
                rb_unlock(&lock);
                if (!kept) { R->ReleaseSession(twin); twin = NULL; st = R->CreateStatus(ORT_FAIL, "the session was released"); }
                else logf_("CPU twin of the GPU session opened");
            }
        }
    }
    free(wpath);
    rb_unlock(&twin_lock);
    if (st) return st;
    return R->Run(twin, ro, in_names, inputs, n_in, out_names, n_out, outputs);
}

/* the text appended to a hit's or miss's line; a miss (model >= 0) also feeds the averages */
static void timing_text(char *out, size_t n, double hook, double model, double between) {
    char m[40] = "", b[64] = "";
    if (model >= 0) snprintf(m, sizeof m, " (model %.2f, bridge %.2f)", model, hook - model);
    if (between >= 0) snprintf(b, sizeof b, ", rekordbox %.2f s since the last chunk", between);
    snprintf(out, n, "  [hook %.2f s%s%s]", hook, m, b);
    if (model < 0) return;
    rb_lock(&lock);
    run_stats.hook += hook; run_stats.model += model; run_stats.n++;
    if (between >= 0) { run_stats.between += between; run_stats.nb++; }
    int report = run_stats.n % 20 == 0;
    double cnt = run_stats.n, ah = run_stats.hook / cnt, am = run_stats.model / cnt, bt = run_stats.nb ? run_stats.between / run_stats.nb : -1;
    rb_unlock(&lock);
    if (report) logf_("timing, average of %d misses: hook %.2f s (model %.2f, bridge %.2f), rekordbox %.2f s between chunks", (int)cnt, ah, am, ah - am, bt);
}

static OrtStatus *run_inner(OrtSession *s, const OrtRunOptions *ro, const char *const *in_names, const OrtValue *const *inputs,
                            size_t n_in, const char *const *out_names, size_t n_out, OrtValue **outputs, double t_enter, double between);

static OrtStatus *ORT_API_CALL hook_Run(OrtSession *s, const OrtRunOptions *ro, const char *const *in_names,
                                        const OrtValue *const *inputs, size_t n_in, const char *const *out_names,
                                        size_t n_out, OrtValue **outputs) {
    double t0 = rb_mono_seconds();
    double between = run_returned > 0 ? t0 - run_returned : -1;
    OrtStatus *st = run_inner(s, ro, in_names, inputs, n_in, out_names, n_out, outputs, t0, between);
    run_returned = rb_mono_seconds();
    return st;
}

static OrtStatus *run_inner(OrtSession *s, const OrtRunOptions *ro, const char *const *in_names, const OrtValue *const *inputs,
                            size_t n_in, const char *const *out_names, size_t n_out, OrtValue **outputs, double t_enter, double between) {
    int rebuild = 0;
    const char *model = demucs_model(s, &rebuild);
    int im = model ? index_of(in_names, n_in, "mix") : -1, ig = model ? index_of(in_names, n_in, "mag") : -1;
    int ix = model ? index_of(out_names, n_out, "x") : -1, ixt = model ? index_of(out_names, n_out, "xt") : -1;
    tensor mix, mag;
    if (!model || im < 0 || ig < 0 || ix < 0 || ixt < 0 || n_out != 2 || view(inputs[im], &mix) || view(inputs[ig], &mag)
        || mix.ndim != 3 || mix.dims[0] != 1 || mix.dims[1] != 2 || mag.ndim != 4)
        return guarded_run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs, 0);
    size_t L = (size_t)mix.dims[2];
    double mean, std;
    mean_std(&mix, &mean, &std);
    /* silence or broken input: never cached; nor anything while a chosen location is pending or
     * lost (no drive I/O). Our model's NaN for silence becomes 0. */
    if (!isfinite(mean) || !isfinite(std) || std <= 0 || L < DESC_FRAME || loc_now() != LOC_AVAILABLE) {
        OrtStatus *st = guarded_run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs, L);
        if (!st) sanitize_ours(outputs, ix, ixt);
        return st;
    }

    /* the exact key: format, model, shapes and the audio itself */
    unsigned char dg[32];
    char key[65];
    rb_sha256 c;
    rb_sha_init(&c);
    rb_sha_update(&c, FORMAT, sizeof FORMAT);
    rb_sha_update(&c, model, 64);
    rb_sha_update(&c, mix.dims, sizeof(int64_t) * 3);
    rb_sha_update(&c, mag.dims, sizeof(int64_t) * 4);
    rb_sha_update(&c, mix.data, (size_t)(mix.count * sizeof(float)));
    rb_sha_final(dg, &c);
    hex(dg, 32, key);
#ifdef RBSTEMS_DUMP_DIR
    /* research builds only (bridge/win/build.sh with RBSTEMS_DUMP_DIR set; never a release): the chunk
     * rekordbox sends, as raw float32, <ms since 1970>-<key>.mix (2 x L, planar) and .mag */
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        long long ms = (long long)((((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ULL) / 10000ULL);
        const tensor *t[2] = {&mix, &mag};
        const char *ext[2] = {"mix", "mag"};
        CreateDirectoryA(RBSTEMS_DUMP_DIR, NULL);
        for (int k = 0; k < 2; k++) {
            char p[RB_PATH_MAX];
            snprintf(p, sizeof p, "%s\\%013lld-%s.%s", RBSTEMS_DUMP_DIR, ms, key, ext[k]);
            FILE *f = fopen(p, "wb");
            if (!f) { logf_("dump: can't write %s", p); continue; }
            size_t n = fwrite(t[k]->data, sizeof(float), t[k]->count, f);
            if (fclose(f) || n != t[k]->count) logf_("dump: %s is incomplete", p);
        }
        logf_("dump: %.12s (%zu samples)", key, L);
    }
#endif
    int64_t xdims[5] = {1, 4, mag.dims[1], mag.dims[2], mag.dims[3]}, xtdims[3] = {1, 8, (int64_t)L};
    int16_t *pcm = malloc(L * 8 * sizeof(int16_t));
    float *desc = malloc(sizeof(float) * 2 * DESC_MAX_FRAMES);
    float *ms = malloc(L * sizeof(float)), *mm = malloc(L * sizeof(float));
    if (!pcm || !desc || !ms || !mm) {
        free(pcm); free(desc); free(ms); free(mm);
        OrtStatus *st = guarded_run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs, L);
        if (!st) sanitize_ours(outputs, ix, ixt);
        return st;
    }
    int frames = summarize(mix.data, L, desc);

    /* 1. exact */
#ifdef RBSTEMS_TEST_REAL_PATH
    static int test_hit_used;
    if (!test_hit_used && test_errno("RBSTEMS_TEST_HIT_OPEN_ERRNO")) { test_hit_used = 1; test_open_errno = test_errno("RBSTEMS_TEST_HIT_OPEN_ERRNO"); }
#endif
    int err = 0, touch = -1, got = read_entry(model, key, pcm, L, &touch, &err) == 0;
    if (!got) entry_failed(err);
    if (got && chosen && !exact_adds_up(pcm, mix.data, L)) {
        logf_("hit  %.12s refused: its stems don't add up to the chunk (a damaged or wrong entry): a miss", key);
        got = 0;
    }
    if (got && serve(pcm, mix.data, L, 0, mean, std, xdims, xtdims, ix, ixt, outputs) == 0) {
        rb_futimes_now(touch);                               /* last used: now */
        close(touch);
        rb_lock(&lock);
        long long h = ++hits, nh = near_hits, m = misses;
        rb_unlock(&lock);
        char tt[160];
        timing_text(tt, sizeof tt, rb_mono_seconds() - t_enter, -1, between);
        logf_("hit  %.12s (%zu samples)  [%lld hits, %lld near, %lld misses]%s", key, L, h, nh, m, tt);
        if (near_on && !desc_is_rbd2(model, key)) {         /* an older or damaged entry: (re)write its summary */
            float *mixc = malloc(mix.count * sizeof(float));
            char *kk = strdup(key), *mdl = strdup(model);
            long long bytes = (long long)(L * 8 * sizeof(int16_t) + mix.count * sizeof(float));
            if (mixc && kk && mdl && writer_admit(bytes)) {
                memcpy(mixc, mix.data, mix.count * sizeof(float));
                int16_t *pc = pcm;
                pcm = NULL;
                queue_store(mdl, kk, pc, mixc, L, 0, 0, bytes);
            } else {
                free(mixc); free(kk); free(mdl);
            }
        }
        free(pcm); free(desc); free(ms); free(mm);
        return NULL;
    }
    if (touch >= 0) close(touch);

    /* 2. similar, verified per channel and frame; the index is reloaded off rekordbox's threads
     * (pre-cached entries, other Macs' and accounts' entries written since). Off: none. */
    rb_lock(&lock);
    int stale = near_on && !index_loading && (strcmp(index_model, model) || difftime(time(NULL), index_time) > reload_every);
    rb_unlock(&lock);
    if (stale) queue_index_load(model);
    summary cands[NEAR_CANDIDATES];
    int nc = near_on ? nearest(model, desc, frames, cands) : 0;
    double mmix = 0;
    for (size_t i = 0; i < L; i++) { float v = 0.5f * (mix.data[i] + mix.data[L + i]); mm[i] = v; mmix += (double)v * v; }
    for (int k = 0; k < nc; k++) {
        const char *why = NULL;
        long d = 0;
        double r = 0, g = 0, worst = 0, t0 = 0, t1 = 0;
        touch = -1;
        if (!cands[k].rt || !cands[k].rf || read_entry(model, cands[k].key, pcm, L, &touch, &err)) {
            why = "unreadable";
            if (cands[k].rt && cands[k].rf) entry_failed(err);
        } else {
            monos(pcm, mix.data, L, ms, mm);
            double sse = 0;
            for (size_t i = 0; i < L; i++) sse += (double)ms[i] * ms[i];
            d = best_shift(ms, mm, L, sse, mmix);
            corr_at(ms, mm, L, d, sse, mmix, &r, &g);
            if (labs(d) > SHIFT_ACCEPT) why = "shift";
            else if (!(fabs(g - cands[k].g0) <= GAIN_SLACK)) why = "level";
            else if (!residual_ok(pcm, mix.data, L, d, &cands[k], &worst, &t0, &t1)) why = "residual";
        }
        if (!why && serve(pcm, mix.data, L, d, mean, std, xdims, xtdims, ix, ixt, outputs) == 0) {
            rb_futimes_now(touch);
            close(touch);
            rb_lock(&lock);
            long long h = hits, nh = ++near_hits, m = misses;
            rb_unlock(&lock);
            logf_("near %.12s for %.12s: shift %ld, level %.3f (cached %.3f), residual L %.1f R %.1f dB (cached %.1f %.1f, worst frame %+.1f dB to its bar)  [%lld hits, %lld near, %lld misses]",
                  cands[k].key, key, d, g, cands[k].g0, t0, t1, cands[k].rt[0], cands[k].rt[1], worst, h, nh, m);
            for (int j = 0; j < nc; j++) free_summary(&cands[j]);
            free(pcm); free(desc); free(ms); free(mm);
            return NULL;
        }
        if (touch >= 0) close(touch);
        logf_("near %.12s rejected (%s) for %.12s: shift %ld, level %.3f (cached %.3f), residual L %.1f R %.1f dB, worst frame %+.1f dB over its bar",
              cands[k].key, why ? why : "serve", key, d, g, cands[k].g0, t0, t1, worst);
    }
    for (int j = 0; j < nc; j++) free_summary(&cands[j]);

    /* 3. the model, then cache its stems and the chunk's summary in the background */
    double t_run = rb_mono_seconds();
    OrtStatus *st = guarded_run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs, L);
    t_run = rb_mono_seconds() - t_run;
    tensor tx, txt;
    free(ms); free(mm); free(desc);
    if (st || view(outputs[ix], &tx) || view(outputs[ixt], &txt) || txt.count != L * 8) {
        free(pcm);
        return st;
    }
    int xzero = 1;
    for (size_t i = 0; i < tx.count && xzero; i++) xzero = tx.data[i] == 0.0f;
    if (!xzero && !rebuild) {                                   /* neither ours nor a rebuilt model: don't cache */
        free(pcm);
        return st;
    }
    if (!xzero) {
        /* a rebuilt model: rekordbox gets its answer untouched; the cache gets the stems rebuilt */
        if (rebuilt_pcm(&tx, &txt, &mag, L, mean, std, pcm, key)) {
            free(pcm);
            return st;
        }
    } else {
        int finite = 1;
        for (size_t i = 0; i < txt.count; i++)
            if (!isfinite(txt.data[i])) { txt.data[i] = 0.0f; finite = 0; }
        if (!finite) {                                          /* never cache NaN stems */
            logf_("model returned non-finite stems for %.12s: not cached", key);
            free(pcm);
            return st;
        }
        for (size_t ch = 0; ch < 8; ch++)
            for (size_t i = 0; i < L; i++) {
                double v = (txt.data[ch * L + i] * std + mean) * 0.5 * 32767.0;
                pcm[i * 8 + ch] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : lrint(v));
            }
    }
    int rebuilt = !xzero;
    float *mixc = malloc(mix.count * sizeof(float));
    char *kk = strdup(key), *mdl = strdup(model);
    long long bytes = (long long)(L * 8 * sizeof(int16_t) + mix.count * sizeof(float));
    rb_lock(&lock);
    long long h = hits, nh = near_hits, m = ++misses;
    rb_unlock(&lock);
    char tt[160];
    timing_text(tt, sizeof tt, rb_mono_seconds() - t_enter, t_run, between);
    logf_("miss %.12s (%zu samples%s, the model took %.1f s: %.2fx real time)  [%lld hits, %lld near, %lld misses]%s", key, L, rebuilt ? ", rebuilt" : "",
          t_run, t_run > 0 ? (double)L / 44100.0 / t_run : 0.0, h, nh, m, tt);
    if (!mixc || !kk || !mdl || !writer_admit(bytes)) {
        free(mixc); free(kk); free(mdl); free(pcm);
        return st;
    }
    memcpy(mixc, mix.data, mix.count * sizeof(float));
    queue_store(mdl, kk, pcm, mixc, L, 1, rebuilt, bytes);
    return st;
}

/* ---------------------------------------------------------------- the library's entry points */

static const OrtApi *ORT_API_CALL bridge_GetApi(uint32_t version) {
    const OrtApi *real = real_base ? real_base->GetApi(version) : NULL;
    if (!real) return NULL;
    static rb_mutex m = RB_MUTEX_INIT;
    rb_lock(&m);
    if (!R) {
        R = real;
        memcpy(&api, real, sizeof api);
        api.CreateSession = hook_CreateSession;
        api.ReleaseSession = hook_ReleaseSession;
        api.Run = hook_Run;
    }
    rb_unlock(&m);
    return real == R ? &api : real;
}

static const char *ORT_API_CALL bridge_GetVersionString(void) {
    return real_base ? real_base->GetVersionString() : "";
}

static OrtApiBase bridge_base = {bridge_GetApi, bridge_GetVersionString};

/* The exports, with the ordinals of Pioneer's onnxruntime.dll (exports.def): rekordbox.exe and
 * Demucs3.dll import OrtGetApiBase (@1) and OrtSessionOptionsAppendExecutionProvider_DML (@4) by
 * ordinal. The three providers pass through to the real library's own. */
EXPORT const OrtApiBase *ORT_API_CALL OrtGetApiBase(void) NO_EXCEPTION {
    rb_once(&once, load_real);
    return hooks_ok ? &bridge_base : real_base;
}

static FARPROC real_proc(const char *name) {
    rb_once(&once, load_real);
    return real_handle ? GetProcAddress(real_handle, name) : NULL;
}

static OrtStatus *missing(const char *what) {
    return R ? R->CreateStatus(ORT_FAIL, what) : real_base && real_base->GetApi(ORT_API_VERSION)
           ? real_base->GetApi(ORT_API_VERSION)->CreateStatus(ORT_FAIL, what) : NULL;
}

/* the execution providers rekordbox asks for, logged once per kind and result (which device runs
 * the models: the CPU, or a GPU through DirectML) */
static void log_provider(const char *what, int device, OrtStatus *st) {
    static rb_mutex m = RB_MUTEX_INIT;
    static char said[8][96];
    char line[96];
    snprintf(line, sizeof line, "%s %d %s", what, device, st ? "failed" : "ok");
    rb_lock(&m);
    int seen = 0, i = 0;
    for (; i < 8 && said[i][0]; i++) if (!strcmp(said[i], line)) seen = 1;
    if (!seen && i < 8) memcpy(said[i], line, sizeof line);
    rb_unlock(&m);
    if (seen) return;
    const char *err = st && R ? R->GetErrorMessage(st) : "";
    if (device >= 0) logf_("rekordbox asked for the %s execution provider (device %d): %s%s%s", what, device, st ? "FAILED: " : "ok", err, "");
    else logf_("rekordbox asked for the %s execution provider: %s%s", what, st ? "FAILED: " : "ok", err);
}

EXPORT OrtStatus *ORT_API_CALL OrtSessionOptionsAppendExecutionProvider_CPU(OrtSessionOptions *o, int use_arena) {
    static OrtStatus *(ORT_API_CALL *f)(OrtSessionOptions *, int);
    if (!f) f = (OrtStatus *(ORT_API_CALL *)(OrtSessionOptions *, int))(void *)real_proc("OrtSessionOptionsAppendExecutionProvider_CPU");
    OrtStatus *st = f ? f(o, use_arena) : missing("rbstems bridge: real CPU provider missing");
    log_provider("CPU", -1, st);
    return st;
}

EXPORT OrtStatus *ORT_API_CALL OrtSessionOptionsAppendExecutionProvider_DML(OrtSessionOptions *o, int device_id) {
    static OrtStatus *(ORT_API_CALL *f)(OrtSessionOptions *, int);
    if (!f) f = (OrtStatus *(ORT_API_CALL *)(OrtSessionOptions *, int))(void *)real_proc("OrtSessionOptionsAppendExecutionProvider_DML");
    OrtStatus *st = f ? f(o, device_id) : missing("rbstems bridge: real DirectML provider missing");
    if (!st) remember_dml(o);
    log_provider("DirectML (GPU)", device_id, st);
    return st;
}

/* (OrtSessionOptions*, IDMLDevice*, ID3D12CommandQueue*): the COM pointers pass through untouched */
EXPORT OrtStatus *ORT_API_CALL OrtSessionOptionsAppendExecutionProviderEx_DML(OrtSessionOptions *o, void *dml_device, void *queue) {
    static OrtStatus *(ORT_API_CALL *f)(OrtSessionOptions *, void *, void *);
    if (!f) f = (OrtStatus *(ORT_API_CALL *)(OrtSessionOptions *, void *, void *))(void *)real_proc("OrtSessionOptionsAppendExecutionProviderEx_DML");
    OrtStatus *st = f ? f(o, dml_device, queue) : missing("rbstems bridge: real DirectML provider missing");
    if (!st) remember_dml(o);
    log_provider("DirectML (GPU, rekordbox's own device)", -1, st);
    return st;
}

/* the bridge's own module, so that open_real can tell itself from the real library */
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        self_module = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
