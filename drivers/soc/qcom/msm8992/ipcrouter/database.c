/*
 * PROJECT:     ReactOS Qualcomm IPC Router driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Node, port and server registry
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ipcrouter.h"

#define QCDBG_COMPONENT "qcomipcrouter"
#include <qcom/qcomdbg.h>

typedef struct _IPC_NODE
{
    LIST_ENTRY Link;
    ULONG Node;
    ULONG PortCount;
    LIST_ENTRY Ports[IPC_PORT_BUCKETS];
} IPC_NODE, *PIPC_NODE;

typedef struct _IPC_SERVER
{
    LIST_ENTRY Link;
    IPC_ADDR Name;
    IPC_ADDR Addr;
} IPC_SERVER, *PIPC_SERVER;

static LIST_ENTRY IpcNodeBuckets[IPC_NODE_BUCKETS];
static FAST_MUTEX IpcNodeLock;

static LIST_ENTRY IpcServerBuckets[IPC_SERVER_BUCKETS];
static FAST_MUTEX IpcServerLock;

VOID
IpcDbInitialize(VOID)
{
    ULONG i;

    ExInitializeFastMutex(&IpcNodeLock);
    ExInitializeFastMutex(&IpcServerLock);

    for (i = 0; i < IPC_NODE_BUCKETS; i++)
        InitializeListHead(&IpcNodeBuckets[i]);
    for (i = 0; i < IPC_SERVER_BUCKETS; i++)
        InitializeListHead(&IpcServerBuckets[i]);
}

static
PIPC_NODE
IpcNodeFind(
    _In_ ULONG Node)
{
    PLIST_ENTRY Entry;
    PIPC_NODE Found;

    Entry = IpcNodeBuckets[Node & (IPC_NODE_BUCKETS - 1)].Flink;
    while (Entry != &IpcNodeBuckets[Node & (IPC_NODE_BUCKETS - 1)])
    {
        Found = CONTAINING_RECORD(Entry, IPC_NODE, Link);
        if (Found->Node == Node)
            return Found;
        Entry = Entry->Flink;
    }

    return NULL;
}

static
PIPC_NODE
IpcNodeGet(
    _In_ ULONG Node)
{
    PIPC_NODE New;
    ULONG i;

    New = IpcNodeFind(Node);
    if (New)
        return New;

    New = ExAllocatePoolWithTag(IPC_POOL, sizeof(*New), IPC_TAG);
    if (!New)
        return NULL;

    New->Node = Node;
    New->PortCount = 0;
    for (i = 0; i < IPC_PORT_BUCKETS; i++)
        InitializeListHead(&New->Ports[i]);

    InsertHeadList(&IpcNodeBuckets[Node & (IPC_NODE_BUCKETS - 1)], &New->Link);
    return New;
}

static
VOID
IpcPortDestroy(
    _In_ PIPC_PORT Port)
{
    /* The owner is torn down, wake anything blocked on it before freeing */
    KeSetEvent(&Port->TxEvent, 0, FALSE);
    KeSetEvent(&Port->RxEvent, 0, FALSE);

    /* Lets the owner drop its own reference once no callback can run again */
    if (Port->OnDestroy)
        Port->OnDestroy(Port->OnDestroyContext);

    ExFreePoolWithTag(Port, IPC_TAG);
}

VOID
IpcPortSetDestroyHook(
    _In_ PIPC_PORT Port,
    _In_ VOID (NTAPI *OnDestroy)(PVOID Context),
    _In_opt_ PVOID Context)
{
    Port->OnDestroy = OnDestroy;
    Port->OnDestroyContext = Context;
}

PIPC_PORT
IpcPortFind(
    _In_ PIPC_ADDR Addr,
    _In_ BOOLEAN Reference)
{
    PIPC_NODE Node;
    PLIST_ENTRY Head, Entry;
    PIPC_PORT Port = NULL;

    ExAcquireFastMutex(&IpcNodeLock);

    Node = IpcNodeFind(Addr->Node);
    if (Node)
    {
        Head = &Node->Ports[Addr->Port & (IPC_PORT_BUCKETS - 1)];
        for (Entry = Head->Flink; Entry != Head; Entry = Entry->Flink)
        {
            Port = CONTAINING_RECORD(Entry, IPC_PORT, BucketLink);
            if (Port->Addr.Port == Addr->Port)
            {
                if (Reference)
                    InterlockedIncrement(&Port->RefCount);
                break;
            }
            Port = NULL;
        }
    }

    ExReleaseFastMutex(&IpcNodeLock);
    return Port;
}

ULONG
IpcPortInsert(
    _In_ PIPC_ADDR Addr,
    _In_opt_ PIPC_XPORT Xport,
    _In_ PIPC_PORT_RX Rx,
    _In_opt_ PVOID RxContext,
    _In_opt_ PIPC_PORT_EVENT Event,
    _In_opt_ PVOID EventContext,
    _In_ ULONG Priority,
    _Outptr_opt_ PIPC_PORT *Out)
{
    PIPC_NODE Node;
    PLIST_ENTRY Head, Entry;
    PIPC_PORT Port;
    ULONG Status = IPC_ENOMEM;

    ExAcquireFastMutex(&IpcNodeLock);

    Node = IpcNodeGet(Addr->Node);
    if (!Node)
        goto Exit;

    Head = &Node->Ports[Addr->Port & (IPC_PORT_BUCKETS - 1)];
    for (Entry = Head->Flink; Entry != Head; Entry = Entry->Flink)
    {
        Port = CONTAINING_RECORD(Entry, IPC_PORT, BucketLink);
        if (Port->Addr.Port == Addr->Port)
        {
            /* A remote announcement with a lower priority replaces the route */
            Status = IPC_EBUSY;
            if (Xport && Port->Xport && Priority < Port->Priority)
            {
                Port->Xport = Xport;
                Port->Priority = Priority;
                Status = IPC_OK;
            }
            if (Out)
            {
                InterlockedIncrement(&Port->RefCount);
                *Out = Port;
            }
            goto Exit;
        }
    }

    Port = ExAllocatePoolWithTag(IPC_POOL, sizeof(*Port), IPC_TAG);
    if (!Port)
        goto Exit;

    RtlZeroMemory(Port, sizeof(*Port));
    Port->Addr = *Addr;
    Port->Xport = Xport;
    Port->Priority = Priority ? Priority : IPC_PRIORITY_DEFAULT;
    Port->RefCount = 1;
    /* Flow window is tracked on the destination, shared by all its senders */
    Port->TxQuota = IPC_TX_WINDOW_DEFAULT;
    Port->Rx = Rx;
    Port->RxContext = RxContext;
    Port->Event = Event;
    Port->EventContext = EventContext;
    ExInitializeFastMutex(&Port->Lock);
    KeInitializeEvent(&Port->TxEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&Port->RxEvent, NotificationEvent, FALSE);

    InsertHeadList(Head, &Port->BucketLink);
    Node->PortCount++;

    if (Out)
    {
        InterlockedIncrement(&Port->RefCount);
        *Out = Port;
    }
    Status = IPC_OK;

Exit:
    ExReleaseFastMutex(&IpcNodeLock);
    return Status;
}

VOID
IpcPortRelease(
    _In_opt_ PIPC_PORT Port)
{
    if (!Port)
        return;

    if (InterlockedDecrement(&Port->RefCount) == 0)
        IpcPortDestroy(Port);
}

VOID
IpcPortRemove(
    _In_ PIPC_ADDR Addr)
{
    PIPC_NODE Node;
    PLIST_ENTRY Head, Entry;
    PIPC_PORT Port = NULL;

    ExAcquireFastMutex(&IpcNodeLock);

    Node = IpcNodeFind(Addr->Node);
    if (Node)
    {
        Head = &Node->Ports[Addr->Port & (IPC_PORT_BUCKETS - 1)];
        for (Entry = Head->Flink; Entry != Head; Entry = Entry->Flink)
        {
            Port = CONTAINING_RECORD(Entry, IPC_PORT, BucketLink);
            if (Port->Addr.Port == Addr->Port)
            {
                RemoveEntryList(&Port->BucketLink);
                Node->PortCount--;
                break;
            }
            Port = NULL;
        }

        if (Node->PortCount == 0)
        {
            RemoveEntryList(&Node->Link);
            ExFreePoolWithTag(Node, IPC_TAG);
        }
    }

    ExReleaseFastMutex(&IpcNodeLock);

    /* Drop the creation reference outside the table lock */
    IpcPortRelease(Port);
}

VOID
IpcPortRemoveByXport(
    _In_ PIPC_XPORT Xport)
{
    ULONG b, p;
    PLIST_ENTRY NodeHead, NodeEntry, NodeNext;
    PLIST_ENTRY Head, Entry, Next;
    PIPC_NODE Node;
    PIPC_PORT Port;
    LIST_ENTRY Dead;

    InitializeListHead(&Dead);
    ExAcquireFastMutex(&IpcNodeLock);

    for (b = 0; b < IPC_NODE_BUCKETS; b++)
    {
        NodeHead = &IpcNodeBuckets[b];
        for (NodeEntry = NodeHead->Flink; NodeEntry != NodeHead; NodeEntry = NodeNext)
        {
            NodeNext = NodeEntry->Flink;
            Node = CONTAINING_RECORD(NodeEntry, IPC_NODE, Link);

            for (p = 0; p < IPC_PORT_BUCKETS; p++)
            {
                Head = &Node->Ports[p];
                for (Entry = Head->Flink; Entry != Head; Entry = Next)
                {
                    Next = Entry->Flink;
                    Port = CONTAINING_RECORD(Entry, IPC_PORT, BucketLink);
                    if (Port->Xport == Xport)
                    {
                        RemoveEntryList(&Port->BucketLink);
                        Node->PortCount--;
                        InsertTailList(&Dead, &Port->BucketLink);
                    }
                }
            }

            if (Node->PortCount == 0)
            {
                RemoveEntryList(&Node->Link);
                ExFreePoolWithTag(Node, IPC_TAG);
            }
        }
    }

    ExReleaseFastMutex(&IpcNodeLock);

    while (!IsListEmpty(&Dead))
    {
        Entry = RemoveHeadList(&Dead);
        Port = CONTAINING_RECORD(Entry, IPC_PORT, BucketLink);
        IpcPortRelease(Port);
    }
}

ULONG
IpcServerAdd(
    _In_ PIPC_ADDR Name,
    _In_ PIPC_ADDR Addr)
{
    PLIST_ENTRY Head, Entry;
    PIPC_SERVER Server;
    ULONG Status = IPC_ENOMEM;

    ExAcquireFastMutex(&IpcServerLock);

    Head = &IpcServerBuckets[Name->Node & (IPC_SERVER_BUCKETS - 1)];
    for (Entry = Head->Flink; Entry != Head; Entry = Entry->Flink)
    {
        Server = CONTAINING_RECORD(Entry, IPC_SERVER, Link);
        if (Server->Name.Node == Name->Node && Server->Name.Port == Name->Port &&
            Server->Addr.Node == Addr->Node && Server->Addr.Port == Addr->Port)
        {
            ExReleaseFastMutex(&IpcServerLock);
            return IPC_EEXIST;
        }
    }

    Server = ExAllocatePoolWithTag(IPC_POOL, sizeof(*Server), IPC_TAG);
    if (Server)
    {
        Server->Name = *Name;
        Server->Addr = *Addr;
        InsertHeadList(Head, &Server->Link);
        Status = IPC_OK;
    }

    ExReleaseFastMutex(&IpcServerLock);
    return Status;
}

ULONG
IpcServerRemove(
    _In_ PIPC_ADDR Name,
    _In_ PIPC_ADDR Addr)
{
    PLIST_ENTRY Head, Entry;
    PIPC_SERVER Server;
    ULONG Status = 1;

    ExAcquireFastMutex(&IpcServerLock);

    Head = &IpcServerBuckets[Name->Node & (IPC_SERVER_BUCKETS - 1)];
    for (Entry = Head->Flink; Entry != Head; Entry = Entry->Flink)
    {
        Server = CONTAINING_RECORD(Entry, IPC_SERVER, Link);
        if (Server->Name.Node == Name->Node && Server->Name.Port == Name->Port &&
            Server->Addr.Node == Addr->Node && Server->Addr.Port == Addr->Port)
        {
            RemoveEntryList(&Server->Link);
            ExFreePoolWithTag(Server, IPC_TAG);
            Status = IPC_OK;
            break;
        }
    }

    ExReleaseFastMutex(&IpcServerLock);
    return Status;
}

PIPC_PORT
IpcServerLookup(
    _In_ PIPC_ADDR Name)
{
    PLIST_ENTRY Head, Entry;
    PIPC_SERVER Server;
    IPC_ADDR Addr;
    BOOLEAN Found = FALSE;

    ExAcquireFastMutex(&IpcServerLock);

    Head = &IpcServerBuckets[Name->Node & (IPC_SERVER_BUCKETS - 1)];
    for (Entry = Head->Flink; Entry != Head; Entry = Entry->Flink)
    {
        Server = CONTAINING_RECORD(Entry, IPC_SERVER, Link);
        if (Server->Name.Node == Name->Node && Server->Name.Port == Name->Port)
        {
            Addr = Server->Addr;
            Found = TRUE;
            break;
        }
    }

    ExReleaseFastMutex(&IpcServerLock);

    if (!Found)
        return NULL;

    return IpcPortFind(&Addr, TRUE);
}

ULONG
IpcServerCollectLocal(
    _In_ PIPC_ADDR Name,
    _In_ ULONG LocalNode,
    _Out_writes_to_(Max, return) PIPC_ADDR Addrs,
    _In_ ULONG Max)
{
    PLIST_ENTRY Head, Entry;
    PIPC_SERVER Server;
    ULONG Count = 0;

    ExAcquireFastMutex(&IpcServerLock);

    Head = &IpcServerBuckets[Name->Node & (IPC_SERVER_BUCKETS - 1)];
    for (Entry = Head->Flink; Entry != Head && Count < Max; Entry = Entry->Flink)
    {
        Server = CONTAINING_RECORD(Entry, IPC_SERVER, Link);
        if (Server->Name.Node == Name->Node && Server->Name.Port == Name->Port &&
            Server->Addr.Node == LocalNode)
        {
            Addrs[Count++] = Server->Addr;
        }
    }

    ExReleaseFastMutex(&IpcServerLock);
    return Count;
}

VOID
IpcBroadcastEvent(
    _In_ ULONG LocalNode,
    _In_ ULONG Event,
    _In_ PIPC_ADDR Peer)
{
    PIPC_NODE Node;
    PLIST_ENTRY Head, Entry;
    PIPC_PORT Port;
    PIPC_PORT *Snapshot = NULL;
    ULONG Capacity = 0, Count = 0, p, i;

    ExAcquireFastMutex(&IpcNodeLock);

    Node = IpcNodeFind(LocalNode);
    if (Node && Node->PortCount)
    {
        Capacity = Node->PortCount;
        Snapshot = ExAllocatePoolWithTag(IPC_POOL, Capacity * sizeof(PIPC_PORT), IPC_TAG);
    }

    if (Snapshot)
    {
        for (p = 0; p < IPC_PORT_BUCKETS && Count < Capacity; p++)
        {
            Head = &Node->Ports[p];
            for (Entry = Head->Flink; Entry != Head && Count < Capacity; Entry = Entry->Flink)
            {
                Port = CONTAINING_RECORD(Entry, IPC_PORT, BucketLink);
                if (Port->Event)
                {
                    InterlockedIncrement(&Port->RefCount);
                    Snapshot[Count++] = Port;
                }
            }
        }
    }

    ExReleaseFastMutex(&IpcNodeLock);

    if (!Snapshot)
        return;

    /* Deliver outside the table lock, the hook allocates and takes its own lock */
    for (i = 0; i < Count; i++)
    {
        Snapshot[i]->Event(Snapshot[i], Snapshot[i]->EventContext, Event, Peer);
        IpcPortRelease(Snapshot[i]);
    }

    ExFreePoolWithTag(Snapshot, IPC_TAG);
}

VOID
IpcServerRemoveByNode(
    _In_ ULONG Node)
{
    ULONG b;
    PLIST_ENTRY Head, Entry, Next;
    PIPC_SERVER Server;

    ExAcquireFastMutex(&IpcServerLock);

    for (b = 0; b < IPC_SERVER_BUCKETS; b++)
    {
        Head = &IpcServerBuckets[b];
        for (Entry = Head->Flink; Entry != Head; Entry = Next)
        {
            Next = Entry->Flink;
            Server = CONTAINING_RECORD(Entry, IPC_SERVER, Link);
            if (Server->Addr.Node == Node)
            {
                RemoveEntryList(&Server->Link);
                ExFreePoolWithTag(Server, IPC_TAG);
            }
        }
    }

    ExReleaseFastMutex(&IpcServerLock);
}

VOID
IpcDbTeardown(VOID)
{
    ULONG b, p;
    PLIST_ENTRY Head, Entry;
    PIPC_NODE Node;
    PIPC_PORT Port;
    PIPC_SERVER Server;

    ExAcquireFastMutex(&IpcNodeLock);
    for (b = 0; b < IPC_NODE_BUCKETS; b++)
    {
        while (!IsListEmpty(&IpcNodeBuckets[b]))
        {
            Entry = RemoveHeadList(&IpcNodeBuckets[b]);
            Node = CONTAINING_RECORD(Entry, IPC_NODE, Link);
            for (p = 0; p < IPC_PORT_BUCKETS; p++)
            {
                Head = &Node->Ports[p];
                while (!IsListEmpty(Head))
                {
                    Port = CONTAINING_RECORD(RemoveHeadList(Head), IPC_PORT, BucketLink);
                    IpcPortDestroy(Port);
                }
            }
            ExFreePoolWithTag(Node, IPC_TAG);
        }
    }
    ExReleaseFastMutex(&IpcNodeLock);

    ExAcquireFastMutex(&IpcServerLock);
    for (b = 0; b < IPC_SERVER_BUCKETS; b++)
    {
        while (!IsListEmpty(&IpcServerBuckets[b]))
        {
            Server = CONTAINING_RECORD(RemoveHeadList(&IpcServerBuckets[b]), IPC_SERVER, Link);
            ExFreePoolWithTag(Server, IPC_TAG);
        }
    }
    ExReleaseFastMutex(&IpcServerLock);
}
