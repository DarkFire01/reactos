/*
 * PROJECT:     ReactOS Kernel Security Support Provider Interface Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Random number generation and entropy gathering
 * COPYRIGHT:   Copyright 2014-2017 Timo Kreuzer <timo.kreuzer@reactos.org>
 *              Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ksecdd.h"

#include <intrin.h>
#include <bcryptk.h>
#include <symcrypt.h>

#define NDEBUG
#include <debug.h>


/* GLOBALS ********************************************************************/

/* How much output goes by before fresh entropy is mixed in */
#define KSEC_RESEED_INTERVAL (1024 * 1024)

static SYMCRYPT_RNG_AES_STATE KsecRngState;
static KSPIN_LOCK KsecRngLock;
static SIZE_T KsecRngSinceReseed;
static BOOLEAN KsecRngReady;


/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Reads entropy straight out of the processor.
 *
 * @param[out] Result
 * Receives SYMCRYPT_SHA512_RESULT_SIZE bytes of entropy.
 *
 * @return
 * FALSE when the processor offers no entropy of its own, leaving @p Result
 * untouched.
 *
 * @remarks
 * RDSEED is asked first, being the seeded source, and RDRAND stands in where
 * there is no RDSEED. Both report failure when the hardware cannot keep up, so
 * a read is given a few goes before it is given up on.
 */
static
BOOLEAN
KsecpHardwareEntropy(
    _Out_writes_bytes_all_(SYMCRYPT_SHA512_RESULT_SIZE) PUCHAR Result)
{
#if defined(_M_IX86) || defined(_M_AMD64)
    int Registers[4];
    ULONG Index;
    ULONG Attempt;
    unsigned int Value;
    BOOLEAN Seed = FALSE;
    BOOLEAN Rand;

    __cpuid(Registers, 0);
    if (Registers[0] >= 7)
    {
        __cpuidex(Registers, 7, 0);
        Seed = (Registers[1] & (1 << 18)) != 0;
    }

    __cpuid(Registers, 1);
    Rand = (Registers[2] & (1 << 30)) != 0;

    if (!Seed && !Rand)
        return FALSE;

    for (Index = 0; Index < SYMCRYPT_SHA512_RESULT_SIZE; Index += sizeof(Value))
    {
        for (Attempt = 0; Attempt < 16; Attempt++)
        {
            if (Seed)
            {
                if (_rdseed32_step(&Value) != 0)
                    break;
            }
            else
            {
                if (_rdrand32_step(&Value) != 0)
                    break;
            }
        }

        if (Attempt == 16)
            return FALSE;

        RtlCopyMemory(&Result[Index], &Value, sizeof(Value));
    }

    return TRUE;
#else
    UNREFERENCED_PARAMETER(Result);

    return FALSE;
#endif
}

/**
 * @brief
 * Puts seed material together for the generator.
 *
 * @remarks
 * What the system can say about itself is hashed together with whatever the
 * processor will hand over, so that the seed is no weaker than the better of
 * the two. Reading the system state needs a thread to run on, which is why
 * this only happens while the driver is starting.
 */
static
VOID
KsecpGatherSeed(
    _Out_writes_bytes_all_(SYMCRYPT_SHA512_RESULT_SIZE) PUCHAR Seed)
{
    PKSEC_ENTROPY_DATA EntropyData;
    SYMCRYPT_SHA512_STATE Sha512;
    UCHAR Hardware[SYMCRYPT_SHA512_RESULT_SIZE];
    LARGE_INTEGER Counter;

    SymCryptSha512Init(&Sha512);

    EntropyData = ExAllocatePoolZero(NonPagedPool, sizeof(*EntropyData), 'dSeK');
    if (EntropyData != NULL)
    {
        KsecGatherEntropyData(EntropyData);
        SymCryptSha512Append(&Sha512, (PCBYTE)EntropyData, sizeof(*EntropyData));
        SymCryptWipe(EntropyData, sizeof(*EntropyData));
        ExFreePoolWithTag(EntropyData, 'dSeK');
    }

    if (KsecpHardwareEntropy(Hardware))
    {
        SymCryptSha512Append(&Sha512, Hardware, sizeof(Hardware));
        SymCryptWipeKnownSize(Hardware, sizeof(Hardware));
    }

    /* A value that does not come round again, so that no two seeds match */
    Counter = KeQueryPerformanceCounter(NULL);
    SymCryptSha512Append(&Sha512, (PCBYTE)&Counter, sizeof(Counter));

    SymCryptSha512Result(&Sha512, Seed);
}

/**
 * @brief
 * Brings the random generator up.
 */
VOID
NTAPI
KsecInitializeRandomSupport(
    VOID)
{
    UCHAR Seed[SYMCRYPT_SHA512_RESULT_SIZE];

    KeInitializeSpinLock(&KsecRngLock);

    KsecpGatherSeed(Seed);

    if (SymCryptRngAesInstantiate(&KsecRngState, Seed, sizeof(Seed)) == SYMCRYPT_NO_ERROR)
        KsecRngReady = TRUE;
    else
        DPRINT1("Failed to seed the random generator\n");

    SymCryptWipeKnownSize(Seed, sizeof(Seed));
}

/**
 * @brief
 * Fills a buffer with random bytes.
 *
 * @remarks
 * This runs at any IRQL, so the entropy for a reseed is read before the lock
 * is taken and only from the processor.
 */
NTSTATUS
NTAPI
KsecGenRandom(
    PVOID Buffer,
    SIZE_T Length)
{
    UCHAR Hardware[SYMCRYPT_SHA512_RESULT_SIZE];
    KIRQL OldIrql;
    BOOLEAN Reseed = FALSE;

    if (!KsecRngReady)
        return STATUS_UNSUCCESSFUL;

    if (KsecRngSinceReseed >= KSEC_RESEED_INTERVAL)
        Reseed = KsecpHardwareEntropy(Hardware);

    KeAcquireSpinLock(&KsecRngLock, &OldIrql);

    if (Reseed)
    {
        SymCryptRngAesReseed(&KsecRngState, Hardware, sizeof(Hardware));
        KsecRngSinceReseed = 0;
    }

    SymCryptRngAesGenerate(&KsecRngState, Buffer, Length);
    KsecRngSinceReseed += Length;

    KeReleaseSpinLock(&KsecRngLock, OldIrql);

    if (Reseed)
        SymCryptWipeKnownSize(Hardware, sizeof(Hardware));

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Fills a buffer with random bytes for a CNG caller.
 *
 * @param[in] Algorithm
 * A provider opened for BCRYPT_RNG_ALGORITHM, or NULL to ask for the
 * generator the system prefers.
 *
 * @param[in] Flags
 * BCRYPT_USE_SYSTEM_PREFERRED_RNG picks the system generator, which is the
 * only one here. BCRYPT_RNG_USE_ENTROPY_IN_BUFFER is accepted and ignored,
 * the way it has been ignored since Windows 8.
 */
NTSTATUS
WINAPI
BCryptGenRandom(
    _In_opt_ BCRYPT_ALG_HANDLE Algorithm,
    _Out_writes_bytes_all_(Length) PUCHAR Buffer,
    _In_ ULONG Length,
    _In_ ULONG Flags)
{
    PBCRYPTK_PROVIDER Provider = Algorithm;

    if (Buffer == NULL)
        return STATUS_INVALID_PARAMETER;

    if ((Flags & ~(BCRYPT_USE_SYSTEM_PREFERRED_RNG |
                   BCRYPT_RNG_USE_ENTROPY_IN_BUFFER)) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (Provider != NULL)
    {
        if (Provider->Tag != BCRYPTK_PROVIDER_TAG)
            return STATUS_INVALID_HANDLE;

        if (Provider->Algorithm->Class != BcryptkClassRandom)
            return STATUS_NOT_SUPPORTED;
    }

    return KsecGenRandom(Buffer, Length);
}

VOID
NTAPI
KsecReadMachineSpecificCounters(
    _Out_ PKSEC_MACHINE_SPECIFIC_COUNTERS MachineSpecificCounters)
{
#if defined(_M_IX86) || defined(_M_AMD64)
    /* Check if RDTSC is available */
    if (ExIsProcessorFeaturePresent(PF_RDTSC_INSTRUCTION_AVAILABLE))
    {
        /* Read the TSC value */
        MachineSpecificCounters->Tsc = __rdtsc();
    }
#if 0 // FIXME: investigate what the requirements are for these
    /* Read the CPU event counter MSRs */
    //MachineSpecificCounters->Ctr0 = __readmsr(0x12);
    //MachineSpecificCounters->Ctr1 = __readmsr(0x13);

    /* Check if this is an MMX capable CPU */
    if (ExIsProcessorFeaturePresent(PF_MMX_INSTRUCTIONS_AVAILABLE))
    {
        /* Read the CPU performance counters 0 and 1 */
        MachineSpecificCounters->Pmc0 = __readpmc(0);
        MachineSpecificCounters->Pmc1 = __readpmc(1);
    }
#endif
#elif defined(_M_ARM)
    /* Read the Cycle Counter Register */
    MachineSpecificCounters->Ccr = _MoveFromCoprocessor(CP15_PMCCNTR);
#else
    #error Implement me!
#endif
}

/*!
 *  \see http://blogs.msdn.com/b/michael_howard/archive/2005/01/14/353379.aspx (DEAD_LINK)
 */
NTSTATUS
NTAPI
KsecGatherEntropyData(
    PKSEC_ENTROPY_DATA EntropyData)
{
    MD4_CTX Md4Context;
    PTEB Teb;
    PPEB Peb;
    PWSTR String;
    ULONG ReturnLength;
    NTSTATUS Status;

    /* Query some generic values */
    EntropyData->CurrentProcessId = PsGetCurrentProcessId();
    EntropyData->CurrentThreadId = PsGetCurrentThreadId();
    KeQueryTickCount(&EntropyData->TickCount);
    KeQuerySystemTime(&EntropyData->SystemTime);
    EntropyData->PerformanceCounter = KeQueryPerformanceCounter(
                                            &EntropyData->PerformanceFrequency);

    /* Check if we have a TEB/PEB for the process environment */
    Teb = PsGetCurrentThread()->Tcb.Teb;
    if (Teb != NULL)
    {
        Peb = Teb->ProcessEnvironmentBlock;

        /* Initialize the MD4 context */
        MD4Init(&Md4Context);
        _SEH2_TRY
        {
            /* Get the end of the environment */
            String = Peb->ProcessParameters->Environment;
            while (*String)
            {
                String += wcslen(String) + 1;
            }

            /* Update the MD4 context from the environment data */
            MD4Update(&Md4Context,
                      (PUCHAR)Peb->ProcessParameters->Environment,
                      (ULONG)((PUCHAR)String - (PUCHAR)Peb->ProcessParameters->Environment));
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            /* Simply ignore the exception */
        }
        _SEH2_END;

        /* Finalize and copy the MD4 hash */
        MD4Final(&Md4Context);
        RtlCopyMemory(&EntropyData->EnvironmentHash, Md4Context.digest, 16);
    }

    /* Read some machine specific hardware counters */
    KsecReadMachineSpecificCounters(&EntropyData->MachineSpecificCounters);

    /* Query processor performance information */
    Status = ZwQuerySystemInformation(SystemProcessorPerformanceInformation,
                                      &EntropyData->SystemProcessorPerformanceInformation,
                                      sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION),
                                      &ReturnLength);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /* Query system performance information */
    Status = ZwQuerySystemInformation(SystemPerformanceInformation,
                                      &EntropyData->SystemPerformanceInformation,
                                      sizeof(SYSTEM_PERFORMANCE_INFORMATION),
                                      &ReturnLength);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /* Query exception information */
    Status = ZwQuerySystemInformation(SystemExceptionInformation,
                                      &EntropyData->SystemExceptionInformation,
                                      sizeof(SYSTEM_EXCEPTION_INFORMATION),
                                      &ReturnLength);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /* Query lookaside information */
    Status = ZwQuerySystemInformation(SystemLookasideInformation,
                                      &EntropyData->SystemLookasideInformation,
                                      sizeof(SYSTEM_LOOKASIDE_INFORMATION),
                                      &ReturnLength);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /* Query interrupt information */
    Status = ZwQuerySystemInformation(SystemInterruptInformation,
                                      &EntropyData->SystemInterruptInformation,
                                      sizeof(SYSTEM_INTERRUPT_INFORMATION),
                                      &ReturnLength);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /* Query process information */
    Status = ZwQuerySystemInformation(SystemProcessInformation,
                                      &EntropyData->SystemProcessInformation,
                                      sizeof(SYSTEM_PROCESS_INFORMATION),
                                      &ReturnLength);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    return STATUS_SUCCESS;
}
