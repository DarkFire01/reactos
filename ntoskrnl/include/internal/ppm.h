/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Internal header for the Processor Power Management (PPM)
 * COPYRIGHT:   Copyright 2023 George Bișoc <george.bisoc@reactos.org>
 *              Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <drivers/processor/ppmdrv.h>

//
// Define this if you want debugging support
//
#define _PPM_DEBUG_                                     0x00

//
// These define the Debug Masks Supported
//
#define PPM_VETO_DEBUG                                  0x01
#define PPM_HETERO_DEBUG                                0x02
#define PPM_CORE_PARK_DEBUG                             0x04
#define PPM_PERF_DEBUG                                  0x06
#define PPM_INIT_SUBSYSTEM_DEBUG                        0x08

//
// Debug/Tracing support
//
#if _PPM_DEBUG_
#ifdef NEW_DEBUG_SYSTEM_IMPLEMENTED // enable when Debug Filters are implemented
#define PPMTRACE DbgPrintEx
#else
#define PPMTRACE(x, ...)                                 \
    if (x & PpmTraceLevel) DbgPrint(__VA_ARGS__)
#endif
#else
#define PPMTRACE(x, fmt, ...) DPRINT(fmt, ##__VA_ARGS__)
#endif

//
// Processor C state types
//
typedef enum _PPM_PROCESSOR_CSTATE_TYPES
{
    ProcessorC0State,
    ProcessorC1State,
    ProcessorC2State,
    ProcessorC3State
} PPM_PROCESSOR_CSTATE_TYPES;

//
// Idle synchronization state
//
typedef union _PPM_IDLE_SYNCHRONIZATION_STATE
{
    struct
    {
        LONG Value;
        LONG Value2;
    };
    ULONG RefCount:30;
    ULONG Idling:1;
    struct
    {
        ULONG Active:1;
        ULONG CriticalIdleOverride:1;
        ULONG ResidentOverride:1;
        ULONG CompleteIdleStatePending:1;
    };
    ULONG Reserved:29;
} PPM_IDLE_SYNCHRONIZATION_STATE, *PPPM_IDLE_SYNCHRONIZATION_STATE;

//
// Idle bucket time type
//
typedef enum _PPM_IDLE_BUCKET_TIME_TYPE
{
    PpmIdleBucketTimeInQpc,
    PpmIdleBucketTimeIn100ns,
    PpmIdleBucketTimeMaximum
} PPM_IDLE_BUCKET_TIME_TYPE;

//
// Performance state selection
//
typedef struct _PERFINFO_PPM_STATE_SELECTION
{
    ULONG SelectedState;
    ULONG VetoedStates;
    ULONG VetoReason[1];
} PERFINFO_PPM_STATE_SELECTION, *PPERFINFO_PPM_STATE_SELECTION;

//
// Concurrency accounting
//
typedef struct _PPM_CONCURRENCY_ACCOUNTING
{
    ULONG Lock;
    ULONG Processors;
    ULONG ActiveProcessors;
    ULONGLONG LastUpdateTime;
    ULONGLONG TotalTime;
    ULONGLONG AccumulatedTime[1];
} PPM_CONCURRENCY_ACCOUNTING, *PPPM_CONCURRENCY_ACCOUNTING;

//
// Fixed Function Hardware (FFH) throttle state info
//
typedef struct _PPM_FFH_THROTTLE_STATE_INFO
{
    BOOLEAN EnableLogging;
    ULONG MismatchCount;
    UCHAR Initialized;
    ULONGLONG LastValue;
    LARGE_INTEGER LastLogTickCount;
} PPM_FFH_THROTTLE_STATE_INFO, *PPPM_FFH_THROTTLE_STATE_INFO;

//
// Selection statistics
//
typedef struct _PPM_SELECTION_STATISTICS
{
    ULONGLONG PlatformOnlyCount;
    ULONGLONG PreVetoCount;
    ULONGLONG VetoCount;
    ULONGLONG IdleDurationCount;
    ULONGLONG LatencyCount;
    ULONGLONG InterruptibleCount;
    ULONGLONG DeviceDependencyCount;
    ULONGLONG ProcessorDependencyCount;
    ULONGLONG WrongProcessorCount;
    ULONGLONG LegacyOverrideCount;
    ULONGLONG CstateCheckCount;
    ULONGLONG NoCStateCount;
    ULONGLONG SelectedCount;
} PPM_SELECTION_STATISTICS, *PPPM_SELECTION_STATISTICS;

//
// Veto accounting
//
typedef struct _PPM_VETO_ACCOUNTING
{
    volatile LONG VetoPresent;
    LIST_ENTRY VetoListHead;
} PPM_VETO_ACCOUNTING, *PPPM_VETO_ACCOUNTING;

//
// Power processor idle state
//
typedef struct _PPM_IDLE_STATE
{
    KAFFINITY_EX DomainMembers;
    ULONG Latency;
    ULONG BreakEvenDuration;
    ULONG Power;
    ULONG StateFlags;
    PPM_VETO_ACCOUNTING VetoAccounting;
    UCHAR StateType;
    BOOLEAN InterruptsEnabled;
    UCHAR Interruptible;
    BOOLEAN ContextRetained;
    BOOLEAN CacheCoherent;
    BOOLEAN WakesSpuriously;
    BOOLEAN PlatformOnly;
    BOOLEAN NoCState;
} PPM_IDLE_STATE, *PPPM_IDLE_STATE;

//
// Performance request for one domain, in plug-in performance units
//
typedef struct _PPM_PERF_REQUEST
{
    ULONG Minimum;
    ULONG Maximum;
    ULONG Desired;
} PPM_PERF_REQUEST, *PPPM_PERF_REQUEST;

//
// Processors a plug-in changes performance for together
//
typedef struct _PPM_PERF_DOMAIN
{
    ULONG DomainId;
    ULONG Members;
    ULONG Highest;
    ULONG Nominal;
    ULONG LowestNonlinear;
    ULONG Lowest;

    /* Requested is what policy asks for, Current is what the plug-in was last told */
    PPM_PERF_REQUEST Requested;
    PPM_PERF_REQUEST Current;
} PPM_PERF_DOMAIN, *PPPM_PERF_DOMAIN;

//
// Processor device the PPM registers with a power engine plug-in
//
typedef struct _PPM_PEP_PROCESSOR
{
    PDEVICE_OBJECT Pdo;
    UNICODE_STRING DeviceId;
    struct _POP_FX_PLUGIN *Plugin;
    PEPHANDLE PepHandle;
    ULONG Number;
    ULONG Domain;
    BOOLEAN Attempted;
} PPM_PEP_PROCESSOR, *PPPM_PEP_PROCESSOR;

//
// Number of feedback counter types the engine tracks
//
#define PPM_FEEDBACK_TYPES                              3

//
// Per processor actions queued to the targeted action DPC
//
#define PPM_ACTION_SNAP_UTILITY                         0x00000001
#define PPM_ACTION_REAPPLY_STATE                        0x00000004
#define PPM_ACTION_APPLY_STATE                          0x00000008

//
// Idle accounting histogram size, bucket i holds idle periods below 2^i ms
//
#define PPM_IDLE_BUCKET_COUNT                           16

//
// Steps of the performance check sequencer
//
typedef enum _PPM_CHECK_STEP
{
    PpmStepNone,
    PpmStepSnapUtility,
    PpmStepRecordUtility,
    PpmStepSelectProcessors,
    PpmStepSelectDomains,
    PpmStepApplyDomains,
    PpmStepMakeupChecks,
    PpmStepApplyProcessors,
    PpmStepPeriodicEnd,
    PpmStepForceDomains,
    PpmStepAsyncApplyDomains,
    PpmStepAsyncApplyProcessors,
    PpmStepAsyncEnd
} PPM_CHECK_STEP;

//
// Feedback counter a processor driver registered, one per processor and type
//
typedef struct _PPM_FEEDBACK_SOURCE
{
    union
    {
        PPPM_READ_INSTANTANEOUS_COUNTER InstantaneousRead;
        PPPM_READ_DIFFERENTIAL_COUNTER DifferentialRead;
    };
    PVOID Context;
    ULONG64 LastReference;
    ULONG64 LastActual;
    ULONG CachedPercent;
    BOOLEAN Affinitized;
    BOOLEAN Differential;
} PPM_FEEDBACK_SOURCE, *PPPM_FEEDBACK_SOURCE;

//
// Performance state of one member of a driver registered domain
//
typedef struct _PPM_PERF_CONSTRAINT
{
    ULONG Processor;
    PVOID PerfContext;
    ULONG PlatformCap;
    ULONG ThermalCap;
    ULONG LimitReasons;
    ULONG TargetPercent;
    ULONG DesiredPercent;
    ULONG SelectedPercent;
    ULONG SelectedFrequency;
    ULONG PreviousPercent;
    ULONG PreviousFrequency;
    ULONG TolerancePercent;
    ULONG LatestFrequencyPercent;
    ULONG FrequencySensitivity;
    ULONG64 SelectedState;
} PPM_PERF_CONSTRAINT, *PPPM_PERF_CONSTRAINT;

//
// Performance domain registered by a processor driver
//
typedef struct _PPM_DRIVER_DOMAIN
{
    LIST_ENTRY Link;
    KAFFINITY Members;
    ULONG Master;
    ULONG ProcessorCount;
    PPPM_PERF_SELECTION_HANDLER SelectionHandler;
    PPPM_PERF_CONTROL_HANDLER ControlHandler;
    PPPM_PERF_POLICY_HANDLER BoostPolicyHandler;
    PPPM_PERF_POLICY_HANDLER BoostModeHandler;
    PPPM_PERF_POLICY_HANDLER EnergyPerfPreferenceHandler;
    PPPM_PERF_POLICY_HANDLER TimeWindowHandler;
    PVOID GlobalContext;
    ULONG NominalFrequency;
    ULONG MaxPercent;
    ULONG MinPerfPercent;
    ULONG MinThrottlePercent;
    ULONG MinPolicyPercent;
    ULONG MaxPolicyPercent;
    ULONG ConstrainedMinPercent;
    ULONG ConstrainedMaxPercent;
    ULONG DesiredPercent;
    ULONG SelectedPercent;
    ULONG SelectedFrequency;
    ULONG GuaranteedPercent;
    ULONG TolerancePercent;
    ULONG PerfChangeIntervalCount;
    ULONG64 SelectedState;
    ULONG64 PerfChangeTime;
    UCHAR Coordination;
    BOOLEAN HardPlatformCap;
    BOOLEAN AffinitizeControl;
    BOOLEAN AutonomousMode;
    BOOLEAN Force;
    _Field_size_(ProcessorCount) PPPM_PERF_CONSTRAINT Constraints;
} PPM_DRIVER_DOMAIN, *PPPM_DRIVER_DOMAIN;

//
// Entry of the dependency array handed to IdlePrepare
//
typedef struct _PPM_IDLE_DEPENDENCY
{
    PROCESSOR_NUMBER Processor;
    UCHAR ExpectedState;
    UCHAR Reserved;
} PPM_IDLE_DEPENDENCY, *PPPM_IDLE_DEPENDENCY;

//
// Kernel copy of one registered idle state
//
typedef struct _PPM_IDLE_STATE_SLOT
{
    KAFFINITY DomainMembers;
    ULONG StateFlags;
    ULONG Latency;
    ULONG BreakEvenDuration;
    ULONG Power;
    UCHAR StateType;
    BOOLEAN Interruptible;
    BOOLEAN ContextRetained;
    BOOLEAN CacheCoherent;
    BOOLEAN InterruptsEnabled;
} PPM_IDLE_STATE_SLOT, *PPPM_IDLE_STATE_SLOT;

//
// Idle states installed on one processor, the dependency array follows State[]
//
typedef struct _PPM_IDLE_STATE_SET
{
    PROCESSOR_IDLE_PREPARE_INFO PrepareInfo;
    PPPM_IDLE_PREPARE IdlePrepare;
    PPPM_IDLE_EXECUTE IdleExecute;
    PPPM_IDLE_COMPLETE IdleComplete;
    PPPM_IDLE_CANCEL IdleCancel;
    PPPM_IDLE_IS_HALTED IdleIsHalted;
    PPPM_IDLE_INITIATE_WAKE IdleInitiateWake;
    KAFFINITY PrimaryProcessorMask;
    KAFFINITY SecondaryProcessorMask;
    NTSTATUS PreviousStatus;
    ULONG ProcessorIdleCount;
    ULONG TargetState;
    ULONG OldState;
    ULONG ActualState;
    ULONG OverrideIndex;
    BOOLEAN ForceIdle;
    BOOLEAN EstimateIdleDuration;
    _Field_size_(ProcessorIdleCount) PPM_IDLE_STATE_SLOT State[ANYSIZE_ARRAY];
} PPM_IDLE_STATE_SET, *PPPM_IDLE_STATE_SET;

typedef struct _PPM_IDLE_STATE_COUNTERS
{
    ULONG64 TotalTime;
    ULONG64 MinTime;
    ULONG64 MaxTime;
    ULONG IdleTransitions;
    ULONG FailedTransitions;
    ULONG CanceledTransitions;
    ULONG BucketHits[PPM_IDLE_BUCKET_COUNT];
} PPM_IDLE_STATE_COUNTERS, *PPPM_IDLE_STATE_COUNTERS;

typedef struct _PPM_IDLE_ACCOUNTING
{
    ULONG StateCount;
    ULONG TotalTransitions;
    ULONG ResetCount;
    ULONG AbortCount;
    ULONG64 StartTime;
    ULONG64 BucketLimits[PPM_IDLE_BUCKET_COUNT];
    _Field_size_(StateCount) PPM_IDLE_STATE_COUNTERS State[ANYSIZE_ARRAY];
} PPM_IDLE_ACCOUNTING, *PPPM_IDLE_ACCOUNTING;

//
// Utilization snapshot taken by the periodic check
//
typedef struct _PPM_UTILITY_SNAPSHOT
{
    ULONG64 Time;
    ULONG64 Active;
    ULONG64 FrequencyScaled;
    ULONG64 PerformanceScaled;
    ULONG64 ScalabilityScaled;
    ULONG64 CyclesActive;
    ULONG64 CyclesAffinitized;
    ULONG64 LastTime;
    ULONG64 LastActive;
} PPM_UTILITY_SNAPSHOT, *PPPM_UTILITY_SNAPSHOT;

//
// Processor power management state of one processor
//
typedef struct _PPM_PROCESSOR
{
    ULONG Index;
    PPPM_DRIVER_DOMAIN Domain;
    PPPM_PERF_CONSTRAINT Constraint;
    PPPM_FEEDBACK_SOURCE Counters[PPM_FEEDBACK_TYPES];
    volatile LONG FeedbackLock;
    ULONG KernelTimesIndex;
    volatile LONG64 UnaccountedTime;
    ULONG64 LastUpdateTime;
    ULONG64 UnscaledTime;
    ULONG64 ScaledTime[PPM_FEEDBACK_TYPES];
    ULONG64 CyclesLast;
    ULONG64 CyclesActive;
    ULONG64 IdleTimeTotal;
    ULONG64 IdleTimeLast;
    volatile LONG64 IdleTimeEntry;
    PPM_UTILITY_SNAPSHOT Check;
    ULONG Utility;
    ULONG AffinitizedUtility;
    ULONG LatestPerformancePercent;
    ULONG LatestAffinitizedPercent;
    ULONG AveragePerformancePercent;
    ULONG GuaranteedPercent;
    PPPM_IDLE_STATE_SET IdleStates;
    PPPM_IDLE_ACCOUNTING IdleAccounting;
    BOOLEAN IdleAllowScaling;
    UCHAR IdlePromoteBase;
    UCHAR IdleDemoteBase;
    UCHAR IdlePromotePercent;
    UCHAR IdleDemotePercent;
    volatile LONG ActionMask;
    KDPC ActionDpc;
} PPM_PROCESSOR, *PPPM_PROCESSOR;

//
// Current processor power policy, pushed by the power setting callbacks
//
typedef struct _PPM_POLICY
{
    ULONG PerfTimeCheck;
    ULONG PerfHistoryCount;
    ULONG PerfIncreaseTime;
    ULONG PerfDecreaseTime;
    ULONG PerfBoostPolicy;
    ULONG PerfBoostMode;
    ULONG EnergyPerfPreference;
    ULONG IdleTimeCheck;
    UCHAR PerfIncreaseThreshold;
    UCHAR PerfDecreaseThreshold;
    UCHAR PerfIncreasePolicy;
    UCHAR PerfDecreasePolicy;
    UCHAR PerfMinPolicy;
    UCHAR PerfMaxPolicy;
    UCHAR ParkingPerfState;
    UCHAR IdlePromotePercent;
    UCHAR IdleDemotePercent;
    UCHAR CoreParkingIncreaseThreshold;
    UCHAR CoreParkingDecreaseThreshold;
    UCHAR CoreParkingIncreasePolicy;
    UCHAR CoreParkingDecreasePolicy;
    UCHAR CoreParkingMaxCores;
    UCHAR CoreParkingMinCores;
    UCHAR CoreParkingOverUtilizationThreshold;
    UCHAR CoreParkingConcurrencyThreshold;
    UCHAR CoreParkingHeadroomThreshold;
    ULONG CoreParkingIncreaseTime;
    ULONG CoreParkingDecreaseTime;
    BOOLEAN DistributeUtility;
    BOOLEAN AllowThrottling;
    BOOLEAN PerfLatencyHints;
    BOOLEAN IdleAllowScaling;
    BOOLEAN IdleDisable;
    BOOLEAN EnergyPerfPreferenceSet;
    BOOLEAN ThrottlePolicyPushed;
} PPM_POLICY, *PPPM_POLICY;

typedef
VOID
(NTAPI *PPPM_PROCESSOR_ROUTINE)(
    _In_ PPPM_PROCESSOR Processor,
    _In_opt_ PVOID Context);

extern PPM_PROCESSOR PpmProcessors[MAXIMUM_PROCESSORS];
extern PPM_POLICY PpmPolicy;
extern ULONG64 PpmQpcFrequency;
extern LIST_ENTRY PpmDriverDomainList;
extern ULONG PpmDriverDomainCount;
extern KAFFINITY PpmPerfRegistered;
extern KAFFINITY PpmDomainsToUpdate;
extern ULONG64 PpmCheckPeriodFloor;

//
// Inline helpers
//
/* The ARM HAL has no performance counter, the interrupt time stands in for it there */
FORCEINLINE
ULONG64
PpmQueryCounter(VOID)
{
#if defined(_M_ARM)
    return KeQueryInterruptTime();
#else
    return (ULONG64)KeQueryPerformanceCounter(NULL).QuadPart;
#endif
}

FORCEINLINE
ULONG64
PpmQueryCounterFrequency(VOID)
{
#if defined(_M_ARM)
    return 10000000;
#else
    LARGE_INTEGER Frequency;

    KeQueryPerformanceCounter(&Frequency);
    return (ULONG64)Frequency.QuadPart;
#endif
}

FORCEINLINE
ULONG64
PpmReadCycles(VOID)
{
#if defined(_M_IX86) || defined(_M_AMD64)
    return __rdtsc();
#else
    return 0;
#endif
}

FORCEINLINE
LONG64
PpmAtomicRead64(
    _In_ volatile LONG64 *Value)
{
    return InterlockedCompareExchange64(Value, 0, 0);
}

FORCEINLINE
LONG64
PpmAtomicExchange64(
    _Inout_ volatile LONG64 *Target,
    _In_ LONG64 Value)
{
    LONG64 Old;

    do
    {
        Old = *Target;
    } while (InterlockedCompareExchange64(Target, Value, Old) != Old);

    return Old;
}

FORCEINLINE
VOID
PpmAtomicAdd64(
    _Inout_ volatile LONG64 *Target,
    _In_ LONG64 Value)
{
    LONG64 Old;

    do
    {
        Old = *Target;
    } while (InterlockedCompareExchange64(Target, Old + Value, Old) != Old);
}

/* The feedback lock is only taken with interrupts disabled */
FORCEINLINE
VOID
PpmLockFeedback(
    _Inout_ PPPM_PROCESSOR Processor)
{
    while (InterlockedCompareExchange(&Processor->FeedbackLock, 1, 0) != 0)
        YieldProcessor();
}

FORCEINLINE
VOID
PpmUnlockFeedback(
    _Inout_ PPPM_PROCESSOR Processor)
{
    InterlockedExchange(&Processor->FeedbackLock, 0);
}

/* Maps a processor number to its index, INVALID_PROCESSOR_INDEX when there is no such processor */
FORCEINLINE
ULONG
PpmProcessorIndex(
    _In_ PPROCESSOR_NUMBER Number)
{
    if (Number->Reserved != 0 ||
        Number->Group != 0 ||
        Number->Number >= (ULONG)KeNumberProcessors)
    {
        return INVALID_PROCESSOR_INDEX;
    }

    return Number->Number;
}

//
// Initialization routines
//
CODE_SEG("INIT")
NTSTATUS
NTAPI
PpmInitialize(
    _In_ BOOLEAN EarlyPhase);

VOID
NTAPI
PpmInitializeProcessor(
    _In_ PKPRCB Prcb);

//
// Engine routines
//
VOID
NTAPI
PpmEngineInitialize(VOID);

VOID
NTAPI
PpmAcquirePolicyLock(VOID);

VOID
NTAPI
PpmReleasePolicyLock(VOID);

VOID
NTAPI
PpmRunOnProcessor(
    _In_ ULONG Index,
    _In_ PPPM_PROCESSOR_ROUTINE Routine,
    _In_opt_ PVOID Context);

VOID
NTAPI
PpmRunOnProcessors(
    _In_ KAFFINITY Processors,
    _In_ PPPM_PROCESSOR_ROUTINE Routine,
    _In_opt_ PVOID Context);

VOID
NTAPI
PpmAccrueActiveTime(
    _Inout_ PPPM_PROCESSOR Processor,
    _In_ ULONG64 Now);

BOOLEAN
NTAPI
PpmFoldFeedback(
    _Inout_ PPPM_PROCESSOR Processor,
    _In_ BOOLEAN Reset,
    _In_ BOOLEAN Remote,
    _In_ BOOLEAN Lazy);

ULONG
NTAPI
PpmReadFeedbackSource(
    _Inout_ PPPM_FEEDBACK_SOURCE Source,
    _In_ BOOLEAN Reset);

ULONG64
NTAPI
PpmFrequencyScaledTime(
    _In_ PPPM_PROCESSOR Processor);

VOID
NTAPI
PpmCalculateMinMax(
    _Inout_ PPPM_DRIVER_DOMAIN Domain);

VOID
NTAPI
PpmResetUtility(
    _In_ PPPM_PROCESSOR Processor,
    _In_opt_ PVOID Context);

VOID
NTAPI
PpmCheckReInitialize(VOID);

VOID
NTAPI
PpmRunPerfSequence(
    _In_ PPM_CHECK_STEP Start);

VOID
NTAPI
PpmUpdateAllDomains(VOID);

VOID
FASTCALL
PpmCheckClockTick(
    _In_ ULONG64 InterruptTime);

VOID
NTAPI
PpmStartBootOverrideTimer(VOID);

//
// Processor idle functions
//
VOID
FASTCALL
PpmIdle(
    _In_ PPROCESSOR_POWER_STATE PowerState);

VOID
NTAPI
PpmIdleInitialize(VOID);

VOID
NTAPI
PpmScaleIdlePolicy(
    _Inout_ PPPM_PROCESSOR Processor);

VOID
NTAPI
PpmAcquireIdlePolicyLock(VOID);

VOID
NTAPI
PpmReleaseIdlePolicyLock(VOID);

VOID
NTAPI
PpmReapplyIdlePolicy(VOID);

VOID
NTAPI
PpmInstallDefaultIdleStates(VOID);

NTSTATUS
NTAPI
PpmRegisterIdleStates(
    _In_ PPPM_IDLE_STATES_REGISTRATION Registration);

NTSTATUS
NTAPI
PpmRegisterIdleDomains(
    _In_ PPPM_IDLE_DOMAINS_REGISTRATION Registration);

//
// Processor performance functions
//
CODE_SEG("INIT")
VOID
NTAPI
PpmPerfInitialize(VOID);

BOOLEAN
NTAPI
PpmPepTrackProcessor(
    _In_ PDEVICE_OBJECT Pdo);

VOID
NTAPI
PpmPepRegisterProcessors(VOID);

VOID
NTAPI
PpmPepAllowRegistration(VOID);

NTSTATUS
NTAPI
PpmSetPerfDomainState(
    _In_ ULONG Domain,
    _In_ PPPM_PERF_REQUEST Request);

VOID
NTAPI
PpmConnectedStandby(
    _In_ BOOLEAN Enter);

NTSTATUS
NTAPI
PpmQueryDriverInterface(
    _In_opt_ PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_opt_(OutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _In_ KPROCESSOR_MODE PreviousMode);

VOID
NTAPI
PpmApplyBoostPolicy(
    _In_ PPPM_PROCESSOR Processor,
    _In_opt_ PVOID Context);

VOID
NTAPI
PpmApplyEnergyPerfPreference(
    _In_ PPPM_PROCESSOR Processor,
    _In_opt_ PVOID Context);

//
// Processor policy functions
//
VOID
NTAPI
PpmPolicyInitialize(VOID);

VOID
NTAPI
PpmRegisterPolicyCallbacks(VOID);

VOID
NTAPI
PpmApplyDefaultScheme(VOID);

//
// Processor statistics functions
//
VOID
NTAPI
PpmQueryProcessorPowerInformation(
    _In_ ULONG Index,
    _Out_ PPROCESSOR_POWER_INFORMATION Information);

BOOLEAN
NTAPI
PpmQueryPerformanceDistribution(
    _In_ ULONG Index,
    _Out_ PULONG64 ActiveTime,
    _Out_ PULONG64 FrequencyScaledTime);

ULONG
NTAPI
PpmQueryDistributionShift(VOID);

/* EOF */
