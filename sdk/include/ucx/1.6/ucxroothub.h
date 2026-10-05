/*
 * PROJECT:     ReactOS USB Host Controller Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     UCX root hub object interface
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#include "ucxcontroller.h"

WDF_EXTERN_C_START

typedef enum _CONTROLLER_TYPE
{
    ControllerTypeXhci = 0,
    ControllerTypeSoftXhci = 1
} CONTROLLER_TYPE;

/* Values are ASCII letters so they read well in a memory dump */
typedef enum _TRISTATE
{
    TriStateUnknown = 'u',
    TriStateFalse = 'f',
    TriStateTrue = 't'
} TRISTATE;

typedef union _CONTROLLER_USB_20_HARDWARE_LPM_FLAGS
{
    UCHAR AsUchar;
    struct
    {
        UCHAR L1CapabilitySupported:1;
        UCHAR BeslLpmCapabilitySupported:1;
    } UCX_ANONYMOUS_FLAGS;
} CONTROLLER_USB_20_HARDWARE_LPM_FLAGS, *PCONTROLLER_USB_20_HARDWARE_LPM_FLAGS;

/* Filled in by EvtRootHubGetInfo */
typedef struct _ROOTHUB_INFO
{
    ULONG Size;
    CONTROLLER_TYPE ControllerType;
    USHORT NumberOf20Ports;
    USHORT NumberOf30Ports;
    USHORT MaxU1ExitLatency;
    USHORT MaxU2ExitLatency;
} ROOTHUB_INFO, *PROOTHUB_INFO;

typedef struct _ROOTHUB_20PORT_INFO
{
    USHORT PortNumber;
    UCHAR MinorRevision;
    UCHAR HubDepth;
    TRISTATE Removable;
    TRISTATE IntegratedHubImplemented;
    TRISTATE DebugCapable;
    CONTROLLER_USB_20_HARDWARE_LPM_FLAGS ControllerUsb20HardwareLpmFlags;
} ROOTHUB_20PORT_INFO, *PROOTHUB_20PORT_INFO;

/** PortInfoArray holds NumberOfPorts pointers, each to a PortInfoSize byte entry. */
typedef struct _ROOTHUB_20PORTS_INFO
{
    ULONG Size;
    USHORT NumberOfPorts;
    USHORT PortInfoSize;
    PROOTHUB_20PORT_INFO *PortInfoArray;
} ROOTHUB_20PORTS_INFO, *PROOTHUB_20PORTS_INFO;

typedef struct _ROOTHUB_30PORT_INFO
{
    USHORT PortNumber;
    UCHAR MinorRevision;
    UCHAR HubDepth;
    TRISTATE Removable;
    TRISTATE DebugCapable;
} ROOTHUB_30PORT_INFO, *PROOTHUB_30PORT_INFO;

/* 32 sublink speed IDs from the descriptor plus 16 for the default speeds */
#define MAX_SPEEDS_COUNT (32 + 16)

/** Port info with the SuperSpeedPlus sublink speeds the port supports. */
typedef struct _ROOTHUB_30PORT_INFO_EX
{
#ifdef __cplusplus
    ROOTHUB_30PORT_INFO Info;
#else
    ROOTHUB_30PORT_INFO;
#endif
    USHORT MaxSpeedsCount;
    USHORT SpeedsCount;
    PUSB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_SPEED Speeds;
} ROOTHUB_30PORT_INFO_EX, *PROOTHUB_30PORT_INFO_EX;

typedef struct _ROOTHUB_30PORTS_INFO
{
    ULONG Size;
    USHORT NumberOfPorts;
    USHORT PortInfoSize;
    PROOTHUB_30PORT_INFO *PortInfoArray;
} ROOTHUB_30PORTS_INFO, *PROOTHUB_30PORTS_INFO;

typedef union _PARENT_HUB_FLAGS
{
    ULONG AsUlong32;
    struct
    {
        ULONG DisableLpmForAllDownstreamDevices:1;
        ULONG HubIsHighSpeedCapable:1;
        ULONG DisableUpdateMaxExitLatency:1;
        ULONG DisableU1:1;
    } UCX_ANONYMOUS_FLAGS;
} PARENT_HUB_FLAGS, *PPARENT_HUB_FLAGS;

C_ASSERT(sizeof(PARENT_HUB_FLAGS) == sizeof(ULONG));

/** Link and latency data a hub collects from its parent for the devices below it. */
typedef struct _HUB_INFO_FROM_PARENT
{
    PDEVICE_OBJECT IoTarget;
    USB_DEVICE_DESCRIPTOR DeviceDescriptor;
    USHORT U1ExitLatency;
    USHORT U2ExitLatency;
    USHORT ExitLatencyOfSlowestLinkForU1;
    UCHAR DepthOfSlowestLinkForU1;
    USHORT ExitLatencyOfSlowestLinkForU2;
    UCHAR DepthOfSlowestLinkForU2;
    USHORT HostInitiatedU1ExitLatency;
    USHORT HostInitiatedU2ExitLatency;
    UCHAR TotalHubDepth;
    USHORT TotalTPPropogationDelay;
    PARENT_HUB_FLAGS HubFlags;
    PUSB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_SPEED SublinkSpeedAttr;
    ULONG SublinkSpeedAttrCount;
} HUB_INFO_FROM_PARENT, *PHUB_INFO_FROM_PARENT;

/* Every root hub callback takes the root hub and the request to complete */

typedef
_Function_class_(EVT_UCX_ROOTHUB_INTERRUPT_TX)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_UCX_ROOTHUB_INTERRUPT_TX(
    _In_ UCXROOTHUB UcxRootHub,
    _In_ WDFREQUEST Request);
typedef EVT_UCX_ROOTHUB_INTERRUPT_TX *PFN_UCX_ROOTHUB_INTERRUPT_TX;

typedef
_Function_class_(EVT_UCX_ROOTHUB_CONTROL_URB)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_UCX_ROOTHUB_CONTROL_URB(
    _In_ UCXROOTHUB UcxRootHub,
    _In_ WDFREQUEST Request);
typedef EVT_UCX_ROOTHUB_CONTROL_URB *PFN_UCX_ROOTHUB_CONTROL_URB;

typedef
_Function_class_(EVT_UCX_ROOTHUB_GET_INFO)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_UCX_ROOTHUB_GET_INFO(
    _In_ UCXROOTHUB UcxRootHub,
    _In_ WDFREQUEST Request);
typedef EVT_UCX_ROOTHUB_GET_INFO *PFN_UCX_ROOTHUB_GET_INFO;

typedef
_Function_class_(EVT_UCX_ROOTHUB_GET_20PORT_INFO)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_UCX_ROOTHUB_GET_20PORT_INFO(
    _In_ UCXROOTHUB UcxRootHub,
    _In_ WDFREQUEST Request);
typedef EVT_UCX_ROOTHUB_GET_20PORT_INFO *PFN_UCX_ROOTHUB_GET_20PORT_INFO;

typedef
_Function_class_(EVT_UCX_ROOTHUB_GET_30PORT_INFO)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_UCX_ROOTHUB_GET_30PORT_INFO(
    _In_ UCXROOTHUB UcxRootHub,
    _In_ WDFREQUEST Request);
typedef EVT_UCX_ROOTHUB_GET_30PORT_INFO *PFN_UCX_ROOTHUB_GET_30PORT_INFO;

/*
 * A client either handles hub class requests one by one through the seven
 * feature and status callbacks, or takes all of them in EvtRootHubControlUrb.
 */
typedef struct _UCX_ROOTHUB_CONFIG
{
    ULONG Size;
    ULONG NumberOfPresentedControlUrbCallbacks;
    PFN_UCX_ROOTHUB_CONTROL_URB EvtRootHubClearHubFeature;
    PFN_UCX_ROOTHUB_CONTROL_URB EvtRootHubClearPortFeature;
    PFN_UCX_ROOTHUB_CONTROL_URB EvtRootHubGetHubStatus;
    PFN_UCX_ROOTHUB_CONTROL_URB EvtRootHubGetPortStatus;
    PFN_UCX_ROOTHUB_CONTROL_URB EvtRootHubSetHubFeature;
    PFN_UCX_ROOTHUB_CONTROL_URB EvtRootHubSetPortFeature;
    PFN_UCX_ROOTHUB_CONTROL_URB EvtRootHubGetPortErrorCount;
    PFN_UCX_ROOTHUB_CONTROL_URB EvtRootHubControlUrb;
    PFN_UCX_ROOTHUB_INTERRUPT_TX EvtRootHubInterruptTx;
    PFN_UCX_ROOTHUB_GET_INFO EvtRootHubGetInfo;
    PFN_UCX_ROOTHUB_GET_20PORT_INFO EvtRootHubGet20PortInfo;
    PFN_UCX_ROOTHUB_GET_30PORT_INFO EvtRootHubGet30PortInfo;
    WDF_OBJECT_ATTRIBUTES WdfRequestAttributes;
} UCX_ROOTHUB_CONFIG, *PUCX_ROOTHUB_CONFIG;

/* Shared part of both initializers */
FORCEINLINE
VOID
NTAPI
UcxpRootHubConfigInitCommon(
    _Out_ PUCX_ROOTHUB_CONFIG Config,
    _In_ PFN_UCX_ROOTHUB_INTERRUPT_TX EvtRootHubInterruptTx,
    _In_ PFN_UCX_ROOTHUB_GET_INFO EvtRootHubGetInfo,
    _In_ PFN_UCX_ROOTHUB_GET_20PORT_INFO EvtRootHubGet20PortInfo,
    _In_ PFN_UCX_ROOTHUB_GET_30PORT_INFO EvtRootHubGet30PortInfo)
{
    RtlZeroMemory(Config, sizeof(*Config));
    Config->Size = sizeof(*Config);
    Config->NumberOfPresentedControlUrbCallbacks = 1;

    Config->EvtRootHubInterruptTx = EvtRootHubInterruptTx;
    Config->EvtRootHubGetInfo = EvtRootHubGetInfo;
    Config->EvtRootHubGet20PortInfo = EvtRootHubGet20PortInfo;
    Config->EvtRootHubGet30PortInfo = EvtRootHubGet30PortInfo;

    WDF_OBJECT_ATTRIBUTES_INIT(&Config->WdfRequestAttributes);
}

/** Root hub config where one callback services every hub class control request. */
FORCEINLINE
VOID
NTAPI
UCX_ROOTHUB_CONFIG_INIT_WITH_CONTROL_URB_HANDLER(
    _Out_ PUCX_ROOTHUB_CONFIG Config,
    _In_ PFN_UCX_ROOTHUB_CONTROL_URB EvtRootHubControlUrb,
    _In_ PFN_UCX_ROOTHUB_INTERRUPT_TX EvtRootHubInterruptTx,
    _In_ PFN_UCX_ROOTHUB_GET_INFO EvtRootHubGetInfo,
    _In_ PFN_UCX_ROOTHUB_GET_20PORT_INFO EvtRootHubGet20PortInfo,
    _In_ PFN_UCX_ROOTHUB_GET_30PORT_INFO EvtRootHubGet30PortInfo)
{
    UcxpRootHubConfigInitCommon(Config,
                                EvtRootHubInterruptTx,
                                EvtRootHubGetInfo,
                                EvtRootHubGet20PortInfo,
                                EvtRootHubGet30PortInfo);

    Config->EvtRootHubControlUrb = EvtRootHubControlUrb;
}

/** Root hub config with a separate callback per hub class request. */
FORCEINLINE
VOID
NTAPI
UCX_ROOTHUB_CONFIG_INIT(
    _Out_ PUCX_ROOTHUB_CONFIG Config,
    _In_ PFN_UCX_ROOTHUB_CONTROL_URB EvtRootHubClearHubFeature,
    _In_ PFN_UCX_ROOTHUB_CONTROL_URB EvtRootHubClearPortFeature,
    _In_ PFN_UCX_ROOTHUB_CONTROL_URB EvtRootHubGetHubStatus,
    _In_ PFN_UCX_ROOTHUB_CONTROL_URB EvtRootHubGetPortStatus,
    _In_ PFN_UCX_ROOTHUB_CONTROL_URB EvtRootHubSetHubFeature,
    _In_ PFN_UCX_ROOTHUB_CONTROL_URB EvtRootHubSetPortFeature,
    _In_ PFN_UCX_ROOTHUB_CONTROL_URB EvtRootHubGetPortErrorCount,
    _In_ PFN_UCX_ROOTHUB_INTERRUPT_TX EvtRootHubInterruptTx,
    _In_ PFN_UCX_ROOTHUB_GET_INFO EvtRootHubGetInfo,
    _In_ PFN_UCX_ROOTHUB_GET_20PORT_INFO EvtRootHubGet20PortInfo,
    _In_ PFN_UCX_ROOTHUB_GET_30PORT_INFO EvtRootHubGet30PortInfo)
{
    UcxpRootHubConfigInitCommon(Config,
                                EvtRootHubInterruptTx,
                                EvtRootHubGetInfo,
                                EvtRootHubGet20PortInfo,
                                EvtRootHubGet30PortInfo);

    Config->EvtRootHubClearHubFeature = EvtRootHubClearHubFeature;
    Config->EvtRootHubClearPortFeature = EvtRootHubClearPortFeature;
    Config->EvtRootHubGetHubStatus = EvtRootHubGetHubStatus;
    Config->EvtRootHubGetPortStatus = EvtRootHubGetPortStatus;
    Config->EvtRootHubSetHubFeature = EvtRootHubSetHubFeature;
    Config->EvtRootHubSetPortFeature = EvtRootHubSetPortFeature;
    Config->EvtRootHubGetPortErrorCount = EvtRootHubGetPortErrorCount;
}

/* PEVT_ spellings from older headers */
typedef PFN_UCX_ROOTHUB_INTERRUPT_TX PEVT_UCX_ROOTHUB_INTERRUPT_TX;
typedef PFN_UCX_ROOTHUB_CONTROL_URB PEVT_UCX_ROOTHUB_CONTROL_URB;
typedef PFN_UCX_ROOTHUB_GET_INFO PEVT_UCX_ROOTHUB_GET_INFO;
typedef PFN_UCX_ROOTHUB_GET_20PORT_INFO PEVT_UCX_ROOTHUB_GET_20PORT_INFO;
typedef PFN_UCX_ROOTHUB_GET_30PORT_INFO PEVT_UCX_ROOTHUB_GET_30PORT_INFO;

/* Class entry points */

typedef
_Must_inspect_result_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
(NTAPI *PFN_UCXROOTHUBCREATE)(
    _In_ PUCX_DRIVER_GLOBALS DriverGlobals,
    _In_ UCXCONTROLLER Controller,
    _In_ PUCX_ROOTHUB_CONFIG Config,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES Attributes,
    _Out_ UCXROOTHUB *RootHub);

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
(NTAPI *PFN_UCXROOTHUBPORTCHANGED)(
    _In_ PUCX_DRIVER_GLOBALS DriverGlobals,
    _In_ UCXROOTHUB UcxRootHub);

_Must_inspect_result_
_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
UcxRootHubCreate(
    _In_ UCXCONTROLLER Controller,
    _In_ PUCX_ROOTHUB_CONFIG Config,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES Attributes,
    _Out_ UCXROOTHUB *RootHub)
{
    PFN_UCXROOTHUBCREATE Create;

    Create = UCX_BOUND_FUNCTION(PFN_UCXROOTHUBCREATE, UcxRootHubCreateTableIndex);
    return Create(UcxDriverGlobals, Controller, Config, Attributes, RootHub);
}

/** Signals a root port status change so UCX completes the pending interrupt transfer. */
_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
VOID
NTAPI
UcxRootHubPortChanged(
    _In_ UCXROOTHUB UcxRootHub)
{
    PFN_UCXROOTHUBPORTCHANGED PortChanged;

    PortChanged = UCX_BOUND_FUNCTION(PFN_UCXROOTHUBPORTCHANGED, UcxRootHubPortChangedTableIndex);
    PortChanged(UcxDriverGlobals, UcxRootHub);
}

WDF_EXTERN_C_END
