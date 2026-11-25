/*
 * PROJECT:     ReactOS Kernel Cryptography Next Generation
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Hash, HMAC and CMAC objects
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "cngp.h"

#include <debug.h>

/* HMAC MD2 AND MD4 ************************************************************/

static
SYMCRYPT_ERROR
SYMCRYPT_CALL
CngHmacMd2ExpandKey(
    _Out_ PVOID ExpandedKey,
    _In_reads_(KeyLength) PCBYTE Key,
    _In_ SIZE_T KeyLength)
{
    return SymCryptHmacExpandKey(SymCryptMd2Algorithm, ExpandedKey, Key, KeyLength);
}

static
SYMCRYPT_ERROR
SYMCRYPT_CALL
CngHmacMd4ExpandKey(
    _Out_ PVOID ExpandedKey,
    _In_reads_(KeyLength) PCBYTE Key,
    _In_ SIZE_T KeyLength)
{
    return SymCryptHmacExpandKey(SymCryptMd4Algorithm, ExpandedKey, Key, KeyLength);
}

static const SYMCRYPT_MAC CngHmacMd2 =
{
    CngHmacMd2ExpandKey,
    (PSYMCRYPT_MAC_INIT)SymCryptHmacInit,
    (PSYMCRYPT_MAC_APPEND)SymCryptHmacAppend,
    (PSYMCRYPT_MAC_RESULT)SymCryptHmacResult,
    sizeof(SYMCRYPT_HMAC_EXPANDED_KEY),
    sizeof(SYMCRYPT_HMAC_STATE),
    SYMCRYPT_MD2_RESULT_SIZE,
    &SymCryptMd2Algorithm,
    0
};

static const SYMCRYPT_MAC CngHmacMd4 =
{
    CngHmacMd4ExpandKey,
    (PSYMCRYPT_MAC_INIT)SymCryptHmacInit,
    (PSYMCRYPT_MAC_APPEND)SymCryptHmacAppend,
    (PSYMCRYPT_MAC_RESULT)SymCryptHmacResult,
    sizeof(SYMCRYPT_HMAC_EXPANDED_KEY),
    sizeof(SYMCRYPT_HMAC_STATE),
    SYMCRYPT_MD4_RESULT_SIZE,
    &SymCryptMd4Algorithm,
    0
};

const PCSYMCRYPT_MAC CngHmacMd2Algorithm = &CngHmacMd2;
const PCSYMCRYPT_MAC CngHmacMd4Algorithm = &CngHmacMd4;

/* HELPERS ********************************************************************/

static
BOOLEAN
CngIsKeyed(
    _In_ PCNG_PROVIDER Provider)
{
    return Provider->Algorithm->Class == CngClassMac || (Provider->Flags & BCRYPT_ALG_HANDLE_HMAC_FLAG);
}

PCSYMCRYPT_MAC
NTAPI
CngMacForHash(
    _In_ const CNG_ALGORITHM *Algorithm)
{
    if (Algorithm->Mac == NULL)
        return NULL;

    return *Algorithm->Mac;
}

static
SIZE_T
CngStateSize(
    _In_ const CNG_ALGORITHM *Algorithm,
    _In_ BOOLEAN Keyed)
{
    if (Keyed)
        return CngMacForHash(Algorithm)->stateSize;

    return SymCryptHashStateSize(*Algorithm->Hash);
}

/* One element is a state followed by the expanded key of a keyed hash */
static
ULONG
CngElementStride(
    _In_ const CNG_ALGORITHM *Algorithm,
    _In_ BOOLEAN Keyed,
    _Out_opt_ PULONG KeyOffset)
{
    ULONG State = (ULONG)CNG_ALIGN(CngStateSize(Algorithm, Keyed));

    if (KeyOffset != NULL)
        *KeyOffset = State;
    if (!Keyed)
        return State;

    return State + (ULONG)CNG_ALIGN(CngMacForHash(Algorithm)->expandedKeySize);
}

/**
 * @brief
 * The object length of a hash: its header, one element, and room to align the start.
 */
ULONG
NTAPI
CngHashObjectLength(
    _In_ const CNG_ALGORITHM *Algorithm,
    _In_ BOOLEAN Hmac)
{
    BOOLEAN Keyed = Hmac || Algorithm->Class == CngClassMac;

    return (ULONG)CNG_ALIGN(sizeof(CNG_HASH)) + CngElementStride(Algorithm, Keyed, NULL) + CNG_OBJECT_ALIGNMENT - 1;
}

ULONG
NTAPI
CngDigestLength(
    _In_ PCNG_PROVIDER Provider)
{
    if (CngIsKeyed(Provider))
        return (ULONG)CngMacForHash(Provider->Algorithm)->resultSize;

    return (ULONG)SymCryptHashResultSize(*Provider->Algorithm->Hash);
}

PCNG_HASH
NTAPI
CngLookupHash(
    _In_opt_ BCRYPT_HANDLE Handle)
{
    PCNG_HASH Hash = (PCNG_HASH)Handle;

    if (Hash == NULL || ((ULONG_PTR)Handle & (CNG_OBJECT_ALIGNMENT - 1)) != 0 || Hash->Signature != CNG_HASH_SIGNATURE)
        return NULL;
    if (!CngObjectUsable(Hash->Provider))
        return NULL;

    return Hash;
}

static
PVOID
CngElementState(
    _In_ PCNG_HASH Hash,
    _In_ ULONG Index)
{
    return Hash->Elements + (SIZE_T)Index * Hash->Stride;
}

static
PVOID
CngElementKey(
    _In_ PCNG_HASH Hash,
    _In_ ULONG Index)
{
    return Hash->Elements + (SIZE_T)Index * Hash->Stride + Hash->KeyOffset;
}

/* Puts an element back to where it stood right after creation */
static
VOID
CngElementStart(
    _In_ PCNG_HASH Hash,
    _In_ ULONG Index)
{
    const CNG_ALGORITHM *Algorithm = Hash->Provider->Algorithm;

    if (CngIsKeyed(Hash->Provider))
        CngMacForHash(Algorithm)->initFunc(CngElementState(Hash, Index), CngElementKey(Hash, Index));
    else
        SymCryptHashInit(*Algorithm->Hash, CngElementState(Hash, Index));
}

static
VOID
CngElementAppend(
    _In_ PCNG_HASH Hash,
    _In_ ULONG Index,
    _In_reads_bytes_(Length) PCBYTE Data,
    _In_ SIZE_T Length)
{
    const CNG_ALGORITHM *Algorithm = Hash->Provider->Algorithm;

    if (CngIsKeyed(Hash->Provider))
        CngMacForHash(Algorithm)->appendFunc(CngElementState(Hash, Index), Data, Length);
    else
        SymCryptHashAppend(*Algorithm->Hash, CngElementState(Hash, Index), Data, Length);
}

static
VOID
CngElementResult(
    _In_ PCNG_HASH Hash,
    _In_ ULONG Index,
    _Out_writes_bytes_(Length) PBYTE Output,
    _In_ SIZE_T Length)
{
    const CNG_ALGORITHM *Algorithm = Hash->Provider->Algorithm;

    if (CngIsKeyed(Hash->Provider))
        CngMacForHash(Algorithm)->resultFunc(CngElementState(Hash, Index), Output);
    else
        SymCryptHashResult(*Algorithm->Hash, CngElementState(Hash, Index), Output, Length);
}

/* Lays out a hash object in a caller buffer or a fresh allocation */
static
NTSTATUS
CngBuildHash(
    _In_ PCNG_PROVIDER Provider,
    _In_ ULONG Count,
    _Out_writes_bytes_opt_(ObjectLength) PUCHAR Object,
    _In_ ULONG ObjectLength,
    _In_reads_bytes_opt_(SecretLength) PUCHAR Secret,
    _In_ ULONG SecretLength,
    _In_ ULONG Flags,
    _In_ BOOLEAN ExpandKey,
    _Out_ PCNG_HASH *Result)
{
    BOOLEAN Keyed = CngIsKeyed(Provider);
    PCSYMCRYPT_MAC Mac = CngMacForHash(Provider->Algorithm);
    ULONG Stride, KeyOffset, Index;
    SIZE_T Needed;
    PVOID Allocation = NULL;
    PCNG_HASH Hash;
    PUCHAR Start;

    if (Object == NULL && ObjectLength != 0)
        return STATUS_INVALID_PARAMETER;
    if (!Keyed && SecretLength != 0)
        return STATUS_INVALID_PARAMETER;
    if (Secret == NULL && SecretLength != 0)
        return STATUS_INVALID_PARAMETER;

    Stride = CngElementStride(Provider->Algorithm, Keyed, &KeyOffset);
    Needed = CNG_ALIGN(sizeof(CNG_HASH)) + (SIZE_T)Stride * Count;

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

    Hash = (PCNG_HASH)Start;
    RtlZeroMemory(Hash, Needed);
    Hash->Provider = Provider;
    Hash->Allocation = Allocation;
    Hash->Elements = Start + CNG_ALIGN(sizeof(CNG_HASH));
    Hash->Count = Count;
    Hash->Stride = Stride;
    Hash->KeyOffset = KeyOffset;
    if ((Flags & BCRYPT_HASH_REUSABLE_FLAG) || (Provider->Flags & BCRYPT_HASH_REUSABLE_FLAG))
        Hash->Flags |= CNG_HASH_REUSABLE;

    for (Index = 0; Index < Count && ExpandKey; Index++)
    {
        if (Keyed && Mac->expandKeyFunc(CngElementKey(Hash, Index), Secret, SecretLength) != SYMCRYPT_NO_ERROR)
        {
            SymCryptWipe(Hash, Needed);
            CngFree(Allocation);
            return STATUS_INVALID_PARAMETER;
        }

        CngElementStart(Hash, Index);
    }

    CngReferenceProvider(Provider);
    Hash->Signature = CNG_HASH_SIGNATURE;

    *Result = Hash;
    return STATUS_SUCCESS;
}

static
VOID
CngDestroyHashObject(
    _In_ PCNG_HASH Hash)
{
    PCNG_PROVIDER Provider = Hash->Provider;
    PVOID Allocation = Hash->Allocation;

    SymCryptWipe(Hash, CNG_ALIGN(sizeof(CNG_HASH)) + (SIZE_T)Hash->Stride * Hash->Count);
    CngFree(Allocation);
    CngDereferenceProvider(Provider);
}

/**
 * @brief
 * Finishes a hash object into a buffer of exactly the digest length.
 */
NTSTATUS
NTAPI
CngHashFinal(
    _In_ PCNG_HASH Hash,
    _Out_writes_bytes_(Length) PUCHAR Output,
    _In_ ULONG Length)
{
    CngElementResult(Hash, 0, Output, Length);

    if (Hash->Flags & CNG_HASH_REUSABLE)
        CngElementStart(Hash, 0);
    else
        Hash->Flags |= CNG_HASH_FINISHED;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Answers the properties of hash providers and hash objects.
 */
NTSTATUS
NTAPI
CngQueryHashProperty(
    _In_ PCNG_PROVIDER Provider,
    _In_ LPCWSTR Property,
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    const CNG_ALGORITHM *Algorithm = Provider->Algorithm;
    BCRYPT_MULTI_OBJECT_LENGTH_STRUCT Multi;
    BOOLEAN Keyed = CngIsKeyed(Provider);
    ULONG Value;

    if (wcscmp(Property, BCRYPT_OBJECT_LENGTH) == 0)
        Value = CngHashObjectLength(Algorithm, Keyed);
    else if (wcscmp(Property, BCRYPT_HASH_LENGTH) == 0)
        Value = CngDigestLength(Provider);
    else if (wcscmp(Property, BCRYPT_HASH_BLOCK_LENGTH) == 0)
        Value = (Algorithm->Hash != NULL) ? (ULONG)SymCryptHashInputBlockSize(*Algorithm->Hash) : SYMCRYPT_AES_BLOCK_SIZE;
    else if (wcscmp(Property, BCRYPT_IS_REUSABLE_HASH) == 0)
        Value = (Provider->Flags & BCRYPT_HASH_REUSABLE_FLAG) ? 1 : 0;
    else if (wcscmp(Property, BCRYPT_IS_KEYED_HASH) == 0)
        Value = Keyed ? 1 : 0;
    else if (wcscmp(Property, BCRYPT_MULTI_OBJECT_LENGTH) == 0)
    {
        Multi.cbPerObject = (ULONG)CNG_ALIGN(sizeof(CNG_HASH)) + CNG_OBJECT_ALIGNMENT - 1;
        Multi.cbPerElement = CngElementStride(Algorithm, Keyed, NULL);
        return CngReturnProperty(&Multi, sizeof(Multi), Output, OutputLength, ResultLength);
    }
    else
        return STATUS_NOT_SUPPORTED;

    return CngReturnProperty(&Value, sizeof(Value), Output, OutputLength, ResultLength);
}

/* EXPORTS ********************************************************************/

/**
 * @brief
 * Creates a hash object.
 *
 * @param[in] Algorithm
 * A hash provider; with BCRYPT_ALG_HANDLE_HMAC_FLAG the object is an HMAC.
 *
 * @param[out] Hash
 * Receives the hash handle.
 *
 * @param[out] HashObject
 * Optional caller buffer holding the object, at least BCRYPT_OBJECT_LENGTH bytes.
 *
 * @param[in] HashObjectLength
 * Size of @p HashObject; zero when it is NULL.
 *
 * @param[in] Secret
 * The key of an HMAC or CMAC. Plain hashes take none.
 *
 * @param[in] SecretLength
 * Size of @p Secret, in bytes.
 *
 * @param[in] Flags
 * BCRYPT_HASH_REUSABLE_FLAG.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_NOT_SUPPORTED,
 * STATUS_INVALID_PARAMETER, STATUS_BUFFER_TOO_SMALL or STATUS_NO_MEMORY.
 */
NTSTATUS
WINAPI
BCryptCreateHash(
    _Inout_ BCRYPT_ALG_HANDLE Algorithm,
    _Out_ BCRYPT_HASH_HANDLE *Hash,
    _Out_writes_bytes_all_opt_(HashObjectLength) PUCHAR HashObject,
    _In_ ULONG HashObjectLength,
    _In_reads_bytes_opt_(SecretLength) PUCHAR Secret,
    _In_ ULONG SecretLength,
    _In_ ULONG Flags)
{
    PCNG_PROVIDER Provider = CngLookupProvider(Algorithm);
    PCNG_HASH NewHash;
    NTSTATUS Status;

    if (Provider == NULL)
        return STATUS_INVALID_HANDLE;
    if (Provider->Algorithm->Class != CngClassHash && Provider->Algorithm->Class != CngClassMac)
        return STATUS_NOT_SUPPORTED;
    if (Hash == NULL || (Flags & ~BCRYPT_HASH_REUSABLE_FLAG))
        return STATUS_INVALID_PARAMETER;

    Status = CngBuildHash(Provider, 1, HashObject, HashObjectLength, Secret, SecretLength, Flags, TRUE, &NewHash);
    if (!NT_SUCCESS(Status))
        return Status;

    *Hash = NewHash;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Creates an object of several hash states sharing one key, for
 * BCryptProcessMultiOperations(). The provider needs BCRYPT_MULTI_FLAG.
 */
NTSTATUS
WINAPI
BCryptCreateMultiHash(
    _Inout_ BCRYPT_ALG_HANDLE Algorithm,
    _Out_ BCRYPT_HASH_HANDLE *Hash,
    _In_ ULONG HashCount,
    _Out_writes_bytes_all_opt_(HashObjectLength) PUCHAR HashObject,
    _In_ ULONG HashObjectLength,
    _In_reads_bytes_opt_(SecretLength) PUCHAR Secret,
    _In_ ULONG SecretLength,
    _In_ ULONG Flags)
{
    PCNG_PROVIDER Provider = CngLookupProvider(Algorithm);
    PCNG_HASH NewHash;
    NTSTATUS Status;

    if (Provider == NULL)
        return STATUS_INVALID_HANDLE;
    if (Provider->Algorithm->Class != CngClassHash && Provider->Algorithm->Class != CngClassMac)
        return STATUS_NOT_SUPPORTED;
    if (!(Provider->Flags & BCRYPT_MULTI_FLAG))
        return STATUS_INVALID_PARAMETER;
    if (Hash == NULL || HashCount == 0 || HashCount > 0x10000 || (Flags & ~BCRYPT_HASH_REUSABLE_FLAG))
        return STATUS_INVALID_PARAMETER;

    Status = CngBuildHash(Provider, HashCount, HashObject, HashObjectLength, Secret, SecretLength,
                          BCRYPT_HASH_REUSABLE_FLAG, TRUE, &NewHash);
    if (!NT_SUCCESS(Status))
        return Status;

    NewHash->Flags |= CNG_HASH_MULTI;
    *Hash = NewHash;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Runs a list of hash data and finish operations on a multi hash object.
 */
NTSTATUS
WINAPI
BCryptProcessMultiOperations(
    _Inout_ BCRYPT_HANDLE Object,
    _In_ BCRYPT_MULTI_OPERATION_TYPE OperationType,
    _In_reads_bytes_(OperationsLength) PVOID Operations,
    _In_ ULONG OperationsLength,
    _In_ ULONG Flags)
{
    PCNG_HASH Hash = CngLookupHash(Object);
    BCRYPT_MULTI_HASH_OPERATION *Operation = Operations;
    ULONG Count, Index, Digest;

    if (Hash == NULL || !(Hash->Flags & CNG_HASH_MULTI))
        return STATUS_INVALID_HANDLE;
    if (OperationType != BCRYPT_OPERATION_TYPE_HASH || Flags != 0 || Operations == NULL ||
        (OperationsLength % sizeof(*Operation)) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Count = OperationsLength / sizeof(*Operation);
    Digest = CngDigestLength(Hash->Provider);

    /* Everything is checked before any state changes */
    for (Index = 0; Index < Count; Index++)
    {
        if (Operation[Index].iHash >= Hash->Count)
            return STATUS_INVALID_PARAMETER;

        if (Operation[Index].hashOperation == BCRYPT_HASH_OPERATION_HASH_DATA)
        {
            if (Operation[Index].pbBuffer == NULL && Operation[Index].cbBuffer != 0)
                return STATUS_INVALID_PARAMETER;
        }
        else if (Operation[Index].hashOperation == BCRYPT_HASH_OPERATION_FINISH_HASH)
        {
            if (Operation[Index].pbBuffer == NULL || Operation[Index].cbBuffer != Digest)
                return STATUS_INVALID_PARAMETER;
        }
        else
        {
            return STATUS_INVALID_PARAMETER;
        }
    }

    for (Index = 0; Index < Count; Index++)
    {
        if (Operation[Index].hashOperation == BCRYPT_HASH_OPERATION_HASH_DATA)
        {
            CngElementAppend(Hash, Operation[Index].iHash, Operation[Index].pbBuffer, Operation[Index].cbBuffer);
        }
        else
        {
            CngElementResult(Hash, Operation[Index].iHash, Operation[Index].pbBuffer, Digest);
            CngElementStart(Hash, Operation[Index].iHash);
        }
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Copies a hash object with its current state.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_INVALID_PARAMETER,
 * STATUS_BUFFER_TOO_SMALL or STATUS_NO_MEMORY.
 */
NTSTATUS
WINAPI
BCryptDuplicateHash(
    _In_ BCRYPT_HASH_HANDLE Hash,
    _Out_ BCRYPT_HASH_HANDLE *NewHash,
    _Out_writes_bytes_all_opt_(HashObjectLength) PUCHAR HashObject,
    _In_ ULONG HashObjectLength,
    _In_ ULONG Flags)
{
    PCNG_HASH Source = CngLookupHash(Hash);
    const CNG_ALGORITHM *Algorithm;
    PCSYMCRYPT_MAC Mac;
    PCNG_HASH Copy;
    NTSTATUS Status;
    ULONG Index;

    if (Source == NULL || (Source->Flags & CNG_HASH_FINISHED))
        return STATUS_INVALID_HANDLE;
    if (NewHash == NULL || Flags != 0)
        return STATUS_INVALID_PARAMETER;

    Status = CngBuildHash(Source->Provider, Source->Count, HashObject, HashObjectLength, NULL, 0, 0, FALSE, &Copy);
    if (!NT_SUCCESS(Status))
        return Status;

    Algorithm = Source->Provider->Algorithm;
    Mac = CngMacForHash(Algorithm);
    Copy->Flags = Source->Flags;

    for (Index = 0; Index < Source->Count; Index++)
    {
        if (CngIsKeyed(Source->Provider))
        {
            RtlCopyMemory(CngElementKey(Copy, Index), CngElementKey(Source, Index), Mac->expandedKeySize);
            Algorithm->MacCopy(CngElementState(Source, Index), CngElementKey(Copy, Index), CngElementState(Copy, Index));
        }
        else
        {
            SymCryptHashStateCopy(*Algorithm->Hash, CngElementState(Source, Index), CngElementState(Copy, Index));
        }
    }

    *NewHash = Copy;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Destroys a hash object.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_PARAMETER for NULL, or STATUS_INVALID_HANDLE.
 */
NTSTATUS
WINAPI
BCryptDestroyHash(
    _Inout_ BCRYPT_HASH_HANDLE Hash)
{
    PCNG_HASH Object;

    if (Hash == NULL)
        return STATUS_INVALID_PARAMETER;

    Object = CngLookupHash(Hash);
    if (Object == NULL)
        return STATUS_INVALID_HANDLE;

    CngDestroyHashObject(Object);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Feeds data into a hash object.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE for a finished object, or
 * STATUS_INVALID_PARAMETER.
 */
NTSTATUS
WINAPI
BCryptHashData(
    _Inout_ BCRYPT_HASH_HANDLE Hash,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_ ULONG Flags)
{
    PCNG_HASH Object = CngLookupHash(Hash);

    if (Object == NULL || (Object->Flags & (CNG_HASH_FINISHED | CNG_HASH_MULTI)))
        return STATUS_INVALID_HANDLE;
    if (Flags != 0 || (Input == NULL && InputLength != 0))
        return STATUS_INVALID_PARAMETER;

    CngElementAppend(Object, 0, Input, InputLength);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Hands back the digest. A reusable object starts over, any other only
 * accepts BCryptDestroyHash() afterwards.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, or STATUS_INVALID_PARAMETER for an
 * output that is not exactly the digest length.
 */
NTSTATUS
WINAPI
BCryptFinishHash(
    _Inout_ BCRYPT_HASH_HANDLE Hash,
    _Out_writes_bytes_all_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _In_ ULONG Flags)
{
    PCNG_HASH Object = CngLookupHash(Hash);

    if (Object == NULL || (Object->Flags & (CNG_HASH_FINISHED | CNG_HASH_MULTI)))
        return STATUS_INVALID_HANDLE;
    if (Flags != 0 || Output == NULL || OutputLength != CngDigestLength(Object->Provider))
        return STATUS_INVALID_PARAMETER;

    return CngHashFinal(Object, Output, OutputLength);
}

/**
 * @brief
 * Hashes a buffer in one call.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_NOT_SUPPORTED,
 * STATUS_INVALID_PARAMETER or STATUS_NO_MEMORY.
 */
NTSTATUS
WINAPI
BCryptHash(
    _Inout_ BCRYPT_ALG_HANDLE Algorithm,
    _In_reads_bytes_opt_(SecretLength) PUCHAR Secret,
    _In_ ULONG SecretLength,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_all_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength)
{
    PCNG_PROVIDER Provider = CngLookupProvider(Algorithm);
    PCNG_HASH Hash;
    NTSTATUS Status;

    if (Provider == NULL)
        return STATUS_INVALID_HANDLE;
    if (Provider->Algorithm->Class != CngClassHash && Provider->Algorithm->Class != CngClassMac)
        return STATUS_NOT_SUPPORTED;
    if (Output == NULL || OutputLength != CngDigestLength(Provider) || (Input == NULL && InputLength != 0))
        return STATUS_INVALID_PARAMETER;

    Status = CngBuildHash(Provider, 1, NULL, 0, Secret, SecretLength, 0, TRUE, &Hash);
    if (!NT_SUCCESS(Status))
        return Status;

    CngElementAppend(Hash, 0, Input, InputLength);
    CngElementResult(Hash, 0, Output, OutputLength);
    CngDestroyHashObject(Hash);
    return STATUS_SUCCESS;
}

/* EOF */
