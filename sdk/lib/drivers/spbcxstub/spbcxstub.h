/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SpbCx client binding for drivers built in tree
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#include <spbcx.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Entry points handed out by spbcx, indexed by SPBFUNCENUM. */
extern SPBFUNC SpbFunctions[SpbFunctionTableNumEntries];

/** The globals spbcx handed this driver; every entry point takes them first. */
extern PSPB_DRIVER_GLOBALS SpbDriverGlobals;

/**
 * Binds the calling driver to spbcx and fills SpbFunctions and SpbDriverGlobals.
 * Call from DriverEntry after WdfDriverCreate.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
SpbCxStubBindClass(VOID);

/** Drops the binding taken by SpbCxStubBindClass. Safe to call when not bound. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
SpbCxStubUnbindClass(VOID);

FORCEINLINE
NTSTATUS
SpbDeviceInitConfig(
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    return ((PFN_SPBDEVICEINITCONFIG)SpbFunctions[SpbDeviceInitConfigTableIndex])(SpbDriverGlobals,
                                                                                 DeviceInit);
}

FORCEINLINE
NTSTATUS
SpbDeviceInitialize(
    _In_ WDFDEVICE FxDevice,
    _In_ PSPB_CONTROLLER_CONFIG Config)
{
    return ((PFN_SPBDEVICEINITIALIZE)SpbFunctions[SpbDeviceInitializeTableIndex])(SpbDriverGlobals,
                                                                                 FxDevice,
                                                                                 Config);
}

FORCEINLINE
VOID
SpbControllerSetIoOtherCallback(
    _In_ WDFDEVICE FxDevice,
    _In_ PFN_SPB_CONTROLLER_OTHER EvtSpbControllerIoOther,
    _In_opt_ PFN_WDF_IO_IN_CALLER_CONTEXT EvtIoInCallerContext)
{
    ((PFN_SPBCONTROLLERSETIOOTHERCALLBACK)SpbFunctions[SpbControllerSetIoOtherCallbackTableIndex])(
        SpbDriverGlobals,
        FxDevice,
        EvtSpbControllerIoOther,
        EvtIoInCallerContext);
}

FORCEINLINE
VOID
SpbControllerSetRequestAttributes(
    _In_ WDFDEVICE FxDevice,
    _In_ PWDF_OBJECT_ATTRIBUTES RequestAttributes)
{
    ((PFN_SPBCONTROLLERSETREQUESTATTRIBUTES)SpbFunctions[SpbControllerSetRequestAttributesTableIndex])(
        SpbDriverGlobals,
        FxDevice,
        RequestAttributes);
}

FORCEINLINE
VOID
SpbControllerSetTargetAttributes(
    _In_ WDFDEVICE FxDevice,
    _In_ PWDF_OBJECT_ATTRIBUTES ObjectAttributes)
{
    ((PFN_SPBCONTROLLERSETTARGETATTRIBUTES)SpbFunctions[SpbControllerSetTargetAttributesTableIndex])(
        SpbDriverGlobals,
        FxDevice,
        ObjectAttributes);
}

FORCEINLINE
VOID
SpbTargetGetConnectionParameters(
    _In_ SPBTARGET Target,
    _Out_ PSPB_CONNECTION_PARAMETERS ConnectionParameters)
{
    ((PFN_SPBTARGETGETCONNECTIONPARAMETERS)SpbFunctions[SpbTargetGetConnectionParametersTableIndex])(
        SpbDriverGlobals,
        Target,
        ConnectionParameters);
}

FORCEINLINE
WDFFILEOBJECT
SpbTargetGetFileObject(
    _In_ SPBTARGET Target)
{
    return ((PFN_SPBTARGETGETFILEOBJECT)SpbFunctions[SpbTargetGetFileObjectTableIndex])(SpbDriverGlobals,
                                                                                       Target);
}

FORCEINLINE
SPBTARGET
SpbRequestGetTarget(
    _In_ SPBREQUEST SpbRequest)
{
    return ((PFN_SPBREQUESTGETTARGET)SpbFunctions[SpbRequestGetTargetTableIndex])(SpbDriverGlobals,
                                                                                 SpbRequest);
}

FORCEINLINE
WDFDEVICE
SpbRequestGetController(
    _In_ SPBREQUEST SpbRequest)
{
    return ((PFN_SPBREQUESTGETCONTROLLER)SpbFunctions[SpbRequestGetControllerTableIndex])(SpbDriverGlobals,
                                                                                         SpbRequest);
}

FORCEINLINE
VOID
SpbRequestGetParameters(
    _In_ SPBREQUEST SpbRequest,
    _Out_ PSPB_REQUEST_PARAMETERS Parameters)
{
    ((PFN_SPBREQUESTGETPARAMETERS)SpbFunctions[SpbRequestGetParametersTableIndex])(SpbDriverGlobals,
                                                                                  SpbRequest,
                                                                                  Parameters);
}

FORCEINLINE
VOID
SpbRequestGetTransferParameters(
    _In_ SPBREQUEST SpbRequest,
    _In_ ULONG Index,
    _Out_opt_ PSPB_TRANSFER_DESCRIPTOR TransferDescriptor,
    _Out_opt_ PMDL *TransferBuffer)
{
    ((PFN_SPBREQUESTGETTRANSFERPARAMETERS)SpbFunctions[SpbRequestGetTransferParametersTableIndex])(
        SpbDriverGlobals,
        SpbRequest,
        Index,
        TransferDescriptor,
        TransferBuffer);
}

FORCEINLINE
VOID
SpbRequestComplete(
    _In_ SPBREQUEST Request,
    _In_ NTSTATUS CompletionStatus)
{
    ((PFN_SPBREQUESTCOMPLETE)SpbFunctions[SpbRequestCompleteTableIndex])(SpbDriverGlobals,
                                                                        Request,
                                                                        CompletionStatus);
}

FORCEINLINE
NTSTATUS
SpbRequestCaptureIoOtherTransferList(
    _In_ SPBREQUEST Request)
{
    return ((PFN_SPBREQUESTCAPTUREIOOTHERTRANSFERLIST)
                SpbFunctions[SpbRequestCaptureIoOtherTransferListTableIndex])(SpbDriverGlobals, Request);
}

#ifdef __cplusplus
}
#endif
