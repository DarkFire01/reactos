#ifndef _WLANSVC_PCH_
#define _WLANSVC_PCH_

#include <stdarg.h>

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H

#include <windef.h>
#include <winbase.h>
#include <winsvc.h>
#include <wlansvc_s.h>

#include <ndk/rtlfuncs.h>
#include <ndk/obfuncs.h>

typedef struct _WLANSVCHANDLE
{
    LIST_ENTRY WlanSvcHandleListEntry;
    DWORD      dwClientVersion;
} WLANSVCHANDLE, *PWLANSVCHANDLE;

/* device.c: driving a native 802.11 adapter through the Native WiFi filter */

HANDLE
WlanOpenInterface(
    _In_ const GUID *InterfaceGuid);

DWORD
WlanSetOid(
    _In_ HANDLE Interface,
    _In_ ULONG Oid,
    _In_reads_bytes_opt_(Length) PVOID Data,
    _In_ ULONG Length);

DWORD
WlanQueryOid(
    _In_ HANDLE Interface,
    _In_ ULONG Oid,
    _Out_writes_bytes_to_(Length, *Returned) PVOID Data,
    _In_ ULONG Length,
    _Out_ PULONG Returned);

struct _DOT11_SSID;
struct _WLAN_DOT11_BYTE_ARRAY;

DWORD
WlanEnumWifiInterfaces(
    _Outptr_result_maybenull_ PWLAN_INTERFACE_INFO_LIST *List);

DWORD
WlanGetAvailableNetworkList(
    _In_ const GUID *InterfaceGuid,
    _Outptr_result_maybenull_ PWLAN_AVAILABLE_NETWORK_LIST *NetworkList);

DWORD
WlanScan(
    _In_ const GUID *InterfaceGuid);

DWORD
WlanGetBssList(
    _In_ const GUID *InterfaceGuid,
    _Outptr_result_maybenull_ struct _WLAN_DOT11_BYTE_ARRAY **BssList);

DWORD
WlanConnect(
    _In_ const GUID *InterfaceGuid,
    _In_ struct _DOT11_SSID *Ssid);

DWORD
WlanConnectProfile(
    _In_ const GUID *InterfaceGuid,
    _In_ struct _DOT11_SSID *Ssid,
    _In_opt_ PCWSTR Profile);

DWORD
WlanDisconnect(
    _In_ const GUID *InterfaceGuid);

/* supplicant.c: the host WPA2-PSK 4-way handshake */

VOID
WlanSupplicantInitialize(VOID);

VOID
WlanStopSupplicant(
    _In_ const GUID *InterfaceGuid);

DWORD
WlanConnectWpa(
    _In_ const GUID *InterfaceGuid,
    _In_ struct _DOT11_SSID *Ssid,
    _In_ PCWSTR Passphrase);

#endif /* _WLANSVC_PCH_ */
