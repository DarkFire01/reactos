/*
 * PROJECT:     ReactOS Kernel Cryptography Next Generation
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Algorithm providers, pseudo handles, properties and enumeration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "cngp.h"

#include <debug.h>

/* DEFINES ********************************************************************/

#define CNG_OPEN_FLAGS          (BCRYPT_PROV_DISPATCH | BCRYPT_ALG_HANDLE_HMAC_FLAG | \
                                 BCRYPT_HASH_REUSABLE_FLAG | BCRYPT_MULTI_FLAG)

/* Pseudo handles carry their table index above a low nibble of 1 */
#define CNG_PSEUDO_TAG          0x1
#define CNG_PSEUDO_COUNT        0x3B

#define CNG_ENUM_OPERATIONS     (BCRYPT_CIPHER_OPERATION | BCRYPT_HASH_OPERATION | \
                                 BCRYPT_ASYMMETRIC_ENCRYPTION_OPERATION | \
                                 BCRYPT_SECRET_AGREEMENT_OPERATION | BCRYPT_SIGNATURE_OPERATION | \
                                 BCRYPT_RNG_OPERATION | BCRYPT_KEY_DERIVATION_OPERATION)

/* GLOBALS ********************************************************************/

extern const PCSYMCRYPT_MAC CngHmacMd2Algorithm;
extern const PCSYMCRYPT_MAC CngHmacMd4Algorithm;

static const CNG_CURVE CngCurves[] =
{
    { BCRYPT_ECC_CURVE_NISTP256, &SymCryptEcurveParamsNistP256, 256,
      BCRYPT_ECDSA_PUBLIC_P256_MAGIC, BCRYPT_ECDSA_PRIVATE_P256_MAGIC,
      BCRYPT_ECDH_PUBLIC_P256_MAGIC, BCRYPT_ECDH_PRIVATE_P256_MAGIC },
    { BCRYPT_ECC_CURVE_NISTP384, &SymCryptEcurveParamsNistP384, 384,
      BCRYPT_ECDSA_PUBLIC_P384_MAGIC, BCRYPT_ECDSA_PRIVATE_P384_MAGIC,
      BCRYPT_ECDH_PUBLIC_P384_MAGIC, BCRYPT_ECDH_PRIVATE_P384_MAGIC },
    { BCRYPT_ECC_CURVE_NISTP521, &SymCryptEcurveParamsNistP521, 521,
      BCRYPT_ECDSA_PUBLIC_P521_MAGIC, BCRYPT_ECDSA_PRIVATE_P521_MAGIC,
      BCRYPT_ECDH_PUBLIC_P521_MAGIC, BCRYPT_ECDH_PRIVATE_P521_MAGIC },
};

/* The SECG names of the NIST curves are accepted as well */
static const PCWSTR CngCurveAliases[] =
{
    BCRYPT_ECC_CURVE_SECP256R1,
    BCRYPT_ECC_CURVE_SECP384R1,
    BCRYPT_ECC_CURVE_SECP521R1,
};

#define HASH_ENTRY(Name, Hash, Mac, Copy) \
    { Name, CngClassHash, BCRYPT_HASH_INTERFACE, BCRYPT_HASH_OPERATION, &Hash, &Mac, (PCNG_MAC_STATE_COPY)Copy }
#define CIPHER_ENTRY(Name, Cipher) \
    { Name, CngClassCipher, BCRYPT_CIPHER_INTERFACE, BCRYPT_CIPHER_OPERATION, NULL, NULL, NULL, Cipher }
#define KDF_ENTRY(Name, Kdf) \
    { Name, CngClassKdf, BCRYPT_KEY_DERIVATION_INTERFACE, BCRYPT_KEY_DERIVATION_OPERATION, NULL, NULL, NULL, 0, Kdf }

/* Ordered as Windows enumerates them */
static const CNG_ALGORITHM CngAlgorithms[] =
{
    HASH_ENTRY(BCRYPT_SHA256_ALGORITHM, SymCryptSha256Algorithm, SymCryptHmacSha256Algorithm, SymCryptHmacSha256StateCopy),
    HASH_ENTRY(BCRYPT_SHA384_ALGORITHM, SymCryptSha384Algorithm, SymCryptHmacSha384Algorithm, SymCryptHmacSha384StateCopy),
    HASH_ENTRY(BCRYPT_SHA512_ALGORITHM, SymCryptSha512Algorithm, SymCryptHmacSha512Algorithm, SymCryptHmacSha512StateCopy),
    HASH_ENTRY(BCRYPT_SHA3_256_ALGORITHM, SymCryptSha3_256Algorithm, SymCryptHmacSha3_256Algorithm, SymCryptHmacSha3_256StateCopy),
    HASH_ENTRY(BCRYPT_SHA3_384_ALGORITHM, SymCryptSha3_384Algorithm, SymCryptHmacSha3_384Algorithm, SymCryptHmacSha3_384StateCopy),
    HASH_ENTRY(BCRYPT_SHA3_512_ALGORITHM, SymCryptSha3_512Algorithm, SymCryptHmacSha3_512Algorithm, SymCryptHmacSha3_512StateCopy),
    HASH_ENTRY(BCRYPT_SHA1_ALGORITHM, SymCryptSha1Algorithm, SymCryptHmacSha1Algorithm, SymCryptHmacSha1StateCopy),
    HASH_ENTRY(BCRYPT_MD5_ALGORITHM, SymCryptMd5Algorithm, SymCryptHmacMd5Algorithm, SymCryptHmacMd5StateCopy),
    HASH_ENTRY(BCRYPT_MD4_ALGORITHM, SymCryptMd4Algorithm, CngHmacMd4Algorithm, SymCryptHmacStateCopy),
    HASH_ENTRY(BCRYPT_MD2_ALGORITHM, SymCryptMd2Algorithm, CngHmacMd2Algorithm, SymCryptHmacStateCopy),
    { BCRYPT_AES_CMAC_ALGORITHM, CngClassMac, BCRYPT_HASH_INTERFACE, BCRYPT_HASH_OPERATION,
      NULL, &SymCryptAesCmacAlgorithm, (PCNG_MAC_STATE_COPY)SymCryptAesCmacStateCopy },
    CIPHER_ENTRY(BCRYPT_AES_ALGORITHM, CngCipherAes),
    CIPHER_ENTRY(BCRYPT_3DES_ALGORITHM, CngCipher3Des),
    CIPHER_ENTRY(BCRYPT_3DES_112_ALGORITHM, CngCipher3Des112),
    CIPHER_ENTRY(BCRYPT_DES_ALGORITHM, CngCipherDes),
    CIPHER_ENTRY(BCRYPT_RC4_ALGORITHM, CngCipherRc4),
    { BCRYPT_RSA_ALGORITHM, CngClassRsa, BCRYPT_ASYMMETRIC_ENCRYPTION_INTERFACE,
      BCRYPT_ASYMMETRIC_ENCRYPTION_OPERATION },
    { BCRYPT_RSA_SIGN_ALGORITHM, CngClassRsa, BCRYPT_SIGNATURE_INTERFACE, BCRYPT_SIGNATURE_OPERATION,
      NULL, NULL, NULL, 0, 0, NULL, TRUE },
    { BCRYPT_ECDSA_P256_ALGORITHM, CngClassEcdsa, BCRYPT_SIGNATURE_INTERFACE, BCRYPT_SIGNATURE_OPERATION,
      NULL, NULL, NULL, 0, 0, &CngCurves[0] },
    { BCRYPT_ECDSA_P384_ALGORITHM, CngClassEcdsa, BCRYPT_SIGNATURE_INTERFACE, BCRYPT_SIGNATURE_OPERATION,
      NULL, NULL, NULL, 0, 0, &CngCurves[1] },
    { BCRYPT_ECDSA_P521_ALGORITHM, CngClassEcdsa, BCRYPT_SIGNATURE_INTERFACE, BCRYPT_SIGNATURE_OPERATION,
      NULL, NULL, NULL, 0, 0, &CngCurves[2] },
    { BCRYPT_ECDSA_ALGORITHM, CngClassEcdsa, BCRYPT_SIGNATURE_INTERFACE, BCRYPT_SIGNATURE_OPERATION },
    { BCRYPT_ECDH_P256_ALGORITHM, CngClassEcdh, BCRYPT_SECRET_AGREEMENT_INTERFACE, BCRYPT_SECRET_AGREEMENT_OPERATION,
      NULL, NULL, NULL, 0, 0, &CngCurves[0] },
    { BCRYPT_ECDH_P384_ALGORITHM, CngClassEcdh, BCRYPT_SECRET_AGREEMENT_INTERFACE, BCRYPT_SECRET_AGREEMENT_OPERATION,
      NULL, NULL, NULL, 0, 0, &CngCurves[1] },
    { BCRYPT_ECDH_P521_ALGORITHM, CngClassEcdh, BCRYPT_SECRET_AGREEMENT_INTERFACE, BCRYPT_SECRET_AGREEMENT_OPERATION,
      NULL, NULL, NULL, 0, 0, &CngCurves[2] },
    { BCRYPT_ECDH_ALGORITHM, CngClassEcdh, BCRYPT_SECRET_AGREEMENT_INTERFACE, BCRYPT_SECRET_AGREEMENT_OPERATION },
    { BCRYPT_RNG_ALGORITHM, CngClassRng, BCRYPT_RNG_INTERFACE, BCRYPT_RNG_OPERATION },
    KDF_ENTRY(BCRYPT_PBKDF2_ALGORITHM, CngKdfPbkdf2),
    KDF_ENTRY(BCRYPT_SP800108_CTR_HMAC_ALGORITHM, CngKdfSp800108),
    KDF_ENTRY(BCRYPT_SP80056A_CONCAT_ALGORITHM, CngKdfSp80056a),
    KDF_ENTRY(BCRYPT_HKDF_ALGORITHM, CngKdfHkdf),
    KDF_ENTRY(BCRYPT_CAPI_KDF_ALGORITHM, CngKdfCapi),
    KDF_ENTRY(BCRYPT_TLS1_1_KDF_ALGORITHM, CngKdfTls11),
    KDF_ENTRY(BCRYPT_TLS1_2_KDF_ALGORITHM, CngKdfTls12),
};

typedef struct _CNG_PSEUDO_ENTRY
{
    PCWSTR Name;
    ULONG Flags;
    CNG_MODE Mode;
} CNG_PSEUDO_ENTRY;

#define PSEUDO_HASH(Name)       { Name, BCRYPT_HASH_REUSABLE_FLAG | BCRYPT_MULTI_FLAG }
#define PSEUDO_HMAC(Name)       { Name, BCRYPT_HASH_REUSABLE_FLAG | BCRYPT_ALG_HANDLE_HMAC_FLAG }
#define PSEUDO_CIPHER(Name, M)  { Name, 0, M }

/* Indexed by the pseudo handle value shifted right by four */
static const CNG_PSEUDO_ENTRY CngPseudoEntries[CNG_PSEUDO_COUNT] =
{
    PSEUDO_HASH(BCRYPT_MD2_ALGORITHM),
    PSEUDO_HASH(BCRYPT_MD4_ALGORITHM),
    PSEUDO_HASH(BCRYPT_MD5_ALGORITHM),
    PSEUDO_HASH(BCRYPT_SHA1_ALGORITHM),
    PSEUDO_HASH(BCRYPT_SHA256_ALGORITHM),
    PSEUDO_HASH(BCRYPT_SHA384_ALGORITHM),
    PSEUDO_HASH(BCRYPT_SHA512_ALGORITHM),
    { BCRYPT_RC4_ALGORITHM },
    { BCRYPT_RNG_ALGORITHM },
    PSEUDO_HMAC(BCRYPT_MD5_ALGORITHM),
    PSEUDO_HMAC(BCRYPT_SHA1_ALGORITHM),
    PSEUDO_HMAC(BCRYPT_SHA256_ALGORITHM),
    PSEUDO_HMAC(BCRYPT_SHA384_ALGORITHM),
    PSEUDO_HMAC(BCRYPT_SHA512_ALGORITHM),
    { BCRYPT_RSA_ALGORITHM },
    { BCRYPT_ECDSA_ALGORITHM },
    PSEUDO_HASH(BCRYPT_AES_CMAC_ALGORITHM),
    { NULL },
    PSEUDO_HMAC(BCRYPT_MD2_ALGORITHM),
    PSEUDO_HMAC(BCRYPT_MD4_ALGORITHM),
    PSEUDO_CIPHER(BCRYPT_3DES_ALGORITHM, CngModeCbc),
    PSEUDO_CIPHER(BCRYPT_3DES_ALGORITHM, CngModeEcb),
    PSEUDO_CIPHER(BCRYPT_3DES_ALGORITHM, CngModeCfb),
    PSEUDO_CIPHER(BCRYPT_3DES_112_ALGORITHM, CngModeCbc),
    PSEUDO_CIPHER(BCRYPT_3DES_112_ALGORITHM, CngModeEcb),
    PSEUDO_CIPHER(BCRYPT_3DES_112_ALGORITHM, CngModeCfb),
    PSEUDO_CIPHER(BCRYPT_AES_ALGORITHM, CngModeCbc),
    PSEUDO_CIPHER(BCRYPT_AES_ALGORITHM, CngModeEcb),
    PSEUDO_CIPHER(BCRYPT_AES_ALGORITHM, CngModeCfb),
    PSEUDO_CIPHER(BCRYPT_AES_ALGORITHM, CngModeCcm),
    PSEUDO_CIPHER(BCRYPT_AES_ALGORITHM, CngModeGcm),
    PSEUDO_CIPHER(BCRYPT_DES_ALGORITHM, CngModeCbc),
    PSEUDO_CIPHER(BCRYPT_DES_ALGORITHM, CngModeEcb),
    PSEUDO_CIPHER(BCRYPT_DES_ALGORITHM, CngModeCfb),
    { NULL }, { NULL }, { NULL }, { NULL }, { NULL }, { NULL }, { NULL },
    { BCRYPT_ECDH_ALGORITHM },
    { BCRYPT_ECDH_P256_ALGORITHM },
    { BCRYPT_ECDH_P384_ALGORITHM },
    { BCRYPT_ECDH_P521_ALGORITHM },
    { NULL },
    { BCRYPT_ECDSA_P256_ALGORITHM },
    { BCRYPT_ECDSA_P384_ALGORITHM },
    { BCRYPT_ECDSA_P521_ALGORITHM },
    { BCRYPT_RSA_SIGN_ALGORITHM },
    { BCRYPT_CAPI_KDF_ALGORITHM },
    { BCRYPT_PBKDF2_ALGORITHM },
    { BCRYPT_SP800108_CTR_HMAC_ALGORITHM },
    { BCRYPT_SP80056A_CONCAT_ALGORITHM },
    { BCRYPT_TLS1_1_KDF_ALGORITHM },
    { BCRYPT_TLS1_2_KDF_ALGORITHM },
    { NULL },
    { BCRYPT_HKDF_ALGORITHM },
    { NULL },
};

static PCNG_PROVIDER CngPseudoProviders[CNG_PSEUDO_COUNT];

/* HELPERS ********************************************************************/

const CNG_ALGORITHM *
NTAPI
CngFindAlgorithm(
    _In_ LPCWSTR Name)
{
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(CngAlgorithms); Index++)
    {
        if (wcscmp(Name, CngAlgorithms[Index].Name) == 0)
            return &CngAlgorithms[Index];
    }

    return NULL;
}

const CNG_CURVE *
NTAPI
CngFindCurve(
    _In_ LPCWSTR Name)
{
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(CngCurves); Index++)
    {
        if (wcscmp(Name, CngCurves[Index].Name) == 0 || wcscmp(Name, CngCurveAliases[Index]) == 0)
            return &CngCurves[Index];
    }

    return NULL;
}

const CNG_CURVE *
NTAPI
CngFindCurveByMagic(
    _In_ ULONG Magic,
    _In_ BOOLEAN Ecdh,
    _Out_ PBOOLEAN Private)
{
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(CngCurves); Index++)
    {
        if (Magic == (Ecdh ? CngCurves[Index].EcdhPublicMagic : CngCurves[Index].EcdsaPublicMagic))
        {
            *Private = FALSE;
            return &CngCurves[Index];
        }

        if (Magic == (Ecdh ? CngCurves[Index].EcdhPrivateMagic : CngCurves[Index].EcdsaPrivateMagic))
        {
            *Private = TRUE;
            return &CngCurves[Index];
        }
    }

    return NULL;
}

/**
 * @brief
 * Compares a string property input against a value. The input may lack its
 * terminator, and a shorter input matches when it is a prefix of the value.
 */
BOOLEAN
NTAPI
CngPropertyIs(
    _In_reads_bytes_(Length) const UCHAR *Input,
    _In_ ULONG Length,
    _In_ PCWSTR Value)
{
    PCWSTR Text = (PCWSTR)Input;
    SIZE_T Chars = Length / sizeof(WCHAR);
    SIZE_T Used;

    Used = 0;
    while (Used < Chars && Text[Used] != UNICODE_NULL)
        Used++;

    if (Used > wcslen(Value))
        return FALSE;

    return wcsncmp(Text, Value, Used) == 0;
}

/**
 * @brief
 * Copies a property value out with the usual size reporting.
 */
NTSTATUS
NTAPI
CngReturnProperty(
    _In_reads_bytes_(Size) const VOID *Value,
    _In_ ULONG Size,
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    *ResultLength = Size;
    if (Output == NULL)
        return STATUS_SUCCESS;
    if (OutputLength < Size)
        return STATUS_BUFFER_TOO_SMALL;

    RtlCopyMemory(Output, Value, Size);
    return STATUS_SUCCESS;
}

static
NTSTATUS
CngCreateProvider(
    _In_ const CNG_ALGORITHM *Algorithm,
    _In_ ULONG Flags,
    _In_ BOOLEAN Pseudo,
    _Out_ PCNG_PROVIDER *Result)
{
    PCNG_PROVIDER Provider;

    CngInitialize();

    Provider = CngAllocate(sizeof(*Provider));
    if (Provider == NULL)
        return STATUS_NO_MEMORY;

    RtlZeroMemory(Provider, sizeof(*Provider));
    Provider->Signature = CNG_PROVIDER_SIGNATURE;
    Provider->References = 1;
    Provider->Algorithm = Algorithm;
    Provider->Flags = Flags;
    Provider->Pseudo = Pseudo;
    Provider->Curve = Algorithm->Curve;

    if (Algorithm->Class == CngClassCipher)
    {
        Provider->Mode = (Algorithm->Cipher == CngCipherRc4) ? CngModeNone : CngModeCbc;
        Provider->MessageBlock = 1;
    }

    *Result = Provider;
    return STATUS_SUCCESS;
}

static
VOID
CngDestroyProvider(
    _In_ PCNG_PROVIDER Provider)
{
    Provider->Signature = 0;
    CngFree(Provider);
}

/* Pseudo providers are opened at passive level on first use and kept forever */
static
PCNG_PROVIDER
CngPseudoProvider(
    _In_ ULONG_PTR Value)
{
    const CNG_PSEUDO_ENTRY *Entry;
    const CNG_ALGORITHM *Algorithm;
    PCNG_PROVIDER Provider, Existing;
    ULONG_PTR Index = Value >> 4;

    if (KeGetCurrentIrql() >= DISPATCH_LEVEL || Index >= CNG_PSEUDO_COUNT)
        return NULL;

    Provider = CngPseudoProviders[Index];
    if (Provider != NULL)
        return Provider;

    Entry = &CngPseudoEntries[Index];
    if (Entry->Name == NULL)
        return NULL;

    Algorithm = CngFindAlgorithm(Entry->Name);
    if (Algorithm == NULL)
        return NULL;
    if (!NT_SUCCESS(CngCreateProvider(Algorithm, Entry->Flags, TRUE, &Provider)))
        return NULL;

    if (Entry->Mode != CngModeNone)
        Provider->Mode = Entry->Mode;

    Existing = InterlockedCompareExchangePointer((PVOID *)&CngPseudoProviders[Index], Provider, NULL);
    if (Existing != NULL)
    {
        CngDestroyProvider(Provider);
        return Existing;
    }

    return Provider;
}

/**
 * @brief
 * Resolves a provider or pseudo handle. A provider opened without
 * BCRYPT_PROV_DISPATCH, and any pseudo handle, is invalid at dispatch level.
 */
PCNG_PROVIDER
NTAPI
CngLookupProvider(
    _In_opt_ BCRYPT_HANDLE Handle)
{
    PCNG_PROVIDER Provider = (PCNG_PROVIDER)Handle;

    if (Provider == NULL)
        return NULL;

    if (((ULONG_PTR)Handle & 0xF) == CNG_PSEUDO_TAG)
        return CngPseudoProvider((ULONG_PTR)Handle);

    if (Provider->Signature != CNG_PROVIDER_SIGNATURE || !CngObjectUsable(Provider))
        return NULL;

    return Provider;
}

/**
 * @brief
 * Tells whether objects of a provider may be used at the current IRQL.
 */
BOOLEAN
NTAPI
CngObjectUsable(
    _In_ PCNG_PROVIDER Provider)
{
    if (Provider->Flags & BCRYPT_PROV_DISPATCH)
        return TRUE;

    return KeGetCurrentIrql() < DISPATCH_LEVEL;
}

VOID
NTAPI
CngReferenceProvider(
    _In_ PCNG_PROVIDER Provider)
{
    InterlockedIncrement(&Provider->References);
}

VOID
NTAPI
CngDereferenceProvider(
    _In_ PCNG_PROVIDER Provider)
{
    if (InterlockedDecrement(&Provider->References) == 0 && !Provider->Pseudo)
        CngDestroyProvider(Provider);
}

static
NTSTATUS
CngQueryProviderProperty(
    _In_ PCNG_PROVIDER Provider,
    _In_opt_ PCNG_KEY Key,
    _In_ LPCWSTR Property,
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    switch (Provider->Algorithm->Class)
    {
        case CngClassHash:
        case CngClassMac:
            return CngQueryHashProperty(Provider, Property, Output, OutputLength, ResultLength);

        case CngClassRng:
            return STATUS_NOT_SUPPORTED;

        case CngClassRsa:
        case CngClassEcdsa:
        case CngClassEcdh:
            return CngQueryAsymmetricProperty(Provider, Key, Property, Output, OutputLength, ResultLength);

        default:
            return CngQueryKeyProperty(Provider, Key, Property, Output, OutputLength, ResultLength);
    }
}

/* EXPORTS ********************************************************************/

/**
 * @brief
 * Opens an algorithm provider.
 *
 * @param[out] Algorithm
 * Receives the provider handle.
 *
 * @param[in] AlgorithmId
 * The algorithm name. Names are case sensitive.
 *
 * @param[in] Implementation
 * NULL or MS_PRIMITIVE_PROVIDER.
 *
 * @param[in] Flags
 * BCRYPT_ALG_HANDLE_HMAC_FLAG, BCRYPT_HASH_REUSABLE_FLAG, BCRYPT_MULTI_FLAG
 * and BCRYPT_PROV_DISPATCH, which makes the provider and its objects usable
 * at dispatch level.
 *
 * @return
 * STATUS_SUCCESS, STATUS_NOT_FOUND, STATUS_NOT_SUPPORTED for a name in the
 * wrong case or above passive level, STATUS_INVALID_PARAMETER or STATUS_NO_MEMORY.
 */
NTSTATUS
WINAPI
BCryptOpenAlgorithmProvider(
    _Out_ BCRYPT_ALG_HANDLE *Algorithm,
    _In_ LPCWSTR AlgorithmId,
    _In_opt_ LPCWSTR Implementation,
    _In_ ULONG Flags)
{
    const CNG_ALGORITHM *Found;
    PCNG_PROVIDER Provider;
    NTSTATUS Status;
    ULONG Index;

    if (KeGetCurrentIrql() >= DISPATCH_LEVEL)
        return STATUS_NOT_SUPPORTED;
    if (Algorithm == NULL || AlgorithmId == NULL || (Flags & ~CNG_OPEN_FLAGS))
        return STATUS_INVALID_PARAMETER;

    *Algorithm = NULL;

    if (Implementation != NULL && wcscmp(Implementation, MS_PRIMITIVE_PROVIDER) != 0)
        return STATUS_NOT_FOUND;

    Found = CngFindAlgorithm(AlgorithmId);
    if (Found == NULL)
    {
        for (Index = 0; Index < ARRAYSIZE(CngAlgorithms); Index++)
        {
            if (_wcsicmp(AlgorithmId, CngAlgorithms[Index].Name) == 0)
                return STATUS_NOT_SUPPORTED;
        }

        DPRINT1("Algorithm %S is not available\n", AlgorithmId);
        return STATUS_NOT_FOUND;
    }

    if ((Flags & BCRYPT_ALG_HANDLE_HMAC_FLAG) && Found->Class == CngClassRng)
        return STATUS_INVALID_PARAMETER;

    Status = CngCreateProvider(Found, Flags, FALSE, &Provider);
    if (!NT_SUCCESS(Status))
        return Status;

    *Algorithm = Provider;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Closes an algorithm provider.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, or STATUS_NOT_SUPPORTED at dispatch level.
 *
 * @remarks
 * A dispatch provider can only be closed once its objects are gone, the
 * others live on until their last hash or key is destroyed.
 */
NTSTATUS
WINAPI
BCryptCloseAlgorithmProvider(
    _Inout_ BCRYPT_ALG_HANDLE Algorithm,
    _In_ ULONG Flags)
{
    PCNG_PROVIDER Provider;

    UNREFERENCED_PARAMETER(Flags);

    if (KeGetCurrentIrql() >= DISPATCH_LEVEL)
        return STATUS_NOT_SUPPORTED;

    Provider = CngLookupProvider(Algorithm);
    if (Provider == NULL || Provider->Pseudo)
        return STATUS_INVALID_HANDLE;
    if ((Provider->Flags & BCRYPT_PROV_DISPATCH) && Provider->References > 1)
        return STATUS_INVALID_HANDLE;

    CngDereferenceProvider(Provider);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reads a property of a provider, hash object, key or secret.
 *
 * @return
 * STATUS_SUCCESS, STATUS_BUFFER_TOO_SMALL, STATUS_NOT_SUPPORTED,
 * STATUS_INVALID_HANDLE or STATUS_INVALID_PARAMETER.
 */
NTSTATUS
WINAPI
BCryptGetProperty(
    _In_ BCRYPT_HANDLE Object,
    _In_ LPCWSTR Property,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    PCNG_PROVIDER Provider = CngLookupProvider(Object);
    PCNG_HASH Hash = NULL;
    PCNG_KEY Key = NULL;

    if (Provider == NULL && ((ULONG_PTR)Object & 0xF) == 0x1)
        return STATUS_INVALID_HANDLE;

    if (Provider == NULL)
    {
        Hash = CngLookupHash(Object);
        Key = (Hash == NULL) ? CngLookupKey(Object) : NULL;

        if (Hash != NULL)
            Provider = Hash->Provider;
        else if (Key != NULL)
            Provider = Key->Provider;
        else if (CngLookupSecret(Object) != NULL)
            return STATUS_NOT_SUPPORTED;
        else
            return STATUS_INVALID_HANDLE;
    }

    if (Property == NULL || ResultLength == NULL || Flags != 0)
        return STATUS_INVALID_PARAMETER;

    if (wcscmp(Property, BCRYPT_ALGORITHM_NAME) == 0)
    {
        return CngReturnProperty(Provider->Algorithm->Name,
                                 (ULONG)(wcslen(Provider->Algorithm->Name) + 1) * sizeof(WCHAR),
                                 Output,
                                 OutputLength,
                                 ResultLength);
    }

    if (wcscmp(Property, BCRYPT_PROVIDER_HANDLE) == 0)
        return CngReturnProperty(&Provider, sizeof(Provider), Output, OutputLength, ResultLength);

    return CngQueryProviderProperty(Provider, Key, Property, Output, OutputLength, ResultLength);
}

/**
 * @brief
 * Sets a property of a provider or key.
 *
 * @return
 * STATUS_SUCCESS, STATUS_NOT_SUPPORTED, STATUS_INVALID_PARAMETER,
 * STATUS_INVALID_HANDLE, or STATUS_ACCESS_DENIED for a pseudo handle.
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
    PCNG_PROVIDER Provider = CngLookupProvider(Object);
    PCNG_KEY Key = NULL;

    UNREFERENCED_PARAMETER(Flags);

    if (Provider == NULL)
    {
        Key = CngLookupKey(Object);
        if (Key == NULL)
            return (CngLookupHash(Object) != NULL) ? STATUS_NOT_SUPPORTED : STATUS_INVALID_HANDLE;
        Provider = Key->Provider;
    }
    else if (Provider->Pseudo)
    {
        return STATUS_ACCESS_DENIED;
    }

    switch (Provider->Algorithm->Class)
    {
        case CngClassHash:
        case CngClassMac:
        case CngClassRng:
            return STATUS_INVALID_HANDLE;

        default:
            break;
    }

    if (Property == NULL || (Input == NULL && InputLength != 0))
        return STATUS_INVALID_PARAMETER;

    return CngSetKeyProperty(Provider, Key, Property, Input, InputLength);
}

/**
 * @brief
 * Lists the algorithms of the given operation classes.
 *
 * @param[in] Operations
 * A mask of BCRYPT_*_OPERATION values, zero for all of them.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_PARAMETER or STATUS_NO_MEMORY. The list is
 * released with BCryptFreeBuffer().
 */
NTSTATUS
WINAPI
BCryptEnumAlgorithms(
    _In_ ULONG Operations,
    _Out_ ULONG *AlgorithmCount,
    _Out_ BCRYPT_ALGORITHM_IDENTIFIER **AlgorithmList,
    _In_ ULONG Flags)
{
    BCRYPT_ALGORITHM_IDENTIFIER *List;
    ULONG Index, Count = 0, Size;
    PWCHAR Strings;

    UNREFERENCED_PARAMETER(Flags);

    if (AlgorithmCount == NULL || AlgorithmList == NULL || (Operations & ~CNG_ENUM_OPERATIONS))
        return STATUS_INVALID_PARAMETER;
    if (Operations == 0)
        Operations = CNG_ENUM_OPERATIONS;

    Size = 0;
    for (Index = 0; Index < ARRAYSIZE(CngAlgorithms); Index++)
    {
        if (CngAlgorithms[Index].Operations & Operations)
        {
            Count++;
            Size += (ULONG)(wcslen(CngAlgorithms[Index].Name) + 1) * sizeof(WCHAR);
        }
    }

    List = ExAllocatePoolWithTag(PagedPool, Count * sizeof(*List) + Size, TAG_CNG_BUFFER);
    if (List == NULL)
        return STATUS_NO_MEMORY;

    Strings = (PWCHAR)&List[Count];
    Count = 0;
    for (Index = 0; Index < ARRAYSIZE(CngAlgorithms); Index++)
    {
        if (!(CngAlgorithms[Index].Operations & Operations))
            continue;

        Size = (ULONG)(wcslen(CngAlgorithms[Index].Name) + 1) * sizeof(WCHAR);
        RtlCopyMemory(Strings, CngAlgorithms[Index].Name, Size);
        List[Count].pszName = Strings;
        List[Count].dwClass = CngAlgorithms[Index].Interface;
        List[Count].dwFlags = 0;
        Strings += Size / sizeof(WCHAR);
        Count++;
    }

    *AlgorithmCount = Count;
    *AlgorithmList = List;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Lists the providers implementing an algorithm, which is only ever the primitive provider.
 */
NTSTATUS
WINAPI
BCryptEnumProviders(
    _In_ LPCWSTR AlgorithmId,
    _Out_ ULONG *ImplementationCount,
    _Out_ BCRYPT_PROVIDER_NAME **ImplementationList,
    _In_ ULONG Flags)
{
    BCRYPT_PROVIDER_NAME *List;

    UNREFERENCED_PARAMETER(Flags);

    if (AlgorithmId == NULL || ImplementationCount == NULL || ImplementationList == NULL)
        return STATUS_INVALID_PARAMETER;
    if (CngFindAlgorithm(AlgorithmId) == NULL)
        return STATUS_NOT_FOUND;

    List = ExAllocatePoolWithTag(PagedPool, sizeof(*List) + sizeof(MS_PRIMITIVE_PROVIDER), TAG_CNG_BUFFER);
    if (List == NULL)
        return STATUS_NO_MEMORY;

    List->pszProviderName = (LPWSTR)(List + 1);
    RtlCopyMemory(List->pszProviderName, MS_PRIMITIVE_PROVIDER, sizeof(MS_PRIMITIVE_PROVIDER));

    *ImplementationCount = 1;
    *ImplementationList = List;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Releases a buffer handed out by the enumeration routines.
 */
VOID
WINAPI
BCryptFreeBuffer(
    _In_opt_ PVOID Buffer)
{
    if (Buffer != NULL)
        ExFreePoolWithTag(Buffer, TAG_CNG_BUFFER);
}

/**
 * @brief
 * Reports the primitive provider for every interface and function asked
 * about. There is no registered provider configuration in ReactOS.
 */
NTSTATUS
WINAPI
BCryptResolveProviders(
    _In_opt_ LPCWSTR Context,
    _In_opt_ ULONG Interface,
    _In_opt_ LPCWSTR Function,
    _In_opt_ LPCWSTR Provider,
    _In_ ULONG Mode,
    _In_ ULONG Flags,
    _Inout_ ULONG *BufferLength,
    _Inout_ PCRYPT_PROVIDER_REFS *Buffer)
{
    const CNG_ALGORITHM *Algorithm = NULL;
    PCRYPT_PROVIDER_REFS Refs;
    PCRYPT_PROVIDER_REF Ref;
    ULONG Needed, NameSize;
    PUCHAR Cursor;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Mode);
    UNREFERENCED_PARAMETER(Flags);

    if (BufferLength == NULL || Buffer == NULL)
        return STATUS_INVALID_PARAMETER;
    if (Provider != NULL && wcscmp(Provider, MS_PRIMITIVE_PROVIDER) != 0)
        return STATUS_NOT_FOUND;

    if (Function != NULL)
    {
        Algorithm = CngFindAlgorithm(Function);
        if (Algorithm == NULL || (Interface != 0 && Algorithm->Interface != Interface))
            return STATUS_NOT_FOUND;
    }

    NameSize = (Algorithm != NULL) ? (ULONG)(wcslen(Algorithm->Name) + 1) * sizeof(WCHAR) : 0;
    Needed = sizeof(*Refs) + sizeof(PCRYPT_PROVIDER_REF) + sizeof(*Ref) + NameSize + sizeof(MS_PRIMITIVE_PROVIDER);

    if (*Buffer == NULL)
    {
        *Buffer = ExAllocatePoolWithTag(PagedPool, Needed, TAG_CNG_BUFFER);
        if (*Buffer == NULL)
            return STATUS_NO_MEMORY;
    }
    else if (*BufferLength < Needed)
    {
        *BufferLength = Needed;
        return STATUS_BUFFER_TOO_SMALL;
    }

    Refs = *Buffer;
    RtlZeroMemory(Refs, Needed);
    Refs->cProviders = 1;
    Refs->rgpProviders = (PCRYPT_PROVIDER_REF *)(Refs + 1);
    Ref = (PCRYPT_PROVIDER_REF)(Refs->rgpProviders + 1);
    Refs->rgpProviders[0] = Ref;

    Cursor = (PUCHAR)(Ref + 1);
    Ref->dwInterface = (Algorithm != NULL) ? Algorithm->Interface : Interface;
    if (Algorithm != NULL)
    {
        Ref->pszFunction = (PWSTR)Cursor;
        RtlCopyMemory(Cursor, Algorithm->Name, NameSize);
        Cursor += NameSize;
    }
    Ref->pszProvider = (PWSTR)Cursor;
    RtlCopyMemory(Cursor, MS_PRIMITIVE_PROVIDER, sizeof(MS_PRIMITIVE_PROVIDER));

    *BufferLength = Needed;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Provider registration is not supported, the primitive provider is built in.
 */
NTSTATUS
WINAPI
BCryptRegisterProvider(
    _In_ LPCWSTR Provider,
    _In_ ULONG Flags,
    _In_ PCRYPT_PROVIDER_REG Registration)
{
    if (Provider == NULL || *Provider == UNICODE_NULL || Registration == NULL || (Flags & ~CRYPT_OVERWRITE))
        return STATUS_INVALID_PARAMETER;

    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
WINAPI
BCryptUnregisterProvider(
    _In_ LPCWSTR Provider)
{
    if (Provider == NULL || *Provider == UNICODE_NULL)
        return STATUS_INVALID_PARAMETER;

    return STATUS_NOT_FOUND;
}

NTSTATUS
WINAPI
BCryptAddContextFunctionProvider(
    _In_ ULONG Table,
    _In_ LPCWSTR Context,
    _In_ ULONG Interface,
    _In_ LPCWSTR Function,
    _In_ LPCWSTR Provider,
    _In_ ULONG Position)
{
    UNREFERENCED_PARAMETER(Interface);
    UNREFERENCED_PARAMETER(Position);

    if ((Table != CRYPT_LOCAL && Table != CRYPT_DOMAIN) || Context == NULL || Function == NULL || Provider == NULL)
        return STATUS_INVALID_PARAMETER;

    return STATUS_NOT_SUPPORTED;
}

/**
 * @brief
 * Registers an event for configuration changes. The configuration never
 * changes, so the event is accepted and never signaled.
 */
NTSTATUS
WINAPI
BCryptRegisterConfigChangeNotify(
    _In_ PRKEVENT Event)
{
    if (Event == NULL)
        return STATUS_INVALID_PARAMETER;

    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
BCryptUnregisterConfigChangeNotify(
    _In_ PRKEVENT Event)
{
    if (Event == NULL)
        return STATUS_INVALID_PARAMETER;

    return STATUS_SUCCESS;
}

/* EOF */
