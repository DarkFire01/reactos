/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Router FDO: PnP, power, device flags and device properties
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4dr.h"
#include <devpkey.h>

#define NDEBUG
#include <debug.h>

/* Every device flag key fits this many characters, as in Windows */
#define USB4DR_FLAG_KEY_CHARS           90

/* Longest inserts that still give the long friendly name forms */
#define USB4DR_NAME_INSERT_CHARS        176

#define USB4DR_FRIENDLY_NAME_CHARS      256
#define USB4DR_VERSION_CHARS            8

/** Formats one device flag key and ORs its flags into Flags. */
static
VOID
__cdecl
Usb4DrQueryFlagsFormat(
    _Inout_ PULONG64 Flags,
    _In_ _Printf_format_string_ PCWSTR Format,
    ...)
{
    WCHAR Key[USB4DR_FLAG_KEY_CHARS];
    va_list Args;
    NTSTATUS Status;

    va_start(Args, Format);
    Status = RtlStringCchVPrintfW(Key, RTL_NUMBER_OF(Key), Format, Args);
    va_end(Args);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Device flag key %ws does not fit\n", Format);
        return;
    }

    Usb4DrQueryDeviceFlagsKey(Key, Flags);
}

/** Copies a DROM name for the friendly name; commas would split the name, so they become dots. */
static
SIZE_T
NTAPI
Usb4DrCopyNameInsert(
    _Out_writes_z_(Chars) PCHAR Destination,
    _In_ SIZE_T Chars,
    _In_opt_ PCSTR Source)
{
    SIZE_T Length = 0;

    Destination[0] = ANSI_NULL;
    if (Source == NULL)
        return 0;

    while (Source[Length] != ANSI_NULL && Length + 1 < Chars)
    {
        Destination[Length] = (Source[Length] == ',') ? '.' : Source[Length];
        Length++;
    }

    Destination[Length] = ANSI_NULL;
    return Length;
}

NTSTATUS
NTAPI
Usb4DrEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPower;
    WDF_OBJECT_ATTRIBUTES Attributes;
    Usb4DrFdo* Fdo;
    WDFDEVICE Device;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPower);
    PnpPower.EvtDevicePrepareHardware = Usb4DrFdo::EvtPrepareHardware;
    PnpPower.EvtDeviceReleaseHardware = Usb4DrFdo::EvtReleaseHardware;
    PnpPower.EvtDeviceD0Entry = Usb4DrFdo::EvtD0Entry;
    PnpPower.EvtDeviceD0Exit = Usb4DrFdo::EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPower);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, Usb4DrFdo);
    Attributes.EvtCleanupCallback = Usb4DrFdo::EvtCleanup;

    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WdfDeviceCreate failed 0x%lx\n", Status);
        return Status;
    }

    Fdo = new (Usb4DrGetFdo(Device)) Usb4DrFdo;
    Status = Fdo->Initialize(Device);
    if (!NT_SUCCESS(Status))
        DPRINT1("Router FDO initialization failed 0x%lx\n", Status);

    return Status;
}

Usb4DrFdo*
Usb4DrFdo::FromDevice(
    _In_ WDFDEVICE Device)
{
    return Usb4DrGetFdo(Device);
}

WDFDEVICE
Usb4DrFdo::Device() const
{
    return m_Device;
}

Usb4DrHostLink*
Usb4DrFdo::HostLink()
{
    return &m_HostLink;
}

Usb4DrRouter*
Usb4DrFdo::Router()
{
    return &m_Router;
}

Usb4DrPortSet*
Usb4DrFdo::Ports()
{
    return &m_Ports;
}

Usb4DrTunnels*
Usb4DrFdo::Tunnels()
{
    return &m_Tunnels;
}

Usb4DrForwarder*
Usb4DrFdo::Forwarder()
{
    return &m_Forwarder;
}

Usb4DrParentLink*
Usb4DrFdo::ParentLink()
{
    return &m_ParentLink;
}

ULONG
Usb4DrFdo::Depth() const
{
    return m_HostLink.Depth();
}

ULONG64
Usb4DrFdo::DeviceFlags() const
{
    return m_DeviceFlags;
}

BOOLEAN
Usb4DrFdo::WasDisconnectedOnResume() const
{
    return m_DisconnectedOnResume;
}

NTSTATUS
Usb4DrFdo::Initialize(
    _In_ WDFDEVICE Device)
{
    NTSTATUS Status;

    m_Device = Device;

    /*
     * Windows also marks the device not disableable and sets up Sx wake here;
     * neither matters without S0 idle and wake support.
     */

    Status = m_HostLink.Create(this);
    if (!NT_SUCCESS(Status))
        return Status;
    m_HostLinkCreated = TRUE;

    Status = m_Router.Create(this);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Router creation failed 0x%lx\n", Status);
        return Status;
    }
    m_RouterCreated = TRUE;

    Status = m_Tunnels.Create(this);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Tunnel manager creation failed 0x%lx\n", Status);
        return Status;
    }
    m_TunnelsCreated = TRUE;

    Status = m_Forwarder.Create(this);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Request forwarder creation failed 0x%lx\n", Status);
        return Status;
    }

    Status = m_ParentLink.Create(this);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Parent link creation failed 0x%lx\n", Status);
        return Status;
    }
    m_ParentLinkCreated = TRUE;

    Status = CreateDefaultQueue();
    if (!NT_SUCCESS(Status))
        return Status;

    DPRINT("Router FDO %p at depth %lu ready\n", Device, Depth());
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4DrFdo::CreateDefaultQueue()
{
    WDF_IO_QUEUE_CONFIG Config;
    WDFQUEUE Queue;
    NTSTATUS Status;

    /*
     * Windows only handles user mode IOCTLs here (the TBT3 firmware update); internal
     * requests a child passes up find no handler and fail with STATUS_INVALID_DEVICE_REQUEST.
     * The queue is not power managed so such a request never waits for this router's D0.
     */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&Config, WdfIoQueueDispatchSequential);
    Config.PowerManaged = WdfFalse;
    Config.EvtIoDeviceControl = EvtIoDeviceControl;
    Config.EvtIoInternalDeviceControl = EvtIoInternalDeviceControl;

    Status = WdfIoQueueCreate(m_Device, &Config, WDF_NO_OBJECT_ATTRIBUTES, &Queue);
    if (!NT_SUCCESS(Status))
        DPRINT1("Default queue creation failed 0x%lx\n", Status);

    return Status;
}

VOID
NTAPI
Usb4DrFdo::EvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    /* TBT3 firmware update is not supported */
    DPRINT("IOCTL 0x%lx not supported\n", IoControlCode);
    WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
}

VOID
NTAPI
Usb4DrFdo::EvtIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    DPRINT1("Internal IOCTL 0x%lx from a child router is not handled\n", IoControlCode);
    WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
}

NTSTATUS
Usb4DrFdo::D0Entry(
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    const USB4HR_HARDWARE_SERVICES* Services = m_HostLink.Services();
    BOOLEAN FirstStart = (PreviousState == WdfPowerDeviceD3Final);
    NTSTATUS Status;

    DPRINT("D0 entry from D%d, depth %lu\n", PreviousState - WdfPowerDeviceD0, Depth());

    Status = m_Router.Start(FirstStart);
    if (NT_SUCCESS(Status))
    {
        m_Started = TRUE;
        m_DisconnectedOnResume = FALSE;
    }
    else
    {
        if (!FirstStart && Status == STATUS_DEVICE_NOT_CONNECTED)
        {
            DPRINT1("Router was replaced or removed while powered down\n");
            m_DisconnectedOnResume = TRUE;
        }

        DPRINT1("Router start failed 0x%lx\n", Status);
        ReportFatalError();
    }

    /* The host router holds its power PDO until the root router reports in, success or not */
    if (Depth() == 0 && Services->RootRouterPoweredOn)
        Services->RootRouterPoweredOn(Services->Header.Context, m_HostLink.RouterHandle(), Status);

    return Status;
}

NTSTATUS
Usb4DrFdo::D0Exit(
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    DPRINT("D0 exit to D%d, depth %lu\n", TargetState - WdfPowerDeviceD0, Depth());

    /* Tunnels first: they are torn down through ports and adapters that must still be there */
    m_Tunnels.Stop(TargetState);
    m_Ports.Stop(TargetState);
    m_Router.Stop(TargetState);

    m_Started = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4DrFdo::ReleaseHardware()
{
    /* D0 exit already stopped everything */
    return STATUS_SUCCESS;
}

VOID
Usb4DrFdo::Cleanup()
{
    DPRINT("Router FDO %p cleanup\n", m_Device);

    if (m_ParentLinkCreated)
        m_ParentLink.Destroy();

    if (m_TunnelsCreated)
        m_Tunnels.Destroy();

    if (m_RouterCreated)
    {
        m_Ports.Destroy();
        m_Router.Destroy();
    }

    if (m_HostLinkCreated)
        m_HostLink.Cleanup();

    m_ParentLinkCreated = FALSE;
    m_TunnelsCreated = FALSE;
    m_RouterCreated = FALSE;
    m_HostLinkCreated = FALSE;
}

VOID
Usb4DrFdo::QueryDeviceFlags()
{
    const Usb4DrDrom* Drom = m_Router.Drom();
    USHORT VendorId = m_Router.VendorId();
    USHORT ProductId = m_Router.ProductId();
    UCHAR Revision = m_Router.Revision();
    USHORT UnitVendorId;
    USHORT UnitProductId;
    UCHAR UnitRevision;
    USHORT Firmware;
    UCHAR FirmwareMajor;
    UCHAR FirmwareMinor;
    ULONG64 Flags = 0;

    if (m_DeviceFlagsValid)
        return;

    /* QUIRK: REV is two digits here and four in the hardware ID */
    Usb4DrQueryFlagsFormat(&Flags, L"USB4DEVICEROUTER:ALL");
    Usb4DrQueryFlagsFormat(&Flags, L"USB4DEVICEROUTER:USB4\\VID_%04X", VendorId);
    Usb4DrQueryFlagsFormat(&Flags, L"USB4DEVICEROUTER:USB4\\VID_%04X&PID_%04X", VendorId, ProductId);
    Usb4DrQueryFlagsFormat(&Flags,
                           L"USB4DEVICEROUTER:USB4\\VID_%04X&PID_%04X&REV_%02X",
                           VendorId,
                           ProductId,
                           Revision);

    if (Depth() == 0)
    {
        Usb4DrQueryFlagsFormat(&Flags, L"USB4DEVICEROUTER:USB4\\ROOT_DEVICE_ROUTER&VID_%04X", VendorId);
        Usb4DrQueryFlagsFormat(&Flags,
                               L"USB4DEVICEROUTER:USB4\\ROOT_DEVICE_ROUTER&VID_%04X&PID_%04X",
                               VendorId,
                               ProductId);
        Usb4DrQueryFlagsFormat(&Flags,
                               L"USB4DEVICEROUTER:USB4\\ROOT_DEVICE_ROUTER&VID_%04X&PID_%04X&REV_%02X",
                               VendorId,
                               ProductId,
                               Revision);
    }

    /* TBT3 routers key on their DROM and NVM version, which this driver does not read */
    if (!m_Router.IsTbt3() &&
        Drom != NULL &&
        Drom->ProductDescriptor(&UnitVendorId, &UnitProductId, &UnitRevision, &Firmware))
    {
        FirmwareMajor = (UCHAR)(Firmware >> 8);
        FirmwareMinor = (UCHAR)(Firmware & 0xFF);

        Usb4DrQueryFlagsFormat(&Flags, L"USB4DEVICEROUTER:USB4DROM\\VID_%04X", UnitVendorId);
        Usb4DrQueryFlagsFormat(&Flags,
                               L"USB4DEVICEROUTER:USB4DROM\\VID_%04X&PID_%04X",
                               UnitVendorId,
                               UnitProductId);

        /* QUIRK: this HW field is the silicon revision, unlike the one in the firmware key below */
        Usb4DrQueryFlagsFormat(&Flags,
                               L"USB4DEVICEROUTER:USB4DROM\\VID_%04X&PID_%04X&HW_%02X",
                               UnitVendorId,
                               UnitProductId,
                               Revision);
        Usb4DrQueryFlagsFormat(&Flags,
                               L"USB4DEVICEROUTER:USB4DROM\\VID_%04X&PID_%04X&FW_%02X.%02X",
                               UnitVendorId,
                               UnitProductId,
                               FirmwareMajor,
                               FirmwareMinor);
        Usb4DrQueryFlagsFormat(&Flags,
                               L"USB4DEVICEROUTER:USB4DROM\\VID_%04X&PID_%04X&FW_%02X.%02X&HW_%02X",
                               UnitVendorId,
                               UnitProductId,
                               FirmwareMajor,
                               FirmwareMinor,
                               UnitRevision);
        Usb4DrQueryFlagsFormat(&Flags,
                               L"USB4DEVICEROUTER:USB4\\VID_%04X&PID_%04X&FW_%02X.%02X",
                               VendorId,
                               ProductId,
                               FirmwareMajor,
                               FirmwareMinor);
    }

    m_DeviceFlags |= Flags;
    m_DeviceFlagsValid = TRUE;

    if (m_DeviceFlags)
        DPRINT1("Router %04X:%04X device flags 0x%I64x\n", VendorId, ProductId, m_DeviceFlags);
}

VOID
Usb4DrFdo::SetProperty(
    _In_ ULONG Pid,
    _In_ DEVPROPTYPE Type,
    _In_ ULONG Size,
    _In_reads_bytes_opt_(Size) PVOID Data)
{
    DEVPROPKEY Key;
    NTSTATUS Status;

    Key.fmtid = GUID_USB4HR_PROPERTY_SET;
    Key.pid = Pid;

    Status = IoSetDevicePropertyData(WdfDeviceWdmGetPhysicalDevice(m_Device), &Key, 0, 0, Type, Size, Data);
    if (!NT_SUCCESS(Status))
        DPRINT1("Setting router property %lu failed 0x%lx\n", Pid, Status);
}

VOID
Usb4DrFdo::PublishFriendlyName()
{
    PDEVICE_OBJECT Pdo = WdfDeviceWdmGetPhysicalDevice(m_Device);
    const Usb4DrDrom* Drom = m_Router.Drom();
    CHAR Vendor[USB4DR_DROM_NAME_CHARS];
    CHAR Model[USB4DR_DROM_NAME_CHARS];
    WCHAR Version[USB4DR_VERSION_CHARS];
    WCHAR Name[USB4DR_FRIENDLY_NAME_CHARS];
    SIZE_T VendorLength = 0;
    SIZE_T ModelLength = 0;
    SIZE_T VersionLength = 0;
    BOOLEAN HaveNames;
    BOOLEAN IsTbt3 = m_Router.IsTbt3();
    UCHAR Usb4Version = m_Router.Usb4Version();
    NTSTATUS Status;

    /* Drop the old name first, as Windows does */
    Status = IoSetDevicePropertyData(Pdo, &DEVPKEY_Device_FriendlyName, 0, 0, DEVPROP_TYPE_STRING, 0, NULL);
    if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_NOT_FOUND)
    {
        DPRINT1("Clearing the friendly name failed 0x%lx\n", Status);
        return;
    }

    Version[0] = UNICODE_NULL;
    if (!IsTbt3)
    {
        /* Major version in bits 7:5, minor in 4:0 */
        Status = RtlStringCchPrintfW(Version, RTL_NUMBER_OF(Version), L"%u.%u", Usb4Version >> 5, Usb4Version & 0x1F);
        if (!NT_SUCCESS(Status))
            return;

        VersionLength = wcslen(Version);
    }

    if (Drom != NULL && Drom->IsParsed())
    {
        VendorLength = Usb4DrCopyNameInsert(Vendor, sizeof(Vendor), Drom->VendorName());
        ModelLength = Usb4DrCopyNameInsert(Model, sizeof(Model), Drom->ModelName());
    }
    else
    {
        Vendor[0] = ANSI_NULL;
        Model[0] = ANSI_NULL;
    }

    HaveNames = (VendorLength != 0 && ModelLength != 0);

    if (Depth() == 0)
    {
        Status = RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"USB4 Root Router %ws", Version);
    }
    else if (HaveNames && VersionLength + VendorLength + ModelLength <= USB4DR_NAME_INSERT_CHARS)
    {
        if (IsTbt3)
            Status = RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"Thunderbolt 3(TM) Router, %hs - %hs", Vendor, Model);
        else
            Status = RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"USB4 Router %ws, %hs - %hs", Version, Vendor, Model);
    }
    else if (HaveNames && VersionLength + VendorLength <= USB4DR_NAME_INSERT_CHARS)
    {
        if (IsTbt3)
            Status = RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"Thunderbolt 3(TM) Router, %hs", Vendor);
        else
            Status = RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"USB4 Router %ws, %hs", Version, Vendor);
    }
    else
    {
        if (IsTbt3)
            Status = RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"Thunderbolt 3(TM) Router");
        else
            Status = RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"USB4 Router %ws", Version);
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Friendly name does not fit 0x%lx\n", Status);
        return;
    }

    /* Windows writes an indirect string into its message table; ReactOS gets the text itself */
    Status = IoSetDevicePropertyData(Pdo,
                                     &DEVPKEY_Device_FriendlyName,
                                     0,
                                     0,
                                     DEVPROP_TYPE_STRING,
                                     (ULONG)((wcslen(Name) + 1) * sizeof(WCHAR)),
                                     Name);
    if (!NT_SUCCESS(Status))
        DPRINT1("Setting the friendly name failed 0x%lx\n", Status);
    else
        DPRINT("Friendly name %ws\n", Name);
}

VOID
Usb4DrFdo::PublishRouterProperties()
{
    const USB4HR_PARENT_INTERFACE* Parent = m_HostLink.Parent();
    DEVPROP_BOOLEAN IsTbt3;
    UCHAR TopologyPorts[USB4HR_MAX_DEPTH + 1];
    ULONG DomainId;
    USHORT VendorId;
    USHORT ProductId;
    USHORT Revision;
    ULONG Usb4Version;

    if (m_PropertiesPublished)
        return;

    m_PropertiesPublished = TRUE;

    PublishFriendlyName();

    IsTbt3 = m_Router.IsTbt3() ? DEVPROP_TRUE : DEVPROP_FALSE;
    SetProperty(USB4HR_PROPERTY_IS_TBT3, DEVPROP_TYPE_BOOLEAN, sizeof(IsTbt3), &IsTbt3);

    DomainId = Parent->DomainId;
    SetProperty(USB4HR_PROPERTY_DOMAIN_ID, DEVPROP_TYPE_UINT32, sizeof(DomainId), &DomainId);

    C_ASSERT(sizeof(TopologyPorts) == sizeof(Parent->TopologyId.Port));
    RtlCopyMemory(TopologyPorts, Parent->TopologyId.Port, sizeof(TopologyPorts));
    SetProperty(USB4HR_PROPERTY_TOPOLOGY_ID, DEVPROP_TYPE_BINARY, sizeof(TopologyPorts), TopologyPorts);

    VendorId = m_Router.VendorId();
    SetProperty(USB4HR_PROPERTY_SILICON_VENDOR_ID, DEVPROP_TYPE_UINT16, sizeof(VendorId), &VendorId);

    ProductId = m_Router.ProductId();
    SetProperty(USB4HR_PROPERTY_SILICON_PRODUCT_ID, DEVPROP_TYPE_UINT16, sizeof(ProductId), &ProductId);

    Revision = m_Router.Revision();
    SetProperty(USB4HR_PROPERTY_SILICON_REVISION, DEVPROP_TYPE_UINT16, sizeof(Revision), &Revision);

    Usb4Version = m_Router.Usb4Version();
    SetProperty(USB4HR_PROPERTY_USB4_VERSION, DEVPROP_TYPE_UINT32, sizeof(Usb4Version), &Usb4Version);
}

VOID
Usb4DrFdo::PublishDromProperties()
{
    const Usb4DrDrom* Drom = m_Router.Drom();
    WCHAR Text[USB4DR_DROM_NAME_CHARS];
    PCSTR Names[2];
    ULONG Pids[2];
    ULONG Index;
    NTSTATUS Status;

    if (Drom == NULL || !Drom->IsParsed())
        return;

    Names[0] = Drom->VendorName();
    Pids[0] = USB4HR_PROPERTY_VENDOR_NAME;
    Names[1] = Drom->ModelName();
    Pids[1] = USB4HR_PROPERTY_MODEL_NAME;

    for (Index = 0; Index < RTL_NUMBER_OF(Names); Index++)
    {
        if (Names[Index] == NULL)
            continue;

        Status = RtlStringCchPrintfW(Text, RTL_NUMBER_OF(Text), L"%hs", Names[Index]);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("DROM name for property %lu does not fit\n", Pids[Index]);
            continue;
        }

        SetProperty(Pids[Index], DEVPROP_TYPE_STRING, (ULONG)((wcslen(Text) + 1) * sizeof(WCHAR)), Text);
    }
}

VOID
Usb4DrFdo::PublishAdapterSupport(
    _In_ ULONG SupportMask)
{
    SetProperty(USB4HR_PROPERTY_ADAPTER_SUPPORT, DEVPROP_TYPE_UINT32, sizeof(SupportMask), &SupportMask);
}

VOID
Usb4DrFdo::ReportFatalError()
{
    DPRINT1("Router at depth %lu failed, restarting it\n", Depth());

    m_Ports.ReportAllChildrenMissing();
    WdfDeviceSetFailed(m_Device, WdfDeviceFailedAttemptRestart);
}

NTSTATUS
Usb4DrFdo::WaitBounded(
    _In_ PKEVENT Event,
    _In_ PCSTR What)
{
    LARGE_INTEGER Timeout;
    ULONG Round;
    NTSTATUS Status;

    for (Round = 0; Round < USB4DR_POWER_WAIT_ROUNDS; Round++)
    {
        Timeout = Usb4DrRelativeMs(USB4DR_POWER_WAIT_MS);
        Status = KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, &Timeout);
        if (Status != STATUS_TIMEOUT)
            return STATUS_SUCCESS;

        DPRINT1("Still waiting for %s after %lu s\n", What, (Round + 1) * (USB4DR_POWER_WAIT_MS / 1000));
    }

    /* Windows waits forever here and hangs the power thread */
    DPRINT1("Gave up waiting for %s\n", What);
    return STATUS_IO_TIMEOUT;
}

NTSTATUS
NTAPI
Usb4DrFdo::EvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
Usb4DrFdo::EvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    return FromDevice(Device)->ReleaseHardware();
}

NTSTATUS
NTAPI
Usb4DrFdo::EvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    return FromDevice(Device)->D0Entry(PreviousState);
}

NTSTATUS
NTAPI
Usb4DrFdo::EvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    return FromDevice(Device)->D0Exit(TargetState);
}

VOID
NTAPI
Usb4DrFdo::EvtCleanup(
    _In_ WDFOBJECT Object)
{
    FromDevice((WDFDEVICE)Object)->Cleanup();
}
