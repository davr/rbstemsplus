/* The MP4 sound track's sample table (stts) and timescale (mdhd): the length rekordbox gives an
 * AAC track. The sound track is the first trak whose hdlr is 'soun' (a music video's first
 * track is usually video). Boxes are read with pread: a video can be large. */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rbsp.h"

static uint32_t be32(const unsigned char *b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static uint64_t be64(const unsigned char *b) { return (uint64_t)be32(b) << 32 | be32(b + 4); }

/* the next box in [*off, end): its type and body range; 0 when there is one */
static int next_box(int fd, uint64_t *off, uint64_t end, char type[4], uint64_t *body, uint64_t *bend) {
    unsigned char h[16];
    if (*off + 8 > end || pread(fd, h, 8, (off_t)*off) != 8) return 1;
    uint64_t size = be32(h), hdr = 8;
    if (size == 1) {
        if (*off + 16 > end || pread(fd, h + 8, 8, (off_t)(*off + 8)) != 8) return 1;
        size = be64(h + 8);
        hdr = 16;
    } else if (size == 0) {
        size = end - *off;
    }
    if (size < hdr || *off + size > end) return 1;
    memcpy(type, h + 4, 4);
    *body = *off + hdr;
    *bend = *off + size;
    *off += size;
    return 0;
}

/* the first child of [off, end) with this type */
static int find(int fd, uint64_t off, uint64_t end, const char *type, uint64_t *body, uint64_t *bend) {
    char t[4];
    while (!next_box(fd, &off, end, t, body, bend))
        if (!memcmp(t, type, 4)) return 0;
    return 1;
}

static int find_path(int fd, uint64_t off, uint64_t end, const char *const *path, int n, uint64_t *body, uint64_t *bend) {
    for (int i = 0; i < n; i++) {
        if (find(fd, off, end, path[i], body, bend)) return 1;
        off = *body;
        end = *bend;
    }
    return 0;
}

int mp4_audio_track(const char *path, mp4_audio *out) {
    memset(out, 0, sizeof *out);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 1;
    struct stat st;
    int rc = 1;
    uint64_t mb, me;
    if (fstat(fd, &st) || find(fd, 0, (uint64_t)st.st_size, "moov", &mb, &me)) goto done;
    char t[4];
    uint64_t off = mb, tb, te;
    while (!next_box(fd, &off, me, t, &tb, &te)) {
        if (memcmp(t, "trak", 4)) continue;
        uint64_t b, e;
        static const char *const hdlr[] = {"mdia", "hdlr"};
        unsigned char h[12];
        if (find_path(fd, tb, te, hdlr, 2, &b, &e) || e - b < 12 || pread(fd, h, 12, (off_t)b) != 12 || memcmp(h + 8, "soun", 4))
            continue;
        static const char *const mdhd[] = {"mdia", "mdhd"};
        unsigned char m[24];
        if (!find_path(fd, tb, te, mdhd, 2, &b, &e) && e - b >= 24 && pread(fd, m, 24, (off_t)b) == 24)
            out->timescale = m[0] == 1 ? be32(m + 20) : be32(m + 12);
        static const char *const stts[] = {"mdia", "minf", "stbl", "stts"};
        if (!find_path(fd, tb, te, stts, 4, &b, &e) && e - b >= 8) {
            size_t len = (size_t)(e - b);
            unsigned char *s = len <= (64u << 20) ? malloc(len) : NULL;
            if (s && pread(fd, s, len, (off_t)b) == (ssize_t)len) {
                uint32_t n = be32(s + 4);
                if (n && 8 + (uint64_t)n * 8 <= len) {
                    uint64_t total = 0;
                    for (uint32_t i = 0; i < n; i++) total += (uint64_t)be32(s + 8 + 8 * i) * be32(s + 12 + 8 * i);
                    out->stts_total = total;
                    out->last_delta = be32(s + 12 + 8 * (n - 1));
                    out->has_stts = 1;
                }
            }
            free(s);
        }
        rc = 0;
        break;
    }
done:
    close(fd);
    return rc;
}
