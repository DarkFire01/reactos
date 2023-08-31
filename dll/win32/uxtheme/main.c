/*
 * Win32 5.1 Themes
 *
 * Copyright (C) 2003 Kevin Koltzau
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

#include "uxthemep.h"
#include <debug.h>

/***********************************************************************/

/* For the moment, do nothing here. */
BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID lpv)
{
    TRACE("%p 0x%x %p: stub\n", hInstDLL, fdwReason, lpv);
    switch(fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hInstDLL);
            UXTHEME_InitSystem(hInstDLL);
            break;
        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}
typedef HANDLE HANIMATIONBUFFER;  // handle to a buffered paint animation

HRESULT
WINAPI
EndBufferedAnimation(
	HANIMATIONBUFFER hbpAnimation,
	BOOL fUpdateTarget
)
{
 //   FIXME("Stub (%p %u)\n", hbpAnimation, fUpdateTarget);

    return E_NOTIMPL;
}

/* Docs:
 * https://learn.microsoft.com/en-us/windows/win32/api/uxtheme/nf-uxtheme-getthemestream
 * @ UNIMPLEMENTED
 */
HRESULT
WINAPI
GetThemeStream(_In_ HTHEME        hTheme,
               _In_ int       iPartId,
               _In_ int       iStateId,
               _In_ int       iPropId,
               _Inout_ VOID      **ppvStream,
               _Inout_ DWORD     *pcbStream,
               _In_   HINSTANCE hInst)
{
    DPRINT1("Needs GetThemeStream - This WILL FAIL!\n");
    UNIMPLEMENTED;
    return 0;
}

