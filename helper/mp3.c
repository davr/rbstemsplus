/* rekordbox 7.2.19's MP3 stream (rbxfrm::FileReadMp3, from its arm64 code), read from sample 0:
 * its frame map (FrameAnalyzer::analyze, checkContinuousMpegSync, checkXingVbri), then each mapped
 * frame fed alone to rekordbox's own libmpg123, set up as decoderOpen sets it up, one
 * decode_frame per feed, with its lag bookkeeping. Ported from the verified prototype
 * (mp3rule/rbmp3emu.py, rbmp3fast.py): every rule below, including the odd ones, is rekordbox's. */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "rbsp.h"
#include "big.h"

static const int M1_BR[3][16] = {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, -1},
                                 {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, -1},
                                 {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, -1}};
static const int M2_BR[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, -1};
static const int M1_SR[3] = {44100, 48000, 32000}, M2_SR[3] = {22050, 24000, 16000};
#define MASK 0xFFFE0CC0u                 /* sync + version + layer, sample rate, channel mode */

typedef struct { size_t off, start, size; } frame;
typedef struct { const unsigned char *d; size_t n; } buf;

/* 4 bytes at pos, or NULL past the end */
static const unsigned char *at4(const buf *b, size_t pos) { return pos + 4 <= b->n ? b->d + pos : NULL; }

static int hdr_ok(const unsigned char *b) {   /* byte0 FF, byte1 >= F0, layer != 0, bitrate 1..14, rate != 3 */
    return b[0] == 0xFF && b[1] >= 0xF0 && ((b[1] >> 1) & 3) && b[2] >= 0x10 && (b[2] >> 4) != 15 && ((b[2] >> 2) & 3) != 3;
}

static size_t fsize(const unsigned char *b) {
    int ver = (b[1] >> 3) & 1, layer = (b[1] >> 1) & 3;
    int br = ver ? M1_BR[layer ^ 3][b[2] >> 4] : M2_BR[b[2] >> 4];
    int sr = (ver ? M1_SR : M2_SR)[(b[2] >> 2) & 3];
    long spf = ver ? 1152 : 576;
    return (size_t)(((spf >> 3) * (long)br * 1000) / sr) + ((b[2] >> 1) & 1);
}

static uint32_t masked(const unsigned char *b) { return ((uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]) & MASK; }

/* 0 = three frames in a row with the same masked header, 1 = no, 2 = the end or "TAG" */
static int check_continuous(const buf *d, size_t pos, uint32_t mh) {
    size_t p = pos;
    for (int k = 0; k < 3; k++) {
        const unsigned char *b = at4(d, p);
        if (!b || !memcmp(b, "TAG", 3)) return 2;
        if (k < 2) {
            if (!hdr_ok(b) || masked(b) != mh) return 1;
            p += fsize(b);
        } else if (b[0] != 0xFF || b[1] < 0xF0 || !((b[1] >> 1) & 3) || ((b[2] >> 2) & 3) == 3 || !(b[2] >= 0x10 && b[2] <= 0xEF)
                   || masked(b) != mh) {
            return 1;
        }
    }
    return 0;
}

typedef struct { int xing, info; } xstate;

static int check_xing_vbri(const buf *d, size_t pos, int side, xstate *st) {
    size_t a = pos + 4 + (size_t)side;
    if (a + 12 > d->n) return -1;
    const unsigned char *b = d->d + a;
    if (!memcmp(b, "Xing", 4) && (b[7] & 1)) { st->xing = 1; return 0; }
    if (!memcmp(b, "Info", 4)) st->info = 1;
    if (st->xing) return 0;
    if (a + 18 > d->n) return -1;
    if (side != 32) {
        if (pos + 54 > d->n) return -1;
        b = d->d + pos + 36;
    }
    if (!memcmp(b, "VBRI", 4)) st->xing = 1;
    return 0;
}

/* FrameAnalyzer::analyze: the frame map, and H (the samples of the Xing/Info/VBRI frame, never fed) */
static frame *analyze(const buf *d, size_t *count, size_t *H) {
    size_t cap = 1024, nf = 0, samples = 0, pos = 0;
    frame *F = malloc(sizeof(frame) * cap);
    if (!F) return NULL;
    int first = 1, mism = 0, ref[4] = {0};
    xstate st = {0, 0};
    *H = 0;
    for (;;) {
        const unsigned char *b = at4(d, pos);
        if (!b) break;
        if (!memcmp(b, "ID3", 3)) {
            if (pos + 5 > d->n) break;
            int vv = d->d[pos + 3] * 10 + d->d[pos + 4];
            if (vv == 20 || vv == 21 || vv == 30 || vv == 31 || vv == 40) {
                if (pos + 10 > d->n) break;      /* "ID3V2 tag size error" */
                const unsigned char *s = d->d + pos + 6;
                size_t n = (size_t)(s[0] & 0x7F) << 21 | (size_t)(s[1] & 0x7F) << 14 | (size_t)(s[2] & 0x7F) << 7 | (s[3] & 0x7F);
                pos = pos + n + 10;
            } else {
                pos += 3;
            }
            continue;
        }
        if (!hdr_ok(b)) { pos++; continue; }
        int ver = (b[1] >> 3) & 1, layer = (b[1] >> 1) & 3, sri = (b[2] >> 2) & 3, chm = b[3] >> 6;
        size_t spf = ver ? 1152 : 576;
        if (first) {
            int r = check_continuous(d, pos, masked(b));
            if (r == 1) { pos++; continue; }
            if (r == 2) break;
            size_t size = fsize(b);
            int side = ver ? (chm == 3 ? 17 : 32) : (chm == 3 ? 9 : 17);
            if (check_xing_vbri(d, pos, side, &st) < 0) break;
            if (st.xing || st.info) *H = spf;
            if (nf == cap) { frame *g = realloc(F, sizeof(frame) * (cap *= 2)); if (!g) { free(F); return NULL; } F = g; }
            F[nf++] = (frame){pos, samples, size};
            samples += spf;
            pos += size;
            first = 0;
            ref[0] = ver; ref[1] = layer; ref[2] = sri; ref[3] = chm;
            continue;
        }
        size_t size = fsize(b);
        if (ver == ref[0] && layer == ref[1] && sri == ref[2] && chm == ref[3]) {
            if (nf == cap) { frame *g = realloc(F, sizeof(frame) * (cap *= 2)); if (!g) { free(F); return NULL; } F = g; }
            F[nf++] = (frame){pos, samples, size};
            samples += spf;
            pos += size;
            mism = 0;
            continue;
        }
        if (nf < 4) {                    /* restart one byte after the last mapped frame */
            pos = nf ? F[nf - 1].off + 1 : pos;
            nf = 0; samples = 0; mism = 0; first = 1;
            st.xing = st.info = 0;
            continue;
        }
        if (mism > 3) break;             /* the fifth mismatch in a row */
        mism++;
        pos++;
    }
    *count = nf;
    return F;
}

/* the libmpg123 calls rekordbox makes */
typedef struct mpg123_handle_struct mpg123_handle;
static struct {
    int (*init)(void);
    mpg123_handle *(*new_)(const char *, int *);
    void (*delete_)(mpg123_handle *);
    int (*param)(mpg123_handle *, int, long, double);
    int (*format_none)(mpg123_handle *);
    int (*format)(mpg123_handle *, long, int, int);
    int (*open_feed)(mpg123_handle *);
    int (*feed)(mpg123_handle *, const unsigned char *, size_t);
    int (*decode_frame)(mpg123_handle *, off_t *, unsigned char **, size_t *);
    int ready;
} m;

static int load_mpg123(const char *lib) {
    if (m.ready) return 0;
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) return 1;
    m.init = (int (*)(void))dlsym(h, "mpg123_init");
    m.new_ = (mpg123_handle * (*)(const char *, int *)) dlsym(h, "mpg123_new");
    m.delete_ = (void (*)(mpg123_handle *))dlsym(h, "mpg123_delete");
    m.param = (int (*)(mpg123_handle *, int, long, double))dlsym(h, "mpg123_param");
    m.format_none = (int (*)(mpg123_handle *))dlsym(h, "mpg123_format_none");
    m.format = (int (*)(mpg123_handle *, long, int, int))dlsym(h, "mpg123_format");
    m.open_feed = (int (*)(mpg123_handle *))dlsym(h, "mpg123_open_feed");
    m.feed = (int (*)(mpg123_handle *, const unsigned char *, size_t))dlsym(h, "mpg123_feed");
    m.decode_frame = (int (*)(mpg123_handle *, off_t *, unsigned char **, size_t *))dlsym(h, "mpg123_decode_frame");
    if (!m.init || !m.new_ || !m.delete_ || !m.param || !m.format_none || !m.format || !m.open_feed || !m.feed || !m.decode_frame)
        return 1;
    m.init();
    m.ready = 1;
    return 0;
}

#define NEED_MORE (-10)

static int feed_decode(mpg123_handle *h, const buf *d, const frame *f, unsigned char **pcm, size_t *nb) {
    size_t len = f->size < 0x2000 ? f->size : 0x2000;
    if (f->off >= d->n) len = 0;
    else if (len > d->n - f->off) len = d->n - f->off;
    m.feed(h, d->d + f->off, len);
    off_t num = 0;
    *pcm = NULL;
    *nb = 0;
    return m.decode_frame(h, &num, pcm, nb);
}

static int read_file(const char *path, buf *b) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size <= 0 || st.st_size > (2LL << 30)) { if (fd >= 0) close(fd); return 1; }
    unsigned char *d = big_alloc((size_t)st.st_size);
    size_t got = 0;
    while (d && got < (size_t)st.st_size) {
        ssize_t r = read(fd, d + got, (size_t)st.st_size - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!d || got != (size_t)st.st_size) { big_free(d); return 1; }
    b->d = d;
    b->n = got;
    return 0;
}

int rb_mp3(const char *path, const char *libmpg123, float **out, size_t *n_out, char *why, size_t whyn) {
    if (load_mpg123(libmpg123)) { snprintf(why, whyn, "rekordbox's MP3 decoder (libmpg123) can't be loaded"); return 1; }
    buf d;
    if (read_file(path, &d)) { snprintf(why, whyn, "the file can't be read"); return 1; }
    size_t nf, H;
    frame *F = analyze(&d, &nf, &H);
    if (!F || !nf) {
        free(F); big_free((void *)d.d);
        snprintf(why, whyn, "MP3 the frame scanner rejects: rekordbox finds no frame map");
        return 1;
    }
    const unsigned char *b = d.d + F[0].off;
    int ver = (b[1] >> 3) & 1;
    int rate = (ver ? M1_SR : M2_SR)[(b[2] >> 2) & 3];
    int ch = (b[3] >> 6) == 3 ? 1 : 2;
    size_t spf = ver ? 1152 : 576;
    int err = 0;
    mpg123_handle *h = m.new_(NULL, &err);
    if (!h) { free(F); big_free((void *)d.d); snprintf(why, whyn, "libmpg123 can't start"); return 1; }
    m.param(h, 0, 0, 0.0);               /* VERBOSE 0 */
    m.param(h, 1, 0x300, 0.0);           /* FLAGS: SEEKBUFFER | FUZZY, no GAPLESS */
    m.param(h, 15, -1, 0.0);             /* INDEX_SIZE -1 */
    m.format_none(h);
    m.format(h, rate, ch, 0xd0);         /* signed 16-bit */
    unsigned char *pcm;
    size_t nb;
    m.open_feed(h);                      /* decoderOpen: probe until the first non-NEED_MORE */
    for (size_t i = 0; i < nf; i++)
        if (F[i].start >= H && feed_decode(h, &d, &F[i], &pcm, &nb) != NEED_MORE) break;
    m.open_feed(h);                      /* decode(0, 0, reopen = true) */
    size_t n = nf * spf, fc = 0;
    int16_t *s16 = big_calloc(n * (size_t)ch, sizeof(int16_t));
    int rc = s16 ? 0 : 1;
    if (rc) snprintf(why, whyn, "out of memory");
    for (size_t i = 0; i < nf && !rc; i++) {
        if (F[i].start < H) continue;    /* the Xing/Info/VBRI frame: never fed, spf zero samples */
        int r = feed_decode(h, &d, &F[i], &pcm, &nb);
        if (r == NEED_MORE && i + 1 < nf) { fc += F[i + 1].start - F[i].start; continue; }
        if (nb != spf * (size_t)ch * 2) {
            /* rekordbox would fill zeros to the end of its current read and seek again: that
             * depends on its read sizes, not modelled */
            snprintf(why, whyn, "MP3 the frame scanner rejects: frame %zu decodes to %zu bytes", i, nb);
            rc = 1;
            break;
        }
        size_t p = F[i].start - fc;
        if (p + spf > n) { snprintf(why, whyn, "MP3 frame bookkeeping out of range"); rc = 1; break; }
        memcpy(s16 + p * (size_t)ch, pcm, nb);
    }
    m.delete_(h);
    free(F);
    big_free((void *)d.d);
    if (rc) { big_free(s16); return 1; }
    float *f = big_alloc(sizeof(float) * 2 * n);
    if (!f) { big_free(s16); snprintf(why, whyn, "out of memory"); return 1; }
    const float k = 1.0f / 32768.0f;
    for (size_t i = 0; i < n; i++) {
        f[i] = s16[i * (size_t)ch] * k;
        f[n + i] = s16[i * (size_t)ch + (size_t)(ch - 1)] * k;
    }
    big_free(s16);
    *out = f;
    *n_out = n;
    return 0;
}
