/* libssh2 crypto backend: axtls + TweetNaCl for the uc386-dos port.
 *
 * Maps libssh2's crypto.h dispatch API (libssh2_sha256_*, _libssh2_rsa_*,
 * libssh2_aes_ctr_*, etc.) onto:
 *
 *   - axtls   (upstream/lib/axtls/crypto/) — SHA1/256/384/512, MD5,
 *              AES-128/256 CBC, HMAC, RSA verify (and sign with the
 *              priv-key path we patch in via fetch.sh)
 *   - TweetNaCl (upstream/lib/tweetnacl/) — Curve25519 / X25519 (KEX)
 *              and Ed25519 (host-key + pubkey auth)
 *
 * What's intentionally OFF:
 *   - ECDSA  (P-256/P-384/P-521) — no NIST EC support in axtls; skip.
 *   - DSA    (legacy) — deprecated, off everywhere.
 *   - Blowfish / RC4 / 3DES — deprecated, off in OpenSSH defaults.
 *   - AES-GCM, ChaCha20-Poly1305 — modern AEAD; nice-to-have, off for v1.
 *
 * The OFF crypto types are still declared (libssh2 references the
 * macros at compile time) but their function bodies all return
 * errors / NULL.
 */
#ifndef LIBSSH2_AXTLS_H
#define LIBSSH2_AXTLS_H

#define LIBSSH2_CRYPTO_ENGINE libssh2_axtls

#include <string.h>
#include <stdint.h>

/* Deliberately do NOT include axtls's headers here — they define
   a `comp` typedef (single-precision bigint component) that
   collides with libssh2_priv.h's struct field `const
   LIBSSH2_COMP_METHOD *comp`. Anything that needs axtls types
   includes them only inside libssh2_axtls.c.

   We use opaque-pointer ctxs so libssh2 sources only see void*
   handles and never pull in the axtls type machinery. */
#include "tweetnacl.h"        /* Curve25519 + Ed25519 (no comp collision) */

/* ------------------------------------------------------------------
 * Feature flags — what this backend supports.
 * ------------------------------------------------------------------ */
#define LIBSSH2_MD5             1

#define LIBSSH2_HMAC_RIPEMD     0
#define LIBSSH2_HMAC_SHA256     1
#define LIBSSH2_HMAC_SHA512     1

#define LIBSSH2_AES_CBC         1
#define LIBSSH2_AES_CTR         1    /* implemented on top of AES-ECB */
#define LIBSSH2_AES_GCM         0
#define LIBSSH2_BLOWFISH        0
#define LIBSSH2_RC4             0
#define LIBSSH2_CAST            0
#define LIBSSH2_3DES            0

#define LIBSSH2_RSA             1
#define LIBSSH2_RSA_SHA1        1
#define LIBSSH2_RSA_SHA2        1
#define LIBSSH2_DSA             0
#define LIBSSH2_ECDSA           0
#define LIBSSH2_ED25519         1

#include "crypto_config.h"

#define MD5_DIGEST_LENGTH       16
#define SHA_DIGEST_LENGTH       20
#define SHA256_DIGEST_LENGTH    32
#define SHA384_DIGEST_LENGTH    48
#define SHA512_DIGEST_LENGTH    64

/* libssh2 wants this even when ECDSA is off (used for sk-* key paths
 * which we don't expose). */
#define EC_MAX_POINT_LEN ((528 * 2 / 8) + 1)

/* Max RSA / DH bigint modulus libssh2 will accept. 2048 bits is
   enough for axtls's RSA (which our backend uses); larger DH
   groups (group14: 2048, group16: 4096) are outside our scope. */
#define LIBSSH2_DH_MAX_MODULUS_BITS 2048

/* ------------------------------------------------------------------
 * Generic init / random.
 * ------------------------------------------------------------------ */
int _libssh2_axtls_init(void);
void _libssh2_axtls_free(void);
int _libssh2_axtls_random(unsigned char *buf, int len);

#define libssh2_crypto_init()  _libssh2_axtls_init()
#define libssh2_crypto_exit()  _libssh2_axtls_free()
#define _libssh2_random(buf, len) _libssh2_axtls_random(buf, len)

#define libssh2_prepare_iovec(vec, len)  /* nothing */

/* ------------------------------------------------------------------
 * Hashes (SHA1/256/384/512, MD5)
 *
 * libssh2 expects per-algo ctx, init/update/final entry points, plus
 * a one-shot "_libssh2_axtls_hash(data,len,out)" helper. We use one
 * unified ctx (libssh2_axtls_hash_ctx) parameterized by algorithm.
 * ------------------------------------------------------------------ */

/* Opaque ctx — backing store sized for the largest axtls hash ctx
   (SHA512 is ~216 bytes). Don't pull axtls types into the public
   header; cast to the real struct inside libssh2_axtls.c. */
#define LIBSSH2_AXTLS_HASH_CTX_BYTES 256

typedef struct libssh2_axtls_hash_ctx_struct {
    int algo;
    unsigned char storage[LIBSSH2_AXTLS_HASH_CTX_BYTES];
} libssh2_axtls_hash_ctx;

#define LIBSSH2_AXTLS_HASH_SHA1    1
#define LIBSSH2_AXTLS_HASH_SHA256  2
#define LIBSSH2_AXTLS_HASH_SHA384  3
#define LIBSSH2_AXTLS_HASH_SHA512  4
#define LIBSSH2_AXTLS_HASH_MD5     5

int _libssh2_axtls_hash_init(libssh2_axtls_hash_ctx *ctx, int algo);
int _libssh2_axtls_hash_update(libssh2_axtls_hash_ctx *ctx,
                                const void *data, size_t len);
int _libssh2_axtls_hash_final(libssh2_axtls_hash_ctx *ctx, unsigned char *out);
int _libssh2_axtls_hash(const void *data, size_t len, int algo, unsigned char *out);

/* SHA1 */
#define libssh2_sha1_ctx     libssh2_axtls_hash_ctx
#define libssh2_sha1_init(pctx) \
    _libssh2_axtls_hash_init(pctx, LIBSSH2_AXTLS_HASH_SHA1)
#define libssh2_sha1_update(ctx, data, datalen) \
    _libssh2_axtls_hash_update(&(ctx), (data), (datalen))
#define libssh2_sha1_final(ctx, hash) \
    _libssh2_axtls_hash_final(&(ctx), (hash))
#define libssh2_sha1(data, datalen, hash) \
    _libssh2_axtls_hash((data), (datalen), LIBSSH2_AXTLS_HASH_SHA1, (hash))

/* SHA256 */
#define libssh2_sha256_ctx   libssh2_axtls_hash_ctx
#define libssh2_sha256_init(pctx) \
    _libssh2_axtls_hash_init(pctx, LIBSSH2_AXTLS_HASH_SHA256)
#define libssh2_sha256_update(ctx, data, datalen) \
    _libssh2_axtls_hash_update(&(ctx), (data), (datalen))
#define libssh2_sha256_final(ctx, hash) \
    _libssh2_axtls_hash_final(&(ctx), (hash))
#define libssh2_sha256(data, datalen, hash) \
    _libssh2_axtls_hash((data), (datalen), LIBSSH2_AXTLS_HASH_SHA256, (hash))

/* SHA384 */
#define libssh2_sha384_ctx   libssh2_axtls_hash_ctx
#define libssh2_sha384_init(pctx) \
    _libssh2_axtls_hash_init(pctx, LIBSSH2_AXTLS_HASH_SHA384)
#define libssh2_sha384_update(ctx, data, datalen) \
    _libssh2_axtls_hash_update(&(ctx), (data), (datalen))
#define libssh2_sha384_final(ctx, hash) \
    _libssh2_axtls_hash_final(&(ctx), (hash))
#define libssh2_sha384(data, datalen, hash) \
    _libssh2_axtls_hash((data), (datalen), LIBSSH2_AXTLS_HASH_SHA384, (hash))

/* SHA512 */
#define libssh2_sha512_ctx   libssh2_axtls_hash_ctx
#define libssh2_sha512_init(pctx) \
    _libssh2_axtls_hash_init(pctx, LIBSSH2_AXTLS_HASH_SHA512)
#define libssh2_sha512_update(ctx, data, datalen) \
    _libssh2_axtls_hash_update(&(ctx), (data), (datalen))
#define libssh2_sha512_final(ctx, hash) \
    _libssh2_axtls_hash_final(&(ctx), (hash))
#define libssh2_sha512(data, datalen, hash) \
    _libssh2_axtls_hash((data), (datalen), LIBSSH2_AXTLS_HASH_SHA512, (hash))

/* MD5 */
#define libssh2_md5_ctx      libssh2_axtls_hash_ctx
#define libssh2_md5_init(pctx) \
    _libssh2_axtls_hash_init(pctx, LIBSSH2_AXTLS_HASH_MD5)
#define libssh2_md5_update(ctx, data, datalen) \
    _libssh2_axtls_hash_update(&(ctx), (data), (datalen))
#define libssh2_md5_final(ctx, hash) \
    _libssh2_axtls_hash_final(&(ctx), (hash))

/* ------------------------------------------------------------------
 * HMAC. axtls only ships HMAC-SHA1 / HMAC-SHA256 directly; we wrap
 * a generic HMAC over our unified hash ctx so SHA512 works too.
 * ------------------------------------------------------------------ */

#define LIBSSH2_AXTLS_HMAC_MAX_KEY    128  /* block size for SHA512 */
#define LIBSSH2_AXTLS_HMAC_MAX_DIGEST 64

typedef struct libssh2_axtls_hmac_ctx_struct {
    int algo;
    int hash_size;
    int block_size;
    unsigned char k_opad[LIBSSH2_AXTLS_HMAC_MAX_KEY];
    libssh2_axtls_hash_ctx inner;
} libssh2_axtls_hmac_ctx;

#define libssh2_hmac_ctx libssh2_axtls_hmac_ctx

/* ------------------------------------------------------------------
 * RSA — uses axtls's RSA_CTX directly. axtls only natively supports
 * verify; signing requires the private exponent we pass through
 * _libssh2_axtls_rsa_new.
 * ------------------------------------------------------------------ */

/* Opaque RSA ctx — points to an axtls RSA_CTX allocated on the
   heap inside libssh2_axtls.c. */
typedef struct {
    void *ctx;     /* really (RSA_CTX *) */
} libssh2_axtls_rsa_ctx;

#define libssh2_rsa_ctx libssh2_axtls_rsa_ctx

#define _libssh2_rsa_new(rsactx, e, e_len, n, n_len, \
                         d, d_len, p, p_len, q, q_len, \
                         e1, e1_len, e2, e2_len, c, c_len) \
    _libssh2_axtls_rsa_new(rsactx, e, e_len, n, n_len, \
                           d, d_len, p, p_len, q, q_len, \
                           e1, e1_len, e2, e2_len, c, c_len)

#define _libssh2_rsa_new_private(rsactx, s, filename, passphrase) \
    _libssh2_axtls_rsa_new_private(rsactx, s, filename, passphrase)
#define _libssh2_rsa_new_private_frommemory(rsactx, s, fd, fd_len, pw) \
    _libssh2_axtls_rsa_new_private_frommemory(rsactx, s, fd, fd_len, pw)

#define _libssh2_rsa_sha1_sign(s, rsactx, hash, hash_len, sig, sig_len) \
    _libssh2_axtls_rsa_sha1_sign(s, rsactx, hash, hash_len, sig, sig_len)
#define _libssh2_rsa_sha2_sign(s, rsactx, hash, hash_len, sig, sig_len) \
    _libssh2_axtls_rsa_sha2_sign(s, rsactx, hash, hash_len, sig, sig_len)
#define _libssh2_rsa_sha1_verify(rsactx, sig, sig_len, m, m_len) \
    _libssh2_axtls_rsa_sha1_verify(rsactx, sig, sig_len, m, m_len)
#define _libssh2_rsa_sha2_verify(rsactx, hash_len, sig, sig_len, m, m_len) \
    _libssh2_axtls_rsa_sha2_verify(rsactx, hash_len, sig, sig_len, m, m_len)

#define _libssh2_rsa_free(rsactx) _libssh2_axtls_rsa_free(rsactx)

/* ------------------------------------------------------------------
 * DSA / ECDSA — stubbed off.
 * ------------------------------------------------------------------ */
#if LIBSSH2_ECDSA
# error "ECDSA not supported in axtls backend"
#endif
#define _libssh2_ec_key void

#if LIBSSH2_DSA
# error "DSA not supported in axtls backend"
#endif

/* ------------------------------------------------------------------
 * Ed25519 — TweetNaCl's crypto_sign / crypto_sign_open.
 * ------------------------------------------------------------------ */

typedef struct {
    unsigned char public_key[32];
    unsigned char private_key[64];   /* secret-key + public-key concat */
    int have_private;
} libssh2_ed25519_ctx;

#define _libssh2_ed25519_new_public(ed_ctx, session, raw_pub_key, key_len) \
    _libssh2_axtls_ed25519_new_public(ed_ctx, session, raw_pub_key, key_len)

#define _libssh2_ed25519_new_private(ed_ctx, session, filename, passphrase) \
    _libssh2_axtls_ed25519_new_private(ed_ctx, session, filename, passphrase)

#define _libssh2_ed25519_new_private_frommemory(ed_ctx, session, fd, fd_len, pw) \
    _libssh2_axtls_ed25519_new_private_frommemory(ed_ctx, session, fd, fd_len, pw)

#define _libssh2_ed25519_sign(ctx, session, sig, sig_len, msg, msg_len) \
    _libssh2_axtls_ed25519_sign(ctx, session, sig, sig_len, msg, msg_len)

#define _libssh2_ed25519_verify(ctx, sig, sig_len, m, m_len) \
    _libssh2_axtls_ed25519_verify(ctx, sig, sig_len, m, m_len)

#define _libssh2_curve25519_gen_k(k, p, srv_pub) \
    _libssh2_axtls_curve25519_gen_k(k, p, srv_pub)

#define _libssh2_curve25519_new(session, out_public, out_private) \
    _libssh2_axtls_curve25519_new(session, out_public, out_private)

#define _libssh2_ed25519_free(ctx) _libssh2_axtls_ed25519_free(ctx)

/* ------------------------------------------------------------------
 * AES — CBC + CTR over axtls's AES core.
 * ------------------------------------------------------------------ */

/* Opaque AES ctx. Backing store sized for axtls's AES_CTX
   (~256 bytes). */
#define LIBSSH2_AXTLS_AES_CTX_BYTES 320
typedef struct {
    int keylen_bits;
    unsigned char ctr_iv[16];
    int is_ctr;
    unsigned char storage[LIBSSH2_AXTLS_AES_CTX_BYTES];
} libssh2_axtls_aes_ctx;

#define _libssh2_cipher_type(name) int name
#define _libssh2_cipher_ctx libssh2_axtls_aes_ctx

#define _libssh2_cipher_aes128       1
#define _libssh2_cipher_aes192       2
#define _libssh2_cipher_aes256       3
#define _libssh2_cipher_aes128ctr    4
#define _libssh2_cipher_aes192ctr    5
#define _libssh2_cipher_aes256ctr    6
#define _libssh2_cipher_3des         7   /* declared but unsupported */
#define _libssh2_cipher_blowfish     8   /* declared but unsupported */

int _libssh2_axtls_cipher_init(_libssh2_cipher_ctx *ctx, _libssh2_cipher_type(algo),
                                unsigned char *iv, unsigned char *secret, int encrypt);
int _libssh2_axtls_cipher_crypt(_libssh2_cipher_ctx *ctx, _libssh2_cipher_type(algo),
                                 int encrypt, unsigned char *block, size_t blocklen);
void _libssh2_axtls_cipher_dtor(_libssh2_cipher_ctx *ctx);

#define _libssh2_cipher_init(ctx, type, iv, secret, encrypt) \
    _libssh2_axtls_cipher_init(ctx, type, iv, secret, encrypt)
#define _libssh2_cipher_crypt(ctx, type, encrypt, block, blocklen) \
    _libssh2_axtls_cipher_crypt(ctx, type, encrypt, block, blocklen)
#define _libssh2_cipher_dtor(ctx) _libssh2_axtls_cipher_dtor(ctx)

/* ------------------------------------------------------------------
 * Public key file parsing — minimal PEM/OpenSSH-format support.
 * ------------------------------------------------------------------ */

#define _libssh2_pub_priv_keyfile(s, m, m_len, p, p_len, pk, pw) \
    _libssh2_axtls_pub_priv_keyfile(s, m, m_len, p, p_len, pk, pw)
#define _libssh2_pub_priv_keyfilememory(s, m, m_len, p, p_len, pk, pk_len, pw) \
    _libssh2_axtls_pub_priv_keyfilememory(s, m, m_len, p, p_len, pk, pk_len, pw)
#define _libssh2_sk_pub_keyfilememory(s, m, ml, p, pl, alg, app, fl, hd, hdl, pk, pkl, pw) \
    _libssh2_axtls_sk_pub_keyfilememory(s, m, ml, p, pl, alg, app, fl, hd, hdl, pk, pkl, pw)
#define _libssh2_sk_pub_openssh_keyfilememory(s, m, ml, p, pl, alg, app, fl, hd, hdl, pk, pkl, pw) \
    _libssh2_axtls_sk_pub_keyfilememory(s, m, ml, p, pl, alg, app, fl, hd, hdl, pk, pkl, pw)

/* ------------------------------------------------------------------
 * Diffie-Hellman group exchange — used as KEX fallback when both
 * peers don't have Curve25519. axtls's bigint covers this.
 * ------------------------------------------------------------------ */

/* Opaque DH/bigint ctx — backing store for axtls's BI_CTX + bigints. */
typedef struct {
    void *x; void *e; void *p; void *g;     /* axtls bigints */
    void *bi_ctx;                            /* axtls BI_CTX */
} _libssh2_bn_ctx;

typedef void _libssh2_bn;
typedef _libssh2_bn_ctx _libssh2_dh_ctx;

#define _libssh2_dh_init(dhctx)  _libssh2_axtls_dh_init(dhctx)
#define _libssh2_dh_key_pair(dhctx, public, g, p, group_order, bnctx) \
    _libssh2_axtls_dh_key_pair(dhctx, public, g, p, group_order, bnctx)
#define _libssh2_dh_secret(dhctx, secret, f, p, bnctx) \
    _libssh2_axtls_dh_secret(dhctx, secret, f, p, bnctx)
#define _libssh2_dh_dtor(dhctx)  _libssh2_axtls_dh_dtor(dhctx)

#define _libssh2_bn_init() _libssh2_axtls_bn_init()
#define _libssh2_bn_init_from_bin() _libssh2_bn_init()
#define _libssh2_bn_set_word(bn, val) _libssh2_axtls_bn_set_word(bn, val)
#define _libssh2_bn_from_bin(bn, len, val) _libssh2_axtls_bn_from_bin(bn, len, val)
#define _libssh2_bn_to_bin(bn, val) _libssh2_axtls_bn_to_bin(bn, val)
#define _libssh2_bn_bytes(bn) _libssh2_axtls_bn_bytes(bn)
#define _libssh2_bn_bits(bn) _libssh2_axtls_bn_bits(bn)
#define _libssh2_bn_free(bn) _libssh2_axtls_bn_free(bn)

#define _libssh2_bn_ctx_new()  _libssh2_axtls_bn_ctx_new()
#define _libssh2_bn_ctx_free(c) _libssh2_axtls_bn_ctx_free(c)

#endif /* LIBSSH2_AXTLS_H */
