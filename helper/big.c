/* big.c: buffers for whole files and whole decoded tracks (big.h) */
#include "big.h"
#include <fcntl.h>
#include <limits.h>
#include <malloc/malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define BIG_MIN ((size_t)32 << 20)
#define BIG_MAX 64                      /* buffers alive at once: MAX_TRACKS x a few, with room */

typedef struct { void *p; size_t n; int fd; } ent;
static ent tab[BIG_MAX];

static ent *find(const void *p) {
    for (int i = 0; p && i < BIG_MAX; i++) if (tab[i].p == p) return &tab[i];
    return NULL;
}

/* a private file, already unlinked: its pages are the buffer's */
static int backing(size_t n) {
    char dir[PATH_MAX], p[PATH_MAX + 32];
    if (!confstr(_CS_DARWIN_USER_TEMP_DIR, dir, sizeof dir)) return -1;     /* this account's alone */
    snprintf(p, sizeof p, "%s/rbsp-pcm.XXXXXX", dir);
    int fd = mkstemp(p);
    if (fd < 0) return -1;
    unlink(p);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (ftruncate(fd, (off_t)n)) { close(fd); return -1; }
    return fd;
}

static void *mapped(size_t n) {
    ent *e = NULL;
    for (int i = 0; !e && i < BIG_MAX; i++) if (!tab[i].p) e = &tab[i];
    int fd = e ? backing(n) : -1;
    if (fd < 0) return NULL;
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return NULL; }
    *e = (ent){p, n, fd};
    return p;
}

void *big_alloc(size_t n) {
    void *p = n >= BIG_MIN ? mapped(n) : NULL;
    return p ? p : malloc(n ? n : 1);
}

void *big_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    size_t n = count * size;
    void *p = n >= BIG_MIN ? mapped(n) : NULL;     /* a new file reads as zeros */
    return p ? p : calloc(count ? count : 1, size ? size : 1);
}

void *big_realloc(void *p, size_t n) {
    ent *e = find(p);
    if (!e) {
        if (n < BIG_MIN) return realloc(p, n);
        void *q = mapped(n);
        if (!q) return realloc(p, n);
        if (p) { size_t old = malloc_size(p); memcpy(q, p, old < n ? old : n); free(p); }
        return q;
    }
    if (ftruncate(e->fd, (off_t)n)) return NULL;
    void *q = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, e->fd, 0);
    if (q == MAP_FAILED) return NULL;
    munmap(e->p, e->n);
    e->p = q;
    e->n = n;
    return q;
}

void big_free(void *p) {
    ent *e = find(p);
    if (!e) { free(p); return; }
    munmap(e->p, e->n);
    close(e->fd);
    *e = (ent){0};
}
