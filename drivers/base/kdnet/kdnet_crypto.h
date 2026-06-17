

#ifndef _KDNET_CRYPTO_H_
#define _KDNET_CRYPTO_H_

#include "kdnet_crypto_prim.h"

#include <pshpack1.h>
typedef struct _KD_NET_HEADER_V2
{
    ULONG     Signature;       /* 0x4742444D == "MDBG" */
    UCHAR     Version;         /* 2 or 4 */
    UCHAR     Flags;           /* bit0: use TargetDebugKey (control channel) */
    ULONGLONG SequenceNumber;  /* packed seq + pad/direction (encrypted) */
} KD_NET_HEADER_V2, *PKD_NET_HEADER_V2;
#include <poppack.h>

#define KDNET_SIGNATURE 0x4742444DUL   /* "MDBG" */

/** Per-link encryption state (lives in the adapter / DEBUG_NET_DATA). */
typedef struct _KDNET_CRYPTO_CONTEXT
{
    KDNET_AES_KEY           TargetDebugKey;   /* AES-256 from the debug key   */
    KDNET_AES_KEY           DebugSessionKey;  /* AES-256 from the session key */
    KDNET_HMAC_SHA256_KEY   HMacKey;          /* HMAC key = ~debugkey         */
    ULONGLONG               LastValidHostSequenceNumber;
    BOOLEAN                 EncryptedLink;
    BOOLEAN                 Connected;        /* session/data channel up      */
} KDNET_CRYPTO_CONTEXT, *PKDNET_CRYPTO_CONTEXT;

/**
 * Initialize the encryption context from the 32-byte debug key (and optional
 * 32-byte session key). Mirrors InitializeEncryption.
 *
 * @return STATUS_SUCCESS, or STATUS_INVALID_PARAMETER if the link is not
 *         encrypted or the key is all-zero.
 */
NTSTATUS
KdNetCryptoInitialize(
    _Out_ PKDNET_CRYPTO_CONTEXT Ctx,
    _In_reads_(32) const UCHAR Key[32],
    _In_reads_opt_(32) const UCHAR SessionKey[32],
    _In_ BOOLEAN Connected,
    _In_ BOOLEAN EncryptedLink);

/**
 * Encrypt + authenticate a KD packet in place. Mirrors EncryptKdPacket.
 *
 * @param KdPacket  Packet buffer (header + KD data starting after SequenceNumber).
 * @param Length    In: KD data length; Out: total on-wire payload length.
 * @param Key       Expanded AES key to use (TargetDebugKey or DebugSessionKey).
 * @param HMac      Expanded HMAC key (the context HMacKey).
 * @param SequenceNumber Monotonic sequence number for this packet.
 * @param Flags     Packet flags (bit0 -> control channel / target key).
 */
VOID
KdNetEncryptKdPacket(
    _Inout_ PKD_NET_HEADER_V2 KdPacket,
    _Inout_ PULONG Length,
    _In_ const KDNET_AES_KEY *Key,
    _In_ const KDNET_HMAC_SHA256_KEY *HMac,
    _In_ ULONGLONG SequenceNumber,
    _In_ UCHAR Flags);

/**
 * Decrypt + authenticate a received KD packet in place. Mirrors DecryptKdPacket.
 * On success advances *Packet past the 6-byte header and 8-byte sequence field
 * and sets *Length to the plaintext KD data length (padding removed).
 *
 * @param IsControlChannel Optional out: TRUE if this was a control-channel packet
 *        (encrypted with the target key); the caller should route it accordingly.
 * @return STATUS_SUCCESS if authentic, else an error status.
 */
NTSTATUS
KdNetDecryptKdPacket(
    _Inout_ PKDNET_CRYPTO_CONTEXT Ctx,
    _Inout_ UCHAR **Packet,
    _Inout_ PULONG Length,
    _Out_opt_ PBOOLEAN IsControlChannel);

/**
 * Derive the data-channel session key from the host's connect response and
 * expand it into the context's DebugSessionKey. Mirrors InitializeDataChannel:
 * SessionKey = SHA-256(Key || Packet[Length]); then mark the link connected.
 *
 * @param Key The 32-byte debug key (DebugParameters->Key).
 * @param Packet Decrypted host response packet body.
 * @param Length Bytes of Packet to hash 
 * @param OutSessionKey Receives the 32-byte derived session key.
 */
VOID
KdNetCryptoSetSessionKey(
    _Inout_ PKDNET_CRYPTO_CONTEXT Ctx,
    _In_reads_(32) const UCHAR Key[32],
    _In_reads_(Length) const UCHAR *Packet,
    _In_ ULONG Length,
    _Out_writes_(32) UCHAR OutSessionKey[32]);

#endif /* _KDNET_CRYPTO_H_ */
