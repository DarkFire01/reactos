/*
 * PROJECT:     ReactOS Keyboard Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Main header file
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _KBDCLASS_H_
#define _KBDCLASS_H_

#include <ntifs.h>
#include <ntpoapi.h>
#include <ntddkbd.h>
#include <kbdmou.h>
#include <wmilib.h>
#include <wmistr.h>
#include <wdmguid.h>
#include <ntstrsafe.h>

#define KBDCLASS_TAG 'CdbK'

/* Registry defaults used when the Parameters key does not override them */
#define KBDCLASS_DEFAULT_QUEUE_ENTRIES      100
#define KBDCLASS_DEFAULT_PORTS_SERVICED     1
#define KBDCLASS_DEFAULT_CONNECT_MULTIPLE   1
#define KBDCLASS_DEFAULT_BASE_NAME          L"KeyboardClass"
#define KBDCLASS_BASE_NAME_CHARS            256

/* Event log message codes understood by the system message table */
#define KBDCLASS_LOG_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC0050001L)
#define KBDCLASS_LOG_NO_QUEUE_BUFFER        ((NTSTATUS)0xC0050002L)
#define KBDCLASS_LOG_NO_DEVICEMAP_ENTRY     ((NTSTATUS)0x80050005L)
#define KBDCLASS_LOG_PORT_OPEN_FAILED       ((NTSTATUS)0xC0050009L)
#define KBDCLASS_LOG_PORT_CLOSE_FAILED      ((NTSTATUS)0xC005000AL)
#define KBDCLASS_LOG_DEVICE_CREATE_FAILED   ((NTSTATUS)0xC005000DL)
#define KBDCLASS_LOG_WAIT_WAKE_RESOURCES    ((NTSTATUS)0x8005000EL)
#define KBDCLASS_LOG_ATTACH_FAILED          ((NTSTATUS)0xC005000FL)

/* Unique error values written with the log entries */
#define KBDCLASS_ERRID_REGISTRY_PATH        (KEYBOARD_ERROR_VALUE_BASE + 2)
#define KBDCLASS_ERRID_CREATE_DEVICE        (KEYBOARD_ERROR_VALUE_BASE + 6)
#define KBDCLASS_ERRID_LEGACY_PORT          (KEYBOARD_ERROR_VALUE_BASE + 8)
#define KBDCLASS_ERRID_DEVICEMAP            (KEYBOARD_ERROR_VALUE_BASE + 14)
#define KBDCLASS_ERRID_QUEUE_BUFFER         (KEYBOARD_ERROR_VALUE_BASE + 20)
#define KBDCLASS_ERRID_PORT_OPEN_CLOSE      (KEYBOARD_ERROR_VALUE_BASE + 120)

#ifndef IOCTL_KEYBOARD_QUERY_EXTENDED_ATTRIBUTES
#define IOCTL_KEYBOARD_QUERY_EXTENDED_ATTRIBUTES \
    CTL_CODE(FILE_DEVICE_KEYBOARD, 0x0080, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

/* Control codes the class hands to the port untouched */
#define KBDCLASS_PORT_IOCTL(Function, Method) \
    CTL_CODE(FILE_DEVICE_KEYBOARD, (Function), (Method), FILE_ANY_ACCESS)
#define KBDCLASS_BATTERY_IOCTL(Function) \
    CTL_CODE(FILE_DEVICE_BATTERY, (Function), METHOD_BUFFERED, FILE_READ_ACCESS)

/* Lets a checked open ask for read access, cleared before the port sees the IRP */
#define KBDCLASS_CREATE_READ_OPTION         FILE_DIRECTORY_FILE

/* Wait wake cancellation handshake with IRP_MN_REMOVE_DEVICE */
#define KBDCLASS_WW_ARMED                   0
#define KBDCLASS_WW_REMOVE_CANCELLING       1
#define KBDCLASS_WW_REMOVE_DONE             2
#define KBDCLASS_WW_COMPLETED               3

/* Ownership handshake for the LED IRP sent after power up */
#define KBDCLASS_LEDS_IDLE                  0
#define KBDCLASS_LEDS_SENT                  1
#define KBDCLASS_LEDS_CANCELLED             2
#define KBDCLASS_LEDS_COMPLETED             3

typedef struct _KBDCLASS_DEVICE_EXTENSION *PKBDCLASS_DEVICE_EXTENSION;

/** @brief One port connected to the shared class device. */
typedef struct _KBDCLASS_PORT_ENTRY
{
    PFILE_OBJECT File;
    PKBDCLASS_DEVICE_EXTENSION Port;
    BOOLEAN Enabled;
    BOOLEAN Free;
} KBDCLASS_PORT_ENTRY, *PKBDCLASS_PORT_ENTRY;

/** @brief Copy of the port list used to send one IOCTL to every port. */
typedef struct _KBDCLASS_PORT_SNAPSHOT
{
    ULONG Count;
    ULONG Next;
    KBDCLASS_PORT_ENTRY Ports[ANYSIZE_ARRAY];
} KBDCLASS_PORT_SNAPSHOT, *PKBDCLASS_PORT_SNAPSHOT;

typedef struct _KBDCLASS_DEVICE_EXTENSION
{
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT ClassDevice;
    PDEVICE_OBJECT TopPort;
    PDEVICE_OBJECT Pdo;
    IO_REMOVE_LOCK RemoveLock;

    BOOLEAN PnP;
    BOOLEAN Started;
    BOOLEAN AllowDisable;
    BOOLEAN SurpriseRemoved;
    BOOLEAN Removed;
    BOOLEAN LegacyEnabled;
    BOOLEAN OkayToLogOverflow;
    BOOLEAN InPortList;

    ULONG TrustedOpens;
    UNICODE_STRING InterfaceName;

    /* Ring buffer of KEYBOARD_INPUT_DATA, guarded by QueueLock */
    KSPIN_LOCK QueueLock;
    PUCHAR QueueBase;
    PUCHAR QueueIn;
    PUCHAR QueueOut;
    ULONG QueueLength;
    ULONG InputCount;
    LIST_ENTRY ReadQueue;

    KEYBOARD_INDICATOR_PARAMETERS Indicators;
    ULONG UnitId;
    PFILE_OBJECT LegacyFile;
    LIST_ENTRY LegacyLink;
    PVOID TargetNotifyHandle;

    /* Power */
    DEVICE_POWER_STATE DeviceState;
    SYSTEM_POWER_STATE SystemState;
    DEVICE_POWER_STATE SystemToDevice[PowerSystemHibernate];
    DEVICE_POWER_STATE MinDeviceWake;
    SYSTEM_POWER_STATE MinSystemWake;

    /* Wait wake, guarded by WaitWakeLock */
    KSPIN_LOCK WaitWakeLock;
    PIRP WaitWakeIrp;
    PIRP ExtraWaitWakeIrp;
    BOOLEAN WaitWakeCancelled;
    BOOLEAN WaitWakeEnabled;
    LONG WaitWakeRemoveState;

    PIRP LedIrp;
    LONG LedState;

    WMILIB_CONTEXT WmiLibInfo;
} KBDCLASS_DEVICE_EXTENSION;

/** @brief Context handed to the work items queued by the driver. */
typedef struct _KBDCLASS_WORK_CONTEXT
{
    PIO_WORKITEM WorkItem;
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension;
    PIRP Irp;
    BOOLEAN Enable;
} KBDCLASS_WORK_CONTEXT, *PKBDCLASS_WORK_CONTEXT;

typedef struct _KBDCLASS_GLOBALS
{
    /* Port list and grand master state, guarded by Mutex */
    FAST_MUTEX Mutex;
    PKBDCLASS_DEVICE_EXTENSION GrandMaster;
    PKBDCLASS_PORT_ENTRY PortList;
    ULONG PortCount;
    LONG GrandMasterOpens;

    LIST_ENTRY LegacyDevices;
    ULONG LegacyPortsFound;
    ULONG NextDeviceNumber;

    UNICODE_STRING RegistryPath;
    UNICODE_STRING BaseClassName;
    WCHAR BaseClassNameBuffer[KBDCLASS_BASE_NAME_CHARS];

    ULONG ConnectOneClassToOnePort;
    ULONG SendOutputToAllPorts;
    ULONG PortsServiced;
    ULONG QueueBytes;
} KBDCLASS_GLOBALS;

extern KBDCLASS_GLOBALS KbdClassGlobals;
extern const UCHAR KbdClassTrustedOpen;

#define KBDCLASS_TRUSTED_CONTEXT ((PVOID)&KbdClassTrustedOpen)

/* kbdclass.c */

VOID
NTAPI
KbdClassLogError(
    _In_ PVOID IoObject,
    _In_ NTSTATUS ErrorCode,
    _In_ ULONG UniqueErrorValue,
    _In_ NTSTATUS FinalStatus,
    _In_ ULONG DumpCount,
    _In_reads_opt_(DumpCount) PULONG DumpData,
    _In_ UCHAR MajorFunction);

NTSTATUS
NTAPI
KbdClassSendIrpSynchronously(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ BOOLEAN CopyStack);

NTSTATUS
NTAPI
KbdClassQueryRegistryUlong(
    _In_ HANDLE KeyHandle,
    _In_ PCWSTR ValueName,
    _Out_ PULONG Value);

/* io.c */

DRIVER_DISPATCH KbdClassCreate;
DRIVER_DISPATCH KbdClassClose;
DRIVER_DISPATCH KbdClassCleanup;
DRIVER_DISPATCH KbdClassRead;
DRIVER_DISPATCH KbdClassFlush;
DRIVER_DISPATCH KbdClassDeviceControl;
DRIVER_DISPATCH KbdClassPassThrough;

VOID
NTAPI
KbdClassServiceCallback(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PKEYBOARD_INPUT_DATA InputDataStart,
    _In_ PKEYBOARD_INPUT_DATA InputDataEnd,
    _Inout_ PULONG InputDataConsumed);

VOID
NTAPI
KbdClassResetQueue(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension);

VOID
NTAPI
KbdClassFlushReads(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_opt_ PFILE_OBJECT FileObject);

NTSTATUS
NTAPI
KbdClassEnableDisablePort(
    _In_ BOOLEAN Enable,
    _In_opt_ PIRP Irp,
    _In_ PKBDCLASS_DEVICE_EXTENSION PortExtension,
    _Inout_ PFILE_OBJECT *File);

/* pnp.c */

DRIVER_DISPATCH KbdClassPnp;
DRIVER_NOTIFICATION_CALLBACK_ROUTINE KbdClassPlugPlayNotification;

BOOLEAN
NTAPI
KbdClassCancelPendingLeds(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension);

/* power.c */

DRIVER_DISPATCH KbdClassPower;
IO_COMPLETION_ROUTINE KbdClassSetLedsComplete;

BOOLEAN
NTAPI
KbdClassWakeCapable(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension);

BOOLEAN
NTAPI
KbdClassWaitWakePending(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension);

BOOLEAN
NTAPI
KbdClassCheckWaitWakeEnabled(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension);

BOOLEAN
NTAPI
KbdClassCreateWaitWakeIrp(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension);

VOID
NTAPI
KbdClassReadWaitWakeState(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension);

NTSTATUS
NTAPI
KbdClassToggleWaitWake(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ BOOLEAN Enable);

/* wmi.c */

DRIVER_DISPATCH KbdClassSystemControl;

VOID
NTAPI
KbdClassSetupWmi(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ BOOLEAN WakeCapable);

#endif /* _KBDCLASS_H_ */
