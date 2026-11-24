/*
 * PROJECT:     ReactOS HID Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     WMI power controls of devices and collections
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/* The FDO offers selective suspend, a collection offers waking the system */
static WMIGUIDREGINFO HidClassFdoWmiGuids[] =
{
    { &GUID_POWER_DEVICE_ENABLE, 1, 0 }
};

static WMIGUIDREGINFO HidClassPdoWmiGuids[] =
{
    { &GUID_POWER_DEVICE_WAKE_ENABLE, 1, 0 }
};

static
NTSTATUS
NTAPI
HidClassWmiQueryRegInfo(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PULONG RegFlags,
    _Inout_ PUNICODE_STRING InstanceName,
    _Out_opt_ PUNICODE_STRING *RegistryPath,
    _Inout_ PUNICODE_STRING MofResourceName,
    _Out_opt_ PDEVICE_OBJECT *Pdo)
{
    PHIDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PHIDCLASS_FDO_EXTENSION Fdo = HidClassGetFdo(DeviceExtension);

    UNREFERENCED_PARAMETER(InstanceName);
    UNREFERENCED_PARAMETER(MofResourceName);

    *RegFlags = WMIREG_FLAG_INSTANCE_PDO;

    if (RegistryPath)
        *RegistryPath = &Fdo->DriverExtension->RegistryPath;

    if (Pdo)
        *Pdo = DeviceExtension->IsClientPdo ? DeviceObject : DeviceExtension->Hid.PhysicalDeviceObject;

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HidClassWmiQueryDataBlock(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG InstanceCount,
    _Out_opt_ PULONG InstanceLengthArray,
    _In_ ULONG BufferAvail,
    _Out_opt_ PUCHAR Buffer)
{
    PHIDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    ULONG Size = 0;
    NTSTATUS Status;

    if (GuidIndex != 0)
    {
        Status = STATUS_WMI_GUID_NOT_FOUND;
    }
    else if (InstanceIndex != 0 || InstanceCount != 1)
    {
        Status = STATUS_INVALID_DEVICE_REQUEST;
    }
    else
    {
        Size = sizeof(BOOLEAN);
        if (BufferAvail < Size || !Buffer)
        {
            Status = STATUS_BUFFER_TOO_SMALL;
        }
        else
        {
            if (DeviceExtension->IsClientPdo)
                *(PBOOLEAN)Buffer = DeviceExtension->Pdo.RemoteWakeEnabled;
            else
                *(PBOOLEAN)Buffer = (DeviceExtension->Fdo.IdleFlags & HIDCLASS_IDLE_SS_ON) != 0;

            if (InstanceLengthArray)
                InstanceLengthArray[0] = Size;
            Status = STATUS_SUCCESS;
        }
    }

    return WmiCompleteRequest(DeviceObject, Irp, Status, Size, IO_NO_INCREMENT);
}

static
NTSTATUS
NTAPI
HidClassWmiApply(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ BOOLEAN Enable)
{
    if (DeviceExtension->IsClientPdo)
        return HidClassToggleRemoteWake(&DeviceExtension->Pdo, Enable);

    return HidClassToggleSelectiveSuspend(&DeviceExtension->Fdo, Enable);
}

static
NTSTATUS
NTAPI
HidClassWmiSetDataBlock(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG BufferSize,
    _In_ PUCHAR Buffer)
{
    NTSTATUS Status;

    if (GuidIndex != 0)
        Status = STATUS_WMI_GUID_NOT_FOUND;
    else if (BufferSize == 0)
        Status = STATUS_BUFFER_TOO_SMALL;
    else if (InstanceIndex != 0)
        Status = STATUS_INVALID_DEVICE_REQUEST;
    else
        Status = HidClassWmiApply(DeviceObject->DeviceExtension, *(PBOOLEAN)Buffer);

    return WmiCompleteRequest(DeviceObject, Irp, Status, (GuidIndex == 0) ? sizeof(BOOLEAN) : 0, IO_NO_INCREMENT);
}

static
NTSTATUS
NTAPI
HidClassWmiSetDataItem(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG DataItemId,
    _In_ ULONG BufferSize,
    _In_ PUCHAR Buffer)
{
    NTSTATUS Status;

    if (GuidIndex != 0)
        Status = STATUS_WMI_GUID_NOT_FOUND;
    else if (BufferSize == 0)
        Status = STATUS_BUFFER_TOO_SMALL;
    else if (DataItemId != 1 || InstanceIndex != 0)
        Status = STATUS_INVALID_DEVICE_REQUEST;
    else
        Status = HidClassWmiApply(DeviceObject->DeviceExtension, *(PBOOLEAN)Buffer);

    return WmiCompleteRequest(DeviceObject, Irp, Status, (GuidIndex == 0) ? sizeof(BOOLEAN) : 0, IO_NO_INCREMENT);
}

static
VOID
NTAPI
HidClassFillWmiContext(
    _Out_ PWMILIB_CONTEXT Context,
    _In_ PWMIGUIDREGINFO GuidList,
    _In_ ULONG GuidCount)
{
    RtlZeroMemory(Context, sizeof(*Context));
    Context->GuidCount = GuidCount;
    Context->GuidList = GuidList;
    Context->QueryWmiRegInfo = HidClassWmiQueryRegInfo;
    Context->QueryWmiDataBlock = HidClassWmiQueryDataBlock;
    Context->SetWmiDataBlock = HidClassWmiSetDataBlock;
    Context->SetWmiDataItem = HidClassWmiSetDataItem;
}

VOID
NTAPI
HidClassInitializeFdoWmi(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    HidClassFillWmiContext(&Fdo->WmiLibInfo, HidClassFdoWmiGuids, RTL_NUMBER_OF(HidClassFdoWmiGuids));

    if (!Fdo->WmiRegistered && NT_SUCCESS(IoWMIRegistrationControl(Fdo->DeviceObject, WMIREG_ACTION_REGISTER)))
        Fdo->WmiRegistered = TRUE;
}

VOID
NTAPI
HidClassInitializePdoWmi(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo)
{
    HidClassFillWmiContext(&Pdo->WmiLibInfo, HidClassPdoWmiGuids, RTL_NUMBER_OF(HidClassPdoWmiGuids));

    if (!Pdo->WmiRegistered && NT_SUCCESS(IoWMIRegistrationControl(Pdo->DeviceObject, WMIREG_ACTION_REGISTER)))
        Pdo->WmiRegistered = TRUE;
}

NTSTATUS
NTAPI
HidClassSystemControl(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp)
{
    SYSCTL_IRP_DISPOSITION Disposition = IrpNotWmi;
    PWMILIB_CONTEXT Context;
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS Status;

    if (DeviceExtension->IsClientPdo)
    {
        Context = &DeviceExtension->Pdo.WmiLibInfo;
        DeviceObject = DeviceExtension->Pdo.DeviceObject;
    }
    else
    {
        Context = &DeviceExtension->Fdo.WmiLibInfo;
        DeviceObject = DeviceExtension->Fdo.DeviceObject;
    }

    if (!Context->GuidCount)
        return HidClassPassOrComplete(DeviceExtension, Irp);

    Status = WmiSystemControl(Context, DeviceObject, Irp, &Disposition);
    switch (Disposition)
    {
        case IrpProcessed:
            return Status;

        case IrpNotCompleted:
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return Status;

        default:
            return HidClassPassOrComplete(DeviceExtension, Irp);
    }
}
