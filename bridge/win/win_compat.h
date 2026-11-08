/*
 * Windows stand-ins for the POSIX and Apple facilities the macOS bridge uses, for
 * rbstems_bridge_win.c (and the tests that #include it). Header-only: every function is static.
 *
 *  - files: descriptors are CRT descriptors (_open_osfhandle) over NT handles, so read(), close()
 *    and fdopen() work as usual. The *at() calls open relative to a folder's handle with
 *    NtCreateFile (OBJECT_ATTRIBUTES.RootDirectory): like openat(), a folder renamed or replaced
 *    on the way can't redirect them. The last component of every open is never followed when it is
 *    a reparse point (a symlink or a junction: FILE_OPEN_REPARSE_POINT, then refused, ELOOP), the
 *    counterpart of O_NOFOLLOW. Files are opened with every sharing mode, so a reader never blocks
 *    a delete or a rename (as on macOS); deletes and replacing renames use POSIX semantics where
 *    the file system has them (NTFS, Windows 10 1809+).
 *  - struct rb_stat: st_dev is the volume serial number, st_ino the 64-bit file index, st_blocks the
 *    allocation size in 512-byte units, st_mtime seconds since 1970.
 *  - paths are UTF-8 everywhere in the bridge and turned into UTF-16 here.
 *  - rb_queue: a serial work queue on one thread, with delayed items (libdispatch's serial queue,
 *    dispatch_async, dispatch_after); rb_apply: a parallel for (dispatch_apply).
 *  - rb_sha256: SHA-256 (CommonCrypto's CC_SHA256), self-contained.
 *  - rb_mutex / rb_once: SRWLOCK and INIT_ONCE (pthread mutex and once).
 */
#ifndef RBSTEMS_WIN_COMPAT_H
#define RBSTEMS_WIN_COMPAT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>
#include <winioctl.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

/* ---------------------------------------------------------------- errno values Windows lacks */

#ifndef ELOOP
#define ELOOP 114
#endif
#ifndef EFTYPE
#define EFTYPE 79                       /* "inappropriate file type or format", as on BSD */
#endif
#ifndef EROFS
#define EROFS 30
#endif
#ifndef ENXIO
#define ENXIO 6
#endif
#ifndef ENODEV
#define ENODEV 19
#endif

#define RB_PATH_MAX 1024                /* bytes of UTF-8: the bridge's PATH_MAX */

/* ---------------------------------------------------------------- strings */

static size_t rb_strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) {
        size_t c = l < n - 1 ? l : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return l;
}

/* UTF-8 -> UTF-16 into a malloc'd string (NULL: invalid UTF-8 or no memory) */
static wchar_t *rb_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)n);
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) <= 0) { free(w); w = NULL; }
    return w;
}

/* UTF-16 (n units, or -1: NUL-terminated) -> UTF-8 into out; -1 when it doesn't fit or isn't valid */
static int rb_utf8(const wchar_t *w, int n, char *out, size_t size) {
    int r = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, n, out, (int)size - 1, NULL, NULL);
    if (r <= 0 && !(n == 0)) { if (size) out[0] = 0; return -1; }
    if (n < 0) { /* r counts the NUL */ return 0; }
    out[r] = 0;
    return 0;
}

/* ---------------------------------------------------------------- errors */

static int rb_errno_of(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME:
    case ERROR_INVALID_DRIVE: case ERROR_BAD_NETPATH: case ERROR_DELETE_PENDING:
        return ENOENT;
    case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD: case ERROR_INVALID_OWNER:
        return EACCES;
    case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION:
        return EBUSY;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS:
        return EEXIST;
    case ERROR_WRITE_PROTECT:
        return EROFS;
    case ERROR_DIRECTORY:
        return ENOTDIR;
    case ERROR_NOT_SAME_DEVICE:
        return EXDEV;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL:
        return ENOSPC;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY:
        return ENOMEM;
    case ERROR_TOO_MANY_OPEN_FILES:
        return EMFILE;
    case ERROR_NOT_READY: case ERROR_DEVICE_NOT_CONNECTED: case ERROR_DEV_NOT_EXIST: case ERROR_UNRECOGNIZED_VOLUME:
        return ENODEV;
    case ERROR_CANT_ACCESS_FILE: case ERROR_CANT_RESOLVE_FILENAME: case ERROR_REPARSE_POINT_ENCOUNTERED:
        return ELOOP;
    case ERROR_DIR_NOT_EMPTY:
        return ENOTEMPTY;
    default:
        return EIO;
    }
}

/* ntdll: always present; declared here so that no particular SDK's winternl.h is needed */
typedef LONG rb_status;
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } rb_ustr;
typedef struct { ULONG Length; HANDLE RootDirectory; rb_ustr *ObjectName; ULONG Attributes; PVOID SecurityDescriptor, SecurityQualityOfService; } rb_oattr;
typedef struct { union { rb_status Status; PVOID Pointer; }; ULONG_PTR Information; } rb_iosb;
typedef rb_status (NTAPI *rb_NtCreateFile_t)(PHANDLE, ACCESS_MASK, rb_oattr *, rb_iosb *, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef rb_status (NTAPI *rb_NtSetInformationFile_t)(HANDLE, rb_iosb *, PVOID, ULONG, ULONG);
typedef ULONG (NTAPI *rb_RtlNtStatusToDosError_t)(rb_status);

static rb_NtCreateFile_t rb_NtCreateFile;
static rb_NtSetInformationFile_t rb_NtSetInformationFile;
static rb_RtlNtStatusToDosError_t rb_RtlNtStatusToDosError;

static BOOL CALLBACK rb_nt_init(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (nt) {
        rb_NtCreateFile = (rb_NtCreateFile_t)(void *)GetProcAddress(nt, "NtCreateFile");
        rb_NtSetInformationFile = (rb_NtSetInformationFile_t)(void *)GetProcAddress(nt, "NtSetInformationFile");
        rb_RtlNtStatusToDosError = (rb_RtlNtStatusToDosError_t)(void *)GetProcAddress(nt, "RtlNtStatusToDosError");
    }
    return TRUE;
}

static int rb_nt_ok(void) {
    static INIT_ONCE o = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&o, rb_nt_init, NULL, NULL);
    return rb_NtCreateFile && rb_NtSetInformationFile && rb_RtlNtStatusToDosError;
}

static int rb_errno_nt(rb_status s) {
    if ((ULONG)s == 0xC0000034UL || (ULONG)s == 0xC000003AUL) return ENOENT;   /* OBJECT_NAME/PATH_NOT_FOUND */
    if ((ULONG)s == 0xC0000035UL) return EEXIST;                               /* OBJECT_NAME_COLLISION */
    if ((ULONG)s == 0xC0000103UL) return ENOTDIR;                              /* NOT_A_DIRECTORY */
    if ((ULONG)s == 0xC00000BAUL) return EISDIR;                               /* FILE_IS_A_DIRECTORY */
    if ((ULONG)s == 0xC00000A2UL) return EROFS;                                /* MEDIA_WRITE_PROTECTED */
    if ((ULONG)s == 0xC000000EUL || (ULONG)s == 0xC0000013UL) return ENODEV;    /* NO_SUCH_DEVICE, NO_MEDIA_IN_DEVICE */
    return rb_errno_of(rb_RtlNtStatusToDosError(s));
}

/* ---------------------------------------------------------------- descriptors */

#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x01000000
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x02000000
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x04000000
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0x08000000
#endif

#define S_IFLNK_RB 0xA000                       /* a reparse point: neither a folder nor a file here */
#define RB_ISDIR(m) (((m) & 0xF000) == 0x4000)
#define RB_ISREG(m) (((m) & 0xF000) == 0x8000)

struct rb_stat {
    uint64_t st_dev, st_ino;
    unsigned st_mode;
    long long st_size, st_blocks;
    time_t st_mtime;
};

static HANDLE rb_h(int fd) { return fd >= 0 ? (HANDLE)_get_osfhandle(fd) : INVALID_HANDLE_VALUE; }

static time_t rb_time(FILETIME ft) {
    ULARGE_INTEGER u = {.LowPart = ft.dwLowDateTime, .HighPart = ft.dwHighDateTime};
    return u.QuadPart < 116444736000000000ULL ? 0 : (time_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

static time_t rb_time_li(LARGE_INTEGER li) {
    FILETIME ft = {.dwLowDateTime = (DWORD)li.LowPart, .dwHighDateTime = (DWORD)li.HighPart};
    return rb_time(ft);
}

/* an absolute UTF-8 path as an NT path ("\??\C:\..."), malloc'd; forward slashes become backslashes */
static wchar_t *rb_nt_path(const char *path) {
    wchar_t *w = rb_wide(path);
    if (!w) return NULL;
    size_t n = wcslen(w);
    wchar_t *nt = malloc(sizeof(wchar_t) * (n + 5));
    if (!nt) { free(w); return NULL; }
    for (size_t i = 0; i < n; i++) if (w[i] == L'/') w[i] = L'\\';
    if (!wcsncmp(w, L"\\\\?\\", 4)) swprintf(nt, n + 5, L"\\??\\%ls", w + 4);
    else swprintf(nt, n + 5, L"\\??\\%ls", w);
    free(w);
    return nt;
}

/* NtCreateFile of `name` in the folder `dir` (a descriptor; AT_FDCWD: name is an absolute path).
 * A relative name must be one component. The last component is opened as itself even if it is a
 * reparse point; `nofollow` then refuses it (ELOOP). INVALID_HANDLE_VALUE with errno otherwise. */
static HANDLE rb_nt_open(int dir, const char *name, ACCESS_MASK access, ULONG disposition, ULONG options, int nofollow, int *created) {
    if (!rb_nt_ok()) { errno = ENOSYS; return INVALID_HANDLE_VALUE; }
    wchar_t *w;
    HANDLE root = NULL;
    if (dir == AT_FDCWD) {
        w = rb_nt_path(name);
    } else {
        if (!*name || strchr(name, '/') || strchr(name, '\\')) { errno = EINVAL; return INVALID_HANDLE_VALUE; }
        root = rb_h(dir);
        if (root == INVALID_HANDLE_VALUE) { errno = EBADF; return INVALID_HANDLE_VALUE; }
        w = rb_wide(name);
    }
    if (!w) { errno = ENOENT; return INVALID_HANDLE_VALUE; }
    size_t wl = wcslen(w);
    if (wl * 2 >= 0xFFFF) { free(w); errno = ENAMETOOLONG; return INVALID_HANDLE_VALUE; }
    rb_ustr us = {(USHORT)(wl * 2), (USHORT)(wl * 2), w};
    rb_oattr oa = {sizeof oa, root, &us, 0x40 /* OBJ_CASE_INSENSITIVE */, NULL, NULL};
    rb_iosb io = {0};
    HANDLE h = NULL;
    rb_status s = rb_NtCreateFile(&h, access | SYNCHRONIZE | FILE_READ_ATTRIBUTES, &oa, &io, NULL, FILE_ATTRIBUTE_NORMAL,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disposition,
                                  options | 0x00200000 /* FILE_OPEN_REPARSE_POINT */ | 0x00000020 /* FILE_SYNCHRONOUS_IO_NONALERT */,
                                  NULL, 0);
    free(w);
    if (s < 0) { errno = rb_errno_nt(s); return INVALID_HANDLE_VALUE; }
    if (created) *created = io.Information == 2 /* FILE_CREATED */;
    if (nofollow) {
        FILE_BASIC_INFO bi;
        if (!GetFileInformationByHandleEx(h, FileBasicInfo, &bi, sizeof bi)) { CloseHandle(h); errno = EIO; return INVALID_HANDLE_VALUE; }
        if (bi.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { CloseHandle(h); errno = ELOOP; return INVALID_HANDLE_VALUE; }
    }
    return h;
}

#define RB_FILE_DIRECTORY_FILE 0x00000001
#define RB_FILE_NON_DIRECTORY_FILE 0x00000040
#define RB_FILE_SUPERSEDE 0
#define RB_FILE_OPEN 1
#define RB_FILE_CREATE 2
#define RB_FILE_OPEN_IF 3
#define RB_FILE_OVERWRITE 4
#define RB_FILE_OVERWRITE_IF 5

static int rb_fd_of(HANDLE h, int crt) {
    int fd = _open_osfhandle((intptr_t)h, crt | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) { CloseHandle(h); errno = EMFILE; }
    return fd;
}

/* openat() with O_NOFOLLOW always in force on the last component */
static int rb_openat(int dir, const char *name, int flags, ...) {
    ACCESS_MASK access;
    ULONG disp, opts;
    int crt = 0, acc = flags & (O_RDONLY | O_WRONLY | O_RDWR);
    if (flags & O_DIRECTORY) {
        access = FILE_LIST_DIRECTORY | FILE_TRAVERSE;
        disp = RB_FILE_OPEN;
        opts = RB_FILE_DIRECTORY_FILE;
        crt = _O_RDONLY;
    } else {
        access = acc == O_WRONLY ? FILE_GENERIC_WRITE : acc == O_RDWR ? FILE_GENERIC_READ | FILE_GENERIC_WRITE : FILE_GENERIC_READ;
        if (flags & O_APPEND) { access = (access & ~FILE_WRITE_DATA) | FILE_APPEND_DATA; crt |= _O_APPEND; }
        crt |= acc == O_WRONLY ? _O_WRONLY : acc == O_RDWR ? _O_RDWR : _O_RDONLY;
        if ((flags & O_CREAT) && (flags & O_EXCL)) disp = RB_FILE_CREATE;
        else if ((flags & O_CREAT) && (flags & O_TRUNC)) disp = RB_FILE_OVERWRITE_IF;
        else if (flags & O_CREAT) disp = RB_FILE_OPEN_IF;
        else if (flags & O_TRUNC) disp = RB_FILE_OVERWRITE;
        else disp = RB_FILE_OPEN;
        opts = RB_FILE_NON_DIRECTORY_FILE;
    }
    HANDLE h = rb_nt_open(dir, name, access, disp, opts, 1, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    return rb_fd_of(h, crt);
}

#define rb_open(path, ...) rb_openat(AT_FDCWD, path, __VA_ARGS__)

static int rb_fstat(int fd, struct rb_stat *st) {
    HANDLE h = rb_h(fd);
    BY_HANDLE_FILE_INFORMATION i;
    FILE_STANDARD_INFO si;
    if (h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h, &i)) { errno = h == INVALID_HANDLE_VALUE ? EBADF : rb_errno_of(GetLastError()); return -1; }
    memset(st, 0, sizeof *st);
    st->st_dev = i.dwVolumeSerialNumber;
    st->st_ino = (uint64_t)i.nFileIndexHigh << 32 | i.nFileIndexLow;
    st->st_mode = i.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ? S_IFLNK_RB : i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? 0x4000 : 0x8000;
    st->st_size = (long long)((uint64_t)i.nFileSizeHigh << 32 | i.nFileSizeLow);
    st->st_mtime = rb_time(i.ftLastWriteTime);
    if (GetFileInformationByHandleEx(h, FileStandardInfo, &si, sizeof si)) st->st_blocks = si.AllocationSize.QuadPart / 512;
    else st->st_blocks = (st->st_size + 511) / 512;
    return 0;
}

/* fstatat(): AT_SYMLINK_NOFOLLOW reports a reparse point as one (S_IFLNK_RB); without it the
 * last component is followed */
static int rb_fstatat(int dir, const char *name, struct rb_stat *st, int flag) {
    HANDLE h;
    if (flag & AT_SYMLINK_NOFOLLOW) h = rb_nt_open(dir, name, FILE_READ_ATTRIBUTES, RB_FILE_OPEN, 0, 0, NULL);
    else {
        /* followed: CreateFileW resolves the reparse point (absolute paths only) */
        if (dir != AT_FDCWD) { errno = EINVAL; return -1; }
        wchar_t *w = rb_wide(name);
        if (!w) { errno = ENOENT; return -1; }
        h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS, NULL);
        free(w);
        if (h == INVALID_HANDLE_VALUE) errno = rb_errno_of(GetLastError());
    }
    if (h == INVALID_HANDLE_VALUE) return -1;
    int fd = rb_fd_of(h, _O_RDONLY);
    if (fd < 0) return -1;
    int r = rb_fstat(fd, st);
    _close(fd);
    return r;
}

static int rb_stat(const char *path, struct rb_stat *st) { return rb_fstatat(AT_FDCWD, path, st, 0); }
static int rb_lstat(const char *path, struct rb_stat *st) { return rb_fstatat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW); }

static int rb_mkdirat(int dir, const char *name) {
    HANDLE h = rb_nt_open(dir, name, FILE_LIST_DIRECTORY, RB_FILE_CREATE, RB_FILE_DIRECTORY_FILE, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    CloseHandle(h);
    return 0;
}

static int rb_mkdir(const char *path) {
    wchar_t *w = rb_wide(path);
    if (!w) { errno = ENOENT; return -1; }
    BOOL ok = CreateDirectoryW(w, NULL);
    DWORD e = GetLastError();
    free(w);
    if (!ok) { errno = rb_errno_of(e); return -1; }
    return 0;
}

/* FILE_DISPOSITION_INFORMATION_EX / FILE_RENAME_INFORMATION(_EX), by the NT information classes */
typedef struct { ULONG Flags; } rb_disp_ex;
typedef struct { BOOLEAN DeleteFile; } rb_disp;
typedef struct { union { BOOLEAN ReplaceIfExists; ULONG Flags; }; HANDLE RootDirectory; ULONG FileNameLength; WCHAR FileName[1]; } rb_rename_info;

static int rb_delete_handle(HANDLE h) {
    rb_iosb io;
    rb_disp_ex dx = {0x1 /* DELETE */ | 0x2 /* POSIX_SEMANTICS */ | 0x10 /* IGNORE_READONLY_ATTRIBUTE */};
    rb_status s = rb_NtSetInformationFile(h, &io, &dx, sizeof dx, 64 /* FileDispositionInformationEx */);
    if (s < 0) {
        rb_disp d = {TRUE};
        s = rb_NtSetInformationFile(h, &io, &d, sizeof d, 13 /* FileDispositionInformation */);
    }
    if (s < 0) { errno = rb_errno_nt(s); return -1; }
    return 0;
}

/* unlinkat() of a file (never a folder: AT_REMOVEDIR isn't used by the bridge) */
static int rb_unlinkat(int dir, const char *name, int flag) {
    (void)flag;
    HANDLE h = rb_nt_open(dir, name, DELETE, RB_FILE_OPEN, RB_FILE_NON_DIRECTORY_FILE, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int r = rb_delete_handle(h);
    CloseHandle(h);
    return r;
}

static int rb_unlink(const char *path) { return rb_unlinkat(AT_FDCWD, path, 0); }

/* renameat() within one folder (the bridge's only use: a temporary file onto its final name),
 * replacing the target */
static int rb_renameat(int from_dir, const char *from, int to_dir, const char *to) {
    if (strchr(to, '/') || strchr(to, '\\')) { errno = EINVAL; return -1; }
    HANDLE h = rb_nt_open(from_dir, from, DELETE, RB_FILE_OPEN, RB_FILE_NON_DIRECTORY_FILE, 1, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    wchar_t *w = rb_wide(to);
    size_t wl = w ? wcslen(w) : 0, size = sizeof(rb_rename_info) + wl * sizeof(WCHAR);
    rb_rename_info *ri = w ? calloc(1, size) : NULL;
    int r = -1;
    if (!ri) errno = w ? ENOMEM : EINVAL;
    else {
        rb_iosb io;
        ri->Flags = 0x1 /* REPLACE_IF_EXISTS */ | 0x2 /* POSIX_SEMANTICS */;
        ri->RootDirectory = rb_h(to_dir);
        ri->FileNameLength = (ULONG)(wl * sizeof(WCHAR));
        memcpy(ri->FileName, w, wl * sizeof(WCHAR));
        rb_status s = rb_NtSetInformationFile(h, &io, ri, (ULONG)size, 65 /* FileRenameInformationEx */);
        if (s < 0) {
            memset(ri, 0, sizeof *ri);
            ri->ReplaceIfExists = TRUE;
            ri->RootDirectory = rb_h(to_dir);
            ri->FileNameLength = (ULONG)(wl * sizeof(WCHAR));
            memcpy(ri->FileName, w, wl * sizeof(WCHAR));
            s = rb_NtSetInformationFile(h, &io, ri, (ULONG)size, 10 /* FileRenameInformation */);
        }
        if (s < 0) errno = rb_errno_nt(s);
        else r = 0;
    }
    free(ri);
    free(w);
    CloseHandle(h);
    return r;
}

/* rename() of absolute paths, replacing the target (the log's rotation, bridge-state, memos) */
static int rb_rename(const char *from, const char *to) {
    wchar_t *a = rb_wide(from), *b = rb_wide(to);
    BOOL ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
    DWORD e = GetLastError();
    free(a);
    free(b);
    if (!ok) { errno = a && b ? rb_errno_of(e) : ENOENT; return -1; }
    return 0;
}

/* the "last used" time: modification time now (futimes(fd, NULL)) */
static int rb_futimes_now(int fd) {
    HANDLE h = rb_h(fd), w;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    w = h == INVALID_HANDLE_VALUE ? INVALID_HANDLE_VALUE
        : ReOpenFile(h, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    if (w == INVALID_HANDLE_VALUE) { errno = EACCES; return -1; }
    BOOL ok = SetFileTime(w, NULL, NULL, &now);
    CloseHandle(w);
    return ok ? 0 : -1;
}

/* each name in the folder fd (not "." or ".."), with what fstatat(AT_SYMLINK_NOFOLLOW) would say
 * of it (from the folder listing itself: no extra opens). f returning nonzero stops the walk
 * (-1); -1 also when the folder couldn't be read whole. */
typedef int (*rb_name_fn)(void *ctx, const char *name, const struct rb_stat *st);
static int rb_each_name(int fd, rb_name_fn f, void *ctx) {
    HANDLE h = rb_h(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    enum { BUF = 64 * 1024 };
    unsigned char *buf = malloc(BUF);
    if (!buf) return -1;
    int r = 0, first = 1;
    for (;;) {
        if (!GetFileInformationByHandleEx(h, first ? FileFullDirectoryRestartInfo : FileFullDirectoryInfo, buf, BUF)) {
            if (GetLastError() != ERROR_NO_MORE_FILES) r = -1;
            break;
        }
        first = 0;
        for (FILE_FULL_DIR_INFO *e = (FILE_FULL_DIR_INFO *)buf;; e = (FILE_FULL_DIR_INFO *)((unsigned char *)e + e->NextEntryOffset)) {
            char name[RB_PATH_MAX];
            int nl = (int)(e->FileNameLength / sizeof(WCHAR));
            if (rb_utf8(e->FileName, nl, name, sizeof name) == 0 && strcmp(name, ".") && strcmp(name, "..")) {
                struct rb_stat st = {0};
                st.st_mode = e->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ? S_IFLNK_RB
                             : e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY ? 0x4000 : 0x8000;
                st.st_size = e->EndOfFile.QuadPart;
                st.st_blocks = e->AllocationSize.QuadPart / 512;
                st.st_mtime = rb_time_li(e->LastWriteTime);
                if (f(ctx, name, &st)) { r = -1; goto done; }
            }
            if (!e->NextEntryOffset) break;
        }
    }
done:
    free(buf);
    return r;
}

/* whether the folder fd is stored under exactly this name (a volume that ignores case also
 * opens "AB" for "ab") */
static int rb_name_is(int fd, const char *name) {
    HANDLE h = rb_h(fd);
    union { FILE_NAME_INFO i; unsigned char b[sizeof(FILE_NAME_INFO) + 2 * 2048]; } u;
    if (h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(h, FileNameInfo, &u, sizeof u)) return 0;
    int n = (int)(u.i.FileNameLength / sizeof(WCHAR)), s = n;
    while (s > 0 && u.i.FileName[s - 1] != L'\\') s--;
    char got[RB_PATH_MAX];
    return rb_utf8(u.i.FileName + s, n - s, got, sizeof got) == 0 && !strcmp(got, name);
}

/* the folder fd can be written: its volume isn't read-only, and the account may add files to it */
static int rb_dir_writable(int fd, int *err) {
    HANDLE h = rb_h(fd), w;
    DWORD flags = 0;
    if (h == INVALID_HANDLE_VALUE) { *err = EBADF; return 0; }
    if (GetVolumeInformationByHandleW(h, NULL, 0, NULL, NULL, &flags, NULL, 0) && (flags & FILE_READ_ONLY_VOLUME)) { *err = EROFS; return 0; }
    w = ReOpenFile(h, FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_BACKUP_SEMANTICS);
    if (w == INVALID_HANDLE_VALUE) { *err = rb_errno_of(GetLastError()); if (*err == EIO) *err = EACCES; return 0; }
    CloseHandle(w);
    return 1;
}

/* the final path of an absolute path, links resolved (realpath) */
static int rb_realpath(const char *path, char *out, size_t n) {
    wchar_t *w = rb_wide(path), buf[RB_PATH_MAX];
    if (!w) return -1;
    HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD l = GetFinalPathNameByHandleW(h, buf, RB_PATH_MAX, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (!l || l >= RB_PATH_MAX) return -1;
    const wchar_t *p = buf;
    if (!wcsncmp(p, L"\\\\?\\UNC\\", 8)) return -1;        /* a network path: never the real library */
    if (!wcsncmp(p, L"\\\\?\\", 4)) p += 4;
    return rb_utf8(p, -1, out, n);
}

/* ---------------------------------------------------------------- volumes, space */

/* the root of the volume holding path ("E:\"), and its serial number and file system's name */
static int rb_volume_of(const char *path, char *root, size_t n, uint64_t *serial, char *fs, size_t fsn, DWORD *flags) {
    wchar_t *w = rb_wide(path), r[MAX_PATH + 1], fsw[64];
    DWORD ser = 0, fl = 0;
    if (!w) return -1;
    BOOL ok = GetVolumePathNameW(w, r, MAX_PATH + 1);
    free(w);
    if (!ok || !GetVolumeInformationW(r, NULL, 0, &ser, NULL, &fl, fsw, 64)) return -1;
    if (root && rb_utf8(r, -1, root, n)) return -1;
    if (serial) *serial = ser;
    if (fs) rb_utf8(fsw, -1, fs, fsn);
    if (flags) *flags = fl;
    return 0;
}

/* whether the volume at root ("E:\") is an external drive: removable, or fixed on a USB, 1394, SD
 * or MMC bus (Thunderbolt and USB enclosures report USB or a removable media type) */
static int rb_volume_external(const char *root) {
    wchar_t *w = rb_wide(root);
    if (!w) return 0;
    UINT t = GetDriveTypeW(w);
    int ext = t == DRIVE_REMOVABLE;
    if (t == DRIVE_FIXED && wcslen(w) >= 2 && w[1] == L':') {
        wchar_t dev[8];
        swprintf(dev, 8, L"\\\\.\\%lc:", w[0]);
        HANDLE h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            STORAGE_PROPERTY_QUERY q = {StorageDeviceProperty, PropertyStandardQuery, {0}};
            union { STORAGE_DEVICE_DESCRIPTOR d; unsigned char b[1024]; } u;
            DWORD got = 0;
            if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &u, sizeof u, &got, NULL) && got >= sizeof u.d)
                ext = u.d.BusType == BusTypeUsb || u.d.BusType == BusType1394 || u.d.BusType == BusTypeSd || u.d.BusType == BusTypeMmc
                      || u.d.RemovableMedia;
            CloseHandle(h);
        }
    }
    free(w);
    return ext;
}

/* whether root is the volume Windows runs from */
static int rb_volume_system(const char *root) {
    wchar_t win[MAX_PATH + 1], vr[MAX_PATH + 1];
    char sys[MAX_PATH * 3];
    if (!GetSystemWindowsDirectoryW(win, MAX_PATH + 1) || !GetVolumePathNameW(win, vr, MAX_PATH + 1) || rb_utf8(vr, -1, sys, sizeof sys)) return 1;
    return !_stricmp(sys, root);
}

static int rb_space(const char *path, double *free_b, double *total_b) {
    wchar_t *w = rb_wide(path);
    ULARGE_INTEGER avail, total, fr;
    BOOL ok = w && GetDiskFreeSpaceExW(w, &avail, &total, &fr);
    free(w);
    if (!ok) return -1;
    if (free_b) *free_b = (double)avail.QuadPart;
    if (total_b) *total_b = (double)total.QuadPart;
    return 0;
}

/* ---------------------------------------------------------------- locks, once, time */

typedef SRWLOCK rb_mutex;
#define RB_MUTEX_INIT SRWLOCK_INIT
static void rb_lock(rb_mutex *m) { AcquireSRWLockExclusive(m); }
static void rb_unlock(rb_mutex *m) { ReleaseSRWLockExclusive(m); }

typedef INIT_ONCE rb_once_t;
#define RB_ONCE_INIT INIT_ONCE_STATIC_INIT
static BOOL CALLBACK rb_once_tramp(PINIT_ONCE o, PVOID f, PVOID *c) { (void)o; (void)c; ((void (*)(void))f)(); return TRUE; }
static void rb_once(rb_once_t *o, void (*f)(void)) { InitOnceExecuteOnce(o, rb_once_tramp, (PVOID)f, NULL); }

static double rb_mono_seconds(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

static void rb_sleep_seconds(double s) { if (s > 0) Sleep((DWORD)(s * 1000)); }

/* when this process started, seconds since 1970 (the app compares it with the pid's) */
static long rb_process_start(void) {
    FILETIME c, e, k, u;
    return GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? (long)rb_time(c) : 0;
}

/* ---------------------------------------------------------------- work queues */

typedef struct rb_item { struct rb_item *next; void (*fn)(void *); void *ctx; double due; } rb_item;
typedef struct { SRWLOCK lock; CONDITION_VARIABLE cv; rb_item *head; HANDLE thread; int low; } rb_queue;

static DWORD WINAPI rb_queue_main(LPVOID p) {
    rb_queue *q = p;
    if (q->low) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    AcquireSRWLockExclusive(&q->lock);
    for (;;) {
        if (!q->head) { SleepConditionVariableSRW(&q->cv, &q->lock, INFINITE, 0); continue; }
        double now = rb_mono_seconds();
        if (q->head->due > now) {               /* the list is ordered by due time */
            double wait = q->head->due - now;
            SleepConditionVariableSRW(&q->cv, &q->lock, (DWORD)(wait * 1000) + 1, 0);
            continue;
        }
        rb_item *it = q->head;
        q->head = it->next;
        ReleaseSRWLockExclusive(&q->lock);
        it->fn(it->ctx);
        free(it);
        AcquireSRWLockExclusive(&q->lock);
    }
    return 0;
}

static rb_queue *rb_queue_create(int low) {
    rb_queue *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    InitializeSRWLock(&q->lock);
    InitializeConditionVariable(&q->cv);
    q->low = low;
    q->thread = CreateThread(NULL, 0, rb_queue_main, q, 0, NULL);
    if (!q->thread) { free(q); return NULL; }
    return q;
}

/* fn(ctx) on q after `delay` seconds (0: next), in order of due time, one at a time; 0 when queued */
static int rb_after(rb_queue *q, double delay, void (*fn)(void *), void *ctx) {
    rb_item *it = q ? malloc(sizeof *it) : NULL;
    if (!it) return -1;
    it->fn = fn;
    it->ctx = ctx;
    it->due = rb_mono_seconds() + delay;
    AcquireSRWLockExclusive(&q->lock);
    rb_item **p = &q->head;
    while (*p && (*p)->due <= it->due) p = &(*p)->next;
    it->next = *p;
    *p = it;
    WakeConditionVariable(&q->cv);
    ReleaseSRWLockExclusive(&q->lock);
    return 0;
}

static int rb_async(rb_queue *q, void (*fn)(void *), void *ctx) { return rb_after(q, 0, fn, ctx); }

/* fn(ctx, i) for i in [0, n), on up to 8 threads at once; returns when all are done */
typedef struct { void (*fn)(void *, size_t); void *ctx; size_t n; volatile LONG64 next; } rb_apply_job;
static DWORD WINAPI rb_apply_main(LPVOID p) {
    rb_apply_job *j = p;
    for (;;) {
        LONG64 i = InterlockedIncrement64(&j->next) - 1;
        if (i >= (LONG64)j->n) return 0;
        j->fn(j->ctx, (size_t)i);
    }
}
static void rb_apply(size_t n, void (*fn)(void *, size_t), void *ctx) {
    rb_apply_job j = {fn, ctx, n, 0};
    HANDLE t[8];
    int k = 0;
    for (; k < 8 && (size_t)k < n; k++)
        if (!(t[k] = CreateThread(NULL, 0, rb_apply_main, &j, 0, NULL))) break;
    if (k == 0) { rb_apply_main(&j); return; }
    WaitForMultipleObjects((DWORD)k, t, TRUE, INFINITE);
    for (int i = 0; i < k; i++) CloseHandle(t[i]);
}

/* ---------------------------------------------------------------- SHA-256 */

typedef struct { uint32_t h[8]; uint64_t len; unsigned char buf[64]; size_t used; } rb_sha256;

static const uint32_t rb_sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define RB_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void rb_sha_block(rb_sha256 *c, const unsigned char *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = RB_ROR(w[i - 15], 7) ^ RB_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = RB_ROR(w[i - 2], 17) ^ RB_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (RB_ROR(e, 6) ^ RB_ROR(e, 11) ^ RB_ROR(e, 25)) + ((e & f) ^ (~e & g)) + rb_sha_k[i] + w[i];
        uint32_t t2 = (RB_ROR(a, 2) ^ RB_ROR(a, 13) ^ RB_ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void rb_sha_init(rb_sha256 *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->used = 0;
}

static void rb_sha_update(rb_sha256 *c, const void *data, size_t n) {
    const unsigned char *p = data;
    c->len += n;
    if (c->used) {
        size_t take = 64 - c->used < n ? 64 - c->used : n;
        memcpy(c->buf + c->used, p, take);
        c->used += take; p += take; n -= take;
        if (c->used == 64) { rb_sha_block(c, c->buf); c->used = 0; }
    }
    for (; n >= 64; p += 64, n -= 64) rb_sha_block(c, p);
    if (n) { memcpy(c->buf, p, n); c->used = n; }
}

static void rb_sha_final(unsigned char out[32], rb_sha256 *c) {
    uint64_t bits = c->len * 8;
    unsigned char pad = 0x80, z = 0, l[8];
    rb_sha_update(c, &pad, 1);
    while (c->used != 56) rb_sha_update(c, &z, 1);
    for (int i = 0; i < 8; i++) l[i] = (unsigned char)(bits >> (56 - 8 * i));
    rb_sha_update(c, l, 8);
    for (int i = 0; i < 8; i++) { out[4 * i] = (unsigned char)(c->h[i] >> 24); out[4 * i + 1] = (unsigned char)(c->h[i] >> 16); out[4 * i + 2] = (unsigned char)(c->h[i] >> 8); out[4 * i + 3] = (unsigned char)c->h[i]; }
}

/* ---------------------------------------------------------------- inverse real FFT */

/* The plain inverse sum of a Hermitian spectrum of N = 2^bits points, given as its N/2 bins with
 * re[0] the DC and im[0] the Nyquist bin (vDSP_fft_zrip's packed form): out[n] =
 * sum_{k=0}^{N-1} X[k] e^{+2 pi i k n / N}, no 1/N (what vDSP's inverse real FFT returns once
 * unpacked by vDSP_ztoc). Double precision inside. tw: a scratch of 2N doubles. */
static void rb_irfft(const float *re, const float *im, int bits, float *out, double *tw) {
    size_t N = (size_t)1 << bits, H = N / 2;
    double *zr = tw, *zi = tw + N;
    for (size_t k = 0; k < N; k++) {                /* the full spectrum */
        if (k == 0) { zr[k] = re[0]; zi[k] = 0; }
        else if (k == H) { zr[k] = im[0]; zi[k] = 0; }
        else if (k < H) { zr[k] = re[k]; zi[k] = im[k]; }
        else { zr[k] = re[N - k]; zi[k] = -im[N - k]; }
    }
    for (size_t i = 1, j = 0; i < N; i++) {         /* bit reversal */
        size_t b = N >> 1;
        for (; j & b; b >>= 1) j ^= b;
        j |= b;
        if (i < j) { double t = zr[i]; zr[i] = zr[j]; zr[j] = t; t = zi[i]; zi[i] = zi[j]; zi[j] = t; }
    }
    for (size_t len = 2; len <= N; len <<= 1) {     /* radix-2, e^{+i...}: the inverse */
        double ang = 2.0 * 3.14159265358979323846 / (double)len;
        for (size_t k = 0; k < len / 2; k++) {
            double wr = cos(ang * (double)k), wi = sin(ang * (double)k);
            for (size_t i = k; i < N; i += len) {
                size_t j = i + len / 2;
                double xr = zr[j] * wr - zi[j] * wi, xi = zr[j] * wi + zi[j] * wr;
                zr[j] = zr[i] - xr; zi[j] = zi[i] - xi;
                zr[i] += xr; zi[i] += xi;
            }
        }
    }
    for (size_t n = 0; n < N; n++) out[n] = (float)zr[n];
}

#endif
