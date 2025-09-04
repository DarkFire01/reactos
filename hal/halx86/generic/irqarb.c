/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GPL-2.0-or-later
 * PURPOSE:         HAL Interrupt Arbiter (minimal bring-up)
 * NOTES:           Mirrors NT Hal IRQ arbiter pattern sufficiently for PCI/PNP usage.
 */

#include <hal.h>
#include <ntifs.h>
#include <ntddk.h>
#include <arbiter.h>

#ifndef PIC_CASCADE_IRQ
#define PIC_CASCADE_IRQ 2
#endif
NTSTATUS
NTAPI
ArbCommitAllocation(
    _In_ PARBITER_INSTANCE Arbiter);

#define NDEBUG
#include <debug.h>

/* Global HAL IRQ Arbiter Instance */
static ARBITER_INSTANCE HalpIrqArbiterInstance; /* Zero-inited by loader */

/* Reference/Dereference stubs (no ref counting yet) */
static VOID NTAPI HalpIrqArbInterfaceReference(_In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static VOID NTAPI HalpIrqArbInterfaceDereference(_In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

/* Simple requirement unpacker */
static NTSTATUS NTAPI
HalpIrqArbUnpackRequirement(
    _In_ PIO_RESOURCE_DESCRIPTOR Descriptor,
    _Out_ PULONGLONG Minimum,
    _Out_ PULONGLONG Maximum,
    _Out_ PULONG Length,
    _Out_ PULONG Alignment)
{
    ASSERT(Descriptor);
    ASSERT(Descriptor->Type == CmResourceTypeInterrupt);
    *Minimum = Descriptor->u.Interrupt.MinimumVector;
    *Maximum = Descriptor->u.Interrupt.MaximumVector;
    *Length  = 1; /* We only allocate a single vector at a time */
    *Alignment = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
HalpIrqArbPackResource(
    _In_ PIO_RESOURCE_DESCRIPTOR Requirement,
    _In_ ULONGLONG Start,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor)
{
    ASSERT(Requirement && Descriptor);
    ASSERT(Requirement->Type == CmResourceTypeInterrupt);
    Descriptor->Type = CmResourceTypeInterrupt;
    Descriptor->ShareDisposition = Requirement->ShareDisposition;
    Descriptor->Flags = Requirement->Flags; /* Level/Latched, ActiveHi/Lo etc. */
    Descriptor->u.Interrupt.Vector = (ULONG)Start;
    Descriptor->u.Interrupt.Level  = (ULONG)Start; /* Simplistic mapping */
    Descriptor->u.Interrupt.Affinity = KeQueryActiveProcessors();
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
HalpIrqArbUnpackResource(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor,
    _Out_ PULONGLONG Start,
    _Out_ PULONG Length)
{
    ASSERT(Descriptor->Type == CmResourceTypeInterrupt);
    *Start = Descriptor->u.Interrupt.Vector;
    *Length = 1;
    return STATUS_SUCCESS;
}

static LONG NTAPI
HalpIrqArbScoreRequirement(_In_ PIO_RESOURCE_DESCRIPTOR Descriptor)
{
    ASSERT(Descriptor->Type == CmResourceTypeInterrupt);
    return (LONG)(Descriptor->u.Interrupt.MaximumVector - Descriptor->u.Interrupt.MinimumVector + 1);
}

/* Basic allocation phase callbacks (thin wrappers around generic library) */
static NTSTATUS NTAPI HalpIrqArbTestAllocation(PARBITER_INSTANCE Arbiter, PLIST_ENTRY List)
{
    /* Delegate to generic test which builds PossibleAllocation */
    return ArbTestAllocation(Arbiter, List);
}
static NTSTATUS NTAPI HalpIrqArbRetestAllocation(PARBITER_INSTANCE Arbiter, PLIST_ENTRY List)
{
    /* If previous PossibleAllocation exists, discard it and re-test */
    if (Arbiter->PossibleAllocation)
    {
        RtlFreeRangeList(Arbiter->PossibleAllocation);
        Arbiter->PossibleAllocation = NULL;
    }
    return ArbTestAllocation(Arbiter, List);
}
static NTSTATUS NTAPI HalpIrqArbCommitAllocation(PARBITER_INSTANCE Arbiter)
{
    /* Use generic commit logic to swap PossibleAllocation into Allocation */
    return ArbCommitAllocation(Arbiter);
}
static NTSTATUS NTAPI HalpIrqArbRollbackAllocation(PARBITER_INSTANCE Arbiter)
{
    /* Abandon PossibleAllocation, keep current Allocation intact */
    if (Arbiter->PossibleAllocation)
    {
        RtlFreeRangeList(Arbiter->PossibleAllocation);
        Arbiter->PossibleAllocation = NULL;
    }
    /* Free any allocation stack built during test */
    if (Arbiter->AllocationStack)
    {
    ExFreePool(Arbiter->AllocationStack);
        Arbiter->AllocationStack = NULL;
        Arbiter->AllocationStackMaxSize = 0;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS NTAPI HalpIrqArbBootAllocation(PARBITER_INSTANCE Arbiter, PLIST_ENTRY List)
{ return ArbBootAllocation(Arbiter, List); }

/* Advanced callbacks (present in NT HAL IRQ arbiter). We keep them minimal/no-op until
   PCI IRQ routing logic is implemented. */
static NTSTATUS NTAPI
HalpIrqArbPreprocessEntry(PARBITER_INSTANCE Arbiter, PARBITER_ALLOCATION_STATE State)
{
    UNREFERENCED_PARAMETER(Arbiter);
    UNREFERENCED_PARAMETER(State);
    return STATUS_SUCCESS; /* No special range attributes */
}

static BOOLEAN NTAPI
HalpIrqArbGetNextAllocationRange(PARBITER_INSTANCE Arbiter, PARBITER_ALLOCATION_STATE State)
{
    /* Delegate to generic helper */
    return ArbGetNextAllocationRange(Arbiter, State);
}

static BOOLEAN NTAPI
HalpIrqArbFindSuitableRange(PARBITER_INSTANCE Arbiter, PARBITER_ALLOCATION_STATE State)
{
    return ArbFindSuitableRange(Arbiter, State);
}

static VOID NTAPI
HalpIrqArbAddAllocation(PARBITER_INSTANCE Arbiter, PARBITER_ALLOCATION_STATE State)
{
    UNREFERENCED_PARAMETER(Arbiter);
    UNREFERENCED_PARAMETER(State);
    /* No link tracking; nothing extra to do */
}

static VOID NTAPI
HalpIrqArbBacktrackAllocation(PARBITER_INSTANCE Arbiter, PARBITER_ALLOCATION_STATE State)
{
    UNREFERENCED_PARAMETER(Arbiter);
    UNREFERENCED_PARAMETER(State);
}

/* Public initialization entry called from halinit.c after PIC init */
NTSTATUS
HalpInitIrqArbiter(VOID)
{
    NTSTATUS Status;

    if (HalpIrqArbiterInstance.MutexEvent)
        return STATUS_SUCCESS; /* Already initialized */

    RtlZeroMemory(&HalpIrqArbiterInstance, sizeof(HalpIrqArbiterInstance));

    HalpIrqArbiterInstance.UnpackRequirement = HalpIrqArbUnpackRequirement;
    HalpIrqArbiterInstance.PackResource      = HalpIrqArbPackResource;
    HalpIrqArbiterInstance.UnpackResource    = HalpIrqArbUnpackResource;
    HalpIrqArbiterInstance.ScoreRequirement  = HalpIrqArbScoreRequirement;
    HalpIrqArbiterInstance.TestAllocation    = HalpIrqArbTestAllocation;
    HalpIrqArbiterInstance.RetestAllocation  = HalpIrqArbRetestAllocation;
    HalpIrqArbiterInstance.CommitAllocation  = HalpIrqArbCommitAllocation;
    HalpIrqArbiterInstance.RollbackAllocation= HalpIrqArbRollbackAllocation;
    HalpIrqArbiterInstance.BootAllocation    = HalpIrqArbBootAllocation;
    HalpIrqArbiterInstance.PreprocessEntry   = HalpIrqArbPreprocessEntry;
    HalpIrqArbiterInstance.GetNextAllocationRange = HalpIrqArbGetNextAllocationRange;
    HalpIrqArbiterInstance.FindSuitableRange = HalpIrqArbFindSuitableRange;
    HalpIrqArbiterInstance.AddAllocation     = HalpIrqArbAddAllocation;
    HalpIrqArbiterInstance.BacktrackAllocation = HalpIrqArbBacktrackAllocation;

    Status = ArbInitializeArbiterInstance(&HalpIrqArbiterInstance,
                                          NULL, /* Root-style */
                                          CmResourceTypeInterrupt,
                                          L"HalIRQ",
                                          L"Root",
                                          NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Hal IRQ Arbiter init failed %lx\n", Status);
        return Status;
    }

    /* Reserve vectors >=16 (legacy PIC ISA line limit for PIC-based allocation) */
    (VOID)RtlAddRange(HalpIrqArbiterInstance.Allocation,
                      16,
                      MAXULONG,
                      0,
                      RTL_RANGE_LIST_ADD_IF_CONFLICT,
                      NULL,
                      NULL);

    /* Also mark the cascade IRQ (IRQ2) as unavailable for generic assignment */
    (VOID)RtlAddRange(HalpIrqArbiterInstance.Allocation,
                      PIC_CASCADE_IRQ,
                      PIC_CASCADE_IRQ,
                      0,
                      RTL_RANGE_LIST_ADD_IF_CONFLICT,
                      NULL,
                      NULL);

    /* Reserve core platform IRQs: Timer(0), Keyboard(1), RTC(8) */
    (VOID)RtlAddRange(HalpIrqArbiterInstance.Allocation, 0, 0, 0, RTL_RANGE_LIST_ADD_IF_CONFLICT, NULL, NULL);
    (VOID)RtlAddRange(HalpIrqArbiterInstance.Allocation, 1, 1, 0, RTL_RANGE_LIST_ADD_IF_CONFLICT, NULL, NULL);
    (VOID)RtlAddRange(HalpIrqArbiterInstance.Allocation, 8, 8, 0, RTL_RANGE_LIST_ADD_IF_CONFLICT, NULL, NULL);

    /* Optionally reserve cascade line (IRQ2) separately if desired; keeping allocatable is fine if PnP wants it */
    DPRINT("Hal IRQ Arbiter initialized\n");
    return STATUS_SUCCESS;
}

/* Accessor for later exporting interface if needed */
PARBITER_INSTANCE
HalpGetIrqArbiter(VOID)
{
    return (HalpIrqArbiterInstance.MutexEvent) ? &HalpIrqArbiterInstance : NULL;
}

/* Static IRQ Arbiter Interface constant mirroring NT pattern */
static const ARBITER_INTERFACE HalpIrqArbiterInterfaceTemplate = {
    sizeof(ARBITER_INTERFACE), /* Size */
    1,                         /* Version */
    &HalpIrqArbiterInstance,   /* Context */
    HalpIrqArbInterfaceReference,
    HalpIrqArbInterfaceDereference,
    ArbArbiterHandler,
    0                          /* Flags (no ARBITER_PARTIAL) */
};

/* Internal fill routine (to be called by Hal PnP query path when GUID_ARBITER_INTERFACE_STANDARD requested) */
NTSTATUS
HalpFillInIrqArbiter(
    _In_ ULONG Version,
    _In_ ULONG InterfaceBufferSize,
    _Out_writes_bytes_(InterfaceBufferSize) PARBITER_INTERFACE Interface,
    _Out_ PULONG Length)
{
    if (!HalpIrqArbiterInstance.MutexEvent)
        return STATUS_DEVICE_NOT_READY;
    if (!Interface || !Length)
        return STATUS_INVALID_PARAMETER;

    *Length = sizeof(ARBITER_INTERFACE);
    if (InterfaceBufferSize < sizeof(ARBITER_INTERFACE))
        return STATUS_BUFFER_TOO_SMALL;
    if (Version != 1)
        return STATUS_NOT_SUPPORTED;

    /* Copy template */
    *Interface = HalpIrqArbiterInterfaceTemplate;
    return STATUS_SUCCESS;
}

/* EOF */
