/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS kernel
 * PURPOSE:           Functions for creation and destruction of DCs
 * FILE:              win32ss/gdi/ntgdi/device.c
 * PROGRAMER:         Timo Kreuzer (timo.kreuzer@rectos.org)
 */

#include <win32k.h>

#define NDEBUG
#include <debug.h>

PDC defaultDCstate = NULL;
HSEMAPHORE hsemDriverMgmt = NULL;

VOID FASTCALL
IntGdiReferencePdev(PPDEVOBJ ppdev)
{
    if(!hsemDriverMgmt) hsemDriverMgmt = EngCreateSemaphore();
    EngAcquireSemaphore(hsemDriverMgmt);
    ppdev->cPdevRefs++;
    EngReleaseSemaphore(hsemDriverMgmt);    
}

VOID FASTCALL
IntGdiUnreferencePdev(PPDEVOBJ ppdev, DWORD CleanUpType)
{
    EngAcquireSemaphore(hsemDriverMgmt);
    ppdev->cPdevRefs--;
    if (!ppdev->cPdevRefs)
    {
       // Handle the destruction of pPDev or GDIDEVICE or PDEVOBJ or PDEV etc.
    }
    EngReleaseSemaphore(hsemDriverMgmt);
}

BOOL FASTCALL
IntCreatePrimarySurface(VOID)
{
    /* Create surface */
    PDEVOBJ_pSurface(gpmdev->ppdevGlobal);

    DPRINT("IntCreatePrimarySurface, ppdevGlobal=%p, ppdevGlobal->pSurface = %p\n",
        gpmdev->ppdevGlobal, gpmdev->ppdevGlobal->pSurface);

    // Init Primary Displays Device Capabilities.
    PDEVOBJ_vGetDeviceCaps(gpmdev->ppdevGlobal, &GdiHandleTable->DevCaps);

    return TRUE;
}

VOID FASTCALL
IntDestroyPrimarySurface(VOID)
{
    UNIMPLEMENTED_ONCE;
}

PPDEVOBJ FASTCALL
IntEnumHDev(VOID)
{
// I guess we will soon have more than one primary surface.
// This will do for now.
    return gpmdev->ppdevGlobal;
}


INT
APIENTRY
NtGdiDrawEscape(
    IN HDC hdc,
    IN INT iEsc,
    IN INT cjIn,
    IN OPTIONAL LPSTR pjIn)
{
    UNIMPLEMENTED_ONCE;
    return 0;
}


