/*
 * PROJECT:     ReactOS Mouse Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared definitions for the mouse class driver
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntifs.h>
#include <ntpoapi.h>
#include <ntddmou.h>
#include <kbdmou.h>
#include <wmilib.h>
#include <wmistr.h>
#include <wdmguid.h>

#define MOUCLASS_TAG                    'CuoM'
#define MOUCLASS_PACKET_SIZE            ((ULONG)sizeof(MOUSE_INPUT_DATA))

/* Registry defaults */
#define MOUCLASS_DEFAULT_QUEUE_ENTRIES  100
#define MOUCLASS_DEFAULT_PORTS_SERVICED 1
#define MOUCLASS_BASE_NAME_CHARS        256

/* Event log codes, facility 5 */
#define MOUCLASS_EVT_NO_RESOURCES       ((NTSTATUS)0xC0050001L)
#define MOUCLASS_EVT_NO_QUEUE_BUFFER    ((NTSTATUS)0xC0050002L)
#define MOUCLASS_EVT_PORT_OPEN_FAILED   ((NTSTATUS)0xC0050009L)
#define MOUCLASS_EVT_PORT_CLOSE_FAILED  ((NTSTATUS)0xC005000AL)
#define MOUCLASS_EVT_NO_DEVICE_OBJECT   ((NTSTATUS)0xC005000DL)
#define MOUCLASS_EVT_ATTACH_FAILED      ((NTSTATUS)0xC005000FL)
#define MOUCLASS_EVT_DEVICEMAP_FAILED   ((NTSTATUS)0x80050005L)
#define MOUCLASS_EVT_WAIT_WAKE_FAILED   ((NTSTATUS)0x8005000EL)

/* WMI data block indexes */
#define MOUCLASS_WMI_CLASS_INFO         0
#define MOUCLASS_WMI_WAKE_ENABLE        1

/* Wait wake versus removal handshake values */
#define MOUCLASS_WW_IDLE                0
#define MOUCLASS_WW_REMOVING            1
#define MOUCLASS_WW_REMOVED             2
#define MOUCLASS_WW_COMPLETED           3

/* HID request that the HID class uses to suppress input, forwarded as is */
#define MOUCLASS_HID_SUPPRESS_INPUT     HID_BUFFER_CTL_CODE(154)

typedef struct _MOUCLASS_EXTENSION *PMOUCLASS_EXTENSION;

/* One port served by the shared class device */
typedef struct _MOUCLASS_PORT
{
    PFILE_OBJECT FileObject;
    PMOUCLASS_EXTENSION Port;
    BOOLEAN Enabled;
    BOOLEAN Free;
} MOUCLASS_PORT, *PMOUCLASS_PORT;

typedef struct _MOUCLASS_EXTENSION
{
    PDEVICE_OBJECT Self;

    /* Device that clients open and that the port reports to */
    PDEVICE_OBJECT ClassDevice;

    PDEVICE_OBJECT LowerDevice;
    PDEVICE_OBJECT Pdo;
    IO_REMOVE_LOCK RemoveLock;

    BOOLEAN IsPnp;
    BOOLEAN IsStarted;
    BOOLEAN IsSurpriseRemoved;
    BOOLEAN IsWaitWakeEnabled;
    BOOLEAN IsWaitWakeCancelling;

    UNICODE_STRING InterfaceName;
    ULONG UnitId;

    /* Ring of MOUSE_INPUT_DATA, guarded by QueueLock */
    KSPIN_LOCK QueueLock;
    LIST_ENTRY ReadQueue;
    PMOUSE_INPUT_DATA QueueStart;
    PMOUSE_INPUT_DATA QueueIn;
    PMOUSE_INPUT_DATA QueueOut;
    ULONG QueueBytes;
    ULONG QueueCount;

    /* Power */
    DEVICE_POWER_STATE DeviceState;
    SYSTEM_POWER_STATE SystemState;
    DEVICE_POWER_STATE SystemToDevice[PowerSystemHibernate];
    DEVICE_POWER_STATE WakeDeviceState;
    SYSTEM_POWER_STATE WakeSystemState;
    KSPIN_LOCK WaitWakeLock;
    PIRP WaitWakeIrp;
    PIRP ExtraWaitWakeIrp;
    LONG WaitWakeRemoval;

    WMILIB_CONTEXT WmiLib;

    /* Shared mode only, PnP notification on the port stack */
    PVOID TargetNotifyHandle;

    /* Legacy ports found through DEVICEMAP */
    LIST_ENTRY LegacyLink;
    PFILE_OBJECT LegacyFile;
} MOUCLASS_EXTENSION;

typedef struct _MOUCLASS_GLOBALS
{
    /* Shared class device when ConnectMultiplePorts is set */
    PMOUCLASS_EXTENSION SharedClass;
    PMOUCLASS_PORT Ports;
    ULONG PortCount;
    LONG SharedOpens;
    FAST_MUTEX Lock;

    BOOLEAN IsSharedMode;
    ULONG PortsServiced;
    ULONG QueueBytes;
    ULONG NextDeviceNumber;

    ULONG LegacyPortCount;
    LIST_ENTRY LegacyList;

    UNICODE_STRING RegistryPath;
    UNICODE_STRING BaseName;
    WCHAR BaseNameBuffer[MOUCLASS_BASE_NAME_CHARS];
} MOUCLASS_GLOBALS;

/* Work item contexts */
typedef struct _MOUCLASS_START_CONTEXT
{
    PIO_WORKITEM WorkItem;
    PIRP Irp;
} MOUCLASS_START_CONTEXT, *PMOUCLASS_START_CONTEXT;

typedef struct _MOUCLASS_WORK_CONTEXT
{
    PIO_WORKITEM WorkItem;
    PMOUCLASS_EXTENSION Extension;
    BOOLEAN Enable;
} MOUCLASS_WORK_CONTEXT, *PMOUCLASS_WORK_CONTEXT;

extern MOUCLASS_GLOBALS MouGlobals;
extern WMIGUIDREGINFO MouWmiGuidList[2];

/* Marks a file object opened by a trusted (TCB) caller */
#define MOUCLASS_TRUSTED_FILE           ((PVOID)&MouGlobals)

/* mouclass.c */

DRIVER_INITIALIZE DriverEntry;
DRIVER_ADD_DEVICE MouseAddDevice;

VOID
NTAPI
MouseLogError(
    _In_ PVOID IoObject,
    _In_ NTSTATUS ErrorCode,
    _In_ ULONG UniqueId,
    _In_ NTSTATUS FinalStatus,
    _In_ ULONG DumpCount,
    _In_reads_opt_(DumpCount) PULONG DumpData,
    _In_ UCHAR MajorFunction);

/* dispatch.c */

DRIVER_DISPATCH MouseCreate;
DRIVER_DISPATCH MouseCleanup;
DRIVER_DISPATCH MouseClose;
DRIVER_DISPATCH MouseRead;
DRIVER_DISPATCH MouseFlush;
DRIVER_DISPATCH MouseDeviceControl;
DRIVER_DISPATCH MousePassThrough;

VOID
NTAPI
MouseServiceCallback(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PMOUSE_INPUT_DATA InputDataStart,
    _In_ PMOUSE_INPUT_DATA InputDataEnd,
    _Inout_ PULONG InputDataConsumed);

VOID
NTAPI
MouseResetQueue(
    _In_ PMOUCLASS_EXTENSION Extension);

VOID
NTAPI
MouseCancelQueuedReads(
    _In_ PMOUCLASS_EXTENSION Extension,
    _In_opt_ PFILE_OBJECT FileObject);

NTSTATUS
NTAPI
MouseSwitchPort(
    _In_ BOOLEAN Enable,
    _In_opt_ PIRP Irp,
    _In_ PMOUCLASS_EXTENSION Port,
    _Inout_ PFILE_OBJECT *FileObject);

NTSTATUS
NTAPI
MouseForwardAndWait(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ BOOLEAN CopyStack);

/* pnp.c */

DRIVER_DISPATCH MousePnp;

/* power.c */

DRIVER_DISPATCH MousePower;
DRIVER_DISPATCH MouseSystemControl;

BOOLEAN
NTAPI
MouseCanWake(
    _In_ PMOUCLASS_EXTENSION Extension);

BOOLEAN
NTAPI
MouseIsWaitWakeArmed(
    _In_ PMOUCLASS_EXTENSION Extension);

BOOLEAN
NTAPI
MouseIsWaitWakeEnabled(
    _In_ PMOUCLASS_EXTENSION Extension);

VOID
NTAPI
MouseRequestWaitWake(
    _In_ PMOUCLASS_EXTENSION Extension);

VOID
NTAPI
MouseReadWaitWakeSetting(
    _In_ PMOUCLASS_EXTENSION Extension);

VOID
NTAPI
MouseCancelWaitWakeForRemoval(
    _In_ PMOUCLASS_EXTENSION Extension);

VOID
NTAPI
MouseInitializeWmi(
    _In_ PMOUCLASS_EXTENSION Extension);
