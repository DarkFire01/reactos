/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Message-signaled interrupt support for the APIC HAL
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#include "apicp.h"
#include <smp.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

/* Structure layouts shared with the kernel and bus drivers */
#ifdef _M_IX86
C_ASSERT(sizeof(INTERRUPT_VECTOR_DATA) == 80);
C_ASSERT(FIELD_OFFSET(INTERRUPT_CONNECTION_DATA, Vectors) == 8);
C_ASSERT(sizeof(HAL_INTERRUPT_TARGET_INFORMATION) == 24);
C_ASSERT(FIELD_OFFSET(HAL_MESSAGE_TARGET_REQUEST, Apic.DestinationMode) == 0x18);
#endif

extern const PPROCESSOR_IDENTITY HalpProcessorIdentity;
extern HALP_APIC_INFO_TABLE HalpApicInfoTable;

#define HALP_MSI_STATE_UNKNOWN  0
#define HALP_MSI_STATE_DISABLED 1
#define HALP_MSI_STATE_ENABLED  2
static UCHAR HalpMessageInterruptState = HALP_MSI_STATE_UNKNOWN;

static KSPIN_LOCK HalpMessageVectorLock;

/* A block of message vectors must fit in one priority row */
#define HALP_MAX_MESSAGE_VECTORS 16

/* PRIVATE FUNCTIONS *********************************************************/

static
BOOLEAN
HalpCpuSupportsMessageInterrupts(VOID)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    BOOLEAN IsIntel, IsAmd;
    UCHAR Family, Model;

    IsIntel = (strncmp((PCSTR)Prcb->VendorString, "GenuineIntel", 12) == 0);
    IsAmd = (strncmp((PCSTR)Prcb->VendorString, "AuthenticAMD", 12) == 0);
    if (!IsIntel && !IsAmd)
    {
        return FALSE;
    }

    Family = Prcb->CpuType;
    if (Family >= 15)
    {
        return TRUE;
    }

    /* Intel Pentium M and newer family 6 processors */
    Model = (UCHAR)(Prcb->CpuStep >> 8);
    return IsIntel && (Family == 6) && (Model >= 0x0D);
}

/**
 * @brief
 * Checks if message-signaled interrupts can be used on this machine.
 * The NOMSI and FORCEMSI boot options override the processor check.
 */
BOOLEAN
NTAPI
HalpMessageInterruptsAllowed(VOID)
{
    BOOLEAN Allowed;

    if (HalpMessageInterruptState == HALP_MSI_STATE_UNKNOWN)
    {
        if (HalpMessageInterruptPolicy & HALP_MESSAGE_INTERRUPTS_FORCE_OFF)
            Allowed = FALSE;
        else if (HalpMessageInterruptPolicy & HALP_MESSAGE_INTERRUPTS_FORCE_ON)
            Allowed = TRUE;
        else
            Allowed = HalpCpuSupportsMessageInterrupts();

        HalpMessageInterruptState = Allowed ? HALP_MSI_STATE_ENABLED :
                                              HALP_MSI_STATE_DISABLED;
    }

    return (HalpMessageInterruptState == HALP_MSI_STATE_ENABLED);
}

/* Flat logical mode has one bit per processor, so it only covers 8 of them */
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

NTSTATUS
NTAPI
HalpGetLocalApicIdForProcessor(
    _In_ ULONG ProcessorNumber,
    _Out_ PULONG ApicId)
{
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
 * Converts a processor set to an APIC destination. A single processor uses
 * its physical APIC ID, several processors use their flat logical IDs.
 */
NTSTATUS
NTAPI
HalpBuildInterruptDestination(
    _In_ KAFFINITY TargetProcessors,
    _Out_ PBOOLEAN Logical,
    _Out_ PUCHAR Destination)
{
    ULONG Processor, ApicId;
    NTSTATUS Status;

    if (TargetProcessors == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

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

    /* The flat logical ID of processor N is bit N */
    if ((HalpGetApicDestinationMode() != ApicDestinationModeLogicalFlat) ||
        (TargetProcessors > 0xFF))
    {
        return STATUS_INVALID_PARAMETER;
    }

    *Logical = TRUE;
    *Destination = (UCHAR)TargetProcessors;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Allocates a block of vectors for message-signaled interrupts. The block
 * is aligned to its size, so MSI devices can add the message number to the
 * base vector.
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

    /* Round up to a power of two */
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
 * Private dispatch routine that allocates message vectors for a device.
 * Vectors are valid on all processors, so the processor set is only
 * checked for being non-empty.
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

/* Private dispatch routine that frees one message vector */
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

VOID
NTAPI
HalpInitializeMessageInterrupts(VOID)
{
    KeInitializeSpinLock(&HalpMessageVectorLock);

    /* Don't replace the routines if the ACPI driver installed its own */
    if (HalAllocateMessageTargetOverride == NULL)
    {
        HalAllocateMessageTargetOverride = HalpAllocateMessageTarget;
        HalFreeMessageTargetOverride = HalpFreeMessageTarget;
    }
}

/* PUBLIC FUNCTIONS **********************************************************/

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
 * Returns interrupt targeting information for the whole machine, or for the
 * processor with the given local APIC ID.
 *
 * @param[in] Type
 * InterruptTargetTypeGlobal or InterruptTargetTypeApic.
 *
 * @param[in] Id
 * Local APIC ID of the processor, only used with InterruptTargetTypeApic.
 *
 * @param[out] Information
 * Receives the targeting information.
 */
NTSTATUS
NTAPI
HalGetInterruptTargetInformation(
    _In_ HAL_INTERRUPT_TARGET_TYPE Type,
    _In_ ULONG Id,
    _Out_ PHAL_INTERRUPT_TARGET_INFORMATION Information)
{
    HAL_APIC_DESTINATION_MODE Mode;
    ULONG Flags, Processor;
    NTSTATUS Status;

    if ((Type != InterruptTargetTypeGlobal) && (Type != InterruptTargetTypeApic))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Mode = HalpGetApicDestinationMode();

    /* Processors are never replaced, so destinations don't change */
    Flags = HAL_INTERRUPT_TARGET_STATIC_DESTINATIONS;
    if (HalpMessageInterruptsAllowed())
    {
        Flags |= HAL_INTERRUPT_TARGET_MSI_SUPPORTED;
    }

    RtlZeroMemory(Information, sizeof(*Information));
    Information->Type = Type;
    Information->Flags = Flags;
    Information->Apic.DestinationMode = Mode;

    if (Type == InterruptTargetTypeApic)
    {
        Status = HalpGetProcessorForLocalApicId(Id, &Processor);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

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
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Builds the MSI address and data for a vector and processor set.
 * For InterruptTargetTypeApicRequest the request is only validated and
 * returned as a message request.
 *
 * @param[in] Request
 * The vector, processor set and destination mode.
 *
 * @param[out] ConnectionData
 * Receives a connection data block with one vector.
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

    if (Mode == ApicDestinationModePhysical)
    {
        if (MultiTarget)
        {
            return STATUS_INVALID_PARAMETER;
        }
    }
    else if (Mode == ApicDestinationModeLogicalFlat)
    {
        if (HalpGetApicDestinationMode() != ApicDestinationModeLogicalFlat)
        {
            return STATUS_INVALID_PARAMETER;
        }
    }
    else
    {
        return STATUS_INVALID_PARAMETER;
    }

    Status = HalpBuildInterruptDestination(Targets, &Logical, &Destination);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /* Flat mode uses logical IDs even for a single processor */
    if (Mode == ApicDestinationModeLogicalFlat)
    {
        if (Targets > 0xFF)
        {
            return STATUS_INVALID_PARAMETER;
        }
        Logical = TRUE;
        Destination = (UCHAR)Targets;
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
