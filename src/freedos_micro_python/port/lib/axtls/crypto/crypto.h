// uc386-dos shim header that mimics the subset of axtls's crypto.h
// used by upstream/extmod/modhashlib.c (when MICROPY_SSL_AXTLS=1).
//
// We don't actually link axtls — instead we route the AXTLS-shaped
// API to Brad Conte's public-domain reference implementations
// (`upstream/lib/crypto-algorithms/{md5,sha1}.[ch]`, fetched
// alongside the existing sha256 sources). The B-Con API is nearly
// identical: same MD5_CTX / SHA1_CTX typedef names, same Update
// argument order. Only `*_Final` differs — axtls takes
// `(digest, ctx)` while B-Con takes `(ctx, digest)` — so we wrap.
//
// Setting MICROPY_SSL_AXTLS=1 in mpconfigport.h tells modhashlib.c
// to take the AXTLS branch of its #if cascade. We don't enable the
// rest of axtls (TLS, RSA, AES, ...); the single include below is
// sufficient for hashlib.md5 and hashlib.sha1.
#ifndef UCDOS_AXTLS_CRYPTO_SHIM_H
#define UCDOS_AXTLS_CRYPTO_SHIM_H

#include "lib/crypto-algorithms/md5.h"
#include "lib/crypto-algorithms/sha1.h"

#define MD5_SIZE  16
#define SHA1_SIZE 20

static inline void MD5_Init(MD5_CTX *ctx) {
    md5_init(ctx);
}
static inline void MD5_Update(MD5_CTX *ctx, const unsigned char *msg, int len) {
    md5_update(ctx, msg, (size_t)len);
}
static inline void MD5_Final(unsigned char *digest, MD5_CTX *ctx) {
    md5_final(ctx, digest);
}

static inline void SHA1_Init(SHA1_CTX *ctx) {
    sha1_init(ctx);
}
static inline void SHA1_Update(SHA1_CTX *ctx, const unsigned char *msg, int len) {
    sha1_update(ctx, msg, (size_t)len);
}
static inline void SHA1_Final(unsigned char *digest, SHA1_CTX *ctx) {
    sha1_final(ctx, digest);
}

#endif  // UCDOS_AXTLS_CRYPTO_SHIM_H
