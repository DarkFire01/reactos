/*
 * kdnet_crypto_prim.c
 *
 * Portable, standards-exact C crypto primitives for KDNET.
 * See kdnet_crypto_prim.h for the public interface and design constraints.
 *
 * Freestanding: implements its own tiny memcpy/memset/memcmp-equivalents,
 * no libc, no dynamic allocation, no SSE intrinsics. Pure portable C.
 *
 * All multi-byte values are processed in big-endian wire order (SHA-256,
 * AES state) per FIPS-180-4 / FIPS-197, so the output is byte-exact
 * regardless of host endianness.
 */

#include "kdnet_crypto_prim.h"

/* ------------------------------------------------------------------ */
/* Fixed-width integer typedefs (self-provided, no libc).             */
/* ------------------------------------------------------------------ */

typedef unsigned char  kd_u8;
typedef unsigned int   kd_u32;   /* >= 32 bits on every MSVC/clang target */

#if defined(_MSC_VER)
typedef unsigned __int64 kd_u64;
#else
typedef unsigned long long kd_u64;
#endif

/* ------------------------------------------------------------------ */
/* Tiny freestanding mem helpers (no libc).                          */
/* ------------------------------------------------------------------ */

static void kd_memcpy(void *dst, const void *src, kd_u32 n)
{
    kd_u8 *d = (kd_u8 *)dst;
    const kd_u8 *s = (const kd_u8 *)src;
    kd_u32 i;
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static void kd_memset(void *dst, kd_u8 v, kd_u32 n)
{
    kd_u8 *d = (kd_u8 *)dst;
    kd_u32 i;
    for (i = 0; i < n; i++) {
        d[i] = v;
    }
}

/* ------------------------------------------------------------------ */
/* Big-endian 32-bit load/store helpers.                             */
/* ------------------------------------------------------------------ */

static kd_u32 kd_load_be32(const kd_u8 *p)
{
    return ((kd_u32)p[0] << 24) |
           ((kd_u32)p[1] << 16) |
           ((kd_u32)p[2] <<  8) |
           ((kd_u32)p[3]);
}

static void kd_store_be32(kd_u8 *p, kd_u32 v)
{
    p[0] = (kd_u8)(v >> 24);
    p[1] = (kd_u8)(v >> 16);
    p[2] = (kd_u8)(v >>  8);
    p[3] = (kd_u8)(v);
}

/* ================================================================== */
/* SHA-256 (FIPS-180-4)                                               */
/* ================================================================== */

#define KD_ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const kd_u32 kd_sha256_k[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

typedef struct _KD_SHA256_CTX {
    kd_u32 h[8];
    kd_u64 length;        /* total message length in bytes */
    kd_u8  block[64];
    kd_u32 blockLen;      /* bytes currently buffered in block[] */
} KD_SHA256_CTX;

static void kd_sha256_init(KD_SHA256_CTX *ctx)
{
    ctx->h[0] = 0x6a09e667UL;
    ctx->h[1] = 0xbb67ae85UL;
    ctx->h[2] = 0x3c6ef372UL;
    ctx->h[3] = 0xa54ff53aUL;
    ctx->h[4] = 0x510e527fUL;
    ctx->h[5] = 0x9b05688cUL;
    ctx->h[6] = 0x1f83d9abUL;
    ctx->h[7] = 0x5be0cd19UL;
    ctx->length = 0;
    ctx->blockLen = 0;
}

static void kd_sha256_transform(KD_SHA256_CTX *ctx, const kd_u8 *p)
{
    kd_u32 w[64];
    kd_u32 a, b, c, d, e, f, g, h;
    kd_u32 i, t1, t2;

    for (i = 0; i < 16; i++) {
        w[i] = kd_load_be32(p + i * 4);
    }
    for (i = 16; i < 64; i++) {
        kd_u32 s0 = KD_ROTR32(w[i - 15], 7) ^ KD_ROTR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        kd_u32 s1 = KD_ROTR32(w[i - 2], 17) ^ KD_ROTR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
    e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];

    for (i = 0; i < 64; i++) {
        kd_u32 S1 = KD_ROTR32(e, 6) ^ KD_ROTR32(e, 11) ^ KD_ROTR32(e, 25);
        kd_u32 ch = (e & f) ^ ((~e) & g);
        t1 = h + S1 + ch + kd_sha256_k[i] + w[i];
        {
            kd_u32 S0 = KD_ROTR32(a, 2) ^ KD_ROTR32(a, 13) ^ KD_ROTR32(a, 22);
            kd_u32 maj = (a & b) ^ (a & c) ^ (b & c);
            t2 = S0 + maj;
        }
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

static void kd_sha256_update(KD_SHA256_CTX *ctx, const kd_u8 *data, kd_u32 len)
{
    kd_u32 i = 0;

    ctx->length += len;

    /* Fill an existing partial block first. */
    if (ctx->blockLen != 0) {
        while (i < len && ctx->blockLen < 64) {
            ctx->block[ctx->blockLen++] = data[i++];
        }
        if (ctx->blockLen == 64) {
            kd_sha256_transform(ctx, ctx->block);
            ctx->blockLen = 0;
        }
    }

    /* Process full blocks directly from input. */
    while (i + 64 <= len) {
        kd_sha256_transform(ctx, data + i);
        i += 64;
    }

    /* Buffer the remainder. */
    while (i < len) {
        ctx->block[ctx->blockLen++] = data[i++];
    }
}

static void kd_sha256_final(KD_SHA256_CTX *ctx, kd_u8 digest[32])
{
    kd_u64 bitLen = ctx->length * (kd_u64)8;
    kd_u32 i;

    /* Append 0x80 then pad with zeros until 56 mod 64. */
    ctx->block[ctx->blockLen++] = 0x80;
    if (ctx->blockLen > 56) {
        while (ctx->blockLen < 64) {
            ctx->block[ctx->blockLen++] = 0;
        }
        kd_sha256_transform(ctx, ctx->block);
        ctx->blockLen = 0;
    }
    while (ctx->blockLen < 56) {
        ctx->block[ctx->blockLen++] = 0;
    }

    /* Append 64-bit big-endian bit length. */
    ctx->block[56] = (kd_u8)(bitLen >> 56);
    ctx->block[57] = (kd_u8)(bitLen >> 48);
    ctx->block[58] = (kd_u8)(bitLen >> 40);
    ctx->block[59] = (kd_u8)(bitLen >> 32);
    ctx->block[60] = (kd_u8)(bitLen >> 24);
    ctx->block[61] = (kd_u8)(bitLen >> 16);
    ctx->block[62] = (kd_u8)(bitLen >>  8);
    ctx->block[63] = (kd_u8)(bitLen);
    kd_sha256_transform(ctx, ctx->block);

    for (i = 0; i < 8; i++) {
        kd_store_be32(digest + i * 4, ctx->h[i]);
    }
}

void KdNetSha256(const unsigned char *Data, unsigned int Length, unsigned char Digest[32])
{
    KD_SHA256_CTX ctx;
    kd_sha256_init(&ctx);
    kd_sha256_update(&ctx, (const kd_u8 *)Data, (kd_u32)Length);
    kd_sha256_final(&ctx, (kd_u8 *)Digest);
}

/* ================================================================== */
/* HMAC-SHA256 (RFC 2104)                                             */
/* ================================================================== */

#define KD_SHA256_BLOCK 64
#define KD_SHA256_DIGEST 32

void KdNetHmacSha256ExpandKey(KDNET_HMAC_SHA256_KEY *Key, const unsigned char *KeyBytes, unsigned int KeyLength)
{
    kd_u8 k0[KD_SHA256_BLOCK];
    kd_u32 i;

    kd_memset(k0, 0, KD_SHA256_BLOCK);

    if (KeyLength > KD_SHA256_BLOCK) {
        /* Keys longer than the block size are hashed down first. */
        KdNetSha256(KeyBytes, KeyLength, k0);
        /* remaining bytes already zero */
    } else {
        kd_memcpy(k0, KeyBytes, (kd_u32)KeyLength);
    }

    for (i = 0; i < KD_SHA256_BLOCK; i++) {
        Key->ipad[i] = (kd_u8)(k0[i] ^ 0x36);
        Key->opad[i] = (kd_u8)(k0[i] ^ 0x5c);
    }

    /* Wipe the local key copy. */
    kd_memset(k0, 0, KD_SHA256_BLOCK);
}

void KdNetHmacSha256(const KDNET_HMAC_SHA256_KEY *Key, const unsigned char *Data, unsigned int Length, unsigned char Mac[32])
{
    KD_SHA256_CTX ctx;
    kd_u8 inner[KD_SHA256_DIGEST];

    /* inner = H(ipad || message) */
    kd_sha256_init(&ctx);
    kd_sha256_update(&ctx, Key->ipad, KD_SHA256_BLOCK);
    kd_sha256_update(&ctx, (const kd_u8 *)Data, (kd_u32)Length);
    kd_sha256_final(&ctx, inner);

    /* mac = H(opad || inner) */
    kd_sha256_init(&ctx);
    kd_sha256_update(&ctx, Key->opad, KD_SHA256_BLOCK);
    kd_sha256_update(&ctx, inner, KD_SHA256_DIGEST);
    kd_sha256_final(&ctx, (kd_u8 *)Mac);
}

/* ================================================================== */
/* AES-256 (FIPS-197)                                                */
/* ================================================================== */

/* Forward S-box. */
static const kd_u8 kd_aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

/* Inverse S-box. */
static const kd_u8 kd_aes_inv_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

/* Round constants. */
static const kd_u8 kd_aes_rcon[15] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40,
    0x80, 0x1b, 0x36, 0x6c, 0xd8, 0xab, 0x4d
};

/* AES-256 parameters. */
#define KD_AES256_NK 8    /* key length in 32-bit words */
#define KD_AES256_NR 14   /* number of rounds */
#define KD_AES_NB    4    /* block size in 32-bit words */
#define KD_AES256_NWORDS ((KD_AES256_NR + 1) * KD_AES_NB)  /* 60 */

/* GF(2^8) multiply by 2 (xtime). */
static kd_u8 kd_xtime(kd_u8 x)
{
    return (kd_u8)((x << 1) ^ (((x >> 7) & 1) * 0x1b));
}

/* General GF(2^8) multiply. */
static kd_u8 kd_gmul(kd_u8 a, kd_u8 b)
{
    kd_u8 p = 0;
    kd_u8 i;
    for (i = 0; i < 8; i++) {
        if (b & 1) {
            p ^= a;
        }
        {
            kd_u8 hi = (kd_u8)(a & 0x80);
            a = (kd_u8)(a << 1);
            if (hi) {
                a ^= 0x1b;
            }
        }
        b >>= 1;
    }
    return p;
}

/* SubWord: apply S-box to each byte of a 32-bit word. */
static kd_u32 kd_aes_subword(kd_u32 w)
{
    return ((kd_u32)kd_aes_sbox[(w >> 24) & 0xff] << 24) |
           ((kd_u32)kd_aes_sbox[(w >> 16) & 0xff] << 16) |
           ((kd_u32)kd_aes_sbox[(w >>  8) & 0xff] <<  8) |
           ((kd_u32)kd_aes_sbox[(w)       & 0xff]);
}

/* RotWord: cyclic left rotate by one byte. */
static kd_u32 kd_aes_rotword(kd_u32 w)
{
    return ((w << 8) | (w >> 24)) & 0xffffffffUL;
}

void KdNetAes256ExpandKey(KDNET_AES_KEY *Key, const unsigned char KeyBytes[32])
{
    kd_u32 *enc = (kd_u32 *)Key->EncRoundKey;
    kd_u32 *dec = (kd_u32 *)Key->DecRoundKey;
    kd_u32 i;
    kd_u32 temp;

    /* First Nk words come directly from the key. */
    for (i = 0; i < KD_AES256_NK; i++) {
        enc[i] = kd_load_be32(&KeyBytes[i * 4]);
    }

    for (i = KD_AES256_NK; i < KD_AES256_NWORDS; i++) {
        temp = enc[i - 1];
        if ((i % KD_AES256_NK) == 0) {
            temp = kd_aes_subword(kd_aes_rotword(temp)) ^
                   ((kd_u32)kd_aes_rcon[i / KD_AES256_NK] << 24);
        } else if ((i % KD_AES256_NK) == 4) {
            /* AES-256 has the extra SubWord at the Nk/2 position. */
            temp = kd_aes_subword(temp);
        }
        enc[i] = enc[i - KD_AES256_NK] ^ temp;
    }

    /*
     * Build the equivalent inverse cipher (Equivalent Decryption Schedule),
     * FIPS-197 5.3.5: dec round keys are the enc round keys in reverse round
     * order with InvMixColumns applied to all but the first and last round
     * keys.
     */
    for (i = 0; i < KD_AES256_NWORDS; i += 4) {
        kd_u32 round = i / 4;                       /* 0 .. NR */
        kd_u32 srcRound = KD_AES256_NR - round;     /* reversed */
        kd_u32 j;
        for (j = 0; j < 4; j++) {
            kd_u32 w = enc[srcRound * 4 + j];
            if (round != 0 && round != KD_AES256_NR) {
                kd_u8 b0 = (kd_u8)(w >> 24);
                kd_u8 b1 = (kd_u8)(w >> 16);
                kd_u8 b2 = (kd_u8)(w >>  8);
                kd_u8 b3 = (kd_u8)(w);
                kd_u8 r0 = (kd_u8)(kd_gmul(b0, 0x0e) ^ kd_gmul(b1, 0x0b) ^ kd_gmul(b2, 0x0d) ^ kd_gmul(b3, 0x09));
                kd_u8 r1 = (kd_u8)(kd_gmul(b0, 0x09) ^ kd_gmul(b1, 0x0e) ^ kd_gmul(b2, 0x0b) ^ kd_gmul(b3, 0x0d));
                kd_u8 r2 = (kd_u8)(kd_gmul(b0, 0x0d) ^ kd_gmul(b1, 0x09) ^ kd_gmul(b2, 0x0e) ^ kd_gmul(b3, 0x0b));
                kd_u8 r3 = (kd_u8)(kd_gmul(b0, 0x0b) ^ kd_gmul(b1, 0x0d) ^ kd_gmul(b2, 0x09) ^ kd_gmul(b3, 0x0e));
                w = ((kd_u32)r0 << 24) | ((kd_u32)r1 << 16) | ((kd_u32)r2 << 8) | (kd_u32)r3;
            }
            dec[i + j] = w;
        }
    }
}

/*
 * AES single-block encryption. state[16] in/out, column-major per FIPS-197
 * (state[r + 4c]). We use the standard byte-oriented state where state[i]
 * is the i-th byte of the 16-byte block (input byte i maps to column i/4,
 * row i%4).
 */
static void kd_aes_encrypt_block(const kd_u32 *enc, const kd_u8 in[16], kd_u8 out[16])
{
    kd_u8 s[16];
    kd_u8 t[16];
    kd_u32 round;
    kd_u32 i;

    /* AddRoundKey (round 0). */
    for (i = 0; i < 16; i++) {
        kd_u32 rk = enc[i / 4];
        kd_u8 kb = (kd_u8)(rk >> (24 - 8 * (i % 4)));
        s[i] = (kd_u8)(in[i] ^ kb);
    }

    for (round = 1; round < KD_AES256_NR; round++) {
        /* SubBytes + ShiftRows into t[]. */
        /* Column c row r -> s[4*c + r]. ShiftRows: row r shifted left r. */
        t[0]  = kd_aes_sbox[s[0]];
        t[1]  = kd_aes_sbox[s[5]];
        t[2]  = kd_aes_sbox[s[10]];
        t[3]  = kd_aes_sbox[s[15]];
        t[4]  = kd_aes_sbox[s[4]];
        t[5]  = kd_aes_sbox[s[9]];
        t[6]  = kd_aes_sbox[s[14]];
        t[7]  = kd_aes_sbox[s[3]];
        t[8]  = kd_aes_sbox[s[8]];
        t[9]  = kd_aes_sbox[s[13]];
        t[10] = kd_aes_sbox[s[2]];
        t[11] = kd_aes_sbox[s[7]];
        t[12] = kd_aes_sbox[s[12]];
        t[13] = kd_aes_sbox[s[1]];
        t[14] = kd_aes_sbox[s[6]];
        t[15] = kd_aes_sbox[s[11]];

        /* MixColumns. */
        for (i = 0; i < 4; i++) {
            kd_u8 a0 = t[4 * i + 0];
            kd_u8 a1 = t[4 * i + 1];
            kd_u8 a2 = t[4 * i + 2];
            kd_u8 a3 = t[4 * i + 3];
            s[4 * i + 0] = (kd_u8)(kd_xtime(a0) ^ (kd_xtime(a1) ^ a1) ^ a2 ^ a3);
            s[4 * i + 1] = (kd_u8)(a0 ^ kd_xtime(a1) ^ (kd_xtime(a2) ^ a2) ^ a3);
            s[4 * i + 2] = (kd_u8)(a0 ^ a1 ^ kd_xtime(a2) ^ (kd_xtime(a3) ^ a3));
            s[4 * i + 3] = (kd_u8)((kd_xtime(a0) ^ a0) ^ a1 ^ a2 ^ kd_xtime(a3));
        }

        /* AddRoundKey. */
        for (i = 0; i < 16; i++) {
            kd_u32 rk = enc[round * 4 + (i / 4)];
            kd_u8 kb = (kd_u8)(rk >> (24 - 8 * (i % 4)));
            s[i] ^= kb;
        }
    }

    /* Final round: SubBytes + ShiftRows + AddRoundKey (no MixColumns). */
    t[0]  = kd_aes_sbox[s[0]];
    t[1]  = kd_aes_sbox[s[5]];
    t[2]  = kd_aes_sbox[s[10]];
    t[3]  = kd_aes_sbox[s[15]];
    t[4]  = kd_aes_sbox[s[4]];
    t[5]  = kd_aes_sbox[s[9]];
    t[6]  = kd_aes_sbox[s[14]];
    t[7]  = kd_aes_sbox[s[3]];
    t[8]  = kd_aes_sbox[s[8]];
    t[9]  = kd_aes_sbox[s[13]];
    t[10] = kd_aes_sbox[s[2]];
    t[11] = kd_aes_sbox[s[7]];
    t[12] = kd_aes_sbox[s[12]];
    t[13] = kd_aes_sbox[s[1]];
    t[14] = kd_aes_sbox[s[6]];
    t[15] = kd_aes_sbox[s[11]];

    for (i = 0; i < 16; i++) {
        kd_u32 rk = enc[KD_AES256_NR * 4 + (i / 4)];
        kd_u8 kb = (kd_u8)(rk >> (24 - 8 * (i % 4)));
        out[i] = (kd_u8)(t[i] ^ kb);
    }
}

/*
 * AES single-block decryption using the equivalent inverse cipher schedule
 * (dec[] from KdNetAes256ExpandKey). Structure mirrors encryption but uses
 * InvSubBytes / InvShiftRows / InvMixColumns.
 */
static void kd_aes_decrypt_block(const kd_u32 *dec, const kd_u8 in[16], kd_u8 out[16])
{
    kd_u8 s[16];
    kd_u8 t[16];
    kd_u32 round;
    kd_u32 i;

    /* AddRoundKey (round 0 of the inverse schedule). */
    for (i = 0; i < 16; i++) {
        kd_u32 rk = dec[i / 4];
        kd_u8 kb = (kd_u8)(rk >> (24 - 8 * (i % 4)));
        s[i] = (kd_u8)(in[i] ^ kb);
    }

    for (round = 1; round < KD_AES256_NR; round++) {
        /* InvShiftRows + InvSubBytes into t[]. Row r rotated right by r. */
        t[0]  = kd_aes_inv_sbox[s[0]];
        t[1]  = kd_aes_inv_sbox[s[13]];
        t[2]  = kd_aes_inv_sbox[s[10]];
        t[3]  = kd_aes_inv_sbox[s[7]];
        t[4]  = kd_aes_inv_sbox[s[4]];
        t[5]  = kd_aes_inv_sbox[s[1]];
        t[6]  = kd_aes_inv_sbox[s[14]];
        t[7]  = kd_aes_inv_sbox[s[11]];
        t[8]  = kd_aes_inv_sbox[s[8]];
        t[9]  = kd_aes_inv_sbox[s[5]];
        t[10] = kd_aes_inv_sbox[s[2]];
        t[11] = kd_aes_inv_sbox[s[15]];
        t[12] = kd_aes_inv_sbox[s[12]];
        t[13] = kd_aes_inv_sbox[s[9]];
        t[14] = kd_aes_inv_sbox[s[6]];
        t[15] = kd_aes_inv_sbox[s[3]];

        /* InvMixColumns (equivalent inverse cipher applies IMC before the
         * IMC-transformed round key is added). */
        for (i = 0; i < 4; i++) {
            kd_u8 a0 = t[4 * i + 0];
            kd_u8 a1 = t[4 * i + 1];
            kd_u8 a2 = t[4 * i + 2];
            kd_u8 a3 = t[4 * i + 3];
            s[4 * i + 0] = (kd_u8)(kd_gmul(a0, 0x0e) ^ kd_gmul(a1, 0x0b) ^ kd_gmul(a2, 0x0d) ^ kd_gmul(a3, 0x09));
            s[4 * i + 1] = (kd_u8)(kd_gmul(a0, 0x09) ^ kd_gmul(a1, 0x0e) ^ kd_gmul(a2, 0x0b) ^ kd_gmul(a3, 0x0d));
            s[4 * i + 2] = (kd_u8)(kd_gmul(a0, 0x0d) ^ kd_gmul(a1, 0x09) ^ kd_gmul(a2, 0x0e) ^ kd_gmul(a3, 0x0b));
            s[4 * i + 3] = (kd_u8)(kd_gmul(a0, 0x0b) ^ kd_gmul(a1, 0x0d) ^ kd_gmul(a2, 0x09) ^ kd_gmul(a3, 0x0e));
        }

        /* AddRoundKey (with IMC-transformed dec round key). */
        for (i = 0; i < 16; i++) {
            kd_u32 rk = dec[round * 4 + (i / 4)];
            kd_u8 kb = (kd_u8)(rk >> (24 - 8 * (i % 4)));
            s[i] ^= kb;
        }
    }

    /* Final round: InvShiftRows + InvSubBytes + AddRoundKey (no InvMixColumns). */
    t[0]  = kd_aes_inv_sbox[s[0]];
    t[1]  = kd_aes_inv_sbox[s[13]];
    t[2]  = kd_aes_inv_sbox[s[10]];
    t[3]  = kd_aes_inv_sbox[s[7]];
    t[4]  = kd_aes_inv_sbox[s[4]];
    t[5]  = kd_aes_inv_sbox[s[1]];
    t[6]  = kd_aes_inv_sbox[s[14]];
    t[7]  = kd_aes_inv_sbox[s[11]];
    t[8]  = kd_aes_inv_sbox[s[8]];
    t[9]  = kd_aes_inv_sbox[s[5]];
    t[10] = kd_aes_inv_sbox[s[2]];
    t[11] = kd_aes_inv_sbox[s[15]];
    t[12] = kd_aes_inv_sbox[s[12]];
    t[13] = kd_aes_inv_sbox[s[9]];
    t[14] = kd_aes_inv_sbox[s[6]];
    t[15] = kd_aes_inv_sbox[s[3]];

    for (i = 0; i < 16; i++) {
        kd_u32 rk = dec[KD_AES256_NR * 4 + (i / 4)];
        kd_u8 kb = (kd_u8)(rk >> (24 - 8 * (i % 4)));
        out[i] = (kd_u8)(t[i] ^ kb);
    }
}

/* ================================================================== */
/* AES-CBC                                                           */
/* ================================================================== */

void KdNetAesCbcEncrypt(const KDNET_AES_KEY *Key, unsigned char Iv[16],
                        const unsigned char *Src, unsigned char *Dst, unsigned int Length)
{
    const kd_u32 *enc = (const kd_u32 *)Key->EncRoundKey;
    kd_u8 chain[16];
    kd_u8 block[16];
    unsigned int off;
    kd_u32 i;

    kd_memcpy(chain, Iv, 16);

    for (off = 0; off + 16 <= Length; off += 16) {
        /* block = plaintext XOR chain */
        for (i = 0; i < 16; i++) {
            block[i] = (kd_u8)(Src[off + i] ^ chain[i]);
        }
        kd_aes_encrypt_block(enc, block, &Dst[off]);
        /* New chaining value is the ciphertext we just produced. */
        kd_memcpy(chain, &Dst[off], 16);
    }

    /* Update IV in place to last ciphertext block (SymCrypt semantics). */
    kd_memcpy(Iv, chain, 16);
}

void KdNetAesCbcDecrypt(const KDNET_AES_KEY *Key, unsigned char Iv[16],
                        const unsigned char *Src, unsigned char *Dst, unsigned int Length)
{
    const kd_u32 *dec = (const kd_u32 *)Key->DecRoundKey;
    kd_u8 chain[16];
    kd_u8 nextChain[16];
    kd_u8 plain[16];
    unsigned int off;
    kd_u32 i;

    kd_memcpy(chain, Iv, 16);

    for (off = 0; off + 16 <= Length; off += 16) {
        /* Save the input ciphertext block first (supports in-place Src==Dst). */
        kd_memcpy(nextChain, &Src[off], 16);
        kd_aes_decrypt_block(dec, &Src[off], plain);
        for (i = 0; i < 16; i++) {
            Dst[off + i] = (kd_u8)(plain[i] ^ chain[i]);
        }
        kd_memcpy(chain, nextChain, 16);
    }

    /* Update IV in place to last input ciphertext block (SymCrypt semantics). */
    kd_memcpy(Iv, chain, 16);
}
