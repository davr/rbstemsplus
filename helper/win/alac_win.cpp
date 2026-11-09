/* ALAC in an MP4 (M4A), decoded with Apple's open-source ALAC decoder (Apache License 2.0,
 * https://github.com/macosforge/alac, fetched at build time: helper/win/build.sh). Windows rekordbox
 * reproduces a plain decode of ALAC (measured, PREPARE.md), and ALAC is lossless, so any correct
 * decoder gives the same samples. f: the open file (the caller closes it). Output: planar stereo float32, x / 2^(bits-1). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ALACDecoder.h"
#include "ALACBitUtilities.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

/* the first box NAME in b[*from..n): its payload; *from moves past it */
static const uint8_t *box(const uint8_t *b, size_t n, const char *name, size_t *plen, size_t *from) {
    size_t i = from ? *from : 0;
    while (i + 8 <= n) {
        uint64_t len = be32(b + i);
        size_t hdr = 8;
        if (len == 1 && i + 16 <= n) { len = be64(b + i + 8); hdr = 16; }
        if (len == 0) len = n - i;
        if (len < hdr || i + len > n) return NULL;
        if (!memcmp(b + i + 4, name, 4)) { *plen = (size_t)(len - hdr); if (from) *from = i + (size_t)len; return b + i + hdr; }
        i += (size_t)len;
    }
    return NULL;
}

extern "C" int alac_decode(FILE *f, float **out, size_t *n_out, int *rate, int *ch, char *why, size_t whyn) {
    uint8_t *moov = NULL;
    uint64_t *pos = NULL;
    uint32_t *sz = NULL;
    float *s = NULL;
    int rc = 1;
    ALACDecoder dec;
    _fseeki64(f, 0, SEEK_END);
    long long size = _ftelli64(f), at = 0;
    size_t moov_n = 0;
    while (at + 8 <= size) {                                 /* top-level boxes, to moov */
        uint8_t h[16];
        _fseeki64(f, at, SEEK_SET);
        if (fread(h, 1, 16, f) < 8) break;
        uint64_t len = be32(h);
        size_t hl = 8;
        if (len == 1) { len = be64(h + 8); hl = 16; }
        if (len == 0) len = (uint64_t)(size - at);
        if (len < hl) break;
        if (!memcmp(h + 4, "moov", 4) && len - hl <= (64u << 20)) {
            moov_n = (size_t)(len - hl);
            moov = (uint8_t *)malloc(moov_n + 1);
            _fseeki64(f, at + (long long)hl, SEEK_SET);
            if (!moov || fread(moov, 1, moov_n, f) != moov_n) { free(moov); moov = NULL; }
            break;
        }
        at += (long long)len;
    }
    if (!moov) { snprintf(why, whyn, "no sample table"); goto done; }
    {
        size_t from = 0, tl;
        const uint8_t *trak, *stbl = NULL;
        size_t sl = 0;
        while (!stbl && (trak = box(moov, moov_n, "trak", &tl, &from))) {
            size_t ml, hl2, il;
            const uint8_t *mdia = box(trak, tl, "mdia", &ml, NULL);
            const uint8_t *hdlr = mdia ? box(mdia, ml, "hdlr", &hl2, NULL) : NULL;
            if (!hdlr || hl2 < 12 || memcmp(hdlr + 8, "soun", 4)) continue;
            const uint8_t *minf = box(mdia, ml, "minf", &il, NULL);
            if (minf) stbl = box(minf, il, "stbl", &sl, NULL);
        }
        size_t dl, zl, cl, ol;
        const uint8_t *stsd = stbl ? box(stbl, sl, "stsd", &dl, NULL) : NULL;
        const uint8_t *stsz = stbl ? box(stbl, sl, "stsz", &zl, NULL) : NULL;
        const uint8_t *stsc = stbl ? box(stbl, sl, "stsc", &cl, NULL) : NULL;
        const uint8_t *stco = stbl ? box(stbl, sl, "stco", &ol, NULL) : NULL;
        int co64 = 0;
        if (stbl && !stco) { stco = box(stbl, sl, "co64", &ol, NULL); co64 = 1; }
        if (!stsd || !stsz || !stsc || !stco || dl < 8 + 36 + 12 || zl < 12 || cl < 8 || ol < 8) { snprintf(why, whyn, "no sample table"); goto done; }
        /* the 'alac' atom: after the sample entry's 36 bytes (its header and AudioSampleEntry fields) */
        const uint8_t *entry = stsd + 8;
        size_t el = be32(entry), cl2 = 0, cf = 36;
        const uint8_t *cookie = el > 36 ? box(entry, el, "alac", &cl2, &cf) : NULL;
        (void)cookie;
        /* box() returned the payload: the cookie for the decoder is the atom itself (size, 'alac', ...) */
        cookie = cookie ? cookie - 8 : NULL;
        if (!cookie || cl2 + 8 < 36) { snprintf(why, whyn, "no ALAC configuration"); goto done; }
        if (dec.Init((void *)cookie, (uint32_t)(cl2 + 8)) != ALAC_noErr) { snprintf(why, whyn, "not an ALAC stream this decoder reads"); goto done; }
        const uint8_t *cfg = cookie + 12;                    /* ALACSpecificConfig */
        uint32_t frameLength = be32(cfg);
        int bits = cfg[5], chans = cfg[9];
        uint32_t sr = be32(cfg + 20);
        if (chans < 1 || chans > 2 || (bits != 16 && bits != 24 && bits != 32)) { snprintf(why, whyn, "ALAC with %d channels, %d bits isn't supported on Windows yet", chans, bits); goto done; }
        uint32_t count = be32(stsz + 8), fixed = be32(stsz + 4);
        if (count == 0 || (fixed == 0 && zl < 12 + (size_t)count * 4)) { snprintf(why, whyn, "no sample table"); goto done; }
        pos = (uint64_t *)malloc(sizeof(uint64_t) * count);
        sz = (uint32_t *)malloc(sizeof(uint32_t) * count);
        if (!pos || !sz) { snprintf(why, whyn, "out of memory"); goto done; }
        for (uint32_t i = 0; i < count; i++) sz[i] = fixed ? fixed : be32(stsz + 12 + (size_t)i * 4);
        uint32_t nchunks = be32(stco + 4), nruns = be32(stsc + 4);
        if (ol < 8 + (size_t)nchunks * (co64 ? 8 : 4) || cl < 8 + (size_t)nruns * 12) { snprintf(why, whyn, "no sample table"); goto done; }
        uint32_t k = 0;
        for (uint32_t c = 1; c <= nchunks && k < count; c++) {
            uint32_t run = 0;                                /* samples in chunk c: the last stsc run starting at or before c */
            for (uint32_t r = 0; r < nruns; r++) if (be32(stsc + 8 + (size_t)r * 12) <= c) run = be32(stsc + 8 + (size_t)r * 12 + 4);
            uint64_t off = co64 ? be64(stco + 8 + (size_t)(c - 1) * 8) : be32(stco + 8 + (size_t)(c - 1) * 4);
            for (uint32_t j = 0; j < run && k < count; j++) { pos[k] = off; off += sz[k]; k++; }
        }
        if (k != count) { snprintf(why, whyn, "the sample table doesn't add up"); goto done; }
        size_t cap = 0, n = 0, bytes = (size_t)bits / 8;
        uint8_t *pk = (uint8_t *)malloc(65536), *pcm = (uint8_t *)malloc((size_t)frameLength * (size_t)chans * bytes + 16);
        size_t pkcap = 65536;
        if (!pk || !pcm) { free(pk); free(pcm); snprintf(why, whyn, "out of memory"); goto done; }
        for (uint32_t i = 0; i < count; i++) {
            if (sz[i] > pkcap) { uint8_t *t = (uint8_t *)realloc(pk, sz[i]); if (!t) { free(pk); free(pcm); snprintf(why, whyn, "out of memory"); goto done; } pk = t; pkcap = sz[i]; }
            _fseeki64(f, (long long)pos[i], SEEK_SET);
            if (fread(pk, 1, sz[i], f) != sz[i]) { free(pk); free(pcm); snprintf(why, whyn, "decoding stopped early"); goto done; }
            BitBuffer bb;
            BitBufferInit(&bb, pk, sz[i]);
            uint32_t got = 0;
            if (dec.Decode(&bb, pcm, frameLength, (uint32_t)chans, &got) != ALAC_noErr || got > frameLength) { free(pk); free(pcm); snprintf(why, whyn, "decoding stopped with an error"); goto done; }
            if (n + got > cap) {
                size_t nc = cap ? cap * 2 : (size_t)sr * 30;
                while (nc < n + got) nc *= 2;
                float *t = (float *)realloc(s, sizeof(float) * 2 * nc);   /* grown planar: moved at the end */
                if (!t) { free(pk); free(pcm); snprintf(why, whyn, "out of memory"); goto done; }
                s = t; cap = nc;
            }
            /* interleaved [L R] frames kept in s's first 2*n floats for now */
            for (uint32_t j = 0; j < got; j++)
                for (int c = 0; c < 2; c++) {
                    const uint8_t *q = pcm + ((size_t)j * (size_t)chans + (size_t)(chans > 1 ? c : 0)) * bytes;
                    float v;
                    if (bits == 16) v = (float)(int16_t)(q[0] | q[1] << 8) / 32768.0f;
                    else if (bits == 24) v = (float)((int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) >> 8) / 8388608.0f;
                    else v = (float)(int32_t)(q[0] | q[1] << 8 | q[2] << 16 | (uint32_t)q[3] << 24) / 2147483648.0f;
                    s[2 * (n + j) + (size_t)c] = v;
                }
            n += got;
        }
        free(pk);
        free(pcm);
        if (!n) { snprintf(why, whyn, "no audio"); goto done; }
        float *p = (float *)malloc(sizeof(float) * 2 * n);       /* planar */
        if (!p) { snprintf(why, whyn, "out of memory"); goto done; }
        for (size_t i = 0; i < n; i++) { p[i] = s[2 * i]; p[n + i] = s[2 * i + 1]; }
        *out = p;
        *n_out = n;
        *rate = (int)sr;
        *ch = chans;
        rc = 0;
    }
done:
    free(s);
    free(pos);
    free(sz);
    free(moov);
    return rc;
}
