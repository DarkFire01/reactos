/*
 * PROJECT:     ReactOS Kernel Cryptography Next Generation
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Key objects, symmetric ciphers and their chaining modes
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "cngp.h"

#include <debug.h>

/* DEFINES ********************************************************************/

#define CNG_BLOCK_MAX           SYMCRYPT_AES_BLOCK_SIZE
#define CNG_RC4_SECRET_MAX      64
#define CNG_KDF_KEY_BITS_MAX    16384

/* TYPES **********************************************************************/

typedef struct _CNG_CIPHER_INFO
{
    const PCSYMCRYPT_BLOCKCIPHER *BlockCipher;
    ULONG BlockLength;
    ULONG ExpandedSize;
    ULONG SecretMax;
    BCRYPT_KEY_LENGTHS_STRUCT Lengths;
} CNG_CIPHER_INFO, *PCNG_CIPHER_INFO;

/* State of a GCM operation spread over several calls */
typedef struct _CNG_GCM_CHAIN
{
    PVOID Context;
    SYMCRYPT_GCM_EXPANDED_KEY Key;
    SYMCRYPT_GCM_STATE State;
} CNG_GCM_CHAIN, *PCNG_GCM_CHAIN;

/* GLOBALS ********************************************************************/

/* Indexed by CNG_CIPHER */
static const CNG_CIPHER_INFO CngCiphers[] =
{
    { &SymCryptAesBlockCipher, SYMCRYPT_AES_BLOCK_SIZE, sizeof(SYMCRYPT_AES_EXPANDED_KEY), 32, { 128, 256, 64 } },
    { &SymCryptDesBlockCipher, SYMCRYPT_DES_BLOCK_SIZE, sizeof(SYMCRYPT_DES_EXPANDED_KEY), 8, { 64, 64, 0 } },
    { &SymCrypt3DesBlockCipher, SYMCRYPT_3DES_BLOCK_SIZE, sizeof(SYMCRYPT_3DES_EXPANDED_KEY), 24, { 192, 192, 0 } },
    { &SymCrypt3DesBlockCipher, SYMCRYPT_3DES_BLOCK_SIZE, sizeof(SYMCRYPT_3DES_EXPANDED_KEY), 16, { 128, 128, 0 } },
    { NULL, 1, sizeof(SYMCRYPT_RC4_STATE), CNG_RC4_SECRET_MAX, { 8, CNG_RC4_SECRET_MAX * 8, 8 } },
};

static const BCRYPT_KEY_LENGTHS_STRUCT CngGcmTagLengths = { 12, 16, 1 };
static const BCRYPT_KEY_LENGTHS_STRUCT CngCcmTagLengths = { 4, 16, 2 };
static const BCRYPT_KEY_LENGTHS_STRUCT CngKdfKeyLengths = { 0, CNG_KDF_KEY_BITS_MAX, 8 };

static const PCWSTR CngModeNames[] =
{
    BCRYPT_CHAIN_MODE_NA,
    BCRYPT_CHAIN_MODE_CBC,
    BCRYPT_CHAIN_MODE_ECB,
    BCRYPT_CHAIN_MODE_CFB,
    BCRYPT_CHAIN_MODE_CCM,
    BCRYPT_CHAIN_MODE_GCM,
};

/* HELPERS ********************************************************************/

static
const CNG_CIPHER_INFO *
CngCipherInfo(
    _In_ PCNG_PROVIDER Provider)
{
    return &CngCiphers[Provider->Algorithm->Cipher];
}

static
BOOLEAN
CngIsAuthenticated(
    _In_ CNG_MODE Mode)
{
    return Mode == CngModeGcm || Mode == CngModeCcm;
}

/* The unit the input of an unpadded call must be a multiple of */
static
ULONG
CngMessageBlock(
    _In_ PCNG_PROVIDER Provider,
    _In_ CNG_MODE Mode,
    _In_ ULONG CfbBlock)
{
    switch (Mode)
    {
        case CngModeCbc:
        case CngModeEcb:
            return CngCipherInfo(Provider)->BlockLength;

        case CngModeCfb:
            return CfbBlock;

        default:
            return 1;
    }
}

ULONG
NTAPI
CngKeyObjectLength(
    _In_ const CNG_ALGORITHM *Algorithm)
{
    ULONG Trailer;

    if (Algorithm->Class == CngClassKdf)
        Trailer = (ULONG)CNG_ALIGN(sizeof(CNG_KDF_STATE));
    else
        Trailer = (ULONG)CNG_ALIGN(CngCiphers[Algorithm->Cipher].ExpandedSize) + CngCiphers[Algorithm->Cipher].SecretMax;

    return (ULONG)CNG_ALIGN(sizeof(CNG_KEY)) + Trailer + CNG_OBJECT_ALIGNMENT - 1;
}

PCNG_KEY
NTAPI
CngLookupKey(
    _In_opt_ BCRYPT_HANDLE Handle)
{
    PCNG_KEY Key = (PCNG_KEY)Handle;

    if (Key == NULL || ((ULONG_PTR)Handle & (CNG_OBJECT_ALIGNMENT - 1)) != 0)
        return NULL;
    if (Key->Signature != CNG_KEY_SIGNATURE || !CngObjectUsable(Key->Provider))
        return NULL;

    return Key;
}

/**
 * @brief
 * Lays out a key in a caller buffer or a fresh allocation. Symmetric and
 * key derivation keys get their trailing storage, asymmetric keys none.
 */
NTSTATUS
NTAPI
CngCreateKey(
    _In_ PCNG_PROVIDER Provider,
    _Out_writes_bytes_opt_(ObjectLength) PUCHAR Object,
    _In_ ULONG ObjectLength,
    _Out_ PCNG_KEY *Key)
{
    const CNG_ALGORITHM *Algorithm = Provider->Algorithm;
    PVOID Allocation = NULL;
    PCNG_KEY NewKey;
    SIZE_T Needed;
    PUCHAR Start;

    if (Algorithm->Class == CngClassCipher || Algorithm->Class == CngClassKdf)
        Needed = CngKeyObjectLength(Algorithm) - (CNG_OBJECT_ALIGNMENT - 1);
    else
        Needed = sizeof(CNG_KEY);

    if (Object == NULL && ObjectLength != 0)
        return STATUS_INVALID_PARAMETER;

    if (Object != NULL)
    {
        Start = ALIGN_UP_POINTER_BY(Object, CNG_OBJECT_ALIGNMENT);
        if ((SIZE_T)(Start - Object) + Needed > ObjectLength)
            return STATUS_BUFFER_TOO_SMALL;
    }
    else
    {
        Allocation = CngAllocate(Needed + CNG_OBJECT_ALIGNMENT - 1);
        if (Allocation == NULL)
            return STATUS_NO_MEMORY;
        Start = ALIGN_UP_POINTER_BY(Allocation, CNG_OBJECT_ALIGNMENT);
    }

    NewKey = (PCNG_KEY)Start;
    RtlZeroMemory(NewKey, Needed);
    NewKey->Signature = CNG_KEY_SIGNATURE;
    NewKey->Provider = Provider;
    NewKey->Allocation = Allocation;
    NewKey->Mode = Provider->Mode;
    NewKey->MessageBlock = Provider->MessageBlock;

    if (Algorithm->Class == CngClassCipher)
    {
        NewKey->Expanded = Start + CNG_ALIGN(sizeof(CNG_KEY));
        NewKey->Secret = (PUCHAR)NewKey->Expanded + CNG_ALIGN(CngCiphers[Algorithm->Cipher].ExpandedSize);
    }
    else if (Algorithm->Class == CngClassKdf)
    {
        NewKey->Expanded = Start + CNG_ALIGN(sizeof(CNG_KEY));
    }

    CngReferenceProvider(Provider);

    *Key = NewKey;
    return STATUS_SUCCESS;
}

VOID
NTAPI
CngFreeKey(
    _In_ PCNG_KEY Key)
{
    PCNG_PROVIDER Provider = Key->Provider;
    PVOID Allocation = Key->Allocation;
    SIZE_T Size = sizeof(*Key);

    CngFreeAsymmetricKey(Key);

    if (Key->Chain != NULL)
    {
        SymCryptWipe(Key->Chain, sizeof(CNG_GCM_CHAIN));
        CngFree(Key->Chain);
    }

    if (Key->Flags & CNG_KEY_OWN_SECRET)
    {
        SymCryptWipe(Key->Secret, Key->SecretLength);
        CngFree(Key->Secret);
    }

    if (Provider->Algorithm->Class == CngClassCipher || Provider->Algorithm->Class == CngClassKdf)
        Size = CngKeyObjectLength(Provider->Algorithm) - (CNG_OBJECT_ALIGNMENT - 1);

    SymCryptWipe(Key, Size);
    CngFree(Allocation);
    CngDereferenceProvider(Provider);
}

static
BOOLEAN
CngSecretLengthValid(
    _In_ const BCRYPT_KEY_LENGTHS_STRUCT *Lengths,
    _In_ ULONG Bits)
{
    if (Bits < Lengths->dwMinLength || Bits > Lengths->dwMaxLength)
        return FALSE;
    if (Lengths->dwIncrement == 0)
        return Bits == Lengths->dwMinLength;

    return ((Bits - Lengths->dwMinLength) % Lengths->dwIncrement) == 0;
}

static
NTSTATUS
CngExpandSymmetricKey(
    _Inout_ PCNG_KEY Key)
{
    SYMCRYPT_ERROR Error;

    switch (Key->Provider->Algorithm->Cipher)
    {
        case CngCipherAes:
            Error = SymCryptAesExpandKey(Key->Expanded, Key->Secret, Key->SecretLength);
            break;

        case CngCipherDes:
            Error = SymCryptDesExpandKey(Key->Expanded, Key->Secret, Key->SecretLength);
            break;

        case CngCipher3Des:
        case CngCipher3Des112:
            Error = SymCrypt3DesExpandKey(Key->Expanded, Key->Secret, Key->SecretLength);
            break;

        default:
            Error = SymCryptRc4Init(Key->Expanded, Key->Secret, Key->SecretLength);
            break;
    }

    return (Error == SYMCRYPT_NO_ERROR) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

/* Copies the secret into a key and makes it usable */
static
NTSTATUS
CngSetSymmetricSecret(
    _Inout_ PCNG_KEY Key,
    _In_reads_bytes_(SecretLength) PUCHAR Secret,
    _In_ ULONG SecretLength)
{
    const CNG_ALGORITHM *Algorithm = Key->Provider->Algorithm;

    if (Algorithm->Class == CngClassKdf)
    {
        if (SecretLength > CNG_KDF_KEY_BITS_MAX / 8)
            return STATUS_INVALID_PARAMETER;

        Key->Secret = CngAllocate(max(SecretLength, 1));
        if (Key->Secret == NULL)
            return STATUS_NO_MEMORY;

        Key->Flags |= CNG_KEY_OWN_SECRET;
        RtlCopyMemory(Key->Secret, Secret, SecretLength);
        Key->SecretLength = SecretLength;
        Key->KeyBits = SecretLength * 8;
        Key->Flags |= CNG_KEY_FINAL;
        return STATUS_SUCCESS;
    }

    if (!CngSecretLengthValid(&CngCiphers[Algorithm->Cipher].Lengths, SecretLength * 8))
        return STATUS_INVALID_PARAMETER;

    RtlCopyMemory(Key->Secret, Secret, SecretLength);
    Key->SecretLength = SecretLength;
    Key->KeyBits = SecretLength * 8;
    Key->Flags |= CNG_KEY_FINAL;

    return CngExpandSymmetricKey(Key);
}

/* BLOCK MODES ****************************************************************/

static
VOID
CngBlockCrypt(
    _In_ PCNG_KEY Key,
    _In_ BOOLEAN Encrypt,
    _Inout_updates_(CNG_BLOCK_MAX) PUCHAR Chain,
    _In_reads_(Length) PCBYTE Source,
    _Out_writes_(Length) PUCHAR Destination,
    _In_ SIZE_T Length)
{
    PCSYMCRYPT_BLOCKCIPHER Cipher;

    if (Length == 0)
        return;

    if (Key->Provider->Algorithm->Cipher == CngCipherRc4)
    {
        SymCryptRc4Crypt(Key->Expanded, Source, Destination, Length);
        return;
    }

    Cipher = *CngCipherInfo(Key->Provider)->BlockCipher;

    switch (Key->Mode)
    {
        case CngModeEcb:
            if (Encrypt)
                SymCryptEcbEncrypt(Cipher, Key->Expanded, Source, Destination, Length);
            else
                SymCryptEcbDecrypt(Cipher, Key->Expanded, Source, Destination, Length);
            break;

        case CngModeCfb:
            if (Encrypt)
                SymCryptCfbEncrypt(Cipher, Key->MessageBlock, Key->Expanded, Chain, Source, Destination, Length);
            else
                SymCryptCfbDecrypt(Cipher, Key->MessageBlock, Key->Expanded, Chain, Source, Destination, Length);
            break;

        default:
            if (Encrypt)
                SymCryptCbcEncrypt(Cipher, Key->Expanded, Chain, Source, Destination, Length);
            else
                SymCryptCbcDecrypt(Cipher, Key->Expanded, Chain, Source, Destination, Length);
            break;
    }
}

/*
 * Picks the chaining value: the caller IV, or without one the value the key
 * carried over from its last call. Wrong IVs only fail once data is processed.
 */
static
NTSTATUS
CngLoadChain(
    _In_ PCNG_KEY Key,
    _In_opt_ PUCHAR Iv,
    _In_ ULONG IvLength,
    _Out_writes_(CNG_BLOCK_MAX) PUCHAR Chain)
{
    ULONG Block = CngCipherInfo(Key->Provider)->BlockLength;

    RtlZeroMemory(Chain, CNG_BLOCK_MAX);

    if (Key->Mode == CngModeEcb || Key->Mode == CngModeNone)
        return (Iv != NULL) ? STATUS_INVALID_PARAMETER : STATUS_SUCCESS;

    if (Iv == NULL)
    {
        RtlCopyMemory(Chain, Key->Iv, Block);
        return STATUS_SUCCESS;
    }

    if (IvLength != Block)
        return STATUS_INVALID_BUFFER_SIZE;

    RtlCopyMemory(Chain, Iv, Block);
    return STATUS_SUCCESS;
}

static
VOID
CngStoreChain(
    _Inout_ PCNG_KEY Key,
    _Out_writes_bytes_opt_(IvLength) PUCHAR Iv,
    _In_ ULONG IvLength,
    _In_reads_(CNG_BLOCK_MAX) PUCHAR Chain)
{
    ULONG Block = CngCipherInfo(Key->Provider)->BlockLength;

    if (Key->Mode != CngModeCbc && Key->Mode != CngModeCfb)
        return;

    if (Iv != NULL)
        RtlCopyMemory(Iv, Chain, min(IvLength, Block));
    else
        RtlCopyMemory(Key->Iv, Chain, Block);
}

static
NTSTATUS
CngBlockEncrypt(
    _In_ PCNG_KEY Key,
    _In_reads_bytes_opt_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _Inout_updates_bytes_opt_(IvLength) PUCHAR Iv,
    _In_ ULONG IvLength,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    ULONG Block = CngCipherInfo(Key->Provider)->BlockLength;
    ULONG Unit = CngMessageBlock(Key->Provider, Key->Mode, Key->MessageBlock);
    UCHAR Chain[CNG_BLOCK_MAX], Last[CNG_BLOCK_MAX];
    ULONG Whole, Remainder, Needed;
    BOOLEAN Pad = (Flags & BCRYPT_BLOCK_PADDING) != 0;
    NTSTATUS Status;

    if (Flags & ~(BCRYPT_BLOCK_PADDING | BCRYPT_BUFFERS_LOCKED_FLAG))
        return STATUS_INVALID_PARAMETER;
    if (Pad && Key->Mode == CngModeNone)
        return STATUS_INVALID_PARAMETER;

    Whole = InputLength - (InputLength % Block);
    Remainder = InputLength - Whole;
    Needed = Pad ? Whole + Block : InputLength;

    /* The size is reported even for input that is not a whole number of units */
    *ResultLength = Needed;
    if (!Pad && (InputLength % Unit) != 0)
        return STATUS_INVALID_BUFFER_SIZE;
    if (Output == NULL)
        return STATUS_SUCCESS;
    if (OutputLength < Needed)
        return STATUS_BUFFER_TOO_SMALL;

    Status = CngLoadChain(Key, Iv, IvLength, Chain);
    if (!NT_SUCCESS(Status))
        return Status;

    /* The tail is read before the whole blocks are written, as the buffers may overlap */
    if (Pad)
    {
        RtlCopyMemory(Last, Input + Whole, Remainder);
        RtlFillMemory(Last + Remainder, Block - Remainder, (UCHAR)(Block - Remainder));
        CngBlockCrypt(Key, TRUE, Chain, Input, Output, Whole);
        CngBlockCrypt(Key, TRUE, Chain, Last, Output + Whole, Block);
        SymCryptWipe(Last, sizeof(Last));
    }
    else
    {
        CngBlockCrypt(Key, TRUE, Chain, Input, Output, InputLength);
    }

    CngStoreChain(Key, Iv, IvLength, Chain);
    return STATUS_SUCCESS;
}

/*
 * With padding only the real plaintext has to fit, so the last block is
 * decrypted on the side first to learn how long that is.
 */
static
NTSTATUS
CngBlockDecrypt(
    _In_ PCNG_KEY Key,
    _In_reads_bytes_opt_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _Inout_updates_bytes_opt_(IvLength) PUCHAR Iv,
    _In_ ULONG IvLength,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    ULONG Block = CngCipherInfo(Key->Provider)->BlockLength;
    ULONG Unit = CngMessageBlock(Key->Provider, Key->Mode, Key->MessageBlock);
    UCHAR Chain[CNG_BLOCK_MAX], LastChain[CNG_BLOCK_MAX];
    UCHAR LastCipher[CNG_BLOCK_MAX], LastPlain[CNG_BLOCK_MAX];
    ULONG Leading, PadLength, Index;
    BOOLEAN Pad = (Flags & BCRYPT_BLOCK_PADDING) != 0;
    NTSTATUS Status;

    if (Pad && Key->Mode == CngModeNone)
        return STATUS_INVALID_PARAMETER;

    *ResultLength = InputLength;
    if ((InputLength % (Pad ? Block : Unit)) != 0 || (Pad && InputLength == 0))
        return STATUS_INVALID_BUFFER_SIZE;
    if (Output == NULL)
        return STATUS_SUCCESS;

    Status = CngLoadChain(Key, Iv, IvLength, Chain);
    if (!NT_SUCCESS(Status))
        return Status;

    if (!Pad)
    {
        if (OutputLength < InputLength)
            return STATUS_BUFFER_TOO_SMALL;

        CngBlockCrypt(Key, FALSE, Chain, Input, Output, InputLength);
        CngStoreChain(Key, Iv, IvLength, Chain);
        return STATUS_SUCCESS;
    }

    Leading = InputLength - Block;
    RtlCopyMemory(LastCipher, Input + Leading, Block);

    if (Key->Mode == CngModeCfb)
    {
        /* CFB chains through the whole stream, so the tail is decrypted from a copy of the state */
        RtlCopyMemory(LastChain, Chain, Block);
        if (Leading != 0)
        {
            if (Leading >= Block)
                RtlCopyMemory(LastChain, Input + Leading - Block, Block);
            else
            {
                RtlMoveMemory(LastChain, LastChain + Leading, Block - Leading);
                RtlCopyMemory(LastChain + Block - Leading, Input, Leading);
            }
        }
    }
    else if (Leading != 0)
    {
        RtlCopyMemory(LastChain, Input + Leading - Block, Block);
    }
    else
    {
        RtlCopyMemory(LastChain, Chain, Block);
    }

    CngBlockCrypt(Key, FALSE, LastChain, LastCipher, LastPlain, Block);

    PadLength = LastPlain[Block - 1];
    if (PadLength == 0 || PadLength > Block)
    {
        if (PadLength <= InputLength)
            *ResultLength = InputLength - PadLength;
        SymCryptWipe(LastPlain, sizeof(LastPlain));
        return STATUS_DATA_ERROR;
    }

    for (Index = Block - PadLength; Index < Block; Index++)
    {
        if (LastPlain[Index] != PadLength)
        {
            SymCryptWipe(LastPlain, sizeof(LastPlain));
            return STATUS_DATA_ERROR;
        }
    }

    *ResultLength = InputLength - PadLength;
    if (OutputLength < *ResultLength)
    {
        SymCryptWipe(LastPlain, sizeof(LastPlain));
        return STATUS_BUFFER_TOO_SMALL;
    }

    CngBlockCrypt(Key, FALSE, Chain, Input, Output, Leading);
    RtlCopyMemory(Output + Leading, LastPlain, Block - PadLength);
    CngStoreChain(Key, Iv, IvLength, LastChain);

    SymCryptWipe(LastPlain, sizeof(LastPlain));
    return STATUS_SUCCESS;
}

/* AUTHENTICATED MODES ********************************************************/

static
NTSTATUS
CngCheckAuthInfo(
    _In_ PCNG_KEY Key,
    _In_opt_ BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO *Info,
    _In_ ULONG InputLength,
    _In_ ULONG Flags)
{
    const BCRYPT_KEY_LENGTHS_STRUCT *Tags;

    if (Info == NULL || Info->cbSize != sizeof(*Info) ||
        Info->dwInfoVersion != BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO_VERSION)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if ((Flags & BCRYPT_BLOCK_PADDING) || Info->pbNonce == NULL || Info->pbTag == NULL)
        return STATUS_INVALID_PARAMETER;
    if (Info->pbAuthData == NULL && Info->cbAuthData != 0)
        return STATUS_INVALID_PARAMETER;

    Tags = (Key->Mode == CngModeGcm) ? &CngGcmTagLengths : &CngCcmTagLengths;
    if (!CngSecretLengthValid(Tags, Info->cbTag))
        return STATUS_INVALID_PARAMETER;

    if (Key->Mode == CngModeGcm)
    {
        if (SymCryptGcmValidateParameters(SymCryptAesBlockCipher, Info->cbNonce, Info->cbAuthData,
                                          InputLength, Info->cbTag) != SYMCRYPT_NO_ERROR)
        {
            return STATUS_INVALID_PARAMETER;
        }
    }
    else if (SymCryptCcmValidateParameters(SymCryptAesBlockCipher, Info->cbNonce, Info->cbAuthData,
                                           InputLength, Info->cbTag) != SYMCRYPT_NO_ERROR)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

/*
 * GCM calls chained with BCRYPT_AUTH_MODE_CHAIN_CALLS_FLAG keep their state
 * in the key. The IV buffer is required, as on Windows, and every call but
 * the last has to be whole blocks.
 */
static
NTSTATUS
CngGcmChained(
    _Inout_ PCNG_KEY Key,
    _In_ BOOLEAN Encrypt,
    _Inout_ BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO *Info,
    _In_reads_bytes_opt_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_opt_ PUCHAR Iv,
    _In_ ULONG IvLength,
    _Out_writes_bytes_(InputLength) PUCHAR Output)
{
    BOOLEAN Last = !(Info->dwFlags & BCRYPT_AUTH_MODE_CHAIN_CALLS_FLAG);
    PCNG_GCM_CHAIN Chain = Key->Chain;
    SYMCRYPT_ERROR Error;

    if (Iv == NULL || IvLength != SYMCRYPT_GCM_BLOCK_SIZE ||
        Info->pbMacContext == NULL || Info->cbMacContext < SYMCRYPT_GCM_BLOCK_SIZE)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!Last && (InputLength % SYMCRYPT_GCM_BLOCK_SIZE) != 0)
        return STATUS_INVALID_BUFFER_SIZE;

    if (!(Info->dwFlags & BCRYPT_AUTH_MODE_IN_PROGRESS_FLAG))
    {
        if (Chain == NULL)
        {
            Chain = CngAllocate(sizeof(*Chain));
            if (Chain == NULL)
                return STATUS_NO_MEMORY;
            Key->Chain = Chain;
        }

        if (SymCryptGcmExpandKey(&Chain->Key, SymCryptAesBlockCipher,
                                 Key->Secret, Key->SecretLength) != SYMCRYPT_NO_ERROR)
        {
            return STATUS_INVALID_PARAMETER;
        }

        SymCryptGcmInit(&Chain->State, &Chain->Key, Info->pbNonce, Info->cbNonce);
        SymCryptGcmAuthPart(&Chain->State, Info->pbAuthData, Info->cbAuthData);
        Chain->Context = Info->pbMacContext;
        Info->cbAAD = Info->cbAuthData;
        Info->cbData = 0;
        Info->dwFlags |= BCRYPT_AUTH_MODE_IN_PROGRESS_FLAG;
    }
    else if (Chain == NULL || Chain->Context != Info->pbMacContext)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (Encrypt)
        SymCryptGcmEncryptPart(&Chain->State, Input, Output, InputLength);
    else
        SymCryptGcmDecryptPart(&Chain->State, Input, Output, InputLength);

    Info->cbData += InputLength;
    if (!Last)
        return STATUS_SUCCESS;

    Info->dwFlags &= ~BCRYPT_AUTH_MODE_IN_PROGRESS_FLAG;
    Chain->Context = NULL;

    if (Encrypt)
    {
        SymCryptGcmEncryptFinal(&Chain->State, Info->pbTag, Info->cbTag);
        return STATUS_SUCCESS;
    }

    Error = SymCryptGcmDecryptFinal(&Chain->State, Info->pbTag, Info->cbTag);
    return (Error == SYMCRYPT_NO_ERROR) ? STATUS_SUCCESS : STATUS_AUTH_TAG_MISMATCH;
}

static
NTSTATUS
CngAuthenticatedCrypt(
    _Inout_ PCNG_KEY Key,
    _In_ BOOLEAN Encrypt,
    _In_reads_bytes_opt_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_opt_ VOID *PaddingInfo,
    _In_opt_ PUCHAR Iv,
    _In_ ULONG IvLength,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO *Info = PaddingInfo;
    PSYMCRYPT_GCM_EXPANDED_KEY GcmKey;
    SYMCRYPT_ERROR Error;
    NTSTATUS Status;

    *ResultLength = InputLength;

    Status = CngCheckAuthInfo(Key, Info, InputLength, Flags);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Output == NULL)
        return STATUS_SUCCESS;
    if (OutputLength < InputLength)
        return STATUS_BUFFER_TOO_SMALL;

    if (Info->dwFlags & (BCRYPT_AUTH_MODE_CHAIN_CALLS_FLAG | BCRYPT_AUTH_MODE_IN_PROGRESS_FLAG))
    {
        if (Key->Mode != CngModeGcm)
            return STATUS_NOT_SUPPORTED;

        return CngGcmChained(Key, Encrypt, Info, Input, InputLength, Iv, IvLength, Output);
    }

    if (Key->Mode == CngModeCcm)
    {
        if (Encrypt)
        {
            SymCryptCcmEncrypt(SymCryptAesBlockCipher, Key->Expanded, Info->pbNonce, Info->cbNonce,
                               Info->pbAuthData, Info->cbAuthData, Input, Output, InputLength,
                               Info->pbTag, Info->cbTag);
            return STATUS_SUCCESS;
        }

        Error = SymCryptCcmDecrypt(SymCryptAesBlockCipher, Key->Expanded, Info->pbNonce, Info->cbNonce,
                                   Info->pbAuthData, Info->cbAuthData, Input, Output, InputLength,
                                   Info->pbTag, Info->cbTag);
        return (Error == SYMCRYPT_NO_ERROR) ? STATUS_SUCCESS : STATUS_AUTH_TAG_MISMATCH;
    }

    /* The GHASH tables are big, so they are only built for the call */
    GcmKey = CngAllocate(sizeof(*GcmKey));
    if (GcmKey == NULL)
        return STATUS_NO_MEMORY;

    if (SymCryptGcmExpandKey(GcmKey, SymCryptAesBlockCipher, Key->Secret, Key->SecretLength) != SYMCRYPT_NO_ERROR)
    {
        CngFree(GcmKey);
        return STATUS_INVALID_PARAMETER;
    }

    if (Encrypt)
    {
        SymCryptGcmEncrypt(GcmKey, Info->pbNonce, Info->cbNonce, Info->pbAuthData, Info->cbAuthData,
                           Input, Output, InputLength, Info->pbTag, Info->cbTag);
        Status = STATUS_SUCCESS;
    }
    else
    {
        Error = SymCryptGcmDecrypt(GcmKey, Info->pbNonce, Info->cbNonce, Info->pbAuthData, Info->cbAuthData,
                                   Input, Output, InputLength, Info->pbTag, Info->cbTag);
        Status = (Error == SYMCRYPT_NO_ERROR) ? STATUS_SUCCESS : STATUS_AUTH_TAG_MISMATCH;
    }

    SymCryptWipe(GcmKey, sizeof(*GcmKey));
    CngFree(GcmKey);
    return Status;
}

/* PROPERTIES *****************************************************************/

/**
 * @brief
 * Answers the properties of cipher and key derivation providers and their keys.
 */
NTSTATUS
NTAPI
CngQueryKeyProperty(
    _In_ PCNG_PROVIDER Provider,
    _In_opt_ PCNG_KEY Key,
    _In_ LPCWSTR Property,
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    const CNG_CIPHER_INFO *Info;
    CNG_MODE Mode;
    ULONG Value;
    PCWSTR Name;

    if (Provider->Algorithm->Class == CngClassKdf)
    {
        if (Key == NULL && wcscmp(Property, BCRYPT_OBJECT_LENGTH) == 0)
            Value = CngKeyObjectLength(Provider->Algorithm);
        else if (Key == NULL && wcscmp(Property, BCRYPT_KEY_LENGTHS) == 0)
            return CngReturnProperty(&CngKdfKeyLengths, sizeof(CngKdfKeyLengths), Output, OutputLength, ResultLength);
        else if (Key != NULL && wcscmp(Property, BCRYPT_KEY_LENGTH) == 0)
            Value = Key->KeyBits;
        else
            return STATUS_NOT_SUPPORTED;

        return CngReturnProperty(&Value, sizeof(Value), Output, OutputLength, ResultLength);
    }

    Info = CngCipherInfo(Provider);
    Mode = (Key != NULL) ? Key->Mode : Provider->Mode;

    if (wcscmp(Property, BCRYPT_BLOCK_LENGTH) == 0)
    {
        Value = Info->BlockLength;
    }
    else if (wcscmp(Property, BCRYPT_CHAINING_MODE) == 0)
    {
        Name = CngModeNames[Mode];
        return CngReturnProperty(Name, (ULONG)(wcslen(Name) + 1) * sizeof(WCHAR), Output, OutputLength, ResultLength);
    }
    else if (wcscmp(Property, BCRYPT_MESSAGE_BLOCK_LENGTH) == 0)
    {
        Value = CngMessageBlock(Provider, Mode, (Key != NULL) ? Key->MessageBlock : Provider->MessageBlock);
    }
    else if (wcscmp(Property, BCRYPT_AUTH_TAG_LENGTH) == 0)
    {
        if (!CngIsAuthenticated(Mode))
            return STATUS_NOT_SUPPORTED;

        return CngReturnProperty((Mode == CngModeGcm) ? &CngGcmTagLengths : &CngCcmTagLengths,
                                 sizeof(BCRYPT_KEY_LENGTHS_STRUCT), Output, OutputLength, ResultLength);
    }
    else if (Key == NULL && wcscmp(Property, BCRYPT_OBJECT_LENGTH) == 0)
    {
        Value = CngKeyObjectLength(Provider->Algorithm);
    }
    else if (Key == NULL && wcscmp(Property, BCRYPT_KEY_LENGTHS) == 0)
    {
        return CngReturnProperty(&Info->Lengths, sizeof(Info->Lengths), Output, OutputLength, ResultLength);
    }
    else if (Key == NULL && wcscmp(Property, BCRYPT_BLOCK_SIZE_LIST) == 0)
    {
        Value = Info->BlockLength;
    }
    else if (Key != NULL && (wcscmp(Property, BCRYPT_KEY_LENGTH) == 0 || wcscmp(Property, BCRYPT_KEY_STRENGTH) == 0))
    {
        Value = Key->KeyBits;
    }
    else if (Key != NULL && wcscmp(Property, BCRYPT_INITIALIZATION_VECTOR) == 0 && Info->BlockCipher != NULL)
    {
        return CngReturnProperty(Key->Iv, Info->BlockLength, Output, OutputLength, ResultLength);
    }
    else
    {
        return STATUS_NOT_SUPPORTED;
    }

    return CngReturnProperty(&Value, sizeof(Value), Output, OutputLength, ResultLength);
}

/**
 * @brief
 * Sets the properties of cipher, ECC and key derivation providers and keys.
 */
NTSTATUS
NTAPI
CngSetKeyProperty(
    _In_ PCNG_PROVIDER Provider,
    _In_opt_ PCNG_KEY Key,
    _In_ LPCWSTR Property,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength)
{
    const CNG_CURVE *Curve;
    CNG_MODE Mode;
    ULONG Value;

    switch (Provider->Algorithm->Class)
    {
        case CngClassKdf:
            if (Key == NULL)
                return STATUS_NOT_SUPPORTED;
            return CngSetKdfProperty(Key, Property, Input, InputLength);

        case CngClassEcdsa:
        case CngClassEcdh:
            if (Key != NULL || Provider->Algorithm->Curve != NULL || wcscmp(Property, BCRYPT_ECC_CURVE_NAME) != 0)
                return STATUS_NOT_SUPPORTED;

            Curve = NULL;
            if (InputLength >= sizeof(WCHAR) && ((PCWSTR)Input)[InputLength / sizeof(WCHAR) - 1] == UNICODE_NULL)
                Curve = CngFindCurve((PCWSTR)Input);
            if (Curve == NULL)
                return STATUS_NOT_SUPPORTED;

            Provider->Curve = Curve;
            return STATUS_SUCCESS;

        case CngClassCipher:
            break;

        default:
            return STATUS_NOT_SUPPORTED;
    }

    if (wcscmp(Property, BCRYPT_CHAINING_MODE) == 0)
    {
        if (Provider->Algorithm->Cipher == CngCipherRc4)
            return STATUS_NOT_SUPPORTED;

        for (Mode = CngModeCbc; Mode <= CngModeGcm; Mode++)
        {
            if (CngPropertyIs(Input, InputLength, CngModeNames[Mode]))
                break;
        }

        if (Mode > CngModeGcm)
            return STATUS_INVALID_PARAMETER;
        if (CngIsAuthenticated(Mode) && Provider->Algorithm->Cipher != CngCipherAes)
            return STATUS_NOT_SUPPORTED;

        if (Key != NULL)
        {
            Key->Mode = Mode;
            Key->MessageBlock = 1;
        }
        else
        {
            Provider->Mode = Mode;
            Provider->MessageBlock = 1;
        }

        return STATUS_SUCCESS;
    }

    if (wcscmp(Property, BCRYPT_MESSAGE_BLOCK_LENGTH) == 0)
    {
        if (InputLength != sizeof(ULONG))
            return STATUS_INVALID_PARAMETER;

        Value = *(PULONG)Input;
        Mode = (Key != NULL) ? Key->Mode : Provider->Mode;
        if (Mode != CngModeCfb)
            return STATUS_NOT_SUPPORTED;
        if (Value != 1 && Value != CngCipherInfo(Provider)->BlockLength)
            return STATUS_INVALID_PARAMETER;

        if (Key != NULL)
            Key->MessageBlock = Value;
        else
            Provider->MessageBlock = Value;

        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

/* EXPORTS ********************************************************************/

/**
 * @brief
 * Creates a symmetric or key derivation key from secret bytes.
 *
 * @param[in] Algorithm
 * A cipher or key derivation provider.
 *
 * @param[out] Key
 * Receives the key handle.
 *
 * @param[out] KeyObject
 * Optional caller buffer holding the key, at least BCRYPT_OBJECT_LENGTH bytes.
 *
 * @param[in] KeyObjectLength
 * Size of @p KeyObject; zero when it is NULL.
 *
 * @param[in] Secret
 * The key bytes.
 *
 * @param[in] SecretLength
 * Size of @p Secret, which must be one of the BCRYPT_KEY_LENGTHS of the provider.
 *
 * @param[in] Flags
 * Must be zero.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_NOT_SUPPORTED,
 * STATUS_INVALID_PARAMETER, STATUS_BUFFER_TOO_SMALL or STATUS_NO_MEMORY.
 */
NTSTATUS
WINAPI
BCryptGenerateSymmetricKey(
    _Inout_ BCRYPT_ALG_HANDLE Algorithm,
    _Out_ BCRYPT_KEY_HANDLE *Key,
    _Out_writes_bytes_all_opt_(KeyObjectLength) PUCHAR KeyObject,
    _In_ ULONG KeyObjectLength,
    _In_reads_bytes_(SecretLength) PUCHAR Secret,
    _In_ ULONG SecretLength,
    _In_ ULONG Flags)
{
    PCNG_PROVIDER Provider = CngLookupProvider(Algorithm);
    PCNG_KEY NewKey;
    NTSTATUS Status;

    if (Provider == NULL)
        return STATUS_INVALID_HANDLE;
    if (Provider->Algorithm->Class != CngClassCipher && Provider->Algorithm->Class != CngClassKdf)
        return STATUS_NOT_SUPPORTED;
    if (Key == NULL || (Secret == NULL && SecretLength != 0) || Flags != 0)
        return STATUS_INVALID_PARAMETER;
    if (Secret == NULL && Provider->Algorithm->Class == CngClassCipher)
        return STATUS_INVALID_PARAMETER;

    Status = CngCreateKey(Provider, KeyObject, KeyObjectLength, &NewKey);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = CngSetSymmetricSecret(NewKey, Secret, SecretLength);
    if (!NT_SUCCESS(Status))
    {
        CngFreeKey(NewKey);
        return Status;
    }

    *Key = NewKey;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Builds a symmetric key from a BCRYPT_KEY_DATA_BLOB or BCRYPT_OPAQUE_KEY_BLOB.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_INVALID_PARAMETER,
 * STATUS_NOT_SUPPORTED, STATUS_BUFFER_TOO_SMALL or STATUS_NO_MEMORY.
 *
 * @remarks
 * Wrapped blobs are not supported, so @p ImportKey must be NULL.
 */
NTSTATUS
WINAPI
BCryptImportKey(
    _In_ BCRYPT_ALG_HANDLE Algorithm,
    _In_opt_ BCRYPT_KEY_HANDLE ImportKey,
    _In_ LPCWSTR BlobType,
    _Out_ BCRYPT_KEY_HANDLE *Key,
    _Out_writes_bytes_all_opt_(KeyObjectLength) PUCHAR KeyObject,
    _In_ ULONG KeyObjectLength,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_ ULONG Flags)
{
    PCNG_PROVIDER Provider = CngLookupProvider(Algorithm);
    BCRYPT_KEY_DATA_BLOB_HEADER *Header = (BCRYPT_KEY_DATA_BLOB_HEADER *)Input;

    if (Provider == NULL)
        return STATUS_INVALID_HANDLE;
    if (Provider->Algorithm->Class != CngClassCipher)
        return STATUS_NOT_SUPPORTED;
    if (BlobType == NULL || Key == NULL || Input == NULL)
        return STATUS_INVALID_PARAMETER;
    if (wcscmp(BlobType, BCRYPT_KEY_DATA_BLOB) != 0 && wcscmp(BlobType, BCRYPT_OPAQUE_KEY_BLOB) != 0)
        return STATUS_NOT_SUPPORTED;
    if (ImportKey != NULL)
        return STATUS_NOT_SUPPORTED;

    if (InputLength < sizeof(*Header) ||
        Header->dwMagic != BCRYPT_KEY_DATA_BLOB_MAGIC ||
        Header->dwVersion != BCRYPT_KEY_DATA_BLOB_VERSION1 ||
        Header->cbKeyData > InputLength - sizeof(*Header))
    {
        return STATUS_INVALID_PARAMETER;
    }

    return BCryptGenerateSymmetricKey(Algorithm,
                                      Key,
                                      KeyObject,
                                      KeyObjectLength,
                                      Input + sizeof(*Header),
                                      Header->cbKeyData,
                                      Flags);
}

/**
 * @brief
 * Exports a key. Symmetric keys export as BCRYPT_KEY_DATA_BLOB, or the
 * same layout as BCRYPT_OPAQUE_KEY_BLOB, asymmetric keys as their key blobs.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_NOT_SUPPORTED,
 * STATUS_INVALID_PARAMETER or STATUS_BUFFER_TOO_SMALL. With no output only
 * the size is returned.
 */
NTSTATUS
WINAPI
BCryptExportKey(
    _In_ BCRYPT_KEY_HANDLE Key,
    _In_opt_ BCRYPT_KEY_HANDLE ExportKey,
    _In_ LPCWSTR BlobType,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    PCNG_KEY Object = CngLookupKey(Key);
    BCRYPT_KEY_DATA_BLOB_HEADER Header;
    ULONG Needed;

    UNREFERENCED_PARAMETER(Flags);

    if (Object == NULL || !(Object->Flags & CNG_KEY_FINAL))
        return STATUS_INVALID_HANDLE;
    if (BlobType == NULL || ResultLength == NULL)
        return STATUS_INVALID_PARAMETER;
    if (ExportKey != NULL)
        return STATUS_NOT_SUPPORTED;

    switch (Object->Provider->Algorithm->Class)
    {
        case CngClassCipher:
            break;

        case CngClassKdf:
            return STATUS_NOT_SUPPORTED;

        default:
            return CngExportAsymmetricKey(Object, BlobType, Output, OutputLength, ResultLength);
    }

    if (wcscmp(BlobType, BCRYPT_KEY_DATA_BLOB) != 0 && wcscmp(BlobType, BCRYPT_OPAQUE_KEY_BLOB) != 0)
    {
        *ResultLength = 0;
        return STATUS_NOT_SUPPORTED;
    }

    Needed = sizeof(Header) + Object->SecretLength;
    *ResultLength = Needed;
    if (Output == NULL)
        return STATUS_SUCCESS;
    if (OutputLength < Needed)
        return STATUS_BUFFER_TOO_SMALL;

    Header.dwMagic = BCRYPT_KEY_DATA_BLOB_MAGIC;
    Header.dwVersion = BCRYPT_KEY_DATA_BLOB_VERSION1;
    Header.cbKeyData = Object->SecretLength;
    RtlCopyMemory(Output, &Header, sizeof(Header));
    RtlCopyMemory(Output + sizeof(Header), Object->Secret, Object->SecretLength);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Copies a key, including its chaining mode and carried over IV.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_INVALID_PARAMETER,
 * STATUS_BUFFER_TOO_SMALL or STATUS_NO_MEMORY.
 */
NTSTATUS
WINAPI
BCryptDuplicateKey(
    _In_ BCRYPT_KEY_HANDLE Key,
    _Out_ BCRYPT_KEY_HANDLE *NewKey,
    _Out_writes_bytes_all_opt_(KeyObjectLength) PUCHAR KeyObject,
    _In_ ULONG KeyObjectLength,
    _In_ ULONG Flags)
{
    PCNG_KEY Source = CngLookupKey(Key);
    const CNG_ALGORITHM *Algorithm;
    PCNG_KEY Copy;
    NTSTATUS Status;

    if (Source == NULL || !(Source->Flags & CNG_KEY_FINAL))
        return STATUS_INVALID_HANDLE;
    if (NewKey == NULL || Flags != 0)
        return STATUS_INVALID_PARAMETER;

    Algorithm = Source->Provider->Algorithm;
    if (Algorithm->Class != CngClassCipher && Algorithm->Class != CngClassKdf && KeyObject != NULL)
        return STATUS_INVALID_PARAMETER;

    Status = CngCreateKey(Source->Provider, KeyObject, KeyObjectLength, &Copy);
    if (!NT_SUCCESS(Status))
        return Status;

    Copy->Mode = Source->Mode;
    Copy->MessageBlock = Source->MessageBlock;
    RtlCopyMemory(Copy->Iv, Source->Iv, sizeof(Copy->Iv));

    if (Algorithm->Class == CngClassCipher || Algorithm->Class == CngClassKdf)
    {
        Status = CngSetSymmetricSecret(Copy, Source->Secret, Source->SecretLength);

        /* RC4 carries its stream position and HKDF its extracted key */
        if (NT_SUCCESS(Status) && Algorithm->Class == CngClassKdf)
            RtlCopyMemory(Copy->Expanded, Source->Expanded, sizeof(CNG_KDF_STATE));
        else if (NT_SUCCESS(Status) && Algorithm->Cipher == CngCipherRc4)
            RtlCopyMemory(Copy->Expanded, Source->Expanded, sizeof(SYMCRYPT_RC4_STATE));
    }
    else
    {
        Status = CngDuplicateAsymmetricKey(Source, Copy);
    }

    if (!NT_SUCCESS(Status))
    {
        CngFreeKey(Copy);
        return Status;
    }

    *NewKey = Copy;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Destroys a key.
 *
 * @return
 * STATUS_SUCCESS or STATUS_INVALID_HANDLE.
 */
NTSTATUS
WINAPI
BCryptDestroyKey(
    _Inout_ BCRYPT_KEY_HANDLE Key)
{
    PCNG_KEY Object = CngLookupKey(Key);

    if (Object == NULL)
        return STATUS_INVALID_HANDLE;

    CngFreeKey(Object);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Encrypts with a symmetric key in its chaining mode, or with an RSA key.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_NOT_SUPPORTED,
 * STATUS_INVALID_PARAMETER, STATUS_INVALID_BUFFER_SIZE or STATUS_BUFFER_TOO_SMALL.
 *
 * @remarks
 * With no output only the size is returned. CBC and CFB write the last
 * chaining value back to the IV buffer, or keep it in the key without one.
 */
NTSTATUS
WINAPI
BCryptEncrypt(
    _Inout_ BCRYPT_KEY_HANDLE Key,
    _In_reads_bytes_opt_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_opt_ VOID *PaddingInfo,
    _Inout_updates_bytes_opt_(IvLength) PUCHAR Iv,
    _In_ ULONG IvLength,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    PCNG_KEY Object = CngLookupKey(Key);

    if (Object == NULL || !(Object->Flags & CNG_KEY_FINAL))
        return STATUS_INVALID_HANDLE;
    if (ResultLength == NULL || (Input == NULL && InputLength != 0))
        return STATUS_INVALID_PARAMETER;

    switch (Object->Provider->Algorithm->Class)
    {
        case CngClassCipher:
            break;

        case CngClassRsa:
            if (Object->Provider->Algorithm->SignOnly)
                return STATUS_NOT_SUPPORTED;
            return CngAsymmetricEncrypt(Object, Input, InputLength, PaddingInfo,
                                        Output, OutputLength, ResultLength, Flags);

        default:
            return STATUS_NOT_SUPPORTED;
    }

    if (CngIsAuthenticated(Object->Mode))
    {
        return CngAuthenticatedCrypt(Object, TRUE, Input, InputLength, PaddingInfo, Iv, IvLength,
                                     Output, OutputLength, ResultLength, Flags);
    }

    return CngBlockEncrypt(Object, Input, InputLength, Iv, IvLength, Output, OutputLength, ResultLength, Flags);
}

/**
 * @brief
 * Decrypts with a symmetric key in its chaining mode, or with an RSA key.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_NOT_SUPPORTED,
 * STATUS_INVALID_PARAMETER, STATUS_INVALID_BUFFER_SIZE, STATUS_BUFFER_TOO_SMALL,
 * STATUS_DATA_ERROR for bad padding or STATUS_AUTH_TAG_MISMATCH.
 */
NTSTATUS
WINAPI
BCryptDecrypt(
    _Inout_ BCRYPT_KEY_HANDLE Key,
    _In_reads_bytes_opt_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_opt_ VOID *PaddingInfo,
    _Inout_updates_bytes_opt_(IvLength) PUCHAR Iv,
    _In_ ULONG IvLength,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    PCNG_KEY Object = CngLookupKey(Key);

    if (Object == NULL || !(Object->Flags & CNG_KEY_FINAL))
        return STATUS_INVALID_HANDLE;
    if (ResultLength == NULL || (Input == NULL && InputLength != 0))
        return STATUS_INVALID_PARAMETER;

    switch (Object->Provider->Algorithm->Class)
    {
        case CngClassCipher:
            break;

        case CngClassRsa:
            if (Object->Provider->Algorithm->SignOnly)
                return STATUS_NOT_SUPPORTED;
            return CngAsymmetricDecrypt(Object, Input, InputLength, PaddingInfo,
                                        Output, OutputLength, ResultLength, Flags);

        default:
            return STATUS_NOT_SUPPORTED;
    }

    if (CngIsAuthenticated(Object->Mode))
    {
        return CngAuthenticatedCrypt(Object, FALSE, Input, InputLength, PaddingInfo, Iv, IvLength,
                                     Output, OutputLength, ResultLength, Flags);
    }

    return CngBlockDecrypt(Object, Input, InputLength, Iv, IvLength, Output, OutputLength, ResultLength, Flags);
}

/* EOF */
