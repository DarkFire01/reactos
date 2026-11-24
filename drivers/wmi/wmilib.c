/*
 * PROJECT:         ReactOS WMI driver
 * COPYRIGHT:       GPL - See COPYING in the top level directory
 * FILE:            drivers/wmi/wmilib.c
 * PURPOSE:         Windows Management Instrumentation
 * PROGRAMMERS:     Aleksey Bragin (aleksey@reactos.org)
 *
 */

/* INCLUDES *****************************************************************/

#include <stdio.h>
#include <ntddk.h>
#include <wmilib.h>
#include <wmistr.h>

#define NDEBUG
#include <debug.h>

#define TAG_WMILIB 'LimW'

#define WMILIB_ALIGN8(Value) (((ULONGLONG)(Value) + 7) & ~(ULONGLONG)7)

/* FUNCTIONS ****************************************************************/

static
BOOLEAN
WmiLibpFindGuid(
    _In_ PWMILIB_CONTEXT WmiLibInfo,
    _In_ LPCGUID Guid,
    _Out_ PULONG GuidIndex)
{
    ULONG Index;

    for (Index = 0; Index < WmiLibInfo->GuidCount; Index++)
    {
        /* GUIDs marked for removal are no longer served */
        if (WmiLibInfo->GuidList[Index].Flags & WMIREG_FLAG_REMOVE_GUID)
            continue;

        if (IsEqualGUID(WmiLibInfo->GuidList[Index].Guid, Guid))
        {
            *GuidIndex = Index;
            return TRUE;
        }
    }

    *GuidIndex = 0;
    return FALSE;
}

static
ULONG
WmiLibpPutCountedString(
    _Out_writes_bytes_(sizeof(USHORT) + String->Length) PUCHAR Buffer,
    _In_ PCUNICODE_STRING String)
{
    *(PUSHORT)Buffer = String->Length;
    RtlCopyMemory(Buffer + sizeof(USHORT), String->Buffer, String->Length);
    return sizeof(USHORT) + String->Length;
}

/**
 * @brief Answers IRP_MN_REGINFO and IRP_MN_REGINFO_EX from the GUID list and QueryWmiRegInfo.
 */
static
NTSTATUS
NTAPI
WmiLibpBuildRegInfo(
    _In_ PWMILIB_CONTEXT WmiLibInfo,
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ BOOLEAN Extended,
    _Out_writes_bytes_(BufferSize) PVOID Buffer,
    _In_ ULONG BufferSize,
    _Out_ PULONG_PTR Information)
{
    UNICODE_STRING InstanceName, MofResourceName, RegistryPathCopy;
    PUNICODE_STRING RegistryPath = NULL;
    PWMIREGINFOW RegInfo = Buffer;
    PDEVICE_OBJECT Pdo = NULL;
    PWMIREGGUIDW RegGuid;
    ULONGLONG Size;
    ULONG PathOffset = 0, MofOffset = 0, NameOffset = 0;
    ULONG RegFlags = 0;
    ULONG Index;
    NTSTATUS Status;

    *Information = 0;

    if (!WmiLibInfo->QueryWmiRegInfo)
        return STATUS_INVALID_DEVICE_REQUEST;

    RtlZeroMemory(&InstanceName, sizeof(InstanceName));
    RtlZeroMemory(&MofResourceName, sizeof(MofResourceName));

    Status = WmiLibInfo->QueryWmiRegInfo(DeviceObject,
                                         &RegFlags,
                                         &InstanceName,
                                         &RegistryPath,
                                         &MofResourceName,
                                         &Pdo);
    if (!NT_SUCCESS(Status))
        goto Exit;

    /* Without a PDO the driver's instance name is the base name for every GUID */
    if (RegFlags & WMIREG_FLAG_INSTANCE_PDO)
    {
        RegFlags &= ~WMIREG_FLAG_INSTANCE_BASENAME;
    }
    else if (InstanceName.Buffer)
    {
        RegFlags |= WMIREG_FLAG_INSTANCE_BASENAME;
    }

    Size = FIELD_OFFSET(WMIREGINFOW, WmiRegGuid) +
           (ULONGLONG)WmiLibInfo->GuidCount * sizeof(WMIREGGUIDW);

    if (RegistryPath && RegistryPath->Buffer)
    {
        PathOffset = (ULONG)Size;
        Size += sizeof(USHORT) + (RegistryPath->Length & ~1);
    }

    if (MofResourceName.Buffer)
    {
        MofOffset = (ULONG)Size;
        Size += sizeof(USHORT) + (MofResourceName.Length & ~1);
    }

    if (RegFlags & WMIREG_FLAG_INSTANCE_BASENAME)
    {
        NameOffset = (ULONG)Size;
        Size += sizeof(USHORT) + (InstanceName.Length & ~1);
    }

    if (Size > MAXULONG)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    /* Too small, WMI retries with the size left in the first ULONG */
    if (Size > BufferSize)
    {
        if (BufferSize >= sizeof(ULONG))
        {
            RegInfo->BufferSize = (ULONG)Size;
            *Information = sizeof(ULONG);
        }
        Status = STATUS_BUFFER_TOO_SMALL;
        goto Exit;
    }

    RtlZeroMemory(RegInfo, (SIZE_T)Size);
    RegInfo->BufferSize = (ULONG)Size;
    RegInfo->NextWmiRegInfo = 0;
    RegInfo->RegistryPath = PathOffset;
    RegInfo->MofResourceName = MofOffset;
    RegInfo->GuidCount = WmiLibInfo->GuidCount;

    for (Index = 0; Index < WmiLibInfo->GuidCount; Index++)
    {
        RegGuid = &RegInfo->WmiRegGuid[Index];
        RegGuid->Guid = *WmiLibInfo->GuidList[Index].Guid;
        RegGuid->Flags = WmiLibInfo->GuidList[Index].Flags | RegFlags;
        RegGuid->InstanceCount = WmiLibInfo->GuidList[Index].InstanceCount;

        if (RegFlags & WMIREG_FLAG_INSTANCE_PDO)
        {
            /* WMI drops one PDO reference per GUID after an extended request */
            RegGuid->Pdo = (ULONG_PTR)Pdo;
            if (Extended && Pdo)
                ObReferenceObject(Pdo);
        }
        else if (RegFlags & WMIREG_FLAG_INSTANCE_BASENAME)
        {
            RegGuid->BaseNameOffset = NameOffset;
        }
    }

    if (PathOffset)
    {
        RegistryPathCopy = *RegistryPath;
        RegistryPathCopy.Length &= ~1;
        WmiLibpPutCountedString((PUCHAR)RegInfo + PathOffset, &RegistryPathCopy);
    }

    if (MofOffset)
    {
        MofResourceName.Length &= ~1;
        WmiLibpPutCountedString((PUCHAR)RegInfo + MofOffset, &MofResourceName);
    }

    if (NameOffset)
    {
        InstanceName.Length &= ~1;
        WmiLibpPutCountedString((PUCHAR)RegInfo + NameOffset, &InstanceName);
    }

    *Information = (ULONG)Size;
    Status = STATUS_SUCCESS;

Exit:
    /* The instance name buffer was allocated by the driver for us to free */
    if (InstanceName.Buffer)
        ExFreePool(InstanceName.Buffer);

    return Status;
}

/* Instances sit on eight byte boundaries, the driver gave only their lengths. */
static
VOID
WmiLibpPackInstances(
    _Inout_ PWNODE_ALL_DATA Wnode)
{
    PULONG Lengths = (PULONG)Wnode->OffsetInstanceDataAndLength;
    ULONG Count = Wnode->InstanceCount;
    ULONG Index, Length;
    ULONGLONG Offset;

    Offset = Wnode->DataBlockOffset;
    for (Index = 0; Index < Count; Index++)
        Offset += WMILIB_ALIGN8(Lengths[Index]);

    /* The lengths fill the first half of the array, so walk it from the end */
    for (Index = Count; Index-- > 0; )
    {
        Length = Lengths[Index];
        Offset -= WMILIB_ALIGN8(Length);
        Wnode->OffsetInstanceDataAndLength[Index].OffsetInstanceData = (ULONG)Offset;
        Wnode->OffsetInstanceDataAndLength[Index].LengthInstanceData = Length;
    }
}

static
VOID
WmiLibpMakeTooSmall(
    _Inout_ PWNODE_HEADER Wnode,
    _In_ ULONGLONG SizeNeeded)
{
    PWNODE_TOO_SMALL TooSmall = (PWNODE_TOO_SMALL)Wnode;

    TooSmall->WnodeHeader.BufferSize = sizeof(WNODE_TOO_SMALL);
    TooSmall->WnodeHeader.Flags |= WNODE_FLAG_TOO_SMALL;
    TooSmall->SizeNeeded = (SizeNeeded > MAXULONG) ? MAXULONG : (ULONG)SizeNeeded;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
WmiCompleteRequest(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status,
    _In_ ULONG BufferUsed,
    _In_ CCHAR PriorityBoost)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PWNODE_SINGLE_INSTANCE WnodeSi;
    PWNODE_METHOD_ITEM WnodeMethod;
    PWNODE_ALL_DATA WnodeAll;
    ULONG_PTR Information = 0;
    ULONG DataOffset;

    UNREFERENCED_PARAMETER(DeviceObject);

    switch (Stack->MinorFunction)
    {
        case IRP_MN_QUERY_ALL_DATA:
            WnodeAll = Stack->Parameters.WMI.Buffer;
            DataOffset = WnodeAll->DataBlockOffset;
            if (NT_SUCCESS(Status))
            {
                WmiLibpPackInstances(WnodeAll);
                WnodeAll->WnodeHeader.BufferSize = DataOffset + BufferUsed;
                KeQuerySystemTime(&WnodeAll->WnodeHeader.TimeStamp);
                Information = WnodeAll->WnodeHeader.BufferSize;
            }
            else if (Status == STATUS_BUFFER_TOO_SMALL)
            {
                WmiLibpMakeTooSmall(&WnodeAll->WnodeHeader, (ULONGLONG)DataOffset + BufferUsed);
                Information = sizeof(WNODE_TOO_SMALL);
                Status = STATUS_SUCCESS;
            }
            break;

        case IRP_MN_QUERY_SINGLE_INSTANCE:
            WnodeSi = Stack->Parameters.WMI.Buffer;
            DataOffset = WnodeSi->DataBlockOffset;
            if (NT_SUCCESS(Status))
            {
                WnodeSi->SizeDataBlock = BufferUsed;
                WnodeSi->WnodeHeader.BufferSize = DataOffset + BufferUsed;
                KeQuerySystemTime(&WnodeSi->WnodeHeader.TimeStamp);
                Information = WnodeSi->WnodeHeader.BufferSize;
            }
            else if (Status == STATUS_BUFFER_TOO_SMALL)
            {
                WmiLibpMakeTooSmall(&WnodeSi->WnodeHeader, (ULONGLONG)DataOffset + BufferUsed);
                Information = sizeof(WNODE_TOO_SMALL);
                Status = STATUS_SUCCESS;
            }
            break;

        case IRP_MN_EXECUTE_METHOD:
            WnodeMethod = Stack->Parameters.WMI.Buffer;
            DataOffset = WnodeMethod->DataBlockOffset;
            if (NT_SUCCESS(Status))
            {
                WnodeMethod->SizeDataBlock = BufferUsed;
                WnodeMethod->WnodeHeader.BufferSize = DataOffset + BufferUsed;
                Information = WnodeMethod->WnodeHeader.BufferSize;
            }
            else if (Status == STATUS_BUFFER_TOO_SMALL)
            {
                WmiLibpMakeTooSmall(&WnodeMethod->WnodeHeader, (ULONGLONG)DataOffset + BufferUsed);
                Information = sizeof(WNODE_TOO_SMALL);
                Status = STATUS_SUCCESS;
            }
            break;

        default:
            break;
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, PriorityBoost);
    return Status;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
WmiFireEvent(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ LPCGUID Guid,
    _In_ ULONG InstanceIndex,
    _In_ ULONG EventDataSize,
    _In_reads_bytes_opt_(EventDataSize) PVOID EventData)
{
    ULONG DataOffset = FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData);
    PWNODE_SINGLE_INSTANCE Event;
    NTSTATUS Status;

    if (EventDataSize > MAXULONG - DataOffset)
    {
        Event = NULL;
    }
    else
    {
        Event = ExAllocatePoolWithTag(NonPagedPool, DataOffset + EventDataSize, TAG_WMILIB);
    }

    if (Event)
    {
        RtlZeroMemory(Event, DataOffset);
        Event->WnodeHeader.BufferSize = DataOffset + EventDataSize;
        Event->WnodeHeader.ProviderId = IoWMIDeviceObjectToProviderId(DeviceObject);
        Event->WnodeHeader.Flags = WNODE_FLAG_SINGLE_INSTANCE |
                                   WNODE_FLAG_EVENT_ITEM |
                                   WNODE_FLAG_STATIC_INSTANCE_NAMES;
        Event->WnodeHeader.Guid = *Guid;
        KeQuerySystemTime(&Event->WnodeHeader.TimeStamp);
        Event->InstanceIndex = InstanceIndex;
        Event->DataBlockOffset = DataOffset;
        Event->SizeDataBlock = EventDataSize;
        if (EventData && EventDataSize)
            RtlCopyMemory(Event->VariableData, EventData, EventDataSize);
    }

    /* The event data belongs to us from here on, whatever happens */
    if (EventData)
        ExFreePool(EventData);

    if (!Event)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoWMIWriteEvent(Event);
    if (!NT_SUCCESS(Status))
        ExFreePoolWithTag(Event, TAG_WMILIB);

    return Status;
}

/* Records an error WmiLib found itself, the driver still completes the IRP. */
static
NTSTATUS
WmiLibpNotCompleted(
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information,
    _Out_ PSYSCTL_IRP_DISPOSITION IrpDisposition)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    *IrpDisposition = IrpNotCompleted;
    return Status;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
WmiSystemControl(
    _In_ PWMILIB_CONTEXT WmiLibInfo,
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _Out_ PSYSCTL_IRP_DISPOSITION IrpDisposition)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PWNODE_SINGLE_INSTANCE WnodeSi;
    PWNODE_SINGLE_ITEM WnodeItem;
    PWNODE_METHOD_ITEM WnodeMethod;
    PWNODE_ALL_DATA WnodeAll;
    ULONG_PTR Information;
    ULONG BufferSize, GuidIndex, InstanceCount;
    ULONGLONG DataOffset;
    PULONG LengthArray;
    PUCHAR Data, Buffer;
    NTSTATUS Status;

    if ((Stack->MajorFunction != IRP_MJ_SYSTEM_CONTROL) ||
        ((Stack->MinorFunction > IRP_MN_EXECUTE_METHOD) && (Stack->MinorFunction != IRP_MN_REGINFO_EX)))
    {
        *IrpDisposition = IrpNotWmi;
        return Irp->IoStatus.Status;
    }

    /* Requests for another device of the stack pass through */
    if (Stack->Parameters.WMI.ProviderId != (ULONG_PTR)DeviceObject)
    {
        *IrpDisposition = IrpForward;
        return Irp->IoStatus.Status;
    }

    Buffer = Stack->Parameters.WMI.Buffer;
    BufferSize = Stack->Parameters.WMI.BufferSize;

    if ((Stack->MinorFunction == IRP_MN_REGINFO) || (Stack->MinorFunction == IRP_MN_REGINFO_EX))
    {
        Status = WmiLibpBuildRegInfo(WmiLibInfo,
                                     DeviceObject,
                                     Stack->MinorFunction == IRP_MN_REGINFO_EX,
                                     Buffer,
                                     BufferSize,
                                     &Information);
        return WmiLibpNotCompleted(Irp, Status, Information, IrpDisposition);
    }

    if (!WmiLibpFindGuid(WmiLibInfo, Stack->Parameters.WMI.DataPath, &GuidIndex))
        return WmiLibpNotCompleted(Irp, STATUS_WMI_GUID_NOT_FOUND, 0, IrpDisposition);

    InstanceCount = WmiLibInfo->GuidList[GuidIndex].InstanceCount;

    switch (Stack->MinorFunction)
    {
        case IRP_MN_QUERY_ALL_DATA:
            if (!WmiLibInfo->QueryWmiDataBlock)
                return WmiLibpNotCompleted(Irp, STATUS_INVALID_DEVICE_REQUEST, 0, IrpDisposition);

            if (BufferSize < (ULONG)FIELD_OFFSET(WNODE_ALL_DATA, OffsetInstanceDataAndLength))
                return WmiLibpNotCompleted(Irp, STATUS_BUFFER_TOO_SMALL, 0, IrpDisposition);

            /* The offset and length array comes first, then the instances */
            WnodeAll = (PWNODE_ALL_DATA)Buffer;
            DataOffset = WMILIB_ALIGN8(FIELD_OFFSET(WNODE_ALL_DATA, OffsetInstanceDataAndLength) +
                                       (ULONGLONG)InstanceCount * sizeof(OFFSETINSTANCEDATAANDLENGTH));
            if (DataOffset > MAXULONG)
                return WmiLibpNotCompleted(Irp, STATUS_INVALID_PARAMETER, 0, IrpDisposition);

            WnodeAll->DataBlockOffset = (ULONG)DataOffset;
            WnodeAll->InstanceCount = InstanceCount;
            if (DataOffset <= BufferSize)
            {
                LengthArray = (PULONG)WnodeAll->OffsetInstanceDataAndLength;
                Data = Buffer + (ULONG)DataOffset;
                BufferSize -= (ULONG)DataOffset;
            }
            else
            {
                LengthArray = NULL;
                Data = NULL;
                BufferSize = 0;
            }

            *IrpDisposition = IrpProcessed;
            return WmiLibInfo->QueryWmiDataBlock(DeviceObject,
                                                 Irp,
                                                 GuidIndex,
                                                 0,
                                                 InstanceCount,
                                                 LengthArray,
                                                 BufferSize,
                                                 Data);

        case IRP_MN_QUERY_SINGLE_INSTANCE:
        case IRP_MN_CHANGE_SINGLE_INSTANCE:
            WnodeSi = (PWNODE_SINGLE_INSTANCE)Buffer;

            /* Instances are only known by the index WMI resolved from a static name */
            if (!(WnodeSi->WnodeHeader.Flags & WNODE_FLAG_STATIC_INSTANCE_NAMES) ||
                (WnodeSi->InstanceIndex >= InstanceCount))
            {
                return WmiLibpNotCompleted(Irp, STATUS_WMI_INSTANCE_NOT_FOUND, 0, IrpDisposition);
            }

            if (Stack->MinorFunction == IRP_MN_QUERY_SINGLE_INSTANCE)
            {
                if (!WmiLibInfo->QueryWmiDataBlock)
                    return WmiLibpNotCompleted(Irp, STATUS_INVALID_DEVICE_REQUEST, 0, IrpDisposition);

                if (WnodeSi->DataBlockOffset <= BufferSize)
                {
                    LengthArray = &WnodeSi->SizeDataBlock;
                    Data = Buffer + WnodeSi->DataBlockOffset;
                    BufferSize -= WnodeSi->DataBlockOffset;
                }
                else
                {
                    LengthArray = NULL;
                    Data = NULL;
                    BufferSize = 0;
                }

                *IrpDisposition = IrpProcessed;
                return WmiLibInfo->QueryWmiDataBlock(DeviceObject,
                                                     Irp,
                                                     GuidIndex,
                                                     WnodeSi->InstanceIndex,
                                                     1,
                                                     LengthArray,
                                                     BufferSize,
                                                     Data);
            }

            if (!WmiLibInfo->SetWmiDataBlock)
                return WmiLibpNotCompleted(Irp, STATUS_WMI_READ_ONLY, 0, IrpDisposition);

            if ((WnodeSi->DataBlockOffset > BufferSize) ||
                (WnodeSi->SizeDataBlock > BufferSize - WnodeSi->DataBlockOffset))
            {
                return WmiLibpNotCompleted(Irp, STATUS_INVALID_PARAMETER, 0, IrpDisposition);
            }

            *IrpDisposition = IrpProcessed;
            return WmiLibInfo->SetWmiDataBlock(DeviceObject,
                                               Irp,
                                               GuidIndex,
                                               WnodeSi->InstanceIndex,
                                               WnodeSi->SizeDataBlock,
                                               Buffer + WnodeSi->DataBlockOffset);

        case IRP_MN_CHANGE_SINGLE_ITEM:
            WnodeItem = (PWNODE_SINGLE_ITEM)Buffer;
            if (!(WnodeItem->WnodeHeader.Flags & WNODE_FLAG_STATIC_INSTANCE_NAMES) ||
                (WnodeItem->InstanceIndex >= InstanceCount))
            {
                return WmiLibpNotCompleted(Irp, STATUS_WMI_INSTANCE_NOT_FOUND, 0, IrpDisposition);
            }

            if (!WmiLibInfo->SetWmiDataItem)
                return WmiLibpNotCompleted(Irp, STATUS_WMI_READ_ONLY, 0, IrpDisposition);

            if ((WnodeItem->DataBlockOffset > BufferSize) ||
                (WnodeItem->SizeDataItem > BufferSize - WnodeItem->DataBlockOffset))
            {
                return WmiLibpNotCompleted(Irp, STATUS_INVALID_PARAMETER, 0, IrpDisposition);
            }

            *IrpDisposition = IrpProcessed;
            return WmiLibInfo->SetWmiDataItem(DeviceObject,
                                              Irp,
                                              GuidIndex,
                                              WnodeItem->InstanceIndex,
                                              WnodeItem->ItemId,
                                              WnodeItem->SizeDataItem,
                                              Buffer + WnodeItem->DataBlockOffset);

        case IRP_MN_EXECUTE_METHOD:
            WnodeMethod = (PWNODE_METHOD_ITEM)Buffer;
            if (!(WnodeMethod->WnodeHeader.Flags & WNODE_FLAG_STATIC_INSTANCE_NAMES) ||
                (WnodeMethod->InstanceIndex >= InstanceCount))
            {
                return WmiLibpNotCompleted(Irp, STATUS_WMI_INSTANCE_NOT_FOUND, 0, IrpDisposition);
            }

            if (!WmiLibInfo->ExecuteWmiMethod)
                return WmiLibpNotCompleted(Irp, STATUS_INVALID_DEVICE_REQUEST, 0, IrpDisposition);

            if ((WnodeMethod->DataBlockOffset > BufferSize) ||
                (WnodeMethod->SizeDataBlock > BufferSize - WnodeMethod->DataBlockOffset))
            {
                return WmiLibpNotCompleted(Irp, STATUS_INVALID_PARAMETER, 0, IrpDisposition);
            }

            *IrpDisposition = IrpProcessed;
            return WmiLibInfo->ExecuteWmiMethod(DeviceObject,
                                                Irp,
                                                GuidIndex,
                                                WnodeMethod->InstanceIndex,
                                                WnodeMethod->MethodId,
                                                WnodeMethod->SizeDataBlock,
                                                BufferSize - WnodeMethod->DataBlockOffset,
                                                Buffer + WnodeMethod->DataBlockOffset);

        case IRP_MN_ENABLE_EVENTS:
        case IRP_MN_DISABLE_EVENTS:
        case IRP_MN_ENABLE_COLLECTION:
        case IRP_MN_DISABLE_COLLECTION:
            /* Drivers without a control routine have nothing to switch */
            if (!WmiLibInfo->WmiFunctionControl)
                return WmiLibpNotCompleted(Irp, STATUS_SUCCESS, 0, IrpDisposition);

            *IrpDisposition = IrpProcessed;
            return WmiLibInfo->WmiFunctionControl(DeviceObject,
                                                  Irp,
                                                  GuidIndex,
                                                  ((Stack->MinorFunction == IRP_MN_ENABLE_EVENTS) ||
                                                   (Stack->MinorFunction == IRP_MN_DISABLE_EVENTS)) ?
                                                      WmiEventControl : WmiDataBlockControl,
                                                  (Stack->MinorFunction == IRP_MN_ENABLE_EVENTS) ||
                                                  (Stack->MinorFunction == IRP_MN_ENABLE_COLLECTION));

        default:
            *IrpDisposition = IrpNotWmi;
            return Irp->IoStatus.Status;
    }
}
