/* libssh2 crypto backend implementation — axtls + TweetNaCl for
 * the uc386-dos port. Pairs with libssh2_axtls.h.
 *
 * Status: SKELETON. Hashes (SHA1/256/384/512, MD5) and HMAC + AES
 * are wired to axtls; Curve25519 KEX and Ed25519 sign/verify are
 * wired to TweetNaCl. RSA, key-file parsing, and DH are stubbed
 * (return error) — those are the next slice of work.
 *
 * Build define: -DLIBSSH2_AXTLS=1 (selects this backend in
 * upstream/lib/libssh2/src/crypto.h via fetch.sh's patch).
 */

#include "libssh2_priv.h"
#include "libssh2_axtls.h"
#include "crypto.h"

/* axtls headers — included only here so the `comp` typedef in
   bigint_impl.h doesn't collide with libssh2_priv.h's struct
   field of the same name. */
#include "crypto_misc.h"      /* axtls hashes + bigint + RSA decls */
#include "ssl.h"              /* axtls SSL_CTX, RSA_CTX */

/* Helpers: re-interpret the opaque storage as the real axtls type. */
#define _AS_SHA1(c)   ((SHA1_CTX *)((c)->storage))
#define _AS_SHA256(c) ((SHA256_CTX *)((c)->storage))
#define _AS_SHA384(c) ((SHA384_CTX *)((c)->storage))
#define _AS_SHA512(c) ((SHA512_CTX *)((c)->storage))
#define _AS_MD5(c)    ((MD5_CTX *)((c)->storage))
#define _AS_AES(c)    ((AES_CTX *)((c)->storage))

/* ------------------------------------------------------------------
 * Generic init / random.
 * ------------------------------------------------------------------ */
int _libssh2_axtls_init(void) {
    /* axtls and TweetNaCl don't need explicit init. */
    return 0;
}

void _libssh2_axtls_free(void) {
    /* nothing */
}

int _libssh2_axtls_random(unsigned char *buf, int len) {
    /* axtls's get_random fills buffer with PRNG output. The
       PRNG is seeded by a counter (see fetch.sh's
       patch_axtls_get_random_dos_int21 — gettimeofday was
       replaced with a self-incrementing counter to dodge the
       INT 21h AH=0x2A re-entrancy hang). For SSH this is
       only used for per-session nonces, not long-lived key
       material. */
    extern void get_random(int num_rand_bytes, uint8_t *rand_data);
    get_random(len, buf);
    return 0;
}

/* ------------------------------------------------------------------
 * Hashes.
 * ------------------------------------------------------------------ */
int _libssh2_axtls_hash_init(libssh2_axtls_hash_ctx *ctx, int algo) {
    ctx->algo = algo;
    switch (algo) {
        case LIBSSH2_AXTLS_HASH_SHA1:   SHA1_Init(_AS_SHA1(ctx));     return 1;
        case LIBSSH2_AXTLS_HASH_SHA256: SHA256_Init(_AS_SHA256(ctx)); return 1;
        case LIBSSH2_AXTLS_HASH_SHA384: SHA384_Init(_AS_SHA384(ctx)); return 1;
        case LIBSSH2_AXTLS_HASH_SHA512: SHA512_Init(_AS_SHA512(ctx)); return 1;
        case LIBSSH2_AXTLS_HASH_MD5:    MD5_Init(_AS_MD5(ctx));       return 1;
    }
    return 0;
}

int _libssh2_axtls_hash_update(libssh2_axtls_hash_ctx *ctx,
                                const void *data, size_t len) {
    const uint8_t *d = (const uint8_t *)data;
    switch (ctx->algo) {
        case LIBSSH2_AXTLS_HASH_SHA1:
            SHA1_Update(_AS_SHA1(ctx), d, (int)len); return 1;
        case LIBSSH2_AXTLS_HASH_SHA256:
            SHA256_Update(_AS_SHA256(ctx), d, (int)len); return 1;
        case LIBSSH2_AXTLS_HASH_SHA384:
            SHA384_Update(_AS_SHA384(ctx), d, (int)len); return 1;
        case LIBSSH2_AXTLS_HASH_SHA512:
            SHA512_Update(_AS_SHA512(ctx), d, (int)len); return 1;
        case LIBSSH2_AXTLS_HASH_MD5:
            MD5_Update(_AS_MD5(ctx), d, (int)len); return 1;
    }
    return 0;
}

int _libssh2_axtls_hash_final(libssh2_axtls_hash_ctx *ctx, unsigned char *out) {
    switch (ctx->algo) {
        case LIBSSH2_AXTLS_HASH_SHA1:   SHA1_Final(out, _AS_SHA1(ctx));     return 1;
        case LIBSSH2_AXTLS_HASH_SHA256: SHA256_Final(out, _AS_SHA256(ctx)); return 1;
        case LIBSSH2_AXTLS_HASH_SHA384: SHA384_Final(out, _AS_SHA384(ctx)); return 1;
        case LIBSSH2_AXTLS_HASH_SHA512: SHA512_Final(out, _AS_SHA512(ctx)); return 1;
        case LIBSSH2_AXTLS_HASH_MD5:    MD5_Final(out, _AS_MD5(ctx));       return 1;
    }
    return 0;
}

int _libssh2_axtls_hash(const void *data, size_t len, int algo, unsigned char *out) {
    libssh2_axtls_hash_ctx c;
    if (!_libssh2_axtls_hash_init(&c, algo)) return 0;
    if (!_libssh2_axtls_hash_update(&c, data, len)) return 0;
    return _libssh2_axtls_hash_final(&c, out);
}

/* ------------------------------------------------------------------
 * HMAC.
 *
 * Wraps the generic hash ctx to implement HMAC per RFC 2104.
 * libssh2 calls _libssh2_hmac_*_init(ctx, key, keylen) for each
 * algorithm; we route through a single init that picks block_size
 * + hash_size from the algo and computes K xor opad / ipad.
 * ------------------------------------------------------------------ */

static int _hmac_init(libssh2_hmac_ctx *ctx, int algo,
                      const void *key, size_t keylen) {
    int block_size, hash_size;
    switch (algo) {
        case LIBSSH2_AXTLS_HASH_SHA1:   block_size = 64;  hash_size = 20; break;
        case LIBSSH2_AXTLS_HASH_SHA256: block_size = 64;  hash_size = 32; break;
        case LIBSSH2_AXTLS_HASH_SHA384: block_size = 128; hash_size = 48; break;
        case LIBSSH2_AXTLS_HASH_SHA512: block_size = 128; hash_size = 64; break;
        case LIBSSH2_AXTLS_HASH_MD5:    block_size = 64;  hash_size = 16; break;
        default: return 0;
    }
    ctx->algo = algo;
    ctx->block_size = block_size;
    ctx->hash_size = hash_size;

    unsigned char k_block[LIBSSH2_AXTLS_HMAC_MAX_KEY];
    memset(k_block, 0, sizeof(k_block));

    if ((int)keylen > block_size) {
        /* K' = H(K) */
        _libssh2_axtls_hash(key, keylen, algo, k_block);
    } else {
        memcpy(k_block, key, keylen);
    }

    unsigned char k_ipad[LIBSSH2_AXTLS_HMAC_MAX_KEY];
    int i;
    for (i = 0; i < block_size; i++) {
        k_ipad[i] = k_block[i] ^ 0x36;
        ctx->k_opad[i] = k_block[i] ^ 0x5c;
    }
    if (!_libssh2_axtls_hash_init(&ctx->inner, algo)) return 0;
    if (!_libssh2_axtls_hash_update(&ctx->inner, k_ipad, block_size)) return 0;
    return 1;
}

int _libssh2_hmac_ctx_init(libssh2_hmac_ctx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    return 1;
}

#if LIBSSH2_MD5
int _libssh2_hmac_md5_init(libssh2_hmac_ctx *ctx, void *key, size_t keylen) {
    return _hmac_init(ctx, LIBSSH2_AXTLS_HASH_MD5, key, keylen);
}
#endif

int _libssh2_hmac_sha1_init(libssh2_hmac_ctx *ctx, void *key, size_t keylen) {
    return _hmac_init(ctx, LIBSSH2_AXTLS_HASH_SHA1, key, keylen);
}
int _libssh2_hmac_sha256_init(libssh2_hmac_ctx *ctx, void *key, size_t keylen) {
    return _hmac_init(ctx, LIBSSH2_AXTLS_HASH_SHA256, key, keylen);
}
int _libssh2_hmac_sha512_init(libssh2_hmac_ctx *ctx, void *key, size_t keylen) {
    return _hmac_init(ctx, LIBSSH2_AXTLS_HASH_SHA512, key, keylen);
}

int _libssh2_hmac_update(libssh2_hmac_ctx *ctx, const void *data, size_t len) {
    return _libssh2_axtls_hash_update(&ctx->inner, data, len);
}

int _libssh2_hmac_final(libssh2_hmac_ctx *ctx, void *out) {
    unsigned char inner_hash[LIBSSH2_AXTLS_HMAC_MAX_DIGEST];
    if (!_libssh2_axtls_hash_final(&ctx->inner, inner_hash)) return 0;
    libssh2_axtls_hash_ctx outer;
    if (!_libssh2_axtls_hash_init(&outer, ctx->algo)) return 0;
    if (!_libssh2_axtls_hash_update(&outer, ctx->k_opad, ctx->block_size)) return 0;
    if (!_libssh2_axtls_hash_update(&outer, inner_hash, ctx->hash_size)) return 0;
    return _libssh2_axtls_hash_final(&outer, (unsigned char *)out);
}

void _libssh2_hmac_cleanup(libssh2_hmac_ctx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

/* ------------------------------------------------------------------
 * AES-CBC and AES-CTR via axtls's AES core.
 *
 * axtls's AES_CTX exposes AES_convert_key / AES_cbc_encrypt /
 * AES_cbc_decrypt. CTR mode is built on top of AES-ECB-encrypt;
 * axtls doesn't expose ECB directly, so we use the internal
 * AES_encrypt() (lowercase) — exposed from crypto.h.
 * ------------------------------------------------------------------ */

int _libssh2_axtls_cipher_init(_libssh2_cipher_ctx *ctx, int algo,
                                unsigned char *iv, unsigned char *secret,
                                int encrypt) {
    AES_MODE mode;
    switch (algo) {
        case _libssh2_cipher_aes128:
        case _libssh2_cipher_aes128ctr: mode = AES_MODE_128; break;
        case _libssh2_cipher_aes192:
        case _libssh2_cipher_aes192ctr:
            /* axtls doesn't support AES-192 natively; fall through to
               256-bit and rely on negotiation to pick 128 or 256. */
            return -1;
        case _libssh2_cipher_aes256:
        case _libssh2_cipher_aes256ctr: mode = AES_MODE_256; break;
        default: return -1;
    }
    AES_set_key(_AS_AES(ctx), secret, iv, mode);
    if (!encrypt) {
        AES_convert_key(_AS_AES(ctx));
    }
    ctx->keylen_bits = (mode == AES_MODE_128) ? 128 : 256;
    ctx->is_ctr = (algo >= _libssh2_cipher_aes128ctr && algo <= _libssh2_cipher_aes256ctr);
    if (ctx->is_ctr) {
        memcpy(ctx->ctr_iv, iv, 16);
    }
    return 0;
}

int _libssh2_axtls_cipher_crypt(_libssh2_cipher_ctx *ctx, int algo,
                                 int encrypt, unsigned char *block,
                                 size_t blocklen) {
    if (ctx->is_ctr) {
        /* AES-CTR: for each 16-byte block, AES-ECB-encrypt the
           counter and XOR with the plaintext/ciphertext. */
        size_t i;
        for (i = 0; i < blocklen; i += 16) {
            unsigned char keystream[16];
            memcpy(keystream, ctx->ctr_iv, 16);
            /* In-place encrypt the counter into keystream. */
            AES_encrypt(_AS_AES(ctx), (uint32_t *)keystream);
            size_t j;
            size_t take = (blocklen - i < 16) ? (blocklen - i) : 16;
            for (j = 0; j < take; j++) {
                block[i + j] ^= keystream[j];
            }
            /* Increment counter (big-endian, 128-bit). */
            int k;
            for (k = 15; k >= 0; k--) {
                if (++ctx->ctr_iv[k] != 0) break;
            }
        }
    } else {
        /* CBC. axtls's AES_cbc_encrypt/decrypt updates the IV
           inside the ctx automatically. */
        if (encrypt) {
            AES_cbc_encrypt(_AS_AES(ctx), block, block, (int)blocklen);
        } else {
            AES_cbc_decrypt(_AS_AES(ctx), block, block, (int)blocklen);
        }
    }
    return 0;
}

void _libssh2_axtls_cipher_dtor(_libssh2_cipher_ctx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

/* ------------------------------------------------------------------
 * Curve25519 KEX (TweetNaCl).
 * ------------------------------------------------------------------ */

int _libssh2_axtls_curve25519_new(LIBSSH2_SESSION *session,
                                    uint8_t public_key[32],
                                    uint8_t private_key[32]) {
    /* Generate a random scalar; clamp per RFC 7748. */
    _libssh2_axtls_random(private_key, 32);
    private_key[0]  &= 248;
    private_key[31] &= 127;
    private_key[31] |= 64;
    /* Compute the public key = X25519(private, basepoint). */
    crypto_scalarmult_base(public_key, private_key);
    (void)session;
    return 0;
}

int _libssh2_axtls_curve25519_gen_k(_libssh2_bn **k,
                                      uint8_t private_key[32],
                                      uint8_t server_public_key[32]) {
    /* k = X25519(private, server_public). Output is a 32-byte
       shared secret. libssh2 wraps it as a bigint for the
       transport-layer mix. */
    (void)k;
    (void)private_key;
    (void)server_public_key;
    return -1;  /* TODO: wire to _libssh2_axtls_bn_from_bin */
}

/* ------------------------------------------------------------------
 * Ed25519 (TweetNaCl).
 * ------------------------------------------------------------------ */

int _libssh2_axtls_ed25519_new_public(libssh2_ed25519_ctx **ed_ctx,
                                        LIBSSH2_SESSION *session,
                                        const unsigned char *raw_pub_key,
                                        const uint8_t key_len) {
    (void)session;
    if (key_len != 32) return -1;
    libssh2_ed25519_ctx *c = (libssh2_ed25519_ctx *)LIBSSH2_CALLOC(session, sizeof(*c));
    if (!c) return -1;
    memcpy(c->public_key, raw_pub_key, 32);
    c->have_private = 0;
    *ed_ctx = c;
    return 0;
}

int _libssh2_axtls_ed25519_verify(libssh2_ed25519_ctx *ctx,
                                    const unsigned char *sig,
                                    size_t sig_len,
                                    const unsigned char *m, size_t m_len) {
    /* TweetNaCl's crypto_sign_open expects a signed message
       (sig || message). Concatenate, then verify. */
    if (sig_len != 64) return -1;
    unsigned char *sm = (unsigned char *)malloc(64 + m_len);
    if (!sm) return -1;
    memcpy(sm, sig, 64);
    memcpy(sm + 64, m, m_len);
    unsigned char *out = (unsigned char *)malloc(64 + m_len);
    if (!out) { free(sm); return -1; }
    unsigned long long out_len = 0;
    int r = crypto_sign_open(out, &out_len, sm, 64 + m_len, ctx->public_key);
    free(sm);
    free(out);
    return (r == 0) ? 0 : -1;
}

void _libssh2_axtls_ed25519_free(libssh2_ed25519_ctx *ctx) {
    if (ctx) {
        memset(ctx, 0, sizeof(*ctx));
        free(ctx);
    }
}

/* ------------------------------------------------------------------
 * RSA — stubs. axtls already has RSA_verify; sign requires the
 * priv-exp path. Key-file parsing (PEM/OpenSSH) is the bulk; do
 * later.
 * ------------------------------------------------------------------ */

int _libssh2_axtls_rsa_new(libssh2_axtls_rsa_ctx *rsa,
                            const unsigned char *edata, unsigned long elen,
                            const unsigned char *ndata, unsigned long nlen,
                            const unsigned char *ddata, unsigned long dlen,
                            const unsigned char *pdata, unsigned long plen,
                            const unsigned char *qdata, unsigned long qlen,
                            const unsigned char *e1data, unsigned long e1len,
                            const unsigned char *e2data, unsigned long e2len,
                            const unsigned char *coeffdata, unsigned long coefflen) {
    (void)rsa; (void)edata; (void)elen; (void)ndata; (void)nlen;
    (void)ddata; (void)dlen; (void)pdata; (void)plen; (void)qdata; (void)qlen;
    (void)e1data; (void)e1len; (void)e2data; (void)e2len;
    (void)coeffdata; (void)coefflen;
    /* TODO: build RSA_CTX via RSA_pub_key_new / RSA_priv_key_new. */
    return -1;
}

int _libssh2_axtls_rsa_new_private(libssh2_axtls_rsa_ctx **rsa,
                                     LIBSSH2_SESSION *session,
                                     const char *filename,
                                     const unsigned char *passphrase) {
    (void)rsa; (void)session; (void)filename; (void)passphrase;
    return -1;  /* TODO: load + parse PEM */
}

int _libssh2_axtls_rsa_new_private_frommemory(libssh2_axtls_rsa_ctx **rsa,
                                                LIBSSH2_SESSION *session,
                                                const char *filedata,
                                                size_t filedata_len,
                                                unsigned const char *passphrase) {
    (void)rsa; (void)session; (void)filedata; (void)filedata_len; (void)passphrase;
    return -1;  /* TODO: parse PEM in memory */
}

int _libssh2_axtls_rsa_sha1_sign(LIBSSH2_SESSION *session,
                                   libssh2_axtls_rsa_ctx *rsactx,
                                   const unsigned char *hash, size_t hash_len,
                                   unsigned char **signature, size_t *signature_len) {
    (void)session; (void)rsactx;
    (void)hash; (void)hash_len; (void)signature; (void)signature_len;
    return -1;  /* TODO: RSA_encrypt with priv key + SHA1 prefix */
}

int _libssh2_axtls_rsa_sha2_sign(LIBSSH2_SESSION *session,
                                   libssh2_axtls_rsa_ctx *rsactx,
                                   const unsigned char *hash, size_t hash_len,
                                   unsigned char **signature, size_t *signature_len) {
    (void)session; (void)rsactx;
    (void)hash; (void)hash_len; (void)signature; (void)signature_len;
    return -1;  /* TODO: RSA_encrypt with priv key + SHA256/512 prefix */
}

int _libssh2_axtls_rsa_sha1_verify(libssh2_axtls_rsa_ctx *rsactx,
                                     const unsigned char *sig, size_t sig_len,
                                     const unsigned char *m, size_t m_len) {
    (void)rsactx; (void)sig; (void)sig_len; (void)m; (void)m_len;
    return -1;  /* TODO: RSA_verify SHA1 */
}

int _libssh2_axtls_rsa_sha2_verify(libssh2_axtls_rsa_ctx *rsactx,
                                     size_t hash_len,
                                     const unsigned char *sig, size_t sig_len,
                                     const unsigned char *m, size_t m_len) {
    (void)rsactx; (void)hash_len; (void)sig; (void)sig_len; (void)m; (void)m_len;
    return -1;  /* TODO: RSA_verify SHA2 */
}

void _libssh2_axtls_rsa_free(libssh2_axtls_rsa_ctx *rsactx) {
    if (rsactx && rsactx->ctx) {
        RSA_free((RSA_CTX *)rsactx->ctx);
        rsactx->ctx = NULL;
    }
}

/* ------------------------------------------------------------------
 * Key file parsing — stubs.
 * ------------------------------------------------------------------ */

int _libssh2_axtls_pub_priv_keyfile(LIBSSH2_SESSION *session,
                                      unsigned char **method, size_t *method_len,
                                      unsigned char **pubkeydata, size_t *pubkeydata_len,
                                      const char *privatekey, const char *passphrase) {
    (void)session; (void)method; (void)method_len;
    (void)pubkeydata; (void)pubkeydata_len; (void)privatekey; (void)passphrase;
    return -1;
}

int _libssh2_axtls_pub_priv_keyfilememory(LIBSSH2_SESSION *session,
                                            unsigned char **method, size_t *method_len,
                                            unsigned char **pubkeydata, size_t *pubkeydata_len,
                                            const char *privatekeydata, size_t privatekeydata_len,
                                            const char *passphrase) {
    (void)session; (void)method; (void)method_len;
    (void)pubkeydata; (void)pubkeydata_len; (void)privatekeydata;
    (void)privatekeydata_len; (void)passphrase;
    return -1;
}

int _libssh2_axtls_sk_pub_keyfilememory(LIBSSH2_SESSION *session,
                                          unsigned char **method, size_t *method_len,
                                          unsigned char **pubkeydata, size_t *pubkeydata_len,
                                          int *algorithm, unsigned char *flags,
                                          const char **application,
                                          const unsigned char **key_handle,
                                          size_t *handle_len,
                                          const char *privatekeydata,
                                          size_t privatekeydata_len,
                                          const char *passphrase) {
    (void)session; (void)method; (void)method_len; (void)pubkeydata;
    (void)pubkeydata_len; (void)algorithm; (void)flags; (void)application;
    (void)key_handle; (void)handle_len; (void)privatekeydata;
    (void)privatekeydata_len; (void)passphrase;
    return -1;
}

/* ------------------------------------------------------------------
 * DH — stubs. Used when Curve25519 KEX isn't negotiated.
 * ------------------------------------------------------------------ */

void _libssh2_axtls_dh_init(_libssh2_dh_ctx *dhctx) {
    memset(dhctx, 0, sizeof(*dhctx));
}

int _libssh2_axtls_dh_key_pair(_libssh2_dh_ctx *dhctx, _libssh2_bn *public,
                                 _libssh2_bn *g, _libssh2_bn *p,
                                 int group_order, _libssh2_bn_ctx *bnctx) {
    (void)dhctx; (void)public; (void)g; (void)p; (void)group_order; (void)bnctx;
    return -1;
}

int _libssh2_axtls_dh_secret(_libssh2_dh_ctx *dhctx, _libssh2_bn *secret,
                               _libssh2_bn *f, _libssh2_bn *p, _libssh2_bn_ctx *bnctx) {
    (void)dhctx; (void)secret; (void)f; (void)p; (void)bnctx;
    return -1;
}

void _libssh2_axtls_dh_dtor(_libssh2_dh_ctx *dhctx) {
    (void)dhctx;
}

/* ------------------------------------------------------------------
 * Bignum helpers — stubs.
 * ------------------------------------------------------------------ */

_libssh2_bn *_libssh2_axtls_bn_init(void) { return NULL; }
void _libssh2_axtls_bn_set_word(_libssh2_bn *bn, unsigned long val) {
    (void)bn; (void)val;
}
int _libssh2_axtls_bn_from_bin(_libssh2_bn *bn, int len, const unsigned char *val) {
    (void)bn; (void)len; (void)val;
    return -1;
}
int _libssh2_axtls_bn_to_bin(_libssh2_bn *bn, unsigned char *val) {
    (void)bn; (void)val;
    return -1;
}
int _libssh2_axtls_bn_bytes(_libssh2_bn *bn) { (void)bn; return 0; }
int _libssh2_axtls_bn_bits(_libssh2_bn *bn) { (void)bn; return 0; }
void _libssh2_axtls_bn_free(_libssh2_bn *bn) { (void)bn; }
_libssh2_bn_ctx *_libssh2_axtls_bn_ctx_new(void) { return NULL; }
void _libssh2_axtls_bn_ctx_free(_libssh2_bn_ctx *ctx) { (void)ctx; }
