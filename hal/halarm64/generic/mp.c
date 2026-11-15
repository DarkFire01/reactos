/*
 * PROJECT:     ReactOS ARM64 HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Starting the secondary processors through PSCI
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

ULONG64
NTAPI
HalpCallFirmware(
    _In_ ULONG64 Function,
    _In_ ULONG64 Argument1,
    _In_ ULONG64 Argument2,
    _In_ ULONG64 Argument3,
    _In_ BOOLEAN UseHvc);

/* PSCI 0.2 CPU_ON, SMC64 calling convention */
#define PSCI_CPU_ON             0xC4000003
#define PSCI_SUCCESS            0
#define PSCI_ALREADY_ON         (-4)

/* Stage 1 descriptors with the 4 KB granule */
#define HALP_DESC_TABLE         0x3ULL
#define HALP_DESC_BLOCK         0x1ULL
#define HALP_DESC_ATTR(Index)   ((ULONG64)(Index) << 2)
#define HALP_DESC_INNER_SHARE   (3ULL << 8)
#define HALP_DESC_AF            (1ULL << 10)
#define HALP_DESC_UXN           (1ULL << 54)
#define HALP_DESC_ADDRESS       0x0000FFFFFFFFF000ULL

/* TCR_EL1 fields for the lower half */
#define HALP_TCR_T0SZ(Tcr)      ((ULONG)(Tcr) & 0x3F)
#define HALP_TCR_EPD0           (1ULL << 7)
#define HALP_TCR_TG0(Tcr)       ((ULONG)((Tcr) >> 14) & 3)

/* MAIR attribute for normal write back memory */
#define HALP_MAIR_NORMAL_WB     0xFF

/* The trampoline page, then the two tables of its identity map */
#define HALP_AP_PAGES           3
#define HALP_AP_START_OFFSET    0x800

/* How long the starting processor has to reach its kernel address */
#define HALP_AP_TIMEOUT_US      1000000

/* GLOBALS *******************************************************************/

static PUCHAR HalpApPage;
static PHYSICAL_ADDRESS HalpApPagePhysical;

/* Which MADT entries have been started, the boot processor counts as started */
static BOOLEAN HalpMadtStarted[MAXIMUM_PROCESSORS];

/* FUNCTIONS *****************************************************************/

static
BOOLEAN
HalpPsciUseHvc(VOID)
{
    return !!(HalpPlatform.ArmBootFlags & FADT_ARM_PSCI_USE_HVC);
}

/* The MAIR slot the kernel uses for ordinary memory */
static
ULONG
HalpFindNormalAttribute(
    _In_ ULONG64 Mair)
{
    ULONG Index;

    for (Index = 0; Index < 8; Index++)
    {
        if (((Mair >> (Index * 8)) & 0xFF) == HALP_MAIR_NORMAL_WB)
            return Index;
    }

    return MAXULONG;
}

/**
 * @brief Builds the page holding the trampoline and the tables that map it one to one.
 */
static
BOOLEAN
HalpPrepareApPage(VOID)
{
    PHYSICAL_ADDRESS Highest;
    SIZE_T CodeSize;

    if (HalpApPage)
        return TRUE;

    CodeSize = (ULONG_PTR)HalpApTrampolineEnd - (ULONG_PTR)HalpApTrampoline;
    ASSERT(CodeSize > 0);
    if (CodeSize > HALP_AP_START_OFFSET)
    {
        DPRINT1("HAL: AP trampoline is %Iu bytes, too big\n", CodeSize);
        return FALSE;
    }

    /* Anything the identity map can reach with a 48 bit lower half */
    Highest.QuadPart = 0x0000FFFFFFFFFFFFULL;
    HalpApPage = MmAllocateContiguousMemory(HALP_AP_PAGES * PAGE_SIZE, Highest);
    if (!HalpApPage)
    {
        DPRINT1("HAL: no memory for the AP trampoline\n");
        return FALSE;
    }

    RtlZeroMemory(HalpApPage, HALP_AP_PAGES * PAGE_SIZE);
    RtlCopyMemory(HalpApPage, (PVOID)HalpApTrampoline, CodeSize);
    HalpApPagePhysical = MmGetPhysicalAddress(HalpApPage);
    ASSERT((HalpApPagePhysical.QuadPart & (PAGE_SIZE - 1)) == 0);
    return TRUE;
}

/**
 * @brief Fills in what the trampoline reads for one processor.
 */
static
BOOLEAN
HalpFillApStart(
    _In_ PKPROCESSOR_STATE ProcessorState)
{
    PHALP_AP_START Start = (PHALP_AP_START)(HalpApPage + HALP_AP_START_OFFSET);
    PULONG64 Level0 = (PULONG64)(HalpApPage + PAGE_SIZE);
    PULONG64 Level1 = (PULONG64)(HalpApPage + 2 * PAGE_SIZE);
    ULONG64 Physical = (ULONG64)HalpApPagePhysical.QuadPart;
    ULONG64 Tcr, Mair, Block;
    ULONG Attribute, T0sz;

    Tcr = KeArm64ReadRegister(ARM64_TCR_EL1);
    Mair = KeArm64ReadRegister(ARM64_MAIR_EL1);
    T0sz = HALP_TCR_T0SZ(Tcr);

    /* The identity map is two levels of 4 KB tables, a 4 level lower half */
    if ((HALP_TCR_TG0(Tcr) != 0) || (Tcr & HALP_TCR_EPD0) || (T0sz > 24) || (T0sz < 16))
    {
        DPRINT1("HAL: TCR_EL1 %I64x does not allow the AP identity map\n", Tcr);
        return FALSE;
    }

    Attribute = HalpFindNormalAttribute(Mair);
    if (Attribute == MAXULONG)
    {
        DPRINT1("HAL: MAIR_EL1 %I64x has no write back slot\n", Mair);
        return FALSE;
    }

    /* One 1 GB block around the page, kernel only and executable */
    RtlZeroMemory(Level0, 2 * PAGE_SIZE);
    Block = (Physical & ~((1ULL << 30) - 1)) | HALP_DESC_BLOCK | HALP_DESC_ATTR(Attribute) |
            HALP_DESC_INNER_SHARE | HALP_DESC_AF | HALP_DESC_UXN;
    Level1[(Physical >> 30) & 0x1FF] = Block;
    Level0[(Physical >> 39) & 0x1FF] = ((Physical + 2 * PAGE_SIZE) & HALP_DESC_ADDRESS) | HALP_DESC_TABLE;

    RtlZeroMemory(Start, sizeof(*Start));
    Start->Mair = Mair;
    Start->Tcr = Tcr;
    Start->Sctlr = KeArm64ReadRegister(ARM64_SCTLR_EL1);
    Start->Cpacr = KeArm64ReadRegister(ARM64_CPACR_EL1);
    Start->IdentityRoot = Physical + PAGE_SIZE;
    Start->Root = KeArm64ReadRegister(ARM64_TTBR1_EL1);
    Start->Stack = ProcessorState->ContextFrame.Sp;
    Start->Entry = ProcessorState->ContextFrame.Pc;
    Start->Argument = ProcessorState->ContextFrame.X0;
    Start->PageVa = (ULONG64)HalpApPage;
    Start->Vbar = ProcessorState->ArchState.Vbar_El1;
    Start->Progress = 0;

    ASSERT(Start->Stack && Start->Entry && Start->Argument);
    ASSERT((Start->Stack & 0xF) == 0);

    /* The processor reads all of it with its MMU and caches off */
    HalpSweepDcacheRange(HalpApPage, HALP_AP_PAGES * PAGE_SIZE);
    return TRUE;
}

/* The trampoline's first store goes around the caches */
static
ULONG64
HalpReadApProgress(VOID)
{
    PHALP_AP_START Start = (PHALP_AP_START)(HalpApPage + HALP_AP_START_OFFSET);

    HalpSweepDcacheRange((PVOID)&Start->Progress, sizeof(Start->Progress));
    return Start->Progress;
}

/**
 * @brief Starts the next processor the MADT lists. The kernel waits for it to check in.
 */
BOOLEAN
NTAPI
HalStartNextProcessor(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ PKPROCESSOR_STATE ProcessorState)
{
    ULONG Number = (ULONG)KeNumberProcessors;
    ULONG Index, Waited;
    ULONG64 Mpidr;
    LONG64 Status;

    UNREFERENCED_PARAMETER(LoaderBlock);
    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    /* The SGI and target code for GICv2 is not ready for more than one processor */
    if (HalpGicIsVersion2())
    {
        DPRINT1("HAL: GICv2, staying on one processor\n");
        return FALSE;
    }

    if (!(HalpPlatform.ArmBootFlags & FADT_ARM_PSCI_COMPLIANT))
    {
        DPRINT1("HAL: firmware is not PSCI compliant, staying on one processor\n");
        return FALSE;
    }

    if (Number >= MAXIMUM_PROCESSORS)
        return FALSE;

    for (Index = 0; Index < HalpMadtCount; Index++)
    {
        Mpidr = HalpMadtMpidr[Index];
        if (HalpMadtStarted[Index])
            continue;

        HalpMadtStarted[Index] = TRUE;
        if (Mpidr == HalpProcessorMpidr[0])
            continue;

        if (!HalpPrepareApPage() || !HalpFillApStart(ProcessorState))
            return FALSE;

        if (!HalpGicMapProcessor(Number, Mpidr))
        {
            DPRINT1("HAL: no redistributor for MPIDR %I64x, skipping it\n", Mpidr);
            continue;
        }

        HalpProcessorMpidr[Number] = Mpidr;
        Status = (LONG64)HalpCallFirmware(PSCI_CPU_ON,
                                          Mpidr,
                                          (ULONG64)HalpApPagePhysical.QuadPart,
                                          (ULONG64)HalpApPagePhysical.QuadPart + HALP_AP_START_OFFSET,
                                          HalpPsciUseHvc());
        if (Status != PSCI_SUCCESS)
        {
            DPRINT1("HAL: PSCI CPU_ON of MPIDR %I64x failed %I64d\n", Mpidr, Status);
            HalpProcessorMpidr[Number] = 0;
            continue;
        }

        /* The kernel half of the trampoline is past the identity map */
        for (Waited = 0; (HalpReadApProgress() < 2) && (Waited < HALP_AP_TIMEOUT_US); Waited += 100)
            KeStallExecutionProcessor(100);

        if (HalpReadApProgress() < 2)
        {
            DPRINT1("HAL: MPIDR %I64x stopped in the trampoline at step %I64u\n", Mpidr, HalpReadApProgress());
            KeBugCheckEx(HAL_INITIALIZATION_FAILED, 0x5C1, Number, (ULONG_PTR)Mpidr, (ULONG_PTR)HalpReadApProgress());
        }

        DPRINT1("HAL: processor %lu, MPIDR %I64x, is in the kernel\n", Number, Mpidr);
        return TRUE;
    }

    return FALSE;
}

/**
 * @brief Gives every processor but the boot one its clock, once they all run threads.
 */
VOID
NTAPI
HalpStartProcessorClocks(VOID)
{
    PHALP_GIC_CPU_STATE State;
    ULONG Number, Before, Waited;

    for (Number = 0; Number < (ULONG)KeNumberProcessors; Number++)
    {
        State = &HalpGicCpuState[Number];

        /* An SGI the processor never takes leaves it deaf to the scheduler and the debugger */
        if (Number != 0)
        {
            Before = State->Acknowledged;
            HalpGicSendSgi(IPI_LEVEL, AFFINITY_MASK(Number));
            for (Waited = 0; (State->Acknowledged == Before) && (Waited < 20000); Waited += 10)
                KeStallExecutionProcessor(10);
            State->NoSgi = (State->Acknowledged == Before);
        }

        DbgPrint("HAL: CPU %lu MPIDR %I64x SGI %s, WAKER %lx GROUP %lx ENABLE %lx PRIO %x SRE %lx CTLR %lx GRPEN1 %lx GICD %lx, %lu taken %lu spurious\n",
                 Number, HalpProcessorMpidr[Number], State->NoSgi ? "LOST" : "ok",
                 State->Waker, State->Group, State->Enable, State->IpiPriority, State->Sre,
                 State->Ctlr, State->GroupEnable, State->DistributorControl,
                 State->Acknowledged, State->Spurious);
    }

    /* Leave the report on a screen debug port long enough to read */
    for (Number = 1; Number < (ULONG)KeNumberProcessors; Number++)
    {
        if (HalpGicCpuState[Number].NoSgi)
        {
            DbgPrint("HAL: some processors miss SGIs, pausing 10 seconds\n");
            for (Waited = 0; Waited < 10000; Waited++)
                KeStallExecutionProcessor(1000);
            break;
        }
    }

    for (Number = 1; Number < (ULONG)KeNumberProcessors; Number++)
    {
        /* It idles by polling, its clock would never be connected */
        if (!HalpGicCpuState[Number].NoSgi)
            HalpStartProcessorClock(Number);
    }
}
