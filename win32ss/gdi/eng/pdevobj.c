/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * PURPOSE:          Support for physical devices
 * FILE:             win32ss/gdi/eng/pdevobj.c
 * PROGRAMERS:       Timo Kreuzer (timo.kreuzer@reactos.org)
 *                   Oleg Dubinskiy (oleg.dubinskij30@gmail.com)
 */

#include <win32k.h>
#define NDEBUG
#include <debug.h>
DBG_DEFAULT_CHANNEL(EngPDev);

static PPDEVOBJ gppdevList = NULL;
static HSEMAPHORE ghsemPDEV;


BOOL
APIENTRY
MultiEnableDriver(
    _In_ ULONG iEngineVersion,
    _In_ ULONG cj,
    _Inout_bytecount_(cj) PDRVENABLEDATA pded);

extern DRVFN gPanDispDrvFn[];
extern ULONG gPanDispDrvCount;

CODE_SEG("INIT")
NTSTATUS
NTAPI
InitPDEVImpl(VOID)
{
    ghsemPDEV = EngCreateSemaphore();
    if (!ghsemPDEV) return STATUS_INSUFFICIENT_RESOURCES;
    return STATUS_SUCCESS;
}

#if DBG
PPDEVOBJ
NTAPI
DbgLookupDHPDEV(DHPDEV dhpdev)
{
    PPDEVOBJ ppdev;

    /* Lock PDEV list */
    EngAcquireSemaphoreShared(ghsemPDEV);

    /* Walk through the list of PDEVs */
    for (ppdev = gppdevList;  ppdev; ppdev = ppdev->ppdevNext)
    {
        /* Compare with the given DHPDEV */
        if (ppdev->dhpdev == dhpdev) break;
    }

    /* Unlock PDEV list */
    EngReleaseSemaphore(ghsemPDEV);

    return ppdev;
}
#endif

/**
 * @brief Tell the CDD its surface is new, so it redraws the whole screen.
 *
 * The CDD does not track damage until win32k asserts its mode carrying this command, and that
 * same assert is what seeds the damage region with the full screen. Until then the region stays
 * empty and every present the CDD builds is thrown away, because it only reaches dxgkrnl when
 * EngGetRgnData reports more than a bare RGNDATAHEADER. Reference win32kbase
 * GreSuspendDirectDraw, which leaves the command on the PDEV across DrvAssertMode.
 */
VOID
NTAPI
PDEVOBJ_vResetGdiOutput(
    _Inout_ PPDEVOBJ ppdev)
{
    if ((ppdev->flFlags & (PDEV_META_DEVICE | PDEV_DISABLED)) != 0)
        return;

    /* Only a WDDM device has a CDD behind it to tell */
    if ((ppdev->pGraphicsDevice == NULL) || (ppdev->pGraphicsDevice->DxgAdapter == NULL))
        return;

    if (ppdev->pfn.AssertMode == NULL)
        return;

    ppdev->ulW32kCommand = W32KCMD_RESET_GDI_OUTPUT;
    ppdev->pfn.AssertMode(ppdev->dhpdev, TRUE);
    ppdev->ulW32kCommand = 0;
}

/** @brief Does this PDEV drive the given adapter's source, and is it in a state to be asserted? */
static
BOOL
PDEVOBJ_bDrivesAdapterSource(
    _In_ PPDEVOBJ ppdev,
    _In_ PVOID pAdapter,
    _In_ ULONG cSources)
{
    if ((ppdev->flFlags & (PDEV_META_DEVICE | PDEV_DISABLED)) != 0)
        return FALSE;

    if (ppdev->pGraphicsDevice == NULL)
        return FALSE;

    if (ppdev->pGraphicsDevice->DxgAdapter != pAdapter)
        return FALSE;

    return ppdev->pGraphicsDevice->VidPnSourceId < cSources;
}

/**
 * @brief Assert the mode of every display the given adapter drives, telling the CDD why.
 *
 * dxgkrnl calls this through the engine interface when a video present source gains or loses its
 * GDI output. Every matching source goes down first, then the ones the CDD owns come back up; the
 * command left on the PDEV across each DrvAssertMode is what the CDD reads back through
 * W32kCddGetWin32kCommand. Reference win32kbase DxgkEngAssertGdiOutput.
 *
 * @param[in] pAdapter
 * dxgkrnl's adapter whose sources are being asserted.
 *
 * @param[in] pCddStates
 * One byte per video present source, non-zero where the CDD drives the output.
 *
 * @param[in] cSources
 * Number of entries in @p pCddStates.
 *
 * @param[out] pbResetPointer
 * Non-zero when at least one source came back up, so the caller redraws the pointer.
 *
 * @return TRUE when every DrvAssertMode succeeded.
 */
BOOL
NTAPI
PDEVOBJ_bAssertGdiOutput(
    _In_ PVOID pAdapter,
    _In_reads_(cSources) const UCHAR *pCddStates,
    _In_ ULONG cSources,
    _Out_ PUCHAR pbResetPointer)
{
    PPDEVOBJ ppdev;
    BOOL     bResult = TRUE;
    BOOL     bEnabled = FALSE;
    BOOL     bAsserted;

    EngAcquireSemaphoreShared(ghsemPDEV);

    for (ppdev = gppdevList; ppdev != NULL; ppdev = ppdev->ppdevNext)
    {
        if (!PDEVOBJ_bDrivesAdapterSource(ppdev, pAdapter, cSources))
            continue;

        ppdev->ulW32kCommand = pCddStates[ppdev->pGraphicsDevice->VidPnSourceId] ?
                               W32KCMD_CDD_ENABLING : W32KCMD_CDD_DISABLING;

        bAsserted = (ppdev->pfn.AssertMode != NULL) &&
                    ppdev->pfn.AssertMode(ppdev->dhpdev, FALSE);

        ppdev->ulW32kCommand = 0;
        bResult = bResult && bAsserted;
    }

    for (ppdev = gppdevList; ppdev != NULL; ppdev = ppdev->ppdevNext)
    {
        if (!PDEVOBJ_bDrivesAdapterSource(ppdev, pAdapter, cSources))
            continue;

        if (!pCddStates[ppdev->pGraphicsDevice->VidPnSourceId])
            continue;

        ppdev->ulW32kCommand = W32KCMD_ASSERT_GDI_OUTPUT;

        bAsserted = (ppdev->pfn.AssertMode != NULL) &&
                    ppdev->pfn.AssertMode(ppdev->dhpdev, TRUE);

        ppdev->ulW32kCommand = 0;
        bResult = bResult && bAsserted;
        bEnabled = TRUE;
    }

    EngReleaseSemaphore(ghsemPDEV);

    *pbResetPointer = bEnabled ? 1 : 0;
    return bResult;
}

/**
 * @brief Let each display driver that asked for it catch up on GDI output, the way the
 *        periodic sync timer and GdiFlush do. CDD only presents what it drew when told to.
 *
 * @param flCaps2 GCAPS2_SYNCTIMER or GCAPS2_SYNCFLUSH, the reason for the call.
 */
VOID
NTAPI
PDEVOBJ_vSynchronizeDrivers(
    _In_ FLONG flCaps2)
{
    PPDEVOBJ ppdev;
    PSURFACE psurf;
    FLONG fl;
    BOOL bLock;

    fl = (flCaps2 & GCAPS2_SYNCTIMER) ? DSS_TIMER_EVENT : DSS_FLUSH_EVENT;

    EngAcquireSemaphoreShared(ghsemPDEV);

    for (ppdev = gppdevList; ppdev != NULL; ppdev = ppdev->ppdevNext)
    {
        if (!(ppdev->devinfo.flGraphicsCaps2 & flCaps2))
            continue;

        if (ppdev->flFlags & PDEV_DISABLED)
            continue;

        psurf = ppdev->pSurface;
        if ((psurf == NULL) || !(psurf->flags & HOOK_SYNCHRONIZE))
            continue;

        /* A surface open to shared access is synchronized by its driver */
        bLock = !(psurf->flags & SHAREACCESS_SURFACE);
        if (bLock)
            EngAcquireSemaphore(ppdev->hsemDevLock);

        if (ppdev->pfn.SynchronizeSurface)
            ppdev->pfn.SynchronizeSurface(&psurf->SurfObj, NULL, fl);
        else if (ppdev->pfn.Synchronize)
            ppdev->pfn.Synchronize(ppdev->dhpdev, NULL);

        if (bLock)
            EngReleaseSemaphore(ppdev->hsemDevLock);
    }

    EngReleaseSemaphore(ghsemPDEV);
}

PPDEVOBJ
PDEVOBJ_AllocPDEV(VOID)
{
    PPDEVOBJ ppdev;

    ppdev = ExAllocatePoolWithTag(PagedPool, sizeof(PDEVOBJ), GDITAG_PDEV);
    if (!ppdev)
        return NULL;

    RtlZeroMemory(ppdev, sizeof(PDEVOBJ));

    ppdev->hsemDevLock = EngCreateSemaphore();
    if (ppdev->hsemDevLock == NULL)
    {
        ExFreePoolWithTag(ppdev, GDITAG_PDEV);
        return NULL;
    }

    /* Allocate EDD_DIRECTDRAW_GLOBAL for our ReactX driver */
    ppdev->pEDDgpl = ExAllocatePoolWithTag(PagedPool, sizeof(EDD_DIRECTDRAW_GLOBAL), GDITAG_PDEV);
    if (ppdev->pEDDgpl)
        RtlZeroMemory(ppdev->pEDDgpl, sizeof(EDD_DIRECTDRAW_GLOBAL));

    ppdev->cPdevRefs = 1;

    return ppdev;
}

static
VOID
PDEVOBJ_vDeletePDEV(
    PPDEVOBJ ppdev)
{
    EngDeleteSemaphore(ppdev->hsemDevLock);
    if (ppdev->pdmwDev)
        ExFreePoolWithTag(ppdev->pdmwDev, GDITAG_DEVMODE);
    if (ppdev->pEDDgpl)
        ExFreePoolWithTag(ppdev->pEDDgpl, GDITAG_PDEV);
    ExFreePoolWithTag(ppdev, GDITAG_PDEV);
}

VOID
NTAPI
PDEVOBJ_vRelease(
    _Inout_ PPDEVOBJ ppdev)
{
    /* Lock loader */
    EngAcquireSemaphore(ghsemPDEV);

    /* Decrease reference count */
    InterlockedDecrement(&ppdev->cPdevRefs);
    ASSERT(ppdev->cPdevRefs >= 0);

    /* Check if references are left */
    if (ppdev->cPdevRefs == 0)
    {
        /* Do we have a surface? */
        if (ppdev->pSurface)
        {
            /* Release the surface and let the driver free it */
            SURFACE_ShareUnlockSurface(ppdev->pSurface);
            TRACE("DrvDisableSurface(dhpdev %p)\n", ppdev->dhpdev);
            ppdev->pfn.DisableSurface(ppdev->dhpdev);
        }

        /* Do we have a palette? */
        if (ppdev->ppalSurf)
        {
            PALETTE_ShareUnlockPalette(ppdev->ppalSurf);
        }

        /* Check if the PDEV was enabled */
        if (ppdev->dhpdev != NULL)
        {
            /* Disable the PDEV */
            TRACE("DrvDisablePDEV(dhpdev %p)\n", ppdev->dhpdev);
            ppdev->pfn.DisablePDEV(ppdev->dhpdev);
        }

        /* Remove it from list */
        if (ppdev == gppdevList)
        {
            gppdevList = ppdev->ppdevNext;
        }
        else if (gppdevList)
        {
            PPDEVOBJ ppdevCurrent = gppdevList;
            BOOL found = FALSE;
            while (!found && ppdevCurrent->ppdevNext)
            {
                if (ppdevCurrent->ppdevNext == ppdev)
                    found = TRUE;
                else
                    ppdevCurrent = ppdevCurrent->ppdevNext;
            }
            if (found)
                ppdevCurrent->ppdevNext = ppdev->ppdevNext;
        }

        /* Unload display driver */
        EngUnloadImage(ppdev->pldev);

        /* Free it */
        PDEVOBJ_vDeletePDEV(ppdev);
    }

    /* Unlock loader */
    EngReleaseSemaphore(ghsemPDEV);
}

BOOL
NTAPI
PDEVOBJ_bEnablePDEV(
    _In_ PPDEVOBJ ppdev,
    _In_ PDEVMODEW pdevmode,
    _In_ PWSTR pwszLogAddress)
{
    PFN_DrvEnablePDEV pfnEnablePDEV;
    ULONG i;

    /* Get the DrvEnablePDEV function */
    pfnEnablePDEV = ppdev->pfn.EnablePDEV;

    /* Call the drivers DrvEnablePDEV function */
    TRACE("DrvEnablePDEV(pdevmode %p (%dx%dx%d %d Hz) hdev %p (%S))\n",
        pdevmode,
        ppdev->pGraphicsDevice ? pdevmode->dmPelsWidth : 0,
        ppdev->pGraphicsDevice ? pdevmode->dmPelsHeight : 0,
        ppdev->pGraphicsDevice ? pdevmode->dmBitsPerPel : 0,
        ppdev->pGraphicsDevice ? pdevmode->dmDisplayFrequency : 0,
        ppdev,
        ppdev->pGraphicsDevice ? ppdev->pGraphicsDevice->szNtDeviceName : L"");
    ppdev->dhpdev = pfnEnablePDEV(pdevmode,
                                  pwszLogAddress,
                                  HS_DDI_MAX,
                                  ppdev->ahsurf,
                                  sizeof(GDIINFO),
                                  (PULONG)&ppdev->gdiinfo,
                                  sizeof(DEVINFO),
                                  &ppdev->devinfo,
                                  (HDEV)ppdev,
                                  ppdev->pGraphicsDevice ? ppdev->pGraphicsDevice->pwszDescription : NULL,
                                  ppdev->pGraphicsDevice ? ppdev->pGraphicsDevice->DeviceObject : NULL);
    TRACE("DrvEnablePDEV(pdevmode %p hdev %p) => dhpdev %p\n", pdevmode, ppdev, ppdev->dhpdev);
    if (ppdev->dhpdev == NULL)
    {
        ERR("Failed to enable PDEV\n");
        return FALSE;
    }

    /* Fix up some values */
    if (ppdev->gdiinfo.ulLogPixelsX == 0)
        ppdev->gdiinfo.ulLogPixelsX = 96;

    if (ppdev->gdiinfo.ulLogPixelsY == 0)
        ppdev->gdiinfo.ulLogPixelsY = 96;

    /* Set raster caps */
    ppdev->gdiinfo.flRaster = RC_OP_DX_OUTPUT | RC_GDI20_OUTPUT | RC_BIGFONT;
    if ((ppdev->gdiinfo.ulTechnology != DT_PLOTTER) && (ppdev->gdiinfo.ulTechnology != DT_CHARSTREAM))
        ppdev->gdiinfo.flRaster |= RC_STRETCHDIB | RC_STRETCHBLT | RC_DIBTODEV | RC_DI_BITMAP | RC_BITMAP64 | RC_BITBLT;
    if (ppdev->gdiinfo.ulTechnology == DT_RASDISPLAY)
        ppdev->gdiinfo.flRaster |= RC_FLOODFILL;
    if (ppdev->devinfo.flGraphicsCaps & GCAPS_PALMANAGED)
        ppdev->gdiinfo.flRaster |= RC_PALETTE;

    /* Setup Palette */
    ppdev->ppalSurf = PALETTE_ShareLockPalette(ppdev->devinfo.hpalDefault);

    /* Setup hatch brushes */
    for (i = 0; i < HS_DDI_MAX; i++)
    {
        if (ppdev->ahsurf[i] == NULL)
            ppdev->ahsurf[i] = gahsurfHatch[i];
    }

    TRACE("PDEVOBJ_bEnablePDEV - dhpdev = %p\n", ppdev->dhpdev);

    return TRUE;
}

VOID
NTAPI
PDEVOBJ_vCompletePDEV(
    PPDEVOBJ ppdev)
{
    /* Call the drivers DrvCompletePDEV function */
    TRACE("DrvCompletePDEV(dhpdev %p hdev %p)\n", ppdev->dhpdev, ppdev);
    ppdev->pfn.CompletePDEV(ppdev->dhpdev, (HDEV)ppdev);
}

static
VOID
PDEVOBJ_vFilterDriverHooks(
    _In_ PPDEVOBJ ppdev)
{
    PLDEVOBJ pldev = ppdev->pldev;
    ULONG dwAccelerationLevel = ppdev->dwAccelerationLevel;

    if (!pldev->pGdiDriverInfo)
        return;
    if (pldev->ldevtype != LDEV_DEVICE_DISPLAY)
        return;

    if (dwAccelerationLevel >= 1)
    {
        ppdev->apfn[INDEX_DrvSetPointerShape] = NULL;
        ppdev->apfn[INDEX_DrvCreateDeviceBitmap] = NULL;
    }

    if (dwAccelerationLevel >= 2)
    {
        /* Remove sophisticated display accelerations */
        ppdev->pSurface->flags &= ~(HOOK_STRETCHBLT |
                                    HOOK_FILLPATH |
                                    HOOK_GRADIENTFILL |
                                    HOOK_LINETO |
                                    HOOK_ALPHABLEND |
                                    HOOK_TRANSPARENTBLT);
    }

    if (dwAccelerationLevel >= 3)
    {
        /* Disable DirectDraw and Direct3D accelerations */
        /* FIXME: need to call DxDdSetAccelLevel */
        UNIMPLEMENTED;
    }

    if (dwAccelerationLevel >= 4)
    {
        /* Remove almost all display accelerations */
        ppdev->pSurface->flags &= ~HOOK_FLAGS |
                                   HOOK_BITBLT |
                                   HOOK_COPYBITS |
                                   HOOK_TEXTOUT |
                                   HOOK_STROKEPATH |
                                   HOOK_SYNCHRONIZE;

    }

    if (dwAccelerationLevel >= 5)
    {
        /* Disable all display accelerations */
        /* (nothing to do. Already handled in PDEVOBJ_Create) */
    }
}

PSURFACE
NTAPI
PDEVOBJ_pSurface(
    PPDEVOBJ ppdev)
{
    HSURF hsurf;

    /* Check if there is no surface for this PDEV yet */
    if (ppdev->pSurface == NULL)
    {
        /* Call the drivers DrvEnableSurface */
        TRACE("DrvEnableSurface(dhpdev %p)\n", ppdev->dhpdev);
        hsurf = ppdev->pfn.EnableSurface(ppdev->dhpdev);
        TRACE("DrvEnableSurface(dhpdev %p) => hsurf %p\n", ppdev->dhpdev, hsurf);
        if (hsurf== NULL)
        {
            ERR("Failed to create PDEV surface!\n");
            return NULL;
        }

        /* Get a reference to the surface */
        ppdev->pSurface = SURFACE_ShareLockSurface(hsurf);
        NT_ASSERT(ppdev->pSurface != NULL);

        /* The CDD starts tracking damage only once its mode is asserted for a fresh surface */
        PDEVOBJ_vResetGdiOutput(ppdev);
    }

    /* Increment reference count */
    GDIOBJ_vReferenceObjectByPointer(&ppdev->pSurface->BaseObject);

    return ppdev->pSurface;
}

BOOL
PDEVOBJ_bEnableDirectDraw(
    _Inout_ PPDEVOBJ ppdev)
{
    PGD_DXDDENABLEDIRECTDRAW pfnDdEnableDirectDraw = (PGD_DXDDENABLEDIRECTDRAW)gpDxFuncs[DXG_INDEX_DxDdEnableDirectDraw].pfn;
    BOOL Success;

    /*
     * gpDxFuncs is filled by DxDdStartupDxGraphics (dxg.sys), which is deliberately not started on
     * a WDDM system - the legacy DirectDraw HAL drives an XPDM driver's DdXxx callbacks and the CDD
     * has none. A PDEV without legacy DirectDraw is a normal, working PDEV, so report success.
     */
    if (pfnDdEnableDirectDraw == NULL)
    {
        TRACE("DxDdEnableDirectDraw: no legacy DirectDraw (WDDM path)\n");
        return TRUE;
    }

    /* Enable DirectDraw */
    TRACE("DxDdEnableDirectDraw(ppdev %p)\n", ppdev);
    Success = pfnDdEnableDirectDraw((HDEV)ppdev, TRUE);
    TRACE("DxDdEnableDirectDraw(ppdev %p) => %d\n", ppdev, Success);

    return Success;
}

VOID
PDEVOBJ_vResumeDirectDraw(
    _Inout_ PPDEVOBJ ppdev)
{
    PGD_DXDDRESUMEDIRECTDRAW pfnDdResumeDirectDraw = (PGD_DXDDRESUMEDIRECTDRAW)gpDxFuncs[DXG_INDEX_DxDdResumeDirectDraw].pfn;

    /* No legacy DirectDraw on the WDDM path - see PDEVOBJ_bEnableDirectDraw. */
    if (pfnDdResumeDirectDraw == NULL)
        return;

    /* Resume DirectDraw after mode change */
    TRACE("DxDdResumeDirectDraw(ppdev %p)\n", ppdev);
    pfnDdResumeDirectDraw((HDEV)ppdev, 0);
}

VOID
PDEVOBJ_vSuspendDirectDraw(
    _Inout_ PPDEVOBJ ppdev)
{
    PGD_DXDDSUSPENDDIRECTDRAW pfnDdSuspendDirectDraw = (PGD_DXDDSUSPENDDIRECTDRAW)gpDxFuncs[DXG_INDEX_DxDdSuspendDirectDraw].pfn;

    /* No legacy DirectDraw on the WDDM path - see PDEVOBJ_bEnableDirectDraw. */
    if (pfnDdSuspendDirectDraw == NULL)
        return;

    /* Suspend DirectDraw for mode change */
    TRACE("DxDdSuspendDirectDraw(ppdev %p)\n", ppdev);
    pfnDdSuspendDirectDraw((HDEV)ppdev, 0);
}

VOID
PDEVOBJ_vSwitchDirectDraw(
    _Inout_ PPDEVOBJ ppdev,
    _Inout_ PPDEVOBJ ppdev2)
{
    PGD_DXDDDYNAMICMODECHANGE pfnDdDynamicModeChange = (PGD_DXDDDYNAMICMODECHANGE)gpDxFuncs[DXG_INDEX_DxDdDynamicModeChange].pfn;

    /* No legacy DirectDraw on the WDDM path - see PDEVOBJ_bEnableDirectDraw. */
    if (pfnDdDynamicModeChange == NULL)
        return;

    /* Switch DirectDraw instances between the PDEVs */
    TRACE("DxDdDynamicModeChange(ppdev %p, ppdev2 %p)\n", ppdev, ppdev2);
    pfnDdDynamicModeChange((HDEV)ppdev, (HDEV)ppdev2, 0);
}

VOID
PDEVOBJ_vEnableDisplay(
    _Inout_ PPDEVOBJ ppdev)
{
    BOOL assertVal;

    if (!(ppdev->flFlags & PDEV_DISABLED))
        return;

    /* Try to enable display until success */
    do
    {
        TRACE("DrvAssertMode(dhpdev %p, TRUE)\n", ppdev->dhpdev);
        assertVal = ppdev->pfn.AssertMode(ppdev->dhpdev, TRUE);
        TRACE("DrvAssertMode(dhpdev %p, TRUE) => %d\n", ppdev->dhpdev, assertVal);
    } while (!assertVal);

    ppdev->flFlags &= ~PDEV_DISABLED;
}

BOOL
PDEVOBJ_bDisableDisplay(
    _Inout_ PPDEVOBJ ppdev)
{
    BOOL assertVal;

    if (ppdev->flFlags & PDEV_DISABLED)
        return TRUE;

    PDEVOBJ_vSuspendDirectDraw(ppdev);

    TRACE("DrvAssertMode(dhpdev %p, FALSE)\n", ppdev->dhpdev);
    assertVal = ppdev->pfn.AssertMode(ppdev->dhpdev, FALSE);
    TRACE("DrvAssertMode(dhpdev %p, FALSE) => %d\n", ppdev->dhpdev, assertVal);

    if (assertVal)
        ppdev->flFlags |= PDEV_DISABLED;

    return assertVal;
}

VOID
NTAPI
PDEVOBJ_vRefreshModeList(
    PPDEVOBJ ppdev)
{
    PGRAPHICS_DEVICE pGraphicsDevice;
    PDEVMODEINFO pdminfo, pdmiNext;

    /* Lock the PDEV */
    EngAcquireSemaphore(ppdev->hsemDevLock);

    pGraphicsDevice = ppdev->pGraphicsDevice;

    /* Clear out the modes */
    for (pdminfo = pGraphicsDevice->pdevmodeInfo;
         pdminfo;
         pdminfo = pdmiNext)
    {
        pdmiNext = pdminfo->pdmiNext;
        ExFreePoolWithTag(pdminfo, GDITAG_DEVMODE);
    }
    pGraphicsDevice->pdevmodeInfo = NULL;

    /* A device the display configuration put on the desktop was never probed, so it has no list yet */
    if (pGraphicsDevice->pDevModeList != NULL)
        ExFreePoolWithTag(pGraphicsDevice->pDevModeList, GDITAG_GDEVICE);
    pGraphicsDevice->pDevModeList = NULL;
    pGraphicsDevice->cDevModes = 0;

    /* Update available display mode list */
    LDEVOBJ_bBuildDevmodeList(pGraphicsDevice);

    /* Unlock PDEV */
    EngReleaseSemaphore(ppdev->hsemDevLock);
}

PPDEVOBJ
PDEVOBJ_Create(
    _In_opt_ PGRAPHICS_DEVICE pGraphicsDevice,
    _In_opt_ PDEVMODEW pdm,
    _In_ ULONG dwAccelerationLevel,
    _In_ ULONG ldevtype)
{
    PPDEVOBJ ppdev, ppdevMatch = NULL;
    PLDEVOBJ pldev;
    PSURFACE pSurface;

    TRACE("PDEVOBJ_Create(%p %p %d)\n", pGraphicsDevice, pdm, ldevtype);

    if (ldevtype != LDEV_DEVICE_META)
    {
        ASSERT(pGraphicsDevice);
        ASSERT(pdm);
        /* Search if we already have a PPDEV with the required characteristics.
         * We will compare the graphics device, the devmode and the desktop
         */
        for (ppdev = gppdevList; ppdev; ppdev = ppdev->ppdevNext)
        {
            if (ppdev->pGraphicsDevice == pGraphicsDevice)
            {
                PDEVOBJ_vReference(ppdev);

                if (RtlEqualMemory(pdm, ppdev->pdmwDev, sizeof(DEVMODEW)) &&
                    ppdev->dwAccelerationLevel == dwAccelerationLevel)
                {
                    PDEVOBJ_vReference(ppdev);
                    ppdevMatch = ppdev;
                }
                else
                {
                    PDEVOBJ_bDisableDisplay(ppdev);
                }

                PDEVOBJ_vRelease(ppdev);
            }
        }

        if (ppdevMatch)
        {
            PDEVOBJ_vEnableDisplay(ppdevMatch);

            return ppdevMatch;
        }
    }

    /* Try to get a display driver */
    if (ldevtype == LDEV_DEVICE_META)
    {
        pldev = LDEVOBJ_pLoadInternal(MultiEnableDriver, ldevtype);
    }
    else
    {
        /*
         * No WDDM special case here: a WDDM adapter advertises cdd_ms as its InstalledDisplayDrivers
         * through the \Device\VideoN key dxgkrnl publishes, so the ordinary path already selects the
         * CDD for it - and only for it. Forcing cdd_ms whenever any WDDM adapter existed also
         * hijacked unrelated display devices (the VGA one), binding the CDD to a graphics device
         * that is not the adapter it actually drives.
         */
        pldev = LDEVOBJ_pLoadDriver(pdm->dmDeviceName, ldevtype);
    }
    if (!pldev)
    {
        ERR("Could not load display driver '%S'\n",
             (ldevtype == LDEV_DEVICE_META) ? L"" : pdm->dmDeviceName);
        return NULL;
    }

    /* Allocate a new PDEVOBJ */
    ppdev = PDEVOBJ_AllocPDEV();
    if (!ppdev)
    {
        ERR("failed to allocate a PDEV\n");
        return NULL;
    }

    if (ldevtype != LDEV_DEVICE_META)
    {
        ppdev->pGraphicsDevice = pGraphicsDevice;

        // DxEngGetHdevData asks for Graphics DeviceObject in hSpooler field
        ppdev->hSpooler = ppdev->pGraphicsDevice->DeviceObject;

        /* Keep selected resolution */
        if (ppdev->pdmwDev)
            ExFreePoolWithTag(ppdev->pdmwDev, GDITAG_DEVMODE);
        ppdev->pdmwDev = ExAllocatePoolWithTag(PagedPool, pdm->dmSize + pdm->dmDriverExtra, GDITAG_DEVMODE);
        if (ppdev->pdmwDev)
        {
            RtlCopyMemory(ppdev->pdmwDev, pdm, pdm->dmSize + pdm->dmDriverExtra);
            /* FIXME: this must be done in a better way */
            pGraphicsDevice->StateFlags |= DISPLAY_DEVICE_ATTACHED_TO_DESKTOP;
        }
    }

    /* FIXME! */
    ppdev->flFlags = PDEV_DISPLAY;

    /* HACK: Don't use the pointer */
    ppdev->Pointer.Exclude.right = -1;

    /* Initialize PDEV */
    ppdev->pldev = pldev;
    ppdev->dwAccelerationLevel = dwAccelerationLevel;

    /* Copy the function table */
    if (ldevtype == LDEV_DEVICE_DISPLAY && (dwAccelerationLevel >= 5 ||
        pdm->dmFields & (DM_PANNINGWIDTH | DM_PANNINGHEIGHT)))
    {
        ULONG i;

        /* Initialize missing fields */
        if (!(pdm->dmFields & DM_PANNINGWIDTH))
            pdm->dmPanningWidth = pdm->dmPelsWidth;
        if (!(pdm->dmFields & DM_PANNINGHEIGHT))
            pdm->dmPanningHeight = pdm->dmPelsHeight;

        /* Replace vtable by panning vtable */
        for (i = 0; i < gPanDispDrvCount; i++)
            ppdev->apfn[gPanDispDrvFn[i].iFunc] = gPanDispDrvFn[i].pfn;
    }
    else
    {
        ppdev->pfn = ppdev->pldev->pfn;
    }

    /* Call the driver to enable the PDEV */
    if (!PDEVOBJ_bEnablePDEV(ppdev, pdm, NULL))
    {
        ERR("Failed to enable PDEV!\n");
        PDEVOBJ_vRelease(ppdev);
        EngUnloadImage(pldev);
        return NULL;
    }

    /* Tell the driver that the PDEV is ready */
    PDEVOBJ_vCompletePDEV(ppdev);

    /* Create the initial surface */
    pSurface = PDEVOBJ_pSurface(ppdev);
    if (!pSurface)
    {
        ERR("Failed to create surface\n");
        PDEVOBJ_vRelease(ppdev);
        EngUnloadImage(pldev);
        return NULL;
    }

    /* Enable DirectDraw */
    if (!PDEVOBJ_bEnableDirectDraw(ppdev))
    {
        ERR("Failed to enable DirectDraw\n");
        PDEVOBJ_vRelease(ppdev);
        EngUnloadImage(pldev);
        return NULL;
    }

    /* Remove some acceleration capabilities from driver */
    PDEVOBJ_vFilterDriverHooks(ppdev);

    /* Set MovePointer function */
    ppdev->pfnMovePointer = ppdev->pfn.MovePointer;
    if (!ppdev->pfnMovePointer)
        ppdev->pfnMovePointer = EngMovePointer;

    /* Insert the PDEV into the list */
    ppdev->ppdevNext = gppdevList;
    gppdevList = ppdev;

    /* Return the PDEV */
    return ppdev;
}

FORCEINLINE
VOID
SwitchPointer(
    _Inout_ PVOID pvPointer1,
    _Inout_ PVOID pvPointer2)
{
    PVOID *ppvPointer1 = pvPointer1;
    PVOID *ppvPointer2 = pvPointer2;
    PVOID pvTemp;

    pvTemp = *ppvPointer1;
    *ppvPointer1 = *ppvPointer2;
    *ppvPointer2 = pvTemp;
}

BOOL
NTAPI
PDEVOBJ_bDynamicModeChange(
    PPDEVOBJ ppdev,
    PPDEVOBJ ppdev2)
{
    union
    {
        DRIVER_FUNCTIONS pfn;
        GDIINFO gdiinfo;
        DEVINFO devinfo;
        DWORD StateFlags;
    } temp;

    /* Exchange driver functions */
    temp.pfn = ppdev->pfn;
    ppdev->pfn = ppdev2->pfn;
    ppdev2->pfn = temp.pfn;

    /* Exchange LDEVs */
    SwitchPointer(&ppdev->pldev, &ppdev2->pldev);

    /* Exchange DHPDEV */
    SwitchPointer(&ppdev->dhpdev, &ppdev2->dhpdev);

    /* Exchange surfaces and associate them with their new PDEV */
    SwitchPointer(&ppdev->pSurface, &ppdev2->pSurface);
    ppdev->pSurface->SurfObj.hdev = (HDEV)ppdev;
    ppdev2->pSurface->SurfObj.hdev = (HDEV)ppdev2;

    /* Exchange devinfo */
    temp.devinfo = ppdev->devinfo;
    ppdev->devinfo = ppdev2->devinfo;
    ppdev2->devinfo = temp.devinfo;

    /* Exchange gdiinfo */
    temp.gdiinfo = ppdev->gdiinfo;
    ppdev->gdiinfo = ppdev2->gdiinfo;
    ppdev2->gdiinfo = temp.gdiinfo;

    /* Exchange DEVMODE */
    SwitchPointer(&ppdev->pdmwDev, &ppdev2->pdmwDev);

    /* Exchange state flags */
    temp.StateFlags = ppdev->pGraphicsDevice->StateFlags;
    ppdev->pGraphicsDevice->StateFlags = ppdev2->pGraphicsDevice->StateFlags;
    ppdev2->pGraphicsDevice->StateFlags = temp.StateFlags;

    /* Notify each driver instance of its new HDEV association */
    ppdev->pfn.CompletePDEV(ppdev->dhpdev, (HDEV)ppdev);
    ppdev2->pfn.CompletePDEV(ppdev2->dhpdev, (HDEV)ppdev2);

    /* Switch DirectDraw mode */
    PDEVOBJ_vSwitchDirectDraw(ppdev, ppdev2);

    return TRUE;
}

/**
 * @brief
 * Moves a display driver instance of another graphics device into a live PDEV, and the live
 * one's instance out, so the DCs and surfaces that hold the live PDEV follow the desktop onto the
 * other adapter. Reference win32kbase swaps in a new MDEV instead, whose HDEVs nothing else holds.
 *
 * @param[in,out] ppdev
 * The PDEV the desktop uses.
 *
 * @param[in,out] ppdev2
 * The PDEV of the device the desktop moves to. It ends up with the old instance.
 */
VOID
NTAPI
PDEVOBJ_vSwitchGraphicsDevice(
    _Inout_ PPDEVOBJ ppdev,
    _Inout_ PPDEVOBJ ppdev2)
{
    union
    {
        DRIVER_FUNCTIONS pfn;
        GDIINFO gdiinfo;
        DEVINFO devinfo;
        POINTL ptl;
        DWORD dw;
        FLONG fl;
    } temp;

    EngAcquireSemaphore(ppdev->hsemDevLock);
    EngAcquireSemaphore(ghsemPDEV);

    PDEVOBJ_vSuspendDirectDraw(ppdev);
    PDEVOBJ_vSuspendDirectDraw(ppdev2);

    temp.pfn = ppdev->pfn;
    ppdev->pfn = ppdev2->pfn;
    ppdev2->pfn = temp.pfn;

    SwitchPointer(&ppdev->pldev, &ppdev2->pldev);
    SwitchPointer(&ppdev->dhpdev, &ppdev2->dhpdev);
    SwitchPointer(&ppdev->ppalSurf, &ppdev2->ppalSurf);
    SwitchPointer(&ppdev->pfnMovePointer, &ppdev2->pfnMovePointer);

    SwitchPointer(&ppdev->pSurface, &ppdev2->pSurface);
    ppdev->pSurface->SurfObj.hdev = (HDEV)ppdev;
    ppdev2->pSurface->SurfObj.hdev = (HDEV)ppdev2;

    temp.devinfo = ppdev->devinfo;
    ppdev->devinfo = ppdev2->devinfo;
    ppdev2->devinfo = temp.devinfo;

    temp.gdiinfo = ppdev->gdiinfo;
    ppdev->gdiinfo = ppdev2->gdiinfo;
    ppdev2->gdiinfo = temp.gdiinfo;

    SwitchPointer(&ppdev->pdmwDev, &ppdev2->pdmwDev);

    /* The physical device goes with the driver instance that drives it */
    SwitchPointer(&ppdev->pGraphicsDevice, &ppdev2->pGraphicsDevice);
    SwitchPointer(&ppdev->hSpooler, &ppdev2->hSpooler);

    temp.ptl = ppdev->ptlOrigion;
    ppdev->ptlOrigion = ppdev2->ptlOrigion;
    ppdev2->ptlOrigion = temp.ptl;

    temp.dw = ppdev->dwAccelerationLevel;
    ppdev->dwAccelerationLevel = ppdev2->dwAccelerationLevel;
    ppdev2->dwAccelerationLevel = temp.dw;

    /* Whether the display is asserted belongs to the driver instance too */
    temp.fl = ppdev->flFlags & PDEV_DISABLED;
    ppdev->flFlags = (ppdev->flFlags & ~PDEV_DISABLED) | (ppdev2->flFlags & PDEV_DISABLED);
    ppdev2->flFlags = (ppdev2->flFlags & ~PDEV_DISABLED) | temp.fl;

    SwitchPointer(&ppdev->pfnCddW32kAddD3DDirtyRgn, &ppdev2->pfnCddW32kAddD3DDirtyRgn);
    SwitchPointer(&ppdev->pfnCddW32kCloseProcess, &ppdev2->pfnCddW32kCloseProcess);
    SwitchPointer(&ppdev->pfnCddW32kDeleteDeviceBitmapEx, &ppdev2->pfnCddW32kDeleteDeviceBitmapEx);
    SwitchPointer(&ppdev->pfnCddW32kDriverSupportsLiteModeChange, &ppdev2->pfnCddW32kDriverSupportsLiteModeChange);
    SwitchPointer(&ppdev->pfnCddW32kUpdateDevMode, &ppdev2->pfnCddW32kUpdateDevMode);

    /* Notify each driver instance of its new HDEV association */
    ppdev->pfn.CompletePDEV(ppdev->dhpdev, (HDEV)ppdev);
    ppdev2->pfn.CompletePDEV(ppdev2->dhpdev, (HDEV)ppdev2);

    PDEVOBJ_vSwitchDirectDraw(ppdev, ppdev2);

    PDEVOBJ_vResumeDirectDraw(ppdev);
    PDEVOBJ_vResumeDirectDraw(ppdev2);

    if (ppdev->pEDDgpl)
    {
        ppdev->pEDDgpl->hDev = (HDEV)ppdev;
        ppdev->pEDDgpl->dhpdev = ppdev->dhpdev;
    }

    EngReleaseSemaphore(ghsemPDEV);
    EngReleaseSemaphore(ppdev->hsemDevLock);
}

/**
 * @brief
 * Does a PDEV still refer to this graphics device?
 */
BOOLEAN
NTAPI
PDEVOBJ_bIsGraphicsDeviceInUse(
    _In_ PGRAPHICS_DEVICE pGraphicsDevice)
{
    PPDEVOBJ ppdev;

    EngAcquireSemaphoreShared(ghsemPDEV);

    for (ppdev = gppdevList; ppdev != NULL; ppdev = ppdev->ppdevNext)
    {
        if (ppdev->pGraphicsDevice == pGraphicsDevice)
            break;
    }

    EngReleaseSemaphore(ghsemPDEV);

    return (ppdev != NULL);
}


BOOL
NTAPI
PDEVOBJ_bSwitchMode(
    PPDEVOBJ ppdev,
    PDEVMODEW pdm)
{
    PPDEVOBJ ppdevTmp;
    PSURFACE pSurface;
    BOOL retval = FALSE;

    /* Lock the PDEV */
    EngAcquireSemaphore(ppdev->hsemDevLock);

    /* And everything else */
    EngAcquireSemaphore(ghsemPDEV);

    DPRINT1("PDEVOBJ_bSwitchMode, ppdev = %p, pSurface = %p\n", ppdev, ppdev->pSurface);

    // Lookup the GraphicsDevice + select DEVMODE
    // pdm = LDEVOBJ_bProbeAndCaptureDevmode(ppdev, pdm);

    /* 1. Temporarily disable the current PDEV and reset video to its default mode */
    if (!PDEVOBJ_bDisableDisplay(ppdev))
    {
        DPRINT1("PDEVOBJ_bDisableDisplay() failed\n");
        /* Resume DirectDraw in case of failure */
        PDEVOBJ_vResumeDirectDraw(ppdev);
        goto leave;
    }

    /* 2. Create new PDEV */
    ppdevTmp = PDEVOBJ_Create(ppdev->pGraphicsDevice, pdm, 0, LDEV_DEVICE_DISPLAY);
    if (!ppdevTmp)
    {
        DPRINT1("Failed to create a new PDEV\n");
        goto leave2;
    }

    /* 3. Create a new surface */
    pSurface = PDEVOBJ_pSurface(ppdevTmp);
    if (!pSurface)
    {
        DPRINT1("PDEVOBJ_pSurface failed\n");
        PDEVOBJ_vRelease(ppdevTmp);
        goto leave2;
    }

    /* 4. Temporarily suspend DirectDraw for mode change */
    PDEVOBJ_vSuspendDirectDraw(ppdev);
    PDEVOBJ_vSuspendDirectDraw(ppdevTmp);

    /* 5. Switch the PDEVs */
    if (!PDEVOBJ_bDynamicModeChange(ppdev, ppdevTmp))
    {
        DPRINT1("PDEVOBJ_bDynamicModeChange() failed\n");
        PDEVOBJ_vRelease(ppdevTmp);
        goto leave2;
    }

    /* 6. Resume DirectDraw */
    PDEVOBJ_vResumeDirectDraw(ppdev);
    PDEVOBJ_vResumeDirectDraw(ppdevTmp);

    /* Release temp PDEV */
    PDEVOBJ_vRelease(ppdevTmp);

    /* Re-initialize DirectDraw data */
    ppdev->pEDDgpl->hDev = (HDEV)ppdev;
    ppdev->pEDDgpl->dhpdev = ppdev->dhpdev;

    /* Update primary display capabilities */
    if (ppdev == gpmdev->ppdevGlobal)
    {
        PDEVOBJ_vGetDeviceCaps(ppdev, &GdiHandleTable->DevCaps);
    }

    /* Success! */
    retval = TRUE;

leave2:
    /* Set the new video mode, or restore the original one in case of failure */
    PDEVOBJ_vEnableDisplay(ppdev);

leave:
    /* Unlock everything else */
    EngReleaseSemaphore(ghsemPDEV);
    /* Unlock the PDEV */
    EngReleaseSemaphore(ppdev->hsemDevLock);

    DPRINT1("leave, ppdev = %p, pSurface = %p\n", ppdev, ppdev->pSurface);

    return retval;
}


PPDEVOBJ
NTAPI
EngpGetPDEV(
    _In_opt_ PUNICODE_STRING pustrDeviceName)
{
    UNICODE_STRING ustrCurrent;
    PPDEVOBJ ppdev = NULL;
    PGRAPHICS_DEVICE pGraphicsDevice;
    ULONG i;

    /* Acquire PDEV lock */
    EngAcquireSemaphoreShared(ghsemPDEV);

    /* Did the caller pass a device name? */
    if (pustrDeviceName)
    {
        /* Loop all present PDEVs */
        for (i = 0; i < gpmdev->cDev; i++)
        {
            /* Get a pointer to the GRAPHICS_DEVICE */
            pGraphicsDevice = gpmdev->dev[i].ppdev->pGraphicsDevice;

            /* Compare the name */
            RtlInitUnicodeString(&ustrCurrent, pGraphicsDevice->szWinDeviceName);
            if (RtlEqualUnicodeString(pustrDeviceName, &ustrCurrent, FALSE))
            {
                /* Found! */
                ppdev = gpmdev->dev[i].ppdev;
                break;
            }
        }
    }
    else if (gpmdev)
    {
        /* Otherwise use the primary PDEV */
        ppdev = gpmdev->ppdevGlobal;
    }

    /* Did we find one? */
    if (ppdev)
    {
        /* Yes, reference the PDEV */
        PDEVOBJ_vReference(ppdev);
    }

    /* Release PDEV lock */
    EngReleaseSemaphore(ghsemPDEV);

    return ppdev;
}

LONG
PDEVOBJ_lChangeDisplaySettings(
    _In_opt_ PUNICODE_STRING pustrDeviceName,
    _In_opt_ PDEVMODEW RequestedMode,
    _In_opt_ PMDEVOBJ pmdevOld,
    _Out_ PMDEVOBJ *ppmdevNew,
    _In_ BOOL bSearchClosestMode)
{
    PGRAPHICS_DEVICE pGraphicsDevice = NULL;
    PMDEVOBJ pmdev = NULL;
    PDEVMODEW pdm = NULL;
    ULONG lRet = DISP_CHANGE_SUCCESSFUL;
    ULONG i, j;

    TRACE("PDEVOBJ_lChangeDisplaySettings('%wZ' '%dx%dx%d (%d Hz)' %p %p)\n",
        pustrDeviceName,
        RequestedMode ? RequestedMode->dmPelsWidth : 0,
        RequestedMode ? RequestedMode->dmPelsHeight : 0,
        RequestedMode ? RequestedMode->dmBitsPerPel : 0,
        RequestedMode ? RequestedMode->dmDisplayFrequency : 0,
        pmdevOld, ppmdevNew);

    if (pustrDeviceName)
    {
        pGraphicsDevice = EngpFindGraphicsDevice(pustrDeviceName, 0);
        if (!pGraphicsDevice)
        {
            ERR("Wrong device name provided: '%wZ'\n", pustrDeviceName);
            lRet = DISP_CHANGE_BADPARAM;
            goto cleanup;
        }
    }
    else if (RequestedMode)
    {
        pGraphicsDevice = EngpFindGraphicsDevice(NULL, 0);
        if (!pGraphicsDevice)
        {
            ERR("Wrong device'\n");
            lRet = DISP_CHANGE_BADPARAM;
            goto cleanup;
        }
    }

    if (pGraphicsDevice)
    {
        if (!LDEVOBJ_bProbeAndCaptureDevmode(pGraphicsDevice, RequestedMode, &pdm, bSearchClosestMode))
        {
            ERR("DrvProbeAndCaptureDevmode() failed\n");
            lRet = DISP_CHANGE_BADMODE;
            goto cleanup;
        }
    }

    /* Here, we know that input parameters were correct */

    {
        /* Create new MDEV. Note that if we provide a device name,
         * MDEV will only contain one device.
         * */

        if (pmdevOld)
        {
            /* Disable old MDEV */
            if (MDEVOBJ_bDisable(pmdevOld))
            {
                /* Create new MDEV. On failure, reenable old MDEV */
                pmdev = MDEVOBJ_Create(pustrDeviceName, pdm);
                if (!pmdev)
                    MDEVOBJ_vEnable(pmdevOld);
            }
        }
        else
        {
            pmdev = MDEVOBJ_Create(pustrDeviceName, pdm);
        }

        if (!pmdev)
        {
            ERR("Failed to create new MDEV\n");
            lRet = DISP_CHANGE_FAILED;
            goto cleanup;
        }

        lRet = DISP_CHANGE_SUCCESSFUL;
        *ppmdevNew = pmdev;

        /* We now have to do the mode switch */

        if (pustrDeviceName && pmdevOld)
        {
            /* We changed settings of one device. Add other devices which were already present */
            for (i = 0; i < pmdevOld->cDev; i++)
            {
                for (j = 0; j < pmdev->cDev; j++)
                {
                    if (pmdev->dev[j].ppdev->pGraphicsDevice == pmdevOld->dev[i].ppdev->pGraphicsDevice)
                    {
                        if (PDEVOBJ_bDynamicModeChange(pmdevOld->dev[i].ppdev, pmdev->dev[j].ppdev))
                        {
                            PPDEVOBJ tmp = pmdevOld->dev[i].ppdev;
                            pmdevOld->dev[i].ppdev = pmdev->dev[j].ppdev;
                            pmdev->dev[j].ppdev = tmp;
                        }
                        else
                        {
                            ERR("Failed to apply new settings\n");
                            UNIMPLEMENTED;
                            ASSERT(FALSE);
                        }
                        break;
                    }
                }
                if (j == pmdev->cDev)
                {
                    PDEVOBJ_vReference(pmdevOld->dev[i].ppdev);
                    pmdev->dev[pmdev->cDev].ppdev = pmdevOld->dev[i].ppdev;
                    pmdev->cDev++;
                }
            }
        }

        if (pmdev->cDev == 1)
        {
            pmdev->ppdevGlobal = pmdev->dev[0].ppdev;
        }
        else
        {
            /* Enable MultiDriver */
            pmdev->ppdevGlobal = PDEVOBJ_Create(NULL, (PDEVMODEW)pmdev, 0, LDEV_DEVICE_META);
            if (!pmdev->ppdevGlobal)
            {
                WARN("Failed to create meta-device. Using only first display\n");
                PDEVOBJ_vReference(pmdev->dev[0].ppdev);
                pmdev->ppdevGlobal = pmdev->dev[0].ppdev;
            }
        }

        if (pmdevOld)
        {
            /* Search PDEVs which were in pmdevOld, but are not anymore in pmdev, and disable them */
            for (i = 0; i < pmdevOld->cDev; i++)
            {
                for (j = 0; j < pmdev->cDev; j++)
                {
                    if (pmdev->dev[j].ppdev->pGraphicsDevice == pmdevOld->dev[i].ppdev->pGraphicsDevice)
                        break;
                }
                if (j == pmdev->cDev)
                    PDEVOBJ_bDisableDisplay(pmdevOld->dev[i].ppdev);
            }
        }
    }

cleanup:
    if (lRet != DISP_CHANGE_SUCCESSFUL)
    {
        *ppmdevNew = NULL;
        if (pmdev)
            MDEVOBJ_vDestroy(pmdev);
        if (pdm && pdm != RequestedMode)
            ExFreePoolWithTag(pdm, GDITAG_DEVMODE);
    }

    return lRet;
}

INT
NTAPI
PDEVOBJ_iGetColorManagementCaps(PPDEVOBJ ppdev)
{
    INT ret = CM_NONE;

    if (ppdev->flFlags & PDEV_DISPLAY)
    {
        if (ppdev->devinfo.iDitherFormat == BMF_8BPP ||
            ppdev->devinfo.flGraphicsCaps2 & GCAPS2_CHANGEGAMMARAMP)
            ret = CM_GAMMA_RAMP;
    }

    if (ppdev->devinfo.flGraphicsCaps & GCAPS_CMYKCOLOR)
        ret |= CM_CMYK_COLOR;
    if (ppdev->devinfo.flGraphicsCaps & GCAPS_ICM)
        ret |= CM_DEVICE_ICM;

    return ret;
}

VOID
NTAPI
PDEVOBJ_vGetDeviceCaps(
    IN PPDEVOBJ ppdev,
    OUT PDEVCAPS pDevCaps)
{
    PGDIINFO pGdiInfo = &ppdev->gdiinfo;

    pDevCaps->ulVersion = pGdiInfo->ulVersion;
    pDevCaps->ulTechnology = pGdiInfo->ulTechnology;
    pDevCaps->ulHorzSizeM = (pGdiInfo->ulHorzSize + 500) / 1000;
    pDevCaps->ulVertSizeM = (pGdiInfo->ulVertSize + 500) / 1000;
    pDevCaps->ulHorzSize = pGdiInfo->ulHorzSize;
    pDevCaps->ulVertSize = pGdiInfo->ulVertSize;
    pDevCaps->ulHorzRes = pGdiInfo->ulHorzRes;
    pDevCaps->ulVertRes = pGdiInfo->ulVertRes;
    pDevCaps->ulBitsPixel = pGdiInfo->cBitsPixel;
    if (pDevCaps->ulBitsPixel == 15) pDevCaps->ulBitsPixel = 16;
    pDevCaps->ulPlanes = pGdiInfo->cPlanes;
    pDevCaps->ulNumPens = pGdiInfo->ulNumColors;
    if (pDevCaps->ulNumPens != -1) pDevCaps->ulNumPens *= 5;
    pDevCaps->ulNumFonts = 0; // PDEVOBJ_cFonts(ppdev);
    pDevCaps->ulNumColors = pGdiInfo->ulNumColors;
    pDevCaps->ulRasterCaps = pGdiInfo->flRaster;
    pDevCaps->ulAspectX = pGdiInfo->ulAspectX;
    pDevCaps->ulAspectY = pGdiInfo->ulAspectY;
    pDevCaps->ulAspectXY = pGdiInfo->ulAspectXY;
    pDevCaps->ulLogPixelsX = pGdiInfo->ulLogPixelsX;
    pDevCaps->ulLogPixelsY = pGdiInfo->ulLogPixelsY;
    pDevCaps->ulSizePalette = pGdiInfo->ulNumPalReg;
    pDevCaps->ulColorRes = pGdiInfo->ulDACRed +
                           pGdiInfo->ulDACGreen +
                           pGdiInfo->ulDACBlue;
    pDevCaps->ulPhysicalWidth = pGdiInfo->szlPhysSize.cx;
    pDevCaps->ulPhysicalHeight = pGdiInfo->szlPhysSize.cy;
    pDevCaps->ulPhysicalOffsetX = pGdiInfo->ptlPhysOffset.x;
    pDevCaps->ulPhysicalOffsetY = pGdiInfo->ptlPhysOffset.y;
    pDevCaps->ulTextCaps = pGdiInfo->flTextCaps;
    pDevCaps->ulTextCaps |= (TC_SO_ABLE|TC_UA_ABLE|TC_CP_STROKE|TC_OP_STROKE|TC_OP_CHARACTER);
    if (pGdiInfo->ulTechnology != DT_PLOTTER)
        pDevCaps->ulTextCaps |= TC_VA_ABLE;
    pDevCaps->ulVRefresh = pGdiInfo->ulVRefresh;
    pDevCaps->ulDesktopHorzRes = pGdiInfo->ulHorzRes;
    pDevCaps->ulDesktopVertRes = pGdiInfo->ulVertRes;
    pDevCaps->ulBltAlignment = pGdiInfo->ulBltAlignment;
    pDevCaps->ulPanningHorzRes = pGdiInfo->ulPanningHorzRes;
    pDevCaps->ulPanningVertRes = pGdiInfo->ulPanningVertRes;
    pDevCaps->xPanningAlignment = pGdiInfo->xPanningAlignment;
    pDevCaps->yPanningAlignment = pGdiInfo->yPanningAlignment;
    pDevCaps->ulShadeBlend = pGdiInfo->flShadeBlend;
    pDevCaps->ulColorMgmtCaps = PDEVOBJ_iGetColorManagementCaps(ppdev);
}


/** Exported functions ********************************************************/

/*
 * @implemented
 */
BOOL
APIENTRY
EngQueryDeviceAttribute(
    _In_ HDEV hdev,
    _In_ ENG_DEVICE_ATTRIBUTE devAttr,
    _In_reads_bytes_(cjInSize) PVOID pvIn,
    _In_ ULONG cjInSize,
    _Out_writes_bytes_(cjOutSize) PVOID pvOut,
    _In_ ULONG cjOutSize)
{
    PPDEVOBJ ppdev = (PPDEVOBJ)hdev;

    if (devAttr != QDA_ACCELERATION_LEVEL)
        return FALSE;

    if (cjOutSize >= sizeof(DWORD))
    {
        /* Set all Accelerations Level Key to enabled Full 0 to 5 turned off. */
        *(DWORD*)pvOut = ppdev->dwAccelerationLevel;
        return TRUE;
    }

    return FALSE;
}

_Must_inspect_result_ _Ret_z_
LPWSTR
APIENTRY
EngGetDriverName(_In_ HDEV hdev)
{
    PPDEVOBJ ppdev = (PPDEVOBJ)hdev;

    ASSERT(ppdev);
    ASSERT(ppdev->pldev);
    ASSERT(ppdev->pldev->pGdiDriverInfo);
    ASSERT(ppdev->pldev->pGdiDriverInfo->DriverName.Buffer);

    return ppdev->pldev->pGdiDriverInfo->DriverName.Buffer;
}


INT
APIENTRY
NtGdiGetDeviceCaps(
    HDC hdc,
    INT Index)
{
    PDC pdc;
    DEVCAPS devcaps;

    /* Lock the given DC */
    pdc = DC_LockDc(hdc);
    if (!pdc)
    {
        EngSetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }

    /* Get the data */
    PDEVOBJ_vGetDeviceCaps(pdc->ppdev, &devcaps);

    /* Unlock the DC */
    DC_UnlockDc(pdc);

    /* Return capability */
    switch (Index)
    {
        case DRIVERVERSION:
            return devcaps.ulVersion;

        case TECHNOLOGY:
            return devcaps.ulTechnology;

        case HORZSIZE:
            return devcaps.ulHorzSize;

        case VERTSIZE:
            return devcaps.ulVertSize;

        case HORZRES:
            return devcaps.ulHorzRes;

        case VERTRES:
            return devcaps.ulVertRes;

        case LOGPIXELSX:
            return devcaps.ulLogPixelsX;

        case LOGPIXELSY:
            return devcaps.ulLogPixelsY;

        case BITSPIXEL:
            return devcaps.ulBitsPixel;

        case PLANES:
            return devcaps.ulPlanes;

        case NUMBRUSHES:
            return -1;

        case NUMPENS:
            return devcaps.ulNumPens;

        case NUMFONTS:
            return devcaps.ulNumFonts;

        case NUMCOLORS:
            return devcaps.ulNumColors;

        case ASPECTX:
            return devcaps.ulAspectX;

        case ASPECTY:
            return devcaps.ulAspectY;

        case ASPECTXY:
            return devcaps.ulAspectXY;

        case CLIPCAPS:
            return CP_RECTANGLE;

        case SIZEPALETTE:
            return devcaps.ulSizePalette;

        case NUMRESERVED:
            return 20;

        case COLORRES:
            return devcaps.ulColorRes;

        case DESKTOPVERTRES:
            return devcaps.ulVertRes;

        case DESKTOPHORZRES:
            return devcaps.ulHorzRes;

        case BLTALIGNMENT:
            return devcaps.ulBltAlignment;

        case SHADEBLENDCAPS:
            return devcaps.ulShadeBlend;

        case COLORMGMTCAPS:
            return devcaps.ulColorMgmtCaps;

        case PHYSICALWIDTH:
            return devcaps.ulPhysicalWidth;

        case PHYSICALHEIGHT:
            return devcaps.ulPhysicalHeight;

        case PHYSICALOFFSETX:
            return devcaps.ulPhysicalOffsetX;

        case PHYSICALOFFSETY:
            return devcaps.ulPhysicalOffsetY;

        case VREFRESH:
            return devcaps.ulVRefresh;

        case RASTERCAPS:
            return devcaps.ulRasterCaps;

        case CURVECAPS:
            return (CC_CIRCLES | CC_PIE | CC_CHORD | CC_ELLIPSES | CC_WIDE |
                    CC_STYLED | CC_WIDESTYLED | CC_INTERIORS | CC_ROUNDRECT);

        case LINECAPS:
            return (LC_POLYLINE | LC_MARKER | LC_POLYMARKER | LC_WIDE |
                    LC_STYLED | LC_WIDESTYLED | LC_INTERIORS);

        case POLYGONALCAPS:
            return (PC_POLYGON | PC_RECTANGLE | PC_WINDPOLYGON | PC_SCANLINE |
                    PC_WIDE | PC_STYLED | PC_WIDESTYLED | PC_INTERIORS);

        case TEXTCAPS:
            return devcaps.ulTextCaps;

        case CAPS1:
        case PDEVICESIZE:
        case SCALINGFACTORX:
        case SCALINGFACTORY:
        default:
            return 0;
    }

    return 0;
}

_Success_(return!=FALSE)
BOOL
APIENTRY
NtGdiGetDeviceCapsAll(
    IN HDC hDC,
    OUT PDEVCAPS pDevCaps)
{
    PDC pdc;
    DEVCAPS devcaps;
    BOOL bResult = TRUE;

    /* Lock the given DC */
    pdc = DC_LockDc(hDC);
    if (!pdc)
    {
        EngSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    /* Get the data */
    PDEVOBJ_vGetDeviceCaps(pdc->ppdev, &devcaps);

    /* Unlock the DC */
    DC_UnlockDc(pdc);

    /* Copy data to caller */
    _SEH2_TRY
    {
        ProbeForWrite(pDevCaps, sizeof(DEVCAPS), 1);
        RtlCopyMemory(pDevCaps, &devcaps, sizeof(DEVCAPS));
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        SetLastNtError(_SEH2_GetExceptionCode());
        bResult = FALSE;
    }
    _SEH2_END;

    return bResult;
}

DHPDEV
APIENTRY
NtGdiGetDhpdev(
    IN HDEV hdev)
{
    PPDEVOBJ ppdev;
    DHPDEV dhpdev = NULL;

    /* Check parameter */
    if (!hdev || (PCHAR)hdev < (PCHAR)MmSystemRangeStart)
        return NULL;

    /* Lock PDEV list */
    EngAcquireSemaphoreShared(ghsemPDEV);

    /* Walk through the list of PDEVs */
    for (ppdev = gppdevList;  ppdev; ppdev = ppdev->ppdevNext)
    {
        /* Compare with the given HDEV */
        if (ppdev == (PPDEVOBJ)hdev)
        {
            /* Found the PDEV! Get it's dhpdev and break */
            dhpdev = ppdev->dhpdev;
            break;
        }
    }

    /* Unlock PDEV list */
    EngReleaseSemaphore(ghsemPDEV);

    return dhpdev;
}

PSIZEL
FASTCALL
PDEVOBJ_sizl(PPDEVOBJ ppdev, PSIZEL psizl)
{
    if (ppdev->flFlags & PDEV_META_DEVICE)
    {
        *psizl = ppdev->szlMetaRes;
    }
    else
    {
        psizl->cx = ppdev->gdiinfo.ulHorzRes;
        psizl->cy = ppdev->gdiinfo.ulVertRes;
    }
    return psizl;
}
