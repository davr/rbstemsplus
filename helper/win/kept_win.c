/* GENERATED once from helper/kept.c: clang blocks -> a callback, the home folder -> %APPDATA%. The rest is identical. */
/* kept.c: kept sets and the cache's room (kept.h) */
#include "kept.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define KEPT_FILE_MAX (8 << 20)                 /* the bridge's */
#define KEPT_LINE 130
static const char HEAD[] = "rbstemsplus-kept 1\nuntil ";

/* ---------------------------------------------------------------- keys */

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static int unhex64(const char *s, unsigned char out[32]) {     /* 64 lowercase hex digits */
    for (int i = 0; i < 32; i++) {
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (unsigned char)(hi << 4 | lo);
    }
    return 0;
}

static void hex32(const unsigned char in[32], char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", in[i]);
}

/* a set of 32-byte keys (open addressing; an all-zero key, never a SHA-256 in practice, marks a
 * free slot). A chunk's key is made from its model's folder name, so the key alone names it. */
typedef struct { unsigned char (*s)[32]; size_t n, cap; } kset;
static const unsigned char ZERO[32];

static size_t slot(const kset *t, const unsigned char *k) {
    uint64_t h;
    memcpy(&h, k, 8);
    size_t i = (size_t)h & (t->cap - 1);
    while (memcmp(t->s[i], ZERO, 32) && memcmp(t->s[i], k, 32)) i = (i + 1) & (t->cap - 1);
    return i;
}

static int kset_has(const kset *t, const unsigned char *k) { return t->cap && memcmp(t->s[slot(t, k)], ZERO, 32); }

/* 1 if added, 0 if there already, -1 out of memory */
static int kset_add(kset *t, const unsigned char *k) {
    if (!memcmp(k, ZERO, 32)) return 0;
    if ((t->n + 1) * 2 > t->cap) {
        kset b = {calloc(t->cap ? t->cap * 2 : 1024, 32), 0, t->cap ? t->cap * 2 : 1024};
        if (!b.s) return -1;
        for (size_t i = 0; i < t->cap; i++)
            if (memcmp(t->s[i], ZERO, 32)) { memcpy(b.s[slot(&b, t->s[i])], t->s[i], 32); b.n++; }
        free(t->s);
        *t = b;
    }
    size_t i = slot(t, k);
    if (memcmp(t->s[i], ZERO, 32)) return 0;
    memcpy(t->s[i], k, 32);
    t->n++;
    return 1;
}

/* backward-shift deletion: what follows in its probe run moves up, so no search stops short */
static void kset_del(kset *t, const unsigned char *k) {
    if (!t->cap) return;
    size_t i = slot(t, k), mask = t->cap - 1;
    if (!memcmp(t->s[i], ZERO, 32)) return;
    for (size_t j = (i + 1) & mask; memcmp(t->s[j], ZERO, 32); j = (j + 1) & mask) {
        uint64_t h;
        memcpy(&h, t->s[j], 8);
        if (((j - ((size_t)h & mask)) & mask) >= ((j - i) & mask)) { memcpy(t->s[i], t->s[j], 32); i = j; }
    }
    memset(t->s[i], 0, 32);
    t->n--;
}

/* entries as written: model folder and key */
typedef struct { unsigned char m[32], k[32]; } line;
typedef struct { line *v; size_t n, cap; } lines;

static int lines_add(lines *l, const unsigned char *m, const unsigned char *k) {
    if (l->n == l->cap) {
        line *t = realloc(l->v, (l->cap ? l->cap * 2 : 1024) * sizeof *t);
        if (!t) return -1;
        l->v = t;
        l->cap = l->cap ? l->cap * 2 : 1024;
    }
    memcpy(l->v[l->n].m, m, 32);
    memcpy(l->v[l->n].k, k, 32);
    l->n++;
    return 0;
}

static int by_line(const void *a, const void *b) { return memcmp(a, b, sizeof(line)); }

struct kept {
    char dir[PATH_MAX], name[65], cache[PATH_MAX];
    time_t until;
    kset others, ours, fresh;           /* other sets' keys; this set's (earlier and this run's); this run's */
    kset held;                          /* this set's earlier keys a track of this run still holds */
    kset counted;                       /* the keys whose size is in bytes (one not in the cache has none yet) */
    lines earlier, now;                 /* this set's earlier file; this run's entries */
    long long bytes;
};

/* ---------------------------------------------------------------- files */

void kept_default_dir(char *buf, size_t n) {         /* %APPDATA%\rbstemsplus\kept, as the bridge's */
    wchar_t *w = NULL;
    buf[0] = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &w)) && w) {
        char u[RB_PATH_MAX];
        if (rb_utf8(w, -1, u, sizeof u) == 0) snprintf(buf, n, "%s\\rbstemsplus\\kept", u);
        CoTaskMemFree(w);
    }
}

/* a kept file's bytes, as the bridge reads them (parse_kept): its date and, for each entry,
 * f(model, key); -1 if they aren't exactly the format */
static int parse(const char *b, size_t n, long long *until, void (*f)(void *, const unsigned char *, const unsigned char *), void *ctx) {
    size_t h = sizeof HEAD - 1, i;
    long long u = 0;
    if (n < h || memcmp(b, HEAD, h)) return -1;
    for (i = h; i < n && i - h < 12 && b[i] >= '0' && b[i] <= '9'; i++) u = u * 10 + (b[i] - '0');
    if (i == h || i >= n || b[i] != '\n' || (b[h] == '0' && i - h > 1)) return -1;
    i++;
    if ((n - i) % KEPT_LINE) return -1;
    for (size_t j = i; j < n; j += KEPT_LINE) {
        unsigned char m[32], k[32];
        if (unhex64(b + j, m) || b[j + 64] != ' ' || unhex64(b + j + 65, k) || b[j + 129] != '\n') return -1;
    }
    *until = u;
    for (; f && i < n; i += KEPT_LINE) {
        unsigned char m[32], k[32];
        unhex64(b + i, m);
        unhex64(b + i + 65, k);
        f(ctx, m, k);
    }
    return 0;
}

/* a whole file (at most KEPT_FILE_MAX, a plain file not followed through a link), or NULL */
static char *slurp(int dir, const char *name, size_t *len) {
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat st;
    char *b = NULL;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size <= KEPT_FILE_MAX && (b = malloc((size_t)st.st_size + 1))) {
        size_t got = 0;
        ssize_t r = 0;
        while (got <= (size_t)st.st_size && (r = read(fd, b + got, (size_t)st.st_size + 1 - got)) > 0) got += (size_t)r;
        if (r < 0 || got != (size_t)st.st_size) { free(b); b = NULL; }
        else *len = got;
    }
    close(fd);
    return b;
}

/* the disk space of entry MODEL/KEY in CACHE: its .flac, .desc and their companions */
static long long entry_bytes(const char *cache, const char *model, const char *key) {
    static const char *const forms[] = {"%s.flac", "%s.desc", "._%s.flac", "._%s.desc"};
    char p[PATH_MAX + 200], name[80];
    long long sum = 0;
    if (!cache[0]) return 0;                /* no cache to look at (--quick, another drive) */
    snprintf(p, sizeof p, "%s/%s/%.2s", cache, model, key);
    int x = open(p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (x < 0) return 0;
    for (int i = 0; i < 4; i++) {
        struct stat st;
        snprintf(name, sizeof name, forms[i], key);
        if (fstatat(x, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode)) sum += (long long)st.st_blocks * 512;
    }
    close(x);
    return sum;
}

long long kept_entry_bytes(const char *cache, const char *model, const char *key) {
    char p[PATH_MAX + 200];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s/%.2s/%s.flac", cache, model, key, key);
    if (lstat(p, &st) || !S_ISREG(st.st_mode)) return -1;
    return entry_bytes(cache, model, key);
}

static int mkdirs(const char *path) {                /* both separators; "C:" and the ones that exist are left */
    char p[PATH_MAX];
    if (snprintf(p, sizeof p, "%s", path) >= (int)sizeof p) return -1;
    for (char *c = p + 1; *c; c++)
        if (*c == '/' || *c == '\\') { char sep = *c; *c = 0; if (mkdir(p, 0755) && errno != EEXIST) { /* only the last one counts */ } *c = sep; }
    return mkdir(p, 0755) && errno != EEXIST ? -1 : 0;
}

static int name_ok(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 64) return 0;
    for (; *s; s++)
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return 0;
    return 1;
}

static int is_set(const char *name) {           /* <name>.kept, not a dot file (someone's temporary) */
    size_t l = strlen(name);
    return name[0] != '.' && l > 5 && !strcmp(name + l - 5, ".kept");
}

typedef struct { kept *k; int mine, oom; } scan;

static void scan_one(void *ctx, const unsigned char *m, const unsigned char *key) {
    scan *c = ctx;
    kept *k = c->k;
    int r = kset_add(c->mine ? &k->ours : &k->others, key);
    if (r < 0 || (c->mine && lines_add(&k->earlier, m, key))) { c->oom = 1; return; }
    /* this set's own count only as its current tracks hold them (kept_hold, kept_add) */
    if (!c->mine && r == 1 && !kset_has(&k->counted, key)) {
        char mh[65], kh[65];
        hex32(m, mh);
        hex32(key, kh);
        long long size = entry_bytes(k->cache, mh, kh);
        if (size > 0) {
            if (kset_add(&k->counted, key) < 0) { c->oom = 1; return; }
            k->bytes += size;
        }
    }
}

kept *kept_open(const char *dir, const char *name, time_t until, const char *cache, char *why, size_t n) {
    if (name && !name_ok(name)) { snprintf(why, n, "a kept set's name is 1 to 64 letters, digits, - or _: %s", name); return NULL; }
    if (name && (!dir[0] || mkdirs(dir))) { snprintf(why, n, "the kept sets' folder can't be made: %s", dir[0] ? strerror(errno) : "no home folder"); return NULL; }
    kept *k = calloc(1, sizeof *k);
    if (!k) { snprintf(why, n, "out of memory"); return NULL; }
    snprintf(k->dir, sizeof k->dir, "%s", dir);
    snprintf(k->name, sizeof k->name, "%s", name ? name : "");
    snprintf(k->cache, sizeof k->cache, "%s", cache);
    k->until = until;
    int d = dir[0] ? open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (d < 0 && !name && (!dir[0] || errno == ENOENT)) return k;          /* nothing is kept */
    if (d < 0) { snprintf(why, n, "the kept sets' folder can't be read: %s", strerror(errno)); kept_close(k); return NULL; }
    char own[80] = "";
    if (name) snprintf(own, sizeof own, "%s.kept", name);
    time_t now = time(NULL);
    size_t others = 0;
    scan sc = {NULL, 0, 0};
    DIR *dp = fdopendir(dup(d));
    struct dirent *e;
    while (dp && (e = readdir(dp))) {
        if (!is_set(e->d_name)) continue;
        size_t len = 0;
        long long u = 0;
        char *b = slurp(d, e->d_name, &len);
        int mine = !strcmp(e->d_name, own), ok = b && parse(b, len, &u, NULL, NULL) == 0;
        if (ok && u <= now) {                   /* its date has passed: it goes, with its manifest */
            char mf[NAME_MAX + 16];
            snprintf(mf, sizeof mf, "%.*s.manifest", (int)(strlen(e->d_name) - 5), e->d_name);
            unlinkat(d, e->d_name, 0);
            unlinkat(d, mf, 0);
            free(b);
            continue;
        }
        if (!mine) others++;          /* one the bridge can't read still counts towards its cap */
        sc.k = k; sc.mine = mine;
        if (ok) parse(b, len, &u, scan_one, &sc);
        free(b);
    }
    if (dp) closedir(dp);
    close(d);
    if (!dp || sc.oom) { snprintf(why, n, "the kept sets can't be read: %s", sc.oom ? "out of memory" : strerror(errno)); kept_close(k); return NULL; }
    if (name && others >= KEPT_SETS_MAX) {
        snprintf(why, n, "%d playlists are kept already: stop keeping one first", KEPT_SETS_MAX);
        kept_close(k);
        return NULL;
    }
    return k;
}

void kept_close(kept *k) {
    if (!k) return;
    free(k->others.s);
    free(k->ours.s);
    free(k->fresh.s);
    free(k->counted.s);
    free(k->held.s);
    free(k->earlier.v);
    free(k->now.v);
    free(k);
}

void kept_add(kept *k, const char *model, const char *key, long long bytes) {
    unsigned char m[32], b[32];
    if (unhex64(model, m) || unhex64(key, b)) return;
    if (kset_add(&k->fresh, b) == 1) lines_add(&k->now, m, b);
    kset_add(&k->ours, b);
    if (bytes > 0 && kset_add(&k->counted, b) == 1) k->bytes += bytes;
}

void kept_hold(kept *k, const char *model, const char *key, int count) {
    unsigned char b[32];
    if (unhex64(key, b) || !kset_has(&k->ours, b)) return;
    kset_add(&k->held, b);
    if (!count || kset_has(&k->counted, b)) return;
    long long size = entry_bytes(k->cache, model, key);
    if (size > 0 && kset_add(&k->counted, b) == 1) k->bytes += size;
}

void kept_unhold(kept *k, const char *key) {
    unsigned char b[32];
    if (!unhex64(key, b) && !kset_has(&k->fresh, b)) kset_del(&k->ours, b);
}

long long kept_adds(const kept *k, const char *model, const char *key) {
    unsigned char b[32];
    return unhex64(key, b) || kset_has(&k->counted, b) ? 0 : entry_bytes(k->cache, model, key);
}

void kept_release(kept *k) {
    for (size_t i = 0; i < k->earlier.n; i++)
        if (!kset_has(&k->held, k->earlier.v[i].k)) kset_del(&k->ours, k->earlier.v[i].k);
}

long long kept_bytes(const kept *k) { return k ? k->bytes : 0; }

int kept_has(const kept *k, const char *key) {
    unsigned char b[32];
    return k && !unhex64(key, b) && (kset_has(&k->ours, b) || kset_has(&k->others, b));
}

int kept_write(kept *k, int all_done, char *why, size_t n) {
    lines all = {0};
    for (size_t i = 0; i < k->now.n; i++) lines_add(&all, k->now.v[i].m, k->now.v[i].k);
    for (size_t i = 0; !all_done && i < k->earlier.n; i++)
        if (!kset_has(&k->fresh, k->earlier.v[i].k)) lines_add(&all, k->earlier.v[i].m, k->earlier.v[i].k);
    if (all.n < k->now.n) { free(all.v); snprintf(why, n, "out of memory"); return -1; }
    if (all.n) qsort(all.v, all.n, sizeof *all.v, by_line);
    char tmp[PATH_MAX + 100], path[PATH_MAX + 100];
    snprintf(tmp, sizeof tmp, "%s/.%s.kept.%d.tmp", k->dir, k->name, getpid());
    snprintf(path, sizeof path, "%s/%s.kept", k->dir, k->name);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) {
        if (fd >= 0) close(fd);
        free(all.v);
        snprintf(why, n, "the kept set can't be written: %s", strerror(errno));
        return -1;
    }
    fprintf(f, "%s%lld\n", HEAD, (long long)k->until);
    for (size_t i = 0; i < all.n; i++) {
        char mh[65], kh[65];
        hex32(all.v[i].m, mh);
        hex32(all.v[i].k, kh);
        fprintf(f, "%s %s\n", mh, kh);
    }
    free(all.v);
    int bad = fflush(f) || fsync(fileno(f));
    bad |= fclose(f);
    if (bad || rename(tmp, path)) {
        snprintf(why, n, "the kept set can't be written: %s", strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- room */

typedef struct { unsigned char m[32], k[32]; time_t mtime; long long bytes; } cand;

static int by_use(const void *a, const void *b) {
    time_t x = ((const cand *)a)->mtime, y = ((const cand *)b)->mtime;
    return x < y ? -1 : x > y;
}

static int hexname(const char *s, size_t len) {
    if (strlen(s) != len) return 0;
    for (size_t i = 0; i < len; i++) if (hexval(s[i]) < 0) return 0;
    return 1;
}

long long make_room(const char *cache, const kept *k, long long *used, long long target) {
    cand *v = NULL;
    size_t nv = 0, cv = 0;
    int c = open(cache, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *cd = c >= 0 ? fdopendir(dup(c)) : NULL;
    struct dirent *me;
    while (cd && (me = readdir(cd))) {                 /* <model>/<xx>/<key>.flac */
        unsigned char m[32];
        if (!hexname(me->d_name, 64) || unhex64(me->d_name, m)) continue;
        int md = openat(c, me->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        DIR *mdp = md >= 0 ? fdopendir(dup(md)) : NULL;
        struct dirent *pe;
        while (mdp && (pe = readdir(mdp))) {
            if (!hexname(pe->d_name, 2)) continue;
            int x = openat(md, pe->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            DIR *xp = x >= 0 ? fdopendir(dup(x)) : NULL;
            struct dirent *ee;
            while (xp && (ee = readdir(xp))) {
                size_t l = strlen(ee->d_name);
                unsigned char key[32];
                struct stat st;
                if (l != 69 || strcmp(ee->d_name + 64, ".flac") || strncmp(ee->d_name, pe->d_name, 2)) continue;
                char kh[65];
                memcpy(kh, ee->d_name, 64);
                kh[64] = 0;
                if (!hexname(kh, 64) || unhex64(kh, key)) continue;
                if (k && (kset_has(&k->others, key) || kset_has(&k->ours, key))) continue;
                if (fstatat(x, ee->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) continue;
                if (nv == cv) {
                    cand *t = realloc(v, (cv ? cv * 2 : 1024) * sizeof *t);
                    if (!t) break;
                    v = t;
                    cv = cv ? cv * 2 : 1024;
                }
                memcpy(v[nv].m, m, 32);
                memcpy(v[nv].k, key, 32);
                v[nv].mtime = st.st_mtime;
                v[nv].bytes = entry_bytes(cache, me->d_name, kh);
                nv++;
            }
            if (xp) closedir(xp);
            if (x >= 0) close(x);
        }
        if (mdp) closedir(mdp);
        if (md >= 0) close(md);
    }
    if (cd) closedir(cd);
    if (c >= 0) close(c);
    if (nv) qsort(v, nv, sizeof *v, by_use);
    long long removed = 0;
    for (size_t i = 0; i < nv && *used > target; i++) {
        static const char *const forms[] = {"%s.flac", "%s.desc", "._%s.flac", "._%s.desc"};
        char mh[65], kh[65], p[PATH_MAX + 200];
        hex32(v[i].m, mh);
        hex32(v[i].k, kh);
        snprintf(p, sizeof p, "%s/%s/%.2s", cache, mh, kh);
        int x = open(p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (x < 0) continue;
        int gone = 0;
        for (int f = 0; f < 4; f++) {                   /* the FLAC first: without it there is no entry */
            char name[80];
            snprintf(name, sizeof name, forms[f], kh);
            if (unlinkat(x, name, 0) == 0 && f == 0) gone = 1;
        }
        close(x);
        if (gone) { *used -= v[i].bytes; removed++; }
    }
    free(v);
    return removed;
}
