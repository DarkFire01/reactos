/*
 * PROJECT:     ReactOS WLAN service
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driving a native 802.11 adapter through NDISUIO and the dot11 OIDs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#include <strsafe.h>
#include <nuiouser.h>

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
