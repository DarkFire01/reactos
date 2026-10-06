/*
 * PROJECT:     ReactOS ACPI Platform Extensions
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Resource Hub translator interface and provider registration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * ACPI opens the hub once at init and trades callbacks with it over
 * IOCTL_UACPINT_RH_QUERY_TRANSLATION. From then on ACPI routes every
 * Connection() descriptor here instead of translating it itself, and tells us
 * which device object each firmware ResourceSource name belongs to. That second
 * call is what gives a provider its controller, and therefore what gives
 * RhpBuildReparseName something to resolve to.
 */

#include "rhpriv.h"

/*
 * What ACPI handed us, kept for as long as the interface is live. Captured now
 * because the exchange is the only chance to get them; RhTranslateDescriptor
 * and RhQueryGsivDescriptor are what will read them, so nothing
 * consumes these yet.
 */
static PVOID RhHostContext = NULL;
static PUACPINT_RH_QUALIFY_BIOS_NAME RhQualifyBiosName = NULL;
static PUACPINT_RH_ALLOCATE_SECONDARY_GSIV RhAllocateSecondaryGsiv = NULL;
static PUACPINT_RH_SET_INTERRUPT_PROPERTIES RhSetInterruptProperties = NULL;

/*
 * QueryGsivDescriptor is handed a vector and nothing else, so the connection
 * list has to be reachable without a Context.
 */
static PRH_DEVICE_CONTEXT RhDeviceContext = NULL;

/*
 * Providers are keyed by the firmware ResourceSource string, which is what both
 * a Connection() descriptor and ACPI's registration call name them by.
 */
static
PRH_PROVIDER
RhpFindProviderLocked(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ PCUNICODE_STRING BiosName)
{
    PLIST_ENTRY Entry;

    for (Entry = DeviceContext->ProviderList.Flink;
         Entry != &DeviceContext->ProviderList;
         Entry = Entry->Flink)
    {
        PRH_PROVIDER Provider = CONTAINING_RECORD(Entry, RH_PROVIDER, Link);

        if (RtlEqualUnicodeString(&Provider->BiosName, BiosName, TRUE))
            return Provider;
    }

    return NULL;
}

/*
 * The reparse target is the controller's PDO name. Two calls: one to size the
 * buffer, one to fill it.
 */
static
NTSTATUS
RhpQueryDeviceObjectName(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Out_ PUNICODE_STRING Name)
{
    ULONG ResultLength = 0;
    NTSTATUS Status;

    RtlZeroMemory(Name, sizeof(*Name));

    Status = IoGetDeviceProperty(DeviceObject,
                                 DevicePropertyPhysicalDeviceObjectName,
                                 0,
                                 NULL,
                                 &ResultLength);
    if (Status != STATUS_BUFFER_TOO_SMALL || ResultLength == 0)
        return NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;

    if (ResultLength > (MAXUSHORT - sizeof(UNICODE_NULL)))
        return STATUS_NAME_TOO_LONG;

    Name->Buffer = ExAllocatePoolWithTag(NonPagedPool,
                                         ResultLength + sizeof(UNICODE_NULL),
                                         RH_POOL_TAG);
    if (Name->Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoGetDeviceProperty(DeviceObject,
                                 DevicePropertyPhysicalDeviceObjectName,
                                 ResultLength,
                                 Name->Buffer,
                                 &ResultLength);
    if (!NT_SUCCESS(Status))
    {
        RhpFreeUnicodeString(Name);
        return Status;
    }

    /* IoGetDeviceProperty returns a NUL-terminated string; Length excludes it */
    Name->MaximumLength = (USHORT)(ResultLength + sizeof(UNICODE_NULL));
    Name->Length = (USHORT)ResultLength;
    if (Name->Length >= sizeof(UNICODE_NULL) &&
        Name->Buffer[(Name->Length / sizeof(WCHAR)) - 1] == UNICODE_NULL)
    {
        Name->Length -= sizeof(UNICODE_NULL);
    }

    return STATUS_SUCCESS;
}

/*
 * ACPI calls this when it has worked out that a firmware ResourceSource name
 * belongs to a particular device object. Find or create the provider for that
 * name and give it target data built from the device's PDO name; connections
 * naming it can then be reparsed.
 */
static
NTSTATUS
RhpConnectProviderDevice(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ PCUNICODE_STRING BiosName,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PRH_PROVIDER Provider;
    PRH_TARGET_DATA TargetData;
    PRH_TARGET_DATA OldTargetData = NULL;
    UNICODE_STRING ControllerName;
    KIRQL OldIrql;
    NTSTATUS Status;

    Status = RhpQueryDeviceObjectName(DeviceObject, &ControllerName);
    if (!NT_SUCCESS(Status))
        return Status;

    TargetData = ExAllocatePoolWithTag(NonPagedPool,
                                       sizeof(*TargetData),
                                       RH_POOL_TAG);
    if (TargetData == NULL)
    {
        RhpFreeUnicodeString(&ControllerName);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(TargetData, sizeof(*TargetData));
    InitializeListHead(&TargetData->Link);
    TargetData->ControllerName = ControllerName;
    TargetData->ReferenceCount = 1;

    KeAcquireSpinLock(&DeviceContext->Lock, &OldIrql);

    Provider = RhpFindProviderLocked(DeviceContext, BiosName);
    if (Provider == NULL)
    {
        KeReleaseSpinLock(&DeviceContext->Lock, OldIrql);

        Status = RhpAllocateProvider(&Provider);
        if (!NT_SUCCESS(Status))
        {
            RhpDereferenceTargetData(TargetData);
            return Status;
        }

        Provider->BiosName.Buffer = ExAllocatePoolWithTag(NonPagedPool,
                                                          BiosName->Length,
                                                          RH_POOL_TAG);
        if (Provider->BiosName.Buffer == NULL)
        {
            RhpFreeProvider(Provider);
            RhpDereferenceTargetData(TargetData);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(Provider->BiosName.Buffer, BiosName->Buffer, BiosName->Length);
        Provider->BiosName.Length = BiosName->Length;
        Provider->BiosName.MaximumLength = BiosName->Length;

        KeAcquireSpinLock(&DeviceContext->Lock, &OldIrql);

        /*
         * Re-check under the lock: two devices naming the same controller can
         * register concurrently, and the loser must not leave a duplicate on
         * the list for RhpFindProviderLocked to pick between.
         */
        if (RhpFindProviderLocked(DeviceContext, BiosName) != NULL)
        {
            KeReleaseSpinLock(&DeviceContext->Lock, OldIrql);
            RhpFreeProvider(Provider);
            KeAcquireSpinLock(&DeviceContext->Lock, &OldIrql);
            Provider = RhpFindProviderLocked(DeviceContext, BiosName);
        }
        else
        {
            InsertTailList(&DeviceContext->ProviderList, &Provider->Link);
        }
    }

    if (Provider != NULL)
    {
        if (Provider->DeviceObject != DeviceObject)
        {
            if (Provider->DeviceObject != NULL)
                ObDereferenceObject(Provider->DeviceObject);

            ObReferenceObject(DeviceObject);
            Provider->DeviceObject = DeviceObject;
        }

        /* A controller that came back gets fresh target data */
        OldTargetData = Provider->TargetData;
        Provider->TargetData = TargetData;
        InsertTailList(&Provider->TargetList, &TargetData->Link);
        TargetData = NULL;
        Status = STATUS_SUCCESS;
    }
    else
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }

    KeReleaseSpinLock(&DeviceContext->Lock, OldIrql);

    if (OldTargetData != NULL)
    {
        RemoveEntryList(&OldTargetData->Link);
        RhpDereferenceTargetData(OldTargetData);
    }

    if (TargetData != NULL)
        RhpDereferenceTargetData(TargetData);

    return Status;
}

/* Exported to ACPI, wraps the connect */
static
NTSTATUS
NTAPI
RhAssociateBiosName(
    _In_ PVOID Context,
    _In_ PCUNICODE_STRING BiosName,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PRH_DEVICE_CONTEXT DeviceContext = (PRH_DEVICE_CONTEXT)Context;

    if (BiosName == NULL || BiosName->Length == 0 || DeviceObject == NULL)
        return STATUS_INVALID_PARAMETER;

    return RhpConnectProviderDevice(DeviceContext, BiosName, DeviceObject);
}

/*
 * Every connection descriptor ends with a ResourceSource string naming the
 * controller, at an offset each descriptor kind states differently. The string
 * is not guaranteed NUL terminated within the descriptor, so its length is
 * whichever comes first: a NUL, or the end of the descriptor.
 */
static
NTSTATUS
RhpGetResourceNameFromDescriptor(
    _In_reads_bytes_(DescriptorLength) PVOID Descriptor,
    _In_ ULONG DescriptorLength,
    _Out_ PANSI_STRING Name)
{
    PUCHAR Bytes = Descriptor;
    ULONG TotalLength;
    ULONG NameOffset;
    ULONG Available;
    ULONG Index;

    RtlZeroMemory(Name, sizeof(*Name));

    if (DescriptorLength < 3)
        return STATUS_INVALID_PARAMETER;

    /* Tag and the two Length bytes are not counted by Length itself */
    TotalLength = (ULONG)(*(UNALIGNED USHORT *)(Bytes + 1)) + 3;
    if (DescriptorLength > TotalLength)
        DescriptorLength = TotalLength;

    switch (Bytes[0])
    {
        case SERIAL_BUS_DESCRIPTOR:
            if (DescriptorLength < sizeof(PNP_SERIAL_BUS_DESCRIPTOR))
                return STATUS_INVALID_PARAMETER;

            /* Fixed header, then TypeDataLength bytes of bus-specific data */
            NameOffset = ((PPNP_SERIAL_BUS_DESCRIPTOR)Descriptor)->TypeDataLength +
                         sizeof(PNP_SERIAL_BUS_DESCRIPTOR);
            break;

        case GPIO_INTERRUPT_IO_DESCRIPTOR:
            if (DescriptorLength < sizeof(PNP_GPIO_INTERRUPT_IO_DESCRIPTOR))
                return STATUS_INVALID_PARAMETER;

            NameOffset = ((PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR)Descriptor)->ResourceSourceOffset;
            break;

        case FUNCTION_CONFIG_DESCRIPTOR:
            if (DescriptorLength < sizeof(PNP_FUNCTION_CONFIG_DESCRIPTOR))
                return STATUS_INVALID_PARAMETER;

            NameOffset = ((PPNP_FUNCTION_CONFIG_DESCRIPTOR)Descriptor)->ResourceSourceOffset;
            break;

        default:
            return STATUS_NOT_SUPPORTED;
    }

    if (NameOffset >= DescriptorLength)
        return STATUS_INVALID_PARAMETER;

    Available = DescriptorLength - NameOffset;
    if (Available > MAXSHORT)
        Available = MAXSHORT;

    for (Index = 0; Index < Available; Index++)
    {
        if (Bytes[NameOffset + Index] == '\0')
            break;
    }

    if (Index == 0)
        return STATUS_INVALID_PARAMETER;

    Name->Buffer = (PCHAR)(Bytes + NameOffset);
    Name->Length = (USHORT)Index;
    Name->MaximumLength = (USHORT)Available;

    return STATUS_SUCCESS;
}

/*
 * The ResourceSource in the descriptor may be an ACPI relative path, so only
 * ACPI can turn it into the fully qualified name providers are keyed by. The
 * buffer starts at 128 bytes and is retried once at the size ACPI asks for.
 */
static
NTSTATUS
RhpGetProviderBiosNameFromDescriptor(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_reads_bytes_(DescriptorLength) PVOID Descriptor,
    _In_ ULONG DescriptorLength,
    _Out_ PUNICODE_STRING BiosName)
{
    ANSI_STRING RawName;
    ANSI_STRING TerminatedName;
    ULONG BufferSize = 128;
    ULONG Required = 0;
    ULONG Attempt;
    NTSTATUS Status;

    RtlZeroMemory(BiosName, sizeof(*BiosName));

    if (RhQualifyBiosName == NULL)
        return STATUS_DEVICE_NOT_READY;

    Status = RhpGetResourceNameFromDescriptor(Descriptor, DescriptorLength, &RawName);
    if (!NT_SUCCESS(Status))
        return Status;

    /* ACPI expects a NUL-terminated name, which the descriptor does not carry */
    TerminatedName.Length = RawName.Length;
    TerminatedName.MaximumLength = RawName.Length + 1;
    TerminatedName.Buffer = ExAllocatePoolWithTag(PagedPool,
                                                  TerminatedName.MaximumLength,
                                                  RH_POOL_TAG);
    if (TerminatedName.Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(TerminatedName.Buffer, RawName.Buffer, RawName.Length);
    TerminatedName.Buffer[RawName.Length] = '\0';

    for (Attempt = 0; Attempt < 2; Attempt++)
    {
        BiosName->Buffer = ExAllocatePoolWithTag(PagedPool, BufferSize, RH_POOL_TAG);
        if (BiosName->Buffer == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        BiosName->Length = 0;
        BiosName->MaximumLength = (USHORT)BufferSize;

        Status = RhQualifyBiosName(DeviceObject, &TerminatedName, BiosName, &Required);
        if (NT_SUCCESS(Status))
            break;

        RhpFreeUnicodeString(BiosName);

        if (Status != STATUS_BUFFER_TOO_SMALL || Required == 0)
            break;

        BufferSize = Required;
    }

    ExFreePoolWithTag(TerminatedName.Buffer, RH_POOL_TAG);

    if (!NT_SUCCESS(Status))
        RhpFreeUnicodeString(BiosName);

    return Status;
}

/*
 * Find or create the connection this descriptor names, and report its id. The
 * descriptor bytes are cached verbatim, because they are exactly what
 * IOCTL_RH_QUERY_CONNECTION_PROPERTIES has to hand back later. Minting an id
 * without caching would publish an id the hub could not then describe.
 */
static
NTSTATUS
RhpCacheAddConnection(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ PCUNICODE_STRING BiosName,
    _In_ UCHAR Class,
    _In_ UCHAR Type,
    _In_ ULONG InterruptVector,
    _In_reads_bytes_(DescriptorLength) PVOID Descriptor,
    _In_ ULONG DescriptorLength,
    _Out_opt_ PLARGE_INTEGER ConnectionId)
{
    PRH_CONNECTION Connection;
    PRH_CONNECTION Existing;
    PRH_PROVIDER Provider;
    PVOID Properties;
    KIRQL OldIrql;
    NTSTATUS Status;

    if (ConnectionId != NULL)
        ConnectionId->QuadPart = 0;

    /* Build the candidate outside the lock; most of this can fail */
    Status = RhpAllocateConnection(&Connection);
    if (!NT_SUCCESS(Status))
        return Status;

    Properties = ExAllocatePoolWithTag(NonPagedPool, DescriptorLength, RH_POOL_TAG);
    if (Properties == NULL)
    {
        RhpFreeConnection(Connection);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(Properties, Descriptor, DescriptorLength);
    Connection->ConnectionProperties = Properties;
    Connection->PropertiesLength = DescriptorLength;
    Connection->Class = Class;
    Connection->Type = Type;
    Connection->InterruptVector = InterruptVector;

    KeAcquireSpinLock(&DeviceContext->Lock, &OldIrql);

    Provider = RhpFindProviderLocked(DeviceContext, BiosName);
    if (Provider == NULL)
    {
        /*
         * ACPI names a controller in a Connection() descriptor before it has
         * told us which device object that name belongs to. Create the provider
         * now and let AssociateBiosName fill in its device later.
         */
        KeReleaseSpinLock(&DeviceContext->Lock, OldIrql);

        Status = RhpAllocateProvider(&Provider);
        if (!NT_SUCCESS(Status))
        {
            RhpFreeConnection(Connection);
            return Status;
        }

        Provider->BiosName.Buffer = ExAllocatePoolWithTag(NonPagedPool,
                                                          BiosName->Length,
                                                          RH_POOL_TAG);
        if (Provider->BiosName.Buffer == NULL)
        {
            RhpFreeProvider(Provider);
            RhpFreeConnection(Connection);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(Provider->BiosName.Buffer, BiosName->Buffer, BiosName->Length);
        Provider->BiosName.Length = BiosName->Length;
        Provider->BiosName.MaximumLength = BiosName->Length;

        KeAcquireSpinLock(&DeviceContext->Lock, &OldIrql);

        if (RhpFindProviderLocked(DeviceContext, BiosName) != NULL)
        {
            KeReleaseSpinLock(&DeviceContext->Lock, OldIrql);
            RhpFreeProvider(Provider);
            KeAcquireSpinLock(&DeviceContext->Lock, &OldIrql);
            Provider = RhpFindProviderLocked(DeviceContext, BiosName);
        }
        else
        {
            InsertTailList(&DeviceContext->ProviderList, &Provider->Link);
        }

        if (Provider == NULL)
        {
            KeReleaseSpinLock(&DeviceContext->Lock, OldIrql);
            RhpFreeConnection(Connection);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    /* An identical descriptor on the same provider is the same connection */
    Existing = RhpFindConnectionLocked(DeviceContext,
                                       Provider,
                                       Class,
                                       Type,
                                       Descriptor,
                                       DescriptorLength);
    if (Existing != NULL)
    {
        /*
         * A caller that supplies a vector disagreeing with the one already
         * recorded gets the recorded one back, and re-reads it rather than
         * trusting its own.
         */
        if (InterruptVector != 0 &&
            Existing->InterruptVector != 0 &&
            Existing->InterruptVector != InterruptVector)
        {
            Status = STATUS_OBJECT_NAME_EXISTS;
        }
        else
        {
            if (InterruptVector != 0)
                Existing->InterruptVector = InterruptVector;

            Status = STATUS_SUCCESS;
        }

        if (ConnectionId != NULL)
            ConnectionId->QuadPart = Existing->Id.QuadPart;

        KeReleaseSpinLock(&DeviceContext->Lock, OldIrql);
        RhpFreeConnection(Connection);
        return Status;
    }

    Status = RhpInsertConnectionLocked(DeviceContext, Connection);
    if (NT_SUCCESS(Status))
    {
        Connection->Provider = Provider;
        InterlockedIncrement(&Provider->ReferenceCount);
        InsertTailList(&Provider->ConnectionList, &Connection->ProviderLink);

        if (ConnectionId != NULL)
            ConnectionId->QuadPart = Connection->Id.QuadPart;
    }

    KeReleaseSpinLock(&DeviceContext->Lock, OldIrql);

    if (!NT_SUCCESS(Status))
        RhpFreeConnection(Connection);

    return Status;
}

/*
 * The three descriptor kinds that become a CmResourceTypeConnection. Each is the
 * same shape: cache the descriptor, then stamp the id and the class/type pair
 * into the requirements descriptor.
 */
static
NTSTATUS
RhpBiosSerialBusToNtResource(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ PCUNICODE_STRING BiosName,
    _In_reads_bytes_(DescriptorLength) PPNP_SERIAL_BUS_DESCRIPTOR Descriptor,
    _In_ ULONG DescriptorLength,
    _Out_ PIO_RESOURCE_DESCRIPTOR IoDescriptor)
{
    LARGE_INTEGER Id;
    ULONG TotalLength;
    NTSTATUS Status;

    /* Length covers everything past Tag and Length itself */
    TotalLength = (ULONG)Descriptor->Length + 3;
    if (TotalLength < sizeof(PNP_SERIAL_BUS_DESCRIPTOR) || TotalLength > DescriptorLength)
        return STATUS_INVALID_PARAMETER;

    Status = RhpCacheAddConnection(DeviceContext,
                                   BiosName,
                                   CM_RESOURCE_CONNECTION_CLASS_SERIAL,
                                   Descriptor->SerialBusType,
                                   0,
                                   Descriptor,
                                   DescriptorLength,
                                   &Id);
    if (!NT_SUCCESS(Status))
        return Status;

    IoDescriptor->Type = CmResourceTypeConnection;
    IoDescriptor->u.Connection.Class = CM_RESOURCE_CONNECTION_CLASS_SERIAL;
    IoDescriptor->u.Connection.Type = Descriptor->SerialBusType;
    IoDescriptor->u.Connection.IdLowPart = Id.LowPart;
    IoDescriptor->u.Connection.IdHighPart = Id.HighPart;

    /*
     * Revision 2 added the shared bit to GeneralFlags. Setting it yields
     * CmResourceShareShared, and clearing it CmResourceShareDeviceExclusive.
     */
    if (Descriptor->RevisionId >= SERIAL_BUS_DESCRIPTOR_REVISION_V2)
    {
        IoDescriptor->ShareDisposition =
            (UCHAR)(((Descriptor->GeneralFlags & SERIAL_BUS_FLAG_SHARED_DESCRIPTOR) | 2) >> 1);
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
RhpBiosGpioIoToNtResource(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ PCUNICODE_STRING BiosName,
    _In_reads_bytes_(DescriptorLength) PVOID Descriptor,
    _In_ ULONG DescriptorLength,
    _Out_ PIO_RESOURCE_DESCRIPTOR IoDescriptor)
{
    LARGE_INTEGER Id;
    NTSTATUS Status;

    Status = RhpCacheAddConnection(DeviceContext,
                                   BiosName,
                                   CM_RESOURCE_CONNECTION_CLASS_GPIO,
                                   CM_RESOURCE_CONNECTION_TYPE_GPIO_IO,
                                   0,
                                   Descriptor,
                                   DescriptorLength,
                                   &Id);
    if (!NT_SUCCESS(Status))
        return Status;

    IoDescriptor->Type = CmResourceTypeConnection;
    IoDescriptor->u.Connection.Class = CM_RESOURCE_CONNECTION_CLASS_GPIO;
    IoDescriptor->u.Connection.Type = CM_RESOURCE_CONNECTION_TYPE_GPIO_IO;
    IoDescriptor->u.Connection.IdLowPart = Id.LowPart;
    IoDescriptor->u.Connection.IdHighPart = Id.HighPart;

    return STATUS_SUCCESS;
}

static
NTSTATUS
RhpBiosFunctionConfigToNtResource(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ PCUNICODE_STRING BiosName,
    _In_reads_bytes_(DescriptorLength) PVOID Descriptor,
    _In_ ULONG DescriptorLength,
    _Out_ PIO_RESOURCE_DESCRIPTOR IoDescriptor)
{
    LARGE_INTEGER Id;
    NTSTATUS Status;

    Status = RhpCacheAddConnection(DeviceContext,
                                   BiosName,
                                   CM_RESOURCE_CONNECTION_CLASS_FUNCTION_CONFIG,
                                   CM_RESOURCE_CONNECTION_TYPE_FUNCTION_CONFIG,
                                   0,
                                   Descriptor,
                                   DescriptorLength,
                                   &Id);
    if (!NT_SUCCESS(Status))
        return Status;

    IoDescriptor->Type = CmResourceTypeConnection;
    IoDescriptor->u.Connection.Class = CM_RESOURCE_CONNECTION_CLASS_FUNCTION_CONFIG;
    IoDescriptor->u.Connection.Type = CM_RESOURCE_CONNECTION_TYPE_FUNCTION_CONFIG;
    IoDescriptor->u.Connection.IdLowPart = Id.LowPart;
    IoDescriptor->u.Connection.IdHighPart = Id.HighPart;

    return STATUS_SUCCESS;
}

/*
 * The three offsets in a GPIO descriptor have to be in ascending order and
 * inside the descriptor, or the pin table and resource-source string overlap.
 * The pin table also has to hold a whole number of USHORT pins.
 */
static
BOOLEAN
RhpIsBiosGpioDescriptorValid(
    _In_reads_bytes_(DescriptorLength) PVOID Descriptor,
    _In_ ULONG DescriptorLength)
{
    PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR Gpio = Descriptor;
    ULONG PinTableSize;

    if (DescriptorLength < sizeof(PNP_GPIO_INTERRUPT_IO_DESCRIPTOR))
        return FALSE;

    if (Gpio->Tag != GPIO_INTERRUPT_IO_DESCRIPTOR)
        return FALSE;

    if (Gpio->Revision < PNP_GPIO_IRQ_DESCRIPTOR_REVISION_MINIMUM)
        return FALSE;

    if (Gpio->DescriptorType > PNP_GPIO_IRQ_DESCRIPTOR_TYPE_IO)
        return FALSE;

    if (Gpio->PinTableOffset < sizeof(PNP_GPIO_INTERRUPT_IO_DESCRIPTOR) ||
        Gpio->PinTableOffset >= DescriptorLength ||
        Gpio->ResourceSourceOffset >= DescriptorLength ||
        Gpio->VendorDataOffset > DescriptorLength)
    {
        return FALSE;
    }

    if (Gpio->PinTableOffset >= Gpio->ResourceSourceOffset ||
        Gpio->ResourceSourceOffset >= Gpio->VendorDataOffset)
    {
        return FALSE;
    }

    PinTableSize = Gpio->ResourceSourceOffset - Gpio->PinTableOffset;
    if (PinTableSize < sizeof(USHORT) || (PinTableSize & 1) != 0)
        return FALSE;

    /* The resource-source string, less its terminator, must fit a USHORT */
    if ((ULONG)(Gpio->VendorDataOffset - Gpio->ResourceSourceOffset - 1) > 0x7FFE)
        return FALSE;

    return TRUE;
}

/*
 * Turn an allocated GSIV plus the firmware's InterruptIoFlags into an ordinary
 * interrupt requirement. The InterruptIoFlags bits are:
 *
 *   bit 0    mode          -> CM_RESOURCE_INTERRUPT_LATCHED when set
 *   bits 1-2 polarity      -> high / low / both, 3 is reserved and refused
 *   bit 3    shared        -> CmResourceShareShared, else DeviceExclusive
 *   bit 4    wake hint     -> CM_RESOURCE_INTERRUPT_WAKE_HINT
 *
 * SECONDARY_INTERRUPT is always set: the vector is not a real GSI, it is one the
 * GPIO controller will demultiplex, and the arbiter has to know not to treat it
 * as a hardware line.
 */
static
NTSTATUS
RhpBiosGpioInterruptToIoDescriptor(
    _In_ ULONG Gsiv,
    _In_ PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR Gpio,
    _Out_ PIO_RESOURCE_DESCRIPTOR IoDescriptor)
{
    USHORT InterruptFlags = Gpio->InterruptIoFlags;
    USHORT Flags;

    Flags = CM_RESOURCE_INTERRUPT_SECONDARY_INTERRUPT;
    if ((InterruptFlags & PNP_GPIO_IRQ_MODE) == PNP_GPIO_IRQ_MODE_EDGE)
        Flags |= CM_RESOURCE_INTERRUPT_LATCHED;

    if ((InterruptFlags & PNP_GPIO_IRQ_WAKE_HINT) != 0)
        Flags |= CM_RESOURCE_INTERRUPT_WAKE_HINT;

    /* 0b11 is not a defined polarity */
    if ((InterruptFlags & PNP_GPIO_IRQ_POLARITY) == PNP_GPIO_IRQ_POLARITY)
        return STATUS_NOT_SUPPORTED;

    IoDescriptor->Type = CmResourceTypeInterrupt;
    IoDescriptor->Flags = Flags;
    IoDescriptor->ShareDisposition =
        (UCHAR)(((InterruptFlags >> 2) & 2) | CmResourceShareDeviceExclusive);
    IoDescriptor->u.Interrupt.MinimumVector = Gsiv;
    IoDescriptor->u.Interrupt.MaximumVector = Gsiv;

    return STATUS_SUCCESS;
}

/* The firmware's polarity bits in the kernel's vocabulary */
static
KINTERRUPT_POLARITY
RhpGpioInterruptPolarity(
    _In_ USHORT InterruptFlags)
{
    switch (InterruptFlags & PNP_GPIO_IRQ_POLARITY)
    {
        case PNP_GPIO_IRQ_POLARITY_HIGH: return InterruptActiveHigh;
        case PNP_GPIO_IRQ_POLARITY_LOW:  return InterruptActiveLow;
        case PNP_GPIO_IRQ_POLARITY_BOTH: return InterruptActiveBoth;
        default:                         return InterruptPolarityUnknown;
    }
}

/*
 * GpioInt, the odd one out.
 *
 * A GPIO interrupt does not become a connection resource. ACPI mints a synthetic
 * GSIV for the pin and the device ends up with an ordinary
 * CmResourceTypeInterrupt, which is why the kernel's interrupt arbiter handles
 * it without knowing GPIO exists. The connection is still cached and keyed by
 * that vector, so QueryGsivDescriptor can find its way back.
 *
 * Exactly one pin is allowed, and it must not be INVALID_PIN_NUMBER: a GSIV maps
 * to a single line, so a multi-pin GpioInt has no single vector to stand for it.
 */
static
NTSTATUS
RhpBiosGpioInterruptToNtResource(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ PCUNICODE_STRING BiosName,
    _In_reads_bytes_(DescriptorLength) PVOID Descriptor,
    _In_ ULONG DescriptorLength,
    _Out_ PIO_RESOURCE_DESCRIPTOR IoDescriptor)
{
    PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR Gpio = Descriptor;
    PRH_CONNECTION Existing;
    ULONG PinCount;
    ULONG Gsiv = 0;
    KIRQL OldIrql;
    NTSTATUS Status;

    PinCount = (ULONG)(Gpio->ResourceSourceOffset - Gpio->PinTableOffset) / sizeof(USHORT);
    if (PinCount != 1)
        return STATUS_INVALID_PARAMETER;

    if (*(UNALIGNED USHORT *)((PUCHAR)Descriptor + Gpio->PinTableOffset) == INVALID_PIN_NUMBER)
        return STATUS_INVALID_PARAMETER;

    /*
     * Two devices can name the same pin. The first translation allocated the
     * GSIV; every later one has to reuse it rather than allocate a second vector
     * for one physical line.
     */
    KeAcquireSpinLock(&DeviceContext->Lock, &OldIrql);
    Existing = RhpFindConnectionLocked(DeviceContext,
                                       RhpFindProviderLocked(DeviceContext, BiosName),
                                       CM_RESOURCE_CONNECTION_CLASS_GPIO,
                                       CM_RESOURCE_CONNECTION_TYPE_GPIO_INTERRUPT,
                                       Descriptor,
                                       DescriptorLength);
    if (Existing != NULL)
        Gsiv = Existing->InterruptVector;
    KeReleaseSpinLock(&DeviceContext->Lock, OldIrql);

    if (Gsiv == 0)
    {
        if (RhAllocateSecondaryGsiv == NULL)
            return STATUS_DEVICE_NOT_READY;

        Status = RhAllocateSecondaryGsiv(NULL, 0, &Gsiv);
        if (!NT_SUCCESS(Status))
            return Status;

        Status = RhpCacheAddConnection(DeviceContext,
                                       BiosName,
                                       CM_RESOURCE_CONNECTION_CLASS_GPIO,
                                       CM_RESOURCE_CONNECTION_TYPE_GPIO_INTERRUPT,
                                       Gsiv,
                                       Descriptor,
                                       DescriptorLength,
                                       NULL);

        /*
         * Somebody cached this same pin while we were allocating, so their
         * vector is the one of record and ours is dropped. Re-read the cached
         * vector.
         */
        if (Status == STATUS_OBJECT_NAME_EXISTS)
        {
            KeAcquireSpinLock(&DeviceContext->Lock, &OldIrql);
            Existing = RhpFindConnectionLocked(DeviceContext,
                                               RhpFindProviderLocked(DeviceContext, BiosName),
                                               CM_RESOURCE_CONNECTION_CLASS_GPIO,
                                               CM_RESOURCE_CONNECTION_TYPE_GPIO_INTERRUPT,
                                               Descriptor,
                                               DescriptorLength);
            Gsiv = (Existing != NULL) ? Existing->InterruptVector : 0;
            KeReleaseSpinLock(&DeviceContext->Lock, OldIrql);

            if (Gsiv == 0)
                return STATUS_UNSUCCESSFUL;
        }
        else if (!NT_SUCCESS(Status))
        {
            return Status;
        }
    }

    Status = RhpBiosGpioInterruptToIoDescriptor(Gsiv, Gpio, IoDescriptor);
    if (!NT_SUCCESS(Status))
        return Status;

    /*
     * Mode and polarity do not travel in the requirements descriptor, which
     * carries only the latched/level bit. ACPI owns the interrupt's properties,
     * so tell it out of band, which is what this import exists for. Failing to
     * do so leaves the arbiter connecting the vector with whatever polarity the
     * platform defaulted to, which for an active low touch controller means an
     * interrupt that never deasserts.
     */
    if (RhSetInterruptProperties != NULL)
    {
        RhSetInterruptProperties(Gsiv,
                                    (Gpio->InterruptIoFlags & PNP_GPIO_IRQ_MODE)
                                        ? Latched
                                        : LevelSensitive,
                                    RhpGpioInterruptPolarity(Gpio->InterruptIoFlags));
    }

    return STATUS_SUCCESS;
}

/*
 * Dispatch by ACPI large item tag, and for GPIO by the descriptor type byte that
 * follows it.
 */
static
NTSTATUS
RhpTranslateAndCacheConnection(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ PCUNICODE_STRING BiosName,
    _In_reads_bytes_(DescriptorLength) PVOID Descriptor,
    _In_ ULONG DescriptorLength,
    _Out_ PIO_RESOURCE_DESCRIPTOR IoDescriptor)
{
    PUCHAR Bytes = Descriptor;

    switch (Bytes[0])
    {
        case GPIO_INTERRUPT_IO_DESCRIPTOR:
            if (!RhpIsBiosGpioDescriptorValid(Descriptor, DescriptorLength))
                return STATUS_INVALID_PARAMETER;

            if (((PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR)Descriptor)->DescriptorType ==
                PNP_GPIO_IRQ_DESCRIPTOR_TYPE_IO)
            {
                return RhpBiosGpioIoToNtResource(DeviceContext, BiosName,
                                                 Descriptor, DescriptorLength,
                                                 IoDescriptor);
            }

            return RhpBiosGpioInterruptToNtResource(DeviceContext, BiosName,
                                                    Descriptor, DescriptorLength,
                                                    IoDescriptor);

        case FUNCTION_CONFIG_DESCRIPTOR:
            if (DescriptorLength < sizeof(PNP_FUNCTION_CONFIG_DESCRIPTOR))
                return STATUS_INVALID_PARAMETER;

            return RhpBiosFunctionConfigToNtResource(DeviceContext, BiosName,
                                                     Descriptor, DescriptorLength,
                                                     IoDescriptor);

        case SERIAL_BUS_DESCRIPTOR:
            if (DescriptorLength < sizeof(PNP_SERIAL_BUS_DESCRIPTOR))
                return STATUS_INVALID_PARAMETER;

            return RhpBiosSerialBusToNtResource(DeviceContext, BiosName,
                                                Descriptor, DescriptorLength,
                                                IoDescriptor);

        default:
            return STATUS_NOT_SUPPORTED;
    }
}

/*
 * ACPI wants the provider name back as ANSI, appended after the descriptor. The
 * reported size is floored at 56 even for an empty name, so a caller that sized
 * its buffer from a previous answer never shrinks below the header.
 */
static
NTSTATUS
RhpAddBiosNameToResult(
    _In_ PCUNICODE_STRING BiosName,
    _Out_ PUACPINT_RH_TRANSLATION_RESULT Result,
    _Inout_ PULONG SizeInOut)
{
    ANSI_STRING AnsiName;
    ULONG AnsiSize;
    ULONG Required;
    NTSTATUS Status;

    AnsiSize = RtlxUnicodeStringToAnsiSize(BiosName);
    if (AnsiSize > 0x7FFF)
        return STATUS_INVALID_PARAMETER;

    Required = AnsiSize + FIELD_OFFSET(UACPINT_RH_TRANSLATION_RESULT, ReferenceName);
    if (Required < UACPINT_RH_TRANSLATION_RESULT_MIN_SIZE)
        Required = UACPINT_RH_TRANSLATION_RESULT_MIN_SIZE;

    if (*SizeInOut < Required)
    {
        *SizeInOut = Required;
        return STATUS_BUFFER_TOO_SMALL;
    }

    AnsiName.Length = 0;
    AnsiName.MaximumLength = (USHORT)AnsiSize;
    AnsiName.Buffer = Result->ReferenceName;

    Status = RtlUnicodeStringToAnsiString(&AnsiName, BiosName, FALSE);
    if (!NT_SUCCESS(Status))
    {
        *SizeInOut = Required;
        return Status;
    }

    Result->ScopedNameOffset = FIELD_OFFSET(UACPINT_RH_TRANSLATION_RESULT, ReferenceName);
    Result->ScopedNameLength = AnsiName.Length + 1;
    *SizeInOut = Required;

    return STATUS_SUCCESS;
}

/*
 * Exported to ACPI. The whole translation in one call: work out which controller
 * the descriptor names, turn it into a connection, and hand back a requirements
 * descriptor plus the provider's name.
 */
static
NTSTATUS
NTAPI
RhTranslateDescriptor(
    _In_ PVOID Context,
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_reads_bytes_(DescriptorLength) PVOID Descriptor,
    _In_ ULONG DescriptorLength,
    _In_ ULONG Flags,
    _Out_ PUACPINT_RH_TRANSLATION_RESULT Result,
    _Inout_ PULONG SizeInOut)
{
    PRH_DEVICE_CONTEXT DeviceContext = (PRH_DEVICE_CONTEXT)Context;
    UNICODE_STRING BiosName;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Flags);

    if (Descriptor == NULL || DescriptorLength == 0 || Result == NULL ||
        *SizeInOut < UACPINT_RH_TRANSLATION_RESULT_MIN_SIZE)
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(Result, UACPINT_RH_TRANSLATION_RESULT_MIN_SIZE);
    Result->Count = 1;

    Status = RhpGetProviderBiosNameFromDescriptor(DeviceObject, Descriptor,
                                                 DescriptorLength, &BiosName);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = RhpTranslateAndCacheConnection(DeviceContext,
                                            &BiosName,
                                            Descriptor,
                                            DescriptorLength,
                                            &Result->Descriptor);
    if (NT_SUCCESS(Status))
        Status = RhpAddBiosNameToResult(&BiosName, Result, SizeInOut);

    RhpFreeUnicodeString(&BiosName);
    return Status;
}

/*
 * Exported to ACPI, to recover the descriptor behind a GPIO backed interrupt.
 *
 * ACPI calls this with only a vector, so unlike every other export there is no
 * Context to route on, hence the module scope device context.
 *
 * The answer is rebuilt from the cached firmware descriptor rather than stored
 * pre-translated, so it cannot drift from what the connection actually says
 * after an IOCTL_RH_UPDATE_CONNECTION_PROPERTIES has changed the pin's mode.
 */
static
NTSTATUS
NTAPI
RhQueryGsivDescriptor(
    _In_ ULONG Gsiv,
    _In_ ULONG Flags,
    _Out_ PIO_RESOURCE_DESCRIPTOR IoDescriptor)
{
    PRH_DEVICE_CONTEXT DeviceContext = RhDeviceContext;
    PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR Gpio = NULL;
    PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR Copy;
    PRH_CONNECTION Connection;
    PLIST_ENTRY Entry;
    ULONG Length = 0;
    KIRQL OldIrql;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Flags);

    if (DeviceContext == NULL || IoDescriptor == NULL || Gsiv == 0)
        return STATUS_INVALID_PARAMETER;

    KeAcquireSpinLock(&DeviceContext->Lock, &OldIrql);

    for (Entry = DeviceContext->ConnectionList.Flink;
         Entry != &DeviceContext->ConnectionList;
         Entry = Entry->Flink)
    {
        Connection = CONTAINING_RECORD(Entry, RH_CONNECTION, Link);

        if (Connection->InterruptVector == Gsiv &&
            Connection->Class == CM_RESOURCE_CONNECTION_CLASS_GPIO &&
            Connection->ConnectionProperties != NULL)
        {
            Gpio = Connection->ConnectionProperties;
            Length = Connection->PropertiesLength;
            break;
        }
    }

    /*
     * Copy it out rather than translate under the lock: the descriptor is small
     * and this keeps the spin lock off a path that only reads.
     */
    if (Gpio != NULL)
    {
        Copy = ExAllocatePoolWithTag(NonPagedPool, Length, RH_POOL_TAG);
        if (Copy != NULL)
            RtlCopyMemory(Copy, Gpio, Length);
    }
    else
    {
        Copy = NULL;
    }

    KeReleaseSpinLock(&DeviceContext->Lock, OldIrql);

    if (Gpio == NULL)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    if (Copy == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(IoDescriptor, sizeof(*IoDescriptor));
    Status = RhpBiosGpioInterruptToIoDescriptor(Gsiv, Copy, IoDescriptor);

    ExFreePoolWithTag(Copy, RH_POOL_TAG);
    return Status;
}

/*
 * One buffered IOCTL carrying the interface in both directions: read ACPI's
 * callbacks out of the input buffer, write ours into the output buffer. With a
 * flat structure the size check alone covers the input.
 */
NTSTATUS
RhpProcessTranslationInterfaceQueryIoctl(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ WDFREQUEST Request,
    _Out_ PULONG_PTR Information)
{
    PUACPINT_RH_TRANSLATION_INTERFACE Input;
    PUACPINT_RH_TRANSLATION_INTERFACE Output;
    size_t InputLength;
    size_t OutputLength;
    NTSTATUS Status;

    *Information = 0;

    Status = WdfRequestRetrieveInputBuffer(Request,
                                           sizeof(*Input),
                                           (PVOID *)&Input,
                                           &InputLength);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfRequestRetrieveOutputBuffer(Request,
                                            sizeof(*Output),
                                            (PVOID *)&Output,
                                            &OutputLength);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Input->Interface.Size < sizeof(*Input))
        return STATUS_INVALID_PARAMETER;

    RhDeviceContext = DeviceContext;
    RhHostContext = Input->HostContext;
    RhQualifyBiosName = Input->QualifyBiosName;
    RhAllocateSecondaryGsiv = Input->AllocateSecondaryGsiv;
    RhSetInterruptProperties = Input->SetInterruptProperties;

    RtlZeroMemory(Output, sizeof(*Output));
    Output->Interface.Size = sizeof(*Output);
    Output->Interface.Version = UACPINT_RH_TRANSLATION_VERSION;

    /*
     * Nothing here is refcounted: ACPI drops the interface by watching the hub
     * device with a PnP target device change notification.
     */
    Output->Interface.InterfaceReference = NULL;
    Output->Interface.InterfaceDereference = NULL;

    Output->Context = DeviceContext;
    Output->TranslateDescriptor = RhTranslateDescriptor;
    Output->AssociateBiosName = RhAssociateBiosName;
    Output->QueryGsivDescriptor = RhQueryGsivDescriptor;

    *Information = sizeof(*Output);
    return STATUS_SUCCESS;
}
