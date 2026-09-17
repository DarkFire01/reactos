#ifndef _HIDCLASS_PCH_
#define _HIDCLASS_PCH_

#define _HIDPI_NO_FUNCTION_MACROS_
#include <wdm.h>
#include <hidpddi.h>
#include <stdio.h>
#include <hidport.h>

#define HIDCLASS_TAG 'CdiH'

/* Reads the class driver keeps outstanding so reports are never missed */
#define HIDCLASS_PING_PONG_COUNT 2

/* Reports held for a file object that is not reading right now */
#define HIDCLASS_RING_REPORT_COUNT 32

/* How long the read loop stands off a device that keeps failing */
#define HIDCLASS_READ_BACKOFF_MS 1000

/*
 * Reports that arrived for a file object before anything asked for them. The
 * oldest report is dropped once the ring is full, which is what a HID client
 * expects: stale input is worth less than the report that just came in.
 */
typedef struct _HIDCLASS_REPORT_RING
{
    PUCHAR Reports;
    ULONG ReportSize;

    /* One more slot than the ring holds reports, so full and empty differ */
    ULONG SlotCount;
    ULONG Head;
    ULONG Tail;
} HIDCLASS_REPORT_RING, *PHIDCLASS_REPORT_RING;

/* One of the reads the class driver keeps running on the minidriver */
typedef struct _HIDCLASS_PING_PONG
{
    PIRP Irp;
    PVOID Report;
    PVOID FDODeviceExtension;
    KTIMER BackoffTimer;
    KDPC BackoffDpc;
} HIDCLASS_PING_PONG, *PHIDCLASS_PING_PONG;

typedef struct
{
    PDRIVER_OBJECT DriverObject;
    ULONG DeviceExtensionSize;
    BOOLEAN DevicesArePolled;
    PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
    PDRIVER_ADD_DEVICE AddDevice;
    PDRIVER_UNLOAD DriverUnload;
    KSPIN_LOCK Lock;

} HIDCLASS_DRIVER_EXTENSION, *PHIDCLASS_DRIVER_EXTENSION;

typedef struct
{
    //
    // hid device extension
    //
    HID_DEVICE_EXTENSION HidDeviceExtension;

    //
    // if it is a pdo
    //
    BOOLEAN IsFDO;

    //
    // driver extension
    //
    PHIDCLASS_DRIVER_EXTENSION DriverExtension;

    //
    // device description
    //
    HIDP_DEVICE_DESC DeviceDescription;

    //
    // hid attributes
    //
    HID_DEVICE_ATTRIBUTES Attributes;

} HIDCLASS_COMMON_DEVICE_EXTENSION, *PHIDCLASS_COMMON_DEVICE_EXTENSION;

typedef struct
{
    //
    // parts shared by fdo and pdo
    //
    HIDCLASS_COMMON_DEVICE_EXTENSION Common;

    //
    // the device object this extension belongs to, which is what the
    // minidriver is called on
    //
    PDEVICE_OBJECT SelfDeviceObject;

    //
    // device capabilities
    //
    DEVICE_CAPABILITIES Capabilities;

    //
    // hid descriptor
    //
    HID_DESCRIPTOR HidDescriptor;

    //
    // report descriptor
    //
    PUCHAR ReportDescriptor;

    //
    // device relations
    //
    PDEVICE_RELATIONS DeviceRelations;

    //
    // reads kept outstanding on the minidriver
    //
    PHIDCLASS_PING_PONG PingPong;
    ULONG PingPongCount;

    //
    // largest input report any collection of this device can produce, the
    // leading report id included
    //
    ULONG MaxReportSize;

    //
    // whether the device numbers its reports
    //
    BOOLEAN UsesReportId;

    //
    // guards the read loop state below
    //
    KSPIN_LOCK ReadLock;

    //
    // reads are re-issued while this is set
    //
    BOOLEAN ReadsRunning;

    //
    // reads still to come back after the loop was stopped
    //
    ULONG ReadsOutstanding;

    //
    // signalled once the last outstanding read has come back
    //
    KEVENT ReadsDrained;

} HIDCLASS_FDO_EXTENSION, *PHIDCLASS_FDO_EXTENSION;

typedef struct
{
    //
    // parts shared by fdo and pdo
    //
    HIDCLASS_COMMON_DEVICE_EXTENSION Common;

    //
    // device capabilities
    //
    DEVICE_CAPABILITIES Capabilities;

    //
    // collection index
    //
    ULONG CollectionNumber;

    //
    // device interface
    //
    UNICODE_STRING DeviceInterface;

    //
    // FDO device object
    //
    PDEVICE_OBJECT FDODeviceObject;

    //
    // fdo device extension
    //
    PHIDCLASS_FDO_EXTENSION FDODeviceExtension;

    //
    // file objects open on this collection, and the lock over the list
    //
    LIST_ENTRY FileOpListHead;
    KSPIN_LOCK FileOpLock;

} HIDCLASS_PDO_DEVICE_EXTENSION, *PHIDCLASS_PDO_DEVICE_EXTENSION;

typedef struct __HIDCLASS_FILEOP_CONTEXT__
{
    //
    // device extension
    //
    PHIDCLASS_PDO_DEVICE_EXTENSION DeviceExtension;

    //
    // spin lock
    //
    KSPIN_LOCK Lock;

    //
    // stop in progress indicator
    //
    BOOLEAN StopInProgress;

    //
    // link on the collection's list of open file objects
    //
    LIST_ENTRY FileOpLink;

    //
    // reports waiting for somebody to read them
    //
    HIDCLASS_REPORT_RING ReportRing;

    //
    // reads waiting for a report to arrive
    //
    LIST_ENTRY PendingReadListHead;

} HIDCLASS_FILEOP_CONTEXT, *PHIDCLASS_FILEOP_CONTEXT;

/* fdo.c */
NTSTATUS
HidClassFDO_PnP(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp);

NTSTATUS
HidClassFDO_DispatchRequest(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp);

NTSTATUS
HidClassFDO_DispatchRequestSynchronous(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp);

/* pdo.c */
NTSTATUS
HidClassPDO_CreatePDO(
    IN PDEVICE_OBJECT DeviceObject,
    OUT PDEVICE_RELATIONS *OutDeviceRelations);

NTSTATUS
HidClassPDO_PnP(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp);

PHIDP_COLLECTION_DESC
HidClassPDO_GetCollectionDescription(
    PHIDP_DEVICE_DESC DeviceDescription,
    ULONG CollectionNumber);

PHIDP_REPORT_IDS
HidClassPDO_GetReportDescription(
    PHIDP_DEVICE_DESC DeviceDescription,
    ULONG CollectionNumber);

PHIDP_REPORT_IDS
HidClassPDO_GetReportDescriptionByReportID(
    PHIDP_DEVICE_DESC DeviceDescription,
    UCHAR ReportID);

/* pingpong.c */
NTSTATUS
HidClass_RingInitialize(
    _Out_ PHIDCLASS_REPORT_RING Ring,
    _In_ ULONG ReportSize,
    _In_ ULONG ReportCount);

VOID
HidClass_RingFree(
    _Inout_ PHIDCLASS_REPORT_RING Ring);

VOID
HidClass_RingPut(
    _Inout_ PHIDCLASS_REPORT_RING Ring,
    _In_reads_bytes_(Length) PVOID Report,
    _In_ ULONG Length);

BOOLEAN
HidClass_RingGet(
    _Inout_ PHIDCLASS_REPORT_RING Ring,
    _Out_writes_bytes_(Length) PVOID Report,
    _In_ ULONG Length);

NTSTATUS
HidClass_StartReads(
    _Inout_ PHIDCLASS_FDO_EXTENSION FDODeviceExtension);

VOID
HidClass_StopReads(
    _Inout_ PHIDCLASS_FDO_EXTENSION FDODeviceExtension);

#endif /* _HIDCLASS_PCH_ */
