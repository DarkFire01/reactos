/*
 * PROJECT:     ReactOS USB Role Switch Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     URS client device interface
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "ursfuncenum.h"
#include "ursglobals.h"

WDF_EXTERN_C_START

#define URS_CLASS_MAJOR_VERSION 1
#define URS_CLASS_MINOR_VERSION 0

/* Resources a role child gets from its parent */
DECLARE_HANDLE(URSIORESLIST);

typedef enum _URS_HOST_INTERFACE_TYPE
{
    UrsHostInterfaceTypeEhci,
    UrsHostInterfaceTypeXhci
} URS_HOST_INTERFACE_TYPE;

typedef enum _URS_ROLE
{
    UrsRoleNone,
    UrsRoleHost,
    UrsRoleFunction
} URS_ROLE;

typedef enum _URS_HARDWARE_EVENT
{
    UrsHardwareEventNone,
    UrsHardwareEventDetach,
    UrsHardwareEventIdGround,
    UrsHardwareEventIdFloat,
    UrsHardwareEventIdA,
    UrsHardwareEventIdB,
    UrsHardwareEventIdC
} URS_HARDWARE_EVENT;

typedef
_Function_class_(EVT_URS_DEVICE_FILTER_RESOURCE_REQUIREMENTS)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EVT_URS_DEVICE_FILTER_RESOURCE_REQUIREMENTS(
    _In_ WDFDEVICE Device,
    _In_ WDFIORESREQLIST IoResourceRequirementsList,
    _In_ URSIORESLIST HostRoleResources,
    _In_ URSIORESLIST FunctionRoleResources);

typedef EVT_URS_DEVICE_FILTER_RESOURCE_REQUIREMENTS *PFN_URS_DEVICE_FILTER_RESOURCE_REQUIREMENTS;

typedef
_Function_class_(EVT_URS_SET_ROLE)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EVT_URS_SET_ROLE(
    _In_ WDFDEVICE Device,
    _In_ URS_ROLE Role);

typedef EVT_URS_SET_ROLE *PFN_URS_SET_ROLE;

typedef struct _URS_CONFIG
{
    ULONG Size;
    URS_HOST_INTERFACE_TYPE HostInterfaceType;
    PFN_URS_DEVICE_FILTER_RESOURCE_REQUIREMENTS EvtUrsFilterRemoveResourceRequirements;
    PFN_URS_SET_ROLE EvtUrsSetRole;
} URS_CONFIG, *PURS_CONFIG;

FORCEINLINE
VOID
URS_CONFIG_INIT(
    _Out_ PURS_CONFIG Config,
    _In_ URS_HOST_INTERFACE_TYPE HostInterfaceType,
    _In_ PFN_URS_DEVICE_FILTER_RESOURCE_REQUIREMENTS EvtUrsFilterRemoveResourceRequirements)
{
    RtlZeroMemory(Config, sizeof(*Config));
    Config->Size = sizeof(*Config);
    Config->HostInterfaceType = HostInterfaceType;
    Config->EvtUrsFilterRemoveResourceRequirements = EvtUrsFilterRemoveResourceRequirements;
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_URSDEVICEINITIALIZE)(
    _In_ PURS_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_ PURS_CONFIG Config);

typedef
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_URSDEVICEINITINITIALIZE)(
    _In_ PURS_DRIVER_GLOBALS DriverGlobals,
    _In_ PWDFDEVICE_INIT DeviceInit);

typedef
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_URSIORESOURCELISTAPPENDDESCRIPTOR)(
    _In_ PURS_DRIVER_GLOBALS DriverGlobals,
    _In_ URSIORESLIST IoResourceList,
    _In_ PIO_RESOURCE_DESCRIPTOR Descriptor);

typedef
_IRQL_requires_max_(HIGH_LEVEL)
VOID
(NTAPI *PFN_URSREPORTHARDWAREEVENT)(
    _In_ PURS_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_ URS_HARDWARE_EVENT HardwareEvent);

typedef
_IRQL_requires_(PASSIVE_LEVEL)
VOID
(NTAPI *PFN_URSSETPOHANDLE)(
    _In_ PURS_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_opt_ POHANDLE PoHandle);

typedef
_IRQL_requires_(PASSIVE_LEVEL)
VOID
(NTAPI *PFN_URSSETHARDWAREEVENTSUPPORT)(
    _In_ PURS_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_ BOOLEAN HardwareEventReportingSupported);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
UrsDeviceInitialize(
    _In_ WDFDEVICE Device,
    _In_ PURS_CONFIG Config)
{
    return URS_BOUND_FUNCTION(PFN_URSDEVICEINITIALIZE, UrsDeviceInitializeTableIndex)(UrsDriverGlobals,
                                                                                     Device,
                                                                                     Config);
}

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
UrsDeviceInitInitialize(
    _In_ PWDFDEVICE_INIT DeviceInit)
{
    return URS_BOUND_FUNCTION(PFN_URSDEVICEINITINITIALIZE, UrsDeviceInitInitializeTableIndex)(UrsDriverGlobals,
                                                                                             DeviceInit);
}

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
UrsIoResourceListAppendDescriptor(
    _In_ URSIORESLIST IoResourceList,
    _In_ PIO_RESOURCE_DESCRIPTOR Descriptor)
{
    return URS_BOUND_FUNCTION(PFN_URSIORESOURCELISTAPPENDDESCRIPTOR,
                              UrsIoResourceListAppendDescriptorTableIndex)(UrsDriverGlobals,
                                                                           IoResourceList,
                                                                           Descriptor);
}

_IRQL_requires_max_(HIGH_LEVEL)
FORCEINLINE
VOID
NTAPI
UrsReportHardwareEvent(
    _In_ WDFDEVICE Device,
    _In_ URS_HARDWARE_EVENT HardwareEvent)
{
    URS_BOUND_FUNCTION(PFN_URSREPORTHARDWAREEVENT, UrsReportHardwareEventTableIndex)(UrsDriverGlobals,
                                                                                    Device,
                                                                                    HardwareEvent);
}

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
UrsSetPoHandle(
    _In_ WDFDEVICE Device,
    _In_opt_ POHANDLE PoHandle)
{
    URS_BOUND_FUNCTION(PFN_URSSETPOHANDLE, UrsSetPoHandleTableIndex)(UrsDriverGlobals, Device, PoHandle);
}

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
UrsSetHardwareEventSupport(
    _In_ WDFDEVICE Device,
    _In_ BOOLEAN HardwareEventReportingSupported)
{
    URS_BOUND_FUNCTION(PFN_URSSETHARDWAREEVENTSUPPORT, UrsSetHardwareEventSupportTableIndex)(
        UrsDriverGlobals,
        Device,
        HardwareEventReportingSupported);
}

WDF_EXTERN_C_END
