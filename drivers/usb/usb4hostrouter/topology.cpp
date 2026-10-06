/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Router and adapter handles, event queues, hot plug and notification packets
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4hr.h"

#define NDEBUG
#include <debug.h>

/** One router (adapter 0) or adapter of the domain. Its address is the handle. */
struct Usb4HrNode
{
    LIST_ENTRY Link;
    volatile LONG References;
    Usb4HrRoute Route;
    UCHAR Adapter;
    USB4HR_ROUTER_TYPE RouterType;
    ULONG Usb4Version;
    volatile LONG Sequence;

    /** Protects everything below. */
    KSPIN_LOCK QueueLock;
    WDFQUEUE WaitQueue;         /**< manual queue of parked WAIT requests, made on first use */
    BOOLEAN EventsStopped;
    BOOLEAN InterDomainStopped;
    ULONG BacklogFirst;
    ULONG BacklogCount;
    BOOLEAN Backlog[USB4HR_EVENT_BACKLOG];  /**< TRUE for an unplug */
};

/** A hot plug packet waiting for the work item. */
struct Usb4HrPlugPacket
{
    LIST_ENTRY Link;
    ULONG Dword[USB4HR_PACKET_HEADER_DWORDS];
};

/* Node helpers ***************************************************************/

static
BOOLEAN
NTAPI
Usb4HrRouteEqual(
    _In_ const Usb4HrRoute* First,
    _In_ const Usb4HrRoute* Second)
{
    if (First->Depth != Second->Depth)
        return FALSE;

    return RtlEqualMemory(First->Port, Second->Port, First->Depth);
}

/** Route of a topology ID; the depth is trimmed to the last nonzero port like a packet route. */
static
NTSTATUS
NTAPI
Usb4HrRouteFromTopologyId(
    _In_ const USB4HR_TOPOLOGY_ID* TopologyId,
    _Out_ Usb4HrRoute* Route)
{
    ULONG Index;

    RtlZeroMemory(Route, sizeof(*Route));

    /* Windows copies Depth ports without a bound check */
    if (TopologyId->Depth > USB4HR_MAX_DEPTH)
    {
        DPRINT1("Topology ID depth %lu is too deep\n", TopologyId->Depth);
        return STATUS_INVALID_PARAMETER;
    }

    for (Index = 0; Index < TopologyId->Depth; Index++)
    {
        Route->Port[Index] = TopologyId->Port[Index];
        if (Route->Port[Index] != 0)
            Route->Depth = (UCHAR)(Index + 1);
    }

    return STATUS_SUCCESS;
}

static
BOOLEAN
NTAPI
Usb4HrIsSpaceValid(
    _In_ const Usb4HrNode* Node,
    _In_ ULONG Space)
{
    if (Space > USB4HR_SPACE_COUNTERS)
        return FALSE;

    /* A router handle reaches only the router space, an adapter handle everything else */
    if (Node->Adapter == 0)
        return Space == USB4HR_SPACE_ROUTER;

    return Space != USB4HR_SPACE_ROUTER;
}

static
VOID
NTAPI
Usb4HrReleaseNode(
    _In_ Usb4HrNode* Node)
{
    if (InterlockedDecrement(&Node->References) != 0)
        return;

    NT_ASSERT(Node->WaitQueue == NULL);
    ExFreePoolWithTag(Node, USB4HR_TAG_TOPOLOGY);
}

/** Completes a parked WAIT request with one event. */
static
VOID
NTAPI
Usb4HrCompleteWait(
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN Unplugged)
{
    PUSB4HR_WAIT_OUTPUT Output;
    NTSTATUS Status;

    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Output), (PVOID*)&Output, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WAIT output buffer lost: 0x%lx\n", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    Output->Status = USB4HR_STATUS_SUCCESS;
    Output->Unplugged = Unplugged ? 1 : 0;
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*Output));
}

/** Pairs stored events with parked requests until either runs out. */
static
VOID
NTAPI
Usb4HrDeliverNodeEvents(
    _In_ Usb4HrNode* Node)
{
    WDFREQUEST Request;
    BOOLEAN Unplugged;
    NTSTATUS Status;
    KIRQL OldIrql;

    for (;;)
    {
        KeAcquireSpinLock(&Node->QueueLock, &OldIrql);

        if (Node->BacklogCount == 0 || Node->WaitQueue == NULL)
        {
            KeReleaseSpinLock(&Node->QueueLock, OldIrql);
            return;
        }

        Status = WdfIoQueueRetrieveNextRequest(Node->WaitQueue, &Request);
        if (!NT_SUCCESS(Status))
        {
            KeReleaseSpinLock(&Node->QueueLock, OldIrql);
            return;
        }

        Unplugged = Node->Backlog[Node->BacklogFirst];
        Node->BacklogFirst = (Node->BacklogFirst + 1) % USB4HR_EVENT_BACKLOG;
        Node->BacklogCount--;

        KeReleaseSpinLock(&Node->QueueLock, OldIrql);

        Usb4HrCompleteWait(Request, Unplugged);
    }
}

/** Stores one hot plug event and hands it to a parked request if there is one. */
static
NTSTATUS
NTAPI
Usb4HrQueueNodeEvent(
    _In_ Usb4HrNode* Node,
    _In_ BOOLEAN Unplugged)
{
    ULONG Slot;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Node->QueueLock, &OldIrql);

    if (Node->EventsStopped)
    {
        KeReleaseSpinLock(&Node->QueueLock, OldIrql);
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (Node->BacklogCount == USB4HR_EVENT_BACKLOG)
    {
        DPRINT1("Event backlog of node %p full, dropping the oldest\n", Node);
        Node->BacklogFirst = (Node->BacklogFirst + 1) % USB4HR_EVENT_BACKLOG;
        Node->BacklogCount--;
    }

    Slot = (Node->BacklogFirst + Node->BacklogCount) % USB4HR_EVENT_BACKLOG;
    Node->Backlog[Slot] = Unplugged;
    Node->BacklogCount++;

    KeReleaseSpinLock(&Node->QueueLock, OldIrql);

    Usb4HrDeliverNodeEvents(Node);
    return STATUS_SUCCESS;
}

/** Parks a WAIT request on the node; STATUS_UNSUCCESSFUL once the node's queue was stopped. */
static
NTSTATUS
NTAPI
Usb4HrParkWaitRequest(
    _In_ Usb4HrNode* Node,
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request)
{
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDFQUEUE NewQueue = NULL;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (Node->WaitQueue == NULL && !Node->EventsStopped)
    {
        /* Parked requests must not hold the host router in D0 */
        WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig, WdfIoQueueDispatchManual);
        QueueConfig.PowerManaged = WdfFalse;

        Status = WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &NewQueue);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Event queue create failed: 0x%lx\n", Status);
            return Status;
        }
    }

    KeAcquireSpinLock(&Node->QueueLock, &OldIrql);

    if (Node->EventsStopped)
    {
        Status = STATUS_UNSUCCESSFUL;
    }
    else
    {
        if (Node->WaitQueue == NULL)
        {
            Node->WaitQueue = NewQueue;
            NewQueue = NULL;
        }

        if (Node->WaitQueue == NULL)
            Status = STATUS_UNSUCCESSFUL;
        else
            Status = WdfRequestForwardToIoQueue(Request, Node->WaitQueue);
    }

    KeReleaseSpinLock(&Node->QueueLock, OldIrql);

    if (NewQueue != NULL)
        WdfObjectDelete(NewQueue);

    if (NT_SUCCESS(Status))
        Usb4HrDeliverNodeEvents(Node);

    return Status;
}

/** Cancels parked requests and refuses later events; the queues stay stopped for good. */
static
VOID
NTAPI
Usb4HrStopNodeQueues(
    _In_ Usb4HrNode* Node,
    _In_ BOOLEAN Notifications,
    _In_ BOOLEAN InterDomain)
{
    WDFQUEUE Queue = NULL;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Node->QueueLock, &OldIrql);

    if (Notifications)
    {
        Node->EventsStopped = TRUE;
        Node->BacklogFirst = 0;
        Node->BacklogCount = 0;
        Queue = Node->WaitQueue;
        Node->WaitQueue = NULL;
    }

    /* Routers have no inter-domain queue */
    if (InterDomain && Node->Adapter != 0)
        Node->InterDomainStopped = TRUE;

    KeReleaseSpinLock(&Node->QueueLock, OldIrql);

    if (Queue != NULL)
    {
        /* Parked requests complete with STATUS_CANCELLED */
        WdfIoQueuePurge(Queue, NULL, NULL);
        WdfObjectDelete(Queue);
    }
}

/* Usb4HrTopology *************************************************************/

NTSTATUS
Usb4HrTopology::Create(
    _In_ Usb4HrHostRouter* HostRouter)
{
    WDF_WORKITEM_CONFIG WorkItemConfig;
    WDF_OBJECT_ATTRIBUTES Attributes;
    Usb4HrRingZero* Ring = HostRouter->Ring();
    Usb4HrRoute RootRoute;
    NTSTATUS Status;

    m_HostRouter = HostRouter;
    KeInitializeSpinLock(&m_Lock);
    InitializeListHead(&m_Nodes);
    KeInitializeEvent(&m_RootPoweredOn, NotificationEvent, FALSE);
    KeInitializeSpinLock(&m_PlugLock);
    InitializeListHead(&m_PlugPackets);

    WDF_WORKITEM_CONFIG_INIT(&WorkItemConfig, EvtPlugWorkItem);
    WorkItemConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = HostRouter->Device();

    Status = WdfWorkItemCreate(&WorkItemConfig, &Attributes, &m_PlugWorkItem);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Hot plug work item create failed: 0x%lx\n", Status);
        return Status;
    }

    Status = Ring->RegisterHandler(USB4HR_PDF_NOTIFICATION, OnNotificationPacket, this);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Notification handler registration failed: 0x%lx\n", Status);
        return Status;
    }

    Status = Ring->RegisterHandler(USB4HR_PDF_HOT_PLUG, OnHotPlugPacket, this);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Hot plug handler registration failed: 0x%lx\n", Status);
        return Status;
    }

    RtlZeroMemory(&RootRoute, sizeof(RootRoute));
    Status = AllocateNode(&RootRoute, 0, &m_RootNode);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Root router handle allocation failed: 0x%lx\n", Status);
        return Status;
    }

    /* QUIRK: Windows stops the root router's own queues right away; WAIT_ROUTER_EVENT on it fails */
    PurgeEventQueues(m_RootNode, TRUE, TRUE);

    return STATUS_SUCCESS;
}

VOID Usb4HrTopology::Cleanup()
{
    LIST_ENTRY Nodes;
    LIST_ENTRY Packets;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    /* Create never ran */
    if (m_Nodes.Flink == NULL)
        return;

    InitializeListHead(&Nodes);
    InitializeListHead(&Packets);

    KeAcquireSpinLock(&m_Lock, &OldIrql);
    while (!IsListEmpty(&m_Nodes))
    {
        Entry = RemoveHeadList(&m_Nodes);
        InsertTailList(&Nodes, Entry);
    }
    m_RouterCount = 0;
    m_RootNode = NULL;
    KeReleaseSpinLock(&m_Lock, OldIrql);

    KeAcquireSpinLock(&m_PlugLock, &OldIrql);
    while (!IsListEmpty(&m_PlugPackets))
    {
        Entry = RemoveHeadList(&m_PlugPackets);
        InsertTailList(&Packets, Entry);
    }
    KeReleaseSpinLock(&m_PlugLock, OldIrql);

    /*
     * The event queues are children of the device, which cancels what is
     * still parked on them as it goes away; only the memory is ours.
     */
    while (!IsListEmpty(&Nodes))
    {
        Usb4HrNode* Node = CONTAINING_RECORD(RemoveHeadList(&Nodes), Usb4HrNode, Link);

        ExFreePoolWithTag(Node, USB4HR_TAG_TOPOLOGY);
    }

    while (!IsListEmpty(&Packets))
    {
        Usb4HrPlugPacket* Packet = CONTAINING_RECORD(RemoveHeadList(&Packets), Usb4HrPlugPacket, Link);

        ExFreePoolWithTag(Packet, USB4HR_TAG_TOPOLOGY);
    }
}

USB4HR_HANDLE Usb4HrTopology::RootRouterHandle() const
{
    return m_RootNode;
}

NTSTATUS
Usb4HrTopology::AllocateNode(
    _In_ const Usb4HrRoute* Route,
    _In_ UCHAR Adapter,
    _Out_ Usb4HrNode** Node)
{
    Usb4HrNode* NewNode;
    KIRQL OldIrql;

    *Node = NULL;

    NewNode = (Usb4HrNode*)ExAllocatePoolWithTag(NonPagedPool, sizeof(*NewNode), USB4HR_TAG_TOPOLOGY);
    if (NewNode == NULL)
    {
        DPRINT1("No memory for a topology node\n");
        return STATUS_UNSUCCESSFUL;
    }

    RtlZeroMemory(NewNode, sizeof(*NewNode));
    NewNode->References = 1;
    NewNode->Route = *Route;
    NewNode->Adapter = Adapter;
    NewNode->RouterType = Usb4HrRouterUsb4;
    KeInitializeSpinLock(&NewNode->QueueLock);

    KeAcquireSpinLock(&m_Lock, &OldIrql);

    if (FindNodeByRouteLocked(Route, Adapter) != NULL)
    {
        KeReleaseSpinLock(&m_Lock, OldIrql);
        DPRINT1("Handle for depth %u adapter %u already exists\n", Route->Depth, Adapter);
        ExFreePoolWithTag(NewNode, USB4HR_TAG_TOPOLOGY);
        return STATUS_UNSUCCESSFUL;
    }

    InsertTailList(&m_Nodes, &NewNode->Link);
    if (Adapter == 0)
        m_RouterCount++;

    KeReleaseSpinLock(&m_Lock, OldIrql);

    *Node = NewNode;
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrTopology::DestroyNode(
    _In_ USB4HR_HANDLE Handle)
{
    Usb4HrNode* Node;
    KIRQL OldIrql;

    KeAcquireSpinLock(&m_Lock, &OldIrql);

    Node = FindNodeLocked(Handle);
    if (Node == NULL)
    {
        KeReleaseSpinLock(&m_Lock, OldIrql);
        DPRINT1("Destroy of unknown handle %p\n", Handle);
        return STATUS_UNSUCCESSFUL;
    }

    RemoveEntryList(&Node->Link);
    if (Node->Adapter == 0)
        m_RouterCount--;
    if (Node == m_RootNode)
        m_RootNode = NULL;

    KeReleaseSpinLock(&m_Lock, OldIrql);

    Usb4HrStopNodeQueues(Node, TRUE, TRUE);
    Usb4HrReleaseNode(Node);
    return STATUS_SUCCESS;
}

Usb4HrNode*
Usb4HrTopology::FindNodeLocked(
    _In_ USB4HR_HANDLE Handle)
{
    PLIST_ENTRY Entry;

    if (Handle == NULL)
        return NULL;

    for (Entry = m_Nodes.Flink; Entry != &m_Nodes; Entry = Entry->Flink)
    {
        Usb4HrNode* Node = CONTAINING_RECORD(Entry, Usb4HrNode, Link);

        if (Node == Handle)
            return Node;
    }

    return NULL;
}

Usb4HrNode*
Usb4HrTopology::FindNodeByRouteLocked(
    _In_ const Usb4HrRoute* Route,
    _In_ UCHAR Adapter)
{
    PLIST_ENTRY Entry;

    for (Entry = m_Nodes.Flink; Entry != &m_Nodes; Entry = Entry->Flink)
    {
        Usb4HrNode* Node = CONTAINING_RECORD(Entry, Usb4HrNode, Link);

        if (Node->Adapter == Adapter && Usb4HrRouteEqual(&Node->Route, Route))
            return Node;
    }

    return NULL;
}

Usb4HrNode*
Usb4HrTopology::ReferenceNode(
    _In_ USB4HR_HANDLE Handle)
{
    Usb4HrSpinLockGuard Guard(&m_Lock);
    Usb4HrNode* Node = FindNodeLocked(Handle);

    if (Node != NULL)
        InterlockedIncrement(&Node->References);

    return Node;
}

Usb4HrNode*
Usb4HrTopology::ReferenceNodeByRoute(
    _In_ const Usb4HrRoute* Route,
    _In_ UCHAR Adapter)
{
    Usb4HrSpinLockGuard Guard(&m_Lock);
    Usb4HrNode* Node = FindNodeByRouteLocked(Route, Adapter);

    if (Node != NULL)
        InterlockedIncrement(&Node->References);

    return Node;
}

VOID
Usb4HrTopology::SetRouteTypeLocked(
    _In_ const Usb4HrRoute* Route,
    _In_ USB4HR_ROUTER_TYPE Type,
    _In_ ULONG Usb4Version)
{
    PLIST_ENTRY Entry;

    for (Entry = m_Nodes.Flink; Entry != &m_Nodes; Entry = Entry->Flink)
    {
        Usb4HrNode* Node = CONTAINING_RECORD(Entry, Usb4HrNode, Link);

        if (Usb4HrRouteEqual(&Node->Route, Route))
        {
            Node->RouterType = Type;
            Node->Usb4Version = Usb4Version;
        }
    }
}

NTSTATUS
Usb4HrTopology::AllocateRouterHandle(
    _In_ const USB4HR_TOPOLOGY_ID* TopologyId,
    _Out_ PUSB4HR_HANDLE Handle)
{
    Usb4HrRoute Route;
    Usb4HrNode* Node;
    NTSTATUS Status;

    *Handle = NULL;

    Status = Usb4HrRouteFromTopologyId(TopologyId, &Route);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = AllocateNode(&Route, 0, &Node);
    if (!NT_SUCCESS(Status))
        return Status;

    DPRINT("Router handle %p at depth %u\n", Node, Route.Depth);
    *Handle = Node;
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrTopology::DestroyRouterHandle(
    _In_ USB4HR_HANDLE Handle)
{
    Usb4HrHandleInfo Info;
    NTSTATUS Status;

    Status = QueryHandle(Handle, &Info);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Destroy of unknown router handle %p\n", Handle);
        return STATUS_UNSUCCESSFUL;
    }

    if (Info.IsRouter)
        m_HostRouter->Tunnels()->OnRouterRemoved(&Info.Route);

    return DestroyNode(Handle);
}

NTSTATUS
Usb4HrTopology::AllocateAdapterHandles(
    _In_ USB4HR_HANDLE RouterHandle,
    _In_ UCHAR Count,
    _Out_writes_(Count) PUSB4HR_HANDLE Handles)
{
    Usb4HrHandleInfo Info;
    Usb4HrNode* Node;
    NTSTATUS Status;
    UCHAR Made;
    KIRQL OldIrql;

    if (Count == 0 || Count > USB4HR_MAX_ADAPTERS)
    {
        DPRINT1("Adapter handle count %u out of range\n", Count);
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(Handles, Count * sizeof(*Handles));

    Status = QueryHandle(RouterHandle, &Info);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Adapter handles for unknown router handle %p\n", RouterHandle);
        return STATUS_INVALID_PARAMETER;
    }

    for (Made = 0; Made < Count; Made++)
    {
        Status = AllocateNode(&Info.Route, (UCHAR)(Made + 1), &Node);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Adapter %u handle failed, undoing %u handles\n", Made + 1, Made);
            while (Made > 0)
            {
                Made--;
                DestroyNode(Handles[Made]);
                Handles[Made] = NULL;
            }
            return STATUS_UNSUCCESSFUL;
        }

        Handles[Made] = Node;
    }

    /* New adapters take the type the router already has */
    KeAcquireSpinLock(&m_Lock, &OldIrql);
    SetRouteTypeLocked(&Info.Route, Info.RouterType, Info.Usb4Version);
    KeReleaseSpinLock(&m_Lock, OldIrql);

    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrTopology::DestroyAdapterHandle(
    _In_ USB4HR_HANDLE Handle)
{
    return DestroyNode(Handle);
}

NTSTATUS
Usb4HrTopology::SetRouterType(
    _In_ USB4HR_HANDLE RouterHandle,
    _In_ USB4HR_ROUTER_TYPE Type,
    _In_ ULONG Usb4Version)
{
    Usb4HrSpinLockGuard Guard(&m_Lock);
    Usb4HrNode* Node = FindNodeLocked(RouterHandle);

    if (Node == NULL)
    {
        DPRINT1("Router type for unknown handle %p\n", RouterHandle);
        return STATUS_INVALID_PARAMETER;
    }

    SetRouteTypeLocked(&Node->Route, Type, Usb4Version);
    return STATUS_SUCCESS;
}

VOID
Usb4HrTopology::PurgeEventQueues(
    _In_ USB4HR_HANDLE Handle,
    _In_ BOOLEAN Notifications,
    _In_ BOOLEAN InterDomain)
{
    Usb4HrNode* Node = ReferenceNode(Handle);

    if (Node == NULL)
    {
        DPRINT1("Purge of unknown handle %p\n", Handle);
        return;
    }

    Usb4HrStopNodeQueues(Node, Notifications, InterDomain);
    Usb4HrReleaseNode(Node);
}

VOID
Usb4HrTopology::OnRootRouterPoweredOn(
    _In_ USB4HR_HANDLE RouterHandle,
    _In_ NTSTATUS D0Status)
{
    Usb4HrHandleInfo Info;

    if (!NT_SUCCESS(QueryHandle(RouterHandle, &Info)))
    {
        DPRINT1("Root router powered on with unknown handle %p\n", RouterHandle);
        return;
    }

    if (Info.Route.Depth != 0)
    {
        DPRINT1("Router at depth %u reported root router power on\n", Info.Route.Depth);
        return;
    }

    /* Windows releases the waits whatever the D0 entry status was */
    if (!NT_SUCCESS(D0Status))
        DPRINT1("Root router D0 entry failed: 0x%lx\n", D0Status);

    KeSetEvent(&m_RootPoweredOn, IO_NO_INCREMENT, FALSE);
}

VOID Usb4HrTopology::SignalRootRouterPoweredOn()
{
    KeSetEvent(&m_RootPoweredOn, IO_NO_INCREMENT, FALSE);
}

VOID Usb4HrTopology::ResetRootRouterPoweredOn()
{
    KeClearEvent(&m_RootPoweredOn);
}

NTSTATUS
Usb4HrTopology::WaitForRootRouterPoweredOn(
    _In_ ULONG TimeoutMs)
{
    LARGE_INTEGER Timeout = Usb4HrRelativeMs(TimeoutMs);
    NTSTATUS Status;

    Status = KeWaitForSingleObject(&m_RootPoweredOn, Executive, KernelMode, FALSE, &Timeout);
    if (Status == STATUS_TIMEOUT)
    {
        DPRINT1("Root router did not power on within %lu ms\n", TimeoutMs);
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

BOOLEAN
Usb4HrTopology::IsHandleValid(
    _In_ USB4HR_HANDLE Handle)
{
    Usb4HrSpinLockGuard Guard(&m_Lock);

    return FindNodeLocked(Handle) != NULL;
}

NTSTATUS
Usb4HrTopology::QueryHandle(
    _In_ USB4HR_HANDLE Handle,
    _Out_ Usb4HrHandleInfo* Info)
{
    Usb4HrSpinLockGuard Guard(&m_Lock);
    Usb4HrNode* Node = FindNodeLocked(Handle);

    RtlZeroMemory(Info, sizeof(*Info));

    if (Node == NULL)
        return STATUS_INVALID_PARAMETER;

    Info->Route = Node->Route;
    Info->Adapter = Node->Adapter;
    Info->IsRouter = (Node->Adapter == 0);
    Info->RouterType = Node->RouterType;
    Info->Usb4Version = Node->Usb4Version;
    return STATUS_SUCCESS;
}

BOOLEAN Usb4HrTopology::AreDevicesConnected()
{
    Usb4HrSpinLockGuard Guard(&m_Lock);

    /* The host router's own handle is always there */
    return m_RouterCount > 1;
}

NTSTATUS
Usb4HrTopology::PrepareConfigTarget(
    _In_ USB4HR_HANDLE Handle,
    _In_ ULONG Space,
    _Out_ Usb4HrConfigTarget* Target)
{
    Usb4HrSpinLockGuard Guard(&m_Lock);
    Usb4HrNode* Node = FindNodeLocked(Handle);

    RtlZeroMemory(Target, sizeof(*Target));

    if (Node == NULL)
    {
        DPRINT1("Config access with unknown handle %p\n", Handle);
        return STATUS_INVALID_PARAMETER;
    }

    if (!Usb4HrIsSpaceValid(Node, Space))
    {
        DPRINT1("Space %lu is not reachable through handle %p\n", Space, Handle);
        return STATUS_INVALID_PARAMETER;
    }

    Target->Route = Node->Route;
    Target->Adapter = Node->Adapter;
    Target->Sequence = (UCHAR)(InterlockedIncrement(&Node->Sequence) & 3);
    Target->Space = Space;
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrTopology::ReadConfig(
    _In_ USB4HR_HANDLE Handle,
    _In_ ULONG Space,
    _In_ ULONG DwordOffset,
    _In_ ULONG DwordCount,
    _Out_writes_(DwordCount) PULONG Buffer,
    _Out_ PUSB4HR_STATUS Status)
{
    Usb4HrConfigTarget Target;
    NTSTATUS NtStatus;

    *Status = USB4HR_STATUS_FAILURE;

    if (DwordCount == 0 || DwordCount > USB4HR_MAX_CONFIG_DWORDS)
        return STATUS_INVALID_PARAMETER;

    NtStatus = PrepareConfigTarget(Handle, Space, &Target);
    if (!NT_SUCCESS(NtStatus))
        return NtStatus;

    return m_HostRouter->ConfigAccessor()->Read(&Target, DwordOffset, DwordCount, Buffer, Status);
}

NTSTATUS
Usb4HrTopology::WriteConfig(
    _In_ USB4HR_HANDLE Handle,
    _In_ ULONG Space,
    _In_ ULONG DwordOffset,
    _In_ ULONG DwordCount,
    _In_reads_(DwordCount) const ULONG* Buffer,
    _Out_ PUSB4HR_STATUS Status)
{
    Usb4HrConfigTarget Target;
    NTSTATUS NtStatus;

    *Status = USB4HR_STATUS_FAILURE;

    if (DwordCount == 0 || DwordCount > USB4HR_MAX_CONFIG_DWORDS)
        return STATUS_INVALID_PARAMETER;

    NtStatus = PrepareConfigTarget(Handle, Space, &Target);
    if (!NT_SUCCESS(NtStatus))
        return NtStatus;

    return m_HostRouter->ConfigAccessor()->Write(&Target, DwordOffset, DwordCount, Buffer, Status);
}

/* Internal IOCTLs ************************************************************/

VOID
Usb4HrTopology::HandleConfigIoctl(
    _In_ WDFREQUEST Request,
    _In_ Usb4HrConfigKind Kind)
{
    PUSB4HR_CONFIG_INPUT Input;
    Usb4HrConfigTarget Target;
    PVOID Output;
    size_t OutputLength;
    NTSTATUS Status;

    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Input), (PVOID*)&Input, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Config IOCTL input buffer: 0x%lx\n", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    if (Kind == Usb4HrConfigKind::ReadEx)
        OutputLength = sizeof(USB4HR_CONFIG_EX_OUTPUT);
    else
        OutputLength = sizeof(USB4HR_CONFIG_OUTPUT);

    Status = WdfRequestRetrieveOutputBuffer(Request, OutputLength, &Output, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Config IOCTL output buffer: 0x%lx\n", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    if (Input->DwordCount == 0 || Input->DwordCount > USB4HR_MAX_CONFIG_DWORDS)
    {
        DPRINT1("Config IOCTL dword count %lu out of range\n", Input->DwordCount);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    /* Windows uses the data pointer without a check and faults at completion */
    if (Input->Buffer == NULL)
    {
        DPRINT1("Config IOCTL without a data buffer\n");
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    Status = PrepareConfigTarget(Input->Handle, Input->Space, &Target);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestComplete(Request, Status);
        return;
    }

    m_HostRouter->ConfigAccessor()->SubmitIoctl(Request, Kind, &Target, Input);
}

VOID
Usb4HrTopology::OnReadConfig(
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN Extended)
{
    HandleConfigIoctl(Request, Extended ? Usb4HrConfigKind::ReadEx : Usb4HrConfigKind::Read);
}

VOID
Usb4HrTopology::OnWriteConfig(
    _In_ WDFREQUEST Request)
{
    HandleConfigIoctl(Request, Usb4HrConfigKind::Write);
}

VOID
Usb4HrTopology::HandleWaitIoctl(
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN RouterEvent)
{
    PUSB4HR_WAIT_OUTPUT Output;
    PUSB4HR_WAIT_INPUT Input;
    Usb4HrNode* Node;
    NTSTATUS Status;

    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Output), (PVOID*)&Output, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WAIT output buffer: 0x%lx\n", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Input), (PVOID*)&Input, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WAIT input buffer: 0x%lx\n", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    /* Windows parks on whatever the handle points at, valid or not */
    Node = ReferenceNode(Input->Handle);
    if (Node == NULL)
    {
        DPRINT1("WAIT with unknown handle %p\n", Input->Handle);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    if ((Node->Adapter == 0) != (RouterEvent != FALSE))
    {
        DPRINT1("WAIT %s event with handle %p of the other kind\n", RouterEvent ? "router" : "adapter", Node);
        Usb4HrReleaseNode(Node);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    Status = Usb4HrParkWaitRequest(Node, m_HostRouter->Device(), Request);
    Usb4HrReleaseNode(Node);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WAIT on handle %p not parked: 0x%lx\n", Input->Handle, Status);
        WdfRequestComplete(Request, Status);
    }
}

VOID
Usb4HrTopology::OnWaitRouterEvent(
    _In_ WDFREQUEST Request)
{
    HandleWaitIoctl(Request, TRUE);
}

VOID
Usb4HrTopology::OnWaitAdapterEvent(
    _In_ WDFREQUEST Request)
{
    HandleWaitIoctl(Request, FALSE);
}

/* Received packets ***********************************************************/

VOID
NTAPI
Usb4HrTopology::OnNotificationPacket(
    _In_ PVOID Context,
    _In_ const Usb4HrRxPacket* Packet)
{
    Usb4HrTopology* Topology = static_cast<Usb4HrTopology*>(Context);
    ULONG Ack[2];
    ULONG Event;
    NTSTATUS Status;

    if (Packet->DwordCount < USB4HR_PACKET_HEADER_DWORDS)
    {
        DPRINT1("Short notification packet, %lu dwords\n", Packet->DwordCount);
        return;
    }

    Event = Packet->Dword[2] & USB4HR_NOTIFY_EVENT_MASK;
    DPRINT("Notification event %lu, route 0x%08lx%08lx\n",
           Event, Packet->Dword[USB4HR_ROUTE_HIGH], Packet->Dword[USB4HR_ROUTE_LOW]);

    if (Event >= 64 || (USB4HR_NOTIFY_ACK_EVENTS & (1ULL << Event)) == 0)
        return;

    /* Notification acknowledgment: the route string only, CM bit cleared */
    Ack[USB4HR_ROUTE_HIGH] = Packet->Dword[USB4HR_ROUTE_HIGH] & ~USB4HR_ROUTE_CM;
    Ack[USB4HR_ROUTE_LOW] = Packet->Dword[USB4HR_ROUTE_LOW];

    Status = Topology->m_HostRouter->Ring()->Send(USB4HR_PDF_NOTIFICATION_ACK, Ack, RTL_NUMBER_OF(Ack));
    if (!NT_SUCCESS(Status))
        DPRINT1("Notification acknowledgment for event %lu failed: 0x%lx\n", Event, Status);
}

VOID
NTAPI
Usb4HrTopology::OnHotPlugPacket(
    _In_ PVOID Context,
    _In_ const Usb4HrRxPacket* Packet)
{
    Usb4HrTopology* Topology = static_cast<Usb4HrTopology*>(Context);
    Usb4HrPlugPacket* Copy;
    KIRQL OldIrql;

    if (Packet->DwordCount < USB4HR_PACKET_HEADER_DWORDS)
    {
        DPRINT1("Short hot plug packet, %lu dwords\n", Packet->DwordCount);
        return;
    }

    Copy = (Usb4HrPlugPacket*)ExAllocatePoolWithTag(NonPagedPool, sizeof(*Copy), USB4HR_TAG_TOPOLOGY);
    if (Copy == NULL)
    {
        DPRINT1("No memory for a hot plug packet, dropped\n");
        return;
    }

    RtlCopyMemory(Copy->Dword, Packet->Dword, sizeof(Copy->Dword));

    KeAcquireSpinLock(&Topology->m_PlugLock, &OldIrql);
    InsertTailList(&Topology->m_PlugPackets, &Copy->Link);
    KeReleaseSpinLock(&Topology->m_PlugLock, OldIrql);

    WdfWorkItemEnqueue(Topology->m_PlugWorkItem);
}

VOID
NTAPI
Usb4HrTopology::EvtPlugWorkItem(
    _In_ WDFWORKITEM WorkItem)
{
    WDFDEVICE Device = static_cast<WDFDEVICE>(WdfWorkItemGetParentObject(WorkItem));
    Usb4HrTopology* Topology = Usb4HrHostRouter::FromDevice(Device)->Topology();
    Usb4HrPlugPacket* Packet;
    KIRQL OldIrql;

    for (;;)
    {
        KeAcquireSpinLock(&Topology->m_PlugLock, &OldIrql);
        if (IsListEmpty(&Topology->m_PlugPackets))
        {
            KeReleaseSpinLock(&Topology->m_PlugLock, OldIrql);
            return;
        }
        Packet = CONTAINING_RECORD(RemoveHeadList(&Topology->m_PlugPackets), Usb4HrPlugPacket, Link);
        KeReleaseSpinLock(&Topology->m_PlugLock, OldIrql);

        Topology->ProcessPlugPacket(Packet->Dword);
        ExFreePoolWithTag(Packet, USB4HR_TAG_TOPOLOGY);
    }
}

VOID
Usb4HrTopology::ProcessPlugPacket(
    _In_reads_(USB4HR_PACKET_HEADER_DWORDS) const ULONG* Dwords)
{
    ULONG Ack[USB4HR_PACKET_HEADER_DWORDS];
    Usb4HrRoute Route;
    Usb4HrNode* Node;
    BOOLEAN Unplugged;
    UCHAR Adapter;
    ULONG Group;
    NTSTATUS Status;

    Usb4HrRouteFromPacket(Dwords, &Route);
    Adapter = (UCHAR)(Dwords[2] & USB4HR_HOT_PLUG_ADAPTER_MASK);
    Unplugged = (Dwords[2] & USB4HR_HOT_PLUG_UNPLUG) != 0;

    /* Nobody asked about this adapter: no acknowledgment, the router repeats the event */
    Node = ReferenceNodeByRoute(&Route, Adapter);
    if (Node == NULL)
    {
        DPRINT("Hot plug for depth %u adapter %u without a handle\n", Route.Depth, Adapter);
        return;
    }

    DPRINT("Hot %s on depth %u adapter %u\n", Unplugged ? "unplug" : "plug", Route.Depth, Adapter);

    /* Hot plug acknowledgment: a notification packet with event HP_ACK */
    Group = Unplugged ? USB4HR_NOTIFY_PG_UNPLUG : USB4HR_NOTIFY_PG_PLUG;
    Ack[USB4HR_ROUTE_HIGH] = Dwords[USB4HR_ROUTE_HIGH] & ~USB4HR_ROUTE_CM;
    Ack[USB4HR_ROUTE_LOW] = Dwords[USB4HR_ROUTE_LOW];
    Ack[2] = USB4HR_STATUS_HP_ACK |
             ((ULONG)Adapter << USB4HR_NOTIFY_ADAPTER_SHIFT) |
             (Group << USB4HR_NOTIFY_PG_SHIFT);

    Status = m_HostRouter->Ring()->Send(USB4HR_PDF_NOTIFICATION, Ack, RTL_NUMBER_OF(Ack));
    if (!NT_SUCCESS(Status))
        DPRINT1("Hot plug acknowledgment failed: 0x%lx\n", Status);

    /* A stopped queue means the router is going away; the event is not wanted */
    Status = Usb4HrQueueNodeEvent(Node, Unplugged);
    if (!NT_SUCCESS(Status) && Status != STATUS_INVALID_DEVICE_STATE)
        DPRINT1("Hot plug event not queued: 0x%lx\n", Status);

    Usb4HrReleaseNode(Node);
}

/* Hardware services interface slots ******************************************/

NTSTATUS
NTAPI
Usb4HrServiceAllocateRouterHandle(
    _In_ PVOID Context,
    _In_ PUSB4HR_TOPOLOGY_ID TopologyId,
    _Out_ PUSB4HR_HANDLE RouterHandle)
{
    if (Context == NULL || TopologyId == NULL || RouterHandle == NULL)
    {
        DPRINT1("AllocateRouterHandle with a NULL argument\n");
        return STATUS_INVALID_PARAMETER;
    }

    return static_cast<Usb4HrTopology*>(Context)->AllocateRouterHandle(TopologyId, RouterHandle);
}

NTSTATUS
NTAPI
Usb4HrServiceDestroyRouterHandle(
    _In_ PVOID Context,
    _In_ USB4HR_HANDLE RouterHandle)
{
    Usb4HrTopology* Topology = static_cast<Usb4HrTopology*>(Context);

    if (Context == NULL || RouterHandle == NULL)
    {
        DPRINT1("DestroyRouterHandle with a NULL argument\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* Tunnel teardown needs PASSIVE_LEVEL; at DISPATCH_LEVEL only the handle goes */
    if (KeGetCurrentIrql() > PASSIVE_LEVEL)
    {
        DPRINT1("DestroyRouterHandle above PASSIVE_LEVEL, tunnels left alone\n");
        return Topology->DestroyAdapterHandle(RouterHandle);
    }

    return Topology->DestroyRouterHandle(RouterHandle);
}

NTSTATUS
NTAPI
Usb4HrServiceAllocateAdapterHandles(
    _In_ PVOID Context,
    _In_ USB4HR_HANDLE RouterHandle,
    _In_ UCHAR Count,
    _Out_writes_(Count) PUSB4HR_HANDLE AdapterHandles)
{
    if (Context == NULL || RouterHandle == NULL || AdapterHandles == NULL)
    {
        DPRINT1("AllocateAdapterHandles with a NULL argument\n");
        return STATUS_INVALID_PARAMETER;
    }

    return static_cast<Usb4HrTopology*>(Context)->AllocateAdapterHandles(RouterHandle, Count, AdapterHandles);
}

NTSTATUS
NTAPI
Usb4HrServiceDestroyAdapterHandle(
    _In_ PVOID Context,
    _In_ USB4HR_HANDLE AdapterHandle)
{
    if (Context == NULL || AdapterHandle == NULL)
    {
        DPRINT1("DestroyAdapterHandle with a NULL argument\n");
        return STATUS_INVALID_PARAMETER;
    }

    return static_cast<Usb4HrTopology*>(Context)->DestroyAdapterHandle(AdapterHandle);
}

NTSTATUS
NTAPI
Usb4HrServiceSetRouterType(
    _In_ PVOID Context,
    _In_ USB4HR_HANDLE RouterHandle,
    _In_ USB4HR_ROUTER_TYPE Type,
    _In_ ULONG Usb4Version)
{
    if (Context == NULL || RouterHandle == NULL)
    {
        DPRINT1("SetRouterType with a NULL argument\n");
        return STATUS_INVALID_PARAMETER;
    }

    return static_cast<Usb4HrTopology*>(Context)->SetRouterType(RouterHandle, Type, Usb4Version);
}

VOID
NTAPI
Usb4HrServicePurgeEventQueues(
    _In_ PVOID Context,
    _In_ USB4HR_HANDLE Handle,
    _In_ BOOLEAN PurgeNotifications,
    _In_ BOOLEAN PurgeInterDomain)
{
    if (Context == NULL || Handle == NULL)
    {
        DPRINT1("PurgeEventQueues with a NULL argument\n");
        return;
    }

    static_cast<Usb4HrTopology*>(Context)->PurgeEventQueues(Handle, PurgeNotifications, PurgeInterDomain);
}

VOID
NTAPI
Usb4HrServiceRootRouterPoweredOn(
    _In_ PVOID Context,
    _In_ USB4HR_HANDLE RouterHandle,
    _In_ NTSTATUS D0Status)
{
    if (Context == NULL || RouterHandle == NULL)
    {
        DPRINT1("RootRouterPoweredOn with a NULL argument\n");
        return;
    }

    static_cast<Usb4HrTopology*>(Context)->OnRootRouterPoweredOn(RouterHandle, D0Status);
}

NTSTATUS
NTAPI
Usb4HrServiceNotifyUfpLinkBandwidth(
    _In_ PVOID Context,
    _In_ USB4HR_HANDLE RouterHandle,
    _In_ USHORT LinkBandwidthGbps,
    _In_ UCHAR LinkWidth,
    _In_ BOOLEAN Asymmetric)
{
    if (Context == NULL || !static_cast<Usb4HrTopology*>(Context)->IsHandleValid(RouterHandle))
    {
        DPRINT1("NotifyUfpLinkBandwidth with unknown handle %p\n", RouterHandle);
        return STATUS_INVALID_PARAMETER;
    }

    /* No link bandwidth bookkeeping this round; USB3 tunnels size from the link rate alone */
    DPRINT1("NotifyUfpLinkBandwidth %p: %u Gbps, width %u, asymmetric %u ignored\n",
            RouterHandle, LinkBandwidthGbps, LinkWidth, Asymmetric);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
Usb4HrServiceQueryDpBandwidth(
    _In_ PVOID Context,
    _In_ USB4HR_HANDLE TunnelHandle,
    _Out_ PUSHORT AllocatedBandwidth,
    _Out_ PULONG Detail,
    _Out_ PBOOLEAN Flag0,
    _Out_ PBOOLEAN Flag1)
{
    UNREFERENCED_PARAMETER(Context);

    if (AllocatedBandwidth != NULL)
        *AllocatedBandwidth = 0;
    if (Detail != NULL)
        *Detail = 0;
    if (Flag0 != NULL)
        *Flag0 = FALSE;
    if (Flag1 != NULL)
        *Flag1 = FALSE;

    /* No DisplayPort tunnels exist; Windows would fault on the missing record */
    DPRINT1("QueryDpBandwidth for %p: no DisplayPort tunnels\n", TunnelHandle);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
Usb4HrServiceApplyDpRedriverWorkaround(
    _In_ PVOID Context,
    _In_ UCHAR DpInAdapter)
{
    UNREFERENCED_PARAMETER(Context);

    DPRINT1("DP redriver workaround for DP IN adapter %u not kept\n", DpInAdapter);
    return STATUS_SUCCESS;
}

VOID
NTAPI
Usb4HrServiceClearDpRedriverWorkaround(
    _In_ PVOID Context,
    _In_ UCHAR DpInAdapter)
{
    UNREFERENCED_PARAMETER(Context);

    DPRINT1("DP redriver workaround clear for DP IN adapter %u\n", DpInAdapter);
}
