/*
 * PROJECT:     ReactOS Kernel Cryptography
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Symmetric keys and the block cipher modes
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "bcryptk.h"

/* FUNCTIONS ******************************************************************/

/* Is a key of this many bytes one the algorithm accepts? */
static
BOOLEAN
BcryptkKeyLengthAllowed(
    _In_ PCBCRYPTK_ALGORITHM Algorithm,
    _In_ ULONG Length)
{
    ULONG Bits = Length * 8;

    if (Bits < Algorithm->MinKeyLength || Bits > Algorithm->MaxKeyLength)
        return FALSE;

    if (Algorithm->KeyIncrement == 0)
        return Bits == Algorithm->MinKeyLength;

    return ((Bits - Algorithm->MinKeyLength) % Algorithm->KeyIncrement) == 0;
}

/* Builds a key object around a raw key */
static
NTSTATUS
BcryptkCreateKey(
    _In_ PBCRYPTK_PROVIDER Provider,
    _Out_ BCRYPT_KEY_HANDLE *KeyHandle,
    _Inout_opt_ PUCHAR Object,
    _In_ ULONG ObjectLength,
    _In_reads_bytes_(SecretLength) PUCHAR Secret,
    _In_ ULONG SecretLength)
{
    PBCRYPTK_KEY Key;
    BOOLEAN Allocated;

    if (Provider->Algorithm->Class != BcryptkClassCipher)
        return STATUS_NOT_SUPPORTED;

    if (!BcryptkKeyLengthAllowed(Provider->Algorithm, SecretLength))
        return STATUS_INVALID_PARAMETER;

    Key = BcryptkAllocateObject(BcryptkKeyObjectLength(Provider->Algorithm),
                                Object,
                                ObjectLength,
                                &Allocated);
    if (Key == NULL)
        return Object != NULL ? STATUS_BUFFER_TOO_SMALL : STATUS_INSUFFICIENT_RESOURCES;

    Key->Algorithm = Provider->Algorithm;
    Key->ChainingMode = Provider->ChainingMode;
    Key->MessageBlockLength = Provider->MessageBlockLength;
    Key->KeyLength = SecretLength;
    Key->Allocated = Allocated;
    Key->ExpandedKey = BcryptkAlign(&Key->Storage[0]);

    if ((*Key->Algorithm->Cipher)->expandKeyFunc(Key->ExpandedKey,
                                                 Secret,
                                                 SecretLength) != SYMCRYPT_NO_ERROR)
    {
        BcryptkFreeObject(Key, Allocated);
        return STATUS_INVALID_PARAMETER;
    }

    Key->Tag = BCRYPTK_KEY_TAG;
    *KeyHandle = Key;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Makes a key out of raw key material.
 *
 * @param[in,out] Object
 * Buffer the key lives in, of the size the BCRYPT_OBJECT_LENGTH property
 * reports, or NULL to have one allocated.
 */
NTSTATUS
WINAPI
BCryptGenerateSymmetricKey(
    _In_ BCRYPT_ALG_HANDLE Algorithm,
    _Out_ BCRYPT_KEY_HANDLE *Key,
    _Inout_updates_bytes_opt_(ObjectLength) PUCHAR Object,
    _In_ ULONG ObjectLength,
    _In_reads_bytes_(SecretLength) PUCHAR Secret,
    _In_ ULONG SecretLength,
    _In_ ULONG Flags)
{
    PBCRYPTK_PROVIDER Provider = Algorithm;

    UNREFERENCED_PARAMETER(Flags);

    if (Provider == NULL || Provider->Tag != BCRYPTK_PROVIDER_TAG)
        return STATUS_INVALID_HANDLE;

    if (Key == NULL || Secret == NULL)
        return STATUS_INVALID_PARAMETER;

    *Key = NULL;

    return BcryptkCreateKey(Provider, Key, Object, ObjectLength, Secret, SecretLength);
}

/**
 * @brief
 * Makes a key out of a key blob.
 *
 * @param[in] BlobType
 * The layout of @p Input. Only BCRYPT_KEY_DATA_BLOB is read here.
 */
NTSTATUS
WINAPI
BCryptImportKey(
    _In_ BCRYPT_ALG_HANDLE Algorithm,
    _In_opt_ BCRYPT_KEY_HANDLE ImportKey,
    _In_ LPCWSTR BlobType,
    _Out_ BCRYPT_KEY_HANDLE *Key,
    _Inout_updates_bytes_opt_(ObjectLength) PUCHAR Object,
    _In_ ULONG ObjectLength,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_ ULONG Flags)
{
    PBCRYPTK_PROVIDER Provider = Algorithm;
    PBCRYPT_KEY_DATA_BLOB_HEADER Header;

    UNREFERENCED_PARAMETER(Flags);

    if (Provider == NULL || Provider->Tag != BCRYPTK_PROVIDER_TAG)
        return STATUS_INVALID_HANDLE;

    if (Key == NULL || BlobType == NULL || Input == NULL)
        return STATUS_INVALID_PARAMETER;

    *Key = NULL;

    /* Wrapping a key with another key is not offered */
    if (ImportKey != NULL)
        return STATUS_NOT_SUPPORTED;

    if (_wcsicmp(BlobType, BCRYPT_KEY_DATA_BLOB) != 0)
        return STATUS_NOT_SUPPORTED;

    if (InputLength < sizeof(*Header))
        return STATUS_INVALID_PARAMETER;

    Header = (PBCRYPT_KEY_DATA_BLOB_HEADER)Input;

    if (Header->dwMagic != BCRYPT_KEY_DATA_BLOB_MAGIC ||
        Header->dwVersion != BCRYPT_KEY_DATA_BLOB_VERSION1)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (Header->cbKeyData > InputLength - sizeof(*Header))
        return STATUS_INVALID_PARAMETER;

    return BcryptkCreateKey(Provider,
                            Key,
                            Object,
                            ObjectLength,
                            Input + sizeof(*Header),
                            Header->cbKeyData);
}

/**
 * @brief
 * Copies a key, chaining mode and all.
 */
NTSTATUS
WINAPI
BCryptDuplicateKey(
    _In_ BCRYPT_KEY_HANDLE Key,
    _Out_ BCRYPT_KEY_HANDLE *NewKey,
    _Inout_updates_bytes_opt_(ObjectLength) PUCHAR Object,
    _In_ ULONG ObjectLength,
    _In_ ULONG Flags)
{
    PBCRYPTK_KEY Source = Key;
    PBCRYPTK_KEY Copy;
    BOOLEAN Allocated;
    ULONG Length;

    UNREFERENCED_PARAMETER(Flags);

    if (Source == NULL || Source->Tag != BCRYPTK_KEY_TAG)
        return STATUS_INVALID_HANDLE;

    if (NewKey == NULL)
        return STATUS_INVALID_PARAMETER;

    *NewKey = NULL;
    Length = BcryptkKeyObjectLength(Source->Algorithm);

    Copy = BcryptkAllocateObject(Length, Object, ObjectLength, &Allocated);
    if (Copy == NULL)
        return Object != NULL ? STATUS_BUFFER_TOO_SMALL : STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(Copy, Source, Length);
    Copy->Allocated = Allocated;
    Copy->ExpandedKey = BcryptkAlign(&Copy->Storage[0]);

    *NewKey = Copy;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Makes a key that stands for one this library holds nothing of.
 *
 * @remarks
 * A provider whose keys are not symmetric ones still has to hand back
 * something BCryptDestroyKey() will take, which is what this is for.
 */
NTSTATUS
BcryptkCreateOpaqueKey(
    _Out_ BCRYPT_KEY_HANDLE *Key)
{
    PBCRYPTK_KEY Object;

    Object = ExAllocatePoolZero(NonPagedPool, sizeof(*Object), BCRYPTK_POOL_TAG);
    if (Object == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Object->Tag = BCRYPTK_OPAQUE_TAG;
    Object->Allocated = TRUE;

    *Key = Object;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Destroys a key and wipes the key material with it.
 */
NTSTATUS
WINAPI
BCryptDestroyKey(
    _In_ BCRYPT_KEY_HANDLE Key)
{
    PBCRYPTK_KEY Object = Key;
    ULONG Length;
    BOOLEAN Allocated;

    if (Object == NULL)
        return STATUS_INVALID_HANDLE;

    if (Object->Tag == BCRYPTK_OPAQUE_TAG)
    {
        ExFreePoolWithTag(Object, BCRYPTK_POOL_TAG);
        return STATUS_SUCCESS;
    }

    if (Object->Tag != BCRYPTK_KEY_TAG)
        return STATUS_INVALID_HANDLE;

    Length = BcryptkKeyObjectLength(Object->Algorithm);
    Allocated = Object->Allocated;

    SymCryptWipe(Object, Length);
    BcryptkFreeObject(Object, Allocated);

    return STATUS_SUCCESS;
}

/*
 * Works out how much room a run needs and checks that the caller may ask for
 * it at all. Padding is only offered for the modes that work a block at a
 * time.
 */
static
NTSTATUS
BcryptkCipherResultLength(
    _In_ PBCRYPTK_KEY Key,
    _In_ ULONG InputLength,
    _In_ BOOLEAN Padding,
    _Out_ PULONG Result)
{
    ULONG BlockSize = (ULONG)(*Key->Algorithm->Cipher)->blockSize;

    if (Padding)
    {
        if (Key->ChainingMode != BcryptkChainCbc && Key->ChainingMode != BcryptkChainEcb)
            return STATUS_INVALID_PARAMETER;

        if (InputLength > MAXULONG - BlockSize)
            return STATUS_INVALID_PARAMETER;

        *Result = InputLength + BlockSize - (InputLength % BlockSize);

        return STATUS_SUCCESS;
    }

    if (Key->ChainingMode == BcryptkChainCfb)
    {
        if ((InputLength % Key->MessageBlockLength) != 0)
            return STATUS_INVALID_BUFFER_SIZE;
    }
    else if ((InputLength % BlockSize) != 0)
    {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    *Result = InputLength;

    return STATUS_SUCCESS;
}

/*
 * Points at the chaining value a run works from. The caller's own buffer is
 * used where there is one, so that it comes back holding the value the next
 * run has to carry on from.
 */
static
NTSTATUS
BcryptkChainingValue(
    _In_ PBCRYPTK_KEY Key,
    _In_reads_bytes_opt_(IvLength) PUCHAR Iv,
    _In_ ULONG IvLength,
    _Out_writes_bytes_(SYMCRYPT_MAX_BLOCK_SIZE) PUCHAR Local,
    _Outptr_ PUCHAR *Chain)
{
    ULONG BlockSize = (ULONG)(*Key->Algorithm->Cipher)->blockSize;

    if (Key->ChainingMode == BcryptkChainEcb)
    {
        *Chain = NULL;
        return STATUS_SUCCESS;
    }

    if (Iv == NULL)
    {
        RtlZeroMemory(Local, BlockSize);
        *Chain = Local;

        return STATUS_SUCCESS;
    }

    if (IvLength != BlockSize)
        return STATUS_INVALID_PARAMETER;

    *Chain = Iv;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Encrypts a buffer with a symmetric key.
 *
 * @param[in] PaddingInfo
 * Only the authenticated modes take one, so it goes unread here.
 *
 * @param[in,out] Iv
 * The chaining value to start from. It comes back holding the value the run
 * ended on, so a long message can go out piece by piece.
 *
 * @param[out] Output
 * Receives the ciphertext, or NULL to ask how much room it needs.
 *
 * @param[in] Flags
 * BCRYPT_BLOCK_PADDING rounds the message up to a whole block.
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
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    PBCRYPTK_KEY Object = Key;
    UCHAR Local[SYMCRYPT_MAX_BLOCK_SIZE];
    UCHAR Tail[SYMCRYPT_MAX_BLOCK_SIZE];
    PUCHAR Chain;
    NTSTATUS Status;
    SIZE_T Padded;
    ULONG BlockSize;
    ULONG Whole;
    ULONG Required;
    BOOLEAN Padding = (Flags & BCRYPT_BLOCK_PADDING) != 0;

    UNREFERENCED_PARAMETER(PaddingInfo);

    if (Object == NULL || Object->Tag != BCRYPTK_KEY_TAG)
        return STATUS_INVALID_HANDLE;

    if (ResultLength == NULL || (Input == NULL && InputLength != 0))
        return STATUS_INVALID_PARAMETER;

    if ((Flags & ~BCRYPT_BLOCK_PADDING) != 0)
        return STATUS_INVALID_PARAMETER;

    Status = BcryptkCipherResultLength(Object, InputLength, Padding, &Required);
    if (!NT_SUCCESS(Status))
        return Status;

    *ResultLength = Required;

    if (Output == NULL)
        return STATUS_SUCCESS;

    if (OutputLength < Required)
        return STATUS_BUFFER_TOO_SMALL;

    Status = BcryptkChainingValue(Object, Iv, IvLength, Local, &Chain);
    if (!NT_SUCCESS(Status))
        return Status;

    BlockSize = (ULONG)(*Object->Algorithm->Cipher)->blockSize;
    Whole = InputLength - (InputLength % BlockSize);

    switch (Object->ChainingMode)
    {
        case BcryptkChainEcb:
            SymCryptEcbEncrypt(*Object->Algorithm->Cipher,
                               Object->ExpandedKey,
                               Input,
                               Output,
                               Whole);
            break;

        case BcryptkChainCbc:
            SymCryptCbcEncrypt(*Object->Algorithm->Cipher,
                               Object->ExpandedKey,
                               Chain,
                               Input,
                               Output,
                               Whole);
            break;

        case BcryptkChainCfb:
            SymCryptCfbEncrypt(*Object->Algorithm->Cipher,
                               Object->MessageBlockLength,
                               Object->ExpandedKey,
                               Chain,
                               Input,
                               Output,
                               InputLength);
            break;

        default:
            return STATUS_NOT_SUPPORTED;
    }

    if (!Padding)
        return STATUS_SUCCESS;

    /* Round the leftover up to one more block and put that out as well */
    SymCryptPaddingPkcs7Add(BlockSize,
                            Input + Whole,
                            InputLength - Whole,
                            Tail,
                            sizeof(Tail),
                            &Padded);

    if (Object->ChainingMode == BcryptkChainEcb)
    {
        SymCryptEcbEncrypt(*Object->Algorithm->Cipher,
                           Object->ExpandedKey,
                           Tail,
                           Output + Whole,
                           Padded);
    }
    else
    {
        SymCryptCbcEncrypt(*Object->Algorithm->Cipher,
                           Object->ExpandedKey,
                           Chain,
                           Tail,
                           Output + Whole,
                           Padded);
    }

    SymCryptWipeKnownSize(Tail, sizeof(Tail));

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Decrypts a buffer with a symmetric key.
 *
 * @remarks
 * With BCRYPT_BLOCK_PADDING the plaintext is shorter than the ciphertext, so
 * the last block is worked out first. That way the length is known before
 * anything is written and a buffer that turns out too small is reported
 * without half an answer in it.
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
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    PBCRYPTK_KEY Object = Key;
    UCHAR Local[SYMCRYPT_MAX_BLOCK_SIZE];
    UCHAR Last[SYMCRYPT_MAX_BLOCK_SIZE];
    UCHAR Tail[SYMCRYPT_MAX_BLOCK_SIZE];
    UCHAR LastChain[SYMCRYPT_MAX_BLOCK_SIZE];
    PUCHAR Chain;
    NTSTATUS Status;
    SIZE_T Plain;
    ULONG BlockSize;
    ULONG Whole;
    ULONG Required;
    BOOLEAN Padding = (Flags & BCRYPT_BLOCK_PADDING) != 0;

    UNREFERENCED_PARAMETER(PaddingInfo);

    if (Object == NULL || Object->Tag != BCRYPTK_KEY_TAG)
        return STATUS_INVALID_HANDLE;

    if (ResultLength == NULL || (Input == NULL && InputLength != 0))
        return STATUS_INVALID_PARAMETER;

    if ((Flags & ~BCRYPT_BLOCK_PADDING) != 0)
        return STATUS_INVALID_PARAMETER;

    BlockSize = (ULONG)(*Object->Algorithm->Cipher)->blockSize;

    if (Padding)
    {
        if (Object->ChainingMode != BcryptkChainCbc && Object->ChainingMode != BcryptkChainEcb)
            return STATUS_INVALID_PARAMETER;

        if (InputLength == 0 || (InputLength % BlockSize) != 0)
            return STATUS_INVALID_BUFFER_SIZE;
    }
    else
    {
        Status = BcryptkCipherResultLength(Object, InputLength, FALSE, &Required);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* Without a buffer the answer is an upper bound on the plaintext */
    if (Output == NULL)
    {
        *ResultLength = InputLength;
        return STATUS_SUCCESS;
    }

    Status = BcryptkChainingValue(Object, Iv, IvLength, Local, &Chain);
    if (!NT_SUCCESS(Status))
        return Status;

    if (!Padding)
    {
        *ResultLength = Required;

        if (OutputLength < Required)
            return STATUS_BUFFER_TOO_SMALL;

        switch (Object->ChainingMode)
        {
            case BcryptkChainEcb:
                SymCryptEcbDecrypt(*Object->Algorithm->Cipher,
                                   Object->ExpandedKey,
                                   Input,
                                   Output,
                                   InputLength);
                break;

            case BcryptkChainCbc:
                SymCryptCbcDecrypt(*Object->Algorithm->Cipher,
                                   Object->ExpandedKey,
                                   Chain,
                                   Input,
                                   Output,
                                   InputLength);
                break;

            case BcryptkChainCfb:
                SymCryptCfbDecrypt(*Object->Algorithm->Cipher,
                                   Object->MessageBlockLength,
                                   Object->ExpandedKey,
                                   Chain,
                                   Input,
                                   Output,
                                   InputLength);
                break;

            default:
                return STATUS_NOT_SUPPORTED;
        }

        return STATUS_SUCCESS;
    }

    Whole = InputLength - BlockSize;

    /*
     * Take the last block on its own. Its chaining value is the ciphertext
     * block before it, which is to hand, so the run below is untouched by it.
     */
    if (Object->ChainingMode == BcryptkChainEcb)
    {
        SymCryptEcbDecrypt(*Object->Algorithm->Cipher,
                           Object->ExpandedKey,
                           Input + Whole,
                           Last,
                           BlockSize);
    }
    else
    {
        if (Whole != 0)
            RtlCopyMemory(LastChain, Input + Whole - BlockSize, BlockSize);
        else
            RtlCopyMemory(LastChain, Chain, BlockSize);

        SymCryptCbcDecrypt(*Object->Algorithm->Cipher,
                           Object->ExpandedKey,
                           LastChain,
                           Input + Whole,
                           Last,
                           BlockSize);
    }

    if (SymCryptPaddingPkcs7Remove(BlockSize,
                                   Last,
                                   BlockSize,
                                   Tail,
                                   sizeof(Tail),
                                   &Plain) != SYMCRYPT_NO_ERROR)
    {
        SymCryptWipeKnownSize(Last, sizeof(Last));
        return STATUS_INVALID_BUFFER_SIZE;
    }

    *ResultLength = Whole + (ULONG)Plain;

    if (OutputLength < *ResultLength)
    {
        SymCryptWipeKnownSize(Last, sizeof(Last));
        SymCryptWipeKnownSize(Tail, sizeof(Tail));

        return STATUS_BUFFER_TOO_SMALL;
    }

    if (Whole != 0)
    {
        if (Object->ChainingMode == BcryptkChainEcb)
        {
            SymCryptEcbDecrypt(*Object->Algorithm->Cipher,
                               Object->ExpandedKey,
                               Input,
                               Output,
                               Whole);
        }
        else
        {
            SymCryptCbcDecrypt(*Object->Algorithm->Cipher,
                               Object->ExpandedKey,
                               Chain,
                               Input,
                               Output,
                               Whole);
        }
    }

    RtlCopyMemory(Output + Whole, Tail, Plain);

    /* The next run carries on from the last block that came in */
    if (Chain != NULL)
        RtlCopyMemory(Chain, Input + Whole, BlockSize);

    SymCryptWipeKnownSize(Last, sizeof(Last));
    SymCryptWipeKnownSize(Tail, sizeof(Tail));

    return STATUS_SUCCESS;
}

/* EOF */
