/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     HID keyboard mapper WMI data blocks
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kbdhid.h"

#define NDEBUG
#include <debug.h>

static GUID KbdHidPortInformationGuid = KEYBOARD_PORT_WMI_STD_DATA_GUID;
static GUID KbdHidExtendedIdGuid = KEYBOARD_PORT_WMI_EXTENDED_ID;

enum
{
    KbdHidWmiPortInformation = 0,
    KbdHidWmiExtendedId = 1
};

WMIGUIDREGINFO KbdHidWmiGuidList[2] =
{
    { &KbdHidPortInformationGuid, 1, 0 },
    { &KbdHidExtendedIdGuid, 1, 0 }
};

NTSTATUS
NTAPI
KbdHidQueryWmiRegInfo(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PULONG RegFlags,
    _Inout_ PUNICODE_STRING InstanceName,
    _Out_opt_ PUNICODE_STRING *RegistryPath,
    _Inout_ PUNICODE_STRING MofResourceName,
    _Out_opt_ PDEVICE_OBJECT *Pdo)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;

    UNREFERENCED_PARAMETER(InstanceName);
    UNREFERENCED_PARAMETER(MofResourceName);

    *RegFlags = WMIREG_FLAG_INSTANCE_PDO;
    *RegistryPath = &KbdHidGlobals.RegistryPath;
    *Pdo = DeviceExtension->PhysicalDeviceObject;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KbdHidQueryWmiDataBlock(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG InstanceCount,
    _Out_opt_ PULONG InstanceLengthArray,
    _In_ ULONG BufferAvail,
    _Out_opt_ PUCHAR Buffer)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PKEYBOARD_PORT_WMI_STD_DATA PortData;
    ULONG Size;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(InstanceIndex);
    UNREFERENCED_PARAMETER(InstanceCount);

    switch (GuidIndex)
    {
        case KbdHidWmiPortInformation:
            Size = sizeof(*PortData);
            if (BufferAvail < Size)
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PortData = (PKEYBOARD_PORT_WMI_STD_DATA)Buffer;
            PortData->ConnectorType = KEYBOARD_PORT_WMI_STD_USB;
            PortData->DataQueueSize = DeviceExtension->Attributes.InputDataQueueLength;
            PortData->ErrorCount = 0;
            PortData->FunctionKeys = DeviceExtension->Attributes.NumberOfFunctionKeys;
            PortData->Indicators = DeviceExtension->Attributes.NumberOfIndicators;
            *InstanceLengthArray = Size;
            Status = STATUS_SUCCESS;
            break;

        case KbdHidWmiExtendedId:
            Size = sizeof(DeviceExtension->IdEx);
            if (BufferAvail < Size)
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            RtlCopyMemory(Buffer, &DeviceExtension->IdEx, Size);
            *InstanceLengthArray = Size;
            Status = STATUS_SUCCESS;
            break;

        default:
            Size = 0;
            Status = STATUS_WMI_GUID_NOT_FOUND;
            break;
    }

    return WmiCompleteRequest(DeviceObject, Irp, Status, Size, IO_NO_INCREMENT);
}

static
NTSTATUS
KbdHidCompleteWmiSet(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ ULONG GuidIndex)
{
    NTSTATUS Status;

    /* Both blocks are read only */
    if ((GuidIndex == KbdHidWmiPortInformation) || (GuidIndex == KbdHidWmiExtendedId))
        Status = STATUS_WMI_READ_ONLY;
    else
        Status = STATUS_WMI_GUID_NOT_FOUND;

    return WmiCompleteRequest(DeviceObject, Irp, Status, 0, IO_NO_INCREMENT);
}

NTSTATUS
NTAPI
KbdHidSetWmiDataBlock(
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

    return KbdHidCompleteWmiSet(DeviceObject, Irp, GuidIndex);
}

NTSTATUS
NTAPI
KbdHidSetWmiDataItem(
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

    return KbdHidCompleteWmiSet(DeviceObject, Irp, GuidIndex);
}

NTSTATUS
NTAPI
KbdHidSystemControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    SYSCTL_IRP_DISPOSITION Disposition = IrpProcessed;
    NTSTATUS Status;

    Status = WmiSystemControl(&DeviceExtension->WmiLibInfo, DeviceObject, Irp, &Disposition);

    switch (Disposition)
    {
        case IrpProcessed:
            break;

        case IrpNotCompleted:
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            break;

        default:
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
            break;
    }

    return Status;
}
