/*
 * WoW64 syscall wrapping
 *
 * Copyright 2021 Alexandre Julliard
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

#ifndef __REACTOS__
#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "winternl.h"
#endif
#include "wow64win_private.h"

#ifdef __REACTOS__
#define SVC_(name,numArgs) extern NTSTATUS WINAPI wow64_Nt ## name( UINT *args );
#include "../../../win32ss/w32ksvc32.h"
#undef SVC_

#define SVC_(name,numArgs) static int Num ## name = __COUNTER__;
#include "../../../win32ss/w32ksvc32.h"
#undef SVC_

static NTSTATUS wow64win_Unimplemented(UINT* pArgs)
{
    DPRINT1("UNIMPLEMENTED SYSCALL\n");
    return STATUS_NOT_IMPLEMENTED;
}
#endif

#ifndef __REACTOS__
static void * const win32_syscalls[] =
#else
static void* win32_syscalls[] =
#endif
{
#ifdef __REACTOS__
#define SVC_(name, argc) NULL,
#include "../../../win32ss/w32ksvc32.h"
#undef SVC_
#else
#define SYSCALL_ENTRY(id,name,args) wow64_ ## name,
    ALL_SYSCALLS32
#undef SYSCALL_ENTRY
#endif
};

#ifdef __REACTOS__
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(*x))
#endif

static BYTE arguments[ARRAY_SIZE(win32_syscalls)] =
{
#ifdef __REACTOS__
#define SVC_(name, argc) argc,
#include "../../../win32ss/w32ksvc32.h"
#undef SVC_
#else
#define SYSCALL_ENTRY(id,name,args) args,
    ALL_SYSCALLS32
#undef SYSCALL_ENTRY
#endif
};

#ifdef __REACTOS__
__declspec(dllexport)
#endif
const SYSTEM_SERVICE_TABLE sdwhwin32 =
{
    (ULONG_PTR *)win32_syscalls,
    NULL,
    ARRAY_SIZE(win32_syscalls),
    arguments
}; 

#ifdef __REACTOS__

PSERVERINFO g_ServerInfo = NULL;

static BOOL wow64_NtGdiInit(UINT* pArgs)
{
    return NtGdiInit();
}

static VOID InitServiceTable(VOID)
{
#define IMPLEMENT_SERVICE(name) do { win32_syscalls[Num ## name] = (PVOID*)wow64_Nt ## name; } while(0)

    IMPLEMENT_SERVICE(UserProcessConnect);
    IMPLEMENT_SERVICE(UserInitializeClientPfnArrays);
    IMPLEMENT_SERVICE(GdiInit);
    IMPLEMENT_SERVICE(GdiCreateBitmap);
    IMPLEMENT_SERVICE(GdiCreateCompatibleDC);
    IMPLEMENT_SERVICE(GdiDeleteObjectApp);
    IMPLEMENT_SERVICE(UserCallNoParam);
    IMPLEMENT_SERVICE(UserCallOneParam);
    IMPLEMENT_SERVICE(UserCallHwndLock);
    IMPLEMENT_SERVICE(UserGetThreadState);
    IMPLEMENT_SERVICE(UserGetMessage);
    IMPLEMENT_SERVICE(UserMessageCall);
    IMPLEMENT_SERVICE(UserRegisterClassExWOW);
    IMPLEMENT_SERVICE(UserGetClassInfo);
    IMPLEMENT_SERVICE(UserCreateWindowEx);
    IMPLEMENT_SERVICE(UserShowWindow);
    IMPLEMENT_SERVICE(UserGetMessage);
    IMPLEMENT_SERVICE(UserDispatchMessage);
    IMPLEMENT_SERVICE(UserFindExistingCursorIcon);
    IMPLEMENT_SERVICE(UserBeginPaint);
    IMPLEMENT_SERVICE(UserEndPaint);
    IMPLEMENT_SERVICE(GdiPatBlt);
    IMPLEMENT_SERVICE(UserSetFocus);
    IMPLEMENT_SERVICE(GdiSelectBitmap);
    IMPLEMENT_SERVICE(GdiStretchDIBitsInternal);
    IMPLEMENT_SERVICE(GdiGetDeviceCaps);
    IMPLEMENT_SERVICE(GdiCreateCompatibleBitmap);
    IMPLEMENT_SERVICE(GdiGetAppClipBox);
    IMPLEMENT_SERVICE(GdiFlush);
    IMPLEMENT_SERVICE(UserDestroyWindow);
    IMPLEMENT_SERVICE(UserThunkedMenuItemInfo);
    IMPLEMENT_SERVICE(GdiCreateSolidBrush);
    
#undef IMPLEMENT_SERVICE
}

#endif

BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
#ifndef __REACTOS__
    LdrDisableThreadCalloutsForDll( inst );
    NtCurrentTeb()->Peb->KernelCallbackTable = user_callbacks;
#else
    NtCurrentPeb()->KernelCallbackTable = UserCallbacks;
    InitServiceTable();
#endif
    return TRUE;
}
