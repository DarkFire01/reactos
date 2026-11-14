/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Synaptics RMI4 touch controller as a HID digitizer under mshidkmdf
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define RESHUB_USE_HELPER_ROUTINES

#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>
#include <spb.h>
#include <gpio.h>
#include <reshub.h>

#define QCTOUCH_POOL_TAG            'cTcQ'

/* RMI4 function numbers this driver drives */
#define RMI_FUNCTION_F01            0x01
#define RMI_FUNCTION_F12            0x12

/* Page select lives at the top of every 256 byte page */
#define RMI_PAGE_SELECT             0xFF
#define RMI_PAGE_SIZE               0x100

/* Page descriptor table, scanned from the top down in six byte entries */
#define RMI_PDT_ENTRY_SIZE          6
#define RMI_PDT_SCAN_START          0xE9
#define RMI_PDT_SCAN_END            0x05
#define RMI_PDT_INT_SOURCE_MASK     0x07

/* F01 device control and status */
#define RMI_F01_CMD_RESET           0x01
#define RMI_F01_CTRL0_SLEEP_MASK    0x03
#define RMI_F01_CTRL0_SENSOR_SLEEP  0x01
#define RMI_F01_CTRL0_NOSLEEP       0x04
#define RMI_F01_CTRL0_CONFIGURED    0x80
#define RMI_F01_STATUS_UNCONFIGURED 0x80

/* F12 object types reported in the first byte of each finger slot */
#define RMI_F12_OBJECT_FINGER       0x01
#define RMI_F12_OBJECT_GLOVED       0x06
#define RMI_F12_FINGER_BYTES        8

/* F12 register descriptors: general info, then query, control and data */
#define RMI_F12_QUERY_HAS_DESC      0x01
#define RMI_F12_CTRL_SENSOR_TUNING  8
#define RMI_F12_DATA_SENSED_OBJECTS 1

#define RMI_REG_DESC_MAX_REGS       64
#define RMI_READ_MAX                128

/* How long the controller takes to come back from an F01 reset */
#define RMI_RESET_DELAY_MS          200

typedef struct _RMI_REG_ITEM
{
    ULONG Register;
    ULONG Size;
    ULONG SubpacketCount;
    BOOLEAN HasSubpacket0;
} RMI_REG_ITEM, *PRMI_REG_ITEM;

typedef struct _RMI_REG_DESC
{
    ULONG Count;
    RMI_REG_ITEM Items[RMI_REG_DESC_MAX_REGS];
} RMI_REG_DESC, *PRMI_REG_DESC;

/* A function picked up by the descriptor table scan */
typedef struct _RMI_FUNCTION
{
    BOOLEAN Found;
    USHORT QueryBase;
    USHORT CommandBase;
    USHORT ControlBase;
    USHORT DataBase;
    ULONG InterruptCount;
    ULONG InterruptPosition;
} RMI_FUNCTION, *PRMI_FUNCTION;

/* HID layout, see hid.c */
#define QCTOUCH_MAX_CONTACTS        10
#define QCTOUCH_LOGICAL_MAX         0x7FFF

#define QCTOUCH_REPORT_ID_TOUCH     0x01
#define QCTOUCH_REPORT_ID_MAXCOUNT  0x03
#define QCTOUCH_REPORT_ID_CONFIG    0x04

#include <pshpack1.h>
typedef struct _QCTOUCH_CONTACT_REPORT
{
    UCHAR Flags;
    UCHAR ContactId;
    USHORT X;
    USHORT Y;
} QCTOUCH_CONTACT_REPORT, *PQCTOUCH_CONTACT_REPORT;

typedef struct _QCTOUCH_TOUCH_REPORT
{
    UCHAR ReportId;
    QCTOUCH_CONTACT_REPORT Contacts[QCTOUCH_MAX_CONTACTS];
    UCHAR ContactCount;
} QCTOUCH_TOUCH_REPORT, *PQCTOUCH_TOUCH_REPORT;

#include <poppack.h>

#define QCTOUCH_CONTACT_TIP         0x01
#define QCTOUCH_CONTACT_IN_RANGE    0x02

/* Input reports wait here until hidclass has a read pending */
#define QCTOUCH_REPORT_RING         32

typedef struct _QCTOUCH_PENDING_REPORT
{
    ULONG Length;
    UCHAR Data[sizeof(QCTOUCH_TOUCH_REPORT)];
} QCTOUCH_PENDING_REPORT, *PQCTOUCH_PENDING_REPORT;

typedef struct _QCTOUCH_DEVICE
{
    WDFDEVICE Device;

    /* Resource hub connections from the _CRS */
    LARGE_INTEGER I2cId;
    LARGE_INTEGER ResetId;
    LARGE_INTEGER EnableId;
    BOOLEAN HasI2c;
    BOOLEAN HasReset;
    BOOLEAN HasEnable;
    WDFIOTARGET I2c;
    WDFIOTARGET Reset;
    WDFIOTARGET Enable;

    WDFINTERRUPT Interrupt;
    WDFWAITLOCK RmiLock;
    BOOLEAN Ready;

    /* RMI4 state */
    UCHAR Page;
    RMI_FUNCTION F01;
    RMI_FUNCTION F12;
    ULONG InterruptRegisters;
    ULONG DataPacketSize;
    ULONG Data1Offset;
    ULONG FingerCount;
    ULONG MaxX;
    ULONG MaxY;

    /* Contact tracking between attention reads */
    ULONG ContactMask;
    USHORT ContactX[QCTOUCH_MAX_CONTACTS];
    USHORT ContactY[QCTOUCH_MAX_CONTACTS];
    UCHAR InputMode;

    /* hidclass reads and the reports waiting for them */
    WDFQUEUE ReadQueue;
    WDFSPINLOCK ReportLock;
    ULONG ReportHead;
    ULONG ReportCount;
    QCTOUCH_PENDING_REPORT Reports[QCTOUCH_REPORT_RING];
} QCTOUCH_DEVICE, *PQCTOUCH_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(QCTOUCH_DEVICE, QcTouchGetDevice)

/* spb.c */

NTSTATUS
NTAPI
QcTouchOpenConnection(
    _In_ PQCTOUCH_DEVICE Device,
    _In_ LARGE_INTEGER Id,
    _Out_ WDFIOTARGET *Target);

NTSTATUS
NTAPI
QcTouchI2cWrite(
    _In_ PQCTOUCH_DEVICE Device,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length);

NTSTATUS
NTAPI
QcTouchI2cWriteRead(
    _In_ PQCTOUCH_DEVICE Device,
    _In_reads_bytes_(WriteLength) PVOID WriteBuffer,
    _In_ ULONG WriteLength,
    _Out_writes_bytes_(ReadLength) PVOID ReadBuffer,
    _In_ ULONG ReadLength);

NTSTATUS
NTAPI
QcTouchGpioWrite(
    _In_ WDFIOTARGET Target,
    _In_ BOOLEAN High);

/* rmi4.c */

NTSTATUS
NTAPI
RmiScanFunctions(
    _In_ PQCTOUCH_DEVICE Device);

NTSTATUS
NTAPI
RmiConfigure(
    _In_ PQCTOUCH_DEVICE Device);

NTSTATUS
NTAPI
RmiSleep(
    _In_ PQCTOUCH_DEVICE Device);

NTSTATUS
NTAPI
RmiReadAttention(
    _In_ PQCTOUCH_DEVICE Device,
    _Out_ PBOOLEAN HasObjects,
    _Out_writes_bytes_(RMI_READ_MAX) PUCHAR Packet);

/* hid.c */

VOID
NTAPI
QcTouchHidInternalIoctl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode);

VOID
NTAPI
QcTouchReportObjects(
    _In_ PQCTOUCH_DEVICE Device,
    _In_reads_bytes_(RMI_READ_MAX) const UCHAR *Packet);

VOID
NTAPI
QcTouchReleaseAll(
    _In_ PQCTOUCH_DEVICE Device);
