/*
 * PROJECT:     ReactOS USB Role Switch Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Role selection and the role child
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "urscx.h"

#define NDEBUG
#include <debug.h>

DECLARE_CONST_UNICODE_STRING(UrsModeValueName, L"RoleSwitchMode");
DECLARE_CONST_UNICODE_STRING(UrsOsDefaultValueName, L"OsDefaultRoleSwitchMode");
DECLARE_CONST_UNICODE_STRING(UrsVBusValueName, L"DisableVBusManagement");
DECLARE_CONST_UNICODE_STRING(UrsUsbControlKey, L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\USB");

static EVT_WDF_DPC UrsRoleEvtLaunchDpc;
static EVT_WDF_WORKITEM UrsRoleEvtWorkItem;
static URS_ROLE_CALLBACK UrsRoleEvtPrepare;
static URS_ROLE_CALLBACK UrsRoleEvtRelease;

static
BOOLEAN
UrsRoleIsConfigurableMode(
    _In_ ULONG Mode)
{
    return (Mode >= UrsModeHost && Mode <= UrsModeNone) || Mode == UrsModeHardwareEventsHostOnly;
}

static
URS_ROLE_SWITCH_MODE
UrsRoleReadMode(
    _In_opt_ WDFKEY Key,
    _In_ PCUNICODE_STRING ValueName)
{
    ULONG Value;

    if (!Key || !NT_SUCCESS(WdfRegistryQueryULong(Key, ValueName, &Value)))
        return UrsModeUnset;

    return UrsRoleIsConfigurableMode(Value) ? (URS_ROLE_SWITCH_MODE)Value : UrsModeUnset;
}

static
URS_ROLE_SWITCH_MODE
UrsRoleReadDevNodeMode(
    _In_ PURS_FDO Fdo)
{
    URS_ROLE_SWITCH_MODE Mode;
    WDFKEY Key;

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Fdo->Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ, NULL, &Key)))
        return UrsModeUnset;

    Mode = UrsRoleReadMode(Key, &UrsModeValueName);
    WdfRegistryClose(Key);
    return Mode;
}

static
URS_ROLE_SWITCH_MODE
UrsRoleReadSystemMode(
    _In_ PCUNICODE_STRING ValueName)
{
    URS_ROLE_SWITCH_MODE Mode;
    WDFKEY Key;

    if (!NT_SUCCESS(WdfRegistryOpenKey(NULL, &UrsUsbControlKey, KEY_READ, NULL, &Key)))
        return UrsModeUnset;

    Mode = UrsRoleReadMode(Key, ValueName);
    WdfRegistryClose(Key);
    return Mode;
}

/* The device's own setting wins, then the OEM's, then the OS default */
static
URS_ROLE_SWITCH_MODE
UrsRoleConfiguredMode(
    _In_ PURS_FDO Fdo)
{
    URS_ROLE_SWITCH_MODE Mode = UrsRoleReadDevNodeMode(Fdo);

    if (Mode == UrsModeUnset)
        Mode = UrsRoleReadSystemMode(&UrsModeValueName);
    if (Mode == UrsModeUnset)
        Mode = UrsRoleReadSystemMode(&UrsOsDefaultValueName);
    if (Mode == UrsModeUnset)
        Mode = UrsModeHardwareEvents;

    return Mode;
}

static
VOID
UrsRoleRecordMode(
    _In_ PURS_FDO Fdo,
    _In_ URS_ROLE_SWITCH_MODE Mode,
    _In_ BOOLEAN Clear)
{
    WDFKEY Key;

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Fdo->Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ | KEY_WRITE, NULL, &Key)))
        return;

    if (Clear)
        WdfRegistryRemoveValue(Key, &UrsModeValueName);
    else
        WdfRegistryAssignULong(Key, &UrsModeValueName, Mode);

    WdfRegistryClose(Key);
}

static
NTSTATUS
UrsRoleTeardown(
    _In_ PURS_FDO Fdo)
{
    WDFDEVICE Child;
    NTSTATUS Status = STATUS_SUCCESS;

    WdfFdoLockStaticChildListForIteration(Fdo->Device);
    Child = WdfFdoRetrieveNextStaticChild(Fdo->Device, NULL, WdfRetrieveAddedChildren);
    if (Child)
    {
        WdfObjectReference(Child);
        Status = WdfPdoMarkMissing(Child);
    }
    WdfFdoUnlockStaticChildListFromIteration(Fdo->Device);

    if (Child)
        WdfObjectDereference(Child);

    if (NT_SUCCESS(Status))
        Fdo->CurrentRole = UrsRoleNone;

    return Status;
}

static
NTSTATUS
UrsRoleSetup(
    _In_ PURS_FDO Fdo,
    _In_ URS_ROLE Role)
{
    URS_ROLE_CONFIG Config;
    WDFDEVICE Child = NULL;
    NTSTATUS Status;

    if (Role == UrsRoleNone)
        return STATUS_SUCCESS;

    Config.Size = sizeof(Config);
    Config.Role = Role;
    Config.Prepare = UrsRoleEvtPrepare;
    Config.Release = UrsRoleEvtRelease;

    Status = UrsPdoCreate(Fdo, &Config, &Child);
    if (NT_SUCCESS(Status))
        Status = WdfFdoAddStaticChild(Fdo->Device, Child);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Role %u child could not be added 0x%lx\n", Role, Status);
        if (Child)
            WdfObjectDelete(Child);
        return Status;
    }

    Fdo->CurrentRole = Role;
    return STATUS_SUCCESS;
}

static
NTSTATUS
UrsRoleApply(
    _In_ PURS_FDO Fdo,
    _In_ URS_ROLE Role)
{
    NTSTATUS Status;

    if (Role == Fdo->CurrentRole)
        return STATUS_SUCCESS;

    Status = UrsRoleTeardown(Fdo);
    if (NT_SUCCESS(Status))
        Status = UrsRoleSetup(Fdo, Role);

    return Status;
}

static
VOID
UrsRoleApplyFixedMode(
    _In_ PURS_FDO Fdo,
    _In_ URS_ROLE_SWITCH_MODE Mode)
{
    switch (Mode)
    {
        case UrsModeHost:
            UrsRoleApply(Fdo, UrsRoleHost);
            break;

        case UrsModeFunction:
            UrsRoleApply(Fdo, UrsRoleFunction);
            break;

        case UrsModeNone:
            UrsRoleApply(Fdo, UrsRoleNone);
            break;

        default:
            break;
    }
}

static
URS_ROLE
UrsRoleForEvent(
    _In_ URS_HARDWARE_EVENT Event,
    _Out_ PNTSTATUS Status)
{
    *Status = STATUS_SUCCESS;

    switch (Event)
    {
        case UrsHardwareEventDetach:
            return UrsRoleNone;

        case UrsHardwareEventIdGround:
        case UrsHardwareEventIdA:
            return UrsRoleHost;

        case UrsHardwareEventIdFloat:
        case UrsHardwareEventIdB:
            return UrsRoleFunction;

        default:
            *Status = STATUS_INVALID_DEVICE_REQUEST;
            return UrsRoleNone;
    }
}

/* Called with the lock held, follows events until none arrive while one is applied */
static
VOID
UrsRoleProcessEvents(
    _In_ PURS_FDO Fdo)
{
    LONG Event = InterlockedCompareExchange(&Fdo->PendingEvent, 0, 0);
    LONG Taken;
    URS_ROLE Role;
    NTSTATUS Status;

    while (Event)
    {
        Taken = Event;
        Role = UrsRoleForEvent((URS_HARDWARE_EVENT)Event, &Status);
        if (NT_SUCCESS(Status))
        {
            Status = UrsRoleApply(Fdo, Role);
            if (NT_SUCCESS(Status))
                Fdo->LastEvent = (URS_HARDWARE_EVENT)Event;
        }

        if (!NT_SUCCESS(Status))
            break;

        Event = InterlockedCompareExchange(&Fdo->PendingEvent, 0, Taken);
        if (Event == Taken)
            break;
    }
}

static
VOID
UrsRoleEvtWorkItem(
    _In_ WDFWORKITEM WorkItem)
{
    PURS_FDO Fdo = UrsGetFdo(WdfWorkItemGetParentObject(WorkItem));

    WdfWaitLockAcquire(Fdo->Lock, NULL);
    if (Fdo->FollowHardwareEvents || Fdo->HostOnlyEvents)
        UrsRoleProcessEvents(Fdo);
    WdfWaitLockRelease(Fdo->Lock);
}

static
VOID
UrsRoleEvtLaunchDpc(
    _In_ WDFDPC Dpc)
{
    WdfWorkItemEnqueue(UrsGetFdo(WdfDpcGetParentObject(Dpc))->EventWorkItem);
}

/* VBUS goes through the platform power extension, which ReactOS does not have */
static
NTSTATUS
UrsRoleSetVBus(
    _In_ PURS_FDO Fdo,
    _In_ BOOLEAN On)
{
    UNREFERENCED_PARAMETER(On);

    if (!Fdo->PoHandle)
    {
        DbgPrint("UrsCx Verifier Break: Client driver should have already reported a POHANDLE to UrsCx by now\n");
        WdfVerifierDbgBreakPoint();
        return STATUS_INVALID_DEVICE_STATE;
    }

    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
NTAPI
UrsRoleEvtPrepare(
    _In_ PURS_FDO Fdo,
    _In_ URS_ROLE Role)
{
    NTSTATUS Status;

    Status = WdfDeviceStopIdle(Fdo->Device, TRUE);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Fdo->Config.EvtUrsSetRole)
        Status = Fdo->Config.EvtUrsSetRole(Fdo->Device, Role);

    if (NT_SUCCESS(Status) && Role == UrsRoleHost && !Fdo->VBusUnmanaged)
        Status = UrsRoleSetVBus(Fdo, TRUE);

    WdfDeviceResumeIdle(Fdo->Device);
    return Status;
}

static
NTSTATUS
NTAPI
UrsRoleEvtRelease(
    _In_ PURS_FDO Fdo,
    _In_ URS_ROLE Role)
{
    if (Role != UrsRoleHost || Fdo->VBusUnmanaged)
        return STATUS_SUCCESS;

    return UrsRoleSetVBus(Fdo, FALSE);
}

/**
 * @brief
 * Picks the starting role from the configured mode and sets up event delivery.
 */
NTSTATUS
NTAPI
UrsRoleInitialize(
    _In_ PURS_FDO Fdo)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_WORKITEM_CONFIG WorkConfig;
    WDF_DPC_CONFIG DpcConfig;
    URS_ROLE_SWITCH_MODE Mode;
    WDFKEY Key;
    ULONG Value;
    NTSTATUS Status;

    Fdo->FollowHardwareEvents = TRUE;
    Fdo->HostOnlyEvents = FALSE;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Fdo->Device;
    Status = WdfWaitLockCreate(&Attributes, &Fdo->Lock);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_DPC_CONFIG_INIT(&DpcConfig, UrsRoleEvtLaunchDpc);
    DpcConfig.AutomaticSerialization = TRUE;
    Status = WdfDpcCreate(&DpcConfig, &Attributes, &Fdo->EventDpc);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_WORKITEM_CONFIG_INIT(&WorkConfig, UrsRoleEvtWorkItem);
    WorkConfig.AutomaticSerialization = TRUE;
    Status = WdfWorkItemCreate(&WorkConfig, &Attributes, &Fdo->EventWorkItem);
    if (!NT_SUCCESS(Status))
        return Status;

    Mode = UrsRoleConfiguredMode(Fdo);
    DPRINT("Starting in role switch mode %u\n", Mode);

    if (Mode == UrsModeHardwareEventsHostOnly)
    {
        Fdo->HostOnlyEvents = TRUE;
    }
    else if (Mode != UrsModeHardwareEvents)
    {
        Fdo->FollowHardwareEvents = FALSE;
        UrsRoleApplyFixedMode(Fdo, Mode);
    }

    Status = WdfDeviceOpenRegistryKey(Fdo->Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ, NULL, &Key);
    if (!NT_SUCCESS(Status))
        return Status;

    if (NT_SUCCESS(WdfRegistryQueryULong(Key, &UrsVBusValueName, &Value)))
        Fdo->VBusUnmanaged = (Value != 0);

    Fdo->LastEvent = UrsHardwareEventDetach;
    WdfRegistryClose(Key);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reports the mode the role manager runs in right now.
 */
URS_ROLE_SWITCH_MODE
NTAPI
UrsRoleGetMode(
    _In_ PURS_FDO Fdo)
{
    BOOLEAN Follow, HostOnly;
    URS_ROLE Role;

    WdfWaitLockAcquire(Fdo->Lock, NULL);
    Role = Fdo->CurrentRole;
    Follow = Fdo->FollowHardwareEvents;
    HostOnly = Fdo->HostOnlyEvents;
    WdfWaitLockRelease(Fdo->Lock);

    if (Follow)
        return UrsModeHardwareEvents;
    if (HostOnly)
        return UrsModeHardwareEventsHostOnly;

    switch (Role)
    {
        case UrsRoleNone:
            return UrsModeNone;

        case UrsRoleHost:
            return UrsModeHost;

        case UrsRoleFunction:
            return UrsModeFunction;

        default:
            return UrsModeUnset;
    }
}

/**
 * @brief
 * Switches to a new mode and remembers it on the device.
 */
NTSTATUS
NTAPI
UrsRoleSetMode(
    _In_ PURS_FDO Fdo,
    _In_ URS_ROLE_SWITCH_MODE Mode)
{
    URS_ROLE_SWITCH_MODE Effective = Mode;

    WdfWaitLockAcquire(Fdo->Lock, NULL);

    if (Mode == UrsModeOemDefault)
        Effective = UrsRoleReadSystemMode(&UrsModeValueName);

    if (Effective == UrsModeHardwareEvents || Effective == UrsModeHardwareEventsHostOnly)
    {
        /* Pick up the hardware state that was last reported */
        Fdo->FollowHardwareEvents = TRUE;
        InterlockedCompareExchange(&Fdo->PendingEvent, Fdo->LastEvent, 0);
        UrsRoleProcessEvents(Fdo);
    }
    else
    {
        Fdo->FollowHardwareEvents = FALSE;
        UrsRoleApplyFixedMode(Fdo, Effective);
    }

    WdfWaitLockRelease(Fdo->Lock);

    UrsRoleRecordMode(Fdo, Effective, Mode == UrsModeOemDefault);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Queues a hardware event for the role manager, collapsing bursts into the newest one.
 */
NTSTATUS
NTAPI
UrsRoleReportEvent(
    _In_ PURS_FDO Fdo,
    _In_ URS_HARDWARE_EVENT Event)
{
    if (Event < UrsHardwareEventDetach || Event > UrsHardwareEventIdB)
        return STATUS_INVALID_PARAMETER;

    if (InterlockedExchange(&Fdo->PendingEvent, Event) == 0)
    {
        if (KeGetCurrentIrql() > DISPATCH_LEVEL)
            WdfDpcEnqueue(Fdo->EventDpc);
        else
            WdfWorkItemEnqueue(Fdo->EventWorkItem);
    }

    return STATUS_SUCCESS;
}
