/*
 * PROJECT:     ReactOS Processor Power Management Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared core of the Intel and AMD processor drivers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntddk.h>
#include <ntpoapi.h>
#include <acpiioct.h>
#include <poclass.h>
#include <wdmguid.h>
#include <intrin.h>
#include <drivers/processor/ppmdrv.h>

#define PPMLIB_TAG                      'lmpP'

#ifndef AFFINITY_MASK
#define AFFINITY_MASK(Index)            ((KAFFINITY)1 << (Index))
#endif

/* Architectural feedback counters */
#define PPMLIB_MSR_MPERF                0xE7
#define PPMLIB_MSR_APERF                0xE8

/* FEATURE MASK ****************************************************************/

/* Idle */
#define PPMLIB_ACPI1_C1                 0x0000000000000001ULL
#define PPMLIB_ACPI1_C2                 0x0000000000000002ULL
#define PPMLIB_ACPI1_C3                 0x0000000000000004ULL
#define PPMLIB_ACPI1_IDLE               0x0000000000000007ULL
#define PPMLIB_CST_C1                   0x0000000000000010ULL
#define PPMLIB_CST_C2                   0x0000000000000020ULL
#define PPMLIB_CST_C3                   0x0000000000000040ULL
#define PPMLIB_PEP_IDLE_V1              0x0000000000000100ULL
#define PPMLIB_PEP_IDLE_V2              0x0000000000000200ULL
#define PPMLIB_PEP_IDLE_MASK            0x0000000000000F00ULL
#define PPMLIB_NATIVE_C1                0x0000000000001000ULL
#define PPMLIB_NATIVE_C2                0x0000000000002000ULL
#define PPMLIB_NATIVE_C3                0x0000000000004000ULL
#define PPMLIB_NATIVE_IDLE              0x000000000007F000ULL
#define PPMLIB_CST_IDLE                 0x000000000007F070ULL
#define PPMLIB_ALL_IDLE                 0x000000000007F077ULL

/* Performance */
#define PPMLIB_ACPI1_THROTTLE           0x0000000000100000ULL
#define PPMLIB_ACPI1_THROTTLE_MP        0x0000000000200000ULL
#define PPMLIB_ACPI1_THROTTLE_MASK      0x0000000000300000ULL
#define PPMLIB_TSS_IO                   0x0000000001000000ULL
#define PPMLIB_TSS_FFH                  0x0000000002000000ULL
#define PPMLIB_TSS_MASK                 0x0000000003000000ULL
#define PPMLIB_THROTTLE_MASK            0x0000000003300000ULL
#define PPMLIB_FEEDBACK                 0x0000000004000000ULL
#define PPMLIB_CPPC                     0x0000000008000000ULL
#define PPMLIB_PSS_IO                   0x0000000010000000ULL
#define PPMLIB_PSS_FFH                  0x0000000020000000ULL
#define PPMLIB_PSS_MASK                 0x0000000030000000ULL
#define PPMLIB_XPSS                     0x0000000040000000ULL
#define PPMLIB_PSTATE_MASK              0x0000000070000000ULL
#define PPMLIB_PCC                      0x0000000080000000ULL
#define PPMLIB_PERF_MASK                0x00000000FF300000ULL

/* High dword */
#define PPMLIB_PEP_IDLE_HIGH            0x0000000300000000ULL
#define PPMLIB_PEP_PERF                 0x0000001000000000ULL
#define PPMLIB_ENERGY                   0x0000002000000000ULL
#define PPMLIB_NATIVE_INTERRUPT         0x0000004000000000ULL
#define PPMLIB_NONWRAPPING_COUNTERS     0x0000008000000000ULL
#define PPMLIB_PEP_PARK                 0x0000010000000000ULL
#define PPMLIB_PEP_ANY                  0x0000011300000F00ULL

/* Default offer before the vendor adds its own */
#define PPMLIB_DEFAULT_CAPABILITIES     0x0000010099100777ULL

/* REGISTERS *******************************************************************/

#define PPMLIB_SPACE_MEMORY             0x00
#define PPMLIB_SPACE_IO                 0x01
#define PPMLIB_SPACE_PCC                0x0A
#define PPMLIB_SPACE_STATIC             0x7E
#define PPMLIB_SPACE_FFH                0x7F

/**
 * @brief An ACPI generic address. AccessBits holds the access width in bits
 * once translated, a static register keeps its value in Address.
 */
typedef struct _PPMLIB_GAS
{
    UCHAR SpaceId;
    UCHAR BitWidth;
    UCHAR BitOffset;
    UCHAR AccessBits;
    ULONG64 Address;
} PPMLIB_GAS, *PPPMLIB_GAS;

typedef struct _PPMLIB_CONTROL_PAIR
{
    PPMLIB_GAS Control;
    PPMLIB_GAS Status;
} PPMLIB_CONTROL_PAIR, *PPPMLIB_CONTROL_PAIR;

/* ACPI OBJECTS ****************************************************************/

typedef struct _PPMLIB_PSTATE
{
    ULONG Frequency;
    ULONG Power;
    ULONG Latency;
    ULONG BusMasterLatency;
    ULONG Control;
    ULONG Status;
} PPMLIB_PSTATE, *PPPMLIB_PSTATE;

typedef struct _PPMLIB_PSS
{
    ULONG Count;
    PPMLIB_PSTATE State[ANYSIZE_ARRAY];
} PPMLIB_PSS, *PPPMLIB_PSS;

typedef struct _PPMLIB_TSTATE
{
    ULONG Percent;
    ULONG Power;
    ULONG Latency;
    ULONG Control;
    ULONG Status;
} PPMLIB_TSTATE, *PPPMLIB_TSTATE;

typedef struct _PPMLIB_TSS
{
    ULONG Count;
    PPMLIB_TSTATE State[ANYSIZE_ARRAY];
} PPMLIB_TSS, *PPPMLIB_TSS;

typedef struct _PPMLIB_CSTATE
{
    PPMLIB_GAS Register;
    UCHAR Type;
    USHORT Latency;
    ULONG Power;
} PPMLIB_CSTATE, *PPPMLIB_CSTATE;

typedef struct _PPMLIB_CST
{
    ULONG Count;
    PPMLIB_CSTATE State[ANYSIZE_ARRAY];
} PPMLIB_CST, *PPPMLIB_CST;

/* One _PSD or _TSD package */
typedef struct _PPMLIB_DEPENDENCY
{
    ULONG Entries;
    ULONG Revision;
    ULONG Domain;
    ULONG Coordination;
    ULONG Processors;
} PPMLIB_DEPENDENCY, *PPPMLIB_DEPENDENCY;

typedef struct _PPMLIB_DEPENDENCY_LIST
{
    ULONG Count;
    PPMLIB_DEPENDENCY Entry[ANYSIZE_ARRAY];
} PPMLIB_DEPENDENCY_LIST, *PPPMLIB_DEPENDENCY_LIST;

/* One _CSD package */
typedef struct _PPMLIB_IDLE_DEPENDENCY
{
    ULONG Entries;
    ULONG Revision;
    ULONG Domain;
    ULONG Coordination;
    ULONG Processors;
    ULONG Index;
} PPMLIB_IDLE_DEPENDENCY, *PPPMLIB_IDLE_DEPENDENCY;

typedef struct _PPMLIB_CSD
{
    ULONG Count;
    PPMLIB_IDLE_DEPENDENCY Entry[ANYSIZE_ARRAY];
} PPMLIB_CSD, *PPPMLIB_CSD;

/* _CPC, revision 1 fills the registers up to Enable */
typedef struct _PPMLIB_CPC
{
    ULONG Entries;
    ULONG Revision;
    PPMLIB_GAS Highest;
    PPMLIB_GAS Nominal;
    PPMLIB_GAS LowestNonlinear;
    PPMLIB_GAS Lowest;
    PPMLIB_GAS Guaranteed;
    PPMLIB_GAS Desired;
    PPMLIB_GAS Minimum;
    PPMLIB_GAS Maximum;
    PPMLIB_GAS Tolerance;
    PPMLIB_GAS TimeWindow;
    PPMLIB_GAS Wraparound;
    PPMLIB_GAS ReferenceCounter;
    PPMLIB_GAS DeliveredCounter;
    PPMLIB_GAS Limited;
    PPMLIB_GAS Enable;
    PPMLIB_GAS AutonomousEnable;
    PPMLIB_GAS ActivityWindow;
    PPMLIB_GAS EnergyPreference;
    PPMLIB_GAS ReferencePerformance;
} PPMLIB_CPC, *PPPMLIB_CPC;

/* _OSC capability buffer a vendor hands in */
typedef struct _PPMLIB_OSC_REQUEST
{
    GUID Uuid;
    ULONG Revision;
    ULONG Count;
    ULONG Capabilities[2];
} PPMLIB_OSC_REQUEST, *PPPMLIB_OSC_REQUEST;

/* _PDC capability buffer a vendor hands in */
typedef struct _PPMLIB_PDC_REQUEST
{
    ULONG Revision;
    ULONG Count;
    ULONG Capabilities[1];
} PPMLIB_PDC_REQUEST, *PPPMLIB_PDC_REQUEST;

/* IOCTL_GET_PROCESSOR_OBJ_INFO reply with the initial APIC ID */
typedef struct _PPMLIB_PROCESSOR_OBJECT
{
    ULONG PhysicalId;
    ULONG BlockAddress;
    UCHAR BlockLength;
    ULONG InitialApicId;
} PPMLIB_PROCESSOR_OBJECT, *PPPMLIB_PROCESSOR_OBJECT;

/* RUNTIME *********************************************************************/

struct _PPMLIB_DEVICE;

typedef
NTSTATUS
(NTAPI *PPPMLIB_SET_STATE)(
    _In_ PVOID Context,
    _In_ ULONG64 Control,
    _In_ ULONG64 Status);

/* Running totals behind a reference/actual counter pair */
typedef struct _PPMLIB_COUNTER
{
    ULONG64 LastActual;
    ULONG64 LastReference;
    ULONG64 TotalActual;
    ULONG64 TotalReference;
    ULONG64 ActualMask;
    ULONG64 ReferenceMask;
    PPPM_READ_DIFFERENTIAL_COUNTER Read;
    PVOID ReadContext;
} PPMLIB_COUNTER, *PPPMLIB_COUNTER;

#define PPMLIB_STATE_PERFORMANCE        1
#define PPMLIB_STATE_THROTTLE           2

typedef struct _PPMLIB_PERF_STATE
{
    ULONG64 Control;
    ULONG64 Status;
    ULONG Frequency;
    ULONG Power;
    UCHAR Percent;
    UCHAR Type;
    USHORT Latency;
} PPMLIB_PERF_STATE, *PPPMLIB_PERF_STATE;

/* P and T states of a domain, shared by every member */
typedef struct _PPMLIB_PERF_TABLE
{
    PPPMLIB_SET_STATE PStateHandler;
    PPPMLIB_SET_STATE TStateHandler;
    PVOID PStateContext;
    PVOID TStateContext;
    ULONG StateCount;
    ULONG PStateCount;
    ULONG TStateCount;
    PPMLIB_PERF_STATE State[ANYSIZE_ARRAY];
} PPMLIB_PERF_TABLE, *PPPMLIB_PERF_TABLE;

typedef struct _PPMLIB_PT_RUNTIME
{
    ULONG CurrentType;
    ULONG CurrentPercent;
    ULONG TargetState;
    PPPMLIB_PERF_TABLE Table;
    PPMLIB_COUNTER Hardware;
} PPMLIB_PT_RUNTIME, *PPPMLIB_PT_RUNTIME;

typedef struct _PPMLIB_FEEDBACK_RUNTIME
{
    PPMLIB_COUNTER Hardware;
    ULONG NominalFrequency;
} PPMLIB_FEEDBACK_RUNTIME, *PPPMLIB_FEEDBACK_RUNTIME;

typedef struct _PPMLIB_CPC_RUNTIME
{
    ULONG64 Highest;
    ULONG64 Nominal;
    ULONG64 LowestNonlinear;
    ULONG64 Lowest;
    ULONG64 Reference;
    ULONG64 Selected;
    ULONG64 Guaranteed;
    ULONG NominalFrequency;
    ULONG HighestPercent;
    ULONG HighestObservedPercent;
    ULONG LowestNonlinearPercent;
    ULONG LowestPercent;
    ULONG GuaranteedPercent;
    BOOLEAN EnablePresent;
    BOOLEAN DesiredPresent;
    BOOLEAN MinimumPresent;
    BOOLEAN MaximumPresent;
    BOOLEAN TolerancePresent;
    BOOLEAN GuaranteedPresent;
    BOOLEAN NativeInterruptEnabled;
    PPPMLIB_CPC Cpc;
    PPMLIB_GAS SharedRegister;
    PPMLIB_COUNTER Delivered;
    PPMLIB_COUNTER Hardware;
    struct _PPMLIB_DEVICE *Device;
    KDPC NativeDpc;
} PPMLIB_CPC_RUNTIME, *PPPMLIB_CPC_RUNTIME;

/* Idle */

typedef struct _PPMLIB_IDLE_CONTEXT
{
    PPMLIB_GAS Register;
    ULONG Hints;
    ULONG Extensions;
    PVOID MonitorAddress;
    ULONG Flags;
} PPMLIB_IDLE_CONTEXT, *PPPMLIB_IDLE_CONTEXT;

/* MWAIT flags, taken from the access size field of an FFH _CST entry */
#define PPMLIB_MWAIT_HW_COORDINATED     0x00000001
#define PPMLIB_MWAIT_BUS_MASTER_CHECK   0x00000002

typedef
VOID
(NTAPI *PPPMLIB_IDLE_HANDLER)(
    _In_ PPPMLIB_IDLE_CONTEXT Context);

typedef
NTSTATUS
(NTAPI *PPPMLIB_IDLE_CHECK)(
    _In_ PPPMLIB_IDLE_CONTEXT Context);

struct _PPMLIB_IDLE_DOMAIN;

/* Links one _CST entry of a processor into a _CSD domain */
typedef struct _PPMLIB_IDLE_LINK
{
    PPPMLIB_CSTATE CState;
    struct _PPMLIB_IDLE_DOMAIN *Domain;
    LIST_ENTRY Link;
} PPMLIB_IDLE_LINK, *PPPMLIB_IDLE_LINK;

typedef struct _PPMLIB_IDLE_STATE
{
    PPPMLIB_CSTATE CState;
    PPPMLIB_IDLE_LINK Entry;
    PPPMLIB_IDLE_CHECK Check;
    PPPMLIB_IDLE_HANDLER Handler;
    PPMLIB_IDLE_CONTEXT Context;
    BOOLEAN Native;
} PPMLIB_IDLE_STATE, *PPPMLIB_IDLE_STATE;

#define PPMLIB_MAX_IDLE_STATES          3

typedef struct _PPMLIB_IDLE_RUNTIME
{
    ULONG Selected;
    ULONG Target;
    ULONG Preferred;
    UCHAR PreferredCount;
    UCHAR PreferredStates[PPMLIB_MAX_IDLE_STATES];
    ULONG64 LastIdle;
    ULONG64 LastTotal;
    ULONG Count;
    PPMLIB_IDLE_STATE State[PPMLIB_MAX_IDLE_STATES];
} PPMLIB_IDLE_RUNTIME, *PPPMLIB_IDLE_RUNTIME;

/* Global domain bookkeeping */

typedef struct _PPMLIB_PERF_DOMAIN
{
    LIST_ENTRY Link;
    LIST_ENTRY Members;
    ULONG Count;
    ULONG Domain;
    ULONG Coordination;
    ULONG Total;
} PPMLIB_PERF_DOMAIN, *PPPMLIB_PERF_DOMAIN;

typedef struct _PPMLIB_PACKAGE
{
    LIST_ENTRY Link;
    LIST_ENTRY Members;
    ULONG Count;
    ULONG Total;
    KAFFINITY Mask;
} PPMLIB_PACKAGE, *PPPMLIB_PACKAGE;

typedef struct _PPMLIB_IDLE_DOMAIN
{
    LIST_ENTRY Link;
    LIST_ENTRY Entries;
    ULONG Count;
    ULONG Domain;
    ULONG Coordination;
    ULONG Total;
    KAFFINITY Mask;
} PPMLIB_IDLE_DOMAIN, *PPPMLIB_IDLE_DOMAIN;

/* DEVICE **********************************************************************/

typedef struct _PPMLIB_DEVICE
{
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT Pdo;
    PDEVICE_OBJECT LowerDevice;
    PFILE_OBJECT PdoFile;
    UNICODE_STRING CoolingInterface;

    LIST_ENTRY DeviceLink;
    LIST_ENTRY DomainLink;
    LIST_ENTRY PackageLink;
    PPPMLIB_PERF_DOMAIN PerfDomain;
    PPPMLIB_PACKAGE Package;

    ULONG64 Capabilities;
    ULONG64 Enabled;
    ULONG64 Found;
    ULONG Invalidated;

    ULONG AcpiId;
    ULONG ApicId;
    ULONG NtNumber;
    PPMLIB_GAS PBlk;
    PACPI_EVAL_OUTPUT_BUFFER OscReply;
    PULONG OscOutput;
    ULONG OscOutputLength;

    /* Serializes evaluation and replacement of the ACPI objects below */
    KMUTEX AcpiLock;

    PPPMLIB_CST Cst;
    PPPMLIB_CSD Csd;
    PPPMLIB_IDLE_LINK IdleLinks;
    ULONG IdleLinkCount;
    PPPMLIB_IDLE_RUNTIME IdleRuntime;
    BOOLEAN IdleInitialized;
    BOOLEAN MwaitBreakOnInterrupt;
    PVOID MonitorLine;

    PPMLIB_CONTROL_PAIR Pct;
    PPPMLIB_PSS Pss;
    PPMLIB_CONTROL_PAIR Ptc;
    PPPMLIB_TSS Tss;
    PPPMLIB_CPC Cpc;
    ULONG64 Highest;
    ULONG64 Nominal;
    ULONG64 LowestNonlinear;
    ULONG64 Lowest;
    PPPMLIB_DEPENDENCY_LIST Psd;
    PPPMLIB_DEPENDENCY_LIST Tsd;
    PPPMLIB_DEPENDENCY_LIST Xsd;

    ULONG Ppc;
    ULONG Tpc;
    ULONG PlatformCap;
    ULONG ThermalCap;

    PPPMLIB_SET_STATE SetPState;
    PPPMLIB_SET_STATE SetTState;
    PPPM_READ_DIFFERENTIAL_COUNTER PStateFeedback;
    PPPM_PERF_POLICY_HANDLER SetBoostPolicy;
    PPPM_PERF_POLICY_HANDLER SetBoostMode;
    PPPM_GET_FFH_THROTTLE_STATE GetFfhThrottleState;

    PPPMLIB_PT_RUNTIME PtRuntime;
    PPPMLIB_CPC_RUNTIME CpcRuntime;
    PPPMLIB_FEEDBACK_RUNTIME FeedbackRuntime;

    ACPI_INTERFACE_STANDARD AcpiInterface;
    BOOLEAN AcpiInterfaceHeld;
    BOOLEAN NotifyRegistered;
    BOOLEAN ResumeFromSleep;
    BOOLEAN Started;
} PPMLIB_DEVICE, *PPPMLIB_DEVICE;

/* VENDOR INTERFACE ************************************************************/

/**
 * @brief What a vendor driver fills in at load. Every hook is optional.
 */
typedef struct _PPMLIB_VENDOR
{
    PPPMLIB_PDC_REQUEST Pdc;
    ULONG PdcSize;
    PPPMLIB_OSC_REQUEST Osc;
    ULONG OscSize;
    NTSTATUS (NTAPI *ValidatePStates)(_In_ PPPMLIB_CONTROL_PAIR Pct, _In_ PPPMLIB_PSS Pss, _Inout_ PULONG Errors);
    PPPMLIB_SET_STATE SetPStateFfh;
    PPPMLIB_SET_STATE SetTStateFfh;
    PPPM_READ_DIFFERENTIAL_COUNTER PStateFeedback;
    PPPM_GET_FFH_THROTTLE_STATE GetFfhThrottleState;
    PPPM_PERF_POLICY_HANDLER BoostPolicyHandler;
    PPPM_PERF_POLICY_HANDLER BoostModeHandler;
    NTSTATUS (NTAPI *InitializeCStates)(_Inout_ PPPMLIB_DEVICE Device);
    NTSTATUS (NTAPI *DecodeCState)(_In_ PPPMLIB_DEVICE Device, _In_ PPPMLIB_CSTATE CState, _Out_opt_ PBOOLEAN InterruptsEnabled, _Out_opt_ PPPMLIB_IDLE_STATE State);
    BOOLEAN (NTAPI *IsThrottleEfficient)(_In_ PPPMLIB_CPC Cpc);
    NTSTATUS (NTAPI *ConnectNativeInterrupt)(_In_ PPPMLIB_DEVICE Device);
    VOID (NTAPI *EnableNativeInterrupt)(VOID);
    NTSTATUS (NTAPI *DeviceStart)(_In_ PPPMLIB_DEVICE Device);
} PPMLIB_VENDOR, *PPPMLIB_VENDOR;

typedef
NTSTATUS
(NTAPI *PPPMLIB_VENDOR_INIT)(
    _Inout_ PPPMLIB_VENDOR Vendor,
    _Out_ PULONG64 Capabilities);

/* GLOBAL STATE ****************************************************************/

#define PPMLIB_MAX_PROCESSORS           (sizeof(KAFFINITY) * 8)

typedef struct _PPMLIB_FADT_INFO
{
    PPMLIB_GAS Pm1aEvent;
    PPMLIB_GAS Pm1bEvent;
    PPMLIB_GAS Pm1aControl;
    PPMLIB_GAS Pm1bControl;
    PPMLIB_GAS Pm2Control;
    ULONG SmiCommand;
    UCHAR DutyWidth;
    UCHAR DutyOffset;
    USHORT C2Latency;
    USHORT C3Latency;
    UCHAR PStateControl;
    UCHAR CStateControl;
} PPMLIB_FADT_INFO, *PPPMLIB_FADT_INFO;

typedef struct _PPMLIB_DATA
{
    PDRIVER_OBJECT DriverObject;
    PPM_DRIVER_INTERFACE_TABLE Kernel;
    PPMLIB_VENDOR Vendor;
    ULONG64 Capabilities;
    ULONG64 DisableMask;
    ULONG Overrides;
    BOOLEAN AllowPepPerf;
    BOOLEAN SyntheticPsdAllowed;
    ULONG ProcessorCount;
    KAFFINITY Enumerated;
    PPPMLIB_DEVICE Devices[PPMLIB_MAX_PROCESSORS];
    KMUTEX Lock;
    LIST_ENTRY DeviceList;
    LIST_ENTRY PerfDomainList;
    LIST_ENTRY PackageList;
    LIST_ENTRY IdleDomainList;
    PPMLIB_FADT_INFO Fadt;
    volatile LONG BusMasterReload;
} PPMLIB_DATA, *PPPMLIB_DATA;

extern PPMLIB_DATA PpmLib;

/* Validation errors kept for diagnostics */
#define PPMLIB_ERROR_CST                0x00000008
#define PPMLIB_ERROR_CSD                0x00000010
#define PPMLIB_ERROR_PCT                0x00000020
#define PPMLIB_ERROR_PSS                0x00000040
#define PPMLIB_ERROR_PPC                0x00000100
#define PPMLIB_ERROR_PSD                0x00000200
#define PPMLIB_ERROR_PTC                0x00000400
#define PPMLIB_ERROR_TSS                0x00000800
#define PPMLIB_ERROR_TPC                0x00001000
#define PPMLIB_ERROR_TSD                0x00002000
#define PPMLIB_ERROR_OSC                0x00010000
#define PPMLIB_ERROR_PDC                0x00020000
#define PPMLIB_ERROR_CPC                0x00040000

/* FUNCTIONS *******************************************************************/

FORCEINLINE
VOID
PpmLibAcquireGlobal(VOID)
{
    KeWaitForSingleObject(&PpmLib.Lock, Executive, KernelMode, FALSE, NULL);
}

FORCEINLINE
VOID
PpmLibReleaseGlobal(VOID)
{
    KeReleaseMutex(&PpmLib.Lock, FALSE);
}

FORCEINLINE
VOID
PpmLibAcquireAcpi(
    _In_ PPPMLIB_DEVICE Device)
{
    KeWaitForSingleObject(&Device->AcpiLock, Executive, KernelMode, FALSE, NULL);
}

FORCEINLINE
VOID
PpmLibReleaseAcpi(
    _In_ PPPMLIB_DEVICE Device)
{
    KeReleaseMutex(&Device->AcpiLock, FALSE);
}

/* init.c */

NTSTATUS
NTAPI
PpmLibDriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath,
    _In_ PPPMLIB_VENDOR_INIT VendorInit);

ULONG
NTAPI
PpmLibQueryNominalFrequency(VOID);

/* device.c */

DRIVER_ADD_DEVICE PpmLibAddDevice;

_Dispatch_type_(IRP_MJ_PNP)
DRIVER_DISPATCH PpmLibDispatchPnp;

_Dispatch_type_(IRP_MJ_POWER)
DRIVER_DISPATCH PpmLibDispatchPower;

_Dispatch_type_(IRP_MJ_CREATE)
_Dispatch_type_(IRP_MJ_CLOSE)
_Dispatch_type_(IRP_MJ_CLEANUP)
DRIVER_DISPATCH PpmLibDispatchCreateClose;

_Dispatch_type_(IRP_MJ_SYSTEM_CONTROL)
DRIVER_DISPATCH PpmLibDispatchForward;

VOID
NTAPI
PpmLibCapChange(
    _Inout_ PPPMLIB_DEVICE Device,
    _In_ ULONG Ppc,
    _In_ ULONG Tpc,
    _In_ ULONG PlatformCap);

VOID
NTAPI
PpmLibAcquireBiosControl(
    _In_ PPPMLIB_DEVICE Device);

NTSTATUS
NTAPI
PpmLibSleepNotify(
    _In_ POWER_STATE_HANDLER_TYPE State,
    _In_ PVOID Context,
    _In_ BOOLEAN Entering);

/* acpi.c */

NTSTATUS
NTAPI
PpmLibEvaluate(
    _In_ PPPMLIB_DEVICE Device,
    _In_reads_bytes_(InputSize) PVOID Input,
    _In_ ULONG InputSize,
    _Outptr_result_maybenull_ PACPI_EVAL_OUTPUT_BUFFER *Output);

NTSTATUS
NTAPI
PpmLibGetProcessorObject(
    _In_ PPPMLIB_DEVICE Device,
    _Out_ PPPMLIB_PROCESSOR_OBJECT Info);

NTSTATUS
NTAPI
PpmLibEvaluateOsc(
    _Inout_ PPPMLIB_DEVICE Device,
    _In_ PPPMLIB_OSC_REQUEST Request);

NTSTATUS
NTAPI
PpmLibEvaluatePdc(
    _In_ PPPMLIB_DEVICE Device,
    _In_ PPPMLIB_PDC_REQUEST Request,
    _In_ ULONG RequestSize);

NTSTATUS
NTAPI
PpmLibEvaluateControlPair(
    _In_ PPPMLIB_DEVICE Device,
    _In_ BOOLEAN Throttle,
    _Out_ PPPMLIB_CONTROL_PAIR Pair);

NTSTATUS
NTAPI
PpmLibEvaluatePss(
    _In_ PPPMLIB_DEVICE Device,
    _Outptr_ PPPMLIB_PSS *Pss);

NTSTATUS
NTAPI
PpmLibEvaluateTss(
    _In_ PPPMLIB_DEVICE Device,
    _Outptr_ PPPMLIB_TSS *Tss);

NTSTATUS
NTAPI
PpmLibEvaluatePpc(
    _In_ PPPMLIB_DEVICE Device,
    _Out_ PULONG Ppc);

NTSTATUS
NTAPI
PpmLibEvaluateTpc(
    _In_ PPPMLIB_DEVICE Device,
    _Out_ PULONG Tpc);

NTSTATUS
NTAPI
PpmLibEvaluateCst(
    _In_ PPPMLIB_DEVICE Device,
    _Outptr_ PPPMLIB_CST *Cst);

NTSTATUS
NTAPI
PpmLibEvaluateCsd(
    _In_ PPPMLIB_DEVICE Device,
    _Outptr_ PPPMLIB_CSD *Csd);

NTSTATUS
NTAPI
PpmLibEvaluateDependency(
    _In_ PPPMLIB_DEVICE Device,
    _In_ BOOLEAN Throttle,
    _Outptr_ PPPMLIB_DEPENDENCY_LIST *List);

NTSTATUS
NTAPI
PpmLibEvaluateCpc(
    _In_ PPPMLIB_DEVICE Device,
    _Outptr_ PPPMLIB_CPC *Cpc);

NTSTATUS
NTAPI
PpmLibAcquireAcpiInterface(
    _Inout_ PPPMLIB_DEVICE Device);

VOID
NTAPI
PpmLibAcpiNotify(
    _In_ PVOID Context,
    _In_ ULONG NotifyCode);

VOID
NTAPI
PpmLibReleaseAcpiInterface(
    _Inout_ PPPMLIB_DEVICE Device);

/* gas.c */

VOID
NTAPI
PpmLibTranslateAccessSize(
    _Inout_ PPPMLIB_GAS Register);

BOOLEAN
NTAPI
PpmLibIsValidRegister(
    _In_ PPPMLIB_GAS Register);

BOOLEAN
NTAPI
PpmLibRegisterPresent(
    _In_ PPPMLIB_GAS Register);

ULONG64
NTAPI
PpmLibReadRaw(
    _In_ PPPMLIB_GAS Register);

VOID
NTAPI
PpmLibWriteRaw(
    _In_ PPPMLIB_GAS Register,
    _In_ ULONG64 Value);

ULONG64
NTAPI
PpmLibReadRegister(
    _In_opt_ PPPMLIB_GAS Register);

VOID
NTAPI
PpmLibWriteRegister(
    _In_opt_ PPPMLIB_GAS Register,
    _In_ ULONG64 Value);

VOID
NTAPI
PpmLibReadAperfMperf(
    _In_opt_ PVOID Context,
    _In_ BOOLEAN Reset,
    _Out_ PULONG64 Reference,
    _Out_ PULONG64 Actual);

ULONG64
NTAPI
PpmLibMergeField(
    _In_ PPPMLIB_GAS Field,
    _In_ ULONG64 Raw,
    _In_ ULONG64 Value);

/* perf.c */

NTSTATUS
NTAPI
PpmLibInitPerfStates(
    _Inout_ PPPMLIB_DEVICE Device);

NTSTATUS
NTAPI
PpmLibInitThrottleStates(
    _Inout_ PPPMLIB_DEVICE Device);

VOID
NTAPI
PpmLibInitDomains(
    _Inout_ PPPMLIB_DEVICE Device);

VOID
NTAPI
PpmLibRegisterPerformance(
    _Inout_ PPPMLIB_DEVICE Device);

NTSTATUS
NTAPI
PpmLibRegisterCap(
    _In_ PPPMLIB_DEVICE Device);

ULONG
NTAPI
PpmLibGetDomainMembers(
    _In_ PPPMLIB_DEVICE Device,
    _Out_writes_(PPMLIB_MAX_PROCESSORS) PPPMLIB_DEVICE *Members,
    _Out_ PUCHAR Coordination);

PPPM_PERF_DOMAIN_REGISTRATION
NTAPI
PpmLibAllocateRegistration(
    _In_ ULONG ProcessorCount,
    _In_ ULONG CounterCount,
    _In_ UCHAR Coordination);

NTSTATUS
NTAPI
PpmLibSetStateIo(
    _In_ PVOID Context,
    _In_ ULONG64 Control,
    _In_ ULONG64 Status);

VOID
NTAPI
PpmLibReadWrappingCounter(
    _In_ PVOID Context,
    _In_ BOOLEAN Reset,
    _Out_ PULONG64 Reference,
    _Out_ PULONG64 Actual);

/* cppc.c */

NTSTATUS
NTAPI
PpmLibInitCpc(
    _Inout_ PPPMLIB_DEVICE Device);

NTSTATUS
NTAPI
PpmLibRegisterCpc(
    _Inout_ PPPMLIB_DEVICE Device);

VOID
NTAPI
PpmLibCpcGuaranteedChange(
    _In_ PPPMLIB_DEVICE Device);

VOID
NTAPI
PpmLibCpcHighestChange(
    _In_ PPPMLIB_DEVICE Device);

/* idle.c */

NTSTATUS
NTAPI
PpmLibInitCStates(
    _Inout_ PPPMLIB_DEVICE Device);

NTSTATUS
NTAPI
PpmLibInitCstAndValidate(
    _In_ PPPMLIB_DEVICE Device,
    _Outptr_ PPPMLIB_CST *Cst);

NTSTATUS
NTAPI
PpmLibInitIdleDomain(
    _In_ PPPMLIB_DEVICE Device,
    _In_ PPPMLIB_CST Cst,
    _Outptr_result_maybenull_ PPPMLIB_CSD *Csd);

NTSTATUS
NTAPI
PpmLibRegisterIdleStates(
    _Inout_ PPPMLIB_DEVICE Device);

NTSTATUS
NTAPI
PpmLibBusMasterCheck(VOID);

VOID
NTAPI
PpmLibCStateChange(
    _Inout_ PPPMLIB_DEVICE Device);

/* EOF */
