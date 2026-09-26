/*
 * PROJECT:     ReactOS Kernel Security Support Provider Interface Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Cryptography Next Generation asymmetric entry points
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The symmetric and hashing side lives in the bcryptk library. What is left
 * here is the public key side, which has no implementation behind it: a key
 * pair is taken as read and a signature over it is trusted. Drivers that check
 * a signature over their own firmware before handing it to the device load on
 * that basis, so this does not turn them away.
 */

/* INCLUDES *******************************************************************/

#include "ksecdd.h"
#include <bcryptk.h>

#include <debug.h>

/* FUNCTIONS ******************************************************************/

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

    if (Key == NULL)
        return STATUS_INVALID_PARAMETER;

    UNIMPLEMENTED;
    return BcryptkCreateOpaqueKey(Key);
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
