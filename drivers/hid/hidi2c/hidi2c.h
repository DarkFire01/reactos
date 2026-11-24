/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     HID over I2C private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <spb.h>
#include <hidport.h>
#include <hidclass.h>

/* Shows up as "HidI" in pool dumps */
#define HIDI2C_POOL_TAG 'IdiH'

#ifndef IOCTL_HID_DEVICERESET_NOTIFICATION
#define IOCTL_HID_DEVICERESET_NOTIFICATION HID_CTL_CODE(140)
#endif

/** HID descriptor as the device returns it, little endian. */
#include <pshpack1.h>
typedef struct _HIDI2C_DESCRIPTOR
{
    USHORT Length;
    USHORT Version;
    USHORT ReportDescriptorLength;
    USHORT ReportDescriptorRegister;
    USHORT InputRegister;
    USHORT MaxInputLength;
    USHORT OutputRegister;
    USHORT MaxOutputLength;
    USHORT CommandRegister;
    USHORT DataRegister;
    USHORT VendorId;
    USHORT ProductId;
    USHORT VersionId;
    ULONG Reserved;
} HIDI2C_DESCRIPTOR, *PHIDI2C_DESCRIPTOR;
#include <poppack.h>

C_ASSERT(sizeof(HIDI2C_DESCRIPTOR) == 30);

/* Command register opcodes, in the high byte of the command word */
#define HIDI2C_OP_RESET      0x01
#define HIDI2C_OP_GET_REPORT 0x02
#define HIDI2C_OP_SET_REPORT 0x03
#define HIDI2C_OP_SET_POWER  0x08

/* Report types, in bits 4-5 of the command word */
#define HIDI2C_REPORT_INPUT   1
#define HIDI2C_REPORT_OUTPUT  2
#define HIDI2C_REPORT_FEATURE 3

/* Report ids from here up travel in a third command byte */
#define HIDI2C_REPORT_ID_ESCAPE 0x0F

#define HIDI2C_POWER_ON    0
#define HIDI2C_POWER_SLEEP 1

/* Seconds allowed for each kind of bus transaction */
#define HIDI2C_DESCRIPTOR_TIMEOUT 10
#define HIDI2C_POWER_TIMEOUT      5
#define HIDI2C_DEFAULT_TIMEOUT    1

/* How long the default queue waits for the reset response */
#define HIDI2C_RESET_TIMEOUT_MS 4000

typedef struct _HIDI2C_DEVICE
{
    WDFDEVICE Device;
    WDFINTERRUPT Interrupt;

    /* Read requests filled by the ISR wait here for the DPC to complete them */
    WDFSPINLOCK CompletionLock;
    BOOLEAN CompletionRunning;
    BOOLEAN CompletionRerun;

    WDFQUEUE DefaultQueue;
    WDFQUEUE ReadQueue;
    WDFQUEUE CompletionQueue;
    WDFQUEUE IdleQueue;
    WDFQUEUE ResetNotifyQueue;

    LARGE_INTEGER ConnectionId;
    WDFIOTARGET Target;

    /* Reused for every input report read from the ISR */
    WDFREQUEST InputRequest;
    PUCHAR InputBuffer;

    USHORT DescriptorRegister;
    HIDI2C_DESCRIPTOR Descriptor;

    WDFTIMER ResetTimer;
    BOOLEAN ResetPending;
} HIDI2C_DEVICE, *PHIDI2C_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(HIDI2C_DEVICE, HidI2cGetDevice)

/** Ties an idle notification work item to its request. */
typedef struct _HIDI2C_IDLE_WORK
{
    WDFDEVICE Device;
    WDFREQUEST Request;
} HIDI2C_IDLE_WORK, *PHIDI2C_IDLE_WORK;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(HIDI2C_IDLE_WORK, HidI2cGetIdleWork)

/* driver.c */
_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
HidI2cReportProblem(
    _In_ PHIDI2C_DEVICE Context,
    _In_ NTSTATUS Problem);

/* acpi.c */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cQueryDescriptorRegister(
    _Inout_ PHIDI2C_DEVICE Context);

/* spb.c */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cSpbInitialize(
    _Inout_ PHIDI2C_DEVICE Context,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated);

_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
HidI2cSpbRelease(
    _Inout_ PHIDI2C_DEVICE Context);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cReadRegister(
    _In_ PHIDI2C_DEVICE Context,
    _In_ USHORT Register,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _In_ ULONG TimeoutSeconds);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cWriteRegister(
    _In_ PHIDI2C_DEVICE Context,
    _In_ USHORT Register,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length,
    _In_ ULONG TimeoutSeconds);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cCommandRead(
    _In_ PHIDI2C_DEVICE Context,
    _In_reads_bytes_(CommandLength) PUCHAR Command,
    _In_ USHORT CommandLength,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ USHORT Length);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cCommandWrite(
    _In_ PHIDI2C_DEVICE Context,
    _In_reads_bytes_(CommandLength) PUCHAR Command,
    _In_ USHORT CommandLength,
    _In_reads_bytes_(Length) PUCHAR Data,
    _In_ USHORT Length);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cReadInput(
    _In_ PHIDI2C_DEVICE Context,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length);

/* hid.c */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cInitialize(
    _Inout_ PHIDI2C_DEVICE Context,
    _In_ WDF_POWER_DEVICE_STATE PreviousState);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cShutdown(
    _Inout_ PHIDI2C_DEVICE Context,
    _In_ WDF_POWER_DEVICE_STATE TargetState);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cReset(
    _Inout_ PHIDI2C_DEVICE Context);

VOID
NTAPI
HidI2cCompleteIdleRequest(
    _In_ PHIDI2C_DEVICE Context);

EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL HidI2cEvtInternalDeviceControl;
EVT_WDF_IO_QUEUE_IO_STOP HidI2cEvtIoStop;
EVT_WDF_TIMER HidI2cEvtResetTimer;

/* interrupt.c */
EVT_WDF_INTERRUPT_ISR HidI2cEvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC HidI2cEvtInterruptDpc;
