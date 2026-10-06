/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Root router PDO, its query interfaces and IOCTL forwarding
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4hr.h"

#define NDEBUG
#include <debug.h>

#define USB4HR_ROOT_ROUTER_ID           L"USB4\\ROOT_DEVICE_ROUTER"
#define USB4HR_ROOT_ROUTER_ID_CHARS     96

/* USB4 version byte of the parent interface: 2.0 with version 2 support, else 1.0 */
#define USB4HR_CM_USB4_VERSION_2        0x20
#define USB4HR_CM_USB4_VERSION_1        0x10

static const UNICODE_STRING Usb4HrRootRouterInstanceId = RTL_CONSTANT_STRING(L"0");
static const UNICODE_STRING Usb4HrRootRouterCompatibleId = RTL_CONSTANT_STRING(USB4HR_ROOT_ROUTER_ID);
static const UNICODE_STRING Usb4HrDeviceRouterCompatibleId = RTL_CONSTANT_STRING(L"USB4\\DEVICE_ROUTER");

NTSTATUS
Usb4HrRootRouter::AssignIds(
    _In_ Usb4HrHostRouter* HostRouter,
    _In_ PWDFDEVICE_INIT DeviceInit)
{
    const Usb4HrIdentity* Identity = HostRouter->Hardware()->Identity();
    WCHAR FullBuffer[USB4HR_ROOT_ROUTER_ID_CHARS];
    WCHAR ShortBuffer[USB4HR_ROOT_ROUTER_ID_CHARS];
    UNICODE_STRING FullId;
    UNICODE_STRING ShortId;
    NTSTATUS Status;

    RtlInitEmptyUnicodeString(&FullId, FullBuffer, sizeof(FullBuffer));
    RtlInitEmptyUnicodeString(&ShortId, ShortBuffer, sizeof(ShortBuffer));

    switch (Identity->Bus)
    {
        case Usb4HrParentBus::Pci:
            Status = RtlUnicodeStringPrintf(&FullId,
                                            USB4HR_ROOT_ROUTER_ID L"&VID_%04X&PID_%04X&REV_%04X",
                                            Identity->VendorId,
                                            Identity->DeviceId,
                                            Identity->RevisionId);
            if (NT_SUCCESS(Status))
            {
                Status = RtlUnicodeStringPrintf(&ShortId,
                                                USB4HR_ROOT_ROUTER_ID L"&VID_%04X&PID_%04X",
                                                Identity->VendorId,
                                                Identity->DeviceId);
            }
            break;

        case Usb4HrParentBus::Acpi:
            Status = RtlUnicodeStringPrintf(&FullId,
                                            USB4HR_ROOT_ROUTER_ID L"&VID_%hs&PID_%hs&REV_%04X",
                                            Identity->AcpiVendor,
                                            Identity->AcpiDevice,
                                            Identity->AcpiRevision);
            if (NT_SUCCESS(Status))
            {
                Status = RtlUnicodeStringPrintf(&ShortId,
                                                USB4HR_ROOT_ROUTER_ID L"&VID_%hs&PID_%hs",
                                                Identity->AcpiVendor,
                                                Identity->AcpiDevice);
            }
            break;

        default:
            DPRINT1("Root router: host router on an unknown bus %lu\n", (ULONG)Identity->Bus);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Root router: hardware IDs not formatted 0x%lx\n", Status);
        return Status;
    }

    /* The device ID is the vendor and product ID without the revision */
    Status = WdfPdoInitAddHardwareID(DeviceInit, &FullId);
    if (NT_SUCCESS(Status))
        Status = WdfPdoInitAddHardwareID(DeviceInit, &ShortId);
    if (NT_SUCCESS(Status))
        Status = WdfPdoInitAssignDeviceID(DeviceInit, &ShortId);
    if (NT_SUCCESS(Status))
        Status = WdfPdoInitAddCompatibleID(DeviceInit, &Usb4HrRootRouterCompatibleId);
    if (NT_SUCCESS(Status))
        Status = WdfPdoInitAddCompatibleID(DeviceInit, &Usb4HrDeviceRouterCompatibleId);

    if (!NT_SUCCESS(Status))
        DPRINT1("Root router: IDs not assigned 0x%lx\n", Status);

    return Status;
}

NTSTATUS
Usb4HrRootRouter::CreateDevice(
    _In_ Usb4HrHostRouter* HostRouter,
    _In_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPower;
    WDF_PDO_EVENT_CALLBACKS PdoCallbacks;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_DEVICE_PNP_CAPABILITIES PnpCaps;
    Usb4HrRootRouter* RootRouter;
    WDFDEVICE Device;
    NTSTATUS Status;

    PAGED_CODE();

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_USB4);

    Status = WdfPdoInitAssignInstanceID(DeviceInit, &Usb4HrRootRouterInstanceId);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Root router: instance ID not assigned 0x%lx\n", Status);
        return Status;
    }

    Status = AssignIds(HostRouter, DeviceInit);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Prepare, release and D0 exit only feed SleepStudy in Windows */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPower);
    PnpPower.EvtDeviceD0Entry = EvtD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPower);

    WDF_PDO_EVENT_CALLBACKS_INIT(&PdoCallbacks);
    PdoCallbacks.EvtDeviceEnableWakeAtBus = EvtEnableWakeAtBus;
    PdoCallbacks.EvtDeviceDisableWakeAtBus = EvtDisableWakeAtBus;
    WdfPdoInitSetEventCallbacks(DeviceInit, &PdoCallbacks);

    /* Every internal IOCTL is served by the host router FDO */
    WdfPdoInitAllowForwardingRequestToParent(DeviceInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, Usb4HrRootRouter);

    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Root router: device not created 0x%lx\n", Status);
        return Status;
    }

    WDF_DEVICE_PNP_CAPABILITIES_INIT(&PnpCaps);
    PnpCaps.Removable = WdfFalse;
    PnpCaps.UniqueID = WdfFalse;
    PnpCaps.Address = 0xFFFFFFFF;
    PnpCaps.UINumber = 0xFFFFFFFF;
    WdfDeviceSetPnpCapabilities(Device, &PnpCaps);

    RootRouter = FromDevice(Device);
    Status = RootRouter->Initialize(HostRouter, Device);
    if (!NT_SUCCESS(Status))
        return Status;

    DPRINT("Root router %p created, router handle %p\n", RootRouter, RootRouter->m_RouterHandle);
    return STATUS_SUCCESS;
}

Usb4HrRootRouter*
Usb4HrRootRouter::FromDevice(
    _In_ WDFDEVICE Device)
{
    return Usb4HrGetRootRouter(Device);
}

WDFDEVICE Usb4HrRootRouter::Device() const
{
    return m_Device;
}

NTSTATUS
Usb4HrRootRouter::Initialize(
    _In_ Usb4HrHostRouter* HostRouter,
    _In_ WDFDEVICE Device)
{
    Usb4HrHardware* Hardware = HostRouter->Hardware();
    WDF_QUERY_INTERFACE_CONFIG QueryConfig;
    USB4HR_PARENT_INTERFACE ParentTemplate;
    USB4HR_HARDWARE_SERVICES ServicesTemplate;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    NTSTATUS Status;

    m_Device = Device;
    m_HostRouter = HostRouter;
    m_RouterHandle = HostRouter->Topology()->RootRouterHandle();
    m_DomainId = HostRouter->DomainId();
    m_Cmid = Hardware->Cmid();
    m_HostInterfaceVersion = Hardware->HostInterfaceVersion();
    m_Usb4V2Enabled = HostRouter->IsUsb4V2Enabled();
    m_DomainUuid = *HostRouter->DomainUuid();
    m_TunnelPermissions = HostRouter->TunnelPermissions();
    m_DomainFlags = HostRouter->DomainFlags();

    /* The callbacks fill the whole interface, so the templates only carry size and version */
    RtlZeroMemory(&ParentTemplate, sizeof(ParentTemplate));
    ParentTemplate.Header.Size = sizeof(ParentTemplate);
    ParentTemplate.Header.Version = USB4HR_PARENT_INTERFACE_VERSION;

    WDF_QUERY_INTERFACE_CONFIG_INIT(&QueryConfig,
                                    &ParentTemplate.Header,
                                    &GUID_USB4HR_PARENT_INTERFACE,
                                    EvtQueryParentInterface);
    QueryConfig.ImportInterface = TRUE;

    Status = WdfDeviceAddQueryInterface(Device, &QueryConfig);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Root router: parent interface not added 0x%lx\n", Status);
        return Status;
    }

    Status = WdfDeviceCreateDeviceInterface(Device, &GUID_USB4HR_HARDWARE_SERVICES, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Root router: hardware services interface not created 0x%lx\n", Status);
        return Status;
    }

    RtlZeroMemory(&ServicesTemplate, sizeof(ServicesTemplate));
    ServicesTemplate.Header.Size = sizeof(ServicesTemplate);
    ServicesTemplate.Header.Version = USB4HR_HARDWARE_SERVICES_VERSION;

    WDF_QUERY_INTERFACE_CONFIG_INIT(&QueryConfig,
                                    &ServicesTemplate.Header,
                                    &GUID_USB4HR_HARDWARE_SERVICES,
                                    EvtQueryHardwareServices);
    QueryConfig.ImportInterface = TRUE;

    Status = WdfDeviceAddQueryInterface(Device, &QueryConfig);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Root router: hardware services query interface not added 0x%lx\n", Status);
        return Status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.PowerManaged = WdfFalse;
    QueueConfig.EvtIoInternalDeviceControl = EvtIoInternalDeviceControl;

    Status = WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Root router: default queue not created 0x%lx\n", Status);
        return Status;
    }

    Status = WdfDeviceCreateDeviceInterface(Device, &GUID_USB4HR_ROOT_ROUTER_INTERFACE, NULL);
    if (!NT_SUCCESS(Status))
        DPRINT1("Root router: device interface not created 0x%lx\n", Status);

    return Status;
}

NTSTATUS
Usb4HrRootRouter::FillParentInterface(
    _Inout_ PUSB4HR_PARENT_INTERFACE Interface)
{
    if (Interface->Header.Size != sizeof(*Interface) ||
        Interface->Header.Version != USB4HR_PARENT_INTERFACE_VERSION)
    {
        DPRINT1("Root router: parent interface size %u version %u refused\n",
                Interface->Header.Size, Interface->Header.Version);
        return STATUS_NOT_SUPPORTED;
    }

    Interface->Header.Context = this;
    Interface->Header.InterfaceReference = InterfaceNoop;
    Interface->Header.InterfaceDereference = InterfaceNoop;

    Interface->DomainId = m_DomainId;
    Interface->DpBwModeCmId = m_Cmid;
    RtlZeroMemory(Interface->Reserved0, sizeof(Interface->Reserved0));
    Interface->DomainUuid = m_DomainUuid;

    /* The root router sits at depth 0 */
    RtlZeroMemory(&Interface->TopologyId, sizeof(Interface->TopologyId));
    Interface->Reserved1 = 0;

    Interface->CanBondLanes = CanBondLanes;
    Interface->RouterHandle = m_RouterHandle;
    Interface->DomainRootPdo = WdfDeviceWdmGetDeviceObject(m_Device);
    Interface->TunnelPermissions = m_TunnelPermissions;
    Interface->Reserved2 = 0;
    Interface->DomainFlags = m_DomainFlags;
    Interface->IsParentConnectionLive = IsParentConnectionLive;
    Interface->CmVersionLimit = m_Usb4V2Enabled ? USB4HR_CM_USB4_VERSION_2 : USB4HR_CM_USB4_VERSION_1;
    Interface->HostInterfaceVersion = m_HostInterfaceVersion;
    Interface->EnhancedUniTmuSupported = m_Usb4V2Enabled;
    RtlZeroMemory(Interface->Reserved3, sizeof(Interface->Reserved3));
    Interface->IsAsymmetricSupported = IsAsymmetricSupported;
    Interface->AgreeEnumerationDeadline = AgreeEnumerationDeadline;
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrRootRouter::FillHardwareServices(
    _Inout_ PUSB4HR_HARDWARE_SERVICES Interface)
{
    if (Interface->Header.Size != sizeof(*Interface) ||
        Interface->Header.Version != USB4HR_HARDWARE_SERVICES_VERSION)
    {
        DPRINT1("Root router: hardware services size %u version %u refused\n",
                Interface->Header.Size, Interface->Header.Version);
        return STATUS_NOT_SUPPORTED;
    }

    Interface->Header.Context = m_HostRouter->Topology();
    Interface->Header.InterfaceReference = InterfaceNoop;
    Interface->Header.InterfaceDereference = InterfaceNoop;

    Interface->AllocateRouterHandle = Usb4HrServiceAllocateRouterHandle;
    Interface->DestroyRouterHandle = Usb4HrServiceDestroyRouterHandle;
    Interface->IssueAdapterHandles = Usb4HrServiceIssueAdapterHandles;
    Interface->RevokeAdapterHandle = Usb4HrServiceRevokeAdapterHandle;
    Interface->ReportRouterFamily = Usb4HrServiceReportRouterFamily;
    Interface->PurgeEventQueues = Usb4HrServicePurgeEventQueues;
    Interface->RootRouterStarted = Usb4HrServiceRootRouterStarted;
    Interface->ReportUpstreamLinkBandwidth = Usb4HrServiceReportUpstreamLinkBandwidth;
    Interface->ReadDpBandwidthGrant = Usb4HrServiceReadDpBandwidthGrant;
    Interface->PinDomainForDpAltMode = Usb4HrServicePinDomainForDpAltMode;
    Interface->UnpinDomainForDpAltMode = Usb4HrServiceUnpinDomainForDpAltMode;
    return STATUS_SUCCESS;
}

/* Interface functions ********************************************************/

VOID
NTAPI
Usb4HrRootRouter::InterfaceNoop(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

BOOLEAN
NTAPI
Usb4HrRootRouter::CanBondLanes(
    _In_ PVOID Context)
{
    DPRINT1("Root router %p: lane bonding asked of a router without an upstream port\n", Context);
    return FALSE;
}

BOOLEAN
NTAPI
Usb4HrRootRouter::IsAsymmetricSupported(
    _In_ PVOID Context)
{
    DPRINT1("Root router %p: asymmetric link asked of a router without an upstream port\n", Context);
    return FALSE;
}

BOOLEAN
NTAPI
Usb4HrRootRouter::IsParentConnectionLive(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    return TRUE;
}

ULONG
NTAPI
Usb4HrRootRouter::AgreeEnumerationDeadline(
    _In_ PVOID Context,
    _In_ ULONG Seconds)
{
    UNREFERENCED_PARAMETER(Seconds);

    DPRINT1("Root router %p: enumeration time asked of a router without a parent\n", Context);
    return 0;
}

/* KMDF callbacks *************************************************************/

NTSTATUS
NTAPI
Usb4HrRootRouter::EvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);

    FromDevice(Device)->m_HostRouter->OnRootRouterD0Entry();
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
Usb4HrRootRouter::EvtEnableWakeAtBus(
    _In_ WDFDEVICE Device,
    _In_ SYSTEM_POWER_STATE PowerState)
{
    Usb4HrRootRouter* RootRouter = FromDevice(Device);

    DPRINT("Root router %p: wake at bus armed for S%lu\n", RootRouter, (ULONG)PowerState - PowerSystemWorking);
    RootRouter->m_WakeAtBusArmed = TRUE;
    return STATUS_SUCCESS;
}

VOID
NTAPI
Usb4HrRootRouter::EvtDisableWakeAtBus(
    _In_ WDFDEVICE Device)
{
    Usb4HrRootRouter* RootRouter = FromDevice(Device);

    DPRINT("Root router %p: wake at bus disarmed\n", RootRouter);
    RootRouter->m_WakeAtBusArmed = FALSE;
}

VOID
NTAPI
Usb4HrRootRouter::EvtIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    Usb4HrRootRouter* RootRouter = FromDevice(WdfIoQueueGetDevice(Queue));
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    Status = RootRouter->m_HostRouter->ForwardFromRootRouter(Request);
    if (!NT_SUCCESS(Status))
    {
        /* Windows neither completes nor returns a request it failed to forward */
        DPRINT1("Root router %p: IOCTL 0x%lx not forwarded 0x%lx\n", RootRouter, IoControlCode, Status);
        WdfRequestComplete(Request, Status);
    }
}

NTSTATUS
NTAPI
Usb4HrRootRouter::EvtQueryParentInterface(
    _In_ WDFDEVICE Device,
    _In_ LPGUID InterfaceType,
    _Inout_ PINTERFACE ExposedInterface,
    _Inout_opt_ PVOID ExposedInterfaceSpecificData)
{
    UNREFERENCED_PARAMETER(InterfaceType);
    UNREFERENCED_PARAMETER(ExposedInterfaceSpecificData);

    if (ExposedInterface == NULL)
        return STATUS_INVALID_PARAMETER;

    return FromDevice(Device)->FillParentInterface((PUSB4HR_PARENT_INTERFACE)ExposedInterface);
}

NTSTATUS
NTAPI
Usb4HrRootRouter::EvtQueryHardwareServices(
    _In_ WDFDEVICE Device,
    _In_ LPGUID InterfaceType,
    _Inout_ PINTERFACE ExposedInterface,
    _Inout_opt_ PVOID ExposedInterfaceSpecificData)
{
    UNREFERENCED_PARAMETER(InterfaceType);
    UNREFERENCED_PARAMETER(ExposedInterfaceSpecificData);

    if (ExposedInterface == NULL)
        return STATUS_INVALID_PARAMETER;

    return FromDevice(Device)->FillHardwareServices((PUSB4HR_HARDWARE_SERVICES)ExposedInterface);
}
