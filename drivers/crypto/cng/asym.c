/*
 * PROJECT:     ReactOS Kernel Cryptography Next Generation
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     RSA, ECDSA and ECDH keys, signatures and secret agreement
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "cngp.h"

#include <debug.h>

/* DEFINES ********************************************************************/

#define CNG_RSA_MIN_BITS        512
#define CNG_RSA_MAX_BITS        16384
#define CNG_RSA_STEP_BITS       8
#define CNG_RSA_MAX_EXPONENT    sizeof(UINT64)
#define CNG_ECC_MIN_BITS        112
#define CNG_ECC_MAX_BITS        528

#define CNG_RSA_HEADER          sizeof(BCRYPT_RSAKEY_BLOB)
#define CNG_ECC_HEADER          sizeof(BCRYPT_ECCKEY_BLOB)

#define CNG_IMPORT_FLAGS        BCRYPT_NO_KEY_VALIDATION

/* TYPES **********************************************************************/

typedef enum _CNG_BLOB
{
    CngBlobPublic,
    CngBlobPrivate,
    CngBlobFullPrivate
} CNG_BLOB;

typedef struct _CNG_SIGNATURE_HASH
{
    PCWSTR Name;
    const PCSYMCRYPT_HASH *Hash;
    PCSYMCRYPT_OID Oids;
    SIZE_T OidCount;
} CNG_SIGNATURE_HASH, *PCNG_SIGNATURE_HASH;

/* GLOBALS ********************************************************************/

static const CNG_SIGNATURE_HASH CngSignatureHashes[] =
{
    { BCRYPT_MD5_ALGORITHM, &SymCryptMd5Algorithm, SymCryptMd5OidList, SYMCRYPT_MD5_OID_COUNT },
    { BCRYPT_SHA1_ALGORITHM, &SymCryptSha1Algorithm, SymCryptSha1OidList, SYMCRYPT_SHA1_OID_COUNT },
    { BCRYPT_SHA256_ALGORITHM, &SymCryptSha256Algorithm, SymCryptSha256OidList, SYMCRYPT_SHA256_OID_COUNT },
    { BCRYPT_SHA384_ALGORITHM, &SymCryptSha384Algorithm, SymCryptSha384OidList, SYMCRYPT_SHA384_OID_COUNT },
    { BCRYPT_SHA512_ALGORITHM, &SymCryptSha512Algorithm, SymCryptSha512OidList, SYMCRYPT_SHA512_OID_COUNT },
    { BCRYPT_SHA3_256_ALGORITHM, &SymCryptSha3_256Algorithm, SymCryptSha3_256OidList, SYMCRYPT_SHA3_256_OID_COUNT },
    { BCRYPT_SHA3_384_ALGORITHM, &SymCryptSha3_384Algorithm, SymCryptSha3_384OidList, SYMCRYPT_SHA3_384_OID_COUNT },
    { BCRYPT_SHA3_512_ALGORITHM, &SymCryptSha3_512Algorithm, SymCryptSha3_512OidList, SYMCRYPT_SHA3_512_OID_COUNT },
};

static const BCRYPT_KEY_LENGTHS_STRUCT CngRsaKeyLengths = { CNG_RSA_MIN_BITS, CNG_RSA_MAX_BITS, CNG_RSA_STEP_BITS };
static const BCRYPT_KEY_LENGTHS_STRUCT CngEccGenericLengths = { CNG_ECC_MIN_BITS, CNG_ECC_MAX_BITS, 1 };

/* HELPERS ********************************************************************/

static
const CNG_SIGNATURE_HASH *
CngFindSignatureHash(
    _In_ LPCWSTR Name)
{
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(CngSignatureHashes); Index++)
    {
        if (wcscmp(Name, CngSignatureHashes[Index].Name) == 0)
            return &CngSignatureHashes[Index];
    }

    return NULL;
}

static
BOOLEAN
CngIsEcc(
    _In_ PCNG_PROVIDER Provider)
{
    return Provider->Algorithm->Class == CngClassEcdsa || Provider->Algorithm->Class == CngClassEcdh;
}

static
ULONG
CngFieldLength(
    _In_ const CNG_CURVE *Curve)
{
    return (Curve->Bits + 7) / 8;
}

static
ULONG
CngRsaBits(
    _In_ PCNG_KEY Key)
{
    if (Key->Rsa != NULL)
        return SymCryptRsakeyModulusBits(Key->Rsa);

    return Key->KeyBits;
}

static
ULONG
CngRsaModulusLength(
    _In_ PCNG_KEY Key)
{
    return (ULONG)SymCryptRsakeySizeofModulus(Key->Rsa);
}

static
ULONG
CngRsaKeyFlags(
    _In_ PCNG_KEY Key,
    _In_ ULONG ImportFlags)
{
    ULONG Flags = SYMCRYPT_FLAG_KEY_NO_FIPS | SYMCRYPT_FLAG_RSAKEY_SIGN;

    if (!Key->Provider->Algorithm->SignOnly)
        Flags |= SYMCRYPT_FLAG_RSAKEY_ENCRYPT;
    if (ImportFlags & BCRYPT_NO_KEY_VALIDATION)
        Flags |= SYMCRYPT_FLAG_KEY_MINIMAL_VALIDATION;

    return Flags;
}

static
ULONG
CngEcKeyFlags(
    _In_ PCNG_KEY Key,
    _In_ ULONG ImportFlags)
{
    ULONG Flags = SYMCRYPT_FLAG_KEY_NO_FIPS;

    Flags |= (Key->Provider->Algorithm->Class == CngClassEcdh) ? SYMCRYPT_FLAG_ECKEY_ECDH : SYMCRYPT_FLAG_ECKEY_ECDSA;
    if (ImportFlags & BCRYPT_NO_KEY_VALIDATION)
        Flags |= SYMCRYPT_FLAG_KEY_MINIMAL_VALIDATION;

    return Flags;
}

/* The generic blob names stand for the public and private blob of the key type */
static
NTSTATUS
CngParseBlobType(
    _In_ PCNG_PROVIDER Provider,
    _In_ LPCWSTR BlobType,
    _Out_ CNG_BLOB *Blob)
{
    if (wcscmp(BlobType, BCRYPT_PUBLIC_KEY_BLOB) == 0)
        *Blob = CngBlobPublic;
    else if (wcscmp(BlobType, BCRYPT_PRIVATE_KEY_BLOB) == 0)
        *Blob = CngBlobPrivate;
    else if (CngIsEcc(Provider) && wcscmp(BlobType, BCRYPT_ECCPUBLIC_BLOB) == 0)
        *Blob = CngBlobPublic;
    else if (CngIsEcc(Provider) && wcscmp(BlobType, BCRYPT_ECCPRIVATE_BLOB) == 0)
        *Blob = CngBlobPrivate;
    else if (!CngIsEcc(Provider) && wcscmp(BlobType, BCRYPT_RSAPUBLIC_BLOB) == 0)
        *Blob = CngBlobPublic;
    else if (!CngIsEcc(Provider) && wcscmp(BlobType, BCRYPT_RSAPRIVATE_BLOB) == 0)
        *Blob = CngBlobPrivate;
    else if (!CngIsEcc(Provider) && wcscmp(BlobType, BCRYPT_RSAFULLPRIVATE_BLOB) == 0)
        *Blob = CngBlobFullPrivate;
    else
        return STATUS_NOT_SUPPORTED;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Releases the SymCrypt parts of a key.
 */
VOID
NTAPI
CngFreeAsymmetricKey(
    _In_ PCNG_KEY Key)
{
    if (Key->Rsa != NULL)
        SymCryptRsakeyFree(Key->Rsa);
    if (Key->Ec != NULL)
        SymCryptEckeyFree(Key->Ec);
    if (Key->EcCurve != NULL)
        SymCryptEcurveFree(Key->EcCurve);

    Key->Rsa = NULL;
    Key->Ec = NULL;
    Key->EcCurve = NULL;
}

/* Gives an ECC key its curve and an empty SymCrypt key on it */
static
NTSTATUS
CngPrepareEcKey(
    _Inout_ PCNG_KEY Key,
    _In_ const CNG_CURVE *Curve)
{
    Key->Curve = Curve;
    Key->EcCurve = SymCryptEcurveAllocate(*Curve->Params, 0);
    if (Key->EcCurve == NULL)
        return STATUS_NO_MEMORY;

    Key->Ec = SymCryptEckeyAllocate(Key->EcCurve);
    if (Key->Ec == NULL)
        return STATUS_NO_MEMORY;

    Key->KeyBits = Curve->Bits;
    return STATUS_SUCCESS;
}

/* IMPORT AND EXPORT **********************************************************/

static
NTSTATUS
CngImportRsa(
    _Inout_ PCNG_KEY Key,
    _In_ CNG_BLOB Blob,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_ ULONG Flags)
{
    BCRYPT_RSAKEY_BLOB *Header = (BCRYPT_RSAKEY_BLOB *)Input;
    static const ULONG Magics[] = { BCRYPT_RSAPUBLIC_MAGIC, BCRYPT_RSAPRIVATE_MAGIC, BCRYPT_RSAFULLPRIVATE_MAGIC };
    SYMCRYPT_RSA_PARAMS Params;
    PCBYTE Primes[2];
    SIZE_T PrimeLengths[2];
    ULONGLONG Needed;
    UINT64 Exponent = 0;
    PUCHAR Cursor;
    ULONG Index;

    if (InputLength < CNG_RSA_HEADER || Header->Magic != Magics[Blob])
        return NTE_BAD_DATA;
    if (Header->cbPublicExp == 0 || Header->cbPublicExp > CNG_RSA_MAX_EXPONENT || Header->cbModulus == 0)
        return NTE_BAD_DATA;

    Needed = (ULONGLONG)CNG_RSA_HEADER + Header->cbPublicExp + Header->cbModulus;
    if (Blob != CngBlobPublic)
    {
        if (Header->cbPrime1 == 0 || Header->cbPrime2 == 0)
            return NTE_BAD_DATA;
        Needed += (ULONGLONG)Header->cbPrime1 + Header->cbPrime2;
    }
    if (Blob == CngBlobFullPrivate)
        Needed += 2ULL * Header->cbPrime1 + Header->cbPrime2 + Header->cbModulus;

    /* The blob has to be exactly as long as its header says */
    if (Needed != InputLength)
        return NTE_BAD_DATA;

    Cursor = Input + CNG_RSA_HEADER;
    for (Index = 0; Index < Header->cbPublicExp; Index++)
        Exponent = (Exponent << 8) | Cursor[Index];
    Cursor += Header->cbPublicExp;

    Params.version = 1;
    Params.nBitsOfModulus = Header->BitLength;
    Params.nPrimes = (Blob == CngBlobPublic) ? 0 : 2;
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
                               (Blob == CngBlobPublic) ? NULL : Primes,
                               (Blob == CngBlobPublic) ? NULL : PrimeLengths,
                               Params.nPrimes,
                               SYMCRYPT_NUMBER_FORMAT_MSB_FIRST,
                               CngRsaKeyFlags(Key, Flags),
                               Key->Rsa) != SYMCRYPT_NO_ERROR)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Key->KeyBits = SymCryptRsakeyModulusBits(Key->Rsa);
    return STATUS_SUCCESS;
}

static
NTSTATUS
CngImportEcc(
    _Inout_ PCNG_KEY Key,
    _In_ CNG_BLOB Blob,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_ ULONG Flags)
{
    BCRYPT_ECCKEY_BLOB *Header = (BCRYPT_ECCKEY_BLOB *)Input;
    PCNG_PROVIDER Provider = Key->Provider;
    BOOLEAN Ecdh = Provider->Algorithm->Class == CngClassEcdh;
    BOOLEAN Private = (Blob != CngBlobPublic);
    const CNG_CURVE *Curve;
    BOOLEAN BlobPrivate;
    PUCHAR Point;
    ULONG Field;
    NTSTATUS Status;

    if (InputLength < CNG_ECC_HEADER)
        return STATUS_INVALID_PARAMETER;

    /* A blob of a named curve brings its curve, a generic one takes the provider's */
    Curve = CngFindCurveByMagic(Header->dwMagic, Ecdh, &BlobPrivate);
    if (Curve == NULL)
    {
        if (Header->dwMagic == (ULONG)(Ecdh ? BCRYPT_ECDH_PUBLIC_GENERIC_MAGIC : BCRYPT_ECDSA_PUBLIC_GENERIC_MAGIC))
            BlobPrivate = FALSE;
        else if (Header->dwMagic == (ULONG)(Ecdh ? BCRYPT_ECDH_PRIVATE_GENERIC_MAGIC : BCRYPT_ECDSA_PRIVATE_GENERIC_MAGIC))
            BlobPrivate = TRUE;
        else
            return STATUS_INVALID_PARAMETER;

        Curve = Provider->Curve;
        if (Curve == NULL)
            return STATUS_INVALID_PARAMETER;
    }
    else if (Provider->Curve != NULL && Provider->Curve != Curve)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Field = CngFieldLength(Curve);
    if (BlobPrivate != Private || Header->cbKey != Field)
        return STATUS_INVALID_PARAMETER;
    if (InputLength < CNG_ECC_HEADER + (Private ? 3 : 2) * Field)
        return STATUS_INVALID_PARAMETER;

    Status = CngPrepareEcKey(Key, Curve);
    if (!NT_SUCCESS(Status))
        return Status;

    Point = Input + CNG_ECC_HEADER;
    if (SymCryptEckeySetValue(Private ? Point + 2 * Field : NULL,
                              Private ? Field : 0,
                              Point,
                              2 * Field,
                              SYMCRYPT_NUMBER_FORMAT_MSB_FIRST,
                              SYMCRYPT_ECPOINT_FORMAT_XY,
                              CngEcKeyFlags(Key, Flags),
                              Key->Ec) != SYMCRYPT_NO_ERROR)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
CngExportRsa(
    _In_ PCNG_KEY Key,
    _In_ CNG_BLOB Blob,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    static const ULONG Magics[] = { BCRYPT_RSAPUBLIC_MAGIC, BCRYPT_RSAPRIVATE_MAGIC, BCRYPT_RSAFULLPRIVATE_MAGIC };
    BCRYPT_RSAKEY_BLOB Header;
    PBYTE Primes[2], Exponents[2];
    SIZE_T PrimeLengths[2];
    UINT64 Exponent;
    ULONG Needed, Index;
    PUCHAR Cursor;

    if (Blob != CngBlobPublic && !SymCryptRsakeyHasPrivateKey(Key->Rsa))
        return STATUS_INVALID_PARAMETER;

    if (SymCryptRsakeyGetValue(Key->Rsa, NULL, 0, &Exponent, 1, NULL, NULL, 0,
                               SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, 0) != SYMCRYPT_NO_ERROR)
    {
        return STATUS_INTERNAL_ERROR;
    }

    RtlZeroMemory(&Header, sizeof(Header));
    Header.Magic = Magics[Blob];
    Header.BitLength = SymCryptRsakeyModulusBits(Key->Rsa);
    Header.cbModulus = CngRsaModulusLength(Key);
    for (Header.cbPublicExp = sizeof(Exponent); Header.cbPublicExp > 1; Header.cbPublicExp--)
    {
        if (Exponent >> ((Header.cbPublicExp - 1) * 8))
            break;
    }
    if (Blob != CngBlobPublic)
    {
        Header.cbPrime1 = (ULONG)SymCryptRsakeySizeofPrime(Key->Rsa, 0);
        Header.cbPrime2 = (ULONG)SymCryptRsakeySizeofPrime(Key->Rsa, 1);
    }

    Needed = CNG_RSA_HEADER + Header.cbPublicExp + Header.cbModulus + Header.cbPrime1 + Header.cbPrime2;
    if (Blob == CngBlobFullPrivate)
        Needed += 2 * Header.cbPrime1 + Header.cbPrime2 + Header.cbModulus;

    *ResultLength = Needed;
    if (Output == NULL)
        return STATUS_SUCCESS;
    if (OutputLength < Needed)
        return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(Output, Needed);
    RtlCopyMemory(Output, &Header, sizeof(Header));
    Cursor = Output + CNG_RSA_HEADER;
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
                               (Blob == CngBlobPublic) ? NULL : Primes,
                               (Blob == CngBlobPublic) ? NULL : PrimeLengths,
                               (Blob == CngBlobPublic) ? 0 : 2,
                               SYMCRYPT_NUMBER_FORMAT_MSB_FIRST,
                               0) != SYMCRYPT_NO_ERROR)
    {
        return STATUS_INTERNAL_ERROR;
    }

    if (Blob != CngBlobFullPrivate)
        return STATUS_SUCCESS;

    /* Exponent1, Exponent2, Coefficient and PrivateExponent follow the primes */
    Exponents[0] = Primes[1] + Header.cbPrime2;
    Exponents[1] = Exponents[0] + Header.cbPrime1;
    if (SymCryptRsakeyGetCrtValue(Key->Rsa,
                                  Exponents,
                                  PrimeLengths,
                                  2,
                                  Exponents[1] + Header.cbPrime2,
                                  Header.cbPrime1,
                                  Exponents[1] + Header.cbPrime2 + Header.cbPrime1,
                                  Header.cbModulus,
                                  SYMCRYPT_NUMBER_FORMAT_MSB_FIRST,
                                  0) != SYMCRYPT_NO_ERROR)
    {
        return STATUS_INTERNAL_ERROR;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
CngExportEcc(
    _In_ PCNG_KEY Key,
    _In_ CNG_BLOB Blob,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    BOOLEAN Ecdh = Key->Provider->Algorithm->Class == CngClassEcdh;
    BOOLEAN Private = (Blob != CngBlobPublic);
    ULONG Field = CngFieldLength(Key->Curve);
    BCRYPT_ECCKEY_BLOB Header;
    ULONG Needed;
    PUCHAR Point;

    if (Private && !SymCryptEckeyHasPrivateKey(Key->Ec))
        return STATUS_INVALID_PARAMETER;

    Needed = CNG_ECC_HEADER + (Private ? 3 : 2) * Field;
    *ResultLength = Needed;
    if (Output == NULL)
        return STATUS_SUCCESS;
    if (OutputLength < Needed)
        return STATUS_BUFFER_TOO_SMALL;

    if (Ecdh)
        Header.dwMagic = Private ? Key->Curve->EcdhPrivateMagic : Key->Curve->EcdhPublicMagic;
    else
        Header.dwMagic = Private ? Key->Curve->EcdsaPrivateMagic : Key->Curve->EcdsaPublicMagic;
    Header.cbKey = Field;
    RtlCopyMemory(Output, &Header, sizeof(Header));
    Point = Output + CNG_ECC_HEADER;

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
 * Exports an RSA or ECC key as a key blob.
 */
NTSTATUS
NTAPI
CngExportAsymmetricKey(
    _In_ PCNG_KEY Key,
    _In_ LPCWSTR BlobType,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    CNG_BLOB Blob;
    NTSTATUS Status;

    Status = CngParseBlobType(Key->Provider, BlobType, &Blob);
    if (!NT_SUCCESS(Status))
        return Status;

    if (CngIsEcc(Key->Provider))
        return CngExportEcc(Key, Blob, Output, OutputLength, ResultLength);

    return CngExportRsa(Key, Blob, Output, OutputLength, ResultLength);
}

/**
 * @brief
 * Fills a new key with a copy of an RSA or ECC key, through its blob.
 */
NTSTATUS
NTAPI
CngDuplicateAsymmetricKey(
    _In_ PCNG_KEY Source,
    _Inout_ PCNG_KEY Copy)
{
    CNG_BLOB Blob = CngBlobPublic;
    ULONG Length;
    PUCHAR Buffer;
    NTSTATUS Status;

    if (CngIsEcc(Source->Provider))
    {
        if (SymCryptEckeyHasPrivateKey(Source->Ec))
            Blob = CngBlobPrivate;

        Status = CngExportEcc(Source, Blob, NULL, 0, &Length);
    }
    else
    {
        if (SymCryptRsakeyHasPrivateKey(Source->Rsa))
            Blob = CngBlobPrivate;

        Status = CngExportRsa(Source, Blob, NULL, 0, &Length);
    }

    if (!NT_SUCCESS(Status))
        return Status;

    Buffer = CngAllocate(Length);
    if (Buffer == NULL)
        return STATUS_NO_MEMORY;

    if (CngIsEcc(Source->Provider))
    {
        Status = CngExportEcc(Source, Blob, Buffer, Length, &Length);
        if (NT_SUCCESS(Status))
            Status = CngImportEcc(Copy, Blob, Buffer, Length, BCRYPT_NO_KEY_VALIDATION);
    }
    else
    {
        Status = CngExportRsa(Source, Blob, Buffer, Length, &Length);
        if (NT_SUCCESS(Status))
            Status = CngImportRsa(Copy, Blob, Buffer, Length, BCRYPT_NO_KEY_VALIDATION);
    }

    SymCryptWipe(Buffer, Length);
    CngFree(Buffer);

    if (NT_SUCCESS(Status))
        Copy->Flags |= CNG_KEY_FINAL;

    return Status;
}

/* PROPERTIES *****************************************************************/

/**
 * @brief
 * Answers the properties of RSA and ECC providers and their keys.
 */
NTSTATUS
NTAPI
CngQueryAsymmetricProperty(
    _In_ PCNG_PROVIDER Provider,
    _In_opt_ PCNG_KEY Key,
    _In_ LPCWSTR Property,
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    const CNG_ALGORITHM *Algorithm = Provider->Algorithm;
    BCRYPT_KEY_LENGTHS_STRUCT Lengths;
    const CNG_CURVE *Curve;
    ULONG Value;

    Curve = (Key != NULL && Key->Curve != NULL) ? Key->Curve : Provider->Curve;

    if (wcscmp(Property, BCRYPT_KEY_LENGTHS) == 0 && Key == NULL)
    {
        if (!CngIsEcc(Provider))
            return CngReturnProperty(&CngRsaKeyLengths, sizeof(CngRsaKeyLengths), Output, OutputLength, ResultLength);
        if (Algorithm->Curve == NULL)
            return CngReturnProperty(&CngEccGenericLengths, sizeof(CngEccGenericLengths), Output, OutputLength, ResultLength);

        Lengths.dwMinLength = Algorithm->Curve->Bits;
        Lengths.dwMaxLength = Algorithm->Curve->Bits;
        Lengths.dwIncrement = 0;
        return CngReturnProperty(&Lengths, sizeof(Lengths), Output, OutputLength, ResultLength);
    }

    if (wcscmp(Property, BCRYPT_ECC_CURVE_NAME) == 0 && CngIsEcc(Provider))
    {
        if (Curve == NULL)
            return STATUS_INVALID_PARAMETER;

        return CngReturnProperty(Curve->Name, (ULONG)(wcslen(Curve->Name) + 1) * sizeof(WCHAR),
                                 Output, OutputLength, ResultLength);
    }

    if (wcscmp(Property, BCRYPT_PADDING_SCHEMES) == 0 && Key == NULL && !CngIsEcc(Provider))
    {
        Value = BCRYPT_SUPPORTED_PAD_PKCS1_SIG | BCRYPT_SUPPORTED_PAD_PSS;
        if (!Algorithm->SignOnly)
            Value |= BCRYPT_SUPPORTED_PAD_PKCS1_ENC | BCRYPT_SUPPORTED_PAD_OAEP;

        return CngReturnProperty(&Value, sizeof(Value), Output, OutputLength, ResultLength);
    }

    if (Key == NULL)
        return STATUS_NOT_SUPPORTED;

    if (wcscmp(Property, BCRYPT_KEY_LENGTH) == 0 || wcscmp(Property, BCRYPT_KEY_STRENGTH) == 0)
    {
        Value = CngIsEcc(Provider) ? Key->KeyBits : CngRsaBits(Key);
    }
    else if (wcscmp(Property, BCRYPT_SIGNATURE_LENGTH) == 0 && (Key->Flags & CNG_KEY_FINAL))
    {
        if (Algorithm->Class == CngClassEcdh)
            return STATUS_NOT_SUPPORTED;

        Value = CngIsEcc(Provider) ? 2 * CngFieldLength(Key->Curve) : CngRsaModulusLength(Key);
    }
    else if (wcscmp(Property, BCRYPT_BLOCK_LENGTH) == 0 && !CngIsEcc(Provider) && (Key->Flags & CNG_KEY_FINAL))
    {
        Value = CngRsaModulusLength(Key);
    }
    else
    {
        return STATUS_NOT_SUPPORTED;
    }

    return CngReturnProperty(&Value, sizeof(Value), Output, OutputLength, ResultLength);
}

/* RSA ENCRYPTION *************************************************************/

/**
 * @brief
 * RSA encryption with BCRYPT_PAD_PKCS1, BCRYPT_PAD_OAEP or BCRYPT_PAD_NONE.
 */
NTSTATUS
NTAPI
CngAsymmetricEncrypt(
    _In_ PCNG_KEY Key,
    _In_reads_bytes_opt_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_opt_ VOID *PaddingInfo,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    BCRYPT_OAEP_PADDING_INFO *Oaep = PaddingInfo;
    ULONG Modulus = CngRsaModulusLength(Key);
    const CNG_SIGNATURE_HASH *Hash;
    SYMCRYPT_ERROR Error;
    SIZE_T Written = 0;

    Flags &= ~BCRYPT_BUFFERS_LOCKED_FLAG;
    if (Flags != BCRYPT_PAD_PKCS1 && Flags != BCRYPT_PAD_OAEP && Flags != BCRYPT_PAD_NONE)
        return STATUS_INVALID_PARAMETER;

    *ResultLength = Modulus;

    if (Flags == BCRYPT_PAD_NONE)
    {
        if (InputLength != Modulus)
            return STATUS_INVALID_PARAMETER;
        if (Output == NULL)
            return STATUS_SUCCESS;
        if (OutputLength < Modulus)
            return STATUS_BUFFER_TOO_SMALL;

        Error = SymCryptRsaRawEncrypt(Key->Rsa, Input, InputLength, SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, 0, Output, Modulus);
        return (Error == SYMCRYPT_NO_ERROR) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    }

    if (Flags == BCRYPT_PAD_PKCS1)
    {
        if (InputLength + 11 > Modulus)
            return STATUS_INVALID_PARAMETER;
        if (Output == NULL)
            return STATUS_SUCCESS;
        if (OutputLength < Modulus)
            return STATUS_BUFFER_TOO_SMALL;

        Error = SymCryptRsaPkcs1Encrypt(Key->Rsa, Input, InputLength, 0, SYMCRYPT_NUMBER_FORMAT_MSB_FIRST,
                                        Output, Modulus, &Written);
        return (Error == SYMCRYPT_NO_ERROR) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    }

    if (Oaep == NULL || Oaep->pszAlgId == NULL || (Oaep->pbLabel == NULL && Oaep->cbLabel != 0))
        return STATUS_INVALID_PARAMETER;

    Hash = CngFindSignatureHash(Oaep->pszAlgId);
    if (Hash == NULL)
        return STATUS_NOT_SUPPORTED;
    if (InputLength + 2 * SymCryptHashResultSize(*Hash->Hash) + 2 > Modulus)
        return STATUS_INVALID_PARAMETER;
    if (Output == NULL)
        return STATUS_SUCCESS;
    if (OutputLength < Modulus)
        return STATUS_BUFFER_TOO_SMALL;

    Error = SymCryptRsaOaepEncrypt(Key->Rsa, Input, InputLength, *Hash->Hash, Oaep->pbLabel, Oaep->cbLabel, 0,
                                   SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, Output, Modulus, &Written);
    return (Error == SYMCRYPT_NO_ERROR) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

/**
 * @brief
 * RSA decryption. A size query with PKCS1 or OAEP padding decrypts to learn the length.
 */
NTSTATUS
NTAPI
CngAsymmetricDecrypt(
    _In_ PCNG_KEY Key,
    _In_reads_bytes_opt_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_opt_ VOID *PaddingInfo,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    BCRYPT_OAEP_PADDING_INFO *Oaep = PaddingInfo;
    ULONG Modulus = CngRsaModulusLength(Key);
    const CNG_SIGNATURE_HASH *Hash = NULL;
    SYMCRYPT_ERROR Error;
    SIZE_T Written = 0;
    PUCHAR Plain;

    Flags &= ~BCRYPT_BUFFERS_LOCKED_FLAG;
    if (Flags != BCRYPT_PAD_PKCS1 && Flags != BCRYPT_PAD_OAEP && Flags != BCRYPT_PAD_NONE)
        return STATUS_INVALID_PARAMETER;
    if (!SymCryptRsakeyHasPrivateKey(Key->Rsa) || InputLength != Modulus)
        return STATUS_INVALID_PARAMETER;

    if (Flags == BCRYPT_PAD_NONE)
    {
        *ResultLength = Modulus;
        if (Output == NULL)
            return STATUS_SUCCESS;
        if (OutputLength < Modulus)
            return STATUS_BUFFER_TOO_SMALL;

        Error = SymCryptRsaRawDecrypt(Key->Rsa, Input, InputLength, SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, 0, Output, Modulus);
        return (Error == SYMCRYPT_NO_ERROR) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    }

    if (Flags == BCRYPT_PAD_OAEP)
    {
        if (Oaep == NULL || Oaep->pszAlgId == NULL || (Oaep->pbLabel == NULL && Oaep->cbLabel != 0))
            return STATUS_INVALID_PARAMETER;

        Hash = CngFindSignatureHash(Oaep->pszAlgId);
        if (Hash == NULL)
            return STATUS_NOT_SUPPORTED;
    }

    /* The message goes to a side buffer first, so a short output still learns the length */
    Plain = CngAllocate(Modulus);
    if (Plain == NULL)
        return STATUS_NO_MEMORY;

    if (Hash == NULL)
    {
        Error = SymCryptRsaPkcs1Decrypt(Key->Rsa, Input, InputLength, SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, 0,
                                        Plain, Modulus, &Written);
    }
    else
    {
        Error = SymCryptRsaOaepDecrypt(Key->Rsa, Input, InputLength, SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, *Hash->Hash,
                                       Oaep->pbLabel, Oaep->cbLabel, 0, Plain, Modulus, &Written);
    }

    if (Error != SYMCRYPT_NO_ERROR)
    {
        SymCryptWipe(Plain, Modulus);
        CngFree(Plain);
        return STATUS_INVALID_PARAMETER;
    }

    *ResultLength = (ULONG)Written;
    if (Output != NULL)
    {
        if (OutputLength < Written)
        {
            SymCryptWipe(Plain, Modulus);
            CngFree(Plain);
            return STATUS_BUFFER_TOO_SMALL;
        }

        RtlCopyMemory(Output, Plain, Written);
    }

    SymCryptWipe(Plain, Modulus);
    CngFree(Plain);
    return STATUS_SUCCESS;
}

/* EXPORTS ********************************************************************/

/**
 * @brief
 * Imports an RSA or ECC public or private key blob.
 *
 * @param[in] Flags
 * BCRYPT_NO_KEY_VALIDATION.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_NOT_SUPPORTED,
 * STATUS_INVALID_PARAMETER, NTE_BAD_DATA for an RSA blob that does not
 * check out, or STATUS_NO_MEMORY.
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
    PCNG_PROVIDER Provider = CngLookupProvider(Algorithm);
    PCNG_KEY NewKey;
    NTSTATUS Status;
    CNG_BLOB Blob;

    if (Provider == NULL)
        return STATUS_INVALID_HANDLE;
    if (Provider->Algorithm->Class != CngClassRsa && !CngIsEcc(Provider))
        return STATUS_NOT_SUPPORTED;
    if (BlobType == NULL || Key == NULL || Input == NULL || (Flags & ~CNG_IMPORT_FLAGS))
        return STATUS_INVALID_PARAMETER;
    if (ImportKey != NULL)
        return STATUS_NOT_SUPPORTED;

    Status = CngParseBlobType(Provider, BlobType, &Blob);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = CngCreateKey(Provider, NULL, 0, &NewKey);
    if (!NT_SUCCESS(Status))
        return Status;

    if (CngIsEcc(Provider))
        Status = CngImportEcc(NewKey, Blob, Input, InputLength, Flags);
    else
        Status = CngImportRsa(NewKey, Blob, Input, InputLength, Flags);

    if (!NT_SUCCESS(Status))
    {
        CngFreeKey(NewKey);
        return Status;
    }

    NewKey->Flags |= CNG_KEY_FINAL;
    *Key = NewKey;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Starts an RSA or ECC key pair that BCryptFinalizeKeyPair() then generates.
 *
 * @param[in] Length
 * The key size in bits: 512 to 16384 in steps of 8 for RSA, the curve size
 * or zero for ECC.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_NOT_SUPPORTED,
 * STATUS_INVALID_PARAMETER or STATUS_NO_MEMORY.
 */
NTSTATUS
WINAPI
BCryptGenerateKeyPair(
    _Inout_ BCRYPT_ALG_HANDLE Algorithm,
    _Out_ BCRYPT_KEY_HANDLE *Key,
    _In_ ULONG Length,
    _In_ ULONG Flags)
{
    PCNG_PROVIDER Provider = CngLookupProvider(Algorithm);
    PCNG_KEY NewKey;
    NTSTATUS Status;

    if (Provider == NULL)
        return STATUS_INVALID_HANDLE;
    if (Provider->Algorithm->Class != CngClassRsa && !CngIsEcc(Provider))
        return STATUS_NOT_SUPPORTED;
    if (Key == NULL || Flags != 0)
        return STATUS_INVALID_PARAMETER;

    if (!CngIsEcc(Provider))
    {
        if (Length < CNG_RSA_MIN_BITS || Length > CNG_RSA_MAX_BITS || (Length % CNG_RSA_STEP_BITS) != 0)
            return STATUS_INVALID_PARAMETER;
    }
    else if (Length != 0 && (Provider->Curve == NULL || Length != Provider->Curve->Bits))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Status = CngCreateKey(Provider, NULL, 0, &NewKey);
    if (!NT_SUCCESS(Status))
        return Status;

    NewKey->Curve = Provider->Curve;
    NewKey->KeyBits = CngIsEcc(Provider) ? ((Provider->Curve != NULL) ? Provider->Curve->Bits : 0) : Length;

    *Key = NewKey;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Generates the key pair started by BCryptGenerateKeyPair().
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE for a key that is already final,
 * STATUS_INVALID_PARAMETER, STATUS_NO_MEMORY or STATUS_INTERNAL_ERROR.
 */
NTSTATUS
WINAPI
BCryptFinalizeKeyPair(
    _Inout_ BCRYPT_KEY_HANDLE Key,
    _In_ ULONG Flags)
{
    PCNG_KEY Object = CngLookupKey(Key);
    SYMCRYPT_RSA_PARAMS Params;
    SYMCRYPT_ERROR Error;
    NTSTATUS Status;

    if (Object == NULL || (Object->Flags & CNG_KEY_FINAL))
        return STATUS_INVALID_HANDLE;
    if (Object->Provider->Algorithm->Class != CngClassRsa && !CngIsEcc(Object->Provider))
        return STATUS_INVALID_HANDLE;
    if (Flags != 0)
        return STATUS_INVALID_PARAMETER;

    if (!CngIsEcc(Object->Provider))
    {
        Params.version = 1;
        Params.nBitsOfModulus = Object->KeyBits;
        Params.nPrimes = 2;
        Params.nPubExp = 1;

        Object->Rsa = SymCryptRsakeyAllocate(&Params, 0);
        if (Object->Rsa == NULL)
            return STATUS_NO_MEMORY;

        Error = SymCryptRsakeyGenerate(Object->Rsa, NULL, 0, CngRsaKeyFlags(Object, 0));
    }
    else
    {
        if (Object->Curve == NULL)
            return STATUS_INVALID_PARAMETER;

        Status = CngPrepareEcKey(Object, Object->Curve);
        if (!NT_SUCCESS(Status))
            return Status;

        Error = SymCryptEckeySetRandom(CngEcKeyFlags(Object, 0), Object->Ec);
    }

    if (Error != SYMCRYPT_NO_ERROR)
    {
        CngFreeAsymmetricKey(Object);
        return STATUS_INTERNAL_ERROR;
    }

    Object->Flags |= CNG_KEY_FINAL;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Signs a hash with an RSA key (BCRYPT_PAD_PKCS1 or BCRYPT_PAD_PSS) or an ECDSA key.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_NOT_SUPPORTED,
 * STATUS_INVALID_PARAMETER or STATUS_BUFFER_TOO_SMALL. With no output only
 * the signature size is returned.
 */
NTSTATUS
WINAPI
BCryptSignHash(
    _In_ BCRYPT_KEY_HANDLE Key,
    _In_opt_ VOID *PaddingInfo,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    PCNG_KEY Object = CngLookupKey(Key);
    BCRYPT_PKCS1_PADDING_INFO *Pkcs1 = PaddingInfo;
    BCRYPT_PSS_PADDING_INFO *Pss = PaddingInfo;
    const CNG_SIGNATURE_HASH *Hash = NULL;
    SYMCRYPT_ERROR Error;
    SIZE_T Written;
    ULONG Needed;

    if (Object == NULL || !(Object->Flags & CNG_KEY_FINAL))
        return STATUS_INVALID_HANDLE;
    if (ResultLength == NULL || (Input == NULL && InputLength != 0))
        return STATUS_INVALID_PARAMETER;

    if (Object->Provider->Algorithm->Class == CngClassEcdsa)
    {
        if (!SymCryptEckeyHasPrivateKey(Object->Ec))
            return STATUS_INVALID_PARAMETER;

        Needed = 2 * CngFieldLength(Object->Curve);
        *ResultLength = Needed;
        if (Output == NULL)
            return STATUS_SUCCESS;
        if (OutputLength < Needed)
            return STATUS_BUFFER_TOO_SMALL;

        Error = SymCryptEcDsaSign(Object->Ec, Input, InputLength, SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, 0, Output, Needed);
        return (Error == SYMCRYPT_NO_ERROR) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    }

    if (Object->Provider->Algorithm->Class != CngClassRsa)
        return STATUS_NOT_SUPPORTED;

    Flags &= ~BCRYPT_BUFFERS_LOCKED_FLAG;
    if ((Flags != BCRYPT_PAD_PKCS1 && Flags != BCRYPT_PAD_PSS) || PaddingInfo == NULL)
        return STATUS_INVALID_PARAMETER;
    if (!SymCryptRsakeyHasPrivateKey(Object->Rsa))
        return STATUS_INVALID_PARAMETER;

    /* PKCS1 without an algorithm signs the bare hash, with no DigestInfo */
    if (Flags == BCRYPT_PAD_PSS || Pkcs1->pszAlgId != NULL)
    {
        Hash = CngFindSignatureHash((Flags == BCRYPT_PAD_PSS) ? Pss->pszAlgId : Pkcs1->pszAlgId);
        if (Hash == NULL)
            return (Flags == BCRYPT_PAD_PSS && Pss->pszAlgId == NULL) ? STATUS_INVALID_PARAMETER : STATUS_NOT_SUPPORTED;
        if (InputLength != SymCryptHashResultSize(*Hash->Hash))
            return STATUS_INVALID_PARAMETER;
    }

    Needed = CngRsaModulusLength(Object);
    *ResultLength = Needed;
    if (Output == NULL)
        return STATUS_SUCCESS;
    if (OutputLength < Needed)
        return STATUS_BUFFER_TOO_SMALL;

    if (Flags == BCRYPT_PAD_PSS)
    {
        Error = SymCryptRsaPssSign(Object->Rsa, Input, InputLength, *Hash->Hash, Pss->cbSalt, 0,
                                   SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, Output, Needed, &Written);
    }
    else if (Hash != NULL)
    {
        Error = SymCryptRsaPkcs1Sign(Object->Rsa, Input, InputLength, Hash->Oids, Hash->OidCount, 0,
                                     SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, Output, Needed, &Written);
    }
    else
    {
        Error = SymCryptRsaPkcs1Sign(Object->Rsa, Input, InputLength, NULL, 0, SYMCRYPT_FLAG_RSA_PKCS1_NO_ASN1,
                                     SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, Output, Needed, &Written);
    }

    return (Error == SYMCRYPT_NO_ERROR) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

/**
 * @brief
 * Checks an RSA (PKCS1 or PSS) or ECDSA signature over a hash.
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
    PCNG_KEY Object = CngLookupKey(Key);
    BCRYPT_PKCS1_PADDING_INFO *Pkcs1 = PaddingInfo;
    BCRYPT_PSS_PADDING_INFO *Pss = PaddingInfo;
    const CNG_SIGNATURE_HASH *SignatureHash = NULL;
    UINT32 OidFlags = 0;
    SYMCRYPT_ERROR Error;

    if (Object == NULL || !(Object->Flags & CNG_KEY_FINAL))
        return STATUS_INVALID_HANDLE;
    if (Hash == NULL || Signature == NULL)
        return STATUS_INVALID_PARAMETER;

    if (Object->Provider->Algorithm->Class == CngClassEcdsa)
    {
        if (SignatureLength != 2 * CngFieldLength(Object->Curve))
            return STATUS_INVALID_PARAMETER;

        Error = SymCryptEcDsaVerify(Object->Ec, Hash, HashLength, Signature, SignatureLength,
                                    SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, 0);
        return (Error == SYMCRYPT_NO_ERROR) ? STATUS_SUCCESS : STATUS_INVALID_SIGNATURE;
    }

    if (Object->Provider->Algorithm->Class != CngClassRsa)
        return STATUS_NOT_SUPPORTED;

    Flags &= ~BCRYPT_BUFFERS_LOCKED_FLAG;
    if (Flags == (BCRYPT_PAD_PKCS1 | BCRYPT_PAD_PKCS1_OPTIONAL_HASH_OID))
    {
        OidFlags = SYMCRYPT_FLAG_RSA_PKCS1_OPTIONAL_HASH_OID;
        Flags = BCRYPT_PAD_PKCS1;
    }
    if ((Flags != BCRYPT_PAD_PKCS1 && Flags != BCRYPT_PAD_PSS) || PaddingInfo == NULL)
        return STATUS_INVALID_PARAMETER;
    if (SignatureLength != CngRsaModulusLength(Object))
        return STATUS_INVALID_PARAMETER;

    if (Flags == BCRYPT_PAD_PSS || Pkcs1->pszAlgId != NULL)
    {
        SignatureHash = CngFindSignatureHash((Flags == BCRYPT_PAD_PSS) ? Pss->pszAlgId : Pkcs1->pszAlgId);
        if (SignatureHash == NULL)
            return (Flags == BCRYPT_PAD_PSS && Pss->pszAlgId == NULL) ? STATUS_INVALID_PARAMETER : STATUS_NOT_SUPPORTED;
        if (HashLength != SymCryptHashResultSize(*SignatureHash->Hash))
            return STATUS_INVALID_PARAMETER;
    }

    if (Flags == BCRYPT_PAD_PSS)
    {
        Error = SymCryptRsaPssVerify(Object->Rsa, Hash, HashLength, Signature, SignatureLength,
                                     SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, *SignatureHash->Hash, Pss->cbSalt, 0);
    }
    else if (SignatureHash != NULL)
    {
        Error = SymCryptRsaPkcs1Verify(Object->Rsa, Hash, HashLength, Signature, SignatureLength,
                                       SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, SignatureHash->Oids, SignatureHash->OidCount,
                                       OidFlags);
    }
    else
    {
        Error = SymCryptRsaPkcs1Verify(Object->Rsa, Hash, HashLength, Signature, SignatureLength,
                                       SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, NULL, 0, 0);
    }

    return (Error == SYMCRYPT_NO_ERROR) ? STATUS_SUCCESS : STATUS_INVALID_SIGNATURE;
}

/**
 * @brief
 * Agrees on an ECDH secret between a private key and a peer public key.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_NOT_SUPPORTED for keys
 * that are not ECDH, STATUS_INVALID_PARAMETER or STATUS_NO_MEMORY.
 */
NTSTATUS
WINAPI
BCryptSecretAgreement(
    _In_ BCRYPT_KEY_HANDLE PrivateKey,
    _In_ BCRYPT_KEY_HANDLE PublicKey,
    _Out_ BCRYPT_SECRET_HANDLE *Secret,
    _In_ ULONG Flags)
{
    PCNG_KEY Private = CngLookupKey(PrivateKey);
    PCNG_KEY Public = CngLookupKey(PublicKey);
    PCNG_SECRET NewSecret;
    ULONG Field;

    if (Private == NULL || Public == NULL ||
        !(Private->Flags & CNG_KEY_FINAL) || !(Public->Flags & CNG_KEY_FINAL))
    {
        return STATUS_INVALID_HANDLE;
    }

    if (Private->Provider->Algorithm->Class != CngClassEcdh || Public->Provider->Algorithm->Class != CngClassEcdh)
        return STATUS_NOT_SUPPORTED;
    if (Secret == NULL || Flags != 0)
        return STATUS_INVALID_PARAMETER;
    if (Private->Curve != Public->Curve || !SymCryptEckeyHasPrivateKey(Private->Ec))
        return STATUS_INVALID_PARAMETER;

    Field = CngFieldLength(Private->Curve);
    if (Field > sizeof(NewSecret->Value))
        return STATUS_INVALID_PARAMETER;

    NewSecret = CngAllocate(sizeof(*NewSecret));
    if (NewSecret == NULL)
        return STATUS_NO_MEMORY;

    RtlZeroMemory(NewSecret, sizeof(*NewSecret));
    if (SymCryptEcDhSecretAgreement(Private->Ec, Public->Ec, SYMCRYPT_NUMBER_FORMAT_MSB_FIRST, 0,
                                    NewSecret->Value, Field) != SYMCRYPT_NO_ERROR)
    {
        CngFree(NewSecret);
        return STATUS_INVALID_PARAMETER;
    }

    NewSecret->Signature = CNG_SECRET_SIGNATURE;
    NewSecret->Provider = Private->Provider;
    NewSecret->Length = Field;
    CngReferenceProvider(Private->Provider);

    *Secret = NewSecret;
    return STATUS_SUCCESS;
}

PCNG_SECRET
NTAPI
CngLookupSecret(
    _In_opt_ BCRYPT_SECRET_HANDLE Handle)
{
    PCNG_SECRET Secret = (PCNG_SECRET)Handle;

    if (Secret == NULL || ((ULONG_PTR)Handle & (sizeof(PVOID) - 1)) != 0)
        return NULL;
    if (Secret->Signature != CNG_SECRET_SIGNATURE || !CngObjectUsable(Secret->Provider))
        return NULL;

    return Secret;
}

/**
 * @brief
 * Destroys a secret agreement.
 */
NTSTATUS
WINAPI
BCryptDestroySecret(
    _In_ BCRYPT_SECRET_HANDLE Secret)
{
    PCNG_SECRET Object = CngLookupSecret(Secret);
    PCNG_PROVIDER Provider;

    if (Object == NULL)
        return STATUS_INVALID_HANDLE;

    Provider = Object->Provider;
    SymCryptWipe(Object, sizeof(*Object));
    CngFree(Object);
    CngDereferenceProvider(Provider);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Derives key material from a secret agreement with the HASH, HMAC,
 * TLS_PRF, SP800_56A_CONCAT or TRUNCATE key derivation functions.
 */
NTSTATUS
WINAPI
BCryptDeriveKey(
    _In_ BCRYPT_SECRET_HANDLE SharedSecret,
    _In_ LPCWSTR KeyDerivationFunction,
    _In_opt_ BCryptBufferDesc *ParameterList,
    _Out_writes_bytes_to_opt_(DerivedKeyLength, *ResultLength) PUCHAR DerivedKey,
    _In_ ULONG DerivedKeyLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    PCNG_SECRET Secret = CngLookupSecret(SharedSecret);

    if (Secret == NULL)
        return STATUS_INVALID_HANDLE;
    if (KeyDerivationFunction == NULL || ResultLength == NULL)
        return STATUS_INVALID_PARAMETER;

    return CngDeriveFromSecret(Secret, KeyDerivationFunction, ParameterList, DerivedKey,
                               DerivedKeyLength, ResultLength, Flags);
}

/**
 * @brief
 * Key encapsulation needs an ML-KEM key, which this provider does not have.
 */
NTSTATUS
WINAPI
BCryptEncapsulate(
    _In_ BCRYPT_KEY_HANDLE Key,
    _Out_writes_bytes_to_opt_(SecretKeyLength, *SecretKeyResult) PUCHAR SecretKey,
    _In_ ULONG SecretKeyLength,
    _Out_ ULONG *SecretKeyResult,
    _Out_writes_bytes_to_opt_(CipherTextLength, *CipherTextResult) PUCHAR CipherText,
    _In_ ULONG CipherTextLength,
    _Out_ ULONG *CipherTextResult,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(SecretKey);
    UNREFERENCED_PARAMETER(SecretKeyLength);
    UNREFERENCED_PARAMETER(SecretKeyResult);
    UNREFERENCED_PARAMETER(CipherText);
    UNREFERENCED_PARAMETER(CipherTextLength);
    UNREFERENCED_PARAMETER(CipherTextResult);
    UNREFERENCED_PARAMETER(Flags);

    if (CngLookupKey(Key) == NULL)
        return STATUS_INVALID_HANDLE;

    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
WINAPI
BCryptDecapsulate(
    _In_ BCRYPT_KEY_HANDLE Key,
    _In_reads_bytes_(CipherTextLength) PUCHAR CipherText,
    _In_ ULONG CipherTextLength,
    _Out_writes_bytes_to_opt_(SecretKeyLength, *SecretKeyResult) PUCHAR SecretKey,
    _In_ ULONG SecretKeyLength,
    _Out_ ULONG *SecretKeyResult,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(CipherText);
    UNREFERENCED_PARAMETER(CipherTextLength);
    UNREFERENCED_PARAMETER(SecretKey);
    UNREFERENCED_PARAMETER(SecretKeyLength);
    UNREFERENCED_PARAMETER(SecretKeyResult);
    UNREFERENCED_PARAMETER(Flags);

    if (CngLookupKey(Key) == NULL)
        return STATUS_INVALID_HANDLE;

    return STATUS_NOT_SUPPORTED;
}

/* EOF */
