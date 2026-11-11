/*
 * PROJECT:     ReactOS Qualcomm subsystem manager
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <acpiioct.h>
#include <initguid.h>
#include <wdmguid.h>
#include <qcom/smdif.h>
#include <qcom/tzif.h>
#include <qcom/pilif.h>

#define SS_TAG                      'sSqQ'

/* {55BECA03-BA2C-4FBC-9EFC-EE8ECA56F0A9} */
DEFINE_GUID(GUID_DEVINTERFACE_QCOM_AMSS,
            0x55beca03, 0xba2c, 0x4fbc, 0x9e, 0xfc, 0xee, 0x8e, 0xca, 0x56, 0xf0, 0xa9);

/* {F2D900FD-4D9F-49A4-A025-6758D858CFD6} */
DEFINE_GUID(GUID_DEVINTERFACE_QCOM_AMSS_GNSS,
            0xf2d900fd, 0x4d9f, 0x49a4, 0xa0, 0x25, 0x67, 0x58, 0xd8, 0x58, 0xcf, 0xd6);

/* {4A190298-5AC7-4CC1-9382-53C0AA13448F} */
DEFINE_GUID(GUID_BUS_TYPE_QCOM_AMSS,
            0x4a190298, 0x5ac7, 0x4cc1, 0x93, 0x82, 0x53, 0xc0, 0xaa, 0x13, 0x44, 0x8f);

/* Events */
#define SS_EV_NONE                  0
#define SS_EV_START                 1
#define SS_EV_VOTE_ON_DONE          2
#define SS_EV_HANDOVER_DONE         3
#define SS_EV_VOTE_OFF_DONE         4
#define SS_EV_BOOTED                5
#define SS_EV_MODEM_READY           6
#define SS_EV_MODEM_HANDOVER        7
#define SS_EV_POWERED_UP            8
#define SS_EV_RESTART_OK            9
#define SS_EV_PEER_SMD              10
#define SS_EV_PEER_IPA              11
#define SS_EV_PEER_OTHER            12
#define SS_EV_PEERS_DONE            13
#define SS_EV_FAILURE               14
#define SS_EV_FATAL                 16
#define SS_EV_DUMP_STARTED          17
#define SS_EV_DUMP_STOPPED          18
#define SS_EV_UNLOADED              19
#define SS_EV_BROADCAST             20
#define SS_EV_REVERT                21
#define SS_EV_CHILDREN_GONE         31
#define SS_EV_POWERED_DOWN          42
#define SS_EV_SHUTTING_DOWN         43
#define SS_EV_SHUTDOWN_READY        44
#define SS_EV_QMI_STARTED           45
#define SS_EV_STOP_ACK              46
#define SS_EV_TIMEOUT               47
#define SS_EV_RESTART_REQUEST       48
#define SS_EV_APPS_SHUTDOWN         49
#define SS_EV_MBA_FAILED            50
#define SS_EV_IMAGE_FAILED          51
#define SS_EV_TIMER_CANCELLED       52
#define SS_EV_MONITOR_STOPPED       53
#define SS_EV_FORCE_STOP_SENT       54
#define SS_EV_FORCE_STOP_FAILED     55
#define SS_EV_RESTART_REFUSED       56
#define SS_EV_STATE_PUBLISHED       57
#define SS_EV_SHUTDOWN_DUMP_DONE    58
#define SS_EV_SHUTDOWN_DUMP_FAILED  59
#define SS_EV_HALTED                60

/* Returned by actions but handled by no state, the machine stops there */
#define SS_EV_NO_MESSAGE            34
#define SS_EV_BROADCAST_FAILED      69

/* States, numbered like the event log of the platform driver so traces compare */
#define SS_ST_INIT                  0
#define SS_ST_BOOTED                1
#define SS_ST_POWERED_UP            2
#define SS_ST_WAIT_MODEM            5
#define SS_ST_VOTING_ON             6
#define SS_ST_SSR_WAIT_ACKS         7
#define SS_ST_READY                 12
#define SS_ST_DUMP_STARTED          13
#define SS_ST_FATAL                 14
#define SS_ST_SSR_WAIT_CHILDREN     15
#define SS_ST_ERROR                 16
#define SS_ST_DECIDE                25
#define SS_ST_BOTH_BITS             37
#define SS_ST_GOT_HANDOVER          38
#define SS_ST_WAIT_UNVOTE           39
#define SS_ST_WAIT_UNVOTE_FATAL     40
#define SS_ST_BOOT_ERROR            42
#define SS_ST_BOOT_ERROR_WAIT       43
#define SS_ST_BOOT_ERROR_UNVOTE     44
#define SS_ST_CANCEL_TIMER          45
#define SS_ST_SSR_STOPPING          46
#define SS_ST_SHUTDOWN_REQUEST      47
#define SS_ST_RESTART_REQUEST       48
#define SS_ST_SHUTDOWN_STOPPING     49
#define SS_ST_RESTART_STOPPING      50
#define SS_ST_SHUTDOWN_CRASH        51
#define SS_ST_RESTART_CRASH         52
#define SS_ST_OFF                   53
#define SS_ST_SHUTDOWN_DUMP         54
#define SS_ST_GOT_READY             55
#define SS_ST_UNVOTED               56
#define SS_ST_SYSMON_FORWARD        58
#define SS_ST_RESTART_POWER_CYCLE   59
#define SS_ST_SSR_POWER_CYCLE       60
#define SS_ST_REBOOTED_AFTER_SSR    61
#define SS_ST_DUMP_ABORT            62
#define SS_ST_LOAD_TIMEOUT          63
#define SS_ST_LOAD_TIMEOUT_UNVOTE   64
#define SS_ST_LOAD_TIMEOUT_UNVOTED  65
#define SS_ST_REBOOT_MBA_FAILED     66
#define SS_ST_STARTING_QMI          67
#define SS_ST_SHUTDOWN_FORCE_STOP   68
#define SS_ST_RESTART_FORCE_STOP    69
#define SS_ST_IMAGE_FAILED          70

/* Event parameters for SS_EV_FATAL, also the failure reason */
#define SS_FAULT_WATCHDOG           1
#define SS_FAULT_ERR_FATAL          2
#define SS_FAULT_MONITOR            4
#define SS_FAULT_STATE_MACHINE      5

/* Power votes, the platform power engine's own numbering */
#define SS_VOTE_ON                  0
#define SS_VOTE_HANDOVER            1
#define SS_VOTE_OFF                 2
#define SS_VOTE_MSS_CLOCK           3

/* States published to restart peers */
#define SS_PEER_STOP_REQUESTED      2
#define SS_PEER_DOWN                3
#define SS_PEER_GOING_DOWN          4
#define SS_PEER_BOOTING             5
#define SS_PEER_UP                  6
#define SS_PEER_CRASHED             8

/* SMP2P entries shared with the modem */
#define SS_SMP2P_HOST_MODEM         1
#define SS_SMP2P_OUT_NAME           "master-kernel"
#define SS_SMP2P_IN_NAME            "slave-kernel"
#define SS_SMP2P_ERR_FATAL          0x0001
#define SS_SMP2P_READY              0x0002
#define SS_SMP2P_HANDOVER           0x0004
#define SS_SMP2P_STOP_ACK           0x0008
#define SS_SMP2P_NO_DUMP            0x8000
#define SS_SMP2P_OUT_CLEAR          0
#define SS_SMP2P_OUT_FORCE_STOP     1
#define SS_SMP2P_OUT_GRACEFUL_STOP  2
#define SS_SMP2P_NOT_READY          (-3)
#define SS_SMP2P_NOT_CREATED        (-6)

/* SMEM items */
#define SS_SMEM_CRASH_REASON        421
#define SS_SMEM_IMAGE_VERSIONS      469

/* Chip families selecting the Q6 power up sequence */
#define SS_FAMILY_UNKNOWN           0
#define SS_FAMILY_8994              0x32
#define SS_FAMILY_8992              0x39

/* MSS registers mapped at device add */
typedef enum _SS_REG
{
    SsRegMssCfgAhbCbcr,
    SsRegMssQ6AxiCbcr,
    SsRegMssClockVote,
    SsRegMssRestart,
    SsRegQ6Reset,
    SsRegQ6GfmuxCtl,
    SsRegQ6PwrCtl,
    SsRegQ6XoCbcr,
    SsRegQ6StrapAcc,
    SsRegHaltQ6Req,
    SsRegHaltQ6Ack,
    SsRegHaltQ6Idle,
    SsRegHaltModemReq,
    SsRegHaltModemAck,
    SsRegHaltModemIdle,
    SsRegHaltNcReq,
    SsRegHaltNcAck,
    SsRegHaltNcIdle,
    SsRegCount
} SS_REG;

typedef struct _SS_DEVICE *PSS_DEVICE;

typedef ULONG
(*PSS_ACTION)(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param);

typedef struct _SS_TRANSITION
{
    LONG State;
    LONG Event;
    LONG Next;
    PSS_ACTION Action;
} SS_TRANSITION;

typedef struct _SS_HISTORY
{
    ULONG Event;
    ULONG State;
    ULONG64 Time;
} SS_HISTORY;

#define SS_HISTORY_DEPTH            40

/* Reason text handed to the bugcheck and the dump collector */
typedef struct _SS_FAILURE_RECORD
{
    ULONG SubsystemType;
    ULONG Reserved[5];
    ULONG Length;
    CHAR Reason[ANYSIZE_ARRAY];
} SS_FAILURE_RECORD, *PSS_FAILURE_RECORD;

/* One CHLD entry */
typedef struct _SS_CHILD
{
    UNICODE_STRING DeviceId;
    ULONG DeviceType;
    ULONG Instance;
    ULONG RemovalPolicy;
    ULONG RemovalHook;
    ULONG Index;
    BOOLEAN Missing;
    IO_CSQ WakeQueue;
    LIST_ENTRY WakeIrps;
    KSPIN_LOCK WakeLock;
} SS_CHILD, *PSS_CHILD;

#define SS_CHILD_POLICY_KEEP        0
#define SS_CHILD_POLICY_SSR         1
#define SS_CHILD_POLICY_SHUTDOWN    2
#define SS_CHILD_HOOK_CLEANUP       1
#define SS_CHILD_HOOK_SURPRISE      2
#define SS_CHILD_TYPE_MODEM         0x2B

typedef struct _SS_CHILD_DESCRIPTION
{
    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER Header;
    ULONG Index;
} SS_CHILD_DESCRIPTION, *PSS_CHILD_DESCRIPTION;

typedef struct _SS_PDO
{
    PSS_DEVICE Parent;
    ULONG Index;
} SS_PDO, *PSS_PDO;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SS_PDO, SsGetPdo)

typedef struct _SS_DEVICE
{
    LIST_ENTRY Link;
    WDFDEVICE WdfDevice;

    /* State machine */
    WDFWAITLOCK FsmLock;
    LONG State;
    SS_HISTORY History[SS_HISTORY_DEPTH];
    ULONG HistoryNext;

    /* Configuration */
    BOOLEAN GracefulShutdown;
    BOOLEAN ShutdownInProgress;
    BOOLEAN RestartEnabled;
    ULONG MaxRestartCount;
    ULONG RestartTimerMinutes;
    BOOLEAN CrashDetection;
    ULONG LoadTimerSeconds;
    ULONG DebugMss;
    ULONG RmbTimeoutMs;
    ULONG ChipFamily;
    WCHAR FwImagePath[QCOM_PIL_PATH_LENGTH];
    WCHAR SwImagePath[QCOM_PIL_PATH_LENGTH];

    /* Hardware */
    PULONG Regs[SsRegCount];
    PUCHAR Rmb;
    WDFINTERRUPT Watchdog;
    volatile LONG WatchdogArmed;
    WDFTIMER Timer;

    /* Image loader */
    WDFIOTARGET Pil;
    QCOM_PIL_MSA_RMB MsaRmb;
    QCOM_PIL_MSA_IMAGE MsaMba;
    QCOM_PIL_MSA_IMAGE MsaModem;

    /* Shared memory and SMP2P */
    WDFIOTARGET Smd;
    QCOM_SMP2P_INTERFACE Smp2p;
    QCOM_SMEM_INTERFACE Smem;
    QCOM_SMDL_INTERFACE Smdl;
    PQCOM_SMP2P_ENTRY Outbound;
    PQCOM_SMP2P_ENTRY Inbound;
    PKTHREAD Monitor;
    KEVENT MonitorStop;

    /* System monitor channel */
    WDFWAITLOCK SysmonLock;
    QCOM_SMDL_HANDLE Sysmon;
    KEVENT SysmonOpened;
    KEVENT SysmonReadable;

    /* TrustZone, kept as a power relation */
    PVOID TzNotification;
    KEVENT TzArrived;
    WDFIOTARGET Tz;
    PDEVICE_OBJECT TzDevice;

    /* Restart bookkeeping */
    ULONG BootAttempts;
    BOOLEAN WindowValid;
    LARGE_INTEGER WindowStart;
    ULONG WindowBase;
    BOOLEAN NoDump;
    ULONG Acks;
    ULONG AcksRequired;
    PSS_FAILURE_RECORD Failure;
    PVOID VersionPage;
    KEVENT OffEvent;

    /* Children */
    WDFCHILDLIST ChildList;
    PSS_CHILD Children;
    ULONG ChildCount;
    LONG ChildrenPending;
} SS_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SS_DEVICE, SsGetDevice)

/* fsm.c */
extern const SS_TRANSITION SsAmssTable[];

VOID
SsDispatch(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param);

NTSTATUS
SsPost(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param);

/* hw.c */
NTSTATUS
SsMapRegisters(
    _Inout_ PSS_DEVICE Device);

VOID
SsUnmapRegisters(
    _Inout_ PSS_DEVICE Device);

NTSTATUS
SsBringUpMba(
    _Inout_ PSS_DEVICE Device);

NTSTATUS
SsLoadModemImage(
    _Inout_ PSS_DEVICE Device);

NTSTATUS
SsUnloadImages(
    _Inout_ PSS_DEVICE Device);

VOID
SsHaltAxi(
    _Inout_ PSS_DEVICE Device);

VOID
SsAssertReset(
    _Inout_ PSS_DEVICE Device);

VOID
SsClocksOff(
    _Inout_ PSS_DEVICE Device);

VOID
SsArmWatchdog(
    _Inout_ PSS_DEVICE Device);

VOID
SsMaskWatchdog(
    _Inout_ PSS_DEVICE Device);

/* monitor.c */
NTSTATUS
SsStartMonitor(
    _Inout_ PSS_DEVICE Device);

VOID
SsStopMonitor(
    _Inout_ PSS_DEVICE Device);

LONG
SsWriteOutbound(
    _Inout_ PSS_DEVICE Device,
    _In_ ULONG Value);

/* ssr.c */
VOID
SsSetFailure(
    _Inout_ PSS_DEVICE Device,
    _In_ ULONG_PTR Reason);

ULONG
SsDecideRestart(
    _Inout_ PSS_DEVICE Device);

DECLSPEC_NORETURN
VOID
SsFatal(
    _Inout_ PSS_DEVICE Device);

NTSTATUS
SsBroadcast(
    _In_ PSS_DEVICE Device,
    _In_ PCSTR Message);

NTSTATUS
SsPublishState(
    _Inout_ PSS_DEVICE Device,
    _In_ ULONG State);

NTSTATUS
SsSysmonOpen(
    _Inout_ PSS_DEVICE Device);

VOID
SsSysmonClose(
    _Inout_ PSS_DEVICE Device);

NTSTATUS
SsSysmonForward(
    _Inout_ PSS_DEVICE Device,
    _In_ PCSTR Message);

BOOLEAN
SsPowerVote(
    _Inout_ PSS_DEVICE Device,
    _In_ ULONG Vote);

VOID
SsCopyVersions(
    _Inout_ PSS_DEVICE Device);

/* child.c */
NTSTATUS
SsSetupChildren(
    _Inout_ PSS_DEVICE Device,
    _Inout_ PWDFDEVICE_INIT DeviceInit);

NTSTATUS
SsReadChildren(
    _Inout_ PSS_DEVICE Device);

VOID
SsAnnounceChildren(
    _Inout_ PSS_DEVICE Device);

ULONG
SsRemoveChildren(
    _Inout_ PSS_DEVICE Device);

VOID
SsReleaseWakeIrps(
    _Inout_ PSS_DEVICE Device,
    _In_ ULONG Index);

VOID
SsFreeChildren(
    _Inout_ PSS_DEVICE Device);

/* main.c */
NTSTATUS
SsEvaluateMethod(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Method,
    _Out_ PACPI_EVAL_OUTPUT_BUFFER *Output);
