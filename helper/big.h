/* big.c: buffers for whole files and whole decoded tracks (requirement 5: a 2-hour mix next to
 * rekordbox on a 16 GB Mac must not swap). From 32 MB a buffer's pages belong to a private file,
 * unlinked at once, in this account's temporary folder: macOS writes them out and drops them
 * under memory pressure, as it does a file's, instead of compressing and swapping them. Smaller
 * ones, or when no file can be made, are malloc's. Free each with big_free, never free. */
#ifndef RBSP_BIG_H
#define RBSP_BIG_H
#include <stddef.h>

void *big_alloc(size_t n);
void *big_calloc(size_t count, size_t size);
void *big_realloc(void *p, size_t n);      /* as realloc: NULL leaves P as it was */
void big_free(void *p);

#endif
