/*
 * PROJECT:     ReactOS HID Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared definitions of the HID class driver
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _HIDCLASS_PCH_
#define _HIDCLASS_PCH_

#define _HIDPI_NO_FUNCTION_MACROS_
#include <ntifs.h>
/* Before hidclass.h, which turns GUID_CLASS_INPUT into a macro */
#include <poclass.h>
#include <hidpddi.h>
#include <hidport.h>
#include <wmilib.h>
#include <wmistr.h>
#include <wdmguid.h>
#include <devpropdef.h>
#include <ntstrsafe.h>

/* Interface properties hidclass.h publishes for every HID interface */
DEFINE_DEVPROPKEY(DEVPKEY_DeviceInterface_HID_UsagePage,
                  0xcbf38310, 0x4a17, 0x4310, 0xa1, 0xeb, 0x24, 0x7f, 0x0b, 0x67, 0x59, 0x3b, 2);
DEFINE_DEVPROPKEY(DEVPKEY_DeviceInterface_HID_UsageId,
                  0xcbf38310, 0x4a17, 0x4310, 0xa1, 0xeb, 0x24, 0x7f, 0x0b, 0x67, 0x59, 0x3b, 3);
DEFINE_DEVPROPKEY(DEVPKEY_DeviceInterface_HID_IsReadOnly,
                  0xcbf38310, 0x4a17, 0x4310, 0xa1, 0xeb, 0x24, 0x7f, 0x0b, 0x67, 0x59, 0x3b, 4);
DEFINE_DEVPROPKEY(DEVPKEY_DeviceInterface_HID_VendorId,
                  0xcbf38310, 0x4a17, 0x4310, 0xa1, 0xeb, 0x24, 0x7f, 0x0b, 0x67, 0x59, 0x3b, 5);
DEFINE_DEVPROPKEY(DEVPKEY_DeviceInterface_HID_ProductId,
                  0xcbf38310, 0x4a17, 0x4310, 0xa1, 0xeb, 0x24, 0x7f, 0x0b, 0x67, 0x59, 0x3b, 6);
DEFINE_DEVPROPKEY(DEVPKEY_DeviceInterface_HID_VersionNumber,
                  0xcbf38310, 0x4a17, 0x4310, 0xa1, 0xeb, 0x24, 0x7f, 0x0b, 0x67, 0x59, 0x3b, 7);

#define HIDCLASS_TAG                    'CdiH'
#define HIDCLASS_EXTENSION_SIGNATURE    'EdiH'

/* Public hidclass.h codes the in tree header does not carry yet */
#ifndef IOCTL_HID_GET_OUTPUT_REPORT
#define IOCTL_HID_GET_OUTPUT_REPORT     HID_OUT_CTL_CODE(105)
#endif
#ifndef IOCTL_HID_ENABLE_WAKE_ON_SX
#define IOCTL_HID_ENABLE_WAKE_ON_SX     HID_BUFFER_CTL_CODE(107)
#endif
#ifndef IOCTL_HID_SET_S0_IDLE_TIMEOUT
#define IOCTL_HID_SET_S0_IDLE_TIMEOUT   HID_BUFFER_CTL_CODE(108)
#endif
#ifndef IOCTL_HID_ENABLE_SECURE_READ
#define IOCTL_HID_ENABLE_SECURE_READ    HID_CTL_CODE(130)
#endif
#ifndef IOCTL_HID_DISABLE_SECURE_READ
#define IOCTL_HID_DISABLE_SECURE_READ   HID_CTL_CODE(131)
#endif
#ifndef IOCTL_HID_DEVICERESET_NOTIFICATION
#define IOCTL_HID_DEVICERESET_NOTIFICATION  HID_CTL_CODE(140)
#endif

/* Output report with a timeout, used when the minidriver announces support */
#define IOCTL_HIDCLASS_WRITE_REPORT_EX  HID_CTL_CODE(152)

typedef struct _HIDCLASS_XFER_PACKET_EX
{
    HID_XFER_PACKET Packet;
    ULONG Flags;
    ULONG Timeout;
} HIDCLASS_XFER_PACKET_EX, *PHIDCLASS_XFER_PACKET_EX;

#define HIDCLASS_XFER_FLAG_TIMEOUT      0x00000001

/* The one device reset request hidclass keeps outstanding */
typedef struct _HIDCLASS_RESET_REQUEST
{
    PIRP Irp;
    LONG State;
    struct _HIDCLASS_FDO_EXTENSION *Fdo;
} HIDCLASS_RESET_REQUEST, *PHIDCLASS_RESET_REQUEST;

/* Language every string request is issued in, US English */
#define HIDCLASS_STRING_LANGID          0x0409

/* Input reports a file object queues before the oldest one is dropped */
#define HIDCLASS_DEFAULT_INPUT_BUFFERS  32
#define HIDCLASS_MIN_INPUT_BUFFERS      2
#define HIDCLASS_MAX_INPUT_BUFFERS      512

/* Keyboard reports kept for a collection nobody has opened yet */
#define HIDCLASS_MAX_UNREAD_REPORTS     12

#define HIDCLASS_DEFAULT_POLL_MSEC      5
#define HIDCLASS_MAX_POLL_MSEC          10000

#define HIDCLASS_MAX_S0_IDLE_TIMEOUT    100000

/* Completions done inline on one file object before they move to a work item */
#define HIDCLASS_MAX_INLINE_COMPLETIONS 4

#define HIDCLASS_PINGPONG_COUNT         2

/* Seconds the read backoff grows to, one second per failure */
#define HIDCLASS_BACKOFF_STEP           (-10000000LL)
#define HIDCLASS_BACKOFF_LIMIT          (-50000000LL)

/* Synchronous calls to the minidriver give up after five seconds */
#define HIDCLASS_SYNC_TIMEOUT           (-50000000LL)

typedef enum _HIDCLASS_COLLECTION_KIND
{
    HidClassCollectionOther = 0,
    HidClassCollectionMouse,
    HidClassCollectionKeyboard
} HIDCLASS_COLLECTION_KIND;

typedef enum _HIDCLASS_PDO_STATE
{
    HidClassPdoUninitialized = 1,
    HidClassPdoStarting,
    HidClassPdoStarted,
    HidClassPdoStopPending,
    HidClassPdoStopped,
    HidClassPdoRemovePending
} HIDCLASS_PDO_STATE;

/* Whether requests reaching the FDO are failed, sent down or held */
typedef enum _HIDCLASS_IO_STATE
{
    HidClassIoFail = 0,
    HidClassIoPass,
    HidClassIoQueue
} HIDCLASS_IO_STATE;

typedef enum _HIDCLASS_PINGPONG_STATE
{
    HidClassPingPongIdle = 0,
    HidClassPingPongSubmitting,
    HidClassPingPongPending,
    HidClassPingPongCompletedInline
} HIDCLASS_PINGPONG_STATE;

typedef enum _HIDCLASS_READS_STATE
{
    HidClassReadsNone = 0,
    HidClassReadsStopped,
    HidClassReadsRunning
} HIDCLASS_READS_STATE;

typedef enum _HIDCLASS_IDLE_STATE
{
    HidClassIdleActive = 0,
    HidClassIdleIrpPending,
    HidClassIdlePoweringDown,
    HidClassIdlePoweredDown,
    HidClassIdlePoweringUp
} HIDCLASS_IDLE_STATE;

/* FDO idle configuration bits read from the device key */
#define HIDCLASS_IDLE_ENHANCED_PM       0x00000001
#define HIDCLASS_IDLE_USE_MONITOR       0x00000002
#define HIDCLASS_IDLE_SS_ENABLED        0x00000004
#define HIDCLASS_IDLE_SS_ON             0x00000008

typedef struct _HIDCLASS_DRIVER_EXTENSION
{
    LIST_ENTRY Link;
    PDRIVER_OBJECT DriverObject;
    UNICODE_STRING RegistryPath;
    ULONG DeviceExtensionSize;
    BOOLEAN DevicesArePolled;
    LONG ReferenceCount;
    PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
    PDRIVER_ADD_DEVICE AddDevice;
    PDRIVER_UNLOAD DriverUnload;
} HIDCLASS_DRIVER_EXTENSION, *PHIDCLASS_DRIVER_EXTENSION;

/* One input report waiting in a queue */
typedef struct _HIDCLASS_REPORT
{
    LIST_ENTRY Link;
    ULONG Length;
    UCHAR Data[ANYSIZE_ARRAY];
} HIDCLASS_REPORT, *PHIDCLASS_REPORT;

typedef struct _HIDCLASS_FDO_EXTENSION HIDCLASS_FDO_EXTENSION, *PHIDCLASS_FDO_EXTENSION;
typedef struct _HIDCLASS_PDO_EXTENSION HIDCLASS_PDO_EXTENSION, *PHIDCLASS_PDO_EXTENSION;

/* One top level collection of the device */
typedef struct _HIDCLASS_COLLECTION
{
    ULONG CollectionNumber;
    ULONG CollectionIndex;
    USAGE UsagePage;
    USAGE Usage;
    HIDCLASS_COLLECTION_KIND Kind;
    LONG PendingReadCount;

    KSPIN_LOCK FileLock;
    LIST_ENTRY FileList;
    LIST_ENTRY UnreadReports;
    ULONG UnreadReportCount;
    LONG OpenCount;
    LONG ReaderCount;

    HID_COLLECTION_INFORMATION Information;
    PHIDP_PREPARSED_DATA PreparsedData;

    UNICODE_STRING InterfaceName;
    UNICODE_STRING SysButtonInterfaceName;

    KSPIN_LOCK PowerEventLock;
    PIRP PowerEventIrp;

    KSPIN_LOCK SecureReadLock;
    ULONG SecureReadCount;

    /* Polled devices only */
    BOOLEAN PollingStarted;
    BOOLEAN PolledReportStale;
    ULONG PollIntervalMsec;
    KSPIN_LOCK PolledLock;
    LIST_ENTRY PolledReads;
    PUCHAR PolledReport;
    ULONG PolledReportLength;
    KTIMER PollTimer;
    KDPC PollDpc;
} HIDCLASS_COLLECTION, *PHIDCLASS_COLLECTION;

/* One of the reads kept outstanding on an interrupt driven device */
typedef struct _HIDCLASS_PINGPONG
{
    PHIDCLASS_FDO_EXTENSION Fdo;
    PIRP Irp;
    PUCHAR Buffer;
    PUCHAR Report;
    LONG State;
    LONG CancelCount;
    KEVENT SubmitDone;
    KEVENT ReadDone;
    LARGE_INTEGER BackoffDelay;
    KTIMER BackoffTimer;
    KDPC BackoffDpc;
} HIDCLASS_PINGPONG, *PHIDCLASS_PINGPONG;

struct _HIDCLASS_FDO_EXTENSION
{
    PDEVICE_OBJECT DeviceObject;
    PHIDCLASS_DRIVER_EXTENSION DriverExtension;
    ULONG HidId;
    UNICODE_STRING DeviceName;

    HID_DESCRIPTOR HidDescriptor;
    HID_DEVICE_ATTRIBUTES Attributes;
    PUCHAR ReportDescriptor;
    ULONG ReportDescriptorLength;
    HIDP_DEVICE_DESC DeviceDesc;
    BOOLEAN DeviceDescValid;
    BOOLEAN Initialized;
    BOOLEAN NoInputCollections;
    BOOLEAN ComboHardwareIdV2;
    PHIDCLASS_COLLECTION Collections;
    ULONG MaxReportLength;

    ULONG PingPongCount;
    PHIDCLASS_PINGPONG PingPongs;
    LONG ReadsState;

    /* Collection PDOs, indexed by collection index */
    KSPIN_LOCK ClientPdoLock;
    PDEVICE_RELATIONS ClientPdoRelations;
    PHIDCLASS_PDO_EXTENSION *ClientPdos;
    BOOLEAN RelationsReported;
    LONG IsPresent;

    DEVICE_CAPABILITIES Capabilities;
    SYSTEM_POWER_STATE SystemPowerState;
    DEVICE_POWER_STATE DevicePowerState;

    IO_REMOVE_LOCK RemoveLock;

    /* Open handles plus one reference held until remove */
    KSPIN_LOCK OpenCountLock;
    LONG OpenCount;

    /* Request flow while powered down, stopped or removed */
    KSPIN_LOCK IoLock;
    HIDCLASS_IO_STATE IoState;
    LIST_ENTRY DelayedIrps;
    LONG InFlightCount;

    /* Power */
    PIRP PendingSystemIrp;
    PIRP WaitWakeIrp;
    KSPIN_LOCK WaitWakeLock;
    LIST_ENTRY CollectionWaitWakeIrps;
    BOOLEAN WokeSystem;
    POHANDLE PoFxHandle;
    PO_FX_COMPONENT_IDLE_STATE PoFxIdleState;
    BOOLEAN PolicyReferenceHeld;
    LONG IdleState;
    ULONG IdleFlags;
    ULONG SelectiveSuspendTimeout;
    ULONG NoHandleTimeout;
    ULONG NoHandleInitialTimeout;
    ULONG ClientIdleTimeout;
    BOOLEAN PolicyStarted;
    BOOLEAN PowerHeldIo;
    BOOLEAN DrainWaiting;
    LONG NotRequiredPending;
    KEVENT IoDrained;
    PIRP IdleIrp;
    HID_SUBMIT_IDLE_NOTIFICATION_CALLBACK_INFO IdleCallbackInfo;
    KEVENT IdleIrpDone;
    KEVENT PowerDone;
    NTSTATUS PowerStatus;

    WMILIB_CONTEXT WmiLibInfo;
    BOOLEAN WmiRegistered;

    /* Minidriver features announced in the device key */
    BOOLEAN DeviceResetSupported;
    BOOLEAN WriteReportExSupported;

    /* Device reset notifications from clients, one request below at a time */
    KSPIN_LOCK ResetLock;
    LIST_ENTRY ResetIrps;
    BOOLEAN ResetDrained;
    struct _HIDCLASS_RESET_REQUEST *ResetRequest;
};

struct _HIDCLASS_PDO_EXTENSION
{
    HIDCLASS_PDO_STATE PreviousState;
    HIDCLASS_PDO_STATE State;
    ULONG CollectionNumber;
    ULONG CollectionIndex;
    PDEVICE_OBJECT DeviceObject;
    PDEVICE_OBJECT FdoDeviceObject;
    PHIDCLASS_FDO_EXTENSION Fdo;
    IO_REMOVE_LOCK RemoveLock;

    SYSTEM_POWER_STATE SystemPowerState;
    DEVICE_POWER_STATE DevicePowerState;
    PIRP WaitWakeIrp;

    KSPIN_LOCK RemoteWakeLock;
    BOOLEAN RemoteWakeEnabled;
    LONG RemoteWakeIrpPending;
    PIRP RemoteWakeIrp;

    PHID_STATUS_CHANGE StatusChange;
    PVOID StatusChangeContext;

    /* Share access accounting for Create */
    LONG OpenCount;
    LONG ReaderCount;
    LONG WriterCount;
    LONG DenyReadCount;
    LONG DenyWriteCount;
    LONG ExclusiveCount;

    BOOLEAN IsKeyboardOrMouse;
    BOOLEAN SessionSecurity;
    BOOLEAN WakeOnSx;
    ULONG S0IdleTimeout;

    WMILIB_CONTEXT WmiLibInfo;
    BOOLEAN WmiRegistered;
};

/* What a minidriver finds at DeviceObject->DeviceExtension */
typedef struct _HIDCLASS_DEVICE_EXTENSION
{
    HID_DEVICE_EXTENSION Hid;
    BOOLEAN IsClientPdo;
    ULONG Signature;
    union
    {
        HIDCLASS_FDO_EXTENSION Fdo;
        HIDCLASS_PDO_EXTENSION Pdo;
    };
} HIDCLASS_DEVICE_EXTENSION, *PHIDCLASS_DEVICE_EXTENSION;

#define HIDCLASS_EXTENSION_SIZE \
    ALIGN_UP_BY(sizeof(HIDCLASS_DEVICE_EXTENSION), MEMORY_ALLOCATION_ALIGNMENT)

/* Per open handle state, kept in FileObject->FsContext */
typedef struct _HIDCLASS_FILE_EXTENSION
{
    ULONG CollectionNumber;
    PHIDCLASS_FDO_EXTENSION Fdo;
    PHIDCLASS_PDO_EXTENSION Pdo;
    PFILE_OBJECT FileObject;
    LIST_ENTRY CollectionLink;

    KSPIN_LOCK Lock;
    LIST_ENTRY PendingReads;
    LIST_ENTRY Reports;
    ULONG ReportCount;
    ULONG MaxReportCount;
    LONG CompletionDepth;

    ACCESS_MASK DesiredAccess;
    USHORT ShareAccess;
    KPROCESSOR_MODE RequestorMode;
    ULONG SessionId;
    BOOLEAN HasTcbPrivilege;
    BOOLEAN OpportunisticPolling;
    BOOLEAN HasRead;
    ULONG SecureReadCount;
} HIDCLASS_FILE_EXTENSION, *PHIDCLASS_FILE_EXTENSION;

FORCEINLINE
PHIDCLASS_FDO_EXTENSION
NTAPI
HidClassGetFdo(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension)
{
    if (DeviceExtension->IsClientPdo)
        return DeviceExtension->Pdo.Fdo;

    return &DeviceExtension->Fdo;
}

FORCEINLINE
PHIDCLASS_DEVICE_EXTENSION
NTAPI
HidClassExtensionFromFdo(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    return CONTAINING_RECORD(Fdo, HIDCLASS_DEVICE_EXTENSION, Fdo);
}

FORCEINLINE
PHIDCLASS_DEVICE_EXTENSION
NTAPI
HidClassExtensionFromPdo(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo)
{
    return CONTAINING_RECORD(Pdo, HIDCLASS_DEVICE_EXTENSION, Pdo);
}

FORCEINLINE
PVOID
NTAPI
HidClassMapIrpBuffer(
    _In_ PIRP Irp)
{
    if (Irp->MdlAddress == NULL)
        return NULL;

    return MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);
}

/* hidclass.c */
PHIDCLASS_DRIVER_EXTENSION
NTAPI
HidClassReferenceDriver(
    _In_ PDRIVER_OBJECT DriverObject);

VOID
NTAPI
HidClassDereferenceDriver(
    _In_ PDRIVER_OBJECT DriverObject);

NTSTATUS
NTAPI
HidClassCallMinidriver(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp);

NTSTATUS
NTAPI
HidClassCallMinidriverSynchronous(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp);

NTSTATUS
NTAPI
HidClassForwardToMinidriver(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp);

NTSTATUS
NTAPI
HidClassPassOrComplete(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp);

NTSTATUS
NTAPI
HidClassSetDeviceRegistryValue(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PCWSTR ValueName,
    _In_ ULONG Value);

NTSTATUS
NTAPI
HidClassQueryDeviceRegistryValue(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PCWSTR ValueName,
    _Out_ PULONG Value);

/* fdo.c */
NTSTATUS
NTAPI
HidClassFdoPnp(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp);

PHIDCLASS_COLLECTION
NTAPI
HidClassFindCollection(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ ULONG CollectionNumber);

PHIDP_COLLECTION_DESC
NTAPI
HidClassFindCollectionDesc(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ ULONG CollectionNumber);

PHIDP_REPORT_IDS
NTAPI
HidClassFindReportId(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ UCHAR ReportId);

BOOLEAN
NTAPI
HidClassDeviceUsesReportIds(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo);

NTSTATUS
NTAPI
HidClassCopyCollectionInformation(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ ULONG CollectionNumber,
    _Out_writes_bytes_(*Length) PVOID Buffer,
    _Inout_ PULONG Length);

NTSTATUS
NTAPI
HidClassCopyCollectionDescriptor(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ ULONG CollectionNumber,
    _Out_writes_bytes_(*Length) PVOID Buffer,
    _Inout_ PULONG Length,
    _In_ BOOLEAN UserBuffer);

VOID
NTAPI
HidClassFreeUnreadReports(
    _In_ PHIDCLASS_COLLECTION Collection);

LONG
NTAPI
HidClassUpdateOpenCount(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_opt_ PHIDCLASS_COLLECTION Collection,
    _In_opt_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ BOOLEAN Open);

VOID
NTAPI
HidClassCleanUpFdo(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo);

/* pdo.c */
NTSTATUS
NTAPI
HidClassPdoPnp(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ PIRP Irp);

NTSTATUS
NTAPI
HidClassCreateCollectionPdos(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo);

/* read.c */
NTSTATUS
NTAPI
HidClassAllocatePingPongs(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo);

VOID
NTAPI
HidClassFreePingPongs(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo);

NTSTATUS
NTAPI
HidClassStartReads(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo);

VOID
NTAPI
HidClassStopReads(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo);

NTSTATUS
NTAPI
HidClassRead(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ PIRP Irp);

PIRP
NTAPI
HidClassDequeuePendingRead(
    _In_ PHIDCLASS_COLLECTION Collection,
    _In_ PHIDCLASS_FILE_EXTENSION FileExtension);

VOID
NTAPI
HidClassCompletePendingReads(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo);

VOID
NTAPI
HidClassFlushReports(
    _In_ PHIDCLASS_FILE_EXTENSION FileExtension);

VOID
NTAPI
HidClassSignalPowerEvent(
    _In_ PHIDCLASS_COLLECTION Collection,
    _In_reads_bytes_(Length) PUCHAR Report,
    _In_ USHORT Length);

VOID
NTAPI
HidClassStartPolling(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_COLLECTION Collection);

VOID
NTAPI
HidClassRestartPolling(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_COLLECTION Collection);

VOID
NTAPI
HidClassStopPolling(
    _In_ PHIDCLASS_COLLECTION Collection,
    _In_ BOOLEAN CompleteReads);

BOOLEAN
NTAPI
HidClassReadPolledDevice(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ BOOLEAN TimerDriven);

/* file.c */
NTSTATUS
NTAPI
HidClassCreate(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp);

NTSTATUS
NTAPI
HidClassClose(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp);

NTSTATUS
NTAPI
HidClassWrite(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp);

NTSTATUS
NTAPI
HidClassSendWriteReport(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ PIRP Irp,
    _In_reads_bytes_opt_(Length) PUCHAR Buffer,
    _In_ ULONG Length,
    _Out_ PBOOLEAN Sent);

/* ioctl.c */
VOID
NTAPI
HidClassCancelResetNotifications(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_PDO_EXTENSION Pdo);

VOID
NTAPI
HidClassDrainResetNotifications(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo);

NTSTATUS
NTAPI
HidClassDeviceControl(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp);

NTSTATUS
NTAPI
HidClassInternalDeviceControl(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp);

/* power.c */
NTSTATUS
NTAPI
HidClassFdoPower(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp);

NTSTATUS
NTAPI
HidClassPdoPower(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ PIRP Irp);

NTSTATUS
NTAPI
HidClassBeginIo(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp,
    _Out_ PBOOLEAN Counted);

VOID
NTAPI
HidClassEndIo(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo);

VOID
NTAPI
HidClassSetIoState(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ HIDCLASS_IO_STATE IoState);

VOID
NTAPI
HidClassCompleteDelayedIrpsForPdo(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ NTSTATUS Status);

VOID
NTAPI
HidClassCompleteCollectionWaitWakes(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ NTSTATUS Status);

VOID
NTAPI
HidClassResetIdleTimer(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo);

VOID
NTAPI
HidClassConfigureIdle(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo);

VOID
NTAPI
HidClassUpdateIdlePolicy(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo);

VOID
NTAPI
HidClassUnregisterPoFx(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo);

NTSTATUS
NTAPI
HidClassToggleSelectiveSuspend(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ BOOLEAN Enable);

NTSTATUS
NTAPI
HidClassToggleRemoteWake(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ BOOLEAN Enable);

VOID
NTAPI
HidClassRequestRemoteWake(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo);

VOID
NTAPI
HidClassCancelRemoteWake(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo);

BOOLEAN
NTAPI
HidClassPdoCanWake(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo);

/* wmi.c */
NTSTATUS
NTAPI
HidClassSystemControl(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp);

VOID
NTAPI
HidClassInitializeFdoWmi(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo);

VOID
NTAPI
HidClassInitializePdoWmi(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo);

#endif /* _HIDCLASS_PCH_ */
