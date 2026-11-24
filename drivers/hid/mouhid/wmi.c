/*
 * PROJECT:     ReactOS HID Mouse Mapper Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     WMI pointer port information
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "mouhid.h"

#define NDEBUG
#include <debug.h>

static GUID MouHidPointerPortGuid = POINTER_PORT_WMI_STD_DATA_GUID;

static WMIGUIDREGINFO MouHidWmiGuidList[] =
{
    { &MouHidPointerPortGuid, 1, 0 }
};

static
NTSTATUS
NTAPI
MouHidQueryWmiRegInfo(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PULONG RegFlags,
    _Inout_ PUNICODE_STRING InstanceName,
    _Out_opt_ PUNICODE_STRING *RegistryPath,
    _Inout_ PUNICODE_STRING MofResourceName,
    _Out_opt_ PDEVICE_OBJECT *Pdo)
{
    PMOUHID_EXTENSION Extension = DeviceObject->DeviceExtension;

    UNREFERENCED_PARAMETER(InstanceName);
    UNREFERENCED_PARAMETER(MofResourceName);

    *RegFlags = WMIREG_FLAG_INSTANCE_PDO;
    *RegistryPath = &MouHidGlobals.RegistryPath;
    *Pdo = Extension->Pdo;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
MouHidQueryWmiDataBlock(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG InstanceCount,
    _Out_opt_ PULONG InstanceLengthArray,
    _In_ ULONG BufferAvail,
    _Out_opt_ PUCHAR Buffer)
{
    PMOUHID_EXTENSION Extension = DeviceObject->DeviceExtension;
    PPOINTER_PORT_WMI_STD_DATA PortData;
    ULONG Size = 0;
    NTSTATUS Status;

    if (GuidIndex != 0)
    {
        Status = STATUS_WMI_GUID_NOT_FOUND;
    }
    else if ((InstanceIndex != 0) || (InstanceCount != 1))
    {
        Status = STATUS_INVALID_DEVICE_REQUEST;
    }
    else
    {
        Size = sizeof(*PortData);
        if (BufferAvail < Size)
        {
            Status = STATUS_BUFFER_TOO_SMALL;
        }
        else
        {
            PortData = (PPOINTER_PORT_WMI_STD_DATA)Buffer;
            PortData->ConnectorType = POINTER_PORT_WMI_STD_USB;
            PortData->DataQueueSize = Extension->Attributes.InputDataQueueLength;
            PortData->ErrorCount = 0;
            PortData->Buttons = Extension->Attributes.NumberOfButtons;
            PortData->HardwareType = POINTER_PORT_WMI_STD_MOUSE;
            *InstanceLengthArray = Size;
            Status = STATUS_SUCCESS;
        }
    }

    return WmiCompleteRequest(DeviceObject, Irp, Status, Size, IO_NO_INCREMENT);
}

static
NTSTATUS
NTAPI
MouHidSetWmiDataBlock(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG BufferSize,
    _In_ PUCHAR Buffer)
{
    UNREFERENCED_PARAMETER(InstanceIndex);
    UNREFERENCED_PARAMETER(BufferSize);
    UNREFERENCED_PARAMETER(Buffer);

    return WmiCompleteRequest(DeviceObject,
                              Irp,
                              (GuidIndex != 0) ? STATUS_WMI_GUID_NOT_FOUND : STATUS_WMI_READ_ONLY,
                              0,
                              IO_NO_INCREMENT);
}

static
NTSTATUS
NTAPI
MouHidSetWmiDataItem(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG DataItemId,
    _In_ ULONG BufferSize,
    _In_ PUCHAR Buffer)
{
    UNREFERENCED_PARAMETER(InstanceIndex);
    UNREFERENCED_PARAMETER(DataItemId);
    UNREFERENCED_PARAMETER(BufferSize);
    UNREFERENCED_PARAMETER(Buffer);

    return WmiCompleteRequest(DeviceObject,
                              Irp,
                              (GuidIndex != 0) ? STATUS_WMI_GUID_NOT_FOUND : STATUS_WMI_READ_ONLY,
                              0,
                              IO_NO_INCREMENT);
}

VOID
NTAPI
MouHidInitializeWmi(
    _In_ PMOUHID_EXTENSION Extension)
{
    PWMILIB_CONTEXT Context = &Extension->WmiLibInfo;

    Context->GuidCount = RTL_NUMBER_OF(MouHidWmiGuidList);
    Context->GuidList = MouHidWmiGuidList;
    Context->QueryWmiRegInfo = MouHidQueryWmiRegInfo;
    Context->QueryWmiDataBlock = MouHidQueryWmiDataBlock;
    Context->SetWmiDataBlock = MouHidSetWmiDataBlock;
    Context->SetWmiDataItem = MouHidSetWmiDataItem;
    Context->ExecuteWmiMethod = NULL;
    Context->WmiFunctionControl = NULL;
}

NTSTATUS
NTAPI
MouHidSystemControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PMOUHID_EXTENSION Extension = DeviceObject->DeviceExtension;
    SYSCTL_IRP_DISPOSITION Disposition = IrpProcessed;
    NTSTATUS Status;

    Status = WmiSystemControl(&Extension->WmiLibInfo, DeviceObject, Irp, &Disposition);
    switch (Disposition)
    {
        case IrpProcessed:
            break;

        case IrpNotCompleted:
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            break;

        default:
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(Extension->TopOfStack, Irp);
            break;
    }

    return Status;
}
