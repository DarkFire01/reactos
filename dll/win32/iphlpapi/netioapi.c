/*
 * PROJECT:     ReactOS IP Helper API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     The Vista network I/O helper interface (netioapi.h)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * These are the interface, route and address APIs that arrived with Vista and
 * replaced the MIB_*ROW/MIB_*TABLE ones. They answer from the same place the
 * old APIs do - there is no netio layer here - so what they add over
 * GetIfTable(), GetIpForwardTable() and GetBestRoute() is the shape of the
 * answer, not new information.
 *
 * That shape is the whole point, and it is why this file exists rather than
 * the code that was here before. MIB_IPINTERFACE_ROW, MIB_IPFORWARD_ROW2 and
 * PIPFORWARD_CHANGE_CALLBACK had all been redeclared locally, with layouts
 * invented to be convenient - MIB_IPFORWARD_ROW2 was a copy of the v1
 * MIB_IPFORWARDROW, dwForwardDest and all - and the functions filled those.
 * A caller compiled against the real netioapi.h reads InterfaceLuid where we
 * wrote dwForwardDest. GetIpForwardTable2() and NotifyRouteChange2() were
 * already exported that way, so anything that used them was already being
 * handed nonsense; the structures here now come from <netioapi.h> only.
 *
 * Where a field has no source on this system it is left zeroed rather than
 * guessed at. Zero is a defined value for every one of them, and a caller that
 * reads it gets "not set" instead of a fabricated number.
 */

#include "iphlpapi_private.h"
#include <netioapi.h>

WINE_DEFAULT_DEBUG_CHANNEL(iphlpapi);

/* ALLOCATION ****************************************************************/

/*
 * Every *Table2 function hands back a block the caller releases with
 * FreeMibTable(), so both ends of that contract live here and use the process
 * heap. Nothing else in iphlpapi may free one of these.
 */
static
PVOID
IpHlpAllocMibTable(_In_ SIZE_T Size)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
}

/*
 * @implemented
 */
VOID
WINAPI
FreeMibTable(
    _In_opt_ PVOID Memory)
{
    TRACE("FreeMibTable(%p)\n", Memory);

    /* Windows accepts NULL here, and callers rely on it in cleanup paths */
    if (Memory != NULL)
        HeapFree(GetProcessHeap(), 0, Memory);
}

/* INTERFACES ****************************************************************/

/*
 * @implemented
 */
DWORD
WINAPI
GetIpInterfaceTable(
    _In_ ADDRESS_FAMILY Family,
    _Outptr_ PMIB_IPINTERFACE_TABLE *Table)
{
    InterfaceIndexTable *IndexTable;
    PMIB_IPINTERFACE_TABLE OutTable;
    SIZE_T Size;
    DWORD i;

    TRACE("GetIpInterfaceTable(%u, %p)\n", Family, Table);

    if (Table == NULL)
        return ERROR_INVALID_PARAMETER;

    if (Family != AF_UNSPEC && Family != AF_INET)
        return ERROR_NOT_SUPPORTED;

    IndexTable = getInterfaceIndexTable();
    if (IndexTable == NULL)
        return ERROR_OUTOFMEMORY;

    /*
     * The table is variable length, and the caller frees it, so it is sized to
     * what was found rather than to a fixed maximum. ANY_SIZE already accounts
     * for the first row.
     */
    Size = FIELD_OFFSET(MIB_IPINTERFACE_TABLE, Table) +
           (SIZE_T)IndexTable->numIndexes * sizeof(MIB_IPINTERFACE_ROW);

    OutTable = IpHlpAllocMibTable(Size);
    if (OutTable == NULL)
    {
        free(IndexTable);
        return ERROR_OUTOFMEMORY;
    }

    for (i = 0; i < IndexTable->numIndexes; i++)
    {
        PMIB_IPINTERFACE_ROW Row = &OutTable->Table[OutTable->NumEntries];
        NET_IFINDEX Index = IndexTable->indexes[i];
        MIB_IFROW IfRow;

        ZeroMemory(&IfRow, sizeof(IfRow));
        IfRow.dwIndex = Index;
        if (GetIfEntry(&IfRow) != NO_ERROR)
            continue;

        Row->Family = AF_INET;
        Row->InterfaceIndex = Index;
        ConvertInterfaceIndexToLuid(Index, &Row->InterfaceLuid);

        Row->NlMtu = IfRow.dwMtu;
        Row->Connected = (IfRow.dwOperStatus == MIB_IF_OPER_STATUS_OPERATIONAL) ||
                         (IfRow.dwOperStatus == MIB_IF_OPER_STATUS_CONNECTED);

        /*
         * We do not compute metrics, so say so rather than reporting a made up
         * one: with UseAutomaticMetric set a caller knows Metric is not its
         * own choice, and zero is what an unconfigured interface reports.
         */
        Row->UseAutomaticMetric = TRUE;
        Row->Metric = 0;

        OutTable->NumEntries++;
    }

    free(IndexTable);

    *Table = OutTable;
    return NO_ERROR;
}

/* ROUTES ********************************************************************/

/*
 * Fills a v2 route row from the v1 row the stack gives us.
 *
 * The v1 row carries an IPv4 destination, mask and next hop; the v2 row wants
 * a prefix length and SOCKADDR_INETs, so the mask is counted into a prefix
 * length here rather than stored.
 */
static
VOID
IpHlpConvertRouteRow(
    _In_ const MIB_IPFORWARDROW *Old,
    _Out_ PMIB_IPFORWARD_ROW2 New)
{
    ULONG Mask;
    UINT8 PrefixLength;

    ZeroMemory(New, sizeof(*New));

    New->InterfaceIndex = Old->dwForwardIfIndex;
    ConvertInterfaceIndexToLuid(Old->dwForwardIfIndex, &New->InterfaceLuid);

    New->DestinationPrefix.Prefix.si_family = AF_INET;
    New->DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
    New->DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr = Old->dwForwardDest;

    /* Contiguous masks only, which is all the v1 table can express anyway */
    for (PrefixLength = 0, Mask = ntohl(Old->dwForwardMask);
         Mask & 0x80000000;
         Mask <<= 1)
    {
        PrefixLength++;
    }
    New->DestinationPrefix.PrefixLength = PrefixLength;

    New->NextHop.si_family = AF_INET;
    New->NextHop.Ipv4.sin_family = AF_INET;
    New->NextHop.Ipv4.sin_addr.S_un.S_addr = Old->dwForwardNextHop;

    New->Metric = Old->dwForwardMetric1;
    New->Age = Old->dwForwardAge;
    New->Protocol = (NL_ROUTE_PROTOCOL)Old->dwForwardProto;

    /*
     * A route whose next hop is 0.0.0.0 goes out of the interface directly,
     * which is what Loopback means here for an interface of that type.
     */
    New->Loopback = FALSE;
    New->Origin = NlroManual;

    /* Lifetimes are not tracked; infinite is the value for "does not expire" */
    New->ValidLifetime = 0xFFFFFFFF;
    New->PreferredLifetime = 0xFFFFFFFF;
}

/*
 * @implemented
 */
DWORD
WINAPI
GetIpForwardTable2(
    _In_ ADDRESS_FAMILY Family,
    _Outptr_ PMIB_IPFORWARD_TABLE2 *Table)
{
    PMIB_IPFORWARDTABLE OldTable = NULL;
    PMIB_IPFORWARD_TABLE2 OutTable;
    ULONG Size = 0;
    SIZE_T OutSize;
    DWORD Error, i;

    TRACE("GetIpForwardTable2(%u, %p)\n", Family, Table);

    if (Table == NULL)
        return ERROR_INVALID_PARAMETER;

    if (Family != AF_UNSPEC && Family != AF_INET)
        return ERROR_NOT_SUPPORTED;

    Error = GetIpForwardTable(NULL, &Size, FALSE);
    if (Error != ERROR_INSUFFICIENT_BUFFER)
        return Error;

    OldTable = HeapAlloc(GetProcessHeap(), 0, Size);
    if (OldTable == NULL)
        return ERROR_OUTOFMEMORY;

    Error = GetIpForwardTable(OldTable, &Size, FALSE);
    if (Error != NO_ERROR)
    {
        HeapFree(GetProcessHeap(), 0, OldTable);
        return Error;
    }

    OutSize = FIELD_OFFSET(MIB_IPFORWARD_TABLE2, Table) +
              (SIZE_T)OldTable->dwNumEntries * sizeof(MIB_IPFORWARD_ROW2);

    OutTable = IpHlpAllocMibTable(OutSize);
    if (OutTable == NULL)
    {
        HeapFree(GetProcessHeap(), 0, OldTable);
        return ERROR_OUTOFMEMORY;
    }

    for (i = 0; i < OldTable->dwNumEntries; i++)
    {
        IpHlpConvertRouteRow(&OldTable->table[i], &OutTable->Table[i]);
    }
    OutTable->NumEntries = OldTable->dwNumEntries;

    HeapFree(GetProcessHeap(), 0, OldTable);

    *Table = OutTable;
    return NO_ERROR;
}

/*
 * @implemented
 */
DWORD
WINAPI
GetBestRoute2(
    _In_opt_ NET_LUID *InterfaceLuid,
    _In_ NET_IFINDEX InterfaceIndex,
    _In_opt_ const SOCKADDR_INET *SourceAddress,
    _In_ const SOCKADDR_INET *DestinationAddress,
    _In_ ULONG AddressSortOptions,
    _Out_ PMIB_IPFORWARD_ROW2 BestRoute,
    _Out_opt_ SOCKADDR_INET *BestSourceAddress)
{
    MIB_IPFORWARDROW OldRoute;
    PMIB_IPADDRTABLE AddrTable;
    ULONG Size = 0;
    DWORD Error, i;
    DWORD Source = 0;

    TRACE("GetBestRoute2(%p, %u, %p, %p, %lu, %p, %p)\n",
          InterfaceLuid, InterfaceIndex, SourceAddress, DestinationAddress,
          AddressSortOptions, BestRoute, BestSourceAddress);

    UNREFERENCED_PARAMETER(AddressSortOptions);

    if (DestinationAddress == NULL || BestRoute == NULL)
        return ERROR_INVALID_PARAMETER;

    if (DestinationAddress->si_family != AF_INET)
    {
        FIXME("Only IPv4 is supported\n");
        return ERROR_NOT_SUPPORTED;
    }

    /*
     * The interface may be named by LUID or by index, and a caller may give
     * neither. Resolve to an index, which is what the v1 lookup understands,
     * but do not fail on a LUID we cannot resolve - the route lookup below
     * does not need it.
     */
    if (InterfaceIndex == 0 && InterfaceLuid != NULL)
        ConvertInterfaceLuidToIndex(InterfaceLuid, &InterfaceIndex);

    if (SourceAddress != NULL && SourceAddress->si_family == AF_INET)
        Source = SourceAddress->Ipv4.sin_addr.S_un.S_addr;

    Error = GetBestRoute(DestinationAddress->Ipv4.sin_addr.S_un.S_addr,
                         Source,
                         &OldRoute);
    if (Error != NO_ERROR)
        return Error;

    IpHlpConvertRouteRow(&OldRoute, BestRoute);

    if (BestSourceAddress == NULL)
        return NO_ERROR;

    /*
     * The best source address is an address of the interface the route leaves
     * by. Failing to find one is not a failure of the route lookup, so the
     * route is still returned - but the address is left as an unspecified
     * AF_INET address rather than as whatever the caller's stack held.
     */
    ZeroMemory(BestSourceAddress, sizeof(*BestSourceAddress));
    BestSourceAddress->si_family = AF_INET;
    BestSourceAddress->Ipv4.sin_family = AF_INET;

    if (GetIpAddrTable(NULL, &Size, FALSE) != ERROR_INSUFFICIENT_BUFFER)
        return NO_ERROR;

    AddrTable = HeapAlloc(GetProcessHeap(), 0, Size);
    if (AddrTable == NULL)
        return ERROR_OUTOFMEMORY;

    if (GetIpAddrTable(AddrTable, &Size, FALSE) == NO_ERROR)
    {
        for (i = 0; i < AddrTable->dwNumEntries; i++)
        {
            if (AddrTable->table[i].dwIndex == OldRoute.dwForwardIfIndex)
            {
                BestSourceAddress->Ipv4.sin_addr.S_un.S_addr =
                    AddrTable->table[i].dwAddr;
                break;
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, AddrTable);
    return NO_ERROR;
}

/* CHANGE NOTIFICATION *******************************************************/

/*
 * A registration made by one of the Notify*Change2 functions.
 *
 * Windows hands back an opaque HANDLE that CancelMibChangeNotify2() takes,
 * and the same cancel serves all three kinds, so one structure covers them
 * and carries the kind with it.
 *
 * There is no source of change events here, so a registration records the
 * callback and does nothing further. That is deliberate: the alternative
 * these functions replaced was to raise EXCEPTION_WINE_STUB, which kills a
 * caller that expected to be told about interfaces coming and going, when
 * what it actually needs is to be told nothing has changed. A caller that
 * asks for the initial notification still gets it, because that one is real -
 * it is the current state, which we can answer.
 */
typedef enum _IPHLP_NOTIFY_KIND
{
    IpHlpNotifyIpInterface,
    IpHlpNotifyUnicastIpAddress,
    IpHlpNotifyRoute
} IPHLP_NOTIFY_KIND;

typedef struct _IPHLP_NOTIFICATION
{
    IPHLP_NOTIFY_KIND Kind;
    ADDRESS_FAMILY Family;
    PVOID Callback;
    PVOID CallerContext;
} IPHLP_NOTIFICATION, *PIPHLP_NOTIFICATION;

static
DWORD
IpHlpRegisterNotification(
    _In_ IPHLP_NOTIFY_KIND Kind,
    _In_ ADDRESS_FAMILY Family,
    _In_ PVOID Callback,
    _In_opt_ PVOID CallerContext,
    _Out_ HANDLE *NotificationHandle)
{
    PIPHLP_NOTIFICATION Notification;

    if (Callback == NULL || NotificationHandle == NULL)
        return ERROR_INVALID_PARAMETER;

    if (Family != AF_UNSPEC && Family != AF_INET && Family != AF_INET6)
        return ERROR_INVALID_PARAMETER;

    Notification = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                             sizeof(*Notification));
    if (Notification == NULL)
        return ERROR_OUTOFMEMORY;

    Notification->Kind = Kind;
    Notification->Family = Family;
    Notification->Callback = Callback;
    Notification->CallerContext = CallerContext;

    *NotificationHandle = Notification;
    return NO_ERROR;
}

/*
 * @implemented
 */
DWORD
WINAPI
NotifyIpInterfaceChange(
    _In_ ADDRESS_FAMILY Family,
    _In_ PIPINTERFACE_CHANGE_CALLBACK Callback,
    _In_opt_ PVOID CallerContext,
    _In_ BOOLEAN InitialNotification,
    _Inout_ HANDLE *NotificationHandle)
{
    DWORD Error;

    TRACE("NotifyIpInterfaceChange(%u, %p, %p, %d, %p)\n",
          Family, Callback, CallerContext, InitialNotification,
          NotificationHandle);

    Error = IpHlpRegisterNotification(IpHlpNotifyIpInterface,
                                      Family,
                                      Callback,
                                      CallerContext,
                                      NotificationHandle);
    if (Error != NO_ERROR)
        return Error;

    if (InitialNotification)
    {
        /*
         * Windows delivers one MibInitialNotification with a NULL row, not one
         * per interface: the row is only meaningful for a real change.
         */
        Callback(CallerContext, NULL, MibInitialNotification);
    }

    return NO_ERROR;
}

/*
 * @implemented
 */
DWORD
WINAPI
NotifyUnicastIpAddressChange(
    _In_ ADDRESS_FAMILY Family,
    _In_ PUNICAST_IPADDRESS_CHANGE_CALLBACK Callback,
    _In_opt_ PVOID CallerContext,
    _In_ BOOLEAN InitialNotification,
    _Inout_ HANDLE *NotificationHandle)
{
    DWORD Error;

    TRACE("NotifyUnicastIpAddressChange(%u, %p, %p, %d, %p)\n",
          Family, Callback, CallerContext, InitialNotification,
          NotificationHandle);

    Error = IpHlpRegisterNotification(IpHlpNotifyUnicastIpAddress,
                                      Family,
                                      Callback,
                                      CallerContext,
                                      NotificationHandle);
    if (Error != NO_ERROR)
        return Error;

    if (InitialNotification)
        Callback(CallerContext, NULL, MibInitialNotification);

    return NO_ERROR;
}

/*
 * @implemented
 */
DWORD
WINAPI
NotifyRouteChange2(
    _In_ ADDRESS_FAMILY Family,
    _In_ PIPFORWARD_CHANGE_CALLBACK Callback,
    _In_opt_ PVOID CallerContext,
    _In_ BOOLEAN InitialNotification,
    _Inout_ HANDLE *NotificationHandle)
{
    DWORD Error;

    TRACE("NotifyRouteChange2(%u, %p, %p, %d, %p)\n",
          Family, Callback, CallerContext, InitialNotification,
          NotificationHandle);

    Error = IpHlpRegisterNotification(IpHlpNotifyRoute,
                                      Family,
                                      Callback,
                                      CallerContext,
                                      NotificationHandle);
    if (Error != NO_ERROR)
        return Error;

    if (InitialNotification)
        Callback(CallerContext, NULL, MibInitialNotification);

    return NO_ERROR;
}

/*
 * @implemented
 */
DWORD
WINAPI
CancelMibChangeNotify2(
    _In_ HANDLE NotificationHandle)
{
    TRACE("CancelMibChangeNotify2(%p)\n", NotificationHandle);

    if (NotificationHandle == NULL)
        return ERROR_INVALID_PARAMETER;

    HeapFree(GetProcessHeap(), 0, NotificationHandle);
    return NO_ERROR;
}

/* BSD INTERFACE NAMES *******************************************************/

/*
 * @implemented
 */
NET_IFINDEX
WINAPI
if_nametoindex(
    _In_ PCSTR InterfaceName)
{
    NET_LUID Luid;
    NET_IFINDEX Index;

    TRACE("if_nametoindex(%s)\n", debugstr_a(InterfaceName));

    if (InterfaceName == NULL)
        return 0;

    /* 0 is "no such interface", which is the only failure this API can report */
    if (ConvertInterfaceNameToLuidA(InterfaceName, &Luid) != NO_ERROR)
        return 0;

    if (ConvertInterfaceLuidToIndex(&Luid, &Index) != NO_ERROR)
        return 0;

    return Index;
}

/*
 * @implemented
 */
PCHAR
WINAPI
if_indextoname(
    _In_ NET_IFINDEX InterfaceIndex,
    _Out_writes_(IF_NAMESIZE) PCHAR InterfaceName)
{
    NET_LUID Luid;

    TRACE("if_indextoname(%u, %p)\n", InterfaceIndex, InterfaceName);

    if (InterfaceName == NULL)
        return NULL;

    if (ConvertInterfaceIndexToLuid(InterfaceIndex, &Luid) != NO_ERROR)
        return NULL;

    if (ConvertInterfaceLuidToNameA(&Luid, InterfaceName, IF_NAMESIZE) != NO_ERROR)
        return NULL;

    return InterfaceName;
}

/* EOF */
