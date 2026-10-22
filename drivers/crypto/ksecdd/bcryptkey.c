/*
 * PROJECT:     ReactOS Kernel Security Support Provider Interface Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Cryptography Next Generation keys: AES, RSA and ECDSA
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ksecdd.h"
#include "bcryptp.h"

#include <debug.h>

/* DEFINES ********************************************************************/

#define KSEC_AES_BLOCK          SYMCRYPT_AES_BLOCK_SIZE
#define KSEC_RSA_MIN_BITS       512
#define KSEC_RSA_MAX_BITS       16384
#define KSEC_RSA_STEP_BITS      8
#define KSEC_NO_KEY_VALIDATION  0x00000008

/* Blob layout errors come back as this HRESULT, as on Windows */
#ifndef NTE_BAD_DATA
#define NTE_BAD_DATA            ((NTSTATUS)0x80090005L)
#endif

/* The fixed headers in front of the RSA and ECC key blobs */
#define KSEC_RSA_HEADER         sizeof(BCRYPT_RSAKEY_BLOB)
#define KSEC_ECC_HEADER         sizeof(BCRYPT_ECCKEY_BLOB)

/* TYPES **********************************************************************/

typedef struct _KSEC_SIGNATURE_HASH
{
    PCWSTR Name;
    const PCSYMCRYPT_HASH *Hash;
    PCSYMCRYPT_OID Oids;
    SIZE_T OidCount;
} KSEC_SIGNATURE_HASH, *PKSEC_SIGNATURE_HASH;

/* GLOBALS ********************************************************************/

static const KSEC_SIGNATURE_HASH KsecSignatureHashes[] =
{
    { BCRYPT_MD5_ALGORITHM, &SymCryptMd5Algorithm, SymCryptMd5OidList, SYMCRYPT_MD5_OID_COUNT },
    { BCRYPT_SHA1_ALGORITHM, &SymCryptSha1Algorithm, SymCryptSha1OidList, SYMCRYPT_SHA1_OID_COUNT },
    { BCRYPT_SHA256_ALGORITHM, &SymCryptSha256Algorithm, SymCryptSha256OidList, SYMCRYPT_SHA256_OID_COUNT },
    { BCRYPT_SHA384_ALGORITHM, &SymCryptSha384Algorithm, SymCryptSha384OidList, SYMCRYPT_SHA384_OID_COUNT },
    { BCRYPT_SHA512_ALGORITHM, &SymCryptSha512Algorithm, SymCryptSha512OidList, SYMCRYPT_SHA512_OID_COUNT },
};

/* HELPERS ********************************************************************/

PKSEC_KEY
KsecReferenceKey(
    _In_opt_ BCRYPT_HANDLE Handle)
{
    PKSEC_KEY Key = (PKSEC_KEY)Handle;

    if (Key == NULL || Key->Magic != KSEC_KEY_MAGIC)
        return NULL;

    return Key;
}

static
const KSEC_SIGNATURE_HASH *
KsecFindSignatureHash(
    _In_ LPCWSTR Name)
{
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(KsecSignatureHashes); Index++)
    {
        if (_wcsicmp(Name, KsecSignatureHashes[Index].Name) == 0)
            return &KsecSignatureHashes[Index];
    }

    return NULL;
}

static
ULONG
KsecEccFieldLength(
    _In_ PKSEC_PROVIDER Provider)
{
    return (Provider->Algorithm->CurveBits + 7) / 8;
}

/* The string compare is exact, and a value without its terminator is fine */
static
BOOLEAN
KsecPropertyIs(
    _In_reads_bytes_(Length) PUCHAR Input,
    _In_ ULONG Length,
    _In_ PCWSTR Value)
{
    SIZE_T Chars = wcslen(Value);
    PCWSTR Text = (PCWSTR)Input;

    if (Length != Chars * sizeof(WCHAR) && Length != (Chars + 1) * sizeof(WCHAR))
        return FALSE;
    if (wcsncmp(Text, Value, Chars) != 0)
        return FALSE;

    return (Length == Chars * sizeof(WCHAR)) || (Text[Chars] == UNICODE_NULL);
}

static
PKSEC_KEY
KsecCreateKey(
    _In_ PKSEC_PROVIDER Provider,
    _Out_writes_bytes_opt_(ObjectLength) PUCHAR Object,
    _In_ ULONG ObjectLength,
    _Out_ NTSTATUS *Status)
{
    PVOID Allocation = NULL;
    PKSEC_KEY Key;

    if (Object != NULL)
    {
        if (ObjectLength < KSEC_KEY_OBJECT_LENGTH)
        {
            *Status = STATUS_BUFFER_TOO_SMALL;
            return NULL;
        }
    }
    else
    {
        Allocation = ExAllocatePoolWithTag(NonPagedPool, KSEC_KEY_OBJECT_LENGTH, TAG_KSEC_BCRYPT);
        if (Allocation == NULL)
        {
            *Status = STATUS_NO_MEMORY;
            return NULL;
        }
        Object = Allocation;
    }

    Key = (PKSEC_KEY)ALIGN_UP_POINTER_BY(Object, SYMCRYPT_ALIGN_VALUE);
    RtlZeroMemory(Key, sizeof(*Key));
    Key->Magic = KSEC_KEY_MAGIC;
    Key->Provider = Provider;
    Key->Allocation = Allocation;
    Key->ChainingMode = Provider->ChainingMode;
    InterlockedIncrement(&Provider->References);

    *Status = STATUS_SUCCESS;
    return Key;
}

static
VOID
KsecFreeKey(
    _In_ PKSEC_KEY Key)
{
    PKSEC_PROVIDER Provider = Key->Provider;
    PVOID Allocation = Key->Allocation;

    if (Key->Rsa != NULL)
        SymCryptRsakeyFree(Key->Rsa);
    if (Key->Ec != NULL)
        SymCryptEckeyFree(Key->Ec);

    SymCryptWipe(Key, sizeof(*Key));
    if (Allocation != NULL)
        ExFreePoolWithTag(Allocation, TAG_KSEC_BCRYPT);

    KsecReleaseProvider(Provider);
}

static
ULONG
KsecKeyBits(
    _In_ PKSEC_KEY Key)
{
    if (Key->Rsa != NULL)
        return SymCryptRsakeyModulusBits(Key->Rsa);

    return Key->KeyBits;
}

/**
 * @brief
 * Answers the properties of cipher, RSA and ECDSA providers and their keys.
 */
NTSTATUS
KsecQueryKeyProperty(
    _In_ PKSEC_PROVIDER Provider,
    _In_opt_ PKSEC_KEY Key,
    _In_ LPCWSTR Property,
    _Out_ const VOID **Value,
    _Out_ PULONG Size,
    _Out_writes_bytes_(sizeof(BCRYPT_KEY_LENGTHS_STRUCT)) PVOID Scratch)
{
    BCRYPT_KEY_LENGTHS_STRUCT *Lengths = Scratch;
    KSEC_ALGORITHM_CLASS Class = Provider->Algorithm->Class;
    KSEC_CHAINING_MODE Mode;

    *Value = &Lengths->dwMinLength;
    *Size = sizeof(ULONG);

    if (_wcsicmp(Property, BCRYPT_KEY_LENGTHS) == 0)
    {
        if (Class == KsecClassCipher)
        {
            Lengths->dwMinLength = 128;
            Lengths->dwMaxLength = 256;
            Lengths->dwIncrement = 64;
        }
        else if (Class == KsecClassRsa)
        {
            Lengths->dwMinLength = KSEC_RSA_MIN_BITS;
            Lengths->dwMaxLength = KSEC_RSA_MAX_BITS;
            Lengths->dwIncrement = KSEC_RSA_STEP_BITS;
        }
        else
        {
            Lengths->dwMinLength = Provider->Algorithm->CurveBits;
            Lengths->dwMaxLength = Provider->Algorithm->CurveBits;
            Lengths->dwIncrement = 0;
        }

        *Value = Lengths;
        *Size = sizeof(*Lengths);
        return STATUS_SUCCESS;
    }

    if (_wcsicmp(Property, BCRYPT_KEY_LENGTH) == 0 && Key != NULL)
    {
        Lengths->dwMinLength = KsecKeyBits(Key);
        return STATUS_SUCCESS;
    }

    if (Class == KsecClassCipher)
    {
        if (_wcsicmp(Property, BCRYPT_BLOCK_LENGTH) == 0)
        {
            Lengths->dwMinLength = KSEC_AES_BLOCK;
            return STATUS_SUCCESS;
        }

        if (_wcsicmp(Property, BCRYPT_OBJECT_LENGTH) == 0)
        {
            Lengths->dwMinLength = KSEC_KEY_OBJECT_LENGTH;
            return STATUS_SUCCESS;
        }

        if (_wcsicmp(Property, BCRYPT_CHAINING_MODE) == 0)
        {
            Mode = (Key != NULL) ? Key->ChainingMode : Provider->ChainingMode;
            *Value = (Mode == KsecChainingEcb) ? BCRYPT_CHAIN_MODE_ECB : BCRYPT_CHAIN_MODE_CBC;
            *Size = (ULONG)(wcslen(*Value) + 1) * sizeof(WCHAR);
            return STATUS_SUCCESS;
        }

        return STATUS_NOT_SUPPORTED;
    }

    if (_wcsicmp(Property, BCRYPT_SIGNATURE_LENGTH) == 0 && Key != NULL)
    {
        if (Class == KsecClassRsa)
            Lengths->dwMinLength = (KsecKeyBits(Key) + 7) / 8;
        else
            Lengths->dwMinLength = 2 * KsecEccFieldLength(Provider);
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

/* SYMMETRIC KEYS *************************************************************/

/**
 * @brief
 * Creates an AES key from secret bytes.
 *
 * @param[in] Algorithm
 * An AES provider.
 *
 * @param[out] Key
 * Receives the key handle.
 *
 * @param[in] KeyObject
 * Optional caller owned buffer that backs the key, at least ObjectLength bytes.
 *
 * @param[in] KeyObjectLength
 * Size of @p KeyObject, in bytes.
 *
 * @param[in] Secret
 * The key bytes, 16, 24 or 32 of them.
 *
 * @param[in] SecretLength
 * Size of @p Secret, in bytes.
 *
 * @param[in] Flags
 * Must be zero.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_INVALID_PARAMETER,
 * STATUS_BUFFER_TOO_SMALL or STATUS_NO_MEMORY.
 */
NTSTATUS
WINAPI
BCryptGenerateSymmetricKey(
    _In_ BCRYPT_ALG_HANDLE Algorithm,
    _Out_ BCRYPT_KEY_HANDLE *Key,
    _Out_writes_bytes_opt_(KeyObjectLength) PUCHAR KeyObject,
    _In_ ULONG KeyObjectLength,
    _In_reads_bytes_(SecretLength) PUCHAR Secret,
    _In_ ULONG SecretLength,
    _In_ ULONG Flags)
{
    PKSEC_PROVIDER Provider = KsecReferenceProvider(Algorithm);
    PKSEC_KEY NewKey;
    NTSTATUS Status;

    if (Provider == NULL)
        return STATUS_INVALID_HANDLE;
    if (Provider->Algorithm->Class != KsecClassCipher)
        return STATUS_NOT_SUPPORTED;
    if (Key == NULL || Secret == NULL || Flags != 0)
        return STATUS_INVALID_PARAMETER;
    if (SecretLength != 16 && SecretLength != 24 && SecretLength != 32)
        return STATUS_INVALID_PARAMETER;

    NewKey = KsecCreateKey(Provider, KeyObject, KeyObjectLength, &Status);
    if (NewKey == NULL)
        return Status;

    if (SymCryptAesExpandKey(&NewKey->Aes, Secret, SecretLength) != SYMCRYPT_NO_ERROR)
    {
        KsecFreeKey(NewKey);
        return STATUS_INVALID_PARAMETER;
    }

    RtlCopyMemory(NewKey->Secret, Secret, SecretLength);
    NewKey->SecretLength = SecretLength;
    NewKey->KeyBits = SecretLength * 8;
    NewKey->Finalized = TRUE;

    *Key = NewKey;
    return STATUS_SUCCESS;
}

/* Checks shared by encryption and decryption, and the chaining value to start from */
static
NTSTATUS
KsecPrepareCipher(
    _In_ PKSEC_KEY Key,
    _In_opt_ PUCHAR Iv,
    _In_ ULONG IvLength,
    _In_ ULONG Flags,
    _Out_writes_(KSEC_AES_BLOCK) PUCHAR Chain)
{
    if (Flags & ~BCRYPT_BLOCK_PADDING)
        return STATUS_INVALID_PARAMETER;

    if (Key->ChainingMode == KsecChainingEcb)
    {
        if (Iv != NULL)
            return STATUS_INVALID_PARAMETER;
    }
    else if (Iv != NULL && IvLength != KSEC_AES_BLOCK)
    {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    /* A missing IV chains from zero, and nothing is handed back */
    RtlZeroMemory(Chain, KSEC_AES_BLOCK);
    if (Iv != NULL)
        RtlCopyMemory(Chain, Iv, KSEC_AES_BLOCK);

    return STATUS_SUCCESS;
}

static
VOID
KsecAesEncryptBlocks(
    _In_ PKSEC_KEY Key,
    _Inout_updates_(KSEC_AES_BLOCK) PUCHAR Chain,
    _In_reads_(Length) PCBYTE Source,
    _Out_writes_(Length) PUCHAR Destination,
    _In_ SIZE_T Length)
{
    if (Length == 0)
        return;

    if (Key->ChainingMode == KsecChainingEcb)
        SymCryptAesEcbEncrypt(&Key->Aes, Source, Destination, Length);
    else
        SymCryptAesCbcEncrypt(&Key->Aes, Chain, Source, Destination, Length);
}

static
VOID
KsecAesDecryptBlocks(
    _In_ PKSEC_KEY Key,
    _Inout_updates_(KSEC_AES_BLOCK) PUCHAR Chain,
    _In_reads_(Length) PCBYTE Source,
    _Out_writes_(Length) PUCHAR Destination,
    _In_ SIZE_T Length)
{
    if (Length == 0)
        return;

    if (Key->ChainingMode == KsecChainingEcb)
        SymCryptAesEcbDecrypt(&Key->Aes, Source, Destination, Length);
    else
        SymCryptAesCbcDecrypt(&Key->Aes, Chain, Source, Destination, Length);
}

/**
 * @brief
 * Encrypts with an AES key, in CBC or ECB mode, optionally adding PKCS #7 padding.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_NOT_SUPPORTED,
 * STATUS_INVALID_PARAMETER, STATUS_INVALID_BUFFER_SIZE or STATUS_BUFFER_TOO_SMALL.
 *
 * @remarks
 * With no output buffer only the needed size is returned. A CBC IV buffer
 * receives the last ciphertext block, so the next call carries on from it.
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
    PKSEC_KEY Object = KsecReferenceKey(Key);
    UCHAR Chain[KSEC_AES_BLOCK], Last[KSEC_AES_BLOCK];
    ULONG Whole, Remainder, Needed;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(PaddingInfo);

    if (Object == NULL || !Object->Finalized)
        return STATUS_INVALID_HANDLE;
    if (Object->Provider->Algorithm->Class != KsecClassCipher)
        return STATUS_NOT_SUPPORTED;
    if (ResultLength == NULL || (Input == NULL && InputLength != 0))
        return STATUS_INVALID_PARAMETER;

    Status = KsecPrepareCipher(Object, Iv, IvLength, Flags, Chain);
    if (!NT_SUCCESS(Status))
        return Status;

    Whole = InputLength - (InputLength % KSEC_AES_BLOCK);
    Remainder = InputLength - Whole;
    Needed = (Flags & BCRYPT_BLOCK_PADDING) ? Whole + KSEC_AES_BLOCK : InputLength;

    /* The size is reported even for input that is not whole blocks */
    *ResultLength = Needed;
    if (!(Flags & BCRYPT_BLOCK_PADDING) && Remainder != 0)
        return STATUS_INVALID_BUFFER_SIZE;
    if (Output == NULL)
        return STATUS_SUCCESS;
    if (OutputLength < Needed)
        return STATUS_BUFFER_TOO_SMALL;

    /* The tail is read before the whole blocks are written, as they may share a buffer */
    if (Flags & BCRYPT_BLOCK_PADDING)
    {
        RtlCopyMemory(Last, Input + Whole, Remainder);
        RtlFillMemory(Last + Remainder, KSEC_AES_BLOCK - Remainder, (UCHAR)(KSEC_AES_BLOCK - Remainder));
    }

    KsecAesEncryptBlocks(Object, Chain, Input, Output, Whole);
    if (Flags & BCRYPT_BLOCK_PADDING)
        KsecAesEncryptBlocks(Object, Chain, Last, Output + Whole, KSEC_AES_BLOCK);

    if (Iv != NULL && Object->ChainingMode == KsecChainingCbc)
        RtlCopyMemory(Iv, Chain, KSEC_AES_BLOCK);

    SymCryptWipe(Last, sizeof(Last));
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Decrypts with an AES key, in CBC or ECB mode, optionally removing PKCS #7 padding.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_NOT_SUPPORTED,
 * STATUS_INVALID_PARAMETER, STATUS_INVALID_BUFFER_SIZE, STATUS_BUFFER_TOO_SMALL
 * or STATUS_DATA_ERROR for padding that does not check out.
 *
 * @remarks
 * With padding the output only needs room for the real plaintext, so the
 * last block is decrypted first to learn its length.
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
    PKSEC_KEY Object = KsecReferenceKey(Key);
    UCHAR Chain[KSEC_AES_BLOCK], LastChain[KSEC_AES_BLOCK];
    UCHAR LastCipher[KSEC_AES_BLOCK], LastPlain[KSEC_AES_BLOCK];
    ULONG Leading, Pad, Plain, Index;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(PaddingInfo);

    if (Object == NULL || !Object->Finalized)
        return STATUS_INVALID_HANDLE;
    if (Object->Provider->Algorithm->Class != KsecClassCipher)
        return STATUS_NOT_SUPPORTED;
    if (ResultLength == NULL || (Input == NULL && InputLength != 0))
        return STATUS_INVALID_PARAMETER;

    /* Unlike encryption, decryption ignores flags it does not know */
    Flags &= BCRYPT_BLOCK_PADDING;

    Status = KsecPrepareCipher(Object, Iv, IvLength, 0, Chain);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Ciphertext is whole blocks with or without padding */
    *ResultLength = InputLength;
    if ((InputLength % KSEC_AES_BLOCK) != 0 || ((Flags & BCRYPT_BLOCK_PADDING) && InputLength == 0))
        return STATUS_INVALID_BUFFER_SIZE;

    if (Output == NULL)
    {
        *ResultLength = InputLength;
        return STATUS_SUCCESS;
    }

    if (!(Flags & BCRYPT_BLOCK_PADDING))
    {
        *ResultLength = InputLength;
        if (OutputLength < InputLength)
            return STATUS_BUFFER_TOO_SMALL;

        KsecAesDecryptBlocks(Object, Chain, Input, Output, InputLength);
        if (Iv != NULL && Object->ChainingMode == KsecChainingCbc)
            RtlCopyMemory(Iv, Chain, KSEC_AES_BLOCK);
        return STATUS_SUCCESS;
    }

    Leading = InputLength - KSEC_AES_BLOCK;
    RtlCopyMemory(LastCipher, Input + Leading, KSEC_AES_BLOCK);
    if (Leading != 0)
        RtlCopyMemory(LastChain, Input + Leading - KSEC_AES_BLOCK, KSEC_AES_BLOCK);
    else
        RtlCopyMemory(LastChain, Chain, KSEC_AES_BLOCK);

    KsecAesDecryptBlocks(Object, LastChain, LastCipher, LastPlain, KSEC_AES_BLOCK);

    Pad = LastPlain[KSEC_AES_BLOCK - 1];
    if (Pad == 0 || Pad > KSEC_AES_BLOCK)
    {
        SymCryptWipe(LastPlain, sizeof(LastPlain));
        return STATUS_DATA_ERROR;
    }
    for (Index = KSEC_AES_BLOCK - Pad; Index < KSEC_AES_BLOCK; Index++)
    {
        if (LastPlain[Index] != Pad)
        {
            SymCryptWipe(LastPlain, sizeof(LastPlain));
            return STATUS_DATA_ERROR;
        }
    }

    Plain = InputLength - Pad;
    *ResultLength = Plain;
    if (OutputLength < Plain)
    {
        SymCryptWipe(LastPlain, sizeof(LastPlain));
        return STATUS_BUFFER_TOO_SMALL;
    }

    KsecAesDecryptBlocks(Object, Chain, Input, Output, Leading);
    RtlCopyMemory(Output + Leading, LastPlain, KSEC_AES_BLOCK - Pad);

    if (Iv != NULL && Object->ChainingMode == KsecChainingCbc)
        RtlCopyMemory(Iv, LastCipher, KSEC_AES_BLOCK);

    SymCryptWipe(LastPlain, sizeof(LastPlain));
    return STATUS_SUCCESS;
}

/* ASYMMETRIC KEYS ************************************************************/

static
NTSTATUS
KsecImportRsa(
    _In_ PKSEC_KEY Key,
    _In_ LPCWSTR BlobType,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength)
{
    BCRYPT_RSAKEY_BLOB *Header = (BCRYPT_RSAKEY_BLOB *)Input;
    SYMCRYPT_RSA_PARAMS Params;
    PCBYTE Primes[2];
    SIZE_T PrimeLengths[2];
    ULONG Expected, Needed, Index;
    BOOLEAN Private;
    UINT64 Exponent = 0;
    PUCHAR Cursor;

    if (_wcsicmp(BlobType, BCRYPT_RSAPUBLIC_BLOB) == 0)
    {
        Expected = BCRYPT_RSAPUBLIC_MAGIC;
        Private = FALSE;
    }
    else if (_wcsicmp(BlobType, BCRYPT_RSAPRIVATE_BLOB) == 0)
    {
        Expected = BCRYPT_RSAPRIVATE_MAGIC;
        Private = TRUE;
    }
    else if (_wcsicmp(BlobType, BCRYPT_RSAFULLPRIVATE_BLOB) == 0)
    {
        Expected = BCRYPT_RSAFULLPRIVATE_MAGIC;
        Private = TRUE;
    }
    else
    {
        return STATUS_NOT_SUPPORTED;
    }

    if (InputLength < KSEC_RSA_HEADER || Header->Magic != Expected)
        return NTE_BAD_DATA;
    if (Header->cbPublicExp == 0 || Header->cbPublicExp > sizeof(Exponent) || Header->cbModulus == 0)
        return NTE_BAD_DATA;

    Needed = KSEC_RSA_HEADER + Header->cbPublicExp + Header->cbModulus;
    if (Private)
        Needed += Header->cbPrime1 + Header->cbPrime2;
    if (Needed > InputLength || (Private && (Header->cbPrime1 == 0 || Header->cbPrime2 == 0)))
        return NTE_BAD_DATA;

    Cursor = Input + KSEC_RSA_HEADER;
    for (Index = 0; Index < Header->cbPublicExp; Index++)
        Exponent = (Exponent << 8) | Cursor[Index];
    Cursor += Header->cbPublicExp;

    Params.version = 1;
    Params.nBitsOfModulus = Header->BitLength;
    Params.nPrimes = Private ? 2 : 0;
    Params.nPubExp = 1;

    Key->Rsa = SymCryptRsakeyAllocate(&Params, 0);
    if (Key->Rsa == NULL)
        return STATUS_INVALID_PARAMETER;

    Primes[0] = Cursor + Header->cbModulus;
    PrimeLengths[0] = Header->cbPrime1;
    Primes[1] = Primes[0] + Header->cbPrime1;
    PrimeLengths[1] = Header->cbPrime2;

    if (SymCryptRsakeySetValue(Cursor,
                               Header->cbModulus,
                               &Exponent,
                               1,
                               Private ? Primes : NULL,
                               Private ? PrimeLengths : NULL,
                               Private ? 2 : 0,
                               SYMCRYPT_NUMBER_FORMAT_MSB_FIRST,
                               SYMCRYPT_FLAG_KEY_NO_FIPS | SYMCRYPT_FLAG_RSAKEY_SIGN | SYMCRYPT_FLAG_RSAKEY_ENCRYPT,
                               Key->Rsa) != SYMCRYPT_NO_ERROR)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
KsecImportEcc(
    _In_ PKSEC_KEY Key,
    _In_ LPCWSTR BlobType,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength)
{
    BCRYPT_ECCKEY_BLOB *Header = (BCRYPT_ECCKEY_BLOB *)Input;
    const KSEC_ALGORITHM *Algorithm = Key->Provider->Algorithm;
    ULONG Field = KsecEccFieldLength(Key->Provider);
    PUCHAR Point = Input + KSEC_ECC_HEADER;
    BOOLEAN Private;

    if (_wcsicmp(BlobType, BCRYPT_ECCPUBLIC_BLOB) == 0)
        Private = FALSE;
    else if (_wcsicmp(BlobType, BCRYPT_ECCPRIVATE_BLOB) == 0)
        Private = TRUE;
    else
        return STATUS_NOT_SUPPORTED;

    if (InputLength < KSEC_ECC_HEADER)
        return STATUS_INVALID_PARAMETER;
    if (Header->dwMagic != (Private ? Algorithm->PrivateMagic : Algorithm->PublicMagic) || Header->cbKey != Field)
        return STATUS_INVALID_PARAMETER;
    if (InputLength < KSEC_ECC_HEADER + (Private ? 3 : 2) * Field)
        return STATUS_INVALID_PARAMETER;

    Key->Ec = SymCryptEckeyAllocate(Key->Provider->Curve);
    if (Key->Ec == NULL)
        return STATUS_NO_MEMORY;

    if (SymCryptEckeySetValue(Private ? Point + 2 * Field : NULL,
                              Private ? Field : 0,
                              Point,
                              2 * Field,
                              SYMCRYPT_NUMBER_FORMAT_MSB_FIRST,
                              SYMCRYPT_ECPOINT_FORMAT_XY,
                              SYMCRYPT_FLAG_KEY_NO_FIPS | SYMCRYPT_FLAG_ECKEY_ECDSA,
                              Key->Ec) != SYMCRYPT_NO_ERROR)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Imports an RSA or ECDSA public or private key blob.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_NOT_SUPPORTED,
 * STATUS_INVALID_PARAMETER, NTE_BAD_DATA or STATUS_NO_MEMORY.
 */
NTSTATUS
WINAPI
BCryptImportKeyPair(
    _In_ BCRYPT_ALG_HANDLE Algorithm,
    _In_opt_ BCRYPT_KEY_HANDLE ImportKey,
    _In_ LPCWSTR BlobType,
    _Out_ BCRYPT_KEY_HANDLE *Key,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_ ULONG Flags)
{
    PKSEC_PROVIDER Provider = KsecReferenceProvider(Algorithm);
    PKSEC_KEY NewKey;
    NTSTATUS Status;

    if (Provider == NULL)
        return STATUS_INVALID_HANDLE;
    if (Provider->Algorithm->Class != KsecClassRsa && Provider->Algorithm->Class != KsecClassEcdsa)
        return STATUS_NOT_SUPPORTED;
    if (ImportKey != NULL || BlobType == NULL || Key == NULL || Input == NULL ||
        (Flags & ~KSEC_NO_KEY_VALIDATION))
    {
        return STATUS_INVALID_PARAMETER;
    }

    NewKey = KsecCreateKey(Provider, NULL, 0, &Status);
    if (NewKey == NULL)
        return Status;

    if (Provider->Algorithm->Class == KsecClassRsa)
        Status = KsecImportRsa(NewKey, BlobType, Input, InputLength);
    else
        Status = KsecImportEcc(NewKey, BlobType, Input, InputLength);

    if (!NT_SUCCESS(Status))
    {
        KsecFreeKey(NewKey);
        return Status;
    }

    NewKey->KeyBits = (Provider->Algorithm->Class == KsecClassRsa) ?
                      SymCryptRsakeyModulusBits(NewKey->Rsa) : Provider->Algorithm->CurveBits;
    NewKey->Finalized = TRUE;

    *Key = NewKey;
    return STATUS_SUCCESS;
}

static
NTSTATUS
KsecExportRsa(
    _In_ PKSEC_KEY Key,
    _In_ LPCWSTR BlobType,
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    BCRYPT_RSAKEY_BLOB Header;
    PBYTE Primes[2];
    SIZE_T PrimeLengths[2];
    UINT64 Exponent;
    ULONG Needed, Index;
    BOOLEAN Private;
    PUCHAR Cursor;

    if (_wcsicmp(BlobType, BCRYPT_RSAPUBLIC_BLOB) == 0)
        Private = FALSE;
    else if (_wcsicmp(BlobType, BCRYPT_RSAPRIVATE_BLOB) == 0)
        Private = TRUE;
    else
        return STATUS_NOT_SUPPORTED;

    if (Private && !SymCryptRsakeyHasPrivateKey(Key->Rsa))
        return STATUS_INVALID_PARAMETER;

    if (SymCryptRsakeyGetValue(Key->Rsa, NULL, 0, &Exponent, 1, NULL, NULL, 0,
                               SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, 0) != SYMCRYPT_NO_ERROR)
    {
        return STATUS_INTERNAL_ERROR;
    }

    RtlZeroMemory(&Header, sizeof(Header));
    Header.Magic = Private ? BCRYPT_RSAPRIVATE_MAGIC : BCRYPT_RSAPUBLIC_MAGIC;
    Header.BitLength = SymCryptRsakeyModulusBits(Key->Rsa);
    Header.cbModulus = SymCryptRsakeySizeofModulus(Key->Rsa);
    for (Header.cbPublicExp = sizeof(Exponent); Header.cbPublicExp > 1; Header.cbPublicExp--)
    {
        if (Exponent >> ((Header.cbPublicExp - 1) * 8))
            break;
    }
    if (Private)
    {
        Header.cbPrime1 = SymCryptRsakeySizeofPrime(Key->Rsa, 0);
        Header.cbPrime2 = SymCryptRsakeySizeofPrime(Key->Rsa, 1);
    }

    Needed = KSEC_RSA_HEADER + Header.cbPublicExp + Header.cbModulus + Header.cbPrime1 + Header.cbPrime2;
    *ResultLength = Needed;
    if (Output == NULL)
        return STATUS_SUCCESS;
    if (OutputLength < Needed)
        return STATUS_BUFFER_TOO_SMALL;

    RtlCopyMemory(Output, &Header, sizeof(Header));
    Cursor = Output + KSEC_RSA_HEADER;
    for (Index = 0; Index < Header.cbPublicExp; Index++)
        Cursor[Index] = (UCHAR)(Exponent >> ((Header.cbPublicExp - 1 - Index) * 8));
    Cursor += Header.cbPublicExp;

    Primes[0] = Cursor + Header.cbModulus;
    PrimeLengths[0] = Header.cbPrime1;
    Primes[1] = Primes[0] + Header.cbPrime1;
    PrimeLengths[1] = Header.cbPrime2;

    if (SymCryptRsakeyGetValue(Key->Rsa,
                               Cursor,
                               Header.cbModulus,
                               NULL,
                               0,
                               Private ? Primes : NULL,
                               Private ? PrimeLengths : NULL,
                               Private ? 2 : 0,
                               SYMCRYPT_NUMBER_FORMAT_MSB_FIRST,
                               0) != SYMCRYPT_NO_ERROR)
    {
        return STATUS_INTERNAL_ERROR;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
KsecExportEcc(
    _In_ PKSEC_KEY Key,
    _In_ LPCWSTR BlobType,
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    const KSEC_ALGORITHM *Algorithm = Key->Provider->Algorithm;
    ULONG Field = KsecEccFieldLength(Key->Provider);
    BCRYPT_ECCKEY_BLOB Header;
    PUCHAR Point;
    BOOLEAN Private;
    ULONG Needed;

    if (_wcsicmp(BlobType, BCRYPT_ECCPUBLIC_BLOB) == 0)
        Private = FALSE;
    else if (_wcsicmp(BlobType, BCRYPT_ECCPRIVATE_BLOB) == 0)
        Private = TRUE;
    else
        return STATUS_NOT_SUPPORTED;

    if (Private && !SymCryptEckeyHasPrivateKey(Key->Ec))
        return STATUS_INVALID_PARAMETER;

    Needed = KSEC_ECC_HEADER + (Private ? 3 : 2) * Field;
    *ResultLength = Needed;
    if (Output == NULL)
        return STATUS_SUCCESS;
    if (OutputLength < Needed)
        return STATUS_BUFFER_TOO_SMALL;

    Header.dwMagic = Private ? Algorithm->PrivateMagic : Algorithm->PublicMagic;
    Header.cbKey = Field;
    RtlCopyMemory(Output, &Header, sizeof(Header));
    Point = Output + KSEC_ECC_HEADER;

    if (SymCryptEckeyGetValue(Key->Ec,
                              Private ? Point + 2 * Field : NULL,
                              Private ? Field : 0,
                              Point,
                              2 * Field,
                              SYMCRYPT_NUMBER_FORMAT_MSB_FIRST,
                              SYMCRYPT_ECPOINT_FORMAT_XY,
                              0) != SYMCRYPT_NO_ERROR)
    {
        return STATUS_INTERNAL_ERROR;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Exports a key as an RSA, ECC or key data blob.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_NOT_SUPPORTED,
 * STATUS_INVALID_PARAMETER or STATUS_BUFFER_TOO_SMALL. With no output buffer
 * only the size is returned.
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
    PKSEC_KEY Object = KsecReferenceKey(Key);
    BCRYPT_KEY_DATA_BLOB_HEADER Header;
    ULONG Needed;

    UNREFERENCED_PARAMETER(Flags);

    if (Object == NULL || !Object->Finalized)
        return STATUS_INVALID_HANDLE;
    if (BlobType == NULL || ResultLength == NULL)
        return STATUS_INVALID_PARAMETER;
    if (ExportKey != NULL)
        return STATUS_NOT_SUPPORTED;

    switch (Object->Provider->Algorithm->Class)
    {
        case KsecClassRsa:
            return KsecExportRsa(Object, BlobType, Output, OutputLength, ResultLength);

        case KsecClassEcdsa:
            return KsecExportEcc(Object, BlobType, Output, OutputLength, ResultLength);

        default:
            break;
    }

    if (_wcsicmp(BlobType, BCRYPT_KEY_DATA_BLOB) != 0)
        return STATUS_NOT_SUPPORTED;

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
 * Starts an RSA or ECDSA key pair; BCryptFinalizeKeyPair() generates it.
 *
 * @param[in] Length
 * The key size in bits: 512 to 16384 in steps of 8 for RSA, the curve size for ECDSA.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_NOT_SUPPORTED,
 * STATUS_INVALID_PARAMETER or STATUS_NO_MEMORY.
 */
NTSTATUS
WINAPI
BCryptGenerateKeyPair(
    _In_ BCRYPT_ALG_HANDLE Algorithm,
    _Out_ BCRYPT_KEY_HANDLE *Key,
    _In_ ULONG Length,
    _In_ ULONG Flags)
{
    PKSEC_PROVIDER Provider = KsecReferenceProvider(Algorithm);
    PKSEC_KEY NewKey;
    NTSTATUS Status;

    if (Provider == NULL)
        return STATUS_INVALID_HANDLE;
    if (Provider->Algorithm->Class != KsecClassRsa && Provider->Algorithm->Class != KsecClassEcdsa)
        return STATUS_NOT_SUPPORTED;
    if (Key == NULL || Flags != 0)
        return STATUS_INVALID_PARAMETER;

    if (Provider->Algorithm->Class == KsecClassRsa)
    {
        if (Length < KSEC_RSA_MIN_BITS || Length > KSEC_RSA_MAX_BITS || (Length % KSEC_RSA_STEP_BITS) != 0)
            return STATUS_INVALID_PARAMETER;
    }
    else if (Length != Provider->Algorithm->CurveBits)
    {
        return STATUS_INVALID_PARAMETER;
    }

    NewKey = KsecCreateKey(Provider, NULL, 0, &Status);
    if (NewKey == NULL)
        return Status;

    NewKey->KeyBits = Length;
    *Key = NewKey;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Generates the key pair started by BCryptGenerateKeyPair().
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE for a key that is already final,
 * STATUS_NO_MEMORY or STATUS_INTERNAL_ERROR.
 */
NTSTATUS
WINAPI
BCryptFinalizeKeyPair(
    _Inout_ BCRYPT_KEY_HANDLE Key,
    _In_ ULONG Flags)
{
    PKSEC_KEY Object = KsecReferenceKey(Key);
    SYMCRYPT_RSA_PARAMS Params;
    SYMCRYPT_ERROR Error;

    UNREFERENCED_PARAMETER(Flags);

    if (Object == NULL || Object->Finalized)
        return STATUS_INVALID_HANDLE;

    if (Object->Provider->Algorithm->Class == KsecClassRsa)
    {
        Params.version = 1;
        Params.nBitsOfModulus = Object->KeyBits;
        Params.nPrimes = 2;
        Params.nPubExp = 1;

        Object->Rsa = SymCryptRsakeyAllocate(&Params, 0);
        if (Object->Rsa == NULL)
            return STATUS_NO_MEMORY;

        Error = SymCryptRsakeyGenerate(Object->Rsa, NULL, 0,
                                       SYMCRYPT_FLAG_KEY_NO_FIPS | SYMCRYPT_FLAG_RSAKEY_SIGN | SYMCRYPT_FLAG_RSAKEY_ENCRYPT);
    }
    else
    {
        Object->Ec = SymCryptEckeyAllocate(Object->Provider->Curve);
        if (Object->Ec == NULL)
            return STATUS_NO_MEMORY;

        Error = SymCryptEckeySetRandom(SYMCRYPT_FLAG_KEY_NO_FIPS | SYMCRYPT_FLAG_ECKEY_ECDSA, Object->Ec);
    }

    if (Error != SYMCRYPT_NO_ERROR)
        return STATUS_INTERNAL_ERROR;

    Object->Finalized = TRUE;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Checks an RSA (PKCS #1 or PSS) or ECDSA signature over a hash.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_SIGNATURE, STATUS_INVALID_HANDLE,
 * STATUS_NOT_SUPPORTED or STATUS_INVALID_PARAMETER.
 *
 * @remarks
 * ECDSA takes no padding, and like Windows it ignores the flags.
 */
NTSTATUS
WINAPI
BCryptVerifySignature(
    _In_ BCRYPT_KEY_HANDLE Key,
    _In_opt_ VOID *PaddingInfo,
    _In_reads_bytes_(HashLength) PUCHAR Hash,
    _In_ ULONG HashLength,
    _In_reads_bytes_(SignatureLength) PUCHAR Signature,
    _In_ ULONG SignatureLength,
    _In_ ULONG Flags)
{
    PKSEC_KEY Object = KsecReferenceKey(Key);
    BCRYPT_PKCS1_PADDING_INFO *Pkcs1 = PaddingInfo;
    BCRYPT_PSS_PADDING_INFO *Pss = PaddingInfo;
    const KSEC_SIGNATURE_HASH *SignatureHash = NULL;
    SYMCRYPT_ERROR Error;

    if (Object == NULL || !Object->Finalized)
        return STATUS_INVALID_HANDLE;
    if (Hash == NULL || Signature == NULL)
        return STATUS_INVALID_PARAMETER;

    if (Object->Provider->Algorithm->Class == KsecClassEcdsa)
    {
        if (SignatureLength != 2 * KsecEccFieldLength(Object->Provider))
            return STATUS_INVALID_PARAMETER;

        Error = SymCryptEcDsaVerify(Object->Ec, Hash, HashLength, Signature, SignatureLength,
                                    SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, 0);
        return (Error == SYMCRYPT_NO_ERROR) ? STATUS_SUCCESS : STATUS_INVALID_SIGNATURE;
    }

    if (Object->Provider->Algorithm->Class != KsecClassRsa)
        return STATUS_NOT_SUPPORTED;
    if (PaddingInfo == NULL || (Flags != BCRYPT_PAD_PKCS1 && Flags != BCRYPT_PAD_PSS))
        return STATUS_INVALID_PARAMETER;
    if (SignatureLength != SymCryptRsakeySizeofModulus(Object->Rsa))
        return STATUS_INVALID_PARAMETER;

    if (Flags == BCRYPT_PAD_PKCS1)
    {
        /* Without an algorithm the padding holds the bare hash, with no DigestInfo */
        if (Pkcs1->pszAlgId == NULL)
        {
            Error = SymCryptRsaPkcs1Verify(Object->Rsa, Hash, HashLength, Signature, SignatureLength,
                                           SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, NULL, 0, 0);
        }
        else
        {
            SignatureHash = KsecFindSignatureHash(Pkcs1->pszAlgId);
            if (SignatureHash == NULL)
                return STATUS_NOT_SUPPORTED;
            if (HashLength != SymCryptHashResultSize(*SignatureHash->Hash))
                return STATUS_INVALID_PARAMETER;

            Error = SymCryptRsaPkcs1Verify(Object->Rsa, Hash, HashLength, Signature, SignatureLength,
                                           SYMCRYPT_NUMBER_FORMAT_MSB_FIRST,
                                           SignatureHash->Oids, SignatureHash->OidCount, 0);
        }
    }
    else
    {
        if (Pss->pszAlgId == NULL)
            return STATUS_INVALID_PARAMETER;

        SignatureHash = KsecFindSignatureHash(Pss->pszAlgId);
        if (SignatureHash == NULL)
            return STATUS_NOT_SUPPORTED;
        if (HashLength != SymCryptHashResultSize(*SignatureHash->Hash))
            return STATUS_INVALID_PARAMETER;

        Error = SymCryptRsaPssVerify(Object->Rsa, Hash, HashLength, Signature, SignatureLength,
                                     SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, *SignatureHash->Hash,
                                     Pss->cbSalt, 0);
    }

    return (Error == SYMCRYPT_NO_ERROR) ? STATUS_SUCCESS : STATUS_INVALID_SIGNATURE;
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
    PKSEC_KEY Object = KsecReferenceKey(Key);

    if (Object == NULL)
        return STATUS_INVALID_HANDLE;

    KsecFreeKey(Object);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Sets a property of a provider or key. Only the AES chaining mode is settable.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_NOT_SUPPORTED or
 * STATUS_INVALID_PARAMETER.
 *
 * @remarks
 * GCM, CCM and CFB are valid names that this provider does not implement.
 */
NTSTATUS
WINAPI
BCryptSetProperty(
    _Inout_ BCRYPT_HANDLE Object,
    _In_ LPCWSTR Property,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_ ULONG Flags)
{
    PKSEC_PROVIDER Provider = KsecReferenceProvider(Object);
    PKSEC_KEY Key = KsecReferenceKey(Object);
    KSEC_CHAINING_MODE Mode;

    UNREFERENCED_PARAMETER(Flags);

    if (Key != NULL)
        Provider = Key->Provider;
    if (Provider == NULL)
        return STATUS_INVALID_HANDLE;
    if (Property == NULL || (Input == NULL && InputLength != 0))
        return STATUS_INVALID_PARAMETER;

    if (Provider->Algorithm->Class != KsecClassCipher || _wcsicmp(Property, BCRYPT_CHAINING_MODE) != 0)
        return STATUS_NOT_SUPPORTED;

    if (KsecPropertyIs(Input, InputLength, BCRYPT_CHAIN_MODE_CBC))
    {
        Mode = KsecChainingCbc;
    }
    else if (KsecPropertyIs(Input, InputLength, BCRYPT_CHAIN_MODE_ECB))
    {
        Mode = KsecChainingEcb;
    }
    else if (KsecPropertyIs(Input, InputLength, BCRYPT_CHAIN_MODE_GCM) ||
             KsecPropertyIs(Input, InputLength, BCRYPT_CHAIN_MODE_CCM) ||
             KsecPropertyIs(Input, InputLength, BCRYPT_CHAIN_MODE_CFB))
    {
        return STATUS_NOT_SUPPORTED;
    }
    else
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (Key != NULL)
        Key->ChainingMode = Mode;
    else
        Provider->ChainingMode = Mode;

    return STATUS_SUCCESS;
}

/* EOF */
