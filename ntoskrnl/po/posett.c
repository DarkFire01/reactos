/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Power Manager power settings callback mechanism
 * COPYRIGHT:   Copyright 2023 George Bișoc <george.bisoc@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

LIST_ENTRY PopPowerSettingCallbacksList;
ULONG PopPowerSettingCallbacksCount;
PKTHREAD PopPowerSettingOwnerLockThread;
FAST_MUTEX PopPowerSettingLock;

/* Values the power service set, a separate lock as they are read under the callback lock */
LIST_ENTRY PopSettingValueList;
FAST_MUTEX PopSettingValueLock;

/* Notification names handed to user mode, each publishes the value of one setting */
typedef struct _POP_SETTING_WNF_NAME
{
    LIST_ENTRY Link;
    GUID SettingGuid;
    ULONGLONG StateName;
    ULONG LastLength;
    PUCHAR LastValue;
} POP_SETTING_WNF_NAME, *PPOP_SETTING_WNF_NAME;

static LIST_ENTRY PopSettingWnfNames = { &PopSettingWnfNames, &PopSettingWnfNames };
static volatile LONG PopSettingWnfNameCount;
FAST_MUTEX PopSettingWnfLock;

/* Connected standby state settings the graphics kernel listens to */
const GUID PopIdleResiliencySettingGuid =
    {0x307bcc77, 0xe09b, 0x43ce, {0x8d, 0x97, 0x46, 0x75, 0x6f, 0xfb, 0x04, 0xa7}};
const GUID PopLowPowerPhaseSettingGuid =
    {0xe1233993, 0xeaa4, 0x470f, {0x9d, 0xe7, 0xa3, 0x51, 0xc1, 0xb6, 0xfb, 0x71}};

/* POWER SETTINGS DATABASE ****************************************************/

POP_POWER_SETTING_DATABASE PopPowerSettingsDatabase[POP_MAX_POWER_SETTINGS] =
{
    {&GUID_MAX_POWER_SAVINGS, NULL},
    {&GUID_MIN_POWER_SAVINGS, NULL},
    {&GUID_TYPICAL_POWER_SAVINGS, NULL},
    {&NO_SUBGROUP_GUID, NULL},
    {&ALL_POWERSCHEMES_GUID, NULL},
    {&GUID_POWERSCHEME_PERSONALITY, NULL},
    {&GUID_ACTIVE_POWERSCHEME, NULL},
    {&GUID_VIDEO_POWERDOWN_TIMEOUT, NULL},
    {&GUID_VIDEO_ANNOYANCE_TIMEOUT, NULL},
    {&GUID_VIDEO_ADAPTIVE_PERCENT_INCREASE, NULL},
    {&GUID_VIDEO_DIM_TIMEOUT, NULL},
    {&GUID_VIDEO_ADAPTIVE_POWERDOWN, NULL},
    {&GUID_MONITOR_POWER_ON, PopDisplayStateSettingWorker},
    {&GUID_DEVICE_POWER_POLICY_VIDEO_BRIGHTNESS, NULL},
    {&GUID_DEVICE_POWER_POLICY_VIDEO_DIM_BRIGHTNESS, NULL},
    {&GUID_VIDEO_CURRENT_MONITOR_BRIGHTNESS, NULL},
    {&GUID_VIDEO_ADAPTIVE_DISPLAY_BRIGHTNESS, NULL},
    {&GUID_SESSION_DISPLAY_STATE, PopDisplayStateSettingWorker},
    {&GUID_CONSOLE_DISPLAY_STATE, PopDisplayStateSettingWorker},
    {&GUID_ALLOW_DISPLAY_REQUIRED, NULL},
    {&GUID_DISK_SUBGROUP, NULL},
    {&GUID_DISK_POWERDOWN_TIMEOUT, NULL},
    {&GUID_DISK_IDLE_TIMEOUT, NULL},
    {&GUID_DISK_BURST_IGNORE_THRESHOLD, NULL},
    {&GUID_DISK_ADAPTIVE_POWERDOWN, NULL},
    {&GUID_SLEEP_SUBGROUP, NULL},
    {&GUID_SLEEP_IDLE_THRESHOLD, NULL},
    {&GUID_STANDBY_TIMEOUT, NULL},
    {&GUID_UNATTEND_SLEEP_TIMEOUT, NULL},
    {&GUID_HIBERNATE_TIMEOUT, NULL},
    {&GUID_HIBERNATE_FASTS4_POLICY, NULL},
    {&GUID_CRITICAL_POWER_TRANSITION, NULL},
    {&GUID_SYSTEM_AWAYMODE, NULL},
    {&GUID_ALLOW_AWAYMODE, NULL},
    {&GUID_ALLOW_STANDBY_STATES, NULL},
    {&GUID_ALLOW_RTC_WAKE, NULL},
    {&GUID_ALLOW_SYSTEM_REQUIRED, NULL},
    {&GUID_SYSTEM_BUTTON_SUBGROUP, NULL},
    {&GUID_POWERBUTTON_ACTION, NULL},
    {&GUID_POWERBUTTON_ACTION_FLAGS, NULL},
    {&GUID_SLEEPBUTTON_ACTION, NULL},
    {&GUID_SLEEPBUTTON_ACTION_FLAGS, NULL},
    {&GUID_USERINTERFACEBUTTON_ACTION, NULL},
    {&GUID_LIDCLOSE_ACTION, NULL},
    {&GUID_LIDCLOSE_ACTION_FLAGS, NULL},
    {&GUID_LIDOPEN_POWERSTATE, NULL},
    {&GUID_BATTERY_SUBGROUP, NULL},
    {&GUID_BATTERY_DISCHARGE_ACTION_0, NULL},
    {&GUID_BATTERY_DISCHARGE_LEVEL_0, NULL},
    {&GUID_BATTERY_DISCHARGE_FLAGS_0, NULL},
    {&GUID_BATTERY_DISCHARGE_ACTION_1, NULL},
    {&GUID_BATTERY_DISCHARGE_LEVEL_1, NULL},
    {&GUID_BATTERY_DISCHARGE_FLAGS_1, NULL},
    {&GUID_BATTERY_DISCHARGE_ACTION_2, NULL},
    {&GUID_BATTERY_DISCHARGE_LEVEL_2, NULL},
    {&GUID_BATTERY_DISCHARGE_FLAGS_2, NULL},
    {&GUID_BATTERY_DISCHARGE_ACTION_3, NULL},
    {&GUID_BATTERY_DISCHARGE_LEVEL_3, NULL},
    {&GUID_BATTERY_DISCHARGE_FLAGS_3, NULL},
    {&GUID_PROCESSOR_SETTINGS_SUBGROUP, NULL},
    {&GUID_PROCESSOR_THROTTLE_POLICY, NULL},
    {&GUID_PROCESSOR_THROTTLE_MAXIMUM, NULL},
    {&GUID_PROCESSOR_THROTTLE_MINIMUM, NULL},
    {&GUID_PROCESSOR_ALLOW_THROTTLING, NULL},
    {&GUID_PROCESSOR_IDLESTATE_POLICY, NULL},
    {&GUID_PROCESSOR_PERFSTATE_POLICY, NULL},
    {&GUID_PROCESSOR_PERF_INCREASE_THRESHOLD, NULL},
    {&GUID_PROCESSOR_PERF_DECREASE_THRESHOLD, NULL},
    {&GUID_PROCESSOR_PERF_INCREASE_POLICY, NULL},
    {&GUID_PROCESSOR_PERF_DECREASE_POLICY, NULL},
    {&GUID_PROCESSOR_PERF_INCREASE_TIME, NULL},
    {&GUID_PROCESSOR_PERF_DECREASE_TIME, NULL},
    {&GUID_PROCESSOR_PERF_TIME_CHECK, NULL},
    {&GUID_PROCESSOR_PERF_BOOST_POLICY, NULL},
    {&GUID_PROCESSOR_IDLE_ALLOW_SCALING, NULL},
    {&GUID_PROCESSOR_IDLE_DISABLE, NULL},
    {&GUID_PROCESSOR_IDLE_TIME_CHECK, NULL},
    {&GUID_PROCESSOR_IDLE_DEMOTE_THRESHOLD, NULL},
    {&GUID_PROCESSOR_IDLE_PROMOTE_THRESHOLD, NULL},
    {&GUID_PROCESSOR_CORE_PARKING_INCREASE_THRESHOLD, NULL},
    {&GUID_PROCESSOR_CORE_PARKING_DECREASE_THRESHOLD, NULL},
    {&GUID_PROCESSOR_CORE_PARKING_INCREASE_POLICY, NULL},
    {&GUID_PROCESSOR_CORE_PARKING_DECREASE_POLICY, NULL},
    {&GUID_PROCESSOR_CORE_PARKING_MAX_CORES, NULL},
    {&GUID_PROCESSOR_CORE_PARKING_MIN_CORES, NULL},
    {&GUID_PROCESSOR_CORE_PARKING_INCREASE_TIME, NULL},
    {&GUID_PROCESSOR_CORE_PARKING_DECREASE_TIME, NULL},
    {&GUID_PROCESSOR_CORE_PARKING_AFFINITY_HISTORY_DECREASE_FACTOR, NULL},
    {&GUID_PROCESSOR_CORE_PARKING_AFFINITY_HISTORY_THRESHOLD, NULL},
    {&GUID_PROCESSOR_CORE_PARKING_AFFINITY_WEIGHTING, NULL},
    {&GUID_PROCESSOR_CORE_PARKING_OVER_UTILIZATION_HISTORY_DECREASE_FACTOR, NULL},
    {&GUID_PROCESSOR_CORE_PARKING_OVER_UTILIZATION_HISTORY_THRESHOLD, NULL},
    {&GUID_PROCESSOR_CORE_PARKING_OVER_UTILIZATION_WEIGHTING, NULL},
    {&GUID_PROCESSOR_CORE_PARKING_OVER_UTILIZATION_THRESHOLD, NULL},
    {&GUID_PROCESSOR_PARKING_CORE_OVERRIDE, NULL},
    {&GUID_PROCESSOR_PARKING_PERF_STATE, NULL},
    {&GUID_PROCESSOR_PERF_HISTORY, NULL},
    {&GUID_SYSTEM_COOLING_POLICY, NULL},
    {&GUID_LOCK_CONSOLE_ON_WAKE, NULL},
    {&GUID_DEVICE_IDLE_POLICY, NULL},
    {&GUID_ACDC_POWER_SOURCE, PopPowerSourceSettingWorker},
    {&GUID_LIDSWITCH_STATE_CHANGE, PopLidStateChangeSettingWorker},
    {&GUID_BATTERY_PERCENTAGE_REMAINING, PopBatteryRemainingSettingWorker},
    {&GUID_IDLE_BACKGROUND_TASK, NULL},
    {&GUID_BACKGROUND_TASK_NOTIFICATION, NULL},
    {&GUID_APPLAUNCH_BUTTON, NULL},
    {&GUID_PCIEXPRESS_SETTINGS_SUBGROUP, NULL},
    {&GUID_PCIEXPRESS_ASPM_POLICY, NULL},
    {&GUID_ENABLE_SWITCH_FORCED_SHUTDOWN, NULL},
    {&PopIdleResiliencySettingGuid, PopStandbyStateSettingWorker},
    {&PopLowPowerPhaseSettingGuid, PopStandbyStateSettingWorker},
};

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief
 * Executes the driver's supplied callback function due to a change
 * in a power setting.
 *
 * @param[in] SettingCallback
 * The internal power setting callback buffer of which Power Manager
 * invokes the driver's callback from it.
 *
 * @param[in] SettingValue
 * A pointer to arbitrary value that points to the data of the power
 * setting value that changed.
 *
 * @param[in] SettingValueLength
 * The total length size of the power setting value, in bytes.
 */
static
VOID
PopExecutePowerSettingCallback(
    _In_ PPOP_POWER_SETTING_CALLBACK SettingCallback,
    _In_ PVOID SettingValue,
    _In_ ULONG SettingValueLength)
{
    NTSTATUS Status;
    PAGED_CODE();

    /*
     * This power setting is notified, acknowledge the Power Manager
     * that this power setting is entering into the driver's callback
     * to deliver the state change of the power setting.
     */
    PopPowerSettingClearAttribute(SettingCallback, POP_PSC_GETTING_NOTIFIED);
    PopPowerSettingApplyAttribute(SettingCallback, POP_PSC_ENTERING_CALLBACK);

    /* Execute the driver's supplied callback */
    Status = SettingCallback->Callback(&SettingCallback->SettingGuid,
                                       SettingValue,
                                       SettingValueLength,
                                       SettingCallback->Context);

    /* Log to the debugger what happened with the callback */
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("The driver's callback of power setting 0x%p has failed (Status 0x%lx)\n", SettingCallback, Status);
    }

    /* Operation done, alert the Power Manager this power setting has finished its job */
    PopPowerSettingClearAttribute(SettingCallback, POP_PSC_ENTERING_CALLBACK);
    PopAlertCallbackReturned(SettingCallback);
}

/* The power condition whose values the setting callbacks get */
static
SYSTEM_POWER_CONDITION
PopCurrentPowerCondition(VOID)
{
    /* Short-term batteries such as a UPS count as their own condition */
    if (PopCapabilities.BatteriesAreShortTerm)
        return PoHot;

    return (PopDefaultPowerPolicy == &PopAcPowerPolicy) ? PoAc : PoDc;
}

/**
 * @brief Reads whether the lid of a portable machine is open.
 *
 * @return FALSE when the machine has no lid.
 */
static
BOOLEAN
PopQueryLidState(
    _Out_ PULONG LidIsOpen)
{
    PPOP_CONTROL_SWITCH ControlSwitch;
    BOOLEAN LidFound = FALSE;
    PLIST_ENTRY Entry;

    PAGED_CODE();

    PopAcquirePowerPolicyLock();
    for (Entry = PopControlSwitches.Flink;
         Entry != &PopControlSwitches;
         Entry = Entry->Flink)
    {
        ControlSwitch = CONTAINING_RECORD(Entry, POP_CONTROL_SWITCH, Link);
        if (ControlSwitch->SwitchType == SwitchLid)
        {
            *LidIsOpen = ControlSwitch->Switch.Lid.Opened;
            LidFound = TRUE;
            break;
        }
    }
    PopReleasePowerPolicyLock();

    return LidFound;
}

/* A machine running from AC, or one whose battery reports no capacity, counts as full */
static
ULONG
PopQueryBatteryRemaining(VOID)
{
    ULONG CurrentCapacity, MaxCapacity;

    if (PopDefaultPowerPolicy == &PopAcPowerPolicy || !PopBattery)
        return 100;

    CurrentCapacity = PopBattery->Status.Capacity;
    MaxCapacity = PopBattery->BattInfo.FullChargedCapacity;
    if (!MaxCapacity || CurrentCapacity > MaxCapacity)
        return 100;

    return (ULONG)((100ULL * CurrentCapacity + MaxCapacity / 2) / MaxCapacity);
}

/**
 * @brief
 * Main worker thread for the GUID_ACDC_POWER_SOURCE power setting.
 * It determines the currently enforced power policy and sets the
 * new power condiition setting value to the registered driver.
 *
 * @param[in] StartContext
 * A pointer to a start argument context. This points to the setting
 * callback that has registered for this power setting.
 */
_Function_class_(KSTART_ROUTINE)
VOID
NTAPI
PopPowerSourceSettingWorker(
    _In_ PVOID StartContext)
{
    SYSTEM_POWER_CONDITION PowerCondition;
    PPOP_POWER_SETTING_CALLBACK SettingCallback = (PPOP_POWER_SETTING_CALLBACK)StartContext;

    PAGED_CODE();

    ASSERT(SettingCallback != NULL);

    /* Sanity check: the following worker can only work with the right GUID callbacks */
    ASSERT(IsEqualGUID(&SettingCallback->SettingGuid, &GUID_ACDC_POWER_SOURCE) == TRUE);

    PowerCondition = PopCurrentPowerCondition();

    /* Execute the callback now */
    PopExecutePowerSettingCallback(SettingCallback, &PowerCondition, sizeof(SYSTEM_POWER_CONDITION));
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/**
 * @brief
 * Worker for the display state settings. The console and session states and
 * the monitor power setting all read 0 while connected standby has the display
 * off and 1 while it is on.
 *
 * @param[in] StartContext
 * The setting callback to notify.
 */
_Function_class_(KSTART_ROUTINE)
VOID
NTAPI
PopDisplayStateSettingWorker(
    _In_ PVOID StartContext)
{
    PPOP_POWER_SETTING_CALLBACK SettingCallback = (PPOP_POWER_SETTING_CALLBACK)StartContext;
    ULONG DisplayOn;

    PAGED_CODE();

    ASSERT(SettingCallback != NULL);

    DisplayOn = PopAoAcInStandby ? 0 : 1;
    PopExecutePowerSettingCallback(SettingCallback, &DisplayOn, sizeof(DisplayOn));
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/**
 * @brief
 * Main worker thread for the GUID_LIDSWITCH_STATE_CHANGE power setting.
 * It determines whether the lid is open or closed of a portable machine,
 * such as a laptop.
 *
 * @param[in] StartContext
 * A pointer to a start argument context. This points to the setting
 * callback that has registered for this power setting.
 */
_Function_class_(KSTART_ROUTINE)
VOID
NTAPI
PopLidStateChangeSettingWorker(
    _In_ PVOID StartContext)
{
    ULONG LidIsOpen;
    PPOP_POWER_SETTING_CALLBACK SettingCallback = (PPOP_POWER_SETTING_CALLBACK)StartContext;

    PAGED_CODE();

    ASSERT(SettingCallback != NULL);

    /* Sanity check: the following worker can only work with the right GUID callbacks */
    ASSERT(IsEqualGUID(&SettingCallback->SettingGuid, &GUID_LIDSWITCH_STATE_CHANGE) == TRUE);

    /* Do not execute the driver's callback if no lid was found on this system */
    if (!PopQueryLidState(&LidIsOpen))
    {
        DPRINT("No lid found, the driver's callback won't be called\n");
        PopPowerSettingClearAttribute(SettingCallback, POP_PSC_GETTING_NOTIFIED);

        /* The registration waits for this worker to be done, callback or not */
        PopAlertCallbackReturned(SettingCallback);
        PsTerminateSystemThread(STATUS_SUCCESS);
        return;
    }

    /* Execute the callback now */
    PopExecutePowerSettingCallback(SettingCallback, &LidIsOpen, sizeof(ULONG));
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/**
 * @brief
 * Main worker thread for the GUID_BATTERY_PERCENTAGE_REMAINING power setting.
 * It determines the current percentage remaining of the battery capacity.
 *
 * @param[in] StartContext
 * A pointer to a start argument context. This points to the setting
 * callback that has registered for this power setting.
 */
_Function_class_(KSTART_ROUTINE)
VOID
NTAPI
PopBatteryRemainingSettingWorker(
    _In_ PVOID StartContext)
{
    ULONG CapacityRemaining;
    PPOP_POWER_SETTING_CALLBACK SettingCallback = (PPOP_POWER_SETTING_CALLBACK)StartContext;

    PAGED_CODE();

    ASSERT(SettingCallback != NULL);

    /* Sanity check: the following worker can only work with the right GUID callbacks */
    ASSERT(IsEqualGUID(&SettingCallback->SettingGuid, &GUID_BATTERY_PERCENTAGE_REMAINING) == TRUE);

    CapacityRemaining = PopQueryBatteryRemaining();

    /* Execute the callback now */
    PopExecutePowerSettingCallback(SettingCallback, &CapacityRemaining, sizeof(ULONG));
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Must be called with the value lock held */
static
PPOP_SETTING_VALUE_SLOT
PopFindSettingValueSlot(
    _In_ LPCGUID SettingGuid)
{
    PPOP_SETTING_VALUE_SLOT Slot;
    PLIST_ENTRY Entry;

    for (Entry = PopSettingValueList.Flink; Entry != &PopSettingValueList; Entry = Entry->Flink)
    {
        Slot = CONTAINING_RECORD(Entry, POP_SETTING_VALUE_SLOT, Link);
        if (IsEqualGUID(&Slot->SettingGuid, SettingGuid))
            return Slot;
    }

    return NULL;
}

/**
 * @brief
 * Stores the value of a setting for one power condition, as the power service
 * pushes it from the active power scheme.
 *
 * @return
 * STATUS_SUCCESS, also when the value did not change. Callbacks are notified
 * only for a new value of the condition the system runs on.
 */
NTSTATUS
NTAPI
PopSetPowerSettingValue(
    _In_ LPCGUID SettingGuid,
    _In_ SYSTEM_POWER_CONDITION Condition,
    _In_ ULONG DataLength,
    _In_reads_bytes_(DataLength) PVOID Data)
{
    PPOP_SETTING_VALUE_SLOT Slot;
    PUCHAR Copy, Previous = NULL;
    BOOLEAN Notify = FALSE;

    PAGED_CODE();

    if (Condition >= PoConditionMaximum || DataLength == 0)
        return STATUS_INVALID_PARAMETER;

    Copy = ExAllocatePoolWithTag(PagedPool, DataLength, TAG_PO_POWER_SETTING_VALUE);
    if (!Copy)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(Copy, Data, DataLength);

    ExAcquireFastMutex(&PopSettingValueLock);

    Slot = PopFindSettingValueSlot(SettingGuid);
    if (!Slot)
    {
        Slot = ExAllocatePoolZero(PagedPool, sizeof(*Slot), TAG_PO_POWER_SETTING_VALUE);
        if (!Slot)
        {
            ExReleaseFastMutex(&PopSettingValueLock);
            ExFreePoolWithTag(Copy, TAG_PO_POWER_SETTING_VALUE);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        Slot->SettingGuid = *SettingGuid;
        InsertTailList(&PopSettingValueList, &Slot->Link);
    }

    /* The same value again changes nothing */
    if (Slot->Data[Condition] &&
        Slot->DataLength[Condition] == DataLength &&
        RtlCompareMemory(Slot->Data[Condition], Copy, DataLength) == DataLength)
    {
        ExReleaseFastMutex(&PopSettingValueLock);
        ExFreePoolWithTag(Copy, TAG_PO_POWER_SETTING_VALUE);
        return STATUS_SUCCESS;
    }

    Previous = Slot->Data[Condition];
    Slot->Data[Condition] = Copy;
    Slot->DataLength[Condition] = DataLength;
    Notify = (Condition == PopCurrentPowerCondition());

    ExReleaseFastMutex(&PopSettingValueLock);

    if (Previous)
        ExFreePoolWithTag(Previous, TAG_PO_POWER_SETTING_VALUE);

    if (Notify)
        PopNotifyPowerSettingChange(SettingGuid);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Copies out the stored value of a setting for the power condition the system
 * runs on.
 *
 * @return
 * STATUS_NOT_FOUND when the power service has not set one yet.
 */
static
NTSTATUS
PopQuerySettingValue(
    _In_ LPCGUID SettingGuid,
    _Out_writes_bytes_to_(BufferLength, *DataLength) PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG DataLength)
{
    SYSTEM_POWER_CONDITION Condition = PopCurrentPowerCondition();
    PPOP_SETTING_VALUE_SLOT Slot;
    NTSTATUS Status = STATUS_NOT_FOUND;

    *DataLength = 0;

    ExAcquireFastMutex(&PopSettingValueLock);
    Slot = PopFindSettingValueSlot(SettingGuid);
    if (Slot && Slot->Data[Condition])
    {
        *DataLength = Slot->DataLength[Condition];
        if (Slot->DataLength[Condition] <= BufferLength)
        {
            RtlCopyMemory(Buffer, Slot->Data[Condition], Slot->DataLength[Condition]);
            Status = STATUS_SUCCESS;
        }
        else
        {
            Status = STATUS_BUFFER_TOO_SMALL;
        }
    }
    ExReleaseFastMutex(&PopSettingValueLock);

    return Status;
}

/* A power source change matters to a setting whose values for the two sources differ */
static
BOOLEAN
PopSettingDiffersAcrossConditions(
    _In_ LPCGUID SettingGuid)
{
    PPOP_SETTING_VALUE_SLOT Slot;
    BOOLEAN Differs = FALSE;
    ULONG Condition;

    ExAcquireFastMutex(&PopSettingValueLock);
    Slot = PopFindSettingValueSlot(SettingGuid);
    if (Slot)
    {
        for (Condition = PoDc; Condition < PoConditionMaximum && !Differs; Condition++)
        {
            if (!Slot->Data[Condition] || !Slot->Data[PoAc])
                continue;

            Differs = Slot->DataLength[Condition] != Slot->DataLength[PoAc] ||
                      RtlCompareMemory(Slot->Data[Condition], Slot->Data[PoAc], Slot->DataLength[PoAc]) !=
                      Slot->DataLength[PoAc];
        }
    }
    ExReleaseFastMutex(&PopSettingValueLock);

    return Differs;
}

/**
 * @brief
 * Worker for settings that only live in the power scheme. Until the power
 * service sets a value there is nothing to give, the callback runs once it does.
 */
_Function_class_(KSTART_ROUTINE)
VOID
NTAPI
PopSchemeSettingWorker(
    _In_ PVOID StartContext)
{
    PPOP_POWER_SETTING_CALLBACK SettingCallback = (PPOP_POWER_SETTING_CALLBACK)StartContext;
    UCHAR Small[sizeof(ULONG) * 4];
    PVOID Value = Small;
    ULONG Length;
    NTSTATUS Status;

    PAGED_CODE();

    ASSERT(SettingCallback != NULL);

    Status = PopQuerySettingValue(&SettingCallback->SettingGuid, Small, sizeof(Small), &Length);
    if (Status == STATUS_BUFFER_TOO_SMALL)
    {
        Value = ExAllocatePoolWithTag(PagedPool, Length, TAG_PO_POWER_SETTING_VALUE);
        Status = Value ? PopQuerySettingValue(&SettingCallback->SettingGuid, Value, Length, &Length)
                       : STATUS_INSUFFICIENT_RESOURCES;
    }

    if (NT_SUCCESS(Status))
    {
        PopExecutePowerSettingCallback(SettingCallback, Value, Length);
    }
    else
    {
        PopPowerSettingClearAttribute(SettingCallback, POP_PSC_GETTING_NOTIFIED);
        PopAlertCallbackReturned(SettingCallback);
    }

    if (Value && Value != Small)
        ExFreePoolWithTag(Value, TAG_PO_POWER_SETTING_VALUE);

    PsTerminateSystemThread(STATUS_SUCCESS);
}

/**
 * @brief
 * Worker for the connected standby state settings, 1 while the system is in
 * connected standby and 0 while it is not.
 */
_Function_class_(KSTART_ROUTINE)
VOID
NTAPI
PopStandbyStateSettingWorker(
    _In_ PVOID StartContext)
{
    PPOP_POWER_SETTING_CALLBACK SettingCallback = (PPOP_POWER_SETTING_CALLBACK)StartContext;
    ULONG InStandby;

    PAGED_CODE();

    ASSERT(SettingCallback != NULL);

    InStandby = PopAoAcInStandby ? 1 : 0;
    PopExecutePowerSettingCallback(SettingCallback, &InStandby, sizeof(InStandby));
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/**
 * @brief
 * Copies out the value a setting has right now, the live state for settings
 * the Power Manager tracks itself and the scheme value for all others.
 *
 * @return
 * STATUS_NOT_FOUND when the setting has no value yet.
 */
static
NTSTATUS
PopQueryCurrentSettingValue(
    _In_ LPCGUID SettingGuid,
    _Out_writes_bytes_to_(BufferLength, *DataLength) PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG DataLength)
{
    PKSTART_ROUTINE Helper = PopGetPowerSettingHelper(SettingGuid);
    ULONG Value;

    PAGED_CODE();

    if (Helper == PopPowerSourceSettingWorker)
        Value = PopCurrentPowerCondition();
    else if (Helper == PopDisplayStateSettingWorker)
        Value = PopAoAcInStandby ? 0 : 1;
    else if (Helper == PopStandbyStateSettingWorker)
        Value = PopAoAcInStandby ? 1 : 0;
    else if (Helper == PopBatteryRemainingSettingWorker)
        Value = PopQueryBatteryRemaining();
    else if (Helper == PopLidStateChangeSettingWorker)
    {
        if (!PopQueryLidState(&Value))
        {
            *DataLength = 0;
            return STATUS_NOT_FOUND;
        }
    }
    else
        return PopQuerySettingValue(SettingGuid, Buffer, BufferLength, DataLength);

    *DataLength = sizeof(Value);
    if (BufferLength < sizeof(Value))
        return STATUS_BUFFER_TOO_SMALL;

    RtlCopyMemory(Buffer, &Value, sizeof(Value));
    return STATUS_SUCCESS;
}

/* Must be called with the notification name lock held */
static
PPOP_SETTING_WNF_NAME
PopFindSettingWnfName(
    _In_ LPCGUID SettingGuid)
{
    PPOP_SETTING_WNF_NAME Name;
    PLIST_ENTRY Entry;

    for (Entry = PopSettingWnfNames.Flink; Entry != &PopSettingWnfNames; Entry = Entry->Flink)
    {
        Name = CONTAINING_RECORD(Entry, POP_SETTING_WNF_NAME, Link);
        if (IsEqualGUID(&Name->SettingGuid, SettingGuid))
            return Name;
    }

    return NULL;
}

/**
 * @brief
 * Publishes the current value of a setting under its notification name.
 * The same value is not published twice in a row.
 *
 * @remarks
 * Must be called with the notification name lock held.
 */
static
VOID
PopPublishSettingWnfName(
    _Inout_ PPOP_SETTING_WNF_NAME Name)
{
    UCHAR Small[sizeof(GUID)];
    PUCHAR Value = Small, Copy;
    ULONG Length;
    NTSTATUS Status;

    PAGED_CODE();

    Status = PopQueryCurrentSettingValue(&Name->SettingGuid, Small, sizeof(Small), &Length);
    if (Status == STATUS_BUFFER_TOO_SMALL)
    {
        Value = ExAllocatePoolWithTag(PagedPool, Length, TAG_PO_POWER_SETTING_VALUE);
        Status = Value ? PopQueryCurrentSettingValue(&Name->SettingGuid, Value, Length, &Length)
                       : STATUS_INSUFFICIENT_RESOURCES;
    }

    if (!NT_SUCCESS(Status) || Length == 0 || Length > WNF_MAXIMUM_DATA_SIZE)
        goto Quit;

    if (Name->LastValue &&
        Name->LastLength == Length &&
        RtlCompareMemory(Name->LastValue, Value, Length) == Length)
    {
        goto Quit;
    }

    Copy = ExAllocatePoolWithTag(PagedPool, Length, TAG_PO_POWER_SETTING_VALUE);
    if (!Copy)
        goto Quit;
    RtlCopyMemory(Copy, Value, Length);

    Status = ZwUpdateWnfStateData(&Name->StateName, Value, Length, NULL, NULL, 0, FALSE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Publishing a power setting failed (Status 0x%lx)\n", Status);
        ExFreePoolWithTag(Copy, TAG_PO_POWER_SETTING_VALUE);
        goto Quit;
    }

    if (Name->LastValue)
        ExFreePoolWithTag(Name->LastValue, TAG_PO_POWER_SETTING_VALUE);
    Name->LastValue = Copy;
    Name->LastLength = Length;

Quit:
    if (Value && Value != Small)
        ExFreePoolWithTag(Value, TAG_PO_POWER_SETTING_VALUE);
}

/* Settings nobody asked a notification name for are skipped */
static
VOID
PopPublishSettingNotification(
    _In_ LPCGUID SettingGuid)
{
    PPOP_SETTING_WNF_NAME Name;

    PAGED_CODE();

    if (!PopSettingWnfNameCount)
        return;

    ExAcquireFastMutex(&PopSettingWnfLock);
    Name = PopFindSettingWnfName(SettingGuid);
    if (Name)
        PopPublishSettingWnfName(Name);
    ExReleaseFastMutex(&PopSettingWnfLock);
}

/* A power source change can change the value of every setting with a notification name */
static
VOID
PopPublishAllSettingNotifications(VOID)
{
    PLIST_ENTRY Entry;

    PAGED_CODE();

    if (!PopSettingWnfNameCount)
        return;

    ExAcquireFastMutex(&PopSettingWnfLock);
    for (Entry = PopSettingWnfNames.Flink; Entry != &PopSettingWnfNames; Entry = Entry->Flink)
        PopPublishSettingWnfName(CONTAINING_RECORD(Entry, POP_SETTING_WNF_NAME, Link));
    ExReleaseFastMutex(&PopSettingWnfLock);
}

/* A power source change reaches the scheme settings whose AC and DC values differ */
static
VOID
PopNotifySchemeSettings(VOID)
{
    PPOP_POWER_SETTING_CALLBACK SettingCallback, Earlier;
    PLIST_ENTRY Entry, Scan;
    BOOLEAN Seen;

    PAGED_CODE();

    PopAcquirePowerSettingLock();
    for (Entry = PopPowerSettingCallbacksList.Flink; Entry != &PopPowerSettingCallbacksList; Entry = Entry->Flink)
    {
        SettingCallback = CONTAINING_RECORD(Entry, POP_POWER_SETTING_CALLBACK, Link);
        if (PopGetPowerSettingHelper(&SettingCallback->SettingGuid) ||
            !PopSettingDiffersAcrossConditions(&SettingCallback->SettingGuid))
        {
            continue;
        }

        /* One notification per setting reaches all of its callbacks */
        Seen = FALSE;
        for (Scan = PopPowerSettingCallbacksList.Flink; Scan != Entry; Scan = Scan->Flink)
        {
            Earlier = CONTAINING_RECORD(Scan, POP_POWER_SETTING_CALLBACK, Link);
            if (IsEqualGUID(&Earlier->SettingGuid, &SettingCallback->SettingGuid))
            {
                Seen = TRUE;
                break;
            }
        }

        if (!Seen)
            PopNotifyPowerSettingChange(&SettingCallback->SettingGuid);
    }
    PopReleasePowerSettingLock();
}

/**
 * @brief
 * Main core worker thread of the power settings mechanism that deploys
 * the power setting workers for each registered callback for the specific
 * power setting GUID.
 *
 * @param[in] Parameter
 * A pointer to a parameter context. This points to the power setting GUID
 * that every power setting callback that has registered notifications for
 * this setting shall be notified.
 *
 * @remarks
 * A power setting might not be notified at all if the POP_PSC_UNREGISTERED bit
 * has been set, that is, the callback is being unregistered soon on behalf of
 * a request by the driver who registered it.
 *
 * Note that power setting callbacks are invoked asynchronously. The master deployer
 * services the threads for each callback to be invoked regardless if the said
 * callback workers still have to process the long-running callbacks.
 */
_Use_decl_annotations_
static
VOID
NTAPI
PopDeployPowerSettingWorkers(
    _In_ PVOID Parameter)
{
    NTSTATUS Status;
    GUID SettingGuid;
    PLIST_ENTRY ListHead, NextEntry;
    PPOP_POWER_SETTING_CALLBACK SettingCallback;
    PKSTART_ROUTINE PowerSettingWorker = NULL;
    PPOP_POWER_SETTING_NOTIFY_BLOCK NotifyBlock = (PPOP_POWER_SETTING_NOTIFY_BLOCK)Parameter;

    PAGED_CODE();

    ASSERT(NotifyBlock != NULL);

    /* Cache the setting GUID and free the notify block as we don't need it */
    RtlCopyMemory(&SettingGuid,
                  &NotifyBlock->NotifySettingGuid,
                  sizeof(GUID));
    PopFreePool(NotifyBlock, TAG_PO_POWER_SETTING_NOTIFY_BLOCK);

    /* User mode learns about the change through the notification name of the setting */
    if (IsEqualGUID(&SettingGuid, &GUID_ACDC_POWER_SOURCE))
        PopPublishAllSettingNotifications();
    else
        PopPublishSettingNotification(&SettingGuid);

    /* Iterate over the list of registered setting callbacks and look for those we must notify */
    PopAcquirePowerSettingLock();
    ListHead = &PopPowerSettingCallbacksList;
    for (NextEntry = ListHead->Flink;
         NextEntry != ListHead;
         NextEntry = NextEntry->Flink)
    {
        /* Check if this is the callback we must notify */
        SettingCallback = CONTAINING_RECORD(NextEntry, POP_POWER_SETTING_CALLBACK, Link);
        if (IsEqualGUID(&SettingCallback->SettingGuid, &SettingGuid))
        {
            /*
             * The following setting callback is marked for deletion.
             * In this case don't bother with it, go for the next setting callback.
             */
            if (PopPowerSettingReadAttribute(SettingCallback, POP_PSC_UNREGISTERED))
            {
                continue;
            }

            /* Mark this setting callback as it's getting notified soon */
            PopPowerSettingApplyAttribute(SettingCallback, POP_PSC_GETTING_NOTIFIED);

            /*
             * Retrieve the power setting worker to handle this callback and
             * submit it to the Power Manager to create a worker thread for it,
             * the kernel will execute the worker shortly.
             */
            PowerSettingWorker = PopGetPowerSettingHelper(&SettingGuid);

            /* Settings with no live state of their own take their value from the power scheme */
            if (PowerSettingWorker == NULL)
                PowerSettingWorker = PopSchemeSettingWorker;

            /*
             * Now service this power setting callback with a power manager
             * worker thread. We're putting it at a highest priority as the
             * driver who has registered for power setting notifications
             * must be notified as soon as possible at the time of a state
             * in change of a power setting.
             */
            Status = PopCreateWorkerThread(PowerSettingWorker,
                                           SettingCallback,
                                           POP_POWER_SETTING_WORKER_THREAD_PRIORITY);
            if (!NT_SUCCESS(Status))
            {
                /*
                 * Failing to service it with a power manager worker thread could
                 * lead to bugs like drivers never being notified of power setting
                 * changes. Unfabricate the system!
                 */
                KeBugCheckEx(INTERNAL_POWER_ERROR,
                             0,
                             POP_POWER_SETTING_CALLBACK_THREAD_CREATION_FAILURE,
                             (ULONG_PTR)&SettingGuid,
                             (ULONG_PTR)SettingCallback);
            }
        }
    }

    /* We're done deploying power setting workers */
    PopReleasePowerSettingLock();

    if (IsEqualGUID(&SettingGuid, &GUID_ACDC_POWER_SOURCE))
        PopNotifySchemeSettings();
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Creates a power setting callback.
 *
 * @param[in] DeviceObject
 * A pointer to a device object that wants to register a callback to receive
 * notifications of changes in a power setting. T
 *
 * @param[in] SettingGuid
 * A pointer to a power setting GUID that identifies the power setting in question.
 *
 * @param[in] Callback
 * A pointer to a driver's supplied callback. The Power Manager calls this callback
 * whenever the driver has to be notified.
 *
 * @param[in] Context
 * A pointer to a parameter context that is passed to the callback routine.
 * This parameter is optional.
 *
 * @param[out] PowerSettingCallback
 * A pointer to a power setting callback that's been created, returned to the caller.
 *
 * @return
 * Returns STATUS_SUCCESS if the power setting callback has been created successfully.
 * STATUS_INSUFFICIENT_RESOURCES is returned if there's no enough memory to be
 * allocated for the power setting callback.
 */
NTSTATUS
NTAPI
PopAllocatePowerSettingCallback(
    _In_opt_ PDEVICE_OBJECT DeviceObject,
    _In_ LPCGUID SettingGuid,
    _In_ PPOWER_SETTING_CALLBACK Callback,
    _In_opt_ PVOID Context,
    _Out_ PPOP_POWER_SETTING_CALLBACK *PowerSettingCallback)
{
    PPOP_POWER_SETTING_CALLBACK SettingCallback;

    /* The calling thread must own the lock in order to register callbacks */
    POP_ASSERT_POWER_SETTING_LOCK_THREAD_OWNERSHIP();

    /* Allocate some memory pool for the power setting callback */
    SettingCallback = PopAllocatePool(sizeof(*SettingCallback),
                                      TRUE,
                                      TAG_PO_POWER_SETTING_CALLBACK);
    if (SettingCallback == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Setup the callback fields */
    SettingCallback->DeviceObject = DeviceObject;
    SettingCallback->Callback = Callback;
    SettingCallback->Context = Context;
    SettingCallback->Flags |= POP_PSC_REGISTERED;

    /* Initialize the link entry */
    InitializeListHead(&SettingCallback->Link);

    /* Copy the power setting GUID identifier */
    RtlCopyMemory(&SettingCallback->SettingGuid,
                  SettingGuid,
                  sizeof(GUID));

    /* Initialize the callback return event */
    KeInitializeEvent(&SettingCallback->CallbackReturned,
                      SynchronizationEvent,
                      FALSE);

    /* Give the allocated power setting callback to the caller */
    *PowerSettingCallback = SettingCallback;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Frees a power setting callback from memory.
 *
 * @param[in] PowerSettingCallback
 * A pointer to a power setting callback to be freed.
 */
VOID
NTAPI
PopReleasePowerSettingCallback(
    _In_ _Post_invalid_ PPOP_POWER_SETTING_CALLBACK PowerSettingCallback)
{
    /* The calling thread must own the lock in order to unregister callbacks */
    POP_ASSERT_POWER_SETTING_LOCK_THREAD_OWNERSHIP();

    /* Unlink the registered callback from the list */
    RemoveEntryList(&PowerSettingCallback->Link);

    /* And punt the callback from memory */
    PopFreePool(PowerSettingCallback, TAG_PO_POWER_SETTING_CALLBACK);
}

/**
 * @brief
 * Looks for the appropriate power setting worker routine to be used
 * to notify the callback.
 *
 * @param[in] SettingGuid
 * A pointer to a power setting GUID that identifies the said setting.
 *
 * @return
 * Returns a pointer to the power setting routine.
 */
PKSTART_ROUTINE
NTAPI
PopGetPowerSettingHelper(
    _In_ LPCGUID SettingGuid)
{
    ULONG SettingIndex;

    /* Iterate over the power settings database and look for the right setting */
    for (SettingIndex = 0;
         SettingIndex < POP_MAX_POWER_SETTINGS;
         SettingIndex++)
    {
        /* Is this what the caller asked for? */
        if (IsEqualGUID(PopPowerSettingsDatabase[SettingIndex].SettingGuid, SettingGuid))
        {
            return PopPowerSettingsDatabase[SettingIndex].SettingRoutine;
        }
    }

    return NULL;
}

/**
 * @brief
 * Notifies every power setting callback that has registered to receive
 * notifications for changes in a power setting pointed by the setting GUID.
 *
 * @param[in] SettingGuid
 * A pointer to a power setting GUID that identifies the said setting.
 * The master worker will notify the callbacks registered for this GUID.
 *
 * @remarks
 * The reason the master worker is handled by a Executive worker item thread
 * and not by creating a system thread just straight away is because we want
 * to notify changes of power setting(s) in all cases when we are in a higher
 * IRQL, DISPATCH_LEVEL respectively. Creating a system thread for the master
 * worker at DISPATCH_LEVEL is simply not possible.
 *
 * Cases for the Power Manager to enter in a state of being in DISPATCH_LEVEL
 * is not to be ruled out.
 */
VOID
NTAPI
PopNotifyPowerSettingChange(
    _In_ LPCGUID SettingGuid)
{
    PPOP_POWER_SETTING_NOTIFY_BLOCK NotifyBlock;

    /*
     * There's no active power setting callback occuring and user mode has no
     * notification name to publish to. It's pointless to bother the master worker.
     */
    if (PopPowerSettingCallbacksCount == 0 && PopSettingWnfNameCount == 0)
    {
        ASSERT(IsListEmpty(&PopPowerSettingCallbacksList));
        return;
    }

    /*
     * The list is not empty, allocate a notify block and let the master
     * worker to deploy power setting workers specifically for this GUID.
     */
    NotifyBlock = PopAllocatePool(sizeof(*NotifyBlock),
                                  FALSE,
                                  TAG_PO_POWER_SETTING_NOTIFY_BLOCK);
    if (NotifyBlock == NULL)
    {
        /*
         * Failing to allocate a notification block for this setting GUID
         * could lead to bugs like power settings never being notified.
         * Unfabricate the system!
         */
        KeBugCheckEx(INTERNAL_POWER_ERROR,
                     0,
                     POP_POWER_SETTING_CALLBACK_DEPLOY_WORKERS_FAILURE,
                     (ULONG_PTR)SettingGuid,
                     0);
    }

    /* Copy the power setting GUID of which all the respective callbacks must be notified */
    RtlCopyMemory(&NotifyBlock->NotifySettingGuid,
                  SettingGuid,
                  sizeof(GUID));

    /*
     * Setup the notification item and invoke the master worker
     * to deploy power setting workers for this GUID.
     */
    ExInitializeWorkItem(&NotifyBlock->NotifyWorkItem,
                         PopDeployPowerSettingWorkers,
                         NotifyBlock);
    ExQueueWorkItem(&NotifyBlock->NotifyWorkItem, CriticalWorkQueue);
}

/**
 * @brief
 * Returns the notification name user mode subscribes to for changes of a
 * setting. The data published under it is the current value of the setting.
 *
 * @remarks
 * A new name gets the current value right away, if the setting has one, so
 * the first subscriber is told about it without waiting for a change.
 */
NTSTATUS
NTAPI
PopGetSettingNotificationName(
    _In_ LPCGUID SettingGuid,
    _Out_ PULONGLONG StateName)
{
    PPOP_SETTING_WNF_NAME Name;
    NTSTATUS Status = STATUS_SUCCESS;

    PAGED_CODE();

    ExAcquireFastMutex(&PopSettingWnfLock);

    Name = PopFindSettingWnfName(SettingGuid);
    if (!Name)
    {
        Name = ExAllocatePoolZero(PagedPool, sizeof(*Name), TAG_PO_POWER_SETTING_VALUE);
        if (!Name)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Quit;
        }

        Status = ZwCreateWnfStateName(&Name->StateName,
                                      WnfTemporaryStateName,
                                      WnfDataScopeMachine,
                                      FALSE,
                                      NULL,
                                      WNF_MAXIMUM_DATA_SIZE,
                                      NULL);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(Name, TAG_PO_POWER_SETTING_VALUE);
            goto Quit;
        }

        Name->SettingGuid = *SettingGuid;
        InsertTailList(&PopSettingWnfNames, &Name->Link);
        InterlockedIncrement(&PopSettingWnfNameCount);
        PopPublishSettingWnfName(Name);
    }

    *StateName = Name->StateName;

Quit:
    ExReleaseFastMutex(&PopSettingWnfLock);
    return Status;
}

/* EOF */
