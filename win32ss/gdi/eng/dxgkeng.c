/*
 * PROJECT:     ReactOS Win32k (WDDM display path)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The engine interface dxgkrnl calls win32k back through
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * win32k hands this table to dxgkrnl for every GUI process (gdi/eng/dxgkrnl.c, the process
 * callout) and to the CDD when it enables a PDEV. dxgkrnl calls most of these without checking
 * them for NULL, so every slot is filled. The ones that answer a question return nothing and
 * zero what they were asked to fill, which keeps the caller on its "not available" path.
 */

#include <win32k.h>

#define NDEBUG
#include <debug.h>

/* Slot order comes from the 14361 win32kbase PDB, and dxgkrnl indexes it by offset */
typedef struct _DXGKWIN32KENG_INTERFACE
{
    USHORT Size;
    USHORT Version;
    PVOID pfnDxgkEngVisRgnUniq;
    PVOID pfnDxgkEngLockVisRgn;
    PVOID pfnDxgkEngUnlockVisRgn;
    PVOID pfnDxgkEngEnterUserCrit;
    PVOID pfnDxgkEngLeaveUserCrit;
    PVOID pfnDxgkEngGetDC;
    PVOID pfnDxgkEngIsRedirectionDC;
    PVOID pfnDxgkEngReleaseDC;
    PVOID pfnDxgkEngGetClientRect;
    PVOID pfnDxgkEngCreateRectRgn;
    PVOID pfnDxgkEngGetVisRgn;
    PVOID pfnDxgkEngSetRgn;
    PVOID pfnDxgkEngCombineRgn;
    PVOID pfnDxgkEngGetRgnData;
    PVOID pfnDxgkEngGetBoxRgn;
    PVOID pfnDxgkEngDeleteObject;
    PVOID pfnDxgkEngDetectGDIPath;
    PVOID pfnDxgkEngBltViaGDI;
    PVOID pfnDxgkEngColorFillViaGDI;
    PVOID pfnDxgkEngLockShareSem;
    PVOID pfnDxgkEngUnlockShareSem;
    PVOID pfnDxgkEngAcquireWin32kAndPDEVLocks;
    PVOID pfnDxgkEngAssertGdiOutput;
    PVOID pfnDxgkEngResetPointer;
    PVOID pfnDxgkEngReleaseWin32kAndPDEVLocks;
    PVOID pfnDxgkEngScreenAccessCheck;
    PVOID pfnDxgkEngIsDwmProcess;
    PVOID pfnDxgkEngIsRemoteConnection;
    PVOID pfnDxgkEngGetRedirBitmapSharedHandle;
    PVOID pfnDxgkEngAddRedirBitmapD3DDirtyRgn;
    PVOID pfnDxgkEngAccumD3DPresentBounds;
    PVOID pfnDxgkEngRefPresentHistoryToken;
    PVOID pfnDxgkEngAcquireStableVisRgn;
    PVOID pfnDxgkEngReleaseStableVisRgn;
    PVOID pfnDxgkEngAcquireStableSprite;
    PVOID pfnDxgkEngReleaseStableSprite;
    PVOID pfnDxgkEngWatchVisRgnChange;
    PVOID pfnDxgkEngFindViewDesktopPosition;
    PVOID pfnDxgkEngIsDwmComposing;
    PVOID pfnDxgkEngQuerySwapChainBindingStatus;
    PVOID pfnDxgkEngGetRedirectedWindowOrigin;
    PVOID pfnDxgkEngAdjustMonitorPosition;
    PVOID pfnDxgkEngGetRemoteDeviceCount;
    PVOID pfnDxgkEngGetAdapterUniquenessPointer;
    PVOID pfbDxgkEngIncSpritetUniq;
    PVOID pfnDxgkEngQueryWin32Info;
    PVOID pfnDxgkEngGetWindowRect;
    PVOID pfnDxgkEngNotifyDisplayChange;
} DXGKWIN32KENG_INTERFACE;

/* Size and Version share the first pointer slot, so this is 392 bytes on x64 */
C_ASSERT(sizeof(DXGKWIN32KENG_INTERFACE) == 49 * sizeof(PVOID));

#define DXGKWIN32KENG_INTERFACE_VERSION     5

static ULONG gulVisRgnUniq = 0;
static ULONG gulSpriteUniq = 0;
static volatile LONG glAdapterUniqueness = 0;

/* PRIVATE FUNCTIONS **********************************************************/

static
ULONG
APIENTRY
DxgkEngVisRgnUniq(VOID)
{
    return gulVisRgnUniq;
}

/* Nothing here hands out a window's visible region yet */
static
HANDLE
APIENTRY
DxgkEngLockVisRgn(
    _In_ HDC hdc)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hdc);

    return NULL;
}

static
VOID
APIENTRY
DxgkEngUnlockVisRgn(
    _In_ HANDLE hrgn)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hrgn);
}

static
VOID
APIENTRY
DxgkEngEnterUserCrit(
    _In_ LONG Flags)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(Flags);
}

static
VOID
APIENTRY
DxgkEngLeaveUserCrit(VOID)
{
    UNIMPLEMENTED_ONCE;
}

static
HDC
APIENTRY
DxgkEngGetDC(
    _In_ HWND hwnd,
    _Out_ HDC *phdc)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hwnd);

    if (phdc != NULL)
        *phdc = NULL;

    return NULL;
}

static
BOOL
APIENTRY
DxgkEngIsRedirectionDC(
    _In_ HDC hdc)
{
    UNREFERENCED_PARAMETER(hdc);

    return FALSE;
}

static
BOOL
APIENTRY
DxgkEngReleaseDC(
    _In_ HDC hdc,
    _In_ HDC hdcRedirected)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hdc);
    UNREFERENCED_PARAMETER(hdcRedirected);

    return FALSE;
}

static
LONG
APIENTRY
DxgkEngGetClientRect(
    _In_ HDC hdc,
    _Out_ LPRECT prcl)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hdc);

    if (prcl != NULL)
        RtlZeroMemory(prcl, sizeof(*prcl));

    return 0;
}

static
HANDLE
APIENTRY
DxgkEngCreateRectRgn(
    _In_ INT left,
    _In_ INT top,
    _In_ INT right,
    _In_ INT bottom)
{
    return EngCreateRectRgn(left, top, right, bottom);
}

static
LONG
APIENTRY
DxgkEngGetVisRgn(
    _In_ HDC hdc,
    _In_ HANDLE hrgn,
    _In_ BOOL bClient)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hdc);
    UNREFERENCED_PARAMETER(hrgn);
    UNREFERENCED_PARAMETER(bClient);

    return 0;
}

static
BOOL
APIENTRY
DxgkEngSetRgn(
    _In_ HANDLE hrgn,
    _In_ INT left,
    _In_ INT top,
    _In_ INT right,
    _In_ INT bottom)
{
    return EngSetRectRgn(hrgn, left, top, right, bottom);
}

static
INT
APIENTRY
DxgkEngCombineRgn(
    _In_ HANDLE hrgnTrg,
    _In_ HANDLE hrgnSrc1,
    _In_ HANDLE hrgnSrc2,
    _In_ INT iMode)
{
    return EngCombineRgn(hrgnTrg, hrgnSrc1, hrgnSrc2, iMode);
}

static
DWORD
APIENTRY
DxgkEngGetRgnData(
    _In_ HANDLE hrgn,
    _In_ DWORD nCount,
    _Out_ LPRGNDATA lpRgnData)
{
    return EngGetRgnData(hrgn, nCount, lpRgnData);
}

static
INT
APIENTRY
DxgkEngGetBoxRgn(
    _In_ HANDLE hrgn,
    _Out_ LPRECT prcl)
{
    return EngGetRgnBox(hrgn, prcl);
}

/* dxgkrnl only ever hands back regions it got from DxgkEngCreateRectRgn */
static
BOOL
APIENTRY
DxgkEngDeleteObject(
    _In_ HANDLE hobj)
{
    EngDeleteRgn(hobj);
    return TRUE;
}

static
LONG
APIENTRY
DxgkEngDetectGDIPath(
    _In_ PVOID pvSurface,
    _In_ HDEV hdev,
    _In_ HWND hwnd,
    _In_ HANDLE hrgn)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(pvSurface);
    UNREFERENCED_PARAMETER(hdev);
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(hrgn);

    return 0;
}

/* Presenting through GDI is not supported, so the caller keeps the work */
static
LONG
APIENTRY
DxgkEngBltViaGDI(
    _In_ PVOID pPresent,
    _In_ HDC hdc,
    _In_ PVOID prclSrc,
    _In_ PVOID prclDst,
    _In_ PVOID pvAllocation,
    _In_ ULONG ulFlags,
    _In_ ULONG ulRop,
    _In_ ULONG ulColor,
    _In_ UCHAR bFlag1,
    _In_ UCHAR bFlag2,
    _In_ ULONG ulCount,
    _In_ PVOID pfnCallback1,
    _In_ PVOID pfnCallback2)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(pPresent);
    UNREFERENCED_PARAMETER(hdc);
    UNREFERENCED_PARAMETER(prclSrc);
    UNREFERENCED_PARAMETER(prclDst);
    UNREFERENCED_PARAMETER(pvAllocation);
    UNREFERENCED_PARAMETER(ulFlags);
    UNREFERENCED_PARAMETER(ulRop);
    UNREFERENCED_PARAMETER(ulColor);
    UNREFERENCED_PARAMETER(bFlag1);
    UNREFERENCED_PARAMETER(bFlag2);
    UNREFERENCED_PARAMETER(ulCount);
    UNREFERENCED_PARAMETER(pfnCallback1);
    UNREFERENCED_PARAMETER(pfnCallback2);

    return 0;
}

static
LONG
APIENTRY
DxgkEngColorFillViaGDI(
    _In_ HDC hdc,
    _In_ PVOID prclDst,
    _In_ PVOID prclClip,
    _In_ ULONG ulColor,
    _In_ ULONG ulFlags)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hdc);
    UNREFERENCED_PARAMETER(prclDst);
    UNREFERENCED_PARAMETER(prclClip);
    UNREFERENCED_PARAMETER(ulColor);
    UNREFERENCED_PARAMETER(ulFlags);

    return 0;
}

static
LONG
APIENTRY
DxgkEngLockShareSem(VOID)
{
    UNIMPLEMENTED_ONCE;
    return 0;
}

static
LONG
APIENTRY
DxgkEngUnlockShareSem(VOID)
{
    UNIMPLEMENTED_ONCE;
    return 0;
}

static
VOID
APIENTRY
DxgkEngAcquireWin32kAndPDEVLocks(
    _In_ PVOID pvDev,
    _In_ ULONG ulFlags)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(pvDev);
    UNREFERENCED_PARAMETER(ulFlags);
}

static
LONG
APIENTRY
DxgkEngAssertGdiOutput(
    _In_ PVOID pvDev,
    _In_ PVOID pvData,
    _In_ ULONG ulSize,
    _Out_ PUCHAR pbResult)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(pvDev);
    UNREFERENCED_PARAMETER(pvData);
    UNREFERENCED_PARAMETER(ulSize);

    if (pbResult != NULL)
        *pbResult = 0;

    return 0;
}

static
VOID
APIENTRY
DxgkEngResetPointer(VOID)
{
    UNIMPLEMENTED_ONCE;
}

static
VOID
APIENTRY
DxgkEngReleaseWin32kAndPDEVLocks(
    _In_ PVOID pvDev,
    _In_ ULONG ulFlags)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(pvDev);
    UNREFERENCED_PARAMETER(ulFlags);
}

static
BOOL
APIENTRY
DxgkEngScreenAccessCheck(VOID)
{
    return TRUE;
}

/* There is no desktop window manager here */
static
BOOL
APIENTRY
DxgkEngIsDwmProcess(VOID)
{
    return FALSE;
}

static
LONG
APIENTRY
DxgkEngIsRemoteConnection(
    _Out_ PBOOL pbRemote)
{
    if (pbRemote != NULL)
        *pbRemote = FALSE;

    return 0;
}

static
VOID
APIENTRY
DxgkEngGetRedirBitmapSharedHandle(
    _In_ HDC hdc,
    _Out_ PVOID *phSurface)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hdc);

    if (phSurface != NULL)
        *phSurface = NULL;
}

static
VOID
APIENTRY
DxgkEngAddRedirBitmapD3DDirtyRgn(
    _In_ HDC hdc,
    _In_ PVOID pPresentInfo)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hdc);
    UNREFERENCED_PARAMETER(pPresentInfo);
}

static
VOID
APIENTRY
DxgkEngAccumD3DPresentBounds(
    _In_ HDC hdc,
    _In_ PVOID prcl)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hdc);
    UNREFERENCED_PARAMETER(prcl);
}

static
LONG
APIENTRY
DxgkEngRefPresentHistoryToken(
    _In_ PVOID pToken)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(pToken);

    return 0;
}

static
VOID
APIENTRY
DxgkEngAcquireStableVisRgn(
    _In_ HDC hdc)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hdc);
}

static
VOID
APIENTRY
DxgkEngReleaseStableVisRgn(VOID)
{
    UNIMPLEMENTED_ONCE;
}

static
VOID
APIENTRY
DxgkEngAcquireStableSprite(
    _In_ PVOID pvDev,
    _In_ LONG lFlags)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(pvDev);
    UNREFERENCED_PARAMETER(lFlags);
}

static
VOID
APIENTRY
DxgkEngReleaseStableSprite(
    _In_ PVOID pvDev,
    _In_ LONG lFlags)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(pvDev);
    UNREFERENCED_PARAMETER(lFlags);
}

static
VOID
APIENTRY
DxgkEngWatchVisRgnChange(
    _In_ HDC hdc,
    _In_ LONG lWatch)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hdc);
    UNREFERENCED_PARAMETER(lWatch);
}

static
LONG
APIENTRY
DxgkEngFindViewDesktopPosition(
    _In_ const LUID *pAdapterLuid,
    _In_ ULONG VidPnSourceId,
    _Out_ PPOINTL pptl)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(pAdapterLuid);
    UNREFERENCED_PARAMETER(VidPnSourceId);

    if (pptl != NULL)
        RtlZeroMemory(pptl, sizeof(*pptl));

    return 0;
}

static
LONG
APIENTRY
DxgkEngIsDwmComposing(
    _Out_ PBOOL pbComposing)
{
    if (pbComposing != NULL)
        *pbComposing = FALSE;

    return 0;
}

static
LONG
APIENTRY
DxgkEngQuerySwapChainBindingStatus(
    _In_ LONG lIndex,
    _Out_ PVOID pToken)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(lIndex);
    UNREFERENCED_PARAMETER(pToken);

    return 0;
}

static
LONG
APIENTRY
DxgkEngGetRedirectedWindowOrigin(
    _In_ HDC hdc,
    _Out_ PPOINT ppt)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hdc);

    if (ppt != NULL)
        RtlZeroMemory(ppt, sizeof(*ppt));

    return 0;
}

static
LONG
APIENTRY
DxgkEngAdjustMonitorPosition(
    _Inout_ LPRECT prcl,
    _In_ ULONG ulFlags1,
    _In_ ULONG ulFlags2)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(prcl);
    UNREFERENCED_PARAMETER(ulFlags1);
    UNREFERENCED_PARAMETER(ulFlags2);

    return 0;
}

static
ULONG
APIENTRY
DxgkEngGetRemoteDeviceCount(VOID)
{
    return 0;
}

/* dxgkrnl watches this for display changes it has to notice */
static
volatile LONG *
APIENTRY
DxgkEngGetAdapterUniquenessPointer(VOID)
{
    return &glAdapterUniqueness;
}

static
VOID
APIENTRY
DxgkEngIncSpritetUniq(VOID)
{
    gulSpriteUniq++;
}

static
LONG
APIENTRY
DxgkEngQueryWin32Info(
    _Inout_ PVOID pInfo)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(pInfo);

    return 0;
}

static
LONG
APIENTRY
DxgkEngGetWindowRect(
    _In_ HWND hwnd,
    _Out_ LPRECT prcl)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hwnd);

    if (prcl != NULL)
        RtlZeroMemory(prcl, sizeof(*prcl));

    return 0;
}

static
VOID
APIENTRY
DxgkEngNotifyDisplayChange(VOID)
{
    UNIMPLEMENTED_ONCE;
}

/* GLOBALS ********************************************************************/

DXGKWIN32KENG_INTERFACE gDxgkWin32kEngInterface =
{
    sizeof(DXGKWIN32KENG_INTERFACE),
    DXGKWIN32KENG_INTERFACE_VERSION,
    DxgkEngVisRgnUniq,
    DxgkEngLockVisRgn,
    DxgkEngUnlockVisRgn,
    DxgkEngEnterUserCrit,
    DxgkEngLeaveUserCrit,
    DxgkEngGetDC,
    DxgkEngIsRedirectionDC,
    DxgkEngReleaseDC,
    DxgkEngGetClientRect,
    DxgkEngCreateRectRgn,
    DxgkEngGetVisRgn,
    DxgkEngSetRgn,
    DxgkEngCombineRgn,
    DxgkEngGetRgnData,
    DxgkEngGetBoxRgn,
    DxgkEngDeleteObject,
    DxgkEngDetectGDIPath,
    DxgkEngBltViaGDI,
    DxgkEngColorFillViaGDI,
    DxgkEngLockShareSem,
    DxgkEngUnlockShareSem,
    DxgkEngAcquireWin32kAndPDEVLocks,
    DxgkEngAssertGdiOutput,
    DxgkEngResetPointer,
    DxgkEngReleaseWin32kAndPDEVLocks,
    DxgkEngScreenAccessCheck,
    DxgkEngIsDwmProcess,
    DxgkEngIsRemoteConnection,
    DxgkEngGetRedirBitmapSharedHandle,
    DxgkEngAddRedirBitmapD3DDirtyRgn,
    DxgkEngAccumD3DPresentBounds,
    DxgkEngRefPresentHistoryToken,
    DxgkEngAcquireStableVisRgn,
    DxgkEngReleaseStableVisRgn,
    DxgkEngAcquireStableSprite,
    DxgkEngReleaseStableSprite,
    DxgkEngWatchVisRgnChange,
    DxgkEngFindViewDesktopPosition,
    DxgkEngIsDwmComposing,
    DxgkEngQuerySwapChainBindingStatus,
    DxgkEngGetRedirectedWindowOrigin,
    DxgkEngAdjustMonitorPosition,
    DxgkEngGetRemoteDeviceCount,
    DxgkEngGetAdapterUniquenessPointer,
    DxgkEngIncSpritetUniq,
    DxgkEngQueryWin32Info,
    DxgkEngGetWindowRect,
    DxgkEngNotifyDisplayChange
};

/* EOF */
