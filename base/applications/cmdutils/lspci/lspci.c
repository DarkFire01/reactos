/*
 * PROJECT:     ReactOS lspci Command
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Lists the present PCI devices.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#define NTOS_MODE_USER
#include <ndk/cmtypes.h>
#include <conutils.h>
#include <stdlib.h>
#include <wchar.h>

#define ANY_VALUE ((ULONG)-1)

typedef struct _LSPCI_OPTIONS
{
    UINT Verbose;
    UINT Numeric;
    BOOL ShowKernel;
    BOOL ShowDomain;
    ULONG FilterBus;
    ULONG FilterSlot;
    ULONG FilterFunction;
    ULONG FilterVendor;
    ULONG FilterDevice;
    ULONG FilterClass;
} LSPCI_OPTIONS, *PLSPCI_OPTIONS;

typedef struct _LSPCI_DEVICE
{
    SP_DEVINFO_DATA DeviceInfoData;
    BOOL HasLocation;
    ULONG Bus;
    ULONG Slot;
    ULONG Function;
    BOOL HasIds;
    USHORT VendorId;
    USHORT DeviceId;
    BOOL HasSubsystem;
    USHORT SubVendorId;
    USHORT SubDeviceId;
    BOOL HasRevision;
    UCHAR Revision;
    BOOL HasClass;
    BOOL HasProgIf;
    UCHAR BaseClass;
    UCHAR SubClass;
    UCHAR ProgIf;
} LSPCI_DEVICE, *PLSPCI_DEVICE;

typedef struct _LSPCI_SUBCLASS_NAME
{
    UCHAR BaseClass;
    UCHAR SubClass;
    PCWSTR Name;
} LSPCI_SUBCLASS_NAME;

typedef struct _LSPCI_PROGIF_NAME
{
    UCHAR BaseClass;
    UCHAR SubClass;
    UCHAR ProgIf;
    PCWSTR Name;
} LSPCI_PROGIF_NAME;

typedef struct _IRQ_CONTEXT
{
    ULONG LineCount;
    ULONG Lines[16];
    ULONG MessageCount;
    ULONG MessageMin;
    ULONG MessageMax;
} IRQ_CONTEXT, *PIRQ_CONTEXT;

typedef VOID (*RESOURCE_CALLBACK)(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor,
    _Inout_ PVOID Context);

/* Base class names, indexed by the base class code */
static const PCWSTR BaseClassNames[] =
{
    L"Unclassified device",
    L"Mass storage controller",
    L"Network controller",
    L"Display controller",
    L"Multimedia controller",
    L"Memory controller",
    L"Bridge",
    L"Communication controller",
    L"Generic system peripheral",
    L"Input device controller",
    L"Docking station",
    L"Processor",
    L"Serial bus controller",
    L"Wireless controller",
    L"Intelligent controller",
    L"Satellite communications controller",
    L"Encryption controller",
    L"Signal processing controller",
    L"Processing accelerators",
    L"Non-Essential Instrumentation",
};

static const LSPCI_SUBCLASS_NAME SubClassNames[] =
{
    { 0x00, 0x00, L"Non-VGA unclassified device" },
    { 0x00, 0x01, L"VGA compatible unclassified device" },
    { 0x00, 0x05, L"Image coprocessor" },
    { 0x01, 0x00, L"SCSI storage controller" },
    { 0x01, 0x01, L"IDE interface" },
    { 0x01, 0x02, L"Floppy disk controller" },
    { 0x01, 0x03, L"IPI bus controller" },
    { 0x01, 0x04, L"RAID bus controller" },
    { 0x01, 0x05, L"ATA controller" },
    { 0x01, 0x06, L"SATA controller" },
    { 0x01, 0x07, L"Serial Attached SCSI controller" },
    { 0x01, 0x08, L"Non-Volatile memory controller" },
    { 0x01, 0x09, L"Universal Flash Storage controller" },
    { 0x01, 0x80, L"Mass storage controller" },
    { 0x02, 0x00, L"Ethernet controller" },
    { 0x02, 0x01, L"Token ring network controller" },
    { 0x02, 0x02, L"FDDI network controller" },
    { 0x02, 0x03, L"ATM network controller" },
    { 0x02, 0x04, L"ISDN controller" },
    { 0x02, 0x05, L"WorldFip controller" },
    { 0x02, 0x06, L"PICMG controller" },
    { 0x02, 0x07, L"Infiniband controller" },
    { 0x02, 0x08, L"Fabric controller" },
    { 0x02, 0x80, L"Network controller" },
    { 0x03, 0x00, L"VGA compatible controller" },
    { 0x03, 0x01, L"XGA compatible controller" },
    { 0x03, 0x02, L"3D controller" },
    { 0x03, 0x80, L"Display controller" },
    { 0x04, 0x00, L"Multimedia video controller" },
    { 0x04, 0x01, L"Multimedia audio controller" },
    { 0x04, 0x02, L"Computer telephony device" },
    { 0x04, 0x03, L"Audio device" },
    { 0x04, 0x80, L"Multimedia controller" },
    { 0x05, 0x00, L"RAM memory" },
    { 0x05, 0x01, L"FLASH memory" },
    { 0x05, 0x02, L"CXL" },
    { 0x05, 0x80, L"Memory controller" },
    { 0x06, 0x00, L"Host bridge" },
    { 0x06, 0x01, L"ISA bridge" },
    { 0x06, 0x02, L"EISA bridge" },
    { 0x06, 0x03, L"MicroChannel bridge" },
    { 0x06, 0x04, L"PCI bridge" },
    { 0x06, 0x05, L"PCMCIA bridge" },
    { 0x06, 0x06, L"NuBus bridge" },
    { 0x06, 0x07, L"CardBus bridge" },
    { 0x06, 0x08, L"RACEway bridge" },
    { 0x06, 0x09, L"Semi-transparent PCI-to-PCI bridge" },
    { 0x06, 0x0A, L"InfiniBand to PCI host bridge" },
    { 0x06, 0x80, L"Bridge" },
    { 0x07, 0x00, L"Serial controller" },
    { 0x07, 0x01, L"Parallel controller" },
    { 0x07, 0x02, L"Multiport serial controller" },
    { 0x07, 0x03, L"Modem" },
    { 0x07, 0x04, L"GPIB controller" },
    { 0x07, 0x05, L"Smart Card controller" },
    { 0x07, 0x80, L"Communication controller" },
    { 0x08, 0x00, L"PIC" },
    { 0x08, 0x01, L"DMA controller" },
    { 0x08, 0x02, L"Timer" },
    { 0x08, 0x03, L"RTC" },
    { 0x08, 0x04, L"PCI Hot-plug controller" },
    { 0x08, 0x05, L"SD Host controller" },
    { 0x08, 0x06, L"IOMMU" },
    { 0x08, 0x80, L"System peripheral" },
    { 0x09, 0x00, L"Keyboard controller" },
    { 0x09, 0x01, L"Digitizer Pen" },
    { 0x09, 0x02, L"Mouse controller" },
    { 0x09, 0x03, L"Scanner controller" },
    { 0x09, 0x04, L"Gameport controller" },
    { 0x09, 0x80, L"Input device controller" },
    { 0x0A, 0x00, L"Generic Docking Station" },
    { 0x0A, 0x80, L"Docking Station" },
    { 0x0B, 0x00, L"386" },
    { 0x0B, 0x01, L"486" },
    { 0x0B, 0x02, L"Pentium" },
    { 0x0B, 0x10, L"Alpha" },
    { 0x0B, 0x20, L"Power PC" },
    { 0x0B, 0x30, L"MIPS" },
    { 0x0B, 0x40, L"Co-processor" },
    { 0x0C, 0x00, L"FireWire (IEEE 1394)" },
    { 0x0C, 0x01, L"ACCESS Bus" },
    { 0x0C, 0x02, L"SSA" },
    { 0x0C, 0x03, L"USB controller" },
    { 0x0C, 0x04, L"Fibre Channel" },
    { 0x0C, 0x05, L"SMBus" },
    { 0x0C, 0x06, L"InfiniBand" },
    { 0x0C, 0x07, L"IPMI Interface" },
    { 0x0C, 0x08, L"SERCOS interface" },
    { 0x0C, 0x09, L"CANBUS" },
    { 0x0C, 0x80, L"Serial bus controller" },
    { 0x0D, 0x00, L"IRDA controller" },
    { 0x0D, 0x01, L"Consumer IR controller" },
    { 0x0D, 0x10, L"RF controller" },
    { 0x0D, 0x11, L"Bluetooth" },
    { 0x0D, 0x12, L"Broadband" },
    { 0x0D, 0x20, L"802.1a controller" },
    { 0x0D, 0x21, L"802.1b controller" },
    { 0x0D, 0x80, L"Wireless controller" },
    { 0x0E, 0x00, L"I2O" },
    { 0x0F, 0x01, L"Satellite TV controller" },
    { 0x0F, 0x02, L"Satellite audio communication controller" },
    { 0x0F, 0x03, L"Satellite voice communication controller" },
    { 0x0F, 0x04, L"Satellite data communication controller" },
    { 0x10, 0x00, L"Network and computing encryption device" },
    { 0x10, 0x10, L"Entertainment encryption device" },
    { 0x10, 0x80, L"Encryption controller" },
    { 0x11, 0x00, L"DPIO module" },
    { 0x11, 0x01, L"Performance counters" },
    { 0x11, 0x10, L"Communication synchronizer" },
    { 0x11, 0x20, L"Signal processing management" },
    { 0x11, 0x80, L"Signal processing controller" },
    { 0x12, 0x00, L"Processing accelerators" },
};

static const LSPCI_PROGIF_NAME ProgIfNames[] =
{
    { 0x01, 0x01, 0x00, L"ISA Compatibility mode-only controller" },
    { 0x01, 0x01, 0x05, L"PCI native mode-only controller" },
    { 0x01, 0x01, 0x0A, L"ISA Compatibility mode controller, "
                        L"supports both channels switched to PCI native mode" },
    { 0x01, 0x01, 0x0F, L"PCI native mode controller, "
                        L"supports both channels switched to ISA compatibility mode" },
    { 0x01, 0x01, 0x80, L"ISA Compatibility mode-only controller, supports bus mastering" },
    { 0x01, 0x01, 0x85, L"PCI native mode-only controller, supports bus mastering" },
    { 0x01, 0x01, 0x8A, L"ISA Compatibility mode controller, "
                        L"supports both channels switched to PCI native mode, "
                        L"supports bus mastering" },
    { 0x01, 0x01, 0x8F, L"PCI native mode controller, "
                        L"supports both channels switched to ISA compatibility mode, "
                        L"supports bus mastering" },
    { 0x01, 0x05, 0x20, L"ADMA single stepping" },
    { 0x01, 0x05, 0x30, L"ADMA continuous operation" },
    { 0x01, 0x06, 0x00, L"Vendor specific" },
    { 0x01, 0x06, 0x01, L"AHCI 1.0" },
    { 0x01, 0x06, 0x02, L"Serial Storage Bus" },
    { 0x01, 0x07, 0x01, L"Serial Storage Bus" },
    { 0x01, 0x08, 0x01, L"NVMHCI" },
    { 0x01, 0x08, 0x02, L"NVM Express" },
    { 0x03, 0x00, 0x00, L"VGA controller" },
    { 0x03, 0x00, 0x01, L"8514 controller" },
    { 0x06, 0x04, 0x00, L"Normal decode" },
    { 0x06, 0x04, 0x01, L"Subtractive decode" },
    { 0x07, 0x00, 0x00, L"8250" },
    { 0x07, 0x00, 0x01, L"16450" },
    { 0x07, 0x00, 0x02, L"16550" },
    { 0x07, 0x00, 0x03, L"16650" },
    { 0x07, 0x00, 0x04, L"16750" },
    { 0x07, 0x00, 0x05, L"16850" },
    { 0x07, 0x00, 0x06, L"16950" },
    { 0x0C, 0x00, 0x10, L"OHCI" },
    { 0x0C, 0x03, 0x00, L"UHCI" },
    { 0x0C, 0x03, 0x10, L"OHCI" },
    { 0x0C, 0x03, 0x20, L"EHCI" },
    { 0x0C, 0x03, 0x30, L"XHCI" },
    { 0x0C, 0x03, 0x40, L"USB4 Host Interface" },
    { 0x0C, 0x03, 0xFE, L"USB Device" },
};

static
PCWSTR
GetPciClassName(
    _In_ UCHAR BaseClass,
    _In_ UCHAR SubClass)
{
    UINT i;

    for (i = 0; i < _countof(SubClassNames); i++)
    {
        if (SubClassNames[i].BaseClass == BaseClass &&
            SubClassNames[i].SubClass == SubClass)
        {
            return SubClassNames[i].Name;
        }
    }

    if (BaseClass < _countof(BaseClassNames))
        return BaseClassNames[BaseClass];
    if (BaseClass == 0x40)
        return L"Coprocessor";

    return NULL;
}

static
PCWSTR
GetProgIfName(
    _In_ UCHAR BaseClass,
    _In_ UCHAR SubClass,
    _In_ UCHAR ProgIf)
{
    UINT i;

    for (i = 0; i < _countof(ProgIfNames); i++)
    {
        if (ProgIfNames[i].BaseClass == BaseClass &&
            ProgIfNames[i].SubClass == SubClass &&
            ProgIfNames[i].ProgIf == ProgIf)
        {
            return ProgIfNames[i].Name;
        }
    }

    return NULL;
}

static
BOOL
ParseHex(
    _In_reads_(Digits) PCWSTR String,
    _In_ UINT Digits,
    _Out_ PULONG Value)
{
    ULONG Result = 0;
    UINT i;

    for (i = 0; i < Digits; i++)
    {
        WCHAR Character = String[i];
        ULONG Digit;

        if (Character >= L'0' && Character <= L'9')
            Digit = Character - L'0';
        else if (Character >= L'a' && Character <= L'f')
            Digit = Character - L'a' + 10;
        else if (Character >= L'A' && Character <= L'F')
            Digit = Character - L'A' + 10;
        else
            return FALSE;

        Result = (Result << 4) | Digit;
    }

    *Value = Result;
    return TRUE;
}

/* Finds a FIELD_xxxx token like "SUBSYS_29228086" in a hardware or compatible ID list */
static
BOOL
FindIdField(
    _In_ PCZZWSTR MultiSz,
    _In_ PCWSTR Field,
    _In_ UINT Digits,
    _Out_ PULONG Value)
{
    SIZE_T FieldLength = wcslen(Field);
    PCWSTR String;
    PCWSTR Current;

    for (String = MultiSz; *String != UNICODE_NULL; String += wcslen(String) + 1)
    {
        for (Current = String; *Current != UNICODE_NULL; Current++)
        {
            WCHAR Terminator;

            if (Current != String && Current[-1] != L'\\' && Current[-1] != L'&')
                continue;

            if (_wcsnicmp(Current, Field, FieldLength) != 0)
                continue;

            if (!ParseHex(Current + FieldLength, Digits, Value))
                continue;

            Terminator = Current[FieldLength + Digits];
            if (Terminator == UNICODE_NULL || Terminator == L'&')
                return TRUE;
        }
    }

    return FALSE;
}

static
BOOL
GetStringProperty(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_ DWORD Property,
    _Out_writes_(BufferCount) PWSTR Buffer,
    _In_ DWORD BufferCount)
{
    /* Zero the whole buffer so REG_MULTI_SZ data stays double terminated */
    ZeroMemory(Buffer, BufferCount * sizeof(*Buffer));
    if (!SetupDiGetDeviceRegistryPropertyW(DeviceInfoSet,
                                           DeviceInfoData,
                                           Property,
                                           NULL,
                                           (PBYTE)Buffer,
                                           (BufferCount - 2) * sizeof(*Buffer),
                                           NULL))
    {
        Buffer[0] = UNICODE_NULL;
        Buffer[1] = UNICODE_NULL;
        return FALSE;
    }

    return TRUE;
}

static
BOOL
GetDwordProperty(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_ DWORD Property,
    _Out_ PDWORD Value)
{
    DWORD Type;
    DWORD RequiredSize;

    return SetupDiGetDeviceRegistryPropertyW(DeviceInfoSet,
                                             DeviceInfoData,
                                             Property,
                                             &Type,
                                             (PBYTE)Value,
                                             sizeof(*Value),
                                             &RequiredSize) &&
           Type == REG_DWORD &&
           RequiredSize == sizeof(*Value);
}

static
VOID
ParseDevice(
    _In_ HDEVINFO DeviceInfoSet,
    _Inout_ PLSPCI_DEVICE Device)
{
    WCHAR HardwareIds[512];
    WCHAR CompatibleIds[1024];
    DWORD BusNumber;
    DWORD Address;
    ULONG Value;
    ULONG SecondValue;

    GetStringProperty(DeviceInfoSet, &Device->DeviceInfoData, SPDRP_HARDWAREID,
                      HardwareIds, _countof(HardwareIds));
    GetStringProperty(DeviceInfoSet, &Device->DeviceInfoData, SPDRP_COMPATIBLEIDS,
                      CompatibleIds, _countof(CompatibleIds));

    if (FindIdField(HardwareIds, L"VEN_", 4, &Value) &&
        FindIdField(HardwareIds, L"DEV_", 4, &SecondValue))
    {
        Device->HasIds = TRUE;
        Device->VendorId = (USHORT)Value;
        Device->DeviceId = (USHORT)SecondValue;
    }

    /* SUBSYS_ddddvvvv has the subsystem device ID first, then the subsystem vendor ID */
    if (FindIdField(HardwareIds, L"SUBSYS_", 8, &Value))
    {
        Device->HasSubsystem = TRUE;
        Device->SubDeviceId = HIWORD(Value);
        Device->SubVendorId = LOWORD(Value);
    }

    if (FindIdField(HardwareIds, L"REV_", 2, &Value) ||
        FindIdField(CompatibleIds, L"REV_", 2, &Value))
    {
        Device->HasRevision = TRUE;
        Device->Revision = (UCHAR)Value;
    }

    /* CC_ccssii carries the programming interface, CC_ccss does not */
    if (FindIdField(CompatibleIds, L"CC_", 6, &Value) ||
        FindIdField(HardwareIds, L"CC_", 6, &Value))
    {
        Device->HasClass = TRUE;
        Device->HasProgIf = TRUE;
        Device->BaseClass = (UCHAR)(Value >> 16);
        Device->SubClass = (UCHAR)(Value >> 8);
        Device->ProgIf = (UCHAR)Value;
    }
    else if (FindIdField(CompatibleIds, L"CC_", 4, &Value) ||
             FindIdField(HardwareIds, L"CC_", 4, &Value))
    {
        Device->HasClass = TRUE;
        Device->BaseClass = (UCHAR)(Value >> 8);
        Device->SubClass = (UCHAR)Value;
    }

    /* SPDRP_ADDRESS for PCI is (device << 16) | function */
    if (GetDwordProperty(DeviceInfoSet, &Device->DeviceInfoData,
                         SPDRP_BUSNUMBER, &BusNumber) &&
        GetDwordProperty(DeviceInfoSet, &Device->DeviceInfoData,
                         SPDRP_ADDRESS, &Address))
    {
        Device->HasLocation = TRUE;
        Device->Bus = BusNumber;
        Device->Slot = (Address >> 16) & 0x1F;
        Device->Function = Address & 0x7;
    }
}

static
int
__cdecl
CompareDevices(
    _In_ const void *FirstElement,
    _In_ const void *SecondElement)
{
    const LSPCI_DEVICE *FirstDevice = FirstElement;
    const LSPCI_DEVICE *SecondDevice = SecondElement;

    if (FirstDevice->HasLocation != SecondDevice->HasLocation)
        return FirstDevice->HasLocation ? -1 : 1;
    if (FirstDevice->Bus != SecondDevice->Bus)
        return (FirstDevice->Bus < SecondDevice->Bus) ? -1 : 1;
    if (FirstDevice->Slot != SecondDevice->Slot)
        return (FirstDevice->Slot < SecondDevice->Slot) ? -1 : 1;
    if (FirstDevice->Function != SecondDevice->Function)
        return (FirstDevice->Function < SecondDevice->Function) ? -1 : 1;

    return 0;
}

static
BOOL
MatchesFilter(
    _In_ PLSPCI_OPTIONS Options,
    _In_ PLSPCI_DEVICE Device)
{
    if (Options->FilterBus != ANY_VALUE ||
        Options->FilterSlot != ANY_VALUE ||
        Options->FilterFunction != ANY_VALUE)
    {
        if (!Device->HasLocation)
            return FALSE;
        if (Options->FilterBus != ANY_VALUE && Options->FilterBus != Device->Bus)
            return FALSE;
        if (Options->FilterSlot != ANY_VALUE && Options->FilterSlot != Device->Slot)
            return FALSE;
        if (Options->FilterFunction != ANY_VALUE &&
            Options->FilterFunction != Device->Function)
        {
            return FALSE;
        }
    }

    if (Options->FilterVendor != ANY_VALUE || Options->FilterDevice != ANY_VALUE)
    {
        if (!Device->HasIds)
            return FALSE;
        if (Options->FilterVendor != ANY_VALUE && Options->FilterVendor != Device->VendorId)
            return FALSE;
        if (Options->FilterDevice != ANY_VALUE && Options->FilterDevice != Device->DeviceId)
            return FALSE;
    }

    if (Options->FilterClass != ANY_VALUE)
    {
        if (!Device->HasClass)
            return FALSE;
        if (Options->FilterClass != (((ULONG)Device->BaseClass << 8) | Device->SubClass))
            return FALSE;
    }

    return TRUE;
}

/* Prints a size the same way Linux lspci does, e.g. "[size=16M]" */
static
VOID
PrintSize(
    _In_ ULONGLONG Size)
{
    static const PCWSTR Suffixes[] = { L"", L"K", L"M", L"G", L"T" };
    UINT i;

    if (Size == 0)
        return;

    for (i = 0; i < _countof(Suffixes) - 1; i++)
    {
        if (Size % 1024)
            break;
        Size /= 1024;
    }

    ConPrintf(StdOut, L" [size=%I64u%s]", Size, Suffixes[i]);
}

static
PCM_RESOURCE_LIST
GetAllocatedResources(
    _In_ PCWSTR InstanceId,
    _Out_ PDWORD ListSize)
{
    WCHAR KeyName[MAX_DEVICE_ID_LEN + 64];
    PCM_RESOURCE_LIST ResourceList = NULL;
    HKEY hKey;
    DWORD Size = 0;
    DWORD Type;

    *ListSize = 0;

    _snwprintf(KeyName, _countof(KeyName),
               L"SYSTEM\\CurrentControlSet\\Enum\\%s\\Control", InstanceId);
    KeyName[_countof(KeyName) - 1] = UNICODE_NULL;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, KeyName, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return NULL;

    if (RegQueryValueExW(hKey, L"AllocConfig", NULL, &Type, NULL, &Size) == ERROR_SUCCESS &&
        Type == REG_RESOURCE_LIST &&
        Size >= FIELD_OFFSET(CM_RESOURCE_LIST, List))
    {
        ResourceList = HeapAlloc(GetProcessHeap(), 0, Size);
        if (ResourceList &&
            RegQueryValueExW(hKey, L"AllocConfig", NULL, NULL,
                             (PBYTE)ResourceList, &Size) != ERROR_SUCCESS)
        {
            HeapFree(GetProcessHeap(), 0, ResourceList);
            ResourceList = NULL;
        }
    }

    RegCloseKey(hKey);

    if (ResourceList)
        *ListSize = Size;

    return ResourceList;
}

/* Walks every partial descriptor of a resource list, with bounds checking */
static
VOID
ForEachResource(
    _In_reads_bytes_(ListSize) PCM_RESOURCE_LIST ResourceList,
    _In_ DWORD ListSize,
    _In_ RESOURCE_CALLBACK Callback,
    _Inout_ PVOID Context)
{
    PBYTE End = (PBYTE)ResourceList + ListSize;
    PCM_FULL_RESOURCE_DESCRIPTOR FullDescriptor = ResourceList->List;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    ULONG i;
    ULONG j;

    for (i = 0; i < ResourceList->Count; i++)
    {
        if ((PBYTE)FullDescriptor +
            FIELD_OFFSET(CM_FULL_RESOURCE_DESCRIPTOR, PartialResourceList.PartialDescriptors) > End)
        {
            return;
        }

        Descriptor = FullDescriptor->PartialResourceList.PartialDescriptors;
        for (j = 0; j < FullDescriptor->PartialResourceList.Count; j++)
        {
            if ((PBYTE)(Descriptor + 1) > End)
                return;

            Callback(Descriptor, Context);

            /* Device specific data immediately follows its descriptor */
            if (Descriptor->Type == CmResourceTypeDeviceSpecific)
            {
                Descriptor = (PCM_PARTIAL_RESOURCE_DESCRIPTOR)
                    ((PBYTE)(Descriptor + 1) + Descriptor->u.DeviceSpecificData.DataSize);
            }
            else
            {
                Descriptor++;
            }
        }

        FullDescriptor = (PCM_FULL_RESOURCE_DESCRIPTOR)Descriptor;
    }
}

static
VOID
CollectIrqCallback(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor,
    _Inout_ PVOID Context)
{
    PIRQ_CONTEXT IrqContext = Context;
    ULONG Vector = Descriptor->u.Interrupt.Vector;
    ULONG i;

    if (Descriptor->Type != CmResourceTypeInterrupt)
        return;

    /* Message signaled vectors are not IRQ lines, they are shown in hex */
    if (Descriptor->Flags & CM_RESOURCE_INTERRUPT_MESSAGE)
    {
        if (IrqContext->MessageCount == 0 || Vector < IrqContext->MessageMin)
            IrqContext->MessageMin = Vector;
        if (IrqContext->MessageCount == 0 || Vector > IrqContext->MessageMax)
            IrqContext->MessageMax = Vector;
        IrqContext->MessageCount++;
        return;
    }

    for (i = 0; i < IrqContext->LineCount; i++)
    {
        if (IrqContext->Lines[i] == Vector)
            return;
    }

    if (IrqContext->LineCount < _countof(IrqContext->Lines))
        IrqContext->Lines[IrqContext->LineCount++] = Vector;
}

/* Prints the Linux style "Flags:" line */
static
VOID
PrintFlags(
    _In_opt_ PCM_RESOURCE_LIST ResourceList,
    _In_ DWORD ListSize,
    _In_ BOOL IsDisabled)
{
    IRQ_CONTEXT IrqContext = { 0 };
    BOOL IsFirst = TRUE;
    ULONG i;

    if (ResourceList)
        ForEachResource(ResourceList, ListSize, CollectIrqCallback, &IrqContext);

    if (!IsDisabled && IrqContext.LineCount == 0 && IrqContext.MessageCount == 0)
        return;

    ConPuts(StdOut, L"\tFlags: ");

    if (IsDisabled)
    {
        ConPuts(StdOut, L"disabled");
        IsFirst = FALSE;
    }

    for (i = 0; i < IrqContext.LineCount; i++)
    {
        ConPrintf(StdOut, L"%sIRQ %lu", IsFirst ? L"" : L", ", IrqContext.Lines[i]);
        IsFirst = FALSE;
    }

    if (IrqContext.MessageCount == 1)
    {
        ConPrintf(StdOut, L"%sMSI 0x%08lx", IsFirst ? L"" : L", ", IrqContext.MessageMin);
    }
    else if (IrqContext.MessageCount > 1)
    {
        /* Vectors are normally allocated as one contiguous block */
        if (IrqContext.MessageMax - IrqContext.MessageMin + 1 == IrqContext.MessageCount)
        {
            ConPrintf(StdOut, L"%sMSI 0x%08lx-0x%08lx", IsFirst ? L"" : L", ",
                      IrqContext.MessageMin, IrqContext.MessageMax);
        }
        else
        {
            ConPrintf(StdOut, L"%sMSI", IsFirst ? L"" : L", ");
        }
        ConPrintf(StdOut, L" (%lu vectors)", IrqContext.MessageCount);
    }

    ConPuts(StdOut, L"\n");
}

static
VOID
PrintRangeCallback(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor,
    _Inout_ PVOID Context)
{
    PLSPCI_DEVICE Device = Context;
    ULONGLONG Length;

    switch (Descriptor->Type)
    {
        case CmResourceTypeMemory:
        case CmResourceTypeMemoryLarge:
        {
            Length = Descriptor->u.Memory.Length;
            if (Descriptor->Type == CmResourceTypeMemoryLarge)
            {
                if (Descriptor->Flags & CM_RESOURCE_MEMORY_LARGE_40)
                    Length <<= 8;
                else if (Descriptor->Flags & CM_RESOURCE_MEMORY_LARGE_48)
                    Length <<= 16;
                else if (Descriptor->Flags & CM_RESOURCE_MEMORY_LARGE_64)
                    Length <<= 32;
            }

            ConPrintf(StdOut, L"\tMemory at %08I64x (%s%s)",
                      Descriptor->u.Memory.Start.QuadPart,
                      (Descriptor->Flags & CM_RESOURCE_MEMORY_PREFETCHABLE) ?
                          L"prefetchable" : L"non-prefetchable",
                      ((Descriptor->Flags & CM_RESOURCE_MEMORY_WRITEABILITY_MASK) ==
                          CM_RESOURCE_MEMORY_READ_ONLY) ? L", read-only" : L"");
            PrintSize(Length);
            ConPuts(StdOut, L"\n");
            break;
        }

        case CmResourceTypePort:
        {
            ConPrintf(StdOut, L"\tI/O ports at %04I64x", Descriptor->u.Port.Start.QuadPart);
            PrintSize(Descriptor->u.Port.Length);
            ConPuts(StdOut, L"\n");
            break;
        }

        case CmResourceTypeDma:
            ConPrintf(StdOut, L"\tDMA channel %lu\n", Descriptor->u.Dma.Channel);
            break;

        case CmResourceTypeBusNumber:
        {
            if (Descriptor->u.BusNumber.Length == 0)
                break;

            if (Device->HasLocation)
                ConPrintf(StdOut, L"\tBus: primary=%02lx, ", Device->Bus);
            else
                ConPuts(StdOut, L"\tBus: ");

            ConPrintf(StdOut, L"secondary=%02lx, subordinate=%02lx\n",
                      Descriptor->u.BusNumber.Start,
                      Descriptor->u.BusNumber.Start + Descriptor->u.BusNumber.Length - 1);
            break;
        }
    }
}

static
PCWSTR
GetProblemName(
    _In_ ULONG Problem)
{
    switch (Problem)
    {
        case CM_PROB_NOT_CONFIGURED:
            return L"not configured";
        case CM_PROB_FAILED_START:
            return L"failed to start";
        case CM_PROB_NORMAL_CONFLICT:
            return L"resource conflict";
        case CM_PROB_DISABLED:
            return L"disabled";
        case CM_PROB_FAILED_INSTALL:
            return L"driver not installed";
        case CM_PROB_FAILED_ADD:
        case CM_PROB_DRIVER_FAILED_LOAD:
            return L"driver failed to load";
        case CM_PROB_DRIVER_SERVICE_KEY_INVALID:
            return L"driver service key invalid";
        case CM_PROB_FAILED_DRIVER_ENTRY:
            return L"driver entry failed";
        case CM_PROB_HARDWARE_DISABLED:
            return L"hardware disabled";
        default:
            return NULL;
    }
}

static
VOID
PrintMultiSzList(
    _In_ PCZZWSTR MultiSz,
    _Inout_ PBOOL IsFirst)
{
    PCWSTR Current;

    for (Current = MultiSz; *Current != UNICODE_NULL; Current += wcslen(Current) + 1)
    {
        ConPrintf(StdOut, L"%s%s", *IsFirst ? L"" : L", ", Current);
        *IsFirst = FALSE;
    }
}

static
VOID
PrintDriverImage(
    _In_ PCWSTR Service)
{
    WCHAR KeyName[MAX_PATH];
    WCHAR ImagePath[MAX_PATH];
    HKEY hKey;
    DWORD Size = sizeof(ImagePath) - sizeof(*ImagePath);
    DWORD Type;

    _snwprintf(KeyName, _countof(KeyName),
               L"SYSTEM\\CurrentControlSet\\Services\\%s", Service);
    KeyName[_countof(KeyName) - 1] = UNICODE_NULL;

    ZeroMemory(ImagePath, sizeof(ImagePath));
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, KeyName, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(hKey, L"ImagePath", NULL, &Type,
                             (PBYTE)ImagePath, &Size) != ERROR_SUCCESS ||
            (Type != REG_SZ && Type != REG_EXPAND_SZ))
        {
            ImagePath[0] = UNICODE_NULL;
        }
        RegCloseKey(hKey);
    }

    /* Drivers without an ImagePath are loaded from the default location */
    if (ImagePath[0] != UNICODE_NULL)
        ConPrintf(StdOut, L"\tDriver image: %s\n", ImagePath);
    else
        ConPrintf(StdOut, L"\tDriver image: System32\\drivers\\%s.sys\n", Service);
}

static
VOID
PrintDeviceHeader(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PLSPCI_OPTIONS Options,
    _In_ PLSPCI_DEVICE Device)
{
    WCHAR Description[512];
    PCWSTR ClassName = NULL;
    PCWSTR ProgIfName;
    BOOL HasDescription;

    GetStringProperty(DeviceInfoSet, &Device->DeviceInfoData, SPDRP_FRIENDLYNAME,
                      Description, _countof(Description));
    if (Description[0] == UNICODE_NULL)
    {
        GetStringProperty(DeviceInfoSet, &Device->DeviceInfoData, SPDRP_DEVICEDESC,
                          Description, _countof(Description));
    }
    HasDescription = (Description[0] != UNICODE_NULL);

    /* Location */
    if (Options->ShowDomain)
        ConPuts(StdOut, L"0000:");

    if (Device->HasLocation)
        ConPrintf(StdOut, L"%02lx:%02lx.%lx ", Device->Bus, Device->Slot, Device->Function);
    else
        ConPuts(StdOut, L"??:??.? ");

    /* Class */
    if (Device->HasClass)
        ClassName = GetPciClassName(Device->BaseClass, Device->SubClass);

    if (!Device->HasClass)
    {
        ConPuts(StdOut, L"Unknown class");
    }
    else if (Options->Numeric == 1 || !ClassName)
    {
        ConPrintf(StdOut, L"%s%02x%02x",
                  (Options->Numeric == 1) ? L"" : L"Class ",
                  Device->BaseClass, Device->SubClass);
    }
    else if (Options->Numeric >= 2)
    {
        ConPrintf(StdOut, L"%s [%02x%02x]", ClassName, Device->BaseClass, Device->SubClass);
    }
    else
    {
        ConPuts(StdOut, ClassName);
    }

    ConPuts(StdOut, L": ");

    /* Vendor and device */
    if (Device->HasIds && (Options->Numeric == 1 || !HasDescription))
    {
        ConPrintf(StdOut, L"%s%04x:%04x",
                  (Options->Numeric == 1) ? L"" : L"Device ",
                  Device->VendorId, Device->DeviceId);
    }
    else
    {
        ConPuts(StdOut, HasDescription ? Description : L"Unknown device");
        if (Options->Numeric >= 2 && Device->HasIds)
            ConPrintf(StdOut, L" [%04x:%04x]", Device->VendorId, Device->DeviceId);
    }

    if (Device->HasRevision && Device->Revision != 0)
        ConPrintf(StdOut, L" (rev %02x)", Device->Revision);

    if (Options->Verbose && Device->HasProgIf)
    {
        ProgIfName = GetProgIfName(Device->BaseClass, Device->SubClass, Device->ProgIf);
        if (Device->ProgIf != 0 || ProgIfName)
        {
            ConPrintf(StdOut, L" (prog-if %02x", Device->ProgIf);
            if (ProgIfName && Options->Numeric != 1)
                ConPrintf(StdOut, L" [%s]", ProgIfName);
            ConPuts(StdOut, L")");
        }
    }

    ConPuts(StdOut, L"\n");
}

static
VOID
PrintKernelDrivers(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PLSPCI_OPTIONS Options,
    _In_ PLSPCI_DEVICE Device,
    _In_ BOOL IsStarted)
{
    WCHAR Service[256];
    WCHAR LowerFilters[512];
    WCHAR UpperFilters[512];
    BOOL HasService;
    BOOL IsFirst = TRUE;

    GetStringProperty(DeviceInfoSet, &Device->DeviceInfoData, SPDRP_SERVICE,
                      Service, _countof(Service));
    GetStringProperty(DeviceInfoSet, &Device->DeviceInfoData, SPDRP_LOWERFILTERS,
                      LowerFilters, _countof(LowerFilters));
    GetStringProperty(DeviceInfoSet, &Device->DeviceInfoData, SPDRP_UPPERFILTERS,
                      UpperFilters, _countof(UpperFilters));
    HasService = (Service[0] != UNICODE_NULL);

    if (HasService && IsStarted)
        ConPrintf(StdOut, L"\tKernel driver in use: %s\n", Service);

    /* The full driver stack, from the bottom up */
    if (HasService ||
        LowerFilters[0] != UNICODE_NULL ||
        UpperFilters[0] != UNICODE_NULL)
    {
        ConPuts(StdOut, L"\tKernel modules: ");
        PrintMultiSzList(LowerFilters, &IsFirst);
        if (HasService)
        {
            ConPrintf(StdOut, L"%s%s", IsFirst ? L"" : L", ", Service);
            IsFirst = FALSE;
        }
        PrintMultiSzList(UpperFilters, &IsFirst);
        ConPuts(StdOut, L"\n");
    }

    if (Options->Verbose >= 2 && HasService)
        PrintDriverImage(Service);
}

static
VOID
PrintDeviceStatus(
    _In_ ULONG Status,
    _In_ ULONG Problem)
{
    PCWSTR ProblemName;

    if (!(Status & DN_HAS_PROBLEM))
    {
        ConPuts(StdOut, (Status & DN_STARTED) ? L"\tStatus: started\n" :
                                                L"\tStatus: not started\n");
        return;
    }

    ConPrintf(StdOut, L"\tStatus: problem %lu", Problem);
    ProblemName = GetProblemName(Problem);
    if (ProblemName)
        ConPrintf(StdOut, L" (%s)", ProblemName);
    ConPuts(StdOut, L"\n");
}

static
VOID
PrintDevice(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PLSPCI_OPTIONS Options,
    _In_ PLSPCI_DEVICE Device)
{
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    PCM_RESOURCE_LIST ResourceList = NULL;
    DWORD ListSize = 0;
    ULONG Status = 0;
    ULONG Problem = 0;
    BOOL HasStatus;
    BOOL IsStarted;
    BOOL IsDisabled;

    PrintDeviceHeader(DeviceInfoSet, Options, Device);

    if (!Options->Verbose && !Options->ShowKernel)
        return;

    HasStatus = (CM_Get_DevNode_Status(&Status, &Problem,
                                       Device->DeviceInfoData.DevInst, 0) == CR_SUCCESS);
    IsStarted = HasStatus && (Status & DN_STARTED);
    IsDisabled = HasStatus && (Status & DN_HAS_PROBLEM) && Problem == CM_PROB_DISABLED;

    if (Options->Verbose)
    {
        if (!SetupDiGetDeviceInstanceIdW(DeviceInfoSet, &Device->DeviceInfoData,
                                         InstanceId, _countof(InstanceId), NULL))
        {
            InstanceId[0] = UNICODE_NULL;
        }

        if (InstanceId[0] != UNICODE_NULL)
            ResourceList = GetAllocatedResources(InstanceId, &ListSize);

        if (Device->HasSubsystem && (Device->SubVendorId != 0 || Device->SubDeviceId != 0))
        {
            ConPrintf(StdOut, L"\tSubsystem: %s%04x:%04x\n",
                      (Options->Numeric == 1) ? L"" : L"Device ",
                      Device->SubVendorId, Device->SubDeviceId);
        }

        PrintFlags(ResourceList, ListSize, IsDisabled);

        if (ResourceList)
        {
            ForEachResource(ResourceList, ListSize, PrintRangeCallback, Device);
            HeapFree(GetProcessHeap(), 0, ResourceList);
        }

        if (Options->Verbose >= 2 && InstanceId[0] != UNICODE_NULL)
            ConPrintf(StdOut, L"\tDevice instance: %s\n", InstanceId);

        if (Options->Verbose >= 2 && HasStatus)
            PrintDeviceStatus(Status, Problem);
    }

    PrintKernelDrivers(DeviceInfoSet, Options, Device, IsStarted);

    if (Options->Verbose)
        ConPuts(StdOut, L"\n");
}

static
VOID
PrintUsage(
    _In_ PCON_STREAM Stream)
{
    ConPuts(Stream,
            L"Usage: lspci [<switches>]\n"
            L"\n"
            L"Basic display modes:\n"
            L"  -n\t\tShow numeric ID's\n"
            L"  -nn\t\tShow both textual and numeric ID's (names & numbers)\n"
            L"\n"
            L"Display options:\n"
            L"  -v\t\tBe verbose (-vv for even more details)\n"
            L"  -k\t\tShow kernel drivers handling each device\n"
            L"  -D\t\tAlways show domain numbers\n"
            L"\n"
            L"Selection of devices:\n"
            L"  -s [[[[<domain>]:]<bus>]:][<slot>][.[<func>]]\n"
            L"\t\tShow only devices in selected slots\n"
            L"  -d [<vendor>]:[<device>][:<class>]\n"
            L"\t\tShow only devices with specified ID's\n"
            L"\n"
            L"Verbose output (-v) shows the subsystem, IRQs, memory and I/O ranges\n"
            L"and the kernel drivers bound to each device.\n");
}

/* Parses an optional hex value ending at End, where empty or "*" means any value */
static
BOOL
ParseFilterValue(
    _In_ PCWSTR Start,
    _In_ PCWSTR End,
    _In_ ULONG Max,
    _Out_ PULONG Value)
{
    ULONG Result = 0;
    ULONG Digit;
    PCWSTR Current;

    if (Start == End || (End - Start == 1 && *Start == L'*'))
    {
        *Value = ANY_VALUE;
        return TRUE;
    }

    if (End - Start > 8)
        return FALSE;

    for (Current = Start; Current < End; Current++)
    {
        if (!ParseHex(Current, 1, &Digit))
            return FALSE;
        Result = (Result << 4) | Digit;
    }

    if (Result > Max)
        return FALSE;

    *Value = Result;
    return TRUE;
}

static
BOOL
ParseSlotFilter(
    _In_ PCWSTR Argument,
    _Inout_ PLSPCI_OPTIONS Options)
{
    PCWSTR End = Argument + wcslen(Argument);
    PCWSTR Dot = wcschr(Argument, L'.');
    PCWSTR SlotEnd = Dot ? Dot : End;
    PCWSTR Colon = NULL;
    PCWSTR Current;
    ULONG Domain;

    if (Dot && !ParseFilterValue(Dot + 1, End, 7, &Options->FilterFunction))
        return FALSE;

    for (Current = Argument; Current < SlotEnd; Current++)
    {
        if (*Current == L':')
            Colon = Current;
    }

    if (!Colon)
        return ParseFilterValue(Argument, SlotEnd, 0x1F, &Options->FilterSlot);

    if (!ParseFilterValue(Colon + 1, SlotEnd, 0x1F, &Options->FilterSlot))
        return FALSE;

    /* Look for a domain in front of the bus */
    for (Current = Colon - 1; Current >= Argument; Current--)
    {
        if (*Current != L':')
            continue;

        /* Only PCI domain 0 is supported */
        if (!ParseFilterValue(Argument, Current, 0xFFFF, &Domain) ||
            (Domain != ANY_VALUE && Domain != 0))
        {
            return FALSE;
        }

        return ParseFilterValue(Current + 1, Colon, 0xFF, &Options->FilterBus);
    }

    return ParseFilterValue(Argument, Colon, 0xFF, &Options->FilterBus);
}

static
BOOL
ParseIdFilter(
    _In_ PCWSTR Argument,
    _Inout_ PLSPCI_OPTIONS Options)
{
    PCWSTR End = Argument + wcslen(Argument);
    PCWSTR Colon = wcschr(Argument, L':');
    PCWSTR SecondColon;

    if (!Colon)
        return FALSE;

    if (!ParseFilterValue(Argument, Colon, 0xFFFF, &Options->FilterVendor))
        return FALSE;

    SecondColon = wcschr(Colon + 1, L':');
    if (!SecondColon)
        return ParseFilterValue(Colon + 1, End, 0xFFFF, &Options->FilterDevice);

    return ParseFilterValue(Colon + 1, SecondColon, 0xFFFF, &Options->FilterDevice) &&
           ParseFilterValue(SecondColon + 1, End, 0xFFFF, &Options->FilterClass);
}

static
BOOL
ParseArguments(
    _In_ int argc,
    _In_reads_(argc) WCHAR *argv[],
    _Out_ PLSPCI_OPTIONS Options,
    _Out_ PBOOL ShowHelp)
{
    PCWSTR Argument;
    PCWSTR Value;
    WCHAR Switch;
    int i;

    ZeroMemory(Options, sizeof(*Options));
    Options->FilterBus = ANY_VALUE;
    Options->FilterSlot = ANY_VALUE;
    Options->FilterFunction = ANY_VALUE;
    Options->FilterVendor = ANY_VALUE;
    Options->FilterDevice = ANY_VALUE;
    Options->FilterClass = ANY_VALUE;
    *ShowHelp = FALSE;

    for (i = 1; i < argc; i++)
    {
        Argument = argv[i];

        if (wcscmp(Argument, L"/?") == 0 ||
            wcscmp(Argument, L"--help") == 0 ||
            wcscmp(Argument, L"-h") == 0)
        {
            *ShowHelp = TRUE;
            return TRUE;
        }

        if (Argument[0] != L'-' || Argument[1] == UNICODE_NULL)
        {
            ConPrintf(StdErr, L"lspci: invalid argument '%s'\n", Argument);
            return FALSE;
        }

        /* Single letter switches may be combined, e.g. "-vvnn" or "-ks 00:1f" */
        for (Argument++; *Argument != UNICODE_NULL; Argument++)
        {
            Switch = *Argument;

            if (Switch == L'v')
            {
                if (Options->Verbose < 2)
                    Options->Verbose++;
                continue;
            }

            if (Switch == L'n')
            {
                if (Options->Numeric < 2)
                    Options->Numeric++;
                continue;
            }

            if (Switch == L'k')
            {
                Options->ShowKernel = TRUE;
                continue;
            }

            if (Switch == L'D')
            {
                Options->ShowDomain = TRUE;
                continue;
            }

            if (Switch != L's' && Switch != L'd')
            {
                ConPrintf(StdErr, L"lspci: invalid option: -%c\n", Switch);
                return FALSE;
            }

            /* The value is either the rest of this argument or the next one */
            if (Argument[1] != UNICODE_NULL)
            {
                Value = Argument + 1;
            }
            else if (i + 1 < argc)
            {
                Value = argv[++i];
            }
            else
            {
                ConPrintf(StdErr, L"lspci: option requires an argument: -%c\n", Switch);
                return FALSE;
            }

            if (Switch == L's' && !ParseSlotFilter(Value, Options))
            {
                ConPrintf(StdErr, L"lspci: -s: Invalid slot number '%s'\n", Value);
                return FALSE;
            }

            if (Switch == L'd' && !ParseIdFilter(Value, Options))
            {
                ConPrintf(StdErr, L"lspci: -d: Invalid ID '%s'\n", Value);
                return FALSE;
            }

            /* -s and -d consume the rest of the argument */
            break;
        }
    }

    return TRUE;
}

int
wmain(
    _In_ int argc,
    _In_reads_(argc) WCHAR *argv[])
{
    LSPCI_OPTIONS Options;
    HDEVINFO DeviceInfoSet;
    SP_DEVINFO_DATA DeviceInfoData;
    PLSPCI_DEVICE Devices = NULL;
    PLSPCI_DEVICE NewDevices;
    DWORD Count = 0;
    DWORD Capacity = 0;
    DWORD NewCapacity;
    DWORD Index;
    DWORD Error;
    BOOL ShowHelp;
    int Result = 0;

    ConInitStdStreams();

    if (!ParseArguments(argc, argv, &Options, &ShowHelp))
    {
        ConPuts(StdErr, L"Try 'lspci --help' for more information.\n");
        return 1;
    }

    if (ShowHelp)
    {
        PrintUsage(StdOut);
        return 0;
    }

    DeviceInfoSet = SetupDiGetClassDevsW(NULL, L"PCI", NULL,
                                         DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        ConPrintf(StdErr, L"lspci: unable to enumerate PCI devices (error %lu)\n",
                  GetLastError());
        return 1;
    }

    for (Index = 0; ; Index++)
    {
        DeviceInfoData.cbSize = sizeof(DeviceInfoData);
        if (!SetupDiEnumDeviceInfo(DeviceInfoSet, Index, &DeviceInfoData))
        {
            Error = GetLastError();
            if (Error == ERROR_NO_MORE_ITEMS)
                break;

            ConPrintf(StdErr, L"lspci: unable to enumerate PCI devices (error %lu)\n", Error);
            Result = 1;
            goto Cleanup;
        }

        if (Count == Capacity)
        {
            NewCapacity = Capacity ? Capacity * 2 : 32;
            if (Devices)
            {
                NewDevices = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         Devices, NewCapacity * sizeof(*Devices));
            }
            else
            {
                NewDevices = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                       NewCapacity * sizeof(*Devices));
            }

            if (!NewDevices)
            {
                ConPuts(StdErr, L"lspci: out of memory\n");
                Result = 1;
                goto Cleanup;
            }

            Devices = NewDevices;
            Capacity = NewCapacity;
        }

        Devices[Count].DeviceInfoData = DeviceInfoData;
        ParseDevice(DeviceInfoSet, &Devices[Count]);
        if (MatchesFilter(&Options, &Devices[Count]))
            Count++;
        else
            ZeroMemory(&Devices[Count], sizeof(*Devices));
    }

    /* Show the devices in bus order, like Linux does */
    if (Count != 0)
        qsort(Devices, Count, sizeof(*Devices), CompareDevices);

    for (Index = 0; Index < Count; Index++)
        PrintDevice(DeviceInfoSet, &Options, &Devices[Index]);

Cleanup:
    if (Devices)
        HeapFree(GetProcessHeap(), 0, Devices);
    SetupDiDestroyDeviceInfoList(DeviceInfoSet);
    return Result;
}
