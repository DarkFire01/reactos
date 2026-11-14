/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ARM64 processor support
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define ARM64_CLIDR_EL1     ARM64_SYSREG(3, 1, 0, 0, 1)
#define ARM64_CCSIDR_EL1    ARM64_SYSREG(3, 1, 0, 0, 0)
#define ARM64_CSSELR_EL1    ARM64_SYSREG(3, 2, 0, 0, 0)

/* GLOBALS *******************************************************************/

ULONG KeLargestCacheLine = 64;
ULONG KiDmaIoCoherency = 0;
volatile LONG KiTbFlushTimeStamp;

/* FUNCTIONS *****************************************************************/

VOID
KiSetProcessorType(VOID)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    ULONG64 Midr;

    /* MIDR_EL1: implementer, variant, architecture, part number, revision */
    Midr = KeArm64ReadRegister(ARM64_MIDR_EL1);
    Prcb->CpuType = (CHAR)(Midr >> 24);
    Prcb->CpuID = 1;
    Prcb->CpuStep = (USHORT)(((Midr >> 4) & 0xFFF) << 4 | (Midr & 0xF));
    Prcb->CpuVendor = (UCHAR)(Midr >> 24);
}

ULONG64
KiGetFeatureBits(VOID)
{
    /* None of the x86 feature flags apply */
    return 0;
}

VOID
NTAPI
KiGetCacheInformation(VOID)
{
    PKIPCR Pcr = (PKIPCR)KeGetPcr();
    PKPRCB Prcb = &Pcr->Prcb;
    ULONG64 Clidr, Ccsidr;
    ULONG Level, Type, LineSize, Ways, Sets;

    Clidr = KeArm64ReadRegister(ARM64_CLIDR_EL1);
    Prcb->CacheCount = 0;
    Pcr->SecondLevelCacheSize = 0;

    /* CLIDR_EL1 describes up to seven levels with three bits each */
    for (Level = 0; (Level < 7) && (Prcb->CacheCount < RTL_NUMBER_OF(Prcb->Cache)); Level++)
    {
        Type = (Clidr >> (Level * 3)) & 7;
        if (!Type)
            break;

        /* Data or unified side of the level */
        KeArm64WriteRegister(ARM64_CSSELR_EL1, Level << 1);
        __isb(_ARM64_BARRIER_SY);
        Ccsidr = KeArm64ReadRegister(ARM64_CCSIDR_EL1);

        LineSize = 1 << ((Ccsidr & 7) + 4);
        Ways = ((Ccsidr >> 3) & 0x3FF) + 1;
        Sets = ((Ccsidr >> 13) & 0x7FFF) + 1;

        Prcb->Cache[Prcb->CacheCount].Level = (UCHAR)(Level + 1);
        Prcb->Cache[Prcb->CacheCount].Associativity = (UCHAR)min(Ways, 0xFF);
        Prcb->Cache[Prcb->CacheCount].LineSize = (USHORT)LineSize;
        Prcb->Cache[Prcb->CacheCount].Size = LineSize * Ways * Sets;
        Prcb->Cache[Prcb->CacheCount].Type = (Type == 4) ? CacheUnified :
                                             (Type == 1) ? CacheInstruction : CacheData;

        if (LineSize > KeLargestCacheLine)
            KeLargestCacheLine = LineSize;

        if (Level == 1)
        {
            Pcr->SecondLevelCacheSize = Prcb->Cache[Prcb->CacheCount].Size;
            Pcr->SecondLevelCacheAssociativity = Prcb->Cache[Prcb->CacheCount].Associativity;
        }

        Prcb->CacheCount++;
    }
}

VOID
NTAPI
KeFlushCurrentTb(VOID)
{
    KeArm64FlushTbAll();
}

/* The inner shareable invalidation already reaches every processor */
VOID
NTAPI
KeFlushEntireTb(
    _In_ BOOLEAN Invalid,
    _In_ BOOLEAN AllProcessors)
{
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(Invalid);
    UNREFERENCED_PARAMETER(AllProcessors);

    OldIrql = KeRaiseIrqlToSynchLevel();
    KeArm64FlushTbAll();
    InterlockedExchangeAdd(&KiTbFlushTimeStamp, 1);
    KeLowerIrql(OldIrql);
}

VOID
NTAPI
KiSaveProcessorControlState(
    _Out_ PKPROCESSOR_STATE ProcessorState)
{
    PKARM64_ARCH_STATE ArchState = &ProcessorState->ArchState;
    PKSPECIAL_REGISTERS Special = &ProcessorState->SpecialRegisters;

    ArchState->Midr_El1 = KeArm64ReadRegister(ARM64_MIDR_EL1);
    ArchState->Sctlr_El1 = KeArm64ReadRegister(ARM64_SCTLR_EL1);
    ArchState->Cpacr_El1 = KeArm64ReadRegister(ARM64_CPACR_EL1);
    ArchState->Tcr_El1 = KeArm64ReadRegister(ARM64_TCR_EL1);
    ArchState->Ttbr0_El1 = KeArm64ReadRegister(ARM64_TTBR0_EL1);
    ArchState->Ttbr1_El1 = KeArm64ReadRegister(ARM64_TTBR1_EL1);
    ArchState->Esr_El1 = KeArm64ReadRegister(ARM64_ESR_EL1);
    ArchState->Far_El1 = KeArm64ReadRegister(ARM64_FAR_EL1);
    ArchState->Mair_El1 = KeArm64ReadRegister(ARM64_MAIR_EL1);
    ArchState->Vbar_El1 = KeArm64ReadRegister(ARM64_VBAR_EL1);

    Special->Tpidr_El1 = KeArm64ReadRegister(ARM64_TPIDR_EL1);
}

VOID
NTAPI
KiRestoreProcessorControlState(
    _In_ PKPROCESSOR_STATE ProcessorState)
{
    PKARM64_ARCH_STATE ArchState = &ProcessorState->ArchState;

    KeArm64WriteRegister(ARM64_MAIR_EL1, ArchState->Mair_El1);
    KeArm64WriteRegister(ARM64_TCR_EL1, ArchState->Tcr_El1);
    KeArm64WriteRegister(ARM64_CPACR_EL1, ArchState->Cpacr_El1);
    KeArm64WriteRegister(ARM64_VBAR_EL1, ArchState->Vbar_El1);
    KeArm64WriteRegister(ARM64_TPIDR_EL1, ProcessorState->SpecialRegisters.Tpidr_El1);
    __isb(_ARM64_BARRIER_SY);
    KeArm64SetTranslationBase(ArchState->Ttbr0_El1);
}

VOID
NTAPI
KeSaveStateForHibernate(
    _In_ PKPROCESSOR_STATE State)
{
    RtlCaptureContext(&State->ContextFrame);
    KiSaveProcessorControlState(State);
}

/* Data caches stay coherent with the point of coherency, nothing to write back */
BOOLEAN
NTAPI
KeInvalidateAllCaches(VOID)
{
    __dsb(_ARM64_BARRIER_SY);
    return TRUE;
}

/**
 * @brief Writes a range back to memory and drops it from the data and instruction caches.
 *
 * @remarks DC CIVAC and IC IALLUIS are broadcast to the inner shareable domain, so no IPI is needed.
 */
VOID
NTAPI
KeInvalidateRangeAllCaches(
    _In_ PVOID BaseAddress,
    _In_ ULONG Length)
{
    KeArm64SweepDcacheRange(BaseAddress, Length);

    /* CTR_EL0.DIC means instruction fetches already see the data side */
    if (!(KeArm64ReadRegister(ARM64_CTR_EL0) & (1ULL << 29)))
        KeArm64SweepIcacheRange(BaseAddress, Length);

    __dsb(_ARM64_BARRIER_SY);
    __isb(_ARM64_BARRIER_SY);
}

ULONG
NTAPI
KeGetRecommendedSharedDataAlignment(VOID)
{
    return KeLargestCacheLine;
}

VOID
NTAPI
KeSetDmaIoCoherency(
    _In_ ULONG Coherency)
{
    KiDmaIoCoherency = Coherency;
}

/**
 * @brief Returns what KeSetDmaIoCoherency was given, the HAL picks its DMA memory from it.
 */
ULONG
NTAPI
KeGetDmaIoCoherency(VOID)
{
    return KiDmaIoCoherency;
}

VOID
NTAPI
KeFlushIoBuffers(
    _In_ PMDL Mdl,
    _In_ BOOLEAN ReadOperation,
    _In_ BOOLEAN DmaOperation)
{
    PVOID Buffer;

    UNREFERENCED_PARAMETER(ReadOperation);
    UNREFERENCED_PARAMETER(DmaOperation);

    /* Coherent DMA only needs the writes ordered */
    if (!(KiDmaIoCoherency & KE_DMA_IO_NOT_COHERENT))
    {
        __dsb(_ARM64_BARRIER_SY);
        return;
    }

    /* Clean for the device to read, invalidate so no stale line hides what it writes */
    Buffer = MmGetSystemAddressForMdlSafe(Mdl, HighPagePriority);
    if (!Buffer)
    {
        DPRINT1("No system mapping for MDL %p, its DMA buffer is not flushed\n", Mdl);
        return;
    }

    KeArm64SweepDcacheRange(Buffer, Mdl->ByteCount);
}
