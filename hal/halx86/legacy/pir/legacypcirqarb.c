/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Legacy HAL IRQ Arbiter
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>
#include <arbiter.h>

#define NDEBUG
#include <debug.h>

#define HALP_PIC_LAST_IRQ       15
#define HALP_IDE_PRIMARY_IRQ    14
#define HALP_IDE_SECONDARY_IRQ  15

static ARBITER_INSTANCE LegacyPCArbiter;

static
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpLegacyPCArbUnpackRequirement(
    _In_ PIO_RESOURCE_DESCRIPTOR IoDescriptor,
    _Out_ PUINT64 OutMinimumAddress,
    _Out_ PUINT64 OutMaximumAddress,
    _Out_ PUINT64 OutLength,
    _Out_ PUINT64 OutAlignment)
{
    PAGED_CODE();

    *OutMinimumAddress = (UINT64)IoDescriptor->u.Interrupt.MinimumVector;
    *OutMaximumAddress = (UINT64)IoDescriptor->u.Interrupt.MaximumVector;
    *OutLength = 1;
    *OutAlignment = 1;
    return STATUS_SUCCESS;
}

static
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpLegacyPCArbPackResource(
    _In_ PIO_RESOURCE_DESCRIPTOR IoDescriptor,
    _In_ UINT64 Start,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDescriptor)
{
    PAGED_CODE();

    CmDescriptor->Type = CmResourceTypeInterrupt;
    CmDescriptor->ShareDisposition = IoDescriptor->ShareDisposition;
    CmDescriptor->Flags = IoDescriptor->Flags;
    CmDescriptor->u.Interrupt.Level = (ULONG)Start;
    CmDescriptor->u.Interrupt.Vector = (ULONG)Start;
    CmDescriptor->u.Interrupt.Affinity = (KAFFINITY)-1;
    return STATUS_SUCCESS;
}

static
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpLegacyPCArbUnpackResource(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDescriptor,
    _Out_ PUINT64 Start,
    _Out_ PUINT64 OutLength)
{
    PAGED_CODE();

    *Start = CmDescriptor->u.Interrupt.Vector;
    *OutLength = 1;
    return STATUS_SUCCESS;
}

static
CODE_SEG("PAGE")
INT32
NTAPI
HalpLegacyPCArbScoreRequirement(
    _In_ PIO_RESOURCE_DESCRIPTOR IoDescriptor)
{
    INT32 ScoreReg;

    PAGED_CODE();

    ScoreReg = (INT32)(IoDescriptor->u.Interrupt.MaximumVector - IoDescriptor->u.Interrupt.MinimumVector + 1);

    return (ScoreReg < 0) ? MAXLONG : ScoreReg;
}

static
VOID
NTAPI
HalpLegacyPCArbReference(PVOID Context)
{
    NOTHING;
}

static
VOID
NTAPI
HalpLegacyPCArbDereference(PVOID Context)
{
    NOTHING;
}

/* LINK ROUTING *****************************************************************/

/* First routed range matching Link and Irq. A NULL Link or a zero Irq matches any. */
static
PRTL_RANGE
HalpLegacyPCArbFindRouted(
    _In_ PRTL_RANGE_LIST Ranges,
    _In_opt_ PHALP_PCI_LINK Link,
    _In_ ULONG Irq)
{
    RTL_RANGE_LIST_ITERATOR Iterator;
    PRTL_RANGE Range;

    if (!NT_SUCCESS(RtlGetFirstRange(Ranges, &Iterator, &Range)))
        return NULL;

    while (Range)
    {
        if (Range->UserData &&
            (!Link || Range->UserData == Link) &&
            (Irq == 0 || Range->Start == Irq))
        {
            return Range;
        }

        if (!NT_SUCCESS(RtlGetNextRange(&Iterator, &Range, TRUE)))
            break;
    }

    return NULL;
}

static
ULONG
HalpLegacyPCArbLinkIrq(
    _In_ PRTL_RANGE_LIST Ranges,
    _In_ PHALP_PCI_LINK Link)
{
    PRTL_RANGE Range = HalpLegacyPCArbFindRouted(Ranges, Link, 0);

    return Range ? (ULONG)Range->Start : 0;
}

static
BOOLEAN
NTAPI
HalpLegacyPCArbShareableRange(
    _In_ PVOID Context,
    _In_ PRTL_RANGE Range)
{
    UNREFERENCED_PARAMETER(Context);

    return (Range->Attributes & HALP_IRQ_RANGE_LEVEL) && (Range->Flags & RTL_RANGE_SHARED);
}

static
BOOLEAN
HalpLegacyPCArbLinkAccepts(
    _In_ PARBITER_INSTANCE Arbiter,
    _In_ PARBITER_ALLOCATION_STATE State,
    _In_ PHALP_PCI_LINK Link,
    _In_ ULONG Irq)
{
    BOOLEAN Available = FALSE;

    if (Irq < State->CurrentMinimum || Irq > State->CurrentMaximum)
        return FALSE;

    if (Irq == 0 || Irq > HALP_PIC_LAST_IRQ || !(Link->IrqMask & (1 << Irq)))
        return FALSE;

    if (!NT_SUCCESS(RtlIsRangeAvailable(Arbiter->PossibleAllocation,
                                        Irq,
                                        Irq,
                                        0,
                                        0,
                                        NULL,
                                        HalpLegacyPCArbShareableRange,
                                        &Available)))
    {
        return FALSE;
    }

    return Available;
}

/* Print every range holding an IRQ the link can reach */
static
VOID
HalpLegacyPCArbDumpLinkRanges(
    _In_ PARBITER_INSTANCE Arbiter,
    _In_ PHALP_PCI_LINK Link)
{
    RTL_RANGE_LIST_ITERATOR Iterator;
    PRTL_RANGE Range;
    ULONG Irq;

    if (!NT_SUCCESS(RtlGetFirstRange(Arbiter->PossibleAllocation, &Iterator, &Range)))
        return;

    while (Range)
    {
        for (Irq = (ULONG)Range->Start; Irq <= Range->End && Irq <= HALP_PIC_LAST_IRQ; Irq++)
        {
            if (Link->IrqMask & (1 << Irq))
            {
                DPRINT1("HalIRQ:   IRQ %lu-%lu owner %p link %p attributes 0x%x flags 0x%x\n",
                        (ULONG)Range->Start, (ULONG)Range->End, Range->Owner,
                        Range->UserData, Range->Attributes, Range->Flags);
                break;
            }
        }

        if (!NT_SUCCESS(RtlGetNextRange(&Iterator, &Range, TRUE)))
            break;
    }
}

/**
 * @brief
 * Picks the IRQ for a routed device.
 *
 * @remarks
 * A link keeps the IRQ it already has in this allocation. An unused link
 * prefers its firmware IRQ, then an IRQ no other link has, then any
 * reachable IRQ, higher IRQs first.
 */
static
ULONG
HalpLegacyPCArbPickLinkIrq(
    _In_ PARBITER_INSTANCE Arbiter,
    _In_ PARBITER_ALLOCATION_STATE State,
    _In_ PHALP_PCI_LINK Link)
{
    PDEVICE_OBJECT DeviceObject = State->Entry->PhysicalDeviceObject;
    ULONG Irq, Best = 0, BestRank = 0, Rank;
    UCHAR Firmware;

    Irq = HalpLegacyPCArbLinkIrq(Arbiter->PossibleAllocation, Link);
    if (Irq != 0)
    {
        if (Irq < State->CurrentMinimum || Irq > State->CurrentMaximum)
        {
            DPRINT1("HalIRQ: %p link 0x%x already on IRQ %lu, outside %I64u-%I64u\n",
                    DeviceObject, Link->Link, Irq, State->CurrentMinimum, State->CurrentMaximum);
            return 0;
        }

        DPRINT1("HalIRQ: %p link 0x%x shares IRQ %lu\n", DeviceObject, Link->Link, Irq);
        return Irq;
    }

    if (!NT_SUCCESS(HalpLegacyPCGetLinkIrq(Link, &Firmware)))
        Firmware = 0;

    if (Firmware != 0 && (Firmware < State->CurrentMinimum || Firmware > State->CurrentMaximum))
    {
        DPRINT1("HalIRQ: %p link 0x%x firmware IRQ %u outside %I64u-%I64u\n",
                DeviceObject, Link->Link, Firmware, State->CurrentMinimum, State->CurrentMaximum);
        return 0;
    }

    for (Irq = 1; Irq <= HALP_PIC_LAST_IRQ; Irq++)
    {
        if (!HalpLegacyPCArbLinkAccepts(Arbiter, State, Link, Irq))
            continue;

        if (Irq == Firmware)
        {
            DPRINT1("HalIRQ: %p link 0x%x keeps firmware IRQ %lu\n", DeviceObject, Link->Link, Irq);
            return Irq;
        }

        Rank = HalpLegacyPCArbFindRouted(Arbiter->PossibleAllocation, NULL, Irq) ? 1 : 2;
        if (Rank >= BestRank)
        {
            Best = Irq;
            BestRank = Rank;
        }
    }

    DPRINT1("HalIRQ: %p link 0x%x mask 0x%x firmware IRQ %u unavailable, picked %lu\n",
            DeviceObject, Link->Link, Link->IrqMask, Firmware, Best);
    HalpLegacyPCArbDumpLinkRanges(Arbiter, Link);

    return Best;
}

/* ARBITER CALLBACKS ************************************************************/

static
NTSTATUS
NTAPI
HalpLegacyPCArbPreprocessEntry(
    _In_ PARBITER_INSTANCE Arbiter,
    _Inout_ PARBITER_ALLOCATION_STATE State)
{
    UNREFERENCED_PARAMETER(Arbiter);

    PAGED_CODE();

    State->RangeAttributes &= ~(HALP_IRQ_RANGE_LEVEL | HALP_IRQ_RANGE_LATCHED);

    if (State->Alternatives->Descriptor->Flags & CM_RESOURCE_INTERRUPT_LATCHED)
        State->RangeAttributes |= HALP_IRQ_RANGE_LATCHED;
    else
        State->RangeAttributes |= HALP_IRQ_RANGE_LEVEL;

    return STATUS_SUCCESS;
}

/* One pass over the whole window of each alternative */
static
BOOLEAN
NTAPI
HalpLegacyPCArbGetNextAllocationRange(
    _In_ PARBITER_INSTANCE Arbiter,
    _Inout_ PARBITER_ALLOCATION_STATE State)
{
    PARBITER_ALTERNATIVE Alternative;

    UNREFERENCED_PARAMETER(Arbiter);

    PAGED_CODE();

    Alternative = State->CurrentAlternative ? State->CurrentAlternative + 1 : State->Alternatives;
    if (Alternative >= State->Alternatives + State->AlternativeCount)
        return FALSE;

    State->CurrentAlternative = Alternative;
    State->CurrentMinimum = Alternative->Minimum;
    State->CurrentMaximum = Alternative->Maximum;
    return TRUE;
}

static
BOOLEAN
NTAPI
HalpLegacyPCArbFindSuitableRange(
    _In_ PARBITER_INSTANCE Arbiter,
    _Inout_ PARBITER_ALLOCATION_STATE State)
{
    PHALP_PCI_LINK Link;
    NTSTATUS Status;
    ULONG Irq;

    PAGED_CODE();

    Status = HalpLegacyPCFindLink(State->Entry->PhysicalDeviceObject, &Link);

    if (Status == STATUS_NOT_SUPPORTED)
        return ArbiterLibFindSuitableRange(Arbiter, State);

    if (!NT_SUCCESS(Status))
    {
        /* Edge triggered ISA interrupts are exclusive */
        if (Status == STATUS_NOT_FOUND)
            State->CurrentAlternative->Flags &= ~ARBITER_ALTERNATIVE_FLAG_SHARED;

        if ((State->Entry->Flags & ARBITER_FLAG_BOOT_CONFIG) &&
            State->CurrentMinimum == State->CurrentMaximum &&
            (State->CurrentMinimum == HALP_IDE_PRIMARY_IRQ ||
             State->CurrentMinimum == HALP_IDE_SECONDARY_IRQ))
        {
            State->RangeAvailableAttributes |= ARBITER_RANGE_BOOT_ALLOCATED;
        }

        return ArbiterLibFindSuitableRange(Arbiter, State);
    }

    if (!Link)
    {
        DPRINT1("HalIRQ: %p is routed but has no link\n", State->Entry->PhysicalDeviceObject);
        return FALSE;
    }

    Irq = HalpLegacyPCArbPickLinkIrq(Arbiter, State, Link);
    if (Irq == 0)
        return FALSE;

    State->Start = Irq;
    State->End = Irq;
    return TRUE;
}

static
VOID
NTAPI
HalpLegacyPCArbAddAllocation(
    _In_ PARBITER_INSTANCE Arbiter,
    _Inout_ PARBITER_ALLOCATION_STATE State)
{
    ULONG Flags = RTL_RANGE_LIST_ADD_IF_CONFLICT;
    PHALP_PCI_LINK Link;

    PAGED_CODE();

    if (!NT_SUCCESS(HalpLegacyPCFindLink(State->Entry->PhysicalDeviceObject, &Link)))
        Link = NULL;

    if (State->CurrentAlternative->Flags & ARBITER_ALTERNATIVE_FLAG_SHARED)
        Flags |= RTL_RANGE_LIST_ADD_SHARED;

    RtlAddRange(Arbiter->PossibleAllocation,
                State->Start,
                State->End,
                State->RangeAttributes,
                Flags,
                Link,
                State->Entry->PhysicalDeviceObject);
}

static
NTSTATUS
NTAPI
HalpLegacyPCArbCommitAllocation(
    _In_ PARBITER_INSTANCE Arbiter)
{
    RTL_RANGE_LIST_ITERATOR Iterator;
    PHALP_PCI_LINK Link;
    PRTL_RANGE Range;
    UCHAR Wanted, Current;

    PAGED_CODE();

    if (NT_SUCCESS(RtlGetFirstRange(Arbiter->PossibleAllocation, &Iterator, &Range)))
    {
        while (Range)
        {
            if (Range->UserData)
                HalpLegacyPCUpdateInterruptLine(Range->Owner, (UCHAR)Range->Start);

            if (!NT_SUCCESS(RtlGetNextRange(&Iterator, &Range, TRUE)))
                break;
        }
    }

    for (Link = HalpLegacyPCFirstLink(); Link; Link = Link->Next)
    {
        Wanted = (UCHAR)HalpLegacyPCArbLinkIrq(Arbiter->PossibleAllocation, Link);

        if (!NT_SUCCESS(HalpLegacyPCGetLinkIrq(Link, &Current)) || Current == Wanted)
            continue;

        if (Wanted == 0)
        {
            /* Leave links we never assigned as the firmware set them */
            if (HalpLegacyPCArbLinkIrq(Arbiter->Allocation, Link) == 0)
                continue;

            if (!HalpLegacyPCArbFindRouted(Arbiter->PossibleAllocation, NULL, Current))
                HalDisableSystemInterrupt(HalpIrqToVector(Current), 0);
        }

        DPRINT1("HalIRQ: link 0x%x IRQ %u -> %u\n", Link->Link, Current, Wanted);
        HalpLegacyPCSetLinkIrq(Link, Wanted);
    }

    return ArbiterLibCommitAllocation(Arbiter);
}

/**
 * @brief
 * Initialize the legacy PC arbiter that uses PIR for PIC IRQ assignments.
 *
 * @param[in] BusFdo
 * Hal Bus FDO deviceobject
 */
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpLegacyPCCreateArbiter(
    _In_ PDEVICE_OBJECT BusFdo)
{
    NTSTATUS Status;

    PAGED_CODE();

    LegacyPCArbiter.UnpackRequirement = HalpLegacyPCArbUnpackRequirement;
    LegacyPCArbiter.PackResource = HalpLegacyPCArbPackResource;
    LegacyPCArbiter.UnpackResource = HalpLegacyPCArbUnpackResource;
    LegacyPCArbiter.ScoreRequirement = HalpLegacyPCArbScoreRequirement;
    LegacyPCArbiter.PreprocessEntry = HalpLegacyPCArbPreprocessEntry;
    LegacyPCArbiter.GetNextAllocationRange = HalpLegacyPCArbGetNextAllocationRange;
    LegacyPCArbiter.FindSuitableRange = HalpLegacyPCArbFindSuitableRange;
    LegacyPCArbiter.AddAllocation = HalpLegacyPCArbAddAllocation;
    LegacyPCArbiter.CommitAllocation = HalpLegacyPCArbCommitAllocation;

    Status = ArbiterLibInitializeInstance(&LegacyPCArbiter,
                                          BusFdo,
                                          CmResourceTypeInterrupt,
                                          L"HalIRQ",
                                          L"Root",
                                          NULL);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /* Lock to the 0-15 the PIR table only understands. */
    RtlAddRange(LegacyPCArbiter.Allocation, HALP_PIC_LAST_IRQ + 1,
                ARBITER_MAXIMUM_ADDRESS, 0,
                RTL_RANGE_LIST_ADD_IF_CONFLICT, NULL, NULL);
    RtlAddRange(LegacyPCArbiter.Allocation, PIC_CASCADE_IRQ, PIC_CASCADE_IRQ,
                0, RTL_RANGE_LIST_ADD_IF_CONFLICT, NULL, NULL);

    return Status;
}

/**
 * @brief
 * Query the legacy PC Arbiter
 *
 * @param[out] Interface
 * Caller-supplied buffer that receives the ARBITER_INTERFACE.
 *
 * @param[in] Size
 * Size of the Interface buffer, in bytes.
 *
 * @param[out] Length
 * Receives sizeof(ARBITER_INTERFACE), whether or not the buffer was large
 * enough.
 *
 * @return
 * STATUS_SUCCESS on success, or STATUS_BUFFER_TOO_SMALL if Size is smaller
 * than sizeof(ARBITER_INTERFACE).
 */
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpLegacyPCQueryArbInterface(
    _Out_writes_bytes_(Size) PVOID Interface,
    _In_                     ULONG Size,
    _Out_                    PULONG Length)
{
    PARBITER_INTERFACE ArbiterInterface = Interface;

    PAGED_CODE();

    *Length = sizeof(ARBITER_INTERFACE);
    if (Size < sizeof(ARBITER_INTERFACE))
        return STATUS_BUFFER_TOO_SMALL;

    ArbiterInterface->Size = sizeof(ARBITER_INTERFACE);
    ArbiterInterface->Version = 1;
    ArbiterInterface->Context = &LegacyPCArbiter;
    ArbiterInterface->ArbiterHandler = ArbiterLibHandler;
    ArbiterInterface->Flags = 0;

    ArbiterInterface->InterfaceDereference = HalpLegacyPCArbDereference;
    ArbiterInterface->InterfaceReference = HalpLegacyPCArbReference;

    return STATUS_SUCCESS;
}
