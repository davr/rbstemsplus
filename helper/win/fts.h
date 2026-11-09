/* the part of fts prepare.c uses: every regular file under a folder, with its st_blocks */
#ifndef RBSP_WIN_FTS_H
#define RBSP_WIN_FTS_H
#define FTS_PHYSICAL 1
#define FTS_NOCHDIR 4
#define FTS_F 8
typedef struct { int fts_info; struct rbp_stat *fts_statp; } FTSENT;
typedef struct { FTSENT *e; struct rbp_stat *st; size_t n, i, cap; } FTS;
static void rbp_fts_walk(FTS *t, int fd);
typedef struct { FTS *t; int fd; } rbp_fts_ctx;
static int rbp_fts_add(void *ctx, const char *name, const struct rb_stat *rs) {
    rbp_fts_ctx *c = ctx;
    if (RB_ISDIR(rs->st_mode)) {
        int sub = rb_openat(c->fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (sub >= 0) { rbp_fts_walk(c->t, sub); close(sub); }
    } else if (RB_ISREG(rs->st_mode)) {
        FTS *t = c->t;
        if (t->n == t->cap) {
            size_t cap = t->cap ? t->cap * 2 : 256;
            void *a = realloc(t->e, cap * sizeof *t->e), *b = realloc(t->st, cap * sizeof *t->st);
            if (!a || !b) return 1;
            t->e = a; t->st = b; t->cap = cap;
        }
        memset(&t->st[t->n], 0, sizeof t->st[0]);
        t->st[t->n].st_blocks = rs->st_blocks;
        t->st[t->n].st_size = rs->st_size;
        t->st[t->n].st_mode = rs->st_mode;
        t->n++;
    }
    return 0;
}
static void rbp_fts_walk(FTS *t, int fd) { rbp_fts_ctx c = {t, fd}; rb_each_name(fd, rbp_fts_add, &c); }
static FTS *fts_open(char *const *paths, int options, void *cmp) {
    FTS *t = calloc(1, sizeof *t);
    int fd;
    (void)options; (void)cmp;
    if (!t) return NULL;
    fd = rb_openat(AT_FDCWD, paths[0], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd >= 0) { rbp_fts_walk(t, fd); close(fd); }
    return t;
}
static FTSENT *fts_read(FTS *t) {
    static FTSENT e;
    if (t->i >= t->n) return NULL;
    e.fts_info = FTS_F;
    e.fts_statp = &t->st[t->i++];
    return &e;
}
static int fts_close(FTS *t) { if (t) { free(t->e); free(t->st); free(t); } return 0; }
#endif
