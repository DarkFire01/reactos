/*
 * PROJECT:     ReactOS Kernel Security Support Provider Interface Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Cryptography Next Generation hashing entry points
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ksecdd.h"
#include <bcrypt.h>

#include <debug.h>

/*
 * FIXME: These only satisfy the imports of the drivers that link against
 * ksecdd. None of them hash anything yet.
 */

/* An opaque non-NULL value handed back as a key handle */
static ULONG KsecCryptKey;

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Opens an algorithm provider.
 *
 * @param[out] Algorithm
 * Receives the handle of the provider.
 *
 * @param[in] AlgorithmId
 * Names the algorithm to open.
 *
 * @param[in] Implementation
 * Optionally names the implementation to open the algorithm from.
 *
 * @param[in] Flags
 * Provider flags.
 *
 * @return
 * STATUS_SUCCESS.
 *
 * @unimplemented
 */
NTSTATUS
WINAPI
BCryptOpenAlgorithmProvider(
    _Out_ BCRYPT_ALG_HANDLE *Algorithm,
    _In_ LPCWSTR AlgorithmId,
    _In_opt_ LPCWSTR Implementation,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(AlgorithmId);
    UNREFERENCED_PARAMETER(Implementation);
    UNREFERENCED_PARAMETER(Flags);

    if (Algorithm != NULL)
        *Algorithm = NULL;

    UNIMPLEMENTED;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Closes an algorithm provider.
 *
 * @param[in] Algorithm
 * The handle returned by BCryptOpenAlgorithmProvider().
 *
 * @param[in] Flags
 * Provider flags.
 *
 * @return
 * STATUS_SUCCESS.
 *
 * @unimplemented
 */
NTSTATUS
WINAPI
BCryptCloseAlgorithmProvider(
    _In_ BCRYPT_ALG_HANDLE Algorithm,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(Algorithm);
    UNREFERENCED_PARAMETER(Flags);

    UNIMPLEMENTED;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reads a property of an algorithm provider or a hash object.
 *
 * @param[in] Object
 * The object to read from.
 *
 * @param[in] Property
 * Names the property to read.
 *
 * @param[out] Output
 * Receives the value of the property.
 *
 * @param[in] OutputLength
 * Size of @p Output, in bytes.
 *
 * @param[out] ResultLength
 * Receives the number of bytes written to @p Output.
 *
 * @param[in] Flags
 * Property flags.
 *
 * @return
 * STATUS_SUCCESS.
 *
 * @unimplemented
 */
NTSTATUS
WINAPI
BCryptGetProperty(
    _In_ BCRYPT_HANDLE Object,
    _In_ LPCWSTR Property,
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(Object);
    UNREFERENCED_PARAMETER(Property);
    UNREFERENCED_PARAMETER(Output);
    UNREFERENCED_PARAMETER(OutputLength);
    UNREFERENCED_PARAMETER(Flags);

    if (ResultLength != NULL)
        *ResultLength = 0;

    UNIMPLEMENTED;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Creates a hash object.
 *
 * @param[in] Algorithm
 * The provider the hash is created from.
 *
 * @param[out] Hash
 * Receives the handle of the hash object.
 *
 * @param[in] Object
 * Optional caller owned buffer that backs the hash object.
 *
 * @param[in] ObjectLength
 * Size of @p Object, in bytes.
 *
 * @param[in] Secret
 * Optional key of a keyed hash.
 *
 * @param[in] SecretLength
 * Size of @p Secret, in bytes.
 *
 * @param[in] Flags
 * Hash flags.
 *
 * @return
 * STATUS_SUCCESS.
 *
 * @unimplemented
 */
NTSTATUS
WINAPI
BCryptCreateHash(
    _In_ BCRYPT_ALG_HANDLE Algorithm,
    _Out_ BCRYPT_HASH_HANDLE *Hash,
    _Out_writes_bytes_opt_(ObjectLength) PUCHAR Object,
    _In_ ULONG ObjectLength,
    _In_reads_bytes_opt_(SecretLength) PUCHAR Secret,
    _In_ ULONG SecretLength,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(Algorithm);
    UNREFERENCED_PARAMETER(Object);
    UNREFERENCED_PARAMETER(ObjectLength);
    UNREFERENCED_PARAMETER(Secret);
    UNREFERENCED_PARAMETER(SecretLength);
    UNREFERENCED_PARAMETER(Flags);

    if (Hash != NULL)
        *Hash = NULL;

    UNIMPLEMENTED;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Destroys a hash object.
 *
 * @param[in] Hash
 * The handle returned by BCryptCreateHash().
 *
 * @return
 * STATUS_SUCCESS.
 *
 * @unimplemented
 */
NTSTATUS
WINAPI
BCryptDestroyHash(
    _In_ BCRYPT_HASH_HANDLE Hash)
{
    UNREFERENCED_PARAMETER(Hash);

    UNIMPLEMENTED;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Feeds data into a hash object.
 *
 * @param[in] Hash
 * The handle returned by BCryptCreateHash().
 *
 * @param[in] Input
 * The data to hash.
 *
 * @param[in] InputLength
 * Size of @p Input, in bytes.
 *
 * @param[in] Flags
 * Hash flags.
 *
 * @return
 * STATUS_SUCCESS.
 *
 * @unimplemented
 */
NTSTATUS
WINAPI
BCryptHashData(
    _In_ BCRYPT_HASH_HANDLE Hash,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(Hash);
    UNREFERENCED_PARAMETER(Input);
    UNREFERENCED_PARAMETER(InputLength);
    UNREFERENCED_PARAMETER(Flags);

    UNIMPLEMENTED;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Closes a hash object and hands back the digest.
 *
 * @param[in] Hash
 * The handle returned by BCryptCreateHash().
 *
 * @param[out] Output
 * Receives the digest.
 *
 * @param[in] OutputLength
 * Size of @p Output, in bytes.
 *
 * @param[in] Flags
 * Hash flags.
 *
 * @return
 * STATUS_SUCCESS.
 *
 * @unimplemented
 */
NTSTATUS
WINAPI
BCryptFinishHash(
    _In_ BCRYPT_HASH_HANDLE Hash,
    _Out_writes_bytes_all_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(Hash);
    UNREFERENCED_PARAMETER(Flags);

    if (Output != NULL)
        RtlZeroMemory(Output, OutputLength);

    UNIMPLEMENTED;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Imports a public key pair for signature checks.
 *
 * @unimplemented
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
    UNREFERENCED_PARAMETER(Algorithm);
    UNREFERENCED_PARAMETER(ImportKey);
    UNREFERENCED_PARAMETER(BlobType);
    UNREFERENCED_PARAMETER(Input);
    UNREFERENCED_PARAMETER(InputLength);
    UNREFERENCED_PARAMETER(Flags);

    if (Key != NULL)
        *Key = (BCRYPT_KEY_HANDLE)&KsecCryptKey;

    UNIMPLEMENTED;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Destroys a key.
 *
 * @unimplemented
 */
NTSTATUS
WINAPI
BCryptDestroyKey(
    _In_ BCRYPT_KEY_HANDLE Key)
{
    UNREFERENCED_PARAMETER(Key);

    UNIMPLEMENTED;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Verifies a signature over a hash. A stub trusts the signature.
 *
 * @unimplemented
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
    UNREFERENCED_PARAMETER(Key);
    UNREFERENCED_PARAMETER(PaddingInfo);
    UNREFERENCED_PARAMETER(Hash);
    UNREFERENCED_PARAMETER(HashLength);
    UNREFERENCED_PARAMETER(Signature);
    UNREFERENCED_PARAMETER(SignatureLength);
    UNREFERENCED_PARAMETER(Flags);

    UNIMPLEMENTED;
    return STATUS_SUCCESS;
}

/* EOF */
