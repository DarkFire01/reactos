/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Interface between the kernel processor power manager and processor drivers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/*
 * A processor driver gets the kernel table at DriverEntry with
 * ZwPowerInformation(ProcessorStateHandler, NULL, 0, &Table, sizeof(Table))
 * and refuses to load unless Table.InterfaceVersion is PPM_DRIVER_INTERFACE_VERSION.
 * Every structure handed to the kernel carries the same version. The kernel
 * copies what it needs, so the driver may free its buffers after a call.
 * All table entries are called at PASSIVE_LEVEL.
 */
#define PPM_DRIVER_INTERFACE_VERSION        43

/* Coordination types, as ACPI _PSD/_TSD/_CSD report them */
#define PPM_COORDINATION_NONE               0x00
#define PPM_COORDINATION_PACKAGE            0x01
#define PPM_COORDINATION_SW_ALL             0xFC
#define PPM_COORDINATION_SW_ANY             0xFD
#define PPM_COORDINATION_HW_ALL             0xFE

/* PerfSelectionHandler flags */
#define PPM_SELECT_ALLOW_THROTTLE           0x00000001
#define PPM_SELECT_ROUND_DOWN               0x00000002

/* Boost modes, the GUID_PROCESSOR_PERF_BOOST_MODE values */
#define PPM_BOOST_MODE_DISABLED             0
#define PPM_BOOST_MODE_ENABLED              1
#define PPM_BOOST_MODE_AGGRESSIVE           2
#define PPM_BOOST_MODE_EFFICIENT_ENABLED    3
#define PPM_BOOST_MODE_EFFICIENT_AGGRESSIVE 4

/* LimitReasons */
#define PPM_LIMIT_REASON_PLATFORM           0x00000002

/* Feedback counter types */
#define PPM_FEEDBACK_FREQUENCY              0
#define PPM_FEEDBACK_PERFORMANCE            1
#define PPM_FEEDBACK_SCALABILITY            2

/* Idle state flags */
#define PPM_IDLE_STATE_INTERRUPTIBLE        0x00000001
#define PPM_IDLE_STATE_CACHE_COHERENT       0x00000002
#define PPM_IDLE_STATE_CONTEXT_RETAINED     0x00000004
#define PPM_IDLE_STATE_CSTATE_SHIFT         3
#define PPM_IDLE_STATE_CSTATE_MASK          0x00000078
#define PPM_IDLE_STATE_WAKES_SPURIOUSLY     0x00000080
#define PPM_IDLE_STATE_PLATFORM_ONLY        0x00000100
#define PPM_IDLE_STATE_NO_CSTATE            0x40000000
#define PPM_IDLE_STATE_INTERRUPTS_ENABLED   0x80000000

/* Idle selection results written by IdlePrepare */
#define PPM_IDLE_INDEX_NONE                 0xFFFFFFFF
#define PPM_IDLE_INDEX_HAL                  0xFFFFFFFE

/* Idle cancel codes */
#define PPM_IDLE_CANCEL_WORK_PENDING        0
#define PPM_IDLE_CANCEL_DEPENDENCY          1
#define PPM_IDLE_CANCEL_NO_CSTATE           2

/* PERFORMANCE *****************************************************************/

/**
 * @brief Reads a counter that reports a percent of nominal directly.
 */
typedef
VOID
(NTAPI *PPPM_READ_INSTANTANEOUS_COUNTER)(
    _In_ PVOID Context,
    _Out_ PULONG Value);

/**
 * @brief Reads a reference/actual counter pair, such as MPERF and APERF.
 *
 * @param[in] Reset
 * TRUE to start a new interval, the values then only prime the next read.
 */
typedef
VOID
(NTAPI *PPPM_READ_DIFFERENTIAL_COUNTER)(
    _In_ PVOID Context,
    _In_ BOOLEAN Reset,
    _Out_ PULONG64 Reference,
    _Out_ PULONG64 Actual);

typedef struct _PPM_FEEDBACK_COUNTER
{
    union
    {
        PPPM_READ_INSTANTANEOUS_COUNTER InstantaneousRead;
        PPPM_READ_DIFFERENTIAL_COUNTER DifferentialRead;
    };
    ULONG NominalRate;
    UCHAR Type;
    BOOLEAN Affinitized;
    BOOLEAN Differential;
} PPM_FEEDBACK_COUNTER, *PPPM_FEEDBACK_COUNTER;

/**
 * @brief Snaps a target percent to a state the driver has.
 *
 * @param[in] Flags
 * PPM_SELECT_ALLOW_THROTTLE lets throttle states be picked, PPM_SELECT_ROUND_DOWN
 * picks the highest state not above Target instead of the lowest not below it.
 *
 * @param[out] Frequency
 * The frequency of the picked state in MHz.
 *
 * @param[out] State
 * The driver's identifier for the picked state, handed back to PerfControlHandler.
 *
 * @return The percent of nominal of the picked state.
 */
typedef
ULONG
(NTAPI *PPPM_PERF_SELECTION_HANDLER)(
    _In_ PVOID Context,
    _In_ ULONG TargetPercent,
    _In_ ULONG MinPercent,
    _In_ ULONG MaxPercent,
    _In_ ULONG Flags,
    _Out_ PULONG Frequency,
    _Out_ PULONG64 State);

/**
 * @brief Programs a selected state.
 *
 * @param[in] Initiate
 * FALSE only records the state, for coordinated members whose master programs it.
 *
 * @param[in] Force
 * TRUE drops whatever the driver cached about the hardware, nothing is written.
 *
 * @remarks Runs at DISPATCH_LEVEL, on the owning processor when the domain
 * asked for AffinitizeControl.
 */
typedef
VOID
(NTAPI *PPPM_PERF_CONTROL_HANDLER)(
    _In_ PVOID Context,
    _In_ ULONG64 State,
    _In_ ULONG MinPercent,
    _In_ ULONG MaxPercent,
    _In_ ULONG TolerancePercent,
    _In_ BOOLEAN Autonomous,
    _In_ BOOLEAN Initiate,
    _In_ BOOLEAN Force);

/**
 * @brief Hands a policy value to the driver, run on every member processor.
 */
typedef
VOID
(NTAPI *PPPM_PERF_POLICY_HANDLER)(
    _In_ PVOID Context,
    _In_ ULONG Value);

typedef
VOID
(NTAPI *PPPM_PERF_REINITIALIZE_HANDLER)(
    _In_ PVOID Context);

typedef
VOID
(NTAPI *PPPM_GET_FFH_THROTTLE_STATE)(
    _Out_ PULONG64 ThrottleState);

typedef struct _PPM_PROCESSOR_PERF_ENTRY
{
    PROCESSOR_NUMBER Number;
    PVOID PerfContext;
    ULONG PlatformCap;
    ULONG ThermalCap;
    ULONG LimitReasons;
} PPM_PROCESSOR_PERF_ENTRY, *PPPM_PROCESSOR_PERF_ENTRY;

/**
 * @brief One performance domain, registered once by its last member to start.
 *
 * @remarks FeedbackCounters and CounterContexts point into the same buffer.
 * CounterContexts holds ProcessorCount * FeedbackCounterCount entries, the
 * contexts of processor i start at i * FeedbackCounterCount.
 */
typedef struct _PPM_PERF_DOMAIN_REGISTRATION
{
    ULONG Version;
    UCHAR Coordination;
    BOOLEAN HardPlatformCap;
    BOOLEAN AffinitizeControl;
    BOOLEAN EfficientThrottle;
    BOOLEAN AutonomousMode;
    ULONG NominalFrequency;
    ULONG MaxPerfPercent;
    ULONG MinPerfPercent;
    ULONG MinThrottlePercent;
    ULONG MinimumRelativePerformance;
    ULONG NominalRelativePerformance;
    ULONG64 MinimumPerfCheckPeriod;
    PVOID GlobalContext;
    PPPM_PERF_SELECTION_HANDLER PerfSelectionHandler;
    PPPM_PERF_CONTROL_HANDLER PerfControlHandler;
    PPPM_PERF_POLICY_HANDLER BoostPolicyHandler;
    PPPM_PERF_POLICY_HANDLER BoostModeHandler;
    PPPM_PERF_POLICY_HANDLER EnergyPerfPreferenceHandler;
    PPPM_PERF_POLICY_HANDLER AutonomousActivityWindowHandler;
    PPPM_PERF_POLICY_HANDLER AutonomousModeHandler;
    PPPM_PERF_POLICY_HANDLER TimeWindowHandler;
    PPPM_PERF_REINITIALIZE_HANDLER ReinitializeHandler;
    PPPM_GET_FFH_THROTTLE_STATE GetFFHThrottleState;
    ULONG FeedbackCounterCount;
    PPPM_FEEDBACK_COUNTER FeedbackCounters;
    PVOID *CounterContexts;
    ULONG ProcessorCount;
    _Field_size_(ProcessorCount) PPM_PROCESSOR_PERF_ENTRY Processors[ANYSIZE_ARRAY];
} PPM_PERF_DOMAIN_REGISTRATION, *PPPM_PERF_DOMAIN_REGISTRATION;

typedef struct _PPM_PERF_DOMAIN_UPDATE
{
    ULONG Version;
    ULONG MaxPerfPercent;
    KAFFINITY TargetProcessors;
} PPM_PERF_DOMAIN_UPDATE, *PPPM_PERF_DOMAIN_UPDATE;

typedef struct _PPM_PROCESSOR_CAP
{
    ULONG Version;
    PROCESSOR_NUMBER ProcessorNumber;
    ULONG PlatformCap;
    ULONG ThermalCap;
    ULONG LimitReasons;
} PPM_PROCESSOR_CAP, *PPPM_PROCESSOR_CAP;

/* IDLE ************************************************************************/

typedef struct _PPM_IDLE_CONSTRAINTS
{
    ULONG64 TotalTime;
    ULONG64 IdleTime;
    ULONG64 ExpectedIdleDuration;
    ULONG64 TimeCheck;
    ULONG OverrideState;
    UCHAR PromotePercent;
    UCHAR DemotePercent;
    BOOLEAN Parked;
    BOOLEAN Interruptible;
    BOOLEAN PlatformIdle;
} PPM_IDLE_CONSTRAINTS, *PPPM_IDLE_CONSTRAINTS;

/* The kernel fills Context and Constraints, IdlePrepare writes the rest */
typedef struct _PROCESSOR_IDLE_PREPARE_INFO
{
    PVOID Context;
    PPM_IDLE_CONSTRAINTS Constraints;
    ULONG DependencyCount;
    ULONG DependencyUsed;
    PVOID DependencyArray;
    ULONG PlatformIdleStateIndex;
    ULONG ProcessorIdleStateIndex;
    ULONG IdleSelectFailureMask;
} PROCESSOR_IDLE_PREPARE_INFO, *PPROCESSOR_IDLE_PREPARE_INFO;

typedef
VOID
(NTAPI *PPPM_IDLE_PREPARE)(
    _Inout_ PPROCESSOR_IDLE_PREPARE_INFO PrepareInfo);

/**
 * @brief Enters the selected state, entered with interrupts disabled.
 *
 * @remarks The handler enables interrupts together with halting, sti then hlt,
 * or uses a wakeup that works with interrupts masked such as mwait.
 */
typedef
NTSTATUS
(NTAPI *PPPM_IDLE_EXECUTE)(
    _In_ PVOID Context,
    _In_ ULONG ProcessorState,
    _In_ ULONG PlatformState);

typedef
VOID
(NTAPI *PPPM_IDLE_COMPLETE)(
    _In_ PVOID Context,
    _In_ ULONG ProcessorState,
    _In_ ULONG PlatformState);

typedef
VOID
(NTAPI *PPPM_IDLE_CANCEL)(
    _In_ PVOID Context,
    _In_ ULONG CancelCode);

typedef
BOOLEAN
(NTAPI *PPPM_IDLE_IS_HALTED)(
    _In_ PVOID Context);

typedef
BOOLEAN
(NTAPI *PPPM_IDLE_INITIATE_WAKE)(
    _In_ PVOID Context);

typedef struct _PPM_IDLE_STATE_ENTRY
{
    ULONG StateFlags;
    ULONG Latency;
    ULONG BreakEvenDuration;
    ULONG Power;
} PPM_IDLE_STATE_ENTRY, *PPPM_IDLE_STATE_ENTRY;

/**
 * @brief The idle states of one processor. Latency and BreakEvenDuration are
 * in 100ns units, Power in milliwatts.
 *
 * @remarks A ProcessorIdleCount of 0 removes the states, the processor then
 * idles through HalProcessorIdle.
 */
typedef struct _PPM_IDLE_STATES_REGISTRATION
{
    ULONG Version;
    PROCESSOR_NUMBER Processor;
    PVOID Context;
    BOOLEAN EstimateIdleDuration;
    BOOLEAN Update;
    ULONG MaximumDependencies;
    PPPM_IDLE_PREPARE IdlePrepare;
    PPPM_IDLE_EXECUTE IdleExecute;
    PPPM_IDLE_COMPLETE IdleComplete;
    PPPM_IDLE_CANCEL IdleCancel;
    PPPM_IDLE_IS_HALTED IdleIsHalted;
    PPPM_IDLE_INITIATE_WAKE IdleInitiateWake;
    ULONG ProcessorIdleCount;
    _Field_size_(ProcessorIdleCount) PPM_IDLE_STATE_ENTRY State[ANYSIZE_ARRAY];
} PPM_IDLE_STATES_REGISTRATION, *PPPM_IDLE_STATES_REGISTRATION;

typedef struct _PPM_IDLE_DOMAIN_ENTRY
{
    KAFFINITY Members;
} PPM_IDLE_DOMAIN_ENTRY, *PPPM_IDLE_DOMAIN_ENTRY;

typedef struct _PPM_IDLE_DOMAINS_REGISTRATION
{
    ULONG Version;
    ULONG Count;
    KAFFINITY TargetProcessors;
    _Field_size_(Count) PPM_IDLE_DOMAIN_ENTRY State[ANYSIZE_ARRAY];
} PPM_IDLE_DOMAINS_REGISTRATION, *PPPM_IDLE_DOMAINS_REGISTRATION;

/* DISPATCH TABLE **************************************************************/

typedef
NTSTATUS
(NTAPI *PPPM_REGISTER_PERF_STATES)(
    _In_ PPPM_PERF_DOMAIN_REGISTRATION Registration);

typedef
VOID
(NTAPI *PPPM_UPDATE_PERF_STATES)(
    _In_ PPPM_PERF_DOMAIN_UPDATE Update);

typedef
NTSTATUS
(NTAPI *PPPM_REGISTER_PERF_CAP)(
    _In_ PPPM_PROCESSOR_CAP Cap);

typedef
NTSTATUS
(NTAPI *PPPM_REGISTER_IDLE_STATES)(
    _In_ PPPM_IDLE_STATES_REGISTRATION Registration);

typedef
NTSTATUS
(NTAPI *PPPM_REGISTER_IDLE_DOMAINS)(
    _In_ PPPM_IDLE_DOMAINS_REGISTRATION Registration);

/**
 * @brief Gives the driver an energy estimator, the kernel may ignore it.
 */
typedef
VOID
(NTAPI *PPPM_REGISTER_ENERGY_ESTIMATION)(
    _In_opt_ PVOID ComputeEnergy,
    _In_opt_ PVOID SnapEnergyCounters);

typedef struct _PPM_DRIVER_INTERFACE_TABLE
{
    ULONG InterfaceVersion;
    PPPM_REGISTER_PERF_STATES RegisterPerfStates;
    PPPM_UPDATE_PERF_STATES UpdatePerfStates;
    PPPM_REGISTER_PERF_CAP RegisterPerfCap;
    PPPM_REGISTER_IDLE_STATES RegisterIdleStates;
    PPPM_REGISTER_IDLE_DOMAINS RegisterIdleDomains;
    PPPM_REGISTER_ENERGY_ESTIMATION RegisterEnergyEstimation;
} PPM_DRIVER_INTERFACE_TABLE, *PPPM_DRIVER_INTERFACE_TABLE;

/* EOF */
