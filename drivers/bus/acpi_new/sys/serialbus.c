/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     GenericSerialBus operation regions, run as SPB sequences on the connection's controller
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <uacpint.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <spb.h>
#include <uacpi/opregion.h>
#define NDEBUG
#include <debug.h>

/* The field buffer starts with a status byte and a length byte, the data follows */
#define UACPINT_SERIAL_STATUS       0
#define UACPINT_SERIAL_LENGTH       1
#define UACPINT_SERIAL_DATA         2

#define UACPINT_SERIAL_OK           0x00
#define UACPINT_SERIAL_FAILED       0x01

/* A command byte then the data, the most an access needs */
#define UACPINT_SERIAL_TRANSFERS    2

/* The device a region belongs to is the consumer the hub translates for */
static
PDEVICE_OBJECT
UacpiNtSerialConsumer(
    _In_ uacpi_namespace_node *RegionNode)
{
    uacpi_namespace_node *Node;
    PUACPINT_PDO Pdo;

    for (Node = uacpi_namespace_node_parent(RegionNode); Node; Node = uacpi_namespace_node_parent(Node))
    {
        Pdo = UacpiNtFindPdoByNode(NULL, Node);
        if (Pdo && Pdo->Shared.Self)
            return Pdo->Shared.Self;
    }

    return NULL;
}

/* Bytes the access moves after its command, and whether it sends a command first */
static
BOOLEAN
UacpiNtSerialShape(
    _In_ const uacpi_region_serial_rw_data *Access,
    _Out_ PULONG DataLength,
    _Out_ PBOOLEAN HasCommand)
{
    switch (Access->access_attribute)
    {
        case UACPI_ACCESS_ATTRIBUTE_RAW_BYTES:
            *DataLength = Access->access_length;
            *HasCommand = FALSE;
            return TRUE;

        case UACPI_ACCESS_ATTRIBUTE_BYTES:
            *DataLength = Access->access_length;
            *HasCommand = TRUE;
            return TRUE;

        case UACPI_ACCESS_ATTRIBUTE_SEND_RECEIVE:
            *DataLength = 1;
            *HasCommand = FALSE;
            return TRUE;

        case UACPI_ACCESS_ATTRIBUTE_BYTE:
            *DataLength = 1;
            *HasCommand = TRUE;
            return TRUE;

        case UACPI_ACCESS_ATTRIBUTE_WORD:
            *DataLength = 2;
            *HasCommand = TRUE;
            return TRUE;

        default:
            return FALSE;
    }
}

static
NTSTATUS
UacpiNtSerialOpen(
    _In_ uacpi_namespace_node *RegionNode,
    _In_ const uacpi_data_view *Connection,
    _Out_ PHANDLE Handle)
{
    WCHAR PathBuffer[RESOURCE_HUB_CONNECTION_PATH_SIZE / sizeof(WCHAR)];
    UNICODE_STRING Path;
    IO_RESOURCE_DESCRIPTOR Descriptor;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;
    PDEVICE_OBJECT Consumer;
    NTSTATUS Status;

    *Handle = NULL;

    Consumer = UacpiNtSerialConsumer(RegionNode);
    if (!Consumer)
        return STATUS_NO_SUCH_DEVICE;

    Status = UacpiNtTranslateConnectionDescriptor(Consumer,
                                                  (PVOID)Connection->const_bytes,
                                                  (ULONG)Connection->length,
                                                  &Descriptor);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Descriptor.Type != CmResourceTypeConnection)
        return STATUS_INVALID_PARAMETER;

    RtlInitEmptyUnicodeString(&Path, PathBuffer, sizeof(PathBuffer));
    Status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&Path,
                                              Descriptor.u.Connection.IdLowPart,
                                              Descriptor.u.Connection.IdHighPart);
    if (!NT_SUCCESS(Status))
        return Status;

    InitializeObjectAttributes(&Attributes, &Path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwCreateFile(Handle,
                        GENERIC_READ | GENERIC_WRITE,
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

/* The command, if any, goes first, then the data in the direction of the access */
static
NTSTATUS
UacpiNtSerialTransfer(
    _In_ HANDLE Handle,
    _In_ BOOLEAN Read,
    _In_ BOOLEAN HasCommand,
    _In_ UCHAR Command,
    _Inout_updates_bytes_(DataLength) PUCHAR Data,
    _In_ ULONG DataLength,
    _Out_ PULONG Transferred)
{
    UCHAR ListBuffer[FIELD_OFFSET(SPB_TRANSFER_LIST, Transfers) +
                     UACPINT_SERIAL_TRANSFERS * sizeof(SPB_TRANSFER_LIST_ENTRY)];
    PSPB_TRANSFER_LIST List = (PSPB_TRANSFER_LIST)ListBuffer;
    IO_STATUS_BLOCK IoStatus;
    PUCHAR Write = NULL;
    ULONG Count = 0;
    NTSTATUS Status;

    *Transferred = 0;
    RtlZeroMemory(ListBuffer, sizeof(ListBuffer));

    /* A write sends the command and the data as one message */
    if (!Read && HasCommand)
    {
        Write = ExAllocatePoolWithTag(NonPagedPool, DataLength + 1, UACPINT_POOL_TAG);
        if (!Write)
            return STATUS_INSUFFICIENT_RESOURCES;

        Write[0] = Command;
        RtlCopyMemory(Write + 1, Data, DataLength);
        List->Transfers[Count++] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0,
                                                                       Write, DataLength + 1);
    }
    else
    {
        if (HasCommand)
        {
            List->Transfers[Count++] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0,
                                                                           &Command, sizeof(Command));
        }

        List->Transfers[Count++] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(Read ? SpbTransferDirectionFromDevice :
                                                                              SpbTransferDirectionToDevice,
                                                                       0,
                                                                       Data,
                                                                       DataLength);
    }

    List->Size = sizeof(SPB_TRANSFER_LIST);
    List->TransferCount = Count;

    Status = ZwDeviceIoControlFile(Handle,
                                   NULL,
                                   NULL,
                                   NULL,
                                   &IoStatus,
                                   IOCTL_SPB_EXECUTE_SEQUENCE,
                                   List,
                                   FIELD_OFFSET(SPB_TRANSFER_LIST, Transfers) + Count * sizeof(SPB_TRANSFER_LIST_ENTRY),
                                   NULL,
                                   0);
    if (NT_SUCCESS(Status))
        *Transferred = (ULONG)IoStatus.Information;

    if (Write)
        ExFreePoolWithTag(Write, UACPINT_POOL_TAG);
    return Status;
}

static
uacpi_status
UacpiNtSerialAccess(
    _In_ BOOLEAN Read,
    _Inout_ uacpi_region_serial_rw_data *Access)
{
    PUCHAR Buffer = Access->in_out_buffer.bytes;
    SIZE_T Length = Access->in_out_buffer.length;
    ULONG DataLength, Transferred;
    BOOLEAN HasCommand;
    HANDLE Handle;
    NTSTATUS Status;

    if (!Buffer || Length < UACPINT_SERIAL_DATA)
        return UACPI_STATUS_INVALID_ARGUMENT;

    if (!UacpiNtSerialShape(Access, &DataLength, &HasCommand))
    {
        DPRINT1("uACPI-NT: serial bus access attribute 0x%x is not supported\n", Access->access_attribute);
        return UACPI_STATUS_UNIMPLEMENTED;
    }

    DataLength = (ULONG)min(DataLength, Length - UACPINT_SERIAL_DATA);

    Status = UacpiNtSerialOpen(Access->region_context, &Access->connection, &Handle);
    if (NT_SUCCESS(Status))
    {
        Status = UacpiNtSerialTransfer(Handle,
                                       Read,
                                       HasCommand,
                                       (UCHAR)Access->command,
                                       Buffer + UACPINT_SERIAL_DATA,
                                       DataLength,
                                       &Transferred);
        ZwClose(Handle);
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("uACPI-NT: serial bus %s of %lu bytes failed 0x%lx\n", Read ? "read" : "write", DataLength, Status);
        Buffer[UACPINT_SERIAL_STATUS] = UACPINT_SERIAL_FAILED;
        Buffer[UACPINT_SERIAL_LENGTH] = 0;
        return UACPI_STATUS_HARDWARE_TIMEOUT;
    }

    Buffer[UACPINT_SERIAL_STATUS] = UACPINT_SERIAL_OK;
    Buffer[UACPINT_SERIAL_LENGTH] = (UCHAR)min(DataLength, 0xFF);
    return UACPI_STATUS_OK;
}

static
uacpi_status
UacpiNtSerialDispatch(
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

        case UACPI_REGION_OP_SERIAL_READ:
            return UacpiNtSerialAccess(TRUE, Data);

        case UACPI_REGION_OP_SERIAL_WRITE:
            return UacpiNtSerialAccess(FALSE, Data);

        default:
            return UACPI_STATUS_UNIMPLEMENTED;
    }
}

/**
 * @brief Serves every GenericSerialBus region from ACPI itself, as Windows does.
 *
 * Accesses fail until the resource hub and the connection's controller are up.
 */
VOID
NTAPI
UacpiNtSerialBusInit(VOID)
{
    uacpi_status UacpiStatus;

    UacpiStatus = uacpi_install_address_space_handler(uacpi_namespace_root(),
                                                      UACPI_ADDRESS_SPACE_GENERIC_SERIAL_BUS,
                                                      UacpiNtSerialDispatch,
                                                      NULL);
    if (uacpi_unlikely_error(UacpiStatus))
        DPRINT1("uACPI-NT: serial bus regions not served: %s\n", uacpi_status_to_string(UacpiStatus));
}
