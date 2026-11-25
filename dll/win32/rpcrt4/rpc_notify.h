/*
 * PROJECT:     ReactOS RPC runtime
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Server call notifications (client disconnect and call cancel)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef __RPC_NOTIFY_H
#define __RPC_NOTIFY_H

#include "rpcasync.h"
#include "wine/list.h"

struct _RpcBinding;

typedef struct _RPCP_NOTIFY_SUBSCRIPTION
{
    RPC_NOTIFICATION_TYPES Type;
    RPC_ASYNC_NOTIFICATION_INFO Info;
    ULONG Queued;
} RPCP_NOTIFY_SUBSCRIPTION;

/* Notification state of one server call, valid while the call is dispatched */
typedef struct _RPCP_CALL_NOTIFY
{
    struct list Entry;
    struct _RpcBinding *Binding;
    DWORD ThreadId;
    HANDLE Thread;
    ULONG Subscribed;
    ULONG Status;
    RPCP_NOTIFY_SUBSCRIPTION Subscription[2];
} RPCP_CALL_NOTIFY;

VOID
RpcpCallNotifyBegin(
    _Out_ RPCP_CALL_NOTIFY *CallNotify,
    _In_ struct _RpcBinding *Binding);

VOID
RpcpCallNotifyEnd(
    _Inout_ RPCP_CALL_NOTIFY *CallNotify);

VOID
RpcpCallNotifySignal(
    _In_opt_ struct _RpcBinding *Binding,
    _In_ RPC_NOTIFICATIONS Notification);

#endif /* __RPC_NOTIFY_H */
