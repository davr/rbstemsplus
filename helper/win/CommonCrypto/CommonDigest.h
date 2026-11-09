/* CC_SHA256 over the bridge's rb_sha256 */
#ifndef RBSP_WIN_COMMONDIGEST_H
#define RBSP_WIN_COMMONDIGEST_H
#include <stdint.h>
typedef uint32_t CC_LONG;
typedef rb_sha256 CC_SHA256_CTX;
#define CC_SHA256_Init(c) rb_sha_init(c)
#define CC_SHA256_Update(c, d, n) rb_sha_update((c), (d), (n))
#define CC_SHA256_Final(out, c) rb_sha_final((out), (c))
#define CC_SHA256_DIGEST_LENGTH 32
#endif
