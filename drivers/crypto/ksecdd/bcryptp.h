/*
 * PROJECT:     ReactOS Kernel Security Support Provider Interface Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Cryptography Next Generation objects shared by the bcrypt files
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <bcrypt.h>
#include <symcrypt.h>

#define TAG_KSEC_BCRYPT         'cBsK'
#define KSEC_PROVIDER_MAGIC     'vPsK'
#define KSEC_HASH_MAGIC         'hHsK'
#define KSEC_KEY_MAGIC          'yKsK'

typedef enum _KSEC_ALGORITHM_CLASS
{
    KsecClassRng,
    KsecClassHash,
    KsecClassCipher,
    KsecClassRsa,
    KsecClassEcdsa
} KSEC_ALGORITHM_CLASS;

typedef enum _KSEC_CHAINING_MODE
{
    KsecChainingCbc,
    KsecChainingEcb
} KSEC_CHAINING_MODE;

typedef struct _KSEC_ALGORITHM
{
    PCWSTR Name;
    KSEC_ALGORITHM_CLASS Class;
    const PCSYMCRYPT_HASH *Hash;
    const PCSYMCRYPT_MAC *Hmac;
    const PCSYMCRYPT_ECURVE_PARAMS *CurveParams;
    ULONG CurveBits;
    ULONG PublicMagic;
    ULONG PrivateMagic;
} KSEC_ALGORITHM, *PKSEC_ALGORITHM;

/* Keys hold a reference, so a provider closed under them lives until the last one goes */
typedef struct _KSEC_PROVIDER
{
    ULONG Magic;
    LONG References;
    const KSEC_ALGORITHM *Algorithm;
    BOOLEAN Hmac;
    KSEC_CHAINING_MODE ChainingMode;
    PSYMCRYPT_ECURVE Curve;
} KSEC_PROVIDER, *PKSEC_PROVIDER;

typedef struct _KSEC_KEY
{
    ULONG Magic;
    PKSEC_PROVIDER Provider;
    PVOID Allocation;
    BOOLEAN Finalized;
    KSEC_CHAINING_MODE ChainingMode;
    ULONG KeyBits;
    ULONG SecretLength;
    UCHAR Secret[32];
    SYMCRYPT_AES_EXPANDED_KEY Aes;
    PSYMCRYPT_RSAKEY Rsa;
    PSYMCRYPT_ECKEY Ec;
} KSEC_KEY, *PKSEC_KEY;

/* Symmetric key objects live at the first suitably aligned address of their buffer */
#define KSEC_KEY_OBJECT_LENGTH  (sizeof(KSEC_KEY) + SYMCRYPT_ALIGN_VALUE)

PKSEC_PROVIDER
KsecReferenceProvider(
    _In_opt_ BCRYPT_HANDLE Handle);

VOID
KsecReleaseProvider(
    _In_ PKSEC_PROVIDER Provider);

PKSEC_KEY
KsecReferenceKey(
    _In_opt_ BCRYPT_HANDLE Handle);

NTSTATUS
KsecQueryKeyProperty(
    _In_ PKSEC_PROVIDER Provider,
    _In_opt_ PKSEC_KEY Key,
    _In_ LPCWSTR Property,
    _Out_ const VOID **Value,
    _Out_ PULONG Size,
    _Out_writes_bytes_(sizeof(BCRYPT_KEY_LENGTHS_STRUCT)) PVOID Scratch);
