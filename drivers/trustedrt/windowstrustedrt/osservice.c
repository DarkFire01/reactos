/*
 * PROJECT:     ReactOS Trusted Runtime class extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     OS services the secure environment calls back into
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "wtrp.h"

#define NDEBUG
#include <debug.h>

typedef NTSTATUS NTAPI WTR_IO_RESOLVE_DEPENDENCY(_In_ PCUNICODE_STRING DependencyId, _In_ PDEVICE_OBJECT Device);

/* How long to back off when no request can be kept around */
#define WTR_REQUEST_RETRY_DELAY     (-50000000LL)

static EVT_WDF_OBJECT_CONTEXT_CLEANUP WtrEvtOsServicesCleanup;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP WtrEvtPluginRequestCleanup;
static EVT_WDF_IO_TARGET_QUERY_REMOVE WtrEvtPluginQueryRemove;
static EVT_WDF_IO_TARGET_REMOVE_CANCELED WtrEvtPluginRemoveCanceled;
static EVT_WDF_IO_TARGET_REMOVE_COMPLETE WtrEvtPluginRemoveComplete;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE WtrEvtPluginRequestDone;
static EVT_WDF_WORKITEM WtrEvtPluginWork;
static DRIVER_NOTIFICATION_CALLBACK_ROUTINE WtrPluginNotification;

NTSTATUS
NTAPI
WtrOsServicesInitialize(
    _In_ WDFDEVICE MasterDevice)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    PWTR_OS_SERVICES Services;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WTR_OS_SERVICES);
    Attributes.EvtCleanupCallback = WtrEvtOsServicesCleanup;
    Status = WdfObjectAllocateContext(MasterDevice, &Attributes, (PVOID *)&Services);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = MasterDevice;
    Status = WdfWaitLockCreate(&Attributes, &Services->Lock);
    if (!NT_SUCCESS(Status))
        return Status;

    return WdfCollectionCreate(&Attributes, &Services->Plugins);
}

static
BOOLEAN
NTAPI
WtrFindPluginLocked(
    _In_ PWTR_OS_SERVICES Services,
    _In_opt_ PCUNICODE_STRING SymbolicLink,
    _In_opt_ const GUID *Id,
    _Out_ WDFOBJECT *Plugin,
    _Out_opt_ PWTR_PLUGIN *PluginContext)
{
    UNICODE_STRING Link;
    PWTR_PLUGIN Context;
    WDFOBJECT Handle;
    ULONG Index;

    *Plugin = NULL;
    if (PluginContext)
        *PluginContext = NULL;

    for (Index = 0; Index < WdfCollectionGetCount(Services->Plugins); Index++)
    {
        Handle = WdfCollectionGetItem(Services->Plugins, Index);
        Context = WtrGetPlugin(Handle);

        if (Id && !IsEqualGUID(Id, &Context->Id))
            continue;

        /* A plugin that has not arrived yet matches any link */
        if (Context->SymbolicLink && SymbolicLink)
        {
            WdfStringGetUnicodeString(Context->SymbolicLink, &Link);
            if (!RtlEqualUnicodeString(SymbolicLink, &Link, TRUE))
                continue;
        }

        WdfObjectReference(Handle);
        *Plugin = Handle;
        if (PluginContext)
            *PluginContext = Context;
        return TRUE;
    }

    return FALSE;
}

BOOLEAN
NTAPI
WtrFindPlugin(
    _In_ PWTR_OS_SERVICES Services,
    _In_opt_ PCUNICODE_STRING SymbolicLink,
    _In_opt_ const GUID *Id,
    _Out_ WDFOBJECT *Plugin,
    _Out_opt_ PWTR_PLUGIN *PluginContext)
{
    BOOLEAN Found;

    WdfWaitLockAcquire(Services->Lock, NULL);
    Found = WtrFindPluginLocked(Services, SymbolicLink, Id, Plugin, PluginContext);
    WdfWaitLockRelease(Services->Lock);

    return Found;
}

static
NTSTATUS
NTAPI
WtrEvtPluginQueryRemove(
    _In_ WDFIOTARGET Target)
{
    WdfIoTargetCloseForQueryRemove(Target);
    return STATUS_UNSUCCESSFUL;
}

static
VOID
NTAPI
WtrEvtPluginRemoveCanceled(
    _In_ WDFIOTARGET Target)
{
    WDF_IO_TARGET_OPEN_PARAMS OpenParams;

    WDF_IO_TARGET_OPEN_PARAMS_INIT_REOPEN(&OpenParams);
    WdfIoTargetOpen(Target, &OpenParams);
}

static
VOID
NTAPI
WtrEvtPluginRemoveComplete(
    _In_ WDFIOTARGET Target)
{
    UNREFERENCED_PARAMETER(Target);
}

static
VOID
NTAPI
WtrEvtPluginRequestCleanup(
    _In_ WDFOBJECT Object)
{
    PWTR_PLUGIN_REQUEST_CONTEXT Context = WtrGetPluginRequest(Object);

    if (Context->Target)
    {
        WdfObjectDelete(Context->Target);
        Context->Target = NULL;
    }
}

/* Each request owns a handle to the plugin's device */
static
NTSTATUS
NTAPI
WtrCreatePluginRequest(
    _In_ WDFDEVICE Device,
    _In_ PWTR_PLUGIN Plugin,
    _Out_ WDFREQUEST *Request,
    _Out_opt_ PWTR_PLUGIN_REQUEST_CONTEXT *RequestContext)
{
    WDF_IO_TARGET_OPEN_PARAMS OpenParams;
    WDF_OBJECT_ATTRIBUTES Attributes;
    PWTR_PLUGIN_REQUEST_CONTEXT Context = NULL;
    UNICODE_STRING Link;
    WDFIOTARGET Target = NULL;
    WDFREQUEST NewRequest = NULL;
    NTSTATUS Status;

    *Request = NULL;
    if (RequestContext)
        *RequestContext = NULL;

    if (!Plugin->SymbolicLink)
        return STATUS_DEVICE_NOT_CONNECTED;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = WdfObjectContextGetObject(Plugin);
    Status = WdfIoTargetCreate(Device, &Attributes, &Target);
    if (!NT_SUCCESS(Status))
        return Status;

    WdfStringGetUnicodeString(Plugin->SymbolicLink, &Link);
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&OpenParams, &Link, STANDARD_RIGHTS_ALL);
    OpenParams.EvtIoTargetQueryRemove = WtrEvtPluginQueryRemove;
    OpenParams.EvtIoTargetRemoveCanceled = WtrEvtPluginRemoveCanceled;
    OpenParams.EvtIoTargetRemoveComplete = WtrEvtPluginRemoveComplete;
    Status = WdfIoTargetOpen(Target, &OpenParams);
    if (!NT_SUCCESS(Status))
        goto Fail;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WTR_PLUGIN_REQUEST_CONTEXT);
    Attributes.EvtCleanupCallback = WtrEvtPluginRequestCleanup;
    Attributes.ParentObject = WdfObjectContextGetObject(Plugin);
    Status = WdfRequestCreate(&Attributes, Target, &NewRequest);
    if (!NT_SUCCESS(Status))
        goto Fail;

    Context = WtrGetPluginRequest(NewRequest);
    Context->Plugin = Plugin;
    Context->Device = Device;
    Context->Generation = Plugin->Generation;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = NewRequest;
    Status = WdfMemoryCreate(&Attributes, NonPagedPoolNx, WTR_POOL_TAG, sizeof(WTR_SERVICE_REQUEST), &Context->Input, NULL);
    if (!NT_SUCCESS(Status))
        goto Fail;

    Status = WdfMemoryCreate(&Attributes, NonPagedPoolNx, WTR_POOL_TAG, sizeof(WTR_EXECUTE_RESULT), &Context->Output, NULL);
    if (!NT_SUCCESS(Status))
        goto Fail;

    Status = WdfIoTargetFormatRequestForIoctl(Target, NewRequest, IOCTL_WTR_EXECUTE_REQUEST, Context->Input, NULL, Context->Output, NULL);
    if (!NT_SUCCESS(Status))
        goto Fail;

    Context->Target = Target;
    *Request = NewRequest;
    if (RequestContext)
        *RequestContext = Context;
    return STATUS_SUCCESS;

Fail:
    if (NewRequest)
        WdfObjectDelete(NewRequest);
    if (Target)
        WdfObjectDelete(Target);
    return Status;
}

static
NTSTATUS
NTAPI
WtrRegisterPlugin(
    _In_ WDFDEVICE Device,
    _In_ PWTR_OS_SERVICES Services,
    _In_ const GUID *Id)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_WORKITEM_CONFIG WorkConfig;
    PWTR_PLUGIN Plugin;
    WDFOBJECT Handle = NULL;
    WDFOBJECT Existing;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WTR_PLUGIN);
    Attributes.ParentObject = WdfObjectContextGetObject(Services);
    Status = WdfObjectCreate(&Attributes, &Handle);
    if (!NT_SUCCESS(Status))
        return Status;

    Plugin = WtrGetPlugin(Handle);
    Plugin->Generation = 0;
    Plugin->Id = *Id;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Handle;
    Status = WdfSpinLockCreate(&Attributes, &Plugin->Lock);
    if (NT_SUCCESS(Status))
        Status = WdfCollectionCreate(&Attributes, &Plugin->FreeRequests);
    if (NT_SUCCESS(Status))
        Status = WdfCollectionCreate(&Attributes, &Plugin->ActiveRequests);
    if (NT_SUCCESS(Status))
        Status = WdfCollectionCreate(&Attributes, &Plugin->PendingCalls);
    if (!NT_SUCCESS(Status))
        goto Fail;

    /* RPMB is spoken directly to the storage stack */
    if (IsEqualGUID(Id, &GUID_DEVINTERFACE_EMMC_PARTITION_ACCESS_RPMB))
    {
        Plugin->Initialize = WtrRpmbInitialize;
        Plugin->Handler = WtrRpmbHandler;
    }

    WDF_WORKITEM_CONFIG_INIT(&WorkConfig, WtrEvtPluginWork);
    WorkConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WTR_PLUGIN_WORK);
    Attributes.ParentObject = Device;
    Status = WdfWorkItemCreate(&WorkConfig, &Attributes, &Plugin->WorkItem);
    if (!NT_SUCCESS(Status))
        goto Fail;

    WtrGetPluginWork(Plugin->WorkItem)->Plugin = Handle;

    DPRINT1("WTR: watching for OS service {%08lx-%04x-%04x}\n", Id->Data1, Id->Data2, Id->Data3);

    WdfWaitLockAcquire(Services->Lock, NULL);
    if (WtrFindPluginLocked(Services, NULL, Id, &Existing, NULL))
    {
        WdfWaitLockRelease(Services->Lock);
        WdfObjectDereference(Existing);
        Status = STATUS_ALREADY_REGISTERED;
        goto Fail;
    }

    Status = WdfCollectionAdd(Services->Plugins, Handle);
    WdfWaitLockRelease(Services->Lock);
    if (!NT_SUCCESS(Status))
        goto Fail;

    Status = IoRegisterPlugPlayNotification(EventCategoryDeviceInterfaceChange,
                                            PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
                                            &Plugin->Id,
                                            WdfDriverWdmGetDriverObject(WdfDeviceGetDriver(Device)),
                                            WtrPluginNotification,
                                            Device,
                                            &Plugin->Notification);
    if (NT_SUCCESS(Status))
        return Status;

Fail:
    if (Handle)
        WdfObjectDelete(Handle);
    return Status;
}

static
NTSTATUS
NTAPI
WtrPluginAvailable(
    _In_ WDFDEVICE Device,
    _In_ WDFOBJECT Handle,
    _In_ PCUNICODE_STRING SymbolicLink)
{
    static UNICODE_STRING RoutineName = RTL_CONSTANT_STRING(L"IoResolveDependency");
    PWTR_PLUGIN Plugin = WtrGetPlugin(Handle);
    WDF_MEMORY_DESCRIPTOR Output;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WTR_IO_RESOLVE_DEPENDENCY *ResolveDependency;
    PWTR_PLUGIN_REQUEST_CONTEXT Context;
    DECLARE_UNICODE_STRING_SIZE(IdString, 50);
    WDFREQUEST Request = NULL;
    ULONG_PTR Returned = 0;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Handle;
    DPRINT1("WTR: OS service {%08lx-%04x-%04x} arrived at %wZ\n", Plugin->Id.Data1, Plugin->Id.Data2, Plugin->Id.Data3, SymbolicLink);

    Status = WdfStringCreate(SymbolicLink, &Attributes, &Plugin->SymbolicLink);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WtrCreatePluginRequest(Device, Plugin, &Request, &Context);
    if (!NT_SUCCESS(Status))
        goto Done;

    RtlZeroMemory(&Plugin->Information, sizeof(Plugin->Information));
    Plugin->Information.Version = 1;

    if (Plugin->Initialize)
    {
        Plugin->Initialize(Handle, Context->Target, &Plugin->Information);
    }
    else
    {
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Output, &Plugin->Information, sizeof(Plugin->Information));
        Status = WdfIoTargetSendInternalIoctlSynchronously(Context->Target,
                                                           NULL,
                                                           IOCTL_WTR_QUERY_SERVICE,
                                                           NULL,
                                                           &Output,
                                                           NULL,
                                                           &Returned);
        if (!NT_SUCCESS(Status))
            goto Done;

        if (Returned < sizeof(Plugin->Information))
        {
            Status = STATUS_UNSUCCESSFUL;
            goto Done;
        }
    }

    WdfSpinLockAcquire(Plugin->Lock);
    Status = WdfCollectionAdd(Plugin->FreeRequests, Request);
    Plugin->Available = NT_SUCCESS(Status);
    Plugin->Generation++;
    WdfSpinLockRelease(Plugin->Lock);
    if (!NT_SUCCESS(Status))
        goto Done;

    Request = NULL;

    Status = RtlUnicodeStringPrintf(&IdString,
                                    L"{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                                    Plugin->Id.Data1, Plugin->Id.Data2, Plugin->Id.Data3,
                                    Plugin->Id.Data4[0], Plugin->Id.Data4[1], Plugin->Id.Data4[2],
                                    Plugin->Id.Data4[3], Plugin->Id.Data4[4], Plugin->Id.Data4[5],
                                    Plugin->Id.Data4[6], Plugin->Id.Data4[7]);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Lets PnP order the secure service after the device it depends on */
    ResolveDependency = (WTR_IO_RESOLVE_DEPENDENCY *)MmGetSystemRoutineAddress(&RoutineName);
    if (ResolveDependency)
        Status = ResolveDependency(&IdString, WdfIoTargetWdmGetTargetPhysicalDevice(Context->Target));

Done:
    DPRINT1("WTR: OS service {%08lx-%04x-%04x} available %u v%lu.%lu status 0x%lx\n", Plugin->Id.Data1, Plugin->Id.Data2, Plugin->Id.Data3, Plugin->Available, Plugin->Information.MajorVersion, Plugin->Information.MinorVersion, Status);
    if (Request)
        WdfObjectDelete(Request);
    return Status;
}

static
NTSTATUS
NTAPI
WtrPluginUnavailable(
    _In_ WDFDEVICE Device,
    _In_ WDFOBJECT Handle,
    _In_ PCUNICODE_STRING SymbolicLink)
{
    PWTR_PLUGIN Plugin = WtrGetPlugin(Handle);
    WDFCOLLECTION Orphans = NULL;
    WDFSTRING OldLink = NULL;
    UNICODE_STRING Link = { 0 };
    WDFOBJECT Call;
    NTSTATUS Status;

    if (Plugin->SymbolicLink)
        WdfStringGetUnicodeString(Plugin->SymbolicLink, &Link);
    if (!RtlEqualUnicodeString(SymbolicLink, &Link, TRUE))
        return STATUS_SUCCESS;

    DPRINT1("WTR: OS service {%08lx-%04x-%04x} removed\n", Plugin->Id.Data1, Plugin->Id.Data2, Plugin->Id.Data3);

    Status = WdfCollectionCreate(WDF_NO_OBJECT_ATTRIBUTES, &Orphans);
    if (!NT_SUCCESS(Status))
        Orphans = NULL;

    WdfSpinLockAcquire(Plugin->Lock);
    if (Plugin->Available)
    {
        OldLink = Plugin->SymbolicLink;
        Plugin->Available = FALSE;
        Plugin->SymbolicLink = NULL;

        while (Orphans && WdfCollectionGetCount(Plugin->PendingCalls))
        {
            Status = WdfCollectionAdd(Orphans, WdfCollectionGetFirstItem(Plugin->PendingCalls));
            if (!NT_SUCCESS(Status))
                break;
            WdfCollectionRemoveItem(Plugin->PendingCalls, 0);
        }
    }
    WdfSpinLockRelease(Plugin->Lock);

    if (Orphans)
    {
        while (WdfCollectionGetCount(Orphans))
        {
            Call = WdfCollectionGetFirstItem(Orphans);
            WtrFinishCall(Device, Call, STATUS_DEVICE_NOT_CONNECTED, 0);
            WdfCollectionRemoveItem(Orphans, 0);
        }
        WdfObjectDelete(Orphans);
    }

    if (OldLink)
        WdfObjectDelete(OldLink);

    return Status;
}

static
NTSTATUS
NTAPI
WtrPluginNotification(
    _In_ PVOID NotificationStructure,
    _Inout_opt_ PVOID Context)
{
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION Notification = NotificationStructure;
    WDFDEVICE Device = (WDFDEVICE)Context;
    WDFOBJECT Handle = NULL;

    if (WtrFindPlugin(WtrGetOsServices(Device), Notification->SymbolicLinkName, &Notification->InterfaceClassGuid, &Handle, NULL))
    {
        if (IsEqualGUID(&Notification->Event, &GUID_DEVICE_INTERFACE_ARRIVAL))
            WtrPluginAvailable(Device, Handle, Notification->SymbolicLinkName);
        else if (IsEqualGUID(&Notification->Event, &GUID_DEVICE_INTERFACE_REMOVAL))
            WtrPluginUnavailable(Device, Handle, Notification->SymbolicLinkName);
    }

    if (Handle)
        WdfObjectDereference(Handle);

    return STATUS_SUCCESS;
}

/* Puts a used request back in the pool, or lets it go if the plugin moved on */
static
VOID
NTAPI
WtrReturnPluginRequest(
    _Inout_ WDFREQUEST *Slot)
{
    WDFREQUEST Request = *Slot;
    PWTR_PLUGIN_REQUEST_CONTEXT Context = WtrGetPluginRequest(Request);
    PWTR_PLUGIN Plugin = Context->Plugin;
    WDF_REQUEST_REUSE_PARAMS Reuse;
    LARGE_INTEGER Delay;
    BOOLEAN Reused = FALSE;
    BOOLEAN Retry;
    ULONG FreeCount;
    ULONG ActiveCount;
    ULONG Index;
    NTSTATUS Status = STATUS_SUCCESS;

    WdfSpinLockAcquire(Plugin->Lock);
    ActiveCount = WdfCollectionGetCount(Plugin->ActiveRequests);
    for (Index = 0; Index < ActiveCount; Index++)
    {
        if (WdfCollectionGetItem(Plugin->ActiveRequests, Index) == Request)
        {
            WdfCollectionRemoveItem(Plugin->ActiveRequests, Index);
            break;
        }
    }
    WdfSpinLockRelease(Plugin->Lock);

    Context->Flags = 0;
    WDF_REQUEST_REUSE_PARAMS_INIT(&Reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);

    for (;;)
    {
        if (!Reused)
        {
            Status = WdfRequestReuse(Request, &Reuse);
            Reused = NT_SUCCESS(Status);
        }

        WdfSpinLockAcquire(Plugin->Lock);
        if (!Plugin->Available || Context->Generation != Plugin->Generation)
        {
            WdfSpinLockRelease(Plugin->Lock);
            return;
        }

        /* Keep retrying only while nothing else could serve the plugin */
        Retry = TRUE;
        FreeCount = WdfCollectionGetCount(Plugin->FreeRequests);
        for (Index = 0; Index < FreeCount && Retry; Index++)
        {
            if (WtrGetPluginRequest(WdfCollectionGetItem(Plugin->FreeRequests, Index))->Generation == Plugin->Generation)
                Retry = FALSE;
        }

        ActiveCount = WdfCollectionGetCount(Plugin->ActiveRequests);
        for (Index = 0; Index < ActiveCount && Retry; Index++)
        {
            if (WtrGetPluginRequest(WdfCollectionGetItem(Plugin->ActiveRequests, Index))->Generation == Plugin->Generation)
                Retry = FALSE;
        }

        if (Reused)
        {
            Status = WdfCollectionAdd(Plugin->FreeRequests, Request);
            if (NT_SUCCESS(Status))
            {
                *Slot = NULL;
                FreeCount++;
                Retry = FALSE;
            }
        }
        WdfSpinLockRelease(Plugin->Lock);

        if (!NT_SUCCESS(Status) && !FreeCount && !ActiveCount)
        {
            Delay.QuadPart = WTR_REQUEST_RETRY_DELAY;
            KeDelayExecutionThread(KernelMode, FALSE, &Delay);
            Retry = TRUE;
        }

        if (!Retry)
            return;
    }
}

static
NTSTATUS
NTAPI
WtrTakePluginRequest(
    _In_ WDFDEVICE Device,
    _In_ WDFOBJECT Handle,
    _Out_ WDFREQUEST *Slot)
{
    PWTR_PLUGIN Plugin = WtrGetPlugin(Handle);
    WDFREQUEST Request = NULL;
    NTSTATUS Status = STATUS_NOT_FOUND;

    WdfSpinLockAcquire(Plugin->Lock);
    while (Status == STATUS_NOT_FOUND && WdfCollectionGetCount(Plugin->FreeRequests))
    {
        Request = WdfCollectionGetFirstItem(Plugin->FreeRequests);
        WdfCollectionRemove(Plugin->FreeRequests, Request);
        WdfSpinLockRelease(Plugin->Lock);

        /* Requests from before the plugin came back point at a stale handle */
        if (WtrGetPluginRequest(Request)->Generation == Plugin->Generation)
            Status = STATUS_SUCCESS;
        else
            WdfObjectDelete(Request);

        WdfSpinLockAcquire(Plugin->Lock);
    }
    WdfSpinLockRelease(Plugin->Lock);

    if (Status == STATUS_NOT_FOUND)
        Status = WtrCreatePluginRequest(Device, Plugin, &Request, NULL);

    if (!NT_SUCCESS(Status))
        return Status;

    WdfSpinLockAcquire(Plugin->Lock);
    if (Plugin->Available)
    {
        WdfCollectionAdd(Plugin->ActiveRequests, Request);
        WdfSpinLockRelease(Plugin->Lock);
        *Slot = Request;
        return STATUS_SUCCESS;
    }
    WdfSpinLockRelease(Plugin->Lock);

    WdfObjectDelete(Request);
    return STATUS_DEVICE_NOT_CONNECTED;
}

static
VOID
NTAPI
WtrCompletePluginWork(
    _Inout_ WDFREQUEST *Slot,
    _In_ NTSTATUS Status,
    _In_ ULONG64 Information)
{
    WDFREQUEST Request = *Slot;
    PWTR_PLUGIN_REQUEST_CONTEXT Context = WtrGetPluginRequest(Request);
    WDFOBJECT CallData = Context->CallData;

    *Slot = NULL;
    Context->CallData = NULL;

    WdfSpinLockAcquire(Context->Plugin->Lock);
    Context->Flags |= WTR_PLUGIN_REQUEST_DONE;
    WdfSpinLockRelease(Context->Plugin->Lock);

    if (CallData)
    {
        WtrFinishCall(Context->Device, CallData, Status, Information);
        WdfObjectDereference(CallData);
    }

    WtrReturnPluginRequest(&Request);
    if (Request)
        WdfObjectDelete(Request);
}

static
VOID
NTAPI
WtrEvtPluginRequestDone(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context)
{
    PWTR_PLUGIN_REQUEST_CONTEXT RequestContext = WtrGetPluginRequest(Request);
    PWTR_EXECUTE_RESULT Result;
    ULONG64 Information = 0;

    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Params);
    UNREFERENCED_PARAMETER(Context);

    if (WdfRequestGetInformation(Request) >= sizeof(*Result))
    {
        Result = WdfMemoryGetBuffer(RequestContext->Output, NULL);
        Information = Result->Information;
    }

    WtrCompletePluginWork(&Request, WdfRequestGetStatus(Request), Information);
}

static
VOID
NTAPI
WtrSendPluginRequest(
    _In_ WDFREQUEST Request,
    _In_ WDFOBJECT CallData)
{
    PWTR_PLUGIN_REQUEST_CONTEXT Context = WtrGetPluginRequest(Request);
    PWTR_PLUGIN Plugin = Context->Plugin;
    WDFREQUEST Slot = Request;
    ULONG SendCount;
    NTSTATUS Status;

    WdfObjectReference(CallData);
    Context->CallData = CallData;
    RtlCopyMemory(WdfMemoryGetBuffer(Context->Input, NULL), &WtrGetCallData(CallData)->Request, sizeof(WTR_SERVICE_REQUEST));

    Status = WdfIoTargetFormatRequestForInternalIoctl(Context->Target,
                                                      Request,
                                                      IOCTL_WTR_EXECUTE_REQUEST,
                                                      Context->Input,
                                                      NULL,
                                                      Context->Output,
                                                      NULL);
    if (!NT_SUCCESS(Status))
        goto Fail;

    WdfRequestSetCompletionRoutine(Request, WtrEvtPluginRequestDone, NULL);

    WdfSpinLockAcquire(Plugin->Lock);
    SendCount = ++Context->SendCount;
    WdfSpinLockRelease(Plugin->Lock);

    if (!WdfRequestSend(Request, Context->Target, WDF_NO_SEND_OPTIONS))
    {
        Status = WdfRequestGetStatus(Request);
        goto Fail;
    }

    /* A cancel asked for before the send went out is honored now */
    WdfSpinLockAcquire(Plugin->Lock);
    if (SendCount == Context->SendCount)
    {
        Context->Flags |= WTR_PLUGIN_REQUEST_SENT;
        if (Context->Flags & WTR_PLUGIN_REQUEST_CANCEL)
            WdfRequestCancelSentRequest(Request);
    }
    WdfSpinLockRelease(Plugin->Lock);
    return;

Fail:
    WtrCompletePluginWork(&Slot, Status, 0);
}

static
VOID
NTAPI
WtrEvtPluginWork(
    _In_ WDFWORKITEM WorkItem)
{
    WDFDEVICE Device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
    WDFOBJECT Handle = WtrGetPluginWork(WorkItem)->Plugin;
    PWTR_PLUGIN Plugin = WtrGetPlugin(Handle);
    WDFREQUEST Request = NULL;
    WDFOBJECT CallData;
    ULONG Pending;
    NTSTATUS Status;

    do
    {
        WdfSpinLockAcquire(Plugin->Lock);
        Pending = WdfCollectionGetCount(Plugin->PendingCalls);
        WdfSpinLockRelease(Plugin->Lock);
        if (!Pending)
            break;

        Status = WtrTakePluginRequest(Device, Handle, &Request);
        if (!NT_SUCCESS(Status))
            break;

        WdfSpinLockAcquire(Plugin->Lock);
        if (!WdfCollectionGetCount(Plugin->PendingCalls))
        {
            WdfSpinLockRelease(Plugin->Lock);
            WtrReturnPluginRequest(&Request);
            if (Request)
                WdfObjectDelete(Request);
            return;
        }

        CallData = WdfCollectionGetFirstItem(Plugin->PendingCalls);
        WdfObjectReference(CallData);
        WdfCollectionRemove(Plugin->PendingCalls, CallData);
        WdfSpinLockRelease(Plugin->Lock);

        if (Plugin->Handler)
            Plugin->Handler(Handle, WtrGetPluginRequest(Request)->Target, Request, CallData);
        else
            WtrSendPluginRequest(Request, CallData);

        WdfObjectDereference(CallData);
        Request = NULL;
    } while (NT_SUCCESS(Status));
}

static
NTSTATUS
NTAPI
WtrOpenPlugin(
    _In_ WDFDEVICE MasterDevice,
    _In_ const GUID *OsServiceId,
    _Out_ WDFOBJECT *Handle,
    _Out_ PWTR_PLUGIN *Plugin)
{
    PWTR_OS_SERVICES Services = WtrGetOsServices(MasterDevice);
    NTSTATUS Status;

    if (!Services->Powered)
        return STATUS_DEVICE_POWERED_OFF;

    if (WtrFindPlugin(Services, NULL, OsServiceId, Handle, Plugin))
        return STATUS_SUCCESS;

    Status = WtrRegisterPlugin(MasterDevice, Services, OsServiceId);
    if (WtrFindPlugin(Services, NULL, OsServiceId, Handle, Plugin))
        return STATUS_SUCCESS;

    return NT_SUCCESS(Status) ? STATUS_NOT_FOUND : Status;
}

NTSTATUS
NTAPI
WtrOsServiceQuery(
    _In_ WDFDEVICE MasterDevice,
    _In_ const GUID *OsServiceId,
    _Out_ PWTR_OS_SERVICE_INFORMATION Information)
{
    WDFOBJECT Handle = NULL;
    PWTR_PLUGIN Plugin;
    NTSTATUS Status;

    Status = WtrOpenPlugin(MasterDevice, OsServiceId, &Handle, &Plugin);
    if (NT_SUCCESS(Status))
    {
        if (Plugin->Available)
            *Information = Plugin->Information;
        else
            Status = STATUS_DEVICE_NOT_CONNECTED;
    }

    if (Handle)
        WdfObjectDereference(Handle);

    return Status;
}

NTSTATUS
NTAPI
WtrOsServiceExecute(
    _In_ WDFDEVICE MasterDevice,
    _In_ const GUID *OsServiceId,
    _In_ WDFOBJECT CallData)
{
    PWTR_CALL_DATA Data = WtrGetCallData(CallData);
    WDFOBJECT Handle = NULL;
    WDFREQUEST Request = NULL;
    PWTR_PLUGIN Plugin;
    LONG Previous;
    NTSTATUS Status;

    Status = WtrOpenPlugin(MasterDevice, OsServiceId, &Handle, &Plugin);
    if (!NT_SUCCESS(Status))
        goto Done;

    Status = WtrTakePluginRequest(MasterDevice, Handle, &Request);

    Data->Status = STATUS_SUCCESS;
    Data->Information = 0;
    InterlockedExchange(&Data->Flags, 0);

    if (!NT_SUCCESS(Status))
    {
        /* No free request yet, the work item retries once one comes back */
        if (Status != STATUS_DEVICE_NOT_CONNECTED)
        {
            WdfSpinLockAcquire(Plugin->Lock);
            Status = WdfCollectionAdd(Plugin->PendingCalls, CallData);
            WdfSpinLockRelease(Plugin->Lock);
            if (NT_SUCCESS(Status))
            {
                WdfWorkItemEnqueue(Plugin->WorkItem);
                Status = STATUS_PENDING;
            }
        }
        goto Done;
    }

    if (Plugin->Handler)
    {
        Status = Plugin->Handler(Handle, WtrGetPluginRequest(Request)->Target, Request, CallData);
        if (Status != STATUS_PENDING)
        {
            WtrReturnPluginRequest(&Request);
            if (Request)
                WdfObjectDelete(Request);
            goto Done;
        }
    }
    else
    {
        WtrSendPluginRequest(Request, CallData);
    }

    Previous = InterlockedOr(&Data->Flags, WTR_CALL_WAITER);
    Status = (Previous & WTR_CALL_COMPLETED) ? Data->Status : STATUS_PENDING;

Done:
    if (Handle)
        WdfObjectDereference(Handle);
    return Status;
}

static
VOID
NTAPI
WtrEvtOsServicesCleanup(
    _In_ WDFOBJECT Object)
{
    PWTR_OS_SERVICES Services = WtrGetOsServices(Object);
    PWTR_PLUGIN Plugin;
    BOOLEAN Again;
    ULONG Index;

    do
    {
        Again = FALSE;
        WdfWaitLockAcquire(Services->Lock, NULL);
        for (Index = 0; Index < WdfCollectionGetCount(Services->Plugins); Index++)
        {
            Plugin = WtrGetPlugin(WdfCollectionGetItem(Services->Plugins, Index));
            if (!Plugin->Notification)
                continue;

            WdfWaitLockRelease(Services->Lock);
            if (KeGetCurrentIrql() == PASSIVE_LEVEL)
                IoUnregisterPlugPlayNotification(Plugin->Notification);
            Plugin->Notification = NULL;
            Again = TRUE;
            WdfWaitLockAcquire(Services->Lock, NULL);
            break;
        }
        WdfWaitLockRelease(Services->Lock);
    } while (Again);
}
