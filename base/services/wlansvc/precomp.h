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

/* device.c: driving a native 802.11 adapter through NDISUIO */

struct _DOT11_SSID;
struct _WLAN_DOT11_BYTE_ARRAY;

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
WlanDisconnect(
    _In_ const GUID *InterfaceGuid);

#endif /* _WLANSVC_PCH_ */
