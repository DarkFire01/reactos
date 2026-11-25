/*
 * PROJECT:     ReactOS Kernel Cryptography Next Generation
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Driver entry, the system random number generator and pool helpers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "cngp.h"

#include <debug.h>

/* GLOBALS ********************************************************************/

#define CNG_RANDOM_FLAGS        (BCRYPT_RNG_USE_ENTROPY_IN_BUFFER | BCRYPT_USE_SYSTEM_PREFERRED_RNG)

typedef struct _CNG_SEED_SAMPLE
{
    LARGE_INTEGER PerformanceCounter;
    LARGE_INTEGER PerformanceFrequency;
    LARGE_INTEGER SystemTime;
    LARGE_INTEGER TickCount;
    ULONGLONG InterruptTime;
    ULONGLONG TimeStamp;
    HANDLE ProcessId;
    HANDLE ThreadId;
    PVOID StackAddress;
} CNG_SEED_SAMPLE, *PCNG_SEED_SAMPLE;

static LONG CngState;
static KSPIN_LOCK CngRngLock;
static SYMCRYPT_RNG_AES_STATE CngRngState;

/* SEEDING ********************************************************************/

static
VOID
CngSampleCounters(
    _Out_ PCNG_SEED_SAMPLE Sample)
{
    RtlZeroMemory(Sample, sizeof(*Sample));

    Sample->PerformanceCounter = KeQueryPerformanceCounter(&Sample->PerformanceFrequency);
    KeQuerySystemTime(&Sample->SystemTime);
    KeQueryTickCount(&Sample->TickCount);
    Sample->InterruptTime = KeQueryInterruptTime();
    Sample->ProcessId = PsGetCurrentProcessId();
    Sample->ThreadId = PsGetCurrentThreadId();
    Sample->StackAddress = &Sample;

#if defined(_M_IX86) || defined(_M_AMD64)
    Sample->TimeStamp = __rdtsc();
#endif
}

/* Folds a system information class into the seed hash, skipping ones that fail */
static
VOID
CngHashSystemInformation(
    _Inout_ PSYMCRYPT_SHA512_STATE State,
    _In_ SYSTEM_INFORMATION_CLASS Class,
    _Inout_updates_bytes_(Size) PVOID Scratch,
    _In_ ULONG Size)
{
    ULONG Returned = 0;

    if (NT_SUCCESS(ZwQuerySystemInformation(Class, Scratch, Size, &Returned)))
        SymCryptSha512Append(State, Scratch, min(Returned, Size));
}

/*
 * The first half of the seed is SHA-512 over timers and system statistics,
 * the second half comes from RdSeed or RdRand when the processor has them.
 * System statistics are only read at passive level.
 */
static
VOID
CngCollectSeed(
    _Out_writes_(SYMCRYPT_SHA512_RESULT_SIZE * 2) PBYTE Seed)
{
    SYMCRYPT_SHA512_STATE State;
    CNG_SEED_SAMPLE Sample;
    PVOID Scratch;

    SymCryptSha512Init(&State);

    CngSampleCounters(&Sample);
    SymCryptSha512Append(&State, (PCBYTE)&Sample, sizeof(Sample));

    if (KeGetCurrentIrql() == PASSIVE_LEVEL)
    {
        Scratch = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, TAG_CNG);
        if (Scratch != NULL)
        {
            CngHashSystemInformation(&State, SystemProcessorPerformanceInformation, Scratch, PAGE_SIZE);
            CngHashSystemInformation(&State, SystemPerformanceInformation, Scratch, PAGE_SIZE);
            CngHashSystemInformation(&State, SystemExceptionInformation, Scratch, PAGE_SIZE);
            CngHashSystemInformation(&State, SystemInterruptInformation, Scratch, PAGE_SIZE);
            CngHashSystemInformation(&State, SystemProcessInformation, Scratch, PAGE_SIZE);
            RtlSecureZeroMemory(Scratch, PAGE_SIZE);
            ExFreePoolWithTag(Scratch, TAG_CNG);
        }
    }

    CngSampleCounters(&Sample);
    SymCryptSha512Append(&State, (PCBYTE)&Sample, sizeof(Sample));
    SymCryptSha512Result(&State, Seed);

#if SYMCRYPT_CPU_X86 | SYMCRYPT_CPU_AMD64
    {
        BYTE Scratch64[64];

        if (SymCryptRdseedStatus() == SYMCRYPT_NO_ERROR &&
            SymCryptRdseedGetBytes(&Seed[SYMCRYPT_SHA512_RESULT_SIZE], SYMCRYPT_SHA512_RESULT_SIZE) == SYMCRYPT_NO_ERROR)
        {
            return;
        }

        if (SymCryptRdrandStatus() == SYMCRYPT_NO_ERROR &&
            SymCryptRdrandGetBytes(Scratch64, sizeof(Scratch64), &Seed[SYMCRYPT_SHA512_RESULT_SIZE]) == SYMCRYPT_NO_ERROR)
        {
            RtlSecureZeroMemory(Scratch64, sizeof(Scratch64));
            return;
        }
    }
#endif

    /* No hardware source, so the second half is another hash of the first */
    SymCryptSha512(Seed, SYMCRYPT_SHA512_RESULT_SIZE, &Seed[SYMCRYPT_SHA512_RESULT_SIZE]);
}

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Brings up SymCrypt and the random number generator, once.
 */
VOID
NTAPI
CngInitialize(VOID)
{
    BYTE Seed[SYMCRYPT_SHA512_RESULT_SIZE * 2];

    if (CngState == 2)
        return;

    if (InterlockedCompareExchange(&CngState, 1, 0) != 0)
    {
        while (CngState != 2)
            YieldProcessor();
        return;
    }

    SymCryptInit();
    KeInitializeSpinLock(&CngRngLock);

    CngCollectSeed(Seed);
    SymCryptRngAesInstantiate(&CngRngState, Seed, sizeof(Seed));
    RtlSecureZeroMemory(Seed, sizeof(Seed));

    InterlockedExchange(&CngState, 2);
}

/**
 * @brief
 * Fills a buffer from the AES-CTR DRBG. Usable up to dispatch level.
 */
NTSTATUS
NTAPI
CngGenerateRandom(
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ SIZE_T Length)
{
    KIRQL OldIrql;

    CngInitialize();

    KeAcquireSpinLock(&CngRngLock, &OldIrql);
    SymCryptRngAesGenerate(&CngRngState, Buffer, Length);
    KeReleaseSpinLock(&CngRngLock, OldIrql);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reseeds the DRBG with entropy a hardware source provided.
 */
VOID
NTAPI
CngMixEntropy(
    _In_reads_bytes_(Length) const VOID *Data,
    _In_ SIZE_T Length)
{
    BYTE Digest[SYMCRYPT_SHA512_RESULT_SIZE];
    KIRQL OldIrql;

    CngInitialize();

    SymCryptSha512(Data, Length, Digest);

    KeAcquireSpinLock(&CngRngLock, &OldIrql);
    SymCryptRngAesReseed(&CngRngState, Digest, sizeof(Digest));
    KeReleaseSpinLock(&CngRngLock, OldIrql);

    RtlSecureZeroMemory(Digest, sizeof(Digest));
}

/**
 * @brief
 * Allocates nonpaged memory for CNG objects, so they work at dispatch level.
 */
PVOID
NTAPI
CngAllocate(
    _In_ SIZE_T Size)
{
    return ExAllocatePoolWithTag(NonPagedPool, Size, TAG_CNG);
}

VOID
NTAPI
CngFree(
    _In_opt_ PVOID Block)
{
    if (Block != NULL)
        ExFreePoolWithTag(Block, TAG_CNG);
}

/**
 * @brief
 * Fills a buffer with random bytes for kernel components, at any IRQL up to dispatch.
 *
 * @return
 * TRUE on success, FALSE for a NULL buffer.
 */
BOOLEAN
NTAPI
SystemPrng(
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ SIZE_T Length)
{
    if (Buffer == NULL && Length != 0)
        return FALSE;
    if (Length == 0)
        return TRUE;

    return NT_SUCCESS(CngGenerateRandom(Buffer, Length));
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
 * STATUS_SUCCESS, STATUS_INVALID_HANDLE, STATUS_INVALID_PARAMETER, or
 * STATUS_NOT_SUPPORTED for the system preferred generator above passive level.
 */
NTSTATUS
WINAPI
BCryptGenRandom(
    _In_opt_ BCRYPT_ALG_HANDLE Algorithm,
    _Out_writes_bytes_all_(Length) PUCHAR Buffer,
    _In_ ULONG Length,
    _In_ ULONG Flags)
{
    PCNG_PROVIDER Provider;

    if (Flags & BCRYPT_USE_SYSTEM_PREFERRED_RNG)
    {
        if (KeGetCurrentIrql() != PASSIVE_LEVEL)
            return STATUS_NOT_SUPPORTED;
        if (Algorithm != NULL)
            return STATUS_INVALID_PARAMETER;
    }
    else
    {
        Provider = CngLookupProvider(Algorithm);
        if (Provider == NULL || Provider->Algorithm->Class != CngClassRng)
            return STATUS_INVALID_HANDLE;
    }

    if (Buffer == NULL || (Flags & ~CNG_RANDOM_FLAGS))
        return STATUS_INVALID_PARAMETER;
    if (Length == 0)
        return STATUS_SUCCESS;

    return CngGenerateRandom(Buffer, Length);
}

/**
 * @brief
 * Reports whether FIPS mode is enforced. ReactOS does not enforce it.
 */
NTSTATUS
WINAPI
BCryptGetFipsAlgorithmMode(
    _Out_ BOOLEAN *Enabled)
{
    if (Enabled == NULL)
        return STATUS_INVALID_PARAMETER;

    *Enabled = FALSE;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Kernel side variant of BCryptGetFipsAlgorithmMode().
 */
BOOLEAN
NTAPI
CngGetFipsAlgorithmMode(VOID)
{
    return FALSE;
}

/**
 * @brief
 * Runs when cng.sys is loaded as an import of another module.
 */
NTSTATUS
NTAPI
DllInitialize(
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    CngInitialize();
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

    CngInitialize();
    return STATUS_SUCCESS;
}

/* EOF */
