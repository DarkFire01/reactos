/*
 * PROJECT:     ReactOS Cryptography Next Generation kernel provider
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The cng.sys entry points drivers import to check a signed image
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * These satisfy the imports of drivers that hash and verify a signed firmware
 * image through cng.sys. They compute nothing: the provider, hash and key
 * handles are opaque, and a signature is always reported valid, so a driver
 * carrying a genuine image loads.
 */

#include <ntifs.h>
#include <bcrypt.h>

static ULONG CngProvider;
static ULONG CngHash;
static ULONG CngKey;

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
        *Algorithm = &CngProvider;
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
BCryptCloseAlgorithmProvider(
    _In_ BCRYPT_ALG_HANDLE Algorithm,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(Algorithm);
    UNREFERENCED_PARAMETER(Flags);

    return STATUS_SUCCESS;
}

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
    return STATUS_SUCCESS;
}

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
        *Hash = &CngHash;
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
BCryptDestroyHash(
    _In_ BCRYPT_HASH_HANDLE Hash)
{
    UNREFERENCED_PARAMETER(Hash);

    return STATUS_SUCCESS;
}

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

    return STATUS_SUCCESS;
}

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
    return STATUS_SUCCESS;
}

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
        *Key = &CngKey;
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
BCryptDestroyKey(
    _In_ BCRYPT_KEY_HANDLE Key)
{
    UNREFERENCED_PARAMETER(Key);

    return STATUS_SUCCESS;
}

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

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    return STATUS_SUCCESS;
}
