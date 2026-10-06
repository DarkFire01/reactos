/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Hot plug event pump: one pended WAIT_ADAPTER_EVENT request, re-armed on completion
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4dr.h"

#define NDEBUG
#include <debug.h>

NTSTATUS
Usb4DrEventPump::Create(
    _In_ Usb4DrFdo* Fdo,
    _In_ PUSB4DR_EVENT_CALLBACK Callback,
    _In_ PVOID Context)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;

    m_Fdo = Fdo;
    m_Callback = Callback;
    m_Context = Context;
    m_State = Idle;
    m_SendState = SendIdle;
    KeInitializeSpinLock(&m_Lock);
    KeInitializeEvent(&m_Idle, NotificationEvent, TRUE);

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Fdo->Device();

    Status = WdfRequestCreate(&Attributes, Fdo->HostLink()->RootTarget(), &m_Request);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Event request create failed: 0x%lx\n", Status);
        m_Request = NULL;
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = m_Request;

    Status = WdfMemoryCreate(&Attributes,
                             NonPagedPool,
                             USB4DR_TAG_PORT,
                             sizeof(*m_Input),
                             &m_InputMemory,
                             (PVOID*)&m_Input);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Event input buffer create failed: 0x%lx\n", Status);
        m_InputMemory = NULL;
        return Status;
    }

    Status = WdfMemoryCreate(&Attributes,
                             NonPagedPool,
                             USB4DR_TAG_PORT,
                             sizeof(*m_Output),
                             &m_OutputMemory,
                             (PVOID*)&m_Output);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Event output buffer create failed: 0x%lx\n", Status);
        m_OutputMemory = NULL;
        return Status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Usb4DrEventPump::Start(
    _In_ USB4HR_HANDLE Handle,
    _In_ ULONG IoControlCode)
{
    if (IoControlCode != IOCTL_USB4HR_WAIT_ADAPTER_EVENT &&
        IoControlCode != IOCTL_USB4HR_WAIT_ROUTER_EVENT)
    {
        DPRINT1("Event pump cannot wait with IOCTL 0x%lx\n", IoControlCode);
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (m_Request == NULL || m_InputMemory == NULL || m_OutputMemory == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    {
        Usb4DrSpinLockGuard Guard(&m_Lock);

        if (m_State != Idle && m_State != Stopped)
        {
            DPRINT1("Event pump started twice, state %ld\n", m_State);
            return STATUS_DEVICE_BUSY;
        }

        m_Handle = Handle;
        m_IoControlCode = IoControlCode;
        m_State = Sending;
        KeClearEvent(&m_Idle);
    }

    return SendRequest();
}

VOID
Usb4DrEventPump::Stop()
{
    BOOLEAN CancelNow = FALSE;
    NTSTATUS Status;

    if (m_Request == NULL)
        return;

    {
        Usb4DrSpinLockGuard Guard(&m_Lock);

        switch (m_State)
        {
            case Sending:
                m_State = CancelRequested;
                CancelNow = TRUE;
                break;

            case Completing:
                /* The completion routine sees the request and stops */
                m_State = CancelRequested;
                break;

            case CancelRequested:
                break;

            default:
                return;
        }
    }

    if (CancelNow)
        WdfRequestCancelSentRequest(m_Request);

    Status = Usb4DrFdo::WaitBounded(&m_Idle, "event pump stop");
    if (!NT_SUCCESS(Status))
        DPRINT1("Event request on handle %p did not come back: 0x%lx\n", m_Handle, Status);
}

BOOLEAN
Usb4DrEventPump::IsRunning() const
{
    return m_State == Sending || m_State == Completing;
}

VOID
Usb4DrEventPump::EnterStoppedLocked()
{
    m_State = Stopped;
    KeSetEvent(&m_Idle, IO_NO_INCREMENT, FALSE);
}

NTSTATUS
Usb4DrEventPump::SendRequest()
{
    NTSTATUS Status;

    /* A host router that answers at once would otherwise nest one send per event */
    do
    {
        InterlockedExchange(&m_SendState, SendActive);

        Status = SendOnce();
        if (!NT_SUCCESS(Status))
        {
            InterlockedExchange(&m_SendState, SendIdle);
            return Status;
        }
    } while (InterlockedExchange(&m_SendState, SendIdle) == SendAgain);

    return STATUS_SUCCESS;
}

NTSTATUS
Usb4DrEventPump::SendOnce()
{
    WDF_REQUEST_REUSE_PARAMS Reuse;
    BOOLEAN CancelNow;
    NTSTATUS Status;

    WDF_REQUEST_REUSE_PARAMS_INIT(&Reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    Status = WdfRequestReuse(m_Request, &Reuse);
    if (NT_SUCCESS(Status))
    {
        m_Input->Handle = m_Handle;
        RtlZeroMemory(m_Output, sizeof(*m_Output));

        Status = WdfIoTargetFormatRequestForInternalIoctl(m_Fdo->HostLink()->RootTarget(),
                                                          m_Request,
                                                          m_IoControlCode,
                                                          m_InputMemory,
                                                          NULL,
                                                          m_OutputMemory,
                                                          NULL);
    }

    if (NT_SUCCESS(Status))
    {
        WdfRequestSetCompletionRoutine(m_Request, EvtCompletion, this);

        if (WdfRequestSend(m_Request, m_Fdo->HostLink()->RootTarget(), WDF_NO_SEND_OPTIONS))
        {
            /* Stop may have run before the request reached the target */
            {
                Usb4DrSpinLockGuard Guard(&m_Lock);
                CancelNow = (m_State == CancelRequested);
            }

            if (CancelNow)
                WdfRequestCancelSentRequest(m_Request);

            return STATUS_SUCCESS;
        }

        Status = WdfRequestGetStatus(m_Request);
        if (NT_SUCCESS(Status))
            Status = STATUS_UNSUCCESSFUL;
    }

    DPRINT1("Event request on handle %p not sent: 0x%lx\n", m_Handle, Status);

    {
        Usb4DrSpinLockGuard Guard(&m_Lock);
        EnterStoppedLocked();
    }

    return Status;
}

VOID
NTAPI
Usb4DrEventPump::EvtCompletion(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context)
{
    Usb4DrEventPump* Pump = static_cast<Usb4DrEventPump*>(Context);
    NTSTATUS Status = Params->IoStatus.Status;
    BOOLEAN Rearm = TRUE;

    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Target);

    {
        Usb4DrSpinLockGuard Guard(&Pump->m_Lock);

        if (Pump->m_State == CancelRequested)
        {
            Pump->EnterStoppedLocked();
            return;
        }

        Pump->m_State = Completing;
    }

    if (NT_SUCCESS(Status))
    {
        if (Params->IoStatus.Information >= sizeof(*Pump->m_Output))
            Pump->m_Callback(Pump->m_Context, Status, Pump->m_Output);
        else
            DPRINT1("Event on handle %p with %Iu output bytes dropped\n",
                    Pump->m_Handle, Params->IoStatus.Information);
    }
    else if (Status == STATUS_CANCELLED)
    {
        DPRINT("Event request on handle %p canceled by the host router\n", Pump->m_Handle);
    }
    else
    {
        /* Windows sends again after any failure, which spins on a request that keeps failing */
        DPRINT1("Event request on handle %p failed: 0x%lx, pump stopped\n", Pump->m_Handle, Status);
        Rearm = FALSE;
    }

    {
        Usb4DrSpinLockGuard Guard(&Pump->m_Lock);

        if (Pump->m_State == CancelRequested || !Rearm)
        {
            Pump->EnterStoppedLocked();
            return;
        }

        Pump->m_State = Sending;
    }

    /* Completed inside WdfRequestSend: the sender's loop sends again once it unwinds */
    if (InterlockedCompareExchange(&Pump->m_SendState, SendAgain, SendActive) == SendActive)
        return;

    Pump->SendRequest();
}
