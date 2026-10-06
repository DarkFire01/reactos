/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tunnel segments: HopID allocation, path credits, both halves of a tunnel request
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4dr.h"

#define NDEBUG
#include <debug.h>

/* Path entry 0 of each lane adapter carries control packets; protocol paths start above 7 */
#define HOPID_RESERVED_COUNT        USB4DR_FIRST_PATH_HOPID

/* Smallest bitmap Windows builds, enough for the reserved HopIDs */
#define HOPID_MIN_BITMAP_BITS       8

/* Bonded lanes report a negotiated width of x2 */
#define LANE_WIDTH_DUAL             2

/** What a lane adapter's buffers have to be shared by; counts are in lane buffers. */
struct Usb4DrBufferBudget
{
    USHORT Available;       /**< total buffers less the control path */
    UCHAR MaxUsb3;
    UCHAR MaxPcie;
    UCHAR MaxHostInterface;
    UCHAR DpTunnels;        /**< DP tunnels the policy wants room for */
    USHORT DpPerTunnel;     /**< baMinDPaux + baMinDPmain */
};

/** Every share is kept in a byte; Windows caps it at 255. */
static
UCHAR
NTAPI
Usb4DrCreditByte(
    _In_ ULONG Buffers)
{
    return (Buffers > MAXUCHAR) ? MAXUCHAR : (UCHAR)Buffers;
}

/**
 * USB3 takes its full baMaxUSB3 or nothing. What is left goes to DP tunnels while
 * PCIe and the host interface keep their full share, then to PCIe.
 */
static
VOID
NTAPI
Usb4DrSplitBuffers(
    _In_ const Usb4DrBufferBudget* Budget,
    _Out_ PUCHAR Usb3Credits,
    _Out_ PUCHAR PcieCredits)
{
    LONG HostFloor = (Budget->MaxHostInterface != 0) ? 1 : 0;
    LONG PcieFloor = (Budget->MaxPcie != 0) ? USB4DR_PCIE_MIN_CREDITS : 0;
    LONG Spare;
    ULONG Left;
    ULONG Fit;
    ULONG DpTunnels = 0;
    ULONG Count;

    if (Budget->Available < Budget->MaxUsb3)
    {
        /* Not even USB3 fits: DP gets what the policy asks for, PCIe the rest */
        *Usb3Credits = 0;
        Left = Budget->Available;

        if (Budget->DpPerTunnel != 0 && Left >= Budget->DpPerTunnel)
        {
            /* QUIRK: Windows truncates the number of DP tunnels that fit to a byte */
            Fit = (UCHAR)(Left / Budget->DpPerTunnel);
            DpTunnels = min((ULONG)Budget->DpTunnels, Fit);
            Left -= DpTunnels * Budget->DpPerTunnel;
        }

        if ((LONG)Left < PcieFloor)
            *PcieCredits = 0;
        else
            *PcieCredits = Usb4DrCreditByte(min(Left, (ULONG)Budget->MaxPcie));

        return;
    }

    *Usb3Credits = Budget->MaxUsb3;
    Left = Budget->Available - Budget->MaxUsb3;
    if (Left == 0)
    {
        *PcieCredits = 0;
        return;
    }

    if (Budget->DpPerTunnel != 0)
    {
        for (Count = 1; Count <= Budget->DpTunnels; Count++)
        {
            Spare = (LONG)Left - (LONG)(Budget->DpPerTunnel * Count);
            if (Spare < HostFloor + Budget->MaxPcie)
            {
                /* This DP tunnel stays only if PCIe and the host interface keep their minimum */
                if (Spare >= HostFloor + PcieFloor)
                    DpTunnels = Count;
                break;
            }

            DpTunnels = Count;
        }
    }

    Left -= DpTunnels * Budget->DpPerTunnel;

    if ((LONG)Left == HostFloor + PcieFloor)
        *PcieCredits = (UCHAR)PcieFloor;
    else if ((LONG)Left > HostFloor + Budget->MaxPcie)
        *PcieCredits = Budget->MaxPcie;
    else
        *PcieCredits = Usb4DrCreditByte((USHORT)(Left - HostFloor));
}

/** What one parent half took: the DFP lane 0 HopID, keyed by the tunnel handle once committed. */
struct Usb4DrSegments::Reservation
{
    LIST_ENTRY Link;
    USB4HR_HANDLE TunnelHandle;     /**< NULL until CREATE_TUNNEL succeeded */
    UCHAR LaneAdapter;
    UCHAR DownAdapter;
    USHORT HopId;
};

/* Usb4DrHopIdAllocator *******************************************************/

NTSTATUS
Usb4DrHopIdAllocator::Initialize(
    _In_ USHORT MaxHopId)
{
    ULONG BitCount;

    if (MaxHopId > USB4DR_MAX_HOPID)
    {
        DPRINT1("Max HopID %u is out of range, using %u\n", MaxHopId, USB4DR_MAX_HOPID);
        MaxHopId = USB4DR_MAX_HOPID;
    }

    KeInitializeSpinLock(&m_Lock);
    RtlZeroMemory(m_Bits, sizeof(m_Bits));

    BitCount = max((ULONG)MaxHopId + 1, (ULONG)HOPID_MIN_BITMAP_BITS);
    RtlInitializeBitMap(&m_Bitmap, m_Bits, BitCount);
    RtlSetBits(&m_Bitmap, 0, HOPID_RESERVED_COUNT);

    m_MaxHopId = MaxHopId;
    return STATUS_SUCCESS;
}

VOID
Usb4DrHopIdAllocator::Destroy()
{
    m_MaxHopId = 0;
}

NTSTATUS
Usb4DrHopIdAllocator::Reserve(
    _Out_ PUSHORT HopId)
{
    ULONG Index;

    *HopId = 0;

    if (m_MaxHopId == 0)
        return STATUS_INSUFFICIENT_RESOURCES;

    {
        Usb4DrSpinLockGuard Guard(&m_Lock);
        Index = RtlFindClearBitsAndSet(&m_Bitmap, 1, HOPID_RESERVED_COUNT);
    }

    if (Index == MAXULONG)
        return STATUS_INSUFFICIENT_RESOURCES;

    *HopId = (USHORT)Index;
    return STATUS_SUCCESS;
}

VOID
Usb4DrHopIdAllocator::Release(
    _In_ USHORT HopId)
{
    if (m_MaxHopId == 0 || HopId < HOPID_RESERVED_COUNT || HopId > m_MaxHopId)
    {
        DPRINT1("Releasing HopID %u outside 8..%u\n", HopId, m_MaxHopId);
        return;
    }

    Usb4DrSpinLockGuard Guard(&m_Lock);

    if (!RtlTestBit(&m_Bitmap, HopId))
    {
        DPRINT1("Releasing HopID %u that is not reserved\n", HopId);
        return;
    }

    RtlClearBit(&m_Bitmap, HopId);
}

/* Usb4DrSegments *************************************************************/

NTSTATUS
Usb4DrSegments::Create(
    _In_ Usb4DrFdo* Fdo)
{
    m_Fdo = Fdo;
    KeInitializeSpinLock(&m_Lock);
    InitializeListHead(&m_Reservations);
    return STATUS_SUCCESS;
}

VOID
Usb4DrSegments::Destroy()
{
    PLIST_ENTRY Entry;
    ULONG Index;

    /* Cleanup may run without a successful Create when AddDevice failed early */
    if (m_Reservations.Flink == NULL)
        return;

    while (!IsListEmpty(&m_Reservations))
    {
        Entry = RemoveHeadList(&m_Reservations);
        ExFreePoolWithTag(CONTAINING_RECORD(Entry, Reservation, Link), USB4DR_TAG_TUNNEL);
    }

    for (Index = 0; Index < RTL_NUMBER_OF(m_HopIds); Index++)
        m_HopIds[Index].Destroy();
}

VOID
Usb4DrSegments::BuildAllocators()
{
    Usb4DrRouter* Router = m_Fdo->Router();
    Usb4DrAdapter* Adapter;
    USHORT MaxHopId;
    ULONG Number;

    for (Number = 1; Number <= Router->MaxAdapter() && Number < USB4DR_MAX_ADAPTERS; Number++)
    {
        Adapter = Router->Adapter((UCHAR)Number);
        if (Adapter == NULL || !Adapter->IsPresent() || !Adapter->IsLane())
            continue;

        MaxHopId = min(Adapter->MaxInputHopId(), Adapter->MaxOutputHopId());
        m_HopIds[Number].Initialize(MaxHopId);
    }
}

Usb4DrHopIdAllocator*
Usb4DrSegments::Allocator(
    _In_ Usb4DrAdapter* Adapter)
{
    NT_ASSERT(Adapter->Number() < USB4DR_MAX_ADAPTERS);
    return &m_HopIds[Adapter->Number()];
}

BOOLEAN
Usb4DrSegments::HasUsb3Adapters()
{
    Usb4DrRouter* Router = m_Fdo->Router();
    Usb4DrAdapter* Adapter;
    ULONG Number;

    for (Number = 1; Number <= Router->MaxAdapter(); Number++)
    {
        Adapter = Router->Adapter((UCHAR)Number);
        if (Adapter != NULL && Adapter->IsPresent() && Adapter->IsUsb3())
            return TRUE;
    }

    return FALSE;
}

BOOLEAN
Usb4DrSegments::HasPcieAdapters()
{
    Usb4DrRouter* Router = m_Fdo->Router();
    Usb4DrAdapter* Adapter;
    ULONG Number;

    /* Windows never registers PCIe adapters the policy disabled, so they get no share */
    if (Router->IsPcieTunnelingDisabled())
        return FALSE;

    for (Number = 1; Number <= Router->MaxAdapter(); Number++)
    {
        Adapter = Router->Adapter((UCHAR)Number);
        if (Adapter != NULL && Adapter->IsPresent() && Adapter->IsPcie())
            return TRUE;
    }

    return FALSE;
}

BOOLEAN
Usb4DrSegments::HasTunnelThrough(
    _In_ UCHAR DownAdapter)
{
    PLIST_ENTRY Entry;
    Reservation* Item;

    Usb4DrSpinLockGuard Guard(&m_Lock);

    for (Entry = m_Reservations.Flink; Entry != &m_Reservations; Entry = Entry->Flink)
    {
        Item = CONTAINING_RECORD(Entry, Reservation, Link);
        if (Item->DownAdapter == DownAdapter)
            return TRUE;
    }

    return FALSE;
}

NTSTATUS
Usb4DrSegments::FillChildHalf(
    _In_ Usb4DrProtocolAdapter* UpAdapter,
    _Out_ PUSB4HR_CREATE_TUNNEL_INPUT Input)
{
    PUSB4HR_TUNNEL_SEGMENT Protocol;
    PUSB4HR_TUNNEL_SEGMENT Lane;
    Usb4DrAdapter* UfpLane0;
    Usb4DrPort* Ufp;
    USHORT HopId;
    NTSTATUS Status;

    RtlZeroMemory(Input, sizeof(*Input));
    Input->TunnelType = UpAdapter->TunnelType();

    Ufp = m_Fdo->Ports()->Ufp();
    if (Ufp == NULL || !UpAdapter->IsUp())
    {
        DPRINT1("Adapter %u cannot start a tunnel from depth %lu\n",
                UpAdapter->Adapter()->Number(), m_Fdo->Depth());
        return STATUS_INVALID_DEVICE_STATE;
    }

    UfpLane0 = Ufp->Lane0();

    /* The up adapter hop: the parent fills in the HopID it reserves on its DFP */
    Protocol = &Input->Protocol.Inbound[0];
    Protocol->AdapterHandle = UpAdapter->Adapter()->Handle();
    Protocol->CapabilityOffset = UpAdapter->CapabilityOffset();
    Protocol->IngressHopId = UpAdapter->InputHopId();
    Protocol->OutputAdapter = UfpLane0->Number();
    Protocol->OutputHopId = 0;
    Protocol->EgressMaxOutputHopId = UfpLane0->MaxOutputHopId();
    Protocol->PathCredits = m_Fdo->Router()->IsTbt3() ? USB4DR_TBT3_PROTOCOL_CREDITS : 0;
    Protocol->IngressFlowControl = TRUE;
    Protocol->EgressFlowControl = TRUE;

    Status = Allocator(UfpLane0)->Reserve(&HopId);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("No free HopID on UFP lane adapter %u\n", UfpLane0->Number());
        return Status;
    }

    /* The UFP lane hop towards the up adapter */
    Lane = &Input->Protocol.Outbound[1];
    Lane->AdapterHandle = UfpLane0->Handle();
    Lane->CapabilityOffset = 0;
    Lane->IngressHopId = HopId;
    Lane->OutputAdapter = UpAdapter->Adapter()->Number();
    Lane->OutputHopId = UpAdapter->OutputHopId();
    Lane->IngressFlowControl = TRUE;

    Status = LaneCredits(Ufp, Input->TunnelType, &Lane->PathCredits);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("No path credits on UFP lane adapter %u: 0x%08lx\n", UfpLane0->Number(), Status);
        Allocator(UfpLane0)->Release(HopId);
        return Status;
    }

    if (Input->TunnelType == USB4HR_TUNNEL_USB3)
    {
        if (UpAdapter->MaxLinkRateField() == USB4DR_USB3_MAX_RATE_GEN2X2 && Ufp->LinkGbps() >= 20)
            Input->Protocol.Usb3MaxLinkRate = USB4HR_USB3_RATE_20G;
        else
            Input->Protocol.Usb3MaxLinkRate = USB4HR_USB3_RATE_10G;
    }
    else
    {
        Input->Protocol.PcieExtendedEncapsulation = m_Fdo->Router()->IsUsb4V2();
    }

    DPRINT("Child half: up adapter %u, UFP lane %u HopID %u, credits %u\n",
           UpAdapter->Adapter()->Number(), UfpLane0->Number(), HopId, Lane->PathCredits);
    return STATUS_SUCCESS;
}

VOID
Usb4DrSegments::ReleaseChildHalf(
    _In_ const USB4HR_CREATE_TUNNEL_INPUT* Input)
{
    Usb4DrPort* Ufp = m_Fdo->Ports()->Ufp();

    if (Ufp == NULL || Input->Protocol.Outbound[1].IngressHopId == 0)
        return;

    Allocator(Ufp->Lane0())->Release(Input->Protocol.Outbound[1].IngressHopId);
}

NTSTATUS
Usb4DrSegments::FillParentHalf(
    _In_ Usb4DrPort* Dfp,
    _Inout_ PUSB4HR_CREATE_TUNNEL_INPUT Input,
    _Out_ PVOID* Reservation)
{
    PUSB4HR_PROTOCOL_TUNNEL Tunnel = &Input->Protocol;
    Usb4DrProtocolAdapter* Down;
    Usb4DrAdapter* DfpLane0;
    Usb4DrAdapter* DownAdapter;
    Usb4DrSegments::Reservation* Item;
    USHORT HopId;
    NTSTATUS Status;

    *Reservation = NULL;

    if (Input->TunnelType != USB4HR_TUNNEL_USB3 && Input->TunnelType != USB4HR_TUNNEL_PCIE)
    {
        DPRINT1("Tunnel type %lu has no parent half here\n", Input->TunnelType);
        return STATUS_INVALID_PARAMETER;
    }

    Down = m_Fdo->Tunnels()->DownAdapterForDfp(Input->TunnelType, Dfp->DfpIndex());
    if (Down == NULL)
    {
        DPRINT1("No type %lu down adapter for DFP %lu\n", Input->TunnelType, Dfp->DfpIndex());
        return STATUS_NOT_SUPPORTED;
    }

    Status = Down->WaitUntilReady();
    if (!NT_SUCCESS(Status))
        return Status;

    DfpLane0 = Dfp->Lane0();
    DownAdapter = Down->Adapter();

    Item = (Usb4DrSegments::Reservation*)ExAllocatePoolWithTag(NonPagedPool,
                                                                sizeof(*Item),
                                                                USB4DR_TAG_TUNNEL);
    if (Item == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = Allocator(DfpLane0)->Reserve(&HopId);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("No free HopID on DFP lane adapter %u\n", DfpLane0->Number());
        ExFreePoolWithTag(Item, USB4DR_TAG_TUNNEL);
        return Status;
    }

    /* The child's UFP must be able to send to the HopID we picked */
    if (Tunnel->Inbound[0].EgressMaxOutputHopId == 0)
        DPRINT1("Child did not give its UFP max output HopID\n");

    if (HopId > Tunnel->Inbound[0].EgressMaxOutputHopId)
    {
        DPRINT1("DFP HopID %u is above the child UFP limit %u\n",
                HopId, Tunnel->Inbound[0].EgressMaxOutputHopId);
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Fail;
    }

    /* The DFP lane hop towards our down adapter */
    Tunnel->Inbound[1].AdapterHandle = DfpLane0->Handle();
    Tunnel->Inbound[1].CapabilityOffset = 0;
    Tunnel->Inbound[1].IngressHopId = HopId;
    Tunnel->Inbound[1].OutputAdapter = DownAdapter->Number();
    Tunnel->Inbound[1].OutputHopId = Down->OutputHopId();
    Tunnel->Inbound[1].IngressFlowControl = TRUE;
    Tunnel->Inbound[1].EgressFlowControl = FALSE;

    if (Tunnel->Inbound[0].OutputHopId != 0)
        DPRINT1("Child already filled its output HopID %u\n", Tunnel->Inbound[0].OutputHopId);

    Tunnel->Inbound[0].OutputHopId = HopId;

    Status = LaneCredits(Dfp, Input->TunnelType, &Tunnel->Inbound[1].PathCredits);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("No path credits on DFP lane adapter %u: 0x%08lx\n", DfpLane0->Number(), Status);
        goto Fail;
    }

    if (Tunnel->Outbound[1].IngressHopId > DfpLane0->MaxOutputHopId())
    {
        DPRINT1("Child HopID %u is above the DFP limit %u\n",
                Tunnel->Outbound[1].IngressHopId, DfpLane0->MaxOutputHopId());
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Fail;
    }

    /* Our down adapter hop towards the DFP lane */
    Tunnel->Outbound[0].AdapterHandle = DownAdapter->Handle();
    Tunnel->Outbound[0].CapabilityOffset = Down->CapabilityOffset();
    Tunnel->Outbound[0].IngressHopId = Down->InputHopId();
    Tunnel->Outbound[0].OutputAdapter = DfpLane0->Number();
    Tunnel->Outbound[0].OutputHopId = Tunnel->Outbound[1].IngressHopId;
    Tunnel->Outbound[0].PathCredits = m_Fdo->Router()->IsTbt3() ? USB4DR_TBT3_PROTOCOL_CREDITS : 0;
    Tunnel->Outbound[0].IngressFlowControl = TRUE;
    Tunnel->Outbound[0].EgressFlowControl = TRUE;

    /* Both ends of the link have to support the faster rate or extended encapsulation */
    if (Input->TunnelType == USB4HR_TUNNEL_USB3)
    {
        if (Down->MaxLinkRateField() != USB4DR_USB3_MAX_RATE_GEN2X2)
            Tunnel->Usb3MaxLinkRate = USB4HR_USB3_RATE_10G;
    }
    else if (Tunnel->PcieExtendedEncapsulation)
    {
        Tunnel->PcieExtendedEncapsulation = m_Fdo->Router()->IsUsb4V2();
    }

    Item->TunnelHandle = NULL;
    Item->LaneAdapter = DfpLane0->Number();
    Item->DownAdapter = DownAdapter->Number();
    Item->HopId = HopId;

    {
        Usb4DrSpinLockGuard Guard(&m_Lock);
        InsertTailList(&m_Reservations, &Item->Link);
    }

    DPRINT("Parent half: DFP lane %u HopID %u, down adapter %u, credits %u\n",
           DfpLane0->Number(), HopId, DownAdapter->Number(), Tunnel->Inbound[1].PathCredits);

    *Reservation = Item;
    return STATUS_SUCCESS;

Fail:
    Allocator(DfpLane0)->Release(HopId);
    ExFreePoolWithTag(Item, USB4DR_TAG_TUNNEL);
    return Status;
}

VOID
Usb4DrSegments::CommitParentHalf(
    _In_ PVOID Reservation,
    _In_ USB4HR_HANDLE TunnelHandle)
{
    Usb4DrSegments::Reservation* Item = (Usb4DrSegments::Reservation*)Reservation;

    Usb4DrSpinLockGuard Guard(&m_Lock);
    Item->TunnelHandle = TunnelHandle;
}

VOID
Usb4DrSegments::ReleaseParentHalf(
    _In_ PVOID Reservation)
{
    Usb4DrSegments::Reservation* Item = (Usb4DrSegments::Reservation*)Reservation;
    Usb4DrAdapter* Lane;

    {
        Usb4DrSpinLockGuard Guard(&m_Lock);
        RemoveEntryList(&Item->Link);
    }

    Lane = m_Fdo->Router()->Adapter(Item->LaneAdapter);
    if (Lane != NULL)
        Allocator(Lane)->Release(Item->HopId);

    ExFreePoolWithTag(Item, USB4DR_TAG_TUNNEL);
}

PVOID
Usb4DrSegments::FindByTunnel(
    _In_ USB4HR_HANDLE TunnelHandle)
{
    PLIST_ENTRY Entry;
    Reservation* Item;

    if (TunnelHandle == NULL)
        return NULL;

    Usb4DrSpinLockGuard Guard(&m_Lock);

    for (Entry = m_Reservations.Flink; Entry != &m_Reservations; Entry = Entry->Flink)
    {
        Item = CONTAINING_RECORD(Entry, Reservation, Link);
        if (Item->TunnelHandle == TunnelHandle)
            return Item;
    }

    return NULL;
}

NTSTATUS
Usb4DrSegments::LaneCredits(
    _In_ Usb4DrPort* Port,
    _In_ ULONG TunnelType,
    _Out_ PUCHAR Credits)
{
    Usb4DrRouter* Router = m_Fdo->Router();
    Usb4DrPort* Ufp;
    ULONG LaneStatus;
    UCHAR Usb3Credits;
    UCHAR PcieCredits;
    NTSTATUS Status;

    *Credits = 0;

    if (Router->IsTbt3())
    {
        /* Thunderbolt 3 uses fixed credits that follow the bonding of the upstream link */
        Ufp = m_Fdo->Ports()->Ufp();
        if (Ufp == NULL)
            return STATUS_INVALID_DEVICE_STATE;

        Status = Ufp->Lane0()->ReadLaneStatus(&LaneStatus);
        if (!NT_SUCCESS(Status))
            return Status;

        if (Usb4DrField(LaneStatus, USB4DR_LANE_CS1_NEGOTIATED_WIDTH) == LANE_WIDTH_DUAL)
            *Credits = USB4DR_TBT3_LANE_CREDITS_BONDED;
        else
            *Credits = USB4DR_TBT3_LANE_CREDITS_SINGLE;

        return STATUS_SUCCESS;
    }

    Status = PlanLaneBuffers(Port, &Usb3Credits, &PcieCredits);
    if (!NT_SUCCESS(Status))
        return Status;

    if (TunnelType == USB4HR_TUNNEL_PCIE)
        *Credits = PcieCredits;
    else if (TunnelType == USB4HR_TUNNEL_USB3)
        *Credits = Usb3Credits;

    if (*Credits == 0)
    {
        DPRINT1("Lane adapter %u has no credits for a type %lu tunnel\n", Port->Lane0Number(), TunnelType);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Usb4DrSegments::PlanLaneBuffers(
    _In_ Usb4DrPort* Port,
    _Out_ PUCHAR Usb3Credits,
    _Out_ PUCHAR PcieCredits)
{
    const USB4HR_PARENT_INTERFACE* Parent = m_Fdo->HostLink()->Parent();
    Usb4DrRouter* Router = m_Fdo->Router();
    Usb4DrBufferBudget Budget;
    Usb4DrAdapter* ControlLane;
    Usb4DrPort* Ufp;
    ULONG TotalBuffers;
    UCHAR ControlCredits;
    UCHAR DpMain;
    NTSTATUS Status;

    *Usb3Credits = 0;
    *PcieCredits = 0;

    TotalBuffers = Usb4DrField(Port->Lane0()->BasicDword(USB4DR_ADAPTER_CS_4), USB4DR_ADAPTER_CS4_TOTAL_BUFFERS);
    if (TotalBuffers == 0)
    {
        DPRINT1("Lane adapter %u reports no buffers\n", Port->Lane0Number());
        return STATUS_INVALID_PARAMETER;
    }

    /* The control path buffers are the same on every port; Windows reads them from one lane */
    if (m_Fdo->Depth() != 0)
    {
        Ufp = m_Fdo->Ports()->Ufp();
        ControlLane = (Ufp != NULL) ? Ufp->Lane0() : NULL;
    }
    else
    {
        ControlLane = (m_Fdo->Ports()->DfpCount() != 0) ? m_Fdo->Ports()->Dfp(0)->Lane0() : NULL;
    }

    if (ControlLane == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    Status = ControlLane->ReadControlCredits(&ControlCredits);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Reading the control path credits failed 0x%08lx\n", Status);
        return Status;
    }

    if (ControlCredits > TotalBuffers)
    {
        DPRINT1("Control path takes %u of %lu buffers\n", ControlCredits, TotalBuffers);
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&Budget, sizeof(Budget));
    Budget.Available = (USHORT)(TotalBuffers - ControlCredits);

    /* A protocol the router has no adapters for gets no share */
    if (HasUsb3Adapters())
        Budget.MaxUsb3 = Router->BufferAllocation(USB4DR_BUFFER_MAX_USB3);

    if (HasPcieAdapters())
        Budget.MaxPcie = Router->BufferAllocation(USB4DR_BUFFER_MAX_PCIE);

    if (Budget.MaxPcie != 0 && Budget.MaxPcie < USB4DR_PCIE_MIN_CREDITS)
    {
        /* QUIRK: Windows plans with 6 buffers when the router asks for fewer */
        DPRINT1("baMaxPCIe %u is below %u, using %u\n",
                Budget.MaxPcie, USB4DR_PCIE_MIN_CREDITS, USB4DR_PCIE_MIN_CREDITS);
        Budget.MaxPcie = USB4DR_PCIE_MIN_CREDITS;
    }

    /* The host interface share comes from the root router, handed down in the tunnel policy */
    if (m_Fdo->Depth() == 0)
        Budget.MaxHostInterface = Router->BufferAllocation(USB4DR_BUFFER_MAX_HI);
    else
        Budget.MaxHostInterface = Parent->TunnelPolicy.HostInterfaceBufferLimit;

    if (Budget.MaxHostInterface == 0)
        DPRINT1("No host interface buffers from the host router\n");

    if (m_Fdo->DeviceFlags() & USB4DR_FLAG_BUFFER_CALCULATION)
        DpMain = USB4DR_DP_MAIN_CREDITS_ADJUSTED;
    else
        DpMain = Router->BufferAllocation(USB4DR_BUFFER_MIN_DP_MAIN);

    Budget.DpPerTunnel = (USHORT)(Router->BufferAllocation(USB4DR_BUFFER_MIN_DP_AUX) + DpMain);

    /* Room is kept for one DP tunnel per DP IN adapter of the host */
    if (m_Fdo->Depth() == 0)
        Budget.DpTunnels = Router->DpInAdapterCount();
    else
        Budget.DpTunnels = Parent->TunnelPolicy.HostDpAdapters;

    Usb4DrSplitBuffers(&Budget, Usb3Credits, PcieCredits);

    DPRINT("Lane adapter %u: %u buffers, %u control, USB3 %u, PCIe %u\n",
           Port->Lane0Number(), TotalBuffers, ControlCredits, *Usb3Credits, *PcieCredits);
    return STATUS_SUCCESS;
}
