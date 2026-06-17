/*
 * kdnet_crypto_prim.h
 *
 * Portable, standards-exact C crypto primitives for the Windows kernel
 * debugger transport (KDNET). Byte-exact / wire-compatible with the
 * SymCrypt-backed implementation used by Windows windbg.
 *
 * Implements:
 *   - AES-256 key expansion (enc + dec schedules)
 *   - AES-CBC encrypt / decrypt (FIPS-197 + CBC mode, SymCrypt IV semantics)
 *   - SHA-256 (one-shot)
 *   - HMAC-SHA256 (RFC 2104, expand-key + run)
 *
 * Freestanding: no libc, no dynamic allocation, no CRT-initialized globals.
 * Pure portable C (MSVC + clang). No SSE / intrinsics.
 */

#ifndef _KDNET_CRYPTO_PRIM_H_
#define _KDNET_CRYPTO_PRIM_H_

/*
 * Minimal fixed-width typedefs.
 *
 * In a real kernel build the surrounding code pulls in <ntdef.h>-style
 * types; here we only depend on the unsized C keyword types in the public
 * signatures (unsigned char / unsigned int / unsigned long). The guard
 * below lets the header (and the .c that includes it) compile standalone
 * for the KAT harness as well.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _KDNET_AES_KEY {
    unsigned long EncRoundKey[60];  /* AES-256: 14 rounds -> 60 words */
    unsigned long DecRoundKey[60];
} KDNET_AES_KEY;

typedef struct _KDNET_HMAC_SHA256_KEY {
    unsigned char ipad[64];
    unsigned char opad[64];
} KDNET_HMAC_SHA256_KEY;

/* AES-256 only (32-byte key). Expands both encryption and decryption schedules. */
void KdNetAes256ExpandKey(KDNET_AES_KEY *Key, const unsigned char KeyBytes[32]);

/* AES-CBC over Length bytes (Length must be a multiple of 16). Iv is the 16-byte
 * chaining value (updated in place to the last cipher block, like SymCrypt). In-place
 * allowed (Src==Dst). */
void KdNetAesCbcEncrypt(const KDNET_AES_KEY *Key, unsigned char Iv[16],
                        const unsigned char *Src, unsigned char *Dst, unsigned int Length);
void KdNetAesCbcDecrypt(const KDNET_AES_KEY *Key, unsigned char Iv[16],
                        const unsigned char *Src, unsigned char *Dst, unsigned int Length);

/* SHA-256 one-shot. */
void KdNetSha256(const unsigned char *Data, unsigned int Length, unsigned char Digest[32]);

/* HMAC-SHA256: expand key once, then run over data (mirrors SymCryptHmacSha256ExpandKey +
 * SymCryptHmacSha256). */
void KdNetHmacSha256ExpandKey(KDNET_HMAC_SHA256_KEY *Key, const unsigned char *KeyBytes, unsigned int KeyLength);
void KdNetHmacSha256(const KDNET_HMAC_SHA256_KEY *Key, const unsigned char *Data, unsigned int Length, unsigned char Mac[32]);

#ifdef __cplusplus
}
#endif

#endif /* _KDNET_CRYPTO_PRIM_H_ */
