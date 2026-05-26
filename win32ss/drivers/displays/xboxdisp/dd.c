/*
 * PROJECT:     Xbox NV2A accelerated GDI display driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     DirectDraw enable / disable stubs
 *
 * Forked from framebuf/ddenable.c — we expose the symbols GDI expects but
 * advertise no optional callbacks.  DirectDraw acceleration would attach
 * here later if we ever ship a full xboxdd.dll.
 */

#include "xboxdisp.h"

BOOL APIENTRY
DrvEnableDirectDraw(DHPDEV dhpdev,
                    DD_CALLBACKS *pCallbacks,
                    DD_SURFACECALLBACKS *pSurfaceCallbacks,
                    DD_PALETTECALLBACKS *pPaletteCallbacks)
{
    RtlZeroMemory(pCallbacks, sizeof(*pCallbacks));
    RtlZeroMemory(pSurfaceCallbacks, sizeof(*pSurfaceCallbacks));
    RtlZeroMemory(pPaletteCallbacks, sizeof(*pPaletteCallbacks));

    pCallbacks->dwSize        = sizeof(*pCallbacks);
    pSurfaceCallbacks->dwSize = sizeof(*pSurfaceCallbacks);
    pPaletteCallbacks->dwSize = sizeof(*pPaletteCallbacks);
    return TRUE;
}

VOID APIENTRY
DrvDisableDirectDraw(DHPDEV dhpdev)
{
}
