/*
 * PROJECT:     ReactOS Kernel Cryptography
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private definitions of the kernel mode CNG primitives
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntddk.h>
#include <bcrypt.h>

#include <symcrypt.h>

#define BCRYPTK_POOL_TAG 'krcB'

/* SymCrypt keeps its states in the Xmm registers, which want this alignment */
#define BCRYPTK_ALIGNMENT 16

#define BCRYPTK_PROVIDER_TAG 0x76507243
#define BCRYPTK_KEY_TAG      0x79654B43
#define BCRYPTK_HASH_TAG     0x73614843
#define BCRYPTK_OPAQUE_TAG   0x7571704F

typedef enum _BCRYPTK_CLASS
{
    BcryptkClassCipher,
    BcryptkClassHash,
    BcryptkClassRandom
} BCRYPTK_CLASS;

typedef enum _BCRYPTK_CHAIN
{
    BcryptkChainNone,
    BcryptkChainEcb,
    BcryptkChainCbc,
    BcryptkChainCfb
} BCRYPTK_CHAIN, *PBCRYPTK_CHAIN;

/* One row per algorithm this provider answers to */
typedef struct _BCRYPTK_ALGORITHM
{
    PCWSTR Name;
    BCRYPTK_CLASS Class;
    const PCSYMCRYPT_BLOCKCIPHER *Cipher;
    const PCSYMCRYPT_HASH *Hash;
    const PCSYMCRYPT_MAC *Mac;
    ULONG MinKeyLength;
    ULONG MaxKeyLength;
    ULONG KeyIncrement;
} BCRYPTK_ALGORITHM;

typedef const BCRYPTK_ALGORITHM *PCBCRYPTK_ALGORITHM;

typedef struct _BCRYPTK_PROVIDER
{
    ULONG Tag;
    PCBCRYPTK_ALGORITHM Algorithm;
    BCRYPTK_CHAIN ChainingMode;
    ULONG MessageBlockLength;
    BOOLEAN Mac;
} BCRYPTK_PROVIDER, *PBCRYPTK_PROVIDER;

typedef struct _BCRYPTK_KEY
{
    ULONG Tag;
    PCBCRYPTK_ALGORITHM Algorithm;
    BCRYPTK_CHAIN ChainingMode;
    ULONG MessageBlockLength;
    ULONG KeyLength;
    BOOLEAN Allocated;
    PVOID ExpandedKey;
    UCHAR Storage[ANYSIZE_ARRAY];
} BCRYPTK_KEY, *PBCRYPTK_KEY;

typedef struct _BCRYPTK_HASH
{
    ULONG Tag;
    PCBCRYPTK_ALGORITHM Algorithm;
    BOOLEAN Allocated;
    BOOLEAN Mac;
    PVOID State;
    PVOID ExpandedKey;
    UCHAR Storage[ANYSIZE_ARRAY];
} BCRYPTK_HASH, *PBCRYPTK_HASH;

VOID
BcryptkInitialize(VOID);

NTSTATUS
BcryptkCreateOpaqueKey(
    _Out_ BCRYPT_KEY_HANDLE *Key);

PCBCRYPTK_ALGORITHM
BcryptkFindAlgorithm(
    _In_ PCWSTR Name);

ULONG
BcryptkKeyObjectLength(
    _In_ PCBCRYPTK_ALGORITHM Algorithm);

ULONG
BcryptkHashObjectLength(
    _In_ PCBCRYPTK_ALGORITHM Algorithm,
    _In_ BOOLEAN Mac);

PVOID
BcryptkAllocateObject(
    _In_ ULONG Length,
    _Inout_opt_ PUCHAR Caller,
    _In_ ULONG CallerLength,
    _Out_ PBOOLEAN Allocated);

VOID
BcryptkFreeObject(
    _In_ PVOID Object,
    _In_ BOOLEAN Allocated);

FORCEINLINE
PVOID
BcryptkAlign(
    _In_ PVOID Address)
{
    return (PVOID)(((ULONG_PTR)Address + BCRYPTK_ALIGNMENT - 1) & ~(ULONG_PTR)(BCRYPTK_ALIGNMENT - 1));
}

/* EOF */
