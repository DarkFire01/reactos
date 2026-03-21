/*
 * PROJECT:     ReactOS XInput HID Filter Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     XInput-compatible HID filter driver for game controllers
 * COPYRIGHT:   Based on Microsoft xinputhid.sys reference
 *
 * This driver provides XInput API compatibility for HID game controllers
 * by creating the XUSB interface and translating HID reports to XInput format.
 */

#ifndef _XINPUTHID_H_
#define _XINPUTHID_H_

#include <ntddk.h>
#include <hidclass.h>
#include <hidpi.h>
#include <hidusage.h>
#include <wdf.h>

/* XUSB Interface Class GUID - {EC87F1E3-C46B-4FDB-B745-F8B0C6C2D64B} */
DEFINE_GUID(GUID_XUSB_INTERFACE_CLASS,
    0xEC87F1E3, 0xC46B, 0x4FDB, 0xB7, 0x45, 0xF8, 0xB0, 0xC6, 0xC2, 0xD6, 0x4B);

/* Device property flags from xinputhid.inf */
typedef enum _XINPUTHID_DEVICEPROPERTY_FLAGS
{
    XInputBusDevice = 0x1,
    XInputCompatibleDevice = 0x2,
    XInputGenericHidDevice = 0x4,
    XInputGipRootedDevice = 0x8,
} XINPUTHID_DEVICEPROPERTY_FLAGS;

/* XInput IOCTL codes - FILE_DEVICE_UNKNOWN (0x22), METHOD_BUFFERED */
#define XINPUT_CTL_CODE(Function) \
    CTL_CODE(FILE_DEVICE_UNKNOWN, (Function), METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_XINPUT_GET_INFORMATION        XINPUT_CTL_CODE(0x600)  /* 0x80006000 */
#define IOCTL_XINPUT_SET_GAMEPAD_STATE      XINPUT_CTL_CODE(0x804)  /* 0x8000A010 */
#define IOCTL_XINPUT_GET_GAMEPAD_STATE      XINPUT_CTL_CODE(0xC03)  /* 0x8000E00C */
#define IOCTL_XINPUT_GET_CAPABILITIES       XINPUT_CTL_CODE(0xC04)  /* 0x8000E010 */
#define IOCTL_XINPUT_GET_LED_STATE          XINPUT_CTL_CODE(0xC05)  /* 0x8000E014 */
#define IOCTL_XINPUT_GET_BATTERY            XINPUT_CTL_CODE(0xC06)  /* 0x8000E018 */
#define IOCTL_XINPUT_WAIT_FOR_INPUT         XINPUT_CTL_CODE(0xC0D)  /* 0x8000E034 */
#define IOCTL_XINPUT_GET_INFORMATION_EX     XINPUT_CTL_CODE(0xC0F)  /* 0x8000E03C */

/* XInput Information structure (12 bytes) */
typedef struct _XINPUT_INFORMATION
{
    ULONG Type;           /* 0x01010103 */
    ULONG SubType;
    USHORT VendorId;
    USHORT ProductId;
} XINPUT_INFORMATION, *PXINPUT_INFORMATION;

#define XINPUT_INFO_TYPE  0x01010103

/* XInput Gamepad State 1.0 (20 bytes) */
typedef struct _XINPUT_GAMEPAD_STATE_0100
{
    UCHAR Type;           /* 1 */
    UCHAR SubType;
    CHAR Reserved;
    UCHAR PacketNumber;
    USHORT Buttons;
    UCHAR LeftTrigger;
    UCHAR RightTrigger;
    SHORT ThumbLX;
    SHORT ThumbLY;
    SHORT ThumbRX;
    SHORT ThumbRY;
} XINPUT_GAMEPAD_STATE_0100, *PXINPUT_GAMEPAD_STATE_0100;

/* XInput Gamepad State 1.1 (29 bytes) */
typedef struct _XINPUT_GAMEPAD_STATE_0101
{
    USHORT Type;          /* 0x0103 */
    UCHAR SubType;
    UCHAR Reserved;
    UCHAR PacketNumber;
    USHORT Buttons;
    UCHAR LeftTrigger;
    UCHAR RightTrigger;
    SHORT ThumbLX;
    SHORT ThumbLY;
    SHORT ThumbRX;
    SHORT ThumbRY;
    UCHAR BatteryState;
    UCHAR Padding;
} XINPUT_GAMEPAD_STATE_0101, *PXINPUT_GAMEPAD_STATE_0101;

#define XINPUT_STATE_TYPE_0101  0x0103

/* Set Gamepad State (5 bytes input) */
typedef struct _XINPUT_SET_GAMEPAD_STATE
{
    UCHAR LedState;
    UCHAR LeftMotorSpeed;
    UCHAR RightMotorSpeed;
    UCHAR Flags;  /* 1=LED, 2=Vibration */
} XINPUT_SET_GAMEPAD_STATE, *PXINPUT_SET_GAMEPAD_STATE;

/* Get LED State (3 bytes in/out) */
typedef struct _XINPUT_LED_STATE
{
    USHORT Type;
    UCHAR LedState;
} XINPUT_LED_STATE, *PXINPUT_LED_STATE;

/* Default capabilities */
typedef struct _XINPUT_CAPABILITIES_POSITION
{
    USHORT Buttons;
    USHORT ThumbLX;
    USHORT ThumbLY;
    USHORT ThumbRX;
    USHORT ThumbRY;
    USHORT LeftTrigger;
    USHORT RightTrigger;
    USHORT ControllerCapabilities;
    USHORT Reserved;
} XINPUT_CAPABILITIES_POSITION;

/* Parsed HID report */
typedef struct _HID_REPORT
{
    USHORT LeftThumbX;
    USHORT LeftThumbY;
    USHORT RightThumbX;
    USHORT RightThumbY;
    USHORT Triggers[2];
    UCHAR ButtonA : 1;
    UCHAR ButtonB : 1;
    UCHAR ButtonX : 1;
    UCHAR ButtonY : 1;
    UCHAR LeftShoulder : 1;
    UCHAR RightShoulder : 1;
    UCHAR BackButton : 1;
    UCHAR StartButton : 1;
    UCHAR LeftThumbB : 1;
    UCHAR RightThumbB : 1;
    UCHAR Dpad : 4;
    UCHAR GuideButton : 1;
    UCHAR Padding : 1;
    UCHAR BatteryState;
    UCHAR MorePadding;
} HID_REPORT, *PHID_REPORT;

/* D-pad mapping */
enum
{
    DPAD_UP = 1,
    DPAD_DOWN = 2,
    DPAD_LEFT = 4,
    DPAD_RIGHT = 8,
};

/* Device context */
typedef struct _DEVICE_CONTEXT
{
    UCHAR LedState;
    UCHAR Disposed;
    UCHAR BatteryInSeparateReport;
    UCHAR Processed;
    USHORT VendorId;
    USHORT ProductId;
    USHORT BcdDevice;
    ULONG LastStatus;
    XINPUTHID_DEVICEPROPERTY_FLAGS Flags;
    ULONG CachedIndex;
    WDFDEVICE Device;
    WDFSPINLOCK DeviceSpinLock;
    WDFMEMORY PreparsedData;
    WDFMEMORY PreparsedDataChanged;
    WDFIOTARGET IoTarget;
    WDFMEMORY ReadReport;
    WDFMEMORY EmptyReport;
    WDFMEMORY WriteReport;
    WDFMEMORY ButtonUsages;
    WDFQUEUE PendingReadQueue;
    WDFQUEUE ReadyReadQueue;
    WDFQUEUE OverflowReadQueue;
    WDFQUEUE WaitingXInputQueue;
    HID_REPORT ParsedReport;
    XINPUT_GAMEPAD_STATE_0100 GamepadState0100;
    XINPUT_GAMEPAD_STATE_0101 GamepadState0101;
    HIDP_CAPS Capabilities;
    HIDP_CAPS CapabilitiesChanged;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

#endif /* _XINPUTHID_H_ */
