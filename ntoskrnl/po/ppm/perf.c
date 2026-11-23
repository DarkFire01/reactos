/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Processor Power Management performance handling
 * COPYRIGHT:   Copyright 2023 George Bișoc <george.bisoc@reactos.org>
 *              Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Serializes processor registration and every performance request to the plug-in */
static KMUTEX PpmPerfLock;

static PPM_PEP_PROCESSOR PpmPepProcessors[MAXIMUM_PROCESSORS];
static ULONG PpmPepProcessorCount;
static volatile LONG PpmPepProcessorsPending;
static BOOLEAN PpmPepFailureLogged;
static BOOLEAN PpmPepRegistrationAllowed;

static PPM_PERF_DOMAIN PpmPerfDomains[MAXIMUM_PROCESSORS];
static ULONG PpmPerfDomainCount;
static BOOLEAN PpmPerfInStandby;

/* PRIVATE FUNCTIONS **********************************************************/

static
VOID
PpmAcquirePerfLock(VOID)
{
    KeWaitForSingleObject(&PpmPerfLock, Executive, KernelMode, FALSE, NULL);
}

static
VOID
PpmReleasePerfLock(VOID)
{
    KeReleaseMutex(&PpmPerfLock, FALSE);
}

/**
 * @brief Maps an ACPI processor device to a processor number through its instance, the _UID.
 *
 * @return The processor number, MAXULONG when the _UID names no running processor.
 */
static
ULONG
PpmProcessorNumberFromInstance(
    _In_ PCUNICODE_STRING InstancePath)
{
    UNICODE_STRING Instance;
    USHORT Start;
    ULONG Number;

    Start = InstancePath->Length / sizeof(WCHAR);
    while (Start > 0 && InstancePath->Buffer[Start - 1] != L'\\')
        Start--;

    Instance.Buffer = &InstancePath->Buffer[Start];
    Instance.Length = InstancePath->Length - Start * sizeof(WCHAR);
    Instance.MaximumLength = Instance.Length;

    if (!NT_SUCCESS(RtlUnicodeStringToInteger(&Instance, 10, &Number)) ||
        Number >= (ULONG)KeNumberProcessors)
    {
        return MAXULONG;
    }

    return Number;
}

/**
 * @brief Adds a processor to the domain the plug-in reported, creating the domain on first sight.
 *
 * @remarks The plug-in starts every domain at its nominal level, which becomes the first request.
 */
static
ULONG
PpmJoinPerfDomain(
    _In_ PPEP_PPM_QUERY_PERF_CAPABILITIES Capabilities)
{
    PPPM_PERF_DOMAIN Domain;
    ULONG Index;

    for (Index = 0; Index < PpmPerfDomainCount; Index++)
    {
        if (PpmPerfDomains[Index].DomainId == Capabilities->DomainId)
            return Index;
    }

    if (PpmPerfDomainCount == RTL_NUMBER_OF(PpmPerfDomains))
        return MAXULONG;

    Domain = &PpmPerfDomains[PpmPerfDomainCount];
    Domain->DomainId = Capabilities->DomainId;
    Domain->Members = Capabilities->DomainMembers;
    Domain->Highest = Capabilities->HighestPerformance;
    Domain->Nominal = Capabilities->NominalPerformance;
    Domain->LowestNonlinear = Capabilities->LowestNonlinearPerformance;
    Domain->Lowest = Capabilities->LowestPerformance;
    Domain->Requested.Minimum = Domain->Lowest;
    Domain->Requested.Maximum = Domain->Highest;
    Domain->Requested.Desired = Domain->Nominal;
    Domain->Current = Domain->Requested;

    DPRINT1("PPM: perf domain %lu, %lu processors, lowest %lu nominal %lu highest %lu\n",
            Domain->DomainId, Domain->Members, Domain->Lowest, Domain->Nominal, Domain->Highest);

    return PpmPerfDomainCount++;
}

/**
 * @brief Registers one processor device with the plug-in that prepares it and queries its performance.
 *
 * @return NULL on success, else why the processor got no performance control.
 */
static
PCSTR
PpmRegisterPepProcessor(
    _Inout_ PPPM_PEP_PROCESSOR Processor)
{
    PEP_PPM_QUERY_PERF_CAPABILITIES PerfCapabilities;
    PEP_PPM_QUERY_CAPABILITIES Capabilities;
    PEP_DEVICE_REGISTER_V2 Register;
    PEP_REGISTER_DEVICE_V2 Device;
    PEP_PREPARE_DEVICE Prepare;
    PPOP_FX_PLUGIN Plugin;

    if (!NT_SUCCESS(PopFxQueryDeviceId(Processor->Pdo, &Processor->DeviceId)))
        return "no device id";

    for (Plugin = PopFxNextPlugin(NULL); Plugin; Plugin = PopFxNextPlugin(Plugin))
    {
        if (!Plugin->AcceptProcessorNotification)
            continue;

        Prepare.DeviceId = &Processor->DeviceId;
        Prepare.DeviceAccepted = FALSE;
        if (PopFxNotifyPlugin(Plugin, PEP_DPM_PREPARE_DEVICE, &Prepare) && Prepare.DeviceAccepted)
            break;
    }

    if (!Plugin)
        return "not prepared";

    /* Processors carry no components, the plug-in only needs a kernel handle to call back with */
    RtlZeroMemory(&Register, sizeof(Register));
    RtlZeroMemory(&Device, sizeof(Device));
    Device.DeviceId = &Processor->DeviceId;
    Device.KernelHandle = (POHANDLE)Processor;
    Device.Register = &Register;
    if (!PopFxNotifyPlugin(Plugin, PEP_DPM_REGISTER_DEVICE, &Device) ||
        Device.DeviceAccepted != PepDeviceAccepted ||
        !Device.DeviceHandle)
    {
        return "not registered";
    }

    Processor->Plugin = Plugin;
    Processor->PepHandle = Device.DeviceHandle;

    /* The plug-in brings up a processor's performance tables when it answers this */
    RtlZeroMemory(&Capabilities, sizeof(Capabilities));
    if (!Plugin->AcceptProcessorNotification(Processor->PepHandle, PEP_NOTIFY_PPM_QUERY_CAPABILITIES, &Capabilities))
        return "no capabilities";

    if (!Capabilities.PerformanceStatesSupported)
        return "no performance states";

    RtlZeroMemory(&PerfCapabilities, sizeof(PerfCapabilities));
    if (!Plugin->AcceptProcessorNotification(Processor->PepHandle,
                                             PEP_NOTIFY_PPM_QUERY_PERF_CAPABILITIES,
                                             &PerfCapabilities) ||
        !PerfCapabilities.HighestPerformance ||
        PerfCapabilities.LowestPerformance > PerfCapabilities.HighestPerformance)
    {
        return "no performance range";
    }

    Processor->Domain = PpmJoinPerfDomain(&PerfCapabilities);
    if (Processor->Domain == MAXULONG)
        return "too many domains";

    return NULL;
}

/**
 * @brief Tells every processor of a domain its new performance request.
 *
 * @remarks The plug-in only records the request, PpmCompletePerfCheck makes it act on it.
 */
static
BOOLEAN
PpmSendPerfDomain(
    _In_ ULONG Domain,
    _In_ PPPM_PERF_REQUEST Request)
{
    PPPM_PEP_PROCESSOR Processor;
    PEP_PPM_PERF_SET PerfSet;
    BOOLEAN Sent = FALSE;
    ULONG Index;

    RtlZeroMemory(&PerfSet, sizeof(PerfSet));
    PerfSet.MinimumPerformance = Request->Minimum;
    PerfSet.MaximumPerformance = Request->Maximum;
    PerfSet.DesiredPerformance = Request->Desired;

    for (Index = 0; Index < PpmPepProcessorCount; Index++)
    {
        Processor = &PpmPepProcessors[Index];
        if (Processor->Domain != Domain)
            continue;

        if (!Processor->Plugin->AcceptProcessorNotification(Processor->PepHandle, PEP_NOTIFY_PPM_PERF_SET, &PerfSet))
            return FALSE;

        Sent = TRUE;
    }

    if (Sent)
        PpmPerfDomains[Domain].Current = *Request;

    return Sent;
}

/**
 * @brief Ends a round of performance requests, the plug-in changes the clocks from here.
 */
static
VOID
PpmCompletePerfCheck(VOID)
{
    PEP_PPM_PERF_CHECK_COMPLETE Complete;
    PPPM_PEP_PROCESSOR Processor;
    ULONG Index;

    for (Index = 0; Index < PpmPepProcessorCount; Index++)
    {
        Processor = &PpmPepProcessors[Index];
        if (Processor->Domain == MAXULONG)
            continue;

        Complete.EvaluationTime = KeQueryInterruptTime();
        Processor->Plugin->AcceptProcessorNotification(Processor->PepHandle,
                                                       PEP_NOTIFY_PPM_PERF_CHECK_COMPLETE,
                                                       &Complete);
        return;
    }
}

/* PUBLIC FUNCTIONS ***********************************************************/

CODE_SEG("INIT")
VOID
NTAPI
PpmPerfInitialize(VOID)
{
    KeInitializeMutex(&PpmPerfLock, 0);
}

/**
 * @brief Takes an ACPI processor device for the PPM, which registers it with the plug-in itself.
 *
 * @return TRUE when the device is a processor, the caller then leaves it alone.
 */
BOOLEAN
NTAPI
PpmPepTrackProcessor(
    _In_ PDEVICE_OBJECT Pdo)
{
    static const UNICODE_STRING ProcessorPrefix = RTL_CONSTANT_STRING(L"ACPI\\ACPI0007\\");
    PPPM_PEP_PROCESSOR Processor;
    PDEVICE_NODE DeviceNode;
    BOOLEAN Known = FALSE;
    ULONG Index;

    PAGED_CODE();

    DeviceNode = IopGetDeviceNode(Pdo);
    if (!DeviceNode || !RtlPrefixUnicodeString(&ProcessorPrefix, &DeviceNode->InstancePath, TRUE))
        return FALSE;

    PpmAcquirePerfLock();
    for (Index = 0; Index < PpmPepProcessorCount; Index++)
    {
        if (PpmPepProcessors[Index].Pdo == Pdo)
        {
            Known = TRUE;
            break;
        }
    }

    if (!Known && PpmPepProcessorCount < RTL_NUMBER_OF(PpmPepProcessors))
    {
        Processor = &PpmPepProcessors[PpmPepProcessorCount++];
        ObReferenceObject(Pdo);
        Processor->Pdo = Pdo;
        Processor->Number = PpmProcessorNumberFromInstance(&DeviceNode->InstancePath);
        Processor->Domain = MAXULONG;
        InterlockedIncrement(&PpmPepProcessorsPending);
    }
    PpmReleasePerfLock();

    return TRUE;
}

/**
 * @brief Lets the processors go to the plug-in once the boot devices have started.
 *
 * @remarks The Qualcomm plug-in builds its CPU performance tables from the thermal
 * zones it drives and bugchecks when they are not up yet.
 */
VOID
NTAPI
PpmPepAllowRegistration(VOID)
{
    PAGED_CODE();

    PpmPepRegistrationAllowed = TRUE;
    if (PopFxNextPlugin(NULL))
        PpmPepRegisterProcessors();
}

/**
 * @brief Registers the processors not yet offered to a plug-in.
 *
 * @remarks Called once a plug-in is known. Each processor gets one try, a refusal
 * leaves its clocks with the plug-in's own defaults.
 */
VOID
NTAPI
PpmPepRegisterProcessors(VOID)
{
    PPPM_PEP_PROCESSOR Processor;
    PCSTR Failure;
    ULONG Index;

    PAGED_CODE();

    if (!PpmPepRegistrationAllowed || !ReadNoFence(&PpmPepProcessorsPending))
        return;

    PpmAcquirePerfLock();
    for (Index = 0; Index < PpmPepProcessorCount; Index++)
    {
        Processor = &PpmPepProcessors[Index];
        if (Processor->Attempted)
            continue;

        Processor->Attempted = TRUE;
        InterlockedDecrement(&PpmPepProcessorsPending);

        Failure = PpmRegisterPepProcessor(Processor);
        if (!Failure)
            continue;

        Processor->Domain = MAXULONG;
        if (!PpmPepFailureLogged)
        {
            PpmPepFailureLogged = TRUE;
            DPRINT1("PPM: %wZ has no performance control, %s\n", &Processor->DeviceId, Failure);
        }
    }
    PpmReleasePerfLock();
}

/**
 * @brief Sets the performance request for one domain, held back while in connected standby.
 *
 * @param[in] Request
 * Limits and desired level in plug-in units, clamped to the domain's range. A
 * desired level of zero lets the plug-in pick.
 */
NTSTATUS
NTAPI
PpmSetPerfDomainState(
    _In_ ULONG Domain,
    _In_ PPPM_PERF_REQUEST Request)
{
    PPM_PERF_REQUEST Clamped;
    PPPM_PERF_DOMAIN PerfDomain;
    NTSTATUS Status = STATUS_SUCCESS;

    PAGED_CODE();

    PpmAcquirePerfLock();
    if (Domain >= PpmPerfDomainCount)
    {
        PpmReleasePerfLock();
        return STATUS_INVALID_PARAMETER;
    }

    PerfDomain = &PpmPerfDomains[Domain];
    Clamped.Minimum = max(Request->Minimum, PerfDomain->Lowest);
    Clamped.Maximum = min(Request->Maximum, PerfDomain->Highest);
    Clamped.Maximum = max(Clamped.Maximum, Clamped.Minimum);
    Clamped.Desired = Request->Desired;
    if (Clamped.Desired)
        Clamped.Desired = min(max(Clamped.Desired, Clamped.Minimum), Clamped.Maximum);

    PerfDomain->Requested = Clamped;
    if (!PpmPerfInStandby)
    {
        if (PpmSendPerfDomain(Domain, &Clamped))
            PpmCompletePerfCheck();
        else
            Status = STATUS_UNSUCCESSFUL;
    }
    PpmReleasePerfLock();

    return Status;
}

/**
 * @brief Drops every domain to its lowest level for connected standby, or puts the requests back.
 *
 * @param[in] Enter
 * TRUE when the console display turns off in connected standby, FALSE when it comes back on.
 */
VOID
NTAPI
PpmConnectedStandby(
    _In_ BOOLEAN Enter)
{
    PPM_PERF_REQUEST Request;
    PPPM_PERF_DOMAIN Domain;
    ULONG Applied = 0;
    ULONG Index;

    PAGED_CODE();

    PpmAcquirePerfLock();
    if (PpmPerfInStandby == Enter)
    {
        PpmReleasePerfLock();
        return;
    }

    PpmPerfInStandby = Enter;
    for (Index = 0; Index < PpmPerfDomainCount; Index++)
    {
        Domain = &PpmPerfDomains[Index];
        if (Enter)
        {
            Request.Minimum = Domain->Lowest;
            Request.Maximum = Domain->Lowest;
            Request.Desired = Domain->Lowest;
        }
        else
        {
            Request = Domain->Requested;
        }

        if (PpmSendPerfDomain(Index, &Request))
            Applied++;
    }

    if (Applied)
        PpmCompletePerfCheck();
    PpmReleasePerfLock();

    if (PpmPerfDomainCount)
    {
        DPRINT1("PPM: %s connected standby, %lu of %lu perf domains %s\n",
                Enter ? "entered" : "left", Applied, PpmPerfDomainCount,
                Enter ? "at lowest" : "restored");
    }
}

/* DRIVER INTERFACE ***********************************************************/

static PVOID PpmEnergyEstimationRoutine;
static PVOID PpmEnergySnapRoutine;

/**
 * @brief Installs the feedback counters of a new domain member, runs on the member.
 *
 * @param[in] Context
 * The PPM_FEEDBACK_TYPES sources of this processor, NULL for absent types.
 */
static
VOID
NTAPI
PpmInstallFeedback(
    _In_ PPPM_PROCESSOR Processor,
    _In_opt_ PVOID Context)
{
    PPPM_FEEDBACK_SOURCE *Sources = (PPPM_FEEDBACK_SOURCE *)Context;
    BOOLEAN Enabled;
    ULONG Type;

    /* Whatever ran so far is charged to the counters already installed */
    PpmAccrueActiveTime(Processor, PpmQueryCounter());
    PpmFoldFeedback(Processor, FALSE, FALSE, FALSE);

    Enabled = KeDisableInterrupts();
    PpmLockFeedback(Processor);
    for (Type = 0; Type < PPM_FEEDBACK_TYPES; Type++)
    {
        if (!Sources[Type])
            continue;

        /* A new counter starts out at 100 percent of the time it did not see */
        Processor->Counters[Type] = Sources[Type];
        Processor->ScaledTime[Type] = 100 * Processor->UnscaledTime;
        PpmReadFeedbackSource(Sources[Type], TRUE);
    }

    if (Processor->Counters[PPM_FEEDBACK_PERFORMANCE])
        Processor->KernelTimesIndex = PPM_FEEDBACK_PERFORMANCE;
    else if (Processor->Counters[PPM_FEEDBACK_FREQUENCY])
        Processor->KernelTimesIndex = PPM_FEEDBACK_FREQUENCY;

    PpmUnlockFeedback(Processor);
    KeRestoreInterrupts(Enabled);
}

/**
 * @brief Registers one performance domain of a processor driver.
 *
 * @return STATUS_INVALID_PARAMETER for bad counters or processors, which includes
 * processors that already belong to a domain.
 */
static
NTSTATUS
NTAPI
PpmRegisterPerfStates(
    _In_ PPPM_PERF_DOMAIN_REGISTRATION Registration)
{
    PPPM_FEEDBACK_SOURCE Sources[PPM_FEEDBACK_TYPES];
    ULONG FirstCounter[PPM_FEEDBACK_TYPES];
    PPPM_FEEDBACK_COUNTER Counter;
    PPPM_PERF_CONSTRAINT Constraint;
    PPPM_FEEDBACK_SOURCE Source;
    PPPM_DRIVER_DOMAIN Domain;
    PPPM_PROCESSOR Processor;
    ULONG Index, Type, Number, Count, TypeCount = 0, Master = MAXULONG;
    KAFFINITY Members = 0;
    SIZE_T Size;

    PAGED_CODE();

    if (Registration->Version != PPM_DRIVER_INTERFACE_VERSION)
        return STATUS_REVISION_MISMATCH;

    Count = Registration->ProcessorCount;
    if (!Count || !Registration->PerfSelectionHandler || !Registration->PerfControlHandler)
        return STATUS_INVALID_PARAMETER;

    PpmAcquirePolicyLock();

    /* Only the first counter of each type is used */
    for (Type = 0; Type < PPM_FEEDBACK_TYPES; Type++)
        FirstCounter[Type] = MAXULONG;

    for (Index = 0; Index < Registration->FeedbackCounterCount; Index++)
    {
        Type = Registration->FeedbackCounters[Index].Type;
        if (Type >= PPM_FEEDBACK_TYPES)
            goto Invalid;

        if (FirstCounter[Type] == MAXULONG)
        {
            FirstCounter[Type] = Index;
            TypeCount++;
        }
    }

    if (FirstCounter[PPM_FEEDBACK_FREQUENCY] == MAXULONG &&
        FirstCounter[PPM_FEEDBACK_PERFORMANCE] == MAXULONG)
    {
        goto Invalid;
    }

    for (Index = 0; Index < Count; Index++)
    {
        Number = PpmProcessorIndex(&Registration->Processors[Index].Number);
        if (Number == INVALID_PROCESSOR_INDEX ||
            PpmProcessors[Number].Domain ||
            (Members & AFFINITY_MASK(Number)))
        {
            goto Invalid;
        }

        Members |= AFFINITY_MASK(Number);
        Master = min(Master, Number);
    }

    Size = sizeof(*Domain) + Count * sizeof(*Constraint) + Count * TypeCount * sizeof(*Source);
    Domain = ExAllocatePoolZero(NonPagedPoolNx, Size, TAG_PPM_PERF);
    if (!Domain)
    {
        PpmReleasePolicyLock();
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Domain->Constraints = (PPPM_PERF_CONSTRAINT)(Domain + 1);
    Source = (PPPM_FEEDBACK_SOURCE)(Domain->Constraints + Count);

    Domain->Members = Members;
    Domain->Master = Master;
    Domain->ProcessorCount = Count;
    Domain->SelectionHandler = Registration->PerfSelectionHandler;
    Domain->ControlHandler = Registration->PerfControlHandler;
    Domain->BoostPolicyHandler = Registration->BoostPolicyHandler;
    Domain->BoostModeHandler = Registration->BoostModeHandler;
    Domain->EnergyPerfPreferenceHandler = Registration->EnergyPerfPreferenceHandler;
    Domain->TimeWindowHandler = Registration->TimeWindowHandler;
    Domain->GlobalContext = Registration->GlobalContext;
    Domain->NominalFrequency = Registration->NominalFrequency;
    Domain->MaxPercent = Registration->MaxPerfPercent;
    Domain->MinPerfPercent = Registration->MinPerfPercent;
    Domain->MinThrottlePercent = Registration->MinThrottlePercent;
    Domain->Coordination = Registration->Coordination;
    Domain->HardPlatformCap = Registration->HardPlatformCap;
    Domain->AffinitizeControl = Registration->AffinitizeControl;
    Domain->AutonomousMode = Registration->AutonomousMode;
    Domain->SelectedPercent = 100;
    Domain->DesiredPercent = 0;
    Domain->PerfChangeTime = PpmQueryCounter();

    for (Index = 0; Index < Count; Index++)
    {
        Number = PpmProcessorIndex(&Registration->Processors[Index].Number);
        Constraint = &Domain->Constraints[Index];
        Constraint->Processor = Number;
        Constraint->PerfContext = Registration->Processors[Index].PerfContext;
        Constraint->PlatformCap = Registration->Processors[Index].PlatformCap;
        Constraint->ThermalCap = Registration->Processors[Index].ThermalCap;
        Constraint->LimitReasons = Registration->Processors[Index].LimitReasons;
        Constraint->TargetPercent = 100;
        Constraint->SelectedPercent = 100;
        Constraint->LatestFrequencyPercent = 100;
        Constraint->FrequencySensitivity = 100;

        Processor = &PpmProcessors[Number];
        Processor->Domain = Domain;
        Processor->Constraint = Constraint;

        /* The contexts of processor i start at i * FeedbackCounterCount */
        RtlZeroMemory(Sources, sizeof(Sources));
        for (Type = 0; Type < PPM_FEEDBACK_TYPES; Type++)
        {
            if (FirstCounter[Type] == MAXULONG)
                continue;

            Counter = &Registration->FeedbackCounters[FirstCounter[Type]];
            Source->InstantaneousRead = Counter->InstantaneousRead;
            Source->Affinitized = Counter->Affinitized;
            Source->Differential = Counter->Differential;
            if (Registration->CounterContexts)
            {
                Source->Context = Registration->CounterContexts[Index * Registration->FeedbackCounterCount +
                                                                FirstCounter[Type]];
            }

            Sources[Type] = Source++;
        }

        PpmRunOnProcessor(Number, PpmInstallFeedback, Sources);
    }

    PpmCheckPeriodFloor = max(PpmCheckPeriodFloor, Registration->MinimumPerfCheckPeriod);
    PpmCalculateMinMax(Domain);

    InsertTailList(&PpmDriverDomainList, &Domain->Link);
    PpmDriverDomainCount++;
    PpmPerfRegistered |= Members;
    PpmDomainsToUpdate |= AFFINITY_MASK(Master);

    PpmRunOnProcessors(Members, PpmApplyBoostPolicy, NULL);
    if (PpmPolicy.EnergyPerfPreferenceSet)
        PpmRunOnProcessors(Members, PpmApplyEnergyPerfPreference, NULL);

    DPRINT1("PPM: driver perf domain, %lu processors from %lu, %lu MHz nominal, %lu-%lu%% (throttle %lu%%), coordination 0x%x\n",
            Count, Master, Domain->NominalFrequency, Domain->MinPerfPercent, Domain->MaxPercent,
            Domain->MinThrottlePercent, Domain->Coordination);

    /* New members start their utilization history now, then the first state is programmed */
    PpmRunOnProcessors(Members, PpmResetUtility, NULL);
    PpmCheckReInitialize();
    PpmRunPerfSequence(PpmStepForceDomains);
    return STATUS_SUCCESS;

Invalid:
    PpmReleasePolicyLock();
    return STATUS_INVALID_PARAMETER;
}

/**
 * @brief Changes the highest performance of the domains whose master is targeted.
 */
static
VOID
NTAPI
PpmUpdatePerfStates(
    _In_ PPPM_PERF_DOMAIN_UPDATE Update)
{
    PPPM_DRIVER_DOMAIN Domain;
    PLIST_ENTRY Entry;

    PAGED_CODE();

    if (Update->Version != PPM_DRIVER_INTERFACE_VERSION)
        return;

    PpmAcquirePolicyLock();
    for (Entry = PpmDriverDomainList.Flink; Entry != &PpmDriverDomainList; Entry = Entry->Flink)
    {
        Domain = CONTAINING_RECORD(Entry, PPM_DRIVER_DOMAIN, Link);
        if (!(Update->TargetProcessors & AFFINITY_MASK(Domain->Master)))
            continue;

        Domain->MaxPercent = Update->MaxPerfPercent;
        PpmDomainsToUpdate |= AFFINITY_MASK(Domain->Master);
    }

    PpmRunPerfSequence(PpmStepAsyncApplyDomains);
}

/**
 * @brief Records new platform and thermal caps of a processor and reapplies its domain.
 */
static
NTSTATUS
NTAPI
PpmRegisterPerfCap(
    _In_ PPPM_PROCESSOR_CAP Cap)
{
    PPPM_PERF_CONSTRAINT Constraint;
    PPPM_PROCESSOR Processor;
    ULONG Number;

    PAGED_CODE();

    if (Cap->Version != PPM_DRIVER_INTERFACE_VERSION)
        return STATUS_REVISION_MISMATCH;

    Number = PpmProcessorIndex(&Cap->ProcessorNumber);
    if (Number == INVALID_PROCESSOR_INDEX)
        return STATUS_INVALID_PARAMETER;

    PpmAcquirePolicyLock();

    Processor = &PpmProcessors[Number];
    Constraint = Processor->Constraint;
    if (!Constraint)
    {
        PpmReleasePolicyLock();
        return STATUS_SUCCESS;
    }

    if (Constraint->PlatformCap != Cap->PlatformCap)
    {
        DPRINT("PPM: processor %lu platform cap %lu%%\n", Number, Cap->PlatformCap);
        Constraint->PlatformCap = Cap->PlatformCap;
    }

    if (Constraint->ThermalCap != Cap->ThermalCap)
    {
        DPRINT("PPM: processor %lu thermal cap %lu%%\n", Number, Cap->ThermalCap);
        Constraint->ThermalCap = Cap->ThermalCap;
    }

    Constraint->LimitReasons = Cap->LimitReasons;
    PpmDomainsToUpdate |= AFFINITY_MASK(Processor->Domain->Master);
    PpmRunPerfSequence(PpmStepAsyncApplyDomains);
    return STATUS_SUCCESS;
}

/**
 * @brief Takes the energy estimation routines of the driver, nothing consumes them yet.
 */
static
VOID
NTAPI
PpmRegisterEnergyEstimation(
    _In_opt_ PVOID ComputeEnergy,
    _In_opt_ PVOID SnapEnergyCounters)
{
    PpmEnergyEstimationRoutine = ComputeEnergy;
    PpmEnergySnapRoutine = SnapEnergyCounters;
}

static const PPM_DRIVER_INTERFACE_TABLE PpmDriverInterface =
{
    PPM_DRIVER_INTERFACE_VERSION,
    PpmRegisterPerfStates,
    PpmUpdatePerfStates,
    PpmRegisterPerfCap,
    PpmRegisterIdleStates,
    PpmRegisterIdleDomains,
    PpmRegisterEnergyEstimation
};

/**
 * @brief Hands the boost policy and mode to the driver, runs on the member processor.
 */
VOID
NTAPI
PpmApplyBoostPolicy(
    _In_ PPPM_PROCESSOR Processor,
    _In_opt_ PVOID Context)
{
    PPPM_DRIVER_DOMAIN Domain = Processor->Domain;

    UNREFERENCED_PARAMETER(Context);

    if (!Domain)
        return;

    if (Domain->BoostPolicyHandler)
        Domain->BoostPolicyHandler(Processor->Constraint->PerfContext, PpmPolicy.PerfBoostPolicy);
    if (Domain->BoostModeHandler)
        Domain->BoostModeHandler(Processor->Constraint->PerfContext, PpmPolicy.PerfBoostMode);
}

/**
 * @brief Hands the energy performance preference to the driver, runs on the member processor.
 */
VOID
NTAPI
PpmApplyEnergyPerfPreference(
    _In_ PPPM_PROCESSOR Processor,
    _In_opt_ PVOID Context)
{
    PPPM_DRIVER_DOMAIN Domain = Processor->Domain;

    UNREFERENCED_PARAMETER(Context);

    if (Domain && Domain->EnergyPerfPreferenceHandler)
        Domain->EnergyPerfPreferenceHandler(Processor->Constraint->PerfContext, PpmPolicy.EnergyPerfPreference);
}

/**
 * @brief Answers the ProcessorStateHandler query with the processor driver table.
 *
 * @return STATUS_ACCESS_DENIED for user mode callers.
 */
NTSTATUS
NTAPI
PpmQueryDriverInterface(
    _In_opt_ PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_opt_(OutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _In_ KPROCESSOR_MODE PreviousMode)
{
    if (PreviousMode != KernelMode)
        return STATUS_ACCESS_DENIED;

    if (InputBuffer || InputBufferLength || !OutputBuffer)
        return STATUS_INVALID_PARAMETER;

    if (OutputBufferLength < sizeof(PpmDriverInterface))
        return STATUS_BUFFER_TOO_SMALL;

    RtlCopyMemory(OutputBuffer, &PpmDriverInterface, sizeof(PpmDriverInterface));
    return STATUS_SUCCESS;
}

/* EOF */
