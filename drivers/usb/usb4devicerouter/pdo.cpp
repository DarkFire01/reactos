/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Child router PDO: IDs, parent interface, hardware services and request forwarding
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4dr.h"

#define NDEBUG
#include <debug.h>

/* Longest ID is USB4\VID_XXXX&PID_XXXX&REV_XXXX */
#define USB4DR_PDO_ID_CHARS             40

/* Decimal adapter number */
#define USB4DR_PDO_INSTANCE_CHARS       16

/* Routers reporting a USB4 version byte below this one are Thunderbolt 3 routers */
static const UCHAR Usb4DrFirstUsb4Version = 0x20;

static const UNICODE_STRING Usb4DrTbt3RouterCompatibleId = RTL_CONSTANT_STRING(L"USB4\\TBT3_DEVICE_ROUTER");
static const UNICODE_STRING Usb4DrRouterCompatibleId = RTL_CONSTANT_STRING(L"USB4\\DEVICE_ROUTER");

NTSTATUS
Usb4DrChildPdo::CreateDevice(
    _In_ Usb4DrFdo* Fdo,
    _In_ const Usb4DrChildDescription* Description,
    _In_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPower;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    Usb4DrChildPdo* Pdo;
    WDFDEVICE Device;
    NTSTATUS Status;

    PAGED_CODE();

    /* D0 entry and exit only fed SleepStudy in Windows */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPower);
    PnpPower.EvtDevicePrepareHardware = EvtPrepareHardware;
    PnpPower.EvtDeviceReleaseHardware = EvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPower);

    /* Router to router requests go on to a queue of the parent FDO */
    WdfPdoInitAllowForwardingRequestToParent(DeviceInit);
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_USB4);

    Status = AssignIds(&Description->Identity, DeviceInit);
    if (!NT_SUCCESS(Status))
        return Status;

    if (!Description->Identity.IsKnown)
    {
        /* Windows logs this failure and creates the device anyway */
        Status = WdfPdoInitAssignRawDevice(DeviceInit, &GUID_USB4DR_UNKNOWN_ROUTER_CLASS);
        if (!NT_SUCCESS(Status))
            DPRINT1("Child router on DFP %u: raw device not assigned 0x%lx\n",
                    Description->Identity.Lane0AdapterNumber, Status);
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, Usb4DrChildPdo);

    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Child router on DFP %u: device not created 0x%lx\n",
                Description->Identity.Lane0AdapterNumber, Status);
        return Status;
    }

    Pdo = FromDevice(Device);
    Status = Pdo->Initialize(Fdo, Description, Device);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.PowerManaged = WdfFalse;
    QueueConfig.EvtIoInternalDeviceControl = EvtIoInternalDeviceControl;

    Status = WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Child router %p: default queue not created 0x%lx\n", Pdo, Status);
        return Status;
    }

    DPRINT("Child router %p on DFP %u: %04X:%04X rev %02X, version 0x%02X%s\n",
           Pdo,
           Pdo->m_Identity.Lane0AdapterNumber,
           Pdo->m_Identity.VendorId,
           Pdo->m_Identity.ProductId,
           Pdo->m_Identity.Revision,
           Pdo->m_Identity.Usb4Version,
           Pdo->m_Identity.IsKnown ? "" : ", unknown");
    return STATUS_SUCCESS;
}

Usb4DrChildPdo*
Usb4DrChildPdo::FromDevice(
    _In_ WDFDEVICE Device)
{
    return Usb4DrGetChildPdo(Device);
}

WDFDEVICE
Usb4DrChildPdo::Device() const
{
    return m_Device;
}

const Usb4DrRouterIdentity*
Usb4DrChildPdo::Identity() const
{
    return &m_Identity;
}

Usb4DrPort*
Usb4DrChildPdo::Port()
{
    if (m_Fdo == NULL)
        return NULL;

    return m_Fdo->Ports()->DfpByLane0(m_Identity.Lane0AdapterNumber);
}

NTSTATUS
Usb4DrChildPdo::AssignIds(
    _In_ const Usb4DrRouterIdentity* Identity,
    _In_ PWDFDEVICE_INIT DeviceInit)
{
    WCHAR ShortBuffer[USB4DR_PDO_ID_CHARS];
    WCHAR FullBuffer[USB4DR_PDO_ID_CHARS];
    WCHAR InstanceBuffer[USB4DR_PDO_INSTANCE_CHARS];
    UNICODE_STRING ShortId;
    UNICODE_STRING FullId;
    UNICODE_STRING InstanceId;
    NTSTATUS Status;

    RtlInitEmptyUnicodeString(&ShortId, ShortBuffer, sizeof(ShortBuffer));
    RtlInitEmptyUnicodeString(&FullId, FullBuffer, sizeof(FullBuffer));
    RtlInitEmptyUnicodeString(&InstanceId, InstanceBuffer, sizeof(InstanceBuffer));

    Status = RtlUnicodeStringPrintf(&ShortId,
                                    L"USB4\\VID_%04X&PID_%04X",
                                    Identity->VendorId,
                                    Identity->ProductId);
    if (NT_SUCCESS(Status))
    {
        Status = RtlUnicodeStringPrintf(&FullId,
                                        L"USB4\\VID_%04X&PID_%04X&REV_%04X",
                                        Identity->VendorId,
                                        Identity->ProductId,
                                        Identity->Revision);
    }

    if (NT_SUCCESS(Status))
        Status = RtlIntegerToUnicodeString(Identity->Lane0AdapterNumber, 10, &InstanceId);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Child router on DFP %u: IDs not formatted 0x%lx\n", Identity->Lane0AdapterNumber, Status);
        return Status;
    }

    /* The device ID leaves out the revision; the full hardware ID comes first */
    Status = WdfPdoInitAssignDeviceID(DeviceInit, &ShortId);
    if (NT_SUCCESS(Status))
        Status = WdfPdoInitAddHardwareID(DeviceInit, &FullId);
    if (NT_SUCCESS(Status))
        Status = WdfPdoInitAddHardwareID(DeviceInit, &ShortId);

    /* An unknown router gets no compatible IDs, so only its raw class applies */
    if (NT_SUCCESS(Status) && Identity->IsKnown)
    {
        if (Identity->Usb4Version < Usb4DrFirstUsb4Version)
            Status = WdfPdoInitAddCompatibleID(DeviceInit, &Usb4DrTbt3RouterCompatibleId);
        if (NT_SUCCESS(Status))
            Status = WdfPdoInitAddCompatibleID(DeviceInit, &Usb4DrRouterCompatibleId);
    }

    if (NT_SUCCESS(Status))
        Status = WdfPdoInitAssignInstanceID(DeviceInit, &InstanceId);

    if (!NT_SUCCESS(Status))
        DPRINT1("Child router on DFP %u: IDs not assigned 0x%lx\n", Identity->Lane0AdapterNumber, Status);

    return Status;
}

NTSTATUS
Usb4DrChildPdo::Initialize(
    _In_ Usb4DrFdo* Fdo,
    _In_ const Usb4DrChildDescription* Description,
    _In_ WDFDEVICE Device)
{
    WDF_QUERY_INTERFACE_CONFIG QueryConfig;
    USB4HR_PARENT_INTERFACE ParentTemplate;
    USB4HR_HARDWARE_SERVICES ServicesTemplate;
    Usb4DrPort* Dfp;
    NTSTATUS Status;

    PAGED_CODE();

    m_Device = Device;
    m_Fdo = Fdo;
    m_Identity = Description->Identity;
    m_Generation = Description->Generation;
    m_RouterHandle = NULL;

    Dfp = Port();
    if (Dfp == NULL)
    {
        DPRINT1("Child router %p: no DFP with lane 0 adapter %u\n", this, m_Identity.Lane0AdapterNumber);
        return STATUS_NO_SUCH_DEVICE;
    }

    /* Domain and position are fixed for the life of the PDO */
    m_DomainId = Fdo->HostLink()->Parent()->DomainId;
    Dfp->ChildTopologyId(&m_TopologyId);

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
        DPRINT1("Child router %p: parent interface not added 0x%lx\n", this, Status);
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
        DPRINT1("Child router %p: hardware services not added 0x%lx\n", this, Status);

    return Status;
}

NTSTATUS
Usb4DrChildPdo::FillParentInterface(
    _Inout_ PUSB4HR_PARENT_INTERFACE Interface)
{
    const USB4HR_PARENT_INTERFACE* Parent = m_Fdo->HostLink()->Parent();
    Usb4DrRouter* Router = m_Fdo->Router();
    ULONG DpIn;

    if (Interface->Header.Size != sizeof(*Interface) ||
        Interface->Header.Version != USB4HR_PARENT_INTERFACE_VERSION)
    {
        DPRINT1("Child router %p: parent interface size %u version %u refused\n",
                this, Interface->Header.Size, Interface->Header.Version);
        return STATUS_NOT_SUPPORTED;
    }

    Interface->Header.Context = this;
    Interface->Header.InterfaceReference = InterfaceNoop;
    Interface->Header.InterfaceDereference = InterfaceNoop;

    /* QUIRK: DpcdCmId is left as the requester filled it, as Windows does */
    Interface->DomainId = m_DomainId;
    Interface->DomainUuid = Parent->DomainUuid;
    Interface->TopologyId = m_TopologyId;
    Interface->RouterHandle = m_RouterHandle;
    Interface->RootRouterPdo = Parent->RootRouterPdo;
    Interface->DomainFlags = Parent->DomainFlags;
    Interface->MaxCmUsb4Version = Parent->MaxCmUsb4Version;
    Interface->HostInterfaceVersion = Parent->HostInterfaceVersion;
    Interface->EnhancedUniTmuSupported = Router->IsUsb4V2();

    /* Children size their DP and PCIe buffer shares from these counts */
    Interface->TunnelPolicy = Parent->TunnelPolicy;
    if (m_Fdo->Depth() == 0)
    {
        Interface->TunnelPolicy.HostInterfaceBufferLimit = Router->BufferAllocation(USB4DR_BUFFER_MAX_HI);
        Interface->TunnelPolicy.HostDpAdapters = Router->DpInAdapterCount();
    }

    DpIn = (ULONG)Parent->TunnelPolicy.MaxUpstreamDpIn + Router->DpInAdapterCount();
    Interface->TunnelPolicy.MaxUpstreamDpIn = (UCHAR)min(DpIn, (ULONG)MAXUCHAR);

    Interface->IsLaneBondingSupported = IsLaneBondingSupported;
    Interface->IsDeviceGenerationCurrent = IsDeviceGenerationCurrent;
    Interface->IsAsymmetricSupported = IsAsymmetricSupported;
    Interface->GetMaxEnumerationTime = GetMaxEnumerationTime;
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4DrChildPdo::FillHardwareServices(
    _Inout_ PUSB4HR_HARDWARE_SERVICES Interface)
{
    if (Interface->Header.Size != sizeof(*Interface) ||
        Interface->Header.Version != USB4HR_HARDWARE_SERVICES_VERSION)
    {
        DPRINT1("Child router %p: hardware services size %u version %u refused\n",
                this, Interface->Header.Size, Interface->Header.Version);
        return STATUS_NOT_SUPPORTED;
    }

    /* QUIRK: the child calls the host router directly, header included */
    RtlCopyMemory(Interface, m_Fdo->HostLink()->Services(), sizeof(*Interface));
    return STATUS_SUCCESS;
}

VOID
Usb4DrChildPdo::ForwardToParentDefaultQueue(
    _In_ WDFREQUEST Request,
    _In_ ULONG IoControlCode)
{
    WDF_REQUEST_FORWARD_OPTIONS Options;
    WDFQUEUE ParentQueue;
    NTSTATUS Status;

    ParentQueue = WdfDeviceGetDefaultQueue(m_Fdo->Device());
    if (ParentQueue == NULL)
    {
        DPRINT1("Child router %p: IOCTL 0x%lx has no parent queue to go to\n", this, IoControlCode);
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    WDF_REQUEST_FORWARD_OPTIONS_INIT(&Options);
    Status = WdfRequestForwardToParentDeviceIoQueue(Request, ParentQueue, &Options);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Child router %p: IOCTL 0x%lx not forwarded 0x%lx\n", this, IoControlCode, Status);
        WdfRequestComplete(Request, Status);
    }
}

/* Interface functions ********************************************************/

VOID
NTAPI
Usb4DrChildPdo::InterfaceNoop(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

BOOLEAN
NTAPI
Usb4DrChildPdo::IsLaneBondingSupported(
    _In_ PVOID Context)
{
    Usb4DrPort* Dfp = ((Usb4DrChildPdo*)Context)->Port();

    if (Dfp == NULL)
        return FALSE;

    return Dfp->IsLaneBondingSupported();
}

BOOLEAN
NTAPI
Usb4DrChildPdo::IsAsymmetricSupported(
    _In_ PVOID Context)
{
    Usb4DrPort* Dfp = ((Usb4DrChildPdo*)Context)->Port();

    if (Dfp == NULL)
        return FALSE;

    return Dfp->IsAsymmetricSupported();
}

BOOLEAN
NTAPI
Usb4DrChildPdo::IsDeviceGenerationCurrent(
    _In_ PVOID Context)
{
    Usb4DrChildPdo* Pdo = (Usb4DrChildPdo*)Context;
    Usb4DrPort* Dfp = Pdo->Port();

    if (Dfp == NULL)
        return FALSE;

    return Dfp->IsGenerationCurrent(Pdo->m_Generation);
}

ULONG
NTAPI
Usb4DrChildPdo::GetMaxEnumerationTime(
    _In_ PVOID Context,
    _In_ ULONG Seconds)
{
    Usb4DrPort* Dfp = ((Usb4DrChildPdo*)Context)->Port();

    if (Dfp == NULL)
        return Seconds;

    return Dfp->GetMaxEnumerationTime(Seconds);
}

/* KMDF callbacks *************************************************************/

NTSTATUS
NTAPI
Usb4DrChildPdo::EvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    Usb4DrChildPdo* Pdo = FromDevice(Device);
    Usb4DrPort* Dfp;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    Dfp = Pdo->Port();
    if (Dfp == NULL)
    {
        DPRINT1("Child router %p: DFP gone at prepare hardware\n", Pdo);
        return STATUS_UNSUCCESSFUL;
    }

    /* Only the connection this PDO was reported for may take the handle */
    Status = Dfp->AcquireChildHandle(Pdo->m_Generation, &Pdo->m_RouterHandle);
    if (!NT_SUCCESS(Status) || Pdo->m_RouterHandle == NULL)
    {
        DPRINT1("Child router %p: no router handle for generation %lu, 0x%lx\n",
                Pdo, Pdo->m_Generation, Status);
        Pdo->m_RouterHandle = NULL;
        return STATUS_UNSUCCESSFUL;
    }

    DPRINT("Child router %p: router handle %p\n", Pdo, Pdo->m_RouterHandle);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
Usb4DrChildPdo::EvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    Usb4DrChildPdo* Pdo = FromDevice(Device);
    Usb4DrPort* Dfp;

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (Pdo->m_RouterHandle == NULL)
        return STATUS_SUCCESS;

    Dfp = Pdo->Port();
    if (Dfp != NULL)
        Dfp->ReleaseChildHandle();
    else
        DPRINT1("Child router %p: DFP gone, handle reference not dropped\n", Pdo);

    Pdo->m_RouterHandle = NULL;
    return STATUS_SUCCESS;
}

VOID
NTAPI
Usb4DrChildPdo::EvtIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    Usb4DrChildPdo* Pdo = FromDevice(WdfIoQueueGetDevice(Queue));
    const Usb4DrRequestLimits* Limits;
    Usb4DrPort* Dfp;

    /* Client inter-domain create is only in the parent's table */
    Limits = Usb4DrFindRequestLimits(IoControlCode);
    if (Limits == NULL || IoControlCode == IOCTL_USB4HR_CLIENT_CREATE_INTER_DOMAIN_TUNNEL)
    {
        Pdo->ForwardToParentDefaultQueue(Request, IoControlCode);
        return;
    }

    if (InputBufferLength < Limits->MinInput || OutputBufferLength < Limits->MinOutput)
    {
        /* Windows leaves such a request pending forever */
        DPRINT1("Child router %p: IOCTL 0x%lx buffers %Iu/%Iu below %lu/%lu\n",
                Pdo, IoControlCode, InputBufferLength, OutputBufferLength,
                Limits->MinInput, Limits->MinOutput);
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    Dfp = Pdo->Port();
    if (Dfp == NULL)
    {
        DPRINT1("Child router %p: IOCTL 0x%lx with the DFP gone\n", Pdo, IoControlCode);
        WdfRequestComplete(Request, STATUS_NO_SUCH_DEVICE);
        return;
    }

    Pdo->m_Fdo->Forwarder()->ForwardFromChild(Dfp, Request);
}

NTSTATUS
NTAPI
Usb4DrChildPdo::EvtQueryParentInterface(
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
Usb4DrChildPdo::EvtQueryHardwareServices(
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
