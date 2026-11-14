/*
 * PROJECT:     ReactOS ACPI extension driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Translation of ACPI connection descriptors and the connection database
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "acpiexp.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

static FAST_MUTEX RhLock;
static LIST_ENTRY RhConnections;
static LIST_ENTRY RhProviders;
static volatile LONG64 RhNextConnectionId;

/* What ACPI handed over in the interface exchange */
static PUACPINT_RH_QUALIFY_BIOS_NAME RhQualifyBiosName;
static PUACPINT_RH_ALLOCATE_SECONDARY_GSIV RhAllocateSecondaryGsiv;
static PUACPINT_RH_SET_INTERRUPT_PROPERTIES RhSetInterruptProperties;

/* Field offsets inside the ACPI large resource descriptors */
#define RH_ITEM_HEADER              3
#define RH_GPIO_TYPE                4
#define RH_GPIO_INTERRUPT_IO_FLAGS  7
#define RH_GPIO_SOURCE_OFFSET       17
#define RH_GPIO_VENDOR_OFFSET       19
#define RH_FUNCTION_FLAGS           4
#define RH_FUNCTION_SOURCE_OFFSET   12
#define RH_FUNCTION_VENDOR_OFFSET   14
#define RH_SERIAL_TYPE              5
#define RH_SERIAL_GENERAL_FLAGS     6
#define RH_SERIAL_TYPE_DATA_LENGTH  10
#define RH_SERIAL_TYPE_DATA         12

/* FUNCTIONS ******************************************************************/

static
USHORT
RhReadUshort(
    _In_ PUCHAR Descriptor,
    _In_ ULONG Offset)
{
    return (USHORT)(Descriptor[Offset] | (Descriptor[Offset + 1] << 8));
}

/**
 * @brief Finds the ResourceSource string of a descriptor, the controller it names.
 *
 * @param[out] Source
 * Points into the descriptor, Length excludes the terminator.
 */
static
NTSTATUS
RhGetResourceSource(
    _In_reads_bytes_(ItemLength) PUCHAR Descriptor,
    _In_ ULONG ItemLength,
    _Out_ PANSI_STRING Source)
{
    ULONG First, Last;

    switch (Descriptor[0])
    {
        case GPIO_INTERRUPT_IO_DESCRIPTOR:
            if (ItemLength < RH_GPIO_VENDOR_OFFSET + 2)
                return STATUS_INVALID_PARAMETER;
            First = RhReadUshort(Descriptor, RH_GPIO_SOURCE_OFFSET);
            Last = RhReadUshort(Descriptor, RH_GPIO_VENDOR_OFFSET);
            break;

        case FUNCTION_CONFIG_DESCRIPTOR:
            if (ItemLength < RH_FUNCTION_VENDOR_OFFSET + 2)
                return STATUS_INVALID_PARAMETER;
            First = RhReadUshort(Descriptor, RH_FUNCTION_SOURCE_OFFSET);
            Last = RhReadUshort(Descriptor, RH_FUNCTION_VENDOR_OFFSET);
            break;

        case SERIAL_BUS_DESCRIPTOR:
            /* The name follows the bus specific data and runs to the end of the item */
            if (ItemLength < RH_SERIAL_TYPE_DATA)
                return STATUS_INVALID_PARAMETER;
            First = RH_SERIAL_TYPE_DATA + RhReadUshort(Descriptor, RH_SERIAL_TYPE_DATA_LENGTH);
            Last = ItemLength;
            break;

        default:
            return STATUS_NOT_SUPPORTED;
    }

    /* A vendor data offset of zero means the name runs to the end */
    if (Last == 0)
        Last = ItemLength;
    if (First == 0 || First >= Last || Last > ItemLength)
        return STATUS_INVALID_PARAMETER;

    Source->Buffer = (PCHAR)Descriptor + First;
    Source->MaximumLength = (USHORT)(Last - First);
    Source->Length = (USHORT)strnlen(Source->Buffer, Source->MaximumLength);
    return Source->Length ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

/**
 * @brief Has ACPI resolve a ResourceSource in the consumer's scope to an absolute path.
 */
static
NTSTATUS
RhQualifyName(
    _In_ PDEVICE_OBJECT Consumer,
    _In_ PANSI_STRING Source,
    _Out_ PUNICODE_STRING Qualified)
{
    STRING Reference;
    ULONG Required = 0;
    ULONG Attempt;
    NTSTATUS Status = STATUS_NOT_SUPPORTED;

    RtlZeroMemory(Qualified, sizeof(*Qualified));
    if (!RhQualifyBiosName)
        return STATUS_NOT_SUPPORTED;

    /* ACPI walks the name as a C string */
    Reference.Buffer = ExAllocatePoolWithTag(PagedPool, Source->Length + 1, RH_POOL_TAG);
    if (!Reference.Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(Reference.Buffer, Source->Buffer, Source->Length);
    Reference.Buffer[Source->Length] = ANSI_NULL;
    Reference.Length = Source->Length;
    Reference.MaximumLength = Source->Length + 1;

    Required = (Source->Length + 32) * sizeof(WCHAR);
    for (Attempt = 0; Attempt < 2; Attempt++)
    {
        Qualified->Buffer = ExAllocatePoolWithTag(PagedPool, Required, RH_POOL_TAG);
        if (!Qualified->Buffer)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        Qualified->Length = 0;
        Qualified->MaximumLength = (USHORT)Required;

        Status = RhQualifyBiosName(Consumer, &Reference, Qualified, &Required);
        if (NT_SUCCESS(Status))
            break;

        ExFreePoolWithTag(Qualified->Buffer, RH_POOL_TAG);
        RtlZeroMemory(Qualified, sizeof(*Qualified));
        if (Status != STATUS_BUFFER_TOO_SMALL || Required == 0)
            break;
    }

    ExFreePoolWithTag(Reference.Buffer, RH_POOL_TAG);
    return Status;
}

/**
 * @brief Turns a GpioInt into a secondary interrupt resource.
 */
static
NTSTATUS
RhTranslateGpioInterrupt(
    _Inout_ PRH_CONNECTION Connection)
{
    PIO_RESOURCE_DESCRIPTOR Translated = &Connection->Translated;
    KINTERRUPT_POLARITY Polarity;
    KINTERRUPT_MODE Mode;
    USHORT Flags;
    NTSTATUS Status;

    if (!RhAllocateSecondaryGsiv)
        return STATUS_NOT_SUPPORTED;

    Status = RhAllocateSecondaryGsiv(NULL, 0, &Connection->Gsiv);
    if (!NT_SUCCESS(Status))
        return Status;

    Flags = RhReadUshort(Connection->Descriptor, RH_GPIO_INTERRUPT_IO_FLAGS);
    Mode = (Flags & PNP_GPIO_IRQ_MODE) ? Latched : LevelSensitive;
    switch (Flags & PNP_GPIO_IRQ_POLARITY)
    {
        case PNP_GPIO_IRQ_POLARITY_LOW:
            Polarity = InterruptActiveLow;
            break;
        case PNP_GPIO_IRQ_POLARITY_BOTH:
            Polarity = InterruptActiveBoth;
            break;
        default:
            Polarity = InterruptActiveHigh;
            break;
    }

    Translated->Type = CmResourceTypeInterrupt;
    Translated->ShareDisposition = (Flags & PNP_GPIO_IRQ_SHARED) ? CmResourceShareShared :
                                                                   CmResourceShareDeviceExclusive;
    Translated->Flags = ((Mode == Latched) ? CM_RESOURCE_INTERRUPT_LATCHED : CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE) |
                        CM_RESOURCE_INTERRUPT_SECONDARY_INTERRUPT;
    if (Flags & PNP_GPIO_IRQ_WAKE_HINT)
        Translated->Flags |= CM_RESOURCE_INTERRUPT_WAKE_HINT;
    Translated->u.Interrupt.MinimumVector = Connection->Gsiv;
    Translated->u.Interrupt.MaximumVector = Connection->Gsiv;

    if (RhSetInterruptProperties)
        RhSetInterruptProperties(Connection->Gsiv, Mode, Polarity);

    return STATUS_SUCCESS;
}

/**
 * @brief Gives a descriptor a connection id and the resource that carries it.
 */
static
NTSTATUS
RhTranslateConnection(
    _Inout_ PRH_CONNECTION Connection)
{
    PIO_RESOURCE_DESCRIPTOR Translated = &Connection->Translated;
    PUCHAR Descriptor = Connection->Descriptor;
    BOOLEAN Shared = FALSE;

    Translated->Type = CmResourceTypeConnection;

    switch (Descriptor[0])
    {
        case GPIO_INTERRUPT_IO_DESCRIPTOR:
            Translated->u.Connection.Class = CM_RESOURCE_CONNECTION_CLASS_GPIO;
            Translated->u.Connection.Type = CM_RESOURCE_CONNECTION_TYPE_GPIO_IO;
            Shared = (RhReadUshort(Descriptor, RH_GPIO_INTERRUPT_IO_FLAGS) & PNP_GPIO_IRQ_SHARED) != 0;
            break;

        case FUNCTION_CONFIG_DESCRIPTOR:
            Translated->u.Connection.Class = CM_RESOURCE_CONNECTION_CLASS_FUNCTION_CONFIG;
            Translated->u.Connection.Type = CM_RESOURCE_CONNECTION_TYPE_FUNCTION_CONFIG;
            Shared = (RhReadUshort(Descriptor, RH_FUNCTION_FLAGS) & 1) != 0;
            break;

        case SERIAL_BUS_DESCRIPTOR:
            Translated->u.Connection.Class = CM_RESOURCE_CONNECTION_CLASS_SERIAL;
            switch (Descriptor[RH_SERIAL_TYPE])
            {
                case SERIAL_BUS_I2C_DESCRIPTOR_TYPE:
                    Translated->u.Connection.Type = CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C;
                    break;
                case SERIAL_BUS_SPI_DESCRIPTOR_TYPE:
                    Translated->u.Connection.Type = CM_RESOURCE_CONNECTION_TYPE_SERIAL_SPI;
                    break;
                case SERIAL_BUS_UART_DESCRIPTOR_TYPE:
                    Translated->u.Connection.Type = CM_RESOURCE_CONNECTION_TYPE_SERIAL_UART;
                    break;
                default:
                    return STATUS_NOT_SUPPORTED;
            }
            Shared = (Descriptor[RH_SERIAL_GENERAL_FLAGS] & 0x4) != 0;
            break;

        default:
            return STATUS_NOT_SUPPORTED;
    }

    Translated->ShareDisposition = Shared ? CmResourceShareShared : CmResourceShareDeviceExclusive;
    Translated->u.Connection.IdLowPart = Connection->Id.LowPart;
    Translated->u.Connection.IdHighPart = (ULONG)Connection->Id.HighPart;
    return STATUS_SUCCESS;
}

/**
 * @brief Looks up a connection the consumer translated before, so repeated queries keep their ids.
 */
static
PRH_CONNECTION
RhFindTranslatedLocked(
    _In_ PDEVICE_OBJECT Consumer,
    _In_reads_bytes_(Length) PUCHAR Descriptor,
    _In_ ULONG Length)
{
    PLIST_ENTRY Entry;

    for (Entry = RhConnections.Flink; Entry != &RhConnections; Entry = Entry->Flink)
    {
        PRH_CONNECTION Connection = CONTAINING_RECORD(Entry, RH_CONNECTION, Link);

        if (Connection->Consumer == Consumer &&
            Connection->DescriptorLength == Length &&
            RtlEqualMemory(Connection->Descriptor, Descriptor, Length))
        {
            return Connection;
        }
    }

    return NULL;
}

static
PRH_CONNECTION
RhFindByIdLocked(
    _In_ LARGE_INTEGER Id)
{
    PLIST_ENTRY Entry;

    for (Entry = RhConnections.Flink; Entry != &RhConnections; Entry = Entry->Flink)
    {
        PRH_CONNECTION Connection = CONTAINING_RECORD(Entry, RH_CONNECTION, Link);

        if (Connection->Id.QuadPart == Id.QuadPart)
            return Connection;
    }

    return NULL;
}

static
PRH_CONNECTION
RhFindByGsivLocked(
    _In_ ULONG Gsiv)
{
    PLIST_ENTRY Entry;

    for (Entry = RhConnections.Flink; Entry != &RhConnections; Entry = Entry->Flink)
    {
        PRH_CONNECTION Connection = CONTAINING_RECORD(Entry, RH_CONNECTION, Link);

        if (Connection->Gsiv != 0 && Connection->Gsiv == Gsiv)
            return Connection;
    }

    return NULL;
}

/**
 * @brief Translates one connection descriptor for ACPI.
 *
 * @param[in,out] ResultSize
 * The size of Result, or the size needed with STATUS_BUFFER_TOO_SMALL.
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
    _Inout_ PULONG ResultSize)
{
    PUCHAR Item = Descriptor;
    PRH_CONNECTION Connection, Existing;
    ANSI_STRING Source;
    ULONG ItemLength, Required;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Flags);

    PAGED_CODE();

    if (!Descriptor || DescriptorLength < RH_ITEM_HEADER)
        return STATUS_INVALID_PARAMETER;
    ItemLength = RH_ITEM_HEADER + RhReadUshort(Item, 1);
    if (ItemLength > DescriptorLength)
        return STATUS_INVALID_PARAMETER;

    Status = RhGetResourceSource(Item, ItemLength, &Source);
    if (!NT_SUCCESS(Status))
        return Status;

    Required = max(UACPINT_RH_TRANSLATION_RESULT_MIN_SIZE,
                   FIELD_OFFSET(UACPINT_RH_TRANSLATION_RESULT, ReferenceName) + Source.Length + 1);
    if (*ResultSize < Required)
    {
        *ResultSize = Required;
        return STATUS_BUFFER_TOO_SMALL;
    }

    ExAcquireFastMutex(&RhLock);
    Connection = RhFindTranslatedLocked(DeviceObject, Item, ItemLength);
    if (Connection)
        Result->Descriptor = Connection->Translated;
    ExReleaseFastMutex(&RhLock);

    if (!Connection)
    {
        Connection = ExAllocatePoolZero(NonPagedPool,
                                        FIELD_OFFSET(RH_CONNECTION, Descriptor[ItemLength]),
                                        RH_POOL_TAG);
        if (!Connection)
            return STATUS_INSUFFICIENT_RESOURCES;

        Connection->Consumer = DeviceObject;
        Connection->DescriptorLength = ItemLength;
        RtlCopyMemory(Connection->Descriptor, Item, ItemLength);
        Connection->Id.QuadPart = InterlockedIncrement64(&RhNextConnectionId);

        Status = RhQualifyName(DeviceObject, &Source, &Connection->Provider);
        if (NT_SUCCESS(Status))
        {
            if (Item[0] == GPIO_INTERRUPT_IO_DESCRIPTOR && Item[RH_GPIO_TYPE] == PNP_GPIO_IRQ_DESCRIPTOR_TYPE_INTERRUPT)
                Status = RhTranslateGpioInterrupt(Connection);
            else
                Status = RhTranslateConnection(Connection);
        }

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ACPIEX: descriptor 0x%x for %Z not translated, 0x%lx\n", Item[0], &Source, Status);
            if (Connection->Provider.Buffer)
                ExFreePoolWithTag(Connection->Provider.Buffer, RH_POOL_TAG);
            ExFreePoolWithTag(Connection, RH_POOL_TAG);
            return Status;
        }

        /* Two translations of the same descriptor may race, the first one in is kept */
        ExAcquireFastMutex(&RhLock);
        Existing = RhFindTranslatedLocked(DeviceObject, Item, ItemLength);
        if (!Existing)
            InsertTailList(&RhConnections, &Connection->Link);
        ExReleaseFastMutex(&RhLock);

        if (Existing)
        {
            ExFreePoolWithTag(Connection->Provider.Buffer, RH_POOL_TAG);
            ExFreePoolWithTag(Connection, RH_POOL_TAG);
            Connection = Existing;
        }
        else
        {
            DPRINT1("ACPIEX: %Z connection %I64u, GSIV %lu, provider %wZ\n",
                    &Source, Connection->Id.QuadPart, Connection->Gsiv, &Connection->Provider);
        }

        Result->Descriptor = Connection->Translated;
    }

    Result->Count = 1;
    Result->Reserved = 0;
    Result->ScopedNameOffset = FIELD_OFFSET(UACPINT_RH_TRANSLATION_RESULT, ReferenceName);
    Result->ScopedNameLength = Source.Length;
    RtlCopyMemory(Result->ReferenceName, Source.Buffer, Source.Length);
    Result->ReferenceName[Source.Length] = ANSI_NULL;
    *ResultSize = Required;
    return STATUS_SUCCESS;
}

/**
 * @brief Records the device ACPI knows by a namespace path.
 */
static
NTSTATUS
NTAPI
RhAssociateBiosName(
    _In_ PVOID Context,
    _In_ PCUNICODE_STRING ReferenceName,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PRH_PROVIDER Provider;
    PLIST_ENTRY Entry;

    UNREFERENCED_PARAMETER(Context);

    PAGED_CODE();

    if (!ReferenceName || !ReferenceName->Length || !DeviceObject)
        return STATUS_INVALID_PARAMETER;

    ExAcquireFastMutex(&RhLock);
    for (Entry = RhProviders.Flink; Entry != &RhProviders; Entry = Entry->Flink)
    {
        Provider = CONTAINING_RECORD(Entry, RH_PROVIDER, Link);
        if (RtlEqualUnicodeString(&Provider->BiosName, ReferenceName, TRUE))
        {
            Provider->DeviceObject = DeviceObject;
            ExReleaseFastMutex(&RhLock);
            return STATUS_SUCCESS;
        }
    }
    ExReleaseFastMutex(&RhLock);

    Provider = ExAllocatePoolZero(PagedPool, sizeof(*Provider) + ReferenceName->Length, RH_POOL_TAG);
    if (!Provider)
        return STATUS_INSUFFICIENT_RESOURCES;

    Provider->DeviceObject = DeviceObject;
    Provider->BiosName.Buffer = (PWCHAR)(Provider + 1);
    Provider->BiosName.MaximumLength = ReferenceName->Length;
    RtlCopyUnicodeString(&Provider->BiosName, ReferenceName);

    ExAcquireFastMutex(&RhLock);
    InsertTailList(&RhProviders, &Provider->Link);
    ExReleaseFastMutex(&RhLock);
    return STATUS_SUCCESS;
}

/**
 * @brief Returns the resource a GpioInt was translated to, by its GSIV.
 */
static
NTSTATUS
NTAPI
RhQueryGsivDescriptor(
    _In_ ULONG Gsiv,
    _In_ ULONG Flags,
    _Out_ PIO_RESOURCE_DESCRIPTOR IoDescriptor)
{
    PRH_CONNECTION Connection;
    NTSTATUS Status = STATUS_NOT_FOUND;

    UNREFERENCED_PARAMETER(Flags);

    ExAcquireFastMutex(&RhLock);
    Connection = RhFindByGsivLocked(Gsiv);
    if (Connection)
    {
        *IoDescriptor = Connection->Translated;
        Status = STATUS_SUCCESS;
    }
    ExReleaseFastMutex(&RhLock);

    return Status;
}

VOID
NTAPI
RhInitializeTranslation(VOID)
{
    ExInitializeFastMutex(&RhLock);
    InitializeListHead(&RhConnections);
    InitializeListHead(&RhProviders);
    RhNextConnectionId = 0;
}

/**
 * @brief Takes ACPI's half of the translation interface and fills in the hub's.
 */
NTSTATUS
NTAPI
RhExchangeTranslationInterface(
    _Inout_ PUACPINT_RH_TRANSLATION_INTERFACE Interface,
    _In_ ULONG InputLength,
    _In_ ULONG OutputLength,
    _Out_ PULONG_PTR Information)
{
    *Information = 0;

    if (InputLength < sizeof(*Interface) || OutputLength < sizeof(*Interface) ||
        Interface->Interface.Size < sizeof(*Interface))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (Interface->Interface.Version != UACPINT_RH_TRANSLATION_VERSION)
        return STATUS_REVISION_MISMATCH;

    ExAcquireFastMutex(&RhLock);
    RhQualifyBiosName = Interface->QualifyBiosName;
    RhAllocateSecondaryGsiv = Interface->AllocateSecondaryGsiv;
    RhSetInterruptProperties = Interface->SetInterruptProperties;
    ExReleaseFastMutex(&RhLock);

    Interface->Context = NULL;
    Interface->TranslateDescriptor = RhTranslateDescriptor;
    Interface->AssociateBiosName = RhAssociateBiosName;
    Interface->QueryGsivDescriptor = RhQueryGsivDescriptor;

    *Information = sizeof(*Interface);
    return STATUS_SUCCESS;
}

/**
 * @brief Answers IOCTL_RH_QUERY_CONNECTION_PROPERTIES with the raw descriptor.
 *
 * @return
 * STATUS_BUFFER_TOO_SMALL with the header filled in when the descriptor does not fit.
 */
NTSTATUS
NTAPI
RhQueryConnectionProperties(
    _In_ PRH_QUERY_CONNECTION_PROPERTIES_INPUT_BUFFER Input,
    _Out_writes_bytes_(OutputLength) PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Output,
    _In_ ULONG OutputLength,
    _Out_ PULONG_PTR Information)
{
    ULONG Header = FIELD_OFFSET(RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER, ConnectionProperties);
    PRH_CONNECTION Connection;
    NTSTATUS Status;

    *Information = 0;

    if (Input->Version != RH_QUERY_CONNECTION_PROPERTIES_INPUT_VERSION)
        return STATUS_REVISION_MISMATCH;
    if (OutputLength < Header)
        return STATUS_BUFFER_TOO_SMALL;

    ExAcquireFastMutex(&RhLock);

    if (Input->QueryType == ConnectionIdType)
        Connection = RhFindByIdLocked(Input->u.ConnectionId);
    else if (Input->QueryType == InterruptVectorType)
        Connection = RhFindByGsivLocked(Input->u.InterruptVector);
    else
        Connection = NULL;

    if (!Connection)
    {
        Status = STATUS_NOT_FOUND;
    }
    else
    {
        Output->Version = RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_VERSION;
        Output->PropertiesLength = Connection->DescriptorLength;
        *Information = Header;

        if (OutputLength - Header < Connection->DescriptorLength)
        {
            Status = STATUS_BUFFER_TOO_SMALL;
        }
        else
        {
            RtlCopyMemory(Output->ConnectionProperties, Connection->Descriptor, Connection->DescriptorLength);
            *Information = Header + Connection->DescriptorLength;
            Status = STATUS_SUCCESS;
        }
    }

    ExReleaseFastMutex(&RhLock);
    return Status;
}

NTSTATUS
NTAPI
RhQueryInitialPolarity(
    _In_ ULONG Gsiv,
    _Out_ PUCHAR InitialPolarity)
{
    PRH_CONNECTION Connection;
    NTSTATUS Status = STATUS_NOT_FOUND;

    ExAcquireFastMutex(&RhLock);
    Connection = RhFindByGsivLocked(Gsiv);
    if (Connection)
    {
        *InitialPolarity = Connection->InitialPolarity;
        Status = STATUS_SUCCESS;
    }
    ExReleaseFastMutex(&RhLock);

    return Status;
}

NTSTATUS
NTAPI
RhUpdateInitialPolarity(
    _In_ ULONG Gsiv,
    _In_ UCHAR InitialPolarity)
{
    PRH_CONNECTION Connection;
    NTSTATUS Status = STATUS_NOT_FOUND;

    ExAcquireFastMutex(&RhLock);
    Connection = RhFindByGsivLocked(Gsiv);
    if (Connection)
    {
        Connection->InitialPolarity = InitialPolarity;
        Status = STATUS_SUCCESS;
    }
    ExReleaseFastMutex(&RhLock);

    return Status;
}

/**
 * @brief Finds the NT name of the controller a connection belongs to.
 *
 * @param[out] TargetName
 * Receives the device name in RH_POOL_TAG pool the caller frees.
 */
NTSTATUS
NTAPI
RhGetConnectionTarget(
    _In_ LARGE_INTEGER Id,
    _Out_ PUNICODE_STRING TargetName)
{
    POBJECT_NAME_INFORMATION NameInfo;
    PDEVICE_OBJECT Target = NULL;
    PRH_CONNECTION Connection;
    PLIST_ENTRY Entry;
    ULONG Length = 0;
    NTSTATUS Status;

    PAGED_CODE();

    RtlZeroMemory(TargetName, sizeof(*TargetName));

    ExAcquireFastMutex(&RhLock);
    Connection = RhFindByIdLocked(Id);
    if (Connection)
    {
        for (Entry = RhProviders.Flink; Entry != &RhProviders; Entry = Entry->Flink)
        {
            PRH_PROVIDER Provider = CONTAINING_RECORD(Entry, RH_PROVIDER, Link);

            if (RtlEqualUnicodeString(&Provider->BiosName, &Connection->Provider, TRUE))
            {
                Target = Provider->DeviceObject;
                ObReferenceObject(Target);
                break;
            }
        }
    }
    ExReleaseFastMutex(&RhLock);

    if (!Connection)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    if (!Target)
        return STATUS_NO_SUCH_DEVICE;

    Status = ObQueryNameString(Target, NULL, 0, &Length);
    if (Status != STATUS_INFO_LENGTH_MISMATCH || Length == 0)
    {
        ObDereferenceObject(Target);
        return NT_SUCCESS(Status) ? STATUS_OBJECT_NAME_INVALID : Status;
    }

    NameInfo = ExAllocatePoolWithTag(PagedPool, Length, RH_POOL_TAG);
    if (!NameInfo)
    {
        ObDereferenceObject(Target);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ObQueryNameString(Target, NameInfo, Length, &Length);
    ObDereferenceObject(Target);
    if (NT_SUCCESS(Status) && !NameInfo->Name.Length)
        Status = STATUS_OBJECT_NAME_INVALID;

    if (NT_SUCCESS(Status))
    {
        /* Hand back only the string, in a buffer of its own */
        TargetName->Buffer = ExAllocatePoolWithTag(PagedPool, NameInfo->Name.Length, RH_POOL_TAG);
        if (TargetName->Buffer)
        {
            TargetName->MaximumLength = NameInfo->Name.Length;
            RtlCopyUnicodeString(TargetName, &NameInfo->Name);
        }
        else
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    ExFreePoolWithTag(NameInfo, RH_POOL_TAG);
    return Status;
}
