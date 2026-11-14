/*
 * PROJECT:     ReactOS ACPI driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Operation region handlers registered by other drivers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <uacpint.h>
#include <oprghdlr.h>
#include <uacpi/opregion.h>

#define NDEBUG
#include <debug.h>

/* What RegisterOpRegionHandler hands back as the operation region object */
typedef struct _UACPINT_OPREGION_CLIENT
{
    LIST_ENTRY Link;
    PACPI_OP_REGION_HANDLER Handler;
    PVOID Context;
    ULONG AccessType;
    ULONG RegionSpace;
} UACPINT_OPREGION_CLIENT, *PUACPINT_OPREGION_CLIENT;

static LIST_ENTRY UacpiNtOpRegionClients = { &UacpiNtOpRegionClients, &UacpiNtOpRegionClients };
static KSPIN_LOCK UacpiNtOpRegionLock;

static
VOID
__cdecl
UacpiNtOpRegionDone(
    _In_ PVOID Context)
{
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
}

/* Hands an access to the driver's handler, waiting for it when the driver completes it later */
static
uacpi_status
UacpiNtOpRegionAccess(
    _In_ PUACPINT_OPREGION_CLIENT Client,
    _In_ ULONG Direction,
    _Inout_ uacpi_region_rw_data *Access)
{
    ULONG64 Value = Access->value;
    KEVENT Done;
    NTSTATUS Status;

    KeInitializeEvent(&Done, NotificationEvent, FALSE);

    Status = Client->Handler(Direction,
                             Access->region_context,
                             (ULONG)Access->offset,
                             Access->byte_width,
                             (PULONG)&Value,
                             (ULONG_PTR)Client->Context,
                             (PACPI_OP_REGION_CALLBACK)UacpiNtOpRegionDone,
                             &Done);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Done, Executive, KernelMode, FALSE, NULL);
        Status = STATUS_SUCCESS;
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Operation region space 0x%lx handler failed 0x%lx\n", Client->RegionSpace, Status);
        return UACPI_STATUS_INTERNAL_ERROR;
    }

    if (Direction == ACPI_OPREGION_READ)
        Access->value = Value;

    return UACPI_STATUS_OK;
}

static
uacpi_status
UacpiNtOpRegionDispatch(
    _In_ uacpi_region_op Op,
    _In_ uacpi_handle Data)
{
    uacpi_region_attach_data *Attach;
    uacpi_region_rw_data *Access;

    switch (Op)
    {
        case UACPI_REGION_OP_ATTACH:
            /* The driver identifies a region by its namespace object */
            Attach = Data;
            Attach->out_region_context = Attach->region_node;
            return UACPI_STATUS_OK;

        case UACPI_REGION_OP_DETACH:
            return UACPI_STATUS_OK;

        case UACPI_REGION_OP_READ:
            Access = Data;
            return UacpiNtOpRegionAccess(Access->handler_context, ACPI_OPREGION_READ, Access);

        case UACPI_REGION_OP_WRITE:
            Access = Data;
            return UacpiNtOpRegionAccess(Access->handler_context, ACPI_OPREGION_WRITE, Access);

        default:
            return UACPI_STATUS_UNIMPLEMENTED;
    }
}

/**
 * @brief Routes every operation region of a space to a driver's handler.
 *
 * @return STATUS_INVALID_PARAMETER_2 for an unknown access type, STATUS_INVALID_PARAMETER_3 for a
 *         space that is neither predefined nor OEM defined.
 */
NTSTATUS
RegisterOpRegionHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG AccessType,
    _In_ ULONG RegionSpace,
    _In_ PACPI_OP_REGION_HANDLER Handler,
    _In_opt_ PVOID Context,
    _In_ ULONG Flags,
    _Out_ PVOID *OperationRegionObject)
{
    PUACPINT_OPREGION_CLIENT Client;
    uacpi_status UacpiStatus;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(Flags);

    if (!DeviceObject)
        return STATUS_INVALID_PARAMETER;

    *OperationRegionObject = NULL;

    if (AccessType != ACPI_OPREGION_ACCESS_AS_RAW && AccessType != ACPI_OPREGION_ACCESS_AS_COOKED)
        return STATUS_INVALID_PARAMETER_2;

    if (RegionSpace > 9 && (RegionSpace < 0x80 || RegionSpace > 0xFF))
        return STATUS_INVALID_PARAMETER_3;

    Client = ExAllocatePoolZero(NonPagedPool, sizeof(*Client), UACPINT_POOL_TAG);
    if (!Client)
        return STATUS_INSUFFICIENT_RESOURCES;

    Client->Handler = Handler;
    Client->Context = Context;
    Client->AccessType = AccessType;
    Client->RegionSpace = RegionSpace;

    UacpiStatus = uacpi_install_address_space_handler(uacpi_namespace_root(),
                                                      (enum uacpi_address_space)RegionSpace,
                                                      UacpiNtOpRegionDispatch,
                                                      Client);
    if (uacpi_unlikely_error(UacpiStatus))
    {
        DPRINT1("Operation region space 0x%lx refused: %s\n", RegionSpace, uacpi_status_to_string(UacpiStatus));
        ExFreePoolWithTag(Client, UACPINT_POOL_TAG);
        return STATUS_UNSUCCESSFUL;
    }

    KeAcquireSpinLock(&UacpiNtOpRegionLock, &OldIrql);
    InsertTailList(&UacpiNtOpRegionClients, &Client->Link);
    KeReleaseSpinLock(&UacpiNtOpRegionLock, OldIrql);

    *OperationRegionObject = Client;
    return STATUS_SUCCESS;
}

NTSTATUS
DeRegisterOpRegionHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PVOID OperationRegionObject)
{
    PUACPINT_OPREGION_CLIENT Client = NULL;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    if (!DeviceObject)
        return STATUS_INVALID_PARAMETER;

    KeAcquireSpinLock(&UacpiNtOpRegionLock, &OldIrql);
    for (Entry = UacpiNtOpRegionClients.Flink; Entry != &UacpiNtOpRegionClients; Entry = Entry->Flink)
    {
        if (Entry == OperationRegionObject)
        {
            Client = CONTAINING_RECORD(Entry, UACPINT_OPREGION_CLIENT, Link);
            RemoveEntryList(&Client->Link);
            break;
        }
    }
    KeReleaseSpinLock(&UacpiNtOpRegionLock, OldIrql);

    if (!Client)
        return STATUS_INVALID_PARAMETER;

    uacpi_uninstall_address_space_handler(uacpi_namespace_root(),
                                          (enum uacpi_address_space)Client->RegionSpace,
                                          UacpiNtOpRegionDispatch);
    ExFreePoolWithTag(Client, UACPINT_POOL_TAG);
    return STATUS_SUCCESS;
}
