/* decode.c on Windows: a file as Windows rekordbox's separator reads it (docs/windows-port/PREPARE.md,
 * measured on rekordbox 7.2.14). 44.1 kHz only; other rates are skipped ("not supported on Windows
 * yet"): rekordbox's resampler is not reproduced exactly yet.
 *   MP3         upstream's rb_mp3 (frame map + rekordbox's own libmpg123.dll, 16-bit / 32768)
 *   AAC (M4A)   Media Foundation's source reader asked for 16-bit PCM, / 32768
 *   FLAC        libFLAC;  WAV, AIFF: parsed here;  ALAC: not done yet
 * The planar float output, the chunking and the tracks are decode.c's. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <FLAC/stream_decoder.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include "../rbsp.h"
#include "../big.h"

void tracks_free(tracks *ts) {
    for (int i = 0; i < ts->count; i++)
        if (ts->t[i].owner) { big_free(ts->t[i].s); big_free(ts->t[i].p16); }
    ts->count = 0;
}


static int add_track(tracks *ts, float *s, size_t n) {
    if (ts->count == MAX_TRACKS) { big_free(s); return 1; }
    ts->t[ts->count] = (track){s, NULL, 2, 1, n};
    ts->count++;
    return 0;
}

static void lower_ext(const char *path, char *ext, size_t n) {
    const char *slash = strrchr(path, '/'), *bs = strrchr(path, '\\'), *dot = strrchr(path, '.');
    if (bs && (!slash || bs > slash)) slash = bs;
    ext[0] = 0;
    if (!dot || (slash && dot < slash)) return;
    size_t i = 0;
    for (; dot[i] && i + 1 < n; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    ext[i] = 0;
}

enum { K_PCM, K_FLAC, K_ALAC, K_AAC, K_MP3 };
typedef struct { int kind, rate, ch, bits, flt, big_endian; double seconds; unsigned long long frames, data_at, data_len; } probe_info;

static unsigned be32(const unsigned char *p) { return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static unsigned le32(const unsigned char *p) { return (unsigned)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }
static unsigned le16(const unsigned char *p) { return (unsigned)p[1] << 8 | p[0]; }
static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }

static unsigned char *read_head(const char *path, size_t want, size_t *got, long long *size) {
    FILE *f = fopen(path, "rb");
    unsigned char *b = malloc(want + 1);
    *got = 0;
    if (!f || !b) { if (f) fclose(f); free(b); return NULL; }
    *got = fread(b, 1, want, f);
    _fseeki64(f, 0, SEEK_END);
    *size = _ftelli64(f);
    fclose(f);
    return b;
}

/* WAV: RIFF chunks up to "data" */
static int probe_wav(const char *path, probe_info *p) {
    size_t n;
    long long size;
    unsigned char *b = read_head(path, 1 << 16, &n, &size);
    int ok = 0;
    if (b && n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) {
        unsigned fmt = 0;
        for (size_t i = 12; i + 8 <= n;) {
            unsigned long long len = le32(b + i + 4);
            if (!memcmp(b + i, "fmt ", 4) && len >= 16 && i + 8 + 16 <= n) {
                fmt = le16(b + i + 8);
                if (fmt == 0xFFFE && len >= 26 && i + 8 + 26 <= n) fmt = le16(b + i + 8 + 24);
                p->ch = (int)le16(b + i + 10);
                p->rate = (int)le32(b + i + 12);
                p->bits = (int)le16(b + i + 22);
                p->flt = fmt == 3;
            } else if (!memcmp(b + i, "data", 4)) {
                p->data_at = i + 8;
                p->data_len = len;
                if (p->data_at + len > (unsigned long long)size) p->data_len = (unsigned long long)size - p->data_at;
                if ((fmt == 1 || fmt == 3) && p->ch > 0 && p->bits >= 8 && p->bits % 8 == 0 && p->bits <= 64 && p->rate > 0) {
                    p->frames = p->data_len / ((unsigned)p->ch * (unsigned)p->bits / 8);
                    p->seconds = (double)p->frames / p->rate;
                    ok = 1;
                }
                break;
            }
            i += 8 + len + (len & 1);
        }
    }
    free(b);
    p->kind = K_PCM;
    p->big_endian = 0;
    return ok ? 0 : 1;
}

static double ext80(const unsigned char *p) {         /* AIFF's 80-bit extended sample rate */
    int e = (int)(be16(p) & 0x7FFF) - 16383;
    double m = (double)be32(p + 2) * 4294967296.0 + be32(p + 6);
    return ldexp(m, e - 63);
}

static int probe_aiff(const char *path, probe_info *p) {
    size_t n;
    long long size;
    unsigned char *b = read_head(path, 1 << 16, &n, &size);
    int ok = 0;
    p->kind = K_PCM;
    p->big_endian = 1;
    if (b && n >= 12 && !memcmp(b, "FORM", 4) && (!memcmp(b + 8, "AIFF", 4) || !memcmp(b + 8, "AIFC", 4))) {
        int comp_ok = 1;
        for (size_t i = 12; i + 8 <= n;) {
            unsigned long long len = be32(b + i + 4);
            if (!memcmp(b + i, "COMM", 4) && len >= 18 && i + 8 + 18 <= n) {
                p->ch = (int)be16(b + i + 8);
                p->frames = be32(b + i + 10);
                p->bits = (int)be16(b + i + 14);
                p->rate = (int)(ext80(b + i + 16) + 0.5);
                if (len >= 22 && i + 8 + 22 <= n) {
                    if (!memcmp(b + i + 26, "sowt", 4)) p->big_endian = 0;
                    else if (!memcmp(b + i + 26, "fl32", 4) || !memcmp(b + i + 26, "FL32", 4)) { p->flt = 1; p->bits = 32; }
                    else if (memcmp(b + i + 26, "NONE", 4) && memcmp(b + i + 26, "twos", 4)) comp_ok = 0;
                }
            } else if (!memcmp(b + i, "SSND", 4) && i + 16 <= n) {
                unsigned off = be32(b + i + 8);
                p->data_at = i + 16 + off;
                p->data_len = len >= 8 + off ? len - 8 - off : 0;
                if (p->data_at + p->data_len > (unsigned long long)size) p->data_len = (unsigned long long)size - p->data_at;
                ok = comp_ok && p->ch > 0 && p->bits > 0 && p->rate > 0;
                if (ok) {
                    unsigned long long avail = p->data_len / ((unsigned)p->ch * (unsigned)((p->bits + 7) / 8));
                    if (avail < p->frames) p->frames = avail;
                    p->seconds = (double)p->frames / p->rate;
                }
                break;
            }
            i += 8 + len + (len & 1);
        }
    }
    free(b);
    return ok ? 0 : 1;
}

static int probe_flac(const char *path, probe_info *p) {
    size_t n, o = 0;
    long long size;
    unsigned char *b = read_head(path, 4096, &n, &size);
    int ok = 0;
    if (b && n >= 10 && !memcmp(b, "ID3", 3)) o = 10 + ((size_t)(b[6] & 127) << 21 | (size_t)(b[7] & 127) << 14 | (size_t)(b[8] & 127) << 7 | (b[9] & 127));
    if (b && o == 0 && n >= 26 && !memcmp(b, "fLaC", 4) && (b[4] & 127) == 0) {
        p->rate = (int)((unsigned)b[18] << 12 | b[19] << 4 | b[20] >> 4);
        p->ch = ((b[20] >> 1) & 7) + 1;
        p->bits = (((b[20] & 1) << 4) | b[21] >> 4) + 1;
        p->frames = ((unsigned long long)(b[21] & 15) << 32) | be32(b + 22);
        p->seconds = p->rate > 0 ? (double)p->frames / p->rate : 0;
        ok = p->rate > 0;
    } else if (b && o) ok = 0;                            /* an ID3-prefixed FLAC: not read here */
    free(b);
    p->kind = K_FLAC;
    return ok ? 0 : 1;
}

static int probe_mp3(const char *path, probe_info *p) {
    static const int br1[3][15] = {{0,32,64,96,128,160,192,224,256,288,320,352,384,416,448},
                                   {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384},
                                   {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320}};
    static const int br2[3][15] = {{0,32,48,56,64,80,96,112,128,144,160,176,192,224,256},
                                   {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160},
                                   {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160}};
    static const int sr[4][3] = {{11025,12000,8000},{0,0,0},{22050,24000,16000},{44100,48000,32000}};
    size_t n, i = 0;
    long long size;
    unsigned char *b = read_head(path, 1 << 16, &n, &size);
    int ok = 0;
    p->kind = K_MP3;
    if (!b) return 1;
    if (n >= 10 && !memcmp(b, "ID3", 3)) i = 10 + ((size_t)(b[6] & 127) << 21 | (size_t)(b[7] & 127) << 14 | (size_t)(b[8] & 127) << 7 | (b[9] & 127)) + (b[5] & 16 ? 10 : 0);
    size_t id3 = i;
    if (id3 > n) {                                        /* a long tag: look past it */
        free(b);
        FILE *f = fopen(path, "rb");
        b = malloc(4096);
        if (!f || !b) { if (f) fclose(f); free(b); return 1; }
        _fseeki64(f, (long long)id3, SEEK_SET);
        n = fread(b, 1, 4096, f);
        fclose(f);
        i = 0;
    }
    for (; i + 4 <= n && !ok; i++) {
        if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0) continue;
        int ver = (b[i + 1] >> 3) & 3, layer = (b[i + 1] >> 1) & 3, bi = b[i + 2] >> 4, si = (b[i + 2] >> 2) & 3;
        if (ver == 1 || layer == 0 || bi == 0 || bi == 15 || si == 3) continue;
        int mpeg1 = ver == 3, l = 3 - layer;              /* 0 layer I, 1 II, 2 III */
        int kbps = mpeg1 ? br1[l == 0 ? 0 : l == 1 ? 1 : 2][bi] : br2[l == 0 ? 0 : 1][bi];
        p->rate = sr[ver][si];
        p->ch = (b[i + 3] >> 6) == 3 ? 1 : 2;
        int spf = l == 0 ? 384 : (l == 1 || mpeg1) ? 1152 : 576;
        size_t x = i + 4 + (mpeg1 ? (p->ch == 1 ? 17 : 32) : (p->ch == 1 ? 9 : 17));
        unsigned long long frames = 0;
        if (x + 12 <= n && (!memcmp(b + x, "Xing", 4) || !memcmp(b + x, "Info", 4)) && (b[x + 7] & 1)) frames = be32(b + x + 8);
        else if (i + 4 + 32 + 18 <= n && !memcmp(b + i + 4 + 32, "VBRI", 4)) frames = be32(b + i + 4 + 32 + 14);
        if (frames) p->seconds = (double)frames * spf / p->rate;
        else p->seconds = kbps > 0 ? (double)(size - (long long)(id3 + i)) * 8 / (kbps * 1000.0) : 0;
        ok = 1;
    }
    free(b);
    return ok ? 0 : 1;
}

/* the first box NAME inside B[0..N): its payload */
static const unsigned char *box(const unsigned char *b, size_t n, const char *name, size_t *plen, size_t *from) {
    size_t i = from ? *from : 0;
    while (i + 8 <= n) {
        unsigned long long len = be32(b + i);
        size_t hdr = 8;
        if (len == 1 && i + 16 <= n) { len = ((unsigned long long)be32(b + i + 8) << 32) | be32(b + i + 12); hdr = 16; }
        if (len == 0) len = n - i;
        if (len < hdr || i + len > n) return NULL;
        if (!memcmp(b + i + 4, name, 4)) { *plen = (size_t)len - hdr; if (from) *from = i + (size_t)len; return b + i + hdr; }
        i += (size_t)len;
    }
    return NULL;
}

/* M4A/MP4: the sound track's sample entry (AAC "mp4a", ALAC "alac"), its channels and rate */
static int probe_mp4(const char *path, probe_info *p) {
    FILE *f = fopen(path, "rb");
    unsigned char hdr[16], *moov = NULL;
    size_t moov_n = 0;
    int ok = 0;
    p->kind = K_AAC;
    if (!f) return 1;
    _fseeki64(f, 0, SEEK_END);
    long long size = _ftelli64(f), at = 0;
    while (at + 8 <= size) {                              /* the top-level boxes, to moov */
        _fseeki64(f, at, SEEK_SET);
        if (fread(hdr, 1, 16, f) < 8) break;
        unsigned long long len = be32(hdr);
        size_t h = 8;
        if (len == 1) { len = ((unsigned long long)be32(hdr + 8) << 32) | be32(hdr + 12); h = 16; }
        if (len == 0) len = (unsigned long long)(size - at);
        if (len < h) break;
        if (!memcmp(hdr + 4, "moov", 4) && len - h <= (64u << 20)) {
            moov_n = (size_t)(len - h);
            moov = malloc(moov_n + 1);
            _fseeki64(f, at + (long long)h, SEEK_SET);
            if (!moov || fread(moov, 1, moov_n, f) != moov_n) { free(moov); moov = NULL; }
            break;
        }
        at += (long long)len;
    }
    fclose(f);
    if (!moov) return 1;
    size_t from = 0, tl;
    const unsigned char *trak;
    while (!ok && (trak = box(moov, moov_n, "trak", &tl, &from))) {
        size_t ml, hl, il, sl, tl2;
        const unsigned char *mdia = box(trak, tl, "mdia", &ml, NULL);
        const unsigned char *hdlr = mdia ? box(mdia, ml, "hdlr", &hl, NULL) : NULL;
        if (!hdlr || hl < 12 || memcmp(hdlr + 8, "soun", 4)) continue;
        const unsigned char *minf = box(mdia, ml, "minf", &il, NULL);
        const unsigned char *stbl = minf ? box(minf, il, "stbl", &sl, NULL) : NULL;
        const unsigned char *stsd = stbl ? box(stbl, sl, "stsd", &tl2, NULL) : NULL;
        if (!stsd || tl2 < 8 + 8 + 28) continue;
        const unsigned char *e = stsd + 8;                /* the first entry: size, format, ... */
        if (!memcmp(e + 4, "mp4a", 4)) p->kind = K_AAC;
        else if (!memcmp(e + 4, "alac", 4)) p->kind = K_ALAC;
        else continue;
        p->ch = (int)be16(e + 8 + 16);
        p->rate = (int)be16(e + 8 + 24);
        ok = 1;
    }
    free(moov);
    mp4_audio a;
    if (ok && mp4_audio_track(path, &a) == 0 && a.has_stts && a.timescale) p->seconds = (double)a.stts_total / a.timescale;
    return ok ? 0 : 1;
}

/* ---------------------------------------------------------------- decoding */

/* WAV/AIFF: integers as x / 2^(bits-1) (what rekordbox's reader gives for 8 to 32 bits), floats as
 * they are; the first two channels, mono on both sides */
static int decode_pcm(const char *path, const probe_info *p, float **out, size_t *n_out, char *why, size_t whyn) {
    size_t n = (size_t)p->frames, bytes = (size_t)(p->bits / 8 + (p->bits % 8 != 0)), fr = bytes * (size_t)p->ch;
    FILE *f = fopen(path, "rb");
    float *s = big_alloc(sizeof(float) * 2 * (n ? n : 1));
    unsigned char *buf = malloc(fr * 32768);
    size_t got = 0;
    int rc = 1;
    if (!f || !s || !buf) { snprintf(why, whyn, f ? "out of memory" : "the file can't be opened"); goto done; }
    if (p->flt ? !(p->bits == 32 || p->bits == 64) : !(p->bits == 8 || p->bits == 16 || p->bits == 24 || p->bits == 32)) {
        snprintf(why, whyn, "%d-bit %s samples aren't supported on Windows yet", p->bits, p->flt ? "float" : "integer");
        goto done;
    }
    _fseeki64(f, (long long)p->data_at, SEEK_SET);
    while (got < n) {
        size_t want = n - got < 32768 ? n - got : 32768;
        if (fread(buf, fr, want, f) != want) { snprintf(why, whyn, "decoding stopped early"); goto done; }
        for (size_t i = 0; i < want; i++) {
            float v[2];
            for (int c = 0; c < 2; c++) {
                const unsigned char *q = buf + i * fr + (size_t)(p->ch > 1 ? c : 0) * bytes;
                if (p->flt) {
                    if (p->bits == 32) { unsigned u = p->big_endian ? be32(q) : le32(q); float x; memcpy(&x, &u, 4); v[c] = x; }
                    else { unsigned long long u = 0; for (int k = 0; k < 8; k++) u = p->big_endian ? u << 8 | q[k] : u | (unsigned long long)q[k] << (8 * k); double x; memcpy(&x, &u, 8); v[c] = (float)x; }
                } else {
                    int32_t x;
                    if (bytes == 1) x = p->big_endian ? (int32_t)(signed char)q[0] : (int32_t)q[0] - 128;
                    else if (bytes == 2) x = (int16_t)(p->big_endian ? be16(q) : le16(q));
                    else if (bytes == 3) x = p->big_endian ? (int32_t)((unsigned)q[0] << 24 | q[1] << 16 | q[2] << 8) >> 8 : (int32_t)((unsigned)q[2] << 24 | q[1] << 16 | q[0] << 8) >> 8;
                    else x = (int32_t)(p->big_endian ? be32(q) : le32(q));
                    v[c] = (float)x / (float)(1u << (p->bits - 1)) * (p->bits == 32 ? 1.0f : 1.0f);
                }
            }
            s[got + i] = v[0];
            s[n + got + i] = v[1];
        }
        got += want;
    }
    *out = s;
    s = NULL;
    *n_out = n;
    rc = 0;
done:
    if (f) fclose(f);
    big_free(s);
    free(buf);
    return rc;
}

typedef struct { float *s; size_t n, got; int bps, err; char why[200]; } flac_state;

static FLAC__StreamDecoderWriteStatus flac_write(const FLAC__StreamDecoder *d, const FLAC__Frame *fr, const FLAC__int32 *const buf[], void *u) {
    flac_state *st = u;
    (void)d;
    size_t cnt = fr->header.blocksize;
    if (!st->s || st->got + cnt > st->n) { st->err = 1; snprintf(st->why, sizeof st->why, "longer than its stated length"); return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT; }
    unsigned ch = fr->header.channels, bps = fr->header.bits_per_sample;
    const float k = 1.0f / (float)(1u << (bps - 1));
    for (size_t i = 0; i < cnt; i++) {
        st->s[st->got + i] = (float)buf[0][i] * k;
        st->s[st->n + st->got + i] = (float)buf[ch > 1 ? 1 : 0][i] * k;
    }
    st->got += cnt;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void flac_meta(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *m, void *u) {
    flac_state *st = u;
    (void)d;
    if (m->type == FLAC__METADATA_TYPE_STREAMINFO) st->bps = (int)m->data.stream_info.bits_per_sample;
}
static void flac_err(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus e, void *u) {
    flac_state *st = u;
    (void)d; (void)e;
    st->err = 1;
    snprintf(st->why, sizeof st->why, "decoding stopped with an error");
}

static int decode_flac(const char *path, const probe_info *p, float **out, size_t *n_out, char *why, size_t whyn) {
    flac_state st = {0};
    FILE *f = fopen(path, "rb");
    FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
    int rc = 1;
    st.n = (size_t)p->frames;
    st.s = big_alloc(sizeof(float) * 2 * (st.n ? st.n : 1));
    if (!f || !d || !st.s || !st.n) { snprintf(why, whyn, !st.n ? "no length" : "out of memory"); if (f && d) goto done; }
    else if (FLAC__stream_decoder_init_FILE(d, f, flac_write, flac_meta, flac_err, &st) != FLAC__STREAM_DECODER_INIT_STATUS_OK) { f = NULL; snprintf(why, whyn, "not a FLAC stream"); goto done; }
    else {
        f = NULL;                                          /* the decoder closes it */
        if (!FLAC__stream_decoder_process_until_end_of_stream(d) || st.err) { snprintf(why, whyn, "%s", st.why[0] ? st.why : "decoding stopped with an error"); goto done; }
        if (st.got != st.n) { snprintf(why, whyn, "decoded %zu samples, the file says %zu", st.got, st.n); goto done; }
        *out = st.s;
        st.s = NULL;
        *n_out = st.n;
        rc = 0;
    }
done:
    if (d) { FLAC__stream_decoder_finish(d); FLAC__stream_decoder_delete(d); }
    if (f) fclose(f);
    big_free(st.s);
    return rc;
}

/* AAC through Media Foundation's source reader, 16-bit PCM requested (not float: measured), at
 * the rate and channels the decoder gives. 0 and *pcm (interleaved, big_alloc'd) */
static int decode_aac(const char *path, int16_t **pcm, size_t *frames, int *rate, int *ch, char *why, size_t whyn) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int com = SUCCEEDED(hr), rc = 1, mf = 0;
    IMFSourceReader *rd = NULL;
    IMFMediaType *want = NULL, *got = NULL;
    wchar_t *w = rb_wide(path);
    int16_t *buf = NULL;
    size_t n = 0, cap = 0;
    UINT32 ch_ = 0, rate_ = 0;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { snprintf(why, whyn, "Windows' Media Foundation can't start"); goto done; }
    mf = 1;
    if (!w || FAILED(MFCreateSourceReaderFromURL(w, NULL, &rd))) { snprintf(why, whyn, "Windows can't open it as audio"); goto done; }
    if (FAILED(MFCreateMediaType(&want))) goto done;
    want->lpVtbl->SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    want->lpVtbl->SetGUID(want, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    want->lpVtbl->SetUINT32(want, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    rd->lpVtbl->SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rd->lpVtbl->SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (FAILED(rd->lpVtbl->SetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, want))) { snprintf(why, whyn, "Windows can't decode it to 16-bit PCM"); goto done; }
    if (SUCCEEDED(rd->lpVtbl->GetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got))) {
        got->lpVtbl->GetUINT32(got, &MF_MT_AUDIO_NUM_CHANNELS, &ch_);
        got->lpVtbl->GetUINT32(got, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate_);
    }
    if (!ch_ || !rate_) { snprintf(why, whyn, "Windows reports no audio format"); goto done; }
    for (;;) {
        DWORD flags = 0;
        IMFSample *smp = NULL;
        IMFMediaBuffer *mb = NULL;
        BYTE *data = NULL;
        DWORD len = 0;
        if (FAILED(rd->lpVtbl->ReadSample(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, NULL, &smp))) { snprintf(why, whyn, "decoding stopped with an error"); goto done; }
        if (flags & MF_SOURCE_READERF_ERROR) { if (smp) smp->lpVtbl->Release(smp); snprintf(why, whyn, "decoding stopped with an error"); goto done; }
        if (smp && SUCCEEDED(smp->lpVtbl->ConvertToContiguousBuffer(smp, &mb)) && SUCCEEDED(mb->lpVtbl->Lock(mb, &data, NULL, &len))) {
            size_t add = len / (2 * ch_);
            if (n + add > cap) {
                size_t nc = cap ? cap : (size_t)rate_ * 60;
                while (nc < n + add) nc *= 2;
                int16_t *t = big_realloc(buf, nc * ch_ * sizeof(int16_t));
                if (!t) { mb->lpVtbl->Unlock(mb); mb->lpVtbl->Release(mb); smp->lpVtbl->Release(smp); snprintf(why, whyn, "out of memory"); goto done; }
                buf = t;
                cap = nc;
            }
            memcpy(buf + n * ch_, data, add * ch_ * 2);
            n += add;
            mb->lpVtbl->Unlock(mb);
        }
        if (mb) mb->lpVtbl->Release(mb);
        if (smp) smp->lpVtbl->Release(smp);
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    if (!n) { snprintf(why, whyn, "no audio"); goto done; }
    *pcm = buf;
    buf = NULL;
    *frames = n;
    *rate = (int)rate_;
    *ch = (int)ch_;
    rc = 0;
done:
    big_free(buf);
    if (got) got->lpVtbl->Release(got);
    if (want) want->lpVtbl->Release(want);
    if (rd) rd->lpVtbl->Release(rd);
    if (mf) MFShutdown();
    if (com) CoUninitialize();
    free(w);
    return rc;
}

static size_t scat(char *d, const char *s, size_t n) { size_t l = strlen(d); snprintf(d + l, n - l, "%s", s); return strlen(d); }

/* the files load_tracks refuses before decoding, and why: 0, or 1 and why */
static int refused(const char *path, char *ext, size_t extn, probe_info *p, char *why, size_t whyn) {
    lower_ext(path, ext, extn);
    if (!strcmp(ext, ".aac")) { snprintf(why, whyn, "raw AAC (ADTS): rekordbox can't load it"); return 1; }
    static const char *const exts[] = {".mp3", ".m4a", ".mp4", ".wav", ".aif", ".aiff", ".flac"};
    int known = 0;
    for (size_t i = 0; i < sizeof exts / sizeof *exts; i++)
        if (!strcmp(ext, exts[i])) known = 1;
    if (!known) { snprintf(why, whyn, "%s: not a file type rekordbox plays", ext[0] ? ext : "no extension"); return 1; }
    memset(p, 0, sizeof *p);
    int bad = !strcmp(ext, ".mp3") ? probe_mp3(path, p) : !strcmp(ext, ".wav") ? probe_wav(path, p)
              : !strcmp(ext, ".flac") ? probe_flac(path, p) : (!strcmp(ext, ".aif") || !strcmp(ext, ".aiff")) ? probe_aiff(path, p) : probe_mp4(path, p);
    if (bad) { snprintf(why, whyn, "no audio stream Windows can read"); return 1; }
    if (p->kind == K_ALAC) { snprintf(why, whyn, "ALAC: not supported on Windows yet"); return 1; }
    if (p->kind == K_AAC && p->ch > 2) { snprintf(why, whyn, "AAC with %d channels: rekordbox loads it but doesn't separate it", p->ch); return 1; }
    if (p->rate != 44100) { snprintf(why, whyn, "%d Hz: not supported on Windows yet", p->rate); return 1; }
    return 0;
}

int probe_track(const char *path, double *seconds, int *aac, char *why, size_t whyn) {
    char ext[16];
    probe_info p;
    if (refused(path, ext, sizeof ext, &p, why, whyn)) return 1;
    *seconds = p.seconds;
    *aac = 0;                                              /* one decoder output here */
    return 0;
}

static int find_mpg123(const char *dir, char *lib, size_t n) {
    static const char *const names[] = {"libmpg123.dll", "libmpg123-0.dll", "mpg123.dll"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        struct stat st;
        snprintf(lib, n, "%s\\%s", dir, names[i]);
        if (stat(lib, &st) == 0) return 0;
    }
    return -1;
}

int load_tracks(const char *path, const char *rekordbox_app, tracks *ts, char *why, size_t whyn) {
    memset(ts, 0, sizeof *ts);
    char ext[16];
    probe_info p;
    if (refused(path, ext, sizeof ext, &p, why, whyn)) return 1;
    if (p.kind == K_MP3) {
        char lib[1024];
        float *s;
        size_t n;
        if (find_mpg123(rekordbox_app, lib, sizeof lib)) { snprintf(why, whyn, "rekordbox's libmpg123.dll isn't in %s", rekordbox_app); return 1; }
        if (rb_mp3(path, lib, &s, &n, why, whyn)) return 1;
        add_track(ts, s, n);
        snprintf(ts->how, sizeof ts->how, "rekordbox's MP3 frame map + libmpg123");
    } else if (p.kind == K_AAC) {
        int16_t *pcm = NULL;
        size_t n = 0;
        int rate = 0, ch = 0;
        if (decode_aac(path, &pcm, &n, &rate, &ch, why, whyn)) return 1;
        if (rate != 44100) { big_free(pcm); snprintf(why, whyn, "%d Hz: not supported on Windows yet", rate); return 1; }
        if (ch > 2) { big_free(pcm); snprintf(why, whyn, "AAC with %d channels: rekordbox loads it but doesn't separate it", ch); return 1; }
        ts->t[0] = (track){NULL, pcm, ch, 1, n};
        ts->count = 1;
        snprintf(ts->how, sizeof ts->how, "Media Foundation, 16-bit, length %zu", n);
    } else {
        float *s;
        size_t n;
        if (p.kind == K_FLAC ? decode_flac(path, &p, &s, &n, why, whyn) : decode_pcm(path, &p, &s, &n, why, whyn)) return 1;
        add_track(ts, s, n);
        snprintf(ts->how, sizeof ts->how, "%s %d Hz%s", p.kind == K_FLAC ? "flac" : "pcm", p.rate, p.ch > 2 ? ", first 2 channels" : "");
    }
    for (int i = 0; i < ts->count; i++)
        if (!ts->t[i].n) { tracks_free(ts); snprintf(why, whyn, "no audio"); return 1; }
    (void)scat;
    return 0;
}


/* rekordbox's chunk i of a track: start i*HOP-PRE, zero-padded; the track's final sample is
 * never sent */
void chunk_at(const track *t, size_t i, float *c) {
    long n = (long)t->n, lo = (long)i * CHUNK_HOP - CHUNK_PRE;
    memset(c, 0, sizeof(float) * 2 * CHUNK_L);
    long s0 = lo > 0 ? lo : 0, s1 = lo + CHUNK_L < n ? lo + CHUNK_L : n;
    const float k = 1.0f / 32768.0f;
    size_t tch = (size_t)t->ch;
    for (int ch = 0; ch < 2; ch++) {
        float *o = c + (size_t)ch * CHUNK_L;
        if (s1 > s0 && t->s) memcpy(o + (s0 - lo), t->s + (size_t)ch * t->n + s0, sizeof(float) * (size_t)(s1 - s0));
        else if (s1 > s0) {
            size_t c1 = ch && tch > 1 ? 1 : 0;
            for (long j = s0; j < s1; j++) o[j - lo] = t->p16[(size_t)j * tch + c1] * k;
        }
        if (lo <= n - 1 && n - 1 < lo + CHUNK_L) o[n - 1 - lo] = 0;
    }
}

size_t n_chunks(size_t n) { return (n + CHUNK_PRE + CHUNK_HOP - 1) / CHUNK_HOP; }
