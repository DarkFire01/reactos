#pragma once

#define _HIDPI_NO_FUNCTION_MACROS_
#include <ntddk.h>
#include <hidclass.h>
#include <hidpddi.h>
#include <hidpi.h>
#define NDEBUG
#include <debug.h>
#include <ntddmou.h>
#include <kbdmou.h>
#include <debug.h>


/* How many consecutive failed or empty reads to ride out before giving up
   on the report cycle rather than resubmitting for ever. */
#define MOUHID_MAX_READ_ERRORS 32

typedef struct
{
    //
    // lower device object
    //
    PDEVICE_OBJECT NextDeviceObject;

    //
    // irp which is used for reading input reports
    //
    PIRP Irp;

    //
    // event
    //
    KEVENT ReadCompletionEvent;

    //
    // device object for class callback
    //
    PDEVICE_OBJECT ClassDeviceObject;

    //
    // class callback
    //
    PVOID ClassService;

    //
    // mouse type
    //
    USHORT MouseIdentifier;

    //
    // wheel usage page
    //
    USHORT WheelUsagePage;

    //
    // buffer for the four usage lists below
    //
    PVOID UsageListBuffer;

    //
    // usage list length
    //
    USHORT UsageListLength;

    //
    // current usage list length
    //
    PUSAGE CurrentUsageList;

    //
    // previous usage list
    //
    PUSAGE PreviousUsageList;

    //
    // removed usage item list
    //
    PUSAGE BreakUsageList;

    //
    // new item usage list
    //
    PUSAGE MakeUsageList;

    //
    // preparsed data
    //
    PVOID PreparsedData;

    //
    // mdl for reading input report
    //
    PMDL ReportMDL;

    //
    // input report buffer
    //
    PCHAR Report;

    //
    // input report length
    //
    ULONG ReportLength;

    //
    // file object the device is reading reports from
    //
    PFILE_OBJECT FileObject;

    //
    // report read is active
    //
    UCHAR ReadReportActive;

    //
    // stop reading flag
    //
    UCHAR StopReadReport;

    /* consecutive failed or empty reads, see MouHid_ReadCompletion */
    ULONG ReadErrorCount;

    /* how many reports have been dumped to the log so far */
    ULONG ReportsTraced;

    /* guards against MouHid_InitiateRead re-entering on its own stack
       when the request below it completes inline */
    volatile LONG ReadSubmitCount;

    //
    // mouse absolute
    //
    UCHAR MouseAbsolute;

    //
    // value caps x
    //
    HIDP_VALUE_CAPS ValueCapsX;

    //
    // value caps y button
    //
    HIDP_VALUE_CAPS ValueCapsY;

} MOUHID_DEVICE_EXTENSION, *PMOUHID_DEVICE_EXTENSION;

#define WHEEL_DELTA 120
#define VIRTUAL_SCREEN_SIZE_X (65536)
#define VIRTUAL_SCREEN_SIZE_Y (65536)

NTSTATUS
MouHid_InitiateRead(
    IN PMOUHID_DEVICE_EXTENSION DeviceExtension);

#define MOUHID_TAG 'diHM'
