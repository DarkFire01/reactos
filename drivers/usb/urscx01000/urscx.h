/*
 * PROJECT:     ReactOS USB Role Switch Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntddk.h>
#include <windef.h>
#include <wdf.h>
#include <fxldr.h>
#include <acpiioct.h>
#include <ntstrsafe.h>
#include <wdfcx.h>
#include <ursdevice.h>

#define URS_POOL_TAG                    'XsrU'

/* Marks globals handed out by this class extension */
#define URS_GLOBALS_SIGNATURE           'lbGU'

/* Role switch requests on the controller, FILE_DEVICE_BUS_EXTENDER based */
#define URS_IOCTL(Function)             CTL_CODE(FILE_DEVICE_BUS_EXTENDER, (Function), METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_URS_INTERNAL_GET_BUS_INFO URS_IOCTL(1)
#define IOCTL_URS_GET_ROLE_SWITCH_MODE  URS_IOCTL(2)
#define IOCTL_URS_SET_ROLE_SWITCH_MODE  URS_IOCTL(3)
#define IOCTL_URS_GET_CURRENT_ROLE      URS_IOCTL(4)
#define IOCTL_URS_REPORT_HARDWARE_EVENT URS_IOCTL(5)
#define IOCTL_URS_INTERNAL_FAKE_LOAD    URS_IOCTL(6)
#define IOCTL_URS_GET_PLD_LIST          URS_IOCTL(7)

/* Role switch modes as the registry and the mode requests carry them */
typedef enum _URS_ROLE_SWITCH_MODE
{
    UrsModeUnset = 0,
    UrsModeHost = 1,
    UrsModeFunction = 2,
    UrsModeHardwareEvents = 3,
    UrsModeNone = 4,
    UrsModeOemDefault = 5,
    UrsModeHardwareEventsHostOnly = 6
} URS_ROLE_SWITCH_MODE;

/* Kind of bus the controller sits on, as the bus information header carries it */
#define URS_BUS_UNKNOWN                 0
#define URS_BUS_ACPI                    1
#define URS_BUS_PCI                     2

typedef struct _URS_BUS_INFORMATION
{
    ULONG Size;
    ULONG BusType;
    /* ACPI_DEVICE_INFORMATION_OUTPUT_BUFFER follows for ACPI controllers */
} URS_BUS_INFORMATION, *PURS_BUS_INFORMATION;

#define URS_UNKNOWN_BUS_INFORMATION_SIZE 336

typedef struct _URS_PLD_LIST
{
    ULONG Size;
    ULONG Count;
    ULONG64 Entries[ANYSIZE_ARRAY];
} URS_PLD_LIST, *PURS_PLD_LIST;

typedef struct _URS_GLOBALS_HEADER
{
    ULONG Signature;
    URS_DRIVER_GLOBALS Globals;
} URS_GLOBALS_HEADER, *PURS_GLOBALS_HEADER;

typedef struct _URS_FDO
{
    WDFDEVICE Device;
    URS_CONFIG Config;
    WDFCOLLECTION HostResources;
    WDFCOLLECTION FunctionResources;

    /* Last hardware event not taken by the role manager yet */
    volatile LONG PendingEvent;
    BOOLEAN FollowHardwareEvents;
    BOOLEAN HostOnlyEvents;

    /* 0 none, 1 supported, 2 not reported by the client yet */
    ULONG HardwareEventSupport;
    BOOLEAN VBusUnmanaged;

    URS_ROLE CurrentRole;
    URS_HARDWARE_EVENT LastEvent;
    WDFDPC EventDpc;
    WDFWORKITEM EventWorkItem;
    WDFMEMORY BusInformation;
    POHANDLE PoHandle;
    WDFWAITLOCK Lock;
    WDFQUEUE ControlQueue;
    WDFQUEUE PldQueue;
    BOOLEAN FakeFunctionLoad;
    WDFCOLLECTION HardwareIds;
    WDFCOLLECTION CompatibleIds;
    WDFCOLLECTION Plds;
} URS_FDO, *PURS_FDO;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(URS_FDO, UrsGetFdo)

typedef
NTSTATUS
NTAPI
URS_ROLE_CALLBACK(
    _In_ PURS_FDO Fdo,
    _In_ URS_ROLE Role);

typedef URS_ROLE_CALLBACK *PURS_ROLE_CALLBACK;

typedef struct _URS_ROLE_CONFIG
{
    ULONG Size;
    URS_ROLE Role;
    PURS_ROLE_CALLBACK Prepare;
    PURS_ROLE_CALLBACK Release;
} URS_ROLE_CONFIG, *PURS_ROLE_CONFIG;

typedef struct _URS_PDO
{
    WDFDEVICE Device;
    PURS_FDO Fdo;
    URS_ROLE_CONFIG RoleConfig;
    WDFCOLLECTION Resources;
} URS_PDO, *PURS_PDO;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(URS_PDO, UrsGetPdo)

extern WDF_CLASS_LIBRARY_INFO UrsClassLibraryInfo;

/* The class extension device init calls have no public wrappers in this KMDF */
FORCEINLINE
PWDFCXDEVICE_INIT
UrsCxDeviceInitAllocate(
    _In_ PWDFDEVICE_INIT DeviceInit)
{
    return ((PFN_WDFCXDEVICEINITALLOCATE)WdfFunctions[WdfCxDeviceInitAllocateTableIndex])(WdfDriverGlobals,
                                                                                         DeviceInit);
}

FORCEINLINE
NTSTATUS
UrsCxDeviceInitAssignWdmIrpPreprocessCallback(
    _In_ PWDFCXDEVICE_INIT CxDeviceInit,
    _In_ PFN_WDFCXDEVICE_WDM_IRP_PREPROCESS Callback,
    _In_ UCHAR MajorFunction,
    _In_reads_(MinorCount) PUCHAR MinorFunctions,
    _In_ ULONG MinorCount)
{
    return ((PFN_WDFCXDEVICEINITASSIGNWDMIRPPREPROCESSCALLBACK)
            WdfFunctions[WdfCxDeviceInitAssignWdmIrpPreprocessCallbackTableIndex])(WdfDriverGlobals,
                                                                                  CxDeviceInit,
                                                                                  Callback,
                                                                                  MajorFunction,
                                                                                  MinorFunctions,
                                                                                  MinorCount);
}

/* acpi.c */
NTSTATUS
NTAPI
UrsAcpiEnumerateChildren(
    _In_ WDFDEVICE Device,
    _Out_ WDFMEMORY *Children);

NTSTATUS
NTAPI
UrsAcpiEvaluate(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR Path,
    _Out_ WDFMEMORY *Result);

NTSTATUS
NTAPI
UrsAcpiEnumeratePlds(
    _In_ PURS_FDO Fdo);

/* api.c */
VOID
NTAPI
UrsBuildExportTable(
    _Out_writes_(UrsFunctionTableNumEntries) PFN_URSFUNC *Table);

/* fdo.c */
NTSTATUS
NTAPI
UrsFdoInitializeDeviceInit(
    _In_ PWDFDEVICE_INIT DeviceInit);

NTSTATUS
NTAPI
UrsFdoInitialize(
    _In_ WDFDEVICE Device,
    _In_ PURS_CONFIG Config);

VOID
NTAPI
UrsFdoSetHardwareEventSupport(
    _In_ WDFDEVICE Device,
    _In_ BOOLEAN Supported);

/* rolemgr.c */
NTSTATUS
NTAPI
UrsRoleInitialize(
    _In_ PURS_FDO Fdo);

URS_ROLE_SWITCH_MODE
NTAPI
UrsRoleGetMode(
    _In_ PURS_FDO Fdo);

NTSTATUS
NTAPI
UrsRoleSetMode(
    _In_ PURS_FDO Fdo,
    _In_ URS_ROLE_SWITCH_MODE Mode);

NTSTATUS
NTAPI
UrsRoleReportEvent(
    _In_ PURS_FDO Fdo,
    _In_ URS_HARDWARE_EVENT Event);

/* pdo.c */
NTSTATUS
NTAPI
UrsPdoCreate(
    _In_ PURS_FDO Fdo,
    _In_ PURS_ROLE_CONFIG RoleConfig,
    _Out_ WDFDEVICE *Pdo);
