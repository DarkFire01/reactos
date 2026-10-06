/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     USB4 ports: plug handling, child router detection, handles and child lists
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4dr.h"

#define NDEBUG
#include <debug.h>

/* Enumeration time a DFP offers its child before the parent says otherwise */
#define USB4DR_DEFAULT_ENUM_SECONDS     3

/* USB4 version byte of the first USB4 routers; anything below is Thunderbolt 3 */
#define USB4DR_USB4_VERSION_1           0x20

/* ROUTER_CS_7 and ROUTER_CS_8 hold the router UUID */
#define USB4DR_ROUTER_CS_UUID_LOW       7
#define USB4DR_ROUTER_CS_UUID_HIGH      8

/* LANE_ADP_CS_1 current link speed and negotiated width values */
#define USB4DR_LANE_SPEED_GEN3          0x4
#define USB4DR_LANE_SPEED_GEN2          0x8
#define USB4DR_LANE_WIDTH_DUAL          0x2

/* Upstream link bandwidth classes in Gbps */
#define USB4DR_LINK_GBPS_GEN2_LANE      10
#define USB4DR_LINK_GBPS_GEN3_LANE      20
#define USB4DR_LINK_GBPS_GEN4           80

/* Thunderbolt 3 routers this driver lets through, all Intel */
#define USB4DR_INTEL_VENDOR_ID          0x8086

static const USHORT Usb4DrAlpineRidgeIds[] =
{
    0x1575, 0x1576, 0x1577, 0x1578, 0x15BF, 0x15C0, 0x15D2, 0x15D3, 0x15D9, 0x15DA
};

static const USHORT Usb4DrTitanRidgeIds[] =
{
    0x15E7, 0x15E8, 0x15EA, 0x15EB, 0x15EF
};

/** Work item context: the port whose plug state changed. */
struct Usb4DrPortWorkContext
{
    Usb4DrPort* Port;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Usb4DrPortWorkContext, Usb4DrGetPortWorkContext);

static EVT_WDF_CHILD_LIST_CREATE_DEVICE Usb4DrEvtChildCreateDevice;
static EVT_WDF_CHILD_LIST_IDENTIFICATION_DESCRIPTION_COMPARE Usb4DrEvtChildCompare;
static EVT_WDF_CHILD_LIST_IDENTIFICATION_DESCRIPTION_COPY Usb4DrEvtChildCopy;
static EVT_WDF_CHILD_LIST_IDENTIFICATION_DESCRIPTION_DUPLICATE Usb4DrEvtChildDuplicate;

static
BOOLEAN
NTAPI
Usb4DrIsIdInList(
    _In_ USHORT ProductId,
    _In_reads_(Count) const USHORT* List,
    _In_ ULONG Count)
{
    ULONG Index;

    for (Index = 0; Index < Count; Index++)
    {
        if (List[Index] == ProductId)
            return TRUE;
    }

    return FALSE;
}

/** Router type of a Thunderbolt 3 router; unknown unless it is one of the supported Intel parts. */
static
USB4HR_ROUTER_FAMILY
NTAPI
Usb4DrClassifyTbt3Router(
    _In_ USHORT VendorId,
    _In_ USHORT ProductId)
{
    if (VendorId != USB4DR_INTEL_VENDOR_ID)
        return Usb4HrRouterUnknown;

    if (Usb4DrIsIdInList(ProductId, Usb4DrAlpineRidgeIds, RTL_NUMBER_OF(Usb4DrAlpineRidgeIds)))
        return Usb4HrRouterTbt3AlpineRidge;

    if (Usb4DrIsIdInList(ProductId, Usb4DrTitanRidgeIds, RTL_NUMBER_OF(Usb4DrTitanRidgeIds)))
        return Usb4HrRouterTbt3TitanRidge;

    return Usb4HrRouterUnknown;
}

/** Turns off hot plug events on an adapter that must never deliver them. */
static
VOID
NTAPI
Usb4DrSilenceAdapter(
    _In_ Usb4DrFdo* Fdo,
    _In_opt_ Usb4DrAdapter* Adapter)
{
    const USB4HR_HARDWARE_SERVICES* Services = Fdo->HostLink()->Services();

    if (Adapter == NULL || Adapter->Handle() == NULL)
        return;

    Services->PurgeEventQueues(Services->Header.Context, Adapter->Handle(), TRUE, TRUE);
}

/* Child list callbacks *******************************************************/

static
NTSTATUS
NTAPI
Usb4DrEvtChildCreateDevice(
    _In_ WDFCHILDLIST ChildList,
    _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER IdentificationDescription,
    _In_ PWDFDEVICE_INIT ChildInit)
{
    Usb4DrFdo* Fdo = Usb4DrFdo::FromDevice(WdfChildListGetDevice(ChildList));
    const Usb4DrChildDescription* Description;

    Description = CONTAINING_RECORD(IdentificationDescription, Usb4DrChildDescription, Header);

    if (Description->Type != Usb4DrChildType::DeviceRouter)
    {
        DPRINT1("Child of type %lu cannot be created\n", static_cast<ULONG>(Description->Type));
        return STATUS_NOT_SUPPORTED;
    }

    return Usb4DrChildPdo::CreateDevice(Fdo, Description, ChildInit);
}

static
BOOLEAN
NTAPI
Usb4DrEvtChildCompare(
    _In_ WDFCHILDLIST ChildList,
    _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER FirstIdentificationDescription,
    _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER SecondIdentificationDescription)
{
    const Usb4DrChildDescription* First;
    const Usb4DrChildDescription* Second;

    UNREFERENCED_PARAMETER(ChildList);

    First = CONTAINING_RECORD(FirstIdentificationDescription, Usb4DrChildDescription, Header);
    Second = CONTAINING_RECORD(SecondIdentificationDescription, Usb4DrChildDescription, Header);

    if (First->Type != Second->Type || First->Generation != Second->Generation)
        return FALSE;

    return RtlCompareMemory(&First->Identity, &Second->Identity, sizeof(First->Identity)) ==
           sizeof(First->Identity);
}

static
VOID
NTAPI
Usb4DrEvtChildCopy(
    _In_ WDFCHILDLIST ChildList,
    _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER SourceIdentificationDescription,
    _Out_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER DestinationIdentificationDescription)
{
    UNREFERENCED_PARAMETER(ChildList);

    *CONTAINING_RECORD(DestinationIdentificationDescription, Usb4DrChildDescription, Header) =
        *CONTAINING_RECORD(SourceIdentificationDescription, Usb4DrChildDescription, Header);
}

static
NTSTATUS
NTAPI
Usb4DrEvtChildDuplicate(
    _In_ WDFCHILDLIST ChildList,
    _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER SourceIdentificationDescription,
    _Out_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER DestinationIdentificationDescription)
{
    Usb4DrEvtChildCopy(ChildList, SourceIdentificationDescription, DestinationIdentificationDescription);
    return STATUS_SUCCESS;
}

/* Usb4DrPort *****************************************************************/

NTSTATUS
Usb4DrPort::Initialize(
    _In_ Usb4DrFdo* Fdo,
    _In_ Usb4DrAdapter* Lane0,
    _In_opt_ Usb4DrAdapter* Lane1,
    _In_ BOOLEAN IsDfp,
    _In_ ULONG DfpIndex)
{
    const USB4HR_PARENT_INTERFACE* Parent = Fdo->HostLink()->Parent();
    WDF_CHILD_LIST_CONFIG ListConfig;
    WDF_WORKITEM_CONFIG WorkConfig;
    WDF_OBJECT_ATTRIBUTES Attributes;
    ULONG Depth = Fdo->Depth();
    NTSTATUS Status;

    m_Fdo = Fdo;
    m_Lane0 = Lane0;
    m_Lane1 = Lane1;
    m_IsDfp = IsDfp;
    m_DfpIndex = DfpIndex;
    m_ChildList = NULL;
    m_ChildIoQueue = NULL;
    m_PlugWork = NULL;

    KeInitializeSpinLock(&m_Lock);
    KeInitializeEvent(&m_ChildHandleIdle, NotificationEvent, TRUE);
    m_Generation = 0;
    m_ChildHandle = NULL;
    m_ChildHandleRefs = 0;
    m_ChildHandleClosing = FALSE;
    m_Stopping = FALSE;
    m_Plugged = FALSE;
    m_UnplugSeen = FALSE;
    m_PlugWorkPending = FALSE;
    m_PlugWorkBusy = FALSE;
    m_ChildReported = FALSE;
    m_ChildType = Usb4HrRouterUnknown;
    RtlZeroMemory(&m_ChildTopology, sizeof(m_ChildTopology));
    RtlZeroMemory(&m_ChildIdentity, sizeof(m_ChildIdentity));

    if (!IsDfp)
        return STATUS_SUCCESS;

    /* Port[n] is the downstream adapter taken to reach depth n + 1 */
    if (Depth < USB4HR_MAX_DEPTH)
    {
        m_ChildTopology = Parent->TopologyId;
        m_ChildTopology.Port[Depth] = Lane0->Number();
        m_ChildTopology.Depth = Depth + 1;
    }
    else
    {
        DPRINT1("DFP %u at depth %lu: no router can be enumerated behind it\n", Lane0->Number(), Depth);
    }

    m_MaxEnumSeconds = USB4DR_DEFAULT_ENUM_SECONDS;
    if (Depth > 0 && Parent->AgreeEnumerationDeadline != NULL)
        m_MaxEnumSeconds = Parent->AgreeEnumerationDeadline(Parent->Header.Context, m_MaxEnumSeconds);
    m_InitialEnumSeconds = m_MaxEnumSeconds;

    WDF_CHILD_LIST_CONFIG_INIT(&ListConfig, sizeof(Usb4DrChildDescription), Usb4DrEvtChildCreateDevice);
    ListConfig.EvtChildListIdentificationDescriptionCompare = Usb4DrEvtChildCompare;
    ListConfig.EvtChildListIdentificationDescriptionCopy = Usb4DrEvtChildCopy;
    ListConfig.EvtChildListIdentificationDescriptionDuplicate = Usb4DrEvtChildDuplicate;

    Status = WdfChildListCreate(Fdo->Device(), &ListConfig, WDF_NO_OBJECT_ATTRIBUTES, &m_ChildList);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DFP %u: child list create failed: 0x%lx\n", Lane0->Number(), Status);
        m_ChildList = NULL;
        return Status;
    }

    WDF_WORKITEM_CONFIG_INIT(&WorkConfig, EvtPlugWork);
    WorkConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, Usb4DrPortWorkContext);
    Attributes.ParentObject = Fdo->Device();

    Status = WdfWorkItemCreate(&WorkConfig, &Attributes, &m_PlugWork);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DFP %u: plug work item create failed: 0x%lx\n", Lane0->Number(), Status);
        m_PlugWork = NULL;
        return Status;
    }

    Usb4DrGetPortWorkContext(m_PlugWork)->Port = this;

    Status = m_Pump.Create(Fdo, OnEvent, this);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = Fdo->Forwarder()->CreatePortQueue(this, &m_ChildIoQueue);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DFP %u: PDO request queue create failed: 0x%lx\n", Lane0->Number(), Status);
        m_ChildIoQueue = NULL;
        return Status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Usb4DrPort::Start(
    _In_ BOOLEAN FirstStart)
{
    BOOLEAN QueueWork = FALSE;
    BOOLEAN CheckNow = FALSE;
    BOOLEAN Closing;
    BOOLEAN Plugged;
    ULONG Cs4;
    NTSTATUS Status;

    if (!m_IsDfp)
        return STATUS_SUCCESS;

    Status = m_Lane0->ReadDword(USB4DR_ADAPTER_CS_4, &Cs4);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DFP %u: plug state read failed: 0x%lx\n", Lane0Number(), Status);
        return Status;
    }

    Plugged = (Cs4 & USB4DR_ADAPTER_CS4_PLUGGED) != 0;
    DPRINT("DFP %u %s, %s\n", Lane0Number(), Plugged ? "plugged" : "empty", FirstStart ? "first start" : "resume");

    {
        Usb4DrSpinLockGuard Guard(&m_Lock);

        m_Stopping = FALSE;
        Closing = (m_ChildHandle != NULL && m_ChildHandleClosing);
    }

    /* An unplug during D0 exit left the handle behind; it goes now if the child PDO let go of it */
    if (Closing)
        DeleteChildHandle(FALSE);

    {
        Usb4DrSpinLockGuard Guard(&m_Lock);

        m_Plugged = Plugged;
        if (Plugged)
        {
            /* Windows checks the router on resume inside D0 entry, before the child powers up */
            if (!FirstStart && !(m_ChildHandle != NULL && m_ChildHandleClosing))
                CheckNow = TRUE;
            else
                QueueWork = TRUE;
        }
        else
        {
            /* Windows moves to a new generation when a port starts empty */
            m_Generation++;

            /* The router behind this port left while we were powered down */
            if (m_ChildHandle != NULL || m_ChildReported)
            {
                m_UnplugSeen = TRUE;
                QueueWork = TRUE;
            }
        }

        if (QueueWork)
            m_PlugWorkPending = TRUE;
    }

    if (CheckNow)
        HandlePlug(TRUE);

    Status = m_Pump.Start(m_Lane0->Handle(), IOCTL_USB4HR_WAIT_ADAPTER_EVENT);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DFP %u: hot plug events not armed: 0x%lx\n", Lane0Number(), Status);
        return Status;
    }

    if (QueueWork)
        WdfWorkItemEnqueue(m_PlugWork);

    return STATUS_SUCCESS;
}

VOID
Usb4DrPort::Stop(
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    BOOLEAN Reported;

    if (!m_IsDfp)
        return;

    {
        Usb4DrSpinLockGuard Guard(&m_Lock);

        /* Wakes an unplug waiting for the child PDO; it leaves the handle for the next start */
        m_Stopping = TRUE;
        KeSetEvent(&m_ChildHandleIdle, IO_NO_INCREMENT, FALSE);
    }

    m_Pump.Stop();

    if (m_PlugWork != NULL)
        WdfWorkItemFlush(m_PlugWork);

    if (TargetState == WdfPowerDeviceD3Final)
    {
        {
            Usb4DrSpinLockGuard Guard(&m_Lock);
            Reported = m_ChildReported;
        }

        /* The PDO may outlive the restart; the same router then comes back as a new one */
        if (Reported)
            BumpGeneration();

        ReportChildMissing();
    }
}

VOID
Usb4DrPort::Destroy()
{
    const USB4HR_HARDWARE_SERVICES* Services;
    USB4HR_HANDLE Handle;
    LONG References;
    NTSTATUS Status;

    if (!m_IsDfp || m_Fdo == NULL)
        return;

    {
        Usb4DrSpinLockGuard Guard(&m_Lock);

        Handle = m_ChildHandle;
        References = m_ChildHandleRefs;
        m_ChildHandle = NULL;
    }

    if (Handle != NULL && References != 0)
    {
        DPRINT1("DFP %u: child router handle %p still has %ld references, leaking it\n",
                Lane0Number(), Handle, References);
    }
    else if (Handle != NULL)
    {
        Services = m_Fdo->HostLink()->Services();
        Status = Services->DestroyRouterHandle(Services->Header.Context, Handle);
        if (!NT_SUCCESS(Status))
            DPRINT1("DFP %u: router handle %p not destroyed: 0x%lx\n", Lane0Number(), Handle, Status);
    }

    /* The WDF objects go with the device */
    m_ChildList = NULL;
    m_ChildIoQueue = NULL;
    m_PlugWork = NULL;
}

BOOLEAN
Usb4DrPort::IsDfp() const
{
    return m_IsDfp;
}

ULONG
Usb4DrPort::DfpIndex() const
{
    return m_DfpIndex;
}

Usb4DrAdapter*
Usb4DrPort::Lane0()
{
    return m_Lane0;
}

Usb4DrAdapter*
Usb4DrPort::Lane1()
{
    return m_Lane1;
}

UCHAR
Usb4DrPort::Lane0Number() const
{
    return m_Lane0 ? m_Lane0->Number() : 0;
}

WDFCHILDLIST
Usb4DrPort::ChildList() const
{
    return m_ChildList;
}

WDFQUEUE
Usb4DrPort::ChildIoQueue() const
{
    return m_ChildIoQueue;
}

VOID
Usb4DrPort::ChildTopologyId(
    _Out_ PUSB4HR_TOPOLOGY_ID TopologyId) const
{
    *TopologyId = m_ChildTopology;
}

ULONG
Usb4DrPort::Generation()
{
    Usb4DrSpinLockGuard Guard(&m_Lock);

    return m_Generation;
}

BOOLEAN
Usb4DrPort::IsGenerationCurrent(
    _In_ ULONG Generation)
{
    Usb4DrSpinLockGuard Guard(&m_Lock);

    return Generation == m_Generation;
}

NTSTATUS
Usb4DrPort::AcquireChildHandle(
    _In_ ULONG Generation,
    _Out_ PUSB4HR_HANDLE Handle)
{
    Usb4DrSpinLockGuard Guard(&m_Lock);

    *Handle = NULL;

    if (Generation != m_Generation)
    {
        DPRINT("DFP %u: handle asked for generation %lu, current is %lu\n",
               Lane0Number(), Generation, m_Generation);
        return STATUS_UNSUCCESSFUL;
    }

    if (m_ChildHandle == NULL || m_ChildHandleClosing)
    {
        DPRINT1("DFP %u: no child router handle to hand out\n", Lane0Number());
        return STATUS_UNSUCCESSFUL;
    }

    m_ChildHandleRefs++;
    KeClearEvent(&m_ChildHandleIdle);
    *Handle = m_ChildHandle;
    return STATUS_SUCCESS;
}

VOID
Usb4DrPort::ReleaseChildHandle()
{
    Usb4DrSpinLockGuard Guard(&m_Lock);

    if (m_ChildHandleRefs <= 0)
    {
        DPRINT1("DFP %u: child router handle released too often\n", Lane0Number());
        return;
    }

    m_ChildHandleRefs -= 1;
    if (m_ChildHandleRefs == 0)
        KeSetEvent(&m_ChildHandleIdle, IO_NO_INCREMENT, FALSE);
}

BOOLEAN
Usb4DrPort::CanBondLanes()
{
    /* No lane bonding control this round; the link keeps its hardware default */
    return FALSE;
}

BOOLEAN
Usb4DrPort::IsAsymmetricSupported()
{
    return FALSE;
}

ULONG
Usb4DrPort::AgreeEnumerationDeadline(
    _In_ ULONG Seconds)
{
    BOOLEAN Tbt3 = m_Fdo->Router()->IsTbt3();
    Usb4DrSpinLockGuard Guard(&m_Lock);

    if (Tbt3)
        m_MaxEnumSeconds = USB4DR_TBT3_MAX_ENUM_SECONDS;
    else if (Seconds > m_MaxEnumSeconds)
        m_MaxEnumSeconds = Seconds;

    return m_MaxEnumSeconds;
}

ULONG
Usb4DrPort::LinkGbps()
{
    ULONG LaneCs1;
    ULONG State;
    ULONG Speed;
    ULONG PerLane;
    NTSTATUS Status;

    if (m_Lane0 == NULL)
        return 0;

    Status = m_Lane0->ReadLaneStatus(&LaneCs1);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Port %u: lane status read failed: 0x%lx\n", Lane0Number(), Status);
        return 0;
    }

    State = Usb4DrField(LaneCs1, USB4DR_LANE_CS1_ADAPTER_STATE);
    if (State == USB4DR_LANE_STATE_DISABLED || State == USB4DR_LANE_STATE_TRAINING)
        return 0;

    Speed = Usb4DrField(LaneCs1, USB4DR_LANE_CS1_CURRENT_SPEED);

    /* A version 2 link that is neither Gen 2 nor Gen 3 runs at Gen 4 */
    if (m_Fdo->Router()->IsUsb4V2() && Speed != USB4DR_LANE_SPEED_GEN2 && Speed != USB4DR_LANE_SPEED_GEN3)
        return USB4DR_LINK_GBPS_GEN4;

    PerLane = (Speed == USB4DR_LANE_SPEED_GEN3) ? USB4DR_LINK_GBPS_GEN3_LANE : USB4DR_LINK_GBPS_GEN2_LANE;

    if (Usb4DrField(LaneCs1, USB4DR_LANE_CS1_NEGOTIATED_WIDTH) == USB4DR_LANE_WIDTH_DUAL)
        return PerLane * 2;

    return PerLane;
}

VOID
NTAPI
Usb4DrPort::OnEvent(
    _In_ PVOID Context,
    _In_ NTSTATUS Status,
    _In_opt_ const USB4HR_WAIT_OUTPUT* Event)
{
    Usb4DrPort* Port = static_cast<Usb4DrPort*>(Context);

    if (!NT_SUCCESS(Status) || Event == NULL)
        return;

    DPRINT("DFP %u: hot %s\n", Port->Lane0Number(), Event->Unplugged ? "unplug" : "plug");

    {
        Usb4DrSpinLockGuard Guard(&Port->m_Lock);

        /* No debounce: the work item handles the unplug, then the latest plug state */
        if (Event->Unplugged)
        {
            Port->m_UnplugSeen = TRUE;
            Port->m_Plugged = FALSE;
        }
        else
        {
            Port->m_Plugged = TRUE;
        }

        Port->m_PlugWorkPending = TRUE;
    }

    WdfWorkItemEnqueue(Port->m_PlugWork);
}

VOID
NTAPI
Usb4DrPort::EvtPlugWork(
    _In_ WDFWORKITEM WorkItem)
{
    Usb4DrPort* Port = Usb4DrGetPortWorkContext(WorkItem)->Port;
    BOOLEAN Unplug;
    BOOLEAN Plugged;

    {
        Usb4DrSpinLockGuard Guard(&Port->m_Lock);

        /* KMDF may start a second run while one is going; the first one loops instead */
        if (Port->m_PlugWorkBusy)
            return;

        Port->m_PlugWorkBusy = TRUE;
    }

    for (;;)
    {
        {
            Usb4DrSpinLockGuard Guard(&Port->m_Lock);

            if (!Port->m_PlugWorkPending)
            {
                Port->m_PlugWorkBusy = FALSE;
                return;
            }

            Port->m_PlugWorkPending = FALSE;
            Unplug = Port->m_UnplugSeen;
            Port->m_UnplugSeen = FALSE;
            Plugged = Port->m_Plugged;
        }

        if (Unplug)
            Port->HandleUnplug();

        if (Plugged)
            Port->HandlePlug(FALSE);
    }
}

NTSTATUS
Usb4DrPort::ReadChildRouter(
    _In_ USB4HR_HANDLE Handle,
    _Out_writes_(USB4DR_ROUTER_BASIC_DWORDS) PULONG Basic,
    _Out_ PUCHAR UpstreamAdapter)
{
    Usb4DrHostLink* Link = m_Fdo->HostLink();
    USB4HR_CONFIG_EX_OUTPUT ExOutput;
    USB4HR_CONFIG_INPUT ExInput;
    USB4HR_STATUS Usb4Status = USB4HR_STATUS_FAILURE;
    ULONG_PTR Information = 0;
    UCHAR Version;
    NTSTATUS Status;

    RtlZeroMemory(Basic, USB4DR_ROUTER_BASIC_DWORDS * sizeof(*Basic));
    *UpstreamAdapter = 0;

    Status = Link->ReadConfig(Handle, USB4HR_SPACE_ROUTER, USB4DR_ROUTER_CS_0,
                              USB4DR_ROUTER_HEADER_DWORDS, Basic, &Usb4Status);
    if (NT_SUCCESS(Status) && Usb4Status != USB4HR_STATUS_SUCCESS)
        Status = STATUS_UNSUCCESSFUL;
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DFP %u: child router header read failed: 0x%lx, USB4 status %lu\n",
                Lane0Number(), Status, Usb4Status);
        return Status;
    }

    Version = (UCHAR)Usb4DrField(Basic[USB4DR_ROUTER_CS_4], USB4DR_ROUTER_CS4_VERSION_MASK);
    if (Version >= USB4DR_USB4_VERSION_1)
    {
        Usb4Status = USB4HR_STATUS_FAILURE;
        Status = Link->ReadConfig(Handle, USB4HR_SPACE_ROUTER, USB4DR_ROUTER_CS_0,
                                  USB4DR_ROUTER_BASIC_DWORDS, Basic, &Usb4Status);
        if (NT_SUCCESS(Status) && Usb4Status != USB4HR_STATUS_SUCCESS)
            Status = STATUS_UNSUCCESSFUL;
        if (!NT_SUCCESS(Status))
            DPRINT1("DFP %u: child router space read failed: 0x%lx, USB4 status %lu\n",
                    Lane0Number(), Status, Usb4Status);
        return Status;
    }

    /* A Thunderbolt 3 router names its upstream adapter only in the read response */
    RtlZeroMemory(&ExInput, sizeof(ExInput));
    RtlZeroMemory(&ExOutput, sizeof(ExOutput));
    ExInput.Handle = Handle;
    ExInput.Space = USB4HR_SPACE_ROUTER;
    ExInput.DwordOffset = USB4DR_ROUTER_CS_0;
    ExInput.DwordCount = USB4DR_ROUTER_HEADER_DWORDS;
    ExInput.Buffer = Basic;

    Status = Link->SendIoctl(Link->RootTarget(),
                             IOCTL_USB4HR_READ_CONFIG_EX,
                             &ExInput,
                             sizeof(ExInput),
                             &ExOutput,
                             sizeof(ExOutput),
                             USB4DR_CONFIG_TIMEOUT_MS,
                             &Information);
    if (NT_SUCCESS(Status) &&
        (Information < sizeof(ExOutput) || ExOutput.Status != USB4HR_STATUS_SUCCESS))
    {
        Status = STATUS_UNSUCCESSFUL;
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DFP %u: Thunderbolt 3 router read failed: 0x%lx, USB4 status %lu\n",
                Lane0Number(), Status, ExOutput.Status);
        return Status;
    }

    *UpstreamAdapter = (UCHAR)ExOutput.AdapterNumber;
    return STATUS_SUCCESS;
}

VOID
Usb4DrPort::HandlePlug(
    _In_ BOOLEAN InD0Entry)
{
    const USB4HR_HARDWARE_SERVICES* Services = m_Fdo->HostLink()->Services();
    const USB4HR_PARENT_INTERFACE* Parent = m_Fdo->HostLink()->Parent();
    ULONG Basic[USB4DR_ROUTER_BASIC_DWORDS];
    Usb4DrRouterIdentity Identity;
    Usb4DrRouterIdentity Previous;
    USB4HR_ROUTER_FAMILY Type = Usb4HrRouterUnknown;
    USB4HR_HANDLE Handle;
    ULONG TypeArgument = 0;
    ULONG LaneCs1;
    ULONG PortCs18;
    UCHAR Upstream;
    UCHAR Version = 0;
    BOOLEAN ConfigValid;
    BOOLEAN Closing;
    NTSTATUS Status;

    if (m_ChildTopology.Depth == 0 || m_ChildTopology.Depth > USB4HR_MAX_DEPTH)
    {
        DPRINT1("DFP %u: router plugged deeper than USB4 allows, ignored\n", Lane0Number());
        return;
    }

    {
        Usb4DrSpinLockGuard Guard(&m_Lock);
        Closing = (m_ChildHandle != NULL && m_ChildHandleClosing);
    }

    /* The router that left before must be gone from PnP before a new one is reported */
    if (Closing)
        DeleteChildHandle(!InD0Entry);

    {
        Usb4DrSpinLockGuard Guard(&m_Lock);
        Handle = m_ChildHandle;
        Closing = m_ChildHandleClosing;
    }

    if (Handle != NULL && Closing)
    {
        DPRINT1("DFP %u: the previous router still holds its handle, plug ignored\n", Lane0Number());
        return;
    }

    if (Handle == NULL)
    {
        Status = Services->AllocateRouterHandle(Services->Header.Context, &m_ChildTopology, &Handle);
        if (!NT_SUCCESS(Status) || Handle == NULL)
        {
            DPRINT1("DFP %u: child router handle allocation failed: 0x%lx\n", Lane0Number(), Status);
            return;
        }

        Usb4DrSpinLockGuard Guard(&m_Lock);
        m_ChildHandle = Handle;
        m_ChildHandleRefs = 0;
        m_ChildHandleClosing = FALSE;
        KeSetEvent(&m_ChildHandleIdle, IO_NO_INCREMENT, FALSE);
    }

    Status = ReadChildRouter(Handle, Basic, &Upstream);
    ConfigValid = NT_SUCCESS(Status);

    if (ConfigValid)
    {
        Version = (UCHAR)Usb4DrField(Basic[USB4DR_ROUTER_CS_4], USB4DR_ROUTER_CS4_VERSION_MASK);

        if (Version >= USB4DR_USB4_VERSION_1)
        {
            Type = Usb4HrRouterUsb4;
            TypeArgument = Version;

            /* QUIRK: below a version 1 connection manager the host router is told 0x20 */
            if (Parent->CmVersionLimit < USB4DR_CM_VERSION_2)
                TypeArgument = USB4DR_USB4_VERSION_1;
        }
        else
        {
            Type = Usb4DrClassifyTbt3Router(
                (USHORT)Usb4DrField(Basic[USB4DR_ROUTER_CS_0], USB4DR_ROUTER_CS0_VENDOR_MASK),
                (USHORT)Usb4DrField(Basic[USB4DR_ROUTER_CS_0], USB4DR_ROUTER_CS0_PRODUCT_MASK));
            if (Type == Usb4HrRouterUnknown)
                DPRINT1("DFP %u: Thunderbolt 3 router 0x%08lx is not supported\n",
                        Lane0Number(), Basic[USB4DR_ROUTER_CS_0]);
        }
    }

    Status = Services->ReportRouterFamily(Services->Header.Context, Handle, Type, TypeArgument);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DFP %u: router type %d not accepted: 0x%lx\n", Lane0Number(), Type, Status);
        Type = Usb4HrRouterUnknown;
    }

    m_ChildType = Type;

    RtlZeroMemory(&Identity, sizeof(Identity));
    Identity.Lane0AdapterNumber = Lane0Number();
    if (ConfigValid)
    {
        Identity.VendorId = (USHORT)Usb4DrField(Basic[USB4DR_ROUTER_CS_0], USB4DR_ROUTER_CS0_VENDOR_MASK);
        Identity.ProductId = (USHORT)Usb4DrField(Basic[USB4DR_ROUTER_CS_0], USB4DR_ROUTER_CS0_PRODUCT_MASK);
    }

    if (Type == Usb4HrRouterUnknown)
    {
        DPRINT1("DFP %u: reporting an unknown router\n", Lane0Number());
        ReportChild(&Identity);
        return;
    }

    Identity.Revision = (UCHAR)Usb4DrField(Basic[USB4DR_ROUTER_CS_1], USB4DR_ROUTER_CS1_REVISION_MASK);
    Identity.Usb4Version = Version;
    Identity.MaxAdapterNumber = (UCHAR)Usb4DrField(Basic[USB4DR_ROUTER_CS_1], USB4DR_ROUTER_CS1_MAX_ADAPTER_MASK);
    Identity.IsKnown = TRUE;

    if (Version >= USB4DR_USB4_VERSION_1)
    {
        Identity.UfpLane0AdapterNumber =
            (UCHAR)Usb4DrField(Basic[USB4DR_ROUTER_CS_1], USB4DR_ROUTER_CS1_UPSTREAM_MASK);
        Identity.RouterUuid = ((ULONG64)Basic[USB4DR_ROUTER_CS_UUID_HIGH] << 32) |
                              Basic[USB4DR_ROUTER_CS_UUID_LOW];
    }
    else
    {
        /* The Thunderbolt 3 router ID lives in a vendor capability this driver does not read */
        Identity.UfpLane0AdapterNumber = Upstream;
    }

    if (NT_SUCCESS(m_Lane0->ReadLaneStatus(&LaneCs1)))
        Identity.CurrentLinkSpeed = Usb4DrField(LaneCs1, USB4DR_LANE_CS1_CURRENT_SPEED);

    /* Windows takes Thunderbolt 3 from this router or from the port's compatibility mode */
    Identity.IsTbt3 = m_Fdo->Router()->IsTbt3();
    if (!Identity.IsTbt3 &&
        m_Lane0->PortCapability() != 0 &&
        NT_SUCCESS(m_Lane0->ReadDword(m_Lane0->PortCapability() + USB4DR_PORT_CS_18, &PortCs18)) &&
        (PortCs18 & USB4DR_PORT_CS18_TBT3_COMPAT))
    {
        Identity.IsTbt3 = TRUE;
    }

    if (m_ChildReported)
    {
        if (RtlCompareMemory(&Identity, &m_ChildIdentity, sizeof(Identity)) == sizeof(Identity))
            return;

        /* Windows treats a new link speed on resume as unexpected and still reports a new router */
        Previous = m_ChildIdentity;
        Previous.CurrentLinkSpeed = Identity.CurrentLinkSpeed;
        if (InD0Entry && RtlCompareMemory(&Identity, &Previous, sizeof(Identity)) == sizeof(Identity))
        {
            DPRINT1("DFP %u: link speed changed from %lu to %lu while powered down\n",
                    Lane0Number(), m_ChildIdentity.CurrentLinkSpeed, Identity.CurrentLinkSpeed);
        }
        else
        {
            DPRINT("DFP %u: a different router is present\n", Lane0Number());
        }

        BumpGeneration();
    }

    DPRINT("DFP %u: router %04x:%04x version 0x%02x\n",
           Lane0Number(), Identity.VendorId, Identity.ProductId, Identity.Usb4Version);
    ReportChild(&Identity);
}

VOID
Usb4DrPort::HandleUnplug()
{
    const USB4HR_HARDWARE_SERVICES* Services = m_Fdo->HostLink()->Services();
    USB4HR_HANDLE Handle;
    BOOLEAN Reported;

    {
        Usb4DrSpinLockGuard Guard(&m_Lock);

        Handle = m_ChildHandle;
        Reported = m_ChildReported;
    }

    if (Handle == NULL && !Reported)
        return;

    DPRINT("DFP %u: router gone\n", Lane0Number());

    if (Handle != NULL)
    {
        /* QUIRK: an Alpine Ridge router keeps its notification queue, as in Windows */
        Services->PurgeEventQueues(Services->Header.Context,
                                   Handle,
                                   m_ChildType != Usb4HrRouterTbt3AlpineRidge,
                                   TRUE);
    }

    ReportChildMissing();
    ClearLane1Disable();
    DeleteChildHandle(TRUE);
}

VOID
Usb4DrPort::ReportChild(
    _In_ const Usb4DrRouterIdentity* Identity)
{
    Usb4DrChildDescription Description;
    NTSTATUS Status;

    RtlZeroMemory(&Description, sizeof(Description));
    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&Description.Header, sizeof(Description));
    Description.Type = Usb4DrChildType::DeviceRouter;
    Description.Generation = Generation();
    Description.Identity = *Identity;

    WdfChildListBeginScan(m_ChildList);
    Status = WdfChildListAddOrUpdateChildDescriptionAsPresent(m_ChildList, &Description.Header, NULL);
    WdfChildListEndScan(m_ChildList);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DFP %u: child router not reported: 0x%lx\n", Lane0Number(), Status);
        return;
    }

    m_ChildIdentity = *Identity;

    Usb4DrSpinLockGuard Guard(&m_Lock);
    m_ChildReported = TRUE;
}

VOID
Usb4DrPort::ReportChildMissing()
{
    if (m_ChildList == NULL)
        return;

    /* An empty scan drops every child of the list */
    WdfChildListBeginScan(m_ChildList);
    WdfChildListEndScan(m_ChildList);

    Usb4DrSpinLockGuard Guard(&m_Lock);
    m_ChildReported = FALSE;
    m_MaxEnumSeconds = m_InitialEnumSeconds;
}

VOID
Usb4DrPort::DeleteChildHandle(
    _In_ BOOLEAN MayWait)
{
    const USB4HR_HARDWARE_SERVICES* Services = m_Fdo->HostLink()->Services();
    USB4HR_HANDLE Handle;
    NTSTATUS Status;

    for (;;)
    {
        {
            Usb4DrSpinLockGuard Guard(&m_Lock);

            Handle = m_ChildHandle;
            if (Handle == NULL)
                return;

            /* No new references from here on */
            m_ChildHandleClosing = TRUE;

            if (m_ChildHandleRefs == 0)
            {
                m_ChildHandle = NULL;
                m_ChildHandleClosing = FALSE;
                break;
            }

            /* PnP cannot release the child PDO during a power transition; a later start or Destroy finishes */
            if (!MayWait || m_Stopping)
            {
                DPRINT("DFP %u: child router handle stays until its PDO is gone\n", Lane0Number());
                return;
            }

            /* Stop signals the event to end this wait, so arm it again under the lock */
            KeClearEvent(&m_ChildHandleIdle);
        }

        Status = Usb4DrFdo::WaitBounded(&m_ChildHandleIdle, "child router handle release");
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("DFP %u: child PDO kept router handle %p, leaving it for later\n", Lane0Number(), Handle);
            return;
        }
    }

    Status = Services->DestroyRouterHandle(Services->Header.Context, Handle);
    if (!NT_SUCCESS(Status))
        DPRINT1("DFP %u: router handle %p not destroyed: 0x%lx\n", Lane0Number(), Handle, Status);

    Usb4DrSpinLockGuard Guard(&m_Lock);
    m_ChildType = Usb4HrRouterUnknown;
}

VOID
Usb4DrPort::ClearLane1Disable()
{
    ULONG LaneCs1;
    UCHAR Capability;
    NTSTATUS Status;

    if (m_Lane1 == NULL)
        return;

    Capability = m_Lane1->LaneCapability();
    if (Capability == 0)
        return;

    Status = m_Lane1->ReadDword(Capability + USB4DR_LANE_CS_1, &LaneCs1);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DFP %u: lane 1 read failed: 0x%lx\n", Lane0Number(), Status);
        return;
    }

    if (!(LaneCs1 & USB4DR_LANE_CS1_LANE_DISABLE))
        return;

    Status = m_Lane1->WriteDword(Capability + USB4DR_LANE_CS_1, LaneCs1 & ~USB4DR_LANE_CS1_LANE_DISABLE);
    if (!NT_SUCCESS(Status))
        DPRINT1("DFP %u: lane 1 enable failed: 0x%lx\n", Lane0Number(), Status);
}

VOID
Usb4DrPort::BumpGeneration()
{
    Usb4DrSpinLockGuard Guard(&m_Lock);

    m_Generation++;
}

/* Usb4DrPortSet **************************************************************/

NTSTATUS
Usb4DrPortSet::Build(
    _In_ Usb4DrFdo* Fdo)
{
    Usb4DrRouter* Router = Fdo->Router();
    Usb4DrAdapter* Lanes[USB4DR_MAX_ADAPTERS];
    Usb4DrAdapter* Adapter;
    ULONG LaneCount = 0;
    ULONG UfpPair = MAXULONG;
    ULONG Number;
    ULONG Pair;
    BOOLEAN IsDfp;
    NTSTATUS Status;

    /* A start after D3Final keeps the ports; only the adapter handles are new */
    if (m_PortCount != 0)
    {
        SilenceAdapters();
        return STATUS_SUCCESS;
    }

    m_Fdo = Fdo;
    m_Ufp = NULL;
    m_DfpCount = 0;

    for (Number = 1; Number <= Router->MaxAdapter() && LaneCount < RTL_NUMBER_OF(Lanes); Number++)
    {
        Adapter = Router->Adapter((UCHAR)Number);
        if (Adapter == NULL)
            continue;

        if (Adapter->IsLane())
            Lanes[LaneCount++] = Adapter;
    }

    if (LaneCount % 2 != 0)
    {
        DPRINT1("Router has %lu lane adapters, not a whole number of ports\n", LaneCount);
        return STATUS_UNSUCCESSFUL;
    }

    for (Pair = 0; Pair < LaneCount; Pair += 2)
    {
        if (Lanes[Pair + 1]->Number() != Lanes[Pair]->Number() + 1)
        {
            DPRINT1("Lane adapters %u and %u do not form a port\n",
                    Lanes[Pair]->Number(), Lanes[Pair + 1]->Number());
            return STATUS_UNSUCCESSFUL;
        }
    }

    if (Fdo->Depth() > 0)
    {
        if (LaneCount == 0)
        {
            DPRINT1("Router at depth %lu has no lane adapters\n", Fdo->Depth());
            return STATUS_UNSUCCESSFUL;
        }

        /* QUIRK: a USB4 router's upstream port is its lowest lane pair; only TBT3 uses ROUTER_CS_1 */
        UfpPair = 0;
        if (Router->IsTbt3())
        {
            for (Pair = 0; Pair < LaneCount; Pair += 2)
            {
                if (Lanes[Pair]->Number() == Router->UpstreamAdapter())
                    UfpPair = Pair;
            }
        }
    }

    for (Pair = 0; Pair < LaneCount; Pair += 2)
    {
        IsDfp = (Pair != UfpPair);

        Status = m_Ports[m_PortCount].Initialize(Fdo, Lanes[Pair], Lanes[Pair + 1], IsDfp, m_DfpCount);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Port on lane adapter %u failed to initialize: 0x%lx\n", Lanes[Pair]->Number(), Status);
            m_PortCount++;
            return Status;
        }

        if (IsDfp)
            m_DfpCount++;
        else
            m_Ufp = &m_Ports[m_PortCount];

        m_PortCount++;
    }

    SilenceAdapters();

    DPRINT("Router at depth %lu: %lu ports, %lu downstream\n", Fdo->Depth(), m_PortCount, m_DfpCount);
    return STATUS_SUCCESS;
}

/* Only the lane 0 adapter of a DFP delivers hot plug events */
VOID
Usb4DrPortSet::SilenceAdapters()
{
    ULONG Index;

    for (Index = 0; Index < m_PortCount; Index++)
    {
        if (!m_Ports[Index].IsDfp())
            Usb4DrSilenceAdapter(m_Fdo, m_Ports[Index].Lane0());

        Usb4DrSilenceAdapter(m_Fdo, m_Ports[Index].Lane1());
    }
}

NTSTATUS
Usb4DrPortSet::Start(
    _In_ BOOLEAN FirstStart)
{
    ULONG Index;
    NTSTATUS Status;

    for (Index = 0; Index < m_PortCount; Index++)
    {
        Status = m_Ports[Index].Start(FirstStart);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return STATUS_SUCCESS;
}

VOID
Usb4DrPortSet::Stop(
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    ULONG Index;

    for (Index = 0; Index < m_PortCount; Index++)
        m_Ports[Index].Stop(TargetState);
}

VOID
Usb4DrPortSet::Destroy()
{
    ULONG Index;

    for (Index = 0; Index < m_PortCount; Index++)
        m_Ports[Index].Destroy();
}

Usb4DrPort*
Usb4DrPortSet::Ufp()
{
    return m_Ufp;
}

ULONG
Usb4DrPortSet::DfpCount() const
{
    return m_DfpCount;
}

Usb4DrPort*
Usb4DrPortSet::Dfp(
    _In_ ULONG Index)
{
    ULONG Slot;

    for (Slot = 0; Slot < m_PortCount; Slot++)
    {
        if (m_Ports[Slot].IsDfp() && m_Ports[Slot].DfpIndex() == Index)
            return &m_Ports[Slot];
    }

    return NULL;
}

Usb4DrPort*
Usb4DrPortSet::DfpByLane0(
    _In_ UCHAR Lane0Number)
{
    ULONG Slot;

    for (Slot = 0; Slot < m_PortCount; Slot++)
    {
        if (m_Ports[Slot].IsDfp() && m_Ports[Slot].Lane0Number() == Lane0Number)
            return &m_Ports[Slot];
    }

    return NULL;
}

VOID
Usb4DrPortSet::ReportAllChildrenMissing()
{
    ULONG Index;

    for (Index = 0; Index < m_PortCount; Index++)
    {
        if (m_Ports[Index].IsDfp())
            m_Ports[Index].ReportChildMissing();
    }
}
