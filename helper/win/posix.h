/* helper/win/posix.h: the POSIX calls the macOS helper's sources use, over the bridge's Windows layer
 * (bridge/win/win_compat.h: UTF-8 paths, openat-style calls that never follow a link). Included
 * first (-include) when compiling the macOS helper's files that need no other change: manifest.c,
 * mp3.c, mp4.c, resample.c. Also the shim the Windows versions of the rest build on. */
#ifndef RBSP_WIN_POSIX_H
#define RBSP_WIN_POSIX_H
#define _POSIX_C_SOURCE 200809L
#define __USE_MINGW_ANSI_STDIO 1
#define _DIRENT_H_                      /* mingw's dirent.h (ANSI paths): this file has its own */
#include <sys/stat.h>                   /* before `stat` becomes a macro */
#include <stdbool.h>
#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_            /* onnxruntime_c_api.h: an SAL annotation */
#endif
#include <sys/types.h>
#include <unistd.h>
#include "../../bridge/win/win_compat.h"

/* ---------------------------------------------------------------- stat */

struct rbp_stat {
    uint64_t st_dev, st_ino;
    unsigned st_mode;
    long long st_size, st_blocks;
    time_t st_mtime;
    struct { long long tv_sec; long tv_nsec; } st_mtimespec;    /* 100 ns steps, as Windows keeps it */
};
#define stat rbp_stat
#define lstat rbp_lstat
#define fstat rbp_fstat
#define fstatat rbp_fstatat

static long long rbp_ft_ns(FILETIME ft) {       /* since 1970 */
    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ULL;
    return (long long)(t * 100ULL);
}
static void rbp_fill(struct rbp_stat *to, const struct rb_stat *from, long long ns) {
    to->st_dev = from->st_dev; to->st_ino = from->st_ino; to->st_mode = from->st_mode;
    to->st_size = from->st_size; to->st_blocks = from->st_blocks; to->st_mtime = from->st_mtime;
    to->st_mtimespec.tv_sec = ns / 1000000000LL;
    to->st_mtimespec.tv_nsec = (long)(ns % 1000000000LL);
}
static int rbp_fstat(int fd, struct rbp_stat *st) {
    struct rb_stat s;
    BY_HANDLE_FILE_INFORMATION i;
    HANDLE h = rb_h(fd);
    if (rb_fstat(fd, &s)) return -1;
    rbp_fill(st, &s, GetFileInformationByHandle(h, &i) ? rbp_ft_ns(i.ftLastWriteTime) : (long long)s.st_mtime * 1000000000LL);
    return 0;
}
static int rbp_fstatat(int dir, const char *name, struct rbp_stat *st, int flag) {
    struct rb_stat s;
    long long ns;
    if (rb_fstatat(dir, name, &s, flag)) return -1;
    ns = (long long)s.st_mtime * 1000000000LL;
    if (dir == AT_FDCWD) {                      /* the exact time, from the path */
        wchar_t *w = rb_wide(name);
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (w && GetFileAttributesExW(w, GetFileExInfoStandard, &d)) ns = rbp_ft_ns(d.ftLastWriteTime);
        free(w);
    } else {
        int fd = rb_openat(dir, name, O_RDONLY | (flag & AT_SYMLINK_NOFOLLOW ? O_NOFOLLOW : 0));
        if (fd >= 0) { struct rbp_stat t; if (rbp_fstat(fd, &t) == 0) ns = (long long)t.st_mtimespec.tv_sec * 1000000000LL + t.st_mtimespec.tv_nsec; close(fd); }
    }
    rbp_fill(st, &s, ns);
    return 0;
}
static int rbp_stat(const char *path, struct rbp_stat *st) { return rbp_fstatat(AT_FDCWD, path, st, 0); }
static int rbp_lstat(const char *path, struct rbp_stat *st) { return rbp_fstatat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW); }

/* ---------------------------------------------------------------- files */

#undef open
#define open(path, ...) rb_openat(AT_FDCWD, (path), __VA_ARGS__)
#define openat rb_openat
#define unlink rb_unlink
#define unlinkat rb_unlinkat
#define rename rb_rename
#define renameat rb_renameat
#define mkdir(p, mode) rb_mkdir(p)
#define fsync _commit
#define getpid _getpid
#define realpath(p, out) (rb_realpath((p), (out), RB_PATH_MAX) ? NULL : (out))
#ifndef PATH_MAX
#define PATH_MAX RB_PATH_MAX
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
static ssize_t pread(int fd, void *buf, size_t n, off_t off) {      /* one thread reads a file at a time */
    __int64 here = _telli64(fd);
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    int r = _read(fd, buf, (unsigned)n);
    _lseeki64(fd, here, SEEK_SET);
    return r;
}

/* ---------------------------------------------------------------- folders */

typedef struct { char (*names)[RB_PATH_MAX]; size_t n, i, cap; int fd; } DIR;
struct dirent { char d_name[RB_PATH_MAX]; };
static int rbp_dir_add(void *ctx, const char *name, const struct rb_stat *st) {
    DIR *d = ctx;
    (void)st;
    if (d->n == d->cap) {
        void *t = realloc(d->names, (d->cap ? d->cap * 2 : 64) * RB_PATH_MAX);
        if (!t) return 1;
        d->names = t;
        d->cap = d->cap ? d->cap * 2 : 64;
    }
    rb_strlcpy(d->names[d->n++], name, RB_PATH_MAX);
    return 0;
}
/* the folder's whole listing at once; closedir closes fd (as fdopendir owns it) */
static DIR *fdopendir(int fd) {
    DIR *d = calloc(1, sizeof *d);
    if (d && rb_each_name(fd, rbp_dir_add, d)) { free(d->names); free(d); d = NULL; }
    if (d) d->fd = fd;
    return d;
}
static DIR *opendir(const char *path) {
    int fd = rb_openat(AT_FDCWD, path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
    if (fd >= 0 && !d) close(fd);
    return d;
}
static struct dirent *readdir(DIR *d) {
    static __thread struct dirent e;
    if (d->i >= d->n) return NULL;
    rb_strlcpy(e.d_name, d->names[d->i++], sizeof e.d_name);
    return &e;
}
static int closedir(DIR *d) { if (d) { if (d->fd >= 0) close(d->fd); free(d->names); free(d); } return 0; }

/* ---------------------------------------------------------------- getline */

static ssize_t getline(char **line, size_t *cap, FILE *f) {
    size_t n = 0;
    int c;
    if (!*line || !*cap) { *cap = 256; *line = malloc(*cap); if (!*line) return -1; }
    while ((c = fgetc(f)) != EOF) {
        if (n + 2 > *cap) {
            char *t = realloc(*line, *cap * 2);
            if (!t) return -1;
            *line = t; *cap *= 2;
        }
        (*line)[n++] = (char)c;
        if (c == '\n') break;
    }
    if (n == 0) return -1;
    (*line)[n] = 0;
    return (ssize_t)n;
}

/* ---------------------------------------------------------------- libraries */

#define RTLD_NOW 0
#define RTLD_LOCAL 0
static void *dlopen(const char *path, int flags) {
    wchar_t *w = rb_wide(path);
    HMODULE h = w ? LoadLibraryExW(w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH) : NULL;
    free(w);
    (void)flags;
    return h;
}
#define dlsym(h, name) ((void *)GetProcAddress((HMODULE)(h), (name)))

/* ---------------------------------------------------------------- the rest prepare.c uses */

#include <signal.h>
#include <wchar.h>
static FILE *rbp_fopen(const char *path, const char *mode) {      /* UTF-8 path */
    wchar_t *w = rb_wide(path), wm[8];
    FILE *f = NULL;
    size_t i = 0;
    for (; mode[i] && i < 7; i++) wm[i] = (wchar_t)mode[i];
    wm[i] = 0;
    if (w) f = _wfopen(w, wm);
    free(w);
    return f;
}
#define fopen rbp_fopen

#define AT_REMOVEDIR 0x200
static int utimensat(int dir, const char *name, const void *times, int flag) {      /* times NULL: now */
    int fd = rb_openat(dir, name, O_RDONLY | (flag & AT_SYMLINK_NOFOLLOW ? O_NOFOLLOW : 0));
    int r;
    (void)times;
    if (fd < 0) return -1;
    r = rb_futimes_now(fd);
    close(fd);
    return r;
}

struct statfs { unsigned long long f_bsize, f_blocks, f_bavail; };
static int rbp_statfs(const char *path, struct statfs *fs) {
    wchar_t *w = rb_wide(path);
    ULARGE_INTEGER avail, total, freeb;
    BOOL ok = w && GetDiskFreeSpaceExW(w, &avail, &total, &freeb);
    free(w);
    if (!ok) return -1;
#ifdef RBP_TEST                                 /* tests: a roomy disk, whatever this one has */
    if (getenv("RBSP_TEST_FREE_GB")) { total.QuadPart = 1000ULL << 30; avail.QuadPart = (unsigned long long)atoi(getenv("RBSP_TEST_FREE_GB")) << 30; }
#endif
    fs->f_bsize = 4096;
    fs->f_blocks = total.QuadPart / 4096;
    fs->f_bavail = avail.QuadPart / 4096;
    return 0;
}
#define statfs(path, fs) rbp_statfs((path), (fs))

#undef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
static int rbp_clock_gettime(int clk, struct timespec *ts) {
    LARGE_INTEGER f, c;
    (void)clk;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    ts->tv_sec = (time_t)(c.QuadPart / f.QuadPart);
    ts->tv_nsec = (long)((c.QuadPart % f.QuadPart) * 1000000000LL / f.QuadPart);
    return 0;
}
#define clock_gettime rbp_clock_gettime

/* a Windows path: C:\... or C:/... or \\server\... (prepare.c's "path[0] != '/'") */
static int rbp_abs(const char *p) { return (((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) || (p[0] == '\\' && p[1] == '\\'); }

static const wchar_t *rbp_wide_path(const char *p) { return rb_wide(p); }      /* ONNX Runtime's ORTCHAR_T; lives to the exit */

static const char *dlerror(void) { return "LoadLibrary failed"; }

/* glue_win.c: SIGTERM = the stop event (Local\rbsp-prepare-<pid>-stop), SIGINT = Ctrl+C;
 * rbp_pause_wait blocks while the pause event is set */
typedef void (*rbp_handler)(int);
rbp_handler rbp_signal(int sig, rbp_handler fn);
void rbp_pause_wait(void);
#define signal rbp_signal
#endif
