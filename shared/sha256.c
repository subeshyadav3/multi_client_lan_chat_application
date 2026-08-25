#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "sha256.h"

/* =====================================================================
 * SHA-256 password hashing (implemented from scratch -- no libcrypto)
 *
 * SHA-256 turns any input string into a fixed 32-byte (256-bit) digest.
 * We store and compare password hashes instead of plain text. The hash
 * is computed in 64-byte blocks; each block goes through 64 rounds of
 * mixing using the constants below.
 * ===================================================================== */

/* The working state of the hash while bytes are being fed in. */
typedef struct {
    uint32_t state[8];  /* the 8 running 32-bit words (the hash so far) */
    uint64_t bitlen;    /* total number of BITS processed so far        */
    uint8_t  data[64];  /* current 64-byte block being filled           */
    size_t   datalen;   /* how many bytes of data[] are used so far     */
} SHA256_CTX;

/* The 64 fixed round constants (part of the SHA-256 standard). */
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

/* Rotate the 32-bit value x right by n positions. */
static uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

/* Compress ONE 64-byte block: this is the heart of SHA-256. It mixes
 * the block into the running 8-word state using 64 rounds.       */
static void sha256_transform(SHA256_CTX *ctx, const uint8_t data[64]) {
    uint32_t w[64];

    /* First 16 words: take the block 4 bytes at a time (big-endian). */
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) | (uint32_t)data[i * 4 + 3];
    }
    /* Remaining 48 words: derived from the first 16 (the message schedule). */
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    /* Load the current 8-word state into local variables a..h. */
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    /* 64 mixing rounds combining the schedule with the round constants. */
    for (int i = 0; i < 64; i++) {
        uint32_t S1  = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch  = (e & f) ^ (~e & g);
        uint32_t t1  = h + S1 + ch + K[i] + w[i];
        uint32_t S0  = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2  = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }

    /* Add the results back into the running state. */
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

/* Start a fresh hash: zero the counters and set the 8 initial constants
 * (these fixed values are part of the SHA-256 standard).          */
static void sha256_init(SHA256_CTX *ctx) {
    ctx->datalen = 0;
    ctx->bitlen  = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

/* Feed bytes into the hash one at a time. When the 64-byte block fills
 * up, compress it and start a fresh block.                       */
static void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {          /* block is full: compress it */
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;            /* 64 bytes = 512 bits */
            ctx->datalen = 0;
        }
    }
}

/* Finish the hash: add the standard padding (a 0x80 byte, then zeros,
 * then the 64-bit length) and write out the final 32 raw digest bytes. */
static void sha256_final(SHA256_CTX *ctx, uint8_t hash[32]) {
    uint64_t bitlen = ctx->bitlen + (uint64_t)ctx->datalen * 8;
    uint8_t pad = 0x80, zero = 0;
    sha256_update(ctx, &pad, 1);
    while (ctx->datalen != 56) sha256_update(ctx, &zero, 1);   /* pad to 56 bytes */
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++) lenbuf[i] = (uint8_t)(bitlen >> (56 - i * 8)); /* big-endian 64-bit length */
    sha256_update(ctx, lenbuf, 8);
    /* Write each 32-bit state word out as 4 bytes (big-endian). */
    for (int i = 0; i < 8; i++) {
        hash[i * 4 + 0] = (uint8_t)(ctx->state[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

/* Public entry point: hash a string and format the result as a
 * 64-character lowercase hex string (used to store/verify passwords). */
void sha256_hex(const char *in, char out[65]) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)in, strlen(in));
    uint8_t h[32];
    sha256_final(&ctx, h);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", h[i]);
    out[64] = '\0';
}
