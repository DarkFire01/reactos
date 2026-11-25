/*
 * PROJECT:     ReactOS Kernel Cryptography Next Generation
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private objects and routines shared by the cng files
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntifs.h>
#include <ndk/exfuncs.h>
#include <ndk/ketypes.h>
#include <bcrypt.h>
#include <symcrypt.h>

/* PUBLIC DEFINITIONS MISSING FROM THE PSDK HEADER ****************************/

#ifndef BCRYPT_PROV_DISPATCH
#define BCRYPT_PROV_DISPATCH                0x00000001
#endif
#ifndef BCRYPT_MULTI_FLAG
#define BCRYPT_MULTI_FLAG                   0x00000040
#endif
#ifndef BCRYPT_NO_KEY_VALIDATION
#define BCRYPT_NO_KEY_VALIDATION            0x00000008
#endif
#ifndef BCRYPT_IS_REUSABLE_HASH
#define BCRYPT_IS_REUSABLE_HASH             L"IsReusableHash"
#endif
#ifndef BCRYPT_IS_KEYED_HASH
#define BCRYPT_IS_KEYED_HASH                L"IsKeyedHash"
#endif
#ifndef BCRYPT_MULTI_OBJECT_LENGTH
#define BCRYPT_MULTI_OBJECT_LENGTH          L"MultiObjectLength"
#endif
#ifndef BCRYPT_ECC_CURVE_NISTP256
#define BCRYPT_ECC_CURVE_NISTP256           L"nistP256"
#define BCRYPT_ECC_CURVE_NISTP384           L"nistP384"
#define BCRYPT_ECC_CURVE_NISTP521           L"nistP521"
#endif
#ifndef BCRYPT_SHA3_256_ALGORITHM
#define BCRYPT_SHA3_256_ALGORITHM           L"SHA3-256"
#define BCRYPT_SHA3_384_ALGORITHM           L"SHA3-384"
#define BCRYPT_SHA3_512_ALGORITHM           L"SHA3-512"
#endif
#ifndef BCRYPT_AES_CMAC_ALGORITHM
#define BCRYPT_AES_CMAC_ALGORITHM           L"AES-CMAC"
#endif
#ifndef BCRYPT_3DES_112_ALGORITHM
#define BCRYPT_3DES_112_ALGORITHM           L"3DES_112"
#endif
#ifndef BCRYPT_CAPI_KDF_ALGORITHM
#define BCRYPT_CAPI_KDF_ALGORITHM           L"CAPI_KDF"
#endif
#ifndef BCRYPT_SP800108_CTR_HMAC_ALGORITHM
#define BCRYPT_SP800108_CTR_HMAC_ALGORITHM  L"SP800_108_CTR_HMAC"
#endif
#ifndef BCRYPT_SP80056A_CONCAT_ALGORITHM
#define BCRYPT_SP80056A_CONCAT_ALGORITHM    L"SP800_56A_CONCAT"
#endif
#ifndef KDF_GENERIC_PARAMETER
#define KDF_GENERIC_PARAMETER               0x11
#endif
#ifndef KDF_KEYBITLENGTH
#define KDF_KEYBITLENGTH                    0x12
#endif
#ifndef KDF_USE_SECRET_AS_HMAC_KEY_FLAG
#define KDF_USE_SECRET_AS_HMAC_KEY_FLAG     0x00000001
#endif
#ifndef BCRYPT_BUFFERS_LOCKED_FLAG
#define BCRYPT_BUFFERS_LOCKED_FLAG          0x00000040
#endif
#ifndef BCRYPT_CAPI_AES_FLAG
#define BCRYPT_CAPI_AES_FLAG                0x00000010
#endif
#ifndef CRYPT_OVERWRITE
#define CRYPT_OVERWRITE                     0x00000001
#endif
#ifndef NTE_BAD_DATA
#define NTE_BAD_DATA                        ((NTSTATUS)0x80090005L)
#endif
#ifndef STATUS_AUTH_TAG_MISMATCH
#define STATUS_AUTH_TAG_MISMATCH            ((NTSTATUS)0xC000A002L)
#endif

#ifndef TLS1_PROTOCOL_VERSION
#define TLS1_PROTOCOL_VERSION               0x0301
#define TLS1_1_PROTOCOL_VERSION             0x0302
#define TLS1_2_PROTOCOL_VERSION             0x0303
#define DTLS1_0_PROTOCOL_VERSION            0xFEFF
#define DTLS1_2_PROTOCOL_VERSION            0xFEFD
#endif

typedef struct _BCRYPT_PROVIDER_NAME
{
    LPWSTR pszProviderName;
} BCRYPT_PROVIDER_NAME;

typedef enum
{
    BCRYPT_HASH_OPERATION_HASH_DATA = 1,
    BCRYPT_HASH_OPERATION_FINISH_HASH = 2,
} BCRYPT_HASH_OPERATION_TYPE;

typedef struct _BCRYPT_MULTI_HASH_OPERATION
{
    ULONG iHash;
    BCRYPT_HASH_OPERATION_TYPE hashOperation;
    PUCHAR pbBuffer;
    ULONG cbBuffer;
} BCRYPT_MULTI_HASH_OPERATION;

typedef enum
{
    BCRYPT_OPERATION_TYPE_HASH = 1,
} BCRYPT_MULTI_OPERATION_TYPE;

typedef struct _BCRYPT_MULTI_OBJECT_LENGTH_STRUCT
{
    ULONG cbPerObject;
    ULONG cbPerElement;
} BCRYPT_MULTI_OBJECT_LENGTH_STRUCT;

typedef struct _CRYPT_PROPERTY_REF
{
    PWSTR pszProperty;
    ULONG cbValue;
    PUCHAR pbValue;
} CRYPT_PROPERTY_REF, *PCRYPT_PROPERTY_REF;

typedef struct _CRYPT_IMAGE_REF
{
    PWSTR pszImage;
    ULONG dwFlags;
} CRYPT_IMAGE_REF, *PCRYPT_IMAGE_REF;

typedef struct _CRYPT_PROVIDER_REF
{
    ULONG dwInterface;
    PWSTR pszFunction;
    PWSTR pszProvider;
    ULONG cProperties;
    PCRYPT_PROPERTY_REF *rgpProperties;
    PCRYPT_IMAGE_REF pUM;
    PCRYPT_IMAGE_REF pKM;
} CRYPT_PROVIDER_REF, *PCRYPT_PROVIDER_REF;

typedef struct _CRYPT_PROVIDER_REFS
{
    ULONG cProviders;
    PCRYPT_PROVIDER_REF *rgpProviders;
} CRYPT_PROVIDER_REFS, *PCRYPT_PROVIDER_REFS;

/* TAGS AND SIGNATURES ********************************************************/

#define TAG_CNG                 'gnCK'
#define TAG_CNG_SYMCRYPT        'ySCK'
#define TAG_CNG_BUFFER          'fBCK'

#define CNG_PROVIDER_SIGNATURE  'PgnC'
#define CNG_HASH_SIGNATURE      'HgnC'
#define CNG_KEY_SIGNATURE       'KgnC'
#define CNG_SECRET_SIGNATURE    'SgnC'

#define CNG_OBJECT_ALIGNMENT    ((SYMCRYPT_ALIGN_VALUE > 8) ? SYMCRYPT_ALIGN_VALUE : 8)
#define CNG_ALIGN(Size)         (((Size) + CNG_OBJECT_ALIGNMENT - 1) & ~(CNG_OBJECT_ALIGNMENT - 1))

/* ALGORITHMS *****************************************************************/

typedef enum _CNG_CLASS
{
    CngClassRng,
    CngClassHash,
    CngClassMac,
    CngClassCipher,
    CngClassRsa,
    CngClassEcdsa,
    CngClassEcdh,
    CngClassKdf
} CNG_CLASS;

typedef enum _CNG_CIPHER
{
    CngCipherAes,
    CngCipherDes,
    CngCipher3Des,
    CngCipher3Des112,
    CngCipherRc4
} CNG_CIPHER;

typedef enum _CNG_KDF
{
    CngKdfPbkdf2,
    CngKdfSp800108,
    CngKdfSp80056a,
    CngKdfHkdf,
    CngKdfCapi,
    CngKdfTls11,
    CngKdfTls12
} CNG_KDF;

typedef enum _CNG_MODE
{
    CngModeNone,
    CngModeCbc,
    CngModeEcb,
    CngModeCfb,
    CngModeCcm,
    CngModeGcm
} CNG_MODE;

typedef struct _CNG_CURVE
{
    PCWSTR Name;
    const PCSYMCRYPT_ECURVE_PARAMS *Params;
    ULONG Bits;
    ULONG EcdsaPublicMagic;
    ULONG EcdsaPrivateMagic;
    ULONG EcdhPublicMagic;
    ULONG EcdhPrivateMagic;
} CNG_CURVE, *PCNG_CURVE;

typedef
VOID
(SYMCRYPT_CALL *PCNG_MAC_STATE_COPY)(
    _In_ PCVOID Source,
    _In_opt_ PCVOID ExpandedKey,
    _Out_ PVOID Destination);

typedef struct _CNG_ALGORITHM
{
    PCWSTR Name;
    CNG_CLASS Class;
    ULONG Interface;
    ULONG Operations;
    const PCSYMCRYPT_HASH *Hash;
    const PCSYMCRYPT_MAC *Mac;
    PCNG_MAC_STATE_COPY MacCopy;
    CNG_CIPHER Cipher;
    CNG_KDF Kdf;
    const CNG_CURVE *Curve;
    BOOLEAN SignOnly;
} CNG_ALGORITHM, *PCNG_ALGORITHM;

/* OBJECTS ********************************************************************/

/* Hash objects and keys hold a reference, so a provider closed under them lives on */
typedef struct _CNG_PROVIDER
{
    ULONG Signature;
    LONG References;
    const CNG_ALGORITHM *Algorithm;
    ULONG Flags;
    BOOLEAN Pseudo;
    CNG_MODE Mode;
    ULONG MessageBlock;
    const CNG_CURVE *Curve;
} CNG_PROVIDER, *PCNG_PROVIDER;

#define CNG_HASH_REUSABLE       0x00000001
#define CNG_HASH_FINISHED       0x00000002
#define CNG_HASH_MULTI          0x00000004

/* A multi hash keeps Count elements of Stride bytes, each a state and an expanded key */
typedef struct _CNG_HASH
{
    ULONG Signature;
    ULONG Flags;
    PCNG_PROVIDER Provider;
    PVOID Allocation;
    PUCHAR Elements;
    ULONG Count;
    ULONG Stride;
    ULONG KeyOffset;
} CNG_HASH, *PCNG_HASH;

#define CNG_KEY_FINAL           0x00000001
#define CNG_KEY_OWN_SECRET      0x00000002

/* Symmetric keys keep their expanded form and a copy of the secret behind the header */
typedef struct _CNG_KEY
{
    ULONG Signature;
    ULONG Flags;
    PCNG_PROVIDER Provider;
    PVOID Allocation;
    PUCHAR Secret;
    PVOID Expanded;
    PVOID Chain;
    ULONG KeyBits;
    ULONG SecretLength;
    CNG_MODE Mode;
    ULONG MessageBlock;
    UCHAR Iv[SYMCRYPT_AES_BLOCK_SIZE];
    PSYMCRYPT_RSAKEY Rsa;
    PSYMCRYPT_ECKEY Ec;
    const CNG_CURVE *Curve;
    PSYMCRYPT_ECURVE EcCurve;
} CNG_KEY, *PCNG_KEY;

/* The expanded part of a key derivation key */
typedef struct _CNG_KDF_STATE
{
    PCSYMCRYPT_MAC Mac;
    ULONG PrkLength;
    BOOLEAN Final;
    UCHAR Prk[SYMCRYPT_HASH_MAX_RESULT_SIZE];
} CNG_KDF_STATE, *PCNG_KDF_STATE;

typedef struct _CNG_SECRET
{
    ULONG Signature;
    PCNG_PROVIDER Provider;
    ULONG Length;
    UCHAR Value[72];
} CNG_SECRET, *PCNG_SECRET;

/* ROUTINES *******************************************************************/

/* cng.c */
VOID
NTAPI
CngInitialize(VOID);

NTSTATUS
NTAPI
CngGenerateRandom(
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ SIZE_T Length);

VOID
NTAPI
CngMixEntropy(
    _In_reads_bytes_(Length) const VOID *Data,
    _In_ SIZE_T Length);

PVOID
NTAPI
CngAllocate(
    _In_ SIZE_T Size);

VOID
NTAPI
CngFree(
    _In_opt_ PVOID Block);

/* provider.c */
PCNG_PROVIDER
NTAPI
CngLookupProvider(
    _In_opt_ BCRYPT_HANDLE Handle);

BOOLEAN
NTAPI
CngObjectUsable(
    _In_ PCNG_PROVIDER Provider);

VOID
NTAPI
CngReferenceProvider(
    _In_ PCNG_PROVIDER Provider);

VOID
NTAPI
CngDereferenceProvider(
    _In_ PCNG_PROVIDER Provider);

const CNG_ALGORITHM *
NTAPI
CngFindAlgorithm(
    _In_ LPCWSTR Name);

const CNG_CURVE *
NTAPI
CngFindCurve(
    _In_ LPCWSTR Name);

const CNG_CURVE *
NTAPI
CngFindCurveByMagic(
    _In_ ULONG Magic,
    _In_ BOOLEAN Ecdh,
    _Out_ PBOOLEAN Private);

BOOLEAN
NTAPI
CngPropertyIs(
    _In_reads_bytes_(Length) const UCHAR *Input,
    _In_ ULONG Length,
    _In_ PCWSTR Value);

NTSTATUS
NTAPI
CngReturnProperty(
    _In_reads_bytes_(Size) const VOID *Value,
    _In_ ULONG Size,
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength);

/* hash.c */
PCNG_HASH
NTAPI
CngLookupHash(
    _In_opt_ BCRYPT_HANDLE Handle);

ULONG
NTAPI
CngHashObjectLength(
    _In_ const CNG_ALGORITHM *Algorithm,
    _In_ BOOLEAN Hmac);

ULONG
NTAPI
CngDigestLength(
    _In_ PCNG_PROVIDER Provider);

PCSYMCRYPT_MAC
NTAPI
CngMacForHash(
    _In_ const CNG_ALGORITHM *Algorithm);

NTSTATUS
NTAPI
CngQueryHashProperty(
    _In_ PCNG_PROVIDER Provider,
    _In_ LPCWSTR Property,
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength);

NTSTATUS
NTAPI
CngHashFinal(
    _In_ PCNG_HASH Hash,
    _Out_writes_bytes_(Length) PUCHAR Output,
    _In_ ULONG Length);

/* key.c */
PCNG_KEY
NTAPI
CngLookupKey(
    _In_opt_ BCRYPT_HANDLE Handle);

NTSTATUS
NTAPI
CngCreateKey(
    _In_ PCNG_PROVIDER Provider,
    _Out_writes_bytes_opt_(ObjectLength) PUCHAR Object,
    _In_ ULONG ObjectLength,
    _Out_ PCNG_KEY *Key);

VOID
NTAPI
CngFreeKey(
    _In_ PCNG_KEY Key);

ULONG
NTAPI
CngKeyObjectLength(
    _In_ const CNG_ALGORITHM *Algorithm);

NTSTATUS
NTAPI
CngQueryKeyProperty(
    _In_ PCNG_PROVIDER Provider,
    _In_opt_ PCNG_KEY Key,
    _In_ LPCWSTR Property,
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength);

NTSTATUS
NTAPI
CngSetKeyProperty(
    _In_ PCNG_PROVIDER Provider,
    _In_opt_ PCNG_KEY Key,
    _In_ LPCWSTR Property,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength);

/* asym.c */
NTSTATUS
NTAPI
CngQueryAsymmetricProperty(
    _In_ PCNG_PROVIDER Provider,
    _In_opt_ PCNG_KEY Key,
    _In_ LPCWSTR Property,
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength);

PCNG_SECRET
NTAPI
CngLookupSecret(
    _In_opt_ BCRYPT_SECRET_HANDLE Handle);

VOID
NTAPI
CngFreeAsymmetricKey(
    _In_ PCNG_KEY Key);

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
    _In_ ULONG Flags);

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
    _In_ ULONG Flags);

NTSTATUS
NTAPI
CngExportAsymmetricKey(
    _In_ PCNG_KEY Key,
    _In_ LPCWSTR BlobType,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength);

NTSTATUS
NTAPI
CngDuplicateAsymmetricKey(
    _In_ PCNG_KEY Source,
    _Inout_ PCNG_KEY Copy);

/* kdf.c */
const CNG_ALGORITHM *
NTAPI
CngHashFromBuffer(
    _In_ const BCryptBuffer *Buffer,
    _Out_ NTSTATUS *Status);

NTSTATUS
NTAPI
CngDeriveFromSecret(
    _In_ PCNG_SECRET Secret,
    _In_ LPCWSTR Kdf,
    _In_opt_ BCryptBufferDesc *ParameterList,
    _Out_writes_bytes_to_opt_(DerivedLength, *ResultLength) PUCHAR DerivedKey,
    _In_ ULONG DerivedLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags);

NTSTATUS
NTAPI
CngCapiExpand(
    _In_ PCSYMCRYPT_HASH Hash,
    _In_reads_bytes_(DigestLength) const UCHAR *Digest,
    _In_ ULONG DigestLength,
    _In_ BOOLEAN Force,
    _Out_writes_bytes_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength);

NTSTATUS
NTAPI
CngSetKdfProperty(
    _In_ PCNG_KEY Key,
    _In_ LPCWSTR Property,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength);
