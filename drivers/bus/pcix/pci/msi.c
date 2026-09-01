/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/pci/msi.c
 * PURPOSE:         Message-Signalled Interrupt Support
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/*
 * A message-signalled interrupt is a posted write the device makes to an
 * address the interrupt controller watches; the data written names the vector.
 * Nothing is wired, so there is no line to route and no line to share.
 *
 * Three parties are involved. The device says how many messages it wants, in
 * its MSI or MSI-X capability. The interrupt arbiter places that request and
 * publishes what it granted as the device's INTERRUPT_CONNECTION_DATA property:
 * one element per message, each naming a vector, the processors it may be
 * delivered to, and how those processors are addressed. This driver is the
 * third party: it turns each of those elements into the address/data pair the
 * hardware needs - which only the HAL can compute - and writes them into the
 * device, because the capability registers belong to the bus driver alone.
 *
 * MSI and MSI-X differ in where those pairs live. MSI keeps one address and one
 * data value in configuration space and the device derives the rest by counting
 * up from it, so its messages must be a power-of-two run of consecutive
 * vectors. MSI-X keeps a full table in device memory with one independent
 * entry per message, so its messages need not be related at all.
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/*
 * The property the interrupt arbiter publishes a device's granted messages
 * under. Reading it needs a kernel routine above this driver's import floor,
 * so it is resolved by name on first use.
 */
static const DEVPROPKEY PciInterruptConnectionDataKey =
{
    { 0xF0E20F09, 0xD97A, 0x49A9, { 0x80, 0x46, 0xBB, 0x6E, 0x22, 0xE6, 0xBB, 0x2E } },
    1
};

typedef NTSTATUS
(NTAPI *PCI_GET_DEVICE_PROPERTY_DATA)(
    IN PDEVICE_OBJECT Pdo,
    IN CONST DEVPROPKEY *PropertyKey,
    IN LCID Lcid,
    IN ULONG Flags,
    IN ULONG Size,
    OUT PVOID Data,
    OUT PULONG RequiredSize,
    OUT PDEVPROPTYPE Type
);

static PCI_GET_DEVICE_PROPERTY_DATA PciGetDevicePropertyDataRoutine;
static BOOLEAN PciGetDevicePropertyDataResolved;

/* FUNCTIONS ******************************************************************/

static
PCI_GET_DEVICE_PROPERTY_DATA
NTAPI
PciLocateGetDevicePropertyData(VOID)
{
    UNICODE_STRING RoutineName;

    if (!PciGetDevicePropertyDataResolved)
    {
        RtlInitUnicodeString(&RoutineName, L"IoGetDevicePropertyData");
        PciGetDevicePropertyDataRoutine = MmGetSystemRoutineAddress(&RoutineName);
        PciGetDevicePropertyDataResolved = TRUE;

        if (!PciGetDevicePropertyDataRoutine)
        {
            DPRINT1("PCI - no IoGetDevicePropertyData, messages unavailable\n");
        }
    }

    return PciGetDevicePropertyDataRoutine;
}

/*
 * Fetch the messages the interrupt arbiter granted this device. The caller owns
 * the returned block.
 */
static
NTSTATUS
NTAPI
PciGetInterruptConnectionData(IN PPCI_PDO_EXTENSION PdoExtension,
                              OUT PINTERRUPT_CONNECTION_DATA *ConnectionData)
{
    PCI_GET_DEVICE_PROPERTY_DATA GetPropertyData;
    PINTERRUPT_CONNECTION_DATA Data;
    DEVPROPTYPE PropertyType;
    NTSTATUS Status;
    ULONG Length;
    PAGED_CODE();

    *ConnectionData = NULL;

    GetPropertyData = PciLocateGetDevicePropertyData();
    if (!GetPropertyData) return STATUS_NOT_SUPPORTED;

    /* Ask for the size first, since it grows with the message count */
    Length = 0;
    Status = GetPropertyData(PdoExtension->PhysicalDeviceObject,
                             &PciInterruptConnectionDataKey,
                             0,
                             0,
                             0,
                             NULL,
                             &Length,
                             &PropertyType);
    if ((Status != STATUS_BUFFER_TOO_SMALL) || !(Length))
    {
        return NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;
    }

    Data = ExAllocatePoolWithTag(PagedPool, Length, PCI_POOL_TAG);
    if (!Data) return STATUS_INSUFFICIENT_RESOURCES;

    Status = GetPropertyData(PdoExtension->PhysicalDeviceObject,
                             &PciInterruptConnectionDataKey,
                             0,
                             0,
                             Length,
                             Data,
                             &Length,
                             &PropertyType);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Data, 0);
        return Status;
    }

    /* Make sure the block really holds as many elements as it claims */
    if ((Data->Count == 0) ||
        (Length < (FIELD_OFFSET(INTERRUPT_CONNECTION_DATA, Vectors) +
                   Data->Count * sizeof(INTERRUPT_VECTOR_DATA))))
    {
        DPRINT1("PCI - connection data for %p is short (%u bytes, %u vectors)\n",
                PdoExtension, Length, Data->Count);
        ExFreePoolWithTag(Data, 0);
        return STATUS_UNSUCCESSFUL;
    }

    *ConnectionData = Data;
    return STATUS_SUCCESS;
}

VOID
NTAPI
PciGetMessageCapabilities(IN PPCI_PDO_EXTENSION PdoExtension)
{
    PPCI_MESSAGE_INFO MessageInfo = &PdoExtension->MessageInfo;
    PCI_CAPABILITIES_HEADER Header;
    USHORT Control;
    ULONG Offset, TablePointer;
    PAGED_CODE();

    /* Assume this function has no message support at all */
    RtlZeroMemory(MessageInfo, sizeof(*MessageInfo));
    MessageInfo->Type = PciMessageNone;

    /* A function with no capability list cannot have either capability */
    if (!PdoExtension->CapabilitiesPtr) return;

    /*
     * MSI-X is looked for first. It carries one independently targetable entry
     * per message, where MSI can only hand out a power-of-two run counting up
     * from one vector, so a device offering both is better served by MSI-X.
     */
    Offset = PciReadDeviceCapability(PdoExtension,
                                     PdoExtension->CapabilitiesPtr,
                                     PCI_CAPABILITY_ID_MSIX,
                                     &Header,
                                     sizeof(Header));
    if (Offset)
    {
        PciReadDeviceConfig(PdoExtension, &Control, Offset + 2, sizeof(Control));
        PciReadDeviceConfig(PdoExtension,
                            &TablePointer,
                            Offset + 4,
                            sizeof(TablePointer));

        MessageInfo->Type = PciMessageMsiX;
        MessageInfo->CapabilityPtr = (USHORT)Offset;

        /* The table size field holds one less than the number of entries */
        MessageInfo->MessagesRequested =
            (Control & PCI_MSIX_CONTROL_TABLE_SIZE_MASK) + 1;

        /* The table lives in one of the function's own memory BARs */
        MessageInfo->TableBarIndex = (UCHAR)(TablePointer & PCI_MSIX_BIR_MASK);
        MessageInfo->TableOffset = TablePointer & PCI_MSIX_OFFSET_MASK;
        MessageInfo->Is64Bit = TRUE;
        MessageInfo->MaskCapable = TRUE;

        DPRINT1("PCI - MSI-X at 0x%x: %u message(s), table in BAR %u at 0x%lx\n",
                Offset,
                MessageInfo->MessagesRequested,
                MessageInfo->TableBarIndex,
                MessageInfo->TableOffset);
        return;
    }

    /* Otherwise fall back to plain MSI */
    Offset = PciReadDeviceCapability(PdoExtension,
                                     PdoExtension->CapabilitiesPtr,
                                     PCI_CAPABILITY_ID_MSI,
                                     &Header,
                                     sizeof(Header));
    if (!Offset) return;

    PciReadDeviceConfig(PdoExtension, &Control, Offset + 2, sizeof(Control));

    MessageInfo->Type = PciMessageMsi;
    MessageInfo->CapabilityPtr = (USHORT)Offset;

    /* The capable field is the log of the count, and tops out at 32 messages */
    MessageInfo->MessagesRequested =
        (USHORT)(1 << ((Control & PCI_MSI_CONTROL_MMC_MASK) >>
                       PCI_MSI_CONTROL_MMC_SHIFT));
    if (MessageInfo->MessagesRequested > 32) MessageInfo->MessagesRequested = 32;

    MessageInfo->Is64Bit = (Control & PCI_MSI_CONTROL_64BIT) ? TRUE : FALSE;
    MessageInfo->MaskCapable = (Control & PCI_MSI_CONTROL_MASKING) ? TRUE : FALSE;

    DPRINT1("PCI - MSI at 0x%x: %u message(s), %s address\n",
            Offset,
            MessageInfo->MessagesRequested,
            MessageInfo->Is64Bit ? "64-bit" : "32-bit");
}

/*
 * Ask the HAL for the address and data one granted message is raised with.
 */
static
NTSTATUS
NTAPI
PciGetMessageAddressAndData(IN PINTERRUPT_VECTOR_DATA VectorData,
                            OUT PPHYSICAL_ADDRESS Address,
                            OUT PULONG Data)
{
    HAL_MESSAGE_TARGET_REQUEST Request;
    INTERRUPT_CONNECTION_DATA Routed;
    NTSTATUS Status;

    RtlZeroMemory(&Request, sizeof(Request));
    Request.Type = InterruptTargetTypeApic;
    Request.Apic.Vector = VectorData->Vector;
    Request.Apic.TargetProcessors = VectorData->TargetProcessors;
    Request.Apic.DestinationMode = VectorData->MessageRequest.DestinationMode;

    RtlZeroMemory(&Routed, sizeof(Routed));
    Status = HalGetMessageRoutingInfo(&Request, &Routed);
    if (!NT_SUCCESS(Status)) return Status;

    *Address = Routed.Vectors[0].XapicMessage.Address;
    *Data = Routed.Vectors[0].XapicMessage.DataPayload;
    return STATUS_SUCCESS;
}

/*
 * Program the function's MSI capability. There is only one address/data pair,
 * and the device raises message N by writing the data value with its low bits
 * replaced by N, so only the first granted vector is written and the count is
 * declared alongside it.
 */
static
NTSTATUS
NTAPI
PciProgramMsi(IN PPCI_PDO_EXTENSION PdoExtension,
              IN PINTERRUPT_CONNECTION_DATA ConnectionData,
              IN ULONG Count)
{
    PPCI_MESSAGE_INFO MessageInfo = &PdoExtension->MessageInfo;
    PHYSICAL_ADDRESS Address;
    NTSTATUS Status;
    USHORT Control, Offset;
    ULONG Data, Log;
    PAGED_CODE();

    Offset = MessageInfo->CapabilityPtr;

    Status = PciGetMessageAddressAndData(&ConnectionData->Vectors[0],
                                         &Address,
                                         &Data);
    if (!NT_SUCCESS(Status)) return Status;

    /* The run has to be a power of two, so round the grant down to one */
    for (Log = 0; (2UL << Log) <= Count; Log++) NOTHING;
    if (Log > 5) Log = 5;

    /* Turn it off while the address and data are in flux */
    PciReadDeviceConfig(PdoExtension, &Control, Offset + 2, sizeof(Control));
    Control &= ~PCI_MSI_CONTROL_ENABLE;
    PciWriteDeviceConfig(PdoExtension, &Control, Offset + 2, sizeof(Control));

    /* The data register sits past the upper address word on a 64-bit function */
    PciWriteDeviceConfig(PdoExtension,
                         &Address.LowPart,
                         Offset + 4,
                         sizeof(ULONG));
    if (MessageInfo->Is64Bit)
    {
        PciWriteDeviceConfig(PdoExtension,
                             &Address.HighPart,
                             Offset + 8,
                             sizeof(ULONG));
        PciWriteDeviceConfig(PdoExtension, &Data, Offset + 0x0C, sizeof(USHORT));
    }
    else
    {
        PciWriteDeviceConfig(PdoExtension, &Data, Offset + 8, sizeof(USHORT));
    }

    /* Declare how many of the messages the device offered are actually in use */
    Control &= ~PCI_MSI_CONTROL_MME_MASK;
    Control |= (USHORT)(Log << PCI_MSI_CONTROL_MME_SHIFT);
    Control |= PCI_MSI_CONTROL_ENABLE;
    PciWriteDeviceConfig(PdoExtension, &Control, Offset + 2, sizeof(Control));

    MessageInfo->MessagesGranted = (USHORT)(1 << Log);
    DPRINT1("PCI - MSI enabled on %p: %u message(s) at 0x%08lx/0x%lx\n",
            PdoExtension, MessageInfo->MessagesGranted, Address.LowPart, Data);
    return STATUS_SUCCESS;
}

/*
 * Program the function's MSI-X table. Each entry is independent, so every
 * granted message is written into its own slot and unmasked.
 */
static
NTSTATUS
NTAPI
PciProgramMsiX(IN PPCI_PDO_EXTENSION PdoExtension,
               IN PINTERRUPT_CONNECTION_DATA ConnectionData,
               IN ULONG Count,
               IN PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource)
{
    PPCI_MESSAGE_INFO MessageInfo = &PdoExtension->MessageInfo;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR TableBar;
    PPCI_MSIX_TABLE_ENTRY Entry;
    PHYSICAL_ADDRESS Address, TableAddress;
    NTSTATUS Status;
    USHORT Control, Offset;
    ULONG Data, Index, Length;
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Resource);

    Offset = MessageInfo->CapabilityPtr;

    /* The table lives inside one of the function's own memory windows */
    if ((!PdoExtension->Resources) ||
        (MessageInfo->TableBarIndex >= PCI_TYPE0_ADDRESSES))
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    TableBar = &PdoExtension->Resources->Current[MessageInfo->TableBarIndex];
    if (TableBar->Type != CmResourceTypeMemory)
    {
        DPRINT1("PCI - MSI-X table BAR %u of %p is not memory\n",
                MessageInfo->TableBarIndex, PdoExtension);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Length = Count * sizeof(PCI_MSIX_TABLE_ENTRY);
    if ((MessageInfo->TableOffset + Length) > TableBar->u.Memory.Length)
    {
        DPRINT1("PCI - MSI-X table of %p does not fit its BAR\n", PdoExtension);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* Map it only for as long as it takes to write the entries */
    TableAddress.QuadPart = TableBar->u.Memory.Start.QuadPart +
                            MessageInfo->TableOffset;
    Entry = MmMapIoSpace(TableAddress, Length, MmNonCached);
    if (!Entry) return STATUS_INSUFFICIENT_RESOURCES;

    /* Keep the whole function masked while the table is being filled in */
    PciReadDeviceConfig(PdoExtension, &Control, Offset + 2, sizeof(Control));
    Control |= PCI_MSIX_CONTROL_FUNCTION_MASK;
    Control |= PCI_MSIX_CONTROL_ENABLE;
    PciWriteDeviceConfig(PdoExtension, &Control, Offset + 2, sizeof(Control));

    Status = STATUS_SUCCESS;
    for (Index = 0; Index < Count; Index++)
    {
        Status = PciGetMessageAddressAndData(&ConnectionData->Vectors[Index],
                                             &Address,
                                             &Data);
        if (!NT_SUCCESS(Status)) break;

        WRITE_REGISTER_ULONG(&Entry[Index].MessageAddressLower, Address.LowPart);
        WRITE_REGISTER_ULONG(&Entry[Index].MessageAddressUpper, Address.HighPart);
        WRITE_REGISTER_ULONG(&Entry[Index].MessageData, Data);

        /* The entry is only allowed to fire once it is fully written */
        WRITE_REGISTER_ULONG(&Entry[Index].VectorControl, 0);
    }

    MmUnmapIoSpace(Entry, Length);

    if (!NT_SUCCESS(Status))
    {
        /* Leave the function masked rather than half-programmed */
        Control &= ~PCI_MSIX_CONTROL_ENABLE;
        PciWriteDeviceConfig(PdoExtension, &Control, Offset + 2, sizeof(Control));
        return Status;
    }

    /* Every entry is written, so the function can raise them now */
    Control &= ~PCI_MSIX_CONTROL_FUNCTION_MASK;
    PciWriteDeviceConfig(PdoExtension, &Control, Offset + 2, sizeof(Control));

    MessageInfo->MessagesGranted = (USHORT)Count;
    DPRINT1("PCI - MSI-X enabled on %p: %u message(s)\n", PdoExtension, Count);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciProgramMessageInterrupt(IN PPCI_PDO_EXTENSION PdoExtension,
                           IN PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource)
{
    PPCI_MESSAGE_INFO MessageInfo = &PdoExtension->MessageInfo;
    PINTERRUPT_CONNECTION_DATA ConnectionData;
    NTSTATUS Status;
    ULONG Count;
    PAGED_CODE();

    /* Nothing to do for a function that has no message capability */
    if (MessageInfo->Type == PciMessageNone) return STATUS_NOT_SUPPORTED;

    /* Find out what the arbiter actually granted */
    Status = PciGetInterruptConnectionData(PdoExtension, &ConnectionData);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("PCI - no granted messages for %p: %08lx\n", PdoExtension, Status);
        return Status;
    }

    /*
     * The resource says how many messages the placement covers and the property
     * says how many vectors were actually allocated. Take the smaller: the
     * device must never be told about a message with no vector behind it.
     */
    Count = ConnectionData->Count;
    if (Count > MessageInfo->MessagesRequested)
    {
        Count = MessageInfo->MessagesRequested;
    }

    if (Resource)
    {
        ULONG Granted = Resource->u.Interrupt.Level >> 16;

        if ((Granted) && (Granted < Count)) Count = Granted;
    }

    if (!Count)
    {
        ExFreePoolWithTag(ConnectionData, 0);
        return STATUS_UNSUCCESSFUL;
    }

    if (MessageInfo->Type == PciMessageMsiX)
    {
        Status = PciProgramMsiX(PdoExtension, ConnectionData, Count, Resource);
    }
    else
    {
        Status = PciProgramMsi(PdoExtension, ConnectionData, Count);
    }

    ExFreePoolWithTag(ConnectionData, 0);
    return Status;
}

VOID
NTAPI
PciDisableMessageInterrupt(IN PPCI_PDO_EXTENSION PdoExtension)
{
    PPCI_MESSAGE_INFO MessageInfo = &PdoExtension->MessageInfo;
    USHORT Control, Offset;
    PAGED_CODE();

    /* Only a function that was actually turned on has anything to turn off */
    if ((MessageInfo->Type == PciMessageNone) || !(MessageInfo->MessagesGranted))
    {
        return;
    }

    Offset = MessageInfo->CapabilityPtr;
    PciReadDeviceConfig(PdoExtension, &Control, Offset + 2, sizeof(Control));

    if (MessageInfo->Type == PciMessageMsiX)
    {
        Control &= ~PCI_MSIX_CONTROL_ENABLE;
    }
    else
    {
        Control &= ~PCI_MSI_CONTROL_ENABLE;
    }

    PciWriteDeviceConfig(PdoExtension, &Control, Offset + 2, sizeof(Control));
    MessageInfo->MessagesGranted = 0;
}

/* EOF */
