/*
 * PROJECT:     ReactOS HID Mouse Mapper Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Driver entry, registry options and IOCTL handling
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "mouhid.h"

#define NDEBUG
#include <debug.h>

#if (NTDDI_VERSION < NTDDI_WIN10_VB)
_IRQL_requires_max_(PASSIVE_LEVEL)
NTKERNELAPI
NTSTATUS
NTAPI
IoOpenDriverRegistryKey(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ DRIVER_REGKEY_TYPE RegKeyType,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ ULONG Flags,
    _Out_ PHANDLE DriverRegKey);
#endif

MOUHID_GLOBALS MouHidGlobals;

static DRIVER_UNLOAD MouHidUnload;

_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
static DRIVER_DISPATCH MouHidPassThrough;

_Dispatch_type_(IRP_MJ_FLUSH_BUFFERS)
static DRIVER_DISPATCH MouHidFlush;

_Dispatch_type_(IRP_MJ_INTERNAL_DEVICE_CONTROL)
static DRIVER_DISPATCH MouHidInternalIoctl;

/**
 * @brief
 * Writes an entry to the system event log on behalf of the driver.
 */
VOID
NTAPI
MouHidLogError(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ NTSTATUS ErrorCode,
    _In_ NTSTATUS FinalStatus)
{
    PIO_ERROR_LOG_PACKET Entry;

    Entry = IoAllocateErrorLogEntry(DriverObject, sizeof(*Entry));
    if (Entry == NULL)
        return;

    RtlZeroMemory(Entry, sizeof(*Entry));
    Entry->ErrorCode = ErrorCode;
    Entry->FinalStatus = FinalStatus;
    IoWriteErrorLogEntry(Entry);
}

/**
 * @brief
 * Reads the driver wide options from the service Parameters key.
 *
 * @remarks
 * Windows stores the options before the query runs, so the registry values
 * never take effect. This driver keeps that behavior.
 */
static
NTSTATUS
NTAPI
MouHidReadDriverParameters(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PRTL_QUERY_REGISTRY_TABLE Table;
    HANDLE Key = NULL;
    ULONG UseOnlyMice;
    ULONG AbsoluteAsRelative;
    ULONG PointerAsAbsolute;
    NTSTATUS Status;

    Table = ExAllocatePool2(POOL_FLAG_PAGED, 4 * sizeof(*Table), MOUHID_TAG);
    if (Table == NULL)
        return STATUS_UNSUCCESSFUL;

    Status = IoOpenDriverRegistryKey(DriverObject, DriverRegKeyParameters, KEY_READ, 0, &Key);
    if (NT_SUCCESS(Status))
    {
        UseOnlyMice = MouHidGlobals.UseOnlyMice;
        AbsoluteAsRelative = MouHidGlobals.TreatAbsoluteAsRelative;
        PointerAsAbsolute = MouHidGlobals.TreatAbsolutePointerAsAbsolute;

        Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        Table[0].Name = L"UseOnlyMice";
        Table[0].EntryContext = &UseOnlyMice;
        Table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
        Table[0].DefaultData = &UseOnlyMice;
        Table[0].DefaultLength = sizeof(UseOnlyMice);

        Table[1].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        Table[1].Name = L"TreatAbsoluteAsRelative";
        Table[1].EntryContext = &AbsoluteAsRelative;
        Table[1].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
        Table[1].DefaultData = &AbsoluteAsRelative;
        Table[1].DefaultLength = sizeof(AbsoluteAsRelative);

        Table[2].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        Table[2].Name = L"TreatAbsolutePointerAsAbsolute";
        Table[2].EntryContext = &PointerAsAbsolute;
        Table[2].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
        Table[2].DefaultData = &PointerAsAbsolute;
        Table[2].DefaultLength = sizeof(PointerAsAbsolute);

        Status = RtlQueryRegistryValues(RTL_REGISTRY_HANDLE, (PCWSTR)Key, Table, NULL, NULL);
    }
    else
    {
        DPRINT1("MouHid: no Parameters key 0x%08lx\n", Status);
    }

    if (!NT_SUCCESS(Status))
    {
        MouHidGlobals.TreatAbsoluteAsRelative = FALSE;
        MouHidGlobals.TreatAbsolutePointerAsAbsolute = FALSE;
        MouHidGlobals.UseOnlyMice = FALSE;
        Status = STATUS_SUCCESS;
    }

    if (Key != NULL)
        ZwClose(Key);

    ExFreePoolWithTag(Table, MOUHID_TAG);
    return Status;
}

/**
 * @brief
 * Hands an IRP to the HID collection unchanged.
 */
static
NTSTATUS
NTAPI
MouHidPassThrough(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PMOUHID_EXTENSION Extension = DeviceObject->DeviceExtension;
    NTSTATUS Status;

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    Status = IoCallDriver(Extension->TopOfStack, Irp);
    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return Status;
}

/**
 * @brief
 * Turns a flush into IOCTL_HID_FLUSH_QUEUE for the HID collection.
 */
static
NTSTATUS
NTAPI
MouHidFlush(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PMOUHID_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION NextStack;
    NTSTATUS Status;

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    IoCopyCurrentIrpStackLocationToNext(Irp);
    NextStack = IoGetNextIrpStackLocation(Irp);
    NextStack->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    NextStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_FLUSH_QUEUE;

    Status = IoCallDriver(Extension->TopOfStack, Irp);
    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return Status;
}

/**
 * @brief
 * Handles the internal requests mouclass sends to its port.
 */
static
NTSTATUS
NTAPI
MouHidInternalIoctl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PMOUHID_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PCONNECT_DATA ConnectData;
    NTSTATUS Status;

    Irp->IoStatus.Information = 0;

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    switch (Stack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_MOUSE_QUERY_ATTRIBUTES:
        {
            if (Stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUSE_ATTRIBUTES))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer,
                          &Extension->Attributes,
                          sizeof(Extension->Attributes));
            Irp->IoStatus.Information = sizeof(Extension->Attributes);
            Status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_INTERNAL_MOUSE_CONNECT:
        {
            /* Only one class driver may own the callback */
            if (Extension->ConnectData.ClassService != NULL)
            {
                Status = STATUS_SHARING_VIOLATION;
                break;
            }

            if (Stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(CONNECT_DATA))
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            ConnectData = Stack->Parameters.DeviceIoControl.Type3InputBuffer;
            Extension->ConnectData = *ConnectData;
            Status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_INTERNAL_MOUSE_DISCONNECT:
            Status = STATUS_NOT_IMPLEMENTED;
            break;

        case IOCTL_INTERNAL_MOUSE_ENABLE:
        case IOCTL_INTERNAL_MOUSE_DISABLE:
            Status = STATUS_NOT_SUPPORTED;
            break;

        default:
            DPRINT1("MouHid: unhandled internal IOCTL 0x%08lx\n",
                    Stack->Parameters.DeviceIoControl.IoControlCode);
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return Status;
}

static
VOID
NTAPI
MouHidUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    ExFreePoolWithTag(MouHidGlobals.RegistryPath.Buffer, MOUHID_TAG);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS Status;

    RtlZeroMemory(&MouHidGlobals, sizeof(MouHidGlobals));

    MouHidGlobals.RegistryPath.Length = RegistryPath->Length;
    MouHidGlobals.RegistryPath.MaximumLength = RegistryPath->Length + sizeof(UNICODE_NULL);
    MouHidGlobals.RegistryPath.Buffer = ExAllocatePool2(POOL_FLAG_PAGED,
                                                        MouHidGlobals.RegistryPath.MaximumLength,
                                                        MOUHID_TAG);
    if (MouHidGlobals.RegistryPath.Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(MouHidGlobals.RegistryPath.Buffer, RegistryPath->Buffer, RegistryPath->Length);

    Status = MouHidReadDriverParameters(DriverObject);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(MouHidGlobals.RegistryPath.Buffer, MOUHID_TAG);
        return Status;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = MouHidCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = MouHidClose;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = MouHidInternalIoctl;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = MouHidPassThrough;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = MouHidFlush;
    DriverObject->MajorFunction[IRP_MJ_PNP] = MouHidPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = MouHidPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = MouHidSystemControl;
    DriverObject->DriverUnload = MouHidUnload;
    DriverObject->DriverExtension->AddDevice = MouHidAddDevice;

    return STATUS_SUCCESS;
}
