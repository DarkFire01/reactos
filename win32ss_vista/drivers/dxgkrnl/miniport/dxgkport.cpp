/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DxgKrnl miniport initial handling
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 */

#include <dxgkrnl.h>
//#define NDEBUG
#include <debug.h>
PDXGKRNL_PRIVATE_EXTENSION DxgkpExtension;

NTSTATUS
NTAPI
DxgkPortpFinializeInitialization(_In_ PDRIVER_OBJECT DriverObject,
                                 _In_ PUNICODE_STRING SourceString)
{
    PDRIVER_EXTENSION DriverExtend;

    /* Fill out the internal structure - WIP */
    DxgkpExtension->MiniportDriverObject = DriverObject;
    DxgkpExtension->InternalDeviceNumber = 0;

    /* Fill out the public dispatch routines */
    DriverExtend = DriverObject->DriverExtension;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = (PDRIVER_DISPATCH)DxgkPortDispatchCreateDevice;
    DriverObject->MajorFunction[IRP_MJ_PNP] = (PDRIVER_DISPATCH)DxgkPortDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = (PDRIVER_DISPATCH)DxgkPortDispatchPower;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = (PDRIVER_DISPATCH)DxgkPortDispatchIoctl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = (PDRIVER_DISPATCH)DxgkPortDispatchInternalIoctl;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = (PDRIVER_DISPATCH)DxgkPortDispatchSystemControl;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = (PDRIVER_DISPATCH)DxgkPortDispatchCloseDevice;

    DriverExtend->AddDevice = DxgkPortAddDevice;
    DriverObject->DriverUnload = (PDRIVER_UNLOAD)DxgkPortDriverUnload;
    DPRINT("DxgkPortpFinializeInitialization: Finished\n");
    return STATUS_SUCCESS;
}

/**
 * @brief The first real transaction between WDDM Miniport and Dxgkrnl, this internal routine
 * gets called by Displib and passes the callback list into a internal struct
 *
 * @param DriverObject DriverObject of the miniport driver itself
 *
 * @param SourceString The registry path provided by Displib
 *
 * @param DriverInitData The callback list provided by a WDDM miniport driver via displib
 *
 * @return NTSTATUS Standard NT return value
 */
NTSTATUS
NTAPI
DxgkPortInitializeMiniport(_In_ PDRIVER_OBJECT DriverObject,
                           _In_ PUNICODE_STRING SourceString,
                           _In_ PDRIVER_INITIALIZATION_DATA DriverInitData)
{
    NTSTATUS Status;

    PAGED_CODE();
    if (!DriverObject || !SourceString || !DriverInitData)
        return STATUS_INVALID_PARAMETER;

    /* let's make sure we can allocate the private extension */
    Status = IoAllocateDriverObjectExtension(DriverObject, DriverObject, sizeof(DXGKRNL_PRIVATE_EXTENSION), (PVOID*)&DxgkpExtension);
    if (Status != STATUS_SUCCESS)
    {
        DPRINT1("DxgkPortInitializeMiniport: Couldn't allocate object extension status: 0x%X", Status);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Using the list above, assign the DxgkpExtension struct members the driverinitdata members */
    DxgkpExtension->DxgkDdiAddDevice = DriverInitData->DxgkDdiAddDevice;
    DxgkpExtension->DxgkDdiStartDevice = DriverInitData->DxgkDdiStartDevice;
    DxgkpExtension->DxgkDdiStopDevice = DriverInitData->DxgkDdiStopDevice;
    DxgkpExtension->DxgkDdiRemoveDevice = DriverInitData->DxgkDdiRemoveDevice;
    DxgkpExtension->DxgkDdiDispatchIoRequest = DriverInitData->DxgkDdiDispatchIoRequest;
    DxgkpExtension->DxgkDdiInterruptRoutine = DriverInitData->DxgkDdiInterruptRoutine;
    DxgkpExtension->DxgkDdiDpcRoutine = DriverInitData->DxgkDdiDpcRoutine;
    DxgkpExtension->DxgkDdiQueryChildRelations = DriverInitData->DxgkDdiQueryChildRelations;
    DxgkpExtension->DxgkDdiQueryChildStatus = DriverInitData->DxgkDdiQueryChildStatus;
    DxgkpExtension->DxgkDdiQueryDeviceDescriptor = DriverInitData->DxgkDdiQueryDeviceDescriptor;
    DxgkpExtension->DxgkDdiSetPowerState = DriverInitData->DxgkDdiSetPowerState;
    DxgkpExtension->DxgkDdiNotifyAcpiEvent = DriverInitData->DxgkDdiNotifyAcpiEvent;
    DxgkpExtension->DxgkDdiResetDevice = DriverInitData->DxgkDdiResetDevice;
    DxgkpExtension->DxgkDdiUnload = DriverInitData->DxgkDdiUnload;
    DxgkpExtension->DxgkDdiQueryInterface = DriverInitData->DxgkDdiQueryInterface;
    DxgkpExtension->DxgkDdiControlEtwLogging = DriverInitData->DxgkDdiControlEtwLogging;
    DxgkpExtension->DxgkDdiQueryAdapterInfo = DriverInitData->DxgkDdiQueryAdapterInfo;
    DxgkpExtension->DxgkDdiSetPalette = DriverInitData->DxgkDdiSetPalette;
    DxgkpExtension->DxgkDdiSetPointerPosition = DriverInitData->DxgkDdiSetPointerPosition;
    DxgkpExtension->DxgkDdiSetPointerShape = DriverInitData->DxgkDdiSetPointerShape;
    DxgkpExtension->DxgkDdiEscape = DriverInitData->DxgkDdiEscape;
    DxgkpExtension->DxgkDdiCollectDbgInfo = DriverInitData->DxgkDdiCollectDbgInfo;
    DxgkpExtension->DxgkDdiIsSupportedVidPn = DriverInitData->DxgkDdiIsSupportedVidPn;
    DxgkpExtension->DxgkDdiRecommendFunctionalVidPn = DriverInitData->DxgkDdiRecommendFunctionalVidPn;
    DxgkpExtension->DxgkDdiEnumVidPnCofuncModality = DriverInitData->DxgkDdiEnumVidPnCofuncModality;
    DxgkpExtension->DxgkDdiSetVidPnSourceVisibility = DriverInitData->DxgkDdiSetVidPnSourceVisibility;
    DxgkpExtension->DxgkDdiCommitVidPn = DriverInitData->DxgkDdiCommitVidPn;
    DxgkpExtension->DxgkDdiUpdateActiveVidPnPresentPath = DriverInitData->DxgkDdiUpdateActiveVidPnPresentPath;
    DxgkpExtension->DxgkDdiRecommendMonitorModes = DriverInitData->DxgkDdiRecommendMonitorModes;
    DxgkpExtension->DxgkDdiGetScanLine = DriverInitData->DxgkDdiGetScanLine;
    DxgkpExtension->DxgkDdiQueryVidPnHWCapability = DriverInitData->DxgkDdiQueryVidPnHWCapability;

    return DxgkPortpFinializeInitialization(DriverObject, SourceString);
}

/**
 * @brief Similar to Standard varient but handles KMDOD drivers specifically instead.
 *
 * @param DriverObject DriverObject of the miniport driver itself
 *
 * @param SourceString The registry path provided by Displib
 *
 * @param DriverInitData The callback list provided by a KMDOD miniport driver via displib
 *
 * @return NTSTATUS Standard NT return value
 */
NTSTATUS
NTAPI
DxgkPortInitializeDodMiniport(_In_ PDRIVER_OBJECT DriverObject,
                              _In_ PUNICODE_STRING SourceString,
                              _In_ PKMDDOD_INITIALIZATION_DATA DriverInitData)
{
    NTSTATUS Status;

    PAGED_CODE();
    if (!DriverObject || !SourceString || !DriverInitData)
        return STATUS_INVALID_PARAMETER;

    /* let's make sure we can allocate the private extension */
    Status = IoAllocateDriverObjectExtension(DriverObject, DriverObject, sizeof(DXGKRNL_PRIVATE_EXTENSION), (PVOID*)&DxgkpExtension);
    if (Status != STATUS_SUCCESS)
    {
        DPRINT1("DxgkPortInitializeMiniport: Couldn't allocate object extension status: 0x%X", Status);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    DxgkpExtension->DxgkDdiAddDevice = DriverInitData->DxgkDdiAddDevice;
    DxgkpExtension->DxgkDdiStartDevice = DriverInitData->DxgkDdiStartDevice;
    DxgkpExtension->DxgkDdiStopDevice = DriverInitData->DxgkDdiStopDevice;
    DxgkpExtension->DxgkDdiRemoveDevice = DriverInitData->DxgkDdiRemoveDevice;
    DxgkpExtension->DxgkDdiDispatchIoRequest = DriverInitData->DxgkDdiDispatchIoRequest;
    DxgkpExtension->DxgkDdiInterruptRoutine = DriverInitData->DxgkDdiInterruptRoutine;
    DxgkpExtension->DxgkDdiDpcRoutine = DriverInitData->DxgkDdiDpcRoutine;
    DxgkpExtension->DxgkDdiQueryChildRelations = DriverInitData->DxgkDdiQueryChildRelations;
    DxgkpExtension->DxgkDdiQueryChildStatus = DriverInitData->DxgkDdiQueryChildStatus;
    DxgkpExtension->DxgkDdiQueryDeviceDescriptor = DriverInitData->DxgkDdiQueryDeviceDescriptor;
    DxgkpExtension->DxgkDdiSetPowerState = DriverInitData->DxgkDdiSetPowerState;
    DxgkpExtension->DxgkDdiNotifyAcpiEvent = DriverInitData->DxgkDdiNotifyAcpiEvent;
    DxgkpExtension->DxgkDdiResetDevice = DriverInitData->DxgkDdiResetDevice;
    DxgkpExtension->DxgkDdiUnload = DriverInitData->DxgkDdiUnload;
    DxgkpExtension->DxgkDdiQueryInterface = DriverInitData->DxgkDdiQueryInterface;
    DxgkpExtension->DxgkDdiControlEtwLogging = DriverInitData->DxgkDdiControlEtwLogging;
    DxgkpExtension->DxgkDdiQueryAdapterInfo = DriverInitData->DxgkDdiQueryAdapterInfo;
    DxgkpExtension->DxgkDdiSetPalette = DriverInitData->DxgkDdiSetPalette;
    DxgkpExtension->DxgkDdiSetPointerPosition = DriverInitData->DxgkDdiSetPointerPosition;
    DxgkpExtension->DxgkDdiSetPointerShape = DriverInitData->DxgkDdiSetPointerShape;
    DxgkpExtension->DxgkDdiEscape = DriverInitData->DxgkDdiEscape;
    DxgkpExtension->DxgkDdiCollectDbgInfo = DriverInitData->DxgkDdiCollectDbgInfo;
    DxgkpExtension->DxgkDdiIsSupportedVidPn = DriverInitData->DxgkDdiIsSupportedVidPn;
    DxgkpExtension->DxgkDdiRecommendFunctionalVidPn = DriverInitData->DxgkDdiRecommendFunctionalVidPn;
    DxgkpExtension->DxgkDdiEnumVidPnCofuncModality = DriverInitData->DxgkDdiEnumVidPnCofuncModality;
    DxgkpExtension->DxgkDdiSetVidPnSourceVisibility = DriverInitData->DxgkDdiSetVidPnSourceVisibility;
    DxgkpExtension->DxgkDdiCommitVidPn = DriverInitData->DxgkDdiCommitVidPn;
    DxgkpExtension->DxgkDdiUpdateActiveVidPnPresentPath = DriverInitData->DxgkDdiUpdateActiveVidPnPresentPath;
    DxgkpExtension->DxgkDdiRecommendMonitorModes = DriverInitData->DxgkDdiRecommendMonitorModes;
    DxgkpExtension->DxgkDdiGetScanLine = DriverInitData->DxgkDdiGetScanLine;
    DxgkpExtension->DxgkDdiQueryVidPnHWCapability = DriverInitData->DxgkDdiQueryVidPnHWCapability;

    return DxgkPortpFinializeInitialization(DriverObject, SourceString);
}
