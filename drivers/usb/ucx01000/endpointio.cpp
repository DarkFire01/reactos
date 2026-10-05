/*
 * PROJECT:     ReactOS USB Host Controller Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Endpoint reset, abort pipe and static streams request paths
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "ucx01000.h"

#define NDEBUG
#include <debug.h>

/* Endpoint reset */

VOID
UcxEndpoint::HandleHubReset(
    _In_ WDFREQUEST Request)
{
    SetPending(Request);
    Post(EpEvent::HubEndpointReset);
}

/* Held until the machine is done with the reset, whoever completed the request */
NTSTATUS
UcxEndpoint::OnHcdResetDone(
    _In_ PIRP Irp)
{
    SetPending(Irp);
    Post(EpEvent::EndpointResetDone);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Abort pipe */

/**
 * Only an endpoint that is running takes the abort; anything else fails on
 * the caller's thread, as USBPORT did. The IRP waits in the cancel safe
 * queue until the controller driver finished the abort.
 */
NTSTATUS
UcxEndpoint::AbortPipe(
    _In_ PIRP Irp,
    _In_ PURB Urb)
{
    if (!m_HasMachine || !Post(EpEvent::ClientAbortUrb))
    {
        return UcxCompleteUrb(Irp,
                              Urb,
                              STATUS_NO_SUCH_DEVICE,
                              UcxNtStatusToUsbdStatus(STATUS_NO_SUCH_DEVICE));
    }

    IoCsqInsertIrp(&m_Controller->m_AbortPipeCsq, Irp, &m_AbortCsqContext);
    Post(EpEvent::AbortUrbParked);

    return STATUS_PENDING;
}

/* Static streams */

/** Fails a streams URB on the submitting thread. */
static
NTSTATUS
NTAPI
UcxFailStreamsUrb(
    _In_ PIRP Irp,
    _In_ PURB Urb,
    _In_ NTSTATUS Status)
{
    return UcxCompleteUrb(Irp, Urb, Status, UcxNtStatusToUsbdStatus(Status));
}

/*
 * Asks the controller driver for a streams object and checks it set every
 * stream. On failure nothing is left behind.
 */
static
NTSTATUS
NTAPI
UcxCreateStaticStreams(
    _In_ UcxEndpoint* Endpoint,
    _In_ ULONG Count,
    _Out_ UcxStaticStreams** StreamsOut)
{
    BOOLEAN Verifying = Endpoint->m_Controller->m_DriverVerifierEnabled;
    UcxStaticStreamsInit Init;
    UcxStaticStreams* Streams;
    ULONG Index;
    NTSTATUS Status;

    *StreamsOut = NULL;

    RtlZeroMemory(&Init, sizeof(Init));
    Init.Endpoint = Endpoint->m_Handle;
    Init.StreamCount = Count;

    Status = Endpoint->m_Callbacks.StaticStreamsAdd(Endpoint->m_Handle, Count, &Init);
    if (!NT_SUCCESS(Status))
    {
        if (Init.Created != NULL)
            WdfObjectDelete(Init.Created);
        return Status;
    }

    /* Windows crashes on a success without a streams object */
    if (Init.Created == NULL)
    {
        UcxVerifierBreak(Verifying);
        return STATUS_INTERNAL_ERROR;
    }

    Streams = UcxStaticStreams::FromHandle(Init.Created);
    Streams->m_Init = NULL;

    Status = Init.Failed ? STATUS_INTERNAL_ERROR : STATUS_SUCCESS;
    for (Index = 0; NT_SUCCESS(Status) && Index < Count; Index++)
    {
        if (!Streams->Stream(Index)->Filled)
            Status = STATUS_INTERNAL_ERROR;
    }

    if (!NT_SUCCESS(Status))
    {
        UcxVerifierBreak(Verifying);
        WdfObjectDelete(Init.Created);
        return Status;
    }

    *StreamsOut = Streams;
    return STATUS_SUCCESS;
}

/* Only XRBs of 0x602 clients get here; the pipe handle is not validated */
NTSTATUS
UcxEndpoint::OpenStaticStreams(
    _In_ PIRP Irp,
    _In_ PURB Urb)
{
    UcxUsbdHandle* Client = UcxXrbFromUrb(Urb)->Handle;
    ULONG Count = Urb->UrbOpenStaticStreams.NumberOfStreams;
    UcxStaticStreams* Streams;
    KIRQL Irql = KeGetCurrentIrql();
    NTSTATUS Status;

    if (Irql != PASSIVE_LEVEL)
    {
        if (Client->m_VerifierEnabled)
        {
            KeBugCheckEx(UCX_BUGCHECK_USB3,
                         UCX_USB3_STREAMS_IRQL,
                         Irql,
                         (ULONG_PTR)Irp,
                         (ULONG_PTR)Client->m_ClientDeviceObject);
        }
        return UcxFailStreamsUrb(Irp, Urb, STATUS_INVALID_PARAMETER);
    }

    if (!Client->m_StreamsGranted)
    {
        if (Client->m_VerifierEnabled)
        {
            KeBugCheckEx(UCX_BUGCHECK_USB3,
                         UCX_USB3_STREAMS_NOT_GRANTED,
                         (ULONG_PTR)Irp,
                         (ULONG_PTR)Urb,
                         (ULONG_PTR)Client->m_ClientDeviceObject);
        }
        return UcxFailStreamsUrb(Irp, Urb, STATUS_INVALID_PARAMETER);
    }

    if (Count == 0 || Count > Client->m_GrantedStreams)
    {
        if (Client->m_VerifierEnabled)
        {
            KeBugCheckEx(UCX_BUGCHECK_USB3,
                         UCX_USB3_STREAMS_BAD_COUNT,
                         Client->m_GrantedStreams,
                         Count,
                         (ULONG_PTR)Urb);
        }
        return UcxFailStreamsUrb(Irp, Urb, STATUS_INVALID_PARAMETER);
    }

    if (UcxVerifierWantsFailure(Client->m_VerifierFailEnableStaticStreams))
        return UcxFailStreamsUrb(Irp, Urb, UcxRandomErrorStatus());

    /* One open at a time per endpoint */
    if (InterlockedIncrement(&m_StreamsOpenCount) != 1)
    {
        InterlockedDecrement(&m_StreamsOpenCount);

        if (Client->m_VerifierEnabled)
        {
            KeBugCheckEx(UCX_BUGCHECK_USB3,
                         UCX_USB3_STREAMS_OPEN,
                         (ULONG_PTR)Irp,
                         (ULONG_PTR)Urb,
                         (ULONG_PTR)Client->m_ClientDeviceObject);
        }
        return UcxFailStreamsUrb(Irp, Urb, STATUS_INVALID_DEVICE_STATE);
    }

    Status = UcxCreateStaticStreams(this, Count, &Streams);
    if (!NT_SUCCESS(Status))
    {
        InterlockedDecrement(&m_StreamsOpenCount);
        return UcxFailStreamsUrb(Irp, Urb, Status);
    }

    {
        SpinLockGuard Guard(&m_Controller->m_TopologyLock);

        NT_ASSERT(m_Streams == NULL);
        m_Streams = Streams;
    }

    return UcxForwardStreamsUrb(m_Controller->m_RootHub->Pdo(), Irp, Urb, m_Controller);
}

/* A close while the root hub sleeps succeeds and leaves the streams in place, as on Windows */
NTSTATUS
UcxEndpoint::CloseStaticStreams(
    _In_ PIRP Irp,
    _In_ PURB Urb)
{
    if (!m_Controller->m_RootHubInD0)
        return UcxCompleteUrb(Irp, Urb, STATUS_SUCCESS, USBD_STATUS_SUCCESS);

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return UcxFailStreamsUrb(Irp, Urb, STATUS_INVALID_PARAMETER);

    if (m_Streams == NULL)
        return UcxFailStreamsUrb(Irp, Urb, STATUS_INVALID_DEVICE_STATE);

    return UcxForwardStreamsUrb(m_Controller->m_RootHub->Pdo(), Irp, Urb, m_Controller);
}

VOID
UcxEndpoint::HandleClientStreamsEnable(
    _In_ WDFREQUEST Request)
{
    SetPending(Request);
    Post(EpEvent::ClientStreamsEnable);
}

VOID
UcxEndpoint::HandleClientStreamsDisable(
    _In_ WDFREQUEST Request)
{
    SetPending(Request);
    Post(EpEvent::ClientStreamsDisable);
}

/**
 * Runs after the XRB completion fixed up the statuses. A failed open drops
 * the streams; a successful one hands the stream pipes to the client.
 */
NTSTATUS
UcxEndpoint::OpenStaticStreamsComplete(
    _In_ PIRP Irp,
    _In_ PURB Urb)
{
    PUSBD_STREAM_INFORMATION Info = Urb->UrbOpenStaticStreams.Streams;
    UcxStaticStreams* Streams;
    UcxStream* Entry;
    ULONG Index;

    if (!NT_SUCCESS(Irp->IoStatus.Status))
    {
        {
            SpinLockGuard Guard(&m_Controller->m_TopologyLock);

            Streams = m_Streams;
            m_Streams = NULL;
        }

        if (Streams != NULL)
            WdfObjectDelete(Streams->m_Handle);
    }
    else
    {
        Streams = m_Streams;

        for (Index = 0; Index < Streams->m_StreamCount; Index++)
        {
            Entry = Streams->Stream(Index);

            Info[Index].PipeHandle = Entry->Pipe.Handle();
            Info[Index].StreamID = Entry->StreamId;
            Info[Index].MaximumTransferSize = Entry->Pipe.MaximumTransferSize;
            Info[Index].PipeFlags = 0;
        }
    }

    /* The machine never saw this open; finish it here */
    if (m_OpenFailedOnReset)
    {
        m_OpenFailedOnReset = FALSE;
        InterlockedDecrement(&m_StreamsOpenCount);
        UcxCompleteHeldUrbIrp(Irp);

        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    SetPending(Irp);
    Post(EpEvent::StreamsOpened);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
UcxEndpoint::CloseStaticStreamsComplete(
    _In_ PIRP Irp)
{
    SetPending(Irp);
    Post(EpEvent::StreamsClosed);

    return STATUS_MORE_PROCESSING_REQUIRED;
}
