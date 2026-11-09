/* rekordbox's own sqlite3.dll (SQLCipher), loaded at run time: the sqlite3 calls library.c makes,
 * as macros over function pointers, with the constants and types they need */
#ifndef RBSP_SQLITE_DYN_H
#define RBSP_SQLITE_DYN_H
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
#define SQLITE_OK 0
#define SQLITE_NOMEM 7
#define SQLITE_NOTADB 26
#define SQLITE_ROW 100
#define SQLITE_DONE 101
#define SQLITE_OPEN_READONLY 0x00000001
#define SQLITE_OPEN_NOMUTEX 0x00008000
#define SQLITE_TRANSIENT ((void (*)(void *))-1)
static struct {
    int (*open_v2)(const char *, sqlite3 **, int, const char *);
    int (*close)(sqlite3 *);
    int (*key)(sqlite3 *, const void *, int);
    int (*busy_timeout)(sqlite3 *, int);
    int (*exec)(sqlite3 *, const char *, int (*)(void *, int, char **, char **), void *, char **);
    const char *(*errmsg)(sqlite3 *);
    int (*prepare_v2)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
    int (*step)(sqlite3_stmt *);
    int (*finalize)(sqlite3_stmt *);
    int (*column_int)(sqlite3_stmt *, int);
    const unsigned char *(*column_text)(sqlite3_stmt *, int);
    int (*bind_text)(sqlite3_stmt *, int, const char *, int, void (*)(void *));
} sq;
#define sqlite3_open_v2 sq.open_v2
#define sqlite3_close sq.close
#define sqlite3_key sq.key
#define sqlite3_busy_timeout sq.busy_timeout
#define sqlite3_exec sq.exec
#define sqlite3_errmsg sq.errmsg
#define sqlite3_prepare_v2 sq.prepare_v2
#define sqlite3_step sq.step
#define sqlite3_finalize sq.finalize
#define sqlite3_column_int sq.column_int
#define sqlite3_column_text sq.column_text
#define sqlite3_bind_text sq.bind_text
#endif
