/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private view of the execution context object
 *
 * An execution context is the polling thread a set of queues share. A client
 * that supplies one to its packet queues gets its advance calls driven by it
 * instead of by the queue callbacks.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Return TRUE when there is more work to do right away. */
typedef
_Function_class_(EVT_NET_EXECUTION_CONTEXT_ADVANCE)
_IRQL_requires_same_
BOOLEAN
NTAPI
EVT_NET_EXECUTION_CONTEXT_ADVANCE(
    _In_ NETEXECUTIONCONTEXT ExecutionContext);

typedef EVT_NET_EXECUTION_CONTEXT_ADVANCE *PFN_NET_EXECUTION_CONTEXT_ADVANCE;

typedef
_Function_class_(EVT_NET_EXECUTION_CONTEXT_SET_NOTIFICATION_ENABLED)
_IRQL_requires_same_
VOID
NTAPI
EVT_NET_EXECUTION_CONTEXT_SET_NOTIFICATION_ENABLED(
    _In_ NETEXECUTIONCONTEXT ExecutionContext,
    _In_ BOOLEAN NotificationEnabled);

typedef EVT_NET_EXECUTION_CONTEXT_SET_NOTIFICATION_ENABLED
    *PFN_NET_EXECUTION_CONTEXT_SET_NOTIFICATION_ENABLED;

typedef struct _NET_EXECUTION_CONTEXT_CONFIG
{
    ULONG Size;
    PFN_NET_EXECUTION_CONTEXT_ADVANCE EvtPreAdvance;
    PFN_NET_EXECUTION_CONTEXT_ADVANCE EvtPostAdvance;
    PFN_NET_EXECUTION_CONTEXT_SET_NOTIFICATION_ENABLED EvtSetNotificationEnabled;
} NET_EXECUTION_CONTEXT_CONFIG;

FORCEINLINE
VOID
NTAPI
NET_EXECUTION_CONTEXT_CONFIG_INIT(
    _Out_ NET_EXECUTION_CONTEXT_CONFIG *Config)
{
    RtlZeroMemory(Config, sizeof(*Config));
    Config->Size = sizeof(*Config);
}

/* Runs on the execution context thread, serialized with its polling. */
typedef
_Function_class_(EVT_NET_EXECUTION_CONTEXT_TASK)
_IRQL_requires_same_
VOID
NTAPI
EVT_NET_EXECUTION_CONTEXT_TASK(
    _In_ NETEXECUTIONCONTEXTTASK Task);

typedef EVT_NET_EXECUTION_CONTEXT_TASK *PFN_NET_EXECUTION_CONTEXT_TASK;

typedef struct _NET_EXECUTION_CONTEXT_TASK_CONFIG
{
    ULONG Size;
    PFN_NET_EXECUTION_CONTEXT_TASK EvtTask;
} NET_EXECUTION_CONTEXT_TASK_CONFIG;

FORCEINLINE
VOID
NTAPI
NET_EXECUTION_CONTEXT_TASK_CONFIG_INIT(
    _Out_ NET_EXECUTION_CONTEXT_TASK_CONFIG *Config,
    _In_ PFN_NET_EXECUTION_CONTEXT_TASK EvtTask)
{
    RtlZeroMemory(Config, sizeof(*Config));
    Config->Size = sizeof(*Config);
    Config->EvtTask = EvtTask;
}

typedef
_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
(NTAPI *PFN_NETEXECUTIONCONTEXTCREATE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_ CONST NET_EXECUTION_CONTEXT_CONFIG *Config,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES ClientAttributes,
    _Out_ NETEXECUTIONCONTEXT *ExecutionContext);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetExecutionContextCreate(
    _In_ WDFDEVICE Device,
    _In_ CONST NET_EXECUTION_CONTEXT_CONFIG *Config,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES ClientAttributes,
    _Out_ NETEXECUTIONCONTEXT *ExecutionContext)
{
    return ((PFN_NETEXECUTIONCONTEXTCREATE)NetFunctions[NetExecutionContextCreateTableIndex])(
        NetDriverGlobals, Device, Config, ClientAttributes, ExecutionContext);
}

typedef
_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
(NTAPI *PFN_NETEXECUTIONCONTEXTTASKCREATE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETEXECUTIONCONTEXT ExecutionContext,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES ClientAttributes,
    _In_ NET_EXECUTION_CONTEXT_TASK_CONFIG *TaskConfig,
    _Out_ NETEXECUTIONCONTEXTTASK *Task);

_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetExecutionContextTaskCreate(
    _In_ NETEXECUTIONCONTEXT ExecutionContext,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES ClientAttributes,
    _In_ NET_EXECUTION_CONTEXT_TASK_CONFIG *TaskConfig,
    _Out_ NETEXECUTIONCONTEXTTASK *Task)
{
    return ((PFN_NETEXECUTIONCONTEXTTASKCREATE)NetFunctions[NetExecutionContextTaskCreateTableIndex])(
        NetDriverGlobals, ExecutionContext, ClientAttributes, TaskConfig, Task);
}

typedef
_IRQL_requires_max_(HIGH_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETEXECUTIONCONTEXTNOTIFY)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETEXECUTIONCONTEXT ExecutionContext);

_IRQL_requires_max_(HIGH_LEVEL)
FORCEINLINE
VOID
NTAPI
NetExecutionContextNotify(
    _In_ NETEXECUTIONCONTEXT ExecutionContext)
{
    ((PFN_NETEXECUTIONCONTEXTNOTIFY)NetFunctions[NetExecutionContextNotifyTableIndex])(
        NetDriverGlobals, ExecutionContext);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETEXECUTIONCONTEXTTASKENQUEUE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETEXECUTIONCONTEXTTASK Task);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
VOID
NTAPI
NetExecutionContextTaskEnqueue(
    _In_ NETEXECUTIONCONTEXTTASK Task)
{
    ((PFN_NETEXECUTIONCONTEXTTASKENQUEUE)NetFunctions[NetExecutionContextTaskEnqueueTableIndex])(
        NetDriverGlobals, Task);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETEXECUTIONCONTEXTTASKWAITCOMPLETION)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ NETEXECUTIONCONTEXTTASK Task);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetExecutionContextTaskWaitCompletion(
    _Inout_ NETEXECUTIONCONTEXTTASK Task)
{
    ((PFN_NETEXECUTIONCONTEXTTASKWAITCOMPLETION)NetFunctions[NetExecutionContextTaskWaitCompletionTableIndex])(
        NetDriverGlobals, Task);
}

#ifdef __cplusplus
}
#endif
