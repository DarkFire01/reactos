/*
 * PROJECT:     ReactOS Settings
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Network page, the adapters with their state and IPv4 settings
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "settings.h"
#include <winsvc.h>
#include <winioctl.h>
#include <iphlpapi.h>
#include <dhcpcsdk.h>
#include <dhcpcapi.h>
#include <time.h>
#include <telephony/rmnetioctl.h>

#define ST_NET_MAX_ADAPTERS         16
#define ST_NET_ADDRESS_MAX          16
#define ST_NET_DNS_MAX              64

/* NetCfgInstanceId of the cellular data adapter, rmnet_reg.inf gives it this one */
static const CHAR StNetCellularId[] = "{ACD88133-6664-4857-8360-EB884C4CC07A}";

typedef enum _ST_NET_VIEW
{
    StNetViewList,
    StNetViewDetail,
    StNetViewEdit
} ST_NET_VIEW;

typedef enum _ST_NET_OPERATION
{
    StNetRenew,
    StNetRelease,
    StNetApply
} ST_NET_OPERATION;

typedef struct _ST_NET_ADAPTER
{
    CHAR Id[MAX_ADAPTER_NAME_LENGTH + 4];
    WCHAR Name[128];
    WCHAR Description[MAX_ADAPTER_DESCRIPTION_LENGTH + 4];
    DWORD Index;
    UINT Type;
    BYTE Mac[MAX_ADAPTER_ADDRESS_LENGTH];
    UINT MacLength;
    WCHAR Address[ST_NET_ADDRESS_MAX];
    WCHAR Mask[ST_NET_ADDRESS_MAX];
    WCHAR Gateway[ST_NET_ADDRESS_MAX];
    WCHAR DhcpServer[ST_NET_ADDRESS_MAX];
    WCHAR Dns[ST_NET_DNS_MAX];
    WCHAR StaticDns[ST_NET_DNS_MAX];
    DWORD AddressContext;
    BOOL Dhcp;
    time_t LeaseExpires;
    BOOL HaveCounters;
    DWORD OperStatus;
    DWORD Speed;
    DWORD Mtu;
    DWORD InOctets;
    DWORD OutOctets;
    BOOL Cellular;
    BOOL HaveRmNet;
    RMNET_QUERY RmNet;
} ST_NET_ADAPTER, *PST_NET_ADAPTER;

/* One request for the network worker, posted back once the stack has it */
typedef struct _ST_NET_JOB
{
    ST_NET_OPERATION Operation;
    ST_NET_ADAPTER Adapter;
    BOOL Dhcp;
    ULONG Address;
    ULONG Mask;
    ULONG Gateway;
    WCHAR Dns[ST_NET_DNS_MAX];
    DWORD Error;
} ST_NET_JOB, *PST_NET_JOB;

const UINT StNetControls[] =
{
    IDC_NET_LIST, IDC_NET_RENEW, IDC_NET_RELEASE, IDC_NET_CONFIGURE, IDC_NET_BACK,
    IDC_NET_MODE, IDC_NET_ADDRESS_LABEL, IDC_NET_ADDRESS, IDC_NET_MASK_LABEL, IDC_NET_MASK,
    IDC_NET_GATEWAY_LABEL, IDC_NET_GATEWAY, IDC_NET_DNS_LABEL, IDC_NET_DNS, IDC_NET_SAVE
};
const DWORD StNetControlCount = _countof(StNetControls);

static const UINT StNetDetailControls[] =
{
    IDC_NET_RENEW, IDC_NET_RELEASE, IDC_NET_CONFIGURE, IDC_NET_BACK
};

static const UINT StNetStaticControls[] =
{
    IDC_NET_ADDRESS_LABEL, IDC_NET_ADDRESS, IDC_NET_MASK_LABEL, IDC_NET_MASK,
    IDC_NET_GATEWAY_LABEL, IDC_NET_GATEWAY
};

static ST_NET_ADAPTER StNetAdapters[ST_NET_MAX_ADAPTERS];
static DWORD StNetCount;
static ST_NET_VIEW StNetView = StNetViewList;
static CHAR StNetSelected[MAX_ADAPTER_NAME_LENGTH + 4];
static BOOL StNetEditDhcp;
static BOOL StNetBusy;
static ST_INFO_ROW StNetRows[16];
static DWORD StNetRowCount;

static
VOID
StNetWiden(
    _In_ LPCSTR Text,
    _Out_writes_(Count) LPWSTR Wide,
    _In_ DWORD Count)
{
    if (!MultiByteToWideChar(CP_ACP, 0, Text, -1, Wide, (int)Count))
        Wide[0] = UNICODE_NULL;
    Wide[Count - 1] = UNICODE_NULL;
}

/* Addresses go to the stack in network byte order, the first octet in the low byte */
static
BOOL
StNetParseAddress(
    _In_ LPCWSTR Text,
    _Out_ PULONG Address)
{
    ULONG Value = 0;
    UINT Part;

    *Address = 0;
    while (*Text == L' ')
        Text++;

    for (Part = 0; Part < 4; Part++)
    {
        ULONG Octet = 0;
        UINT Digits = 0;

        while (*Text >= L'0' && *Text <= L'9' && Digits < 4)
        {
            Octet = Octet * 10 + (*Text++ - L'0');
            Digits++;
        }

        if (!Digits || Octet > 255)
            return FALSE;

        Value |= Octet << (Part * 8);

        if (Part < 3 && *Text++ != L'.')
            return FALSE;
    }

    while (*Text == L' ')
        Text++;
    if (*Text)
        return FALSE;

    *Address = Value;
    return TRUE;
}

static
ULONG
StNetHostOrder(
    _In_ ULONG Address)
{
    return ((Address & 0xFF) << 24) | ((Address & 0xFF00) << 8) | ((Address >> 8) & 0xFF00) | (Address >> 24);
}

static
BOOL
StNetIsMask(
    _In_ ULONG Mask)
{
    ULONG Host = StNetHostOrder(Mask);

    return Host && ((~Host + 1) & ~Host) == 0;
}

static
VOID
StNetFormatAddress(
    _In_ ULONG Address,
    _Out_writes_(Count) LPWSTR Text,
    _In_ DWORD Count)
{
    StringCchPrintfW(Text, Count, L"%lu.%lu.%lu.%lu",
                     Address & 0xFF, (Address >> 8) & 0xFF, (Address >> 16) & 0xFF, Address >> 24);
}

static
BOOL
StNetIsSet(
    _In_ LPCWSTR Address)
{
    return Address[0] && wcscmp(Address, L"0.0.0.0") != 0;
}

static
BOOL
StNetIsUp(
    _In_ const ST_NET_ADAPTER *Adapter)
{
    if (Adapter->Cellular && Adapter->HaveRmNet)
        return Adapter->RmNet.State == RMNET_STATE_CONNECTED;

    if (Adapter->HaveCounters)
        return Adapter->OperStatus >= MIB_IF_OPER_STATUS_CONNECTED;

    return StNetIsSet(Adapter->Address);
}

static
LPCWSTR
StNetTypeText(
    _In_ const ST_NET_ADAPTER *Adapter)
{
    if (Adapter->Cellular)
        return L"Cellular";

    switch (Adapter->Type)
    {
        case MIB_IF_TYPE_ETHERNET:
            return L"Ethernet";
        case IF_TYPE_IEEE80211:
            return L"Wi-Fi";
        case MIB_IF_TYPE_PPP:
            return L"Point to point";
        case MIB_IF_TYPE_LOOPBACK:
            return L"Loopback";
        default:
            return L"Network adapter";
    }
}

static
VOID
StNetStateText(
    _In_ const ST_NET_ADAPTER *Adapter,
    _Out_writes_(Count) LPWSTR Text,
    _In_ DWORD Count)
{
    if (Adapter->Cellular && Adapter->HaveRmNet)
    {
        switch (Adapter->RmNet.State)
        {
            case RMNET_STATE_CONNECTED:
                StringCchCopyW(Text, Count, L"Connected");
                return;
            case RMNET_STATE_READY:
                StringCchCopyW(Text, Count, L"No data session");
                return;
            default:
                StringCchCopyW(Text, Count, L"Waiting for the modem data path");
                return;
        }
    }

    if (StNetIsUp(Adapter))
        StringCchCopyW(Text, Count, L"Connected");
    else if (Adapter->HaveCounters && Adapter->OperStatus == MIB_IF_OPER_STATUS_CONNECTING)
        StringCchCopyW(Text, Count, L"Connecting");
    else
        StringCchCopyW(Text, Count, L"Disconnected");
}

static
VOID
StNetFormatBytes(
    _In_ DWORD Bytes,
    _Out_writes_(Count) LPWSTR Text,
    _In_ DWORD Count)
{
    if (Bytes >= 1024 * 1024)
        StringCchPrintfW(Text, Count, L"%lu.%lu MB", Bytes / (1024 * 1024), (Bytes % (1024 * 1024)) * 10 / (1024 * 1024));
    else if (Bytes >= 1024)
        StringCchPrintfW(Text, Count, L"%lu KB", Bytes / 1024);
    else
        StringCchPrintfW(Text, Count, L"%lu bytes", Bytes);
}

static
VOID
StNetInterfaceKey(
    _In_ LPCSTR Id,
    _Out_writes_(Count) LPWSTR Path,
    _In_ DWORD Count)
{
    WCHAR WideId[MAX_ADAPTER_NAME_LENGTH + 4];

    StNetWiden(Id, WideId, _countof(WideId));
    StringCchPrintfW(Path, Count, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\%s", WideId);
}

static
VOID
StNetReadRegistry(
    _Inout_ PST_NET_ADAPTER Adapter)
{
    WCHAR WideId[MAX_ADAPTER_NAME_LENGTH + 4];
    WCHAR Path[256];
    HKEY Key;
    DWORD Size, Type;

    StNetWiden(Adapter->Id, WideId, _countof(WideId));
    StringCchPrintfW(Path, _countof(Path),
                     L"SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\%s\\Connection",
                     WideId);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_QUERY_VALUE, &Key) == ERROR_SUCCESS)
    {
        Size = sizeof(Adapter->Name) - sizeof(WCHAR);
        if (RegQueryValueExW(Key, L"Name", NULL, &Type, (LPBYTE)Adapter->Name, &Size) != ERROR_SUCCESS ||
            Type != REG_SZ)
        {
            Size = 0;
        }
        Adapter->Name[Size / sizeof(WCHAR)] = UNICODE_NULL;
        RegCloseKey(Key);
    }

    StNetInterfaceKey(Adapter->Id, Path, _countof(Path));
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_QUERY_VALUE, &Key) == ERROR_SUCCESS)
    {
        Size = sizeof(Adapter->StaticDns) - sizeof(WCHAR);
        if (RegQueryValueExW(Key, L"NameServer", NULL, &Type, (LPBYTE)Adapter->StaticDns, &Size) != ERROR_SUCCESS ||
            Type != REG_SZ)
        {
            Size = 0;
        }
        Adapter->StaticDns[Size / sizeof(WCHAR)] = UNICODE_NULL;
        RegCloseKey(Key);
    }
}

static
VOID
StNetReadDns(
    _Inout_ PST_NET_ADAPTER Adapter)
{
    PIP_PER_ADAPTER_INFO Info;
    PIP_ADDR_STRING Server;
    ULONG Size = 0;
    WCHAR One[ST_NET_ADDRESS_MAX];

    Adapter->Dns[0] = UNICODE_NULL;
    if (GetPerAdapterInfo(Adapter->Index, NULL, &Size) != ERROR_BUFFER_OVERFLOW || !Size)
        return;

    Info = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Info)
        return;

    if (GetPerAdapterInfo(Adapter->Index, Info, &Size) == ERROR_SUCCESS)
    {
        for (Server = &Info->DnsServerList; Server; Server = Server->Next)
        {
            StNetWiden(Server->IpAddress.String, One, _countof(One));
            if (!StNetIsSet(One))
                continue;
            if (Adapter->Dns[0])
                StringCchCatW(Adapter->Dns, _countof(Adapter->Dns), L", ");
            StringCchCatW(Adapter->Dns, _countof(Adapter->Dns), One);
        }
    }

    HeapFree(GetProcessHeap(), 0, Info);
}

static
VOID
StNetReadRmNet(
    _Inout_ PST_NET_ADAPTER Adapter)
{
    HANDLE Device;
    DWORD Returned;

    Device = CreateFileW(RMNET_CONTROL_DEVICE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, 0, NULL);
    if (Device == INVALID_HANDLE_VALUE)
        return;

    ZeroMemory(&Adapter->RmNet, sizeof(Adapter->RmNet));
    Adapter->HaveRmNet = DeviceIoControl(Device, IOCTL_RMNET_QUERY, NULL, 0, &Adapter->RmNet,
                                         sizeof(Adapter->RmNet), &Returned, NULL) &&
                         Returned >= sizeof(Adapter->RmNet);
    CloseHandle(Device);
}

/* Reads every adapter the stack knows, it is quick enough to do on the UI thread */
static
VOID
StNetRead(VOID)
{
    PIP_ADAPTER_INFO List = NULL, Info;
    ULONG Size = 0;
    DWORD Error;

    StNetCount = 0;

    Error = GetAdaptersInfo(NULL, &Size);
    if (Error != ERROR_BUFFER_OVERFLOW || !Size)
        return;

    List = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!List)
        return;

    if (GetAdaptersInfo(List, &Size) != ERROR_SUCCESS)
    {
        HeapFree(GetProcessHeap(), 0, List);
        return;
    }

    for (Info = List; Info && StNetCount < _countof(StNetAdapters); Info = Info->Next)
    {
        PST_NET_ADAPTER Adapter = &StNetAdapters[StNetCount++];
        MIB_IFROW Row;

        ZeroMemory(Adapter, sizeof(*Adapter));
        StringCchCopyA(Adapter->Id, _countof(Adapter->Id), Info->AdapterName);
        StNetWiden(Info->Description, Adapter->Description, _countof(Adapter->Description));
        Adapter->Index = Info->Index;
        Adapter->Type = Info->Type;
        Adapter->MacLength = min(Info->AddressLength, (UINT)sizeof(Adapter->Mac));
        CopyMemory(Adapter->Mac, Info->Address, Adapter->MacLength);
        StNetWiden(Info->IpAddressList.IpAddress.String, Adapter->Address, _countof(Adapter->Address));
        StNetWiden(Info->IpAddressList.IpMask.String, Adapter->Mask, _countof(Adapter->Mask));
        StNetWiden(Info->GatewayList.IpAddress.String, Adapter->Gateway, _countof(Adapter->Gateway));
        StNetWiden(Info->DhcpServer.IpAddress.String, Adapter->DhcpServer, _countof(Adapter->DhcpServer));
        Adapter->AddressContext = Info->IpAddressList.Context;
        Adapter->Dhcp = Info->DhcpEnabled;
        Adapter->LeaseExpires = Info->LeaseExpires;
        Adapter->Cellular = (_stricmp(Info->AdapterName, StNetCellularId) == 0);

        ZeroMemory(&Row, sizeof(Row));
        Row.dwIndex = Info->Index;
        if (GetIfEntry(&Row) == NO_ERROR)
        {
            Adapter->HaveCounters = TRUE;
            Adapter->OperStatus = Row.dwOperStatus;
            Adapter->Speed = Row.dwSpeed;
            Adapter->Mtu = Row.dwMtu;
            Adapter->InOctets = Row.dwInOctets;
            Adapter->OutOctets = Row.dwOutOctets;
        }

        StNetReadRegistry(Adapter);
        StNetReadDns(Adapter);
        if (Adapter->Cellular)
            StNetReadRmNet(Adapter);

        if (!Adapter->Name[0])
            StringCchCopyW(Adapter->Name, _countof(Adapter->Name), Adapter->Description);
    }

    HeapFree(GetProcessHeap(), 0, List);
}

static
PST_NET_ADAPTER
StNetCurrent(VOID)
{
    DWORD i;

    for (i = 0; i < StNetCount; i++)
    {
        if (!strcmp(StNetAdapters[i].Id, StNetSelected))
            return &StNetAdapters[i];
    }

    return NULL;
}

static
VOID
StNetBuildRows(VOID)
{
    const ST_NET_ADAPTER *Adapter = StNetCurrent();
    WCHAR State[64];
    WCHAR In[32], Out[32];
    WCHAR Mac[3 * MAX_ADAPTER_ADDRESS_LENGTH];
    UINT i;

    StNetRowCount = 0;
    if (!Adapter)
        return;

    StNetStateText(Adapter, State, _countof(State));
    StSetInfoRow(&StNetRows[StNetRowCount++], L"Status", L"%s", State);
    StSetInfoRow(&StNetRows[StNetRowCount++], L"Adapter", L"%s, %s", StNetTypeText(Adapter), Adapter->Description);

    Mac[0] = UNICODE_NULL;
    for (i = 0; i < Adapter->MacLength; i++)
    {
        WCHAR Octet[4];

        StringCchPrintfW(Octet, _countof(Octet), i ? L":%02X" : L"%02X", Adapter->Mac[i]);
        StringCchCatW(Mac, _countof(Mac), Octet);
    }
    StSetInfoRow(&StNetRows[StNetRowCount++], L"Hardware address", L"%s", Mac);

    StSetInfoRow(&StNetRows[StNetRowCount++], L"IPv4 address", L"%s",
                 StNetIsSet(Adapter->Address) ? Adapter->Address : L"None");
    StSetInfoRow(&StNetRows[StNetRowCount++], L"Subnet mask", L"%s",
                 StNetIsSet(Adapter->Mask) ? Adapter->Mask : L"None");
    StSetInfoRow(&StNetRows[StNetRowCount++], L"Gateway", L"%s",
                 StNetIsSet(Adapter->Gateway) ? Adapter->Gateway : L"None");
    StSetInfoRow(&StNetRows[StNetRowCount++], L"DNS servers", L"%s%s",
                 Adapter->Dns[0] ? Adapter->Dns : L"None",
                 Adapter->StaticDns[0] ? L" (manual)" : L"");

    if (Adapter->Dhcp)
    {
        time_t Left = Adapter->LeaseExpires - time(NULL);

        if (StNetIsSet(Adapter->DhcpServer) && Left > 0)
        {
            StSetInfoRow(&StNetRows[StNetRowCount++], L"Configuration", L"DHCP from %s, lease %lu h %02lu min left",
                         Adapter->DhcpServer, (ULONG)(Left / 3600), (ULONG)((Left / 60) % 60));
        }
        else
        {
            StSetInfoRow(&StNetRows[StNetRowCount++], L"Configuration", L"DHCP, no lease yet");
        }
    }
    else
    {
        StSetInfoRow(&StNetRows[StNetRowCount++], L"Configuration", L"Manual");
    }

    if (Adapter->HaveCounters)
    {
        if (Adapter->Speed >= 1000000)
            StSetInfoRow(&StNetRows[StNetRowCount++], L"Link", L"%lu Mbps, MTU %lu", Adapter->Speed / 1000000, Adapter->Mtu);
        else
            StSetInfoRow(&StNetRows[StNetRowCount++], L"Link", L"MTU %lu", Adapter->Mtu);

        StNetFormatBytes(Adapter->OutOctets, Out, _countof(Out));
        StNetFormatBytes(Adapter->InOctets, In, _countof(In));
        StSetInfoRow(&StNetRows[StNetRowCount++], L"Traffic", L"%s sent, %s received", Out, In);
    }

    if (Adapter->Cellular)
    {
        if (Adapter->HaveRmNet)
        {
            StSetInfoRow(&StNetRows[StNetRowCount++], L"Modem data path", L"Mux %lu, %I64u sent, %I64u received, %I64u dropped",
                         Adapter->RmNet.MuxId, Adapter->RmNet.SentPackets,
                         Adapter->RmNet.ReceivedPackets, Adapter->RmNet.DroppedPackets);
        }
        else
        {
            StSetInfoRow(&StNetRows[StNetRowCount++], L"Modem data path", L"The cellular adapter is not answering");
        }
    }
}

static
VOID
StNetFillList(VOID)
{
    HWND List = GetDlgItem(StWindow, IDC_NET_LIST);
    LRESULT Top = SendMessageW(List, LB_GETTOPINDEX, 0, 0);
    DWORD i;

    SendMessageW(List, WM_SETREDRAW, FALSE, 0);
    SendMessageW(List, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < StNetCount; i++)
        SendMessageW(List, LB_ADDSTRING, 0, (LPARAM)i);
    if (Top != LB_ERR && (DWORD)Top < StNetCount)
        SendMessageW(List, LB_SETTOPINDEX, Top, 0);
    SendMessageW(List, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(List, NULL, TRUE);
}

static
VOID
StNetUpdateControls(VOID)
{
    const ST_NET_ADAPTER *Adapter = StNetCurrent();
    DWORD i;

    if (StPage != ST_PAGE_NETWORK)
        return;

    for (i = 0; i < StNetControlCount; i++)
        ShowWindow(GetDlgItem(StWindow, StNetControls[i]), SW_HIDE);

    switch (StNetView)
    {
        case StNetViewList:
            ShowWindow(GetDlgItem(StWindow, IDC_NET_LIST), SW_SHOW);
            break;

        case StNetViewDetail:
            StShowGroup(StNetDetailControls, _countof(StNetDetailControls), TRUE);
            EnableWindow(GetDlgItem(StWindow, IDC_NET_RENEW), Adapter && Adapter->Dhcp && !StNetBusy);
            EnableWindow(GetDlgItem(StWindow, IDC_NET_RELEASE),
                         Adapter && Adapter->Dhcp && StNetIsSet(Adapter->Address) && !StNetBusy);
            EnableWindow(GetDlgItem(StWindow, IDC_NET_CONFIGURE), Adapter && !Adapter->Cellular);
            break;

        case StNetViewEdit:
            ShowWindow(GetDlgItem(StWindow, IDC_NET_MODE), SW_SHOW);
            ShowWindow(GetDlgItem(StWindow, IDC_NET_DNS_LABEL), SW_SHOW);
            ShowWindow(GetDlgItem(StWindow, IDC_NET_DNS), SW_SHOW);
            ShowWindow(GetDlgItem(StWindow, IDC_NET_SAVE), SW_SHOW);
            ShowWindow(GetDlgItem(StWindow, IDC_NET_BACK), SW_SHOW);
            StShowGroup(StNetStaticControls, _countof(StNetStaticControls), !StNetEditDhcp);
            SetDlgItemTextW(StWindow, IDC_NET_MODE, StNetEditDhcp ? L"Automatic (DHCP)" : L"Manual");
            EnableWindow(GetDlgItem(StWindow, IDC_NET_SAVE), !StNetBusy);
            break;
    }
}

static
VOID
StNetSetView(
    _In_ ST_NET_VIEW View)
{
    const ST_NET_ADAPTER *Adapter = StNetCurrent();

    StNetView = View;

    if (View == StNetViewEdit && Adapter)
    {
        StNetEditDhcp = Adapter->Dhcp;
        SetDlgItemTextW(StWindow, IDC_NET_ADDRESS, StNetIsSet(Adapter->Address) ? Adapter->Address : L"");
        SetDlgItemTextW(StWindow, IDC_NET_MASK, StNetIsSet(Adapter->Mask) ? Adapter->Mask : L"255.255.255.0");
        SetDlgItemTextW(StWindow, IDC_NET_GATEWAY, StNetIsSet(Adapter->Gateway) ? Adapter->Gateway : L"");
        SetDlgItemTextW(StWindow, IDC_NET_DNS, Adapter->StaticDns);
    }
    else if (View == StNetViewList)
    {
        SendDlgItemMessageW(StWindow, IDC_NET_LIST, LB_SETCURSEL, (WPARAM)-1, 0);
    }

    StNetBuildRows();
    StNetUpdateControls();
    StRelayout();
    InvalidateRect(StWindow, NULL, FALSE);
}

static
VOID
StNetRefresh(VOID)
{
    StNetRead();
    if (StNetView != StNetViewList && !StNetCurrent())
        StNetView = StNetViewList;

    StNetFillList();
    StNetBuildRows();
    StNetUpdateControls();
    InvalidateRect(StWindow, NULL, FALSE);
}

/* Network worker, DHCP and the stack can take a while to answer */

static
DWORD
StNetIndexMap(
    _In_ DWORD Index,
    _Out_ PIP_ADAPTER_INDEX_MAP Map)
{
    PIP_INTERFACE_INFO Table;
    ULONG Size = 0;
    DWORD Error;
    LONG i;

    ZeroMemory(Map, sizeof(*Map));
    Error = GetInterfaceInfo(NULL, &Size);
    if (Error != ERROR_INSUFFICIENT_BUFFER || !Size)
        return Error ? Error : ERROR_NOT_FOUND;

    Table = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Table)
        return ERROR_NOT_ENOUGH_MEMORY;

    Error = GetInterfaceInfo(Table, &Size);
    if (Error == NO_ERROR)
    {
        Error = ERROR_NOT_FOUND;
        for (i = 0; i < Table->NumAdapters; i++)
        {
            if (Table->Adapter[i].Index == Index)
            {
                *Map = Table->Adapter[i];
                Error = NO_ERROR;
                break;
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, Table);
    return Error;
}

static
VOID
StNetSetMultiString(
    _In_ HKEY Key,
    _In_ LPCWSTR Name,
    _In_ LPCWSTR Value)
{
    WCHAR Buffer[ST_NET_ADDRESS_MAX + 2];
    size_t Length = 0;

    ZeroMemory(Buffer, sizeof(Buffer));
    StringCchCopyW(Buffer, ST_NET_ADDRESS_MAX, Value);
    StringCchLengthW(Buffer, ST_NET_ADDRESS_MAX, &Length);

    /* An empty list is a single terminator */
    RegSetValueExW(Key, Name, 0, REG_MULTI_SZ, (LPBYTE)Buffer,
                   (DWORD)((Length ? Length + 2 : 1) * sizeof(WCHAR)));
}

static
DWORD
StNetApplyConfiguration(
    _In_ PST_NET_JOB Job)
{
    const ST_NET_ADAPTER *Adapter = &Job->Adapter;
    WCHAR Path[256];
    WCHAR Text[ST_NET_ADDRESS_MAX];
    ULONG OldAddress = 0;
    DWORD Version = 0;
    DWORD Error = NO_ERROR;
    HKEY Key;

    StNetParseAddress(Adapter->Address, &OldAddress);

    StNetInterfaceKey(Adapter->Id, Path, _countof(Path));
    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_SET_VALUE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    /* What tcpip reads when the adapter binds again */
    RegSetValueExW(Key, L"EnableDHCP", 0, REG_DWORD, (LPBYTE)&Job->Dhcp, sizeof(Job->Dhcp));
    if (Job->Dhcp)
    {
        StNetSetMultiString(Key, L"IPAddress", L"0.0.0.0");
        StNetSetMultiString(Key, L"SubnetMask", L"0.0.0.0");
        StNetSetMultiString(Key, L"DefaultGateway", L"");
    }
    else
    {
        StNetFormatAddress(Job->Address, Text, _countof(Text));
        StNetSetMultiString(Key, L"IPAddress", Text);
        StNetFormatAddress(Job->Mask, Text, _countof(Text));
        StNetSetMultiString(Key, L"SubnetMask", Text);
        if (Job->Gateway)
            StNetFormatAddress(Job->Gateway, Text, _countof(Text));
        else
            Text[0] = UNICODE_NULL;
        StNetSetMultiString(Key, L"DefaultGateway", Text);
    }

    if (Job->Dns[0])
        RegSetValueExW(Key, L"NameServer", 0, REG_SZ, (LPBYTE)Job->Dns, (DWORD)((wcslen(Job->Dns) + 1) * sizeof(WCHAR)));
    else
        RegDeleteValueW(Key, L"NameServer");
    RegCloseKey(Key);

    /* Then the running stack, the same way the TCP/IP property sheet applies it */
    if (Job->Dhcp != Adapter->Dhcp)
    {
        if (Job->Dhcp)
        {
            IP_ADAPTER_INDEX_MAP Map;

            if (StNetIsSet(Adapter->Address))
                DeleteIPAddress(Adapter->AddressContext);
            if (StNetIndexMap(Adapter->Index, &Map) == NO_ERROR)
                Error = IpRenewAddress(&Map);
        }
        else if (DhcpCApiInitialize(&Version) == NO_ERROR)
        {
            Error = DhcpStaticRefreshParams(Adapter->Index, Job->Address, Job->Mask);
            DhcpCApiCleanup();
        }
    }
    else if (!Job->Dhcp && Job->Address != OldAddress)
    {
        ULONG Context, Instance;

        if (StNetIsSet(Adapter->Address))
            DeleteIPAddress(Adapter->AddressContext);
        Error = AddIPAddress(Job->Address, Job->Mask, Adapter->Index, &Context, &Instance);
    }

    if (!Job->Dhcp)
    {
        PMIB_IPFORWARDTABLE Routes;
        ULONG Size = 0;
        DWORD i;

        /* The adapter keeps one default route, through the gateway given here */
        if (GetIpForwardTable(NULL, &Size, FALSE) == ERROR_INSUFFICIENT_BUFFER && Size)
        {
            Routes = HeapAlloc(GetProcessHeap(), 0, Size);
            if (Routes)
            {
                if (GetIpForwardTable(Routes, &Size, FALSE) == NO_ERROR)
                {
                    for (i = 0; i < Routes->dwNumEntries; i++)
                    {
                        if (Routes->table[i].dwForwardIfIndex == Adapter->Index &&
                            Routes->table[i].dwForwardDest == 0)
                        {
                            DeleteIpForwardEntry(&Routes->table[i]);
                        }
                    }
                }
                HeapFree(GetProcessHeap(), 0, Routes);
            }
        }

        if (Job->Gateway)
        {
            MIB_IPFORWARDROW Route;

            ZeroMemory(&Route, sizeof(Route));
            Route.dwForwardMetric1 = 1;
            Route.dwForwardIfIndex = Adapter->Index;
            Route.dwForwardNextHop = Job->Gateway;
            CreateIpForwardEntry(&Route);
        }
    }

    if (wcscmp(Job->Dns, Adapter->StaticDns) != 0)
    {
        SC_HANDLE Manager, Service;
        SERVICE_STATUS Status;

        Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
        if (Manager)
        {
            Service = OpenServiceW(Manager, L"Dnscache", SERVICE_PAUSE_CONTINUE);
            if (Service)
            {
                ControlService(Service, SERVICE_CONTROL_PARAMCHANGE, &Status);
                CloseServiceHandle(Service);
            }
            CloseServiceHandle(Manager);
        }
    }

    return Error;
}

static
DWORD
WINAPI
StNetWorker(
    _In_ LPVOID Parameter)
{
    PST_NET_JOB Job = Parameter;
    IP_ADAPTER_INDEX_MAP Map;

    switch (Job->Operation)
    {
        case StNetRenew:
        case StNetRelease:
            Job->Error = StNetIndexMap(Job->Adapter.Index, &Map);
            if (Job->Error == NO_ERROR)
                Job->Error = (Job->Operation == StNetRenew) ? IpRenewAddress(&Map) : IpReleaseAddress(&Map);
            break;

        case StNetApply:
            Job->Error = StNetApplyConfiguration(Job);
            break;
    }

    if (!PostMessageW(StWindow, WM_NET_DONE, 0, (LPARAM)Job))
        HeapFree(GetProcessHeap(), 0, Job);
    return 0;
}

static
VOID
StNetStart(
    _In_ ST_NET_OPERATION Operation,
    _In_opt_ const ST_NET_JOB *Settings)
{
    const ST_NET_ADAPTER *Adapter = StNetCurrent();
    PST_NET_JOB Job;

    if (!Adapter)
        return;

    if (StNetBusy)
    {
        StSetStatus(L"Wait for the current request to finish");
        return;
    }

    Job = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Job));
    if (!Job)
        return;

    if (Settings)
        *Job = *Settings;
    Job->Operation = Operation;
    Job->Adapter = *Adapter;

    if (!StStartWorker(StNetWorker, Job))
    {
        HeapFree(GetProcessHeap(), 0, Job);
        StSetStatus(L"Could not start the request");
        return;
    }

    StNetBusy = TRUE;
    StNetUpdateControls();

    switch (Operation)
    {
        case StNetRenew:
            StSetStatus(L"Asking for a new address");
            break;
        case StNetRelease:
            StSetStatus(L"Giving the address back");
            break;
        case StNetApply:
            StSetStatus(L"Applying the settings");
            break;
    }
}

/* Checks the edit view and hands it to the worker */
static
VOID
StNetSave(VOID)
{
    ST_NET_JOB Settings;
    WCHAR Text[ST_NET_DNS_MAX];
    WCHAR *Part;
    ULONG Server;

    ZeroMemory(&Settings, sizeof(Settings));
    Settings.Dhcp = StNetEditDhcp;

    if (!StNetEditDhcp)
    {
        GetDlgItemTextW(StWindow, IDC_NET_ADDRESS, Text, _countof(Text));
        if (!StNetParseAddress(Text, &Settings.Address) || !Settings.Address)
        {
            StSetStatus(L"Enter an IPv4 address such as 192.168.1.20");
            SetFocus(GetDlgItem(StWindow, IDC_NET_ADDRESS));
            return;
        }

        GetDlgItemTextW(StWindow, IDC_NET_MASK, Text, _countof(Text));
        if (!StNetParseAddress(Text, &Settings.Mask) || !StNetIsMask(Settings.Mask))
        {
            StSetStatus(L"Enter a subnet mask such as 255.255.255.0");
            SetFocus(GetDlgItem(StWindow, IDC_NET_MASK));
            return;
        }

        GetDlgItemTextW(StWindow, IDC_NET_GATEWAY, Text, _countof(Text));
        if (Text[0] && !StNetParseAddress(Text, &Settings.Gateway))
        {
            StSetStatus(L"The gateway is not an IPv4 address");
            SetFocus(GetDlgItem(StWindow, IDC_NET_GATEWAY));
            return;
        }

        if (Settings.Gateway && ((Settings.Gateway ^ Settings.Address) & Settings.Mask))
        {
            StSetStatus(L"The gateway must be on the same subnet as the address");
            SetFocus(GetDlgItem(StWindow, IDC_NET_GATEWAY));
            return;
        }
    }

    /* DNS servers are kept comma separated, as tcpip reads them */
    GetDlgItemTextW(StWindow, IDC_NET_DNS, Text, _countof(Text));
    for (Part = wcstok(Text, L", "); Part; Part = wcstok(NULL, L", "))
    {
        if (!StNetParseAddress(Part, &Server) || !Server)
        {
            StSetStatus(L"%s is not an IPv4 address", Part);
            SetFocus(GetDlgItem(StWindow, IDC_NET_DNS));
            return;
        }

        if (Settings.Dns[0])
            StringCchCatW(Settings.Dns, _countof(Settings.Dns), L",");
        StringCchCatW(Settings.Dns, _countof(Settings.Dns), Part);
    }

    StNetStart(StNetApply, &Settings);
}

VOID
StNetDone(
    _In_ LPARAM Result)
{
    PST_NET_JOB Job = (PST_NET_JOB)Result;
    WCHAR Error[128];

    StNetBusy = FALSE;
    StErrorText(Job->Error, Error, _countof(Error));

    switch (Job->Operation)
    {
        case StNetRenew:
            if (Job->Error == NO_ERROR)
                StSetStatus(L"Asked %s for a new address", Job->Adapter.Name);
            else
                StSetStatus(L"Renew failed: %s", Error);
            break;

        case StNetRelease:
            if (Job->Error == NO_ERROR)
                StSetStatus(L"Released the address of %s", Job->Adapter.Name);
            else
                StSetStatus(L"Release failed: %s", Error);
            break;

        case StNetApply:
            if (Job->Error == NO_ERROR)
            {
                StSetStatus(L"Saved the settings of %s", Job->Adapter.Name);
                if (StNetView == StNetViewEdit)
                    StNetView = StNetViewDetail;
            }
            else
            {
                StSetStatus(L"Saved, but the stack did not take it: %s", Error);
            }
            break;
    }

    HeapFree(GetProcessHeap(), 0, Job);

    StNetRefresh();
    StRelayout();
}

VOID
StNetCreateControls(VOID)
{
    StControl(L"LISTBOX", NULL, WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | LBS_OWNERDRAWFIXED,
              IDC_NET_LIST, StFont);
    StButton(L"Renew", WS_TABSTOP, IDC_NET_RENEW);
    StButton(L"Release", WS_TABSTOP, IDC_NET_RELEASE);
    StButton(L"IP settings", WS_TABSTOP, IDC_NET_CONFIGURE);
    StButton(L"Back", WS_TABSTOP, IDC_NET_BACK);

    StButton(L"Automatic (DHCP)", WS_TABSTOP, IDC_NET_MODE);
    StControl(L"STATIC", L"IP address", SS_CENTERIMAGE, IDC_NET_ADDRESS_LABEL, StFont);
    StControl(L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL, IDC_NET_ADDRESS, StFont);
    StControl(L"STATIC", L"Subnet mask", SS_CENTERIMAGE, IDC_NET_MASK_LABEL, StFont);
    StControl(L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL, IDC_NET_MASK, StFont);
    StControl(L"STATIC", L"Gateway", SS_CENTERIMAGE, IDC_NET_GATEWAY_LABEL, StFont);
    StControl(L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL, IDC_NET_GATEWAY, StFont);
    StControl(L"STATIC", L"DNS servers, empty for automatic", SS_CENTERIMAGE, IDC_NET_DNS_LABEL, StFont);
    StControl(L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL, IDC_NET_DNS, StFont);
    StButton(L"Save", WS_TABSTOP, IDC_NET_SAVE);

    SendDlgItemMessageW(StWindow, IDC_NET_LIST, LB_SETITEMHEIGHT, 0, ST_ITEM_H);
    SendDlgItemMessageW(StWindow, IDC_NET_ADDRESS, EM_LIMITTEXT, ST_NET_ADDRESS_MAX - 1, 0);
    SendDlgItemMessageW(StWindow, IDC_NET_MASK, EM_LIMITTEXT, ST_NET_ADDRESS_MAX - 1, 0);
    SendDlgItemMessageW(StWindow, IDC_NET_GATEWAY, EM_LIMITTEXT, ST_NET_ADDRESS_MAX - 1, 0);
    SendDlgItemMessageW(StWindow, IDC_NET_DNS, EM_LIMITTEXT, ST_NET_DNS_MAX - 1, 0);
}

VOID
StNetShow(VOID)
{
    StNetRefresh();
}

VOID
StNetHeading(
    _Out_writes_(Count) LPWSTR Text,
    _In_ DWORD Count)
{
    const ST_NET_ADAPTER *Adapter = StNetCurrent();

    if (StNetView == StNetViewList || !Adapter)
        StringCchCopyW(Text, Count, L"Network adapters");
    else if (StNetView == StNetViewEdit)
        StringCchPrintfW(Text, Count, L"%s IPv4", Adapter->Name);
    else
        StringCchCopyW(Text, Count, Adapter->Name);
}

VOID
StNetLayout(
    _In_ const RECT *Area)
{
    int Width = Area->right - Area->left;
    int Half = (Width - ST_GAP) / 2;
    int Left = Area->left;
    int Y;

    switch (StNetView)
    {
        case StNetViewList:
            Y = Area->top + ST_CARD_H + ST_GAP;
            StMove(IDC_NET_LIST, Left, Y, Width, max(Area->bottom - Y, ST_ROW));
            break;

        case StNetViewDetail:
            Y = Area->bottom - 2 * ST_ROW - ST_GAP;
            StMove(IDC_NET_RENEW, Left, Y, Half, ST_ROW);
            StMove(IDC_NET_RELEASE, Left + Half + ST_GAP, Y, Half, ST_ROW);
            Y += ST_ROW + ST_GAP;
            StMove(IDC_NET_CONFIGURE, Left, Y, Half, ST_ROW);
            StMove(IDC_NET_BACK, Left + Half + ST_GAP, Y, Half, ST_ROW);
            break;

        case StNetViewEdit:
            Y = Area->top;
            StMove(IDC_NET_MODE, Left, Y, Width, ST_ROW);
            Y += ST_ROW + ST_GAP;

            if (!StNetEditDhcp)
            {
                StMove(IDC_NET_ADDRESS_LABEL, Left, Y, Width, ST_PILL_H);
                StMove(IDC_NET_ADDRESS, Left, Y + ST_PILL_H, Width, ST_ROW);
                Y += ST_PILL_H + ST_ROW + ST_GAP;
                StMove(IDC_NET_MASK_LABEL, Left, Y, Width, ST_PILL_H);
                StMove(IDC_NET_MASK, Left, Y + ST_PILL_H, Width, ST_ROW);
                Y += ST_PILL_H + ST_ROW + ST_GAP;
                StMove(IDC_NET_GATEWAY_LABEL, Left, Y, Width, ST_PILL_H);
                StMove(IDC_NET_GATEWAY, Left, Y + ST_PILL_H, Width, ST_ROW);
                Y += ST_PILL_H + ST_ROW + ST_GAP;
            }

            StMove(IDC_NET_DNS_LABEL, Left, Y, Width, ST_PILL_H);
            StMove(IDC_NET_DNS, Left, Y + ST_PILL_H, Width, ST_ROW);
            Y += ST_PILL_H + ST_ROW + 2 * ST_GAP;

            StMove(IDC_NET_SAVE, Left, Y, Half, ST_ROW);
            StMove(IDC_NET_BACK, Left + Half + ST_GAP, Y, Half, ST_ROW);
            break;
    }
}

VOID
StNetPaint(
    _In_ HDC Dc,
    _In_ const RECT *Area)
{
    const ST_NET_ADAPTER *Adapter = StNetCurrent();
    WCHAR Title[128];
    WCHAR Detail[256];
    RECT Card, Rows;
    DWORD Up = 0;
    DWORD i;

    if (StNetView == StNetViewEdit)
        return;

    Card = *Area;
    Card.bottom = Card.top + ST_CARD_H;

    if (StNetView == StNetViewList || !Adapter)
    {
        for (i = 0; i < StNetCount; i++)
        {
            if (StNetIsUp(&StNetAdapters[i]))
                Up++;
        }

        if (!StNetCount)
        {
            StringCchCopyW(Title, _countof(Title), L"No network adapters");
            StringCchCopyW(Detail, _countof(Detail), L"Adapters show up here once their drivers load and TCP/IP binds to them.");
        }
        else
        {
            StringCchPrintfW(Title, _countof(Title), L"%lu of %lu connected", Up, StNetCount);
            StringCchCopyW(Detail, _countof(Detail), L"Tap an adapter for its addresses, traffic and IP settings.");
        }

        StDrawCard(Dc, &Card, Title, Detail, Up != 0);
        return;
    }

    StNetStateText(Adapter, Title, _countof(Title));
    if (StNetIsSet(Adapter->Address))
        StringCchPrintfW(Detail, _countof(Detail), L"%s, %s", StNetTypeText(Adapter), Adapter->Address);
    else
        StringCchPrintfW(Detail, _countof(Detail), L"%s, no address", StNetTypeText(Adapter));
    StDrawCard(Dc, &Card, Title, Detail, StNetIsUp(Adapter));

    Rows = *Area;
    Rows.top = Card.bottom + ST_GAP;
    Rows.bottom = Area->bottom - 2 * ST_ROW - 2 * ST_GAP;
    StPaintInfoRows(Dc, &Rows, StNetRows, StNetRowCount);
}

VOID
StNetDrawItem(
    _In_ const DRAWITEMSTRUCT *Item)
{
    const ST_NET_ADAPTER *Adapter;
    WCHAR State[64];
    WCHAR Line[160];
    RECT Card = Item->rcItem, Text;
    BOOL Selected = (Item->itemState & ODS_SELECTED) != 0;
    BOOL Up;

    FillRect(Item->hDC, &Item->rcItem, StBgBrush);
    if (Item->itemID == (UINT)-1 || (DWORD)Item->itemData >= StNetCount)
        return;

    Adapter = &StNetAdapters[Item->itemData];
    Up = StNetIsUp(Adapter);

    InflateRect(&Card, 0, -ST_GAP / 2);
    StFillRound(Item->hDC, &Card, ST_RADIUS, Selected ? ST_ACCENT : ST_TILE);
    SetBkMode(Item->hDC, TRANSPARENT);

    Text = Card;
    Text.left += ST_CARD_PAD;
    Text.right -= ST_CARD_PAD;
    Text.top += ST_CARD_PAD / 2;
    Text.bottom = Text.top + (Card.bottom - Card.top - ST_CARD_PAD) / 2 + 8;

    SelectObject(Item->hDC, StBigFont);
    SetTextColor(Item->hDC, ST_TEXT);
    DrawTextW(Item->hDC, Adapter->Name, -1, &Text, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    StNetStateText(Adapter, State, _countof(State));
    if (StNetIsSet(Adapter->Address))
        StringCchPrintfW(Line, _countof(Line), L"%s, %s, %s", StNetTypeText(Adapter), State, Adapter->Address);
    else
        StringCchPrintfW(Line, _countof(Line), L"%s, %s", StNetTypeText(Adapter), State);

    Text.top = Text.bottom;
    Text.bottom = Card.bottom - ST_CARD_PAD / 2;
    SelectObject(Item->hDC, StSmallFont);
    SetTextColor(Item->hDC, Selected ? ST_TEXT : (Up ? ST_ACCENT : ST_SUBTEXT));
    DrawTextW(Item->hDC, Line, -1, &Text, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

VOID
StNetCommand(
    _In_ UINT Id,
    _In_ UINT Code)
{
    LRESULT Index;

    switch (Id)
    {
        case IDC_NET_LIST:
            if (Code != LBN_SELCHANGE)
                break;

            Index = SendDlgItemMessageW(StWindow, IDC_NET_LIST, LB_GETCURSEL, 0, 0);
            if (Index == LB_ERR || (DWORD)Index >= StNetCount)
                break;

            StringCchCopyA(StNetSelected, _countof(StNetSelected), StNetAdapters[Index].Id);
            StSetStatus(L"");
            StNetSetView(StNetViewDetail);
            break;

        case IDC_NET_RENEW:
            StNetStart(StNetRenew, NULL);
            break;

        case IDC_NET_RELEASE:
            StNetStart(StNetRelease, NULL);
            break;

        case IDC_NET_CONFIGURE:
            StSetStatus(L"");
            StNetSetView(StNetViewEdit);
            break;

        case IDC_NET_MODE:
            StNetEditDhcp = !StNetEditDhcp;
            StNetUpdateControls();
            StRelayout();
            InvalidateRect(GetDlgItem(StWindow, IDC_NET_MODE), NULL, FALSE);
            break;

        case IDOK:
        case IDC_NET_SAVE:
            if (StNetView == StNetViewEdit)
                StNetSave();
            break;

        case IDC_NET_BACK:
            StSetStatus(L"");
            StNetSetView(StNetView == StNetViewEdit ? StNetViewDetail : StNetViewList);
            break;
    }
}

VOID
StNetTimer(VOID)
{
    if (StNetView != StNetViewEdit && !StNetBusy)
        StNetRefresh();
}

BOOL
StNetIsActive(
    _In_ UINT Id)
{
    if (Id == IDC_NET_MODE)
        return StNetEditDhcp;
    if (Id == IDC_NET_SAVE)
        return IsWindowEnabled(GetDlgItem(StWindow, Id));
    return FALSE;
}

VOID
StNetCleanup(VOID)
{
    StNetCount = 0;
}
