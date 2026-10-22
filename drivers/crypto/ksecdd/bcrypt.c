/*
 * PROJECT:     ReactOS Kernel Security Support Provider Interface Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Cryptography Next Generation providers, random numbers and hashing
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ksecdd.h"
#include "bcryptp.h"

#include <debug.h>

/* DEFINES ********************************************************************/

/* BCRYPT_PROV_DISPATCH asks for handles usable at dispatch level; all of ours are */
#define KSEC_PROV_DISPATCH      0x00000001
#define KSEC_OPEN_FLAGS         (BCRYPT_ALG_HANDLE_HMAC_FLAG | KSEC_PROV_DISPATCH)
#define KSEC_RANDOM_FLAGS       (BCRYPT_RNG_USE_ENTROPY_IN_BUFFER | BCRYPT_USE_SYSTEM_PREFERRED_RNG)

/* TYPES **********************************************************************/

typedef struct _KSEC_HASH
{
    ULONG Magic;
    PKSEC_PROVIDER Provider;
    PVOID Allocation;
    BOOLEAN Reusable;
    BOOLEAN Finished;
    SYMCRYPT_HASH_STATE HashState;
    SYMCRYPT_MAC_STATE MacState;
    SYMCRYPT_MAC_EXPANDED_KEY MacKey;
} KSEC_HASH, *PKSEC_HASH;

/* A hash object lives at the first suitably aligned address of its buffer */
#define KSEC_HASH_OBJECT_LENGTH (sizeof(KSEC_HASH) + SYMCRYPT_ALIGN_VALUE)

/* GLOBALS ********************************************************************/

static const KSEC_ALGORITHM KsecAlgorithms[] =
{
    { BCRYPT_RNG_ALGORITHM, KsecClassRng },
    { BCRYPT_MD5_ALGORITHM, KsecClassHash, &SymCryptMd5Algorithm, &SymCryptHmacMd5Algorithm },
    { BCRYPT_SHA1_ALGORITHM, KsecClassHash, &SymCryptSha1Algorithm, &SymCryptHmacSha1Algorithm },
    { BCRYPT_SHA256_ALGORITHM, KsecClassHash, &SymCryptSha256Algorithm, &SymCryptHmacSha256Algorithm },
    { BCRYPT_SHA384_ALGORITHM, KsecClassHash, &SymCryptSha384Algorithm, &SymCryptHmacSha384Algorithm },
    { BCRYPT_SHA512_ALGORITHM, KsecClassHash, &SymCryptSha512Algorithm, &SymCryptHmacSha512Algorithm },
    { BCRYPT_AES_ALGORITHM, KsecClassCipher },
    { BCRYPT_RSA_ALGORITHM, KsecClassRsa },
    { BCRYPT_ECDSA_P256_ALGORITHM, KsecClassEcdsa, NULL, NULL, &SymCryptEcurveParamsNistP256, 256,
      BCRYPT_ECDSA_PUBLIC_P256_MAGIC, BCRYPT_ECDSA_PRIVATE_P256_MAGIC },
    { BCRYPT_ECDSA_P384_ALGORITHM, KsecClassEcdsa, NULL, NULL, &SymCryptEcurveParamsNistP384, 384,
      BCRYPT_ECDSA_PUBLIC_P384_MAGIC, BCRYPT_ECDSA_PRIVATE_P384_MAGIC },
    { BCRYPT_ECDSA_P521_ALGORITHM, KsecClassEcdsa, NULL, NULL, &SymCryptEcurveParamsNistP521, 521,
      BCRYPT_ECDSA_PUBLIC_P521_MAGIC, BCRYPT_ECDSA_PRIVATE_P521_MAGIC },
};

static LONG KsecCngState;
static KSPIN_LOCK KsecRngLock;
static SYMCRYPT_RNG_AES_STATE KsecRngState;

/* RANDOM NUMBERS *************************************************************/

/*
 * Seeds the DRBG from the driver's entropy sources folded through SHA-512,
 * plus RdSeed or RdRand output when the processor has them. The process and
 * system information part can only be read at passive level.
 */
static
VOID
KsecCollectSeed(
    _Out_writes_(SYMCRYPT_SHA512_RESULT_SIZE * 2) PBYTE Seed)
{
    SYMCRYPT_SHA512_STATE State;
    PKSEC_ENTROPY_DATA Entropy;
    LARGE_INTEGER Counter, Frequency, Time;
    BYTE Scratch[64];

    SymCryptSha512Init(&State);

    Counter = KeQueryPerformanceCounter(&Frequency);
    KeQuerySystemTime(&Time);
    SymCryptSha512Append(&State, (PCBYTE)&Counter, sizeof(Counter));
    SymCryptSha512Append(&State, (PCBYTE)&Time, sizeof(Time));

    if (KeGetCurrentIrql() == PASSIVE_LEVEL)
    {
        Entropy = ExAllocatePoolZero(NonPagedPool, sizeof(*Entropy), TAG_KSEC_BCRYPT);
        if (Entropy != NULL)
        {
            KsecGatherEntropyData(Entropy);
            SymCryptSha512Append(&State, (PCBYTE)Entropy, sizeof(*Entropy));
            RtlSecureZeroMemory(Entropy, sizeof(*Entropy));
            ExFreePoolWithTag(Entropy, TAG_KSEC_BCRYPT);
        }
    }

    if (NT_SUCCESS(KsecGenRandom(Scratch, sizeof(Scratch))))
        SymCryptSha512Append(&State, Scratch, sizeof(Scratch));

    SymCryptSha512Result(&State, Seed);

#if SYMCRYPT_CPU_X86 | SYMCRYPT_CPU_AMD64
    if (SymCryptRdseedStatus() == SYMCRYPT_NO_ERROR &&
        SymCryptRdseedGetBytes(&Seed[SYMCRYPT_SHA512_RESULT_SIZE], SYMCRYPT_SHA512_RESULT_SIZE) == SYMCRYPT_NO_ERROR)
    {
        RtlSecureZeroMemory(Scratch, sizeof(Scratch));
        return;
    }

    if (SymCryptRdrandStatus() == SYMCRYPT_NO_ERROR &&
        SymCryptRdrandGetBytes(Scratch, sizeof(Scratch), &Seed[SYMCRYPT_SHA512_RESULT_SIZE]) == SYMCRYPT_NO_ERROR)
    {
        RtlSecureZeroMemory(Scratch, sizeof(Scratch));
        return;
    }
#endif

    /* No hardware source; the second half repeats the first through another hash */
    SymCryptSha512(Seed, SYMCRYPT_SHA512_RESULT_SIZE, &Seed[SYMCRYPT_SHA512_RESULT_SIZE]);
    RtlSecureZeroMemory(Scratch, sizeof(Scratch));
}

/**
 * @brief
 * Brings up SymCrypt and the random number generator, once.
 */
VOID
NTAPI
KsecInitializeCng(VOID)
{
    BYTE Seed[SYMCRYPT_SHA512_RESULT_SIZE * 2];

    if (KsecCngState == 2)
        return;

    if (InterlockedCompareExchange(&KsecCngState, 1, 0) != 0)
    {
        while (KsecCngState != 2)
            YieldProcessor();
        return;
    }

    SymCryptInit();
    KeInitializeSpinLock(&KsecRngLock);

    KsecCollectSeed(Seed);
    SymCryptRngAesInstantiate(&KsecRngState, Seed, sizeof(Seed));
    RtlSecureZeroMemory(Seed, sizeof(Seed));

    InterlockedExchange(&KsecCngState, 2);
}

/**
 * @brief
 * Fills a buffer from the AES-CTR DRBG.
 *
 * @param[out] Buffer
 * Receives the random bytes.
 *
 * @param[in] Length
 * Size of @p Buffer, in bytes.
 *
 * @return
 * STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
KsecGenerateRandom(
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ SIZE_T Length)
{
    KIRQL OldIrql;

    KsecInitializeCng();

    KeAcquireSpinLock(&KsecRngLock, &OldIrql);
    SymCryptRngAesGenerate(&KsecRngState, Buffer, Length);
    KeReleaseSpinLock(&KsecRngLock, OldIrql);

    return STATUS_SUCCESS;
}

/* HELPERS ********************************************************************/

PKSEC_PROVIDER
KsecReferenceProvider(
    _In_opt_ BCRYPT_HANDLE Handle)
{
    PKSEC_PROVIDER Provider = (PKSEC_PROVIDER)Handle;

    if (Provider == NULL || Provider->Magic != KSEC_PROVIDER_MAGIC)
        return NULL;

    return Provider;
}

VOID
KsecReleaseProvider(
    _In_ PKSEC_PROVIDER Provider)
{
    if (InterlockedDecrement(&Provider->References) != 0)
        return;

    if (Provider->Curve != NULL)
        SymCryptEcurveFree(Provider->Curve);

    Provider->Magic = 0;
    ExFreePoolWithTag(Provider, TAG_KSEC_BCRYPT);
}

static
PKSEC_HASH
KsecReferenceHash(
    _In_opt_ BCRYPT_HANDLE Handle)
{
    PKSEC_HASH Hash = (PKSEC_HASH)Handle;

    if (Hash == NULL || Hash->Magic != KSEC_HASH_MAGIC)
        return NULL;

    return Hash;
}

static
SIZE_T
KsecDigestLength(
    _In_ PKSEC_PROVIDER Provider)
{
    if (Provider->Hmac)
        return (*Provider->Algorithm->Hmac)->resultSize;

    return SymCryptHashResultSize(*Provider->Algorithm->Hash);
}

/* Puts a hash object back to where it stood right after creation */
static
VOID
KsecHashStart(
    _Inout_ PKSEC_HASH Hash)
{
    if (Hash->Provider->Hmac)
        (*Hash->Provider->Algorithm->Hmac)->initFunc(&Hash->MacState, &Hash->MacKey);
    else
        SymCryptHashInit(*Hash->Provider->Algorithm->Hash, &Hash->HashState);

    Hash->Finished = FALSE;
}

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Opens an algorithm provider.
 *
 * @param[out] Algorithm
 * Receives the handle of the provider.
 *
 * @param[in] AlgorithmId
 * Names the algorithm to open: RNG, MD5, SHA1, SHA256, SHA384, SHA512, AES,
 * RSA, ECDSA_P256, ECDSA_P384 or ECDSA_P521.
 *
 * @param[in] Implementation
 * Optionally names the implementation to open the algorithm from.
 *
 * @param[in] Flags
 * BCRYPT_ALG_HANDLE_HMAC_FLAG for a keyed hash, BCRYPT_PROV_DISPATCH.
 *
 * @return
 * STATUS_SUCCESS, STATUS_NOT_FOUND for an algorithm this provider does not
 * have, STATUS_INVALID_PARAMETER or STATUS_NO_MEMORY.
 */
NTSTATUS
WINAPI
BCryptOpenAlgorithmProvider(
    _Out_ BCRYPT_ALG_HANDLE *Algorithm,
    _In_ LPCWSTR AlgorithmId,
    _In_opt_ LPCWSTR Implementation,
    _In_ ULONG Flags)
{
    const KSEC_ALGORITHM *Found = NULL;
    PKSEC_PROVIDER Provider;
    ULONG Index;

    if (Algorithm == NULL || AlgorithmId == NULL || (Flags & ~KSEC_OPEN_FLAGS))
        return STATUS_INVALID_PARAMETER;

    *Algorithm = NULL;

    if (Implementation != NULL && _wcsicmp(Implementation, MS_PRIMITIVE_PROVIDER) != 0)
        return STATUS_NOT_FOUND;

    for (Index = 0; Index < ARRAYSIZE(KsecAlgorithms); Index++)
    {
        if (_wcsicmp(AlgorithmId, KsecAlgorithms[Index].Name) == 0)
        {
            Found = &KsecAlgorithms[Index];
            break;
        }
    }

    if (Found == NULL || ((Flags & BCRYPT_ALG_HANDLE_HMAC_FLAG) && Found->Hmac == NULL))
    {
        DPRINT1("Algorithm %S is not available\n", AlgorithmId);
        return STATUS_NOT_FOUND;
    }

    KsecInitializeCng();

    Provider = ExAllocatePoolZero(NonPagedPool, sizeof(*Provider), TAG_KSEC_BCRYPT);
    if (Provider == NULL)
        return STATUS_NO_MEMORY;

    if (Found->CurveParams != NULL)
    {
        Provider->Curve = SymCryptEcurveAllocate(*Found->CurveParams, 0);
        if (Provider->Curve == NULL)
        {
            ExFreePoolWithTag(Provider, TAG_KSEC_BCRYPT);
            return STATUS_NO_MEMORY;
        }
    }

    Provider->Magic = KSEC_PROVIDER_MAGIC;
    Provider->References = 1;
    Provider->Algorithm = Found;
    Provider->Hmac = (Flags & BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0;
    Provider->ChainingMode = KsecChainingCbc;

    *Algorithm = Provider;
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
 * Must be zero.
 *
 * @return
 * STATUS_SUCCESS or STATUS_INVALID_HANDLE.
 *
 * @remarks
 * Keys made from the provider keep it alive until they are destroyed.
 */
NTSTATUS
WINAPI
BCryptCloseAlgorithmProvider(
    _In_ BCRYPT_ALG_HANDLE Algorithm,
    _In_ ULONG Flags)
{
    PKSEC_PROVIDER Provider = KsecReferenceProvider(Algorithm);

    UNREFERENCED_PARAMETER(Flags);

    if (Provider == NULL)
        return STATUS_INVALID_HANDLE;

    KsecReleaseProvider(Provider);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Fills a buffer with random bytes.
 *
 * @param[in] Algorithm
 * An RNG provider, or NULL with BCRYPT_USE_SYSTEM_PREFERRED_RNG.
 *
 * @param[out] Buffer
 * Receives the random bytes.
 *
 * @param[in] Length
 * Size of @p Buffer, in bytes.
 *
 * @param[in] Flags
 * BCRYPT_USE_SYSTEM_PREFERRED_RNG. BCRYPT_RNG_USE_ENTROPY_IN_BUFFER is
 * accepted and ignored, as on Windows 8 and later.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE or STATUS_INVALID_PARAMETER.
 */
NTSTATUS
WINAPI
BCryptGenRandom(
    _In_opt_ BCRYPT_ALG_HANDLE Algorithm,
    _Out_writes_bytes_all_(Length) PUCHAR Buffer,
    _In_ ULONG Length,
    _In_ ULONG Flags)
{
    PKSEC_PROVIDER Provider;

    if (Flags & ~KSEC_RANDOM_FLAGS)
        return STATUS_INVALID_PARAMETER;

    if (Flags & BCRYPT_USE_SYSTEM_PREFERRED_RNG)
    {
        if (Algorithm != NULL)
            return STATUS_INVALID_PARAMETER;
    }
    else
    {
        Provider = KsecReferenceProvider(Algorithm);
        if (Provider == NULL || Provider->Algorithm->Class != KsecClassRng)
            return STATUS_INVALID_HANDLE;
    }

    if (Buffer == NULL)
        return STATUS_INVALID_PARAMETER;
    if (Length == 0)
        return STATUS_SUCCESS;

    return KsecGenerateRandom(Buffer, Length);
}

/**
 * @brief
 * Reads a property of a provider, a hash object or a key.
 *
 * @param[in] Object
 * The object to read from.
 *
 * @param[in] Property
 * The property to read.
 *
 * @param[out] Output
 * Receives the value of the property. With NULL only the size is returned.
 *
 * @param[in] OutputLength
 * Size of @p Output, in bytes.
 *
 * @param[out] ResultLength
 * Receives the size of the value, in bytes.
 *
 * @param[in] Flags
 * Must be zero.
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
    _Out_writes_bytes_opt_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    PKSEC_PROVIDER Provider = KsecReferenceProvider(Object);
    PKSEC_HASH Hash = KsecReferenceHash(Object);
    PKSEC_KEY Key = KsecReferenceKey(Object);
    BCRYPT_KEY_LENGTHS_STRUCT Scratch;
    const VOID *Value;
    ULONG Number, Size;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Flags);

    if (Hash != NULL)
        Provider = Hash->Provider;
    else if (Key != NULL)
        Provider = Key->Provider;
    if (Provider == NULL)
        return STATUS_INVALID_HANDLE;
    if (Property == NULL || ResultLength == NULL)
        return STATUS_INVALID_PARAMETER;

    if (_wcsicmp(Property, BCRYPT_ALGORITHM_NAME) == 0)
    {
        Value = Provider->Algorithm->Name;
        Size = (ULONG)(wcslen(Provider->Algorithm->Name) + 1) * sizeof(WCHAR);
    }
    else if (Provider->Algorithm->Class == KsecClassHash)
    {
        if (_wcsicmp(Property, BCRYPT_OBJECT_LENGTH) == 0)
            Number = KSEC_HASH_OBJECT_LENGTH;
        else if (_wcsicmp(Property, BCRYPT_HASH_LENGTH) == 0)
            Number = (ULONG)KsecDigestLength(Provider);
        else if (_wcsicmp(Property, BCRYPT_HASH_BLOCK_LENGTH) == 0)
            Number = (ULONG)SymCryptHashInputBlockSize(*Provider->Algorithm->Hash);
        else
            return STATUS_NOT_SUPPORTED;

        Value = &Number;
        Size = sizeof(Number);
    }
    else if (Provider->Algorithm->Class == KsecClassRng)
    {
        return STATUS_NOT_SUPPORTED;
    }
    else
    {
        Status = KsecQueryKeyProperty(Provider, Key, Property, &Value, &Size, &Scratch);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    *ResultLength = Size;
    if (Output == NULL)
        return STATUS_SUCCESS;
    if (OutputLength < Size)
        return STATUS_BUFFER_TOO_SMALL;

    RtlCopyMemory(Output, Value, Size);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Creates a hash object.
 *
 * @param[in] Algorithm
 * The hash provider the object is created from.
 *
 * @param[out] Hash
 * Receives the handle of the hash object.
 *
 * @param[in] Object
 * Optional caller owned buffer that backs the hash object, at least
 * ObjectLength bytes. Without one the object is allocated.
 *
 * @param[in] ObjectLength
 * Size of @p Object, in bytes.
 *
 * @param[in] Secret
 * The key of an HMAC provider; other providers ignore it.
 *
 * @param[in] SecretLength
 * Size of @p Secret, in bytes.
 *
 * @param[in] Flags
 * BCRYPT_HASH_REUSABLE_FLAG to let the object start over after a finish.
 *
 * @return
 * STATUS_SUCCESS, STATUS_BUFFER_TOO_SMALL, STATUS_INVALID_HANDLE,
 * STATUS_INVALID_PARAMETER or STATUS_NO_MEMORY.
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
    PKSEC_PROVIDER Provider = KsecReferenceProvider(Algorithm);
    PVOID Allocation = NULL;
    PKSEC_HASH NewHash;

    if (Provider == NULL || Provider->Algorithm->Class != KsecClassHash)
        return STATUS_INVALID_HANDLE;
    if (Hash == NULL || (Flags & ~BCRYPT_HASH_REUSABLE_FLAG))
        return STATUS_INVALID_PARAMETER;
    if (Provider->Hmac && Secret == NULL && SecretLength != 0)
        return STATUS_INVALID_PARAMETER;

    if (Object != NULL)
    {
        if (ObjectLength < KSEC_HASH_OBJECT_LENGTH)
            return STATUS_BUFFER_TOO_SMALL;
    }
    else
    {
        Allocation = ExAllocatePoolWithTag(NonPagedPool, KSEC_HASH_OBJECT_LENGTH, TAG_KSEC_BCRYPT);
        if (Allocation == NULL)
            return STATUS_NO_MEMORY;
        Object = Allocation;
    }

    NewHash = (PKSEC_HASH)ALIGN_UP_POINTER_BY(Object, SYMCRYPT_ALIGN_VALUE);
    RtlZeroMemory(NewHash, sizeof(*NewHash));
    NewHash->Magic = KSEC_HASH_MAGIC;
    NewHash->Provider = Provider;
    NewHash->Allocation = Allocation;
    NewHash->Reusable = (Flags & BCRYPT_HASH_REUSABLE_FLAG) != 0;

    if (Provider->Hmac &&
        (*Provider->Algorithm->Hmac)->expandKeyFunc(&NewHash->MacKey, Secret, SecretLength) != SYMCRYPT_NO_ERROR)
    {
        NewHash->Magic = 0;
        if (Allocation != NULL)
            ExFreePoolWithTag(Allocation, TAG_KSEC_BCRYPT);
        return STATUS_INVALID_PARAMETER;
    }

    InterlockedIncrement(&Provider->References);
    KsecHashStart(NewHash);

    *Hash = NewHash;
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
 * STATUS_SUCCESS or STATUS_INVALID_HANDLE.
 */
NTSTATUS
WINAPI
BCryptDestroyHash(
    _In_ BCRYPT_HASH_HANDLE Hash)
{
    PKSEC_HASH Object = KsecReferenceHash(Hash);
    PKSEC_PROVIDER Provider;
    PVOID Allocation;

    if (Object == NULL)
        return STATUS_INVALID_HANDLE;

    Provider = Object->Provider;
    Allocation = Object->Allocation;
    SymCryptWipe(Object, sizeof(*Object));
    if (Allocation != NULL)
        ExFreePoolWithTag(Allocation, TAG_KSEC_BCRYPT);

    KsecReleaseProvider(Provider);
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
 * Must be zero.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE or STATUS_INVALID_PARAMETER.
 */
NTSTATUS
WINAPI
BCryptHashData(
    _In_ BCRYPT_HASH_HANDLE Hash,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_ ULONG Flags)
{
    PKSEC_HASH Object = KsecReferenceHash(Hash);

    UNREFERENCED_PARAMETER(Flags);

    if (Object == NULL || Object->Finished)
        return STATUS_INVALID_HANDLE;
    if (Input == NULL && InputLength != 0)
        return STATUS_INVALID_PARAMETER;

    if (Object->Provider->Hmac)
        (*Object->Provider->Algorithm->Hmac)->appendFunc(&Object->MacState, Input, InputLength);
    else
        SymCryptHashAppend(*Object->Provider->Algorithm->Hash, &Object->HashState, Input, InputLength);

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
 * Size of @p Output, which must be the digest length of the algorithm.
 *
 * @param[in] Flags
 * Must be zero.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE or STATUS_INVALID_PARAMETER.
 *
 * @remarks
 * A reusable object starts over, anything else only accepts BCryptDestroyHash()
 * afterwards.
 */
NTSTATUS
WINAPI
BCryptFinishHash(
    _In_ BCRYPT_HASH_HANDLE Hash,
    _Out_writes_bytes_all_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _In_ ULONG Flags)
{
    PKSEC_HASH Object = KsecReferenceHash(Hash);

    UNREFERENCED_PARAMETER(Flags);

    if (Object == NULL || Object->Finished)
        return STATUS_INVALID_HANDLE;
    if (Output == NULL || OutputLength != KsecDigestLength(Object->Provider))
        return STATUS_INVALID_PARAMETER;

    if (Object->Provider->Hmac)
        (*Object->Provider->Algorithm->Hmac)->resultFunc(&Object->MacState, Output);
    else
        SymCryptHashResult(*Object->Provider->Algorithm->Hash, &Object->HashState, Output, OutputLength);

    if (Object->Reusable)
        KsecHashStart(Object);
    else
        Object->Finished = TRUE;

    return STATUS_SUCCESS;
}

/* EOF */
