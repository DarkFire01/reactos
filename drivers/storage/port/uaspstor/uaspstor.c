/*
 * PROJECT:     ReactOS USB Attached SCSI Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Storport miniport entry points
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "uaspstor.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

/**
 * @brief Describes the device to the port driver.
 *
 * A virtual miniport is handed the device objects of the stack it sits in,
 * because it has no resources to be told about.
 */
ULONG
NTAPI
UaspFindAdapter(
    _In_ PVOID DeviceExtension,
    _In_ PVOID HwContext,
    _In_ PVOID BusInformation,
    _In_ PVOID LowerDevice,
    _In_ PCHAR ArgumentString,
    _Inout_ PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    _Out_ PBOOLEAN Again)
{
    PUASP_ADAPTER_EXTENSION Adapter = DeviceExtension;

    UNREFERENCED_PARAMETER(ArgumentString);

    DPRINT1("UaspFindAdapter(%p)\n", DeviceExtension);

    RtlZeroMemory(Adapter, sizeof(*Adapter));

    Adapter->PhysicalDeviceObject = HwContext;
    Adapter->DeviceObject = BusInformation;
    Adapter->LowerDeviceObject = LowerDevice;

    if (Adapter->DeviceObject == NULL || Adapter->LowerDeviceObject == NULL)
    {
        DPRINT1("The port driver gave us no device stack to work with\n");
        return SP_RETURN_ERROR;
    }

    KeInitializeSpinLock(&Adapter->QueueLock);
    Adapter->QueueState = UaspQueueStopped;
    Adapter->MaximumTransferLength = UASP_MAX_TRANSFER_LENGTH;

    ConfigInfo->NumberOfBuses = 1;
    ConfigInfo->MaximumNumberOfTargets = 1;
    ConfigInfo->MaximumNumberOfLogicalUnits = UASP_MAX_LUN;
    ConfigInfo->MaximumTransferLength = Adapter->MaximumTransferLength;

    /*
     * The buffer goes to the controller whole, so it is never broken up and
     * the port driver has no list to build.
     */
    ConfigInfo->NumberOfPhysicalBreaks = SP_UNINITIALIZED_VALUE;
    ConfigInfo->AlignmentMask = 3;

    ConfigInfo->VirtualDevice = TRUE;
    ConfigInfo->CachesData = FALSE;
    ConfigInfo->ScatterGather = TRUE;
    ConfigInfo->Master = TRUE;
    ConfigInfo->NeedPhysicalAddresses = TRUE;
    ConfigInfo->TaggedQueuing = TRUE;
    ConfigInfo->AutoRequestSense = TRUE;
    ConfigInfo->MultipleRequestPerLu = TRUE;
    ConfigInfo->ResetTargetSupported = TRUE;
    ConfigInfo->SynchronizationModel = StorSynchronizeFullDuplex;
    ConfigInfo->MaxNumberOfIO = UASP_MAX_REQUESTS;

    *Again = FALSE;

    return SP_RETURN_FOUND;
}

/**
 * @brief Asks to be called back somewhere the device can be talked to.
 *
 * Bringing a USB device up means waiting on the bus, which cannot be done
 * here, so the real work happens in the passive callback.
 */
BOOLEAN
NTAPI
UaspInitialize(
    _In_ PVOID DeviceExtension)
{
    DPRINT1("UaspInitialize(%p)\n", DeviceExtension);

    if (!StorPortEnablePassiveInitialization(DeviceExtension, UaspPassiveInitialize))
    {
        DPRINT1("The port driver refused a passive initialization callback\n");
        return FALSE;
    }

    return TRUE;
}

BOOLEAN
NTAPI
UaspPassiveInitialize(
    _In_ PVOID DeviceExtension)
{
    PUASP_ADAPTER_EXTENSION Adapter = DeviceExtension;
    NTSTATUS Status;

    DPRINT1("UaspPassiveInitialize(%p)\n", DeviceExtension);

    Status = UaspStartDevice(Adapter);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Bringing the UAS device up failed (Status 0x%08lx)\n", Status);
        UaspStopDevice(Adapter);
        return FALSE;
    }

    return TRUE;
}

BOOLEAN
NTAPI
UaspStartIo(
    _In_ PVOID DeviceExtension,
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    PUASP_ADAPTER_EXTENSION Adapter = DeviceExtension;
    UCHAR Function = SrbGetSrbFunction(Srb);

    switch (Function)
    {
        case SRB_FUNCTION_EXECUTE_SCSI:
            if (!Adapter->Started || Adapter->Removing)
            {
                UaspCompleteSrb(Adapter, Srb, SRB_STATUS_NO_DEVICE);
                break;
            }

            if (SrbGetLun(Srb) >= UASP_MAX_LUN)
            {
                UaspCompleteSrb(Adapter, Srb, SRB_STATUS_INVALID_LUN);
                break;
            }

            UaspIssueCommand(Adapter, Srb);
            break;

        case SRB_FUNCTION_FLUSH:
        case SRB_FUNCTION_SHUTDOWN:
        case SRB_FUNCTION_PNP:
        case SRB_FUNCTION_POWER:
            UaspCompleteSrb(Adapter, Srb, SRB_STATUS_SUCCESS);
            break;

        case SRB_FUNCTION_RESET_BUS:
        case SRB_FUNCTION_RESET_DEVICE:
        case SRB_FUNCTION_RESET_LOGICAL_UNIT:
            /* FIXME: Recover the device rather than just saying we did */
            UaspCompleteSrb(Adapter, Srb, SRB_STATUS_SUCCESS);
            break;

        default:
            DPRINT1("Request function %u is not one we answer\n", Function);
            UaspCompleteSrb(Adapter, Srb, SRB_STATUS_INVALID_REQUEST);
            break;
    }

    return TRUE;
}

BOOLEAN
NTAPI
UaspResetBus(
    _In_ PVOID DeviceExtension,
    _In_ ULONG PathId)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(PathId);

    /* FIXME: Reset the device and fail everything that was in flight */
    DPRINT1("UaspResetBus()\n");

    return TRUE;
}

SCSI_ADAPTER_CONTROL_STATUS
NTAPI
UaspAdapterControl(
    _In_ PVOID DeviceExtension,
    _In_ SCSI_ADAPTER_CONTROL_TYPE ControlType,
    _In_ PVOID Parameters)
{
    PUASP_ADAPTER_EXTENSION Adapter = DeviceExtension;
    PSCSI_SUPPORTED_CONTROL_TYPE_LIST List;

    DPRINT1("UaspAdapterControl(%p %u)\n", DeviceExtension, ControlType);

    switch (ControlType)
    {
        case ScsiQuerySupportedControlTypes:
            List = Parameters;

            if (ScsiQuerySupportedControlTypes < List->MaxControlType)
                List->SupportedTypeList[ScsiQuerySupportedControlTypes] = TRUE;

            if (ScsiStopAdapter < List->MaxControlType)
                List->SupportedTypeList[ScsiStopAdapter] = TRUE;

            if (ScsiRestartAdapter < List->MaxControlType)
                List->SupportedTypeList[ScsiRestartAdapter] = TRUE;
            break;

        case ScsiStopAdapter:
            /*
             * Nothing new may be issued from here on. Commands already out
             * still answer, and the device is only let go of once the port
             * driver asks for the resources back.
             */
            UaspFreezeQueue(Adapter);
            break;

        case ScsiRestartAdapter:
            if (Adapter->Started)
                Adapter->QueueState = UaspQueueRunning;
            break;

        default:
            return ScsiAdapterControlUnsuccessful;
    }

    return ScsiAdapterControlSuccess;
}

VOID
NTAPI
UaspFreeAdapterResources(
    _In_ PVOID DeviceExtension)
{
    DPRINT1("UaspFreeAdapterResources(%p)\n", DeviceExtension);

    UaspStopDevice(DeviceExtension);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    HW_INITIALIZATION_DATA InitData;

    DPRINT1("UAS Storport Miniport Driver\n");

    RtlZeroMemory(&InitData, sizeof(InitData));

    InitData.HwInitializationDataSize = sizeof(InitData);

    /*
     * USB is not a bus the port driver arbitrates resources on, so it has no
     * interface type of its own and the miniport registers under Internal.
     */
    InitData.AdapterInterfaceType = Internal;

    InitData.HwInitialize = UaspInitialize;
    InitData.HwStartIo = UaspStartIo;
    InitData.HwFindAdapter = UaspFindAdapter;
    InitData.HwResetBus = UaspResetBus;
    InitData.HwAdapterControl = UaspAdapterControl;
    InitData.HwFreeAdapterResources = UaspFreeAdapterResources;

    InitData.DeviceExtensionSize = sizeof(UASP_ADAPTER_EXTENSION);
    InitData.SpecificLuExtensionSize = 0;
    InitData.SrbExtensionSize = 0;
    InitData.NumberOfAccessRanges = 0;

    InitData.MapBuffers = STOR_MAP_NON_READ_WRITE_BUFFERS;
    InitData.NeedPhysicalAddresses = TRUE;
    InitData.TaggedQueuing = TRUE;
    InitData.AutoRequestSense = TRUE;
    InitData.MultipleRequestPerLu = TRUE;

    InitData.FeatureSupport = STOR_FEATURE_VIRTUAL_MINIPORT |
                              STOR_FEATURE_FULL_PNP_DEVICE_CAPABILITIES;

    return StorPortInitialize(DriverObject, RegistryPath, &InitData, NULL);
}
