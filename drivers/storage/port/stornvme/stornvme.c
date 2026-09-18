/*
 * PROJECT:     ReactOS NVM Express Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Driver entry and controller discovery
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "stornvme.h"

#define NDEBUG
#include <debug.h>


/* FUNCTIONS ******************************************************************/

/**
 * @brief Reads one of the 64 bit controller registers.
 *
 * There is no 32 bit register accessor for a quadword, and the specification
 * allows a host without one to read the two halves separately.
 */
static
ULONGLONG
NvmpReadRegister64(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PULONGLONG Register)
{
    PULONG Half = (PULONG)Register;
    ULARGE_INTEGER Value;

    Value.LowPart = StorPortReadRegisterUlong(Adapter, Half);
    Value.HighPart = StorPortReadRegisterUlong(Adapter, Half + 1);

    return Value.QuadPart;
}


/**
 * @brief Maps the controller register block out of the adapter resources.
 *
 * Storport has already turned the bus resources into access ranges, so the
 * register block is simply the first of them.
 */
static
BOOLEAN
NvmpMapRegisters(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PPORT_CONFIGURATION_INFORMATION ConfigInfo)
{
    PACCESS_RANGE Range;

    if (ConfigInfo->NumberOfAccessRanges <= NVME_BAR_REGISTERS)
    {
        DPRINT1("No register access range\n");
        return FALSE;
    }

    Range = &(*ConfigInfo->AccessRanges)[NVME_BAR_REGISTERS];
    if (!Range->RangeInMemory || Range->RangeLength < sizeof(NVME_CONTROLLER_REGISTERS))
    {
        DPRINT1("Register range is unusable (memory %u, length %lu)\n",
                Range->RangeInMemory, Range->RangeLength);
        return FALSE;
    }

    Adapter->Registers = StorPortGetDeviceBase(Adapter,
                                               ConfigInfo->AdapterInterfaceType,
                                               ConfigInfo->SystemIoBusNumber,
                                               Range->RangeStart,
                                               Range->RangeLength,
                                               FALSE);
    if (Adapter->Registers == NULL)
    {
        DPRINT1("Could not map the register range\n");
        return FALSE;
    }

    Adapter->Doorbells = Adapter->Registers->Doorbells;

    return TRUE;
}


/**
 * @brief Works out what the controller supports from its capabilities.
 */
static
VOID
NvmpReadCapabilities(
    _In_ PNVME_ADAPTER_EXTENSION Adapter)
{
    ULONG HostShift;

    Adapter->Capabilities.AsUlonglong =
        NvmpReadRegister64(Adapter, &Adapter->Registers->CAP.AsUlonglong);
    Adapter->Version.AsUlong =
        StorPortReadRegisterUlong(Adapter, &Adapter->Registers->VS.AsUlong);

    DPRINT1("NVMe %u.%u.%u controller, %lu queue entries\n",
            Adapter->Version.MJR, Adapter->Version.MNR, Adapter->Version.TER,
            (ULONG)Adapter->Capabilities.MQES + 1);

    /* Doorbells are spaced by a power of two starting at four bytes */
    Adapter->DoorbellStride = 4 << Adapter->Capabilities.DSTRD;

    Adapter->ReadyTimeout = (ULONG)Adapter->Capabilities.TO * NVME_TIMEOUT_UNIT_MS;

    /*
     * Work in the host page size where the controller allows it, and fall back
     * to the largest it does allow.
     */
    HostShift = NVME_MIN_PAGE_SHIFT;
    if (HostShift > NVME_MIN_PAGE_SHIFT + Adapter->Capabilities.MPSMAX)
        HostShift = NVME_MIN_PAGE_SHIFT + Adapter->Capabilities.MPSMAX;
    if (HostShift < NVME_MIN_PAGE_SHIFT + Adapter->Capabilities.MPSMIN)
        HostShift = NVME_MIN_PAGE_SHIFT + Adapter->Capabilities.MPSMIN;

    Adapter->PageShift = HostShift;
    Adapter->PageSize = 1UL << HostShift;

    /* Neither queue is ever deeper than the controller allows */
    Adapter->AdminQueueDepth = NVME_ADMIN_QUEUE_DEPTH;
    if (Adapter->AdminQueueDepth > (ULONG)Adapter->Capabilities.MQES + 1)
        Adapter->AdminQueueDepth = (ULONG)Adapter->Capabilities.MQES + 1;

    Adapter->IoQueueDepth = NVME_IO_QUEUE_DEPTH;
    if (Adapter->IoQueueDepth > (ULONG)Adapter->Capabilities.MQES + 1)
        Adapter->IoQueueDepth = (ULONG)Adapter->Capabilities.MQES + 1;
}


ULONG
NTAPI
StorNvmeFindAdapter(
    _In_ PVOID DeviceExtension,
    _In_ PVOID HwContext,
    _In_ PVOID BusInformation,
    _In_ PCHAR ArgumentString,
    _Inout_ PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    _In_ PBOOLEAN Again)
{
    PNVME_ADAPTER_EXTENSION Adapter = DeviceExtension;

    UNREFERENCED_PARAMETER(HwContext);
    UNREFERENCED_PARAMETER(BusInformation);
    UNREFERENCED_PARAMETER(ArgumentString);

    DPRINT1("StorNvmeFindAdapter(%p)\n", DeviceExtension);

    *Again = FALSE;

    RtlZeroMemory(Adapter, sizeof(*Adapter));
    Adapter->DumpMode = (ConfigInfo->DumpMode != 0);

    if (!NvmpMapRegisters(Adapter, ConfigInfo))
    {
        Adapter->State = NvmeAdapterFailed;
        return SP_RETURN_ERROR;
    }

    NvmpReadCapabilities(Adapter);

    /* An NVMe controller has to speak the NVM command set to be of any use */
    if (!Adapter->Capabilities.CSS_NVM)
    {
        DPRINT1("Controller does not support the NVM command set\n");
        Adapter->State = NvmeAdapterFailed;
        return SP_RETURN_ERROR;
    }

    /*
     * One controller, one target. Namespaces show up as logical units, and
     * how many there are is only known once the controller is identified.
     */
    ConfigInfo->NumberOfBuses = 1;
    ConfigInfo->MaximumNumberOfTargets = 1;

    /* Transfers are described by physical region pages, which are dword aligned */
    ConfigInfo->AlignmentMask = sizeof(ULONG) - 1;
    ConfigInfo->ScatterGather = TRUE;
    ConfigInfo->Master = TRUE;
    ConfigInfo->CachesData = TRUE;
    ConfigInfo->MapBuffers = STOR_MAP_NON_READ_WRITE_BUFFERS;

    /* Completions arrive without the command having to be handed back first */
    ConfigInfo->SynchronizationModel = StorSynchronizeFullDuplex;

    /* A reset takes the whole controller, never a single namespace */
    ConfigInfo->ResetTargetSupported = FALSE;

    /* The controller addresses memory in its own pages, not in host terms */
    if (ConfigInfo->Dma64BitAddresses == SCSI_DMA64_SYSTEM_SUPPORTED)
        ConfigInfo->Dma64BitAddresses = SCSI_DMA64_MINIPORT_FULL64BIT_SUPPORTED;

    /*
     * Storport reports a latched interrupt when the controller was given
     * messages, and only then is there any point offering it a per message
     * service routine.
     */
    if (ConfigInfo->InterruptMode == Latched)
    {
        ConfigInfo->HwMSInterruptRoutine = StorNvmeMessageInterrupt;
        ConfigInfo->InterruptSynchronizationMode = InterruptSynchronizePerMessage;
        Adapter->MessageInterrupts = TRUE;
    }
    else
    {
        ConfigInfo->InterruptSynchronizationMode = InterruptSynchronizeAll;
    }

    /*
     * Bring the controller up here rather than in HwInitialize, because the
     * uncached extension can only be asked for while the port configuration
     * is still in our hands.
     */
    if (!NvmpStartController(Adapter, ConfigInfo))
    {
        Adapter->State = NvmeAdapterFailed;
        return SP_RETURN_ERROR;
    }

    /* Only now is it known how much the controller will actually take on */
    ConfigInfo->MaximumNumberOfLogicalUnits = (UCHAR)Adapter->NamespaceCount;
    ConfigInfo->MaximumTransferLength = Adapter->MaximumTransferLength;

    /*
     * A transfer is described by a list of pages, and one more than the pages
     * it spans covers the case of a buffer that starts part way into one.
     */
    ConfigInfo->NumberOfPhysicalBreaks =
        (Adapter->MaximumTransferLength / Adapter->PageSize) + 1;

    /*
     * One slot is always left free, because a submission queue counts as full
     * when its tail would catch the head, and because a slot that is still in
     * use must not be handed out again.
     */
    ConfigInfo->MaxNumberOfIO = Adapter->IoQueueDepth - 1;
    ConfigInfo->MaxIOsPerLun = ConfigInfo->MaxNumberOfIO;
    ConfigInfo->InitialLunQueueDepth = ConfigInfo->MaxNumberOfIO;

    Adapter->State = NvmeAdapterFound;

    return SP_RETURN_FOUND;
}


BOOLEAN
NTAPI
StorNvmeInitialize(
    _In_ PVOID DeviceExtension)
{
    PNVME_ADAPTER_EXTENSION Adapter = DeviceExtension;

    DPRINT1("StorNvmeInitialize(%p)\n", DeviceExtension);

    /* FIXME: Reset the controller and bring up the admin queues */
    Adapter->State = NvmeAdapterRunning;

    return TRUE;
}


/**
 * @brief Hands a finished request back to storport.
 */
VOID
NvmpCompleteRequest(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb,
    _In_ UCHAR SrbStatus)
{
    SrbSetSrbStatus(Srb, SrbStatus);
    StorPortNotification(RequestComplete, Adapter, Srb);
}


BOOLEAN
NTAPI
StorNvmeBuildIo(
    _In_ PVOID DeviceExtension,
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    PNVME_ADAPTER_EXTENSION Adapter = DeviceExtension;
    ULONG Function;

    Function = SrbGetSrbFunction(Srb);

    switch (Function)
    {
        case SRB_FUNCTION_EXECUTE_SCSI:
            /*
             * Anything this driver can answer from what it already knows is
             * finished here, and never reaches the controller.
             */
            if (NvmpTranslateScsi(Adapter, Srb))
            {
                StorPortNotification(RequestComplete, Adapter, Srb);
                return FALSE;
            }

            /* The rest are carried out by the controller */
            return NvmpBuildCommand(Adapter, Srb);

        case SRB_FUNCTION_FLUSH:
        case SRB_FUNCTION_SHUTDOWN:
            /* The write cache has to reach the medium before the power does */
            return NvmpBuildFlush(Adapter, Srb);

        case SRB_FUNCTION_PNP:
        case SRB_FUNCTION_POWER:
        case SRB_FUNCTION_RESET_BUS:
        case SRB_FUNCTION_RESET_DEVICE:
        case SRB_FUNCTION_RESET_LOGICAL_UNIT:
            NvmpCompleteRequest(Adapter, Srb, SRB_STATUS_SUCCESS);
            return FALSE;

        default:
            DPRINT1("Unsupported request function 0x%02lx\n", Function);
            NvmpCompleteRequest(Adapter, Srb, SRB_STATUS_INVALID_REQUEST);
            return FALSE;
    }
}


BOOLEAN
NTAPI
StorNvmeStartIo(
    _In_ PVOID DeviceExtension,
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    PNVME_ADAPTER_EXTENSION Adapter = DeviceExtension;

    NvmpPostCommand(Adapter, Srb);

    return TRUE;
}


/**
 * @brief Empties a completion queue, finishing every request it names.
 *
 * @return TRUE when at least one completion was found, which is what tells
 *         the system the interrupt belonged to this adapter.
 */
static
BOOLEAN
NvmpDrainQueue(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PNVME_QUEUE_PAIR Queue)
{
    NVME_COMPLETION_ENTRY Completion;
    BOOLEAN Handled = FALSE;

    while (NvmpNextCompletion(Adapter, Queue, &Completion))
    {
        NvmpCompleteFromEntry(Adapter, &Completion);
        Handled = TRUE;
    }

    return Handled;
}


BOOLEAN
NTAPI
StorNvmeInterrupt(
    _In_ PVOID DeviceExtension)
{
    PNVME_ADAPTER_EXTENSION Adapter = DeviceExtension;

    /* A shared line says nothing about which queue has work */
    return NvmpDrainQueue(Adapter, &Adapter->IoQueue);
}


BOOLEAN
NTAPI
StorNvmeMessageInterrupt(
    _In_ PVOID DeviceExtension,
    _In_ ULONG MessageId)
{
    PNVME_ADAPTER_EXTENSION Adapter = DeviceExtension;

    UNREFERENCED_PARAMETER(MessageId);

    /*
     * Every queue was created against vector zero, so whichever message
     * arrived, the one completion queue is where the work is.
     */
    return NvmpDrainQueue(Adapter, &Adapter->IoQueue);
}


BOOLEAN
NTAPI
StorNvmeResetBus(
    _In_ PVOID DeviceExtension,
    _In_ ULONG PathId)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(PathId);

    /* FIXME: Reset the controller and rebuild its queues */
    return FALSE;
}


SCSI_ADAPTER_CONTROL_STATUS
NTAPI
StorNvmeAdapterControl(
    _In_ PVOID DeviceExtension,
    _In_ SCSI_ADAPTER_CONTROL_TYPE ControlType,
    _In_ PVOID Parameters)
{
    PSCSI_SUPPORTED_CONTROL_TYPE_LIST List;

    UNREFERENCED_PARAMETER(DeviceExtension);

    DPRINT("StorNvmeAdapterControl(%u)\n", ControlType);

    switch (ControlType)
    {
        case ScsiQuerySupportedControlTypes:
        {
            List = Parameters;

            if (List->MaxControlType > ScsiStopAdapter)
                List->SupportedTypeList[ScsiStopAdapter] = TRUE;
            if (List->MaxControlType > ScsiRestartAdapter)
                List->SupportedTypeList[ScsiRestartAdapter] = TRUE;

            return ScsiAdapterControlSuccess;
        }

        case ScsiStopAdapter:
        case ScsiRestartAdapter:
            /* FIXME: Quiesce and rebuild the controller */
            return ScsiAdapterControlSuccess;

        default:
            return ScsiAdapterControlUnsuccessful;
    }
}


SCSI_UNIT_CONTROL_STATUS
NTAPI
StorNvmeUnitControl(
    _In_ PVOID DeviceExtension,
    _In_ SCSI_UNIT_CONTROL_TYPE ControlType,
    _In_ PVOID Parameters)
{
    PSCSI_SUPPORTED_CONTROL_TYPE_LIST List;

    UNREFERENCED_PARAMETER(DeviceExtension);

    DPRINT("StorNvmeUnitControl(%u)\n", ControlType);

    switch (ControlType)
    {
        case ScsiQuerySupportedUnitControlTypes:
        {
            List = Parameters;

            if (List->MaxControlType > ScsiUnitStart)
                List->SupportedTypeList[ScsiUnitStart] = TRUE;
            if (List->MaxControlType > ScsiUnitRemove)
                List->SupportedTypeList[ScsiUnitRemove] = TRUE;

            return ScsiUnitControlSuccess;
        }

        case ScsiUnitStart:
        case ScsiUnitRemove:
            /* FIXME: Track which namespaces are in play */
            return ScsiUnitControlSuccess;

        default:
            return ScsiUnitControlNotSupported;
    }
}


NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    HW_INITIALIZATION_DATA InitData;

    DPRINT1("StorNvme DriverEntry(%p %wZ)\n", DriverObject, RegistryPath);

    RtlZeroMemory(&InitData, sizeof(InitData));

    InitData.HwInitializationDataSize = sizeof(InitData);
    InitData.AdapterInterfaceType = PCIBus;

    InitData.HwFindAdapter = StorNvmeFindAdapter;
    InitData.HwInitialize = StorNvmeInitialize;
    InitData.HwBuildIo = StorNvmeBuildIo;
    InitData.HwStartIo = StorNvmeStartIo;
    InitData.HwInterrupt = StorNvmeInterrupt;
    InitData.HwResetBus = StorNvmeResetBus;
    InitData.HwAdapterControl = StorNvmeAdapterControl;
    InitData.HwUnitControl = StorNvmeUnitControl;

    InitData.DeviceExtensionSize = sizeof(NVME_ADAPTER_EXTENSION);

    /*
     * The per request area carries the command and a page for the region
     * page list, which has to be found on a page boundary inside it.
     */
    InitData.SrbExtensionSize = sizeof(NVME_REQUEST_CONTEXT) + (2 * PAGE_SIZE);

    /* The register block is the only resource the controller exposes */
    InitData.NumberOfAccessRanges = 1;

    InitData.MapBuffers = STOR_MAP_NON_READ_WRITE_BUFFERS;
    InitData.TaggedQueuing = TRUE;
    InitData.AutoRequestSense = TRUE;
    InitData.MultipleRequestPerLu = TRUE;
    InitData.NeedPhysicalAddresses = TRUE;

    /* Commands are built from the extended request block only */
    InitData.SrbTypeFlags = SRB_TYPE_FLAG_STORAGE_REQUEST_BLOCK;
    InitData.AddressTypeFlags = ADDRESS_TYPE_FLAG_BTL8;

    return StorPortInitialize(DriverObject, RegistryPath, &InitData, NULL);
}
