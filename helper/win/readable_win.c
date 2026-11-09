/* readable.c on Windows: no privacy prompt here, so a look at a file is a stat and an open */
#include "../readable.h"
#include <errno.h>
#include <string.h>

read_state readable(const char *path, double secs, struct stat *st, char *why, size_t n, const char **folder) {
    (void)secs;
    *folder = "";
    if (stat(path, st)) {
        if (errno == ENOENT || errno == ENOTDIR) snprintf(why, n, "the file isn't there (moved, renamed, or its drive isn't connected)");
        else if (errno == EACCES || errno == EPERM) { snprintf(why, n, "Windows doesn't let RB Stems Plus read it"); return READ_DENIED; }
        else snprintf(why, n, "the file can't be read: %s", strerror(errno));
        return errno == ENOENT || errno == ENOTDIR ? READ_ABSENT : READ_FAILED;
    }
    if (!S_ISREG(st->st_mode)) { snprintf(why, n, "not a file"); return READ_FAILED; }
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM) { snprintf(why, n, "Windows doesn't let RB Stems Plus read it"); return READ_DENIED; }
        snprintf(why, n, "the file can't be read: %s", strerror(errno));
        return READ_FAILED;
    }
    close(fd);
    return READ_OK;
}

const char *protected_folder(const char *path) { (void)path; return ""; }

const char *home_folder(void) {
    static char h[RB_PATH_MAX];
    if (!h[0]) {
        wchar_t *w = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Profile, 0, NULL, &w)) && w) { rb_utf8(w, -1, h, sizeof h); CoTaskMemFree(w); }
    }
    return h;
}
