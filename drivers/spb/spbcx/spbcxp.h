/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SpbCx private object model
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * A peripheral opens \Device\RESOURCE_HUB\<id>, the hub reparses the open to
 * the controller, and SpbCx turns the id into a target and the <spb.h> IOCTLs
 * into the typed callbacks in <spbcx.h>.
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <wdfcx.h>
#include <wdfldr.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <spb.h>
#include <spbcx.h>
#include <debug.h>

#define SCX_POOL_TAG 'xbpS'

#define SCX_CLIENT_GLOBALS_SIGNATURE 'XbpS'

/* The client is handed &ClientGlobals; the signature validates it on the way back */
typedef struct _SCX_CLIENT_GLOBALS
{
    ULONG Signature;
    WDF_CLASS_VERSION Version;
    WDF_DRIVER_GLOBALS ClientGlobals;
} SCX_CLIENT_GLOBALS, *PSCX_CLIENT_GLOBALS;

/* One SPB controller, a context on the client's WDFDEVICE */
typedef struct _SCX_CONTROLLER
{
    WDFDEVICE Device;
    SPB_CONTROLLER_CONFIG Config;

    PFN_SPB_CONTROLLER_OTHER EvtSpbControllerIoOther;
    PFN_WDF_IO_IN_CALLER_CONTEXT EvtIoInCallerContext;

    /* Client contexts added to every SPBREQUEST and SPBTARGET */
    WDF_OBJECT_ATTRIBUTES RequestAttributes;
    WDF_OBJECT_ATTRIBUTES TargetAttributes;
    BOOLEAN RequestAttributesSet;
    BOOLEAN TargetAttributesSet;

    WDFQUEUE ControllerQueue;
} SCX_CONTROLLER, *PSCX_CONTROLLER;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SCX_CONTROLLER, ScxGetControllerContext)

/* One peripheral connection, a context on the WDFFILEOBJECT it opened */
typedef struct _SCX_TARGET
{
    PSCX_CONTROLLER Controller;
    WDFFILEOBJECT FileObject;
    LARGE_INTEGER ConnectionId;

    /* The hub's RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER, header included */
    PVOID ConnectionProperties;
    ULONG PropertiesLength;
} SCX_TARGET, *PSCX_TARGET;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SCX_TARGET, ScxGetTargetContext)

/* One request, a context on every WDFREQUEST that reaches the controller */
typedef struct _SCX_REQUEST
{
    PSCX_CONTROLLER Controller;
    PSCX_TARGET Target;
    WDFREQUEST FxRequest;

    SPB_REQUEST_PARAMETERS Parameters;

    /* A captured transfer list, its buffers locked and described by MDLs */
    PSPB_TRANSFER_LIST TransferList;
    ULONG TransferCount;
    PMDL *TransferMdls;

    /* A read or write is presented as one transfer over the request buffer */
    PMDL BufferMdl;
    SPB_TRANSFER_DIRECTION BufferDirection;
} SCX_REQUEST, *PSCX_REQUEST;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SCX_REQUEST, ScxGetRequestContext)

/* spbcx.c */

PSCX_CLIENT_GLOBALS
NTAPI
ScxGlobalsFromClient(
    _In_opt_ PSPB_DRIVER_GLOBALS ClientGlobals);

/* controller.c */

NTSTATUS
NTAPI
ScxDeviceInitConfig(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _Inout_ PWDFDEVICE_INIT DeviceInit);

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
    _In_ PFN_SPB_CONTROLLER_OTHER EvtSpbControllerIoOther,
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

EVT_WDFCX_DEVICE_FILE_CREATE ScxEvtDeviceFileCreate;
EVT_WDF_FILE_CLOSE ScxEvtFileClose;
EVT_WDF_FILE_CLEANUP ScxEvtFileCleanup;
EVT_WDFCXDEVICE_WDM_IRP_PREPROCESS ScxEvtWdmIrpPreprocessCreate;

VOID
NTAPI
ScxTargetGetConnectionParameters(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBTARGET SpbTarget,
    _Inout_ SPB_CONNECTION_PARAMETERS *ConnectionParameters);

WDFFILEOBJECT
NTAPI
ScxTargetGetFileObject(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBTARGET SpbTarget);

/* request.c */

SPBTARGET
NTAPI
ScxRequestGetTarget(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest);

WDFDEVICE
NTAPI
ScxRequestGetController(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest);

VOID
NTAPI
ScxRequestGetParameters(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest,
    _Inout_ SPB_REQUEST_PARAMETERS *Parameters);

VOID
NTAPI
ScxRequestGetTransferParameters(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest,
    _In_ ULONG Index,
    _Inout_opt_ SPB_TRANSFER_DESCRIPTOR *TransferDescriptor,
    _Out_opt_ PMDL *TransferBuffer);

VOID
NTAPI
ScxRequestComplete(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest,
    _In_ NTSTATUS CompletionStatus);

NTSTATUS
NTAPI
ScxRequestCaptureIoOtherTransferList(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest);

/* capture.c */

EVT_WDF_IO_IN_CALLER_CONTEXT ScxEvtIoInCallerContext;
EVT_WDF_OBJECT_CONTEXT_CLEANUP ScxEvtRequestCleanup;

NTSTATUS
NTAPI
ScxCaptureRequestTransferList(
    _In_ PSCX_REQUEST Request);

VOID
NTAPI
ScxReleaseTransfers(
    _Inout_ PSCX_REQUEST Request);
