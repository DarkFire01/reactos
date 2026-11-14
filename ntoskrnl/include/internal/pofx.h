/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Internal header for the Power Manager Framework (PoFx)
 * COPYRIGHT:   Copyright 2023 George Bișoc <george.bisoc@reactos.org>
 *              Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

//
// Define this if you want debugging support
//
#define _POFX_DEBUG_                                    0x00

//
// These define the Debug Masks Supported
//
#define POFX_DEVICE_DEBUG                               0x01
#define POFX_COMPONENT_DEBUG                            0x02
#define POFX_PEP_DEBUG                                  0x04
#define POFX_DEPENDENT_DEBUG                            0x06
#define POFX_CALLBACKS_DEBUG                            0x08
#define POFX_IDLE_STATE_DEBUG                           0x10

//
// Debug/Tracing support
//
#if _POFX_DEBUG_
#ifdef NEW_DEBUG_SYSTEM_IMPLEMENTED // enable when Debug Filters are implemented
#define POFXTRACE DbgPrintEx
#else
#define POFXTRACE(x, ...)                                 \
    if (x & PopFxTraceLevel) DbgPrint(__VA_ARGS__)
#endif
#else
#define POFXTRACE(x, fmt, ...) DPRINT(fmt, ##__VA_ARGS__)
#endif

/* Work items a plug-in may have running at once when it allows concurrency */
#define POP_FX_PLUGIN_WORKERS                           4

/* The device registered with only the directed power callbacks */
#define POP_FX_DEVICE_DIRECTED_ONLY                     0x40

/* No F-state transition is in flight */
#define POP_FX_NO_TRANSITION                            MAXULONG

struct _POP_FX_PLUGIN;

typedef struct _POP_FX_PLUGIN_WORKER
{
    WORK_QUEUE_ITEM WorkItem;
    struct _POP_FX_PLUGIN *Plugin;
    LONG Slot;
} POP_FX_PLUGIN_WORKER, *PPOP_FX_PLUGIN_WORKER;

//
// PoFx power engine plug-in (PEP)
//
typedef struct _POP_FX_PLUGIN
{
    LIST_ENTRY Link;
    ULONG Version;
    ULONGLONG Flags;
    PPEPCALLBACKNOTIFYDPM AcceptDeviceNotification;
    PPEPCALLBACKNOTIFYPPM AcceptProcessorNotification;
    PPEPCALLBACKNOTIFYACPI AcceptAcpiNotification;
    volatile LONG WorkRequests;
    volatile LONG WorkersBusy;
    POP_FX_PLUGIN_WORKER Workers[POP_FX_PLUGIN_WORKERS];
} POP_FX_PLUGIN, *PPOP_FX_PLUGIN;

//
// A device a plug-in claimed with PEP_DPM_PREPARE_DEVICE
//
typedef struct _POP_FX_PREPARED_DEVICE
{
    LIST_ENTRY Link;
    PDEVICE_OBJECT Pdo;
    PPOP_FX_PLUGIN Plugin;
    UNICODE_STRING DeviceId;
} POP_FX_PREPARED_DEVICE, *PPOP_FX_PREPARED_DEVICE;

struct _POP_FX_DEVICE;

//
// PoFx component
//
typedef struct _POP_FX_COMPONENT
{
    struct _POP_FX_DEVICE *Device;
    ULONG Index;
    GUID Id;
    ULONGLONG Flags;
    ULONG DeepestWakeableIdleState;
    ULONG IdleStateCount;
    PPO_FX_COMPONENT_IDLE_STATE IdleStates;
    PEP_COMPONENT_V2 PepComponent;

    /* Activation references, the component is in the active condition while nonzero */
    LONG ActiveCount;
    BOOLEAN Active;
    BOOLEAN IdleConditionPending;
    BOOLEAN DriverStatePending;
    BOOLEAN PepStatePending;
    ULONG FState;
    ULONG TargetFState;
    ULONGLONG Latency;
    ULONGLONG Residency;
    KEVENT ActiveEvent;

    /* Performance states */
    PPO_FX_COMPONENT_PERF_INFO PerfInfo;
    PPO_FX_COMPONENT_PERF_STATE_CALLBACK PerfStateCallback;
    PULONGLONG PerfCurrent;
    PVOID PerfRequestContext;
    BOOLEAN PerfRequestPending;
    BOOLEAN PerfSucceeded;
    KEVENT PerfEvent;
} POP_FX_COMPONENT, *PPOP_FX_COMPONENT;

//
// PoFx device
//
typedef struct _POP_FX_DEVICE
{
    LIST_ENTRY Link;
    ULONG Version;
    ULONGLONG Flags;
    PDEVICE_OBJECT Pdo;
    PPOP_FX_PLUGIN Plugin;
    PEPHANDLE PluginHandle;
    UNICODE_STRING DeviceId;
    KSPIN_LOCK Lock;

    PPO_FX_COMPONENT_ACTIVE_CONDITION_CALLBACK ComponentActiveConditionCallback;
    PPO_FX_COMPONENT_IDLE_CONDITION_CALLBACK ComponentIdleConditionCallback;
    PPO_FX_COMPONENT_IDLE_STATE_CALLBACK ComponentIdleStateCallback;
    PPO_FX_DEVICE_POWER_REQUIRED_CALLBACK DevicePowerRequiredCallback;
    PPO_FX_DEVICE_POWER_NOT_REQUIRED_CALLBACK DevicePowerNotRequiredCallback;
    PPO_FX_POWER_CONTROL_CALLBACK PowerControlCallback;
    PPO_FX_DIRECTED_POWER_UP_CALLBACK DirectedPowerUpCallback;
    PPO_FX_DIRECTED_POWER_DOWN_CALLBACK DirectedPowerDownCallback;
    ULONG DirectedFxTimeout;
    PVOID DriverContext;

    BOOLEAN Started;
    BOOLEAN PowerRequired;
    BOOLEAN PoweredOnPending;
    BOOLEAN PowerNotRequiredPending;
    BOOLEAN IdleTimerArmed;
    ULONGLONG IdleTimeout;
    KTIMER IdleTimer;
    KDPC IdleDpc;

    PPEPCALLBACKPOWERONCRASHDUMPDEVICE PowerOnDumpDeviceCallback;

    PPEP_DEVICE_REGISTER_V2 PepRegister;
    ULONG ComponentCount;
    POP_FX_COMPONENT Components[ANYSIZE_ARRAY];
} POP_FX_DEVICE, *PPOP_FX_DEVICE;

/* plugin.c */

PPOP_FX_PLUGIN
NTAPI
PopFxNextPlugin(
    _In_opt_ PPOP_FX_PLUGIN Plugin);

BOOLEAN
NTAPI
PopFxNotifyPlugin(
    _In_ PPOP_FX_PLUGIN Plugin,
    _In_ ULONG Notification,
    _Inout_ PVOID Data);

PPOP_FX_PLUGIN
NTAPI
PopFxFindPreparedDevice(
    _In_ PDEVICE_OBJECT Pdo,
    _Out_ PUNICODE_STRING DeviceId);

NTSTATUS
NTAPI
PopFxQueryDeviceId(
    _In_ PDEVICE_OBJECT Pdo,
    _Out_ PUNICODE_STRING DeviceId);

VOID
NTAPI
PopFxPrepareDevice(
    _In_ PDEVICE_OBJECT Pdo,
    _In_ BOOLEAN StartDevice);

VOID
NTAPI
PopFxAbandonDevice(
    _In_ PDEVICE_OBJECT Pdo);

/* fxapi.c */

PPOP_FX_DEVICE
NTAPI
PopFxFindDevice(
    _In_ PDEVICE_OBJECT DeviceObject);

VOID
NTAPI
PopFxNotifyDevicePowerState(
    _In_ PPOP_FX_DEVICE FxDevice,
    _In_ DEVICE_POWER_STATE PowerState,
    _In_ BOOLEAN Complete,
    _In_ BOOLEAN SystemTransition);

VOID
NTAPI
PopFxCompletePepIdleState(
    _In_ PPOP_FX_DEVICE FxDevice,
    _In_ ULONG Component);

VOID
NTAPI
PopFxCompletePepPerfState(
    _In_ PPOP_FX_DEVICE FxDevice,
    _In_ ULONG Component,
    _In_ BOOLEAN Succeeded);

VOID
NTAPI
PopFxRunPepPowerControl(
    _In_ PPOP_FX_DEVICE FxDevice,
    _In_ PPEP_WORK_POWER_CONTROL Request);

/* EOF */
