/*
 * PROJECT:     ReactOS PCI Bus Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Message Signaled Interrupt Support
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

#define PCI_CONNECTION_DATA_SIZE(Count)                 \
    (FIELD_OFFSET(INTERRUPT_CONNECTION_DATA, Vectors) + \
     ((Count) * sizeof(INTERRUPT_VECTOR_DATA)))

/* The interrupt arbiter publishes the messages it granted under this property */
static const DEVPROPKEY PciInterruptConnectionDataKey =
{
    { 0xF0E20F09, 0xD97A, 0x49A9, { 0x80, 0x46, 0xBB, 0x6E, 0x22, 0xE6, 0xBB, 0x2E } },
    2
};

typedef NTSTATUS
(NTAPI *PCI_GET_DEVICE_PROPERTY_DATA)(
    _In_ PDEVICE_OBJECT Pdo,
    _In_ CONST DEVPROPKEY *PropertyKey,
    _In_ LCID Lcid,
    _In_ ULONG Flags,
    _In_ ULONG Size,
    _Out_writes_bytes_opt_(Size) PVOID Data,
    _Out_ PULONG RequiredSize,
    _Out_ PDEVPROPTYPE Type);

static PCI_GET_DEVICE_PROPERTY_DATA PciGetDevicePropertyDataRoutine;
static BOOLEAN PciGetDevicePropertyDataResolved;

/* FUNCTIONS ******************************************************************/

/* IoGetDevicePropertyData is newer than this driver's import floor */
static
PCI_GET_DEVICE_PROPERTY_DATA
NTAPI
PciLocateGetDevicePropertyData(VOID)
{
    UNICODE_STRING RoutineName;

    if (PciGetDevicePropertyDataResolved)
        return PciGetDevicePropertyDataRoutine;

    RtlInitUnicodeString(&RoutineName, L"IoGetDevicePropertyData");
    PciGetDevicePropertyDataRoutine = MmGetSystemRoutineAddress(&RoutineName);
    PciGetDevicePropertyDataResolved = TRUE;

    if (!PciGetDevicePropertyDataRoutine)
        DPRINT1("PCI: IoGetDevicePropertyData is not available\n");

    return PciGetDevicePropertyDataRoutine;
}

/* The caller frees the returned block */
static
NTSTATUS
NTAPI
PciQueryGrantedMessages(
    _In_ PPCI_PDO_EXTENSION PdoExtension,
    _Outptr_ PINTERRUPT_CONNECTION_DATA *ConnectionData)
{
    PCI_GET_DEVICE_PROPERTY_DATA GetPropertyData;
    PINTERRUPT_CONNECTION_DATA Data;
    DEVPROPTYPE PropertyType;
    NTSTATUS Status;
    ULONG Size, MaxCount;
    PAGED_CODE();

    *ConnectionData = NULL;

    GetPropertyData = PciLocateGetDevicePropertyData();
    if (!GetPropertyData)
        return STATUS_NOT_SUPPORTED;

    Size = 0;
    Status = GetPropertyData(PdoExtension->PhysicalDeviceObject,
                             &PciInterruptConnectionDataKey,
                             0,
                             0,
                             0,
                             NULL,
                             &Size,
                             &PropertyType);
    if (Status != STATUS_BUFFER_TOO_SMALL)
        return NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;

    if (Size < PCI_CONNECTION_DATA_SIZE(1))
        return STATUS_DATA_ERROR;

    Data = ExAllocatePoolWithTag(PagedPool, Size, PCI_POOL_TAG);
    if (!Data)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = GetPropertyData(PdoExtension->PhysicalDeviceObject,
                             &PciInterruptConnectionDataKey,
                             0,
                             0,
                             Size,
                             Data,
                             &Size,
                             &PropertyType);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Data, 0);
        return Status;
    }

    /* Never trust the count past the size of the property */
    MaxCount = (Size - FIELD_OFFSET(INTERRUPT_CONNECTION_DATA, Vectors)) /
               sizeof(INTERRUPT_VECTOR_DATA);
    if (!Data->Count || (Data->Count > MaxCount))
    {
        DPRINT1("PCI: Connection data for %p is short (%lu bytes, %lu vectors)\n",
                PdoExtension,
                Size,
                Data->Count);
        ExFreePoolWithTag(Data, 0);
        return STATUS_DATA_ERROR;
    }

    *ConnectionData = Data;
    return STATUS_SUCCESS;
}

static
BOOLEAN
NTAPI
PciGetMsiXCapability(
    _Inout_ PPCI_PDO_EXTENSION PdoExtension)
{
    PPCI_MESSAGE_INFO MessageInfo = &PdoExtension->MessageInfo;
    PCI_CAPABILITIES_HEADER Header;
    USHORT Control;
    ULONG TableRegister;
    UCHAR Offset;
    PAGED_CODE();

    Offset = PciReadDeviceCapability(PdoExtension,
                                     PdoExtension->CapabilitiesPtr,
                                     PCI_CAPABILITY_ID_MSIX,
                                     &Header,
                                     sizeof(Header));
    if (!Offset)
        return FALSE;

    PciReadDeviceConfig(PdoExtension,
                        &Control,
                        Offset + PCI_MESSAGE_CONTROL_OFFSET,
                        sizeof(Control));
    PciReadDeviceConfig(PdoExtension,
                        &TableRegister,
                        Offset + PCI_MSIX_TABLE_OFFSET,
                        sizeof(TableRegister));

    /* The table size field is encoded as one less than the entry count */
    MessageInfo->Type = PciMessageMsiX;
    MessageInfo->CapabilityPtr = Offset;
    MessageInfo->RequestedCount = (Control & PCI_MSIX_CONTROL_TABLE_SIZE_MASK) + 1;
    MessageInfo->TableBarIndex = (UCHAR)(TableRegister & PCI_MSIX_BIR_MASK);
    MessageInfo->TableBarOffset = TableRegister & PCI_MSIX_OFFSET_MASK;
    MessageInfo->Is64Bit = TRUE;
    MessageInfo->MaskCapable = TRUE;

    DPRINT1("PCI: MSI-X at 0x%x, %u message(s), table in BAR %u at 0x%lx\n",
            Offset,
            MessageInfo->RequestedCount,
            MessageInfo->TableBarIndex,
            MessageInfo->TableBarOffset);
    return TRUE;
}

static
VOID
NTAPI
PciGetMsiCapability(
    _Inout_ PPCI_PDO_EXTENSION PdoExtension)
{
    PPCI_MESSAGE_INFO MessageInfo = &PdoExtension->MessageInfo;
    PCI_CAPABILITIES_HEADER Header;
    USHORT Control, CapableShift;
    UCHAR Offset;
    PAGED_CODE();

    Offset = PciReadDeviceCapability(PdoExtension,
                                     PdoExtension->CapabilitiesPtr,
                                     PCI_CAPABILITY_ID_MSI,
                                     &Header,
                                     sizeof(Header));
    if (!Offset)
        return;

    PciReadDeviceConfig(PdoExtension,
                        &Control,
                        Offset + PCI_MESSAGE_CONTROL_OFFSET,
                        sizeof(Control));

    /* The capable field is a power of two, and anything past 32 messages is reserved */
    CapableShift = (Control & PCI_MSI_CONTROL_MMC_MASK) >> PCI_MSI_CONTROL_MMC_SHIFT;
    if (CapableShift > PCI_MSI_MAX_MESSAGE_SHIFT)
        CapableShift = PCI_MSI_MAX_MESSAGE_SHIFT;

    MessageInfo->Type = PciMessageMsi;
    MessageInfo->CapabilityPtr = Offset;
    MessageInfo->RequestedCount = (USHORT)(1 << CapableShift);
    MessageInfo->Is64Bit = (Control & PCI_MSI_CONTROL_64BIT) != 0;
    MessageInfo->MaskCapable = (Control & PCI_MSI_CONTROL_MASKING) != 0;

    DPRINT1("PCI: MSI at 0x%x, %u message(s), %s address\n",
            Offset,
            MessageInfo->RequestedCount,
            MessageInfo->Is64Bit ? "64-bit" : "32-bit");
}

/**
 * @brief
 * Records which kind of message-signaled interrupt a new function supports.
 * MSI-X is preferred, since each of its messages can be targeted on its own.
 *
 * @param[in,out] PdoExtension
 * The PDO extension of the function being enumerated.
 */
VOID
NTAPI
PciGetMessageCapabilities(
    _Inout_ PPCI_PDO_EXTENSION PdoExtension)
{
    PAGED_CODE();

    RtlZeroMemory(&PdoExtension->MessageInfo, sizeof(PdoExtension->MessageInfo));
    PdoExtension->MessageInfo.Type = PciMessageNone;

    if (!PdoExtension->CapabilitiesPtr)
        return;

    if (PciGetMsiXCapability(PdoExtension))
        return;

    PciGetMsiCapability(PdoExtension);
}

/* Turns one granted message into the address and data the device writes to raise it */
static
NTSTATUS
NTAPI
PciGetMessageAddressAndData(
    _In_ PINTERRUPT_VECTOR_DATA VectorData,
    _Out_ PPHYSICAL_ADDRESS Address,
    _Out_ PULONG Data)
{
    HAL_MESSAGE_TARGET_REQUEST Request;
    INTERRUPT_CONNECTION_DATA Routed;
    NTSTATUS Status;

    if (VectorData->Type == InterruptTypeXapicMessage)
    {
        *Address = VectorData->XapicMessage.Address;
        *Data = VectorData->XapicMessage.DataPayload;
        return STATUS_SUCCESS;
    }

    /* HyperTransport would need an MSI mapping capability this driver does not program */
    if (VectorData->Type != InterruptTypeMessageRequest)
        return STATUS_NOT_SUPPORTED;

    RtlZeroMemory(&Request, sizeof(Request));
    Request.Type = InterruptTargetTypeApic;
    Request.Apic.Vector = VectorData->Vector;
    Request.Apic.TargetProcessors = VectorData->TargetProcessors;
    Request.Apic.DestinationMode = VectorData->MessageRequest.DestinationMode;
    Request.Apic.IntRemapInfo = VectorData->IntRemapInfo;

    RtlZeroMemory(&Routed, sizeof(Routed));
    Status = HalGetMessageRoutingInfo(&Request, &Routed);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Routed.Vectors[0].Type != InterruptTypeXapicMessage)
        return STATUS_UNSUCCESSFUL;

    *Address = Routed.Vectors[0].XapicMessage.Address;
    *Data = Routed.Vectors[0].XapicMessage.DataPayload;
    return STATUS_SUCCESS;
}

/* Some functions only raise messages with the legacy interrupt disable bit clear */
static
VOID
NTAPI
PciApplyMessageCommandHack(
    _In_ PPCI_PDO_EXTENSION PdoExtension)
{
    USHORT Command;

    if (!(PdoExtension->HackFlags & PCI_HACK_CLEAR_INT_DISABLE_FOR_MSI))
        return;

    PciReadDeviceConfig(PdoExtension,
                        &Command,
                        FIELD_OFFSET(PCI_COMMON_HEADER, Command),
                        sizeof(Command));
    Command &= ~PCI_DISABLE_LEVEL_INTERRUPT;
    PciWriteDeviceConfig(PdoExtension,
                         &Command,
                         FIELD_OFFSET(PCI_COMMON_HEADER, Command),
                         sizeof(Command));
}

/* MSI has one address and data pair, and message N is raised with N added to the data */
static
NTSTATUS
NTAPI
PciProgramMsi(
    _Inout_ PPCI_PDO_EXTENSION PdoExtension,
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData,
    _In_ ULONG Count)
{
    PPCI_MESSAGE_INFO MessageInfo = &PdoExtension->MessageInfo;
    PHYSICAL_ADDRESS Address;
    NTSTATUS Status;
    USHORT Control;
    ULONG Data, Mask, DataOffset, EnableShift, Enabled;
    UCHAR Offset;
    PAGED_CODE();

    Status = PciGetMessageAddressAndData(&ConnectionData->Vectors[0], &Address, &Data);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Only a power of two run of messages can be enabled, so round the grant down */
    EnableShift = 0;
    while ((EnableShift < PCI_MSI_MAX_MESSAGE_SHIFT) && ((2UL << EnableShift) <= Count))
        EnableShift++;
    Enabled = 1UL << EnableShift;

    Offset = MessageInfo->CapabilityPtr;
    PciReadDeviceConfig(PdoExtension,
                        &Control,
                        Offset + PCI_MESSAGE_CONTROL_OFFSET,
                        sizeof(Control));

    /* Keep it off while the address and data change */
    Control &= ~PCI_MSI_CONTROL_ENABLE;
    PciWriteDeviceConfig(PdoExtension,
                         &Control,
                         Offset + PCI_MESSAGE_CONTROL_OFFSET,
                         sizeof(Control));

    PciWriteDeviceConfig(PdoExtension,
                         &Address.LowPart,
                         Offset + PCI_MSI_ADDRESS_OFFSET,
                         sizeof(ULONG));

    DataOffset = PCI_MSI_ADDRESS_OFFSET + sizeof(ULONG);
    if (MessageInfo->Is64Bit)
    {
        PciWriteDeviceConfig(PdoExtension,
                             &Address.HighPart,
                             Offset + DataOffset,
                             sizeof(ULONG));
        DataOffset += sizeof(ULONG);
    }

    PciWriteDeviceConfig(PdoExtension, &Data, Offset + DataOffset, sizeof(USHORT));

    /* The mask register follows the data register and its reserved word */
    if (MessageInfo->MaskCapable)
    {
        Mask = (Enabled >= 32) ? 0 : ~((1UL << Enabled) - 1);
        PciWriteDeviceConfig(PdoExtension,
                             &Mask,
                             Offset + DataOffset + sizeof(ULONG),
                             sizeof(ULONG));
    }

    Control &= ~PCI_MSI_CONTROL_MME_MASK;
    Control |= (USHORT)(EnableShift << PCI_MSI_CONTROL_MME_SHIFT);
    Control |= PCI_MSI_CONTROL_ENABLE;
    PciWriteDeviceConfig(PdoExtension,
                         &Control,
                         Offset + PCI_MESSAGE_CONTROL_OFFSET,
                         sizeof(Control));

    PciApplyMessageCommandHack(PdoExtension);

    MessageInfo->GrantedCount = (USHORT)Enabled;
    DPRINT1("PCI: MSI enabled on %p, %lu message(s) at 0x%08lx data 0x%lx\n",
            PdoExtension,
            Enabled,
            Address.LowPart,
            Data);
    return STATUS_SUCCESS;
}

/* Every table entry is written, and entries past the grant share the first message */
static
NTSTATUS
NTAPI
PciProgramMsiX(
    _Inout_ PPCI_PDO_EXTENSION PdoExtension,
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData,
    _In_ ULONG Count)
{
    PPCI_MESSAGE_INFO MessageInfo = &PdoExtension->MessageInfo;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR TableBar;
    PPCI_MSIX_VECTOR Table;
    PHYSICAL_ADDRESS Address, TableAddress;
    NTSTATUS Status;
    USHORT Control;
    ULONG Data, Index, Message, TableSize, EntryControl;
    UCHAR Offset;
    PAGED_CODE();

    if (!PdoExtension->Resources || (MessageInfo->TableBarIndex >= PCI_TYPE0_ADDRESSES))
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    TableBar = &PdoExtension->Resources->Current[MessageInfo->TableBarIndex];
    if (TableBar->Type != CmResourceTypeMemory)
    {
        DPRINT1("PCI: MSI-X table BAR %u of %p is not memory\n",
                MessageInfo->TableBarIndex,
                PdoExtension);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    TableSize = MessageInfo->RequestedCount * sizeof(PCI_MSIX_VECTOR);
    if ((MessageInfo->TableBarOffset > TableBar->u.Memory.Length) ||
        (TableSize > (TableBar->u.Memory.Length - MessageInfo->TableBarOffset)))
    {
        DPRINT1("PCI: MSI-X table of %p does not fit in its BAR\n", PdoExtension);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    TableAddress.QuadPart = TableBar->u.Memory.Start.QuadPart + MessageInfo->TableBarOffset;
    Table = MmMapIoSpace(TableAddress, TableSize, MmNonCached);
    if (!Table)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Enable with the whole function masked so no entry fires half written */
    Offset = MessageInfo->CapabilityPtr;
    PciReadDeviceConfig(PdoExtension,
                        &Control,
                        Offset + PCI_MESSAGE_CONTROL_OFFSET,
                        sizeof(Control));
    Control |= PCI_MSIX_CONTROL_FUNCTION_MASK | PCI_MSIX_CONTROL_ENABLE;
    PciWriteDeviceConfig(PdoExtension,
                         &Control,
                         Offset + PCI_MESSAGE_CONTROL_OFFSET,
                         sizeof(Control));

    Status = STATUS_SUCCESS;
    for (Index = 0; Index < MessageInfo->RequestedCount; Index++)
    {
        Message = (Index < Count) ? Index : 0;
        Status = PciGetMessageAddressAndData(&ConnectionData->Vectors[Message],
                                             &Address,
                                             &Data);
        if (!NT_SUCCESS(Status))
            break;

        WRITE_REGISTER_ULONG(&Table[Index].AddressLowPart, Address.LowPart);
        WRITE_REGISTER_ULONG(&Table[Index].AddressHighPart, (ULONG)Address.HighPart);
        WRITE_REGISTER_ULONG(&Table[Index].Data, Data);

        /* Only the mask bit is ours, the rest of the control word must be preserved */
        EntryControl = READ_REGISTER_ULONG(&Table[Index].Control);
        WRITE_REGISTER_ULONG(&Table[Index].Control,
                             EntryControl & ~PCI_MSIX_VECTOR_CONTROL_MASK);
    }

    MmUnmapIoSpace(Table, TableSize);

    if (!NT_SUCCESS(Status))
    {
        Control &= ~PCI_MSIX_CONTROL_ENABLE;
        PciWriteDeviceConfig(PdoExtension,
                             &Control,
                             Offset + PCI_MESSAGE_CONTROL_OFFSET,
                             sizeof(Control));
        return Status;
    }

    Control &= ~PCI_MSIX_CONTROL_FUNCTION_MASK;
    PciWriteDeviceConfig(PdoExtension,
                         &Control,
                         Offset + PCI_MESSAGE_CONTROL_OFFSET,
                         sizeof(Control));

    PciApplyMessageCommandHack(PdoExtension);

    MessageInfo->GrantedCount = (USHORT)Count;
    DPRINT1("PCI: MSI-X enabled on %p, %lu message(s)\n", PdoExtension, Count);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Programs the messages the interrupt arbiter granted into the function.
 *
 * @param[in,out] PdoExtension
 * The PDO extension of the function being started.
 *
 * @param[in] Resource
 * The message interrupt resource from the start request, if one was found.
 *
 * @return
 * STATUS_SUCCESS if the function was programmed.
 */
NTSTATUS
NTAPI
PciProgramMessageInterrupt(
    _Inout_ PPCI_PDO_EXTENSION PdoExtension,
    _In_opt_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource)
{
    PPCI_MESSAGE_INFO MessageInfo = &PdoExtension->MessageInfo;
    PINTERRUPT_CONNECTION_DATA ConnectionData;
    NTSTATUS Status;
    ULONG Count, Granted;
    PAGED_CODE();

    if (MessageInfo->Type == PciMessageNone)
        return STATUS_NOT_SUPPORTED;

    Status = PciQueryGrantedMessages(PdoExtension, &ConnectionData);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("PCI: No granted messages for %p (0x%lx)\n", PdoExtension, Status);
        return Status;
    }

    /* Never tell the device about a message that has no vector behind it */
    Count = ConnectionData->Count;
    if (Count > MessageInfo->RequestedCount)
        Count = MessageInfo->RequestedCount;

    /* The arbiter packs the message count into the upper word of the level */
    if (Resource)
    {
        Granted = Resource->u.Interrupt.Level >> 16;
        if (Granted && (Granted < Count))
            Count = Granted;
    }

    if (MessageInfo->Type == PciMessageMsiX)
        Status = PciProgramMsiX(PdoExtension, ConnectionData, Count);
    else
        Status = PciProgramMsi(PdoExtension, ConnectionData, Count);

    ExFreePoolWithTag(ConnectionData, 0);
    return Status;
}

/**
 * @brief
 * Turns off message interrupts on a function that has them enabled.
 *
 * @param[in,out] PdoExtension
 * The PDO extension of the function being stopped.
 */
VOID
NTAPI
PciDisableMessageInterrupt(
    _Inout_ PPCI_PDO_EXTENSION PdoExtension)
{
    PPCI_MESSAGE_INFO MessageInfo = &PdoExtension->MessageInfo;
    USHORT Control, EnableBit;
    PAGED_CODE();

    if ((MessageInfo->Type == PciMessageNone) || !MessageInfo->GrantedCount)
        return;

    if (MessageInfo->Type == PciMessageMsiX)
        EnableBit = PCI_MSIX_CONTROL_ENABLE;
    else
        EnableBit = PCI_MSI_CONTROL_ENABLE;

    PciReadDeviceConfig(PdoExtension,
                        &Control,
                        MessageInfo->CapabilityPtr + PCI_MESSAGE_CONTROL_OFFSET,
                        sizeof(Control));
    Control &= ~EnableBit;
    PciWriteDeviceConfig(PdoExtension,
                         &Control,
                         MessageInfo->CapabilityPtr + PCI_MESSAGE_CONTROL_OFFSET,
                         sizeof(Control));

    MessageInfo->GrantedCount = 0;
}

/* EOF */
