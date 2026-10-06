/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     USB3 and PCIe adapters: tunnel creation from the up adapter, teardown and rebuild
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4dr.h"

#define NDEBUG
#include <debug.h>

/* Dwords cached from the protocol adapter capability */
#define USB3_CAPABILITY_DWORDS      5
#define PCIE_CAPABILITY_DWORDS      1

/* D0 exit cancels tunnel requests in flight every 100 ms while it waits for the workers */
#define WORKER_CANCEL_POLL_MS       100
#define WORKER_CANCEL_POLL_COUNT    600

/** Folds the host router's USB4 status into the NT status of a config access. */
static
NTSTATUS
NTAPI
Usb4DrConfigResult(
    _In_ NTSTATUS Status,
    _In_ USB4HR_STATUS Usb4Status)
{
    if (!NT_SUCCESS(Status))
        return Status;

    return (Usb4Status == USB4HR_STATUS_SUCCESS) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

/* Usb4DrProtocolAdapter ******************************************************/

NTSTATUS
Usb4DrProtocolAdapter::Initialize(
    _In_ Usb4DrFdo* Fdo,
    _In_ Usb4DrAdapter* Adapter)
{
    NTSTATUS Status;

    m_Fdo = Fdo;
    m_Adapter = Adapter;
    m_TunnelType = Adapter->IsPcie() ? USB4HR_TUNNEL_PCIE : USB4HR_TUNNEL_USB3;
    m_State = Usb4DrTunnelState::Idle;
    m_TunnelHandle = NULL;
    m_ExtendedEncapsulation = FALSE;
    m_DetectRetries = 0;
    RtlZeroMemory(&m_Request, sizeof(m_Request));
    KeInitializeEvent(&m_Ready, NotificationEvent, TRUE);

    m_CapabilityOffset = Adapter->ProtocolCapability();
    if (m_CapabilityOffset == 0)
    {
        DPRINT1("Adapter %u has no protocol adapter capability\n", Adapter->Number());
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Status = ReadCapability(0,
                            (m_TunnelType == USB4HR_TUNNEL_USB3) ? USB3_CAPABILITY_DWORDS : PCIE_CAPABILITY_DWORDS,
                            m_Cs);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Reading the protocol capability of adapter %u failed 0x%08lx\n", Adapter->Number(), Status);
        return Status;
    }

    /* Path Enable left by an earlier connection manager on the root router is cleared */
    if (Fdo->Depth() == 0 && Fdo->Router()->NeedsRootPathScrub())
    {
        Status = ResetPathEnable();
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return ApplyBackPressureWorkaround();
}

Usb4DrAdapter*
Usb4DrProtocolAdapter::Adapter()
{
    return m_Adapter;
}

ULONG
Usb4DrProtocolAdapter::TunnelType() const
{
    return m_TunnelType;
}

BOOLEAN
Usb4DrProtocolAdapter::IsUp() const
{
    return m_Adapter->IsUp();
}

UCHAR
Usb4DrProtocolAdapter::CapabilityOffset() const
{
    return m_CapabilityOffset;
}

ULONG
Usb4DrProtocolAdapter::MaxLinkRateField() const
{
    if (m_TunnelType != USB4HR_TUNNEL_USB3)
        return 0;

    return Usb4DrField(m_Cs[USB4DR_USB3_CS_4], USB4DR_USB3_CS4_MAX_RATE_MASK);
}

USHORT
Usb4DrProtocolAdapter::InputHopId() const
{
    return USB4DR_PROTOCOL_INPUT_HOPID;
}

USHORT
Usb4DrProtocolAdapter::OutputHopId() const
{
    USHORT MaxOutput = m_Adapter->MaxOutputHopId();

    /* QUIRK: Windows uses the adapter's limit when it is 8 or less, else always 9 */
    if (MaxOutput <= USB4DR_PROTOCOL_INPUT_HOPID)
        return MaxOutput;

    return USB4DR_PROTOCOL_OUTPUT_HOPID;
}

Usb4DrTunnelState
Usb4DrProtocolAdapter::State() const
{
    return m_State;
}

USB4HR_HANDLE
Usb4DrProtocolAdapter::TunnelHandle() const
{
    return m_TunnelHandle;
}

NTSTATUS
Usb4DrProtocolAdapter::CreateTunnel()
{
    USB4HR_TUNNEL_BUILD_RESULT Output;
    Usb4DrSegments* Segments = m_Fdo->Tunnels()->Segments();
    NTSTATUS Status;

    if (!IsUp())
        return STATUS_INVALID_DEVICE_REQUEST;

    if (m_TunnelHandle != NULL)
        return STATUS_SUCCESS;

    /* The PCIe link must be back in Detect before the path is set up */
    if (NeedsPcieDetect())
    {
        Status = WaitForPcieDetect();
        if (Status == STATUS_CANCELLED)
            return Status;

        if (!NT_SUCCESS(Status))
        {
            m_State = Usb4DrTunnelState::Failed;
            return Status;
        }
    }

    m_State = Usb4DrTunnelState::Requested;

    Status = Segments->FillChildHalf(this, &m_Request);
    if (!NT_SUCCESS(Status))
    {
        m_State = Usb4DrTunnelState::Failed;
        return Status;
    }

    RtlZeroMemory(&Output, sizeof(Output));
    Status = m_Fdo->ParentLink()->CreateTunnel(&m_Request, &Output);
    if (NT_SUCCESS(Status) && Output.Status == USB4HR_STATUS_SUCCESS && Output.TunnelHandle != NULL)
    {
        m_TunnelHandle = Output.TunnelHandle;
        if (m_TunnelType == USB4HR_TUNNEL_PCIE)
        {
            /* The host router reports whether both ends run extended encapsulation */
            m_ExtendedEncapsulation = Output.ModeEnabled ? TRUE : FALSE;
            DPRINT("PCIe adapter %u extended encapsulation %u\n", m_Adapter->Number(), m_ExtendedEncapsulation);
        }

        m_State = Usb4DrTunnelState::Tunneling;
        DPRINT("Adapter %u tunnel %p is up\n", m_Adapter->Number(), m_TunnelHandle);
        return STATUS_SUCCESS;
    }

    DPRINT1("CREATE_TUNNEL for adapter %u failed 0x%08lx, USB4 status %lu\n",
            m_Adapter->Number(), Status, Output.Status);

    Segments->ReleaseChildHalf(&m_Request);
    RtlZeroMemory(&m_Request, sizeof(m_Request));
    m_State = Usb4DrTunnelState::Failed;

    return NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;
}

NTSTATUS
Usb4DrProtocolAdapter::DestroyTunnel()
{
    USB4HR_STATUS Usb4Status = USB4HR_STATUS_FAILURE;
    NTSTATUS Status;

    if (m_TunnelHandle == NULL)
        return STATUS_SUCCESS;

    Status = m_Fdo->ParentLink()->DestroyTunnel(m_TunnelHandle, &Usb4Status);
    Status = Usb4DrConfigResult(Status, Usb4Status);
    if (!NT_SUCCESS(Status))
    {
        /* The path may still be programmed, so the HopID stays reserved */
        DPRINT1("DESTROY_TUNNEL %p failed 0x%08lx, USB4 status %lu\n", m_TunnelHandle, Status, Usb4Status);
        return Status;
    }

    DPRINT("Adapter %u tunnel %p is down\n", m_Adapter->Number(), m_TunnelHandle);
    m_Fdo->Tunnels()->Segments()->ReleaseChildHalf(&m_Request);
    RtlZeroMemory(&m_Request, sizeof(m_Request));
    m_TunnelHandle = NULL;
    m_ExtendedEncapsulation = FALSE;
    m_State = Usb4DrTunnelState::Idle;
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4DrProtocolAdapter::RebuildTunnel()
{
    USB4HR_STATUS Usb4Status = USB4HR_STATUS_FAILURE;
    NTSTATUS Status;

    if (m_TunnelHandle == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    m_State = Usb4DrTunnelState::Requested;

    if (NeedsPcieDetect())
    {
        Status = WaitForPcieDetect();
        if (Status == STATUS_CANCELLED)
        {
            m_State = Usb4DrTunnelState::PoweredDown;
            return Status;
        }

        if (!NT_SUCCESS(Status))
        {
            /* Windows tears the tunnel down and fails the router, as for a failed rebuild */
            DestroyTunnel();
            m_State = Usb4DrTunnelState::Failed;
            FailDevice();
            return Status;
        }
    }

    Status = m_Fdo->ParentLink()->RebuildTunnel(m_TunnelHandle, &Usb4Status);
    if (Status == STATUS_CANCELLED)
    {
        /* D0 exit got in the way; the host router still holds the tunnel for the next try */
        m_State = Usb4DrTunnelState::PoweredDown;
        return Status;
    }

    Status = Usb4DrConfigResult(Status, Usb4Status);
    if (NT_SUCCESS(Status))
    {
        m_State = Usb4DrTunnelState::Tunneling;
        DPRINT("Adapter %u tunnel %p rebuilt\n", m_Adapter->Number(), m_TunnelHandle);
        return STATUS_SUCCESS;
    }

    DPRINT1("REBUILD_TUNNEL %p failed 0x%08lx, USB4 status %lu\n", m_TunnelHandle, Status, Usb4Status);

    DestroyTunnel();
    m_State = Usb4DrTunnelState::Failed;
    FailDevice();
    return Status;
}

VOID
Usb4DrProtocolAdapter::MarkPoweredDown()
{
    if (m_TunnelHandle != NULL)
        m_State = Usb4DrTunnelState::PoweredDown;
}

NTSTATUS
Usb4DrProtocolAdapter::ApplyBackPressureWorkaround()
{
    USB4HR_STATUS Usb4Status = USB4HR_STATUS_FAILURE;
    UCHAR VendorCapability;
    ULONG Value;
    NTSTATUS Status;

    if (m_TunnelType != USB4HR_TUNNEL_USB3 ||
        !(m_Fdo->DeviceFlags() & USB4DR_FLAG_USB3_BACK_PRESSURE))
    {
        return STATUS_SUCCESS;
    }

    VendorCapability = m_Adapter->VendorCapability();
    if (VendorCapability == 0)
    {
        DPRINT1("Adapter %u has no vendor capability for the back pressure setting\n", m_Adapter->Number());
        return STATUS_SUCCESS;
    }

    Status = m_Fdo->HostLink()->ReadConfig(m_Adapter->Handle(),
                                           USB4HR_SPACE_ADAPTER,
                                           VendorCapability + USB4DR_USB3_VSC_CS_1,
                                           1,
                                           &Value,
                                           &Usb4Status);
    Status = Usb4DrConfigResult(Status, Usb4Status);
    if (!NT_SUCCESS(Status))
        return Status;

    if ((Value & USB4DR_USB3_VSC1_BACK_PRESSURE_MASK) >= USB4DR_USB3_VSC1_BACK_PRESSURE_MIN)
        return STATUS_SUCCESS;

    Value = (Value & ~USB4DR_USB3_VSC1_BACK_PRESSURE_MASK) | USB4DR_USB3_VSC1_BACK_PRESSURE_MIN;

    Status = m_Fdo->HostLink()->WriteConfig(m_Adapter->Handle(),
                                            USB4HR_SPACE_ADAPTER,
                                            VendorCapability + USB4DR_USB3_VSC_CS_1,
                                            1,
                                            &Value,
                                            &Usb4Status);
    return Usb4DrConfigResult(Status, Usb4Status);
}

NTSTATUS
Usb4DrProtocolAdapter::ResetPathEnable()
{
    NTSTATUS Status;

    /* Path Enable is bit 31 of both the USB3 and the PCIe capability */
    C_ASSERT(USB4DR_USB3_CS0_PATH_ENABLE == USB4DR_PCIE_CS0_PATH_ENABLE);

    if (!(m_Cs[0] & USB4DR_USB3_CS0_PATH_ENABLE))
        return STATUS_SUCCESS;

    DPRINT1("Adapter %u still has Path Enable set, clearing it\n", m_Adapter->Number());

    m_Cs[0] &= ~USB4DR_USB3_CS0_PATH_ENABLE;
    Status = WriteCapability(0, m_Cs[0]);
    if (!NT_SUCCESS(Status))
        DPRINT1("Clearing Path Enable on adapter %u failed 0x%08lx\n", m_Adapter->Number(), Status);

    return Status;
}

NTSTATUS
Usb4DrProtocolAdapter::DisconnectUsb3()
{
    ULONG Cs0;
    NTSTATUS Status;

    Status = ReadCapability(USB4DR_USB3_CS_0, 1, &Cs0);
    if (!NT_SUCCESS(Status))
        return Status;

    m_Cs[USB4DR_USB3_CS_0] = Cs0;

    if ((Cs0 & USB4DR_USB3_CS0_VALID) && !(Cs0 & USB4DR_USB3_CS0_PATH_ENABLE))
        return STATUS_SUCCESS;

    Cs0 &= ~(USB4DR_USB3_CS0_VALID | USB4DR_USB3_CS0_PATH_ENABLE);
    Cs0 |= USB4DR_USB3_CS0_VALID;

    Status = WriteCapability(USB4DR_USB3_CS_0, Cs0);
    if (NT_SUCCESS(Status))
        m_Cs[USB4DR_USB3_CS_0] = Cs0;

    return Status;
}

BOOLEAN
Usb4DrProtocolAdapter::NeedsPcieDetect() const
{
    return m_TunnelType == USB4HR_TUNNEL_PCIE && !m_Fdo->Router()->IsTbt3();
}

VOID
Usb4DrProtocolAdapter::ResetPcieDetect()
{
    m_DetectRetries = 0;
}

NTSTATUS
Usb4DrProtocolAdapter::PollPcieDetect()
{
    ULONG Cs0;
    NTSTATUS Status;

    Status = ReadCapability(USB4DR_PCIE_CS_0, 1, &Cs0);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Reading the LTSSM of PCIe adapter %u failed 0x%08lx\n", m_Adapter->Number(), Status);
        return Status;
    }

    m_Cs[USB4DR_PCIE_CS_0] = Cs0;

    if (Usb4DrField(Cs0, USB4DR_PCIE_CS0_LTSSM_MASK) == 0)
        return STATUS_SUCCESS;

    /* The first read plus 50 retries, 100 ms apart */
    if (++m_DetectRetries <= USB4DR_LTSSM_POLL_COUNT)
        return STATUS_PENDING;

    DPRINT1("PCIe adapter %u never reached Detect, LTSSM %lu\n",
            m_Adapter->Number(), Usb4DrField(Cs0, USB4DR_PCIE_CS0_LTSSM_MASK));
    return STATUS_IO_TIMEOUT;
}

NTSTATUS
Usb4DrProtocolAdapter::WaitForPcieDetect()
{
    LARGE_INTEGER Interval;
    NTSTATUS Status;

    ResetPcieDetect();

    for (;;)
    {
        Status = PollPcieDetect();
        if (Status != STATUS_PENDING)
            return Status;

        Interval = Usb4DrRelativeMs(USB4DR_LTSSM_POLL_MS);
        if (KeWaitForSingleObject(m_StopEvent, Executive, KernelMode, FALSE, &Interval) == STATUS_SUCCESS)
            return STATUS_CANCELLED;
    }
}

NTSTATUS
Usb4DrProtocolAdapter::WaitUntilReady()
{
    NTSTATUS Status;

    /* Windows drops a request that arrives during the Detect wait; it waits here instead */
    if (!KeReadStateEvent(&m_Ready))
    {
        Status = Usb4DrFdo::WaitBounded(&m_Ready, "protocol down adapter start");
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* A failed adapter cancels every tunnel request, as Windows does */
    if (m_State == Usb4DrTunnelState::Failed)
    {
        DPRINT1("Down adapter %u failed, the tunnel request is canceled\n", m_Adapter->Number());
        return STATUS_CANCELLED;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Usb4DrProtocolAdapter::ReadCapability(
    _In_ ULONG Index,
    _In_ ULONG Count,
    _Out_writes_(Count) PULONG Buffer)
{
    USB4HR_STATUS Usb4Status = USB4HR_STATUS_FAILURE;
    NTSTATUS Status;

    Status = m_Fdo->HostLink()->ReadConfig(m_Adapter->Handle(),
                                           USB4HR_SPACE_ADAPTER,
                                           m_CapabilityOffset + Index,
                                           Count,
                                           Buffer,
                                           &Usb4Status);
    return Usb4DrConfigResult(Status, Usb4Status);
}

NTSTATUS
Usb4DrProtocolAdapter::WriteCapability(
    _In_ ULONG Index,
    _In_ ULONG Value)
{
    USB4HR_STATUS Usb4Status = USB4HR_STATUS_FAILURE;
    NTSTATUS Status;

    Status = m_Fdo->HostLink()->WriteConfig(m_Adapter->Handle(),
                                            USB4HR_SPACE_ADAPTER,
                                            m_CapabilityOffset + Index,
                                            1,
                                            &Value,
                                            &Usb4Status);
    return Usb4DrConfigResult(Status, Usb4Status);
}

VOID
Usb4DrProtocolAdapter::FailDevice()
{
    DPRINT1("Protocol adapter %u failed, restarting the router\n", m_Adapter->Number());
    WdfDeviceSetFailed(m_Fdo->Device(), WdfDeviceFailedAttemptRestart);
}

/* Usb4DrTunnels **************************************************************/

NTSTATUS
Usb4DrTunnels::Create(
    _In_ Usb4DrFdo* Fdo)
{
    NTSTATUS Status;

    m_Fdo = Fdo;
    m_Adapters = NULL;
    m_AdapterCount = 0;
    m_RebuildPending = FALSE;
    m_Resuming = FALSE;
    m_Stopping = FALSE;
    KeInitializeEvent(&m_WorkIdle, NotificationEvent, TRUE);
    KeInitializeEvent(&m_DownWorkIdle, NotificationEvent, TRUE);
    KeInitializeEvent(&m_StopEvent, NotificationEvent, FALSE);

    Status = m_Segments.Create(Fdo);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = CreateWorkItem(EvtTunnelWork, &m_Work);
    if (!NT_SUCCESS(Status))
        return Status;

    return CreateWorkItem(EvtDownAdapterWork, &m_DownWork);
}

NTSTATUS
Usb4DrTunnels::CreateWorkItem(
    _In_ PFN_WDF_WORKITEM Callback,
    _Out_ WDFWORKITEM* WorkItem)
{
    WDF_WORKITEM_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;

    WDF_WORKITEM_CONFIG_INIT(&Config, Callback);
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = m_Fdo->Device();

    Status = WdfWorkItemCreate(&Config, &Attributes, WorkItem);
    if (!NT_SUCCESS(Status))
        DPRINT1("Creating a tunnel work item failed 0x%08lx\n", Status);

    return Status;
}

NTSTATUS
Usb4DrTunnels::Build()
{
    const USB4HR_HARDWARE_SERVICES* Services = m_Fdo->HostLink()->Services();
    Usb4DrRouter* Router = m_Fdo->Router();
    BOOLEAN PcieAllowed = !Router->IsPcieTunnelingDisabled();
    Usb4DrAdapter* Adapter;
    ULONG DisabledPcieDown = 0;
    ULONG Count = 0;
    ULONG Number;
    NTSTATUS Status;

    for (Number = 1; Number <= Router->MaxAdapter(); Number++)
    {
        Adapter = Router->Adapter((UCHAR)Number);
        if (Adapter == NULL || !Adapter->IsPresent())
            continue;

        /* USB3 adapters never deliver hot plug events to the device router either */
        if (Adapter->IsUsb3())
            Services->PurgeEventQueues(Services->Header.Context, Adapter->Handle(), TRUE, TRUE);

        if (Adapter->IsPcie())
        {
            /* PCIe adapters never deliver hot plug events to the device router */
            Services->PurgeEventQueues(Services->Header.Context, Adapter->Handle(), TRUE, TRUE);

            if (!PcieAllowed)
            {
                DPRINT("PCIe adapter %lu stays unused, the tunnel policy disabled PCIe\n", Number);
                if (!Adapter->IsUp())
                    DisabledPcieDown++;
                continue;
            }
        }

        if (Adapter->IsUsb3() || Adapter->IsPcie())
            Count++;
    }

    /* Built once per router; a start after D3Final only had new adapter handles to silence */
    if (m_Adapters != NULL)
        return STATUS_SUCCESS;

    m_Segments.BuildAllocators();

    if (Count != 0)
    {
        m_Adapters = (Usb4DrProtocolAdapter*)ExAllocatePoolWithTag(NonPagedPool,
                                                                   Count * sizeof(*m_Adapters),
                                                                   USB4DR_TAG_TUNNEL);
        if (m_Adapters == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        RtlZeroMemory(m_Adapters, Count * sizeof(*m_Adapters));
    }

    /* Ascending adapter numbers: DFP n pairs with the n-th down adapter of each protocol */
    for (Number = 1; Number <= Router->MaxAdapter() && m_AdapterCount < Count; Number++)
    {
        Adapter = Router->Adapter((UCHAR)Number);
        if (Adapter == NULL || !Adapter->IsPresent())
            continue;

        if (!Adapter->IsUsb3() && !(Adapter->IsPcie() && PcieAllowed))
            continue;

        Status = m_Adapters[m_AdapterCount].Initialize(m_Fdo, Adapter);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Protocol adapter %lu failed to initialize 0x%08lx\n", Number, Status);
            return Status;
        }

        m_Adapters[m_AdapterCount].m_StopEvent = &m_StopEvent;
        m_AdapterCount++;
    }

    DPRINT("%lu USB3 and %lu PCIe adapters\n",
           CountAdapters(USB4HR_TUNNEL_USB3, TRUE) + CountAdapters(USB4HR_TUNNEL_USB3, FALSE),
           CountAdapters(USB4HR_TUNNEL_PCIE, TRUE) + CountAdapters(USB4HR_TUNNEL_PCIE, FALSE));

    return ValidateAdapters(DisabledPcieDown);
}

ULONG
Usb4DrTunnels::CountAdapters(
    _In_ ULONG TunnelType,
    _In_ BOOLEAN Up)
{
    ULONG Count = 0;
    ULONG Index;

    for (Index = 0; Index < m_AdapterCount; Index++)
    {
        if (m_Adapters[Index].TunnelType() == TunnelType && m_Adapters[Index].IsUp() == Up)
            Count++;
    }

    return Count;
}

NTSTATUS
Usb4DrTunnels::ValidateAdapters(
    _In_ ULONG DisabledPcieDown)
{
    Usb4DrProtocolAdapter* Up;
    Usb4DrProtocolAdapter* FirstDown;
    BOOLEAN Tbt3 = m_Fdo->Router()->IsTbt3();
    ULONG DfpCount = m_Fdo->Ports()->DfpCount();
    ULONG PcieDown = CountAdapters(USB4HR_TUNNEL_PCIE, FALSE);
    ULONG Usb3Down = CountAdapters(USB4HR_TUNNEL_USB3, FALSE);
    ULONG Pass;
    ULONG Type;
    ULONG Index;

    /* Below the root, a protocol with down adapters needs an up adapter numbered below them */
    for (Pass = 0; Pass < 2 && m_Fdo->Depth() != 0; Pass++)
    {
        Type = (Pass == 0) ? USB4HR_TUNNEL_PCIE : USB4HR_TUNNEL_USB3;
        Up = NULL;
        FirstDown = DownAdapterForDfp(Type, 0);

        for (Index = 0; Index < m_AdapterCount; Index++)
        {
            if (m_Adapters[Index].TunnelType() == Type && m_Adapters[Index].IsUp())
                Up = &m_Adapters[Index];
        }

        if (FirstDown == NULL)
            continue;

        if (Up == NULL || FirstDown->Adapter()->Number() <= Up->Adapter()->Number())
        {
            DPRINT1("Type %lu down adapter %u has no up adapter below it\n", Type, FirstDown->Adapter()->Number());
            return STATUS_INVALID_PARAMETER;
        }
    }

    /* A USB4 router with downstream ports must have PCIe down adapters, even disabled ones */
    if (!Tbt3 && (m_Fdo->Depth() == 0 || DfpCount != 0) && PcieDown == 0 && DisabledPcieDown == 0)
    {
        DPRINT1("Router with %lu downstream ports has no PCIe down adapter\n", DfpCount);
        return STATUS_INVALID_PARAMETER;
    }

    if (PcieDown != 0 && PcieDown < DfpCount)
    {
        DPRINT1("%lu PCIe down adapters for %lu downstream ports\n", PcieDown, DfpCount);
        return STATUS_INVALID_PARAMETER;
    }

    if (!Tbt3 && Usb3Down < DfpCount)
    {
        DPRINT1("%lu USB3 down adapters for %lu downstream ports\n", Usb3Down, DfpCount);
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

VOID
Usb4DrTunnels::OnRouterStarted(
    _In_ BOOLEAN FirstStart)
{
    BOOLEAN HasUp = FALSE;
    BOOLEAN HasDown = FALSE;
    ULONG Index;

    if (m_AdapterCount == 0)
        return;

    m_Stopping = FALSE;
    m_RebuildPending = !FirstStart;
    m_Resuming = !FirstStart;
    KeClearEvent(&m_StopEvent);

    for (Index = 0; Index < m_AdapterCount; Index++)
    {
        if (m_Adapters[Index].IsUp())
        {
            HasUp = TRUE;
        }
        else
        {
            /* Parent halves wait until the down adapter start is done */
            KeClearEvent(&m_Adapters[Index].m_Ready);
            HasDown = TRUE;
        }
    }

    if (HasDown)
    {
        KeClearEvent(&m_DownWorkIdle);
        WdfWorkItemEnqueue(m_DownWork);
    }

    if (HasUp && m_Fdo->Depth() != 0)
    {
        KeClearEvent(&m_WorkIdle);
        WdfWorkItemEnqueue(m_Work);
    }
}

VOID
Usb4DrTunnels::RunUpAdapters()
{
    ULONG Pass;
    ULONG Type;
    ULONG Index;

    /*
     * Windows runs every adapter on its own and orders nothing between USB3 and PCIe.
     * One worker serves both here, USB3 first so the PCIe Detect wait cannot hold it up.
     */
    for (Pass = 0; Pass < 2; Pass++)
    {
        Type = (Pass == 0) ? USB4HR_TUNNEL_USB3 : USB4HR_TUNNEL_PCIE;

        for (Index = 0; Index < m_AdapterCount && !m_Stopping; Index++)
        {
            if (m_Adapters[Index].IsUp() && m_Adapters[Index].TunnelType() == Type)
                RunUpAdapter(&m_Adapters[Index]);
        }
    }

    m_RebuildPending = FALSE;
}

VOID
Usb4DrTunnels::RunUpAdapter(
    _In_ Usb4DrProtocolAdapter* Adapter)
{
    NTSTATUS Status;

    if (Adapter->TunnelHandle() != NULL)
    {
        if (m_RebuildPending)
            Adapter->RebuildTunnel();

        return;
    }

    Status = Adapter->CreateTunnel();
    if (!NT_SUCCESS(Status) && Status != STATUS_CANCELLED && !m_Stopping)
        Adapter->FailDevice();
}

VOID
Usb4DrTunnels::SettleUsb3DownAdapters()
{
    Usb4DrProtocolAdapter* Adapter;
    NTSTATUS Status;
    ULONG Index;

    for (Index = 0; Index < m_AdapterCount; Index++)
    {
        Adapter = &m_Adapters[Index];
        if (Adapter->IsUp() || Adapter->TunnelType() != USB4HR_TUNNEL_USB3)
            continue;

        /* A tunnel through the adapter is kept for its rebuild and must stay enabled */
        if (!m_Segments.HasTunnelThrough(Adapter->Adapter()->Number()))
        {
            Status = Adapter->DisconnectUsb3();
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("Disconnecting USB3 down adapter %u failed 0x%08lx\n",
                        Adapter->Adapter()->Number(), Status);
                Adapter->m_State = Usb4DrTunnelState::Failed;
                Adapter->FailDevice();
            }
        }

        KeSetEvent(&Adapter->m_Ready, IO_NO_INCREMENT, FALSE);
    }
}

ULONG64
Usb4DrTunnels::PollPcieDownAdapters(
    _In_ ULONG64 Pending)
{
    Usb4DrProtocolAdapter* Adapter;
    NTSTATUS Status;
    ULONG Index;

    for (Index = 0; Index < m_AdapterCount; Index++)
    {
        if (!(Pending & (1ULL << Index)))
            continue;

        Adapter = &m_Adapters[Index];
        Status = Adapter->PollPcieDetect();
        if (Status == STATUS_PENDING)
            continue;

        if (!NT_SUCCESS(Status))
        {
            Adapter->m_State = Usb4DrTunnelState::Failed;

            /* Windows fails the router on a first start; after a resume the adapter just stops */
            if (m_Resuming)
                DPRINT1("PCIe down adapter %u stays unusable after resume\n", Adapter->Adapter()->Number());
            else
                Adapter->FailDevice();
        }

        Pending &= ~(1ULL << Index);
        KeSetEvent(&Adapter->m_Ready, IO_NO_INCREMENT, FALSE);
    }

    return Pending;
}

VOID
Usb4DrTunnels::RunDownAdapters()
{
    LARGE_INTEGER Interval;
    Usb4DrProtocolAdapter* Adapter;
    ULONG64 Pending = 0;
    BOOLEAN Usb3Settled = FALSE;
    ULONG Elapsed = 0;
    ULONG Step;
    ULONG Index;

    /* PCIe down adapters wait for LTSSM Detect while USB3 down adapters settle for 500 ms */
    for (Index = 0; Index < m_AdapterCount; Index++)
    {
        Adapter = &m_Adapters[Index];
        if (Adapter->IsUp() || Adapter->TunnelType() != USB4HR_TUNNEL_PCIE)
            continue;

        if (Adapter->NeedsPcieDetect() && Adapter->State() != Usb4DrTunnelState::Failed)
        {
            Adapter->ResetPcieDetect();
            Pending |= 1ULL << Index;
        }
        else
        {
            KeSetEvent(&Adapter->m_Ready, IO_NO_INCREMENT, FALSE);
        }
    }

    for (;;)
    {
        Pending = PollPcieDownAdapters(Pending);

        if (!Usb3Settled && Elapsed >= USB4DR_USB3_DOWN_SETTLE_MS)
        {
            SettleUsb3DownAdapters();
            Usb3Settled = TRUE;
        }

        if (Pending == 0 && Usb3Settled)
            return;

        Step = (Pending != 0) ? USB4DR_LTSSM_POLL_MS : USB4DR_USB3_DOWN_SETTLE_MS - Elapsed;
        if (!Usb3Settled)
            Step = min(Step, USB4DR_USB3_DOWN_SETTLE_MS - Elapsed);

        Interval = Usb4DrRelativeMs(Step);
        if (KeWaitForSingleObject(&m_StopEvent, Executive, KernelMode, FALSE, &Interval) == STATUS_SUCCESS)
            break;

        Elapsed += Step;
    }

    /* D0 exit: leave the adapters as they are; Stop opens them for waiting parent halves */
    DPRINT("Down adapter start stopped after %lu ms\n", Elapsed);
}

VOID
Usb4DrTunnels::WaitForWorker(
    _In_ PKEVENT Idle,
    _In_ PCSTR What)
{
    LARGE_INTEGER Timeout = Usb4DrRelativeMs(WORKER_CANCEL_POLL_MS);
    ULONG Round;

    /* A request sent just after an earlier cancel needs another one */
    for (Round = 0; Round < WORKER_CANCEL_POLL_COUNT; Round++)
    {
        if (KeReadStateEvent(Idle))
            return;

        m_Fdo->ParentLink()->Cancel();

        if (KeWaitForSingleObject(Idle, Executive, KernelMode, FALSE, &Timeout) == STATUS_SUCCESS)
            return;
    }

    Usb4DrFdo::WaitBounded(Idle, What);
}

VOID
Usb4DrTunnels::Stop(
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    Usb4DrProtocolAdapter* Adapter;
    ULONG Index;

    if (m_AdapterCount == 0)
        return;

    m_Stopping = TRUE;
    KeSetEvent(&m_StopEvent, IO_NO_INCREMENT, FALSE);

    WaitForWorker(&m_DownWorkIdle, "down adapter worker");
    WaitForWorker(&m_WorkIdle, "tunnel worker");

    for (Index = 0; Index < m_AdapterCount; Index++)
    {
        Adapter = &m_Adapters[Index];

        /* Release parent half fills that still wait for this adapter */
        KeSetEvent(&Adapter->m_Ready, IO_NO_INCREMENT, FALSE);

        if (!Adapter->IsUp())
            continue;

        if (TargetState == WdfPowerDeviceD3Final)
            Adapter->DestroyTunnel();
        else
            Adapter->MarkPoweredDown();
    }
}

VOID
Usb4DrTunnels::Destroy()
{
    if (m_Adapters != NULL)
    {
        /* A worker that never came back from D0 exit may still use the adapters */
        if (KeReadStateEvent(&m_WorkIdle) && KeReadStateEvent(&m_DownWorkIdle))
            ExFreePoolWithTag(m_Adapters, USB4DR_TAG_TUNNEL);
        else
            DPRINT1("Tunnel worker still running, leaking %lu protocol adapters\n", m_AdapterCount);

        m_Adapters = NULL;
    }

    m_AdapterCount = 0;
    m_Segments.Destroy();
}

Usb4DrSegments*
Usb4DrTunnels::Segments()
{
    return &m_Segments;
}

Usb4DrProtocolAdapter*
Usb4DrTunnels::DownAdapterForDfp(
    _In_ ULONG TunnelType,
    _In_ ULONG DfpIndex)
{
    ULONG Seen = 0;
    ULONG Index;

    for (Index = 0; Index < m_AdapterCount; Index++)
    {
        if (m_Adapters[Index].IsUp() || m_Adapters[Index].TunnelType() != TunnelType)
            continue;

        if (Seen++ == DfpIndex)
            return &m_Adapters[Index];
    }

    return NULL;
}

VOID
NTAPI
Usb4DrTunnels::EvtTunnelWork(
    _In_ WDFWORKITEM WorkItem)
{
    Usb4DrTunnels* Self = Usb4DrFdo::FromDevice((WDFDEVICE)WdfWorkItemGetParentObject(WorkItem))->Tunnels();

    Self->RunUpAdapters();
    KeSetEvent(&Self->m_WorkIdle, IO_NO_INCREMENT, FALSE);
}

VOID
NTAPI
Usb4DrTunnels::EvtDownAdapterWork(
    _In_ WDFWORKITEM WorkItem)
{
    Usb4DrTunnels* Self = Usb4DrFdo::FromDevice((WDFDEVICE)WdfWorkItemGetParentObject(WorkItem))->Tunnels();

    Self->RunDownAdapters();
    KeSetEvent(&Self->m_DownWorkIdle, IO_NO_INCREMENT, FALSE);
}
