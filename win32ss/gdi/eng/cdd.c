/*
 * PROJECT:     ReactOS Win32k (WDDM display path)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The win32k half of the CDD contract - what cdd.dll calls into GDI
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * cdd.dll is the display driver of every WDDM adapter. It asks GDI for the interface below
 * (EngQueryW32kCddInterface), hands GDI its own entry points in the same table, and then drives
 * the desktop primary through dxgkrnl.
 */

#include <win32k.h>

#define NDEBUG
#include <debug.h>

/* The table cdd.dll and win32k exchange. cdd fills the pfnCddW32k* half before the call. */
typedef struct _W32KCDD_INTERFACE
{
    ULONG Size;
    ULONG Version;
    PVOID pfnW32kCddGetWin32kCommand;
    PVOID pfnW32kCddClipRegion;
    PVOID pfnW32kCddIncPresentUniq;
    PVOID pfnW32kCddInitPdev;
    PVOID pfnW32kCddIsNullBrush;
    PVOID pfnW32kCddDisableGdiHwAcceleration;
    PVOID pfnW32kCddLineTo;
    PVOID pfnW32kCddStrokePath;
    PVOID pfnW32kCddGenerateMoveData;
    PVOID pfnW32kCddAcquireDynamicModeChangeLockShared;
    PVOID pfnW32kCddReleaseDynamicModeChangeLockShared;
    PVOID pfnCddW32kAddD3DDirtyRgn;
    PVOID pfnCddW32kAddBitmapD3DDirtyRgn;
    PVOID pfnCddW32kCloseProcess;
    PVOID pfnCddW32kDriverSupportsLiteModeChange;
    PVOID pfnCddW32kUpdateDevMode;
    PVOID pfnCddW32kDeleteDeviceBitmapEx;
} W32KCDD_INTERFACE, *PW32KCDD_INTERFACE;

#define W32KCDD_INTERFACE_VERSION   3

/* Gamma is scaled by 1000, so 1000 means no correction at all */
#define ENG_GAMMA_UNCORRECTED       1000

static ULONG gulPresentUniq = 0;
static BYTE gajGammaIdentity[256];
static BOOLEAN gbGammaIdentityReady = FALSE;

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief
 * Answers a "why am I being asserted" poll from cdd.
 *
 * cdd calls this from DrvAssertMode, so the answer is whatever PDEVOBJ_bAssertGdiOutput left on
 * the PDEV for the duration of that call, and zero at any other time.
 *
 * @param[in] hdev
 * The PDEV cdd drives.
 *
 * @return
 * The command flags, or zero when win32k is not asserting the mode.
 */
static
ULONG
APIENTRY
W32kCddGetWin32kCommand(
    _In_ HDEV hdev)
{
    PPDEVOBJ ppdev = (PPDEVOBJ)hdev;

    if (ppdev == NULL)
        return 0;

    return ppdev->ulW32kCommand;
}

/**
 * @brief
 * Clips a region against a CLIPOBJ on cdd's behalf. Not implemented yet.
 */
static
BOOL
APIENTRY
W32kCddClipRegion(
    _In_ HANDLE hDstRgn,
    _In_ HANDLE hSrcRgn,
    _In_ const CLIPOBJ *pco)
{
    UNREFERENCED_PARAMETER(hDstRgn);
    UNREFERENCED_PARAMETER(hSrcRgn);
    UNREFERENCED_PARAMETER(pco);

    UNIMPLEMENTED;
    return FALSE;
}

/**
 * @brief
 * Counts one more present, which tells GDI the screen contents moved on.
 */
static
VOID
APIENTRY
W32kCddIncPresentUniq(VOID)
{
    InterlockedIncrement((PLONG)&gulPresentUniq);
}

/**
 * @brief
 * Gives cdd the mode list of the device it is being enabled on, plus the engine
 * interface dxgkrnl keeps per process.
 *
 * @param[in] hdev
 * The PDEV being enabled.
 *
 * @param[in] hDevObj
 * The device handle GDI passed to DrvEnablePDEV.
 *
 * @param[out] pNumModes
 * Receives the mode count.
 *
 * @param[out] ppModeList
 * Receives the mode array.
 *
 * @param[out] ppDxgkW32kInterface
 * Receives the win32k engine interface dxgkrnl calls back through.
 *
 * @return
 * STATUS_INVALID_PARAMETER for an unknown device, otherwise STATUS_SUCCESS.
 */
static
NTSTATUS
APIENTRY
W32kCddInitPdev(
    _In_ HDEV hdev,
    _In_ HANDLE hDevObj,
    _Out_ PULONG pNumModes,
    _Out_ PDEVMODEW *ppModeList,
    _Out_ PVOID *ppDxgkW32kInterface)
{
    extern struct _DXGKWIN32KENG_INTERFACE gDxgkWin32kEngInterface;
    PGRAPHICS_DEVICE pGraphicsDevice;

    UNREFERENCED_PARAMETER(hdev);

    pGraphicsDevice = EngpFindGraphicsDeviceByHandle(hDevObj);
    if (pGraphicsDevice == NULL)
        return STATUS_INVALID_PARAMETER;

    /* These are the modes the driver itself reported from DrvGetModes */
    if (pGraphicsDevice->pdevmodeInfo != NULL)
    {
        *pNumModes = pGraphicsDevice->cDevModes;
        *ppModeList = pGraphicsDevice->pdevmodeInfo->adevmode;
    }
    else
    {
        *pNumModes = 0;
        *ppModeList = NULL;
    }

    *ppDxgkW32kInterface = &gDxgkWin32kEngInterface;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Tells cdd whether a brush draws nothing.
 */
static
BOOL
APIENTRY
W32kCddIsNullBrush(
    _In_ BRUSHOBJ *pbo)
{
    return (pbo == NULL);
}

/**
 * @brief
 * Tells cdd whether GDI hardware acceleration has been turned off. It never is here.
 */
static
BOOL
APIENTRY
W32kCddDisableGdiHwAcceleration(VOID)
{
    return FALSE;
}

/**
 * @brief
 * Draws a line for cdd through GDI. Not implemented yet, so cdd draws it itself.
 */
static
BOOL
APIENTRY
W32kCddLineTo(
    _In_ SURFOBJ *pso,
    _In_ CLIPOBJ *pco,
    _In_ BRUSHOBJ *pbo,
    _In_ LONG x1,
    _In_ LONG y1,
    _In_ LONG x2,
    _In_ LONG y2,
    _In_ RECTL *prclBounds,
    _In_ MIX mix,
    _In_ PVOID pEngCallbacks)
{
    UNREFERENCED_PARAMETER(pso);
    UNREFERENCED_PARAMETER(pco);
    UNREFERENCED_PARAMETER(pbo);
    UNREFERENCED_PARAMETER(x1);
    UNREFERENCED_PARAMETER(y1);
    UNREFERENCED_PARAMETER(x2);
    UNREFERENCED_PARAMETER(y2);
    UNREFERENCED_PARAMETER(prclBounds);
    UNREFERENCED_PARAMETER(mix);
    UNREFERENCED_PARAMETER(pEngCallbacks);

    UNIMPLEMENTED;
    return FALSE;
}

/**
 * @brief
 * Strokes a path for cdd through GDI. Not implemented yet, so cdd strokes it itself.
 */
static
BOOL
APIENTRY
W32kCddStrokePath(
    _In_ SURFOBJ *pso,
    _In_ PATHOBJ *ppo,
    _In_ CLIPOBJ *pco,
    _In_ XFORMOBJ *pxo,
    _In_ BRUSHOBJ *pbo,
    _In_ POINTL *pptlBrushOrg,
    _In_ LINEATTRS *plineattrs,
    _In_ MIX mix,
    _In_ PVOID pEngCallbacks)
{
    UNREFERENCED_PARAMETER(pso);
    UNREFERENCED_PARAMETER(ppo);
    UNREFERENCED_PARAMETER(pco);
    UNREFERENCED_PARAMETER(pxo);
    UNREFERENCED_PARAMETER(pbo);
    UNREFERENCED_PARAMETER(pptlBrushOrg);
    UNREFERENCED_PARAMETER(plineattrs);
    UNREFERENCED_PARAMETER(mix);
    UNREFERENCED_PARAMETER(pEngCallbacks);

    UNIMPLEMENTED;
    return FALSE;
}

/**
 * @brief
 * Builds the move data a present needs. Nothing tracks screen-to-screen moves yet.
 */
static
ULONG
APIENTRY
W32kCddGenerateMoveData(VOID)
{
    return 0;
}

/**
 * @brief
 * Holds off a dynamic mode change while cdd presents. ReactOS has no such lock.
 */
static
VOID
APIENTRY
W32kCddAcquireDynamicModeChangeLockShared(VOID)
{
}

/**
 * @brief
 * Releases the lock taken by W32kCddAcquireDynamicModeChangeLockShared.
 */
static
VOID
APIENTRY
W32kCddReleaseDynamicModeChangeLockShared(VOID)
{
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Returns the gamma a device draws text with, scaled by 1000.
 *
 * @param[in] hdev
 * The PDEV.
 *
 * @return
 * 1000, since no gamma correction is applied.
 */
ULONG
APIENTRY
EngCTGetCurrentGamma(
    _In_ HDEV hdev)
{
    UNREFERENCED_PARAMETER(hdev);

    return ENG_GAMMA_UNCORRECTED;
}

/**
 * @brief
 * Returns the lookup tables that convert between linear and gamma space.
 *
 * @param[in] ulGamma
 * The gamma, scaled by 1000.
 *
 * @param[out] ppGammaTable
 * Receives the table into gamma space.
 *
 * @param[out] ppInverseGammaTable
 * Receives the table back to linear space.
 *
 * @remarks
 * Both tables are the identity, which is what Windows also hands out for gamma 1.0.
 */
VOID
APIENTRY
EngCTGetGammaTable(
    _In_ ULONG ulGamma,
    _Out_ const BYTE **ppGammaTable,
    _Out_ const BYTE **ppInverseGammaTable)
{
    ULONG i;

    UNREFERENCED_PARAMETER(ulGamma);

    if (!gbGammaIdentityReady)
    {
        for (i = 0; i < RTL_NUMBER_OF(gajGammaIdentity); i++)
            gajGammaIdentity[i] = (BYTE)i;

        gbGammaIdentityReady = TRUE;
    }

    *ppGammaTable = gajGammaIdentity;
    *ppInverseGammaTable = gajGammaIdentity;
}

/**
 * @brief
 * Creates a device bitmap that stands in for a redirected window's surface.
 *
 * @param[in] dhsurf
 * The driver's handle for the surface.
 *
 * @param[in] sizl
 * Its size in pixels.
 *
 * @param[in] iFormat
 * Its pixel format.
 *
 * @return
 * The bitmap, or NULL.
 */
HBITMAP
APIENTRY
EngCreateRedirectionDeviceBitmap(
    _In_ DHSURF dhsurf,
    _In_ SIZEL sizl,
    _In_ ULONG iFormat)
{
    PSURFACE psurf;
    HBITMAP hbmp;

    hbmp = EngCreateDeviceBitmap(dhsurf, sizl, iFormat);
    if (hbmp == NULL)
        return NULL;

    psurf = SURFACE_ShareLockSurface(hbmp);
    if (psurf == NULL)
    {
        EngDeleteSurface((HSURF)hbmp);
        return NULL;
    }

    psurf->bCddDeviceBitmap = TRUE;
    SURFACE_ShareUnlockSurface(psurf);
    return hbmp;
}

/**
 * @brief
 * Tells whether a surface is one of the device bitmaps the CDD owns.
 *
 * @param[in] pso
 * The surface.
 *
 * @return
 * TRUE for a bitmap from EngCreateRedirectionDeviceBitmap.
 */
BOOL
APIENTRY
EngIsCddDeviceBitmap(
    _In_ SURFOBJ *pso)
{
    PSURFACE psurf;

    if (pso == NULL)
        return FALSE;

    psurf = CONTAINING_RECORD(pso, SURFACE, SurfObj);
    return psurf->bCddDeviceBitmap;
}

/**
 * @brief
 * Republishes a device surface and hands back the region it may draw in.
 *
 * @param[in] pso
 * The device surface.
 *
 * @param[out] ppco
 * Receives the clip region, NULL while nothing restricts drawing.
 *
 * @return
 * TRUE.
 */
BOOL
APIENTRY
EngUpdateDeviceSurface(
    _In_ SURFOBJ *pso,
    _Out_ CLIPOBJ **ppco)
{
    UNREFERENCED_PARAMETER(pso);

    *ppco = NULL;
    return TRUE;
}

/**
 * @brief
 * Exchanges entry points with cdd.dll and tells it which adapter it drives.
 *
 * @param[in] hDevObj
 * The device handle GDI passed to DrvEnablePDEV.
 *
 * @param[in] hDev
 * The PDEV, or NULL on the first call. When given, cdd's own entry points in the
 * table are kept on it.
 *
 * @param[in,out] pW32kCddInterface
 * The shared table, with Size and Version filled in by cdd.
 *
 * @param[out] phAdapter
 * Receives dxgkrnl's adapter for the device.
 *
 * @param[out] pVidPnSourceId
 * Receives the video present source the device draws to.
 *
 * @param[out] ppCSRSS
 * Receives the CSRSS process.
 *
 * @return
 * STATUS_INVALID_PARAMETER for a table or device it does not know, otherwise STATUS_SUCCESS.
 */
NTSTATUS
APIENTRY
EngQueryW32kCddInterface(
    _In_ HANDLE hDevObj,
    _In_opt_ HDEV hDev,
    _Inout_ PW32KCDD_INTERFACE pW32kCddInterface,
    _Out_ PVOID *phAdapter,
    _Out_ PULONG pVidPnSourceId,
    _Out_ PEPROCESS *ppCSRSS)
{
    PGRAPHICS_DEVICE pGraphicsDevice;
    PPDEVOBJ ppdev;

    if ((pW32kCddInterface->Version != W32KCDD_INTERFACE_VERSION) ||
        (pW32kCddInterface->Size != sizeof(*pW32kCddInterface)))
    {
        DPRINT1("win32k: CDD interface is version %lu size %lu, expected %u size %Iu\n",
                pW32kCddInterface->Version, pW32kCddInterface->Size,
                W32KCDD_INTERFACE_VERSION, sizeof(*pW32kCddInterface));
        return STATUS_INVALID_PARAMETER;
    }

    pGraphicsDevice = EngpFindGraphicsDeviceByHandle(hDevObj);
    if (pGraphicsDevice == NULL)
        return STATUS_INVALID_PARAMETER;

    pW32kCddInterface->pfnW32kCddGetWin32kCommand = W32kCddGetWin32kCommand;
    pW32kCddInterface->pfnW32kCddClipRegion = W32kCddClipRegion;
    pW32kCddInterface->pfnW32kCddIncPresentUniq = W32kCddIncPresentUniq;
    pW32kCddInterface->pfnW32kCddInitPdev = W32kCddInitPdev;
    pW32kCddInterface->pfnW32kCddIsNullBrush = W32kCddIsNullBrush;
    pW32kCddInterface->pfnW32kCddDisableGdiHwAcceleration = W32kCddDisableGdiHwAcceleration;
    pW32kCddInterface->pfnW32kCddLineTo = W32kCddLineTo;
    pW32kCddInterface->pfnW32kCddStrokePath = W32kCddStrokePath;
    pW32kCddInterface->pfnW32kCddGenerateMoveData = W32kCddGenerateMoveData;
    pW32kCddInterface->pfnW32kCddAcquireDynamicModeChangeLockShared =
        W32kCddAcquireDynamicModeChangeLockShared;
    pW32kCddInterface->pfnW32kCddReleaseDynamicModeChangeLockShared =
        W32kCddReleaseDynamicModeChangeLockShared;

    *phAdapter = pGraphicsDevice->DxgAdapter;
    *pVidPnSourceId = pGraphicsDevice->VidPnSourceId;
    *ppCSRSS = gpepCSRSS;

    /* Keep cdd's own entry points on the PDEV it is enabling */
    if (hDev != NULL)
    {
        if (pW32kCddInterface->pfnCddW32kAddD3DDirtyRgn == NULL)
            return STATUS_INVALID_PARAMETER;

        ppdev = (PPDEVOBJ)hDev;
        ppdev->pfnCddW32kAddD3DDirtyRgn = pW32kCddInterface->pfnCddW32kAddD3DDirtyRgn;
        ppdev->pfnCddW32kCloseProcess = pW32kCddInterface->pfnCddW32kCloseProcess;
        ppdev->pfnCddW32kDeleteDeviceBitmapEx = pW32kCddInterface->pfnCddW32kDeleteDeviceBitmapEx;
        ppdev->pfnCddW32kDriverSupportsLiteModeChange =
            pW32kCddInterface->pfnCddW32kDriverSupportsLiteModeChange;
        ppdev->pfnCddW32kUpdateDevMode = pW32kCddInterface->pfnCddW32kUpdateDevMode;
    }

    return STATUS_SUCCESS;
}

/* EOF */
