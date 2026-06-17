

#include "kdnet_private.h"
#include "kdnet_crypto.h"

/* Local tiny mem helpers (freestanding; do not depend on CRT in this TU). */
static void KdCryptoMemCopy(void *Dst, const void *Src, ULONG Length)
{
    PUCHAR d = (PUCHAR)Dst;
    const UCHAR *s = (const UCHAR *)Src;
    while (Length--) *d++ = *s++;
}

static void KdCryptoMemZero(void *Dst, ULONG Length)
{
    PUCHAR d = (PUCHAR)Dst;
    while (Length--) *d++ = 0;
}

NTSTATUS
KdNetCryptoInitialize(
    _Out_ PKDNET_CRYPTO_CONTEXT Ctx,
    _In_reads_(32) const UCHAR Key[32],
    _In_reads_opt_(32) const UCHAR SessionKey[32],
    _In_ BOOLEAN Connected,
    _In_ BOOLEAN EncryptedLink)
{
    UCHAR HMacKey[32];
    ULONG i;
    ULONG keyOr;

    if (!Ctx || !Key)
        return STATUS_INVALID_PARAMETER;
    if (!EncryptedLink)
        return STATUS_INVALID_PARAMETER;

    /* HMAC key = bitwise complement of the debug key; also reject an all-zero key
     * (InitializeEncryption: v4 |= *key; HMacKey = ~key). */
    keyOr = 0;
    for (i = 0; i < 8; i++)
    {
        ULONG w;
        KdCryptoMemCopy(&w, Key + 4 * i, 4);
        keyOr |= w;
        w = ~w;
        KdCryptoMemCopy(HMacKey + 4 * i, &w, 4);
    }
    if (keyOr == 0)
        return STATUS_INVALID_PARAMETER;

    KdNetHmacSha256ExpandKey(&Ctx->HMacKey, HMacKey, 32);
    KdNetAes256ExpandKey(&Ctx->TargetDebugKey, Key);
    if (Connected && SessionKey)
        KdNetAes256ExpandKey(&Ctx->DebugSessionKey, SessionKey);

    Ctx->EncryptedLink = EncryptedLink;
    Ctx->Connected = Connected;

    KdCryptoMemZero(HMacKey, sizeof(HMacKey));
    return STATUS_SUCCESS;
}

VOID
KdNetEncryptKdPacket(
    _Inout_ PKD_NET_HEADER_V2 KdPacket,
    _Inout_ PULONG Length,
    _In_ const KDNET_AES_KEY *Key,
    _In_ const KDNET_HMAC_SHA256_KEY *HMac,
    _In_ ULONGLONG SequenceNumber,
    _In_ UCHAR Flags)
{
    PUCHAR seqp = (PUCHAR)&KdPacket->SequenceNumber;
    ULONG len;
    ULONG pad;
    UCHAR HMacResult[32];
    PULONG seqField;
    ULONG encLen;
    UCHAR iv[16];

    /* Plaintext = SequenceNumber field (8) + KD data; pad to 16. */
    len = *Length + 8;
    pad = (ULONG)(-(LONG)len) & 0xF;
    KdCryptoMemZero(seqp + len, pad);
    len += pad;

    KdPacket->Flags = Flags;
    KdPacket->Signature = KDNET_SIGNATURE;

    seqField = (PULONG)&KdPacket->SequenceNumber;
    seqField[1] = _byteswap_ulong((ULONG)pad | ((ULONG)SequenceNumber << 8));
    seqField[0] = _byteswap_ulong((ULONG)(SequenceNumber >> 24));

    KdPacket->Version = 4;

    /* HMAC over [6-byte header .. plaintext]; first 16 bytes become tag + IV. */
    KdNetHmacSha256(HMac, (const UCHAR *)KdPacket, len + 6, HMacResult);
    KdCryptoMemCopy(seqp + len, HMacResult, 16);

    encLen = len - 32 * ((Flags >> 2) & 1);
    KdCryptoMemCopy(iv, HMacResult, 16);
    KdNetAesCbcEncrypt(Key, iv, seqp, seqp, encLen);

    *Length = len + 22;   /* + 6 header + 16 tag */
}

VOID
KdNetCryptoSetSessionKey(
    _Inout_ PKDNET_CRYPTO_CONTEXT Ctx,
    _In_reads_(32) const UCHAR Key[32],
    _In_reads_(Length) const UCHAR *Packet,
    _In_ ULONG Length,
    _Out_writes_(32) UCHAR OutSessionKey[32])
{
    /* SessionKey = SHA-256(Key[32] || Packet[Length]). Both sides compute the
     * same value, so it becomes the shared AES-256 data-channel key. */
    UCHAR buf[32 + 512];
    ULONG n = Length;

    if (n > 512)
        n = 512;
    KdCryptoMemCopy(buf, Key, 32);
    KdCryptoMemCopy(buf + 32, Packet, n);
    KdNetSha256(buf, 32 + n, OutSessionKey);

    KdNetAes256ExpandKey(&Ctx->DebugSessionKey, OutSessionKey);
    Ctx->Connected = TRUE;
    Ctx->LastValidHostSequenceNumber = 0;

    KdCryptoMemZero(buf, sizeof(buf));
}

NTSTATUS
KdNetDecryptKdPacket(
    _Inout_ PKDNET_CRYPTO_CONTEXT Ctx,
    _Inout_ UCHAR **Packet,
    _Inout_ PULONG Length,
    _Out_opt_ PBOOLEAN IsControlChannel)
{
    PUCHAR p = *Packet;
    const KDNET_AES_KEY *key;
    UCHAR version, flags;
    BOOLEAN useTargetKey;
    ULONG len = *Length;
    UCHAR iv[16];
    UCHAR HMacResult[32];
    PULONG seqField;
    ULONG i;
    ULONG hi, lo;
    UCHAR dirPadByte;
    UCHAR padCount;
    ULONGLONG sequence;

    if (IsControlChannel)
        *IsControlChannel = FALSE;

    if (!Ctx->EncryptedLink)
        return STATUS_SUCCESS;   /* nothing to do for a cleartext link */

    key = &Ctx->DebugSessionKey;

    if (len < 0x26)
        return STATUS_UNSUCCESSFUL;
    if (*(PULONG)p != KDNET_SIGNATURE)
        return STATUS_UNSUCCESSFUL;

    version = p[4];
    if (version != 2 && version != 4)
        return STATUS_UNSUCCESSFUL;

    flags = p[5];
    if (flags & 0xFE)
        return STATUS_UNSUCCESSFUL;

    useTargetKey = (BOOLEAN)(flags & 1);
    if (useTargetKey)
    {
        key = &Ctx->TargetDebugKey;
    }
    else
    {
        if (version != 4)
            return STATUS_UNSUCCESSFUL;
        if (!Ctx->Connected)
            return STATUS_UNSUCCESSFUL;
    }

    /* Skip the 6-byte clear header. */
    p += 6;
    *Packet = p;
    len -= 6;
    if (len & 0xF)
        return STATUS_UNSUCCESSFUL;

    /* The trailing 16 bytes are the tag/IV. */
    len -= 16;
    *Length = len;

    seqField = (PULONG)p;  /* the (still-encrypted) SequenceNumber field */

    /* IV = appended tag; AES-CBC decrypt the region in place. */
    KdCryptoMemCopy(iv, p + len, 16);
    KdNetAesCbcDecrypt(key, iv, p, p, len);

    /* Authenticate: recompute HMAC over [header .. decrypted plaintext]. */
    KdNetHmacSha256(&Ctx->HMacKey, p - 6, len + 6, HMacResult);
    for (i = 0; i < 4; i++)
    {
        if (((PULONG)HMacResult)[i] != ((PULONG)(p + len))[i])
            return STATUS_UNSUCCESSFUL;   /* failed authentication */
    }

    /* Strip the 8-byte sequence field; decode sequence + padding. */
    p += 8;
    *Packet = p;
    len -= 8;

    hi = _byteswap_ulong(seqField[1]);
    lo = _byteswap_ulong(seqField[0]);
    dirPadByte = (UCHAR)hi;                       /* low byte of high dword */
    /* sequence = ((lo:hi) >> 8) : full 64-bit value */
    sequence = (((ULONGLONG)lo << 32) | hi) >> 8;

    /* Direction bit must be set for inbound (host->target) packets. */
    if ((dirPadByte & 0x80) == 0)
        return STATUS_UNSUCCESSFUL;                /* bad direction */

    padCount = dirPadByte & 0x7F;
    if (padCount >= 0x10 || padCount > len)
        return STATUS_UNSUCCESSFUL;                /* bad padding */

    if (!useTargetKey)
    {
        if (sequence <= Ctx->LastValidHostSequenceNumber)
            return STATUS_UNSUCCESSFUL;            /* replay / bad sequence */
        Ctx->LastValidHostSequenceNumber = sequence;
    }

    len -= padCount;
    *Length = len;

    if (IsControlChannel)
        *IsControlChannel = useTargetKey;

    return STATUS_SUCCESS;
}
