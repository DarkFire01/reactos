/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Driver entry, registry policy, connection manager IDs and shim flags
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4hr.h"

#define NDEBUG
#include <debug.h>

#define USB4HR_CODE_INTEGRITY_CLASS     103
#define USB4HR_CODE_INTEGRITY_TESTSIGN  0x00000002

#define USB4HR_CMID_COUNT               8

/* Shim engine provider and the length of every key built for it */
#define USB4HR_SHIM_PROVIDER            L"USB4HOSTROUTER"
#define USB4HR_SHIM_KEY_CHARS           70

/* Device key value ORed into the shim flags; ReactOS has no shim database */
#define USB4HR_SHIM_OVERRIDE_VALUE      L"Usb4HostRouterShimFlags"

typedef struct _USB4HR_CODE_INTEGRITY
{
    ULONG Length;
    ULONG CodeIntegrityOptions;
} USB4HR_CODE_INTEGRITY;

typedef NTSTATUS
(NTAPI *PFN_USB4HR_QUERY_DEVICE_FLAGS)(
    _In_ PCWSTR DeviceKey,
    _In_ PCWSTR Provider,
    _Out_ PULONG64 Flags);

/** One quirk entry; a device ID of 0xFFFF matches every device of the vendor. */
struct Usb4HrShimEntry
{
    USHORT VendorId;
    USHORT DeviceId;
    ULONG64 Flags;
};

/* Known host routers that need quirks; the list ends with a zero vendor */
static const Usb4HrShimEntry Usb4HrShimTable[] =
{
    { 0, 0, 0 }
};

extern "C"
NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
    _In_ ULONG SystemInformationClass,
    _Out_writes_bytes_opt_(Length) PVOID SystemInformation,
    _In_ ULONG Length,
    _Out_opt_ PULONG ReturnLength);

Usb4HrDriverData Usb4HrDriver;

static
BOOLEAN
NTAPI
Usb4HrIsTestSigningOn(VOID)
{
    USB4HR_CODE_INTEGRITY Info;
    NTSTATUS Status;

    Info.Length = sizeof(Info);
    Info.CodeIntegrityOptions = 0;

    Status = ZwQuerySystemInformation(USB4HR_CODE_INTEGRITY_CLASS, &Info, sizeof(Info), NULL);
    if (!NT_SUCCESS(Status))
        return FALSE;

    return (Info.CodeIntegrityOptions & USB4HR_CODE_INTEGRITY_TESTSIGN) != 0;
}

/** Reads one REG_DWORD; Present tells whether the value exists. */
static
ULONG
NTAPI
Usb4HrReadPolicyValue(
    _In_ WDFKEY Key,
    _In_ PCWSTR Name,
    _Out_opt_ PBOOLEAN Present)
{
    UNICODE_STRING ValueName;
    ULONG Value = 0;
    NTSTATUS Status;

    RtlInitUnicodeString(&ValueName, Name);
    Status = WdfRegistryQueryULong(Key, &ValueName, &Value);
    if (!NT_SUCCESS(Status))
    {
        if (Status != STATUS_OBJECT_NAME_NOT_FOUND)
            DPRINT1("Reading %ws failed 0x%lx\n", Name, Status);
        Value = 0;
    }

    if (Present)
        *Present = NT_SUCCESS(Status);

    return Value;
}

static
VOID
NTAPI
Usb4HrReadPolicy(
    _In_ WDFDRIVER Driver)
{
    Usb4HrPolicy* Policy = &Usb4HrDriver.Policy;
    BOOLEAN TestSigning = Usb4HrDriver.TestSigning;
    BOOLEAN Present;
    WDFKEY Key;
    ULONG Value;
    NTSTATUS Status;

    Status = WdfDriverOpenParametersRegistryKey(Driver, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &Key);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("No driver Parameters key 0x%lx\n", Status);
        return;
    }

    /* Most knobs only count on a test signed system */
    Policy->DriverDisableable = TestSigning && Usb4HrReadPolicyValue(Key, L"EnableDriverDisableable", NULL) != 0;

    Value = Usb4HrReadPolicyValue(Key, L"EnableUSB4v2Support", &Present);
    if (Present)
    {
        if (TestSigning)
        {
            Policy->Usb4V2Configured = TRUE;
            Policy->Usb4V2Enabled = (Value != 0);
        }
        else
        {
            DPRINT1("EnableUSB4v2Support ignored without test signing\n");
        }
    }

    Policy->BugcheckOnDuplicateGfxRef = Usb4HrReadPolicyValue(Key, L"BugcheckOnDuplicateGfxRef", NULL) != 0;
    Policy->DisableClxDomainWide = TestSigning && Usb4HrReadPolicyValue(Key, L"DisableCLxDomainWide", NULL) != 0;
    Policy->UserIdleControl = TestSigning && Usb4HrReadPolicyValue(Key, L"UserControlOfIdleSettings", NULL) != 0;
    Policy->PerHostRouterDomainUuid = TestSigning &&
                                      Usb4HrReadPolicyValue(Key, L"UsePerHostRouterDomainUUID", NULL) != 0;

    if (TestSigning && Usb4HrReadPolicyValue(Key, L"EnableDebugInterface", NULL) != 0)
        DPRINT1("EnableDebugInterface is set; the debug interface is not supported\n");

    WdfRegistryClose(Key);
}

extern "C"
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING RoutineName;
    WDF_DRIVER_CONFIG Config;
    WDFDRIVER Driver;
    NTSTATUS Status;

    Usb4HrDriver.DriverObject = DriverObject;

    /* Resolved at run time so kernels without the export still load us */
    RtlInitUnicodeString(&RoutineName, L"KseQueryDeviceFlags");
    Usb4HrDriver.QueryDeviceFlags = MmGetSystemRoutineAddress(&RoutineName);

    WDF_DRIVER_CONFIG_INIT(&Config, Usb4HrEvtDeviceAdd);
    Config.EvtDriverUnload = Usb4HrEvtDriverUnload;
    Config.DriverPoolTag = USB4HR_TAG_DRIVER;

    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, &Driver);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WdfDriverCreate failed 0x%lx\n", Status);
        return Status;
    }

    /* A failure leaves the zero UUID; Windows only traces it as well */
    Status = ExUuidCreate(&Usb4HrDriver.SystemDomainUuid);
    if (!NT_SUCCESS(Status))
        DPRINT1("Domain UUID creation failed 0x%lx\n", Status);

    Usb4HrDriver.TestSigning = Usb4HrIsTestSigningOn();
    if (Usb4HrDriver.TestSigning)
        DPRINT1("Test signing is on, test policy values honored\n");

    Usb4HrReadPolicy(Driver);
    return STATUS_SUCCESS;
}

VOID
NTAPI
Usb4HrEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    DPRINT("USB4 host router driver unloading\n");
}

NTSTATUS
NTAPI
Usb4HrAllocateCmid(
    _Out_ PUCHAR Cmid)
{
    LONG Bit;

    for (Bit = 0; Bit < USB4HR_CMID_COUNT; Bit++)
    {
        if (!InterlockedBitTestAndSet(&Usb4HrDriver.CmidBitmap, Bit))
        {
            *Cmid = (UCHAR)Bit;
            return STATUS_SUCCESS;
        }
    }

    *Cmid = 0;
    DPRINT1("Every connection manager ID is in use\n");
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

VOID
NTAPI
Usb4HrReleaseCmid(
    _In_ UCHAR Cmid)
{
    if (Cmid >= USB4HR_CMID_COUNT)
    {
        DPRINT1("Releasing bad connection manager ID %u\n", Cmid);
        return;
    }

    InterlockedBitTestAndReset(&Usb4HrDriver.CmidBitmap, Cmid);
}

/** ORs the shim engine flags of one key into Flags. */
static
VOID
NTAPI
Usb4HrQueryShimKey(
    _In_ PCWSTR Key,
    _Inout_ PULONG64 Flags)
{
    PFN_USB4HR_QUERY_DEVICE_FLAGS QueryDeviceFlags;
    ULONG64 KeyFlags = 0;

    QueryDeviceFlags = (PFN_USB4HR_QUERY_DEVICE_FLAGS)Usb4HrDriver.QueryDeviceFlags;
    if (!QueryDeviceFlags)
        return;

    if (NT_SUCCESS(QueryDeviceFlags(Key, USB4HR_SHIM_PROVIDER, &KeyFlags)))
    {
        if (KeyFlags)
            DPRINT("Shim key %ws: 0x%I64x\n", Key, KeyFlags);
        *Flags |= KeyFlags;
    }
}

ULONG64
NTAPI
Usb4HrQueryShimFlags(
    _In_ WDFDEVICE Device,
    _In_ const Usb4HrIdentity* Identity)
{
    WCHAR Key[USB4HR_SHIM_KEY_CHARS];
    UNICODE_STRING ValueName;
    const Usb4HrShimEntry* Entry;
    ULONG64 Flags = 0;
    WDFKEY DeviceKey;
    ULONG Override;
    NTSTATUS Status;

    /* Same keys, from generic to specific, as the Windows shim database uses */
    if (NT_SUCCESS(RtlStringCchPrintfW(Key, RTL_NUMBER_OF(Key), L"USB4HOSTROUTER:ALL")))
        Usb4HrQueryShimKey(Key, &Flags);

    if (Identity->Bus == Usb4HrParentBus::Pci)
    {
        if (NT_SUCCESS(RtlStringCchPrintfW(Key, RTL_NUMBER_OF(Key),
                                           L"USB4HOSTROUTER:PCI\\VEN_%04X",
                                           Identity->VendorId)))
        {
            Usb4HrQueryShimKey(Key, &Flags);
        }

        if (NT_SUCCESS(RtlStringCchPrintfW(Key, RTL_NUMBER_OF(Key),
                                           L"USB4HOSTROUTER:PCI\\VEN_%04X&DEV_%04X",
                                           Identity->VendorId,
                                           Identity->DeviceId)))
        {
            Usb4HrQueryShimKey(Key, &Flags);
        }

        if (NT_SUCCESS(RtlStringCchPrintfW(Key, RTL_NUMBER_OF(Key),
                                           L"USB4HOSTROUTER:PCI\\VEN_%04X&DEV_%04X&REV_%02X",
                                           Identity->VendorId,
                                           Identity->DeviceId,
                                           Identity->RevisionId)))
        {
            Usb4HrQueryShimKey(Key, &Flags);
        }

        if (NT_SUCCESS(RtlStringCchPrintfW(Key, RTL_NUMBER_OF(Key),
                                           L"USB4HOSTROUTER:PCI\\VEN_%04X&DEV_%04X&SUBSYS_%04X%04X",
                                           Identity->VendorId,
                                           Identity->DeviceId,
                                           Identity->SubsystemId,
                                           Identity->SubsystemVendorId)))
        {
            Usb4HrQueryShimKey(Key, &Flags);
        }

        if (NT_SUCCESS(RtlStringCchPrintfW(Key, RTL_NUMBER_OF(Key),
                                           L"USB4HOSTROUTER:PCI\\VEN_%04X&DEV_%04X&SUBSYS_%04X%04X&REV_%02X",
                                           Identity->VendorId,
                                           Identity->DeviceId,
                                           Identity->SubsystemId,
                                           Identity->SubsystemVendorId,
                                           Identity->RevisionId)))
        {
            Usb4HrQueryShimKey(Key, &Flags);
        }

        for (Entry = Usb4HrShimTable; Entry->VendorId != 0; Entry++)
        {
            if (Entry->VendorId == Identity->VendorId &&
                (Entry->DeviceId == 0xFFFF || Entry->DeviceId == Identity->DeviceId))
            {
                Flags |= Entry->Flags;
            }
        }
    }
    else if (Identity->Bus == Usb4HrParentBus::Acpi)
    {
        if (NT_SUCCESS(RtlStringCchPrintfW(Key, RTL_NUMBER_OF(Key),
                                           L"USB4HOSTROUTER:ACPI\\VEN_%S",
                                           Identity->AcpiVendor)))
        {
            Usb4HrQueryShimKey(Key, &Flags);
        }

        if (NT_SUCCESS(RtlStringCchPrintfW(Key, RTL_NUMBER_OF(Key),
                                           L"USB4HOSTROUTER:ACPI\\VEN_%S&DEV_%S",
                                           Identity->AcpiVendor,
                                           Identity->AcpiDevice)))
        {
            Usb4HrQueryShimKey(Key, &Flags);
        }

        if (NT_SUCCESS(RtlStringCchPrintfW(Key, RTL_NUMBER_OF(Key),
                                           L"USB4HOSTROUTER:ACPI\\VEN_%S&DEV_%S&REV_%04X",
                                           Identity->AcpiVendor,
                                           Identity->AcpiDevice,
                                           Identity->AcpiRevision)))
        {
            Usb4HrQueryShimKey(Key, &Flags);
        }
    }

    Status = WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &DeviceKey);
    if (NT_SUCCESS(Status))
    {
        RtlInitUnicodeString(&ValueName, USB4HR_SHIM_OVERRIDE_VALUE);
        if (NT_SUCCESS(WdfRegistryQueryULong(DeviceKey, &ValueName, &Override)))
        {
            DPRINT1("Shim flags override 0x%lx from the device key\n", Override);
            Flags |= Override;
        }

        WdfRegistryClose(DeviceKey);
    }

    return Flags;
}
