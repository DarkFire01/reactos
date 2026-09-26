/*
 * PROJECT:     ReactOS Kernel Cryptography
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Hashes and keyed hashes
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "bcryptk.h"

/* FUNCTIONS ******************************************************************/

/* How long a digest of this object is */
static
ULONG
BcryptkResultLength(
    _In_ PBCRYPTK_HASH Hash)
{
    if (Hash->Mac)
        return (ULONG)(*Hash->Algorithm->Mac)->resultSize;

    return (*Hash->Algorithm->Hash)->resultSize;
}

/**
 * @brief
 * Opens a hash, or a keyed hash when the provider was opened for one.
 *
 * @param[in,out] Object
 * Buffer the hash lives in, of the size the BCRYPT_OBJECT_LENGTH property
 * reports, or NULL to have one allocated.
 *
 * @param[in] Secret
 * The key of a keyed hash. A plain hash takes none.
 */
NTSTATUS
WINAPI
BCryptCreateHash(
    _In_ BCRYPT_ALG_HANDLE Algorithm,
    _Out_ BCRYPT_HASH_HANDLE *Hash,
    _Inout_updates_bytes_opt_(ObjectLength) PUCHAR Object,
    _In_ ULONG ObjectLength,
    _In_reads_bytes_opt_(SecretLength) PUCHAR Secret,
    _In_ ULONG SecretLength,
    _In_ ULONG Flags)
{
    PBCRYPTK_PROVIDER Provider = Algorithm;
    PBCRYPTK_HASH Entry;
    BOOLEAN Allocated;

    UNREFERENCED_PARAMETER(Flags);

    if (Provider == NULL || Provider->Tag != BCRYPTK_PROVIDER_TAG)
        return STATUS_INVALID_HANDLE;

    if (Hash == NULL)
        return STATUS_INVALID_PARAMETER;

    *Hash = NULL;

    if (Provider->Algorithm->Class != BcryptkClassHash)
        return STATUS_NOT_SUPPORTED;

    if (Provider->Mac && Secret == NULL)
        return STATUS_INVALID_PARAMETER;

    Entry = BcryptkAllocateObject(BcryptkHashObjectLength(Provider->Algorithm, Provider->Mac),
                                  Object,
                                  ObjectLength,
                                  &Allocated);
    if (Entry == NULL)
        return Object != NULL ? STATUS_BUFFER_TOO_SMALL : STATUS_INSUFFICIENT_RESOURCES;

    Entry->Algorithm = Provider->Algorithm;
    Entry->Allocated = Allocated;
    Entry->Mac = Provider->Mac;
    Entry->State = BcryptkAlign(&Entry->Storage[0]);

    if (Entry->Mac)
    {
        PCSYMCRYPT_MAC Mac = *Entry->Algorithm->Mac;

        Entry->ExpandedKey = BcryptkAlign((PUCHAR)Entry->State + Mac->stateSize);

        if (Mac->expandKeyFunc(Entry->ExpandedKey, Secret, SecretLength) != SYMCRYPT_NO_ERROR)
        {
            BcryptkFreeObject(Entry, Allocated);
            return STATUS_INVALID_PARAMETER;
        }

        Mac->initFunc(Entry->State, Entry->ExpandedKey);
    }
    else
    {
        SymCryptHashInit(*Entry->Algorithm->Hash, Entry->State);
    }

    Entry->Tag = BCRYPTK_HASH_TAG;
    *Hash = Entry;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Copies a hash part way through.
 *
 * @remarks
 * A keyed hash refers to its key from inside its own state, which only the
 * algorithm itself can move, so a copy of one is not offered.
 */
NTSTATUS
WINAPI
BCryptDuplicateHash(
    _In_ BCRYPT_HASH_HANDLE Hash,
    _Out_ BCRYPT_HASH_HANDLE *NewHash,
    _Inout_updates_bytes_opt_(ObjectLength) PUCHAR Object,
    _In_ ULONG ObjectLength,
    _In_ ULONG Flags)
{
    PBCRYPTK_HASH Source = Hash;
    PBCRYPTK_HASH Copy;
    BOOLEAN Allocated;

    UNREFERENCED_PARAMETER(Flags);

    if (Source == NULL || Source->Tag != BCRYPTK_HASH_TAG)
        return STATUS_INVALID_HANDLE;

    if (NewHash == NULL)
        return STATUS_INVALID_PARAMETER;

    *NewHash = NULL;

    if (Source->Mac)
        return STATUS_NOT_SUPPORTED;

    Copy = BcryptkAllocateObject(BcryptkHashObjectLength(Source->Algorithm, FALSE),
                                 Object,
                                 ObjectLength,
                                 &Allocated);
    if (Copy == NULL)
        return Object != NULL ? STATUS_BUFFER_TOO_SMALL : STATUS_INSUFFICIENT_RESOURCES;

    Copy->Algorithm = Source->Algorithm;
    Copy->Allocated = Allocated;
    Copy->Mac = FALSE;
    Copy->State = BcryptkAlign(&Copy->Storage[0]);

    SymCryptHashStateCopy(*Source->Algorithm->Hash, Source->State, Copy->State);

    Copy->Tag = BCRYPTK_HASH_TAG;
    *NewHash = Copy;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Feeds data into a hash.
 */
NTSTATUS
WINAPI
BCryptHashData(
    _Inout_ BCRYPT_HASH_HANDLE Hash,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_ ULONG Flags)
{
    PBCRYPTK_HASH Entry = Hash;

    UNREFERENCED_PARAMETER(Flags);

    if (Entry == NULL || Entry->Tag != BCRYPTK_HASH_TAG)
        return STATUS_INVALID_HANDLE;

    if (Input == NULL && InputLength != 0)
        return STATUS_INVALID_PARAMETER;

    if (InputLength == 0)
        return STATUS_SUCCESS;

    if (Entry->Mac)
        (*Entry->Algorithm->Mac)->appendFunc(Entry->State, Input, InputLength);
    else
        SymCryptHashAppend(*Entry->Algorithm->Hash, Entry->State, Input, InputLength);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Closes a hash and hands back the digest.
 *
 * @remarks
 * The state is left ready for another message, which is what a caller that
 * asked for a reusable hash goes on to do.
 */
NTSTATUS
WINAPI
BCryptFinishHash(
    _Inout_ BCRYPT_HASH_HANDLE Hash,
    _Out_writes_bytes_all_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _In_ ULONG Flags)
{
    PBCRYPTK_HASH Entry = Hash;

    UNREFERENCED_PARAMETER(Flags);

    if (Entry == NULL || Entry->Tag != BCRYPTK_HASH_TAG)
        return STATUS_INVALID_HANDLE;

    if (Output == NULL)
        return STATUS_INVALID_PARAMETER;

    if (OutputLength != BcryptkResultLength(Entry))
        return STATUS_INVALID_PARAMETER;

    if (Entry->Mac)
        (*Entry->Algorithm->Mac)->resultFunc(Entry->State, Output);
    else
        SymCryptHashResult(*Entry->Algorithm->Hash, Entry->State, Output, OutputLength);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Destroys a hash and wipes its state.
 */
NTSTATUS
WINAPI
BCryptDestroyHash(
    _In_ BCRYPT_HASH_HANDLE Hash)
{
    PBCRYPTK_HASH Entry = Hash;
    ULONG Length;
    BOOLEAN Allocated;

    if (Entry == NULL || Entry->Tag != BCRYPTK_HASH_TAG)
        return STATUS_INVALID_HANDLE;

    Length = BcryptkHashObjectLength(Entry->Algorithm, Entry->Mac);
    Allocated = Entry->Allocated;

    SymCryptWipe(Entry, Length);
    BcryptkFreeObject(Entry, Allocated);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Hashes a single buffer without an object to keep track of.
 */
NTSTATUS
WINAPI
BCryptHash(
    _In_ BCRYPT_ALG_HANDLE Algorithm,
    _In_reads_bytes_opt_(SecretLength) PUCHAR Secret,
    _In_ ULONG SecretLength,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_all_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength)
{
    BCRYPT_HASH_HANDLE Hash;
    NTSTATUS Status;

    Status = BCryptCreateHash(Algorithm, &Hash, NULL, 0, Secret, SecretLength, 0);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = BCryptHashData(Hash, Input, InputLength, 0);
    if (NT_SUCCESS(Status))
        Status = BCryptFinishHash(Hash, Output, OutputLength, 0);

    BCryptDestroyHash(Hash);

    return Status;
}

/* EOF */
