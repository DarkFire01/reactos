/*
 * PROJECT:     ReactOS WBEM Proxy
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Standard root\wmi classes for drivers that ship no MOF resource
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#define COBJMACROS

#include <stdarg.h>
#include <wchar.h>

#include <windef.h>
#include <winbase.h>
#include <objbase.h>
#include <oleauto.h>
#include <wbemcli.h>

#include "wmiprov.h"

typedef struct _WMI_STD_PROPERTY
{
    PCWSTR Name;
    CIMTYPE Type;
    PCWSTR ObjectClass;
    ULONG DataId;
    ULONG FixedCount;
    PCWSTR SizeIs;
    ULONG Flags;
} WMI_STD_PROPERTY, *PWMI_STD_PROPERTY;

typedef struct _WMI_STD_CLASS
{
    PCWSTR Name;
    PCWSTR SuperName;
    PCWSTR Guid;
    ULONG Flags;
    ULONG PropertyCount;
    const WMI_STD_PROPERTY *Properties;
} WMI_STD_CLASS, *PWMI_STD_CLASS;

#define WMI_STD_PROPERTIES(List) ARRAYSIZE(List), List

/* Schemas match the classes the system MOF registers on Windows */

static const WMI_STD_PROPERTY MSWmi_PnPDeviceIdProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"PnPDeviceId", CIM_STRING, NULL, 1, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSWmi_PnPInstanceNamesProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Count", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"InstanceNameList", CIM_STRING | CIM_FLAG_ARRAY, NULL, 2, 0, L"Count", WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSAcpi_ThermalZoneTemperatureProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"ThermalStamp", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"ThermalConstant1", CIM_UINT32, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"ThermalConstant2", CIM_UINT32, NULL, 3, 0, NULL, WMI_ITEM_READ },
    { L"Reserved", CIM_UINT32, NULL, 4, 0, NULL, WMI_ITEM_READ },
    { L"SamplingPeriod", CIM_UINT32, NULL, 5, 0, NULL, WMI_ITEM_READ },
    { L"CurrentTemperature", CIM_UINT32, NULL, 6, 0, NULL, WMI_ITEM_READ },
    { L"PassiveTripPoint", CIM_UINT32, NULL, 7, 0, NULL, WMI_ITEM_READ },
    { L"CriticalTripPoint", CIM_UINT32, NULL, 8, 0, NULL, WMI_ITEM_READ },
    { L"ActiveTripPointCount", CIM_UINT32, NULL, 9, 0, NULL, WMI_ITEM_READ },
    { L"ActiveTripPoint", CIM_UINT32 | CIM_FLAG_ARRAY, NULL, 10, 10, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSAcpiInfoProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"BootArchitecture", CIM_UINT32, NULL, 1, 0, NULL, 0 },
    { L"PreferredProfile", CIM_UINT32, NULL, 2, 0, NULL, 0 },
    { L"Capabilities", CIM_UINT32, NULL, 3, 0, NULL, 0 },
};

static const WMI_STD_PROPERTY MSSmBios_RawSMBiosTablesProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Used20CallingMethod", CIM_BOOLEAN, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"SmbiosMajorVersion", CIM_UINT8, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"SmbiosMinorVersion", CIM_UINT8, NULL, 3, 0, NULL, WMI_ITEM_READ },
    { L"DmiRevision", CIM_UINT8, NULL, 4, 0, NULL, WMI_ITEM_READ },
    { L"Size", CIM_UINT32, NULL, 5, 0, NULL, WMI_ITEM_READ },
    { L"SMBiosData", CIM_UINT8 | CIM_FLAG_ARRAY, NULL, 6, 0, L"Size", WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MS_SystemInformationProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"BaseBoardManufacturer", CIM_STRING, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"BaseBoardProduct", CIM_STRING, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"BaseBoardVersion", CIM_STRING, NULL, 3, 0, NULL, WMI_ITEM_READ },
    { L"BIOSReleaseDate", CIM_STRING, NULL, 4, 0, NULL, WMI_ITEM_READ },
    { L"BIOSVendor", CIM_STRING, NULL, 5, 0, NULL, WMI_ITEM_READ },
    { L"BIOSVersion", CIM_STRING, NULL, 6, 0, NULL, WMI_ITEM_READ },
    { L"SystemFamily", CIM_STRING, NULL, 7, 0, NULL, WMI_ITEM_READ },
    { L"SystemManufacturer", CIM_STRING, NULL, 8, 0, NULL, WMI_ITEM_READ },
    { L"SystemProductName", CIM_STRING, NULL, 9, 0, NULL, WMI_ITEM_READ },
    { L"SystemSKU", CIM_STRING, NULL, 10, 0, NULL, WMI_ITEM_READ },
    { L"SystemVersion", CIM_STRING, NULL, 11, 0, NULL, WMI_ITEM_READ },
    { L"BiosMajorRelease", CIM_UINT8, NULL, 12, 0, NULL, WMI_ITEM_READ },
    { L"BiosMinorRelease", CIM_UINT8, NULL, 13, 0, NULL, WMI_ITEM_READ },
    { L"ECFirmwareMajorRelease", CIM_UINT8, NULL, 14, 0, NULL, WMI_ITEM_READ },
    { L"ECFirmwareMinorRelease", CIM_UINT8, NULL, 15, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSPower_DeviceEnableProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Enable", CIM_BOOLEAN, NULL, 1, 0, NULL, WMI_ITEM_READ | WMI_ITEM_WRITE },
};

static const WMI_STD_PROPERTY MSPower_DeviceWakeEnableProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Enable", CIM_BOOLEAN, NULL, 1, 0, NULL, WMI_ITEM_READ | WMI_ITEM_WRITE },
};

static const WMI_STD_PROPERTY MSMouse_PortInformationProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"ConnectorType", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"DataQueueSize", CIM_UINT32, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"ErrorCount", CIM_UINT32, NULL, 3, 0, NULL, WMI_ITEM_READ },
    { L"Buttons", CIM_UINT32, NULL, 4, 0, NULL, WMI_ITEM_READ },
    { L"HardwareType", CIM_UINT32, NULL, 5, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSMouse_ClassInformationProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"DeviceId", CIM_UINT64, NULL, 1, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSKeyboard_PortInformationProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"ConnectorType", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"DataQueueSize", CIM_UINT32, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"ErrorCount", CIM_UINT32, NULL, 3, 0, NULL, WMI_ITEM_READ },
    { L"FunctionKeys", CIM_UINT32, NULL, 4, 0, NULL, WMI_ITEM_READ },
    { L"Indicators", CIM_UINT32, NULL, 5, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSKeyboard_ClassInformationProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"DeviceId", CIM_UINT64, NULL, 1, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSKeyboard_ExtendedIDProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Type", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"Subtype", CIM_UINT32, NULL, 2, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSIde_PortDeviceInfoProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Bus", CIM_UINT8, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"Target", CIM_UINT8, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"Lun", CIM_UINT8, NULL, 3, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSDiskDriver_GeometryProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Cylinders", CIM_SINT64, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"MediaType", CIM_UINT32, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"TracksPerCylinder", CIM_UINT32, NULL, 3, 0, NULL, WMI_ITEM_READ },
    { L"SectorsPerTrack", CIM_UINT32, NULL, 4, 0, NULL, WMI_ITEM_READ },
    { L"BytesPerSector", CIM_UINT32, NULL, 5, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSDiskDriver_PerformanceDataProperties[] =
{
    { L"BytesRead", CIM_SINT64, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"BytesWritten", CIM_SINT64, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"ReadTime", CIM_SINT64, NULL, 3, 0, NULL, WMI_ITEM_READ },
    { L"WriteTime", CIM_SINT64, NULL, 4, 0, NULL, WMI_ITEM_READ },
    { L"IdleTime", CIM_SINT64, NULL, 5, 0, NULL, WMI_ITEM_READ },
    { L"ReadCount", CIM_UINT32, NULL, 6, 0, NULL, WMI_ITEM_READ },
    { L"WriteCount", CIM_UINT32, NULL, 7, 0, NULL, WMI_ITEM_READ },
    { L"QueueDepth", CIM_UINT32, NULL, 8, 0, NULL, WMI_ITEM_READ },
    { L"SplitCount", CIM_UINT32, NULL, 9, 0, NULL, WMI_ITEM_READ },
    { L"QueryTime", CIM_SINT64, NULL, 10, 0, NULL, WMI_ITEM_READ },
    { L"StorageDeviceNumber", CIM_UINT32, NULL, 11, 0, NULL, WMI_ITEM_READ },
    { L"StorageManagerName", CIM_UINT16 | CIM_FLAG_ARRAY, NULL, 12, 8, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSDiskDriver_PerformanceProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"PerfData", CIM_OBJECT, L"MSDiskDriver_PerformanceData", 1, 0, NULL, WMI_ITEM_READ },
    { L"DeviceName", CIM_STRING, NULL, 2, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSStorageDriver_FailurePredictStatusProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Reason", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"PredictFailure", CIM_BOOLEAN, NULL, 2, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSStorageDriver_FailurePredictDataProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Length", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"VendorSpecific", CIM_UINT8 | CIM_FLAG_ARRAY, NULL, 2, 512, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSStorageDriver_FailurePredictEventProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Length", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"VendorSpecific", CIM_UINT8 | CIM_FLAG_ARRAY, NULL, 2, 0, L"Length", WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSStorageDriver_FailurePredictThresholdsProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"VendorSpecific", CIM_UINT8 | CIM_FLAG_ARRAY, NULL, 1, 512, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSStorageDriver_ATAPISmartDataProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Length", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"VendorSpecific", CIM_UINT8 | CIM_FLAG_ARRAY, NULL, 2, 362, NULL, WMI_ITEM_READ },
    { L"OfflineCollectionStatus", CIM_UINT8, NULL, 3, 0, NULL, WMI_ITEM_READ },
    { L"SelfTestStatus", CIM_UINT8, NULL, 4, 0, NULL, WMI_ITEM_READ },
    { L"TotalTime", CIM_UINT16, NULL, 5, 0, NULL, WMI_ITEM_READ },
    { L"VendorSpecific2", CIM_UINT8, NULL, 6, 0, NULL, WMI_ITEM_READ },
    { L"OfflineCollectCapability", CIM_UINT8, NULL, 7, 0, NULL, WMI_ITEM_READ },
    { L"SmartCapability", CIM_UINT16, NULL, 8, 0, NULL, WMI_ITEM_READ },
    { L"ErrorLogCapability", CIM_UINT8, NULL, 9, 0, NULL, WMI_ITEM_READ },
    { L"VendorSpecific3", CIM_UINT8, NULL, 10, 0, NULL, WMI_ITEM_READ },
    { L"ShortPollTimeInMinutes", CIM_UINT8, NULL, 11, 0, NULL, WMI_ITEM_READ },
    { L"ExtendedPollTimeInMinutes", CIM_UINT8, NULL, 12, 0, NULL, WMI_ITEM_READ },
    { L"Reserved", CIM_UINT8 | CIM_FLAG_ARRAY, NULL, 13, 12, NULL, WMI_ITEM_READ },
    { L"VendorSpecific4", CIM_UINT8 | CIM_FLAG_ARRAY, NULL, 14, 125, NULL, WMI_ITEM_READ },
    { L"Checksum", CIM_UINT8, NULL, 15, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSChangerParametersProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"NumberOfSlots", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"NumberOfDrives", CIM_UINT32, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"NumberOfIEPorts", CIM_UINT32, NULL, 3, 0, NULL, WMI_ITEM_READ },
    { L"NumberOfTransports", CIM_UINT32, NULL, 4, 0, NULL, WMI_ITEM_READ },
    { L"NumberOfDoors", CIM_UINT32, NULL, 5, 0, NULL, WMI_ITEM_READ },
    { L"NumberOfCleanerSlots", CIM_UINT32, NULL, 6, 0, NULL, WMI_ITEM_READ },
    { L"MagazineSize", CIM_UINT32, NULL, 7, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY BatteryStatusProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Tag", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"RemainingCapacity", CIM_UINT32, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"ChargeRate", CIM_SINT32, NULL, 3, 0, NULL, WMI_ITEM_READ },
    { L"DischargeRate", CIM_SINT32, NULL, 4, 0, NULL, WMI_ITEM_READ },
    { L"Voltage", CIM_UINT32, NULL, 5, 0, NULL, WMI_ITEM_READ },
    { L"PowerOnline", CIM_BOOLEAN, NULL, 6, 0, NULL, WMI_ITEM_READ },
    { L"Charging", CIM_BOOLEAN, NULL, 7, 0, NULL, WMI_ITEM_READ },
    { L"Discharging", CIM_BOOLEAN, NULL, 8, 0, NULL, WMI_ITEM_READ },
    { L"Critical", CIM_BOOLEAN, NULL, 9, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY BatteryStaticDataProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Tag", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"ManufactureDate", CIM_DATETIME, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"Granularity0", CIM_UINT64, NULL, 3, 0, NULL, WMI_ITEM_READ },
    { L"Granularity1", CIM_UINT64, NULL, 4, 0, NULL, WMI_ITEM_READ },
    { L"Granularity2", CIM_UINT64, NULL, 5, 0, NULL, WMI_ITEM_READ },
    { L"Granularity3", CIM_UINT64, NULL, 6, 0, NULL, WMI_ITEM_READ },
    { L"Capabilities", CIM_UINT32, NULL, 7, 0, NULL, WMI_ITEM_READ },
    { L"Technology", CIM_UINT8, NULL, 8, 0, NULL, WMI_ITEM_READ },
    { L"Chemistry", CIM_UINT32, NULL, 9, 0, NULL, WMI_ITEM_READ },
    { L"DesignedCapacity", CIM_UINT32, NULL, 10, 0, NULL, WMI_ITEM_READ },
    { L"DefaultAlert1", CIM_UINT32, NULL, 11, 0, NULL, WMI_ITEM_READ },
    { L"DefaultAlert2", CIM_UINT32, NULL, 12, 0, NULL, WMI_ITEM_READ },
    { L"CriticalBias", CIM_UINT32, NULL, 13, 0, NULL, WMI_ITEM_READ },
    { L"DeviceName", CIM_STRING, NULL, 14, 0, NULL, WMI_ITEM_READ },
    { L"ManufactureName", CIM_STRING, NULL, 15, 0, NULL, WMI_ITEM_READ },
    { L"SerialNumber", CIM_STRING, NULL, 16, 0, NULL, WMI_ITEM_READ },
    { L"UniqueID", CIM_STRING, NULL, 17, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY BatteryRuntimeProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Tag", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"EstimatedRuntime", CIM_UINT32, NULL, 2, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY BatteryTemperatureProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Tag", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"Temperature", CIM_UINT32, NULL, 2, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY BatteryFullChargedCapacityProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Tag", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"FullChargedCapacity", CIM_UINT32, NULL, 2, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY BatteryCycleCountProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Tag", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"CycleCount", CIM_UINT32, NULL, 2, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY BatteryStatusChangeProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Tag", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"PowerOnline", CIM_BOOLEAN, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"Charging", CIM_BOOLEAN, NULL, 3, 0, NULL, WMI_ITEM_READ },
    { L"Discharging", CIM_BOOLEAN, NULL, 4, 0, NULL, WMI_ITEM_READ },
    { L"Critical", CIM_BOOLEAN, NULL, 5, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY BatteryTagChangeProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"Tag", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY WmiMonitorIDProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"ProductCodeID", CIM_UINT16 | CIM_FLAG_ARRAY, NULL, 1, 16, NULL, WMI_ITEM_READ },
    { L"SerialNumberID", CIM_UINT16 | CIM_FLAG_ARRAY, NULL, 2, 16, NULL, WMI_ITEM_READ },
    { L"ManufacturerName", CIM_UINT16 | CIM_FLAG_ARRAY, NULL, 3, 16, NULL, WMI_ITEM_READ },
    { L"WeekOfManufacture", CIM_UINT8, NULL, 4, 0, NULL, WMI_ITEM_READ },
    { L"YearOfManufacture", CIM_UINT16, NULL, 5, 0, NULL, WMI_ITEM_READ },
    { L"UserFriendlyNameLength", CIM_UINT16, NULL, 6, 0, NULL, WMI_ITEM_READ },
    { L"UserFriendlyName", CIM_UINT16 | CIM_FLAG_ARRAY, NULL, 7, 0, L"UserFriendlyNameLength", WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY WmiMonitorSupportedDisplayFeaturesProperties[] =
{
    { L"StandbySupported", CIM_BOOLEAN, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"SuspendSupported", CIM_BOOLEAN, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"ActiveOffSupported", CIM_BOOLEAN, NULL, 3, 0, NULL, WMI_ITEM_READ },
    { L"DisplayType", CIM_UINT8, NULL, 4, 0, NULL, WMI_ITEM_READ },
    { L"sRGBSupported", CIM_BOOLEAN, NULL, 5, 0, NULL, WMI_ITEM_READ },
    { L"HasPreferredTimingMode", CIM_BOOLEAN, NULL, 6, 0, NULL, WMI_ITEM_READ },
    { L"GTFSupported", CIM_BOOLEAN, NULL, 7, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY WmiMonitorBasicDisplayParamsProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"VideoInputType", CIM_UINT8, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"MaxHorizontalImageSize", CIM_UINT8, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"MaxVerticalImageSize", CIM_UINT8, NULL, 3, 0, NULL, WMI_ITEM_READ },
    { L"DisplayTransferCharacteristic", CIM_UINT8, NULL, 4, 0, NULL, WMI_ITEM_READ },
    { L"SupportedDisplayFeatures", CIM_OBJECT, L"WmiMonitorSupportedDisplayFeatures", 5, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY WmiMonitorBrightnessProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"CurrentBrightness", CIM_UINT8, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"Levels", CIM_UINT32, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"Level", CIM_UINT8 | CIM_FLAG_ARRAY, NULL, 3, 0, L"Levels", WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY WmiMonitorConnectionParamsProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"VideoOutputTechnology", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY VideoModeDescriptorProperties[] =
{
    { L"PixelClockRate", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"VerticalRefreshRateNumerator", CIM_UINT32, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"VerticalRefreshRateDenominator", CIM_UINT32, NULL, 3, 0, NULL, WMI_ITEM_READ },
    { L"HorizontalRefreshRateNumerator", CIM_UINT32, NULL, 4, 0, NULL, WMI_ITEM_READ },
    { L"HorizontalRefreshRateDenominator", CIM_UINT32, NULL, 5, 0, NULL, WMI_ITEM_READ },
    { L"HorizontalActivePixels", CIM_UINT16, NULL, 6, 0, NULL, WMI_ITEM_READ },
    { L"VerticalActivePixels", CIM_UINT16, NULL, 7, 0, NULL, WMI_ITEM_READ },
    { L"HorizontalBlankingPixels", CIM_UINT16, NULL, 8, 0, NULL, WMI_ITEM_READ },
    { L"VerticalBlankingPixels", CIM_UINT16, NULL, 9, 0, NULL, WMI_ITEM_READ },
    { L"HorizontalSyncOffset", CIM_UINT16, NULL, 10, 0, NULL, WMI_ITEM_READ },
    { L"VerticalSyncOffset", CIM_UINT16, NULL, 11, 0, NULL, WMI_ITEM_READ },
    { L"HorizontalSyncPulseWidth", CIM_UINT16, NULL, 12, 0, NULL, WMI_ITEM_READ },
    { L"VerticalSyncPulseWidth", CIM_UINT16, NULL, 13, 0, NULL, WMI_ITEM_READ },
    { L"HorizontalImageSize", CIM_UINT16, NULL, 14, 0, NULL, WMI_ITEM_READ },
    { L"VerticalImageSize", CIM_UINT16, NULL, 15, 0, NULL, WMI_ITEM_READ },
    { L"HorizontalBorder", CIM_UINT16, NULL, 16, 0, NULL, WMI_ITEM_READ },
    { L"VerticalBorder", CIM_UINT16, NULL, 17, 0, NULL, WMI_ITEM_READ },
    { L"IsInterlaced", CIM_BOOLEAN, NULL, 18, 0, NULL, WMI_ITEM_READ },
    { L"StereoModeType", CIM_UINT8, NULL, 19, 0, NULL, WMI_ITEM_READ },
    { L"SyncSignalType", CIM_UINT8, NULL, 20, 0, NULL, WMI_ITEM_READ },
    { L"IsSerrationRequired", CIM_UINT8, NULL, 21, 0, NULL, WMI_ITEM_READ },
    { L"IsSyncOnRGB", CIM_UINT8, NULL, 22, 0, NULL, WMI_ITEM_READ },
    { L"CompositePolarityType", CIM_UINT8, NULL, 23, 0, NULL, WMI_ITEM_READ },
    { L"VerticalPolarityType", CIM_UINT8, NULL, 24, 0, NULL, WMI_ITEM_READ },
    { L"HorizontalPolarityType", CIM_UINT8, NULL, 25, 0, NULL, WMI_ITEM_READ },
    { L"VideoStandardType", CIM_UINT8, NULL, 26, 0, NULL, WMI_ITEM_READ },
    { L"Origin", CIM_UINT8, NULL, 27, 0, NULL, WMI_ITEM_READ },
    { L"TimingType", CIM_UINT8, NULL, 28, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY WmiMonitorListedSupportedSourceModesProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"NumOfMonitorSourceModes", CIM_UINT16, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"PreferredMonitorSourceModeIndex", CIM_UINT16, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"MonitorSourceModes", CIM_OBJECT | CIM_FLAG_ARRAY, L"VideoModeDescriptor", 3, 0, L"NumOfMonitorSourceModes", WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSNdis_HardwareStatusProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"NdisHardwareStatus", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSNdis_MediaSupportedProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"NumberElements", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"NdisMediaSupported", CIM_UINT32 | CIM_FLAG_ARRAY, NULL, 2, 0, L"NumberElements", WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSNdis_MediaInUseProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"NumberElements", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"NdisMediaInUse", CIM_UINT32 | CIM_FLAG_ARRAY, NULL, 2, 0, L"NumberElements", WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSNdis_MaximumFrameSizeProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"NdisMaximumFrameSize", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSNdis_LinkSpeedProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"NdisLinkSpeed", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSNdis_VendorIDProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"NdisVendorID", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSNdis_VendorDescriptionProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"NdisVendorDescription", CIM_STRING, NULL, 1, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSNdis_DriverVersionProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"NdisDriverVersion", CIM_UINT16, NULL, 1, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSNdis_MediaConnectStatusProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"NdisMediaConnectStatus", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSNdis_NetworkAddressProperties[] =
{
    { L"Address", CIM_UINT8 | CIM_FLAG_ARRAY, NULL, 1, 6, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSNdis_EthernetPermanentAddressProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"NdisPermanentAddress", CIM_OBJECT, L"MSNdis_NetworkAddress", 1, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSNdis_EthernetCurrentAddressProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"NdisCurrentAddress", CIM_OBJECT, L"MSNdis_NetworkAddress", 1, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSNdis_TransmitsOkProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"NdisTransmitsOk", CIM_UINT64, NULL, 1, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSNdis_ReceivesOkProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
    { L"NdisReceivesOk", CIM_UINT64, NULL, 1, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSNdis_StatusMediaConnectProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSNdis_StatusMediaDisconnectProperties[] =
{
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY | WMI_ITEM_READ },
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSSerial_PortNameProperties[] =
{
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, 0 },
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY },
    { L"PortName", CIM_STRING, NULL, 1, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_PROPERTY MSSerial_CommInfoProperties[] =
{
    { L"Active", CIM_BOOLEAN, NULL, 0, 0, NULL, 0 },
    { L"InstanceName", CIM_STRING, NULL, 0, 0, NULL, WMI_ITEM_KEY },
    { L"BaudRate", CIM_UINT32, NULL, 1, 0, NULL, WMI_ITEM_READ },
    { L"BitsPerByte", CIM_UINT32, NULL, 2, 0, NULL, WMI_ITEM_READ },
    { L"Parity", CIM_UINT32, NULL, 3, 0, NULL, WMI_ITEM_READ },
    { L"ParityCheckEnable", CIM_BOOLEAN, NULL, 4, 0, NULL, WMI_ITEM_READ },
    { L"StopBits", CIM_UINT32, NULL, 5, 0, NULL, WMI_ITEM_READ },
    { L"XoffCharacter", CIM_UINT32, NULL, 6, 0, NULL, WMI_ITEM_READ },
    { L"XoffXmitThreshold", CIM_UINT32, NULL, 7, 0, NULL, WMI_ITEM_READ },
    { L"XonCharacter", CIM_UINT32, NULL, 8, 0, NULL, WMI_ITEM_READ },
    { L"XonXmitThreshold", CIM_UINT32, NULL, 9, 0, NULL, WMI_ITEM_READ },
    { L"MaximumBaudRate", CIM_UINT32, NULL, 10, 0, NULL, WMI_ITEM_READ },
    { L"MaximumOutputBufferSize", CIM_UINT32, NULL, 11, 0, NULL, WMI_ITEM_READ },
    { L"MaximumInputBufferSize", CIM_UINT32, NULL, 12, 0, NULL, WMI_ITEM_READ },
    { L"Support16BitMode", CIM_BOOLEAN, NULL, 13, 0, NULL, WMI_ITEM_READ },
    { L"SupportDTRDSR", CIM_BOOLEAN, NULL, 14, 0, NULL, WMI_ITEM_READ },
    { L"SupportIntervalTimeouts", CIM_BOOLEAN, NULL, 15, 0, NULL, WMI_ITEM_READ },
    { L"SupportParityCheck", CIM_BOOLEAN, NULL, 16, 0, NULL, WMI_ITEM_READ },
    { L"SupportRTSCTS", CIM_BOOLEAN, NULL, 17, 0, NULL, WMI_ITEM_READ },
    { L"SupportXonXoff", CIM_BOOLEAN, NULL, 18, 0, NULL, WMI_ITEM_READ },
    { L"SettableBaudRate", CIM_BOOLEAN, NULL, 19, 0, NULL, WMI_ITEM_READ },
    { L"SettableDataBits", CIM_BOOLEAN, NULL, 20, 0, NULL, WMI_ITEM_READ },
    { L"SettableFlowControl", CIM_BOOLEAN, NULL, 21, 0, NULL, WMI_ITEM_READ },
    { L"SettableParity", CIM_BOOLEAN, NULL, 22, 0, NULL, WMI_ITEM_READ },
    { L"SettableParityCheck", CIM_BOOLEAN, NULL, 23, 0, NULL, WMI_ITEM_READ },
    { L"SettableStopBits", CIM_BOOLEAN, NULL, 24, 0, NULL, WMI_ITEM_READ },
    { L"IsBusy", CIM_BOOLEAN, NULL, 25, 0, NULL, WMI_ITEM_READ },
};

static const WMI_STD_CLASS WmiStdClasses[] =
{
    { L"WMIEvent", L"__ExtrinsicEvent", NULL, 0, 0, NULL },
    { L"MS_WmiInternal", NULL, NULL, WMI_CLASS_ABSTRACT, 0, NULL },
    { L"MSWmi_PnPDeviceId", L"MS_WmiInternal", L"{C7BF35D2-AADB-11D1-BF4A-00A0C9062910}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSWmi_PnPDeviceIdProperties) },
    { L"MSWmi_PnPInstanceNames", L"MS_WmiInternal", L"{C7BF35D3-AADB-11D1-BF4A-00A0C9062910}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSWmi_PnPInstanceNamesProperties) },
    { L"MSAcpi", NULL, NULL, WMI_CLASS_ABSTRACT, 0, NULL },
    { L"MSAcpi_ThermalZoneTemperature", L"MSAcpi", L"{A1BC18C0-A7C8-11D1-BF3C-00A0C9062910}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSAcpi_ThermalZoneTemperatureProperties) },
    { L"MSAcpiInfo", NULL, L"{5DAF38AE-F6F8-4D90-8199-EBDE6800EC3B}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSAcpiInfoProperties) },
    { L"MS_SmBios", NULL, NULL, WMI_CLASS_ABSTRACT, 0, NULL },
    { L"MSSmBios_RawSMBiosTables", L"MS_SmBios", L"{8F680850-A584-11D1-BF38-00A0C9062910}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSSmBios_RawSMBiosTablesProperties) },
    { L"MS_SystemInformation", NULL, L"{98A2B9D7-94DD-496A-847E-67A5557A59F2}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MS_SystemInformationProperties) },
    { L"MSPower", NULL, NULL, WMI_CLASS_ABSTRACT, 0, NULL },
    { L"MSPower_DeviceEnable", L"MSPower", L"{827C0A6F-FEB0-11D0-BD26-00AA00B7B32A}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSPower_DeviceEnableProperties) },
    { L"MSPower_DeviceWakeEnable", L"MSPower", L"{A9546A82-FEB0-11D0-BD26-00AA00B7B32A}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSPower_DeviceWakeEnableProperties) },
    { L"MSMouse", NULL, NULL, 0, 0, NULL },
    { L"MSMouse_PortInformation", L"MSMouse", L"{4731F89C-71CB-11D1-A52C-00A0C9062910}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSMouse_PortInformationProperties) },
    { L"MSMouse_ClassInformation", L"MSMouse", L"{4731F89B-71CB-11D1-A52C-00A0C9062910}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSMouse_ClassInformationProperties) },
    { L"MSKeyboard", NULL, NULL, WMI_CLASS_ABSTRACT, 0, NULL },
    { L"MSKeyboard_PortInformation", L"MSKeyboard", L"{4731F89A-71CB-11D1-A52C-00A0C9062910}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSKeyboard_PortInformationProperties) },
    { L"MSKeyboard_ClassInformation", L"MSKeyboard", L"{4731F899-71CB-11D1-A52C-00A0C9062910}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSKeyboard_ClassInformationProperties) },
    { L"MSKeyboard_ExtendedID", L"MSKeyboard", L"{6AC4E23D-A950-4518-8B2B-AA4DCD5FE14A}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSKeyboard_ExtendedIDProperties) },
    { L"MSIde", NULL, NULL, WMI_CLASS_ABSTRACT, 0, NULL },
    { L"MSIde_PortDeviceInfo", L"MSIde", L"{53F5630F-B6BF-11D0-94F2-00A0C91EFB8B}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSIde_PortDeviceInfoProperties) },
    { L"MSDiskDriver", NULL, NULL, WMI_CLASS_ABSTRACT, 0, NULL },
    { L"MSDiskDriver_Geometry", L"MSDiskDriver", L"{25007F51-57C2-11D1-A528-00A0C9062910}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSDiskDriver_GeometryProperties) },
    { L"MSDiskDriver_PerformanceData", L"MSDiskDriver", L"{BDD865D2-D7C1-11D0-A501-00A0C9062910}", WMI_CLASS_WMI, WMI_STD_PROPERTIES(MSDiskDriver_PerformanceDataProperties) },
    { L"MSDiskDriver_Performance", L"MSDiskDriver", L"{BDD865D1-D7C1-11D0-A501-00A0C9062910}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSDiskDriver_PerformanceProperties) },
    { L"MSStorageDriver", NULL, NULL, WMI_CLASS_ABSTRACT, 0, NULL },
    { L"MSStorageDriver_FailurePredictStatus", L"MSStorageDriver", L"{78EBC102-4CF9-11D2-BA4A-00A0C9062910}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSStorageDriver_FailurePredictStatusProperties) },
    { L"MSStorageDriver_FailurePredictData", L"MSStorageDriver", L"{78EBC103-4CF9-11D2-BA4A-00A0C9062910}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSStorageDriver_FailurePredictDataProperties) },
    { L"MSStorageDriver_FailurePredictEvent", L"WmiEvent", L"{78EBC104-4CF9-11D2-BA4A-00A0C9062910}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSStorageDriver_FailurePredictEventProperties) },
    { L"MSStorageDriver_FailurePredictThresholds", L"MSStorageDriver", L"{DAE10783-CC31-4D2A-8A0F-861C04077A95}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSStorageDriver_FailurePredictThresholdsProperties) },
    { L"MSStorageDriver_ATAPISmartData", L"MSStorageDriver", L"{78EBC103-4CF9-11D2-BA4A-00A0C9062910}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSStorageDriver_ATAPISmartDataProperties) },
    { L"MSChangerDriver", NULL, NULL, WMI_CLASS_ABSTRACT, 0, NULL },
    { L"MSChangerParameters", L"MSChangerDriver", L"{24EB52AC-7C77-438B-AB61-D024DAB0C03A}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSChangerParametersProperties) },
    { L"MSBatteryClass", L"Win32_PerfRawData", NULL, WMI_CLASS_ABSTRACT, 0, NULL },
    { L"BatteryStatus", L"MSBatteryClass", L"{FC4670D1-EBBF-416E-87CE-374A4EBC111A}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(BatteryStatusProperties) },
    { L"BatteryStaticData", L"MSBatteryClass", L"{05E1E463-E4E2-4EA9-80CB-9BD4B3CA0655}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(BatteryStaticDataProperties) },
    { L"BatteryRuntime", L"MSBatteryClass", L"{535A3767-1AC2-49BC-A077-3F7A02E40AEC}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(BatteryRuntimeProperties) },
    { L"BatteryTemperature", L"MSBatteryClass", L"{1A52A14D-ADCE-4A44-9A3E-C8D8F15FF2C2}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(BatteryTemperatureProperties) },
    { L"BatteryFullChargedCapacity", L"MSBatteryClass", L"{40B40565-96F7-4435-8694-97E0E4395905}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(BatteryFullChargedCapacityProperties) },
    { L"BatteryCycleCount", L"MSBatteryClass", L"{EF98DB24-0014-4C25-A50B-C724AE5CD371}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(BatteryCycleCountProperties) },
    { L"MSBatteryClassEvent", L"WmiEvent", NULL, 0, 0, NULL },
    { L"BatteryStatusChange", L"MSBatteryClassEvent", L"{CDDFA0C3-7C5B-4E43-A034-059FA5B84364}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(BatteryStatusChangeProperties) },
    { L"BatteryTagChange", L"MSBatteryClassEvent", L"{5E1F6E19-8786-4D23-94FC-9E746BD5D888}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(BatteryTagChangeProperties) },
    { L"MSMonitorClass", NULL, NULL, WMI_CLASS_ABSTRACT, 0, NULL },
    { L"WmiMonitorID", L"MSMonitorClass", L"{671A8285-4EDB-4CAE-99FE-69A15C48C0BC}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(WmiMonitorIDProperties) },
    { L"WmiMonitorSupportedDisplayFeatures", NULL, L"{9FA9C28B-5E5C-4035-A5F8-67D4554F8D04}", WMI_CLASS_WMI, WMI_STD_PROPERTIES(WmiMonitorSupportedDisplayFeaturesProperties) },
    { L"WmiMonitorBasicDisplayParams", L"MSMonitorClass", L"{9831B7E6-09AC-491F-8D07-3C3D649D8240}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(WmiMonitorBasicDisplayParamsProperties) },
    { L"WmiMonitorBrightness", L"MSMonitorClass", L"{D43412AC-67F9-4FBB-A081-1752A2C33E84}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(WmiMonitorBrightnessProperties) },
    { L"WmiMonitorConnectionParams", L"MSMonitorClass", L"{2E2D2463-B537-4DA7-8EEE-51306F1F482F}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(WmiMonitorConnectionParamsProperties) },
    { L"VideoModeDescriptor", NULL, L"{4A97ED30-BFAA-491A-9DFD-B43ADEDBF8E3}", WMI_CLASS_WMI, WMI_STD_PROPERTIES(VideoModeDescriptorProperties) },
    { L"WmiMonitorListedSupportedSourceModes", L"MSMonitorClass", L"{6DC76655-063C-4524-A862-B8410C26281B}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(WmiMonitorListedSupportedSourceModesProperties) },
    { L"MSNdis", NULL, NULL, WMI_CLASS_ABSTRACT, 0, NULL },
    { L"MSNdis_HardwareStatus", L"MSNdis", L"{5EC10354-A61A-11D0-8DD4-00C04FC3358C}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSNdis_HardwareStatusProperties) },
    { L"MSNdis_MediaSupported", L"MSNdis", L"{5EC10355-A61A-11D0-8DD4-00C04FC3358C}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSNdis_MediaSupportedProperties) },
    { L"MSNdis_MediaInUse", L"MSNdis", L"{5EC10356-A61A-11D0-8DD4-00C04FC3358C}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSNdis_MediaInUseProperties) },
    { L"MSNdis_MaximumFrameSize", L"MSNdis", L"{5EC10358-A61A-11D0-8DD4-00C04FC3358C}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSNdis_MaximumFrameSizeProperties) },
    { L"MSNdis_LinkSpeed", L"MSNdis", L"{5EC10359-A61A-11D0-8DD4-00C04FC3358C}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSNdis_LinkSpeedProperties) },
    { L"MSNdis_VendorID", L"MSNdis", L"{5EC1035E-A61A-11D0-8DD4-00C04FC3358C}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSNdis_VendorIDProperties) },
    { L"MSNdis_VendorDescription", L"MSNdis", L"{5EC1035F-A61A-11D0-8DD4-00C04FC3358C}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSNdis_VendorDescriptionProperties) },
    { L"MSNdis_DriverVersion", L"MSNdis", L"{5EC10362-A61A-11D0-8DD4-00C04FC3358C}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSNdis_DriverVersionProperties) },
    { L"MSNdis_MediaConnectStatus", L"MSNdis", L"{5EC10366-A61A-11D0-8DD4-00C04FC3358C}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSNdis_MediaConnectStatusProperties) },
    { L"MSNdis_NetworkAddress", L"MSNdis", L"{B5BD98B7-0201-11D1-A50E-00A0C9062910}", WMI_CLASS_WMI, WMI_STD_PROPERTIES(MSNdis_NetworkAddressProperties) },
    { L"MSNdis_EthernetPermanentAddress", L"MSNdis", L"{447956FF-A61B-11D0-8DD4-00C04FC3358C}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSNdis_EthernetPermanentAddressProperties) },
    { L"MSNdis_EthernetCurrentAddress", L"MSNdis", L"{44795700-A61B-11D0-8DD4-00C04FC3358C}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSNdis_EthernetCurrentAddressProperties) },
    { L"MSNdis_TransmitsOk", L"MSNdis", L"{447956FA-A61B-11D0-8DD4-00C04FC3358C}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSNdis_TransmitsOkProperties) },
    { L"MSNdis_ReceivesOk", L"MSNdis", L"{447956FB-A61B-11D0-8DD4-00C04FC3358C}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSNdis_ReceivesOkProperties) },
    { L"MSNdis_StatusMediaConnect", L"WMIEvent", L"{981F2D7D-B1F3-11D0-8DD7-00C04FC3358C}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSNdis_StatusMediaConnectProperties) },
    { L"MSNdis_StatusMediaDisconnect", L"WMIEvent", L"{981F2D7E-B1F3-11D0-8DD7-00C04FC3358C}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSNdis_StatusMediaDisconnectProperties) },
    { L"MSSerial", NULL, NULL, WMI_CLASS_ABSTRACT, 0, NULL },
    { L"MSSerial_PortName", L"MSSerial", L"{A0EC11A8-B16C-11D1-BD98-00A0C906BE2D}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSSerial_PortNameProperties) },
    { L"MSSerial_CommInfo", L"MSSerial", L"{EDB16A62-B16C-11D1-BD98-00A0C906BE2D}", WMI_CLASS_WMI | WMI_CLASS_DYNAMIC, WMI_STD_PROPERTIES(MSSerial_CommInfoProperties) },
};

#define WMI_STD_MAX_CLASS_QUALIFIERS    5
#define WMI_STD_MAX_ITEM_QUALIFIERS     7

static
BOOLEAN
WmiStdAddQualifier(
    _Inout_updates_(WMI_STD_MAX_ITEM_QUALIFIERS) PWMI_QUALIFIER_DEF List,
    _Inout_ PULONG Count,
    _In_ PCWSTR Name,
    _In_ const VARIANT *Value)
{
    PWMI_QUALIFIER_DEF Qualifier = &List[*Count];

    Qualifier->Name = BmofStrDup(Name);
    if (!Qualifier->Name)
        return FALSE;

    VariantInit(&Qualifier->Value);
    if (FAILED(VariantCopy(&Qualifier->Value, (VARIANT *)Value)))
    {
        BmofFree(Qualifier->Name);
        Qualifier->Name = NULL;
        return FALSE;
    }
    (*Count)++;
    return TRUE;
}

static
BOOLEAN
WmiStdAddBool(
    _Inout_ PWMI_QUALIFIER_DEF List,
    _Inout_ PULONG Count,
    _In_ PCWSTR Name)
{
    VARIANT Value;

    V_VT(&Value) = VT_BOOL;
    V_BOOL(&Value) = VARIANT_TRUE;
    return WmiStdAddQualifier(List, Count, Name, &Value);
}

static
BOOLEAN
WmiStdAddLong(
    _Inout_ PWMI_QUALIFIER_DEF List,
    _Inout_ PULONG Count,
    _In_ PCWSTR Name,
    _In_ LONG Number)
{
    VARIANT Value;

    V_VT(&Value) = VT_I4;
    V_I4(&Value) = Number;
    return WmiStdAddQualifier(List, Count, Name, &Value);
}

static
BOOLEAN
WmiStdAddString(
    _Inout_ PWMI_QUALIFIER_DEF List,
    _Inout_ PULONG Count,
    _In_ PCWSTR Name,
    _In_ PCWSTR Text)
{
    VARIANT Value;
    BOOLEAN IsAdded;

    V_VT(&Value) = VT_BSTR;
    V_BSTR(&Value) = SysAllocString(Text);
    if (!V_BSTR(&Value))
        return FALSE;
    IsAdded = WmiStdAddQualifier(List, Count, Name, &Value);
    SysFreeString(V_BSTR(&Value));
    return IsAdded;
}

static
PCWSTR
WmiStdTypeName(
    _In_ CIMTYPE Type)
{
    switch (Type & ~CIM_FLAG_ARRAY)
    {
        case CIM_SINT8: return L"sint8";
        case CIM_UINT8: return L"uint8";
        case CIM_SINT16: return L"sint16";
        case CIM_UINT16: return L"uint16";
        case CIM_SINT32: return L"sint32";
        case CIM_UINT32: return L"uint32";
        case CIM_SINT64: return L"sint64";
        case CIM_UINT64: return L"uint64";
        case CIM_REAL32: return L"real32";
        case CIM_REAL64: return L"real64";
        case CIM_BOOLEAN: return L"boolean";
        case CIM_DATETIME: return L"datetime";
        case CIM_CHAR16: return L"char16";
        case CIM_OBJECT: return L"object";
        default: return L"string";
    }
}

static
BOOLEAN
WmiStdBuildProperty(
    _In_ const WMI_STD_PROPERTY *Source,
    _Out_ PWMI_PROPERTY_DEF Property)
{
    WCHAR CimType[128];
    ULONG Count = 0;
    BOOLEAN IsBuilt = TRUE;

    memset(Property, 0, sizeof(*Property));
    Property->Name = BmofStrDup(Source->Name);
    Property->Type = Source->Type;
    Property->DataId = Source->DataId;
    Property->FixedCount = Source->FixedCount;
    Property->Flags = Source->Flags;
    Property->ObjectClass = BmofStrDup(Source->ObjectClass);
    Property->SizeIs = BmofStrDup(Source->SizeIs);
    Property->Qualifiers = BmofAlloc(WMI_STD_MAX_ITEM_QUALIFIERS * sizeof(*Property->Qualifiers));

    if (!Property->Name || !Property->Qualifiers ||
        (Source->ObjectClass && !Property->ObjectClass) ||
        (Source->SizeIs && !Property->SizeIs))
    {
        return FALSE;
    }

    wcscpy(CimType, Source->ObjectClass ? L"object:" : WmiStdTypeName(Source->Type));
    if (Source->ObjectClass)
        wcsncat(CimType, Source->ObjectClass, ARRAYSIZE(CimType) - wcslen(CimType) - 1);

    if (Source->Flags & WMI_ITEM_KEY)
        IsBuilt &= WmiStdAddBool(Property->Qualifiers, &Count, L"key");
    if (Source->Flags & WMI_ITEM_READ)
        IsBuilt &= WmiStdAddBool(Property->Qualifiers, &Count, L"read");
    if (Source->Flags & WMI_ITEM_WRITE)
        IsBuilt &= WmiStdAddBool(Property->Qualifiers, &Count, L"write");
    if (Source->DataId)
        IsBuilt &= WmiStdAddLong(Property->Qualifiers, &Count, L"WmiDataId", Source->DataId);
    if (Source->FixedCount)
        IsBuilt &= WmiStdAddLong(Property->Qualifiers, &Count, L"MAX", Source->FixedCount);
    if (Source->SizeIs)
        IsBuilt &= WmiStdAddString(Property->Qualifiers, &Count, L"WmiSizeIs", Source->SizeIs);
    IsBuilt &= WmiStdAddString(Property->Qualifiers, &Count, L"CIMTYPE", CimType);

    Property->QualifierCount = Count;
    return IsBuilt;
}

static
PWMI_CLASS_DEF
WmiStdBuildClass(
    _In_ const WMI_STD_CLASS *Source)
{
    PWMI_CLASS_DEF Class;
    ULONG Count = 0;
    ULONG Index;
    BOOLEAN IsBuilt = TRUE;

    Class = BmofAlloc(sizeof(*Class));
    if (!Class)
        return NULL;

    Class->Name = BmofStrDup(Source->Name);
    Class->SuperName = BmofStrDup(Source->SuperName);
    Class->Flags = Source->Flags | WMI_CLASS_BUILTIN;
    Class->Qualifiers = BmofAlloc(WMI_STD_MAX_CLASS_QUALIFIERS * sizeof(*Class->Qualifiers));
    if (Source->PropertyCount)
        Class->Properties = BmofAlloc(Source->PropertyCount * sizeof(*Class->Properties));

    if (!Class->Name || !Class->Qualifiers ||
        (Source->SuperName && !Class->SuperName) ||
        (Source->PropertyCount && !Class->Properties))
    {
        BmofFreeClass(Class);
        return NULL;
    }

    if (Source->Flags & WMI_CLASS_ABSTRACT)
        IsBuilt &= WmiStdAddBool(Class->Qualifiers, &Count, L"abstract");
    if (Source->Flags & WMI_CLASS_WMI)
        IsBuilt &= WmiStdAddBool(Class->Qualifiers, &Count, L"WMI");
    if (Source->Flags & WMI_CLASS_DYNAMIC)
    {
        IsBuilt &= WmiStdAddBool(Class->Qualifiers, &Count, L"Dynamic");
        IsBuilt &= WmiStdAddString(Class->Qualifiers, &Count, L"Provider", L"WMIProv");
    }
    if (Source->Guid)
    {
        IsBuilt &= WmiStdAddString(Class->Qualifiers, &Count, L"guid", Source->Guid);
        if (BmofParseGuid(Source->Guid, &Class->Guid))
            Class->Flags |= WMI_CLASS_HAS_GUID;
    }
    Class->QualifierCount = Count;

    for (Index = 0; IsBuilt && Index < Source->PropertyCount; Index++)
    {
        IsBuilt = WmiStdBuildProperty(&Source->Properties[Index], &Class->Properties[Index]);
        Class->PropertyCount = Index + 1;
    }

    if (!IsBuilt)
    {
        BmofFreeClass(Class);
        return NULL;
    }
    return Class;
}

/**
 * @brief Hands out definitions of the standard classes, the caller owns each one.
 */
VOID
WmiStdEnumerateClasses(
    _In_ PBMOF_CLASS_ROUTINE Routine,
    _In_opt_ PVOID Context)
{
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(WmiStdClasses); Index++)
    {
        PWMI_CLASS_DEF Class = WmiStdBuildClass(&WmiStdClasses[Index]);

        if (Class)
            Routine(Context, Class);
    }
}
