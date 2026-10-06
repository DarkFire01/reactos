/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Router to router requests: the parent side queues and the child side sender
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4dr.h"

#define NDEBUG
#include <debug.h>

/* Below this much stack a request goes up from a work item, as Windows does */
#define USB4DR_FORWARD_MIN_STACK_BYTES  (12 * 1024)

/** Context of a DFP's PDO request queue. */
struct Usb4DrPortQueueContext
{
    Usb4DrPort* Port;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Usb4DrPortQueueContext, Usb4DrGetPortQueueContext);

/** Context of the one shot work item that sends a request on a fresh stack. */
struct Usb4DrSendWorkContext
{
    WDFREQUEST Request;
    BOOLEAN SendAndForget;
    PVOID Reservation;              /**< CREATE parent half to give back if the send fails */
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Usb4DrSendWorkContext, Usb4DrGetSendWorkContext);

/* Every request a child router may send its PDO, with the smallest buffers it must carry */
static const Usb4DrRequestLimits Usb4DrRequestTable[] =
{
    { IOCTL_USB4HR_CREATE_TUNNEL, USB4HR_R2R_MIN_CREATE_TUNNEL_IN, USB4HR_R2R_MIN_CREATE_TUNNEL_OUT },
    { IOCTL_USB4HR_DESTROY_TUNNEL, USB4HR_R2R_MIN_DESTROY_TUNNEL_IN, USB4HR_R2R_MIN_DESTROY_TUNNEL_OUT },
    { IOCTL_USB4HR_REBUILD_TUNNEL, USB4HR_R2R_MIN_REBUILD_TUNNEL_IN, USB4HR_R2R_MIN_REBUILD_TUNNEL_OUT },
    { IOCTL_USB4HR_ALLOCATE_DP_IN_ADAPTER, USB4HR_R2R_MIN_ALLOCATE_DP_IN_IN, USB4HR_R2R_MIN_ALLOCATE_DP_IN_OUT },
    { IOCTL_USB4HR_FREE_DP_IN_ADAPTER, USB4HR_R2R_MIN_FREE_DP_IN_IN, USB4HR_R2R_MIN_FREE_DP_IN_OUT },
    { IOCTL_USB4HR_EXCHANGE_DP_CAPABILITIES, USB4HR_R2R_MIN_EXCHANGE_DP_CAPS_IN, USB4HR_R2R_MIN_EXCHANGE_DP_CAPS_OUT },
    { IOCTL_USB4HR_INITIATE_LANE_BONDING, USB4HR_R2R_MIN_LANE_BONDING_IN, 0 },
    { IOCTL_USB4HR_READ_GRANDMASTER_TIME, 0, USB4HR_R2R_MIN_GRANDMASTER_TIME_OUT },
    { IOCTL_USB4HR_CLIENT_CREATE_INTER_DOMAIN_TUNNEL, USB4HR_R2R_MIN_CLIENT_XD_CREATE_IN, USB4HR_R2R_MIN_CLIENT_XD_CREATE_OUT },
    { IOCTL_USB4HR_CREATE_INTER_DOMAIN_PATH, USB4HR_R2R_MIN_XD_CREATE_IN, USB4HR_R2R_MIN_XD_CREATE_OUT },
    { IOCTL_USB4HR_CLIENT_DESTROY_INTER_DOMAIN_TUNNEL, USB4HR_R2R_MIN_CLIENT_XD_DESTROY_IN, 0 },
    { IOCTL_USB4HR_DESTROY_INTER_DOMAIN_PATH, USB4HR_R2R_MIN_XD_DESTROY_IN, USB4HR_R2R_MIN_XD_DESTROY_OUT },
    { IOCTL_USB4HR_ENABLE_UNI_TMU, 0, 0 },
    { IOCTL_USB4HR_ENABLE_ENHANCED_UNI_TMU, 0, 0 },
    { IOCTL_USB4HR_DISABLE_UNI_TMU, 0, 0 },
    { IOCTL_USB4HR_ENABLE_TIME_SYNC, 0, 0 },
    { IOCTL_USB4HR_DISABLE_TIME_SYNC, 0, 0 },
    { IOCTL_USB4HR_ENABLE_CLX, USB4HR_R2R_MIN_ENABLE_CLX_IN, USB4HR_R2R_MIN_ENABLE_CLX_OUT },
    { IOCTL_USB4HR_DISABLE_CLX, 0, 0 },
    { IOCTL_USB4HR_SET_PORT_CONFIGURED, 0, 0 },
    { IOCTL_USB4HR_CLEAR_PORT_CONFIGURED, 0, 0 },
    { IOCTL_USB4HR_GET_LAST_PORT_RESET_TIME, 0, USB4HR_R2R_MIN_LAST_PORT_RESET_OUT },
    { IOCTL_USB4HR_SWITCH_TO_ASYMMETRIC, 0, 0 },
    { IOCTL_USB4HR_SWITCH_TO_SYMMETRIC, 0, 0 },
    { IOCTL_USB4HR_VALIDATE_LINK_WIDTH, 0, 0 },
    { IOCTL_USB4HR_QUERY_RETIMERS, 0, 0 },
};

const Usb4DrRequestLimits*
NTAPI
Usb4DrFindRequestLimits(
    _In_ ULONG IoControlCode)
{
    ULONG Index;

    for (Index = 0; Index < RTL_NUMBER_OF(Usb4DrRequestTable); Index++)
    {
        if (Usb4DrRequestTable[Index].IoControlCode == IoControlCode)
            return &Usb4DrRequestTable[Index];
    }

    return NULL;
}

/* Usb4DrForwarder ************************************************************/

NTSTATUS
Usb4DrForwarder::Create(
    _In_ Usb4DrFdo* Fdo)
{
    PAGED_CODE();

    m_Fdo = Fdo;
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4DrForwarder::CreatePortQueue(
    _In_ Usb4DrPort* Port,
    _Out_ WDFQUEUE* Queue)
{
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDF_OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;

    PAGED_CODE();

    *Queue = NULL;

    WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.PowerManaged = WdfFalse;
    QueueConfig.EvtIoInternalDeviceControl = EvtPortIoctl;

    /* Filling our half reads config space, so the handlers must run at passive level */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, Usb4DrPortQueueContext);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;

    Status = WdfIoQueueCreate(m_Fdo->Device(), &QueueConfig, &Attributes, Queue);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DFP %u: PDO request queue not created 0x%lx\n", Port->Lane0Number(), Status);
        *Queue = NULL;
        return Status;
    }

    Usb4DrGetPortQueueContext(*Queue)->Port = Port;
    return STATUS_SUCCESS;
}

VOID
Usb4DrForwarder::ForwardFromChild(
    _In_ Usb4DrPort* Port,
    _In_ WDFREQUEST Request)
{
    WDF_REQUEST_FORWARD_OPTIONS Options;
    WDFQUEUE Queue;
    NTSTATUS Status;

    Queue = Port->ChildIoQueue();
    if (Queue == NULL)
    {
        DPRINT1("DFP %u: no PDO request queue\n", Port->Lane0Number());
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
        return;
    }

    WDF_REQUEST_FORWARD_OPTIONS_INIT(&Options);
    Status = WdfRequestForwardToParentDeviceIoQueue(Request, Queue, &Options);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DFP %u: child request not forwarded 0x%lx\n", Port->Lane0Number(), Status);
        WdfRequestComplete(Request, Status);
    }
}

Usb4DrForwarder*
Usb4DrForwarder::FromRequest(
    _In_ WDFREQUEST Request)
{
    WDFDEVICE Device = WdfIoQueueGetDevice(WdfRequestGetIoQueue(Request));

    return Usb4DrFdo::FromDevice(Device)->Forwarder();
}

VOID
NTAPI
Usb4DrForwarder::EvtPortIoctl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    Usb4DrPort* Port = Usb4DrGetPortQueueContext(Queue)->Port;
    Usb4DrForwarder* Self = Usb4DrFdo::FromDevice(WdfIoQueueGetDevice(Queue))->Forwarder();
    const Usb4DrRequestLimits* Limits;

    Limits = Usb4DrFindRequestLimits(IoControlCode);
    if (Limits == NULL)
    {
        DPRINT1("DFP %u: IOCTL 0x%lx is not a router to router request\n", Port->Lane0Number(), IoControlCode);
        WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
        return;
    }

    if (InputBufferLength < Limits->MinInput || OutputBufferLength < Limits->MinOutput)
    {
        /* Windows leaves such a request pending forever */
        DPRINT1("DFP %u: IOCTL 0x%lx buffers %Iu/%Iu below %lu/%lu\n",
                Port->Lane0Number(), IoControlCode, InputBufferLength, OutputBufferLength,
                Limits->MinInput, Limits->MinOutput);
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    switch (IoControlCode)
    {
        case IOCTL_USB4HR_CREATE_TUNNEL:
            Self->OnCreateTunnel(Port, Request);
            break;

        case IOCTL_USB4HR_DESTROY_TUNNEL:
            Self->OnDestroyTunnel(Port, Request);
            break;

        case IOCTL_USB4HR_REBUILD_TUNNEL:
            /* The host router keeps every path entry, so nobody below it has work to do */
            Self->PassUp(Request);
            break;

        default:
            /* DP, inter-domain, TMU, CLx, lane bonding and link width requests */
            DPRINT("DFP %u: IOCTL 0x%lx not handled this round\n", Port->Lane0Number(), IoControlCode);
            WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
            break;
    }
}

VOID
Usb4DrForwarder::OnCreateTunnel(
    _In_ Usb4DrPort* Port,
    _In_ WDFREQUEST Request)
{
    PUSB4HR_TUNNEL_BUILD_REQUEST Input;
    PVOID Reservation;
    NTSTATUS Status;

    PAGED_CODE();

    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Input), (PVOID*)&Input, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DFP %u: CREATE_TUNNEL input missing 0x%lx\n", Port->Lane0Number(), Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    if (Input->TunnelType == USB4HR_TUNNEL_DISPLAYPORT)
    {
        DPRINT("DFP %u: DP tunnels are not handled this round\n", Port->Lane0Number());
        WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
        return;
    }

    if (Input->TunnelType != USB4HR_TUNNEL_USB3 && Input->TunnelType != USB4HR_TUNNEL_PCIE)
    {
        DPRINT1("DFP %u: CREATE_TUNNEL of type %lu refused\n", Port->Lane0Number(), Input->TunnelType);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    /* A router further down already added the parent half; this link is not part of the tunnel */
    if (Input->Protocol.Outbound[0].AdapterHandle != NULL)
    {
        PassUp(Request);
        return;
    }

    Status = m_Fdo->Tunnels()->Segments()->FillParentHalf(Port, Input, &Reservation);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DFP %u: parent half of a type %lu tunnel not filled 0x%lx\n",
                Port->Lane0Number(), Input->TunnelType, Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    Status = SendWithCompletion(Request, EvtCreateCompleted, Reservation);
    if (!NT_SUCCESS(Status))
    {
        /* Windows keeps the HopIDs of a request that never left */
        DPRINT1("DFP %u: CREATE_TUNNEL not sent 0x%lx\n", Port->Lane0Number(), Status);
        m_Fdo->Tunnels()->Segments()->ReleaseParentHalf(Reservation);
        WdfRequestComplete(Request, Status);
    }
}

VOID
Usb4DrForwarder::OnDestroyTunnel(
    _In_ Usb4DrPort* Port,
    _In_ WDFREQUEST Request)
{
    PUSB4HR_TUNNEL_TEARDOWN_REQUEST Input;
    PVOID Reservation;
    NTSTATUS Status;

    PAGED_CODE();

    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Input), (PVOID*)&Input, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DFP %u: DESTROY_TUNNEL input missing 0x%lx\n", Port->Lane0Number(), Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    /* Only the router that reserved HopIDs for the tunnel has anything to free */
    Reservation = m_Fdo->Tunnels()->Segments()->FindByTunnel(Input->TunnelHandle);
    if (Reservation == NULL)
    {
        PassUp(Request);
        return;
    }

    Status = SendWithCompletion(Request, EvtDestroyCompleted, Reservation);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DFP %u: DESTROY_TUNNEL not sent 0x%lx\n", Port->Lane0Number(), Status);
        WdfRequestComplete(Request, Status);
    }
}

VOID
Usb4DrForwarder::PassUp(
    _In_ WDFREQUEST Request)
{
    NTSTATUS Status;

    WdfRequestFormatRequestUsingCurrentType(Request);

    Status = Send(Request, TRUE, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Router request not passed up 0x%lx\n", Status);
        WdfRequestComplete(Request, Status);
    }
}

NTSTATUS
Usb4DrForwarder::SendWithCompletion(
    _In_ WDFREQUEST Request,
    _In_ PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion,
    _In_ PVOID Reservation)
{
    WdfRequestFormatRequestUsingCurrentType(Request);
    WdfRequestSetCompletionRoutine(Request, Completion, Reservation);

    return Send(Request, FALSE, (Completion == EvtCreateCompleted) ? Reservation : NULL);
}

NTSTATUS
Usb4DrForwarder::Send(
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN SendAndForget,
    _In_opt_ PVOID Reservation)
{
    Usb4DrSendWorkContext* Context;
    WDF_REQUEST_SEND_OPTIONS Options;
    WDF_WORKITEM_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFWORKITEM WorkItem;
    NTSTATUS Status;

    /* Every router of a deep chain would otherwise stack its part of a CREATE on one thread */
    if (IoGetRemainingStackSize() < USB4DR_FORWARD_MIN_STACK_BYTES)
    {
        WDF_WORKITEM_CONFIG_INIT(&Config, EvtSendWork);
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, Usb4DrSendWorkContext);
        Attributes.ParentObject = m_Fdo->Device();

        Status = WdfWorkItemCreate(&Config, &Attributes, &WorkItem);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Send work item not created 0x%lx\n", Status);
            return Status;
        }

        Context = Usb4DrGetSendWorkContext(WorkItem);
        Context->Request = Request;
        Context->SendAndForget = SendAndForget;
        Context->Reservation = Reservation;

        DPRINT("Router request sent from a work item, %Iu bytes of stack left\n", IoGetRemainingStackSize());
        WdfWorkItemEnqueue(WorkItem);
        return STATUS_SUCCESS;
    }

    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, SendAndForget ? WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET : 0);

    if (WdfRequestSend(Request, WdfDeviceGetIoTarget(m_Fdo->Device()), &Options))
        return STATUS_SUCCESS;

    Status = WdfRequestGetStatus(Request);
    return NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;
}

VOID
NTAPI
Usb4DrForwarder::EvtSendWork(
    _In_ WDFWORKITEM WorkItem)
{
    Usb4DrSendWorkContext* Context = Usb4DrGetSendWorkContext(WorkItem);
    Usb4DrFdo* Fdo = Usb4DrFdo::FromDevice((WDFDEVICE)WdfWorkItemGetParentObject(WorkItem));
    WDF_REQUEST_SEND_OPTIONS Options;
    NTSTATUS Status;

    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, Context->SendAndForget ? WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET : 0);

    if (!WdfRequestSend(Context->Request, WdfDeviceGetIoTarget(Fdo->Device()), &Options))
    {
        Status = WdfRequestGetStatus(Context->Request);
        if (NT_SUCCESS(Status))
            Status = STATUS_UNSUCCESSFUL;

        DPRINT1("Router request not sent from the work item 0x%lx\n", Status);

        if (Context->Reservation != NULL)
            Fdo->Tunnels()->Segments()->ReleaseParentHalf(Context->Reservation);

        WdfRequestComplete(Context->Request, Status);
    }

    WdfObjectDelete(WorkItem);
}

VOID
NTAPI
Usb4DrForwarder::EvtCreateCompleted(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context)
{
    Usb4DrSegments* Segments = FromRequest(Request)->m_Fdo->Tunnels()->Segments();
    PUSB4HR_TUNNEL_BUILD_RESULT Output = NULL;
    NTSTATUS Status = Params->IoStatus.Status;
    size_t OutputLength = 0;
    NTSTATUS BufferStatus;

    UNREFERENCED_PARAMETER(Target);

    BufferStatus = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Output), (PVOID*)&Output, &OutputLength);
    if (!NT_SUCCESS(BufferStatus))
    {
        DPRINT1("CREATE_TUNNEL output missing on completion 0x%lx\n", BufferStatus);
        Status = BufferStatus;
        OutputLength = 0;
    }
    else if (NT_SUCCESS(Status) && OutputLength != sizeof(*Output))
    {
        DPRINT1("CREATE_TUNNEL output of %Iu bytes\n", OutputLength);
        Status = STATUS_INVALID_PARAMETER;
    }

    if (NT_SUCCESS(Status) && Output->Status == USB4HR_STATUS_SUCCESS)
    {
        DPRINT("Tunnel %p: parent half committed\n", Output->TunnelHandle);
        Segments->CommitParentHalf(Context, Output->TunnelHandle);
    }
    else
    {
        DPRINT1("CREATE_TUNNEL failed upstream 0x%lx, USB4 status %lu\n",
                Status, Output != NULL ? Output->Status : USB4HR_STATUS_FAILURE);
        Segments->ReleaseParentHalf(Context);
    }

    WdfRequestCompleteWithInformation(Request, Status, OutputLength);
}

VOID
NTAPI
Usb4DrForwarder::EvtDestroyCompleted(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context)
{
    Usb4DrSegments* Segments = FromRequest(Request)->m_Fdo->Tunnels()->Segments();
    PUSB4HR_TUNNEL_STATUS_OUTPUT Output = NULL;
    NTSTATUS Status = Params->IoStatus.Status;
    size_t OutputLength = 0;
    NTSTATUS BufferStatus;

    UNREFERENCED_PARAMETER(Target);

    BufferStatus = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Output), (PVOID*)&Output, &OutputLength);
    if (!NT_SUCCESS(BufferStatus))
    {
        DPRINT1("DESTROY_TUNNEL output missing on completion 0x%lx\n", BufferStatus);
        Status = BufferStatus;
        OutputLength = 0;
    }
    else if (NT_SUCCESS(Status) && OutputLength != sizeof(*Output))
    {
        DPRINT1("DESTROY_TUNNEL output of %Iu bytes\n", OutputLength);
        Status = STATUS_INVALID_PARAMETER;
    }

    /* After a failure the path may still be programmed, so the HopIDs stay taken */
    if (NT_SUCCESS(Status))
        Segments->ReleaseParentHalf(Context);
    else
        DPRINT1("DESTROY_TUNNEL failed upstream 0x%lx, parent half kept\n", Status);

    WdfRequestCompleteWithInformation(Request, Status, OutputLength);
}

/* Usb4DrParentLink ***********************************************************/

NTSTATUS
Usb4DrParentLink::Create(
    _In_ Usb4DrFdo* Fdo)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDEVICE Device = Fdo->Device();
    NTSTATUS Status;

    PAGED_CODE();

    m_Fdo = Fdo;
    m_InFlight = FALSE;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;

    Status = WdfWaitLockCreate(&Attributes, &m_Lock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Parent link: wait lock not created 0x%lx\n", Status);
        return Status;
    }

    Status = WdfRequestCreate(&Attributes, WdfDeviceGetIoTarget(Device), &m_Request);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Parent link: request not created 0x%lx\n", Status);
        return Status;
    }

    Status = WdfMemoryCreate(&Attributes,
                             NonPagedPool,
                             USB4DR_TAG_TUNNEL,
                             sizeof(USB4HR_TUNNEL_BUILD_REQUEST),
                             &m_InputMemory,
                             &m_Input);
    if (NT_SUCCESS(Status))
    {
        Status = WdfMemoryCreate(&Attributes,
                                 NonPagedPool,
                                 USB4DR_TAG_TUNNEL,
                                 sizeof(USB4HR_TUNNEL_BUILD_RESULT),
                                 &m_OutputMemory,
                                 &m_Output);
    }

    if (!NT_SUCCESS(Status))
        DPRINT1("Parent link: buffers not allocated 0x%lx\n", Status);

    return Status;
}

VOID
Usb4DrParentLink::Destroy()
{
    PAGED_CODE();

    /* Every object here belongs to the device and goes with it */
    m_Request = NULL;
    m_InputMemory = NULL;
    m_OutputMemory = NULL;
    m_Input = NULL;
    m_Output = NULL;
}

NTSTATUS
Usb4DrParentLink::CreateTunnel(
    _In_ const USB4HR_TUNNEL_BUILD_REQUEST* Input,
    _Out_ PUSB4HR_TUNNEL_BUILD_RESULT Output)
{
    PUSB4HR_TUNNEL_BUILD_RESULT Reply;
    NTSTATUS Status;

    PAGED_CODE();

    RtlZeroMemory(Output, sizeof(*Output));
    Output->Status = USB4HR_STATUS_FAILURE;

    if (m_Request == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    Usb4DrWaitLockGuard Guard(m_Lock);

    Reply = (PUSB4HR_TUNNEL_BUILD_RESULT)m_Output;
    RtlCopyMemory(m_Input, Input, sizeof(*Input));
    RtlZeroMemory(Reply, sizeof(*Reply));
    Reply->Status = USB4HR_STATUS_FAILURE;

    Status = Send(IOCTL_USB4HR_CREATE_TUNNEL, sizeof(*Input), sizeof(*Reply));
    RtlCopyMemory(Output, Reply, sizeof(*Output));

    if (!NT_SUCCESS(Status) || Output->Status != USB4HR_STATUS_SUCCESS)
    {
        DPRINT1("Parent link: CREATE_TUNNEL type %lu failed 0x%lx, USB4 status %lu\n",
                Input->TunnelType, Status, Output->Status);
    }

    return Status;
}

NTSTATUS
Usb4DrParentLink::DestroyTunnel(
    _In_ USB4HR_HANDLE TunnelHandle,
    _Out_ PUSB4HR_STATUS Usb4Status)
{
    PUSB4HR_TUNNEL_TEARDOWN_REQUEST Request;
    PUSB4HR_TUNNEL_STATUS_OUTPUT Reply;
    NTSTATUS Status;

    PAGED_CODE();

    *Usb4Status = USB4HR_STATUS_FAILURE;

    if (m_Request == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    Usb4DrWaitLockGuard Guard(m_Lock);

    Request = (PUSB4HR_TUNNEL_TEARDOWN_REQUEST)m_Input;
    Reply = (PUSB4HR_TUNNEL_STATUS_OUTPUT)m_Output;
    RtlZeroMemory(Request, sizeof(*Request));
    Request->TunnelHandle = TunnelHandle;
    Reply->Status = USB4HR_STATUS_FAILURE;

    Status = Send(IOCTL_USB4HR_DESTROY_TUNNEL, sizeof(*Request), sizeof(*Reply));
    *Usb4Status = Reply->Status;

    if (!NT_SUCCESS(Status))
        DPRINT1("Parent link: DESTROY_TUNNEL %p failed 0x%lx\n", TunnelHandle, Status);

    return Status;
}

NTSTATUS
Usb4DrParentLink::RebuildTunnel(
    _In_ USB4HR_HANDLE TunnelHandle,
    _Out_ PUSB4HR_STATUS Usb4Status)
{
    PUSB4HR_TUNNEL_RESTORE_REQUEST Request;
    PUSB4HR_TUNNEL_STATUS_OUTPUT Reply;
    NTSTATUS Status;

    PAGED_CODE();

    *Usb4Status = USB4HR_STATUS_FAILURE;

    if (m_Request == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    Usb4DrWaitLockGuard Guard(m_Lock);

    /* USB3 and PCIe tunnels have no reduced DP rate or lane count to pass */
    Request = (PUSB4HR_TUNNEL_RESTORE_REQUEST)m_Input;
    Reply = (PUSB4HR_TUNNEL_STATUS_OUTPUT)m_Output;
    RtlZeroMemory(Request, sizeof(*Request));
    Request->TunnelHandle = TunnelHandle;
    Reply->Status = USB4HR_STATUS_FAILURE;

    Status = Send(IOCTL_USB4HR_REBUILD_TUNNEL, sizeof(*Request), sizeof(*Reply));
    *Usb4Status = Reply->Status;

    if (!NT_SUCCESS(Status))
        DPRINT1("Parent link: REBUILD_TUNNEL %p failed 0x%lx\n", TunnelHandle, Status);

    return Status;
}

VOID
Usb4DrParentLink::Cancel()
{
    if (m_Request != NULL && InterlockedCompareExchange(&m_InFlight, TRUE, TRUE))
    {
        DPRINT("Parent link: canceling the request in flight\n");
        WdfRequestCancelSentRequest(m_Request);
    }
}

NTSTATUS
Usb4DrParentLink::Send(
    _In_ ULONG IoControlCode,
    _In_ ULONG InputLength,
    _In_ ULONG OutputLength)
{
    WDF_REQUEST_REUSE_PARAMS ReuseParams;
    WDF_REQUEST_SEND_OPTIONS Options;
    WDFMEMORY_OFFSET InputOffset;
    WDFMEMORY_OFFSET OutputOffset;
    WDFIOTARGET Target;
    NTSTATUS Status;

    PAGED_CODE();

    Target = WdfDeviceGetIoTarget(m_Fdo->Device());

    WDF_REQUEST_REUSE_PARAMS_INIT(&ReuseParams, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_NOT_SUPPORTED);
    Status = WdfRequestReuse(m_Request, &ReuseParams);
    if (!NT_SUCCESS(Status))
        return Status;

    InputOffset.BufferOffset = 0;
    InputOffset.BufferLength = InputLength;
    OutputOffset.BufferOffset = 0;
    OutputOffset.BufferLength = OutputLength;

    Status = WdfIoTargetFormatRequestForInternalIoctl(Target,
                                                      m_Request,
                                                      IoControlCode,
                                                      m_InputMemory,
                                                      &InputOffset,
                                                      m_OutputMemory,
                                                      &OutputOffset);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Windows sets no timeout: the parent may wait for its own parent; D0 exit cancels */
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);

    InterlockedExchange(&m_InFlight, TRUE);
    if (!WdfRequestSend(m_Request, Target, &Options))
        DPRINT("Parent link: IOCTL 0x%lx returned 0x%lx\n", IoControlCode, WdfRequestGetStatus(m_Request));
    InterlockedExchange(&m_InFlight, FALSE);

    return WdfRequestGetStatus(m_Request);
}
