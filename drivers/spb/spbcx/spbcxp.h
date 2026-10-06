/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SpbCx private object model
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * SpbCx is the class extension an I2C, SPI or UART controller driver binds to.
 * The controller driver owns the hardware; SpbCx owns everything above it: the
 * target a peripheral opens, the request queue, and turning the IOCTLs in
 * <spb.h> into the typed callbacks in <spbcx.h>.
 *
 * The path a transfer takes:
 *
 *   peripheral opens \Device\RESOURCE_HUB\<id>
 *     -> acpiex reparses to the controller (drivers/bus/acpiex)
 *     -> the create lands on us; we parse the id back out of the file name and
 *        ask the hub what connection it describes
 *     -> EvtSpbTargetConnect tells the controller driver its slave address
 *     -> IOCTL_SPB_EXECUTE_SEQUENCE and friends become EvtSpbIoSequence etc.
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <wdfcx.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <spb.h>
#include <spbcx.h>
#include "cxwrap.h"
#include <debug.h>

#define SCX_POOL_TAG 'xbpS'   /* "Spbx" */

/*
 * Handed to the client as its SPB_DRIVER_GLOBALS. The block is 72 bytes and the
 * client gets a pointer sixteen bytes in, so every entry point recovers the real
 * block by subtracting that again, which is also how it validates that a
 * caller's globals are genuinely ours.
 */
#define SCX_CLIENT_GLOBALS_SIGNATURE 'XbpS'   /* "SpbX" */
#define SCX_CLIENT_GLOBALS_OFFSET    16

typedef struct _SCX_CLIENT_GLOBALS
{
    ULONG Signature;
    WDF_CLASS_VERSION Version;
    UCHAR Reserved[SCX_CLIENT_GLOBALS_OFFSET - sizeof(ULONG)];

    /* What the client actually sees; its address is the value we hand back */
    WDF_DRIVER_GLOBALS ClientGlobals;
} SCX_CLIENT_GLOBALS, *PSCX_CLIENT_GLOBALS;

/*
 * One SPB controller. Lives as a context on the client's WDFDEVICE, which is why
 * SpbDeviceInitialize refuses a device that already has one.
 */
typedef struct _SCX_CONTROLLER
{
    WDFDEVICE Device;

    /* What the controller driver asked for in SpbDeviceInitialize */
    SPB_CONTROLLER_CONFIG Config;

    /* Set separately by SpbControllerSetIoOtherCallback */
    PFN_SPB_CONTROLLER_OTHER EvtSpbControllerIoOther;
    PFN_WDF_IO_IN_CALLER_CONTEXT EvtIoInCallerContext;

    /* Applied to every SPBREQUEST and SPBTARGET we create for this controller */
    WDF_OBJECT_ATTRIBUTES RequestAttributes;
    WDF_OBJECT_ATTRIBUTES TargetAttributes;
    BOOLEAN RequestAttributesSet;
    BOOLEAN TargetAttributesSet;

    /*
     * Every request the controller is serialized through. A second queue per
     * target drains into this one, so a target being torn down does not have to
     * wait on another target's transfer.
     */
    WDFQUEUE ControllerQueue;
} SCX_CONTROLLER, *PSCX_CONTROLLER;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SCX_CONTROLLER, ScxGetControllerContext)

/*
 * One peripheral's connection to the controller. Created on IRP_MJ_CREATE from
 * the resource-hub file name, and handed to the client as an SPBTARGET.
 */
typedef struct _SCX_TARGET
{
    PSCX_CONTROLLER Controller;
    WDFFILEOBJECT FileObject;
    LARGE_INTEGER ConnectionId;

    /* The hub.s whole answer: length header at +4, firmware descriptor at +8 */
    PVOID ConnectionProperties;
    ULONG PropertiesLength;

    /* Requests aimed at this target before the controller picks them up */
    WDFQUEUE TargetQueue;
} SCX_TARGET, *PSCX_TARGET;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SCX_TARGET, ScxGetTargetContext)

/* One captured SPB request; handed to the client as an SPBREQUEST. */
typedef struct _SCX_REQUEST
{
    PSCX_CONTROLLER Controller;
    PSCX_TARGET Target;
    WDFREQUEST FxRequest;

    SPB_REQUEST_PARAMETERS Parameters;

    /*
     * The transfer list, captured out of the caller's buffer in caller context
     * and locked down. A sequence's entries are described one at a time through
     * SpbRequestGetTransferParameters.
     */
    PSPB_TRANSFER_LIST TransferList;
    ULONG TransferCount;
    PMDL *TransferMdls;
} SCX_REQUEST, *PSCX_REQUEST;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SCX_REQUEST, ScxGetRequestContext)

/* spbcx.c */

PSCX_CLIENT_GLOBALS
ScxGlobalsFromClient(
    _In_ PSPB_DRIVER_GLOBALS ClientGlobals);

/* controller.c */

NTSTATUS
NTAPI
ScxDeviceInitConfig(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ PWDFDEVICE_INIT DeviceInit);

NTSTATUS
NTAPI
ScxDeviceInitialize(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE FxDevice,
    _In_ PSPB_CONTROLLER_CONFIG Config);

VOID
NTAPI
ScxControllerSetIoOtherCallback(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE FxDevice,
    _In_opt_ PFN_SPB_CONTROLLER_OTHER EvtSpbControllerIoOther,
    _In_opt_ PFN_WDF_IO_IN_CALLER_CONTEXT EvtIoInCallerContext);

VOID
NTAPI
ScxControllerSetRequestAttributes(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE FxDevice,
    _In_ PWDF_OBJECT_ATTRIBUTES RequestAttributes);

VOID
NTAPI
ScxControllerSetTargetAttributes(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE FxDevice,
    _In_ PWDF_OBJECT_ATTRIBUTES ObjectAttributes);

/* target.c */

BOOLEAN  NTAPI ScxEvtDeviceFileCreate(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request,
                                     _In_opt_ WDFFILEOBJECT FileObject);
VOID     NTAPI ScxEvtFileClose(_In_ WDFFILEOBJECT FileObject);
VOID     NTAPI ScxEvtFileCleanup(_In_ WDFFILEOBJECT FileObject);
NTSTATUS NTAPI ScxEvtWdmIrpPreprocessCreate(_In_ WDFDEVICE Device, _Inout_ PIRP Irp,
                                            _In_ PVOID DispatchContext);

VOID NTAPI ScxTargetGetConnectionParameters(_In_ PSPB_DRIVER_GLOBALS DriverGlobals,
                                            _In_ SPBTARGET SpbTarget,
                                            _Out_ SPB_CONNECTION_PARAMETERS *ConnectionParameters);
WDFFILEOBJECT NTAPI ScxTargetGetFileObject(_In_ PSPB_DRIVER_GLOBALS DriverGlobals,
                                           _In_ SPBTARGET SpbTarget);

/* request.c */

SPBTARGET NTAPI ScxRequestGetTarget(_In_ PSPB_DRIVER_GLOBALS DriverGlobals,
                                    _In_ SPBREQUEST SpbRequest);
WDFDEVICE NTAPI ScxRequestGetController(_In_ PSPB_DRIVER_GLOBALS DriverGlobals,
                                        _In_ SPBREQUEST SpbRequest);
VOID NTAPI ScxRequestGetParameters(_In_ PSPB_DRIVER_GLOBALS DriverGlobals,
                                   _In_ SPBREQUEST SpbRequest,
                                   _Out_ SPB_REQUEST_PARAMETERS *Parameters);
VOID NTAPI ScxRequestGetTransferParameters(_In_ PSPB_DRIVER_GLOBALS DriverGlobals,
                                           _In_ SPBREQUEST SpbRequest, _In_ ULONG Index,
                                           _Out_opt_ SPB_TRANSFER_DESCRIPTOR *TransferDescriptor,
                                           _Out_opt_ PMDL *TransferBuffer);
VOID NTAPI ScxRequestComplete(_In_ PSPB_DRIVER_GLOBALS DriverGlobals,
                              _In_ SPBREQUEST SpbRequest, _In_ NTSTATUS CompletionStatus);
NTSTATUS NTAPI ScxRequestCaptureIoOtherTransferList(_In_ PSPB_DRIVER_GLOBALS DriverGlobals,
                                                    _In_ SPBREQUEST SpbRequest);

/* capture.c */

EVT_WDF_IO_IN_CALLER_CONTEXT ScxEvtIoInCallerContext;
EVT_WDF_OBJECT_CONTEXT_CLEANUP ScxEvtRequestCleanup;
VOID ScxReleaseTransfers(_In_ PSCX_REQUEST Request);
