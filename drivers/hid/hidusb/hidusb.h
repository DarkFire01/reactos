/*
 * PROJECT:     ReactOS USB HID Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared definitions of the USB HID minidriver
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntddk.h>
#include <hidport.h>
#include <usb.h>
#include <usbioctl.h>
#include <usbdlib.h>

#define HIDUSB_TAG                      'UdiH'
#define HIDUSB_RESET_LOCK_TAG           'WRUH'
#define HIDUSB_RESET_SIGNATURE          'teSR'

/* Relative timeout of a synchronous URB, in 100ns units */
#define HIDUSB_SYNC_URB_TIMEOUT         (-50000000LL)

/* Default timeout of a timed output report sent on the control pipe, in ms */
#define HIDUSB_CONTROL_WRITE_TIMEOUT    5000

/* HID class requests */
#define HIDUSB_REQUEST_GET_REPORT       0x01
#define HIDUSB_REQUEST_SET_REPORT       0x09
#define HIDUSB_REQUEST_SET_IDLE         0x0A

/* Report types placed in the high byte of wValue */
#define HIDUSB_REPORT_TYPE_INPUT        0x0100
#define HIDUSB_REPORT_TYPE_OUTPUT       0x0200
#define HIDUSB_REPORT_TYPE_FEATURE      0x0300

/* Value placed in RequestTypeReservedBits of every class request */
#define HIDUSB_CLASS_REQUEST_BITS       0x22

/* bmRequestType of a class OUT request to an interface */
#define HIDUSB_CLASS_INTERFACE_OUT      0x21

/* MS OS genre descriptor index */
#define HIDUSB_MS_GENRE_INDEX           1

#ifndef IOCTL_HID_DEVICERESET_NOTIFICATION
#define IOCTL_HID_DEVICERESET_NOTIFICATION  HID_CTL_CODE(140)
#endif

/* Output report on the control pipe with a caller supplied timeout */
#define HIDUSB_IOCTL_WRITE_REPORT_TIMED     HID_CTL_CODE(152)

/** Transfer packet of HIDUSB_IOCTL_WRITE_REPORT_TIMED. */
typedef struct _HIDUSB_TIMED_XFER_PACKET
{
    HID_XFER_PACKET Packet;
    ULONG Flags;

    /* In ms, zero selects the default */
    ULONG Timeout;
} HIDUSB_TIMED_XFER_PACKET, *PHIDUSB_TIMED_XFER_PACKET;

typedef enum _HIDUSB_DEVICE_STATE
{
    HidUsbStateInitialized = 0,
    HidUsbStateStarting,
    HidUsbStateRunning,
    HidUsbStateStopping,
    HidUsbStateStopped,
    HidUsbStateRemoved,
    HidUsbStateFailed
} HIDUSB_DEVICE_STATE;

/* The HID descriptor follows the endpoints, class requests then target an endpoint */
#define HIDUSB_FLAG_ENDPOINT_REQUESTS   0x00000001

/** Minidriver part of the hidclass device extension. */
typedef struct _HIDUSB_EXTENSION
{
    HIDUSB_DEVICE_STATE State;
    PUSB_DEVICE_DESCRIPTOR DeviceDescriptor;
    PUSBD_INTERFACE_INFORMATION InterfaceInfo;
    USBD_CONFIGURATION_HANDLE ConfigurationHandle;

    /* Drops below zero once the device stops or goes away and nothing is outstanding */
    LONG PendingRequests;
    KEVENT NoPendingRequests;

    ULONG Flags;

    /* Non NULL while a reset work item is queued */
    PVOID ResetContext;

    HID_DESCRIPTOR HidDescriptor;
    PDEVICE_OBJECT DeviceObject;
    IO_REMOVE_LOCK RemoveLock;

    KSPIN_LOCK ResetNotifyLock;
    PIRP ResetNotifyIrp;
} HIDUSB_EXTENSION, *PHIDUSB_EXTENSION;

/** Context of the work item that recovers from a failed transfer. */
typedef struct _HIDUSB_RESET_CONTEXT
{
    ULONG Signature;
    PIO_WORKITEM WorkItem;
    PDEVICE_OBJECT DeviceObject;
    PIRP Irp;
} HIDUSB_RESET_CONTEXT, *PHIDUSB_RESET_CONTEXT;

FORCEINLINE
PHID_DEVICE_EXTENSION
HidUsbGetHidExtension(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    return (PHID_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
}

FORCEINLINE
PHIDUSB_EXTENSION
HidUsbGetExtension(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    return (PHIDUSB_EXTENSION)HidUsbGetHidExtension(DeviceObject)->MiniDeviceExtension;
}

FORCEINLINE
PDEVICE_OBJECT
HidUsbGetNextDevice(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    return HidUsbGetHidExtension(DeviceObject)->NextDeviceObject;
}

/* hidusb.c */

NTSTATUS
NTAPI
HidUsbIncrementPendingRequests(
    _Inout_ PHIDUSB_EXTENSION Extension);

VOID
NTAPI
HidUsbDecrementPendingRequests(
    _Inout_ PHIDUSB_EXTENSION Extension);

/* usb.c */

NTSTATUS
NTAPI
HidUsbCallUsb(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PURB Urb);

NTSTATUS
NTAPI
HidUsbGetDescriptor(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ USHORT Function,
    _In_ UCHAR DescriptorType,
    _Inout_ PVOID *Buffer,
    _Inout_ PULONG BufferLength,
    _In_ UCHAR Index,
    _In_ USHORT LanguageId);

PUSBD_PIPE_INFORMATION
NTAPI
HidUsbFindInterruptPipe(
    _In_ PHIDUSB_EXTENSION Extension,
    _In_ BOOLEAN DirectionIn);

NTSTATUS
NTAPI
HidUsbAbortPendingRequests(
    _In_ PDEVICE_OBJECT DeviceObject);

NTSTATUS
NTAPI
HidUsbQueueResetWorkItem(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp);

VOID
NTAPI
HidUsbCompleteResetNotification(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ NTSTATUS Status);

/* pnp.c */

NTSTATUS
NTAPI
HidUsbSetIdle(
    _In_ PDEVICE_OBJECT DeviceObject);

DRIVER_DISPATCH HidUsbPnp;

/* ioctl.c */

DRIVER_DISPATCH HidUsbInternalIoctl;
