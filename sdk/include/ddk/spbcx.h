/*
 * spbcx.h
 *
 * Simple Peripheral Bus WDF class extension: types and exported methods.
 *
 * This file is part of the ReactOS DDK package.
 *
 * Contributors:
 *   Created by Justin Miller <justinmiller100@gmail.com>
 *
 * THIS SOFTWARE IS NOT COPYRIGHTED
 *
 * This source code is offered for use in the public domain. You may
 * use, modify or distribute it freely.
 *
 * This code is distributed in the hope that it will be useful but
 * WITHOUT ANY WARRANTY. ALL WARRANTIES, EXPRESS OR IMPLIED ARE HEREBY
 * DISCLAIMED. This includes but is not limited to warranties of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 */

#ifndef _SPBCX_H_
#define _SPBCX_H_

#ifndef WDFAPI
#error Include WDF.H first
#endif

#include <spb.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SpbCx is the class extension an I2C/SPI/UART controller driver binds to. It
 * owns the target and request objects, parses the IOCTLs in <spb.h> into typed
 * transfers, serializes them onto the controller, and calls the driver back
 * through SPB_CONTROLLER_CONFIG for the parts only the hardware knows.
 *
 * Every API below dispatches through SpbFunctions[], the table the class
 * extension fills at bind time, and the driver links no SpbCx import library.
 */

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
extern SPBFUNC SpbFunctions[];

typedef struct _WDF_DRIVER_GLOBALS SPB_DRIVER_GLOBALS, *PSPB_DRIVER_GLOBALS;
typedef struct WDFDEVICE_INIT WDFDEVICE_INIT;

#include <spbfuncenum.h>

typedef enum _SPB_REQUEST_TYPE {
  SpbRequestTypeUndefined = 0,
  SpbRequestTypeRead,
  SpbRequestTypeWrite,
  SpbRequestTypeSequence,
  SpbRequestTypeLockController,
  SpbRequestTypeUnlockController,
  SpbRequestTypeLockConnection,
  SpbRequestTypeUnlockConnection,
  SpbRequestTypeOther,
  SpbRequestTypeMax,
} SPB_REQUEST_TYPE, *PSPB_REQUEST_TYPE;

typedef enum _SPB_REQUEST_SEQUENCE_POSITION {
  SpbRequestSequencePositionInvalid = 0,
  SpbRequestSequencePositionSingle,
  SpbRequestSequencePositionFirst,
  SpbRequestSequencePositionContinue,
  SpbRequestSequencePositionLast,
  SpbRequestSequencePositionMax,
} SPB_REQUEST_SEQUENCE_POSITION, *PSPB_REQUEST_SEQUENCE_POSITION;

/* Controller callbacks ******************************************************/

typedef NTSTATUS NTAPI
EVT_SPB_TARGET_CONNECT(
  _In_ WDFDEVICE Controller,
  _In_ SPBTARGET Target);
typedef EVT_SPB_TARGET_CONNECT *PFN_SPB_TARGET_CONNECT;

typedef VOID NTAPI
EVT_SPB_TARGET_DISCONNECT(
  _In_ WDFDEVICE Controller,
  _In_ SPBTARGET Target);
typedef EVT_SPB_TARGET_DISCONNECT *PFN_SPB_TARGET_DISCONNECT;

typedef VOID NTAPI
EVT_SPB_CONTROLLER_LOCK(
  _In_ WDFDEVICE Controller,
  _In_ SPBTARGET Target,
  _In_ SPBREQUEST LockRequest);
typedef EVT_SPB_CONTROLLER_LOCK *PFN_SPB_CONTROLLER_LOCK;

typedef VOID NTAPI
EVT_SPB_CONTROLLER_UNLOCK(
  _In_ WDFDEVICE Controller,
  _In_ SPBTARGET Target,
  _In_ SPBREQUEST UnlockRequest);
typedef EVT_SPB_CONTROLLER_UNLOCK *PFN_SPB_CONTROLLER_UNLOCK;

typedef VOID NTAPI
EVT_SPB_CONTROLLER_READ(
  _In_ WDFDEVICE Controller,
  _In_ SPBTARGET Target,
  _In_ SPBREQUEST Request,
  _In_ size_t Length);
typedef EVT_SPB_CONTROLLER_READ *PFN_SPB_CONTROLLER_READ;

typedef VOID NTAPI
EVT_SPB_CONTROLLER_WRITE(
  _In_ WDFDEVICE Controller,
  _In_ SPBTARGET Target,
  _In_ SPBREQUEST Request,
  _In_ size_t Length);
typedef EVT_SPB_CONTROLLER_WRITE *PFN_SPB_CONTROLLER_WRITE;

typedef VOID NTAPI
EVT_SPB_CONTROLLER_SEQUENCE(
  _In_ WDFDEVICE Controller,
  _In_ SPBTARGET Target,
  _In_ SPBREQUEST Request,
  _In_ ULONG TransferCount);
typedef EVT_SPB_CONTROLLER_SEQUENCE *PFN_SPB_CONTROLLER_SEQUENCE;

typedef VOID NTAPI
EVT_SPB_CONTROLLER_OTHER(
  _In_ WDFDEVICE Controller,
  _In_ SPBTARGET Target,
  _In_ SPBREQUEST Request,
  _In_ size_t OutputBufferLength,
  _In_ size_t InputBufferLength,
  _In_ ULONG IoControlCode);
typedef EVT_SPB_CONTROLLER_OTHER *PFN_SPB_CONTROLLER_OTHER;

/* Configuration and parameter blocks ****************************************/

typedef struct _SPB_CONTROLLER_CONFIG {
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

FORCEINLINE VOID
SPB_CONTROLLER_CONFIG_INIT(
  _Out_ SPB_CONTROLLER_CONFIG *Config)
{
  RtlZeroMemory(Config, sizeof(SPB_CONTROLLER_CONFIG));
  Config->Size = sizeof(SPB_CONTROLLER_CONFIG);
  Config->ControllerDispatchType = WdfIoQueueDispatchSequential;
  Config->PowerManaged = WdfUseDefault;
}

typedef struct _SPB_CONNECTION_PARAMETERS {
  USHORT Size;
  PCWSTR ConnectionTag;
  PVOID ConnectionParameters;
} SPB_CONNECTION_PARAMETERS, *PSPB_CONNECTION_PARAMETERS;

FORCEINLINE VOID
SPB_CONNECTION_PARAMETERS_INIT(
  _Out_ SPB_CONNECTION_PARAMETERS *Parameters)
{
  RtlZeroMemory(Parameters, sizeof(*Parameters));
  Parameters->Size = sizeof(*Parameters);
}

typedef struct _SPB_REQUEST_PARAMETERS {
  USHORT Size;
  SPB_REQUEST_TYPE Type;
  SPB_REQUEST_SEQUENCE_POSITION Position;
  SPB_TRANSFER_DIRECTION PreviousTransferDirection;
  size_t Length;
  ULONG SequenceTransferCount;
} SPB_REQUEST_PARAMETERS, *PSPB_REQUEST_PARAMETERS;

FORCEINLINE VOID
SPB_REQUEST_PARAMETERS_INIT(
  _Out_ SPB_REQUEST_PARAMETERS *Parameters)
{
  RtlZeroMemory(Parameters, sizeof(*Parameters));
  Parameters->Size = sizeof(*Parameters);
}

typedef struct SPB_TRANSFER_DESCRIPTOR {
  USHORT Size;
  SPB_TRANSFER_DIRECTION Direction;
  size_t TransferLength;
  ULONG DelayInUs;
} SPB_TRANSFER_DESCRIPTOR, *PSPB_TRANSFER_DESCRIPTOR;

FORCEINLINE VOID
SPB_TRANSFER_DESCRIPTOR_INIT(
  _Out_ SPB_TRANSFER_DESCRIPTOR *Descriptor)
{
  RtlZeroMemory(Descriptor, sizeof(*Descriptor));
  Descriptor->Size = sizeof(*Descriptor);
}

/* Exported methods **********************************************************/

typedef WDFAPI NTSTATUS
(NTAPI *PFN_SPBDEVICEINITCONFIG)(
  _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
  _In_ WDFDEVICE_INIT *DeviceInit);

FORCEINLINE NTSTATUS
SpbDeviceInitConfig(
  _In_ WDFDEVICE_INIT *DeviceInit)
{
  return ((PFN_SPBDEVICEINITCONFIG)SpbFunctions[SpbDeviceInitConfigTableIndex])(
      SpbDriverGlobals, DeviceInit);
}

typedef WDFAPI NTSTATUS
(NTAPI *PFN_SPBDEVICEINITIALIZE)(
  _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
  _In_ WDFDEVICE FxDevice,
  _In_ PSPB_CONTROLLER_CONFIG Config);

FORCEINLINE NTSTATUS
SpbDeviceInitialize(
  _In_ WDFDEVICE FxDevice,
  _In_ PSPB_CONTROLLER_CONFIG Config)
{
  return ((PFN_SPBDEVICEINITIALIZE)SpbFunctions[SpbDeviceInitializeTableIndex])(
      SpbDriverGlobals, FxDevice, Config);
}

typedef WDFAPI VOID
(NTAPI *PFN_SPBCONTROLLERSETIOOTHERCALLBACK)(
  _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
  _In_ WDFDEVICE FxDevice,
  _In_opt_ PFN_SPB_CONTROLLER_OTHER EvtSpbControllerIoOther,
  _In_opt_ PFN_WDF_IO_IN_CALLER_CONTEXT EvtIoInCallerContext);

FORCEINLINE VOID
SpbControllerSetIoOtherCallback(
  _In_ WDFDEVICE FxDevice,
  _In_opt_ PFN_SPB_CONTROLLER_OTHER EvtSpbControllerIoOther,
  _In_opt_ PFN_WDF_IO_IN_CALLER_CONTEXT EvtIoInCallerContext)
{
  ((PFN_SPBCONTROLLERSETIOOTHERCALLBACK)
       SpbFunctions[SpbControllerSetIoOtherCallbackTableIndex])(
      SpbDriverGlobals, FxDevice, EvtSpbControllerIoOther, EvtIoInCallerContext);
}

typedef WDFAPI VOID
(NTAPI *PFN_SPBCONTROLLERSETREQUESTATTRIBUTES)(
  _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
  _In_ WDFDEVICE FxDevice,
  _In_ PWDF_OBJECT_ATTRIBUTES RequestAttributes);

FORCEINLINE VOID
SpbControllerSetRequestAttributes(
  _In_ WDFDEVICE FxDevice,
  _In_ PWDF_OBJECT_ATTRIBUTES RequestAttributes)
{
  ((PFN_SPBCONTROLLERSETREQUESTATTRIBUTES)
       SpbFunctions[SpbControllerSetRequestAttributesTableIndex])(
      SpbDriverGlobals, FxDevice, RequestAttributes);
}

typedef WDFAPI VOID
(NTAPI *PFN_SPBCONTROLLERSETTARGETATTRIBUTES)(
  _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
  _In_ WDFDEVICE FxDevice,
  _In_ PWDF_OBJECT_ATTRIBUTES ObjectAttributes);

FORCEINLINE VOID
SpbControllerSetTargetAttributes(
  _In_ WDFDEVICE FxDevice,
  _In_ PWDF_OBJECT_ATTRIBUTES ObjectAttributes)
{
  ((PFN_SPBCONTROLLERSETTARGETATTRIBUTES)
       SpbFunctions[SpbControllerSetTargetAttributesTableIndex])(
      SpbDriverGlobals, FxDevice, ObjectAttributes);
}

typedef WDFAPI VOID
(NTAPI *PFN_SPBTARGETGETCONNECTIONPARAMETERS)(
  _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
  _In_ SPBTARGET Target,
  _Out_ SPB_CONNECTION_PARAMETERS *ConnectionParameters);

FORCEINLINE VOID
SpbTargetGetConnectionParameters(
  _In_ SPBTARGET Target,
  _Out_ SPB_CONNECTION_PARAMETERS *ConnectionParameters)
{
  ((PFN_SPBTARGETGETCONNECTIONPARAMETERS)
       SpbFunctions[SpbTargetGetConnectionParametersTableIndex])(
      SpbDriverGlobals, Target, ConnectionParameters);
}

typedef WDFAPI WDFFILEOBJECT
(NTAPI *PFN_SPBTARGETGETFILEOBJECT)(
  _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
  _In_ SPBTARGET Target);

FORCEINLINE WDFFILEOBJECT
SpbTargetGetFileObject(
  _In_ SPBTARGET Target)
{
  return ((PFN_SPBTARGETGETFILEOBJECT)
              SpbFunctions[SpbTargetGetFileObjectTableIndex])(
      SpbDriverGlobals, Target);
}

typedef WDFAPI SPBTARGET
(NTAPI *PFN_SPBREQUESTGETTARGET)(
  _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
  _In_ SPBREQUEST SpbRequest);

FORCEINLINE SPBTARGET
SpbRequestGetTarget(
  _In_ SPBREQUEST SpbRequest)
{
  return ((PFN_SPBREQUESTGETTARGET)SpbFunctions[SpbRequestGetTargetTableIndex])(
      SpbDriverGlobals, SpbRequest);
}

typedef WDFAPI WDFDEVICE
(NTAPI *PFN_SPBREQUESTGETCONTROLLER)(
  _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
  _In_ SPBREQUEST SpbRequest);

FORCEINLINE WDFDEVICE
SpbRequestGetController(
  _In_ SPBREQUEST SpbRequest)
{
  return ((PFN_SPBREQUESTGETCONTROLLER)
              SpbFunctions[SpbRequestGetControllerTableIndex])(
      SpbDriverGlobals, SpbRequest);
}

typedef WDFAPI VOID
(NTAPI *PFN_SPBREQUESTGETPARAMETERS)(
  _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
  _In_ SPBREQUEST SpbRequest,
  _Out_ SPB_REQUEST_PARAMETERS *Parameters);

FORCEINLINE VOID
SpbRequestGetParameters(
  _In_ SPBREQUEST SpbRequest,
  _Out_ SPB_REQUEST_PARAMETERS *Parameters)
{
  ((PFN_SPBREQUESTGETPARAMETERS)SpbFunctions[SpbRequestGetParametersTableIndex])(
      SpbDriverGlobals, SpbRequest, Parameters);
}

typedef WDFAPI VOID
(NTAPI *PFN_SPBREQUESTGETTRANSFERPARAMETERS)(
  _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
  _In_ SPBREQUEST SpbRequest,
  _In_ ULONG Index,
  _Out_opt_ SPB_TRANSFER_DESCRIPTOR *TransferDescriptor,
  _Out_opt_ PMDL *TransferBuffer);

FORCEINLINE VOID
SpbRequestGetTransferParameters(
  _In_ SPBREQUEST SpbRequest,
  _In_ ULONG Index,
  _Out_opt_ SPB_TRANSFER_DESCRIPTOR *TransferDescriptor,
  _Out_opt_ PMDL *TransferBuffer)
{
  ((PFN_SPBREQUESTGETTRANSFERPARAMETERS)
       SpbFunctions[SpbRequestGetTransferParametersTableIndex])(
      SpbDriverGlobals, SpbRequest, Index, TransferDescriptor, TransferBuffer);
}

typedef WDFAPI VOID
(NTAPI *PFN_SPBREQUESTCOMPLETE)(
  _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
  _In_ SPBREQUEST Request,
  _In_ NTSTATUS CompletionStatus);

FORCEINLINE VOID
SpbRequestComplete(
  _In_ SPBREQUEST Request,
  _In_ NTSTATUS CompletionStatus)
{
  ((PFN_SPBREQUESTCOMPLETE)SpbFunctions[SpbRequestCompleteTableIndex])(
      SpbDriverGlobals, Request, CompletionStatus);
}

typedef WDFAPI NTSTATUS
(NTAPI *PFN_SPBREQUESTCAPTUREIOOTHERTRANSFERLIST)(
  _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
  _In_ SPBREQUEST Request);

FORCEINLINE NTSTATUS
SpbRequestCaptureIoOtherTransferList(
  _In_ SPBREQUEST Request)
{
  return ((PFN_SPBREQUESTCAPTUREIOOTHERTRANSFERLIST)
              SpbFunctions[SpbRequestCaptureIoOtherTransferListTableIndex])(
      SpbDriverGlobals, Request);
}

#ifdef __cplusplus
}
#endif

#endif /* _SPBCX_H_ */
