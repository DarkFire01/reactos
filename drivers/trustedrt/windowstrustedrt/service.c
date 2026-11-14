/*
 * PROJECT:     ReactOS Trusted Runtime class extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Secure service devices and their sessions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "wtrp.h"

#define NDEBUG
#include <debug.h>

#define WTR_DEFAULT_SERVICE_SDDL    L"D:P(A;;FA;;;SY)(A;;FRFW;;;BA)(A;;FRFW;;;WD)"

typedef NTSTATUS NTAPI WTR_SDDL_TO_SD(_In_ PCWSTR Sddl, _In_ ULONG Revision, _Out_ PSECURITY_DESCRIPTOR *Sd, _Out_opt_ PULONG Size);

typedef struct _WTR_SERVICE_MAP
{
    GUID ServiceId;
    PCWSTR Value;
} WTR_SERVICE_MAP;

/* Services that get an extra child the OS binds a stock driver to */
static const WTR_SERVICE_MAP WtrServiceChildren[] =
{
    { { 0x36DEAA79, 0xC5DD, 0x447C, { 0x95, 0xE6, 0xB3, 0x85, 0x95, 0x89, 0x29, 0x1A } },
      L"{36DEAA79-C5DD-447C-95E6-B3859589291A}\\TrEETPM" },
};

/* Access to service devices that do not describe their own */
static const WTR_SERVICE_MAP WtrServiceSecurity[] =
{
    { { 0x699AA2F1, 0xA42E, 0x40DF, { 0xBA, 0xBE, 0x3A, 0xAA, 0xD2, 0xBB, 0x6A, 0x47 } }, L"D:P" },
    { { 0x36DEAA79, 0xC5DD, 0x447C, { 0x95, 0xE6, 0xB3, 0x85, 0x95, 0x89, 0x29, 0x1A } }, L"D:P" },
    { { 0xF4637010, 0x03C3, 0x42CD, { 0xB9, 0x32, 0xB4, 0x8A, 0xDF, 0x3A, 0x6A, 0x54 } },
      L"D:P(A;;FRFW;;;WD)(A;;FRFW;;;RC)(A;;FRFW;;;AC)"
      L"(A;;FRFW;;;S-1-15-3-1024-1692970155-4054893335-185714091-3362601943-3526593181-1159816984-2199008581-497492991)" },
};

static EVT_WDF_DEVICE_D0_ENTRY WtrEvtServiceD0Entry;
static EVT_WDF_DEVICE_D0_EXIT WtrEvtServiceD0Exit;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP WtrEvtServiceCleanup;
static EVT_WDF_OBJECT_CONTEXT_DESTROY WtrEvtSessionDestroy;
static EVT_WDF_IO_IN_CALLER_CONTEXT WtrEvtServiceInCallerContext;
static EVT_WDF_IO_QUEUE_IO_DEFAULT WtrEvtServiceDefault;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL WtrEvtServiceIoControl;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL WtrEvtSessionIoControl;
static EVT_WDF_IO_ALLOCATE_RESOURCES_FOR_RESERVED_REQUEST WtrEvtAllocateReserved;
static EVT_WDF_IO_ALLOCATE_REQUEST_RESOURCES WtrEvtAllocateResources;
static EVT_WDF_REQUEST_CANCEL WtrEvtSessionCancel;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE WtrEvtSerializedDone;
static EVT_WDF_WORKITEM WtrEvtFinishCallWork;

typedef struct _WTR_PROCESS_CALL
{
    WTR_EVT_SERVICE_PROCESS_REQUEST *Callback;
    WDFDEVICE Device;
    PVOID Session;
    WDFREQUEST Request;
    ULONG Priority;
    PWTR_SERVICE_REQUEST ServiceRequest;
    BOOLEAN UserMode;
    PULONG64 Information;
    PVOID *CancelContext;
    NTSTATUS Status;
} WTR_PROCESS_CALL, *PWTR_PROCESS_CALL;

typedef struct _WTR_SESSION_IO_CALL
{
    WTR_EVT_SERVICE_OTHER_IO *Callback;
    WDFDEVICE Device;
    PVOID Session;
    WDFREQUEST Request;
} WTR_SESSION_IO_CALL, *PWTR_SESSION_IO_CALL;

static
NTSTATUS
NTAPI
WtrGetSession(
    _In_ WDFFILEOBJECT FileObject,
    _Out_ PWTR_SESSION_CONTEXT *Session)
{
    PWTR_SESSION_CONTEXT Context;

    *Session = NULL;
    if (!FileObject)
        return STATUS_INVALID_PARAMETER;

    Context = WtrGetSessionContext(FileObject);
    if (!Context || !Context->Service || !Context->Service->Master)
        return STATUS_INVALID_PARAMETER;

    *Session = Context;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
WtrEvtServiceD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PWTR_SERVICE_CONTEXT Service = WtrGetServiceContext(Device);
    NTSTATUS Status = STATUS_SUCCESS;

    if (!InterlockedOr(&Service->IdleHeld, 1))
    {
        Status = WdfDeviceStopIdle(Device, FALSE);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    if (PreviousState < WdfPowerDeviceD3)
        return Status;

    if (!Service->Callbacks.EvtServiceConnect)
        return STATUS_SUCCESS;

    Status = Service->Callbacks.EvtServiceConnect(Device);
    DPRINT1("WTR: service {%08lx-%04x-%04x} connect -> 0x%lx\n", Service->ServiceId.Data1, Service->ServiceId.Data2, Service->ServiceId.Data3, Status);
    return Status;
}

static
NTSTATUS
NTAPI
WtrEvtServiceD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PWTR_SERVICE_CONTEXT Service = WtrGetServiceContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    if (!Service->Callbacks.EvtServiceDisconnect)
        return STATUS_SUCCESS;

    return Service->Callbacks.EvtServiceDisconnect(Device);
}

static
VOID
NTAPI
WtrEvtServiceCleanup(
    _In_ WDFOBJECT Object)
{
    WDFDEVICE Device = (WDFDEVICE)Object;
    PWTR_SERVICE_CONTEXT Service = WtrGetServiceContext(Device);

    if (Service->SecurityDescriptor)
    {
        ExFreePool(Service->SecurityDescriptor);
        Service->SecurityDescriptor = NULL;
    }

    if (Service->Descriptor)
        WdfDeviceSetDeviceInterfaceState(Device, &Service->Descriptor->ServiceId, NULL, FALSE);

    if (Service->Master && Service->Callbacks.EvtServiceCleanup)
        Service->Callbacks.EvtServiceCleanup(Device);
}

static
NTSTATUS
NTAPI
WtrEvtAllocateReserved(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request)
{
    PWTR_REQUEST_CONTEXT Context = WtrGetRequestContext(Request);
    WDF_OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Queue);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WTR_CALL_DATA);
    Attributes.ParentObject = Request;
    Status = WdfObjectCreate(&Attributes, &Context->CallData);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Reserved requests cannot allocate later, the buffers are pointed in place */
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Request;
    Status = WdfMemoryCreatePreallocated(&Attributes, &Context->InputMemory, sizeof(PVOID), &Context->InputMemory);
    if (!NT_SUCCESS(Status))
        return Status;

    return WdfMemoryCreatePreallocated(&Attributes, &Context->OutputMemory, sizeof(PVOID), &Context->OutputMemory);
}

static
NTSTATUS
NTAPI
WtrEvtAllocateResources(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request)
{
    PWTR_REQUEST_CONTEXT Context = WtrGetRequestContext(Request);
    WDF_OBJECT_ATTRIBUTES Attributes;

    UNREFERENCED_PARAMETER(Queue);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WTR_CALL_DATA);
    Attributes.ParentObject = Request;
    return WdfObjectCreate(&Attributes, &Context->CallData);
}

/* Captures the caller's buffers while still in its context */
static
NTSTATUS
NTAPI
WtrCaptureIoControl(
    _In_ WDFREQUEST Request,
    _In_ ULONG IoControlCode,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength)
{
    PWTR_REQUEST_CONTEXT Context = WtrGetRequestContext(Request);
    KPROCESSOR_MODE Mode = WdfRequestGetRequestorMode(Request);
    WDF_OBJECT_ATTRIBUTES Attributes;
    PWTR_SERVICE_REQUEST Input = NULL;
    size_t InputNeeded;
    size_t OutputNeeded;
    NTSTATUS Status = STATUS_SUCCESS;

    if (IoControlCode == IOCTL_WTR_QUERY_SERVICE)
    {
        InputNeeded = 0;
        OutputNeeded = sizeof(WTR_SERVICE_VERSION);
    }
    else if (IoControlCode == IOCTL_WTR_EXECUTE_REQUEST)
    {
        InputNeeded = sizeof(WTR_SERVICE_REQUEST);
        OutputNeeded = sizeof(WTR_EXECUTE_RESULT);
    }
    else
    {
        return STATUS_SUCCESS;
    }

    if (InputNeeded > InputBufferLength)
        return STATUS_INVALID_PARAMETER;

    if (InputNeeded)
    {
        Status = WdfRequestRetrieveInputBuffer(Request, InputNeeded, (PVOID *)&Input, NULL);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    if (OutputNeeded > OutputBufferLength)
        return STATUS_INVALID_PARAMETER;

    if (IoControlCode == IOCTL_WTR_QUERY_SERVICE)
        return Status;

    Context->Request = *Input;

    if (Mode == UserMode)
    {
        if (Input->InputBufferLength)
        {
            Status = WdfRequestProbeAndLockUserBufferForRead(Request,
                                                             Input->InputBuffer,
                                                             Input->InputBufferLength,
                                                             &Context->InputMemory);
        }
        if (NT_SUCCESS(Status) && Input->OutputBufferLength)
        {
            Status = WdfRequestProbeAndLockUserBufferForWrite(Request,
                                                              Input->OutputBuffer,
                                                              Input->OutputBufferLength,
                                                              &Context->OutputMemory);
        }
    }
    else if (WdfRequestIsReserved(Request))
    {
        if (Input->InputBufferLength)
            Status = WdfMemoryAssignBuffer(Context->InputMemory, Input->InputBuffer, Input->InputBufferLength);
        if (NT_SUCCESS(Status) && Input->OutputBufferLength)
            Status = WdfMemoryAssignBuffer(Context->OutputMemory, Input->OutputBuffer, Input->OutputBufferLength);
    }
    else
    {
        WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
        Attributes.ParentObject = Request;
        if (Input->InputBufferLength)
        {
            Status = WdfMemoryCreatePreallocated(&Attributes,
                                                 Input->InputBuffer,
                                                 Input->InputBufferLength,
                                                 &Context->InputMemory);
        }
        if (NT_SUCCESS(Status) && Input->OutputBufferLength)
        {
            Status = WdfMemoryCreatePreallocated(&Attributes,
                                                 Input->OutputBuffer,
                                                 Input->OutputBufferLength,
                                                 &Context->OutputMemory);
        }
    }

    return Status;
}

static
VOID
NTAPI
WtrEvtServiceInCallerContext(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request)
{
    PWTR_REQUEST_CONTEXT Context = WtrGetRequestContext(Request);
    WDF_REQUEST_PARAMETERS Parameters;
    NTSTATUS Status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        Context->Priority = 16;
    else
        Context->Priority = KeQueryPriorityThread(KeGetCurrentThread());

    WDF_REQUEST_PARAMETERS_INIT(&Parameters);
    WdfRequestGetParameters(Request, &Parameters);

    if (Parameters.Type == WdfRequestTypeDeviceControl)
    {
        Status = WtrCaptureIoControl(Request,
                                     Parameters.Parameters.DeviceIoControl.IoControlCode,
                                     Parameters.Parameters.DeviceIoControl.InputBufferLength,
                                     Parameters.Parameters.DeviceIoControl.OutputBufferLength);
        if (!NT_SUCCESS(Status))
        {
            WdfRequestComplete(Request, Status);
            return;
        }
    }

    Status = WdfDeviceEnqueueRequest(Device, Request);
    if (!NT_SUCCESS(Status))
        WdfRequestComplete(Request, Status);
}

static
NTSTATUS
NTAPI
WtrCreateSession(
    _In_ WDFDEVICE Device,
    _In_ WDFFILEOBJECT FileObject)
{
    PWTR_SERVICE_CONTEXT Service = WtrGetServiceContext(Device);
    PWTR_SESSION_CONTEXT Session = WtrGetSessionContext(FileObject);
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG Config;
    NTSTATUS Status;

    Session->Service = Service;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = FileObject;
    Status = WdfSpinLockCreate(&Attributes, &Session->Lock);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Session queue clients get one request at a time per open handle */
    if (Service->Master->Mode == WTR_MODE_SESSION_QUEUE)
    {
        WDF_IO_QUEUE_CONFIG_INIT(&Config, WdfIoQueueDispatchSequential);
        Config.EvtIoDeviceControl = WtrEvtSessionIoControl;

        WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
        Attributes.ExecutionLevel = WdfExecutionLevelPassive;
        Attributes.SynchronizationScope = WdfSynchronizationScopeQueue;
        Attributes.ParentObject = FileObject;

        Status = WdfIoQueueCreate(Device, &Config, &Attributes, &Session->Queue);
        if (!NT_SUCCESS(Status))
            return Status;

        WdfSpinLockAcquire(Service->Lock);
        Status = WdfCollectionAdd(Service->SessionQueues, Session->Queue);
        WdfSpinLockRelease(Service->Lock);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    else
    {
        Session->Queue = NULL;
    }

    if (!Service->Callbacks.EvtServiceCreateSession)
        return STATUS_SUCCESS;

    Status = Service->Callbacks.EvtServiceCreateSession(Device, &Session->ClientContext);
    DPRINT1("WTR: session open on {%08lx-%04x-%04x} -> 0x%lx\n", Service->ServiceId.Data1, Service->ServiceId.Data2, Service->ServiceId.Data3, Status);
    return Status;
}

static
VOID
NTAPI
WtrEvtSessionDestroy(
    _In_ WDFOBJECT Object)
{
    WDFFILEOBJECT FileObject = (WDFFILEOBJECT)Object;
    WDFDEVICE Device = WdfFileObjectGetDevice(FileObject);
    PWTR_SERVICE_CONTEXT Service = WtrGetServiceContext(Device);
    PWTR_SESSION_CONTEXT Session = WtrGetSessionContext(FileObject);

    if (Session->Queue)
    {
        WdfSpinLockAcquire(Service->Lock);
        WdfCollectionRemove(Service->SessionQueues, Session->Queue);
        WdfSpinLockRelease(Service->Lock);
        WdfObjectDelete(Session->Queue);
        Session->Queue = NULL;
    }

    if (Session->Service && Session->Service->Master && Service->Callbacks.EvtServiceDestroySession)
        Service->Callbacks.EvtServiceDestroySession(Device, &Session->ClientContext);
}

/* Only opens reach the default queue */
static
VOID
NTAPI
WtrEvtServiceDefault(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request)
{
    WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
    WDFFILEOBJECT FileObject = WdfRequestGetFileObject(Request);
    PWTR_SESSION_CONTEXT Session = WtrGetSessionContext(FileObject);
    WDF_REQUEST_PARAMETERS Parameters;
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;

    WDF_REQUEST_PARAMETERS_INIT(&Parameters);
    WdfRequestGetParameters(Request, &Parameters);

    if (Parameters.Type == WdfRequestTypeCreate)
    {
        if (Session->Queue)
            Status = STATUS_INVALID_DEVICE_STATE;
        else
            Status = WtrCreateSession(Device, FileObject);
    }

    WdfRequestComplete(Request, Status);
}

NTSTATUS
NTAPI
WtrCompleteSessionRequest(
    _In_ WDFREQUEST Request,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information)
{
    PWTR_REQUEST_CONTEXT Context = WtrGetRequestContext(Request);
    PWTR_SESSION_CONTEXT Session = WtrGetSessionContext(WdfRequestGetFileObject(Request));
    WTR_REQUEST_STATE Previous;
    NTSTATUS UnmarkStatus = STATUS_SUCCESS;

    /* A serialized request releases the serializing queue first */
    if (Context->Forwarded)
        WdfRequestComplete(Context->Forwarded, STATUS_SUCCESS);

    InterlockedExchange(&Session->Busy, 0);

    WdfSpinLockAcquire(Session->Lock);
    Previous = Context->State;
    Context->State = WtrRequestCompleted;
    Context->Status = Status;
    Context->Information = Information;
    if (Previous == WtrRequestQueryCancelable || Previous == WtrRequestCancelable)
        UnmarkStatus = WdfRequestUnmarkCancelable(Request);
    else if (Previous == WtrRequestCancelPending || Previous == WtrRequestCanceled)
        UnmarkStatus = STATUS_CANCELLED;
    WdfSpinLockRelease(Session->Lock);

    /* Whoever owns the request at this point completes it */
    if (UnmarkStatus == STATUS_CANCELLED)
    {
        if (Previous != WtrRequestCanceled)
            return STATUS_SUCCESS;
    }
    else if (Previous == WtrRequestExecuting)
    {
        return STATUS_SUCCESS;
    }

    WdfRequestCompleteWithInformation(Request, Status, Information);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
WtrEvtSessionCancel(
    _In_ WDFREQUEST Request)
{
    WDFFILEOBJECT FileObject = WdfRequestGetFileObject(Request);
    WDFDEVICE Device = WdfFileObjectGetDevice(FileObject);
    PWTR_SESSION_CONTEXT Session = WtrGetSessionContext(FileObject);
    PWTR_REQUEST_CONTEXT Context = WtrGetRequestContext(Request);
    PWTR_SERVICE_CONTEXT Service = Session->Service;
    WTR_REQUEST_STATE Previous;

    WdfSpinLockAcquire(Session->Lock);
    Previous = Context->State;
    Context->State = WtrRequestCanceled;
    WdfSpinLockRelease(Session->Lock);

    switch (Previous)
    {
        case WtrRequestCancelable:
        case WtrRequestCancelPending:
            if (Service && Service->Master && Service->Callbacks.EvtServiceCancelRequest)
                Service->Callbacks.EvtServiceCancelRequest(Device, Session->ClientContext, Request, &Context->CancelContext);
            break;

        case WtrRequestCompleted:
            WdfRequestCompleteWithInformation(Request, Context->Status, Context->Information);
            break;

        default:
            break;
    }
}

static
VOID
NTAPI
WtrQueryServiceVersion(
    _In_ WDFREQUEST Request)
{
    WDFDEVICE Device = WdfIoQueueGetDevice(WdfRequestGetIoQueue(Request));
    PWTR_SERVICE_CONTEXT Service = WtrGetServiceContext(Device);
    PWTR_SERVICE_VERSION Version = NULL;
    ULONG_PTR Information = 0;
    NTSTATUS Status;

    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Version), (PVOID *)&Version, NULL);
    if (NT_SUCCESS(Status))
    {
        Version->Version = 1;
        Version->MajorVersion = Service->Descriptor->MajorVersion;
        Version->MinorVersion = Service->Descriptor->MinorVersion;
        Information = sizeof(*Version);
    }

    WtrCompleteSessionRequest(Request, Status, Information);
}

static
VOID
NTAPI
WtrProcessWorker(
    _In_ PVOID Parameter)
{
    PWTR_PROCESS_CALL Call = Parameter;

    Call->Status = Call->Callback(Call->Device,
                                  Call->Session,
                                  Call->Request,
                                  Call->Priority,
                                  Call->ServiceRequest,
                                  Call->UserMode,
                                  Call->Information,
                                  Call->CancelContext);
}

/* Hands an execute request to the client and settles who completes it */
NTSTATUS
NTAPI
WtrExecuteSessionRequest(
    _In_ WDFREQUEST Request)
{
    PWTR_REQUEST_CONTEXT Context = WtrGetRequestContext(Request);
    WDFFILEOBJECT FileObject = WdfRequestGetFileObject(Request);
    WDFDEVICE Device = WdfFileObjectGetDevice(FileObject);
    PWTR_SESSION_CONTEXT Session = WtrGetSessionContext(FileObject);
    PWTR_SERVICE_CONTEXT Service;
    PWTR_EXECUTE_RESULT Result = NULL;
    WTR_SERVICE_REQUEST ServiceRequest = Context->Request;
    WTR_PROCESS_CALL Call;
    ULONG64 Information = 0;
    BOOLEAN FromUser;
    NTSTATUS Status;

    if (ServiceRequest.InputBufferLength)
        ServiceRequest.InputBuffer = WdfMemoryGetBuffer(Context->InputMemory, NULL);
    if (ServiceRequest.OutputBufferLength)
        ServiceRequest.OutputBuffer = WdfMemoryGetBuffer(Context->OutputMemory, NULL);

    FromUser = (WdfRequestGetRequestorMode(Request) == UserMode);

    Status = WtrGetSession(FileObject, &Session);
    if (NT_SUCCESS(Status))
    {
        Service = Session->Service;
        if (!Service->Callbacks.EvtServiceProcessRequest)
        {
            Status = STATUS_NOT_SUPPORTED;
        }
        else if (!Service->Master->StackSize)
        {
            Status = Service->Callbacks.EvtServiceProcessRequest(Device,
                                                                 Session->ClientContext,
                                                                 Request,
                                                                 Context->Priority,
                                                                 &ServiceRequest,
                                                                 FromUser,
                                                                 &Information,
                                                                 &Context->CancelContext);
        }
        else
        {
            Call.Callback = Service->Callbacks.EvtServiceProcessRequest;
            Call.Device = Device;
            Call.Session = Session->ClientContext;
            Call.Request = Request;
            Call.Priority = Context->Priority;
            Call.ServiceRequest = &ServiceRequest;
            Call.UserMode = FromUser;
            Call.Information = &Information;
            Call.CancelContext = &Context->CancelContext;
            Status = WtrExpandStack(WtrProcessWorker, &Call, Service->Master->StackSize);
            if (NT_SUCCESS(Status))
                Status = Call.Status;
        }
    }
    Session = WtrGetSessionContext(FileObject);

    WdfSpinLockAcquire(Session->Lock);
    if (Context->State == WtrRequestCompleted)
    {
        /* The client already finished it from another thread */
        WdfSpinLockRelease(Session->Lock);
        WdfRequestCompleteWithInformation(Request, Context->Status, Context->Information);
        return STATUS_SUCCESS;
    }

    if (!NT_SUCCESS(WdfRequestMarkCancelableEx(Request, WtrEvtSessionCancel)))
    {
        if (Status == STATUS_PENDING)
        {
            Context->State = WtrRequestCancelPending;
            WdfSpinLockRelease(Session->Lock);
            WtrEvtSessionCancel(Request);
            return STATUS_SUCCESS;
        }
        Context->State = WtrRequestCanceled;
    }
    else
    {
        Context->State = WtrRequestCancelable;
    }
    WdfSpinLockRelease(Session->Lock);

    DPRINT1("WTR: execute function 0x%lx in %Iu out %Iu -> 0x%lx info %I64u\n", Context->Request.FunctionCode, Context->Request.InputBufferLength, Context->Request.OutputBufferLength, Status, Information);

    if (Status == STATUS_PENDING)
        return STATUS_SUCCESS;

    if (NT_SUCCESS(Status))
    {
        Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Result), (PVOID *)&Result, NULL);
        if (NT_SUCCESS(Status))
        {
            RtlZeroMemory(Result, sizeof(*Result));
            Result->Version = 1;
            Result->Information = Information;
            return WtrCompleteSessionRequest(Request, STATUS_SUCCESS, sizeof(*Result));
        }
    }

    return WtrCompleteSessionRequest(Request, Status, 0);
}

static
VOID
NTAPI
WtrEvtSerializedDone(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context)
{
    WDF_REQUEST_REUSE_PARAMS Reuse;

    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Params);
    UNREFERENCED_PARAMETER(Context);

    WDF_REQUEST_REUSE_PARAMS_INIT(&Reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    WdfRequestReuse(Request, &Reuse);
}

/* Serialized clients run execute requests through their own device, one at a time */
static
VOID
NTAPI
WtrExecuteDeferred(
    _In_ WDFREQUEST Request)
{
    PWTR_SESSION_CONTEXT Session = WtrGetSessionContext(WdfRequestGetFileObject(Request));
    PWTR_SERVICE_CONTEXT Service = Session->Service;
    PWTR_MASTER_CONTEXT Master = Service->Master;
    PIO_STACK_LOCATION Next;
    NTSTATUS Status;

    Status = WdfIoTargetFormatRequestForInternalIoctlOthers(Master->SelfTarget,
                                                            Service->ReservedRequest,
                                                            IOCTL_WTR_SERIALIZE_REQUEST,
                                                            NULL, NULL, NULL, NULL, NULL, NULL);
    if (!NT_SUCCESS(Status))
        goto Fail;

    WdfRequestSetCompletionRoutine(Service->ReservedRequest, WtrEvtSerializedDone, NULL);

    Next = IoGetNextIrpStackLocation(WdfRequestWdmGetIrp(Service->ReservedRequest));
    Next->Parameters.Others.Argument1 = Request;

    if (WdfRequestSend(Service->ReservedRequest, Master->SelfTarget, WDF_NO_SEND_OPTIONS))
        return;

    Status = WdfRequestGetStatus(Service->ReservedRequest);

Fail:
    WtrCompleteSessionRequest(Request, Status, 0);
}

static
VOID
NTAPI
WtrSessionIoWorker(
    _In_ PVOID Parameter)
{
    PWTR_SESSION_IO_CALL Call = Parameter;

    Call->Callback(Call->Device, Call->Session, Call->Request);
}

static
VOID
NTAPI
WtrSessionOtherIo(
    _In_ WDFDEVICE Device,
    _In_ WDFFILEOBJECT FileObject,
    _In_ WDFREQUEST Request)
{
    PWTR_SESSION_CONTEXT Session;
    PWTR_SERVICE_CONTEXT Service;
    WTR_SESSION_IO_CALL Call;
    NTSTATUS Status;

    Status = WtrGetSession(FileObject, &Session);
    if (!NT_SUCCESS(Status))
        return;

    Service = Session->Service;
    if (!Service->Callbacks.EvtServiceOtherIo)
    {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    if (!Service->Master->StackSize)
    {
        Service->Callbacks.EvtServiceOtherIo(Device, Session->ClientContext, Request);
        return;
    }

    Call.Callback = Service->Callbacks.EvtServiceOtherIo;
    Call.Device = Device;
    Call.Session = Session->ClientContext;
    Call.Request = Request;
    Status = WtrExpandStack(WtrSessionIoWorker, &Call, Service->Master->StackSize);
    if (!NT_SUCCESS(Status))
        WdfRequestComplete(Request, Status);
}

static
VOID
NTAPI
WtrEvtSessionIoControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    WDFFILEOBJECT FileObject = WdfRequestGetFileObject(Request);
    PWTR_SESSION_CONTEXT Session = WtrGetSessionContext(FileObject);
    PWTR_REQUEST_CONTEXT Context = WtrGetRequestContext(Request);
    PWTR_MASTER_CONTEXT Master = Session->Service->Master;
    WDFDEVICE Device = WdfObjectContextGetObject(Session->Service);
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    Context->State = WtrRequestNew;
    Context->Status = STATUS_UNSUCCESSFUL;
    Context->Information = 0;

    if (IoControlCode == IOCTL_WTR_QUERY_SERVICE)
    {
        Context->State = WtrRequestQueryCancelable;
        Status = WdfRequestMarkCancelableEx(Request, WtrEvtSessionCancel);
        if (!NT_SUCCESS(Status))
            WdfRequestComplete(Request, Status);
        else
            WtrQueryServiceVersion(Request);
    }
    else if (IoControlCode == IOCTL_WTR_EXECUTE_REQUEST)
    {
        Context->State = WtrRequestExecuting;
        if (Master->Mode == WTR_MODE_SERIALIZED)
            WtrExecuteDeferred(Request);
        else
            WtrExecuteSessionRequest(Request);
    }
    else
    {
        WtrSessionOtherIo(Device, FileObject, Request);
    }
}

static
VOID
NTAPI
WtrEvtServiceIoControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    WDFFILEOBJECT FileObject = WdfRequestGetFileObject(Request);
    PWTR_SESSION_CONTEXT Session;
    PWTR_MASTER_CONTEXT Master;
    NTSTATUS Status;

    if (!FileObject)
    {
        Status = STATUS_INVALID_DEVICE_REQUEST;
        goto Complete;
    }

    Session = WtrGetSessionContext(FileObject);
    Master = Session->Service->Master;

    /* Reserved requests skip the caller context callback, capture here */
    if (WdfRequestIsReserved(Request))
    {
        if (WdfRequestGetRequestorMode(Request) == UserMode)
        {
            Status = STATUS_DEVICE_NOT_READY;
            goto Complete;
        }

        Status = WtrCaptureIoControl(Request, IoControlCode, InputBufferLength, OutputBufferLength);
        if (!NT_SUCCESS(Status))
            goto Complete;
    }

    switch (Master->Mode)
    {
        case WTR_MODE_PARALLEL:
        case WTR_MODE_SEQUENTIAL:
        case WTR_MODE_SERIALIZED:
            WtrEvtSessionIoControl(Queue, Request, OutputBufferLength, InputBufferLength, IoControlCode);
            return;

        case WTR_MODE_SESSION_QUEUE:
            if (Session->Queue)
            {
                Status = WdfRequestForwardToIoQueue(Request, Session->Queue);
                if (NT_SUCCESS(Status))
                    return;
            }
            else
            {
                Status = STATUS_INVALID_DEVICE_STATE;
            }
            break;

        default:
            Status = STATUS_INVALID_DEVICE_STATE;
            break;
    }

Complete:
    WdfRequestComplete(Request, Status);
}

static
NTSTATUS
NTAPI
WtrCreateServiceQueue(
    _In_ WDFDEVICE Device)
{
    PWTR_SERVICE_CONTEXT Service = WtrGetServiceContext(Device);
    PWTR_MASTER_CONTEXT Master = Service->Master;
    WDF_IO_QUEUE_FORWARD_PROGRESS_POLICY Policy;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG Config;
    WDFQUEUE Queue;
    NTSTATUS Status;

    if (Master->Mode == WTR_MODE_SEQUENTIAL || Master->Mode == WTR_MODE_SERIALIZED)
        WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&Config, WdfIoQueueDispatchSequential);
    else
        WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&Config, WdfIoQueueDispatchParallel);
    Config.EvtIoDefault = WtrEvtServiceDefault;
    Config.EvtIoDeviceControl = WtrEvtServiceIoControl;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    Attributes.SynchronizationScope = WdfSynchronizationScopeNone;

    Status = WdfIoQueueCreate(Device, &Config, &Attributes, &Queue);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfDeviceConfigureRequestDispatching(Device, Queue, WdfRequestTypeCreate);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Master->Mode == WTR_MODE_SERIALIZED)
    {
        Status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES, Master->SelfTarget, &Service->ReservedRequest);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    WDF_IO_QUEUE_FORWARD_PROGRESS_POLICY_DEFAULT_INIT(&Policy, 1);
    Policy.EvtIoAllocateResourcesForReservedRequest = WtrEvtAllocateReserved;
    Policy.EvtIoAllocateRequestResources = WtrEvtAllocateResources;
    WdfIoQueueAssignForwardProgressPolicy(Queue, &Policy);

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
WtrCreateStaticChild(
    _In_ WDFDEVICE Device,
    _In_ const GUID *ServiceId,
    _In_z_ PCWSTR HardwareId)
{
    WDF_DEVICE_PNP_CAPABILITIES PnpCaps;
    PNP_BUS_INFORMATION BusInformation;
    PWDFDEVICE_INIT ChildInit;
    UNICODE_STRING Id;
    WDFDEVICE Child;
    NTSTATUS Status;

    BusInformation.BusTypeGuid = *ServiceId;
    BusInformation.LegacyBusType = PNPBus;
    BusInformation.BusNumber = 0;
    WdfDeviceSetBusInformationForChildren(Device, &BusInformation);

    ChildInit = WdfPdoInitAllocate(Device);
    if (!ChildInit)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlInitUnicodeString(&Id, HardwareId);
    Status = WdfPdoInitAddHardwareID(ChildInit, &Id);
    if (NT_SUCCESS(Status))
        Status = WdfPdoInitAssignDeviceID(ChildInit, &Id);
    if (!NT_SUCCESS(Status))
    {
        WdfDeviceInitFree(ChildInit);
        return Status;
    }

    Status = WdfDeviceCreate(&ChildInit, WDF_NO_OBJECT_ATTRIBUTES, &Child);
    if (!NT_SUCCESS(Status))
    {
        WdfDeviceInitFree(ChildInit);
        return Status;
    }

    Status = WdfFdoAddStaticChild(Device, Child);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_DEVICE_PNP_CAPABILITIES_INIT(&PnpCaps);
    PnpCaps.EjectSupported = WdfFalse;
    PnpCaps.Removable = WdfFalse;
    PnpCaps.SurpriseRemovalOK = WdfFalse;
    WdfDeviceSetPnpCapabilities(Child, &PnpCaps);

    return STATUS_SUCCESS;
}

static
PCWSTR
NTAPI
WtrServiceSddl(
    _In_ PWTR_SERVICE_DESCRIPTOR Descriptor)
{
    PWTR_SERVICE_EXTENSION Extension;
    ULONG Index;

    for (Index = 0; Index < RTL_NUMBER_OF(WtrServiceSecurity); Index++)
    {
        if (IsEqualGUID(&WtrServiceSecurity[Index].ServiceId, &Descriptor->ServiceId))
            return WtrServiceSecurity[Index].Value;
    }

    if (Descriptor->ExtensionOffset)
    {
        Extension = (PWTR_SERVICE_EXTENSION)((PUCHAR)Descriptor + Descriptor->ExtensionOffset);
        if (Extension->SddlOffset)
            return (PCWSTR)((PUCHAR)Descriptor + Extension->SddlOffset);
    }

    return WTR_DEFAULT_SERVICE_SDDL;
}

/* Restricts the service PDO to what the service allows */
static
NTSTATUS
NTAPI
WtrApplyServiceSecurity(
    _In_ WDFDEVICE Device,
    _In_ PWTR_SERVICE_CONTEXT Service)
{
    static UNICODE_STRING RoutineName = RTL_CONSTANT_STRING(L"SeConvertStringSecurityDescriptorToSecurityDescriptor");
    WTR_SDDL_TO_SD *SddlToSd;
    NTSTATUS Status;

    SddlToSd = (WTR_SDDL_TO_SD *)MmGetSystemRoutineAddress(&RoutineName);
    if (!SddlToSd)
    {
        DPRINT1("WTR: no SDDL support, service PDO keeps its default security\n");
        return STATUS_SUCCESS;
    }

    Status = SddlToSd(WtrServiceSddl(Service->Descriptor), SDDL_REVISION_1, &Service->SecurityDescriptor, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    return ObSetSecurityObjectByPointer(WdfDeviceWdmGetPhysicalDevice(Device),
                                       DACL_SECURITY_INFORMATION,
                                       Service->SecurityDescriptor);
}

NTSTATUS
NTAPI
WtrAddServiceDevice(
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ const GUID *ServiceId,
    _Out_ WDFDEVICE *ServiceDevice,
    _Out_ WDFDEVICE *MasterDevice)
{
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS IdleSettings;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
    WDF_DEVICE_POWER_CAPABILITIES PowerCaps;
    WDF_DEVICE_PNP_CAPABILITIES PnpCaps;
    WDF_OBJECT_ATTRIBUTES RequestAttributes;
    WDF_OBJECT_ATTRIBUTES FileAttributes;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_FILEOBJECT_CONFIG FileConfig;
    PWTR_SERVICE_CONTEXT Service;
    WDFDEVICE Device;
    ULONG State;
    ULONG Index;
    NTSTATUS Status;

    WdfDeviceInitSetCharacteristics(DeviceInit, FILE_DEVICE_SECURE_OPEN | FILE_AUTOGENERATED_DEVICE_NAME, TRUE);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_WTR);

    WDF_FILEOBJECT_CONFIG_INIT(&FileConfig, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK);
    FileConfig.FileObjectClass = WdfFileObjectWdfCannotUseFsContexts;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&FileAttributes, WTR_SESSION_CONTEXT);
    FileAttributes.EvtDestroyCallback = WtrEvtSessionDestroy;
    FileAttributes.SynchronizationScope = WdfSynchronizationScopeNone;
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &FileConfig, &FileAttributes);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&RequestAttributes, WTR_REQUEST_CONTEXT);
    WdfDeviceInitSetRequestAttributes(DeviceInit, &RequestAttributes);

    WdfDeviceInitSetIoInCallerContextCallback(DeviceInit, WtrEvtServiceInCallerContext);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDeviceD0Entry = WtrEvtServiceD0Entry;
    PnpPowerCallbacks.EvtDeviceD0Exit = WtrEvtServiceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WTR_SERVICE_CONTEXT);
    Attributes.EvtCleanupCallback = WtrEvtServiceCleanup;
    Attributes.SynchronizationScope = WdfSynchronizationScopeNone;

    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_DEVICE_POWER_CAPABILITIES_INIT(&PowerCaps);
    PowerCaps.DeviceState[PowerSystemWorking] = PowerDeviceD0;
    for (State = PowerSystemSleeping1; State <= PowerSystemSleeping3; State++)
        PowerCaps.DeviceState[State] = PowerDeviceD0;
    PowerCaps.DeviceState[PowerSystemHibernate] = PowerDeviceD3;
    PowerCaps.DeviceState[PowerSystemShutdown] = PowerDeviceD3;
    WdfDeviceSetPowerCapabilities(Device, &PowerCaps);

    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&IdleSettings, IdleCannotWakeFromS0);
    IdleSettings.DxState = PowerDeviceD3;
    IdleSettings.IdleTimeoutType = SystemManagedIdleTimeout;
    Status = WdfDeviceAssignS0IdleSettings(Device, &IdleSettings);
    if (!NT_SUCCESS(Status))
        return Status;

    Service = WtrGetServiceContext(Device);
    Service->ServiceId = *ServiceId;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    Status = WdfCollectionCreate(&Attributes, &Service->SessionQueues);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfSpinLockCreate(&Attributes, &Service->Lock);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Spare call data for OS service calls made when allocation fails */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WTR_CALL_DATA);
    Attributes.ParentObject = Device;
    Status = WdfObjectCreate(&Attributes, &Service->CallData);
    if (!NT_SUCCESS(Status))
        return Status;

    KeInitializeEvent(&Service->CallDataFree, SynchronizationEvent, TRUE);

    WDF_DEVICE_PNP_CAPABILITIES_INIT(&PnpCaps);
    PnpCaps.NoDisplayInUI = WdfTrue;
    WdfDeviceSetPnpCapabilities(Device, &PnpCaps);

    for (Index = 0; Index < RTL_NUMBER_OF(WtrServiceChildren); Index++)
    {
        if (!IsEqualGUID(&WtrServiceChildren[Index].ServiceId, ServiceId))
            continue;

        Status = WtrCreateStaticChild(Device, ServiceId, WtrServiceChildren[Index].Value);
        if (!NT_SUCCESS(Status))
            return Status;
        break;
    }

    Status = WtrReportServiceDevice(Device, &Service->MasterDevice, &Service->Descriptor);
    DPRINT1("WTR: service device report -> 0x%lx\n", Status);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WtrApplyServiceSecurity(Device, Service);
    DPRINT1("WTR: service security -> 0x%lx\n", Status);
    if (!NT_SUCCESS(Status))
        return Status;

    Service->Master = WtrGetMasterContext(Service->MasterDevice);

    Status = WtrCreateServiceQueue(Device);
    if (!NT_SUCCESS(Status))
        return Status;

    *ServiceDevice = Device;
    *MasterDevice = Service->MasterDevice;
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
WtrEvtFinishCallWork(
    _In_ WDFWORKITEM WorkItem)
{
    WDFOBJECT CallData = WtrGetCompletionItem(WorkItem)->CallData;

    WtrGetCallData(CallData)->Completion(CallData);
    WdfObjectDereference(CallData);
    WdfObjectDelete(WorkItem);
}

/* Records the result of an OS service call and wakes whoever waits for it */
VOID
NTAPI
WtrFinishCall(
    _In_ WDFDEVICE Device,
    _In_ WDFOBJECT CallData,
    _In_ NTSTATUS Status,
    _In_ ULONG64 Information)
{
    PWTR_CALL_DATA Data = WtrGetCallData(CallData);
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_WORKITEM_CONFIG Config;
    WDFWORKITEM WorkItem;
    LONG Previous;

    Data->Status = Status;
    Data->Information = Information;

    Previous = InterlockedOr(&Data->Flags, WTR_CALL_COMPLETED);
    if (!(Previous & WTR_CALL_WAITER) || !Data->Completion)
        return;

    /* The completion may block, never run it from a completion routine */
    WDF_WORKITEM_CONFIG_INIT(&Config, WtrEvtFinishCallWork);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WTR_COMPLETION_ITEM);
    Attributes.ParentObject = Device;
    if (!NT_SUCCESS(WdfWorkItemCreate(&Config, &Attributes, &WorkItem)))
    {
        Data->Completion(CallData);
        return;
    }

    WdfObjectReference(CallData);
    WtrGetCompletionItem(WorkItem)->CallData = CallData;
    WdfWorkItemEnqueue(WorkItem);
}

static
VOID
NTAPI
WtrCallDone(
    _In_ WDFOBJECT CallData)
{
    KeSetEvent(WtrGetCallData(CallData)->CompletionEvent, IO_NO_INCREMENT, FALSE);
}

NTSTATUS
NTAPI
WtrCallOsService(
    _In_ WDFDEVICE ServiceDevice,
    _In_ const GUID *OsServiceId,
    _In_ PWTR_SERVICE_REQUEST Request,
    _Out_opt_ PULONG64 Information)
{
    PWTR_SERVICE_CONTEXT Service = WtrGetServiceContext(ServiceDevice);
    WDF_OBJECT_ATTRIBUTES Attributes;
    PWTR_CALL_DATA Data;
    WDFOBJECT CallData = NULL;
    KEVENT Event;
    NTSTATUS Status;

    if (!Service)
        return STATUS_INVALID_PARAMETER;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WTR_CALL_DATA);
    if (!NT_SUCCESS(WdfObjectCreate(&Attributes, &CallData)))
    {
        Status = KeWaitForSingleObject(&Service->CallDataFree, Executive, KernelMode, FALSE, NULL);
        if (Status != STATUS_SUCCESS)
            return Status;
        CallData = Service->CallData;
    }

    Data = WtrGetCallData(CallData);
    RtlZeroMemory(Data, sizeof(*Data));
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Data->Completion = WtrCallDone;
    Data->CompletionEvent = &Event;
    Data->Request = *Request;

    Status = WtrOsServiceExecute(Service->MasterDevice, OsServiceId, CallData);
    if (Status == STATUS_PENDING)
        Status = KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

    if (NT_SUCCESS(Status))
    {
        Status = Data->Status;
        if (Information)
            *Information = (ULONG)Data->Information;
    }

    DPRINT1("WTR: OS service call -> 0x%lx\n", Status);

    if (CallData == Service->CallData)
        KeSetEvent(&Service->CallDataFree, IO_NO_INCREMENT, FALSE);
    else
        WdfObjectDelete(CallData);

    return Status;
}
