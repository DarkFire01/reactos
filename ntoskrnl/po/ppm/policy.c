/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Processor Power Management processor policies
 * COPYRIGHT:   Copyright 2023 George Bișoc <george.bisoc@reactos.org>
 *              Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

PPM_POLICY PpmPolicy;

/* Processor settings the SDK headers do not define */
static const GUID PpmBoostModeSettingGuid =
    {0xbe337238, 0x0d82, 0x4146, {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}};
static const GUID PpmLatencyHintSettingGuid =
    {0x0822df31, 0x9c83, 0x441c, {0xa0, 0x79, 0x0d, 0xe4, 0xcf, 0x00, 0x9c, 0x7b}};
static const GUID PpmEnergyPerfPreferenceSettingGuid =
    {0x36687f9e, 0xe3a5, 0x4dbf, {0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x63}};
static const GUID PpmParkingConcurrencySettingGuid =
    {0x2430ab6f, 0xa520, 0x44a2, {0x96, 0x01, 0xf7, 0xf2, 0x3b, 0x51, 0x34, 0xb1}};
static const GUID PpmParkingHeadroomSettingGuid =
    {0xf735a673, 0x2066, 0x4f80, {0xa0, 0xc5, 0xdd, 0xee, 0x0c, 0xf1, 0xbf, 0x5d}};
static const GUID PpmDistributeUtilitySettingGuid =
    {0xe0007330, 0xf589, 0x42ed, {0xa4, 0x01, 0x5d, 0xdb, 0x10, 0xe7, 0x85, 0xd3}};

static LPCGUID const PpmPerfSettings[] =
{
    &GUID_PROCESSOR_THROTTLE_MAXIMUM,
    &GUID_PROCESSOR_THROTTLE_MINIMUM,
    &GUID_PROCESSOR_PERF_INCREASE_THRESHOLD,
    &GUID_PROCESSOR_PERF_DECREASE_THRESHOLD,
    &GUID_PROCESSOR_PERF_INCREASE_POLICY,
    &GUID_PROCESSOR_PERF_DECREASE_POLICY,
    &GUID_PROCESSOR_PERF_INCREASE_TIME,
    &GUID_PROCESSOR_PERF_DECREASE_TIME,
    &GUID_PROCESSOR_PERF_TIME_CHECK,
    &GUID_PROCESSOR_PERF_BOOST_POLICY,
    &PpmBoostModeSettingGuid,
    &GUID_PROCESSOR_CORE_PARKING_INCREASE_THRESHOLD,
    &GUID_PROCESSOR_CORE_PARKING_DECREASE_THRESHOLD,
    &GUID_PROCESSOR_CORE_PARKING_INCREASE_POLICY,
    &GUID_PROCESSOR_CORE_PARKING_DECREASE_POLICY,
    &GUID_PROCESSOR_CORE_PARKING_MAX_CORES,
    &GUID_PROCESSOR_CORE_PARKING_MIN_CORES,
    &GUID_PROCESSOR_CORE_PARKING_INCREASE_TIME,
    &GUID_PROCESSOR_CORE_PARKING_DECREASE_TIME,
    &GUID_PROCESSOR_CORE_PARKING_OVER_UTILIZATION_THRESHOLD,
    &PpmParkingConcurrencySettingGuid,
    &PpmParkingHeadroomSettingGuid,
    &GUID_PROCESSOR_PARKING_PERF_STATE,
    &GUID_PROCESSOR_ALLOW_THROTTLING,
    &GUID_PROCESSOR_PERF_HISTORY,
    &PpmLatencyHintSettingGuid,
    &PpmDistributeUtilitySettingGuid,
    &PpmEnergyPerfPreferenceSettingGuid,
};

static LPCGUID const PpmIdleSettings[] =
{
    &GUID_PROCESSOR_IDLE_ALLOW_SCALING,
    &GUID_PROCESSOR_IDLE_DISABLE,
    &GUID_PROCESSOR_IDLE_TIME_CHECK,
    &GUID_PROCESSOR_IDLE_DEMOTE_THRESHOLD,
    &GUID_PROCESSOR_IDLE_PROMOTE_THRESHOLD,
};

/* PRIVATE FUNCTIONS **********************************************************/

static
ULONG
PpmClamp(
    _In_ ULONG Value,
    _In_ ULONG Low,
    _In_ ULONG High)
{
    return min(max(Value, Low), High);
}

static
VOID
PpmRecalculateAllDomains(VOID)
{
    PLIST_ENTRY Entry;

    for (Entry = PpmDriverDomainList.Flink; Entry != &PpmDriverDomainList; Entry = Entry->Flink)
        PpmCalculateMinMax(CONTAINING_RECORD(Entry, PPM_DRIVER_DOMAIN, Link));
}

/**
 * @brief Takes a new value of a processor performance setting.
 *
 * @return STATUS_INVALID_PARAMETER for an unknown setting or a value of the wrong size.
 */
_Function_class_(POWER_SETTING_CALLBACK)
static
NTSTATUS
NTAPI
PpmPerfPolicyCallback(
    _In_ LPCGUID SettingGuid,
    _In_reads_bytes_(ValueLength) PVOID Value,
    _In_ ULONG ValueLength,
    _Inout_opt_ PVOID Context)
{
    BOOLEAN Domains = FALSE, Boost = FALSE, ReInit = FALSE, Preference = FALSE;
    BOOLEAN Exact, AtLeast;
    ULONG TimeCheck, Data;

    UNREFERENCED_PARAMETER(Context);

    PAGED_CODE();

    PpmAcquirePolicyLock();
    if (!Value || !ValueLength)
    {
        PpmReleasePolicyLock();
        return STATUS_INVALID_PARAMETER;
    }

    /* The parking settings accept longer values, everything else must be a ULONG */
    Exact = (ValueLength == sizeof(ULONG));
    AtLeast = (ValueLength >= sizeof(ULONG));
    Data = AtLeast ? *(PULONG)Value : 0;
    TimeCheck = PpmPolicy.PerfTimeCheck;

    if (Exact && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_THROTTLE_MAXIMUM))
    {
        PpmPolicy.PerfMaxPolicy = (UCHAR)min(Data, 100);
        PpmPolicy.ThrottlePolicyPushed = TRUE;
        Domains = TRUE;
    }
    else if (Exact && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_THROTTLE_MINIMUM))
    {
        PpmPolicy.PerfMinPolicy = (UCHAR)min(Data, 100);
        PpmPolicy.ThrottlePolicyPushed = TRUE;
        Domains = TRUE;
    }
    else if (Exact && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_PERF_INCREASE_THRESHOLD))
    {
        PpmPolicy.PerfIncreaseThreshold = (UCHAR)PpmClamp(Data, 1, 100);
    }
    else if (Exact && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_PERF_DECREASE_THRESHOLD))
    {
        PpmPolicy.PerfDecreaseThreshold = (UCHAR)PpmClamp(Data, 1, 100);
    }
    else if (Exact && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_PERF_INCREASE_POLICY))
    {
        PpmPolicy.PerfIncreasePolicy = (Data > 2) ? 0 : (UCHAR)Data;
    }
    else if (Exact && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_PERF_DECREASE_POLICY))
    {
        PpmPolicy.PerfDecreasePolicy = (Data > 2) ? 0 : (UCHAR)Data;
    }
    else if (Exact && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_PERF_INCREASE_TIME))
    {
        PpmPolicy.PerfIncreaseTime = PpmClamp(Data, 1, 100);
    }
    else if (Exact && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_PERF_DECREASE_TIME))
    {
        PpmPolicy.PerfDecreaseTime = PpmClamp(Data, 1, 100);
    }
    else if (Exact && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_PERF_TIME_CHECK))
    {
        PpmPolicy.PerfTimeCheck = PpmClamp(Data, 1, 5000);
    }
    else if (Exact && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_PERF_BOOST_POLICY))
    {
        PpmPolicy.PerfBoostPolicy = min(Data, 100);
        Boost = TRUE;
    }
    else if (Exact && IsEqualGUID(SettingGuid, &PpmBoostModeSettingGuid))
    {
        PpmPolicy.PerfBoostMode = min(Data, PPM_BOOST_MODE_EFFICIENT_AGGRESSIVE);
        Boost = TRUE;
        Domains = TRUE;
    }
    else if (Exact && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_ALLOW_THROTTLING))
    {
        PpmPolicy.AllowThrottling = (Data != 0);
        Domains = TRUE;
    }
    else if (Exact && IsEqualGUID(SettingGuid, &PpmEnergyPerfPreferenceSettingGuid))
    {
        PpmPolicy.EnergyPerfPreference = min(Data, 100);
        PpmPolicy.EnergyPerfPreferenceSet = TRUE;
        Preference = TRUE;
    }
    else if (AtLeast && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_CORE_PARKING_INCREASE_THRESHOLD))
    {
        PpmPolicy.CoreParkingIncreaseThreshold = (UCHAR)min(Data, 100);
    }
    else if (AtLeast && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_CORE_PARKING_DECREASE_THRESHOLD))
    {
        PpmPolicy.CoreParkingDecreaseThreshold = (UCHAR)min(Data, 100);
    }
    else if (AtLeast && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_CORE_PARKING_INCREASE_POLICY))
    {
        PpmPolicy.CoreParkingIncreasePolicy = (UCHAR)min(Data, 3);
    }
    else if (AtLeast && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_CORE_PARKING_DECREASE_POLICY))
    {
        PpmPolicy.CoreParkingDecreasePolicy = (UCHAR)min(Data, 3);
    }
    else if (AtLeast && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_CORE_PARKING_MAX_CORES))
    {
        PpmPolicy.CoreParkingMaxCores = (UCHAR)min(Data, 100);
        ReInit = TRUE;
    }
    else if (AtLeast && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_CORE_PARKING_MIN_CORES))
    {
        PpmPolicy.CoreParkingMinCores = (UCHAR)min(Data, 100);
        ReInit = TRUE;
    }
    else if (AtLeast && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_CORE_PARKING_INCREASE_TIME))
    {
        PpmPolicy.CoreParkingIncreaseTime = PpmClamp(Data, 1, 100);
    }
    else if (AtLeast && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_CORE_PARKING_DECREASE_TIME))
    {
        PpmPolicy.CoreParkingDecreaseTime = PpmClamp(Data, 1, 100);
    }
    else if (AtLeast && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_CORE_PARKING_OVER_UTILIZATION_THRESHOLD))
    {
        PpmPolicy.CoreParkingOverUtilizationThreshold = (UCHAR)PpmClamp(Data, 5, 100);
    }
    else if (AtLeast && IsEqualGUID(SettingGuid, &PpmParkingConcurrencySettingGuid))
    {
        PpmPolicy.CoreParkingConcurrencyThreshold = (UCHAR)PpmClamp(Data, 5, 100);
    }
    else if (AtLeast && IsEqualGUID(SettingGuid, &PpmParkingHeadroomSettingGuid))
    {
        PpmPolicy.CoreParkingHeadroomThreshold = (UCHAR)min(Data, 100);
    }
    else if (AtLeast && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_PARKING_PERF_STATE))
    {
        PpmPolicy.ParkingPerfState = (Data > 2) ? 0 : (UCHAR)Data;
    }
    else if (AtLeast && IsEqualGUID(SettingGuid, &GUID_PROCESSOR_PERF_HISTORY))
    {
        PpmPolicy.PerfHistoryCount = PpmClamp(Data, 1, 128);
        ReInit = TRUE;
    }
    else if (AtLeast && IsEqualGUID(SettingGuid, &PpmLatencyHintSettingGuid))
    {
        PpmPolicy.PerfLatencyHints = (Data != 0);
    }
    else if (AtLeast && IsEqualGUID(SettingGuid, &PpmDistributeUtilitySettingGuid))
    {
        PpmPolicy.DistributeUtility = (Data != 0);
    }
    else
    {
        PpmReleasePolicyLock();
        return STATUS_INVALID_PARAMETER;
    }

    if (Domains)
        PpmRecalculateAllDomains();

    if (Boost)
        PpmRunOnProcessors(PpmPerfRegistered, PpmApplyBoostPolicy, NULL);

    if (Preference)
        PpmRunOnProcessors(PpmPerfRegistered, PpmApplyEnergyPerfPreference, NULL);

    if (Domains || ReInit || TimeCheck != PpmPolicy.PerfTimeCheck)
        PpmCheckReInitialize();

    /* Reapplying the domains hands the lock to the sequence */
    if (Domains)
    {
        PpmUpdateAllDomains();
        return STATUS_SUCCESS;
    }

    PpmReleasePolicyLock();
    return STATUS_SUCCESS;
}

/**
 * @brief Takes a new value of a processor idle setting and reloads every processor.
 */
_Function_class_(POWER_SETTING_CALLBACK)
static
NTSTATUS
NTAPI
PpmIdlePolicyCallback(
    _In_ LPCGUID SettingGuid,
    _In_reads_bytes_(ValueLength) PVOID Value,
    _In_ ULONG ValueLength,
    _Inout_opt_ PVOID Context)
{
    ULONG Data;

    UNREFERENCED_PARAMETER(Context);

    PAGED_CODE();

    PpmAcquireIdlePolicyLock();
    if (!Value || ValueLength != sizeof(ULONG))
    {
        PpmReleaseIdlePolicyLock();
        return STATUS_INVALID_PARAMETER;
    }

    Data = *(PULONG)Value;
    if (IsEqualGUID(SettingGuid, &GUID_PROCESSOR_IDLE_ALLOW_SCALING))
    {
        PpmPolicy.IdleAllowScaling = (Data != 0);
    }
    else if (IsEqualGUID(SettingGuid, &GUID_PROCESSOR_IDLE_DISABLE))
    {
        PpmPolicy.IdleDisable = (Data != 0);
    }
    else if (IsEqualGUID(SettingGuid, &GUID_PROCESSOR_IDLE_TIME_CHECK))
    {
        PpmPolicy.IdleTimeCheck = PpmClamp(Data, 1, 200000);
    }
    else if (IsEqualGUID(SettingGuid, &GUID_PROCESSOR_IDLE_DEMOTE_THRESHOLD))
    {
        PpmPolicy.IdleDemotePercent = (UCHAR)PpmClamp(Data, 1, 100);
    }
    else if (IsEqualGUID(SettingGuid, &GUID_PROCESSOR_IDLE_PROMOTE_THRESHOLD))
    {
        PpmPolicy.IdlePromotePercent = (UCHAR)PpmClamp(Data, 1, 100);
    }
    else
    {
        PpmReleaseIdlePolicyLock();
        return STATUS_INVALID_PARAMETER;
    }

    PpmReapplyIdlePolicy();
    PpmReleaseIdlePolicyLock();
    return STATUS_SUCCESS;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief Loads the boot policy, every domain runs at its maximum until a scheme lowers the minimum.
 */
VOID
NTAPI
PpmPolicyInitialize(VOID)
{
    RtlZeroMemory(&PpmPolicy, sizeof(PpmPolicy));
    PpmPolicy.PerfTimeCheck = 50;
    PpmPolicy.PerfHistoryCount = 1;
    PpmPolicy.PerfIncreaseThreshold = 70;
    PpmPolicy.PerfDecreaseThreshold = 30;
    PpmPolicy.PerfIncreasePolicy = 0;
    PpmPolicy.PerfDecreasePolicy = 0;
    PpmPolicy.PerfIncreaseTime = 0;
    PpmPolicy.PerfDecreaseTime = 0;
    PpmPolicy.PerfMinPolicy = 100;
    PpmPolicy.PerfMaxPolicy = 100;
    PpmPolicy.PerfBoostPolicy = 100;
    PpmPolicy.PerfBoostMode = PPM_BOOST_MODE_AGGRESSIVE;
    PpmPolicy.AllowThrottling = FALSE;
    PpmPolicy.ParkingPerfState = 0;
    PpmPolicy.PerfLatencyHints = FALSE;
    PpmPolicy.IdleTimeCheck = 50000;
    PpmPolicy.IdlePromotePercent = 60;
    PpmPolicy.IdleDemotePercent = 40;
    PpmPolicy.IdleAllowScaling = FALSE;
    PpmPolicy.IdleDisable = FALSE;
}

/**
 * @brief Uses the Balanced throttle range when the active scheme carried none.
 *
 * @remarks Called with the policy lock held.
 */
VOID
NTAPI
PpmApplyDefaultScheme(VOID)
{
    PAGED_CODE();

    PpmPolicy.PerfMinPolicy = 5;
    PpmPolicy.PerfMaxPolicy = 100;
    PpmRecalculateAllDomains();
    PpmCheckReInitialize();

    DPRINT1("PPM: no processor throttle policy was pushed, using Balanced %u-%u%%\n",
            PpmPolicy.PerfMinPolicy, PpmPolicy.PerfMaxPolicy);
}

/**
 * @brief Registers for the processor settings of the power scheme.
 */
VOID
NTAPI
PpmRegisterPolicyCallbacks(VOID)
{
    NTSTATUS Status;
    ULONG Index;

    PAGED_CODE();

    for (Index = 0; Index < RTL_NUMBER_OF(PpmPerfSettings); Index++)
    {
        Status = PoRegisterPowerSettingCallback(NULL, PpmPerfSettings[Index], PpmPerfPolicyCallback, NULL, NULL);
        if (!NT_SUCCESS(Status))
            DPRINT1("PPM: perf setting %lu not registered (0x%lx)\n", Index, Status);
    }

    for (Index = 0; Index < RTL_NUMBER_OF(PpmIdleSettings); Index++)
    {
        Status = PoRegisterPowerSettingCallback(NULL, PpmIdleSettings[Index], PpmIdlePolicyCallback, NULL, NULL);
        if (!NT_SUCCESS(Status))
            DPRINT1("PPM: idle setting %lu not registered (0x%lx)\n", Index, Status);
    }
}

/* EOF */
