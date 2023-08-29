/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Dxgkrnl core header
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntddk.h>
#include <stdio.h>
#include <dispmprt.h>


typedef struct _DXGKRNL_PRIVATE_EXTENSION
{
	PDXGKDDI_ADD_DEVICE                     DxgkDdiAddDevice;
    PDXGKDDI_START_DEVICE                   DxgkDdiStartDevice;
    PDXGKDDI_STOP_DEVICE                    DxgkDdiStopDevice;
    PDXGKDDI_REMOVE_DEVICE                  DxgkDdiRemoveDevice;
    PDXGKDDI_DISPATCH_IO_REQUEST            DxgkDdiDispatchIoRequest;
    PDXGKDDI_INTERRUPT_ROUTINE              DxgkDdiInterruptRoutine;
    PDXGKDDI_DPC_ROUTINE                    DxgkDdiDpcRoutine;
    PDXGKDDI_QUERY_CHILD_RELATIONS          DxgkDdiQueryChildRelations;
    PDXGKDDI_QUERY_CHILD_STATUS             DxgkDdiQueryChildStatus;
    PDXGKDDI_QUERY_DEVICE_DESCRIPTOR        DxgkDdiQueryDeviceDescriptor;
    PDXGKDDI_SET_POWER_STATE                DxgkDdiSetPowerState;
    PDXGKDDI_NOTIFY_ACPI_EVENT              DxgkDdiNotifyAcpiEvent;
    PDXGKDDI_RESET_DEVICE                   DxgkDdiResetDevice;
    PDXGKDDI_UNLOAD                         DxgkDdiUnload;
    PDXGKDDI_QUERY_INTERFACE                DxgkDdiQueryInterface;
    PDXGKDDI_CONTROL_ETW_LOGGING            DxgkDdiControlEtwLogging;
    PDXGKDDI_QUERYADAPTERINFO               DxgkDdiQueryAdapterInfo;
    PDXGKDDI_SETPALETTE                     DxgkDdiSetPalette;
    PDXGKDDI_SETPOINTERPOSITION             DxgkDdiSetPointerPosition;
    PDXGKDDI_SETPOINTERSHAPE                DxgkDdiSetPointerShape;
    PDXGKDDI_ESCAPE                         DxgkDdiEscape;
    PDXGKDDI_COLLECTDBGINFO                 DxgkDdiCollectDbgInfo;
    PDXGKDDI_ISSUPPORTEDVIDPN               DxgkDdiIsSupportedVidPn;
    PDXGKDDI_RECOMMENDFUNCTIONALVIDPN       DxgkDdiRecommendFunctionalVidPn;
    PDXGKDDI_ENUMVIDPNCOFUNCMODALITY        DxgkDdiEnumVidPnCofuncModality;
    PDXGKDDI_SETVIDPNSOURCEVISIBILITY       DxgkDdiSetVidPnSourceVisibility;
    PDXGKDDI_COMMITVIDPN                    DxgkDdiCommitVidPn;
    PDXGKDDI_UPDATEACTIVEVIDPNPRESENTPATH   DxgkDdiUpdateActiveVidPnPresentPath;
    PDXGKDDI_RECOMMENDMONITORMODES          DxgkDdiRecommendMonitorModes;
    PDXGKDDI_GETSCANLINE                    DxgkDdiGetScanLine;
    PDXGKDDI_QUERYVIDPNHWCAPABILITY         DxgkDdiQueryVidPnHWCapability;
	DRIVER_INITIALIZATION_DATA DriverInitStdData;
	KMDDOD_INITIALIZATION_DATA DriverInitKmdodData;
	PDRIVER_OBJECT MiniportDriverObject;
    PDRIVER_EXTENSION MiniportDriverExtend;
    KSPIN_LOCK InterruptSpinLock;
    PDEVICE_OBJECT MiniportFdo;
    PDEVICE_OBJECT MiniportPdo;
	INTERFACE_TYPE AdapterInterfaceType;
    PVOID MiniportContext;
	UNICODE_STRING RegistryPath;
    UNICODE_STRING NewRegistryPath;
	PDEVICE_OBJECT NextDeviceObject;
	ULONG SystemIoBusNumber;
	ULONG SystemIoSlotNumber;
    ULONG InternalDeviceNumber;
    BUS_INTERFACE_STANDARD BusInterface;
    ULONG BusInterruptLevel;
    ULONG BusInterruptVector;
    PKDPC DpcObject;
    PKINTERRUPT InterruptObject;
} DXGKRNL_PRIVATE_EXTENSION, *PDXGKRNL_PRIVATE_EXTENSION;

NTSTATUS
APIENTRY
CALLBACK
DxgkCbQueryVidPnInterface(IN_CONST_D3DKMDT_HVIDPN                hVidPn,
                               IN_CONST_DXGK_VIDPN_INTERFACE_VERSION  VidPnInterfaceVersion,
                               DEREF_OUT_CONST_PPDXGK_VIDPN_INTERFACE ppVidPnInterface);

BOOLEAN
APIENTRY
DxgkCbQueueDpc(_In_ HANDLE DeviceHandle);

NTSTATUS
APIENTRY
DxgkCbSynchronizeExecution(_In_ HANDLE DeviceHandle,
                                _In_ PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
                                _In_ PVOID Context,
                                _In_ ULONG MessageNumber,
                                _Out_ PBOOLEAN ReturnValue);
VOID
APIENTRY
CALLBACK
DxgkCbNotifyInterrupt(IN_CONST_HANDLE hAdapter,
                        IN_CONST_PDXGKARGCB_NOTIFY_INTERRUPT_DATA NotifyInterruptData);

VOID
APIENTRY
CALLBACK
DxgkCbNotifyDpc(IN_CONST_HANDLE hAdapter);

BOOLEAN NTAPI
DxgkrnlSetupInterrupt();
#include "dxgkport.h"

#ifdef __cplusplus
#include "dxgkport.hpp"
extern "C"
{
#endif
NTSTATUS
NTAPI
DxgkpSetupDxgkrnl(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath);

NTSTATUS
NTAPI
DxgkPortStartAdapter();

CODE_SEG("PAGE")
NTSTATUS
DxgkpQueryInterface(
    _In_ PDXGKRNL_PRIVATE_EXTENSION DxgkpExtension,
    _In_ const GUID* Guid,
    _Out_ PVOID Interface,
    _In_ ULONG Size);

VOID
NTAPI
RunVidPnMgr();
#ifdef __cplusplus
}
#endif


NTSTATUS
NTAPI
DxgkpCreateIds();

NTSTATUS
NTAPI
IntCreateRegistryPath(
    IN PCUNICODE_STRING DriverRegistryPath,
    IN ULONG DeviceNumber,
    OUT PUNICODE_STRING DeviceRegistryPath);

NTSTATUS
NTAPI
IntCreateNewRegistryPath(
    PDXGKRNL_PRIVATE_EXTENSION DeviceExtension);

NTSTATUS
NTAPI
IntVideoPortAddDeviceMapLink(
    PDXGKRNL_PRIVATE_EXTENSION DeviceExtension);

#include "classes/vidpnmgr.hpp"
//include "../../cdd/cdd_testinterface.h"

#include <reactos/drivers/wddm/wddm_shared.h>