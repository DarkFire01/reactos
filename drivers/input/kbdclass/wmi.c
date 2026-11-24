/*
 * PROJECT:     ReactOS Keyboard Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     WMI support
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kbdclass.h"

#define NDEBUG
#include <debug.h>

#define KBDCLASS_WMI_CLASS_INFO     0
#define KBDCLASS_WMI_WAKE_ENABLE    1

static WMIGUIDREGINFO KbdClassWmiGuidList[] =
{
    { &MSKeyboard_ClassInformation_GUID, 1, 0 },
    { &GUID_POWER_DEVICE_WAKE_ENABLE, 1, 0 }
};

static
NTSTATUS
NTAPI
KbdClassQueryWmiRegInfo(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PULONG RegFlags,
    _Inout_ PUNICODE_STRING InstanceName,
    _Outptr_result_maybenull_ PUNICODE_STRING *RegistryPath,
    _Inout_ PUNICODE_STRING MofResourceName,
    _Outptr_result_maybenull_ PDEVICE_OBJECT *Pdo)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;

    UNREFERENCED_PARAMETER(InstanceName);
    UNREFERENCED_PARAMETER(MofResourceName);

    *RegFlags = WMIREG_FLAG_INSTANCE_PDO;
    *RegistryPath = &KbdClassGlobals.RegistryPath;
    *Pdo = DeviceExtension->Pdo;

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
KbdClassQueryWmiDataBlock(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG InstanceCount,
    _Out_writes_opt_(InstanceCount) PULONG InstanceLengthArray,
    _In_ ULONG BufferAvail,
    _Out_writes_bytes_opt_(BufferAvail) PUCHAR Buffer)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    ULONG Size = 0;
    NTSTATUS Status;

    switch (GuidIndex)
    {
        case KBDCLASS_WMI_CLASS_INFO:
            if (InstanceIndex != 0 || InstanceCount != 1)
            {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                break;
            }

            Size = sizeof(MSKeyboard_ClassInformation);
            if (BufferAvail < Size)
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            ((PMSKeyboard_ClassInformation)Buffer)->DeviceId = (ULONGLONG)(ULONG_PTR)DeviceObject;
            *InstanceLengthArray = Size;
            Status = STATUS_SUCCESS;
            break;

        case KBDCLASS_WMI_WAKE_ENABLE:
            if (InstanceIndex != 0 || InstanceCount != 1)
            {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                break;
            }

            Size = sizeof(BOOLEAN);
            if (BufferAvail < Size)
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            *(PBOOLEAN)Buffer = KbdClassWaitWakePending(DeviceExtension);
            *InstanceLengthArray = Size;
            Status = STATUS_SUCCESS;
            break;

        default:
            Status = STATUS_WMI_GUID_NOT_FOUND;
            break;
    }

    return WmiCompleteRequest(DeviceObject, Irp, Status, Size, IO_NO_INCREMENT);
}

static
NTSTATUS
NTAPI
KbdClassSetWmiDataBlock(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG BufferSize,
    _In_reads_bytes_(BufferSize) PUCHAR Buffer)
{
    ULONG Size = 0;
    NTSTATUS Status;

    switch (GuidIndex)
    {
        case KBDCLASS_WMI_CLASS_INFO:
            Status = STATUS_WMI_READ_ONLY;
            break;

        case KBDCLASS_WMI_WAKE_ENABLE:
            Size = sizeof(BOOLEAN);
            if (BufferSize == 0)
                Status = STATUS_BUFFER_TOO_SMALL;
            else if (InstanceIndex != 0)
                Status = STATUS_INVALID_DEVICE_REQUEST;
            else
                Status = KbdClassToggleWaitWake(DeviceObject->DeviceExtension, *(PBOOLEAN)Buffer);
            break;

        default:
            Status = STATUS_WMI_GUID_NOT_FOUND;
            break;
    }

    return WmiCompleteRequest(DeviceObject, Irp, Status, Size, IO_NO_INCREMENT);
}

static
NTSTATUS
NTAPI
KbdClassSetWmiDataItem(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG DataItemId,
    _In_ ULONG BufferSize,
    _In_reads_bytes_(BufferSize) PUCHAR Buffer)
{
    ULONG Size = 0;
    NTSTATUS Status;

    switch (GuidIndex)
    {
        case KBDCLASS_WMI_CLASS_INFO:
            Status = STATUS_WMI_READ_ONLY;
            break;

        case KBDCLASS_WMI_WAKE_ENABLE:
            Size = sizeof(BOOLEAN);
            if (BufferSize == 0)
                Status = STATUS_BUFFER_TOO_SMALL;
            else if (DataItemId != 1 || InstanceIndex != 0)
                Status = STATUS_INVALID_DEVICE_REQUEST;
            else
                Status = KbdClassToggleWaitWake(DeviceObject->DeviceExtension, *(PBOOLEAN)Buffer);
            break;

        default:
            Status = STATUS_WMI_GUID_NOT_FOUND;
            break;
    }

    return WmiCompleteRequest(DeviceObject, Irp, Status, Size, IO_NO_INCREMENT);
}

/**
 * @brief Registers the WMI blocks, the wake block only when wake works.
 */
VOID
NTAPI
KbdClassSetupWmi(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ BOOLEAN WakeCapable)
{
    PWMILIB_CONTEXT WmiLibInfo = &DeviceExtension->WmiLibInfo;

    WmiLibInfo->GuidCount = WakeCapable ? RTL_NUMBER_OF(KbdClassWmiGuidList) : 1;
    WmiLibInfo->GuidList = KbdClassWmiGuidList;
    WmiLibInfo->QueryWmiRegInfo = KbdClassQueryWmiRegInfo;
    WmiLibInfo->QueryWmiDataBlock = KbdClassQueryWmiDataBlock;
    WmiLibInfo->SetWmiDataBlock = KbdClassSetWmiDataBlock;
    WmiLibInfo->SetWmiDataItem = KbdClassSetWmiDataItem;
    WmiLibInfo->ExecuteWmiMethod = NULL;
    WmiLibInfo->WmiFunctionControl = NULL;

    IoWMIRegistrationControl(DeviceExtension->Self, WMIREG_ACTION_REGISTER);
}

NTSTATUS
NTAPI
KbdClassSystemControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    SYSCTL_IRP_DISPOSITION Disposition = IrpProcessed;
    NTSTATUS Status;

    Status = IoAcquireRemoveLock(&DeviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    Status = WmiSystemControl(&DeviceExtension->WmiLibInfo, DeviceObject, Irp, &Disposition);

    switch (Disposition)
    {
        case IrpProcessed:
            break;

        case IrpNotCompleted:
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            break;

        default:
            Status = KbdClassPassThrough(DeviceObject, Irp);
            break;
    }

    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    return Status;
}
