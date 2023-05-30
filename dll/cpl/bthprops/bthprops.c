/*
* PROJECT:     Safely Remove Hardware Applet
* LICENSE:     GPL - See COPYING in the top level directory
* FILE:        dll/cpl/bthprops/bthprops.c
* PURPOSE:     applet initialization
* PROGRAMMERS: Johannes Anderwald (johannes.anderwald@reactos.org)
*/

#include "bthprops.h"

#include <initguid.h>
#include <devguid.h>

#define NDEBUG
#include <debug.h>


LONG
APIENTRY
InitApplet(
    HWND hwnd,
    UINT uMsg,
    LPARAM wParam,
    LPARAM lParam)
{
    DPRINT("InitApplet()\n");


    // TODO
    return TRUE;
}


LONG
CALLBACK
CPlApplet(
    HWND hwndCPl,
    UINT uMsg,
    LPARAM lParam1,
    LPARAM lParam2)
{
    UINT i = (UINT)lParam1;

    switch(uMsg)
    {
        case CPL_INIT:
            return TRUE;

        case CPL_GETCOUNT:
            return NUM_APPLETS;

        case CPL_INQUIRE:
            break;

        case CPL_DBLCLK:
            return TRUE;
            break;

        case CPL_STARTWPARMSW:
            if (i < NUM_APPLETS)
              //  return Applets[i].AppletProc(hwndCPl, uMsg, lParam1, lParam2);
            break;
    }
    return FALSE;
}


INT
WINAPI
DllMain(
    HINSTANCE hinstDLL,
    DWORD dwReason,
    LPVOID lpvReserved)
{
    UNREFERENCED_PARAMETER(lpvReserved);

    switch (dwReason)
    {
        case DLL_PROCESS_ATTACH:
        case DLL_THREAD_ATTACH:
           // hApplet = hinstDLL;
            break;
    }
    return TRUE;
}
