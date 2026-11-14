/*
 * PROJECT:     ReactOS DDK
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Simple Peripheral Bus framework extension, controller driver interface 1.1
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <spb.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
struct SPBTARGET__ : WDFFILEOBJECT__ { };
typedef struct SPBTARGET__ *SPBTARGET;
struct SPBREQUEST__ : WDFREQUEST__ { };
typedef struct SPBREQUEST__ *SPBREQUEST;
#else
DECLARE_HANDLE(SPBTARGET);
DECLARE_HANDLE(SPBREQUEST);
#endif

typedef VOID (*SPBFUNC)(VOID);
typedef struct _WDF_DRIVER_GLOBALS SPB_DRIVER_GLOBALS, *PSPB_DRIVER_GLOBALS;

/* Slots of the table a controller driver binds, in binding order */
typedef enum _SPBFUNCENUM
{
    SpbDeviceInitConfigTableIndex = 0,
    SpbDeviceInitializeTableIndex = 1,
    SpbControllerSetIoOtherCallbackTableIndex = 2,
    SpbControllerSetRequestAttributesTableIndex = 3,
    SpbControllerSetTargetAttributesTableIndex = 4,
    SpbTargetGetConnectionParametersTableIndex = 5,
    SpbTargetGetFileObjectTableIndex = 6,
    SpbRequestGetTargetTableIndex = 7,
    SpbRequestGetControllerTableIndex = 8,
    SpbRequestGetParametersTableIndex = 9,
    SpbRequestGetTransferParametersTableIndex = 10,
    SpbRequestCompleteTableIndex = 11,
    SpbRequestCaptureIoOtherTransferListTableIndex = 12,
    SpbFunctionTableNumEntries = 13
} SPBFUNCENUM;

typedef enum _SPB_REQUEST_TYPE
{
    SpbRequestTypeUndefined = 0,
    SpbRequestTypeRead,
    SpbRequestTypeWrite,
    SpbRequestTypeSequence,
    SpbRequestTypeLockController,
    SpbRequestTypeUnlockController,
    SpbRequestTypeLockConnection,
    SpbRequestTypeUnlockConnection,
    SpbRequestTypeOther,
    SpbRequestTypeMax
} SPB_REQUEST_TYPE, *PSPB_REQUEST_TYPE;

typedef enum _SPB_REQUEST_SEQUENCE_POSITION
{
    SpbRequestSequencePositionInvalid = 0,
    SpbRequestSequencePositionSingle,
    SpbRequestSequencePositionFirst,
    SpbRequestSequencePositionContinue,
    SpbRequestSequencePositionLast,
    SpbRequestSequencePositionMax
} SPB_REQUEST_SEQUENCE_POSITION, *PSPB_REQUEST_SEQUENCE_POSITION;

/* Controller driver callbacks */

typedef
_Function_class_(EVT_SPB_TARGET_CONNECT)
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EVT_SPB_TARGET_CONNECT(
    _In_ WDFDEVICE Controller,
    _In_ SPBTARGET Target);
typedef EVT_SPB_TARGET_CONNECT *PFN_SPB_TARGET_CONNECT;

typedef
_Function_class_(EVT_SPB_TARGET_DISCONNECT)
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_SPB_TARGET_DISCONNECT(
    _In_ WDFDEVICE Controller,
    _In_ SPBTARGET Target);
typedef EVT_SPB_TARGET_DISCONNECT *PFN_SPB_TARGET_DISCONNECT;

typedef
_Function_class_(EVT_SPB_CONTROLLER_LOCK)
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_SPB_CONTROLLER_LOCK(
    _In_ WDFDEVICE Controller,
    _In_ SPBTARGET Target,
    _In_ SPBREQUEST LockRequest);
typedef EVT_SPB_CONTROLLER_LOCK *PFN_SPB_CONTROLLER_LOCK;

typedef
_Function_class_(EVT_SPB_CONTROLLER_UNLOCK)
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_SPB_CONTROLLER_UNLOCK(
    _In_ WDFDEVICE Controller,
    _In_ SPBTARGET Target,
    _In_ SPBREQUEST UnlockRequest);
typedef EVT_SPB_CONTROLLER_UNLOCK *PFN_SPB_CONTROLLER_UNLOCK;

typedef
_Function_class_(EVT_SPB_CONTROLLER_READ)
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_SPB_CONTROLLER_READ(
    _In_ WDFDEVICE Controller,
    _In_ SPBTARGET Target,
    _In_ SPBREQUEST Request,
    _In_ size_t Length);
typedef EVT_SPB_CONTROLLER_READ *PFN_SPB_CONTROLLER_READ;

typedef
_Function_class_(EVT_SPB_CONTROLLER_WRITE)
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_SPB_CONTROLLER_WRITE(
    _In_ WDFDEVICE Controller,
    _In_ SPBTARGET Target,
    _In_ SPBREQUEST Request,
    _In_ size_t Length);
typedef EVT_SPB_CONTROLLER_WRITE *PFN_SPB_CONTROLLER_WRITE;

typedef
_Function_class_(EVT_SPB_CONTROLLER_SEQUENCE)
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_SPB_CONTROLLER_SEQUENCE(
    _In_ WDFDEVICE Controller,
    _In_ SPBTARGET Target,
    _In_ SPBREQUEST Request,
    _In_ ULONG TransferCount);
typedef EVT_SPB_CONTROLLER_SEQUENCE *PFN_SPB_CONTROLLER_SEQUENCE;

typedef
_Function_class_(EVT_SPB_CONTROLLER_OTHER)
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_SPB_CONTROLLER_OTHER(
    _In_ WDFDEVICE Controller,
    _In_ SPBTARGET Target,
    _In_ SPBREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode);
typedef EVT_SPB_CONTROLLER_OTHER *PFN_SPB_CONTROLLER_OTHER;

typedef struct _SPB_CONTROLLER_CONFIG
{
    ULONG Size;
    WDF_IO_QUEUE_DISPATCH_TYPE ControllerDispatchType;
    WDF_TRI_STATE PowerManaged;
    PFN_SPB_TARGET_CONNECT EvtSpbTargetConnect;
    PFN_SPB_TARGET_DISCONNECT EvtSpbTargetDisconnect;
    PFN_SPB_CONTROLLER_LOCK EvtSpbControllerLock;
    PFN_SPB_CONTROLLER_UNLOCK EvtSpbControllerUnlock;
    PFN_SPB_CONTROLLER_READ EvtSpbIoRead;
    PFN_SPB_CONTROLLER_WRITE EvtSpbIoWrite;
    PFN_SPB_CONTROLLER_SEQUENCE EvtSpbIoSequence;
} SPB_CONTROLLER_CONFIG, *PSPB_CONTROLLER_CONFIG;

/**
 * @brief Defaults to a sequential queue whose power handling WDF decides.
 */
FORCEINLINE
VOID
SPB_CONTROLLER_CONFIG_INIT(
    _Out_ PSPB_CONTROLLER_CONFIG Config)
{
    RtlZeroMemory(Config, sizeof(*Config));
    Config->Size = sizeof(*Config);
    Config->ControllerDispatchType = WdfIoQueueDispatchSequential;
    Config->PowerManaged = WdfUseDefault;
}

typedef struct _SPB_CONNECTION_PARAMETERS
{
    USHORT Size;
    PCWSTR ConnectionTag;
    PVOID ConnectionParameters;
} SPB_CONNECTION_PARAMETERS, *PSPB_CONNECTION_PARAMETERS;

FORCEINLINE
VOID
SPB_CONNECTION_PARAMETERS_INIT(
    _Out_ PSPB_CONNECTION_PARAMETERS Parameters)
{
    RtlZeroMemory(Parameters, sizeof(*Parameters));
    Parameters->Size = sizeof(*Parameters);
}

typedef struct _SPB_REQUEST_PARAMETERS
{
    USHORT Size;
    SPB_REQUEST_TYPE Type;
    SPB_REQUEST_SEQUENCE_POSITION Position;
    SPB_TRANSFER_DIRECTION PreviousTransferDirection;
    size_t Length;
    ULONG SequenceTransferCount;
} SPB_REQUEST_PARAMETERS, *PSPB_REQUEST_PARAMETERS;

FORCEINLINE
VOID
SPB_REQUEST_PARAMETERS_INIT(
    _Out_ PSPB_REQUEST_PARAMETERS Parameters)
{
    RtlZeroMemory(Parameters, sizeof(*Parameters));
    Parameters->Size = sizeof(*Parameters);
}

typedef struct SPB_TRANSFER_DESCRIPTOR
{
    USHORT Size;
    SPB_TRANSFER_DIRECTION Direction;
    size_t TransferLength;
    ULONG DelayInUs;
} SPB_TRANSFER_DESCRIPTOR, *PSPB_TRANSFER_DESCRIPTOR;

FORCEINLINE
VOID
SPB_TRANSFER_DESCRIPTOR_INIT(
    _Out_ PSPB_TRANSFER_DESCRIPTOR Descriptor)
{
    RtlZeroMemory(Descriptor, sizeof(*Descriptor));
    Descriptor->Size = sizeof(*Descriptor);
}

/* Framework entry points, each takes the client's driver globals first */

typedef
NTSTATUS
(NTAPI *PFN_SPBDEVICEINITCONFIG)(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _Inout_ PWDFDEVICE_INIT DeviceInit);

typedef
NTSTATUS
(NTAPI *PFN_SPBDEVICEINITIALIZE)(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE FxDevice,
    _In_ PSPB_CONTROLLER_CONFIG Config);

typedef
VOID
(NTAPI *PFN_SPBCONTROLLERSETIOOTHERCALLBACK)(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE FxDevice,
    _In_ PFN_SPB_CONTROLLER_OTHER EvtSpbControllerIoOther,
    _In_opt_ PFN_WDF_IO_IN_CALLER_CONTEXT EvtIoInCallerContext);

typedef
VOID
(NTAPI *PFN_SPBCONTROLLERSETREQUESTATTRIBUTES)(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE FxDevice,
    _In_ PWDF_OBJECT_ATTRIBUTES RequestAttributes);

typedef
VOID
(NTAPI *PFN_SPBCONTROLLERSETTARGETATTRIBUTES)(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE FxDevice,
    _In_ PWDF_OBJECT_ATTRIBUTES ObjectAttributes);

typedef
VOID
(NTAPI *PFN_SPBTARGETGETCONNECTIONPARAMETERS)(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBTARGET Target,
    _Out_ PSPB_CONNECTION_PARAMETERS ConnectionParameters);

typedef
WDFFILEOBJECT
(NTAPI *PFN_SPBTARGETGETFILEOBJECT)(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBTARGET Target);

typedef
SPBTARGET
(NTAPI *PFN_SPBREQUESTGETTARGET)(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest);

typedef
WDFDEVICE
(NTAPI *PFN_SPBREQUESTGETCONTROLLER)(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest);

typedef
VOID
(NTAPI *PFN_SPBREQUESTGETPARAMETERS)(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest,
    _Out_ PSPB_REQUEST_PARAMETERS Parameters);

typedef
VOID
(NTAPI *PFN_SPBREQUESTGETTRANSFERPARAMETERS)(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest,
    _In_ ULONG Index,
    _Out_opt_ PSPB_TRANSFER_DESCRIPTOR TransferDescriptor,
    _Out_opt_ PMDL *TransferBuffer);

typedef
VOID
(NTAPI *PFN_SPBREQUESTCOMPLETE)(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST Request,
    _In_ NTSTATUS CompletionStatus);

typedef
NTSTATUS
(NTAPI *PFN_SPBREQUESTCAPTUREIOOTHERTRANSFERLIST)(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST Request);

#ifdef __cplusplus
}
#endif
