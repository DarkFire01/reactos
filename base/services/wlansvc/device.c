/*
 * PROJECT:     ReactOS WLAN service
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driving a native 802.11 adapter through NDISUIO and the dot11 OIDs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#include <stdio.h>
#include <wchar.h>
#include <strsafe.h>
#include <nuiouser.h>

#define NDEBUG
#include <debug.h>

/* dot11 OIDs and structures the RPC header does not carry. The base dot11
   types come from wlansvc_s.h through precomp.h. */
#define OID_DOT11_NDIS_START                        0x0D010300
#define OID_DOT11_CURRENT_OPERATION_MODE            (OID_DOT11_NDIS_START + 8)
#define OID_DOT11_SCAN_REQUEST                      (OID_DOT11_NDIS_START + 11)
#define OID_DOT11_ENUM_BSS_LIST \
    ((0x0E000000U) | (0x01U << 16) | (0x01U << 8) | 0x79)
#define OID_DOT11_DESIRED_SSID_LIST \
    ((0x0E000000U) | (0x01U << 16) | (0x01U << 8) | 0x7C)
#define OID_DOT11_CONNECT_REQUEST \
    ((0x0E000000U) | (0x01U << 16) | (0x01U << 8) | 0x81)
#define OID_DOT11_DISCONNECT_REQUEST \
    ((0x0E000000U) | (0x01U << 16) | (0x01U << 8) | 0x8E)

#define OID_GEN_MEDIA_CONNECT_STATUS                0x00010114
#define OID_GEN_PHYSICAL_MEDIUM                     0x00010202

/* NdisPhysicalMediumNative802_11, the adapters this service drives */
#define WLAN_PHYSICAL_MEDIUM_NATIVE_80211           9

/* 802.11 capability bits and information element ids used to read security */
#define WLAN_CAP_PRIVACY                            0x0010
#define WLAN_IE_SSID                                0
#define WLAN_IE_RSN                                 48
#define WLAN_IE_VENDOR                              221

#define DOT11_OPERATION_MODE_EXTENSIBLE_STATION     0x00000004
#define DOT11_SSID_LIST_REVISION_1                  1
#define NDIS_WLAN_OBJECT_TYPE_DEFAULT               0x80

typedef struct _WLAN_DOT11_SSID_LIST
{
    NDIS_OBJECT_HEADER Header;
    ULONG uNumOfEntries;
    ULONG uTotalNumOfEntries;
    DOT11_SSID SSIDs[1];
} WLAN_DOT11_SSID_LIST, *PWLAN_DOT11_SSID_LIST;

typedef struct _WLAN_DOT11_BYTE_ARRAY
{
    NDIS_OBJECT_HEADER Header;
    ULONG uNumOfBytes;
    ULONG uTotalNumOfBytes;
    UCHAR ucBuffer[1];
} WLAN_DOT11_BYTE_ARRAY, *PWLAN_DOT11_BYTE_ARRAY;

/* How long a scan is given before its results are read */
#define WLAN_SCAN_SETTLE_MS         4000
/* How long a connection attempt is polled before it is called a failure */
#define WLAN_CONNECT_TIMEOUT_MS     8000
#define WLAN_CONNECT_POLL_MS        250

/**
 * @brief
 * Opens NDISUIO bound to one adapter by its interface GUID.
 */
HANDLE
WlanOpenInterface(
    _In_ const GUID *InterfaceGuid)
{
    WCHAR Name[64];
    HANDLE Device;
    DWORD Returned;

    Device = CreateFileW(L"\\\\.\\Ndisuio",
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL,
                         OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL,
                         NULL);
    if (Device == INVALID_HANDLE_VALUE)
        return NULL;

    StringCchPrintfW(Name, ARRAYSIZE(Name),
                     L"\\DEVICE\\{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                     InterfaceGuid->Data1, InterfaceGuid->Data2, InterfaceGuid->Data3,
                     InterfaceGuid->Data4[0], InterfaceGuid->Data4[1],
                     InterfaceGuid->Data4[2], InterfaceGuid->Data4[3],
                     InterfaceGuid->Data4[4], InterfaceGuid->Data4[5],
                     InterfaceGuid->Data4[6], InterfaceGuid->Data4[7]);

    if (!DeviceIoControl(Device,
                         IOCTL_NDISUIO_OPEN_DEVICE,
                         Name,
                         (DWORD)((wcslen(Name) + 1) * sizeof(WCHAR)),
                         NULL,
                         0,
                         &Returned,
                         NULL))
    {
        CloseHandle(Device);
        return NULL;
    }

    return Device;
}

/**
 * @brief
 * Sets one OID on the bound adapter.
 */
DWORD
WlanSetOid(
    _In_ HANDLE Interface,
    _In_ NDIS_OID Oid,
    _In_reads_bytes_opt_(Length) PVOID Data,
    _In_ ULONG Length)
{
    PNDISUIO_SET_OID Set;
    ULONG Size;
    DWORD Returned;
    DWORD Error = ERROR_SUCCESS;

    Size = FIELD_OFFSET(NDISUIO_SET_OID, Data) + Length;
    Set = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (Set == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    Set->Oid = Oid;
    if (Length != 0 && Data != NULL)
        RtlCopyMemory(Set->Data, Data, Length);

    if (!DeviceIoControl(Interface,
                         IOCTL_NDISUIO_SET_OID_VALUE,
                         Set,
                         Size,
                         Set,
                         Size,
                         &Returned,
                         NULL))
    {
        Error = GetLastError();
    }

    HeapFree(GetProcessHeap(), 0, Set);
    return Error;
}

/**
 * @brief
 * Queries one OID from the bound adapter into a caller buffer.
 */
DWORD
WlanQueryOid(
    _In_ HANDLE Interface,
    _In_ NDIS_OID Oid,
    _Out_writes_bytes_to_(Length, *Returned) PVOID Data,
    _In_ ULONG Length,
    _Out_ PULONG Returned)
{
    PNDISUIO_QUERY_OID Query;
    ULONG Size;
    DWORD Got;
    DWORD Error = ERROR_SUCCESS;

    *Returned = 0;

    Size = FIELD_OFFSET(NDISUIO_QUERY_OID, Data) + Length;
    Query = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (Query == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    Query->Oid = Oid;

    if (DeviceIoControl(Interface,
                        IOCTL_NDISUIO_QUERY_OID_VALUE,
                        Query,
                        Size,
                        Query,
                        Size,
                        &Got,
                        NULL))
    {
        if (Got >= FIELD_OFFSET(NDISUIO_QUERY_OID, Data))
        {
            *Returned = Got - FIELD_OFFSET(NDISUIO_QUERY_OID, Data);
            RtlCopyMemory(Data, Query->Data, min(*Returned, Length));
        }
    }
    else
    {
        Error = GetLastError();
    }

    HeapFree(GetProcessHeap(), 0, Query);
    return Error;
}

/**
 * @brief
 * Starts a scan and waits long enough for it to settle before the caller
 * reads the results.
 */
DWORD
WlanScan(
    _In_ const GUID *InterfaceGuid)
{
    HANDLE Interface;
    DOT11_SCAN_REQUEST_V2 Request;
    DWORD Error;

    Interface = WlanOpenInterface(InterfaceGuid);
    if (Interface == NULL)
        return ERROR_BAD_UNIT;

    RtlZeroMemory(&Request, sizeof(Request));
    Request.dot11BSSType = dot11_BSS_type_any;
    RtlFillMemory(&Request.dot11BSSID, sizeof(Request.dot11BSSID), 0xFF);
    Request.dot11ScanType = dot11_scan_type_auto;

    Error = WlanSetOid(Interface, OID_DOT11_SCAN_REQUEST, &Request, sizeof(Request));
    CloseHandle(Interface);

    if (Error == ERROR_SUCCESS)
        Sleep(WLAN_SCAN_SETTLE_MS);

    return Error;
}

/**
 * @brief
 * Reads the networks the last scan found as a DOT11_BYTE_ARRAY of
 * DOT11_BSS_ENTRY, returned to the caller to free.
 */
DWORD
WlanGetBssList(
    _In_ const GUID *InterfaceGuid,
    _Outptr_result_maybenull_ PWLAN_DOT11_BYTE_ARRAY *BssList)
{
    HANDLE Interface;
    PWLAN_DOT11_BYTE_ARRAY Array;
    ULONG Size = 8192;
    ULONG Returned;
    DWORD Error;

    *BssList = NULL;

    Interface = WlanOpenInterface(InterfaceGuid);
    if (Interface == NULL)
        return ERROR_BAD_UNIT;

    Array = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (Array == NULL)
    {
        CloseHandle(Interface);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    Error = WlanQueryOid(Interface, OID_DOT11_ENUM_BSS_LIST, Array, Size, &Returned);
    CloseHandle(Interface);

    if (Error != ERROR_SUCCESS || Returned < FIELD_OFFSET(WLAN_DOT11_BYTE_ARRAY, ucBuffer))
    {
        HeapFree(GetProcessHeap(), 0, Array);
        return Error != ERROR_SUCCESS ? Error : ERROR_GEN_FAILURE;
    }

    *BssList = Array;
    return ERROR_SUCCESS;
}

/* Reads the interface GUID out of a "\DEVICE\{guid}" binding name */
static
BOOLEAN
WlanGuidFromDeviceName(
    _In_ PCWSTR Name,
    _Out_ GUID *Guid)
{
    PCWSTR Open = wcschr(Name, L'{');
    unsigned long Data1;
    unsigned int Data2, Data3;
    unsigned int b[8];

    if (Open == NULL)
        return FALSE;

    if (swscanf(Open, L"{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                &Data1, &Data2, &Data3, &b[0], &b[1], &b[2], &b[3],
                &b[4], &b[5], &b[6], &b[7]) != 11)
    {
        return FALSE;
    }

    Guid->Data1 = Data1;
    Guid->Data2 = (USHORT)Data2;
    Guid->Data3 = (USHORT)Data3;
    Guid->Data4[0] = (UCHAR)b[0];
    Guid->Data4[1] = (UCHAR)b[1];
    Guid->Data4[2] = (UCHAR)b[2];
    Guid->Data4[3] = (UCHAR)b[3];
    Guid->Data4[4] = (UCHAR)b[4];
    Guid->Data4[5] = (UCHAR)b[5];
    Guid->Data4[6] = (UCHAR)b[6];
    Guid->Data4[7] = (UCHAR)b[7];
    return TRUE;
}

/* Opens NDISUIO bound to the adapter named exactly as a binding reports it */
static
HANDLE
WlanOpenByName(
    _In_ PCWSTR Name)
{
    HANDLE Device;
    DWORD Returned;

    Device = CreateFileW(L"\\\\.\\Ndisuio", GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Device == INVALID_HANDLE_VALUE)
        return NULL;

    if (!DeviceIoControl(Device, IOCTL_NDISUIO_OPEN_DEVICE,
                         (PVOID)Name, (DWORD)((wcslen(Name) + 1) * sizeof(WCHAR)),
                         NULL, 0, &Returned, NULL))
    {
        CloseHandle(Device);
        return NULL;
    }

    return Device;
}

/**
 * @brief
 * Enumerates the native 802.11 adapters as a WLAN_INTERFACE_INFO_LIST, returned
 * to the caller to free.
 */
DWORD
WlanEnumWifiInterfaces(
    _Outptr_result_maybenull_ PWLAN_INTERFACE_INFO_LIST *List)
{
    HANDLE Ndisuio;
    PWLAN_INTERFACE_INFO_LIST Result;
    UCHAR Buffer[512];
    PNDISUIO_QUERY_BINDING Binding = (PNDISUIO_QUERY_BINDING)Buffer;
    ULONG Capacity = 8;
    ULONG Count = 0;
    ULONG Index;
    DWORD Returned;

    *List = NULL;

    Ndisuio = CreateFileW(L"\\\\.\\Ndisuio",
                          GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Ndisuio == INVALID_HANDLE_VALUE)
        return ERROR_BAD_UNIT;

    Result = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                       FIELD_OFFSET(WLAN_INTERFACE_INFO_LIST, InterfaceInfo) +
                       Capacity * sizeof(WLAN_INTERFACE_INFO));
    if (Result == NULL)
    {
        CloseHandle(Ndisuio);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    /* Let any pending protocol bindings settle before they are walked */
    DeviceIoControl(Ndisuio, IOCTL_NDISUIO_BIND_WAIT, NULL, 0, NULL, 0, &Returned, NULL);

    for (Index = 0; Count < Capacity; Index++)
    {
        PWLAN_INTERFACE_INFO Info;
        PCWSTR Name;
        GUID Guid;
        HANDLE One;
        ULONG Medium = 0;
        ULONG Got;
        ULONG Status = 0;

        RtlZeroMemory(Buffer, sizeof(Buffer));
        Binding->BindingIndex = Index;
        if (!DeviceIoControl(Ndisuio, IOCTL_NDISUIO_QUERY_BINDING,
                             Binding, sizeof(Buffer), Binding, sizeof(Buffer),
                             &Returned, NULL))
        {
            DPRINT1("WLAN enum: binding %lu ends the list (error %lu)\n", Index, GetLastError());
            break;
        }

        Name = (PCWSTR)(Buffer + Binding->DeviceNameOffset);
        DPRINT1("WLAN enum: binding %lu is %S\n", Index, Name);

        if (!WlanGuidFromDeviceName(Name, &Guid))
        {
            DPRINT1("WLAN enum: no GUID in the binding name\n");
            continue;
        }

        One = WlanOpenByName(Name);
        if (One == NULL)
        {
            DPRINT1("WLAN enum: could not open %S (error %lu)\n", Name, GetLastError());
            continue;
        }

        Got = 0;
        if (WlanQueryOid(One, OID_GEN_PHYSICAL_MEDIUM, &Medium, sizeof(Medium), &Got) != ERROR_SUCCESS ||
            Got < sizeof(Medium) || Medium != WLAN_PHYSICAL_MEDIUM_NATIVE_80211)
        {
            DPRINT1("WLAN enum: binding %lu physical medium %lu, not native 802.11\n", Index, Medium);
            CloseHandle(One);
            continue;
        }

        DPRINT1("WLAN enum: binding %lu is a native 802.11 adapter\n", Index);

        Info = &Result->InterfaceInfo[Count];
        Info->InterfaceGuid = Guid;

        if (Binding->DeviceDescrLength != 0)
            StringCchCopyW(Info->strInterfaceDescription,
                           ARRAYSIZE(Info->strInterfaceDescription),
                           (PCWSTR)(Buffer + Binding->DeviceDescrOffset));
        else
            StringCchCopyW(Info->strInterfaceDescription,
                           ARRAYSIZE(Info->strInterfaceDescription),
                           L"Wireless Network Adapter");

        if (WlanQueryOid(One, OID_GEN_MEDIA_CONNECT_STATUS, &Status, sizeof(Status), &Got) == ERROR_SUCCESS &&
            Got >= sizeof(Status) && Status == NdisMediaStateConnected)
        {
            Info->isState = wlan_interface_state_connected;
        }
        else
        {
            Info->isState = wlan_interface_state_disconnected;
        }

        Count++;
        CloseHandle(One);
    }

    CloseHandle(Ndisuio);

    DPRINT1("WLAN enum: %lu native 802.11 adapter(s)\n", Count);

    Result->dwNumberOfItems = Count;
    Result->dwIndex = 0;
    *List = Result;
    return ERROR_SUCCESS;
}

/* Finds one information element by id in a beacon or probe response body */
static
const UCHAR *
WlanIeFind(
    _In_reads_bytes_(Length) const UCHAR *Ies,
    _In_ ULONG Length,
    _In_ UCHAR Id,
    _Out_ PUCHAR IeLength)
{
    ULONG Offset = 0;

    *IeLength = 0;
    while (Offset + 2 <= Length)
    {
        UCHAR ElementId = Ies[Offset];
        UCHAR ElementLength = Ies[Offset + 1];

        if (Offset + 2 + ElementLength > Length)
            break;
        if (ElementId == Id)
        {
            *IeLength = ElementLength;
            return Ies + Offset + 2;
        }
        Offset += 2 + ElementLength;
    }
    return NULL;
}

/* The WPA element is a vendor element with the Microsoft OUI and type 1 */
static
const UCHAR *
WlanWpaIeFind(
    _In_reads_bytes_(Length) const UCHAR *Ies,
    _In_ ULONG Length,
    _Out_ PUCHAR IeLength)
{
    static const UCHAR WpaOui[4] = { 0x00, 0x50, 0xF2, 0x01 };
    ULONG Offset = 0;

    *IeLength = 0;
    while (Offset + 2 <= Length)
    {
        UCHAR ElementId = Ies[Offset];
        UCHAR ElementLength = Ies[Offset + 1];

        if (Offset + 2 + ElementLength > Length)
            break;
        if (ElementId == WLAN_IE_VENDOR && ElementLength >= 4 &&
            RtlEqualMemory(Ies + Offset + 2, WpaOui, sizeof(WpaOui)))
        {
            *IeLength = ElementLength;
            return Ies + Offset + 2;
        }
        Offset += 2 + ElementLength;
    }
    return NULL;
}

static
DOT11_CIPHER_ALGORITHM
WlanSuiteToCipher(
    _In_ UCHAR Type)
{
    switch (Type)
    {
        case 1: return DOT11_CIPHER_ALGO_WEP40;
        case 2: return DOT11_CIPHER_ALGO_TKIP;
        case 4: return DOT11_CIPHER_ALGO_CCMP;
        case 5: return DOT11_CIPHER_ALGO_WEP104;
        default: return DOT11_CIPHER_ALGO_CCMP;
    }
}

/* Reads the pairwise cipher and whether PSK is offered from an RSN or WPA body
   that begins at its version field */
static
VOID
WlanParseSuites(
    _In_reads_bytes_(Length) const UCHAR *Body,
    _In_ ULONG Length,
    _In_ BOOLEAN Wpa,
    _Out_ DOT11_AUTH_ALGORITHM *Auth,
    _Out_ DOT11_CIPHER_ALGORITHM *Cipher)
{
    ULONG Offset = 2;
    USHORT PairwiseCount;
    USHORT AkmCount;
    BOOLEAN Psk = FALSE;
    USHORT i;

    *Cipher = DOT11_CIPHER_ALGO_CCMP;

    /* Skip the group cipher suite */
    if (Offset + 4 > Length)
        goto Done;
    Offset += 4;

    if (Offset + 2 > Length)
        goto Done;
    PairwiseCount = (USHORT)(Body[Offset] | (Body[Offset + 1] << 8));
    Offset += 2;

    if (PairwiseCount != 0 && Offset + 4 <= Length)
        *Cipher = WlanSuiteToCipher(Body[Offset + 3]);
    Offset += (ULONG)PairwiseCount * 4;

    if (Offset + 2 > Length)
        goto Done;
    AkmCount = (USHORT)(Body[Offset] | (Body[Offset + 1] << 8));
    Offset += 2;

    for (i = 0; i < AkmCount && Offset + 4 <= Length; i++)
    {
        if (Body[Offset + 3] == 2)
            Psk = TRUE;
        Offset += 4;
    }

Done:
    if (Wpa)
        *Auth = Psk ? DOT11_AUTH_ALGO_WPA_PSK : DOT11_AUTH_ALGO_WPA;
    else
        *Auth = Psk ? DOT11_AUTH_ALGO_RSNA_PSK : DOT11_AUTH_ALGO_RSNA;
}

static
VOID
WlanParseSecurity(
    _In_ PDOT11_BSS_ENTRY Entry,
    _Out_ DOT11_AUTH_ALGORITHM *Auth,
    _Out_ DOT11_CIPHER_ALGORITHM *Cipher,
    _Out_ PBOOL Secured)
{
    const UCHAR *Ies = Entry->ucBuffer;
    ULONG Length = Entry->uBufferLength;
    const UCHAR *Rsn;
    const UCHAR *Wpa;
    UCHAR IeLength;

    *Auth = DOT11_AUTH_ALGO_80211_OPEN;
    *Cipher = DOT11_CIPHER_ALGO_NONE;
    *Secured = (Entry->usCapabilityInformation & WLAN_CAP_PRIVACY) ? TRUE : FALSE;

    Rsn = WlanIeFind(Ies, Length, WLAN_IE_RSN, &IeLength);
    if (Rsn != NULL && IeLength >= 8)
    {
        WlanParseSuites(Rsn, IeLength, FALSE, Auth, Cipher);
        *Secured = TRUE;
        return;
    }

    Wpa = WlanWpaIeFind(Ies, Length, &IeLength);
    if (Wpa != NULL && IeLength >= 12)
    {
        WlanParseSuites(Wpa + 4, (ULONG)IeLength - 4, TRUE, Auth, Cipher);
        *Secured = TRUE;
        return;
    }

    if (*Secured)
        *Cipher = DOT11_CIPHER_ALGO_WEP;
}

/**
 * @brief
 * Turns the last scan's BSS list into a WLAN_AVAILABLE_NETWORK_LIST, one entry
 * per SSID, returned to the caller to free.
 */
DWORD
WlanGetAvailableNetworkList(
    _In_ const GUID *InterfaceGuid,
    _Outptr_result_maybenull_ PWLAN_AVAILABLE_NETWORK_LIST *NetworkList)
{
    PWLAN_DOT11_BYTE_ARRAY Bss = NULL;
    PWLAN_AVAILABLE_NETWORK_LIST List;
    DWORD Error;
    ULONG Total;
    ULONG Offset;
    ULONG Capacity;
    ULONG Count = 0;

    *NetworkList = NULL;

    Error = WlanGetBssList(InterfaceGuid, &Bss);
    if (Error != ERROR_SUCCESS)
        return Error;

    Total = Bss->uNumOfBytes;

    /* Each entry is at least a header, so this bounds the network count */
    Capacity = Total / FIELD_OFFSET(DOT11_BSS_ENTRY, ucBuffer) + 1;
    List = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                     FIELD_OFFSET(WLAN_AVAILABLE_NETWORK_LIST, Network) +
                     Capacity * sizeof(WLAN_AVAILABLE_NETWORK));
    if (List == NULL)
    {
        HeapFree(GetProcessHeap(), 0, Bss);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    Offset = 0;
    while (Offset + FIELD_OFFSET(DOT11_BSS_ENTRY, ucBuffer) <= Total)
    {
        PDOT11_BSS_ENTRY Entry = (PDOT11_BSS_ENTRY)(Bss->ucBuffer + Offset);
        ULONG EntrySize = (FIELD_OFFSET(DOT11_BSS_ENTRY, ucBuffer) + Entry->uBufferLength + 3) & ~3u;
        const UCHAR *Ssid;
        UCHAR SsidLength;
        DOT11_AUTH_ALGORITHM Auth;
        DOT11_CIPHER_ALGORITHM Cipher;
        BOOL Secured;
        PWLAN_AVAILABLE_NETWORK Network = NULL;
        ULONG n;

        if (Offset + FIELD_OFFSET(DOT11_BSS_ENTRY, ucBuffer) + Entry->uBufferLength > Total)
            break;

        Ssid = WlanIeFind(Entry->ucBuffer, Entry->uBufferLength, WLAN_IE_SSID, &SsidLength);
        if (SsidLength > sizeof(((PDOT11_SSID)0)->ucSSID))
            SsidLength = 0;

        WlanParseSecurity(Entry, &Auth, &Cipher, &Secured);

        /* Fold BSSes of one SSID into a single network */
        for (n = 0; n < Count; n++)
        {
            if (List->Network[n].dot11Ssid.uSSIDLength == SsidLength &&
                (SsidLength == 0 ||
                 RtlEqualMemory(List->Network[n].dot11Ssid.ucSSID, Ssid, SsidLength)))
            {
                Network = &List->Network[n];
                break;
            }
        }

        if (Network == NULL && Count < Capacity)
        {
            Network = &List->Network[Count++];
            Network->dot11Ssid.uSSIDLength = SsidLength;
            if (SsidLength != 0)
                RtlCopyMemory(Network->dot11Ssid.ucSSID, Ssid, SsidLength);
            Network->dot11BssType = dot11_BSS_type_infrastructure;
            Network->bNetworkConnectable = TRUE;
            Network->uNumberOfPhyTypes = 1;
            Network->dot11PhyTypes[0] = dot11_phy_type_erp;
            Network->bSecurityEnabled = Secured;
            Network->dot11DefaultAuthAlgorithm = Auth;
            Network->dot11DefaultCipherAlgorithm = Cipher;
        }

        if (Network != NULL)
        {
            Network->uNumberOfBssids++;
            if (Entry->uLinkQuality > Network->wlanSignalQuality)
                Network->wlanSignalQuality = Entry->uLinkQuality;
        }

        if (EntrySize == 0)
            break;
        Offset += EntrySize;
    }

    HeapFree(GetProcessHeap(), 0, Bss);

    List->dwNumberOfItems = Count;
    List->dwIndex = 0;
    *NetworkList = List;
    return ERROR_SUCCESS;
}

/**
 * @brief
 * Connects to a network by SSID as an open station, then polls the link until
 * it comes up or the attempt times out.
 */
DWORD
WlanConnect(
    _In_ const GUID *InterfaceGuid,
    _In_ PDOT11_SSID Ssid)
{
    HANDLE Interface;
    DOT11_CURRENT_OPERATION_MODE Mode;
    UCHAR ListBuffer[FIELD_OFFSET(WLAN_DOT11_SSID_LIST, SSIDs) + sizeof(DOT11_SSID)];
    PWLAN_DOT11_SSID_LIST List = (PWLAN_DOT11_SSID_LIST)ListBuffer;
    ULONG Connect = 0;
    ULONG Elapsed;
    DWORD Error;

    Interface = WlanOpenInterface(InterfaceGuid);
    if (Interface == NULL)
        return ERROR_BAD_UNIT;

    RtlZeroMemory(&Mode, sizeof(Mode));
    Mode.uCurrentOpMode = DOT11_OPERATION_MODE_EXTENSIBLE_STATION;
    WlanSetOid(Interface, OID_DOT11_CURRENT_OPERATION_MODE, &Mode, sizeof(Mode));

    RtlZeroMemory(ListBuffer, sizeof(ListBuffer));
    List->Header.Type = NDIS_WLAN_OBJECT_TYPE_DEFAULT;
    List->Header.Revision = DOT11_SSID_LIST_REVISION_1;
    List->Header.Size = sizeof(ListBuffer);
    List->uNumOfEntries = 1;
    List->uTotalNumOfEntries = 1;
    List->SSIDs[0] = *Ssid;
    WlanSetOid(Interface, OID_DOT11_DESIRED_SSID_LIST, List, sizeof(ListBuffer));

    Error = WlanSetOid(Interface, OID_DOT11_CONNECT_REQUEST, &Connect, sizeof(Connect));
    if (Error != ERROR_SUCCESS)
    {
        CloseHandle(Interface);
        return Error;
    }

    /* Wait for the link, which comes up once the association is done */
    for (Elapsed = 0; Elapsed < WLAN_CONNECT_TIMEOUT_MS; Elapsed += WLAN_CONNECT_POLL_MS)
    {
        ULONG Status = 0;
        ULONG Got;

        if (WlanQueryOid(Interface, OID_GEN_MEDIA_CONNECT_STATUS, &Status, sizeof(Status), &Got) == ERROR_SUCCESS &&
            Got >= sizeof(Status) && Status == NdisMediaStateConnected)
        {
            CloseHandle(Interface);
            return ERROR_SUCCESS;
        }
        Sleep(WLAN_CONNECT_POLL_MS);
    }

    CloseHandle(Interface);
    return ERROR_TIMEOUT;
}

/* Copies the text between two tags out of a profile, if it is there */
static
BOOL
WlanProfileTag(
    _In_ PCWSTR Xml,
    _In_ PCWSTR Open,
    _In_ PCWSTR Close,
    _Out_writes_(Count) PWSTR Value,
    _In_ int Count)
{
    PCWSTR Start = wcsstr(Xml, Open);
    PCWSTR End;
    SIZE_T Length;

    if (Start == NULL)
        return FALSE;
    Start += wcslen(Open);
    End = wcsstr(Start, Close);
    if (End == NULL)
        return FALSE;

    Length = (SIZE_T)(End - Start);
    if (Length >= (SIZE_T)Count)
        Length = Count - 1;
    RtlCopyMemory(Value, Start, Length * sizeof(WCHAR));
    Value[Length] = L'\0';
    return TRUE;
}

/**
 * @brief
 * Connects using a temporary profile: a secured one runs the WPA handshake, an
 * open one connects directly.
 */
DWORD
WlanConnectProfile(
    _In_ const GUID *InterfaceGuid,
    _In_ PDOT11_SSID Ssid,
    _In_opt_ PCWSTR Profile)
{
    WCHAR Authentication[32];
    WCHAR Key[128];

    if (Profile != NULL &&
        WlanProfileTag(Profile, L"<authentication>", L"</authentication>",
                       Authentication, ARRAYSIZE(Authentication)) &&
        (_wcsicmp(Authentication, L"WPA2PSK") == 0 || _wcsicmp(Authentication, L"WPAPSK") == 0) &&
        WlanProfileTag(Profile, L"<keyMaterial>", L"</keyMaterial>", Key, ARRAYSIZE(Key)))
    {
        return WlanConnectWpa(InterfaceGuid, Ssid, Key);
    }

    return WlanConnect(InterfaceGuid, Ssid);
}

/**
 * @brief
 * Disconnects the adapter from its current network.
 */
DWORD
WlanDisconnect(
    _In_ const GUID *InterfaceGuid)
{
    HANDLE Interface;
    ULONG Reason = 0;
    DWORD Error;

    Interface = WlanOpenInterface(InterfaceGuid);
    if (Interface == NULL)
        return ERROR_BAD_UNIT;

    Error = WlanSetOid(Interface, OID_DOT11_DISCONNECT_REQUEST, &Reason, sizeof(Reason));
    CloseHandle(Interface);
    return Error;
}
