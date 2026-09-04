/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Message-signalled interrupt support for the APIC HAL
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A message-signalled interrupt never touches the I/O APIC: the device
 * writes a data word to a local APIC address and the write is the
 * interrupt. What the HAL contributes is therefore
 *  - the policy decision "may this machine use messages at all",
 *  - the description of how processors are addressed (physical id or
 *    flat logical id) so a message can name its target,
 *  - vectors that belong to no I/O APIC input, and
 *  - the address/data pair a bus driver programs into the device.
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#include "apicp.h"
#include <smp.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

/* The layouts below are shared with the kernel and the bus drivers */
#ifdef _M_IX86
C_ASSERT(sizeof(INTERRUPT_VECTOR_DATA) == 80);
C_ASSERT(FIELD_OFFSET(INTERRUPT_CONNECTION_DATA, Vectors) == 8);
C_ASSERT(sizeof(HAL_INTERRUPT_TARGET_INFORMATION) == 24);
C_ASSERT(FIELD_OFFSET(HAL_MESSAGE_TARGET_REQUEST, Apic.DestinationMode) == 0x18);
#endif

/* Processor identities collected from the firmware tables */
extern const PPROCESSOR_IDENTITY HalpProcessorIdentity;
extern HALP_APIC_INFO_TABLE HalpApicInfoTable;

/* Cached "messages allowed" decision */
static BOOLEAN HalpMsiDecided;
static BOOLEAN HalpMsiAllowed;

/* Serialises message-vector allocation */
static KSPIN_LOCK HalpMessageVectorLock;

/* Largest run of vectors handed out for one device; a message vector run
   never crosses a priority row, so this is one row */
#define HALP_MAX_MESSAGE_VECTORS 16

/* PRIVATE FUNCTIONS *********************************************************/

/**
 * @brief
 * Decides whether this processor family delivers message-signalled
 * interrupts reliably. Intel needs a NetBurst or a Pentium M / Core class
 * part, AMD needs K8 or newer; everything else is refused. The decision
 * can be forced either way from the boot command line (FORCEMSI / NOMSI).
 */
BOOLEAN
NTAPI
HalpMessageInterruptsAllowed(VOID)
{
    PKPRCB Prcb;
    UCHAR Family, Model;
    BOOLEAN Supported;

    if (HalpMsiDecided)
    {
        return HalpMsiAllowed;
    }

    Prcb = KeGetCurrentPrcb();
    Family = Prcb->CpuType;
    Model = (UCHAR)(Prcb->CpuStep >> 8);

    if (strncmp((PCSTR)Prcb->VendorString, "GenuineIntel", 12) == 0)
    {
        Supported = (Family >= 15) || ((Family == 6) && (Model >= 0x0D));
    }
    else if (strncmp((PCSTR)Prcb->VendorString, "AuthenticAMD", 12) == 0)
    {
        Supported = (Family >= 15);
    }
    else
    {
        Supported = FALSE;
    }

    if (HalpMessageInterruptPolicy & HALP_MESSAGE_INTERRUPTS_FORCE_ON)
    {
        Supported = TRUE;
    }
    if (HalpMessageInterruptPolicy & HALP_MESSAGE_INTERRUPTS_FORCE_OFF)
    {
        Supported = FALSE;
    }

    HalpMsiAllowed = Supported;
    HalpMsiDecided = TRUE;
    return Supported;
}

/**
 * @brief
 * Tells how the local APICs are addressed. Flat logical addressing
 * carries one bit per processor and therefore stops at eight of them;
 * larger machines fall back to physical ids and single-processor targets.
 */
static
HAL_APIC_DESTINATION_MODE
HalpGetApicDestinationMode(VOID)
{
    ULONG Count = HalpApicInfoTable.ProcessorCount;

    if (Count < (ULONG)KeNumberProcessors)
    {
        Count = KeNumberProcessors;
    }

    return (Count > 8) ? ApicDestinationModePhysical
                       : ApicDestinationModeLogicalFlat;
}

/**
 * @brief
 * Returns the local APIC id of a processor by NT number.
 */
NTSTATUS
NTAPI
HalpGetLocalApicIdForProcessor(
    _In_ ULONG ProcessorNumber,
    _Out_ PULONG ApicId)
{
    /* The running processor can always answer for itself */
    if (ProcessorNumber == KeGetCurrentProcessorNumber())
    {
        *ApicId = ApicRead(APIC_ID) >> 24;
        return STATUS_SUCCESS;
    }

    if ((ProcessorNumber >= HalpApicInfoTable.ProcessorCount) ||
        (ProcessorNumber >= MAXIMUM_PROCESSORS))
    {
        return STATUS_NOT_FOUND;
    }

    *ApicId = HalpProcessorIdentity[ProcessorNumber].LapicId;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Finds the NT number of the processor with the given local APIC id.
 */
static
NTSTATUS
HalpGetProcessorForLocalApicId(
    _In_ ULONG ApicId,
    _Out_ PULONG ProcessorNumber)
{
    ULONG i;

    if (ApicId == (ApicRead(APIC_ID) >> 24))
    {
        *ProcessorNumber = KeGetCurrentProcessorNumber();
        return STATUS_SUCCESS;
    }

    for (i = 0; i < HalpApicInfoTable.ProcessorCount; i++)
    {
        if (HalpProcessorIdentity[i].LapicId == ApicId)
        {
            *ProcessorNumber = i;
            return STATUS_SUCCESS;
        }
    }

    return STATUS_NOT_FOUND;
}

/**
 * @brief
 * Folds a processor set into an APIC destination byte. One processor is
 * addressed by its physical id, several by the union of their flat
 * logical ids (only possible for the first eight processors).
 */
NTSTATUS
NTAPI
HalpBuildInterruptDestination(
    _In_ KAFFINITY TargetProcessors,
    _Out_ PBOOLEAN Logical,
    _Out_ PUCHAR Destination)
{
    ULONG Processor, ApicId;
    UCHAR Bits = 0;
    NTSTATUS Status;

    if (TargetProcessors == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Single processor: physical delivery to its APIC id */
    if ((TargetProcessors & (TargetProcessors - 1)) == 0)
    {
        BitScanForwardAffinity(&Processor, TargetProcessors);
        Status = HalpGetLocalApicIdForProcessor(Processor, &ApicId);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        *Logical = FALSE;
        *Destination = (UCHAR)ApicId;
        return STATUS_SUCCESS;
    }

    /* Several processors: flat logical delivery, one bit each */
    if (HalpGetApicDestinationMode() != ApicDestinationModeLogicalFlat)
    {
        return STATUS_INVALID_PARAMETER;
    }

    while (TargetProcessors != 0)
    {
        BitScanForwardAffinity(&Processor, TargetProcessors);
        if (Processor >= 8)
        {
            return STATUS_INVALID_PARAMETER;
        }
        Bits |= ApicLogicalId(Processor);
        TargetProcessors &= ~((KAFFINITY)1 << Processor);
    }

    *Logical = TRUE;
    *Destination = Bits;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reserves a run of vectors for a device's messages. The run is aligned
 * to its length so a multi-message MSI device can form each message's
 * vector by adding the message number to the base. Rows are visited in
 * the same order the line-interrupt allocator uses.
 */
NTSTATUS
NTAPI
HalpAllocateMessageVectors(
    _In_ ULONG Count,
    _Out_ PUCHAR BaseVector)
{
    KIRQL OldIrql, Irql;
    ULONG Alignment, Row, LastRow, Base, i;

    if ((Count == 0) || (Count > HALP_MAX_MESSAGE_VECTORS))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Round the alignment up to a power of two */
    Alignment = 1;
    while (Alignment < Count)
    {
        Alignment <<= 1;
    }

    KeAcquireSpinLock(&HalpMessageVectorLock, &OldIrql);

    LastRow = 0;
    for (Irql = CLOCK_LEVEL - 1; Irql >= CMCI_LEVEL; Irql--)
    {
        Row = IrqlToTpr(Irql) & 0xF0;
        if (Row == LastRow)
        {
            continue;
        }
        LastRow = Row;

        for (Base = Row; Base + Count <= Row + 16; Base += Alignment)
        {
            for (i = 0; i < Count; i++)
            {
                if (HalpVectorToIndex[Base + i] != APIC_FREE_VECTOR)
                {
                    break;
                }
            }

            if (i == Count)
            {
                for (i = 0; i < Count; i++)
                {
                    HalpVectorToIndex[Base + i] = APIC_MSI_VECTOR;
                }

                KeReleaseSpinLock(&HalpMessageVectorLock, OldIrql);
                *BaseVector = (UCHAR)Base;
                return STATUS_SUCCESS;
            }
        }
    }

    KeReleaseSpinLock(&HalpMessageVectorLock, OldIrql);
    DPRINT1("No room for %lu message vector(s)\n", Count);
    return STATUS_INSUFFICIENT_RESOURCES;
}

/**
 * @brief
 * Returns a run of message vectors to the free pool.
 */
VOID
NTAPI
HalpFreeMessageVectors(
    _In_ UCHAR BaseVector,
    _In_ ULONG Count)
{
    KIRQL OldIrql;
    ULONG i;

    KeAcquireSpinLock(&HalpMessageVectorLock, &OldIrql);
    for (i = 0; (i < Count) && (BaseVector + i <= 0xFF); i++)
    {
        if (HalpVectorToIndex[BaseVector + i] == APIC_MSI_VECTOR)
        {
            HalpVectorToIndex[BaseVector + i] = APIC_FREE_VECTOR;
        }
    }
    KeReleaseSpinLock(&HalpMessageVectorLock, OldIrql);
}

/**
 * @brief
 * Private-dispatch entry that hands a device the vectors for its
 * messages. Vectors are shared by all processors on this HAL, so the
 * processor set only has to be non-empty.
 */
NTSTATUS
NTAPI
HalpAllocateMessageTarget(
    _In_ PDEVICE_OBJECT Owner,
    _In_ KAFFINITY ProcessorSet,
    _In_ ULONG NumberOfIdtEntries,
    _In_ KINTERRUPT_MODE Mode,
    _In_ BOOLEAN ShareVector,
    _Out_ PULONG Vector,
    _Out_ PKIRQL Irql,
    _Out_ PULONG IdtEntry)
{
    UCHAR Base;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Owner);
    UNREFERENCED_PARAMETER(Mode);
    UNREFERENCED_PARAMETER(ShareVector);

    if ((ProcessorSet == 0) || (NumberOfIdtEntries == 0))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!HalpMessageInterruptsAllowed())
    {
        return STATUS_NOT_SUPPORTED;
    }

    Status = HalpAllocateMessageVectors(NumberOfIdtEntries, &Base);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    *Vector = Base;
    *IdtEntry = Base;
    *Irql = HalpVectorToIrql(Base);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Private-dispatch entry that releases one message vector.
 */
VOID
NTAPI
HalpFreeMessageTarget(
    _In_ PDEVICE_OBJECT Owner,
    _In_ ULONG Vector,
    _In_ KAFFINITY ProcessorSet)
{
    UNREFERENCED_PARAMETER(Owner);
    UNREFERENCED_PARAMETER(ProcessorSet);

    if (Vector <= 0xFF)
    {
        HalpFreeMessageVectors((UCHAR)Vector, 1);
    }
}

/**
 * @brief
 * Publishes the message-target routines to the kernel unless another
 * component (an ACPI driver) already owns them.
 */
VOID
NTAPI
HalpInitializeMessageInterrupts(VOID)
{
    KeInitializeSpinLock(&HalpMessageVectorLock);

    if (HalAllocateMessageTargetOverride == NULL)
    {
        HalAllocateMessageTargetOverride = HalpAllocateMessageTarget;
        HalFreeMessageTargetOverride = HalpFreeMessageTarget;
    }
}

/* PUBLIC FUNCTIONS **********************************************************/

/**
 * @brief
 * Maps an NT processor number to the hardware (local APIC) id.
 */
NTSTATUS
NTAPI
HalGetProcessorIdByNtNumber(
    _In_ ULONG ProcessorNumber,
    _Out_ PULONG ProcessorId)
{
    if (ProcessorNumber >= (ULONG)KeNumberProcessors)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return HalpGetLocalApicIdForProcessor(ProcessorNumber, ProcessorId);
}

/**
 * @brief
 * Describes how interrupts can be targeted. The global query reports
 * whether messages are usable and how processors are addressed; the
 * per-processor query maps a local APIC id to its NT number and logical
 * address.
 *
 * @param[in] Type
 * InterruptTargetTypeGlobal for the machine-wide answer,
 * InterruptTargetTypeApic for one processor.
 *
 * @param[in] Id
 * The local APIC id for a per-processor query, ignored otherwise.
 *
 * @param[out] Information
 * Receives the answer.
 */
NTSTATUS
NTAPI
HalGetInterruptTargetInformation(
    _In_ HAL_INTERRUPT_TARGET_TYPE Type,
    _In_ ULONG Id,
    _Out_ PHAL_INTERRUPT_TARGET_INFORMATION Information)
{
    HAL_APIC_DESTINATION_MODE Mode;
    ULONG Processor;
    NTSTATUS Status;

    if ((Type != InterruptTargetTypeGlobal) && (Type != InterruptTargetTypeApic))
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(Information, sizeof(*Information));
    Information->Type = Type;
    Mode = HalpGetApicDestinationMode();
    Information->Apic.DestinationMode = Mode;

    if (HalpMessageInterruptsAllowed())
    {
        Information->Flags |= HAL_INTERRUPT_TARGET_MSI_SUPPORTED;
    }

    /* Processors never move, so their addresses are fixed */
    Information->Flags |= HAL_INTERRUPT_TARGET_STATIC_DESTINATIONS;

    if (Type == InterruptTargetTypeGlobal)
    {
        return STATUS_SUCCESS;
    }

    Status = HalpGetProcessorForLocalApicId(Id, &Processor);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Information->ProcessorNumber.Group = 0;
    Information->ProcessorNumber.Number = (UCHAR)Processor;

    if ((Mode == ApicDestinationModeLogicalFlat) && (Processor < 8))
    {
        Information->Flags |= HAL_INTERRUPT_TARGET_LOGICAL_ID_VALID;
        Information->Apic.LogicalApicId = ApicLogicalId(Processor);
    }
    else
    {
        Information->Apic.DestinationMode = ApicDestinationModePhysical;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Turns a message request (vector, processor set, addressing mode) into
 * the address/data pair a device writes to raise that message. A
 * request of type InterruptTargetTypeApicRequest is only validated and
 * echoed back as a MessageRequest element.
 *
 * @param[in] Request
 * The message to route.
 *
 * @param[out] ConnectionData
 * Receives a one-element connection data block describing the message.
 */
NTSTATUS
NTAPI
HalGetMessageRoutingInfo(
    _In_ PHAL_MESSAGE_TARGET_REQUEST Request,
    _Out_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    PINTERRUPT_VECTOR_DATA VectorData;
    HAL_APIC_DESTINATION_MODE Mode;
    KAFFINITY Targets;
    BOOLEAN Logical, MultiTarget;
    UCHAR Destination;
    ULONG Address, Data;
    NTSTATUS Status;

    if ((Request->Type != InterruptTargetTypeApic) &&
        (Request->Type != InterruptTargetTypeApicRequest))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if ((Request->Apic.Vector > 0xFF) ||
        (Request->Apic.TargetProcessors.Group != 0) ||
        (Request->Apic.TargetProcessors.Mask == 0))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Targets = Request->Apic.TargetProcessors.Mask;
    MultiTarget = ((Targets & (Targets - 1)) != 0);
    Mode = Request->Apic.DestinationMode;

    /* The requested addressing must be one this machine offers */
    switch (Mode)
    {
        case ApicDestinationModePhysical:
            if (MultiTarget)
            {
                return STATUS_INVALID_PARAMETER;
            }
            break;

        case ApicDestinationModeLogicalFlat:
            if (HalpGetApicDestinationMode() != ApicDestinationModeLogicalFlat)
            {
                return STATUS_INVALID_PARAMETER;
            }
            break;

        default:
            return STATUS_INVALID_PARAMETER;
    }

    Status = HalpBuildInterruptDestination(Targets, &Logical, &Destination);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /* A flat request for a single processor is still delivered by
       logical id, as the caller asked */
    if ((Mode == ApicDestinationModeLogicalFlat) && !Logical)
    {
        ULONG Processor;

        BitScanForwardAffinity(&Processor, Targets);
        if (Processor >= 8)
        {
            return STATUS_INVALID_PARAMETER;
        }
        Logical = TRUE;
        Destination = ApicLogicalId(Processor);
    }

    RtlZeroMemory(ConnectionData, FIELD_OFFSET(INTERRUPT_CONNECTION_DATA, Vectors[1]));
    ConnectionData->Count = 1;
    VectorData = &ConnectionData->Vectors[0];
    VectorData->Vector = Request->Apic.Vector;
    VectorData->Irql = HalpVectorToIrql((UCHAR)Request->Apic.Vector);
    VectorData->Polarity = InterruptActiveHigh;
    VectorData->Mode = Latched;
    VectorData->TargetProcessors = Request->Apic.TargetProcessors;

    if (Request->Type == InterruptTargetTypeApicRequest)
    {
        VectorData->Type = InterruptTypeMessageRequest;
        VectorData->MessageRequest.DestinationMode = Mode;
        return STATUS_SUCCESS;
    }

    Address = APIC_MSI_ADDRESS_BASE | ((ULONG)Destination << 12);
    Data = Request->Apic.Vector | APIC_MSI_DATA_ASSERT;
    if (Logical)
    {
        Address |= APIC_MSI_ADDRESS_LOGICAL;
        Data |= APIC_MSI_DATA_LOGICAL;
        if (MultiTarget)
        {
            Address |= APIC_MSI_ADDRESS_REDIRHINT;
            Data |= APIC_MSI_DATA_LOWEST_PRIORITY;
        }
    }

    VectorData->Type = InterruptTypeXapicMessage;
    VectorData->XapicMessage.Address.QuadPart = Address;
    VectorData->XapicMessage.DataPayload = Data;
    VectorData->IntRemapInfo.FlagTranslated = 1;
    VectorData->IntRemapInfo.u.Msi.MessageAddressLow = Address;
    VectorData->IntRemapInfo.u.Msi.MessageData = (USHORT)Data;

    return STATUS_SUCCESS;
}

/* EOF */
