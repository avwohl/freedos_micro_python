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
 * Linkage stubs for chachapoly_*.
 *
 * libssh2's crypt.c references these three for chacha20-poly1305@
 * openssh.com cipher dispatch even though build_port.sh skips
 * cipher-chachapoly.c / chacha.c / poly1305.c (uc386 doesn't
 * compile the chacha sources cleanly).  modssh_uc386dos.c restricts
 * the cipher preferences to AES-CTR variants so chachapoly_*
 * never actually runs.  Stubs make the link resolve.
 * ------------------------------------------------------------------ */
struct chachapoly_ctx;  /* opaque — we never touch the bytes */

int chachapoly_init(struct chachapoly_ctx *cpctx,
                     const unsigned char *key, unsigned int keylen) {
    (void)cpctx; (void)key; (void)keylen;
    return -1;
}

int chachapoly_crypt(struct chachapoly_ctx *cpctx, unsigned int seqnr,
                     unsigned char *dest, const unsigned char *src,
                     unsigned int len, unsigned int aadlen,
                     int do_encrypt) {
    (void)cpctx; (void)seqnr; (void)dest; (void)src;
    (void)len; (void)aadlen; (void)do_encrypt;
    return -1;
}

int chachapoly_get_length(struct chachapoly_ctx *cpctx,
                          unsigned int *plenp, unsigned int seqnr,
                          const unsigned char *cp, unsigned int len) {
    (void)cpctx; (void)plenp; (void)seqnr; (void)cp; (void)len;
    return -1;
}

/* ------------------------------------------------------------------
 * Linkage stubs for libc difftime / select.
 *
 * uc386's libc declares these (time.h / sys/select.h) but doesn't
 * ship implementations — they're typically in glibc's networking
 * layer rather than the core libc, which uc386 doesn't model.
 * libssh2's session.c references both:
 *   - `difftime(now, start_time)` for elapsed-ms calculation in
 *     wait-on-poll loops (we use blocking I/O via callbacks; the
 *     wait paths never run, but the symbol still gets emitted).
 *   - `select((int)(fd+1), readfd, writefd, NULL, &tv)` for the
 *     wait-for-readable / wait-for-writable paths inside
 *     libssh2_session_block_directions; again, not reached when
 *     callbacks are blocking, but still linked.
 * ------------------------------------------------------------------ */

#include <time.h>
#include <sys/select.h>

double difftime(time_t time1, time_t time0) {
    /* Simple seconds-difference. axtls's time_t is `long` on this
     * port; subtraction fits a double for any reasonable elapsed
     * window. */
    return (double)(time1 - time0);
}

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout) {
    (void)nfds; (void)readfds; (void)writefds; (void)exceptfds;
    (void)timeout;
    /* libssh2's callback-based I/O path never reaches here. */
    return -1;
}

/* ------------------------------------------------------------------
 * Linkage stubs for POSIX recv/send.
 *
 * libssh2's misc.c provides default _libssh2_recv / _libssh2_send
 * that wrap POSIX recv() and send(). The MP port replaces these via
 * LIBSSH2_CALLBACK_SEND / LIBSSH2_CALLBACK_RECV (see
 * port/modssh_uc386dos.c), so the wrappers are never actually
 * called at runtime — but they're still referenced from libssh2's
 * object code, so the symbols must resolve at link time. The uc386
 * libc doesn't ship socket-API recv/send (those would normally
 * come from libnsl / glibc's BSD-sockets layer), so we provide
 * inert stubs here.
 * ------------------------------------------------------------------ */
ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    (void)sockfd; (void)buf; (void)len; (void)flags;
    return -1;
}
ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    (void)sockfd; (void)buf; (void)len; (void)flags;
    return -1;
}

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
    {
        extern int write(int fd, const void *buf, unsigned int n);
        write(1, "[hash:bad-algo]", 15);
    }
    return 0;
}

int _libssh2_axtls_hash_update(libssh2_axtls_hash_ctx *ctx,
                                const void *data, size_t len) {
    /* Dump per-update size + first byte for hash-input bisection
     * against paramiko's hm.asbytes(). Only enable for the
     * exchange-hash SHA256 (algo=SHA256) to avoid noise from
     * other hashes. */
    if (ctx->algo == LIBSSH2_AXTLS_HASH_SHA256) {
        extern int write(int fd, const void *buf, unsigned int n);
        static const char _hx[] = "0123456789abcdef";
        char _b[16] = "[hu:";
        unsigned int l = (unsigned int)len;
        _b[4]  = _hx[(l >> 12) & 0xF];
        _b[5]  = _hx[(l >> 8) & 0xF];
        _b[6]  = _hx[(l >> 4) & 0xF];
        _b[7]  = _hx[l & 0xF];
        _b[8]  = ':';
        if (len > 0) {
            const uint8_t *dd = (const uint8_t *)data;
            _b[9]  = _hx[(dd[0] >> 4) & 0xF];
            _b[10] = _hx[dd[0] & 0xF];
        } else {
            _b[9] = '-'; _b[10] = '-';
        }
        _b[11] = ']';
        write(1, _b, 12);
    }
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

/* Note: libssh2's _libssh2_curve25519_new takes pointer-to-pointer
 * (the openssl backend's contract); we allocate the buffers and
 * store them at *out_*. Earlier signature took `uint8_t pk[32]`
 * which silently received `unsigned char **`, and our scalarmult
 * scribbled 32 bytes across the caller's struct instead of into
 * a real buffer — symptom was an all-zero pubkey on the wire and
 * "Error computing shared key" from the peer. */
int _libssh2_axtls_curve25519_new(LIBSSH2_SESSION *session,
                                    unsigned char **out_public_key,
                                    unsigned char **out_private_key) {
    extern int write(int fd, const void *buf, unsigned int n);
    write(1, "[cv:nE]", 7);
    unsigned char *priv = (unsigned char *)LIBSSH2_ALLOC(session, 32);
    unsigned char *pub  = (unsigned char *)LIBSSH2_ALLOC(session, 32);
    if (!priv || !pub) {
        if (priv) LIBSSH2_FREE(session, priv);
        if (pub)  LIBSSH2_FREE(session, pub);
        return -1;
    }
    /* Generate a random scalar; clamp per RFC 7748. */
    _libssh2_axtls_random(priv, 32);
    priv[0]  &= 248;
    priv[31] &= 127;
    priv[31] |= 64;
    /* Compute the public key = X25519(private, basepoint). */
    crypto_scalarmult_base(pub, priv);
    write(1, "[cv:nM]", 7);
    *out_public_key  = pub;
    *out_private_key = priv;
    return 0;
}

int _libssh2_axtls_curve25519_gen_k(_libssh2_bn **k,
                                      uint8_t private_key[32],
                                      uint8_t server_public_key[32]) {
    extern int write(int fd, const void *buf, unsigned int n);
    write(1, "[cv:kE]", 7);
    if (!k || !*k) return -1;
    uint8_t secret[32];
    if (crypto_scalarmult(secret, private_key, server_public_key) != 0) {
        write(1, "[cv:kFail]", 10);
        return -1;
    }
    /* Dump first 8 bytes of K. Compare with paramiko's debug log
     * (set LOG_LEVEL=DEBUG on the server; paramiko logs the K
     * value in its kex_curve25519 module). If they differ, our
     * X25519 has miscomputed the shared secret. */
    static const char _hx[] = "0123456789abcdef";
    char _b[24] = "[K:";
    int _i;
    for (_i = 0; _i < 8; _i++) {
        _b[3 + 2*_i]     = _hx[(secret[_i] >> 4) & 0xF];
        _b[3 + 2*_i + 1] = _hx[secret[_i] & 0xF];
    }
    _b[19] = ']';
    write(1, _b, 20);
    write(1, "[cv:kM]", 7);
    int rc = _libssh2_axtls_bn_from_bin(*k, 32, secret);
    write(1, "[cv:kB]", 7);
    return rc;
}

/* ------------------------------------------------------------------
 * Ed25519 (TweetNaCl).
 * ------------------------------------------------------------------ */

int _libssh2_axtls_ed25519_new_public(libssh2_ed25519_ctx **ed_ctx,
                                        LIBSSH2_SESSION *session,
                                        const unsigned char *raw_pub_key,
                                        const uint8_t key_len) {
    extern int write(int fd, const void *buf, unsigned int n);
    write(1, "[ed:nP]", 7);
    (void)session;
    if (key_len != 32) {
        write(1, "[ed:nPlen-bad]", 14);
        return -1;
    }
    libssh2_ed25519_ctx *c = (libssh2_ed25519_ctx *)LIBSSH2_CALLOC(session, sizeof(*c));
    if (!c) {
        write(1, "[ed:nPalloc-fail]", 17);
        return -1;
    }
    memcpy(c->public_key, raw_pub_key, 32);
    c->have_private = 0;
    *ed_ctx = c;
    write(1, "[ed:nPok]", 9);
    return 0;
}

int _libssh2_axtls_ed25519_verify(libssh2_ed25519_ctx *ctx,
                                    const unsigned char *sig,
                                    size_t sig_len,
                                    const unsigned char *m, size_t m_len) {
    extern int write(int fd, const void *buf, unsigned int n);
    write(1, "[ed:vE]", 7);
    /* Dump first 8 bytes of m (exchange hash H) for client/server
     * compare. */
    static const char _hx[] = "0123456789abcdef";
    char _hb[24] = "[H:";
    int _i;
    for (_i = 0; _i < 8 && _i < (int)m_len; _i++) {
        _hb[3 + 2*_i]     = _hx[(m[_i] >> 4) & 0xF];
        _hb[3 + 2*_i + 1] = _hx[m[_i] & 0xF];
    }
    _hb[19] = ']';
    write(1, _hb, 20);
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

/* Ed25519 signing stubs. We're an SSH client and never present an
 * Ed25519 user identity — host-key verify (above) is the only side
 * we exercise. libssh2's hostkey.c still references the sign /
 * load-private symbols even for client builds; provide stubs that
 * fail cleanly. */
int _libssh2_axtls_ed25519_new_private(libssh2_ed25519_ctx **ed_ctx,
                                         struct _LIBSSH2_SESSION *session,
                                         const char *filename,
                                         const uint8_t *passphrase) {
    (void)ed_ctx; (void)session; (void)filename; (void)passphrase;
    return -1;
}

int _libssh2_axtls_ed25519_new_private_frommemory(
        libssh2_ed25519_ctx **ed_ctx,
        struct _LIBSSH2_SESSION *session,
        const char *filedata, size_t filedata_len,
        unsigned const char *passphrase) {
    (void)ed_ctx; (void)session; (void)filedata; (void)filedata_len; (void)passphrase;
    return -1;
}

int _libssh2_axtls_ed25519_sign(libssh2_ed25519_ctx *ctx,
                                  struct _LIBSSH2_SESSION *session,
                                  uint8_t **out_sig, size_t *out_sig_len,
                                  const uint8_t *message, size_t message_len) {
    (void)ctx; (void)session; (void)out_sig; (void)out_sig_len;
    (void)message; (void)message_len;
    return -1;
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
 * Bignum — byte-array implementation.
 *
 * libssh2 uses _libssh2_bn only for (a) loading raw bytes in, (b)
 * reading them back out, and (c) querying byte/bit length for SSH
 * mpint encoding.  It does NOT do arithmetic on bn objects when our
 * KEX is Curve25519 (the math is all inside the tweetnacl scalarmult).
 * A trivial big-endian byte buffer covers the entire use surface for
 * that path; full arithmetic would only be needed for diffie-hellman-
 * group* KEX, which the SSH client config can skip via algorithm
 * preferences.
 *
 * Stored without leading zero bytes so `bytes` / `bits` mirror what
 * BN_num_bytes / BN_num_bits return in the OpenSSL backend (which
 * libssh2's kex.c reads to decide whether to prepend an mpint 0x00).
 * ------------------------------------------------------------------ */

typedef struct {
    int len;                 /* bytes stored (no leading zeros) */
    int cap;                 /* allocated capacity of data       */
    unsigned char *data;     /* big-endian payload               */
} _bn_impl;

_libssh2_bn *_libssh2_axtls_bn_init(void) {
    _bn_impl *bn = (_bn_impl *)calloc(1, sizeof(*bn));
    return (_libssh2_bn *)bn;
}

void _libssh2_axtls_bn_free(_libssh2_bn *bn) {
    if (!bn) return;
    _bn_impl *b = (_bn_impl *)bn;
    if (b->data) free(b->data);
    free(b);
}

static int _bn_ensure(_bn_impl *b, int need) {
    if (b->cap >= need) return 0;
    int newcap = need > 0 ? need : 1;
    unsigned char *nd = (unsigned char *)malloc(newcap);
    if (!nd) return -1;
    if (b->data) free(b->data);
    b->data = nd;
    b->cap = newcap;
    return 0;
}

void _libssh2_axtls_bn_set_word(_libssh2_bn *bn, unsigned long val) {
    _bn_impl *b = (_bn_impl *)bn;
    if (!b) return;
    /* Encode as big-endian, strip leading zeros. */
    unsigned char tmp[sizeof(val)];
    int i, n = 0;
    for (i = (int)sizeof(val) - 1; i >= 0; i--) {
        unsigned char byte = (unsigned char)((val >> (i * 8)) & 0xFF);
        if (n == 0 && byte == 0) continue;
        tmp[n++] = byte;
    }
    if (_bn_ensure(b, n) < 0) {
        b->len = 0;
        return;
    }
    if (n > 0) memcpy(b->data, tmp, n);
    b->len = n;
}

int _libssh2_axtls_bn_from_bin(_libssh2_bn *bn, int len, const unsigned char *val) {
    _bn_impl *b = (_bn_impl *)bn;
    if (!b || len < 0) return -1;
    /* Strip leading zeros so `bits` reports the true MSB position
       and `bytes` matches OpenSSL's BN_num_bytes. */
    while (len > 0 && val[0] == 0) {
        val++;
        len--;
    }
    if (_bn_ensure(b, len) < 0) return -1;
    if (len > 0) memcpy(b->data, val, (size_t)len);
    b->len = len;
    return 0;
}

int _libssh2_axtls_bn_to_bin(_libssh2_bn *bn, unsigned char *val) {
    _bn_impl *b = (_bn_impl *)bn;
    if (!b) return 0;
    if (b->len > 0 && val) memcpy(val, b->data, (size_t)b->len);
    return b->len;
}

int _libssh2_axtls_bn_bytes(_libssh2_bn *bn) {
    _bn_impl *b = (_bn_impl *)bn;
    return b ? b->len : 0;
}

int _libssh2_axtls_bn_bits(_libssh2_bn *bn) {
    _bn_impl *b = (_bn_impl *)bn;
    if (!b || b->len == 0) return 0;
    int n_bits = 8 * (b->len - 1);
    unsigned char msb = b->data[0];
    while (msb) {
        n_bits++;
        msb >>= 1;
    }
    return n_bits;
}

_libssh2_bn_ctx *_libssh2_axtls_bn_ctx_new(void) {
    /* No per-call state needed for our byte-array bn. */
    return NULL;
}

void _libssh2_axtls_bn_ctx_free(_libssh2_bn_ctx *ctx) {
    (void)ctx;
}
