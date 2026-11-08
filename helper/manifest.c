/* manifest.c: kept sets' manifests (manifest.h) */
#include "manifest.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char HEAD[] = "rbstemsplus-manifest 1";

long long mtime_ns(const struct stat *st) { return (long long)st->st_mtimespec.tv_sec * 1000000000LL + st->st_mtimespec.tv_nsec; }

static int hex64(const char *s) {
    if (strlen(s) != 64) return 0;
    for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return 0;
    return 1;
}

void manifest_free(manifest *m) {
    for (size_t i = 0; i < m->n; i++) { free(m->f[i].path); free(m->f[i].keys); }
    free(m->f);
    memset(m, 0, sizeof *m);
}

static mfile *add(manifest *m, const char *path) {
    if (strchr(path, '\n')) return NULL;
    if (m->n == m->cap) {
        mfile *t = realloc(m->f, (m->cap ? m->cap * 2 : 64) * sizeof *t);
        if (!t) return NULL;
        m->f = t;
        m->cap = m->cap ? m->cap * 2 : 64;
    }
    mfile *f = &m->f[m->n];
    memset(f, 0, sizeof *f);
    if (!(f->path = strdup(path))) return NULL;
    m->n++;
    return f;
}

mfile *manifest_file(manifest *m, const char *path, const struct stat *st) {
    mfile *f = add(m, path);
    if (f) { f->size = (long long)st->st_size; f->mtime_ns = mtime_ns(st); }
    return f;
}

void mfile_key(mfile *f, const char *key) {
    if (!f) return;
    for (size_t i = 0; i < f->nkeys; i++)          /* AAC's lengths share most entries */
        if (!memcmp(f->keys[i], key, 64)) return;
    if (f->nkeys == f->cap) {
        char (*t)[65] = realloc(f->keys, (f->cap ? f->cap * 2 : 32) * sizeof *t);
        if (!t) return;
        f->keys = t;
        f->cap = f->cap ? f->cap * 2 : 32;
    }
    memcpy(f->keys[f->nkeys++], key, 65);
}

void manifest_drop_last(manifest *m) {
    if (!m->n) return;
    m->n--;
    free(m->f[m->n].path);
    free(m->f[m->n].keys);
}

void manifest_keep_only(manifest *m, char **paths, size_t n) {
    size_t to = 0;
    for (size_t i = 0; i < m->n; i++) {
        size_t j = 0;
        while (j < n && strcmp(paths[j], m->f[i].path)) j++;
        if (j < n) m->f[to++] = m->f[i];
        else { free(m->f[i].path); free(m->f[i].keys); }
    }
    m->n = to;
}

const mfile *manifest_by_path(const manifest *m, const char *path) {
    const mfile *f = NULL;
    for (size_t i = 0; i < m->n; i++) if (!strcmp(m->f[i].path, path)) f = &m->f[i];
    return f;
}

const mfile *manifest_find(const manifest *m, const char *path, const struct stat *st) {
    for (size_t i = 0; i < m->n; i++)
        if (!strcmp(m->f[i].path, path) && m->f[i].size == (long long)st->st_size && m->f[i].mtime_ns == mtime_ns(st)) return &m->f[i];
    return NULL;
}

int manifest_read(const char *dir, const char *name, manifest *m) {
    memset(m, 0, sizeof *m);
    char p[PATH_MAX + 100];
    snprintf(p, sizeof p, "%s/%s.manifest", dir, name);
    int fd = open(p, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat st;
    FILE *in = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) ? fdopen(fd, "r") : NULL;
    if (!in) { if (fd >= 0) close(fd); return -1; }
    char *line = NULL;
    size_t cap = 0;
    ssize_t r;
    int n = 0, bad = 0;
    mfile *f = NULL;
    size_t want = 0;
    while (!bad && (r = getline(&line, &cap, in)) > 0) {
        if (line[r - 1] != '\n') { bad = 1; break; }
        line[--r] = 0;
        n++;
        if (n == 1) bad = strcmp(line, HEAD) != 0;
        else if (n == 2) bad = sscanf(line, "helper %15s", m->helper) != 1;
        else if (n == 3) {
            if (!(bad = strncmp(line, "model ", 6) || !hex64(line + 6))) memcpy(m->model, line + 6, 65);
        } else if (n == 4) {
            if (!(bad = strncmp(line, "playlist ", 9) || strlen(line + 9) > 64)) strcpy(m->playlist, line + 9);
        }
        else if (n == 5) bad = sscanf(line, "rate %lld %lf %d", &m->runs, &m->run_s, &m->threads) != 3;
        else if (want) {
            if (!hex64(line)) { bad = 1; break; }
            mfile_key(f, line);
            want--;
        } else {
            long long size, t;
            int complete, off = 0;
            size_t k;
            if (sscanf(line, "file %lld %lld %d %zu %n", &size, &t, &complete, &k, &off) != 4 || !off || !line[off]
                || !(f = add(m, line + off))) { bad = 1; break; }
            f->size = size;
            f->mtime_ns = t;
            f->complete = complete == 1;
            want = k;
        }
    }
    free(line);
    fclose(in);
    if (bad || n < 5 || want) { manifest_free(m); return -1; }
    if (!strcmp(m->playlist, "-")) m->playlist[0] = 0;
    return 0;
}

static void put(FILE *o, const mfile *f) {
    fprintf(o, "file %lld %lld %d %zu %s\n", f->size, f->mtime_ns, f->complete, f->nkeys, f->path);
    for (size_t k = 0; k < f->nkeys; k++) fprintf(o, "%s\n", f->keys[k]);
}

int manifest_write(const char *dir, const char *name, const manifest *m, const manifest *earlier, char *why, size_t n) {
    char tmp[PATH_MAX + 100], path[PATH_MAX + 100];
    snprintf(tmp, sizeof tmp, "%s/.%s.manifest.%d.tmp", dir, name, getpid());
    snprintf(path, sizeof path, "%s/%s.manifest", dir, name);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    FILE *o = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!o) {
        if (fd >= 0) close(fd);
        snprintf(why, n, "the set's manifest can't be written: %s", strerror(errno));
        return -1;
    }
    fprintf(o, "%s\nhelper %s\nmodel %s\nplaylist %s\nrate %lld %.3f %d\n", HEAD, m->helper, m->model,
            m->playlist[0] ? m->playlist : "-", m->runs, m->run_s, m->threads);
    for (size_t i = 0; i < m->n; i++) put(o, &m->f[i]);
    for (size_t i = 0; earlier && i < earlier->n; i++) {
        int seen = 0;
        for (size_t j = 0; j < m->n && !seen; j++) seen = !strcmp(m->f[j].path, earlier->f[i].path);
        if (!seen) put(o, &earlier->f[i]);
    }
    int bad = fflush(o) || fsync(fileno(o));
    bad |= fclose(o);
    if (bad || rename(tmp, path)) {
        snprintf(why, n, "the set's manifest can't be written: %s", strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}
