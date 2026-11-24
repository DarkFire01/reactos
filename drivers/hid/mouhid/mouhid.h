/*
 * PROJECT:     ReactOS HID Mouse Mapper Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared definitions for the HID mouse mapper
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define _HIDPI_NO_FUNCTION_MACROS_
#include <ntddk.h>
#include <wmilib.h>
#include <wmistr.h>
#include <wmidata.h>
#include <hidclass.h>
#include <hidpi.h>
#include <hidusage.h>
#include <ntddmou.h>
#include <kbdmou.h>

#define MOUHID_TAG                      'HuoM'

/* Usages the mapper looks for that hidusage.h does not name */
#define MOUHID_USAGE_RESOLUTION_MULTIPLIER  ((USAGE)0x48)
#define MOUHID_USAGE_CONSUMER_AC_PAN        ((USAGE)0x238)

/* Default wheel delta per reported detent */
#define MOUHID_DEFAULT_WHEEL_SCALE      120

/* Absolute coordinates are scaled to 0..0xFFFF */
#define MOUHID_ABSOLUTE_RANGE           0xFFFF

/* Device family that advertises HORIZONTAL_WHEEL_PRESENT */
#define MOUHID_FAMILY_XBOX              5

/* Event log codes */
#define MOUHID_LOG_ATTACH_FAILED        ((NTSTATUS)0xC0050002L)
#define MOUHID_LOG_BAD_PHYSICAL_RANGE   ((NTSTATUS)0x80050001L)
#define MOUHID_LOG_ABSOLUTE_IGNORED     ((NTSTATUS)0x80050002L)

/* MOUHID_EXTENSION::ProblemFlags */
#define MOUHID_PROBLEM_ABSOLUTE_IGNORED 0x00000001
#define MOUHID_PROBLEM_RAW_X            0x00000002
#define MOUHID_PROBLEM_RAW_Y            0x00000004
#define MOUHID_PROBLEM_RAW_WHEEL        0x00000008
#define MOUHID_PROBLEM_RAW_HWHEEL       0x00000010

/* MOUHID_EXTENSION::ReadState */
#define MOUHID_READ_IDLE                0
#define MOUHID_READ_SUBMITTING          1
#define MOUHID_READ_REARM               2
#define MOUHID_READ_INLINE_DONE         3

/* MOUHID_EXTENSION::FileState */
#define MOUHID_FILE_NONE                0
#define MOUHID_FILE_OPEN                1
#define MOUHID_FILE_MULTIPLIER_BUSY     2
#define MOUHID_FILE_CLOSING             3

/* MOUHID_EXTENSION::MultiplierState */
#define MOUHID_MULTIPLIER_PENDING       0
#define MOUHID_MULTIPLIER_UNSUPPORTED   1
#define MOUHID_MULTIPLIER_APPLIED       2
#define MOUHID_MULTIPLIER_REAPPLY       3

/* Collection layout gathered at start */
typedef struct _MOUHID_COLLECTION
{
    USHORT BitSizeX;
    USHORT BitSizeY;
    USHORT BitSizeWheel;
    USHORT BitSizeHWheel;
    LONG MaxX;
    LONG MaxY;
    BOOLEAN IsAbsolute;
    BOOLEAN WheelMissing;
    USAGE WheelUsagePage;
    USAGE WheelUsage;
    BOOLEAN HWheelMissing;
    USAGE HWheelUsagePage;
    USAGE HWheelUsage;
    USHORT MaxUsageListLength;
    PHIDP_PREPARSED_DATA PreparsedData;
    HIDP_CAPS Caps;
    PCHAR Report;
    PUSAGE CurrentUsages;
    PUSAGE PreviousUsages;
    PUSAGE BreakUsages;
    PUSAGE MakeUsages;
    PMDL ReportMdl;
} MOUHID_COLLECTION, *PMOUHID_COLLECTION;

typedef struct _MOUHID_EXTENSION
{
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT TopOfStack;
    PDEVICE_OBJECT Pdo;

    volatile LONG EnableCount;
    volatile LONG ReadState;
    BOOLEAN Started;
    BOOLEAN Removed;
    BOOLEAN Initialized;
    USHORT UnitId;

    DEVICE_POWER_STATE DevicePowerState;
    DEVICE_POWER_STATE PreviousDevicePowerState;

    BOOLEAN FlipFlopWheel;
    BOOLEAN FlipFlopHScroll;
    volatile LONG MultiplierState;

    /* Guards the wheel scale and resolution multiplier values */
    KSPIN_LOCK WheelLock;
    LONG WheelScalingFactor;
    LONG WheelScale;
    LONG HScrollScalingFactor;
    LONG HScrollScale;
    BOOLEAN VScrollHighResolutionDisable;
    USAGE VScrollPageOverride;
    USAGE VScrollUsageOverride;
    BOOLEAN HScrollHighResolutionDisable;
    USAGE HScrollPageOverride;
    USAGE HScrollUsageOverride;
    LONG VResolutionMultiplier;
    LONG HResolutionMultiplier;

    PIRP ReadIrp;
    ULONG ProblemFlags;
    PFILE_OBJECT ReadFile;
    volatile LONG FileState;
    KEVENT ReadStoppedEvent;
    KEVENT ReadSentEvent;
    KEVENT MultiplierEvent;

    PMOUHID_COLLECTION Collection;
    CONNECT_DATA ConnectData;
    IO_REMOVE_LOCK RemoveLock;
    FAST_MUTEX CreateMutex;
    KEVENT StartEvent;

    MOUSE_INPUT_DATA InputData;
    MOUSE_ATTRIBUTES Attributes;
    WMILIB_CONTEXT WmiLibInfo;
} MOUHID_EXTENSION, *PMOUHID_EXTENSION;

/* Driver wide options from the service Parameters key */
typedef struct _MOUHID_GLOBALS
{
    BOOLEAN TreatAbsoluteAsRelative;
    BOOLEAN TreatAbsolutePointerAsAbsolute;
    BOOLEAN UseOnlyMice;
    volatile LONG UnitCount;
    ULONG DeviceFamily;
    UNICODE_STRING RegistryPath;
} MOUHID_GLOBALS;

extern MOUHID_GLOBALS MouHidGlobals;

/* mouhid.c */
DRIVER_INITIALIZE DriverEntry;

VOID
NTAPI
MouHidLogError(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ NTSTATUS ErrorCode,
    _In_ NTSTATUS FinalStatus);

/* create.c */
_Dispatch_type_(IRP_MJ_CREATE)
DRIVER_DISPATCH MouHidCreate;

_Dispatch_type_(IRP_MJ_CLOSE)
DRIVER_DISPATCH MouHidClose;

VOID
NTAPI
MouHidApplyResolutionMultipliers(
    _In_ PMOUHID_EXTENSION Extension);

/* pnp.c */
DRIVER_ADD_DEVICE MouHidAddDevice;

_Dispatch_type_(IRP_MJ_PNP)
DRIVER_DISPATCH MouHidPnp;

_Dispatch_type_(IRP_MJ_POWER)
DRIVER_DISPATCH MouHidPower;

NTSTATUS
NTAPI
MouHidCallHidClass(
    _In_ PMOUHID_EXTENSION Extension,
    _In_ ULONG IoControlCode,
    _In_reads_bytes_opt_(InputLength) PVOID InputBuffer,
    _In_ ULONG InputLength,
    _Out_writes_bytes_opt_(OutputLength) PVOID OutputBuffer,
    _In_ ULONG OutputLength);

/* read.c */
NTSTATUS
NTAPI
MouHidStartRead(
    _In_ PMOUHID_EXTENSION Extension);

/* wmi.c */
_Dispatch_type_(IRP_MJ_SYSTEM_CONTROL)
DRIVER_DISPATCH MouHidSystemControl;

VOID
NTAPI
MouHidInitializeWmi(
    _In_ PMOUHID_EXTENSION Extension);
