/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Hot plug event pump: one pended WAIT_ADAPTER_EVENT request, re-armed on completion
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/** One event from the host router. Runs at <= DISPATCH_LEVEL from the request completion. */
typedef VOID
(NTAPI USB4DR_EVENT_CALLBACK)(
    _In_ PVOID Context,
    _In_ NTSTATUS Status,
    _In_opt_ const USB4HR_WAIT_OUTPUT* Event);
typedef USB4DR_EVENT_CALLBACK *PUSB4DR_EVENT_CALLBACK;

/** Keeps one WAIT_ROUTER_EVENT or WAIT_ADAPTER_EVENT request pended on the root router target. */
class Usb4DrEventPump
{
public:
    /** Creates the request and its buffers. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Create(
        _In_ Usb4DrFdo* Fdo,
        _In_ PUSB4DR_EVENT_CALLBACK Callback,
        _In_ PVOID Context);

    /** Starts pending IoControlCode (IOCTL_USB4HR_WAIT_ADAPTER_EVENT or _ROUTER_EVENT) on Handle. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    Start(
        _In_ USB4HR_HANDLE Handle,
        _In_ ULONG IoControlCode);

    /** Cancels the pended request and waits for its completion; no callback after this returns. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Stop();

    BOOLEAN IsRunning() const;

private:
    /** Pump states; a request is outstanding only in Sending and CancelRequested. */
    enum State : LONG
    {
        Idle = 0,
        Sending = 1,
        Completing = 2,
        CancelRequested = 3,
        Stopped = 4
    };

    static EVT_WDF_REQUEST_COMPLETION_ROUTINE EvtCompletion;

    /** Formats and sends the request; the caller already moved the state to Sending. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS SendRequest();

    /** Leaves the running states and wakes Stop. Called with m_Lock held. */
    _IRQL_requires_(DISPATCH_LEVEL)
    VOID EnterStoppedLocked();

    Usb4DrFdo* m_Fdo;
    PUSB4DR_EVENT_CALLBACK m_Callback;
    PVOID m_Context;
    WDFREQUEST m_Request;
    WDFMEMORY m_InputMemory;
    WDFMEMORY m_OutputMemory;
    PUSB4HR_WAIT_INPUT m_Input;
    PUSB4HR_WAIT_OUTPUT m_Output;
    USB4HR_HANDLE m_Handle;
    ULONG m_IoControlCode;
    KSPIN_LOCK m_Lock;
    LONG m_State;

    /** Signaled while no request is outstanding. */
    KEVENT m_Idle;
};
