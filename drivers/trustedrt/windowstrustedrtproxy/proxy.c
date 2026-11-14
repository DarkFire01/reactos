/*
 * PROJECT:     ReactOS Trusted Runtime secure service proxy
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Binds each secure service device to the Trusted Runtime class
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include <ntddk.h>
#include <wdf.h>
#include <wdfldr.h>
#include <drivers/trustedrt/wtrclass.h>

#define NDEBUG
#include <debug.h>

#define PROXY_POOL_TAG 'PrTW'

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD ProxyEvtDeviceAdd;
static EVT_WDF_DRIVER_UNLOAD ProxyEvtDriverUnload;
static
VOID
NTAPI
ProxyReinitialize(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PVOID Context,
    _In_ ULONG Count);

/* Filled by the class when this driver binds */
static PVOID ProxyClassFunctions[WtrFunctionTableNumEntries];
static WTR_CLIENT ProxyClient;

#if defined(__GNUC__)
#define PROXY_CLASS_BIND_SECTION __attribute__((section(".kmdfclassbind$b"), used, aligned(4)))
#else
#pragma section(".kmdfclassbind$b", read, write)
#define PROXY_CLASS_BIND_SECTION __declspec(allocate(".kmdfclassbind$b"))
#endif

PROXY_CLASS_BIND_SECTION WDF_CLASS_BIND_INFO ProxyClassBindInfo =
{
    sizeof(WDF_CLASS_BIND_INFO),
    (PWCHAR)WTR_CLASS_NAME,
    { WTR_CLASS_MAJOR_VERSION, WTR_CLASS_MINOR_VERSION, 0 },
    (VOID (NTAPI **)(VOID))ProxyClassFunctions,
    WtrFunctionTableNumEntries,
    &ProxyClient,
    NULL,
    NULL,
    NULL
};

/* The hardware ID is {bus}\{service}; the service GUID follows the backslash */
static
NTSTATUS
NTAPI
ProxyEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WTR_HANDOFF_SERVICE_DEVICE *HandoffService;
    UNICODE_STRING IdString;
    WDFMEMORY Memory;
    WDFDEVICE Device = NULL;
    size_t Size = 0;
    PWCHAR Id;
    PWCHAR Cursor;
    PWCHAR End;
    GUID ServiceId;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    Status = WdfFdoInitAllocAndQueryProperty(DeviceInit,
                                             DevicePropertyHardwareID,
                                             NonPagedPoolNx,
                                             WDF_NO_OBJECT_ATTRIBUTES,
                                             &Memory);
    if (!NT_SUCCESS(Status))
        return Status;

    Id = WdfMemoryGetBuffer(Memory, &Size);
    if (Size < sizeof(WCHAR) || Id[Size / sizeof(WCHAR) - 1] != UNICODE_NULL)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    Cursor = Id;
    while (*Cursor && *Cursor != L'\\')
        Cursor++;
    if (*Cursor != L'\\')
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    Cursor++;
    End = Cursor;
    while (*End)
        End++;

    IdString.Buffer = Cursor;
    IdString.Length = (USHORT)((End - Cursor) * sizeof(WCHAR));
    IdString.MaximumLength = IdString.Length;
    if (!NT_SUCCESS(RtlGUIDFromString(&IdString, &ServiceId)))
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    HandoffService = (WTR_HANDOFF_SERVICE_DEVICE *)ProxyClassFunctions[WtrHandoffServiceDeviceIndex];
    if (!HandoffService)
    {
        DPRINT1("WTR proxy: class not bound\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Status = HandoffService(ProxyClient, DeviceInit, &ServiceId, NULL, &Device);
    DPRINT1("WTR proxy: %wZ -> 0x%lx\n", &IdString, Status);
    return Status;
}

static
VOID
NTAPI
ProxyEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
}

/* Keeps the driver around between devices the way the stock proxy does */
static
VOID
NTAPI
ProxyReinitialize(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PVOID Context,
    _In_ ULONG Count)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Count);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    WDFDRIVER Driver;
    NTSTATUS Status;

    WDF_DRIVER_CONFIG_INIT(&Config, ProxyEvtDeviceAdd);
    Config.EvtDriverUnload = ProxyEvtDriverUnload;
    Config.DriverPoolTag = PROXY_POOL_TAG;

    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, &Driver);
    if (NT_SUCCESS(Status))
        IoRegisterDriverReinitialization(DriverObject, ProxyReinitialize, NULL);

    return Status;
}
