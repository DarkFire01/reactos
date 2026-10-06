/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Driver entry, registry policy and device flags
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4dr.h"

#define NDEBUG
#include <debug.h>

/* SystemCodeIntegrityInformation and its test signing option */
#define USB4DR_CODE_INTEGRITY_CLASS     103
#define USB4DR_CODE_INTEGRITY_TESTSIGN  0x00000002

/* Shim engine provider every device flag key is looked up under */
#define USB4DR_SHIM_PROVIDER            L"USB4DEVICEROUTER"

typedef struct _USB4DR_CODE_INTEGRITY
{
    ULONG Length;
    ULONG CodeIntegrityOptions;
} USB4DR_CODE_INTEGRITY;

typedef NTSTATUS
(NTAPI *PFN_USB4DR_QUERY_DEVICE_FLAGS)(
    _In_ PCWSTR DeviceKey,
    _In_ PCWSTR Provider,
    _Out_ PULONG64 Flags);

extern "C"
NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
    _In_ ULONG SystemInformationClass,
    _Out_writes_bytes_opt_(Length) PVOID SystemInformation,
    _In_ ULONG Length,
    _Out_opt_ PULONG ReturnLength);

Usb4DrDriverData Usb4DrDriver;

static
BOOLEAN
NTAPI
Usb4DrIsTestSigningOn(VOID)
{
    USB4DR_CODE_INTEGRITY Info;
    NTSTATUS Status;

    Info.Length = sizeof(Info);
    Info.CodeIntegrityOptions = 0;

    Status = ZwQuerySystemInformation(USB4DR_CODE_INTEGRITY_CLASS, &Info, sizeof(Info), NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Code integrity query failed 0x%lx\n", Status);
        return FALSE;
    }

    return (Info.CodeIntegrityOptions & USB4DR_CODE_INTEGRITY_TESTSIGN) != 0;
}

/** Reads one REG_DWORD of the Parameters key; a missing value reads as 0. */
static
ULONG
NTAPI
Usb4DrReadPolicyValue(
    _In_ WDFKEY Key,
    _In_ PCWSTR Name)
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

    return Value;
}

static
VOID
NTAPI
Usb4DrReadPolicy(
    _In_ WDFDRIVER Driver)
{
    WDFKEY Key;
    NTSTATUS Status;

    Status = WdfDriverOpenParametersRegistryKey(Driver, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &Key);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Opening the driver Parameters key failed 0x%lx\n", Status);
        return;
    }

    /* The only knob that works without test signing; the others are diagnostics */
    Usb4DrDriver.Policy.ForceTbt3EnumOnPcieDisabled =
        Usb4DrReadPolicyValue(Key, L"ForceTbt3EnumOnPcieDisabled") != 0;

    if (Usb4DrDriver.Policy.ForceTbt3EnumOnPcieDisabled)
        DPRINT("TBT3 routers enumerate even with PCIe tunneling disabled\n");

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

    Usb4DrDriver.DriverObject = DriverObject;

    /* Resolved at run time so a kernel without the export still loads us */
    RtlInitUnicodeString(&RoutineName, L"KseQueryDeviceFlags");
    Usb4DrDriver.QueryDeviceFlags = MmGetSystemRoutineAddress(&RoutineName);

    Usb4DrDriver.TestSigning = Usb4DrIsTestSigningOn();
    if (Usb4DrDriver.TestSigning)
        DPRINT("Test signing is on\n");

    WDF_DRIVER_CONFIG_INIT(&Config, Usb4DrEvtDeviceAdd);
    Config.EvtDriverUnload = Usb4DrEvtDriverUnload;
    Config.DriverPoolTag = USB4DR_TAG_DRIVER;

    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, &Driver);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WdfDriverCreate failed 0x%lx\n", Status);
        return Status;
    }

    Usb4DrReadPolicy(Driver);
    return STATUS_SUCCESS;
}

VOID
NTAPI
Usb4DrEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    DPRINT("USB4 device router driver unloading\n");
}

VOID
NTAPI
Usb4DrQueryDeviceFlagsKey(
    _In_ PCWSTR Key,
    _Inout_ PULONG64 Flags)
{
    PFN_USB4DR_QUERY_DEVICE_FLAGS QueryDeviceFlags;
    ULONG64 KeyFlags = 0;
    NTSTATUS Status;

    DPRINT("Device flag key %ws\n", Key);

    QueryDeviceFlags = (PFN_USB4DR_QUERY_DEVICE_FLAGS)Usb4DrDriver.QueryDeviceFlags;
    if (!QueryDeviceFlags)
        return;

    Status = QueryDeviceFlags(Key, USB4DR_SHIM_PROVIDER, &KeyFlags);
    if (!NT_SUCCESS(Status))
        return;

    if (KeyFlags)
        DPRINT("Device flags 0x%I64x from %ws\n", KeyFlags, Key);

    *Flags |= KeyFlags;
}
