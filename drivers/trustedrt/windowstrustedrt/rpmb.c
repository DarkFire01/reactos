/*
 * PROJECT:     ReactOS Trusted Runtime class extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Replay protected memory block access for the secure environment
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "wtrp.h"
#include <ntddstor.h>

#define NDEBUG
#include <debug.h>

#ifndef IOCTL_SFFDISK_PARTITION_ACCESS
#define IOCTL_SFFDISK_PARTITION_ACCESS CTL_CODE(FILE_DEVICE_DISK, 0x7A3, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef IOCTL_STORAGE_RPMB_COMMAND
#define IOCTL_STORAGE_RPMB_COMMAND CTL_CODE(IOCTL_STORAGE_BASE, 0x0726, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#define WTR_RPMB_FRAME_SIZE         512

/* Commands the secure environment sends in WTR_SERVICE_REQUEST.FunctionCode */
#define WTR_RPMB_PASSTHROUGH        0x10001
#define WTR_RPMB_READ               0x10002
#define WTR_RPMB_READ_COUNTED       0x10003
#define WTR_RPMB_WRITE              0x10004

/* The eMMC partition access buffer: header, one reply frame, then request frames */
#define WTR_EMMC_ACCESS_SIZE        1056
#define WTR_EMMC_ACCESS_QUERY       0
#define WTR_EMMC_ACCESS_FRAMES      3
#define WTR_EMMC_REPLY_OFFSET       32
#define WTR_EMMC_FRAMES_OFFSET      544
#define WTR_EMMC_MAX_FRAMES         32

typedef struct _WTR_EMMC_ACCESS
{
    ULONG Size;
    ULONG Command;
    ULONG Reserved[2];
    ULONG Present;
    ULONG MaxReliableWrite;
    UCHAR Data[WTR_EMMC_ACCESS_SIZE - 24];
} WTR_EMMC_ACCESS, *PWTR_EMMC_ACCESS;

typedef struct _WTR_RPMB_DESCRIPTOR
{
    ULONG Version;
    ULONG Size;
    ULONG SizeInBytes;
    ULONG MaxReliableWriteSizeInBytes;
    ULONG FrameFormat;
} WTR_RPMB_DESCRIPTOR, *PWTR_RPMB_DESCRIPTOR;

/* Frame counts written back in front of the reply frames */
typedef struct _WTR_RPMB_COUNTS
{
    ULONG Output;
    ULONG Input;
} WTR_RPMB_COUNTS, *PWTR_RPMB_COUNTS;

NTSTATUS
NTAPI
WtrRpmbInitialize(
    _In_ WDFOBJECT Plugin,
    _In_ WDFIOTARGET Target,
    _Out_ PWTR_OS_SERVICE_INFORMATION Information)
{
    WDF_MEMORY_DESCRIPTOR Input;
    WDF_MEMORY_DESCRIPTOR Output;
    WDF_OBJECT_ATTRIBUTES Attributes;
    STORAGE_PROPERTY_QUERY Query;
    WTR_RPMB_DESCRIPTOR Descriptor;
    PWTR_EMMC_ACCESS Access = NULL;
    PWTR_RPMB_CONTEXT Rpmb;
    ULONG_PTR Returned = 0;
    size_t FrameBufferSize;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WTR_RPMB_CONTEXT);
    Status = WdfObjectAllocateContext(Plugin, &Attributes, (PVOID *)&Rpmb);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(&Query, sizeof(Query));
    Query.PropertyId = StorageAdapterRpmbProperty;
    Query.QueryType = PropertyStandardQuery;
    RtlZeroMemory(&Descriptor, sizeof(Descriptor));
    Descriptor.Size = sizeof(Descriptor);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input, &Query, sizeof(Query));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Output, &Descriptor, sizeof(Descriptor));
    Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_STORAGE_QUERY_PROPERTY, &Input, &Output, NULL, &Returned);
    if (NT_SUCCESS(Status))
    {
        if (Returned != sizeof(Descriptor))
            return STATUS_INVALID_BUFFER_SIZE;
        if (!Descriptor.SizeInBytes)
            return STATUS_NOT_SUPPORTED;

        Rpmb->MaxSectors = Descriptor.MaxReliableWriteSizeInBytes / WTR_RPMB_FRAME_SIZE;
        Rpmb->UseStorageProperty = TRUE;
        FrameBufferSize = Rpmb->MaxSectors * WTR_RPMB_FRAME_SIZE;
    }
    else
    {
        /* Not a storage stack RPMB, ask the SD/eMMC stack */
        Access = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*Access), WTR_POOL_TAG);
        if (!Access)
            return STATUS_INSUFFICIENT_RESOURCES;

        RtlZeroMemory(Access, sizeof(*Access));
        Access->Size = sizeof(*Access);
        Access->Command = WTR_EMMC_ACCESS_QUERY;

        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input, Access, sizeof(*Access));
        Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_SFFDISK_PARTITION_ACCESS, &Input, &Input, NULL, &Returned);
        if (NT_SUCCESS(Status))
        {
            if (Returned < sizeof(*Access))
                Status = STATUS_INVALID_PARAMETER;
            else if (!Access->Present)
                Status = STATUS_NOT_SUPPORTED;
            else
                Rpmb->MaxSectors = Access->MaxReliableWrite / WTR_RPMB_FRAME_SIZE;
        }

        ExFreePoolWithTag(Access, WTR_POOL_TAG);
        if (!NT_SUCCESS(Status))
            return Status;

        Rpmb->UseStorageProperty = FALSE;
        FrameBufferSize = WTR_EMMC_FRAMES_OFFSET + WTR_EMMC_MAX_FRAMES * WTR_RPMB_FRAME_SIZE;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Plugin;
    Status = WdfMemoryCreate(&Attributes, NonPagedPoolNx, WTR_POOL_TAG, FrameBufferSize, &Rpmb->FrameMemory, &Rpmb->FrameBuffer);
    if (!NT_SUCCESS(Status))
        return Status;

    DPRINT1("WTR: RPMB ready through %s, %lu sectors per write\n", Rpmb->UseStorageProperty ? "storage" : "SD/eMMC", Rpmb->MaxSectors);

    Information->Version = 1;
    Information->MajorVersion = 1;
    Information->MinorVersion = 1;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
WtrRpmbSendFrames(
    _In_ WDFOBJECT Plugin,
    _In_ WDFIOTARGET Target,
    _In_ BOOLEAN Write,
    _In_reads_bytes_(InputBytes) PUCHAR Input,
    _In_ ULONG InputBytes,
    _Out_writes_bytes_(OutputBytes) PUCHAR Output,
    _In_ ULONG OutputBytes,
    _Out_ PULONG_PTR Returned)
{
    PWTR_RPMB_CONTEXT Rpmb = WtrGetRpmbContext(Plugin);
    WDF_MEMORY_DESCRIPTOR InDescriptor;
    WDF_MEMORY_DESCRIPTOR OutDescriptor;
    PWTR_EMMC_ACCESS Access;
    NTSTATUS Status;

    *Returned = 0;

    if (Write)
    {
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&InDescriptor, Input, InputBytes);
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&OutDescriptor, Output, OutputBytes);
        return WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_STORAGE_RPMB_COMMAND, &InDescriptor, &OutDescriptor, NULL, Returned);
    }

    if (WTR_EMMC_FRAMES_OFFSET + max(InputBytes, OutputBytes) > WTR_EMMC_FRAMES_OFFSET + WTR_EMMC_MAX_FRAMES * WTR_RPMB_FRAME_SIZE)
        return STATUS_INVALID_PARAMETER;

    Access = Rpmb->FrameBuffer;
    RtlZeroMemory(Access, WTR_EMMC_FRAMES_OFFSET + OutputBytes);
    Access->Size = WTR_EMMC_ACCESS_SIZE;
    Access->Command = WTR_EMMC_ACCESS_FRAMES;
    Access->Present = OutputBytes / WTR_RPMB_FRAME_SIZE;
    RtlCopyMemory((PUCHAR)Access + WTR_EMMC_FRAMES_OFFSET, Input, InputBytes);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&InDescriptor, Access, WTR_EMMC_FRAMES_OFFSET + OutputBytes);
    Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_SFFDISK_PARTITION_ACCESS, &InDescriptor, &InDescriptor, NULL, Returned);

    RtlCopyMemory(Output, (PUCHAR)Access + WTR_EMMC_REPLY_OFFSET, WTR_RPMB_FRAME_SIZE);
    return Status;
}

/* Splits a run of RPMB frames into the request and reply pairs the device takes */
static
NTSTATUS
NTAPI
WtrRpmbProcessFrames(
    _In_ WDFOBJECT Plugin,
    _In_ WDFIOTARGET Target,
    _In_ ULONG Command,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ SIZE_T InputLength,
    _Out_writes_bytes_(OutputLength) PUCHAR Output,
    _In_ SIZE_T OutputLength,
    _Out_ PULONG64 Information)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    PWTR_RPMB_COUNTS Counts = (PWTR_RPMB_COUNTS)Output;
    WDFMEMORY Scratch = NULL;
    PUCHAR Source;
    PUCHAR Replies = Output + sizeof(WTR_RPMB_COUNTS);
    ULONG_PTR Returned;
    ULONG InputFrames;
    ULONG OutputFrames;
    ULONG PerRequest;
    ULONG InputStep;
    ULONG OutputStep;
    ULONG Requests;
    ULONG Index;
    BOOLEAN Write;
    NTSTATUS Status = STATUS_SUCCESS;

    if (InputLength < sizeof(WTR_RPMB_COUNTS) || OutputLength < sizeof(WTR_RPMB_COUNTS))
        return STATUS_INVALID_PARAMETER;

    InputFrames = (ULONG)((InputLength - sizeof(WTR_RPMB_COUNTS)) / WTR_RPMB_FRAME_SIZE);
    OutputFrames = (ULONG)((OutputLength - sizeof(WTR_RPMB_COUNTS)) / WTR_RPMB_FRAME_SIZE);

    switch (Command)
    {
        case WTR_RPMB_READ:
        case WTR_RPMB_READ_COUNTED:
            PerRequest = (Command == WTR_RPMB_READ_COUNTED) ? ((PWTR_RPMB_COUNTS)Input)->Input : 2;
            if (!PerRequest)
                return STATUS_INVALID_PARAMETER;
            OutputFrames /= PerRequest;
            Write = FALSE;
            break;

        case WTR_RPMB_WRITE:
            PerRequest = ((PWTR_RPMB_COUNTS)Input)->Input;
            Write = TRUE;
            break;

        default:
            return STATUS_INVALID_PARAMETER;
    }

    if (!PerRequest)
        PerRequest = 1;

    Source = Input + sizeof(WTR_RPMB_COUNTS);
    if (InputFrames >= OutputFrames)
    {
        Requests = InputFrames / PerRequest;
        InputStep = PerRequest;
        OutputStep = (InputFrames > OutputFrames) ? 1 : PerRequest;
    }
    else
    {
        /* Fewer request frames than replies: the request frames are reused per reply */
        InputStep = 1;
        OutputStep = PerRequest;
        Requests = OutputFrames / PerRequest;

        WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
        Attributes.ParentObject = Plugin;
        Status = WdfMemoryCreate(&Attributes, PagedPool, WTR_POOL_TAG, (size_t)Requests * WTR_RPMB_FRAME_SIZE, &Scratch, (PVOID *)&Source);
        if (!NT_SUCCESS(Status))
            return Status;

        RtlZeroMemory(Source, (size_t)Requests * WTR_RPMB_FRAME_SIZE);
        RtlCopyMemory(Source,
                      Input + sizeof(WTR_RPMB_COUNTS),
                      min((size_t)Requests * WTR_RPMB_FRAME_SIZE, InputLength - sizeof(WTR_RPMB_COUNTS)));
    }

    *Information = 0;
    for (Index = 0; Index < Requests; Index++)
    {
        Status = WtrRpmbSendFrames(Plugin,
                                   Target,
                                   Write,
                                   Source + (SIZE_T)Index * InputStep * WTR_RPMB_FRAME_SIZE,
                                   InputStep * WTR_RPMB_FRAME_SIZE,
                                   Replies + (SIZE_T)Index * OutputStep * WTR_RPMB_FRAME_SIZE,
                                   OutputStep * WTR_RPMB_FRAME_SIZE,
                                   &Returned);
        if (!NT_SUCCESS(Status))
            break;

        *Information += Returned;
    }

    Counts->Output = Requests * OutputStep;
    Counts->Input = Requests * InputStep;

    if (Scratch)
        WdfObjectDelete(Scratch);

    return Status;
}

NTSTATUS
NTAPI
WtrRpmbHandler(
    _In_ WDFOBJECT Plugin,
    _In_ WDFIOTARGET Target,
    _In_ WDFREQUEST Request,
    _In_ WDFOBJECT CallData)
{
    PWTR_CALL_DATA Data = WtrGetCallData(CallData);
    PWTR_SERVICE_REQUEST ServiceRequest = &Data->Request;
    WDF_MEMORY_DESCRIPTOR Input;
    WDF_MEMORY_DESCRIPTOR Output;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Request);

    switch (ServiceRequest->FunctionCode)
    {
        case WTR_RPMB_PASSTHROUGH:
            Data->Information = 0;
            WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input, ServiceRequest->InputBuffer, (ULONG)ServiceRequest->InputBufferLength);
            WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Output, ServiceRequest->OutputBuffer, (ULONG)ServiceRequest->OutputBufferLength);
            Status = WdfIoTargetSendIoctlSynchronously(Target,
                                                       NULL,
                                                       IOCTL_SFFDISK_PARTITION_ACCESS,
                                                       &Input,
                                                       &Output,
                                                       NULL,
                                                       (PULONG_PTR)&Data->Information);
            break;

        case WTR_RPMB_READ:
        case WTR_RPMB_READ_COUNTED:
        case WTR_RPMB_WRITE:
            Status = WtrRpmbProcessFrames(Plugin,
                                          Target,
                                          ServiceRequest->FunctionCode,
                                          ServiceRequest->InputBuffer,
                                          ServiceRequest->InputBufferLength,
                                          ServiceRequest->OutputBuffer,
                                          ServiceRequest->OutputBufferLength,
                                          &Data->Information);
            break;

        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
    }

    DPRINT1("WTR: RPMB command 0x%lx -> 0x%lx\n", ServiceRequest->FunctionCode, Status);

    /* Done synchronously, report it the way an async completion would */
    Data->Status = Status;
    Data->Completion(CallData);
    return Status;
}

/* Publishes the legacy RPMB service interface once real RPMB storage shows up */
NTSTATUS
NTAPI
WtrRpmbNotification(
    _In_ PVOID NotificationStructure,
    _Inout_opt_ PVOID Context)
{
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION Notification = NotificationStructure;
    WDFDEVICE Device = (WDFDEVICE)Context;

    if (IsEqualGUID(&Notification->Event, &GUID_DEVICE_INTERFACE_ARRIVAL) &&
        NT_SUCCESS(WdfDeviceCreateDeviceInterface(Device, &GUID_WTR_RPMB_SERVICE, NULL)))
    {
        WdfDeviceSetDeviceInterfaceState(Device, &GUID_WTR_RPMB_SERVICE, NULL, TRUE);
    }

    return STATUS_SUCCESS;
}
