/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     HID keyboard mapper driver definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntddk.h>
#include <wmilib.h>
#include <wmistr.h>
#include <hidclass.h>
#include <hidpi.h>
#include <kbdmou.h>
#include <wmidata.h>

#define KBDHID_TAG 'diHK'

#ifndef IOCTL_KEYBOARD_QUERY_EXTENDED_ATTRIBUTES
#define IOCTL_KEYBOARD_QUERY_EXTENDED_ATTRIBUTES \
    CTL_CODE(FILE_DEVICE_KEYBOARD, 0x0080, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define KEYBOARD_EXTENDED_ATTRIBUTES_STRUCT_VERSION_1 1

typedef struct _KEYBOARD_EXTENDED_ATTRIBUTES
{
    UCHAR Version;
    UCHAR FormFactor;
    UCHAR KeyType;
    UCHAR PhysicalLayout;
    UCHAR VendorSpecificPhysicalLayout;
    UCHAR IETFLanguageTagIndex;
    UCHAR ImplementedInputAssistControls;
} KEYBOARD_EXTENDED_ATTRIBUTES, *PKEYBOARD_EXTENDED_ATTRIBUTES;
#endif

#ifndef KEYBOARD_TYPE_UNKNOWN
#define KEYBOARD_TYPE_GENERIC_101 4
#define KEYBOARD_TYPE_JAPAN       7
#define KEYBOARD_TYPE_KOREA       8
#define KEYBOARD_TYPE_UNKNOWN     0x51
#endif

#ifndef MANUALLY_INITIATED_CRASH
#define MANUALLY_INITIATED_CRASH 0xE2
#endif

/* Consumer page usages of the extended keyboard attributes collection */
#define KBDHID_USAGE_EXTENDED_ATTRIBUTES    ((USAGE)0x2C0)
#define KBDHID_USAGE_FORM_FACTOR            ((USAGE)0x2C1)
#define KBDHID_USAGE_KEY_TYPE               ((USAGE)0x2C2)
#define KBDHID_USAGE_PHYSICAL_LAYOUT        ((USAGE)0x2C3)
#define KBDHID_USAGE_VENDOR_LAYOUT          ((USAGE)0x2C4)
#define KBDHID_USAGE_LANGUAGE_TAG           ((USAGE)0x2C5)
#define KBDHID_USAGE_INPUT_ASSIST           ((USAGE)0x2C6)

/* Event log codes */
#define KBDHID_LOG_ATTACH_FAILED            ((NTSTATUS)0xC0050002L)
#define KBDHID_LOG_CHATTERY_DEVICE          ((NTSTATUS)0x80050001L)

/* ProblemFlags */
#define KBDHID_PROBLEM_CHATTERY             0x00000001

/* Crash dump hot key tracking bits, the low byte follows the Dump1Keys layout */
#define KBDHID_DUMP_RIGHT_SHIFT             0x00000001
#define KBDHID_DUMP_RIGHT_CTRL              0x00000002
#define KBDHID_DUMP_RIGHT_ALT               0x00000004
#define KBDHID_DUMP_LEFT_SHIFT              0x00000010
#define KBDHID_DUMP_LEFT_CTRL               0x00000020
#define KBDHID_DUMP_LEFT_ALT                0x00000040
#define KBDHID_DUMP_KEY_FIRST_RELEASE       0x00000100
#define KBDHID_DUMP_KEY_SECOND_RELEASE      0x00000200
#define KBDHID_DUMP_KEY_RELEASES \
    (KBDHID_DUMP_KEY_FIRST_RELEASE | KBDHID_DUMP_KEY_SECOND_RELEASE)

/* Set 1 scan code bytes */
#define KBDHID_SCAN_EXTENDED0               0xE0
#define KBDHID_SCAN_EXTENDED1               0xE1
#define KBDHID_SCAN_BREAK                   0x80
#define KBDHID_SCAN_CTRL                    0x1D
#define KBDHID_SCAN_LEFT_SHIFT              0x2A
#define KBDHID_SCAN_RIGHT_SHIFT             0x36
#define KBDHID_SCAN_ALT                     0x38
#define KBDHID_SCAN_SCROLL_LOCK             0x46

/* Prefix seen before the current scan code byte */
typedef enum _KBDHID_SCAN_PREFIX
{
    KbdHidPrefixNone = 0,
    KbdHidPrefixE0 = 1,
    KbdHidPrefixE1 = 2
} KBDHID_SCAN_PREFIX;

/* Who owns the read IRP while a read is set up and sent */
typedef enum _KBDHID_READ_STATE
{
    KbdHidReadIssuing = 1,
    KbdHidReadIdle = 2,
    KbdHidReadCompletedInline = 3
} KBDHID_READ_STATE;

typedef enum _KBDHID_QUERY_STATE
{
    KbdHidQueryNotStarted = 0,
    KbdHidQueryInProgress = 1,
    KbdHidQueryDone = 2
} KBDHID_QUERY_STATE;

typedef struct _KBDHID_USAGE_MAPPING
{
    USAGE From;
    USAGE To;
    struct _KBDHID_USAGE_MAPPING *Next;
} KBDHID_USAGE_MAPPING, *PKBDHID_USAGE_MAPPING;

/* Everything tied to the HID collection, allocated as one block at start */
typedef struct _KBDHID_HID_DATA
{
    PHIDP_PREPARSED_DATA PreparsedData;
    HIDP_CAPS Caps;
    ULONG MaxUsages;
    HIDP_KEYBOARD_MODIFIER_STATE ModifierState;
    PCHAR InputReport;
    PUSAGE_AND_PAGE CurrentUsages;
    PUSAGE_AND_PAGE PreviousUsages;
    PUSAGE_AND_PAGE BreakUsages;
    PUSAGE_AND_PAGE RepeatUsages;
    PUSAGE_AND_PAGE ScratchMake;
    PUSAGE_AND_PAGE ScratchBreak;
    KSPIN_LOCK Lock;
    PMDL InputMdl;
} KBDHID_HID_DATA, *PKBDHID_HID_DATA;

typedef struct _KBDHID_DEVICE_EXTENSION
{
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT NextDeviceObject;
    PDEVICE_OBJECT PhysicalDeviceObject;
    DEVICE_POWER_STATE DevicePowerState;

    LONG OpenCount;
    LONG ReadState;
    KEVENT ReadStoppedEvent;
    KEVENT ReadIssueDoneEvent;

    BOOLEAN Started;
    BOOLEAN Removed;
    BOOLEAN Initialized;
    BOOLEAN ChatterReported;
    USHORT UnitId;
    ULONG ProblemFlags;

    PIRP ReadIrp;
    PKBDHID_HID_DATA HidData;
    PFILE_OBJECT ReadFileObject;
    CONNECT_DATA ConnectData;
    IO_REMOVE_LOCK RemoveLock;
    FAST_MUTEX OpenMutex;
    KEVENT StartEvent;

    KEYBOARD_INPUT_DATA InputData;
    KBDHID_SCAN_PREFIX ScanPrefix;

    KEYBOARD_ATTRIBUTES Attributes;
    KEYBOARD_EXTENDED_ATTRIBUTES ExtendedAttributes;
    LONG ExtendedAttributesState;
    KEVENT ExtendedAttributesEvent;
    KEYBOARD_ID_EX IdEx;
    KEYBOARD_INDICATOR_PARAMETERS Indicators;
    KEYBOARD_TYPEMATIC_PARAMETERS Typematic;

    KDPC RepeatDpc;
    KTIMER RepeatTimer;
    LARGE_INTEGER RepeatDelay;
    ULONG RepeatPeriod;

    KDPC RetryDpc;
    KTIMER RetryTimer;
    LARGE_INTEGER RetryDelay;

    WMILIB_CONTEXT WmiLibInfo;

    PKBDHID_USAGE_MAPPING Mappings;
    KSPIN_LOCK MappingLock;

    ULONG CrashDumpState;
} KBDHID_DEVICE_EXTENSION, *PKBDHID_DEVICE_EXTENSION;

typedef struct _KBDHID_GLOBALS
{
    UNICODE_STRING RegistryPath;
    LONG DeviceCount;
    BOOLEAN BreakOnSysRq;
    BOOLEAN DisableAutoRepeat;
    UCHAR DumpKey;
    UCHAR DumpKeyAlt;
    ULONG DumpModifiers;
} KBDHID_GLOBALS, *PKBDHID_GLOBALS;

extern KBDHID_GLOBALS KbdHidGlobals;

/* kbdhid.c */
DRIVER_DISPATCH KbdHidPassThrough;

/* ioctl.c */
DRIVER_DISPATCH KbdHidInternalDeviceControl;

/* pnp.c */
DRIVER_ADD_DEVICE KbdHidAddDevice;
DRIVER_DISPATCH KbdHidPnp;
DRIVER_DISPATCH KbdHidPower;

NTSTATUS
NTAPI
KbdHidCallHidClass(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension,
    _In_ ULONG IoControlCode,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength);

NTSTATUS
NTAPI
KbdHidQueryExtendedAttributes(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension);

/* read.c */
KDEFERRED_ROUTINE KbdHidAutoRepeatDpc;
KDEFERRED_ROUTINE KbdHidRetryReadDpc;

NTSTATUS
NTAPI
KbdHidStartRead(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension);

BOOLEAN
NTAPI
KbdHidCancelAutoRepeat(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension);

/* wmi.c */
DRIVER_DISPATCH KbdHidSystemControl;

NTSTATUS
NTAPI
KbdHidQueryWmiRegInfo(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PULONG RegFlags,
    _Inout_ PUNICODE_STRING InstanceName,
    _Out_opt_ PUNICODE_STRING *RegistryPath,
    _Inout_ PUNICODE_STRING MofResourceName,
    _Out_opt_ PDEVICE_OBJECT *Pdo);

NTSTATUS
NTAPI
KbdHidQueryWmiDataBlock(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG InstanceCount,
    _Out_opt_ PULONG InstanceLengthArray,
    _In_ ULONG BufferAvail,
    _Out_opt_ PUCHAR Buffer);

NTSTATUS
NTAPI
KbdHidSetWmiDataBlock(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG BufferSize,
    _In_ PUCHAR Buffer);

NTSTATUS
NTAPI
KbdHidSetWmiDataItem(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG DataItemId,
    _In_ ULONG BufferSize,
    _In_ PUCHAR Buffer);

extern WMIGUIDREGINFO KbdHidWmiGuidList[2];
