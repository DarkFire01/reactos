/*
 * Kernelbase internal definitions
 *
 * Copyright 2019 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_KERNELBASE_H
#define __WINE_KERNELBASE_H

#include "windef.h"
#include "winbase.h"

#ifdef __REACTOS__
#include "synchacks.h"

/* Wine sources call Win32 names; spec exports use kernelbase_* / KERNELBASE_* */
HANDLE WINAPI kernelbase_GetCurrentProcess(void);
DWORD WINAPI kernelbase_GetCurrentProcessId(void);
HANDLE WINAPI kernelbase_GetProcessHeap(void);
HANDLE WINAPI kernelbase_GetCurrentThread(void);
DWORD WINAPI kernelbase_GetCurrentThreadId(void);
DWORD WINAPI kernelbase_GetLastError(void);

VOID NTAPI RtlSetLastWin32Error(ULONG err);
VOID NTAPI RtlExitUserThread(NTSTATUS status);

#undef GetCurrentProcess
#define GetCurrentProcess kernelbase_GetCurrentProcess
#undef GetCurrentProcessId
#define GetCurrentProcessId kernelbase_GetCurrentProcessId
#undef GetProcessHeap
#define GetProcessHeap kernelbase_GetProcessHeap
#undef GetCurrentThread
#define GetCurrentThread kernelbase_GetCurrentThread
#undef GetCurrentThreadId
#define GetCurrentThreadId kernelbase_GetCurrentThreadId
#undef GetLastError
#define GetLastError kernelbase_GetLastError
#undef SetLastError
#define SetLastError RtlSetLastWin32Error

#undef lstrcpynW
#define lstrcpynW KERNELBASE_lstrcpynW
#undef lstrcpynA
#define lstrcpynA KERNELBASE_lstrcpynA
#undef lstrlenW
#define lstrlenW KERNELBASE_lstrlenW
#undef lstrlenA
#define lstrlenA KERNELBASE_lstrlenA

LPWSTR WINAPI KERNELBASE_lstrcpynW(LPWSTR dst, LPCWSTR src, INT n);
LPSTR WINAPI KERNELBASE_lstrcpynA(LPSTR dst, LPCSTR src, INT n);
INT WINAPI KERNELBASE_lstrlenW(LPCWSTR str);
INT WINAPI KERNELBASE_lstrlenA(LPCSTR str);

#include <wchar.h>
#include <string.h>
#ifndef lstrcpyW
#define lstrcpyW(dst, src) wcscpy((dst), (src))
#endif
#ifndef lstrcpyA
#define lstrcpyA(dst, src) strcpy((dst), (src))
#endif
#ifndef lstrcatW
#define lstrcatW(dst, src) wcscat((dst), (src))
#endif

#undef ExitThread
#define ExitThread(code) RtlExitUserThread((NTSTATUS)(code))

#endif /* __REACTOS__ */


struct pseudo_console
{
    HANDLE signal;
    HANDLE reference;
    HANDLE process;
};

extern WCHAR *file_name_AtoW( LPCSTR name, BOOL alloc );
extern DWORD file_name_WtoA( LPCWSTR src, INT srclen, LPSTR dest, INT destlen );
extern void init_global_data(void);
extern void init_startup_info( RTL_USER_PROCESS_PARAMETERS *params );
extern void init_locale( HMODULE module );
extern void init_console(void);

extern const WCHAR windows_dir[];
extern const WCHAR system_dir[];

static const BOOL is_win64 = (sizeof(void *) > sizeof(int));
extern BOOL is_wow64;

static inline BOOL set_ntstatus( NTSTATUS status )
{
    if (status) SetLastError( RtlNtStatusToDosError( status ));
    return !status;
}

/* make the kernel32 names available */
#define HeapAlloc(heap, flags, size) RtlAllocateHeap(heap, flags, size)
#define HeapReAlloc(heap, flags, ptr, size) RtlReAllocateHeap(heap, flags, ptr, size)
#define HeapFree(heap, flags, ptr) RtlFreeHeap(heap, flags, ptr)

#endif /* __WINE_KERNELBASE_H */
