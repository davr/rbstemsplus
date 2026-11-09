/* big.c on Windows: plain heap buffers (the macOS version backs big ones with unlinked files so a
 * 2-hour mix isn't swapped; Windows pages out as it needs to) */
#include <stdlib.h>
#include "../big.h"
void *big_alloc(size_t n) { return malloc(n); }
void *big_calloc(size_t count, size_t size) { return calloc(count, size); }
void *big_realloc(void *p, size_t n) { return realloc(p, n); }
void big_free(void *p) { free(p); }
