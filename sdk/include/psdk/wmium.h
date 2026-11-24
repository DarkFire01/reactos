/*
 * PROJECT:     ReactOS SDK
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     User mode access to WMI data blocks
 */

#ifndef _WMIUM_
#define _WMIUM_

#include <wmistr.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WMIAPI
#define WMIAPI __stdcall
#endif

typedef PVOID WMIHANDLE, *PWMIHANDLE, MOFHANDLE, *PMOFHANDLE;

ULONG
WMIAPI
WmiOpenBlock(
    _In_ GUID *Guid,
    _In_ ULONG DesiredAccess,
    _Out_ WMIHANDLE *DataBlockHandle);

ULONG
WMIAPI
WmiCloseBlock(
    _In_ WMIHANDLE DataBlockHandle);

ULONG
WMIAPI
WmiQueryAllDataA(
    _In_ WMIHANDLE DataBlockHandle,
    _Inout_ ULONG *BufferSize,
    _Out_writes_bytes_opt_(*BufferSize) LPVOID Buffer);

ULONG
WMIAPI
WmiQueryAllDataW(
    _In_ WMIHANDLE DataBlockHandle,
    _Inout_ ULONG *BufferSize,
    _Out_writes_bytes_opt_(*BufferSize) LPVOID Buffer);

ULONG
WMIAPI
WmiQuerySingleInstanceA(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ LPCSTR InstanceName,
    _Inout_ ULONG *BufferSize,
    _Out_writes_bytes_opt_(*BufferSize) LPVOID Buffer);

ULONG
WMIAPI
WmiQuerySingleInstanceW(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ LPCWSTR InstanceName,
    _Inout_ ULONG *BufferSize,
    _Out_writes_bytes_opt_(*BufferSize) LPVOID Buffer);

#ifdef UNICODE
#define WmiQueryAllData WmiQueryAllDataW
#define WmiQuerySingleInstance WmiQuerySingleInstanceW
#else
#define WmiQueryAllData WmiQueryAllDataA
#define WmiQuerySingleInstance WmiQuerySingleInstanceA
#endif

#ifdef __cplusplus
}
#endif

#endif /* _WMIUM_ */
