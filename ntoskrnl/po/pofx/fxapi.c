/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Power Manager Framework API (PoFx) support routines
 * COPYRIGHT:   Copyright 2023 George Bișoc <george.bisoc@reactos.org>
 *              Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* PRIVATE FUNCTIONS **********************************************************/

typedef enum _POP_FX_ACTION
{
    PopFxActionNone,
    PopFxActionPowerRequired,
    PopFxActionPepState,
    PopFxActionDriverState,
    PopFxActionActive,
    PopFxActionIdle
} POP_FX_ACTION;

typedef struct _POP_FX_ADVANCE_WORK
{
    WORK_QUEUE_ITEM WorkItem;
    PPOP_FX_COMPONENT Component;
} POP_FX_ADVANCE_WORK, *PPOP_FX_ADVANCE_WORK;

static
VOID
PopFxAdvanceComponent(
    _In_ PPOP_FX_COMPONENT Component);

/**
 * @brief Picks the deepest idle state the component's latency and residency hints allow.
 */
static
ULONG
PopFxSelectIdleState(
    _In_ PPOP_FX_COMPONENT Component)
{
    ULONG State;

    for (State = Component->IdleStateCount - 1; State > 0; State--)
    {
        if (Component->IdleStates[State].TransitionLatency <= Component->Latency &&
            Component->IdleStates[State].ResidencyRequirement <= Component->Residency)
        {
            return State;
        }
    }

    return 0;
}

/**
 * @brief Decides the next step of a component, called with the device lock held.
 */
static
POP_FX_ACTION
PopFxNextAction(
    _In_ PPOP_FX_COMPONENT Component)
{
    PPOP_FX_DEVICE Device = Component->Device;
    ULONG Target;

    if (Component->IdleConditionPending || Component->DriverStatePending || Component->PepStatePending)
        return PopFxActionNone;

    if (Component->ActiveCount > 0)
    {
        if (Component->Active)
            return PopFxActionNone;

        /* The device has to be back in D0 before any component may leave its idle state */
        if (Device->PowerNotRequiredPending || Device->PoweredOnPending)
            return PopFxActionNone;
        if (!Device->PowerRequired)
        {
            Device->PowerRequired = TRUE;
            Device->PoweredOnPending = (Device->DevicePowerRequiredCallback != NULL);
            return PopFxActionPowerRequired;
        }

        /* Raising power, the plug-in turns the hardware on before the driver is told */
        if (Component->FState != 0)
        {
            Component->TargetFState = 0;
            Component->PepStatePending = TRUE;
            return PopFxActionPepState;
        }

        Component->Active = TRUE;
        return PopFxActionActive;
    }

    if (Component->Active)
    {
        Component->Active = FALSE;
        Component->IdleConditionPending = TRUE;
        return PopFxActionIdle;
    }

    Target = PopFxSelectIdleState(Component);
    if (Target == Component->FState)
        return PopFxActionNone;

    Component->TargetFState = Target;
    if (Target > Component->FState)
    {
        /* Lowering power, the driver quiesces before the plug-in cuts anything */
        Component->DriverStatePending = TRUE;
        return PopFxActionDriverState;
    }

    Component->PepStatePending = TRUE;
    return PopFxActionPepState;
}

/**
 * @brief Arms the device idle timer once every component of a started device is idle.
 */
static
VOID
PopFxCheckDeviceIdle(
    _In_ PPOP_FX_DEVICE Device)
{
    LARGE_INTEGER DueTime;
    KIRQL OldIrql;
    ULONG Index;

    KeAcquireSpinLock(&Device->Lock, &OldIrql);

    if (!Device->Started || !Device->PowerRequired || Device->PowerNotRequiredPending ||
        Device->IdleTimerArmed || !Device->IdleTimeout || !Device->DevicePowerNotRequiredCallback)
    {
        KeReleaseSpinLock(&Device->Lock, OldIrql);
        return;
    }

    for (Index = 0; Index < Device->ComponentCount; Index++)
    {
        PPOP_FX_COMPONENT Component = &Device->Components[Index];

        if (Component->ActiveCount || Component->Active || Component->IdleConditionPending)
        {
            KeReleaseSpinLock(&Device->Lock, OldIrql);
            return;
        }
    }

    Device->IdleTimerArmed = TRUE;
    DueTime.QuadPart = -(LONGLONG)Device->IdleTimeout;
    KeSetTimer(&Device->IdleTimer, DueTime, &Device->IdleDpc);

    KeReleaseSpinLock(&Device->Lock, OldIrql);
}

/**
 * @brief Fires when every component stayed idle for the device idle timeout.
 */
static
VOID
NTAPI
PopFxDeviceIdleTimerDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PPOP_FX_DEVICE Device = DeferredContext;
    ULONG Index;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    KeAcquireSpinLockAtDpcLevel(&Device->Lock);

    if (!Device->IdleTimerArmed)
    {
        KeReleaseSpinLockFromDpcLevel(&Device->Lock);
        return;
    }
    Device->IdleTimerArmed = FALSE;

    for (Index = 0; Index < Device->ComponentCount; Index++)
    {
        if (Device->Components[Index].ActiveCount || Device->Components[Index].Active)
        {
            KeReleaseSpinLockFromDpcLevel(&Device->Lock);
            return;
        }
    }

    Device->PowerRequired = FALSE;
    Device->PowerNotRequiredPending = TRUE;
    KeReleaseSpinLockFromDpcLevel(&Device->Lock);

    Device->DevicePowerNotRequiredCallback(Device->DriverContext);
}

/**
 * @brief Tells the plug-in about the F-state transition in flight.
 */
static
VOID
PopFxNotifyPepState(
    _In_ PPOP_FX_COMPONENT Component)
{
    PPOP_FX_DEVICE Device = Component->Device;
    PEP_NOTIFY_COMPONENT_IDLE_STATE Notify;

    Notify.DeviceHandle = Device->PluginHandle;
    Notify.Component = Component->Index;
    Notify.IdleState = Component->TargetFState;
    Notify.DriverNotified = (Component->TargetFState > Component->FState);
    Notify.Completed = FALSE;

    /* A plug-in that does not complete inline answers with PepWorkCompleteIdleState */
    if (Device->Plugin &&
        PopFxNotifyPlugin(Device->Plugin, PEP_DPM_NOTIFY_COMPONENT_IDLE_STATE, &Notify) &&
        !Notify.Completed)
    {
        return;
    }

    PopFxCompletePepIdleState(Device, Component->Index);
}

/**
 * @brief Moves the component toward what its activation count asks for, one step at a time.
 *
 * @remarks Callbacks run without the device lock, drivers may complete from inside them.
 */
static
VOID
PopFxAdvanceComponent(
    _In_ PPOP_FX_COMPONENT Component)
{
    PPOP_FX_DEVICE Device = Component->Device;
    POP_FX_ACTION Action;
    KIRQL OldIrql;

    for (;;)
    {
        KeAcquireSpinLock(&Device->Lock, &OldIrql);
        Action = PopFxNextAction(Component);
        if (Action == PopFxActionActive)
            KeSetEvent(&Component->ActiveEvent, IO_NO_INCREMENT, FALSE);
        else if (Action == PopFxActionIdle)
            KeClearEvent(&Component->ActiveEvent);
        KeReleaseSpinLock(&Device->Lock, OldIrql);

        switch (Action)
        {
            case PopFxActionNone:
                PopFxCheckDeviceIdle(Device);
                return;

            case PopFxActionPowerRequired:
                if (Device->DevicePowerRequiredCallback)
                    Device->DevicePowerRequiredCallback(Device->DriverContext);
                break;

            case PopFxActionPepState:
                PopFxNotifyPepState(Component);
                return;

            case PopFxActionDriverState:
                if (Device->ComponentIdleStateCallback)
                {
                    Device->ComponentIdleStateCallback(Device->DriverContext,
                                                       Component->Index,
                                                       Component->TargetFState);
                    return;
                }
                PoFxCompleteIdleState((POHANDLE)Device, Component->Index);
                return;

            case PopFxActionActive:
                if (Device->ComponentActiveConditionCallback)
                    Device->ComponentActiveConditionCallback(Device->DriverContext, Component->Index);
                break;

            case PopFxActionIdle:
                if (Device->ComponentIdleConditionCallback)
                {
                    Device->ComponentIdleConditionCallback(Device->DriverContext, Component->Index);
                    return;
                }
                PoFxCompleteIdleCondition((POHANDLE)Device, Component->Index);
                return;
        }
    }
}

static
VOID
NTAPI
PopFxAdvanceWorker(
    _In_ PVOID Parameter)
{
    PPOP_FX_ADVANCE_WORK Work = Parameter;

    PopFxAdvanceComponent(Work->Component);
    ExFreePoolWithTag(Work, TAG_PO_FX_WORK_ORDER);
}

/**
 * @brief Advances a component now, or from a worker when the caller cannot take callbacks inline.
 */
static
VOID
PopFxKickComponent(
    _In_ PPOP_FX_COMPONENT Component,
    _In_ ULONG Flags)
{
    PPOP_FX_ADVANCE_WORK Work;

    if (Flags & PO_FX_FLAG_ASYNC_ONLY)
    {
        Work = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Work), TAG_PO_FX_WORK_ORDER);
        if (Work)
        {
            Work->Component = Component;
            ExInitializeWorkItem(&Work->WorkItem, PopFxAdvanceWorker, Work);
            ExQueueWorkItem(&Work->WorkItem, CriticalWorkQueue);
            return;
        }
    }

    PopFxAdvanceComponent(Component);
}

static
VOID
PopFxKickAllComponents(
    _In_ PPOP_FX_DEVICE Device)
{
    ULONG Index;

    for (Index = 0; Index < Device->ComponentCount; Index++)
        PopFxAdvanceComponent(&Device->Components[Index]);
}

/**
 * @brief Checks a component index against the device it is used with.
 */
static
PPOP_FX_COMPONENT
PopFxGetComponent(
    _In_opt_ POHANDLE Handle,
    _In_ ULONG Component)
{
    PPOP_FX_DEVICE Device = (PPOP_FX_DEVICE)Handle;

    if (!Device || Component >= Device->ComponentCount)
        return NULL;

    return &Device->Components[Component];
}

/* INTERNAL FUNCTIONS *********************************************************/

/**
 * @brief Finds the PoFx registration of the stack a device object belongs to.
 */
PPOP_FX_DEVICE
NTAPI
PopFxFindDevice(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PPOP_FX_DEVICE Found = NULL;
    KLOCK_QUEUE_HANDLE LockHandle;
    PDEVICE_OBJECT Pdo;
    PLIST_ENTRY Entry;

    Pdo = IoGetDeviceAttachmentBaseRef(DeviceObject);

    KeAcquireInStackQueuedSpinLock(&PopFxDeviceLock, &LockHandle);
    for (Entry = PopFxDeviceList.Flink; Entry != &PopFxDeviceList; Entry = Entry->Flink)
    {
        PPOP_FX_DEVICE Device = CONTAINING_RECORD(Entry, POP_FX_DEVICE, Link);

        if (Device->Pdo == Pdo)
        {
            Found = Device;
            break;
        }
    }
    KeReleaseInStackQueuedSpinLock(&LockHandle);

    ObDereferenceObject(Pdo);
    return Found;
}

/**
 * @brief Tells the plug-in owning a device that a D-IRP is starting or has finished.
 */
VOID
NTAPI
PopFxNotifyDevicePowerState(
    _In_ PPOP_FX_DEVICE FxDevice,
    _In_ DEVICE_POWER_STATE PowerState,
    _In_ BOOLEAN Complete,
    _In_ BOOLEAN SystemTransition)
{
    PEP_DEVICE_POWER_STATE Notify;

    if (!FxDevice->Plugin)
        return;

    Notify.DeviceHandle = FxDevice->PluginHandle;
    Notify.PowerState = PowerState;
    Notify.Complete = Complete;
    Notify.SystemTransition = SystemTransition;
    PopFxNotifyPlugin(FxDevice->Plugin, PEP_DPM_DEVICE_POWER_STATE, &Notify);
}

/**
 * @brief The plug-in finished its part of an F-state transition.
 */
VOID
NTAPI
PopFxCompletePepIdleState(
    _In_ PPOP_FX_DEVICE FxDevice,
    _In_ ULONG Component)
{
    PPOP_FX_COMPONENT FxComponent;
    BOOLEAN Raising;
    KIRQL OldIrql;

    FxComponent = PopFxGetComponent((POHANDLE)FxDevice, Component);
    if (!FxComponent)
        return;

    KeAcquireSpinLock(&FxDevice->Lock, &OldIrql);
    if (!FxComponent->PepStatePending)
    {
        KeReleaseSpinLock(&FxDevice->Lock, OldIrql);
        DPRINT1("PEP completed an idle state for %p/%lu with none in flight\n", FxDevice, Component);
        return;
    }

    FxComponent->PepStatePending = FALSE;
    Raising = (FxComponent->TargetFState < FxComponent->FState);
    if (Raising)
    {
        /* The hardware is on, now the driver restores the component */
        FxComponent->DriverStatePending = TRUE;
    }
    else
    {
        FxComponent->FState = FxComponent->TargetFState;
        FxComponent->TargetFState = POP_FX_NO_TRANSITION;
    }
    KeReleaseSpinLock(&FxDevice->Lock, OldIrql);

    if (Raising && FxDevice->ComponentIdleStateCallback)
    {
        FxDevice->ComponentIdleStateCallback(FxDevice->DriverContext, Component, FxComponent->TargetFState);
        return;
    }
    if (Raising)
    {
        PoFxCompleteIdleState((POHANDLE)FxDevice, Component);
        return;
    }

    PopFxAdvanceComponent(FxComponent);
}

/**
 * @brief The plug-in finished a perf state change it did not complete inline.
 */
VOID
NTAPI
PopFxCompletePepPerfState(
    _In_ PPOP_FX_DEVICE FxDevice,
    _In_ ULONG Component,
    _In_ BOOLEAN Succeeded)
{
    PPOP_FX_COMPONENT FxComponent;
    PVOID RequestContext;
    KIRQL OldIrql;

    FxComponent = PopFxGetComponent((POHANDLE)FxDevice, Component);
    if (!FxComponent)
        return;

    KeAcquireSpinLock(&FxDevice->Lock, &OldIrql);
    if (!FxComponent->PerfRequestPending)
    {
        KeReleaseSpinLock(&FxDevice->Lock, OldIrql);
        return;
    }
    FxComponent->PerfRequestPending = FALSE;
    FxComponent->PerfSucceeded = Succeeded;
    RequestContext = FxComponent->PerfRequestContext;
    KeReleaseSpinLock(&FxDevice->Lock, OldIrql);

    if (FxComponent->PerfStateCallback)
        FxComponent->PerfStateCallback(FxDevice->DriverContext, Component, Succeeded, RequestContext);
    KeSetEvent(&FxComponent->PerfEvent, IO_NO_INCREMENT, FALSE);
}

/**
 * @brief Runs a power control the plug-in sends to the driver, then hands the result back.
 */
VOID
NTAPI
PopFxRunPepPowerControl(
    _In_ PPOP_FX_DEVICE FxDevice,
    _In_ PPEP_WORK_POWER_CONTROL Request)
{
    PEP_POWER_CONTROL_COMPLETE Complete;
    SIZE_T BytesReturned = 0;
    NTSTATUS Status = STATUS_NOT_SUPPORTED;

    if (FxDevice->PowerControlCallback)
    {
        Status = FxDevice->PowerControlCallback(FxDevice->DriverContext,
                                                Request->PowerControlCode,
                                                Request->InBuffer,
                                                Request->InBufferSize,
                                                Request->OutBuffer,
                                                Request->OutBufferSize,
                                                &BytesReturned);
    }

    Complete.DeviceHandle = FxDevice->PluginHandle;
    Complete.PowerControlCode = Request->PowerControlCode;
    Complete.RequestContext = Request->RequestContext;
    Complete.BytesReturned = BytesReturned;
    Complete.Status = Status;
    PopFxNotifyPlugin(FxDevice->Plugin, PEP_DPM_POWER_CONTROL_COMPLETE, &Complete);
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Registers a device with the Power Framework.
 *
 * @param[in] Pdo
 * The physical device object of the stack.
 *
 * @param[in] Device
 * A PO_FX_DEVICE_V1, V2 or V3 descriptor.
 *
 * @param[out] Handle
 * Receives the registration handle.
 *
 * @return
 * STATUS_INVALID_PARAMETER for a bad descriptor, STATUS_INSUFFICIENT_RESOURCES
 * when the registration cannot be allocated.
 *
 * @remarks
 * Every component starts active in F0 with one activation reference, which
 * PoFxStartDevicePowerManagement drops.
 */
NTSTATUS
NTAPI
PoFxRegisterDevice(
    _In_ PDEVICE_OBJECT Pdo,
    _In_ PPO_FX_DEVICE Device,
    _Out_ POHANDLE *Handle)
{
    PPO_FX_DEVICE_V1 DeviceV1 = (PPO_FX_DEVICE_V1)Device;
    PPO_FX_DEVICE_V2 DeviceV2 = (PPO_FX_DEVICE_V2)Device;
    PPO_FX_DEVICE_V3 DeviceV3 = (PPO_FX_DEVICE_V3)Device;
    PEP_REGISTER_DEVICE_V2 Register;
    KLOCK_QUEUE_HANDLE LockHandle;
    PPO_FX_COMPONENT_IDLE_STATE IdleStates;
    PPOP_FX_COMPONENT FxComponent;
    PPOP_FX_DEVICE FxDevice;
    PPOP_FX_PLUGIN Plugin, Candidate;
    ULONG ComponentCount, Index, StateCount = 0;
    SIZE_T Size, RegisterOffset, StatesOffset;

    PAGED_CODE();

    if (!Pdo || !Device || !Handle)
        return STATUS_INVALID_PARAMETER;

    switch (DeviceV1->Version)
    {
        case PO_FX_VERSION_V1:
            ComponentCount = DeviceV1->ComponentCount;
            break;

        case PO_FX_VERSION_V2:
            if (DeviceV2->Flags & POP_FX_DEVICE_DIRECTED_ONLY)
                return STATUS_INVALID_PARAMETER;
            ComponentCount = DeviceV2->ComponentCount;
            break;

        case PO_FX_VERSION_V3:
            if (DeviceV3->Flags & POP_FX_DEVICE_DIRECTED_ONLY)
                return STATUS_INVALID_PARAMETER;
            /* Directed power up and down come as a pair */
            if (!DeviceV3->DirectedPowerUpCallback != !DeviceV3->DirectedPowerDownCallback)
                return STATUS_INVALID_PARAMETER;
            ComponentCount = DeviceV3->ComponentCount;
            break;

        default:
            return STATUS_INVALID_PARAMETER;
    }

    if (!ComponentCount)
        return STATUS_INVALID_PARAMETER;

    /* Every component needs at least F0 */
    for (Index = 0; Index < ComponentCount; Index++)
    {
        ULONG Count;
        PVOID States;

        if (DeviceV1->Version == PO_FX_VERSION_V1)
        {
            Count = DeviceV1->Components[Index].IdleStateCount;
            States = DeviceV1->Components[Index].IdleStates;
        }
        else
        {
            PPO_FX_COMPONENT_V2 Components = (DeviceV1->Version == PO_FX_VERSION_V2) ?
                                             DeviceV2->Components : DeviceV3->Components;
            Count = Components[Index].IdleStateCount;
            States = Components[Index].IdleStates;
        }

        if (!Count || !States)
            return STATUS_INVALID_PARAMETER;
        StateCount += Count;
    }

    /* One block holds the device, its components, the plug-in register copy and the idle states */
    RegisterOffset = ALIGN_UP_BY(FIELD_OFFSET(POP_FX_DEVICE, Components[ComponentCount]), sizeof(PVOID));
    StatesOffset = ALIGN_UP_BY(RegisterOffset +
                               FIELD_OFFSET(PEP_DEVICE_REGISTER_V2, Components[ComponentCount]),
                               sizeof(ULONGLONG));
    Size = StatesOffset + StateCount * sizeof(PO_FX_COMPONENT_IDLE_STATE);

    FxDevice = ExAllocatePoolZero(NonPagedPool, Size, TAG_PO_FX_DEVICE);
    if (!FxDevice)
        return STATUS_INSUFFICIENT_RESOURCES;

    FxDevice->Version = DeviceV1->Version;
    FxDevice->Pdo = Pdo;
    FxDevice->ComponentCount = ComponentCount;
    FxDevice->PepRegister = (PPEP_DEVICE_REGISTER_V2)((PUCHAR)FxDevice + RegisterOffset);
    FxDevice->PowerRequired = TRUE;
    KeInitializeSpinLock(&FxDevice->Lock);
    KeInitializeTimerEx(&FxDevice->IdleTimer, NotificationTimer);
    KeInitializeDpc(&FxDevice->IdleDpc, PopFxDeviceIdleTimerDpc, FxDevice);

    if (DeviceV1->Version == PO_FX_VERSION_V1)
    {
        FxDevice->ComponentActiveConditionCallback = DeviceV1->ComponentActiveConditionCallback;
        FxDevice->ComponentIdleConditionCallback = DeviceV1->ComponentIdleConditionCallback;
        FxDevice->ComponentIdleStateCallback = DeviceV1->ComponentIdleStateCallback;
        FxDevice->DevicePowerRequiredCallback = DeviceV1->DevicePowerRequiredCallback;
        FxDevice->DevicePowerNotRequiredCallback = DeviceV1->DevicePowerNotRequiredCallback;
        FxDevice->PowerControlCallback = DeviceV1->PowerControlCallback;
        FxDevice->DriverContext = DeviceV1->DeviceContext;
    }
    else
    {
        /* V2 and V3 share their layout up to the device context */
        FxDevice->Flags = DeviceV2->Flags;
        FxDevice->ComponentActiveConditionCallback = DeviceV2->ComponentActiveConditionCallback;
        FxDevice->ComponentIdleConditionCallback = DeviceV2->ComponentIdleConditionCallback;
        FxDevice->ComponentIdleStateCallback = DeviceV2->ComponentIdleStateCallback;
        FxDevice->DevicePowerRequiredCallback = DeviceV2->DevicePowerRequiredCallback;
        FxDevice->DevicePowerNotRequiredCallback = DeviceV2->DevicePowerNotRequiredCallback;
        FxDevice->PowerControlCallback = DeviceV2->PowerControlCallback;

        if (DeviceV1->Version == PO_FX_VERSION_V2)
        {
            FxDevice->DriverContext = DeviceV2->DeviceContext;
        }
        else
        {
            FxDevice->DriverContext = DeviceV3->DeviceContext;
            FxDevice->DirectedPowerUpCallback = DeviceV3->DirectedPowerUpCallback;
            FxDevice->DirectedPowerDownCallback = DeviceV3->DirectedPowerDownCallback;
            FxDevice->DirectedFxTimeout = DeviceV3->DirectedFxTimeoutInSeconds;

            if (FxDevice->DirectedPowerUpCallback &&
                !FxDevice->ComponentActiveConditionCallback && !FxDevice->ComponentIdleConditionCallback &&
                !FxDevice->ComponentIdleStateCallback && !FxDevice->DevicePowerRequiredCallback &&
                !FxDevice->DevicePowerNotRequiredCallback)
            {
                FxDevice->Flags |= POP_FX_DEVICE_DIRECTED_ONLY;
            }
        }
    }

    IdleStates = (PPO_FX_COMPONENT_IDLE_STATE)((PUCHAR)FxDevice + StatesOffset);
    FxDevice->PepRegister->ComponentCount = ComponentCount;
    for (Index = 0; Index < ComponentCount; Index++)
    {
        FxComponent = &FxDevice->Components[Index];
        FxComponent->Device = FxDevice;
        FxComponent->Index = Index;

        if (DeviceV1->Version == PO_FX_VERSION_V1)
        {
            FxComponent->Id = DeviceV1->Components[Index].Id;
            FxComponent->DeepestWakeableIdleState = DeviceV1->Components[Index].DeepestWakeableIdleState;
            FxComponent->IdleStateCount = DeviceV1->Components[Index].IdleStateCount;
            RtlCopyMemory(IdleStates, DeviceV1->Components[Index].IdleStates,
                          FxComponent->IdleStateCount * sizeof(*IdleStates));
        }
        else
        {
            PPO_FX_COMPONENT_V2 Source = (DeviceV1->Version == PO_FX_VERSION_V2) ?
                                         &DeviceV2->Components[Index] : &DeviceV3->Components[Index];

            FxComponent->Id = Source->Id;
            FxComponent->Flags = Source->Flags;
            FxComponent->DeepestWakeableIdleState = Source->DeepestWakeableIdleState;
            FxComponent->IdleStateCount = Source->IdleStateCount;
            RtlCopyMemory(IdleStates, Source->IdleStates, FxComponent->IdleStateCount * sizeof(*IdleStates));
        }

        FxComponent->IdleStates = IdleStates;
        IdleStates += FxComponent->IdleStateCount;

        FxComponent->ActiveCount = 1;
        FxComponent->Active = TRUE;
        FxComponent->FState = 0;
        FxComponent->TargetFState = POP_FX_NO_TRANSITION;
        FxComponent->Latency = ~0ULL;
        FxComponent->Residency = 0;
        KeInitializeEvent(&FxComponent->ActiveEvent, NotificationEvent, TRUE);
        KeInitializeEvent(&FxComponent->PerfEvent, NotificationEvent, FALSE);

        FxComponent->PepComponent.Id = FxComponent->Id;
        FxComponent->PepComponent.Flags = FxComponent->Flags;
        FxComponent->PepComponent.DeepestWakeableIdleState = FxComponent->DeepestWakeableIdleState;
        FxComponent->PepComponent.IdleStateCount = FxComponent->IdleStateCount;
        FxComponent->PepComponent.IdleStates = FxComponent->IdleStates;
        FxDevice->PepRegister->Components[Index] = &FxComponent->PepComponent;
    }

    /* The plug-in that prepared the device gets it first, otherwise any plug-in may claim it */
    Plugin = PopFxFindPreparedDevice(Pdo, &FxDevice->DeviceId);
    if (!Plugin && FxDevice->DeviceId.Buffer == NULL)
        PopFxQueryDeviceId(Pdo, &FxDevice->DeviceId);

    Candidate = NULL;
    if (FxDevice->DeviceId.Buffer)
        Candidate = Plugin ? Plugin : PopFxNextPlugin(NULL);

    while (Candidate)
    {
        Register.DeviceId = &FxDevice->DeviceId;
        Register.KernelHandle = (POHANDLE)FxDevice;
        Register.Register = FxDevice->PepRegister;
        Register.DeviceHandle = NULL;
        Register.DeviceAccepted = PepDeviceNotAccepted;

        if (PopFxNotifyPlugin(Candidate, PEP_DPM_REGISTER_DEVICE, &Register) &&
            Register.DeviceAccepted == PepDeviceAccepted)
        {
            FxDevice->Plugin = Candidate;
            FxDevice->PluginHandle = Register.DeviceHandle;
            break;
        }

        /* A plug-in that prepared the device is the only one asked */
        if (Plugin)
            break;
        Candidate = PopFxNextPlugin(Candidate);
    }

    KeAcquireInStackQueuedSpinLock(&PopFxDeviceLock, &LockHandle);
    InsertTailList(&PopFxDeviceList, &FxDevice->Link);
    KeReleaseInStackQueuedSpinLock(&LockHandle);

    DPRINT1("PoFx registered %wZ with %lu components, PEP %p\n",
            &FxDevice->DeviceId, ComponentCount, FxDevice->Plugin);

    *Handle = (POHANDLE)FxDevice;
    return STATUS_SUCCESS;
}

/**
 * @brief Unregisters a device from the Power Framework.
 */
VOID
NTAPI
PoFxUnregisterDevice(
    _In_ POHANDLE Handle)
{
    PPOP_FX_DEVICE FxDevice = (PPOP_FX_DEVICE)Handle;
    PEP_UNREGISTER_DEVICE Unregister;
    KLOCK_QUEUE_HANDLE LockHandle;
    ULONG Index;

    PAGED_CODE();

    if (!FxDevice)
        return;

    KeCancelTimer(&FxDevice->IdleTimer);
    KeFlushQueuedDpcs();

    KeAcquireInStackQueuedSpinLock(&PopFxDeviceLock, &LockHandle);
    RemoveEntryList(&FxDevice->Link);
    KeReleaseInStackQueuedSpinLock(&LockHandle);

    if (FxDevice->Plugin)
    {
        Unregister.DeviceHandle = FxDevice->PluginHandle;
        PopFxNotifyPlugin(FxDevice->Plugin, PEP_DPM_UNREGISTER_DEVICE, &Unregister);
    }

    if (FxDevice->DeviceId.Buffer)
        RtlFreeUnicodeString(&FxDevice->DeviceId);

    for (Index = 0; Index < FxDevice->ComponentCount; Index++)
    {
        if (FxDevice->Components[Index].PerfInfo)
            ExFreePoolWithTag(FxDevice->Components[Index].PerfInfo, TAG_PO_FX_COMPONENT);
    }

    ExFreePoolWithTag(FxDevice, TAG_PO_FX_DEVICE);
}

/**
 * @brief Finishes registration and drops the activation reference every component started with.
 */
VOID
NTAPI
PoFxStartDevicePowerManagement(
    _In_ POHANDLE Handle)
{
    PPOP_FX_DEVICE FxDevice = (PPOP_FX_DEVICE)Handle;
    PEP_DEVICE_STARTED Started;
    ULONG Index;

    PAGED_CODE();

    if (!FxDevice || FxDevice->Started)
        return;

    FxDevice->Started = TRUE;

    if (FxDevice->Plugin)
    {
        Started.DeviceHandle = FxDevice->PluginHandle;
        PopFxNotifyPlugin(FxDevice->Plugin, PEP_DPM_DEVICE_STARTED, &Started);
    }

    for (Index = 0; Index < FxDevice->ComponentCount; Index++)
        PoFxIdleComponent(Handle, Index, 0);
}

/**
 * @brief
 * Takes an activation reference on a component, bringing it to F0 and the active condition.
 *
 * @param[in] Flags
 * PO_FX_FLAG_BLOCKING waits for the active condition, PO_FX_FLAG_ASYNC_ONLY keeps
 * the driver callbacks off the calling thread.
 */
VOID
NTAPI
PoFxActivateComponent(
    _In_ POHANDLE Handle,
    _In_ ULONG Component,
    _In_ ULONG Flags)
{
    PPOP_FX_DEVICE FxDevice = (PPOP_FX_DEVICE)Handle;
    PPOP_FX_COMPONENT FxComponent;
    KIRQL OldIrql;
    LONG Count;

    FxComponent = PopFxGetComponent(Handle, Component);
    if (!FxComponent)
    {
        DPRINT1("PoFxActivateComponent: bad component %lu on %p\n", Component, Handle);
        return;
    }

    KeAcquireSpinLock(&FxDevice->Lock, &OldIrql);
    Count = ++FxComponent->ActiveCount;
    if (Count == 1 && FxDevice->IdleTimerArmed)
    {
        FxDevice->IdleTimerArmed = FALSE;
        KeCancelTimer(&FxDevice->IdleTimer);
    }
    KeReleaseSpinLock(&FxDevice->Lock, OldIrql);

    if (Count == 1)
        PopFxKickComponent(FxComponent, Flags);

    if ((Flags & PO_FX_FLAG_BLOCKING) && KeGetCurrentIrql() < DISPATCH_LEVEL)
        KeWaitForSingleObject(&FxComponent->ActiveEvent, Executive, KernelMode, FALSE, NULL);
}

/**
 * @brief Drops an activation reference, the last one moves the component to the idle condition.
 */
VOID
NTAPI
PoFxIdleComponent(
    _In_ POHANDLE Handle,
    _In_ ULONG Component,
    _In_ ULONG Flags)
{
    PPOP_FX_DEVICE FxDevice = (PPOP_FX_DEVICE)Handle;
    PPOP_FX_COMPONENT FxComponent;
    KIRQL OldIrql;
    LONG Count;

    FxComponent = PopFxGetComponent(Handle, Component);
    if (!FxComponent)
    {
        DPRINT1("PoFxIdleComponent: bad component %lu on %p\n", Component, Handle);
        return;
    }

    KeAcquireSpinLock(&FxDevice->Lock, &OldIrql);
    if (FxComponent->ActiveCount == 0)
    {
        KeReleaseSpinLock(&FxDevice->Lock, OldIrql);
        DPRINT1("PoFxIdleComponent: component %lu on %p has no activation to drop\n", Component, Handle);
        return;
    }
    Count = --FxComponent->ActiveCount;
    KeReleaseSpinLock(&FxDevice->Lock, OldIrql);

    if (Count == 0)
        PopFxKickComponent(FxComponent, Flags);
}

/**
 * @brief The driver finished moving a component into the idle condition.
 */
VOID
NTAPI
PoFxCompleteIdleCondition(
    _In_ POHANDLE Handle,
    _In_ ULONG Component)
{
    PPOP_FX_DEVICE FxDevice = (PPOP_FX_DEVICE)Handle;
    PPOP_FX_COMPONENT FxComponent;
    KIRQL OldIrql;

    FxComponent = PopFxGetComponent(Handle, Component);
    if (!FxComponent)
        return;

    KeAcquireSpinLock(&FxDevice->Lock, &OldIrql);
    FxComponent->IdleConditionPending = FALSE;
    KeReleaseSpinLock(&FxDevice->Lock, OldIrql);

    PopFxAdvanceComponent(FxComponent);
}

/**
 * @brief The driver finished its part of an F-state transition.
 */
VOID
NTAPI
PoFxCompleteIdleState(
    _In_ POHANDLE Handle,
    _In_ ULONG Component)
{
    PPOP_FX_DEVICE FxDevice = (PPOP_FX_DEVICE)Handle;
    PPOP_FX_COMPONENT FxComponent;
    BOOLEAN Lowering;
    KIRQL OldIrql;

    FxComponent = PopFxGetComponent(Handle, Component);
    if (!FxComponent)
        return;

    KeAcquireSpinLock(&FxDevice->Lock, &OldIrql);
    if (!FxComponent->DriverStatePending)
    {
        KeReleaseSpinLock(&FxDevice->Lock, OldIrql);
        DPRINT1("PoFxCompleteIdleState: no transition in flight on %p/%lu\n", Handle, Component);
        return;
    }

    FxComponent->DriverStatePending = FALSE;
    Lowering = (FxComponent->TargetFState > FxComponent->FState);
    if (Lowering)
    {
        /* The driver let go of the hardware, the plug-in may now power it down */
        FxComponent->PepStatePending = TRUE;
    }
    else
    {
        FxComponent->FState = FxComponent->TargetFState;
        FxComponent->TargetFState = POP_FX_NO_TRANSITION;
    }
    KeReleaseSpinLock(&FxDevice->Lock, OldIrql);

    if (Lowering)
        PopFxNotifyPepState(FxComponent);
    else
        PopFxAdvanceComponent(FxComponent);
}

/**
 * @brief The driver finished handling DevicePowerNotRequiredCallback.
 */
VOID
NTAPI
PoFxCompleteDevicePowerNotRequired(
    _In_ POHANDLE Handle)
{
    PPOP_FX_DEVICE FxDevice = (PPOP_FX_DEVICE)Handle;
    KIRQL OldIrql;

    if (!FxDevice)
        return;

    KeAcquireSpinLock(&FxDevice->Lock, &OldIrql);
    FxDevice->PowerNotRequiredPending = FALSE;
    KeReleaseSpinLock(&FxDevice->Lock, OldIrql);

    /* An activation that came in meanwhile now asks for power again */
    PopFxKickAllComponents(FxDevice);
}

/**
 * @brief Tells PoFx a device finished the directed power down it was asked for.
 *
 * @remarks Directed transitions are never started, so there is nothing to complete.
 */
VOID
NTAPI
PoFxCompleteDirectedPowerDown(
    _In_ POHANDLE Handle)
{
    POFXTRACE(POFX_DEVICE_DEBUG, "No directed power down is in flight on %p\n", Handle);
}

/**
 * @brief Sets how long every component must stay idle before DevicePowerNotRequiredCallback.
 *
 * @param[in] IdleTimeout
 * The timeout in 100-nanosecond units.
 */
VOID
NTAPI
PoFxSetDeviceIdleTimeout(
    _In_ POHANDLE Handle,
    _In_ ULONGLONG IdleTimeout)
{
    PPOP_FX_DEVICE FxDevice = (PPOP_FX_DEVICE)Handle;

    if (!FxDevice)
        return;

    FxDevice->IdleTimeout = IdleTimeout;
    PopFxCheckDeviceIdle(FxDevice);
}

/**
 * @brief The driver brought the device back to D0 after DevicePowerRequiredCallback.
 */
VOID
NTAPI
PoFxReportDevicePoweredOn(
    _In_ POHANDLE Handle)
{
    PPOP_FX_DEVICE FxDevice = (PPOP_FX_DEVICE)Handle;
    KIRQL OldIrql;

    if (!FxDevice)
        return;

    KeAcquireSpinLock(&FxDevice->Lock, &OldIrql);
    FxDevice->PoweredOnPending = FALSE;
    FxDevice->PowerRequired = TRUE;
    KeReleaseSpinLock(&FxDevice->Lock, OldIrql);

    PopFxKickAllComponents(FxDevice);
}

/**
 * @brief Sets the longest wake latency a component tolerates, in 100-nanosecond units.
 */
VOID
NTAPI
PoFxSetComponentLatency(
    _In_ POHANDLE Handle,
    _In_ ULONG Component,
    _In_ ULONGLONG Latency)
{
    PPOP_FX_COMPONENT FxComponent;

    FxComponent = PopFxGetComponent(Handle, Component);
    if (!FxComponent)
        return;

    FxComponent->Latency = Latency;
    PopFxAdvanceComponent(FxComponent);
}

/**
 * @brief Sets how long a component is expected to stay idle, in 100-nanosecond units.
 */
VOID
NTAPI
PoFxSetComponentResidency(
    _In_ POHANDLE Handle,
    _In_ ULONG Component,
    _In_ ULONGLONG Residency)
{
    PPOP_FX_COMPONENT FxComponent;

    FxComponent = PopFxGetComponent(Handle, Component);
    if (!FxComponent)
        return;

    FxComponent->Residency = Residency;
    PopFxAdvanceComponent(FxComponent);
}

/**
 * @brief Tells the Power Framework a device came back to D0 on its own.
 *
 * @unimplemented
 */
NTSTATUS
NTAPI
PoFxNotifySurprisePowerOn(
    _In_ PDEVICE_OBJECT Pdo)
{
    UNREFERENCED_PARAMETER(Pdo);

    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief Sends a driver defined power control request to the plug-in owning the device.
 *
 * @return
 * The plug-in's status, or STATUS_NOT_SUPPORTED when no plug-in handles it.
 */
NTSTATUS
NTAPI
PoFxPowerControl(
    _In_ POHANDLE Handle,
    _In_ LPCGUID PowerControlCode,
    _In_reads_bytes_opt_(InBufferSize) PVOID InBuffer,
    _In_ SIZE_T InBufferSize,
    _Out_writes_bytes_opt_(OutBufferSize) PVOID OutBuffer,
    _In_ SIZE_T OutBufferSize,
    _Out_opt_ PSIZE_T BytesReturned)
{
    PPOP_FX_DEVICE FxDevice = (PPOP_FX_DEVICE)Handle;
    PEP_POWER_CONTROL_REQUEST Request;

    if (BytesReturned)
        *BytesReturned = 0;

    if (!FxDevice || !PowerControlCode)
        return STATUS_INVALID_PARAMETER;

    Request.DeviceHandle = FxDevice->PluginHandle;
    Request.PowerControlCode = PowerControlCode;
    Request.InBuffer = InBuffer;
    Request.InBufferSize = InBufferSize;
    Request.OutBuffer = OutBuffer;
    Request.OutBufferSize = OutBufferSize;
    Request.BytesReturned = 0;
    Request.Status = STATUS_NOT_SUPPORTED;

    if (!FxDevice->Plugin ||
        !PopFxNotifyPlugin(FxDevice->Plugin, PEP_DPM_POWER_CONTROL_REQUEST, &Request))
    {
        return STATUS_NOT_SUPPORTED;
    }

    if (BytesReturned)
        *BytesReturned = Request.BytesReturned;
    return Request.Status;
}

/**
 * @brief Makes the power state of one component depend on another.
 * @return STATUS_NOT_SUPPORTED, components are managed one by one.
 */
NTSTATUS
NTAPI
PoFxAddComponentRelation(
    _In_ POHANDLE Handle,
    _In_ ULONG Component,
    _In_ POHANDLE ProviderHandle,
    _In_ ULONG ProviderComponent,
    _In_ ULONG Flags,
    _Out_ PVOID *Relation)
{
    UNREFERENCED_PARAMETER(Handle);
    UNREFERENCED_PARAMETER(Component);
    UNREFERENCED_PARAMETER(ProviderHandle);
    UNREFERENCED_PARAMETER(ProviderComponent);
    UNREFERENCED_PARAMETER(Flags);

    *Relation = NULL;
    return STATUS_NOT_SUPPORTED;
}

/**
 * @brief Removes a relation PoFxAddComponentRelation made.
 */
NTSTATUS
NTAPI
PoFxRemoveComponentRelation(
    _In_ POHANDLE Handle,
    _In_opt_ PVOID Relation)
{
    UNREFERENCED_PARAMETER(Handle);
    UNREFERENCED_PARAMETER(Relation);

    return STATUS_NOT_FOUND;
}

/**
 * @brief
 * Registers the performance states of a component and hands them to the plug-in.
 *
 * @param[out] OutputStateInfo
 * Receives the copy PoFx keeps. It stays valid until the device is unregistered.
 *
 * @return
 * STATUS_NOT_SUPPORTED when no plug-in takes the states and
 * PO_FX_FLAG_PERF_PEP_OPTIONAL is not set.
 */
NTSTATUS
NTAPI
PoFxRegisterComponentPerfStates(
    _In_ POHANDLE Handle,
    _In_ ULONG Component,
    _In_ ULONGLONG Flags,
    _In_ PPO_FX_COMPONENT_PERF_STATE_CALLBACK ComponentPerfStateCallback,
    _In_ PPO_FX_COMPONENT_PERF_INFO InputStateInfo,
    _Out_ PPO_FX_COMPONENT_PERF_INFO *OutputStateInfo)
{
    PPOP_FX_DEVICE FxDevice = (PPOP_FX_DEVICE)Handle;
    PEP_REGISTER_COMPONENT_PERF_STATES Register;
    PPOP_FX_COMPONENT FxComponent;
    PPO_FX_COMPONENT_PERF_INFO PerfInfo;
    PPO_FX_PERF_STATE States;
    SIZE_T SetsSize, CurrentSize, StatesSize = 0;
    ULONG SetIndex;

    PAGED_CODE();

    FxComponent = PopFxGetComponent(Handle, Component);
    if (!FxComponent || !InputStateInfo || !OutputStateInfo || !InputStateInfo->PerfStateSetsCount)
        return STATUS_INVALID_PARAMETER;
    if (FxComponent->PerfInfo)
        return STATUS_INVALID_PARAMETER;

    /* One block holds the sets, the current value of each set and every discrete state array */
    SetsSize = FIELD_OFFSET(PO_FX_COMPONENT_PERF_INFO, PerfStateSets[InputStateInfo->PerfStateSetsCount]);
    SetsSize = ALIGN_UP_BY(SetsSize, sizeof(ULONGLONG));
    CurrentSize = InputStateInfo->PerfStateSetsCount * sizeof(ULONGLONG);
    for (SetIndex = 0; SetIndex < InputStateInfo->PerfStateSetsCount; SetIndex++)
    {
        if (InputStateInfo->PerfStateSets[SetIndex].Type == PoFxPerfStateTypeDiscrete)
            StatesSize += InputStateInfo->PerfStateSets[SetIndex].Discrete.Count * sizeof(PO_FX_PERF_STATE);
    }

    PerfInfo = ExAllocatePoolZero(NonPagedPool, SetsSize + CurrentSize + StatesSize, TAG_PO_FX_COMPONENT);
    if (!PerfInfo)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(PerfInfo, InputStateInfo,
                  FIELD_OFFSET(PO_FX_COMPONENT_PERF_INFO, PerfStateSets[InputStateInfo->PerfStateSetsCount]));
    States = (PPO_FX_PERF_STATE)((PUCHAR)PerfInfo + SetsSize + CurrentSize);
    for (SetIndex = 0; SetIndex < PerfInfo->PerfStateSetsCount; SetIndex++)
    {
        PPO_FX_COMPONENT_PERF_SET Set = &PerfInfo->PerfStateSets[SetIndex];

        if (Set->Type != PoFxPerfStateTypeDiscrete)
            continue;

        RtlCopyMemory(States, Set->Discrete.States, Set->Discrete.Count * sizeof(*States));
        Set->Discrete.States = States;
        States += Set->Discrete.Count;
    }

    /* PEP_COMPONENT_PERF_INFO and PO_FX_COMPONENT_PERF_INFO share their layout */
    Register.DeviceHandle = FxDevice->PluginHandle;
    Register.Component = Component;
    Register.Flags = Flags;
    Register.PerfStateInfo = (PPEP_COMPONENT_PERF_INFO)PerfInfo;
    if (!(FxDevice->Plugin &&
          PopFxNotifyPlugin(FxDevice->Plugin, PEP_DPM_REGISTER_COMPONENT_PERF_STATES, &Register)) &&
        !(Flags & PO_FX_FLAG_PERF_PEP_OPTIONAL))
    {
        ExFreePoolWithTag(PerfInfo, TAG_PO_FX_COMPONENT);
        return STATUS_NOT_SUPPORTED;
    }

    FxComponent->PerfCurrent = (PULONGLONG)((PUCHAR)PerfInfo + SetsSize);
    FxComponent->PerfStateCallback = ComponentPerfStateCallback;
    FxComponent->PerfInfo = PerfInfo;
    *OutputStateInfo = PerfInfo;
    return STATUS_SUCCESS;
}

/**
 * @brief Moves a component to a new performance state through its plug-in.
 *
 * @remarks Without a plug-in the change is recorded and reported as made.
 */
VOID
NTAPI
PoFxIssueComponentPerfStateChange(
    _In_ POHANDLE Handle,
    _In_ ULONG Flags,
    _In_ ULONG Component,
    _In_ PPO_FX_PERF_STATE_CHANGE PerfChange,
    _In_ PVOID Context)
{
    PPOP_FX_DEVICE FxDevice = (PPOP_FX_DEVICE)Handle;
    PEP_REQUEST_COMPONENT_PERF_STATE Request;
    PPOP_FX_COMPONENT FxComponent;
    BOOLEAN Handled = FALSE;
    KIRQL OldIrql;

    /* A blocking change cannot wait at DISPATCH_LEVEL, nor be both blocking and asynchronous */
    if (((Flags & PO_FX_FLAG_BLOCKING) && KeGetCurrentIrql() >= DISPATCH_LEVEL) ||
        ((Flags & (PO_FX_FLAG_BLOCKING | PO_FX_FLAG_ASYNC_ONLY)) == (PO_FX_FLAG_BLOCKING | PO_FX_FLAG_ASYNC_ONLY)))
    {
        DPRINT1("PoFxIssueComponentPerfStateChange: invalid flags 0x%lx\n", Flags);
        return;
    }

    FxComponent = PopFxGetComponent(Handle, Component);
    if (!FxComponent || !FxComponent->PerfInfo || !PerfChange ||
        PerfChange->Set >= FxComponent->PerfInfo->PerfStateSetsCount)
    {
        DPRINT1("PoFxIssueComponentPerfStateChange: invalid parameter\n");
        return;
    }

    KeAcquireSpinLock(&FxDevice->Lock, &OldIrql);
    FxComponent->PerfRequestPending = TRUE;
    FxComponent->PerfRequestContext = Context;
    FxComponent->PerfCurrent[PerfChange->Set] =
        (FxComponent->PerfInfo->PerfStateSets[PerfChange->Set].Type == PoFxPerfStateTypeDiscrete) ?
        PerfChange->StateIndex : PerfChange->StateValue;
    KeClearEvent(&FxComponent->PerfEvent);
    KeReleaseSpinLock(&FxDevice->Lock, OldIrql);

    /* PO_FX_PERF_STATE_CHANGE and PEP_COMPONENT_PERF_STATE_REQUEST share their layout */
    Request.DeviceHandle = FxDevice->PluginHandle;
    Request.Component = Component;
    Request.Completed = FALSE;
    Request.Succeeded = FALSE;
    Request.PerfRequestsCount = 1;
    Request.PerfRequests = (PPEP_COMPONENT_PERF_STATE_REQUEST)PerfChange;
    if (FxDevice->Plugin)
        Handled = PopFxNotifyPlugin(FxDevice->Plugin, PEP_DPM_REQUEST_COMPONENT_PERF_STATE, &Request);

    if (!Handled)
        PopFxCompletePepPerfState(FxDevice, Component, TRUE);
    else if (Request.Completed)
        PopFxCompletePepPerfState(FxDevice, Component, Request.Succeeded);

    if (Flags & PO_FX_FLAG_BLOCKING)
        KeWaitForSingleObject(&FxComponent->PerfEvent, Executive, KernelMode, FALSE, NULL);
}

/**
 * @brief Reads the current performance state of one set of a component.
 *
 * @param[out] CurrentPerf
 * The state index for a discrete set, the value for a range set.
 */
NTSTATUS
NTAPI
PoFxQueryCurrentComponentPerfState(
    _In_ POHANDLE Handle,
    _In_ ULONG Flags,
    _In_ ULONG Component,
    _In_ ULONG SetIndex,
    _Out_ PULONGLONG CurrentPerf)
{
    PPOP_FX_DEVICE FxDevice = (PPOP_FX_DEVICE)Handle;
    PEP_QUERY_CURRENT_COMPONENT_PERF_STATE Query;
    PPOP_FX_COMPONENT FxComponent;
    BOOLEAN Discrete;

    UNREFERENCED_PARAMETER(Flags);

    FxComponent = PopFxGetComponent(Handle, Component);
    if (!FxComponent || !CurrentPerf || !FxComponent->PerfInfo ||
        SetIndex >= FxComponent->PerfInfo->PerfStateSetsCount)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Discrete = (FxComponent->PerfInfo->PerfStateSets[SetIndex].Type == PoFxPerfStateTypeDiscrete);

    Query.DeviceHandle = FxDevice->PluginHandle;
    Query.Component = Component;
    Query.Set = SetIndex;
    Query.StateValue = 0;
    if (FxDevice->Plugin &&
        PopFxNotifyPlugin(FxDevice->Plugin, PEP_DPM_QUERY_CURRENT_COMPONENT_PERF_STATE, &Query))
    {
        *CurrentPerf = Discrete ? Query.StateIndex : Query.StateValue;
        return STATUS_SUCCESS;
    }

    *CurrentPerf = FxComponent->PerfCurrent[SetIndex];
    return STATUS_SUCCESS;
}

/**
 * @brief Marks a device as the crash dump target so its plug-in keeps a way to power it.
 *
 * @return STATUS_NOT_SUPPORTED when no plug-in hands back a power on routine.
 */
NTSTATUS
NTAPI
PoFxRegisterCrashdumpDevice(
    _In_ POHANDLE Handle)
{
    PPOP_FX_DEVICE FxDevice = (PPOP_FX_DEVICE)Handle;
    PEP_REGISTER_CRASHDUMP_DEVICE Register;

    if (!FxDevice)
        return STATUS_INVALID_PARAMETER;

    Register.PowerOnDumpDeviceCallback = NULL;
    Register.DeviceHandle = FxDevice->PluginHandle;
    if (!FxDevice->Plugin ||
        !PopFxNotifyPlugin(FxDevice->Plugin, PEP_DPM_REGISTER_CRASHDUMP_DEVICE, &Register) ||
        !Register.PowerOnDumpDeviceCallback)
    {
        return STATUS_NOT_SUPPORTED;
    }

    FxDevice->PowerOnDumpDeviceCallback = Register.PowerOnDumpDeviceCallback;
    return STATUS_SUCCESS;
}

/**
 * @brief Powers a crash dump device back on through its plug-in.
 */
NTSTATUS
NTAPI
PoFxPowerOnCrashdumpDevice(
    _In_ POHANDLE Handle,
    _In_opt_ PVOID Context)
{
    PPOP_FX_DEVICE FxDevice = (PPOP_FX_DEVICE)Handle;
    PEP_CRASHDUMP_INFORMATION Information;

    if (!FxDevice)
        return STATUS_INVALID_PARAMETER;
    if (!FxDevice->PowerOnDumpDeviceCallback)
        return STATUS_UNSUCCESSFUL;

    Information.DeviceHandle = FxDevice->PluginHandle;
    Information.DeviceContext = Context;
    return FxDevice->PowerOnDumpDeviceCallback(&Information) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

/* EOF */
