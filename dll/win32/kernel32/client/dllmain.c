/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/dllmain.c
 * PURPOSE:         Initialization
 * PROGRAMMERS:     Ariadne (ariadne@xs4all.nl)
 *                  Aleksey Bragin (aleksey@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/
SYSTEM_INFO SystemInfo;

/* FUNCTIONS *****************************************************************/

BOOL
WINAPI
DllMain(HANDLE hDll,
        DWORD dwReason,
        LPVOID lpReserved)
{
    DPRINT("DllMain(hInst %p, dwReason %lu)\n",
           hDll, dwReason);

    switch (dwReason)
    {
        case DLL_PROCESS_ATTACH:
        {
            LdrDisableThreadCalloutsForDll((PVOID)hDll);
            GetSystemInfo(&SystemInfo);
            kernelbase_global_data = KernelBaseGetGlobalData();
            break;
        }

        default:
            break;
    }

    return TRUE;
}

/* EOF */
