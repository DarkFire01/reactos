/*
 * PROJECT:     ReactOS UFS Host Controller Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Driver entry, adapter discovery and StorPort callbacks
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include "storufs.h"

#define NDEBUG
#include <debug.h>

#define UFS_BAR_ADDRESS_MASK 0xFFFFFFF0

VOID
StorUfsCompleteSrb(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb,
    _In_ UCHAR SrbStatus)
{
    SrbSetSrbStatus(Srb, SrbStatus);
    StorPortNotification(RequestComplete, Adapter, Srb);
}

static
BOOLEAN
UfsMapRegisters(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PPORT_CONFIGURATION_INFORMATION ConfigInfo)
{
    PCI_COMMON_HEADER Config;
    PACCESS_RANGE Range = NULL;
    ULONG Index;

    /* On PCI the register window is BAR0, so match it against config space.
       An ACPI enumerated controller has no config space, so fall through to
       the first memory window its _CRS provided. */
    if (ConfigInfo->AdapterInterfaceType == PCIBus &&
        StorPortGetBusData(Adapter,
                           PCIConfiguration,
                           ConfigInfo->SystemIoBusNumber,
                           ConfigInfo->SlotNumber,
                           &Config,
                           sizeof(Config)) == sizeof(Config))
    {
        ULONGLONG Bar = Config.u.type0.BaseAddresses[0] & UFS_BAR_ADDRESS_MASK;

        /* A 64 bit memory BAR carries its upper half in the next register. */
        if ((Config.u.type0.BaseAddresses[0] & 0x6) == 0x4)
            Bar |= (ULONGLONG)Config.u.type0.BaseAddresses[1] << 32;

        for (Index = 0; Index < ConfigInfo->NumberOfAccessRanges; Index++)
        {
            if ((ULONGLONG)(*ConfigInfo->AccessRanges)[Index].RangeStart.QuadPart == Bar)
            {
                Range = &(*ConfigInfo->AccessRanges)[Index];
                break;
            }
        }
    }

    if (Range == NULL)
    {
        for (Index = 0; Index < ConfigInfo->NumberOfAccessRanges; Index++)
        {
            if ((*ConfigInfo->AccessRanges)[Index].RangeInMemory &&
                (*ConfigInfo->AccessRanges)[Index].RangeLength != 0)
            {
                Range = &(*ConfigInfo->AccessRanges)[Index];
                break;
            }
        }
    }

    if (Range == NULL)
    {
        DPRINT1("UFS: no register window in the device resources\n");
        return FALSE;
    }

    Adapter->Registers = StorPortGetDeviceBase(Adapter,
                                               ConfigInfo->AdapterInterfaceType,
                                               ConfigInfo->SystemIoBusNumber,
                                               Range->RangeStart,
                                               Range->RangeLength,
                                               !Range->RangeInMemory);
    if (Adapter->Registers == NULL)
    {
        DPRINT1("UFS: could not map the register range\n");
        return FALSE;
    }

    return TRUE;
}

static
BOOLEAN
UfsAllocateCommonBuffer(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PPORT_CONFIGURATION_INFORMATION ConfigInfo)
{
    ULONG Length;

    Adapter->CommonBuffer = StorPortGetUncachedExtension(Adapter,
                                                         ConfigInfo,
                                                         UFS_COMMON_BUFFER_SIZE);
    if (Adapter->CommonBuffer == NULL)
    {
        DPRINT1("UFS: could not allocate the uncached extension\n");
        return FALSE;
    }

    RtlZeroMemory(Adapter->CommonBuffer, UFS_COMMON_BUFFER_SIZE);

    Adapter->CommonBufferPhysical =
        StorPortGetPhysicalAddress(Adapter, NULL, Adapter->CommonBuffer, &Length);

    Adapter->UtrdList =
        (PUFS_UTRD)((PUCHAR)Adapter->CommonBuffer + UFS_UTRD_LIST_OFFSET);
    Adapter->UtrdListPhysical = Adapter->CommonBufferPhysical;
    Adapter->UtrdListPhysical.QuadPart += UFS_UTRD_LIST_OFFSET;

    Adapter->UtmrdListPhysical = Adapter->CommonBufferPhysical;
    Adapter->UtmrdListPhysical.QuadPart += UFS_UTMRD_LIST_OFFSET;

    Adapter->UcdArray = (PUCHAR)Adapter->CommonBuffer + UFS_UCD_ARRAY_OFFSET;
    Adapter->UcdArrayPhysical = Adapter->CommonBufferPhysical;
    Adapter->UcdArrayPhysical.QuadPart += UFS_UCD_ARRAY_OFFSET;

    return TRUE;
}

ULONG
NTAPI
StorUfsFindAdapter(
    _In_ PVOID DeviceExtension,
    _In_ PVOID HwContext,
    _In_ PVOID BusInformation,
    _In_ PCHAR ArgumentString,
    _Inout_ PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    _In_ PBOOLEAN Again)
{
    PUFS_ADAPTER_EXTENSION Adapter = DeviceExtension;

    UNREFERENCED_PARAMETER(HwContext);
    UNREFERENCED_PARAMETER(BusInformation);
    UNREFERENCED_PARAMETER(ArgumentString);

    DPRINT1("StorUfsFindAdapter(%p)\n", DeviceExtension);

    *Again = FALSE;

    RtlZeroMemory(Adapter, sizeof(*Adapter));
    Adapter->DumpMode = (ConfigInfo->DumpMode != 0);

    if (!UfsMapRegisters(Adapter, ConfigInfo))
    {
        Adapter->State = UfsAdapterFailed;
        return SP_RETURN_ERROR;
    }

    Adapter->Capabilities = UfsRead(Adapter, UFS_REG_CAP);
    Adapter->Version = UfsRead(Adapter, UFS_REG_VER);
    Adapter->Nutrs = (Adapter->Capabilities & UFS_CAP_NUTRS_MASK) + 1;
    if (Adapter->Nutrs > UFS_MAX_SLOTS)
        Adapter->Nutrs = UFS_MAX_SLOTS;

    DPRINT1("UFS: version 0x%08lx, %lu transfer slots\n",
            Adapter->Version, Adapter->Nutrs);

    ConfigInfo->NumberOfBuses = 1;
    ConfigInfo->MaximumNumberOfTargets = 1;
    ConfigInfo->MaximumNumberOfLogicalUnits = 8;

    ConfigInfo->AlignmentMask = sizeof(ULONG) - 1;
    ConfigInfo->ScatterGather = TRUE;
    ConfigInfo->Master = TRUE;
    ConfigInfo->CachesData = TRUE;
    ConfigInfo->MapBuffers = STOR_MAP_NON_READ_WRITE_BUFFERS;
    ConfigInfo->SynchronizationModel = StorSynchronizeFullDuplex;
    ConfigInfo->ResetTargetSupported = FALSE;

    ConfigInfo->MaximumTransferLength = UFS_MAX_TRANSFER;
    ConfigInfo->NumberOfPhysicalBreaks = UFS_PRDT_ENTRIES;

    if (ConfigInfo->Dma64BitAddresses == SCSI_DMA64_SYSTEM_SUPPORTED)
        ConfigInfo->Dma64BitAddresses = SCSI_DMA64_MINIPORT_FULL64BIT_SUPPORTED;

    if (ConfigInfo->InterruptMode == Latched)
    {
        ConfigInfo->HwMSInterruptRoutine = StorUfsMsgInterrupt;
        ConfigInfo->InterruptSynchronizationMode = InterruptSynchronizePerMessage;
        Adapter->MessageInterrupts = TRUE;
    }
    else
    {
        ConfigInfo->InterruptSynchronizationMode = InterruptSynchronizeAll;
    }

    ConfigInfo->MaxNumberOfIO = Adapter->Nutrs;
    ConfigInfo->MaxIOsPerLun = Adapter->Nutrs;
    ConfigInfo->InitialLunQueueDepth = Adapter->Nutrs;

    /* The DMA adapter is built from the fields above, so claim the common
       buffer only once they are set. */
    if (!UfsAllocateCommonBuffer(Adapter, ConfigInfo))
    {
        Adapter->State = UfsAdapterFailed;
        return SP_RETURN_ERROR;
    }

    Adapter->FreeSlots = (Adapter->Nutrs >= 32) ?
                         0xFFFFFFFF : ((1UL << Adapter->Nutrs) - 1);

    Adapter->State = UfsAdapterFound;

    return SP_RETURN_FOUND;
}

BOOLEAN
NTAPI
StorUfsInitialize(
    _In_ PVOID DeviceExtension)
{
    PUFS_ADAPTER_EXTENSION Adapter = DeviceExtension;

    DPRINT1("StorUfsInitialize(%p)\n", DeviceExtension);

    if (!UfsStartAdapter(Adapter))
    {
        Adapter->State = UfsAdapterFailed;
        return FALSE;
    }

    Adapter->State = UfsAdapterRunning;
    return TRUE;
}

BOOLEAN
NTAPI
StorUfsBuildIo(
    _In_ PVOID DeviceExtension,
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    PUFS_ADAPTER_EXTENSION Adapter = DeviceExtension;
    ULONG Function = SrbGetSrbFunction(Srb);

    switch (Function)
    {
        case SRB_FUNCTION_EXECUTE_SCSI:
            return TRUE;

        case SRB_FUNCTION_FLUSH:
        case SRB_FUNCTION_SHUTDOWN:
            StorUfsCompleteSrb(Adapter, Srb, SRB_STATUS_SUCCESS);
            return FALSE;

        case SRB_FUNCTION_PNP:
        case SRB_FUNCTION_POWER:
            StorUfsCompleteSrb(Adapter, Srb, SRB_STATUS_SUCCESS);
            return FALSE;

        default:
            StorUfsCompleteSrb(Adapter, Srb, SRB_STATUS_INVALID_REQUEST);
            return FALSE;
    }
}

BOOLEAN
NTAPI
StorUfsStartIo(
    _In_ PVOID DeviceExtension,
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    PUFS_ADAPTER_EXTENSION Adapter = DeviceExtension;

    return UfsSubmitScsi(Adapter, Srb);
}

BOOLEAN
NTAPI
StorUfsInterrupt(
    _In_ PVOID DeviceExtension)
{
    return UfsScanCompletions((PUFS_ADAPTER_EXTENSION)DeviceExtension);
}

BOOLEAN
NTAPI
StorUfsMsgInterrupt(
    _In_ PVOID DeviceExtension,
    _In_ ULONG MessageId)
{
    UNREFERENCED_PARAMETER(MessageId);

    return UfsScanCompletions((PUFS_ADAPTER_EXTENSION)DeviceExtension);
}

BOOLEAN
NTAPI
StorUfsResetBus(
    _In_ PVOID DeviceExtension,
    _In_ ULONG PathId)
{
    PUFS_ADAPTER_EXTENSION Adapter = DeviceExtension;
    ULONG Slot;

    UNREFERENCED_PARAMETER(PathId);

    DPRINT1("StorUfsResetBus(%p)\n", DeviceExtension);

    for (Slot = 0; Slot < Adapter->Nutrs; Slot++)
    {
        PSCSI_REQUEST_BLOCK Srb = Adapter->SlotSrb[Slot];
        if (Srb != NULL)
        {
            Adapter->SlotSrb[Slot] = NULL;
            UfsFreeSlot(Adapter, Slot);
            StorUfsCompleteSrb(Adapter, Srb, SRB_STATUS_BUS_RESET);
        }
    }

    UfsStartAdapter(Adapter);
    return TRUE;
}

SCSI_ADAPTER_CONTROL_STATUS
NTAPI
StorUfsAdapterControl(
    _In_ PVOID DeviceExtension,
    _In_ SCSI_ADAPTER_CONTROL_TYPE ControlType,
    _In_ PVOID Parameters)
{
    PUFS_ADAPTER_EXTENSION Adapter = DeviceExtension;
    PSCSI_SUPPORTED_CONTROL_TYPE_LIST List;

    switch (ControlType)
    {
        case ScsiQuerySupportedControlTypes:
            List = Parameters;
            if (List->MaxControlType > ScsiStopAdapter)
                List->SupportedTypeList[ScsiStopAdapter] = TRUE;
            if (List->MaxControlType > ScsiRestartAdapter)
                List->SupportedTypeList[ScsiRestartAdapter] = TRUE;
            return ScsiAdapterControlSuccess;

        case ScsiStopAdapter:
            UfsWrite(Adapter, UFS_REG_IE, 0);
            UfsWrite(Adapter, UFS_REG_HCE, 0);
            Adapter->State = UfsAdapterStopped;
            return ScsiAdapterControlSuccess;

        case ScsiRestartAdapter:
            if (!UfsStartAdapter(Adapter))
                return ScsiAdapterControlUnsuccessful;
            Adapter->State = UfsAdapterRunning;
            return ScsiAdapterControlSuccess;

        default:
            return ScsiAdapterControlUnsuccessful;
    }
}

SCSI_UNIT_CONTROL_STATUS
NTAPI
StorUfsUnitControl(
    _In_ PVOID DeviceExtension,
    _In_ SCSI_UNIT_CONTROL_TYPE ControlType,
    _In_ PVOID Parameters)
{
    PSCSI_SUPPORTED_CONTROL_TYPE_LIST List;

    UNREFERENCED_PARAMETER(DeviceExtension);

    switch (ControlType)
    {
        case ScsiQuerySupportedUnitControlTypes:
            List = Parameters;
            if (List->MaxControlType > ScsiUnitStart)
                List->SupportedTypeList[ScsiUnitStart] = TRUE;
            if (List->MaxControlType > ScsiUnitRemove)
                List->SupportedTypeList[ScsiUnitRemove] = TRUE;
            return ScsiUnitControlSuccess;

        case ScsiUnitStart:
        case ScsiUnitRemove:
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
    NTSTATUS PciStatus;
    NTSTATUS AcpiStatus;

    DPRINT1("StorUfs DriverEntry(%p)\n", DriverObject);

    RtlZeroMemory(&InitData, sizeof(InitData));

    InitData.HwInitializationDataSize = sizeof(InitData);

    InitData.HwFindAdapter = StorUfsFindAdapter;
    InitData.HwInitialize = StorUfsInitialize;
    InitData.HwBuildIo = StorUfsBuildIo;
    InitData.HwStartIo = StorUfsStartIo;
    InitData.HwInterrupt = StorUfsInterrupt;
    InitData.HwResetBus = StorUfsResetBus;
    InitData.HwAdapterControl = StorUfsAdapterControl;
    InitData.HwUnitControl = StorUfsUnitControl;

    InitData.DeviceExtensionSize = sizeof(UFS_ADAPTER_EXTENSION);
    InitData.SrbExtensionSize = sizeof(UFS_REQUEST_CONTEXT);
    InitData.NumberOfAccessRanges = 6;

    InitData.MapBuffers = STOR_MAP_NON_READ_WRITE_BUFFERS;
    InitData.TaggedQueuing = TRUE;
    InitData.AutoRequestSense = TRUE;
    InitData.MultipleRequestPerLu = TRUE;
    InitData.NeedPhysicalAddresses = TRUE;

    InitData.SrbTypeFlags = SRB_TYPE_FLAG_STORAGE_REQUEST_BLOCK;
    InitData.AddressTypeFlags = ADDRESS_TYPE_FLAG_BTL8;
    InitData.FeatureSupport = STOR_FEATURE_DEVICE_NAME_NO_SUFFIX;

    /* Register for a PCI UFSHCI controller and for an ACPI enumerated one,
       which StorPort reports under the Internal interface. */
    InitData.AdapterInterfaceType = PCIBus;
    PciStatus = StorPortInitialize(DriverObject, RegistryPath, &InitData, NULL);

    InitData.AdapterInterfaceType = Internal;
    AcpiStatus = StorPortInitialize(DriverObject, RegistryPath, &InitData, NULL);

    return NT_SUCCESS(PciStatus) ? PciStatus : AcpiStatus;
}
