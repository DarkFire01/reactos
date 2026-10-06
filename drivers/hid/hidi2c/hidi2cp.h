/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     HID over I2C - private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * The top of the HID-over-I2C stack. Everything below it is now in place:
 *
 *   ACPI gives this device an I2cSerialBus Connection() descriptor
 *     -> acpiex turns it into a connection id      (drivers/bus/acpiex)
 *     -> we format that id and open it             (transport.c)
 *     -> acpiex reparses the open to the controller
 *     -> SpbCx makes a target of it                (drivers/spb/spbcx)
 *     -> the controller driver clocks the bytes    (drivers/spb/i2c)
 *
 * Above, mshidkmdf presents this driver to the WDM hidclass as a HID minidriver,
 * so the IOCTL_HID_* set arrives as internal device control.
 *
 * The wire protocol is the HID over I2C specification. A device exposes a set of
 * 16-bit registers whose addresses are themselves discovered: ACPI _DSM function
 * 1 returns the address of the HID descriptor, and the HID descriptor names
 * every other register.
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <spb.h>
#include <hidport.h>
#include <debug.h>

#define HIDI2C_POOL_TAG 'I2iH'   /* "Hi2I" */

/*
 * The 30-byte HID descriptor, read from the address _DSM hands back. Byte-packed
 * and little-endian on the wire.
 */
#include <pshpack1.h>
typedef struct _HIDI2C_DESCRIPTOR
{
    USHORT DescLength;          /* always 30 */
    USHORT BcdVersion;
    USHORT ReportDescLength;
    USHORT ReportDescRegister;
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

#define HIDI2C_DESCRIPTOR_LENGTH 30

/*
 * Commands go to the command register as a two-byte opcode. The low byte carries
 * the opcode, the high byte the report type and id for the report commands.
 */
#define HIDI2C_OPCODE_RESET             0x01
#define HIDI2C_OPCODE_GET_REPORT        0x02
#define HIDI2C_OPCODE_SET_REPORT        0x03
#define HIDI2C_OPCODE_GET_IDLE          0x04
#define HIDI2C_OPCODE_SET_IDLE          0x05
#define HIDI2C_OPCODE_GET_PROTOCOL      0x06
#define HIDI2C_OPCODE_SET_PROTOCOL      0x07
#define HIDI2C_OPCODE_SET_POWER         0x08

#define HIDI2C_POWER_ON                 0x00
#define HIDI2C_POWER_SLEEP              0x01

/* ACPI _DSM {3CDFF6F7-4267-4555-AD05-B30A3D8938DE}, function 1 */
#define HIDI2C_DSM_REVISION             1
#define HIDI2C_DSM_FUNCTION_HID_DESC    1

typedef struct _HIDI2C_CONTEXT
{
    WDFDEVICE Device;

    /* The I2C connection this peripheral sits on */
    LARGE_INTEGER ConnectionId;
    WDFIOTARGET SpbTarget;

    /* Where the HID descriptor lives, from _DSM */
    USHORT DescriptorRegister;

    HIDI2C_DESCRIPTOR Descriptor;
    PUCHAR ReportDescriptor;
    ULONG ReportDescriptorLength;

    /*
     * The device asserts its interrupt and holds it until the input report is
     * read, so the DPC reads one report per assertion into here.
     */
    WDFINTERRUPT Interrupt;
    PUCHAR InputBuffer;
    ULONG InputBufferLength;

    /* hidclass keeps exactly one read outstanding; it completes from the DPC */
    WDFQUEUE ReadQueue;
} HIDI2C_CONTEXT, *PHIDI2C_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(HIDI2C_CONTEXT, HidI2cGetContext)

/* transport.c */

NTSTATUS HidI2cOpenSpbTarget(_In_ PHIDI2C_CONTEXT Context);
VOID HidI2cCloseSpbTarget(_In_ PHIDI2C_CONTEXT Context);

NTSTATUS HidI2cSpbWrite(_In_ PHIDI2C_CONTEXT Context,
                        _In_reads_bytes_(Length) PVOID Buffer,
                        _In_ ULONG Length);

NTSTATUS HidI2cSpbWriteRead(_In_ PHIDI2C_CONTEXT Context,
                            _In_reads_bytes_(WriteLength) PVOID WriteBuffer,
                            _In_ ULONG WriteLength,
                            _Out_writes_bytes_(ReadLength) PVOID ReadBuffer,
                            _In_ ULONG ReadLength);

NTSTATUS HidI2cSpbRead(_In_ PHIDI2C_CONTEXT Context,
                       _Out_writes_bytes_(Length) PVOID Buffer,
                       _In_ ULONG Length);

NTSTATUS HidI2cAcpiGetDescriptorRegister(_In_ PHIDI2C_CONTEXT Context);

/* hid.c */

NTSTATUS HidI2cInitialize(_In_ PHIDI2C_CONTEXT Context);
VOID HidI2cDestroy(_In_ PHIDI2C_CONTEXT Context);
NTSTATUS HidI2cSetPower(_In_ PHIDI2C_CONTEXT Context, _In_ UCHAR PowerState);
NTSTATUS HidI2cReset(_In_ PHIDI2C_CONTEXT Context);
NTSTATUS HidI2cReadInputReport(_In_ PHIDI2C_CONTEXT Context, _Out_ PULONG BytesRead);

EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL HidI2cEvtInternalDeviceControl;
EVT_WDF_INTERRUPT_ISR HidI2cEvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC HidI2cEvtInterruptDpc;
