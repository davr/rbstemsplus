/* rbsp-prepare: rekordbox's stems for music files, written into the Stems Cache in advance, as
 * the bridge would write them when rekordbox separates the files itself; rekordbox then loads
 * them as hits.
 *
 *   rbsp-prepare --model MODEL.onnx --cache DIR [options] [--playlist ID] [FILE...]
 *   rbsp-prepare --estimate --model MODEL.onnx --cache DIR [options] [--playlist ID] [FILE...]
 *   rbsp-prepare --playlists [--library DB]
 *     --model     the model rekordbox runs: our Demucs v4, or rekordbox's own (STEMS Engine 0002,
 *                 whose stems are rebuilt as the bridge rebuilds them); any other is refused
 *     --cache     the cache's folder (the one holding the model folders)
 *     --max-gb N  the cache's max_gb (default 20, x 2^30 bytes as the bridge). At 90% of it, the
 *                 level the bridge evicts down to, the helper removes the least recently used
 *                 entries no set keeps (this run's never) down to 80%, and stops if that isn't
 *                 enough. Kept sets take at most 75% of it together: past that it stops too
 *     --chosen    the cache is a chosen location on another drive (the bridge's add-up bar there)
 *     --threads N the model's threads; default rekordbox's: half the active cores, at least 1
 *                 (AudioPartSeparator::initialize: getNumCpus() / 2 when its "parallel analyze"
 *                 flag is set, else 1; measured in the VM: 4 of 8 with the STEMS multithreading
 *                 setting on and off). With the same count, our model's stems are rekordbox's bit
 *                 for bit
 *     --list F    the files to prepare, one path per line, after any on the command line
 *     --playlist ID   a rekordbox playlist's tracks, after those (its ID from --playlists); a
 *                 folder or an intelligent playlist is refused
 *     --first N   prepare only the first N of those files (Prepare the First N Tracks): the set
 *                 keeps what it holds of the others, and drops the rest of what it held, as a
 *                 whole run does (tracks that left the playlist, another model's entries)
 *     --playlists the library's playlists and folders, one JSON object per line (library.h), then
 *                 exit; no model or cache needed
 *     --tracks ID the tracks of playlist ID as rekordbox's library lists them now, one
 *                 {"file": path} per line, then exit: the files aren't looked at (no privacy
 *                 prompt), and no model or cache is needed
 *     --library DB    rekordbox's library (default ~/Library/Pioneer/rekordbox/master.db), read only
 *     --rekordbox APP   rekordbox.app (its own MP3 decoder; default /Applications/rekordbox 7/rekordbox.app)
 *     --ort LIB   Pioneer's ONNX Runtime (default the copy the app keeps)
 *     --keep NAME     keep this run's entries as set NAME (kept.h); with --playlist the set is
 *                 the playlist's ID unless --no-keep. Kept until --until SECONDS (since 1970), or
 *                 --keep-days N from now (default 10). Refused when 5 other sets are kept
 *     --kept-dir DIR  the kept sets' folder (default ~/Library/Application Support/rbstemsplus/kept)
 *     --wait-read S   how long a track's first read may wait (default 60 s; 2 for --estimate): in
 *                 Documents, Desktop and Downloads macOS asks the user first, and the read waits
 *                 for the answer (readable.h). Given up, the track is skipped ("access": "asking")
 *                 and so is the rest of that folder
 *     --quick     an --estimate that reads nothing that could bring up macOS's privacy prompt
 *                 (the watcher's check when rekordbox opens): a track in Documents, Desktop,
 *                 Downloads or on another drive is "unchecked" (to prepare when an entry is gone
 *                 from a cache on this Mac); elsewhere its size and time only
 *                 (no header: a track to prepare has no length); a cache on another drive isn't
 *                 looked at at all (a track unchanged since it was prepared is ready; no sizes)
 *     --estimate  what preparing would take, without running the model or writing anything but
 *                 the kept sets' folder and a cleared cache folder on this Mac (made; sets past
 *                 their date deleted): each file is ready
 *                 (its manifest record matches the file and every entry is in the cache), to
 *                 prepare (its length from the file's header, about 3 MB an entry: 16 MB a minute,
 *                 AAC twice that) or skipped. With --keep NAME and no files, the set's own files:
 *                 what the app checks when a set's playlist can't be read (without --playlist:
 *                 a playlist that was read and has no tracks stays empty)
 *
 * Output: one JSON object per line: per file {"file", "result": "prepared", "written",
 * "already", "silent", "refused", "how"} or {"file", "result": "skipped", "why", "access":
 * "asking"|"denied" and "folder" when macOS's privacy setting is why}; before each chunk {"at":
 * the file's index, "files", "chunk", "chunks"}; {"room":
 * entries removed, "cache_gb"} when room was made; at the end {"done": files, "stopped": why
 * (when it stopped early), "runs" and "run_s" (the model's runs and their seconds), "kept",
 * "until", "kept_gb" (every kept set's, with a set), "kept_error", "manifest_error"}. A file
 * that isn't there is skipped with its reason; a playlist's streaming tracks aren't looked at.
 * "result": "stopped" when the run stopped inside a file. With a set, its manifest (manifest.h) is
 * written with it. SIGTERM or SIGINT stops the run after
 * its current chunk ("stopped": "stopped"), the set written. The run is in the utility QoS class.
 * --estimate: per file {"file", "result": "ready", "entries", "gb"}, {"file", "result":
 * "prepare", "seconds", "entries", "gb", "missing" (a prepared file's entries no longer in the
 * cache)} or {"file", "result": "skipped", "why"}; then {"estimate": files, "ready", "prepare",
 * "skipped", "entries", "runs", "time_s", "need_gb" (new entries), "kept_gb" (every set's with
 * this one's ready files), "share_gb", "max_gb", "cache_gb", "free_gb", "fits", "fit_files" (how
 * many of the first files fit the share), "max_gb_needed" (the smallest limit whose share holds
 * them all), "disk_ok" (the disk holds the new entries above its floor), "asking" (files whose
 * read waits on macOS's prompt), "folders" (the protected folders the files are in, from their
 * paths: macOS asks once for each), "unchecked" (--quick)}, or {"refused": why}
 * when no set can be kept. Exit 0 unless the setup failed. */
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <pthread/qos.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
#include "entry.h"
#include "kept.h"
#include "library.h"
#include "manifest.h"
#include "model.h"
#include "rbsp.h"
#include "readable.h"

/* the helper's stem maths and decoding rules: every entry names it (RBSTEMS_HELPER); an entry
 * from another version isn't "already there", it is rewritten (requirement 1) */
#define HELPER_VERSION "1"
#define FREE_FLOOR_MAX 50000000000ULL
#define ENTRY_EST 3000000LL             /* an entry on disk, for estimates: 2.5 MB measured (Demucs v4), 2.9 (rekordbox's) */
#define RUN_S_EST 2.5                   /* seconds a model run, until a set's manifest has this Mac's (the VM: 4 threads) */

static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(f, "\\%c", *p);
        else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}

/* bytes the cache takes on disk, as the bridge counts (blocks), the whole folder */
static long long disk_used(const char *root) {
    char *const paths[] = {(char *)root, NULL};
    FTS *t = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    long long sum = 0;
    FTSENT *e;
    while (t && (e = fts_read(t)))
        if (e->fts_info == FTS_F && e->fts_statp) sum += (long long)e->fts_statp->st_blocks * 512;
    if (t) fts_close(t);
    return sum;
}

static long long blocks(int dir, const char *name) {
    struct stat st;
    return fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) == 0 ? (long long)st.st_blocks * 512 : 0;
}

/* free bytes as the bridge counts them (Finder's "important usage" count, else statfs) */
static double free_bytes(const char *dir) {
    double b = 0;
    CFURLRef u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)dir, (CFIndex)strlen(dir), true);
    CFTypeRef v = NULL;
    if (u && CFURLCopyResourcePropertyForKey(u, kCFURLVolumeAvailableCapacityForImportantUsageKey, &v, NULL) && v
        && CFGetTypeID(v) == CFNumberGetTypeID()) {
        int64_t n = 0;
        if (CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &n) && n > 0) b = (double)n;
    }
    if (v) CFRelease(v);
    if (u) CFRelease(u);
    struct statfs fs;
    if (b <= 0 && statfs(dir, &fs) == 0) b = (double)fs.f_bavail * fs.f_bsize;
    return b;
}

/* the free space the bridge leaves on the cache's disk: 10% of it, at most 50 GB */
static uint64_t free_floor(const char *cache) {
    struct statfs fs;
    uint64_t disk = statfs(cache, &fs) == 0 ? (uint64_t)fs.f_blocks * fs.f_bsize : 0;
    return disk > 0 && disk / 10 < FREE_FLOOR_MAX ? disk / 10 : FREE_FLOOR_MAX;
}

/* 90%: the bridge evicts down to 90% of max_gb once over it; filling to 100% would make
 * rekordbox's next saved stems drop 10% of the cache at once (measured). The next entry must fit
 * under it too. */
static int over_limit(long long used, long long next, long long max_bytes) { return (double)(used + next) > 0.9 * (double)max_bytes; }

/* NULL if another entry (about `next` bytes) may be written; else why not */
static const char *room(const char *cache, long long used, long long next, long long max_bytes, char *buf, size_t n) {
    if (over_limit(used, next, max_bytes)) {
        snprintf(buf, n, "the cache holds %.2f GB of stems that are kept or being prepared: another entry would take it past 90%% of its %.4g GB limit",
                 used / 1073741824.0, max_bytes / 1073741824.0);
        return buf;
    }
    uint64_t floor_ = free_floor(cache);
    double free_ = free_bytes(cache);
    if (free_ < (double)floor_) {
        snprintf(buf, n, "%.1f GB free on the cache's disk, under its floor of %.0f GB", free_ / 1e9, floor_ / 1e9);
        return buf;
    }
    return NULL;
}

/* the value of a vorbis comment NAME= in a FLAC's first bytes (its length, 32-bit little-endian,
 * comes just before it): 1 and *val, or 0 */
static int comment(const char *head, ssize_t n, const char *name, char *val, size_t vn) {
    size_t nl = strlen(name);
    for (ssize_t i = 4; i + (ssize_t)nl <= n; i++) {
        if (memcmp(head + i, name, nl)) continue;
        const unsigned char *l = (const unsigned char *)head + i - 4;
        uint32_t len = l[0] | l[1] << 8 | l[2] << 16 | (uint32_t)l[3] << 24;
        if (len < nl || len - nl >= vn || i + (ssize_t)len > n) continue;
        memcpy(val, head + i + nl, len - nl);
        val[len - nl] = 0;
        return 1;
    }
    return 0;
}

/* an entry that stays: one the bridge wrote itself (rekordbox's own stems: no helper tag), or one
 * this helper version wrote; an older helper version's is rewritten (requirement 1) */
static int keep(int x, const char *key) {
    char name[80], v[80];
    snprintf(name, sizeof name, "%s.flac", key);
    int fd = openat(x, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return 0;
    char head[8192];
    ssize_t n = read(fd, head, sizeof head);
    close(fd);
    if (n <= 0 || !comment(head, n, "RBSTEMS_KEY=", v, sizeof v) || strcmp(v, key)) return 0;
    if (!comment(head, n, "RBSTEMS_HELPER=", v, sizeof v)) return 1;
    return !strcmp(v, HELPER_VERSION);
}

/* the prefix folder <cache>/<model>/<xx>, made if `create`; an fd or -1 */
static int prefix_dir(const char *cache, const char *model, const char *key, int create) {
    char p[4096];
    if (create) {
        snprintf(p, sizeof p, "%s/%s", cache, model);
        mkdir(p, 0755);
    }
    snprintf(p, sizeof p, "%s/%s/%.2s", cache, model, key);
    if (create) mkdir(p, 0755);
    return open(p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

typedef struct { size_t n; int16_t *pcm; const char *why; } shared;   /* chunk i's stems, per track length */

/* a skipped file's line; ACCESS "asking" or "denied" (macOS's privacy prompt, FOLDER the
 * protected folder) or NULL */
static void skipped(const char *path, const char *why, const char *access, const char *folder) {
    printf("{\"file\":"); json_str(stdout, path); printf(",\"result\":\"skipped\",\"why\":"); json_str(stdout, why);
    if (access) { printf(",\"access\":\"%s\",\"folder\":", access); json_str(stdout, folder); }
    printf("}\n");
    fflush(stdout);
}

/* whether PATH can be opened, within SECS seconds (readable.h): 0 and *st, or 1 with its
 * skipped line printed (*state READ_ASKING for a prompt still open) */
static int opening(const char *path, double secs, struct stat *st, read_state *state) {
    char why[400];
    const char *folder = "";
    *state = READ_FAILED;
    if (path[0] != '/') { skipped(path, "not a file on this Mac", NULL, NULL); return 1; }
    *state = readable(path, secs, st, why, sizeof why, &folder);
    if (*state == READ_OK) return 0;
    skipped(path, why, *state == READ_ASKING ? "asking" : *state == READ_DENIED ? "denied" : NULL, folder);
    return 1;
}

static int cache_gone(const char *cache) {
    struct stat st;
    return stat(cache, &st) || !S_ISDIR(st.st_mode);
}

static const char *share_stop(char *buf, size_t n, double max_bytes) {
    snprintf(buf, n, "prepared stems would take more than %.0f%% of the cache's %.4g GB limit",
             KEPT_SHARE * 100, max_bytes / 1073741824.0);
    return buf;
}

/* a track this run skipped (its file can't be opened, decoded or run through the model) that the
 * set's earlier run prepared to its end: its record and its entries still in the cache stay in the
 * set, so a drive unplugged today doesn't unprepare it. ST: the file as it is now (its record must
 * match it), or NULL when it can't be seen. MF: this run's record for it, if one was started.
 * -1 when the cache folder itself can't be seen (its drive unplugged during the run): nothing is
 * known about its entries, nothing was changed, and the run must stop so its records stay. */
static int carry(manifest *mine, mfile *mf, const manifest *earlier, kept *k, const char *cache, const char *id,
                 const char *path, const struct stat *st) {
    if (cache_gone(cache)) return -1;
    const mfile *r = st ? manifest_find(earlier, path, st) : manifest_by_path(earlier, path);
    if (!r || !r->complete) return 0;
    if (!mf) {
        struct stat was = {0};
        was.st_size = (off_t)r->size;
        was.st_mtimespec.tv_sec = (time_t)(r->mtime_ns / 1000000000LL);
        was.st_mtimespec.tv_nsec = (long)(r->mtime_ns % 1000000000LL);
        if (!(mf = manifest_file(mine, path, &was))) return 0;
    }
    mf->nkeys = 0;
    mf->complete = 1;
    for (size_t j = 0; j < r->nkeys; j++) {
        long long b = kept_entry_bytes(cache, id, r->keys[j]);
        if (b < 0) { mf->complete = 0; continue; }
        kept_add(k, id, r->keys[j], b);
        mfile_key(mf, r->keys[j]);
    }
    return 0;
}

/* SIGTERM or SIGINT (the app's Stop): the run stops after the chunk it is on, its set written */
static const char CACHE_GONE[] = "the cache folder can't be read";
static volatile sig_atomic_t interrupted;
static void on_signal(int sig) { (void)sig; interrupted = 1; }

/* the kept sets' manifests for model ID and this helper version: what the estimate knows is
 * prepared. *rate: seconds a model run, from the newest manifest whose run used THREADS threads */
static manifest *manifests(const char *kdir, const char *id, int threads, size_t *count, double *rate) {
    manifest *v = NULL;
    *count = 0;
    DIR *dp = kdir[0] ? opendir(kdir) : NULL;
    struct dirent *e;
    time_t newest = 0;
    while (dp && (e = readdir(dp))) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l <= 9 || strcmp(e->d_name + l - 9, ".manifest")) continue;
        char name[NAME_MAX + 1], p[PATH_MAX + NAME_MAX + 2];
        snprintf(name, sizeof name, "%.*s", (int)(l - 9), e->d_name);
        snprintf(p, sizeof p, "%s/%s", kdir, e->d_name);
        manifest m;
        struct stat st;
        if (manifest_read(kdir, name, &m)) continue;
        if (strcmp(m.model, id) || strcmp(m.helper, HELPER_VERSION)) { manifest_free(&m); continue; }
        if (m.runs > 0 && m.threads == threads && stat(p, &st) == 0 && st.st_mtime >= newest) {
            newest = st.st_mtime;
            *rate = m.run_s / (double)m.runs;
        }
        manifest *t = realloc(v, (*count + 1) * sizeof *t);
        if (!t) { manifest_free(&m); break; }
        v = t;
        v[(*count)++] = m;
    }
    if (dp) closedir(dp);
    return v;
}

/* iCloud Drive's and other services' folders: macOS may ask before they are read */
static int cloud_folder(const char *path) {
    const char *h = home_folder();
    size_t l = strlen(h);
    return l && !strncmp(path, h, l) && (!strncmp(path + l, "/Library/Mobile Documents/", 26) || !strncmp(path + l, "/Library/CloudStorage/", 22));
}

/* whether even a look at PATH's size may bring up macOS's privacy prompt */
static int may_ask(const char *path) {
    return protected_folder(path)[0] || !strncmp(path, "/Volumes/", 9) || cloud_folder(path);
}

/* a track that can't be looked at now stays in the set with what it holds (carry, in a run) */
static void est_hold(const manifest *ms, size_t nm, kept *k, const char *cache, const char *id, const char *path) {
    for (size_t i = 0; i < nm; i++) {
        const mfile *r = manifest_by_path(&ms[i], path);
        if (!r || !r->complete) continue;
        for (size_t j = 0; j < r->nkeys; j++) {
            long long b = kept_entry_bytes(cache, id, r->keys[j]);
            if (b >= 0) kept_add(k, id, r->keys[j], b);
        }
        return;
    }
}

/* --estimate: what preparing FILES would take (the header comment) */
static void estimate(char **files, size_t nfiles, const char *cache, const char *id, kept *k, const char *keep_name,
                     const char *kdir, int threads, long long max_bytes, double wait, int quick) {
    size_t nm = 0, ready = 0, prep = 0, skip = 0, fit = nfiles, asking = 0, unchecked = 0;
    int cache_off = !strncmp(cache, "/Volumes/", 9);   /* --quick doesn't read another drive */
    int folders = 0;                    /* the protected folders the files are in (bit i: PROTECTED's i) */
    static const char *const PROT[] = {"Documents", "Desktop", "Downloads"};
    double rate = RUN_S_EST;
    manifest *ms = manifests(kdir, id, threads, &nm, &rate);
    long long share = (long long)(KEPT_SHARE * (double)max_bytes), need = 0, entries = 0, runs = 0;
    int keeping = keep_name != NULL;
    long long *adds = malloc(sizeof *adds * (nfiles + 1));   /* what each track to prepare needs; -1: none */
    /* a changed track's record in this set: past N, --first keeps what it holds */
    const mfile **changed = calloc(nfiles + 1, sizeof *changed);
    manifest own = {0};
    int own_ok = keeping && manifest_read(kdir, keep_name, &own) == 0 && !strcmp(own.model, id);
    char why[400];
    for (size_t fi = 0; fi < nfiles; fi++) {
        adds[fi] = -1;
        const char *path = files[fi];
        struct stat st;
        double sec = 0;
        int aac = 0;
        read_state rs;
        for (int i = 0; i < 3; i++) if (!strcmp(protected_folder(path), PROT[i])) folders |= 1 << i;
        if (quick && may_ask(path)) {
            /* a read here may bring up macOS's privacy prompt, naming whoever runs this: only what
             * the manifests say (a track never prepared to its end isn't ready) */
            const mfile *done = NULL;
            for (size_t i = 0; i < nm && !done; i++) {
                const mfile *r = manifest_by_path(&ms[i], path);
                if (r && r->complete) done = r;
            }
            long long gone = 0;     /* the entries in this Mac's cache can be looked at: no prompt there */
            for (size_t j = 0; done && !cache_off && j < done->nkeys; j++)
                if (kept_entry_bytes(cache, id, done->keys[j]) < 0) gone++;
            printf("{\"file\":"); json_str(stdout, path);
            if (done) {
                if (gone) {
                    printf(",\"result\":\"prepare\",\"seconds\":0.0,\"entries\":%lld,\"gb\":%.3f,\"missing\":%lld}\n", gone, gone * ENTRY_EST / 1073741824.0, gone);
                    need += gone * ENTRY_EST;
                    entries += gone;
                    runs += gone;
                    prep++;
                    adds[fi] = gone * ENTRY_EST;
                } else {
                    printf(",\"result\":\"unchecked\"}\n");
                    unchecked++;
                }
                if (!cache_off) est_hold(ms, nm, k, cache, id, path);
            }
            else { printf(",\"result\":\"prepare\",\"seconds\":0.0,\"entries\":0,\"gb\":0.000}\n"); prep++; adds[fi] = 0; }
            continue;
        }
        if (quick) {                    /* its size and time only: no read */
            if (path[0] != '/') { skipped(path, "not a file on this Mac", NULL, NULL); skip++; continue; }
            if (stat(path, &st) || !S_ISREG(st.st_mode)) {
                skipped(path, "the file isn't there (moved, renamed, or its drive isn't connected)", NULL, NULL);
                skip++;
                if (!cache_off) est_hold(ms, nm, k, cache, id, path);
                continue;
            }
        } else {
            if (opening(path, wait, &st, &rs)) { skip++; asking += rs == READ_ASKING; est_hold(ms, nm, k, cache, id, path); continue; }
            if (probe_track(path, &sec, &aac, why, sizeof why)) { skipped(path, why, NULL, NULL); skip++; est_hold(ms, nm, k, cache, id, path); continue; }
        }
        const mfile *rec = NULL;
        for (size_t i = 0; i < nm && !rec; i++) {
            const mfile *r = manifest_find(&ms[i], path, &st);
            if (r && r->complete) rec = r;
        }
        long long have = 0, missing = 0, fe, fr;
        for (size_t j = 0; rec && j < rec->nkeys; j++) {     /* what is there stays: it is kept again */
            long long b = quick && cache_off ? 0 : kept_entry_bytes(cache, id, rec->keys[j]);
            if (b < 0) missing++;
            else { have += b; kept_add(k, id, rec->keys[j], b); }
        }
        printf("{\"file\":"); json_str(stdout, path);
        if (rec && !missing) {
            ready++;
            printf(",\"result\":\"ready\",\"entries\":%zu,\"gb\":%.3f}\n", rec->nkeys, have / 1073741824.0);
        } else {
            size_t ch = sec > 0 ? n_chunks((size_t)(sec * 44100)) : 0;
            fe = rec ? missing : (long long)ch * (aac ? 2 : 1);   /* AAC: two decoder outputs, one run */
            fr = rec ? (aac ? (missing + 1) / 2 : missing) : (long long)ch;
            need += fe * ENTRY_EST;
            adds[fi] = fe * ENTRY_EST;
            if (!rec && own_ok) {
                const mfile *r = manifest_by_path(&own, path);
                changed[fi] = r && r->complete ? r : NULL;
            }
            entries += fe;
            runs += fr;
            prep++;
            printf(",\"result\":\"prepare\",\"seconds\":%.1f,\"entries\":%lld,\"gb\":%.3f", sec, fe, fe * ENTRY_EST / 1073741824.0);
            if (rec) printf(",\"missing\":%lld", missing);
            printf("}\n");
        }
        fflush(stdout);
    }
    long long kb = kept_bytes(k), so_far = 0, later = 0;
    long long *holds = calloc(nfiles + 1, sizeof *holds);
    for (size_t fi = 0; fi < nfiles; fi++)
        for (size_t j = 0; changed[fi] && j < changed[fi]->nkeys; j++) {
            long long b = kept_adds(k, id, changed[fi]->keys[j]);
            holds[fi] += b;
            later += b;
        }
    /* how many of the first tracks fit (--first), with what stays: what the ready tracks hold
     * wherever they are, and what the changed tracks past N hold */
    for (size_t fi = 0; keeping && fi < nfiles; fi++) {
        later -= holds[fi];
        if (adds[fi] < 0) continue;
        so_far += adds[fi];
        if (kb + so_far + later > share) { fit = fi; break; }
    }
    free(adds);
    free(holds);
    free(changed);
    manifest_free(&own);
    int blind = quick && cache_off;     /* nothing on the cache's drive is looked at */
    double free_ = blind ? 0 : free_bytes(cache), floor_ = blind ? 0 : (double)free_floor(cache);
    printf("{\"estimate\":%zu,\"ready\":%zu,\"prepare\":%zu,\"skipped\":%zu,\"entries\":%lld,\"runs\":%lld,\"time_s\":%.0f,"
           "\"need_gb\":%.3f,\"kept_gb\":%.3f,\"share_gb\":%.3f,\"max_gb\":%.4g,\"cache_gb\":%.3f,\"free_gb\":%.3f,"
           "\"fits\":%s,\"fit_files\":%zu,\"max_gb_needed\":%.3f,\"disk_ok\":%s,\"asking\":%zu,\"unchecked\":%zu,\"folders\":[",
           nfiles, ready, prep, skip, entries, runs, runs * rate, need / 1073741824.0, kb / 1073741824.0,
           share / 1073741824.0, max_bytes / 1073741824.0, blind ? 0 : disk_used(cache) / 1073741824.0, free_ / 1e9,
           !keeping || prep == 0 || kb + need <= share ? "true" : "false", fit, (kb + need) / KEPT_SHARE / 1073741824.0,
           free_ - floor_ >= (double)need ? "true" : "false", asking, unchecked);
    for (int i = 0, n = 0; i < 3; i++) if (folders & 1 << i) printf("%s\"%s\"", n++ ? "," : "", PROT[i]);
    printf("]}\n");
    for (size_t i = 0; i < nm; i++) manifest_free(&ms[i]);
    free(ms);
}

int main(int argc, char **argv) {
    long first_n = -1;
    const char *model_path = NULL, *cache = NULL, *list = NULL, *playlist = NULL, *libpath = NULL, *keep_name = NULL, *kept_dir = NULL;
    const char *track_list = NULL;
    int playlists = 0, no_keep = 0, estimating = 0, quick = 0;
    double keep_days = 10, wait = -1;
    long long until = 0;
    const char *rb = "/Applications/rekordbox 7/rekordbox.app";
    const char *ort = "/Library/Application Support/rbstemsplus/ort/libonnxruntime.1.18.0.dylib";
    double max_gb = 20;
    int chosen = 0, first = argc, ncpu = 0;
    size_t sz = sizeof ncpu;
    if (sysctlbyname("hw.activecpu", &ncpu, &sz, NULL, 0) || ncpu < 1) ncpu = 2;
    int threads = ncpu / 2 > 1 ? ncpu / 2 : 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) model_path = argv[++i];
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc) cache = argv[++i];
        else if (!strcmp(argv[i], "--max-gb") && i + 1 < argc) max_gb = atof(argv[++i]);
        else if (!strcmp(argv[i], "--chosen")) chosen = 1;
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--list") && i + 1 < argc) list = argv[++i];
        else if (!strcmp(argv[i], "--playlist") && i + 1 < argc) playlist = argv[++i];
        else if (!strcmp(argv[i], "--first") && i + 1 < argc) first_n = atol(argv[++i]);
        else if (!strcmp(argv[i], "--playlists")) playlists = 1;
        else if (!strcmp(argv[i], "--tracks") && i + 1 < argc) track_list = argv[++i];
        else if (!strcmp(argv[i], "--estimate")) estimating = 1;
        else if (!strcmp(argv[i], "--quick")) estimating = quick = 1;
        else if (!strcmp(argv[i], "--wait-read") && i + 1 < argc) wait = atof(argv[++i]);
        else if (!strcmp(argv[i], "--library") && i + 1 < argc) libpath = argv[++i];
        else if (!strcmp(argv[i], "--keep") && i + 1 < argc) keep_name = argv[++i];
        else if (!strcmp(argv[i], "--no-keep")) no_keep = 1;
        else if (!strcmp(argv[i], "--keep-days") && i + 1 < argc) keep_days = atof(argv[++i]);
        else if (!strcmp(argv[i], "--until") && i + 1 < argc) until = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--kept-dir") && i + 1 < argc) kept_dir = argv[++i];
        else if (!strcmp(argv[i], "--rekordbox") && i + 1 < argc) rb = argv[++i];
        else if (!strcmp(argv[i], "--ort") && i + 1 < argc) ort = argv[++i];
        else if (!strncmp(argv[i], "--", 2)) { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
        else { first = i; break; }
    }
    char libdefault[1024], why[400];
    if (!libpath) { library_default_path(libdefault, sizeof libdefault); libpath = libdefault; }
    if (playlists) {
        library *l = library_open(libpath, why, sizeof why);
        int r = l ? library_list(l, stdout, why, sizeof why) : -1;
        library_close(l);
        if (r) { fprintf(stderr, "%s\n", why); return 1; }
        return 0;
    }
    if (track_list) {
        library *l = library_open(libpath, why, sizeof why);
        char **pl = NULL;
        size_t np = 0;
        int r = l ? library_tracks(l, track_list, &pl, &np, why, sizeof why) : -1;
        library_close(l);
        if (r) { fprintf(stderr, "%s\n", why); return 1; }
        for (size_t i = 0; i < np; i++) {
            printf("{\"file\":"); json_str(stdout, pl[i]); printf("}\n");
            free(pl[i]);
        }
        free(pl);
        return 0;
    }
    time_t now = time(NULL);
    if (!until) until = (long long)now + (long long)(keep_days * 86400);
    if (!keep_name && playlist && !no_keep) keep_name = playlist;
    if (no_keep) keep_name = NULL;
    if (!model_path || !cache || max_gb <= 0 || threads < 1 || threads > 64 || keep_days <= 0 || until <= (long long)now) {
        fprintf(stderr, "usage: %s [--estimate] --model MODEL.onnx --cache DIR [--max-gb N] [--chosen] [--threads N] [--list FILE] [--playlist ID [--library DB]] [--first N]\n"
                        "          [--keep NAME | --no-keep] [--keep-days N | --until SECONDS] [--kept-dir DIR] FILE...\n"
                        "       %s --playlists [--library DB]\n"
                        "       %s --tracks ID [--library DB]\n", argv[0], argv[0], argv[0]);
        return 2;
    }
    /* the model that runs decides the folder (requirement 8): ours under its checksum, rekordbox's
     * under the bridge's rebuilt id */
    char fp[65], id[65];
    if (file_sha256(model_path, fp)) { fprintf(stderr, "can't read the model %s\n", model_path); return 1; }
    int ours = !strcmp(fp, OURS_V4);
    if (!ours && strcmp(fp, PIONEER_0002)) { fprintf(stderr, "not a model the bridge caches: %s\n", fp); return 1; }
    if (ours) memcpy(id, fp, 65);
    else rebuilt_id(fp, id);
    struct stat cst;
    /* --quick with the cache on another drive doesn't look at it (not even this stat): a read there
     * may bring up macOS's privacy prompt over rekordbox */
    int cache_blind = quick && !strncmp(cache, "/Volumes/", 9);
    /* this Mac's may have been cleared; the bridge makes it again only once rekordbox runs a model.
     * A chosen location's is never made: its absence means the drive or the folder is gone */
    if (!cache_blind && !chosen) mkdir(cache, 0755);
    if (!cache_blind && (stat(cache, &cst) || !S_ISDIR(cst.st_mode))) { fprintf(stderr, "no cache folder %s\n", cache); return 1; }

    /* the files: the command line's, then the list's */
    size_t nfiles = 0, cap = 64;
    char **files = malloc(sizeof(char *) * cap);
    for (int i = first; i < argc; i++) {
        if (nfiles == cap) files = realloc(files, sizeof(char *) * (cap *= 2));
        files[nfiles++] = strdup(argv[i]);
    }
    if (list) {
        FILE *lf = fopen(list, "r");
        if (!lf) { fprintf(stderr, "can't read %s\n", list); return 1; }
        char *line = NULL;
        size_t ln = 0;
        ssize_t r;
        while ((r = getline(&line, &ln, lf)) > 0) {
            if (line[r - 1] == '\n') line[--r] = 0;
            if (!r) continue;
            if (nfiles == cap) files = realloc(files, sizeof(char *) * (cap *= 2));
            files[nfiles++] = strdup(line);
        }
        free(line);
        fclose(lf);
    }
    if (playlist) {
        library *l = library_open(libpath, why, sizeof why);
        char **pl = NULL;
        size_t np = 0;
        int r = l ? library_tracks(l, playlist, &pl, &np, why, sizeof why) : -1;
        library_close(l);
        if (r) { fprintf(stderr, "%s\n", why); return 1; }
        for (size_t k = 0; k < np; k++) {
            if (nfiles == cap) files = realloc(files, sizeof(char *) * (cap *= 2));
            files[nfiles++] = pl[k];
        }
        free(pl);
    }
    size_t nrun = first_n >= 0 && (size_t)first_n < nfiles ? (size_t)first_n : nfiles;

    /* the kept sets: this run's (a playlist's, or --keep), and the others, which count towards the
     * share and are never removed to make room */
    char kdir[1024];
    if (kept_dir) snprintf(kdir, sizeof kdir, "%s", kept_dir);
    else kept_default_dir(kdir, sizeof kdir);
    kept *k = kept_open(kdir, keep_name, (time_t)until, cache_blind ? "" : cache, why, sizeof why);   /* "": no sizes */
    long long max_bytes = (long long)(max_gb * 1073741824.0);
    if (estimating) {
        if (!k) { printf("{\"refused\":"); json_str(stdout, why); printf("}\n"); return 0; }
        manifest own;
        if (!nfiles && !playlist && keep_name && manifest_read(kdir, keep_name, &own) == 0) {    /* the set's own files */
            files = realloc(files, sizeof(char *) * (own.n ? own.n : 1));
            for (size_t i = 0; i < own.n; i++) files[nfiles++] = strdup(own.f[i].path);
            manifest_free(&own);
        }
        estimate(files, nfiles, cache, id, k, keep_name, kdir, threads, max_bytes, wait < 0 ? 2 : wait, quick);
        kept_close(k);
        for (size_t i = 0; i < nfiles; i++) free(files[i]);
        free(files);
        return 0;
    }
    if (!k) { fprintf(stderr, "%s\n", why); return 1; }
    /* next to a playing rekordbox: the utility class (its threads, ONNX Runtime's included, inherit it) */
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    if (wait < 0) wait = 60;
    if (model_load_ort(ort, why, sizeof why)) { fprintf(stderr, "%s\n", why); return 1; }
    model *m = model_open(model_path, threads, ours, why, sizeof why);
    if (!m) { fprintf(stderr, "%s\n", why); return 1; }

    /* the set's manifest: this run's files, and the earlier one's (kept while the run goes) */
    manifest mine = {0}, earlier = {0};
    size_t nms = 0;
    double rate_ = 0;
    manifest *ms = manifests(kdir, id, threads, &nms, &rate_);     /* what is prepared already */
    int read_earlier = keep_name && manifest_read(kdir, keep_name, &earlier) == 0;
    int same_model = read_earlier && !strcmp(earlier.model, id);    /* its keys are this model's */
    int have_earlier = same_model && !strcmp(earlier.helper, HELPER_VERSION);
    snprintf(mine.helper, sizeof mine.helper, "%s", HELPER_VERSION);
    snprintf(mine.model, sizeof mine.model, "%s", id);
    snprintf(mine.playlist, sizeof mine.playlist, "%s", playlist && strlen(playlist) < sizeof mine.playlist ? playlist : "");
    mine.threads = threads;
    if (have_earlier && earlier.threads == threads) { mine.runs = earlier.runs; mine.run_s = earlier.run_s; }
    long long runs = 0;
    double run_s = 0;

    /* the set's earlier entries: what these tracks hold stays protected, and counts towards the
     * share as the run reaches them (past N, now: a prepared track's stay). The others (a track that
     * left the playlist, another model's) hold neither the share nor the room, as the estimate
     * says, and leave the set when the run finishes. With the cache out of reach nothing is known,
     * and the run stops below */
    if (keep_name && !cache_gone(cache)) {
        for (size_t fi = 0; same_model && fi < nfiles; fi++) {
            const mfile *r = manifest_by_path(&earlier, files[fi]);
            struct stat st;
            /* a partly prepared track, or one the run will find changed, holds no room for the
             * tracks before it. Where a look may ask the user, the run finds out when it gets there */
            if (!r || !r->complete) continue;
            if (fi < nrun && !may_ask(files[fi]) && !stat(files[fi], &st) && !manifest_find(&earlier, files[fi], &st)) continue;
            for (size_t j = 0; j < r->nkeys; j++) kept_hold(k, id, r->keys[j], fi >= nrun);
        }
        if (same_model) manifest_keep_only(&earlier, files, nfiles);
        kept_release(k);
    }

    long long used = disk_used(cache);
    long long share = (long long)(KEPT_SHARE * (double)max_bytes);
    long long next = 4LL << 20;        /* an entry: about 3 MB (8 channels, 16-bit, 11 s, FLAC) */
    time_t written_at = time(NULL);
    float *c = malloc(sizeof(float) * 2 * CHUNK_L);
    const char *stop = NULL;
    char stopbuf[200];
    size_t done = 0;
    for (size_t fi = 0; fi < nrun && !stop; fi++) {
        const char *path = files[fi];
        tracks ts;
        struct stat fst;
        read_state rs;
        if (interrupted) { stop = "stopped"; break; }
        /* its drive unplugged during the run: nothing can be known about its entries, and the
         * records of the tracks not reached stay as they were */
        if (cache_gone(cache)) { stop = CACHE_GONE; break; }
        if (opening(path, wait, &fst, &rs)) {                           /* one file never stops the batch */
            if (keep_name && have_earlier && carry(&mine, NULL, &earlier, k, cache, id, path, NULL)) stop = CACHE_GONE;
            continue;
        }
        /* changed since this set prepared it: what it held stays protected only if the run finds it */
        if (same_model && !manifest_find(&earlier, path, &fst)) {
            const mfile *old = manifest_by_path(&earlier, path);
            for (size_t j = 0; old && j < old->nkeys; j++) kept_unhold(k, old->keys[j]);
        }
        /* unchanged since a set's manifest recorded it, every entry still there: nothing to decode,
         * its entries are used now (touched) and kept again */
        const mfile *rec = NULL;
        for (size_t i = 0; i < nms && !rec; i++) {
            const mfile *r = manifest_find(&ms[i], path, &fst);
            if (r && r->complete) rec = r;
        }
        /* nothing recorded (every part refused, or silent): decoded again, so the line says why */
        if (rec && rec->nkeys == 0) rec = NULL;
        long long more = 0;                 /* what keeping it adds: its entries no set keeps yet */
        for (size_t j = 0; rec && j < rec->nkeys; j++) {
            long long b = kept_entry_bytes(cache, id, rec->keys[j]);
            if (b < 0) rec = NULL;
            else if (!kept_has(k, rec->keys[j])) more += b;
        }
        if (rec && (!keep_name || more == 0 || kept_bytes(k) + more <= share)) {
            mfile *mf = keep_name ? manifest_file(&mine, path, &fst) : NULL;
            for (size_t j = 0; j < rec->nkeys; j++) {
                char p[4096];
                snprintf(p, sizeof p, "%s/%s/%.2s/%s.flac", cache, id, rec->keys[j], rec->keys[j]);
                utimensat(AT_FDCWD, p, NULL, AT_SYMLINK_NOFOLLOW);
                kept_add(k, id, rec->keys[j], kept_entry_bytes(cache, id, rec->keys[j]));
                mfile_key(mf, rec->keys[j]);
            }
            if (mf) mf->complete = 1;
            printf("{\"file\":"); json_str(stdout, path);
            printf(",\"result\":\"prepared\",\"written\":0,\"already\":%zu,\"silent\":0,\"refused\":0,\"how\":\"unchanged since it was prepared\"}\n", rec->nkeys);
            fflush(stdout);
            done++;
            continue;
        }
        if (load_tracks(path, rb, &ts, why, sizeof why)) {
            skipped(path, why, NULL, NULL);
            if (keep_name && have_earlier && carry(&mine, NULL, &earlier, k, cache, id, path, &fst)) stop = CACHE_GONE;
            continue;
        }
        mfile *mf = keep_name ? manifest_file(&mine, path, &fst) : NULL;
        size_t made = 0, already = 0, silent = 0, refused = 0, most = 0;
        for (int t = 0; t < ts.count; t++) if (n_chunks(ts.t[t].n) > most) most = n_chunks(ts.t[t].n);
        int failed = 0;
        for (size_t i = 0; i < most && !stop && !failed; i++) {
            if (interrupted) { stop = "stopped"; break; }
            printf("{\"at\":%zu,\"files\":%zu,\"chunk\":%zu,\"chunks\":%zu}\n", fi, nrun, i, most);
            fflush(stdout);
            /* the decoded versions share the model's stems for chunk i (per length): the first
             * version's run serves the others */
            shared sh[MAX_TRACKS];
            int nsh = 0;
            /* versions that differ only past this chunk give it the same key: the first one's
             * store or keep serves them all, and the chunk counts once */
            char seen[MAX_TRACKS][65];
            int nseen = 0;
            for (int t = 0; t < ts.count && !stop && !failed; t++) {
                if (i >= n_chunks(ts.t[t].n)) continue;
                chunk_at(&ts.t[t], i, c);
                char key[65];
                chunk_key(id, c, CHUNK_L, FRAMES_F, key);
                int dup = 0;
                for (int d = 0; d < nseen && !dup; d++) dup = !memcmp(seen[d], key, 64);
                if (dup) continue;
                memcpy(seen[nseen++], key, 65);
                int x = prefix_dir(cache, id, key, 0);
                if (x >= 0 && keep(x, key)) {    /* prepared again: it counts as used now */
                    char name[80], dn[80];
                    snprintf(name, sizeof name, "%s.flac", key);
                    snprintf(dn, sizeof dn, "%s.desc", key);
                    long long size = blocks(x, name) + blocks(x, dn);
                    /* the share: one no set keeps yet takes no new disk space, but no longer goes
                     * first to make room. One a set keeps already adds nothing */
                    if (keep_name && !kept_has(k, key) && kept_bytes(k) + size > share) {
                        stop = share_stop(stopbuf, sizeof stopbuf, max_bytes);
                        close(x);
                        break;
                    }
                    utimensat(x, name, NULL, AT_SYMLINK_NOFOLLOW);
                    kept_add(k, id, key, size);
                    mfile_key(mf, key);
                    already++;
                    close(x);
                    continue;
                }
                if (x >= 0) close(x);
                double mean, std;
                mean_std(c, 2 * CHUNK_L, &mean, &std);
                if (!isfinite(mean) || !isfinite(std) || std <= 0) { silent++; continue; }   /* never cached */
                /* the share: kept sets take at most 75% of max_gb together, the rest stays for
                 * the stems rekordbox saves during a set */
                if (keep_name && kept_bytes(k) + next > share) {
                    stop = share_stop(stopbuf, sizeof stopbuf, max_bytes);
                    break;
                }
                /* over 90%: room is made by removing the least recently used entries no set keeps
                 * (this run's included), down to 80%, so it isn't made for every entry */
                if (over_limit(used, next, max_bytes)) {
                    /* with a small limit 80% may not leave room for the next one under 90% */
                    double down_to = 0.8 * (double)max_bytes, under = 0.9 * (double)max_bytes - (double)next;
                    long long removed = make_room(cache, k, &used, (long long)(under < down_to ? under : down_to));
                    if (removed) { printf("{\"room\":%lld,\"cache_gb\":%.3f}\n", removed, used / 1073741824.0); fflush(stdout); }
                }
                if ((stop = room(cache, used, next, max_bytes, stopbuf, sizeof stopbuf))) break;
                x = prefix_dir(cache, id, key, 1);
                if (x < 0) { snprintf(stopbuf, sizeof stopbuf, "the cache folder can't be written: %s", strerror(errno)); stop = stopbuf; break; }
                int s = 0;
                while (s < nsh && sh[s].n != ts.t[t].n) s++;
                if (s == nsh) {
                    float *xo = NULL, *xt = NULL;
                    size_t F = 0;
                    struct timespec t0, t1;
                    clock_gettime(CLOCK_MONOTONIC, &t0);
                    if (model_run(m, c, CHUNK_L, ours, &xo, &xt, &F, why, sizeof why)) {
                        close(x);
                        failed = 1;
                        break;
                    }
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    runs++;
                    run_s += (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
                    sh[s].n = ts.t[t].n;
                    sh[s].pcm = malloc(sizeof(int16_t) * 8 * CHUNK_L);
                    sh[s].why = NULL;
                    if (!sh[s].pcm) sh[s].why = "out of memory";
                    else if (ours) { if (ours_pcm(xt, c, CHUNK_L, sh[s].pcm, &sh[s].why)) { free(sh[s].pcm); sh[s].pcm = NULL; } }
                    else if (rebuilt_pcm(xo, F, xt, c, CHUNK_L, sh[s].pcm, &sh[s].why)) { free(sh[s].pcm); sh[s].pcm = NULL; }
                    free(xo);
                    free(xt);
                    nsh++;
                }
                if (!sh[s].pcm) { refused++; close(x); continue; }
                char fn[80], dn[80];
                snprintf(fn, sizeof fn, "%s.flac", key);
                snprintf(dn, sizeof dn, "%s.desc", key);
                long long before = blocks(x, fn) + blocks(x, dn);
                const char *no;
                int err = 0, r = store(x, key, sh[s].pcm, c, CHUNK_L, !ours, chosen, HELPER_VERSION, &no, &err);
                if (r == 0) {
                    made++;
                    long long size = blocks(x, fn) + blocks(x, dn);
                    used += size - before;
                    kept_add(k, id, key, size);
                    mfile_key(mf, key);
                    if (size + size / 4 > next) next = size + size / 4;   /* the largest yet, with a margin */
                } else if (r == 1) {
                    refused++;
                } else {
                    snprintf(stopbuf, sizeof stopbuf, "writing into the cache failed: %s", strerror(err));
                    stop = stopbuf;
                }
                close(x);
            }
            for (int s = 0; s < nsh; s++) free(sh[s].pcm);
        }
        if (stop && cache_gone(cache)) {            /* gone during this track: its earlier record stays */
            if (mf) { manifest_drop_last(&mine); mf = NULL; }
            stop = CACHE_GONE;
        }
        if (mf) mf->complete = !failed && !stop;
        if (mf && failed && have_earlier && carry(&mine, mf, &earlier, k, cache, id, path, &fst)) {
            manifest_drop_last(&mine);              /* its earlier record stays */
            mf = NULL;
            stop = CACHE_GONE;
        }
        printf("{\"file\":"); json_str(stdout, path);
        if (failed) { printf(",\"result\":\"skipped\",\"why\":"); json_str(stdout, why); }
        else {
            printf(",\"result\":\"%s\",\"written\":%zu,\"already\":%zu,\"silent\":%zu,\"refused\":%zu,\"how\":",
                   stop ? "stopped" : "prepared", made, already, silent, refused);
            json_str(stdout, ts.how);
        }
        printf("}\n");
        fflush(stdout);
        tracks_free(&ts);
        done++;
        /* what is prepared so far is kept at least once a minute: a run that is killed loses no
         * more (this set's earlier entries stay in the file until the run is done) */
        if (keep_name && !stop && time(NULL) - written_at >= 60) {
            if (kept_write(k, 0, why, sizeof why)) { snprintf(stopbuf, sizeof stopbuf, "%s", why); stop = stopbuf; }
            else if (runs) { mine.runs = runs; mine.run_s = run_s; }
            if (!stop) manifest_write(kdir, keep_name, &mine, have_earlier ? &earlier : NULL, why, sizeof why);
            written_at = time(NULL);
        }
    }
    const char *kept_why = NULL, *manifest_why = NULL;
    char mwhy[400];
    /* --first: what an earlier run prepared of the tracks past N stays in the set */
    for (size_t fi = nrun; keep_name && have_earlier && !stop && fi < nfiles; fi++)
        if (carry(&mine, NULL, &earlier, k, cache, id, files[fi], NULL)) stop = CACHE_GONE;
    /* the set becomes this run's tracks only when the run covered them all to the end */
    int whole = !stop;
    if (keep_name && kept_write(k, whole, why, sizeof why)) kept_why = why;
    if (runs) { mine.runs = runs; mine.run_s = run_s; }
    if (keep_name && !kept_why && manifest_write(kdir, keep_name, &mine, whole || !have_earlier ? NULL : &earlier, mwhy, sizeof mwhy))
        manifest_why = mwhy;
    printf("{\"done\":%zu", done);
    if (stop) { printf(",\"stopped\":"); json_str(stdout, stop); }
    printf(",\"runs\":%lld,\"run_s\":%.1f", runs, run_s);
    if (keep_name) {
        printf(",\"kept\":"); json_str(stdout, keep_name);
        printf(",\"until\":%lld,\"kept_gb\":%.3f", until, kept_bytes(k) / 1073741824.0);
        if (kept_why) { printf(",\"kept_error\":"); json_str(stdout, kept_why); }
        if (manifest_why) { printf(",\"manifest_error\":"); json_str(stdout, manifest_why); }
    }
    printf("}\n");
    manifest_free(&mine);
    manifest_free(&earlier);
    for (size_t i = 0; i < nms; i++) manifest_free(&ms[i]);
    free(ms);
    kept_close(k);
    free(c);
    for (size_t i = 0; i < nfiles; i++) free(files[i]);
    free(files);
    model_close(m);
    return 0;
}
