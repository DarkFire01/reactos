/*
 * PROJECT:     ReactOS Network Programming Interface
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel IP Helper: unicast addresses, interface rows and their change notification
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntifs.h>
#include <ndis.h>
#include <ws2def.h>
#include <ws2ipdef.h>
#include <netioapi.h>
#include <tdiinfo.h>
#include <tcpioctl.h>

#define NDEBUG
#include <reactos/debug.h>

#define TAG_NETIO_MIB       'BMIN'
#define NETIO_ADDRESS_LIMIT 64
#define NETIO_POLL_MS       2000
#define NETIO_IFTYPE_LOOPBACK   24
#define NETIO_IFTYPE_WIFI       71
#define NETIO_IF_DESCR_MAX      256

/* IFEntry.if_operstatus from the stack, connected and operational count as up */
#define NETIO_OPER_CONNECTED    4

typedef struct _NETIO_IF_SNAPSHOT
{
    IFEntry Entry;
    UCHAR Description[NETIO_IF_DESCR_MAX];
} NETIO_IF_SNAPSHOT, *PNETIO_IF_SNAPSHOT;

typedef struct _NETIO_ADDRESS_NOTIFY
{
    ADDRESS_FAMILY Family;
    PUNICAST_IPADDRESS_CHANGE_CALLBACK Callback;
    PVOID CallerContext;
    KTIMER Timer;
    KDPC Dpc;
    WORK_QUEUE_ITEM WorkItem;
    KEVENT Idle;
    volatile LONG Queued;
    BOOLEAN Cancelled;
    BOOLEAN InitialPending;
    ULONG KnownCount;
    IPAddrEntry *Known;
} NETIO_ADDRESS_NOTIFY, *PNETIO_ADDRESS_NOTIFY;

static
NTSTATUS
NetiopOpenTcp(
    _Out_ PHANDLE Handle)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Device\\Tcp");
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;

    InitializeObjectAttributes(&Attributes, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwCreateFile(Handle,
                        GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                        &Attributes,
                        &IoStatus,
                        NULL,
                        FILE_ATTRIBUTE_NORMAL,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_OPEN_IF,
                        FILE_SYNCHRONOUS_IO_NONALERT,
                        NULL,
                        0);
}

static
NTSTATUS
NetiopQueryTcp(
    _In_ HANDLE Tcp,
    _In_ ULONG Class,
    _In_ ULONG Id,
    _In_ ULONG Entity,
    _In_ ULONG Instance,
    _Out_writes_bytes_(Size) PVOID Buffer,
    _In_ ULONG Size,
    _Out_ PULONG Returned)
{
    TCP_REQUEST_QUERY_INFORMATION_EX Request;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;

    RtlZeroMemory(&Request, sizeof(Request));
    Request.ID.toi_class = Class;
    Request.ID.toi_type = INFO_TYPE_PROVIDER;
    Request.ID.toi_id = Id;
    Request.ID.toi_entity.tei_entity = Entity;
    Request.ID.toi_entity.tei_instance = Instance;

    Status = ZwDeviceIoControlFile(Tcp,
                                   NULL,
                                   NULL,
                                   NULL,
                                   &IoStatus,
                                   IOCTL_TCP_QUERY_INFORMATION_EX,
                                   &Request,
                                   sizeof(Request),
                                   Buffer,
                                   Size);
    *Returned = NT_SUCCESS(Status) ? (ULONG)IoStatus.Information : 0;
    return Status;
}

/**
 * @brief Collects the IPv4 addresses and the interfaces the stack reports.
 * Either output may be NULL when the caller does not want it.
 */
static
NTSTATUS
NetiopSnapshot(
    _Outptr_opt_result_maybenull_ IPAddrEntry **Addresses,
    _Out_opt_ PULONG AddressCount,
    _Outptr_opt_result_maybenull_ PNETIO_IF_SNAPSHOT *Interfaces,
    _Out_opt_ PULONG InterfaceCount)
{
    IPAddrEntry *AddressList = NULL;
    PNETIO_IF_SNAPSHOT InterfaceList = NULL;
    TDIEntityID *Entities;
    ULONG Entity, Count, Returned;
    ULONG Addressed = 0, Interfaced = 0;
    HANDLE Tcp;
    NTSTATUS Status;

    if (Addresses)
        *Addresses = NULL;
    if (Interfaces)
        *Interfaces = NULL;

    Status = NetiopOpenTcp(&Tcp);
    if (!NT_SUCCESS(Status))
        return Status;

    Entities = ExAllocatePoolWithTag(PagedPool, MAX_TDI_ENTITIES * sizeof(*Entities), TAG_NETIO_MIB);
    if (Addresses)
        AddressList = ExAllocatePoolZero(PagedPool, NETIO_ADDRESS_LIMIT * sizeof(*AddressList), TAG_NETIO_MIB);
    if (Interfaces)
        InterfaceList = ExAllocatePoolZero(PagedPool, NETIO_ADDRESS_LIMIT * sizeof(*InterfaceList), TAG_NETIO_MIB);

    if (!Entities || (Addresses && !AddressList) || (Interfaces && !InterfaceList))
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    Status = NetiopQueryTcp(Tcp, INFO_CLASS_GENERIC, ENTITY_LIST_ID, GENERIC_ENTITY, 0,
                            Entities, MAX_TDI_ENTITIES * sizeof(*Entities), &Returned);
    if (!NT_SUCCESS(Status))
        goto Done;

    Count = Returned / sizeof(*Entities);
    for (Entity = 0; Entity < Count; Entity++)
    {
        if (AddressList && Entities[Entity].tei_entity == CL_NL_ENTITY &&
            Addressed < NETIO_ADDRESS_LIMIT)
        {
            if (NT_SUCCESS(NetiopQueryTcp(Tcp, INFO_CLASS_PROTOCOL, IP_MIB_ADDRTABLE_ENTRY_ID,
                                          CL_NL_ENTITY, Entities[Entity].tei_instance,
                                          &AddressList[Addressed],
                                          (NETIO_ADDRESS_LIMIT - Addressed) * sizeof(*AddressList),
                                          &Returned)))
            {
                Addressed += Returned / sizeof(*AddressList);
            }
        }
        else if (InterfaceList && Entities[Entity].tei_entity == IF_ENTITY &&
                 Interfaced < NETIO_ADDRESS_LIMIT)
        {
            if (NT_SUCCESS(NetiopQueryTcp(Tcp, INFO_CLASS_PROTOCOL, IF_MIB_STATS_ID,
                                          IF_ENTITY, Entities[Entity].tei_instance,
                                          &InterfaceList[Interfaced],
                                          sizeof(*InterfaceList),
                                          &Returned)) &&
                Returned >= FIELD_OFFSET(IFEntry, if_descr))
            {
                Interfaced++;
            }
        }
    }

    Status = STATUS_SUCCESS;

Done:
    ZwClose(Tcp);
    if (Entities)
        ExFreePoolWithTag(Entities, TAG_NETIO_MIB);

    if (!NT_SUCCESS(Status))
    {
        if (AddressList)
            ExFreePoolWithTag(AddressList, TAG_NETIO_MIB);
        if (InterfaceList)
            ExFreePoolWithTag(InterfaceList, TAG_NETIO_MIB);
        return Status;
    }

    if (Addresses)
    {
        *Addresses = AddressList;
        *AddressCount = Addressed;
    }
    if (Interfaces)
    {
        *Interfaces = InterfaceList;
        *InterfaceCount = Interfaced;
    }
    return STATUS_SUCCESS;
}

static
NET_LUID
NetiopLuid(
    _In_ ULONG Index,
    _In_ ULONG Type)
{
    NET_LUID Luid;

    Luid.Value = 0;
    Luid.Info.NetLuidIndex = Index;
    Luid.Info.IfType = (USHORT)Type;
    return Luid;
}

static
ULONG
NetiopInterfaceType(
    _In_reads_(Count) PNETIO_IF_SNAPSHOT Interfaces,
    _In_ ULONG Count,
    _In_ ULONG Index)
{
    ULONG Interface;

    for (Interface = 0; Interface < Count; Interface++)
    {
        if (Interfaces[Interface].Entry.if_index == Index)
            return Interfaces[Interface].Entry.if_type;
    }

    return 0;
}

static
UINT8
NetiopPrefixLength(
    _In_ ULONG Mask)
{
    UINT8 Length = 0;

    while (Mask)
    {
        Length += (UINT8)(Mask & 1);
        Mask >>= 1;
    }

    return Length;
}

static
VOID
NetiopFillAddressRow(
    _Out_ PMIB_UNICASTIPADDRESS_ROW Row,
    _In_ IPAddrEntry *Address,
    _In_ ULONG Type)
{
    BOOLEAN Loopback = ((Address->iae_addr & 0xFF) == 127);

    RtlZeroMemory(Row, sizeof(*Row));
    Row->Address.si_family = AF_INET;
    Row->Address.Ipv4.sin_family = AF_INET;
    Row->Address.Ipv4.sin_addr.s_addr = Address->iae_addr;
    Row->InterfaceIndex = Address->iae_index;
    Row->InterfaceLuid = NetiopLuid(Address->iae_index, Type);
    Row->PrefixOrigin = Loopback ? IpPrefixOriginWellKnown : IpPrefixOriginOther;
    Row->SuffixOrigin = Loopback ? IpSuffixOriginWellKnown : IpSuffixOriginOther;
    Row->ValidLifetime = MAXULONG;
    Row->PreferredLifetime = MAXULONG;
    Row->OnLinkPrefixLength = NetiopPrefixLength(Address->iae_mask);
    Row->DadState = IpDadStatePreferred;
}

static
BOOLEAN
NetiopFamilyHasIpv4(
    _In_ ADDRESS_FAMILY Family)
{
    return (Family == AF_UNSPEC) || (Family == AF_INET);
}

static
ULONG
NetiopUsableAddresses(
    _Inout_updates_(Count) IPAddrEntry *Addresses,
    _In_ ULONG Count)
{
    ULONG Index, Kept = 0;

    /* An interface still waiting for an address reports 0.0.0.0 */
    for (Index = 0; Index < Count; Index++)
    {
        if (Addresses[Index].iae_addr != 0)
            Addresses[Kept++] = Addresses[Index];
    }

    return Kept;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NETIOAPI_API
GetUnicastIpAddressTable(
    _In_ ADDRESS_FAMILY Family,
    _Outptr_ PMIB_UNICASTIPADDRESS_TABLE *Table)
{
    PMIB_UNICASTIPADDRESS_TABLE Result;
    PNETIO_IF_SNAPSHOT Interfaces = NULL;
    IPAddrEntry *Addresses = NULL;
    ULONG AddressCount = 0, InterfaceCount = 0, Index;
    NTSTATUS Status;

    if (!Table || ((Family != AF_UNSPEC) && (Family != AF_INET) && (Family != AF_INET6)))
        return STATUS_INVALID_PARAMETER;

    *Table = NULL;

    if (NetiopFamilyHasIpv4(Family))
    {
        Status = NetiopSnapshot(&Addresses, &AddressCount, &Interfaces, &InterfaceCount);
        if (!NT_SUCCESS(Status))
            return Status;

        AddressCount = NetiopUsableAddresses(Addresses, AddressCount);
    }

    Result = ExAllocatePoolZero(NonPagedPoolNx,
                                FIELD_OFFSET(MIB_UNICASTIPADDRESS_TABLE, Table) +
                                max(AddressCount, 1) * sizeof(MIB_UNICASTIPADDRESS_ROW),
                                TAG_NETIO_MIB);
    if (!Result)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    for (Index = 0; Index < AddressCount; Index++)
    {
        NetiopFillAddressRow(&Result->Table[Index],
                             &Addresses[Index],
                             NetiopInterfaceType(Interfaces, InterfaceCount, Addresses[Index].iae_index));
    }

    Result->NumEntries = AddressCount;
    *Table = Result;
    Status = STATUS_SUCCESS;

Done:
    if (Addresses)
        ExFreePoolWithTag(Addresses, TAG_NETIO_MIB);
    if (Interfaces)
        ExFreePoolWithTag(Interfaces, TAG_NETIO_MIB);
    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NETIOAPI_API
GetUnicastIpAddressEntry(
    _Inout_ PMIB_UNICASTIPADDRESS_ROW Row)
{
    PMIB_UNICASTIPADDRESS_TABLE Table;
    ULONG Index;
    NTSTATUS Status;

    if (!Row)
        return STATUS_INVALID_PARAMETER;

    if ((Row->InterfaceLuid.Value == 0) && (Row->InterfaceIndex == 0))
        return STATUS_INVALID_PARAMETER;

    if (Row->Address.si_family != AF_INET)
        return (Row->Address.si_family == AF_INET6) ? STATUS_NOT_FOUND : STATUS_INVALID_PARAMETER;

    Status = GetUnicastIpAddressTable(AF_INET, &Table);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = STATUS_NOT_FOUND;
    for (Index = 0; Index < Table->NumEntries; Index++)
    {
        PMIB_UNICASTIPADDRESS_ROW Candidate = &Table->Table[Index];

        if (Candidate->Address.Ipv4.sin_addr.s_addr != Row->Address.Ipv4.sin_addr.s_addr)
            continue;

        /* The LUID names the interface when it is given, the index otherwise */
        if (Row->InterfaceLuid.Value != 0)
        {
            if (Candidate->InterfaceLuid.Info.NetLuidIndex != Row->InterfaceLuid.Info.NetLuidIndex)
                continue;
        }
        else if (Candidate->InterfaceIndex != Row->InterfaceIndex)
        {
            continue;
        }

        *Row = *Candidate;
        Status = STATUS_SUCCESS;
        break;
    }

    ExFreePoolWithTag(Table, TAG_NETIO_MIB);
    return Status;
}

VOID
NETIOAPI_API_
FreeMibTable(
    _In_ PVOID Memory)
{
    if (Memory)
        ExFreePoolWithTag(Memory, TAG_NETIO_MIB);
}

static
VOID
NetiopFillInterfaceRow(
    _Inout_ PMIB_IF_ROW2 Row,
    _In_ PNETIO_IF_SNAPSHOT Interface)
{
    IFEntry *Entry = &Interface->Entry;
    BOOLEAN Loopback = (Entry->if_type == NETIO_IFTYPE_LOOPBACK);
    BOOLEAN Up = (Entry->if_operstatus >= NETIO_OPER_CONNECTED);
    ULONG Length, Index;

    RtlZeroMemory(Row, sizeof(*Row));
    Row->InterfaceIndex = Entry->if_index;
    Row->InterfaceLuid = NetiopLuid(Entry->if_index, Entry->if_type);

    Length = min(Entry->if_descrlen, IF_MAX_STRING_SIZE);
    Length = min(Length, NETIO_IF_DESCR_MAX - 1);
    for (Index = 0; Index < Length && Entry->if_descr[Index] != ANSI_NULL; Index++)
    {
        Row->Description[Index] = Entry->if_descr[Index];
        Row->Alias[Index] = Entry->if_descr[Index];
    }

    Length = min(Entry->if_physaddrlen, MAX_PHYSADDR_SIZE);
    Row->PhysicalAddressLength = Length;
    RtlCopyMemory(Row->PhysicalAddress, Entry->if_physaddr, Length);
    RtlCopyMemory(Row->PermanentPhysicalAddress, Entry->if_physaddr, Length);

    Row->Mtu = Entry->if_mtu;
    Row->Type = Entry->if_type;
    Row->TunnelType = TUNNEL_TYPE_NONE;
    Row->MediaType = Loopback ? NdisMediumLoopback : NdisMedium802_3;
    Row->PhysicalMediumType = (Entry->if_type == NETIO_IFTYPE_WIFI) ?
                              NdisPhysicalMediumNative802_11 : NdisPhysicalMediumUnspecified;
    Row->AccessType = Loopback ? NET_IF_ACCESS_LOOPBACK : NET_IF_ACCESS_BROADCAST;
    Row->DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    Row->InterfaceAndOperStatusFlags.HardwareInterface = !Loopback;
    Row->InterfaceAndOperStatusFlags.ConnectorPresent = !Loopback;
    Row->InterfaceAndOperStatusFlags.NotMediaConnected = !Up;
    Row->OperStatus = Up ? IfOperStatusUp : IfOperStatusDown;
    Row->AdminStatus = (Entry->if_adminstatus == 1) ? NET_IF_ADMIN_STATUS_UP : NET_IF_ADMIN_STATUS_DOWN;
    Row->MediaConnectState = Up ? MediaConnectStateConnected : MediaConnectStateDisconnected;
    Row->ConnectionType = NET_IF_CONNECTION_DEDICATED;
    Row->TransmitLinkSpeed = Entry->if_speed;
    Row->ReceiveLinkSpeed = Entry->if_speed;
    Row->InOctets = Entry->if_inoctets;
    Row->InUcastPkts = Entry->if_inucastpkts;
    Row->InNUcastPkts = Entry->if_innucastpkts;
    Row->InDiscards = Entry->if_indiscards;
    Row->InErrors = Entry->if_inerrors;
    Row->InUnknownProtos = Entry->if_inunknownprotos;
    Row->OutOctets = Entry->if_outoctets;
    Row->OutUcastPkts = Entry->if_outucastpkts;
    Row->OutNUcastPkts = Entry->if_outnucastpkts;
    Row->OutDiscards = Entry->if_outdiscards;
    Row->OutErrors = Entry->if_outerrors;
    Row->OutQLen = Entry->if_outqlen;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NETIOAPI_API
GetIfEntry2(
    _Inout_ PMIB_IF_ROW2 Row)
{
    PNETIO_IF_SNAPSHOT Interfaces;
    ULONG Count, Index;
    NTSTATUS Status;

    if (!Row || ((Row->InterfaceLuid.Value == 0) && (Row->InterfaceIndex == 0)))
        return STATUS_INVALID_PARAMETER;

    Status = NetiopSnapshot(NULL, NULL, &Interfaces, &Count);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = STATUS_NOT_FOUND;
    for (Index = 0; Index < Count; Index++)
    {
        ULONG IfIndex = Interfaces[Index].Entry.if_index;

        if ((Row->InterfaceLuid.Value != 0) ?
            (Row->InterfaceLuid.Info.NetLuidIndex == IfIndex) :
            (Row->InterfaceIndex == IfIndex))
        {
            NetiopFillInterfaceRow(Row, &Interfaces[Index]);
            Status = STATUS_SUCCESS;
            break;
        }
    }

    ExFreePoolWithTag(Interfaces, TAG_NETIO_MIB);
    return Status;
}

/**
 * @brief Reads one interface's row, with or without its traffic statistics.
 *
 * @param[in] Level
 * MibIfEntryNormal for the whole row, MibIfEntryNormalWithoutStatistics to leave the counters zero.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
NETIOAPI_API
GetIfEntry2Ex(
    _In_ MIB_IF_ENTRY_LEVEL Level,
    _Inout_ PMIB_IF_ROW2 Row)
{
    NTSTATUS Status;

    if (Level != MibIfEntryNormal && Level != MibIfEntryNormalWithoutStatistics)
        return STATUS_INVALID_PARAMETER;

    Status = GetIfEntry2(Row);
    if (!NT_SUCCESS(Status) || Level == MibIfEntryNormal)
        return Status;

    Row->InOctets = Row->InUcastPkts = Row->InNUcastPkts = Row->InDiscards = 0;
    Row->InErrors = Row->InUnknownProtos = Row->InUcastOctets = Row->InMulticastOctets = 0;
    Row->InBroadcastOctets = Row->OutOctets = Row->OutUcastPkts = Row->OutNUcastPkts = 0;
    Row->OutDiscards = Row->OutErrors = Row->OutUcastOctets = Row->OutMulticastOctets = 0;
    Row->OutBroadcastOctets = Row->OutQLen = 0;
    return STATUS_SUCCESS;
}

static
BOOLEAN
NetiopAddressListed(
    _In_reads_(Count) IPAddrEntry *List,
    _In_ ULONG Count,
    _In_ IPAddrEntry *Address)
{
    ULONG Index;

    for (Index = 0; Index < Count; Index++)
    {
        if ((List[Index].iae_addr == Address->iae_addr) &&
            (List[Index].iae_index == Address->iae_index))
        {
            return TRUE;
        }
    }

    return FALSE;
}

static
VOID
NetiopReportChanges(
    _In_ PNETIO_ADDRESS_NOTIFY Notify,
    _In_reads_(FromCount) IPAddrEntry *From,
    _In_ ULONG FromCount,
    _In_reads_(ToCount) IPAddrEntry *To,
    _In_ ULONG ToCount,
    _In_ MIB_NOTIFICATION_TYPE Type)
{
    MIB_UNICASTIPADDRESS_ROW Row;
    ULONG Index;

    for (Index = 0; Index < FromCount && !Notify->Cancelled; Index++)
    {
        if (NetiopAddressListed(To, ToCount, &From[Index]))
            continue;

        NetiopFillAddressRow(&Row, &From[Index], 0);
        Notify->Callback(Notify->CallerContext, &Row, Type);
    }
}

static
VOID
NTAPI
NetiopAddressWorker(
    _In_ PVOID Context)
{
    PNETIO_ADDRESS_NOTIFY Notify = Context;
    IPAddrEntry *Current;
    ULONG Count;

    if (Notify->InitialPending && !Notify->Cancelled)
    {
        Notify->InitialPending = FALSE;
        Notify->Callback(Notify->CallerContext, NULL, MibInitialNotification);
    }

    if (!Notify->Cancelled && NT_SUCCESS(NetiopSnapshot(&Current, &Count, NULL, NULL)))
    {
        Count = NetiopUsableAddresses(Current, Count);

        /* Arrivals are the current ones the last pass lacked, departures the reverse */
        NetiopReportChanges(Notify, Current, Count, Notify->Known, Notify->KnownCount, MibAddInstance);
        NetiopReportChanges(Notify, Notify->Known, Notify->KnownCount, Current, Count, MibDeleteInstance);

        if (Notify->Known)
            ExFreePoolWithTag(Notify->Known, TAG_NETIO_MIB);
        Notify->Known = Current;
        Notify->KnownCount = Count;
    }

    InterlockedExchange(&Notify->Queued, 0);
    KeSetEvent(&Notify->Idle, IO_NO_INCREMENT, FALSE);
}

static
VOID
NTAPI
NetiopAddressPoll(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PNETIO_ADDRESS_NOTIFY Notify = Context;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    /* One pass at a time, a slow one just skips the next tick */
    if (Notify->Cancelled || InterlockedCompareExchange(&Notify->Queued, 1, 0) != 0)
        return;

    KeClearEvent(&Notify->Idle);
    ExQueueWorkItem(&Notify->WorkItem, DelayedWorkQueue);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NETIOAPI_API
NotifyUnicastIpAddressChange(
    _In_ ADDRESS_FAMILY Family,
    _In_ PUNICAST_IPADDRESS_CHANGE_CALLBACK Callback,
    _In_opt_ PVOID CallerContext,
    _In_ BOOLEAN InitialNotification,
    _Inout_ HANDLE *NotificationHandle)
{
    PNETIO_ADDRESS_NOTIFY Notify;
    LARGE_INTEGER DueTime;

    if (!Callback || !NotificationHandle ||
        ((Family != AF_UNSPEC) && (Family != AF_INET) && (Family != AF_INET6)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Notify = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Notify), TAG_NETIO_MIB);
    if (!Notify)
        return STATUS_INSUFFICIENT_RESOURCES;

    Notify->Family = Family;
    Notify->Callback = Callback;
    Notify->CallerContext = CallerContext;
    Notify->InitialPending = InitialNotification;
    KeInitializeEvent(&Notify->Idle, NotificationEvent, TRUE);
    KeInitializeTimerEx(&Notify->Timer, NotificationTimer);
    KeInitializeDpc(&Notify->Dpc, NetiopAddressPoll, Notify);
    ExInitializeWorkItem(&Notify->WorkItem, NetiopAddressWorker, Notify);

    /* What exists now is the baseline, only later changes are reported */
    if (NetiopFamilyHasIpv4(Family) &&
        NT_SUCCESS(NetiopSnapshot(&Notify->Known, &Notify->KnownCount, NULL, NULL)))
    {
        Notify->KnownCount = NetiopUsableAddresses(Notify->Known, Notify->KnownCount);
    }

    *NotificationHandle = Notify;

    /* IPv6 has no addresses here, so only the initial notification can come */
    if (InitialNotification)
    {
        InterlockedExchange(&Notify->Queued, 1);
        KeClearEvent(&Notify->Idle);
        ExQueueWorkItem(&Notify->WorkItem, DelayedWorkQueue);
    }

    if (NetiopFamilyHasIpv4(Family))
    {
        DueTime.QuadPart = -10000LL * NETIO_POLL_MS;
        KeSetTimerEx(&Notify->Timer, DueTime, NETIO_POLL_MS, &Notify->Dpc);
    }

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NETIOAPI_API
CancelMibChangeNotify2(
    _In_ HANDLE NotificationHandle)
{
    PNETIO_ADDRESS_NOTIFY Notify = NotificationHandle;

    if (!Notify)
        return STATUS_INVALID_PARAMETER;

    Notify->Cancelled = TRUE;
    KeCancelTimer(&Notify->Timer);
    KeFlushQueuedDpcs();

    /* A pass already running finishes before the memory goes */
    KeWaitForSingleObject(&Notify->Idle, Executive, KernelMode, FALSE, NULL);

    if (Notify->Known)
        ExFreePoolWithTag(Notify->Known, TAG_NETIO_MIB);
    ExFreePoolWithTag(Notify, TAG_NETIO_MIB);
    return STATUS_SUCCESS;
}
