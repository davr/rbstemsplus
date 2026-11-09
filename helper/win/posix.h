/* helper/win/posix.h: the POSIX calls the macOS helper's sources use, over the bridge's Windows layer
 * (bridge/win/win_compat.h: UTF-8 paths, openat-style calls that never follow a link). Included
 * first (-include) when compiling the macOS helper's files that need no other change: manifest.c,
 * mp3.c, mp4.c, resample.c. Also the shim the Windows versions of the rest build on. */
#ifndef RBSP_WIN_POSIX_H
#define RBSP_WIN_POSIX_H
#define _POSIX_C_SOURCE 200809L
#define _DIRENT_H_                      /* mingw's dirent.h (ANSI paths): this file has its own */
#include <sys/stat.h>                   /* before `stat` becomes a macro */
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

typedef struct { char (*names)[RB_PATH_MAX]; size_t n, i, cap; } DIR;
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
/* the folder's whole listing at once; fd is closed by closedir only when opendir made it */
static DIR *fdopendir(int fd) {
    DIR *d = calloc(1, sizeof *d);
    if (d && rb_each_name(fd, rbp_dir_add, d)) { free(d->names); free(d); d = NULL; }
    return d;
}
static DIR *opendir(const char *path) {
    int fd = rb_openat(AT_FDCWD, path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
    if (fd >= 0) close(fd);
    return d;
}
static struct dirent *readdir(DIR *d) {
    static __thread struct dirent e;
    if (d->i >= d->n) return NULL;
    rb_strlcpy(e.d_name, d->names[d->i++], sizeof e.d_name);
    return &e;
}
static int closedir(DIR *d) { if (d) { free(d->names); free(d); } return 0; }

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
#endif
