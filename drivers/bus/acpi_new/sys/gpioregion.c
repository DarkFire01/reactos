/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     GeneralPurposeIo operation regions, run as pin reads and writes on the connection's controller
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <uacpint.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <gpio.h>
#include <uacpi/acpi.h>
#include <uacpi/opregion.h>
#include <uacpi/internal/types.h>
#include <uacpi/internal/namespace.h>
#define NDEBUG
#include <debug.h>

/* GpioIo descriptor layout, ACPI 5.0 section 6.4.3.8.1 */
#define UACPINT_GPIO_DESCRIPTOR_TAG     0x8C
#define UACPINT_GPIO_IO_FLAGS           7
#define UACPINT_GPIO_PIN_TABLE          14
#define UACPINT_GPIO_SOURCE_NAME        17
#define UACPINT_GPIO_MINIMUM_LENGTH     0x17

#define UACPINT_GPIO_RESTRICTION_MASK   0x3
#define UACPINT_GPIO_RESTRICTION_INPUT  0x1
#define UACPINT_GPIO_RESTRICTION_OUTPUT 0x2

/* One Connection() a device used, its translation result is kept for good */
typedef struct _UACPINT_GPIO_CONNECTION
{
    LIST_ENTRY Link;
    PDEVICE_OBJECT Consumer;
    PUCHAR Descriptor;
    ULONG DescriptorLength;
    NTSTATUS Status;
    ULONG PinCount;
    ACCESS_MASK Access;
    LARGE_INTEGER Id;
    HANDLE Handle;
} UACPINT_GPIO_CONNECTION, *PUACPINT_GPIO_CONNECTION;

/* A device whose _REG was told the GPIO space is up */
typedef struct _UACPINT_GPIO_REG_DEVICE
{
    LIST_ENTRY Link;
    uacpi_namespace_node *Node;
} UACPINT_GPIO_REG_DEVICE, *PUACPINT_GPIO_REG_DEVICE;

/* The I/O below needs APC delivery, so a resource and not a fast mutex guards these */
static ERESOURCE UacpiNtGpioLock;
static LIST_ENTRY UacpiNtGpioConnections;
static LIST_ENTRY UacpiNtGpioRegDevices;

static
USHORT
UacpiNtGpioReadUshort(
    _In_ const UCHAR *Bytes)
{
    return (USHORT)(Bytes[0] | (Bytes[1] << 8));
}

/**
 * @brief The device a region belongs to is the direct parent of the region, a PDO or a filtered PDO.
 */
static
PDEVICE_OBJECT
UacpiNtGpioConsumer(
    _In_ uacpi_namespace_node *RegionNode)
{
    uacpi_namespace_node *Parent = uacpi_namespace_node_parent(RegionNode);
    PDEVICE_OBJECT Consumer = NULL;
    PUACPINT_FLT Filter;
    PUACPINT_PDO Pdo;
    PLIST_ENTRY Entry;

    if (!Parent || !GlobalAcpiFdo)
        return NULL;

    Pdo = UacpiNtFindPdoByNode(GlobalAcpiFdo, Parent);
    if (Pdo)
        return Pdo->Shared.Self;

    ExAcquireFastMutex(&GlobalAcpiFdo->ChildLock);
    for (Entry = GlobalAcpiFdo->FilterList.Flink; Entry != &GlobalAcpiFdo->FilterList; Entry = Entry->Flink)
    {
        Filter = CONTAINING_RECORD(Entry, UACPINT_FLT, Link);
        if (Filter->Node == Parent)
        {
            Consumer = Filter->ForeignPdo;
            break;
        }
    }
    ExReleaseFastMutex(&GlobalAcpiFdo->ChildLock);

    return Consumer;
}

/**
 * @brief Translates a GpioIo descriptor and records its pin count and IoRestriction.
 *
 * @return A failure here is final for this connection, it is never retried.
 */
static
NTSTATUS
UacpiNtGpioAssociate(
    _Inout_ PUACPINT_GPIO_CONNECTION Connection)
{
    const UCHAR *Bytes = Connection->Descriptor;
    IO_RESOURCE_DESCRIPTOR IoDescriptor;
    USHORT PinTable, SourceName;
    NTSTATUS Status;

    if ((Connection->DescriptorLength < 3) ||
        ((ULONG)UacpiNtGpioReadUshort(Bytes + 1) + 3 > Connection->DescriptorLength))
    {
        return STATUS_UNSUCCESSFUL;
    }

    /* Named connections are resolved to their buffer by the interpreter, anything else is foreign */
    if (Bytes[0] != UACPINT_GPIO_DESCRIPTOR_TAG)
        return STATUS_NOT_SUPPORTED;

    Status = UacpiNtTranslateConnectionDescriptor(Connection->Consumer,
                                                  (PVOID)Bytes,
                                                  Connection->DescriptorLength,
                                                  &IoDescriptor);
    if (!NT_SUCCESS(Status))
        return Status;

    if ((IoDescriptor.Type != CmResourceTypeConnection) ||
        (IoDescriptor.u.Connection.Class != CM_RESOURCE_CONNECTION_CLASS_GPIO))
    {
        return STATUS_UNSUCCESSFUL;
    }

    if (Connection->DescriptorLength < UACPINT_GPIO_MINIMUM_LENGTH)
        return STATUS_NOT_SUPPORTED;

    PinTable = UacpiNtGpioReadUshort(Bytes + UACPINT_GPIO_PIN_TABLE);
    SourceName = UacpiNtGpioReadUshort(Bytes + UACPINT_GPIO_SOURCE_NAME);
    if (SourceName < PinTable)
        return STATUS_NOT_SUPPORTED;

    Connection->PinCount = (SourceName - PinTable) / sizeof(USHORT);
    Connection->Id.LowPart = IoDescriptor.u.Connection.IdLowPart;
    Connection->Id.HighPart = IoDescriptor.u.Connection.IdHighPart;

    switch (Bytes[UACPINT_GPIO_IO_FLAGS] & UACPINT_GPIO_RESTRICTION_MASK)
    {
        case UACPINT_GPIO_RESTRICTION_INPUT:
            Connection->Access = GENERIC_READ;
            break;

        case UACPINT_GPIO_RESTRICTION_OUTPUT:
            Connection->Access = GENERIC_WRITE;
            break;

        default:
            Connection->Access = 0;
            break;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief Finds the connection a device already used with these descriptor bytes, or records a new one.
 */
static
PUACPINT_GPIO_CONNECTION
UacpiNtGpioLookup(
    _In_ PDEVICE_OBJECT Consumer,
    _In_ const uacpi_data_view *Data)
{
    PUACPINT_GPIO_CONNECTION Connection;
    PLIST_ENTRY Entry;

    for (Entry = UacpiNtGpioConnections.Flink; Entry != &UacpiNtGpioConnections; Entry = Entry->Flink)
    {
        Connection = CONTAINING_RECORD(Entry, UACPINT_GPIO_CONNECTION, Link);
        if ((Connection->Consumer == Consumer) &&
            (Connection->DescriptorLength == Data->length) &&
            (RtlCompareMemory(Connection->Descriptor, Data->const_bytes, Data->length) == Data->length))
        {
            return Connection;
        }
    }

    Connection = ExAllocatePoolZero(NonPagedPool, sizeof(*Connection) + Data->length, UACPINT_POOL_TAG);
    if (!Connection)
        return NULL;

    Connection->Consumer = Consumer;
    Connection->Descriptor = (PUCHAR)(Connection + 1);
    Connection->DescriptorLength = (ULONG)Data->length;
    RtlCopyMemory(Connection->Descriptor, Data->const_bytes, Data->length);

    Connection->Status = UacpiNtGpioAssociate(Connection);
    if (!NT_SUCCESS(Connection->Status))
        DPRINT1("uACPI-NT: GPIO connection not usable 0x%lx\n", Connection->Status);

    InsertTailList(&UacpiNtGpioConnections, &Connection->Link);
    return Connection;
}

/**
 * @brief Opens the connection through the resource hub, the first access decides the mode.
 */
static
NTSTATUS
UacpiNtGpioOpen(
    _Inout_ PUACPINT_GPIO_CONNECTION Connection,
    _In_ BOOLEAN Write)
{
    WCHAR PathBuffer[RESOURCE_HUB_CONNECTION_PATH_SIZE / sizeof(WCHAR)];
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;
    UNICODE_STRING Path;
    ACCESS_MASK Access;
    NTSTATUS Status;

    if (Connection->Handle)
        return STATUS_SUCCESS;

    Access = Connection->Access;
    if (!Access)
        Access = Write ? GENERIC_WRITE : GENERIC_READ;

    RtlInitEmptyUnicodeString(&Path, PathBuffer, sizeof(PathBuffer));
    Status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&Path, Connection->Id.LowPart, Connection->Id.HighPart);
    if (!NT_SUCCESS(Status))
        return Status;

    InitializeObjectAttributes(&Attributes, &Path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwCreateFile(&Connection->Handle,
                        Access,
                        &Attributes,
                        &IoStatus,
                        NULL,
                        0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_OPEN,
                        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                        NULL,
                        0);
}

/**
 * @brief Moves the field value to or from the pins, bit n being entry n of the pin table.
 */
static
NTSTATUS
UacpiNtGpioTransfer(
    _In_ PUACPINT_GPIO_CONNECTION Connection,
    _In_ BOOLEAN Write,
    _Inout_ uacpi_u64 *Value)
{
    UCHAR Pins[sizeof(uacpi_u64)];
    IO_STATUS_BLOCK IoStatus;
    ULONG Bytes = (Connection->PinCount + 7) / 8;
    NTSTATUS Status;

    if (Write)
    {
        RtlCopyMemory(Pins, Value, Bytes);
        return ZwDeviceIoControlFile(Connection->Handle,
                                     NULL,
                                     NULL,
                                     NULL,
                                     &IoStatus,
                                     IOCTL_GPIO_WRITE_PINS,
                                     Pins,
                                     Bytes,
                                     NULL,
                                     0);
    }

    RtlZeroMemory(Pins, sizeof(Pins));
    Status = ZwDeviceIoControlFile(Connection->Handle,
                                   NULL,
                                   NULL,
                                   NULL,
                                   &IoStatus,
                                   IOCTL_GPIO_READ_PINS,
                                   NULL,
                                   0,
                                   Pins,
                                   Bytes);
    if (NT_SUCCESS(Status))
        RtlCopyMemory(Value, Pins, Bytes);

    return Status;
}

/**
 * @brief Serves one field access.
 *
 * Only malformed requests fail the AML. Once a request is well formed, problems with the
 * connection or the controller are logged and the access completes, a read returning zero.
 */
static
uacpi_status
UacpiNtGpioAccess(
    _In_ BOOLEAN Write,
    _Inout_ uacpi_region_gpio_rw_data *Access)
{
    PUACPINT_GPIO_CONNECTION Connection;
    PDEVICE_OBJECT Consumer;
    NTSTATUS Status;

    Consumer = UacpiNtGpioConsumer(Access->region_context);
    if (!Consumer || !Access->connection.length)
        return UACPI_STATUS_UNIMPLEMENTED;

    /* The field has to start at the first pin of its Connection */
    if (Access->pin_offset)
        return UACPI_STATUS_INVALID_ARGUMENT;

    if (!Write)
        Access->value = 0;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&UacpiNtGpioLock, TRUE);

    Connection = UacpiNtGpioLookup(Consumer, &Access->connection);
    if (!Connection || !NT_SUCCESS(Connection->Status))
        goto Done;

    /* A field covers the whole pin table or nothing */
    if (Connection->PinCount != Access->num_pins || Connection->PinCount > 64)
    {
        DPRINT1("uACPI-NT: GPIO field of %u pins over a %lu pin connection\n",
                Access->num_pins, Connection->PinCount);
        goto Done;
    }

    Status = UacpiNtGpioOpen(Connection, Write);
    if (NT_SUCCESS(Status))
        Status = UacpiNtGpioTransfer(Connection, Write, &Access->value);

    if (!NT_SUCCESS(Status))
        DPRINT1("uACPI-NT: GPIO %s failed 0x%lx\n", Write ? "write" : "read", Status);

Done:
    ExReleaseResourceLite(&UacpiNtGpioLock);
    KeLeaveCriticalRegion();
    return UACPI_STATUS_OK;
}

static
uacpi_status
UacpiNtGpioDispatch(
    _In_ uacpi_region_op Op,
    _In_ uacpi_handle Data)
{
    uacpi_region_attach_data *Attach;

    switch (Op)
    {
        case UACPI_REGION_OP_ATTACH:
            Attach = Data;
            Attach->out_region_context = Attach->region_node;
            return UACPI_STATUS_OK;

        case UACPI_REGION_OP_DETACH:
            return UACPI_STATUS_OK;

        case UACPI_REGION_OP_GPIO_READ:
            return UacpiNtGpioAccess(FALSE, Data);

        case UACPI_REGION_OP_GPIO_WRITE:
            return UacpiNtGpioAccess(TRUE, Data);

        default:
            return UACPI_STATUS_UNIMPLEMENTED;
    }
}

/* Stops at nested devices, their regions belong to them */
static
uacpi_iteration_decision
UacpiNtGpioFindRegion(
    _In_ void *Context,
    _In_ uacpi_namespace_node *Node,
    _In_ uacpi_u32 Depth)
{
    PBOOLEAN Found = Context;
    uacpi_object_type Type;
    uacpi_object *Object;

    UNREFERENCED_PARAMETER(Depth);

    if (uacpi_unlikely_error(uacpi_namespace_node_type(Node, &Type)))
        return UACPI_ITERATION_DECISION_CONTINUE;

    if (Type == UACPI_OBJECT_DEVICE)
        return UACPI_ITERATION_DECISION_NEXT_PEER;

    if (Type != UACPI_OBJECT_OPERATION_REGION)
        return UACPI_ITERATION_DECISION_CONTINUE;

    Object = uacpi_namespace_node_get_object(Node);
    if (Object && Object->op_region &&
        (Object->op_region->space == UACPI_ADDRESS_SPACE_GENERAL_PURPOSE_IO))
    {
        *Found = TRUE;
        return UACPI_ITERATION_DECISION_BREAK;
    }

    return UACPI_ITERATION_DECISION_CONTINUE;
}

static
VOID
UacpiNtGpioRunReg(
    _In_ uacpi_namespace_node *Node,
    _In_ BOOLEAN Connect)
{
    uacpi_object *Arguments[2];
    uacpi_object_array Array;

    Arguments[0] = uacpi_object_create_integer(UACPI_ADDRESS_SPACE_GENERAL_PURPOSE_IO);
    Arguments[1] = uacpi_object_create_integer(Connect ? ACPI_REG_CONNECT : ACPI_REG_DISCONNECT);
    if (Arguments[0] && Arguments[1])
    {
        Array.objects = Arguments;
        Array.count = RTL_NUMBER_OF(Arguments);
        uacpi_eval(Node, "_REG", &Array, NULL);
    }

    if (Arguments[0])
        uacpi_object_unref(Arguments[0]);
    if (Arguments[1])
        uacpi_object_unref(Arguments[1]);
}

/**
 * @brief Tells a device's _REG that the GPIO space came up or went away with the device.
 *
 * Only devices that own a GeneralPurposeIo region hear about it, and each connect is paired
 * with one disconnect.
 */
VOID
NTAPI
UacpiNtGpioRegionDeviceState(
    _In_opt_ uacpi_namespace_node *Node,
    _In_ BOOLEAN Running)
{
    PUACPINT_GPIO_REG_DEVICE Device = NULL;
    BOOLEAN Found = FALSE;
    PLIST_ENTRY Entry;

    if (!Node)
        return;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&UacpiNtGpioLock, TRUE);

    for (Entry = UacpiNtGpioRegDevices.Flink; Entry != &UacpiNtGpioRegDevices; Entry = Entry->Flink)
    {
        if (CONTAINING_RECORD(Entry, UACPINT_GPIO_REG_DEVICE, Link)->Node == Node)
        {
            Device = CONTAINING_RECORD(Entry, UACPINT_GPIO_REG_DEVICE, Link);
            break;
        }
    }

    if (Running && !Device)
    {
        uacpi_namespace_for_each_child(Node,
                                       UacpiNtGpioFindRegion,
                                       NULL,
                                       UACPI_OBJECT_ANY_BIT,
                                       UACPI_MAX_DEPTH_ANY,
                                       &Found);

        Device = Found ? ExAllocatePoolZero(NonPagedPool, sizeof(*Device), UACPINT_POOL_TAG) : NULL;
        if (Device)
        {
            Device->Node = Node;
            InsertTailList(&UacpiNtGpioRegDevices, &Device->Link);
        }
    }
    else if (!Running && Device)
    {
        RemoveEntryList(&Device->Link);
    }
    else
    {
        Device = NULL;
    }

    ExReleaseResourceLite(&UacpiNtGpioLock);
    KeLeaveCriticalRegion();

    if (!Device)
        return;

    UacpiNtGpioRunReg(Node, Running);
    if (!Running)
        ExFreePoolWithTag(Device, UACPINT_POOL_TAG);
}

/**
 * @brief Serves every GeneralPurposeIo region from ACPI itself, as Windows does.
 *
 * Installed before the namespace loads so no _REG runs early, devices get theirs when they start.
 */
VOID
NTAPI
UacpiNtGpioRegionInit(VOID)
{
    uacpi_status UacpiStatus;

    ExInitializeResourceLite(&UacpiNtGpioLock);
    InitializeListHead(&UacpiNtGpioConnections);
    InitializeListHead(&UacpiNtGpioRegDevices);

    UacpiStatus = uacpi_install_address_space_handler(uacpi_namespace_root(),
                                                      UACPI_ADDRESS_SPACE_GENERAL_PURPOSE_IO,
                                                      UacpiNtGpioDispatch,
                                                      NULL);
    if (uacpi_unlikely_error(UacpiStatus))
        DPRINT1("uACPI-NT: GPIO regions not served: %s\n", uacpi_status_to_string(UacpiStatus));
}
