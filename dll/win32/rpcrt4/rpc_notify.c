/*
 * PROJECT:     ReactOS RPC runtime
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Server call notifications (client disconnect and call cancel)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"

#include "rpc.h"
#include "rpcndr.h"
#include "rpcasync.h"

#include "wine/debug.h"

#include "rpc_binding.h"
#include "rpc_notify.h"

WINE_DEFAULT_DEBUG_CHANNEL(rpc);

#define RPCP_NOTIFY_ALL (RpcNotificationClientDisconnect | RpcNotificationCallCancel)

typedef struct _RPCP_NOTIFY_APC
{
    PFN_RPCNOTIFICATION_ROUTINE Routine;
    RpcBinding *Binding;
    RPC_ASYNC_EVENT Event;
} RPCP_NOTIFY_APC;

static struct list RpcpActiveCalls = LIST_INIT(RpcpActiveCalls);

static CRITICAL_SECTION RpcpNotifyLock;
static CRITICAL_SECTION_DEBUG RpcpNotifyLockDebug =
{
    0, 0, &RpcpNotifyLock,
    { &RpcpNotifyLockDebug.ProcessLocksList, &RpcpNotifyLockDebug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": RpcpNotifyLock") }
};
static CRITICAL_SECTION RpcpNotifyLock = { &RpcpNotifyLockDebug, -1, 0, 0, 0, 0 };

static
ULONG
RpcpNotifyIndex(
    _In_ ULONG Notification)
{
    return (Notification == RpcNotificationClientDisconnect) ? 0 : 1;
}

static
RPC_ASYNC_EVENT
RpcpNotifyEvent(
    _In_ ULONG Notification)
{
    return (Notification == RpcNotificationClientDisconnect) ? RpcClientDisconnect : RpcClientCancel;
}

/* Caller holds RpcpNotifyLock */
static
RPCP_CALL_NOTIFY *
RpcpFindCall(
    _In_ RpcBinding *Binding)
{
    RPCP_CALL_NOTIFY *Call;
    RPCP_CALL_NOTIFY *Found = NULL;
    DWORD ThreadId = GetCurrentThreadId();

    LIST_FOR_EACH_ENTRY(Call, &RpcpActiveCalls, RPCP_CALL_NOTIFY, Entry)
    {
        if (Call->Binding != Binding)
            continue;
        if (Call->ThreadId == ThreadId)
            return Call;
        if (!Found)
            Found = Call;
    }

    return Found;
}

static
RPC_STATUS
RpcpResolveCallBinding(
    _In_opt_ RPC_BINDING_HANDLE Handle,
    _Out_ RpcBinding **Binding)
{
    *Binding = Handle ? (RpcBinding *)Handle : RPCRT4_GetThreadCurrentCallHandle();

    if (!*Binding)
        return RPC_S_NO_CALL_ACTIVE;
    if (!(*Binding)->server)
        return RPC_S_WRONG_KIND_OF_BINDING;

    return RPC_S_OK;
}

static
VOID
WINAPI
RpcpNotifyApcRoutine(
    _In_ ULONG_PTR Parameter)
{
    RPCP_NOTIFY_APC *Apc = (RPCP_NOTIFY_APC *)Parameter;

    Apc->Routine((PRPC_ASYNC_STATE)Apc->Binding, NULL, Apc->Event);
    RPCRT4_ReleaseBinding(Apc->Binding);
    free(Apc);
}

/* Delivers everything except callbacks; caller holds RpcpNotifyLock */
static
VOID
RpcpIssueLocked(
    _In_ RPCP_CALL_NOTIFY *Call,
    _In_ const RPCP_NOTIFY_SUBSCRIPTION *Subscription,
    _In_ RPC_ASYNC_EVENT Event)
{
    const RPC_ASYNC_NOTIFICATION_INFO *Info = &Subscription->Info;
    RPCP_NOTIFY_APC *Apc;
    HANDLE Thread;

    switch (Subscription->Type)
    {
        case RpcNotificationTypeEvent:
            SetEvent(Info->hEvent);
            break;

        case RpcNotificationTypeApc:
            Apc = malloc(sizeof(*Apc));
            if (!Apc)
                break;

            Apc->Routine = Info->APC.NotificationRoutine;
            Apc->Binding = Call->Binding;
            Apc->Event = Event;
            RPCRT4_AddRefBinding(Apc->Binding);

            Thread = Info->APC.hThread ? Info->APC.hThread : Call->Thread;
            if (!QueueUserAPC(RpcpNotifyApcRoutine, Thread, (ULONG_PTR)Apc))
            {
                WARN("QueueUserAPC failed %lu\n", GetLastError());
                RPCRT4_ReleaseBinding(Apc->Binding);
                free(Apc);
            }
            break;

        case RpcNotificationTypeIoc:
            PostQueuedCompletionStatus(Info->IOC.hIOPort,
                                       Info->IOC.dwNumberOfBytesTransferred,
                                       Info->IOC.dwCompletionKey,
                                       Info->IOC.lpOverlapped);
            break;

        default:
            break;
    }
}

/*
 * Marks the requested notifications as subscribed and returns the ones whose
 * event already happened. Caller holds RpcpNotifyLock.
 */
static
ULONG
RpcpAddSubscription(
    _Inout_ RPCP_CALL_NOTIFY *Call,
    _In_ ULONG Notification,
    _In_ RPC_NOTIFICATION_TYPES Type,
    _In_ const RPC_ASYNC_NOTIFICATION_INFO *Info)
{
    RPCP_NOTIFY_SUBSCRIPTION *Subscription;
    ULONG Bit;
    ULONG Pending = 0;

    for (Bit = RpcNotificationClientDisconnect; Bit <= RpcNotificationCallCancel; Bit <<= 1)
    {
        if (!(Notification & Bit))
            continue;

        Subscription = &Call->Subscription[RpcpNotifyIndex(Bit)];
        Subscription->Type = Type;
        Subscription->Info = *Info;
        Subscription->Queued = 0;
        Call->Subscribed |= Bit;

        if (Call->Status & Bit)
        {
            Subscription->Queued++;
            Pending |= Bit;
        }
    }

    return Pending;
}

VOID
RpcpCallNotifyBegin(
    _Out_ RPCP_CALL_NOTIFY *CallNotify,
    _In_ RpcBinding *Binding)
{
    ZeroMemory(CallNotify, sizeof(*CallNotify));
    CallNotify->Binding = Binding;
    CallNotify->ThreadId = GetCurrentThreadId();

    EnterCriticalSection(&RpcpNotifyLock);
    list_add_head(&RpcpActiveCalls, &CallNotify->Entry);
    LeaveCriticalSection(&RpcpNotifyLock);
}

VOID
RpcpCallNotifyEnd(
    _Inout_ RPCP_CALL_NOTIFY *CallNotify)
{
    EnterCriticalSection(&RpcpNotifyLock);
    list_remove(&CallNotify->Entry);
    LeaveCriticalSection(&RpcpNotifyLock);

    if (CallNotify->Thread)
        CloseHandle(CallNotify->Thread);
}

VOID
RpcpCallNotifySignal(
    _In_opt_ RpcBinding *Binding,
    _In_ RPC_NOTIFICATIONS Notification)
{
    RPCP_CALL_NOTIFY *Call;
    RPCP_NOTIFY_SUBSCRIPTION Callback;
    BOOL Found;

    if (!Binding)
        return;

    TRACE("(%p, %u)\n", Binding, Notification);

    EnterCriticalSection(&RpcpNotifyLock);

    do
    {
        Found = FALSE;
        Callback.Type = RpcNotificationTypeNone;

        LIST_FOR_EACH_ENTRY(Call, &RpcpActiveCalls, RPCP_CALL_NOTIFY, Entry)
        {
            RPCP_NOTIFY_SUBSCRIPTION *Subscription;

            if (Call->Binding != Binding || (Call->Status & Notification))
                continue;

            Found = TRUE;
            Call->Status |= Notification;
            if (!(Call->Subscribed & Notification))
                break;

            Subscription = &Call->Subscription[RpcpNotifyIndex(Notification)];
            Subscription->Queued++;

            if (Subscription->Type == RpcNotificationTypeCallback)
                Callback = *Subscription;
            else
                RpcpIssueLocked(Call, Subscription, RpcpNotifyEvent(Notification));
            break;
        }

        if (Callback.Type == RpcNotificationTypeCallback)
        {
            LeaveCriticalSection(&RpcpNotifyLock);
            Callback.Info.NotificationRoutine((PRPC_ASYNC_STATE)Binding, NULL,
                                              RpcpNotifyEvent(Notification));
            EnterCriticalSection(&RpcpNotifyLock);
        }
    } while (Found);

    LeaveCriticalSection(&RpcpNotifyLock);
}

/***********************************************************************
 *             RpcServerSubscribeForNotification (RPCRT4.@)
 */
RPC_STATUS
RPC_ENTRY
RpcServerSubscribeForNotification(
    _In_opt_ RPC_BINDING_HANDLE Binding,
    _In_ RPC_NOTIFICATIONS Notification,
    _In_ RPC_NOTIFICATION_TYPES NotificationType,
    _In_ RPC_ASYNC_NOTIFICATION_INFO *NotificationInfo)
{
    RPCP_CALL_NOTIFY *Call;
    PFN_RPCNOTIFICATION_ROUTINE Routines[2];
    RPC_ASYNC_EVENT Events[2];
    RpcBinding *CallBinding;
    RPC_STATUS Status;
    ULONG Pending;
    ULONG Bit;
    ULONG Count = 0;
    ULONG Index;

    TRACE("(%p, %u, %u, %p)\n", Binding, Notification, NotificationType, NotificationInfo);

    switch (NotificationType)
    {
        case RpcNotificationTypeEvent:
        case RpcNotificationTypeApc:
        case RpcNotificationTypeIoc:
        case RpcNotificationTypeCallback:
            break;

        default:
            return ERROR_INVALID_PARAMETER;
    }

    if (Notification < RpcNotificationClientDisconnect || Notification > RPCP_NOTIFY_ALL)
        return RPC_S_CANNOT_SUPPORT;

    Status = RpcpResolveCallBinding(Binding, &CallBinding);
    if (Status != RPC_S_OK)
        return Status;

    if (!NotificationInfo)
        return ERROR_INVALID_PARAMETER;

    EnterCriticalSection(&RpcpNotifyLock);

    Call = RpcpFindCall(CallBinding);
    if (!Call)
    {
        Status = RPC_S_NO_CALL_ACTIVE;
        goto Exit;
    }

    /* One event cannot serve both notifications */
    if (NotificationType == RpcNotificationTypeEvent)
    {
        if ((Notification & RPCP_NOTIFY_ALL) == RPCP_NOTIFY_ALL)
        {
            Status = ERROR_INVALID_PARAMETER;
            goto Exit;
        }

        for (Bit = RpcNotificationClientDisconnect; Bit <= RpcNotificationCallCancel; Bit <<= 1)
        {
            const RPCP_NOTIFY_SUBSCRIPTION *Other;
            ULONG OtherBit = Bit ^ RPCP_NOTIFY_ALL;

            if (!(Notification & Bit) || !(Call->Subscribed & OtherBit))
                continue;

            Other = &Call->Subscription[RpcpNotifyIndex(OtherBit)];
            if (Other->Info.hEvent == NotificationInfo->hEvent)
            {
                Status = ERROR_INVALID_PARAMETER;
                goto Exit;
            }
        }
    }

    if (Call->Subscribed & Notification)
    {
        Status = RPC_S_CANNOT_SUPPORT;
        goto Exit;
    }

    /* APCs without a target thread go to the thread running the call */
    if (NotificationType == RpcNotificationTypeApc &&
        !NotificationInfo->APC.hThread &&
        !Call->Thread)
    {
        Call->Thread = OpenThread(THREAD_SET_CONTEXT, FALSE, Call->ThreadId);
        if (!Call->Thread)
        {
            Status = ERROR_OUTOFMEMORY;
            goto Exit;
        }
    }

    Pending = RpcpAddSubscription(Call, Notification, NotificationType, NotificationInfo);

    for (Bit = RpcNotificationClientDisconnect; Bit <= RpcNotificationCallCancel; Bit <<= 1)
    {
        const RPCP_NOTIFY_SUBSCRIPTION *Subscription;

        if (!(Pending & Bit))
            continue;

        Subscription = &Call->Subscription[RpcpNotifyIndex(Bit)];
        if (NotificationType == RpcNotificationTypeCallback)
        {
            Routines[Count] = Subscription->Info.NotificationRoutine;
            Events[Count] = RpcpNotifyEvent(Bit);
            Count++;
        }
        else
            RpcpIssueLocked(Call, Subscription, RpcpNotifyEvent(Bit));
    }

    Status = RPC_S_OK;

Exit:
    LeaveCriticalSection(&RpcpNotifyLock);

    for (Index = 0; Index < Count; Index++)
        Routines[Index]((PRPC_ASYNC_STATE)CallBinding, NULL, Events[Index]);

    return Status;
}

/***********************************************************************
 *             RpcServerUnsubscribeForNotification (RPCRT4.@)
 */
RPC_STATUS
RPC_ENTRY
RpcServerUnsubscribeForNotification(
    _In_opt_ RPC_BINDING_HANDLE Binding,
    _In_ RPC_NOTIFICATIONS Notification,
    _Out_ ULONG *NotificationsQueued)
{
    RPCP_CALL_NOTIFY *Call;
    RpcBinding *CallBinding;
    RPC_STATUS Status;

    TRACE("(%p, %u, %p)\n", Binding, Notification, NotificationsQueued);

    if (Notification < RpcNotificationClientDisconnect || Notification > RPCP_NOTIFY_ALL)
        return RPC_S_CANNOT_SUPPORT;

    Status = RpcpResolveCallBinding(Binding, &CallBinding);
    if (Status != RPC_S_OK)
        return Status;

    EnterCriticalSection(&RpcpNotifyLock);

    Call = RpcpFindCall(CallBinding);
    if (!Call || !(Call->Subscribed & Notification))
    {
        Status = RPC_S_CANNOT_SUPPORT;
    }
    else if (Notification == RPCP_NOTIFY_ALL)
    {
        Status = ERROR_INVALID_PARAMETER;
    }
    else
    {
        Call->Subscribed &= ~Notification;
        if (NotificationsQueued)
            *NotificationsQueued = Call->Subscription[RpcpNotifyIndex(Notification)].Queued;
    }

    LeaveCriticalSection(&RpcpNotifyLock);
    return Status;
}
