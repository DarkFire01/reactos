/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     USB3 and PCIe tunnels: path programming, USB3 bandwidth, power down and rebuild
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4hr.h"

#define NDEBUG
#include <debug.h>

/* Pending packet poll after a path entry is invalidated */
static const ULONG Usb4HrHopDrainPollMs = 16;
static const ULONG Usb4HrHopDrainLimitMs = 100;
static const ULONG Usb4HrHopPollFirstStallUs = 3;

/* USB3 bandwidth request handshake: attempts to see HCA and the delay between them */
static const ULONG Usb4HrUsb3AckAttempts = 5;
static const ULONG Usb4HrUsb3AckDelayMs = 20;

/* Link and USB3 rates: 9 bandwidth units (100 Mbps) per Gbps */
static const USHORT Usb4HrUnitsPerGbps = 9;
static const USHORT Usb4HrUsb3Gen1Gbps = 10;
static const USHORT Usb4HrUsb3Gen2Gbps = 20;
static const USHORT Usb4HrUsb3LimitedGbps = 18;

/* USB4 capability header in adapter space and the lane adapter capability ID */
static const ULONG Usb4HrCapNextMask = 0x000000FF;
static const ULONG Usb4HrCapIdShift = 8;
static const ULONG Usb4HrCapIdMask = 0x0000FF00;
static const ULONG Usb4HrCapIdLane = 0x01;
static const ULONG Usb4HrCapWalkLimit = 32;

/* Current link speed and negotiated width codes of the lane adapter CS1 */
static const ULONG Usb4HrLaneSpeedGen2 = 0x8;
static const ULONG Usb4HrLaneSpeedGen3 = 0x4;
static const ULONG Usb4HrLaneWidthDual = 0x2;

/* Path enable values of the protocol adapter CS0 */
static const ULONG Usb4HrUsb3EnableMask = USB4HR_USB3_CS0_PATH_ENABLE | USB4HR_USB3_CS0_VALID;
static const ULONG Usb4HrPcieEnableMask = USB4HR_PCIE_CS0_PATH_ENABLE;

/** One hop of a path: the entry at IngressHopId in the path space of Adapter. */
struct Usb4HrTunnelManager::Hop
{
    USB4HR_HANDLE Adapter;
    Usb4HrRoute Route;
    BOOLEAN UsesV2Fields;       /**< USB4 router running with version 2 path and PCIe fields */
    BOOLEAN ProtocolSide;       /**< ingress is the protocol adapter, egress a lane adapter */
    UCHAR CapabilityOffset;
    USHORT IngressHopId;
    USHORT OutputHopId;
    UCHAR OutputAdapter;
    UCHAR Credits;
    UCHAR Weight;
    UCHAR Priority;
    BOOLEAN IngressFlowControl;
    BOOLEAN EgressFlowControl;
    BOOLEAN IngressShared;
    BOOLEAN EgressShared;
    BOOLEAN PmPackets;
};

/** A USB3 or PCIe tunnel. Its address is the tunnel handle. */
struct Usb4HrTunnelManager::Tunnel
{
    LIST_ENTRY Link;
    ULONG Type;
    BOOLEAN PoweredDown;
    BOOLEAN ExtendedEncapsulation;

    /** Hop 0 of each path starts at a protocol adapter: outbound at the down adapter, inbound at the up one. */
    Hop Outbound[USB4HR_PROTOCOL_PATH_HOPS];
    Hop Inbound[USB4HR_PROTOCOL_PATH_HOPS];

    /** USB3 tunnel from the host router: bandwidth lives in its USB3 down adapter. */
    BOOLEAN OwnsUsb3Bandwidth;
    UCHAR Usb3Scale;
    USHORT Usb3Allocated;       /**< 100 Mbps units, each direction */
};

typedef struct _USB4HR_TUNNEL_QUEUE_CONTEXT
{
    Usb4HrTunnelManager* Manager;
} USB4HR_TUNNEL_QUEUE_CONTEXT, *PUSB4HR_TUNNEL_QUEUE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(USB4HR_TUNNEL_QUEUE_CONTEXT, Usb4HrGetTunnelQueueContext);

static
ULONG
NTAPI
Usb4HrPathDword(
    _In_ USHORT HopId)
{
    return USB4HR_PATH_DWORDS_PER_HOP * (ULONG)HopId;
}

/** Windows keys the PM packet bit and the PCIe encapsulation default on any USB4 version but 0 and 0x20. */
static
BOOLEAN
NTAPI
Usb4HrUsesV2Fields(
    _In_ const Usb4HrHandleInfo* Info)
{
    return Info->RouterType == Usb4HrRouterUsb4 && (Info->Usb4Version & ~0x20UL) != 0;
}

static
BOOLEAN
NTAPI
Usb4HrRouteIsWithin(
    _In_ const Usb4HrRoute* Route,
    _In_ const Usb4HrRoute* Ancestor)
{
    UCHAR Index;

    if (Route->Depth < Ancestor->Depth)
        return FALSE;

    for (Index = 0; Index < Ancestor->Depth; Index++)
    {
        if (Route->Port[Index] != Ancestor->Port[Index])
            return FALSE;
    }

    return TRUE;
}

/** 100 Mbps units to the 12 bit figure of the USB3 adapter registers at the given scale. */
static
ULONG
NTAPI
Usb4HrUsb3RegisterBandwidth(
    _In_ USHORT Units,
    _In_ UCHAR Scale)
{
    ULONG64 Scaled = (ULONG64)Units * 100000;

    Scaled >>= (12 + Scale);
    if (Scaled > USB4HR_USB3_CS2_UP_MASK)
        DPRINT1("USB3 bandwidth %u does not fit the register at scale %u\n", Units, Scale);

    return (ULONG)Scaled & USB4HR_USB3_CS2_UP_MASK;
}

static
VOID
NTAPI
Usb4HrFreeTunnel(
    _In_ PVOID Entry)
{
    ExFreePoolWithTag(Entry, USB4HR_TAG_TUNNEL);
}

NTSTATUS
Usb4HrTunnelManager::Create(
    _In_ Usb4HrHostRouter* HostRouter)
{
    WDF_IO_QUEUE_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;

    PAGED_CODE();

    m_HostRouter = HostRouter;
    m_Queue = NULL;
    KeInitializeSpinLock(&m_ListLock);
    InitializeListHead(&m_Tunnels);
    m_PoweredDownCount = 0;
    KeInitializeEvent(&m_PowerUpEvent, NotificationEvent, TRUE);
    KeInitializeEvent(&m_OperationLock, SynchronizationEvent, TRUE);
    m_Initialized = TRUE;

    WDF_IO_QUEUE_CONFIG_INIT(&Config, WdfIoQueueDispatchSequential);
    Config.EvtIoInternalDeviceControl = EvtIoInternalDeviceControl;
    Config.PowerManaged = WdfFalse;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, USB4HR_TUNNEL_QUEUE_CONTEXT);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;

    Status = WdfIoQueueCreate(m_HostRouter->Device(), &Config, &Attributes, &m_Queue);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Tunnel queue creation failed 0x%08lx\n", Status);
        m_Queue = NULL;
        return Status;
    }

    Usb4HrGetTunnelQueueContext(m_Queue)->Manager = this;
    return STATUS_SUCCESS;
}

VOID Usb4HrTunnelManager::Cleanup()
{
    LIST_ENTRY Doomed;
    Tunnel* Entry;
    KIRQL OldIrql;

    PAGED_CODE();

    if (!m_Initialized)
        return;

    InitializeListHead(&Doomed);

    KeAcquireSpinLock(&m_ListLock, &OldIrql);
    while (!IsListEmpty(&m_Tunnels))
        InsertTailList(&Doomed, RemoveHeadList(&m_Tunnels));
    m_PoweredDownCount = 0;
    KeSetEvent(&m_PowerUpEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&m_ListLock, OldIrql);

    /* The hardware is gone or going; Windows frees its tunnels without touching it either */
    while (!IsListEmpty(&Doomed))
    {
        Entry = CONTAINING_RECORD(RemoveHeadList(&Doomed), Tunnel, Link);
        DPRINT("Freeing tunnel %p at cleanup\n", Entry);
        Usb4HrFreeTunnel(Entry);
    }
}

NTSTATUS
Usb4HrTunnelManager::QueueRequest(
    _In_ WDFREQUEST Request)
{
    NTSTATUS Status;

    if (m_Queue == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    Status = WdfRequestForwardToIoQueue(Request, m_Queue);
    if (!NT_SUCCESS(Status))
        DPRINT1("Forwarding tunnel request %p failed 0x%08lx\n", Request, Status);

    return Status;
}

VOID Usb4HrTunnelManager::HandleDomainSleep()
{
    PLIST_ENTRY Link;
    Tunnel* Entry;
    KIRQL OldIrql;

    PAGED_CODE();

    if (!m_Initialized)
        return;

    /* A CREATE in flight must reach the list before the list is marked */
    AcquireOperationLock();
    KeAcquireSpinLock(&m_ListLock, &OldIrql);

    if (m_PoweredDownCount != 0)
        DPRINT1("%lu tunnels were still waiting for a rebuild at power down\n", m_PoweredDownCount);

    for (Link = m_Tunnels.Flink; Link != &m_Tunnels; Link = Link->Flink)
    {
        Entry = CONTAINING_RECORD(Link, Tunnel, Link);
        if (!Entry->PoweredDown)
        {
            Entry->PoweredDown = TRUE;
            m_PoweredDownCount++;
        }
    }

    if (m_PoweredDownCount != 0)
        KeClearEvent(&m_PowerUpEvent);

    KeReleaseSpinLock(&m_ListLock, OldIrql);
    ReleaseOperationLock();

    DPRINT("Domain power down, %lu tunnels to rebuild\n", m_PoweredDownCount);
}

BOOLEAN Usb4HrTunnelManager::HasPoweredDownTunnels()
{
    BOOLEAN Result;
    KIRQL OldIrql;

    if (!m_Initialized)
        return FALSE;

    KeAcquireSpinLock(&m_ListLock, &OldIrql);
    Result = (m_PoweredDownCount != 0);
    KeReleaseSpinLock(&m_ListLock, OldIrql);

    return Result;
}

NTSTATUS
Usb4HrTunnelManager::WaitForPowerUp(
    _In_ ULONG TimeoutMs)
{
    LARGE_INTEGER Timeout = Usb4HrRelativeMs(TimeoutMs);
    NTSTATUS Status;

    PAGED_CODE();

    if (!m_Initialized)
        return STATUS_SUCCESS;

    Status = KeWaitForSingleObject(&m_PowerUpEvent, Executive, KernelMode, FALSE, &Timeout);
    if (Status == STATUS_TIMEOUT)
    {
        DPRINT1("Domain power up did not finish in %lu ms\n", TimeoutMs);
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

VOID
Usb4HrTunnelManager::OnRouterRemoved(
    _In_ const Usb4HrRoute* Route)
{
    LIST_ENTRY Doomed;
    PLIST_ENTRY Link;
    PLIST_ENTRY Next;
    Tunnel* Entry;
    BOOLEAN Affected;
    UCHAR Index;
    KIRQL OldIrql;

    PAGED_CODE();

    if (!m_Initialized)
        return;

    InitializeListHead(&Doomed);
    AcquireOperationLock();

    KeAcquireSpinLock(&m_ListLock, &OldIrql);
    for (Link = m_Tunnels.Flink; Link != &m_Tunnels; Link = Next)
    {
        Next = Link->Flink;
        Entry = CONTAINING_RECORD(Link, Tunnel, Link);

        Affected = FALSE;
        for (Index = 0; Index < USB4HR_PROTOCOL_PATH_HOPS; Index++)
        {
            if (Usb4HrRouteIsWithin(&Entry->Outbound[Index].Route, Route) ||
                Usb4HrRouteIsWithin(&Entry->Inbound[Index].Route, Route))
            {
                Affected = TRUE;
            }
        }

        if (!Affected)
            continue;

        RemoveEntryList(&Entry->Link);
        InsertTailList(&Doomed, &Entry->Link);
    }
    KeReleaseSpinLock(&m_ListLock, OldIrql);

    while (!IsListEmpty(&Doomed))
    {
        Entry = CONTAINING_RECORD(RemoveHeadList(&Doomed), Tunnel, Link);
        DPRINT("Router at depth %u removed, tearing down tunnel %p\n", Route->Depth, Entry);
        Teardown(Entry, MarkPoweredUp(Entry));
        Usb4HrFreeTunnel(Entry);
    }

    ReleaseOperationLock();
}

BOOLEAN
Usb4HrTunnelManager::TunnelHandleKnown(
    _In_ USB4HR_HANDLE Handle)
{
    BOOLEAN Result;
    KIRQL OldIrql;

    if (!m_Initialized || Handle == NULL)
        return FALSE;

    KeAcquireSpinLock(&m_ListLock, &OldIrql);
    Result = (FindTunnelLocked(Handle) != NULL);
    KeReleaseSpinLock(&m_ListLock, OldIrql);

    return Result;
}

VOID
Usb4HrTunnelManager::EvtIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    Usb4HrTunnelManager* Self = Usb4HrGetTunnelQueueContext(Queue)->Manager;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    Self->AcquireOperationLock();

    switch (IoControlCode)
    {
        case IOCTL_USB4HR_CREATE_TUNNEL:
            Self->OnCreateTunnel(Request);
            break;

        case IOCTL_USB4HR_DESTROY_TUNNEL:
            Self->OnDestroyTunnel(Request);
            break;

        case IOCTL_USB4HR_REBUILD_TUNNEL:
            Self->OnRebuildTunnel(Request);
            break;

        default:
            DPRINT1("Tunnel queue got unexpected IOCTL 0x%08lx\n", IoControlCode);
            WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
            break;
    }

    Self->ReleaseOperationLock();
}

VOID
Usb4HrTunnelManager::OnCreateTunnel(
    _In_ WDFREQUEST Request)
{
    PUSB4HR_TUNNEL_BUILD_REQUEST Input;
    PUSB4HR_TUNNEL_BUILD_RESULT Output;
    const USB4HR_PROTOCOL_TUNNEL* Protocol;
    USB4HR_STATUS Usb4Status;
    Tunnel* Entry;
    UCHAR Index;
    KIRQL OldIrql;
    NTSTATUS Status;

    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Input), (PVOID*)&Input, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("CREATE_TUNNEL input buffer missing 0x%08lx\n", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Output), (PVOID*)&Output, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("CREATE_TUNNEL output buffer missing 0x%08lx\n", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    Output->Status = USB4HR_STATUS_FAILURE;
    Output->TunnelHandle = NULL;

    if (Input->TunnelType == USB4HR_TUNNEL_DISPLAYPORT ||
        Input->TunnelType == USB4HR_TUNNEL_INTER_DOMAIN)
    {
        DPRINT1("Tunnel type %lu is not supported\n", Input->TunnelType);
        WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
        return;
    }

    if (Input->TunnelType != USB4HR_TUNNEL_USB3 && Input->TunnelType != USB4HR_TUNNEL_PCIE)
    {
        DPRINT1("Unknown tunnel type %lu\n", Input->TunnelType);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    if (Input->TunnelType == USB4HR_TUNNEL_PCIE && m_HostRouter->TunnelPermissions().PcieTunnelingDisabled)
        DPRINT1("PCIe tunnel requested although the firmware disabled PCIe tunneling\n");

    Entry = (Tunnel*)ExAllocatePoolWithTag(NonPagedPool, sizeof(*Entry), USB4HR_TAG_TUNNEL);
    if (Entry == NULL)
    {
        WdfRequestComplete(Request, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }

    RtlZeroMemory(Entry, sizeof(*Entry));
    Entry->Type = Input->TunnelType;
    Protocol = &Input->Protocol;

    for (Index = 0; Index < USB4HR_PROTOCOL_PATH_HOPS; Index++)
    {
        Status = DescribeHop(&Protocol->Outbound[Index], Entry->Type, Index == 0, &Entry->Outbound[Index]);
        if (NT_SUCCESS(Status))
            Status = DescribeHop(&Protocol->Inbound[Index], Entry->Type, Index == 0, &Entry->Inbound[Index]);

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("CREATE_TUNNEL with an invalid adapter handle in hop %u\n", Index);
            Usb4HrFreeTunnel(Entry);
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }
    }

    if (Entry->Type == USB4HR_TUNNEL_PCIE)
        Entry->ExtendedEncapsulation = Protocol->PcieExtendedEncapsulation ? TRUE : FALSE;

    /* Windows settles USB3 bandwidth before any path entry is written */
    if (Entry->Type == USB4HR_TUNNEL_USB3)
    {
        PlanUsb3Bandwidth(Entry, Protocol->Usb3MaxLinkRate);
        if (Entry->OwnsUsb3Bandwidth)
        {
            Usb4Status = ProgramUsb3Bandwidth(Entry);
            if (Usb4Status != USB4HR_STATUS_SUCCESS)
            {
                DPRINT1("USB3 bandwidth request failed, USB4 status %lu\n", Usb4Status);
                Output->Status = Usb4Status;
                Usb4HrFreeTunnel(Entry);
                WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
                return;
            }
        }
    }

    Status = Configure(Entry, FALSE, Output, &Usb4Status);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Tunnel type %lu configuration failed, USB4 status %lu\n", Entry->Type, Usb4Status);
        Teardown(Entry, FALSE);
        Usb4HrFreeTunnel(Entry);
        Output->Status = Usb4Status;
        WdfRequestComplete(Request, Status);
        return;
    }

    KeAcquireSpinLock(&m_ListLock, &OldIrql);
    InsertTailList(&m_Tunnels, &Entry->Link);
    KeReleaseSpinLock(&m_ListLock, OldIrql);

    DPRINT("Created tunnel %p type %lu\n", Entry, Entry->Type);
    Output->TunnelHandle = Entry;
    Output->Status = USB4HR_STATUS_SUCCESS;
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID
Usb4HrTunnelManager::OnDestroyTunnel(
    _In_ WDFREQUEST Request)
{
    PUSB4HR_TUNNEL_TEARDOWN_REQUEST Input;
    PUSB4HR_TUNNEL_STATUS_OUTPUT Output;
    Tunnel* Entry;
    KIRQL OldIrql;
    NTSTATUS Status;

    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Input), (PVOID*)&Input, NULL);
    if (NT_SUCCESS(Status))
        Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Output), (PVOID*)&Output, NULL);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DESTROY_TUNNEL buffers missing 0x%08lx\n", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    Output->Status = USB4HR_STATUS_FAILURE;

    KeAcquireSpinLock(&m_ListLock, &OldIrql);
    Entry = FindTunnelLocked(Input->TunnelHandle);
    if (Entry != NULL)
        RemoveEntryList(&Entry->Link);
    KeReleaseSpinLock(&m_ListLock, OldIrql);

    if (Entry == NULL)
    {
        DPRINT1("DESTROY_TUNNEL with unknown handle %p\n", Input->TunnelHandle);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    /* A tunnel still waiting for its rebuild has nothing programmed to undo */
    Teardown(Entry, MarkPoweredUp(Entry));

    DPRINT("Destroyed tunnel %p\n", Entry);
    Usb4HrFreeTunnel(Entry);

    Output->Status = USB4HR_STATUS_SUCCESS;
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID
Usb4HrTunnelManager::OnRebuildTunnel(
    _In_ WDFREQUEST Request)
{
    PUSB4HR_TUNNEL_RESTORE_REQUEST Input;
    PUSB4HR_TUNNEL_STATUS_OUTPUT Output;
    USB4HR_STATUS Usb4Status;
    Tunnel* Entry;
    KIRQL OldIrql;
    NTSTATUS Status;

    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Input), (PVOID*)&Input, NULL);
    if (NT_SUCCESS(Status))
        Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Output), (PVOID*)&Output, NULL);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("REBUILD_TUNNEL buffers missing 0x%08lx\n", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    Output->Status = USB4HR_STATUS_FAILURE;

    /* Only this queue and router removal free tunnels, and both hold the operation lock */
    KeAcquireSpinLock(&m_ListLock, &OldIrql);
    Entry = FindTunnelLocked(Input->TunnelHandle);
    KeReleaseSpinLock(&m_ListLock, OldIrql);

    if (Entry == NULL)
    {
        DPRINT1("REBUILD_TUNNEL with unknown handle %p\n", Input->TunnelHandle);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    if (Entry->OwnsUsb3Bandwidth)
    {
        Usb4Status = ProgramUsb3Bandwidth(Entry);
        if (Usb4Status != USB4HR_STATUS_SUCCESS)
        {
            DPRINT1("USB3 bandwidth request failed on rebuild, USB4 status %lu\n", Usb4Status);

            /* QUIRK: the tunnel stays powered down, so power PDO waits run into their timeout as in Windows */
            Output->Status = Usb4Status;
            WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
            return;
        }
    }

    Status = Configure(Entry, TRUE, NULL, &Usb4Status);
    if (!NT_SUCCESS(Status))
        DPRINT1("Rebuilding tunnel %p failed, USB4 status %lu\n", Entry, Usb4Status);

    Output->Status = Usb4Status;

    /* Windows takes it off the powered down list whether or not the rebuild worked */
    MarkPoweredUp(Entry);
    WdfRequestComplete(Request, Status);
}

USB4HR_STATUS
Usb4HrTunnelManager::ReadSpace(
    _In_ USB4HR_HANDLE Adapter,
    _In_ ULONG Space,
    _In_ ULONG DwordOffset,
    _In_ ULONG DwordCount,
    _Out_writes_(DwordCount) PULONG Buffer)
{
    USB4HR_STATUS Usb4Status = USB4HR_STATUS_FAILURE;
    NTSTATUS Status;

    Status = m_HostRouter->Topology()->ReadConfig(Adapter,
                                                  Space,
                                                  DwordOffset,
                                                  DwordCount,
                                                  Buffer,
                                                  &Usb4Status);
    if (!NT_SUCCESS(Status) && Usb4Status == USB4HR_STATUS_SUCCESS)
        Usb4Status = USB4HR_STATUS_FAILURE;

    return Usb4Status;
}

USB4HR_STATUS
Usb4HrTunnelManager::WriteSpace(
    _In_ USB4HR_HANDLE Adapter,
    _In_ ULONG Space,
    _In_ ULONG DwordOffset,
    _In_ ULONG DwordCount,
    _In_reads_(DwordCount) const ULONG* Buffer)
{
    USB4HR_STATUS Usb4Status = USB4HR_STATUS_FAILURE;
    NTSTATUS Status;

    Status = m_HostRouter->Topology()->WriteConfig(Adapter,
                                                   Space,
                                                   DwordOffset,
                                                   DwordCount,
                                                   Buffer,
                                                   &Usb4Status);
    if (!NT_SUCCESS(Status) && Usb4Status == USB4HR_STATUS_SUCCESS)
        Usb4Status = USB4HR_STATUS_FAILURE;

    return Usb4Status;
}

USB4HR_STATUS
Usb4HrTunnelManager::UpdateAdapterDword(
    _In_ USB4HR_HANDLE Adapter,
    _In_ ULONG DwordOffset,
    _In_ ULONG Value,
    _In_ ULONG Mask)
{
    USB4HR_STATUS Usb4Status;
    ULONG Dword;

    Usb4Status = ReadSpace(Adapter, USB4HR_SPACE_ADAPTER, DwordOffset, 1, &Dword);
    if (Usb4Status != USB4HR_STATUS_SUCCESS)
        return Usb4Status;

    Dword = (Dword & ~Mask) | (Value & Mask);
    return WriteSpace(Adapter, USB4HR_SPACE_ADAPTER, DwordOffset, 1, &Dword);
}

NTSTATUS
Usb4HrTunnelManager::DescribeHop(
    _In_ const USB4HR_TUNNEL_SEGMENT* Segment,
    _In_ ULONG Type,
    _In_ BOOLEAN ProtocolSide,
    _Out_ Hop* Out)
{
    Usb4HrHandleInfo Info;
    NTSTATUS Status;

    RtlZeroMemory(Out, sizeof(*Out));

    if (Segment->AdapterHandle == NULL)
        return STATUS_INVALID_PARAMETER;

    Status = m_HostRouter->Topology()->QueryHandle(Segment->AdapterHandle, &Info);
    if (!NT_SUCCESS(Status))
        return Status;

    Out->Adapter = Segment->AdapterHandle;
    Out->Route = Info.Route;
    Out->UsesV2Fields = Usb4HrUsesV2Fields(&Info);
    Out->ProtocolSide = ProtocolSide;
    Out->CapabilityOffset = Segment->CapabilityOffset;
    Out->IngressHopId = Segment->IngressHopId;
    Out->OutputHopId = Segment->OutputHopId;
    Out->OutputAdapter = Segment->OutputAdapter;
    Out->Credits = Segment->PathCredits;
    Out->IngressFlowControl = Segment->IngressFlowControl;
    Out->EgressFlowControl = Segment->EgressFlowControl;
    Out->IngressShared = Segment->IngressSharedBuffering;
    Out->EgressShared = Segment->EgressSharedBuffering;
    Out->PmPackets = Segment->PmPacketSupport;

    if (Type == USB4HR_TUNNEL_USB3)
    {
        Out->Priority = USB4HR_USB3_PATH_PRIORITY;
        Out->Weight = ProtocolSide ? USB4HR_USB3_PATH_WEIGHT_OUT : USB4HR_USB3_PATH_WEIGHT_IN;
    }
    else
    {
        Out->Priority = USB4HR_PCIE_PATH_PRIORITY;
        Out->Weight = USB4HR_PCIE_PATH_WEIGHT;
    }

    if (ProtocolSide && Info.RouterType == Usb4HrRouterUsb4)
        AdoptFixedPathFields(Out);

    return STATUS_SUCCESS;
}

/** A USB4 protocol adapter presets its own path entry; keep its credits and ingress buffering choices. */
VOID
Usb4HrTunnelManager::AdoptFixedPathFields(
    _Inout_ Hop* Entry)
{
    ULONG Path[USB4HR_PATH_DWORDS_PER_HOP];

    if (ReadSpace(Entry->Adapter,
                  USB4HR_SPACE_PATH,
                  Usb4HrPathDword(Entry->IngressHopId),
                  USB4HR_PATH_DWORDS_PER_HOP,
                  Path) != USB4HR_STATUS_SUCCESS)
    {
        DPRINT1("Reading the preset path entry of HopID %u failed\n", Entry->IngressHopId);
        return;
    }

    Entry->Credits = (UCHAR)((Path[0] & USB4HR_PATH_CS0_CREDITS_MASK) >> USB4HR_PATH_CS0_CREDITS_SHIFT);
    Entry->IngressFlowControl = (Path[1] & USB4HR_PATH_CS1_INGRESS_FC) ? TRUE : FALSE;
    Entry->IngressShared = (Path[1] & USB4HR_PATH_CS1_INGRESS_SHARED) ? TRUE : FALSE;
}

USB4HR_STATUS
Usb4HrTunnelManager::ProgramHop(
    _In_ const Hop* Entry)
{
    ULONG Path[USB4HR_PATH_DWORDS_PER_HOP];

    Path[0] = (Entry->OutputHopId & USB4HR_PATH_CS0_OUTPUT_HOPID_MASK) |
              (((ULONG)Entry->OutputAdapter << USB4HR_PATH_CS0_OUTPUT_ADAPTER_SHIFT) &
               USB4HR_PATH_CS0_OUTPUT_ADAPTER_MASK) |
              (((ULONG)Entry->Credits << USB4HR_PATH_CS0_CREDITS_SHIFT) & USB4HR_PATH_CS0_CREDITS_MASK) |
              USB4HR_PATH_CS0_VALID;

    if (!Entry->ProtocolSide && Entry->UsesV2Fields && Entry->PmPackets)
        Path[0] |= USB4HR_PATH_CS0_PM_PACKETS;

    Path[1] = (Entry->Weight & USB4HR_PATH_CS1_WEIGHT_MASK) |
              (((ULONG)Entry->Priority << USB4HR_PATH_CS1_PRIORITY_SHIFT) & USB4HR_PATH_CS1_PRIORITY_MASK);

    if (Entry->IngressFlowControl)
        Path[1] |= USB4HR_PATH_CS1_INGRESS_FC;
    if (Entry->EgressFlowControl)
        Path[1] |= USB4HR_PATH_CS1_EGRESS_FC;
    if (Entry->IngressShared)
        Path[1] |= USB4HR_PATH_CS1_INGRESS_SHARED;
    if (Entry->EgressShared)
        Path[1] |= USB4HR_PATH_CS1_EGRESS_SHARED;

    return WriteSpace(Entry->Adapter,
                      USB4HR_SPACE_PATH,
                      Usb4HrPathDword(Entry->IngressHopId),
                      USB4HR_PATH_DWORDS_PER_HOP,
                      Path);
}

USB4HR_STATUS
Usb4HrTunnelManager::InvalidateHop(
    _In_ const Hop* Entry)
{
    ULONG Path[USB4HR_PATH_DWORDS_PER_HOP];
    USB4HR_STATUS Usb4Status;

    Usb4Status = ReadSpace(Entry->Adapter,
                           USB4HR_SPACE_PATH,
                           Usb4HrPathDword(Entry->IngressHopId),
                           USB4HR_PATH_DWORDS_PER_HOP,
                           Path);
    if (Usb4Status != USB4HR_STATUS_SUCCESS)
        return Usb4Status;

    if (!Entry->ProtocolSide && Entry->UsesV2Fields)
        Path[0] &= ~USB4HR_PATH_CS0_PM_PACKETS;

    Path[0] &= ~USB4HR_PATH_CS0_VALID;

    return WriteSpace(Entry->Adapter,
                      USB4HR_SPACE_PATH,
                      Usb4HrPathDword(Entry->IngressHopId),
                      USB4HR_PATH_DWORDS_PER_HOP,
                      Path);
}

USB4HR_STATUS
Usb4HrTunnelManager::WaitHopDrained(
    _In_ const Hop* Entry)
{
    ULONGLONG Start = KeQueryInterruptTime();
    ULONGLONG ElapsedMs;
    USB4HR_STATUS Usb4Status;
    ULONG Dword;

    for (;;)
    {
        ElapsedMs = (KeQueryInterruptTime() - Start) / 10000;

        /* QUIRK: Windows tests bit 28 of path dword 0, not the Pending Packets bit of dword 1 */
        Usb4Status = ReadSpace(Entry->Adapter,
                               USB4HR_SPACE_PATH,
                               Usb4HrPathDword(Entry->IngressHopId),
                               1,
                               &Dword);
        if (Usb4Status != USB4HR_STATUS_SUCCESS)
            return Usb4Status;

        if ((Dword & USB4HR_PATH_CS1_PENDING_PACKETS) == 0)
            return USB4HR_STATUS_SUCCESS;

        Usb4HrSleepMs(Usb4HrHopDrainPollMs);

        if (ElapsedMs > Usb4HrHopDrainLimitMs)
        {
            DPRINT1("HopID %u still has pending packets\n", Entry->IngressHopId);
            return USB4HR_STATUS_POLLING_TIMEOUT;
        }
    }
}

USB4HR_STATUS
Usb4HrTunnelManager::TeardownHop(
    _In_ const Hop* Entry)
{
    USB4HR_STATUS Usb4Status;

    if (m_HostRouter->Hardware()->HasShimFlag(USB4HR_SHIM_POLL_PENDING_FIRST) && Entry->Route.Depth == 0)
    {
        KeStallExecutionProcessor(Usb4HrHopPollFirstStallUs);
        Usb4Status = WaitHopDrained(Entry);
        if (Usb4Status == USB4HR_STATUS_SUCCESS)
            Usb4Status = InvalidateHop(Entry);
    }
    else
    {
        Usb4Status = InvalidateHop(Entry);
        if (Usb4Status == USB4HR_STATUS_SUCCESS)
            Usb4Status = WaitHopDrained(Entry);
    }

    return Usb4Status;
}

/** Briefly validates the entry with zero credits so the adapter returns them, then invalidates it again. */
USB4HR_STATUS
Usb4HrTunnelManager::ReleaseHopCredits(
    _In_ const Hop* Entry)
{
    ULONG Path[USB4HR_PATH_DWORDS_PER_HOP];
    ULONG Offset = Usb4HrPathDword(Entry->IngressHopId);
    USB4HR_STATUS Usb4Status;

    if (!Entry->IngressFlowControl)
        return USB4HR_STATUS_SUCCESS;

    Usb4Status = ReadSpace(Entry->Adapter, USB4HR_SPACE_PATH, Offset, USB4HR_PATH_DWORDS_PER_HOP, Path);
    if (Usb4Status != USB4HR_STATUS_SUCCESS)
        return Usb4Status;

    Path[0] = (Path[0] & ~USB4HR_PATH_CS0_CREDITS_MASK) | USB4HR_PATH_CS0_VALID;
    Path[1] = (Path[1] & ~USB4HR_PATH_CS1_EGRESS_SHARED) | USB4HR_PATH_CS1_INGRESS_FC;

    Usb4Status = WriteSpace(Entry->Adapter, USB4HR_SPACE_PATH, Offset, USB4HR_PATH_DWORDS_PER_HOP, Path);
    if (Usb4Status != USB4HR_STATUS_SUCCESS)
        return Usb4Status;

    Usb4Status = ReadSpace(Entry->Adapter, USB4HR_SPACE_PATH, Offset, USB4HR_PATH_DWORDS_PER_HOP, Path);
    if (Usb4Status != USB4HR_STATUS_SUCCESS)
        return Usb4Status;

    Path[0] &= ~USB4HR_PATH_CS0_VALID;
    return WriteSpace(Entry->Adapter, USB4HR_SPACE_PATH, Offset, USB4HR_PATH_DWORDS_PER_HOP, Path);
}

USB4HR_STATUS
Usb4HrTunnelManager::ProgramPath(
    _In_reads_(USB4HR_PROTOCOL_PATH_HOPS) const Hop* Path)
{
    USB4HR_STATUS Usb4Status;
    UCHAR Index;

    for (Index = 0; Index < USB4HR_PROTOCOL_PATH_HOPS; Index++)
    {
        Usb4Status = ProgramHop(&Path[Index]);
        if (Usb4Status != USB4HR_STATUS_SUCCESS)
        {
            DPRINT1("Programming HopID %u failed, USB4 status %lu\n", Path[Index].IngressHopId, Usb4Status);
            return Usb4Status;
        }
    }

    return USB4HR_STATUS_SUCCESS;
}

VOID
Usb4HrTunnelManager::TeardownPath(
    _In_reads_(USB4HR_PROTOCOL_PATH_HOPS) const Hop* Path)
{
    USB4HR_STATUS Usb4Status;
    UCHAR Index;

    /* Every hop is attempted even when an earlier one fails */
    for (Index = 0; Index < USB4HR_PROTOCOL_PATH_HOPS; Index++)
    {
        Usb4Status = TeardownHop(&Path[Index]);
        if (Usb4Status != USB4HR_STATUS_SUCCESS)
            DPRINT1("Tearing down HopID %u failed, USB4 status %lu\n", Path[Index].IngressHopId, Usb4Status);
    }
}

USB4HR_STATUS
Usb4HrTunnelManager::SetPathEnable(
    _In_ const Tunnel* Entry,
    _In_ BOOLEAN UpAdapter,
    _In_ BOOLEAN Enable)
{
    const Hop* Protocol = UpAdapter ? &Entry->Inbound[0] : &Entry->Outbound[0];
    ULONG Mask;
    ULONG Value;

    if (Entry->Type == USB4HR_TUNNEL_USB3)
    {
        /* A disabled USB3 adapter keeps its Valid bit */
        Mask = Usb4HrUsb3EnableMask;
        Value = Enable ? Usb4HrUsb3EnableMask : USB4HR_USB3_CS0_VALID;
    }
    else
    {
        Mask = Usb4HrPcieEnableMask;
        Value = Enable ? Usb4HrPcieEnableMask : 0;
    }

    return UpdateAdapterDword(Protocol->Adapter, Protocol->CapabilityOffset, Value, Mask);
}

USB4HR_STATUS
Usb4HrTunnelManager::WriteExtendedEncapsulation(
    _In_ const Tunnel* Entry,
    _In_ BOOLEAN UpAdapter,
    _In_ BOOLEAN Enable)
{
    const Hop* Protocol = UpAdapter ? &Entry->Inbound[0] : &Entry->Outbound[0];

    return UpdateAdapterDword(Protocol->Adapter,
                              Protocol->CapabilityOffset + USB4HR_PCIE_CS_1,
                              Enable ? USB4HR_PCIE_CS1_EXTENDED_ENCAP : 0,
                              USB4HR_PCIE_CS1_EXTENDED_ENCAP);
}

NTSTATUS
Usb4HrTunnelManager::Configure(
    _In_ const Tunnel* Entry,
    _In_ BOOLEAN Rebuild,
    _Out_opt_ PUSB4HR_TUNNEL_BUILD_RESULT Output,
    _Out_ PUSB4HR_STATUS Usb4Status)
{
    USB4HR_STATUS Result;

    Result = ProgramPath(Entry->Outbound);
    if (Result == USB4HR_STATUS_SUCCESS)
        Result = ProgramPath(Entry->Inbound);

    if (Result != USB4HR_STATUS_SUCCESS)
        goto Failed;

    if (Entry->Type == USB4HR_TUNNEL_USB3)
    {
        /* USB3 enables the down adapter first, PCIe the up adapter */
        Result = SetPathEnable(Entry, FALSE, TRUE);
        if (Result == USB4HR_STATUS_SUCCESS)
            Result = SetPathEnable(Entry, TRUE, TRUE);

        if (Result != USB4HR_STATUS_SUCCESS)
            goto Failed;

        *Usb4Status = USB4HR_STATUS_SUCCESS;
        return STATUS_SUCCESS;
    }

    if (Entry->ExtendedEncapsulation)
    {
        Result = WriteExtendedEncapsulation(Entry, TRUE, TRUE);
        if (Result == USB4HR_STATUS_SUCCESS)
            Result = WriteExtendedEncapsulation(Entry, FALSE, TRUE);
    }
    else
    {
        /* Version 2 routers get the bit cleared explicitly */
        Result = USB4HR_STATUS_SUCCESS;
        if (Entry->Inbound[0].UsesV2Fields)
            Result = WriteExtendedEncapsulation(Entry, TRUE, FALSE);
        if (Result == USB4HR_STATUS_SUCCESS && Entry->Outbound[0].UsesV2Fields)
            Result = WriteExtendedEncapsulation(Entry, FALSE, FALSE);
    }

    if (Result != USB4HR_STATUS_SUCCESS)
        goto Failed;

    if (!Rebuild && Output != NULL)
        Output->ModeEnabled = Entry->ExtendedEncapsulation;

    Result = SetPathEnable(Entry, TRUE, TRUE);
    if (Result == USB4HR_STATUS_SUCCESS)
        Result = SetPathEnable(Entry, FALSE, TRUE);

    if (Result != USB4HR_STATUS_SUCCESS)
        goto Failed;

    *Usb4Status = USB4HR_STATUS_SUCCESS;
    return STATUS_SUCCESS;

Failed:
    *Usb4Status = Result;
    return STATUS_UNSUCCESSFUL;
}

VOID
Usb4HrTunnelManager::Teardown(
    _In_ const Tunnel* Entry,
    _In_ BOOLEAN SkipHardware)
{
    USB4HR_STATUS Usb4Status;

    if (SkipHardware)
    {
        DPRINT("Tunnel %p was powered down, nothing to tear down\n", Entry);
        return;
    }

    /* Failures are logged and the teardown goes on, as in Windows */
    if (SetPathEnable(Entry, FALSE, FALSE) != USB4HR_STATUS_SUCCESS)
        DPRINT1("Clearing path enable of the down adapter failed\n");
    if (SetPathEnable(Entry, TRUE, FALSE) != USB4HR_STATUS_SUCCESS)
        DPRINT1("Clearing path enable of the up adapter failed\n");

    if (Entry->Type == USB4HR_TUNNEL_PCIE && Entry->ExtendedEncapsulation)
    {
        WriteExtendedEncapsulation(Entry, TRUE, FALSE);
        WriteExtendedEncapsulation(Entry, FALSE, FALSE);
    }

    TeardownPath(Entry->Outbound);
    TeardownPath(Entry->Inbound);

    /* The last inbound hop is the host router's lane adapter feeding its protocol adapter */
    if (m_HostRouter->Hardware()->HasShimFlag(USB4HR_SHIM_CLEAR_PATH_CREDITS) &&
        Entry->Outbound[0].Route.Depth == 0)
    {
        Usb4Status = ReleaseHopCredits(&Entry->Inbound[USB4HR_PROTOCOL_PATH_HOPS - 1]);
        if (Usb4Status != USB4HR_STATUS_SUCCESS)
            DPRINT1("Clearing path credits failed, USB4 status %lu\n", Usb4Status);
    }
}

/**
 * Only a tunnel whose down adapter is in the host router programs bandwidth.
 * The demand is 1.5 times 90 % of the USB3 rate, capped at the link, and two
 * thirds of it are allocated in each direction.
 */
VOID
Usb4HrTunnelManager::PlanUsb3Bandwidth(
    _Inout_ Tunnel* Entry,
    _In_ ULONG MaxLinkRate)
{
    USHORT RateGbps;
    USHORT Demand;
    USHORT Capacity;

    if (Entry->Outbound[0].Route.Depth != 0)
    {
        Entry->OwnsUsb3Bandwidth = FALSE;
        return;
    }

    RateGbps = (MaxLinkRate == USB4HR_USB3_RATE_20G) ? Usb4HrUsb3Gen2Gbps : Usb4HrUsb3Gen1Gbps;
    if (RateGbps > Usb4HrUsb3Gen1Gbps && m_HostRouter->Hardware()->HasShimFlag(USB4HR_SHIM_LIMIT_USB3_20G))
    {
        DPRINT("Limiting the USB3 tunnel to %u Gbps\n", Usb4HrUsb3LimitedGbps);
        RateGbps = Usb4HrUsb3LimitedGbps;
    }

    Demand = RateGbps * Usb4HrUnitsPerGbps;
    Demand += Demand / 2;

    Capacity = QueryLinkCapacity(Entry->Inbound[USB4HR_PROTOCOL_PATH_HOPS - 1].Adapter);
    if (Capacity != 0 && Capacity < Demand)
        Demand = Capacity;

    Entry->OwnsUsb3Bandwidth = TRUE;
    Entry->Usb3Scale = (RateGbps > Usb4HrUsb3Gen1Gbps) ? 1 : 0;
    Entry->Usb3Allocated = (USHORT)((2 * Demand) / 3);

    DPRINT("USB3 tunnel at %u Gbps gets %u x 100 Mbps each way\n", RateGbps, Entry->Usb3Allocated);
}

/** Link bandwidth in 100 Mbps units from the lane adapter capability; 0 when unknown. */
USHORT
Usb4HrTunnelManager::QueryLinkCapacity(
    _In_ USB4HR_HANDLE LaneAdapter)
{
    ULONG Dword;
    ULONG Next;
    ULONG Walked;
    ULONG Speed;
    ULONG Width;
    USHORT LaneGbps;

    if (ReadSpace(LaneAdapter, USB4HR_SPACE_ADAPTER, USB4HR_ADAPTER_CS_1, 1, &Dword) != USB4HR_STATUS_SUCCESS)
        return 0;

    Next = Dword & Usb4HrCapNextMask;
    for (Walked = 0; Next != 0 && Walked < Usb4HrCapWalkLimit; Walked++)
    {
        if (ReadSpace(LaneAdapter, USB4HR_SPACE_ADAPTER, Next, 1, &Dword) != USB4HR_STATUS_SUCCESS)
            return 0;

        if (((Dword & Usb4HrCapIdMask) >> Usb4HrCapIdShift) == Usb4HrCapIdLane)
            break;

        Next = Dword & Usb4HrCapNextMask;
    }

    if (Next == 0 || Walked == Usb4HrCapWalkLimit)
    {
        DPRINT1("Lane adapter capability not found\n");
        return 0;
    }

    if (ReadSpace(LaneAdapter, USB4HR_SPACE_ADAPTER, Next + USB4HR_LANE_CS_1, 1, &Dword) != USB4HR_STATUS_SUCCESS)
        return 0;

    Speed = (Dword & USB4HR_LANE_CS1_SPEED_MASK) >> USB4HR_LANE_CS1_SPEED_SHIFT;
    Width = (Dword & USB4HR_LANE_CS1_WIDTH_MASK) >> USB4HR_LANE_CS1_WIDTH_SHIFT;

    if (Speed == Usb4HrLaneSpeedGen3)
        LaneGbps = 20;
    else if (Speed == Usb4HrLaneSpeedGen2)
        LaneGbps = 10;
    else
        return 0;

    if (Width == Usb4HrLaneWidthDual)
        LaneGbps *= 2;

    return LaneGbps * Usb4HrUnitsPerGbps;
}

/**
 * USB3 bandwidth handshake on the host router's USB3 down adapter: scale,
 * then CMR set, wait for HCA, then the new allocation with CMR cleared.
 */
USB4HR_STATUS
Usb4HrTunnelManager::ProgramUsb3Bandwidth(
    _In_ const Tunnel* Entry)
{
    USB4HR_HANDLE Adapter = Entry->Outbound[0].Adapter;
    ULONG Capability = Entry->Outbound[0].CapabilityOffset;
    ULONG Registers[2];
    ULONG Scale;
    ULONG Allocated;
    ULONG Attempt;
    BOOLEAN RequestSet = FALSE;
    USB4HR_STATUS Usb4Status;

    Usb4Status = ReadSpace(Adapter, USB4HR_SPACE_ADAPTER, Capability + USB4HR_USB3_CS_3, 1, &Scale);
    if (Usb4Status != USB4HR_STATUS_SUCCESS)
        return Usb4Status;

    if ((Scale & USB4HR_USB3_CS3_SCALE_MASK) != Entry->Usb3Scale)
    {
        Scale = (Scale & ~USB4HR_USB3_CS3_SCALE_MASK) | Entry->Usb3Scale;
        Usb4Status = WriteSpace(Adapter, USB4HR_SPACE_ADAPTER, Capability + USB4HR_USB3_CS_3, 1, &Scale);
        if (Usb4Status != USB4HR_STATUS_SUCCESS)
            return Usb4Status;
    }

    Usb4Status = ReadSpace(Adapter, USB4HR_SPACE_ADAPTER, Capability + USB4HR_USB3_CS_2, 1, &Allocated);
    if (Usb4Status != USB4HR_STATUS_SUCCESS)
        return Usb4Status;

    Allocated |= USB4HR_USB3_CS2_CMR;
    Usb4Status = WriteSpace(Adapter, USB4HR_SPACE_ADAPTER, Capability + USB4HR_USB3_CS_2, 1, &Allocated);
    if (Usb4Status != USB4HR_STATUS_SUCCESS)
        goto Failed;

    RequestSet = TRUE;

    for (Attempt = 1; ; Attempt++)
    {
        Usb4Status = ReadSpace(Adapter, USB4HR_SPACE_ADAPTER, Capability + USB4HR_USB3_CS_1, 2, Registers);
        if (Usb4Status != USB4HR_STATUS_SUCCESS)
            goto Failed;

        Allocated = Registers[1];
        if (Registers[0] & USB4HR_USB3_CS1_HCA)
            break;

        if (Attempt == Usb4HrUsb3AckAttempts)
        {
            DPRINT1("USB3 adapter never acknowledged the bandwidth request\n");
            Usb4Status = USB4HR_STATUS_POLLING_TIMEOUT;
            goto Failed;
        }

        Usb4HrSleepMs(Usb4HrUsb3AckDelayMs);
    }

    Allocated &= ~(USB4HR_USB3_CS2_CMR | USB4HR_USB3_CS2_UP_MASK | USB4HR_USB3_CS2_DOWN_MASK);
    Allocated |= Usb4HrUsb3RegisterBandwidth(Entry->Usb3Allocated, Entry->Usb3Scale);
    Allocated |= (Usb4HrUsb3RegisterBandwidth(Entry->Usb3Allocated, Entry->Usb3Scale) <<
                  USB4HR_USB3_CS2_DOWN_SHIFT) & USB4HR_USB3_CS2_DOWN_MASK;

    return WriteSpace(Adapter, USB4HR_SPACE_ADAPTER, Capability + USB4HR_USB3_CS_2, 1, &Allocated);

Failed:
    if (RequestSet)
    {
        Allocated &= ~USB4HR_USB3_CS2_CMR;
        WriteSpace(Adapter, USB4HR_SPACE_ADAPTER, Capability + USB4HR_USB3_CS_2, 1, &Allocated);
    }

    return Usb4Status;
}

Usb4HrTunnelManager::Tunnel*
Usb4HrTunnelManager::FindTunnelLocked(
    _In_ USB4HR_HANDLE Handle)
{
    PLIST_ENTRY Link;

    for (Link = m_Tunnels.Flink; Link != &m_Tunnels; Link = Link->Flink)
    {
        if (CONTAINING_RECORD(Link, Tunnel, Link) == Handle)
            return CONTAINING_RECORD(Link, Tunnel, Link);
    }

    return NULL;
}

/** Takes the tunnel off the powered down list; TRUE when it was on it. */
BOOLEAN
Usb4HrTunnelManager::MarkPoweredUp(
    _Inout_ Tunnel* Entry)
{
    BOOLEAN WasDown;
    KIRQL OldIrql;

    KeAcquireSpinLock(&m_ListLock, &OldIrql);

    WasDown = Entry->PoweredDown;
    if (WasDown)
    {
        Entry->PoweredDown = FALSE;
        m_PoweredDownCount -= 1;
    }

    if (m_PoweredDownCount == 0)
        KeSetEvent(&m_PowerUpEvent, IO_NO_INCREMENT, FALSE);

    KeReleaseSpinLock(&m_ListLock, OldIrql);

    return WasDown;
}

VOID Usb4HrTunnelManager::AcquireOperationLock()
{
    KeWaitForSingleObject(&m_OperationLock, Executive, KernelMode, FALSE, NULL);
}

VOID Usb4HrTunnelManager::ReleaseOperationLock()
{
    KeSetEvent(&m_OperationLock, IO_NO_INCREMENT, FALSE);
}
