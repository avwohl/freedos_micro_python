/* Minimal SHA-256 — uc386-friendly C. Matches axtls os_port.h's
 * sha256_init/update/final API and CRYAL_SHA256_CTX struct layout. */
#include <stdlib.h>
#include <string.h>

#ifndef BYTE
typedef unsigned char BYTE;
typedef unsigned int  WORD;
#endif

#ifndef SHA256_BLOCK_SIZE
#define SHA256_BLOCK_SIZE 32
typedef struct {
    BYTE data[64];
    WORD datalen;
    unsigned long long bitlen;
    WORD state[8];
} CRYAL_SHA256_CTX;
#endif

static const WORD K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u,
};

static WORD rotr(WORD x, WORD n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(CRYAL_SHA256_CTX *ctx, const BYTE *d) {
    WORD m[64];
    int i;
    for (i = 0; i < 16; ++i) {
        int j = i * 4;
        m[i] = ((WORD)d[j] << 24) | ((WORD)d[j+1] << 16)
             | ((WORD)d[j+2] << 8) | (WORD)d[j+3];
    }
    for (i = 16; i < 64; ++i) {
        WORD s0 = rotr(m[i-15], 7) ^ rotr(m[i-15], 18) ^ (m[i-15] >> 3);
        WORD s1 = rotr(m[i-2], 17) ^ rotr(m[i-2], 19) ^ (m[i-2]  >> 10);
        m[i] = m[i-16] + s0 + m[i-7] + s1;
    }
    WORD a = ctx->state[0], b = ctx->state[1], c = ctx->state[2];
    WORD d2 = ctx->state[3], e2 = ctx->state[4];
    WORD f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (i = 0; i < 64; ++i) {
        WORD S1 = rotr(e2, 6) ^ rotr(e2, 11) ^ rotr(e2, 25);
        WORD ch = (e2 & f) ^ ((~e2) & g);
        WORD S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        WORD mj = (a & b) ^ (a & c) ^ (b & c);
        WORD t1 = h + S1 + ch + K[i] + m[i];
        WORD t2 = S0 + mj;
        h = g; g = f; f = e2; e2 = d2 + t1;
        d2 = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d2; ctx->state[4] += e2;
    ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(CRYAL_SHA256_CTX *ctx) {
    ctx->datalen = 0; ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
}

void sha256_update(CRYAL_SHA256_CTX *ctx, const BYTE *data, size_t len) {
    size_t i;
    for (i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

void sha256_final(CRYAL_SHA256_CTX *ctx, BYTE *hash) {
    WORD i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += (unsigned long long)ctx->datalen * 8ULL;
    ctx->data[63] = (BYTE)(ctx->bitlen);
    ctx->data[62] = (BYTE)(ctx->bitlen >> 8);
    ctx->data[61] = (BYTE)(ctx->bitlen >> 16);
    ctx->data[60] = (BYTE)(ctx->bitlen >> 24);
    ctx->data[59] = (BYTE)(ctx->bitlen >> 32);
    ctx->data[58] = (BYTE)(ctx->bitlen >> 40);
    ctx->data[57] = (BYTE)(ctx->bitlen >> 48);
    ctx->data[56] = (BYTE)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);
    for (i = 0; i < 4; ++i) {
        hash[i]      = (BYTE)((ctx->state[0] >> (24 - i * 8)) & 0xFF);
        hash[i + 4]  = (BYTE)((ctx->state[1] >> (24 - i * 8)) & 0xFF);
        hash[i + 8]  = (BYTE)((ctx->state[2] >> (24 - i * 8)) & 0xFF);
        hash[i + 12] = (BYTE)((ctx->state[3] >> (24 - i * 8)) & 0xFF);
        hash[i + 16] = (BYTE)((ctx->state[4] >> (24 - i * 8)) & 0xFF);
        hash[i + 20] = (BYTE)((ctx->state[5] >> (24 - i * 8)) & 0xFF);
        hash[i + 24] = (BYTE)((ctx->state[6] >> (24 - i * 8)) & 0xFF);
        hash[i + 28] = (BYTE)((ctx->state[7] >> (24 - i * 8)) & 0xFF);
    }
}
