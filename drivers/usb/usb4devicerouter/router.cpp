/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Router startup: basic configuration, capabilities, adapters, Configuration Valid
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4dr.h"

#define NDEBUG
#include <debug.h>

/* USB4 version byte of a version 1 router; anything below is a Thunderbolt 3 router */
#define USB4DR_USB4_VERSION_1           0x20

/* Host interface version a version 2 root router needs */
#define USB4DR_HOST_INTERFACE_VERSION_2 0x40

/* Second dword of a long vendor capability: next pointer in bits 15:0 */
#define USB4DR_VSEC_NEXT_MASK           0x0000FFFF

/* Long vendor capability ID of the TBT3 router capability */
#define USB4DR_ROUTER_VSC_ID_TBT3       USB4DR_TBT3_VSC_1

static
NTSTATUS
NTAPI
Usb4DrRouterAccessResult(
    _In_ NTSTATUS Status,
    _In_ USB4HR_STATUS Usb4Status)
{
    if (!NT_SUCCESS(Status))
        return Status;

    return (Usb4Status == USB4HR_STATUS_SUCCESS) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

NTSTATUS
Usb4DrRouter::Create(
    _In_ Usb4DrFdo* Fdo)
{
    m_Fdo = Fdo;
    m_Drom.Reset();
    return m_Ops.Create(Fdo);
}

BOOLEAN Usb4DrRouter::CanBeEnumerated()
{
    const USB4HR_PARENT_INTERFACE* Parent = m_Fdo->HostLink()->Parent();

    /* QUIRK: a version 2 root router behind an older host interface is not enumerated */
    if (m_Fdo->HostLink()->Depth() == 0 &&
        Usb4Version() >= USB4DR_USB4_VERSION_2 &&
        Parent->MaxCmUsb4Version >= USB4DR_CM_VERSION_2 &&
        Parent->HostInterfaceVersion < USB4DR_HOST_INTERFACE_VERSION_2)
    {
        DPRINT1("Version 2 root router behind host interface version 0x%02x is not enumerated\n",
                Parent->HostInterfaceVersion);
        return FALSE;
    }

    return TRUE;
}

NTSTATUS Usb4DrRouter::WalkCapabilities()
{
    ULONG Offset;
    ULONG Header;
    ULONG Vsec[2];
    ULONG Count;
    UCHAR Id;
    NTSTATUS Status;

    m_TmuCap = 0;
    m_Vsc1Cap = 0;
    m_Vsec6Cap = 0;

    Offset = Usb4DrField(m_Basic[USB4DR_ROUTER_CS_1], USB4DR_ROUTER_CS1_NEXT_CAP_MASK);

    for (Count = 0; Offset != 0; Count++)
    {
        if (Count >= USB4DR_MAX_CAPABILITIES)
        {
            DPRINT1("Router capability list does not end\n");
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        Status = ReadDwords(Offset, 1, &Header);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Router capability read at %lu failed 0x%lx\n", Offset, Status);
            return Status;
        }

        Id = (UCHAR)Usb4DrField(Header, USB4DR_CAP_ID_MASK);

        if (Id == USB4DR_ROUTER_CAP_TMU)
        {
            if (m_TmuCap != 0)
            {
                DPRINT1("Router has a second TMU capability at %lu\n", Offset);
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            m_TmuCap = (USHORT)Offset;
        }
        else if (Id == USB4DR_ROUTER_CAP_VENDOR && Usb4DrField(Header, USB4DR_CAP_VSC_LENGTH_MASK) == 0)
        {
            /* Long form: the short next pointer must be unused, the real one is in the second dword */
            if (Usb4DrField(Header, USB4DR_CAP_NEXT_MASK) != 0)
            {
                DPRINT1("Router vendor capability at %lu has a short next pointer\n", Offset);
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }

            Status = ReadDwords(Offset, RTL_NUMBER_OF(Vsec), Vsec);
            if (!NT_SUCCESS(Status))
                return Status;

            if (Usb4DrField(Vsec[0], USB4DR_CAP_VSC_ID_MASK) == USB4DR_TBT3_VSEC_6 && m_Vsec6Cap == 0)
                m_Vsec6Cap = (USHORT)Offset;

            Offset = Vsec[1] & USB4DR_VSEC_NEXT_MASK;
            continue;
        }
        else if (Id == USB4DR_ROUTER_CAP_VENDOR)
        {
            if (Usb4DrField(Header, USB4DR_CAP_VSC_ID_MASK) == USB4DR_ROUTER_VSC_ID_TBT3 && m_Vsc1Cap == 0)
                m_Vsc1Cap = (USHORT)Offset;
        }

        Offset = Usb4DrField(Header, USB4DR_CAP_NEXT_MASK);
    }

    if (m_TmuCap == 0)
    {
        DPRINT1("Router has no TMU capability\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    return STATUS_SUCCESS;
}

NTSTATUS Usb4DrRouter::AllocateAdapters()
{
    const USB4HR_HARDWARE_SERVICES* Services = m_Fdo->HostLink()->Services();
    USB4HR_HANDLE Handles[USB4DR_MAX_ADAPTERS];
    UCHAR Count = MaxAdapter();
    UCHAR Number;
    NTSTATUS Status;

    ReleaseAdapters();

    if (Count == 0)
        return STATUS_SUCCESS;

    RtlZeroMemory(Handles, sizeof(Handles));
    Status = Services->AllocateAdapterHandles(Services->Header.Context, m_Handle, Count, Handles);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Adapter handle allocation for %u adapters failed 0x%lx\n", Count, Status);
        return Status;
    }

    for (Number = 1; Number <= Count; Number++)
        m_Adapters[Number].Attach(m_Fdo, Number, Handles[Number - 1]);

    m_HandleCount = Count;
    return STATUS_SUCCESS;
}

NTSTATUS Usb4DrRouter::Configure()
{
    const USB4HR_TOPOLOGY_ID* Topology = &m_Fdo->HostLink()->Parent()->TopologyId;
    ULONG Depth = m_Fdo->HostLink()->Depth();
    ULONG Config[4];
    ULONG CmVersion;
    NTSTATUS Status;

    CmVersion = IsUsb4V2() ? USB4DR_CM_VERSION_BYTE_2 : USB4DR_CM_VERSION_BYTE_1;

    /* ROUTER_CS_1..4: depth, topology ID with its valid bit, connection manager version, notification timeout */
    Config[0] = (m_Basic[USB4DR_ROUTER_CS_1] & ~USB4DR_ROUTER_CS1_DEPTH_MASK) |
                ((Depth << 20) & USB4DR_ROUTER_CS1_DEPTH_MASK);

    Config[1] = Topology->Port[0] |
                ((ULONG)Topology->Port[1] << 8) |
                ((ULONG)Topology->Port[2] << 16) |
                ((ULONG)Topology->Port[3] << 24);

    Config[2] = (m_Basic[USB4DR_ROUTER_CS_3] & ~USB4DR_ROUTER_CS3_TOPOLOGY_HIGH) |
                Topology->Port[4] |
                ((ULONG)Topology->Port[5] << 8) |
                ((ULONG)Topology->Port[6] << 16) |
                USB4DR_ROUTER_CS3_TOPOLOGY_VALID;

    Config[3] = (m_Basic[USB4DR_ROUTER_CS_4] & ~(USB4DR_ROUTER_CS4_CM_VERSION_MASK | USB4DR_ROUTER_CS4_NOTIFY_TIMEOUT)) |
                (CmVersion << 8) |
                USB4DR_NOTIFY_TIMEOUT_USB4;

    m_TopologyValidAtPowerUp = (Depth == 0) &&
                               (m_Basic[USB4DR_ROUTER_CS_3] & USB4DR_ROUTER_CS3_TOPOLOGY_VALID) != 0;

    Status = WriteDwords(USB4DR_ROUTER_CS_1, RTL_NUMBER_OF(Config), Config);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Router configuration write failed 0x%lx\n", Status);
        return Status;
    }

    RtlCopyMemory(&m_Basic[USB4DR_ROUTER_CS_1], Config, sizeof(Config));
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4DrRouter::WaitForReadyBit(
    _In_ ULONG Bit)
{
    ULONG Poll;
    NTSTATUS Status;

    for (Poll = 1; ; Poll++)
    {
        Status = ReadDwords(USB4DR_ROUTER_CS_6, 1, &m_Basic[USB4DR_ROUTER_CS_6]);
        if (!NT_SUCCESS(Status))
            return Status;

        if (m_Basic[USB4DR_ROUTER_CS_6] & Bit)
            return STATUS_SUCCESS;

        if (Poll >= USB4DR_READY_POLL_COUNT)
        {
            DPRINT1("Router ROUTER_CS_6 bit 0x%lx never set\n", Bit);
            return STATUS_IO_TIMEOUT;
        }

        Usb4DrSleepMs(Poll > USB4DR_READY_POLL_FAST_COUNT ? USB4DR_READY_POLL_SLOW_MS : USB4DR_READY_POLL_FAST_MS);
    }
}

VOID Usb4DrRouter::ReadDrom()
{
    PUCHAR Data;
    ULONG Size;
    NTSTATUS Status;

    m_Drom.Reset();

    /* A router without a readable DROM still works; it just has no names */
    Status = m_Ops.ReadDrom(&Data, &Size);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DROM read failed 0x%lx\n", Status);
        return;
    }

    Status = m_Drom.Parse(Data, Size);
    if (!NT_SUCCESS(Status))
        DPRINT1("DROM parse stopped with 0x%lx\n", Status);

    ExFreePoolWithTag(Data, USB4DR_TAG_DROM);
}

BOOLEAN Usb4DrRouter::IsUfpInTbt3Mode()
{
    Usb4DrPort* Ufp = m_Fdo->Ports()->Ufp();
    Usb4DrAdapter* Lane0;
    ULONG PortCs18;

    if (!Ufp)
        return FALSE;

    Lane0 = Ufp->Lane0();
    if (!Lane0 || Lane0->PortCapability() == 0)
        return FALSE;

    if (!NT_SUCCESS(Lane0->ReadDword(Lane0->PortCapability() + USB4DR_PORT_CS_18, &PortCs18)))
        return FALSE;

    return (PortCs18 & USB4DR_PORT_CS18_TBT3_COMPAT) != 0;
}

NTSTATUS Usb4DrRouter::EnableTunneling()
{
    BOOLEAN HasPcie = FALSE;
    BOOLEAN HasUsb3 = FALSE;
    BOOLEAN Pcie;
    BOOLEAN Usb3;
    BOOLEAN HostController;
    ULONG Cs5;
    ULONG Number;
    NTSTATUS Status;

    for (Number = 1; Number <= m_HandleCount; Number++)
    {
        if (m_Adapters[Number].IsPcie())
            HasPcie = TRUE;
        if (m_Adapters[Number].IsUsb3())
            HasUsb3 = TRUE;
    }

    Pcie = HasPcie && !IsPcieTunnelingDisabled();
    Usb3 = HasUsb3 && !IsUfpInTbt3Mode();

    /* With PCIe but no USB3 tunneling the router may expose its own xHCI over PCIe instead */
    HostController = Pcie && !Usb3 && (m_Basic[USB4DR_ROUTER_CS_6] & USB4DR_ROUTER_CS6_HOST_CONTROLLER) != 0;

    Status = ReadDwords(USB4DR_ROUTER_CS_5, 1, &Cs5);
    if (!NT_SUCCESS(Status))
        return Status;

    Cs5 &= ~(USB4DR_ROUTER_CS5_RESERVED_23 |
             USB4DR_ROUTER_CS5_PCIE_TUNNELING |
             USB4DR_ROUTER_CS5_USB3_TUNNELING |
             USB4DR_ROUTER_CS5_HOST_CONTROLLER);
    if (Pcie)
        Cs5 |= USB4DR_ROUTER_CS5_PCIE_TUNNELING;
    if (Usb3)
        Cs5 |= USB4DR_ROUTER_CS5_USB3_TUNNELING;
    if (HostController)
        Cs5 |= USB4DR_ROUTER_CS5_HOST_CONTROLLER;

    Status = WriteDwords(USB4DR_ROUTER_CS_5, 1, &Cs5);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Router tunneling enable write failed 0x%lx\n", Status);
        return Status;
    }

    Cs5 |= USB4DR_ROUTER_CS5_CONFIG_VALID;
    Status = WriteDwords(USB4DR_ROUTER_CS_5, 1, &Cs5);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Router Configuration Valid write failed 0x%lx\n", Status);
        return Status;
    }

    m_Basic[USB4DR_ROUTER_CS_5] = Cs5;
    DPRINT("Router tunneling PCIe %u USB3 %u host controller %u\n", Pcie, Usb3, HostController);

    return WaitForReadyBit(USB4DR_ROUTER_CS6_CONFIG_READY);
}

VOID Usb4DrRouter::ClassifyAdapters()
{
    const USB4HR_HARDWARE_SERVICES* Services = m_Fdo->HostLink()->Services();
    Usb4DrAdapter* Adapter;
    ULONG Number;

    m_AdapterSupport = 0;
    m_DpInCount = 0;

    for (Number = 1; Number <= m_HandleCount; Number++)
    {
        Adapter = &m_Adapters[Number];
        if (!Adapter->IsPresent())
            continue;

        /* Windows leaves an adapter the DROM marks unused alone; a TBT3 router keeps it */
        if (!m_IsTbt3 && m_Drom.IsAdapterUnused((UCHAR)Number))
        {
            DPRINT("Adapter %lu is marked unused in the DROM\n", Number);
            Adapter->MarkDromUnused();
            Services->PurgeEventQueues(Services->Header.Context, Adapter->Handle(), TRUE, TRUE);
            continue;
        }

        if (Adapter->IsLane())
            m_AdapterSupport |= USB4HR_ADAPTER_SUPPORT_LANE;
        if (Adapter->IsPcie() && !IsPcieTunnelingDisabled())
            m_AdapterSupport |= USB4HR_ADAPTER_SUPPORT_PCIE;
        if (Adapter->IsDp())
            m_AdapterSupport |= USB4HR_ADAPTER_SUPPORT_DP;
        if (Adapter->IsUsb3())
            m_AdapterSupport |= USB4HR_ADAPTER_SUPPORT_USB3;
        if (Adapter->IsDpIn() && m_DpInCount < MAXUCHAR)
            m_DpInCount++;
    }
}

VOID Usb4DrRouter::ReleaseAdapters()
{
    ULONG Number;

    for (Number = 1; Number <= m_HandleCount; Number++)
        m_Adapters[Number].Destroy();

    m_HandleCount = 0;
    m_AdapterSupport = 0;
    m_DpInCount = 0;
}

NTSTATUS
Usb4DrRouter::Start(
    _In_ BOOLEAN FirstStart)
{
    Usb4DrHostLink* HostLink = m_Fdo->HostLink();
    const USB4HR_PARENT_INTERFACE* Parent;
    Usb4DrAdapter* Adapter;
    ULONG Depth;
    UCHAR Number;
    NTSTATUS Status;

    /* The parent hands out a new router handle after every reconnect */
    Status = HostLink->RefreshParentInterface();
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Parent interface refresh failed 0x%lx\n", Status);
        return Status;
    }

    Parent = HostLink->Parent();
    Depth = HostLink->Depth();
    m_Handle = HostLink->RouterHandle();

    Status = ReadDwords(USB4DR_ROUTER_CS_0, USB4DR_ROUTER_HEADER_DWORDS, m_Basic);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Router header read failed 0x%lx\n", Status);
        return Status;
    }

    m_IsTbt3 = (Usb4Version() < USB4DR_USB4_VERSION_1);
    if (m_IsTbt3)
    {
        if (Parent->TunnelPolicy.PcieTunnelingDisabled && !Usb4DrDriver.Policy.ForceTbt3EnumOnPcieDisabled)
        {
            DPRINT1("TBT3 router %04x:%04x is not enumerated while PCIe tunneling is disabled\n",
                    VendorId(), ProductId());
            return STATUS_NOT_SUPPORTED;
        }

        DPRINT1("TBT3 router %04x:%04x is not supported; it stays idle\n", VendorId(), ProductId());
        return STATUS_SUCCESS;
    }

    if (!FirstStart &&
        Parent->IsDeviceGenerationCurrent &&
        !Parent->IsDeviceGenerationCurrent(Parent->Header.Context))
    {
        DPRINT1("Router was replaced while the system was asleep\n");
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    Status = ReadDwords(USB4DR_ROUTER_CS_0, USB4DR_ROUTER_BASIC_DWORDS, m_Basic);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Router basic config read failed 0x%lx\n", Status);
        return Status;
    }

    if (FirstStart)
    {
        if (!CanBeEnumerated())
            return STATUS_NOT_SUPPORTED;

        Status = AllocateAdapters();
        if (!NT_SUCCESS(Status))
            return Status;

        Status = WalkCapabilities();
        if (!NT_SUCCESS(Status))
            return Status;

        Status = ReadDwords(USB4DR_ROUTER_CS_0, USB4DR_ROUTER_HEADER_DWORDS, m_Basic);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    else if ((m_Basic[USB4DR_ROUTER_CS_3] & USB4DR_ROUTER_CS3_TOPOLOGY_VALID) &&
             WdfDeviceGetSystemPowerAction(m_Fdo->Device()) != PowerActionHibernate)
    {
        /* Windows fails the start here; programming the router again is harmless */
        DPRINT1("Router kept its configuration across sleep; programming it again\n");
    }

    Status = Configure();
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WaitForReadyBit(USB4DR_ROUTER_CS6_ROUTER_READY);
    if (!NT_SUCCESS(Status))
        return Status;

    if (FirstStart)
    {
        for (Number = 1; Number <= m_HandleCount; Number++)
        {
            Adapter = &m_Adapters[Number];
            Status = Adapter->Initialize(m_Fdo, Number, Adapter->Handle());

            /* An adapter number the router does not implement keeps its handle until destroy */
            if (Status == STATUS_NO_SUCH_DEVICE)
                continue;
            if (!NT_SUCCESS(Status))
                return Status;
        }
    }

    /* Windows requires the buffer allocation values; without them USB3 gets no lane credits */
    Status = m_Ops.QueryBufferAllocation(m_BufferAllocation);
    if (!NT_SUCCESS(Status))
        return Status;

    if (FirstStart)
    {
        ReadDrom();
        ClassifyAdapters();
    }

    m_Fdo->PublishRouterProperties();
    m_Fdo->QueryDeviceFlags();
    m_Fdo->PublishDromProperties();

    if (FirstStart)
    {
        Status = m_Fdo->Ports()->Build(m_Fdo);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Port build failed 0x%lx\n", Status);
            return Status;
        }

        Status = m_Fdo->Tunnels()->Build();
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Tunnel build failed 0x%lx\n", Status);
            return Status;
        }

        m_Fdo->PublishAdapterSupport(m_AdapterSupport);
    }

    /* The host router owns Configuration Valid of the root router */
    if (Depth > 0)
    {
        Status = EnableTunneling();
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Status = m_Fdo->Ports()->Start(FirstStart);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Port start failed 0x%lx\n", Status);
        return Status;
    }

    m_Fdo->Tunnels()->OnRouterStarted(FirstStart);

    DPRINT("Router %04x:%04x depth %lu version 0x%02x started\n",
           VendorId(), ProductId(), Depth, Usb4Version());
    return STATUS_SUCCESS;
}

VOID
Usb4DrRouter::Stop(
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    /* Other D states keep the handles; the router is reprogrammed on the way back */
    if (TargetState == WdfPowerDeviceD3Final)
        ReleaseAdapters();
}

VOID Usb4DrRouter::Destroy()
{
    ReleaseAdapters();
    m_Drom.Reset();
    m_Handle = NULL;
}

USB4HR_HANDLE
Usb4DrRouter::Handle() const
{
    return m_Handle;
}

BOOLEAN
Usb4DrRouter::IsTbt3() const
{
    return m_IsTbt3;
}

BOOLEAN
Usb4DrRouter::IsUsb4V2() const
{
    const USB4HR_PARENT_INTERFACE* Parent = m_Fdo->HostLink()->Parent();

    return Usb4Version() >= USB4DR_USB4_VERSION_2 && Parent->MaxCmUsb4Version >= USB4DR_CM_VERSION_2;
}

BOOLEAN
Usb4DrRouter::NeedsHostCleanup() const
{
    /* A version 2 root router behind a version 2 connection manager keeps its state */
    if (m_Fdo->Depth() == 0 && IsUsb4V2())
        return FALSE;

    return m_TopologyValidAtPowerUp;
}

UCHAR
Usb4DrRouter::Usb4Version() const
{
    return (UCHAR)Usb4DrField(m_Basic[USB4DR_ROUTER_CS_4], USB4DR_ROUTER_CS4_VERSION_MASK);
}

USHORT
Usb4DrRouter::VendorId() const
{
    return (USHORT)Usb4DrField(m_Basic[USB4DR_ROUTER_CS_0], USB4DR_ROUTER_CS0_VENDOR_MASK);
}

USHORT
Usb4DrRouter::ProductId() const
{
    return (USHORT)Usb4DrField(m_Basic[USB4DR_ROUTER_CS_0], USB4DR_ROUTER_CS0_PRODUCT_MASK);
}

UCHAR
Usb4DrRouter::Revision() const
{
    return (UCHAR)Usb4DrField(m_Basic[USB4DR_ROUTER_CS_1], USB4DR_ROUTER_CS1_REVISION_MASK);
}

UCHAR
Usb4DrRouter::UpstreamAdapter() const
{
    return (UCHAR)Usb4DrField(m_Basic[USB4DR_ROUTER_CS_1], USB4DR_ROUTER_CS1_UPSTREAM_MASK);
}

UCHAR
Usb4DrRouter::MaxAdapter() const
{
    return (UCHAR)Usb4DrField(m_Basic[USB4DR_ROUTER_CS_1], USB4DR_ROUTER_CS1_MAX_ADAPTER_MASK);
}

ULONG
Usb4DrRouter::BasicDword(
    _In_ ULONG Index) const
{
    if (Index >= USB4DR_ROUTER_BASIC_DWORDS)
        return 0;

    return m_Basic[Index];
}

Usb4DrAdapter*
Usb4DrRouter::Adapter(
    _In_ UCHAR Number)
{
    if (Number == 0 || Number > m_HandleCount || Number >= USB4DR_MAX_ADAPTERS)
        return NULL;

    if (!m_Adapters[Number].IsPresent())
        return NULL;

    return &m_Adapters[Number];
}

UCHAR
Usb4DrRouter::BufferAllocation(
    _In_ ULONG Parameter) const
{
    if (Parameter >= RTL_NUMBER_OF(m_BufferAllocation))
        return 0;

    return m_BufferAllocation[Parameter];
}

ULONG
Usb4DrRouter::AdapterSupport() const
{
    return m_AdapterSupport;
}

UCHAR
Usb4DrRouter::DpInAdapterCount() const
{
    return m_DpInCount;
}

BOOLEAN
Usb4DrRouter::IsPcieTunnelingDisabled() const
{
    return m_Fdo->HostLink()->Parent()->TunnelPolicy.PcieTunnelingDisabled != FALSE;
}

const Usb4DrDrom*
Usb4DrRouter::Drom() const
{
    return &m_Drom;
}

Usb4DrRouterOps*
Usb4DrRouter::Ops()
{
    return &m_Ops;
}

NTSTATUS
Usb4DrRouter::ReadDwords(
    _In_ ULONG DwordOffset,
    _In_ ULONG DwordCount,
    _Out_writes_(DwordCount) PULONG Buffer)
{
    USB4HR_STATUS Usb4Status = USB4HR_STATUS_FAILURE;
    NTSTATUS Status;

    Status = m_Fdo->HostLink()->ReadConfig(m_Handle,
                                           USB4HR_SPACE_ROUTER,
                                           DwordOffset,
                                           DwordCount,
                                           Buffer,
                                           &Usb4Status);
    return Usb4DrRouterAccessResult(Status, Usb4Status);
}

NTSTATUS
Usb4DrRouter::WriteDwords(
    _In_ ULONG DwordOffset,
    _In_ ULONG DwordCount,
    _In_reads_(DwordCount) const ULONG* Buffer)
{
    USB4HR_STATUS Usb4Status = USB4HR_STATUS_FAILURE;
    NTSTATUS Status;

    Status = m_Fdo->HostLink()->WriteConfig(m_Handle,
                                            USB4HR_SPACE_ROUTER,
                                            DwordOffset,
                                            DwordCount,
                                            Buffer,
                                            &Usb4Status);
    return Usb4DrRouterAccessResult(Status, Usb4Status);
}
