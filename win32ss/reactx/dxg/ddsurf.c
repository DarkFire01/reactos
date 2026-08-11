/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * PURPOSE:          Native driver for dxg implementation
 * FILE:             win32ss/reactx/dxg/ddsurf.c
 * PROGRAMER:        Sebastian Gasiorek (sebastian.gasiorek@reactos.org)
 */

#include <dxg_int.h>
#include <debug.h>
/*++
* @name DxDdLock
* @implemented
*
* The function DxDdLock locks the surface and calls
* MapMemory driver function to assign surface memory.
* Surface memory is returned in mapMemoryData.fpProcess variable
*
* @param HANDLE hSurface
* Handle to DirectDraw surface
*
* @param PDD_LOCKDATA puLockData
* Structure with lock details
*
* @param HDC hdcClip
* Reserved
*
* @return
* Returns DDHAL_DRIVER_HANDLED or DDHAL_DRIVER_NOTHANDLED. 
*
* @remarks.
* Missing lock data and error handling.
*--*/
DWORD
NTAPI
DxDdLock(HANDLE hSurface,
         PDD_LOCKDATA puLockData,
         HDC hdcClip)
{
    PEDD_SURFACE pSurface;
    PEDD_DIRECTDRAW_LOCAL peDdL;
    PEDD_DIRECTDRAW_GLOBAL peDdGl;
    DD_MAPMEMORYDATA mapMemoryData;
    DD_LOCKDATA lockData;
    DWORD retVal = DDHAL_DRIVER_NOTHANDLED;

    if (!puLockData)
        return retVal;

    pSurface = (PEDD_SURFACE)DdHmgLock(hSurface, ObjType_DDSURFACE_TYPE, FALSE);
    if (!pSurface)
    {
        puLockData->ddRVal = DDERR_INVALIDOBJECT;
        return DDHAL_DRIVER_HANDLED;
    }

    peDdL = pSurface->peDirectDrawLocal;
    peDdGl = peDdL->peDirectDrawGlobal2;

    /* Some callers create/reuse a surface handle before DXG fully wires it up.
     * Miniport callbacks (e.g. framebuf DdLock) require lpGbl to be valid. */
    if (!pSurface->ddsSurfaceLocal.lpGbl)
        pSurface->ddsSurfaceLocal.lpGbl = &pSurface->ddsSurfaceGlobal;
    if (!pSurface->ddsSurfaceLocal.lpSurfMore)
        pSurface->ddsSurfaceLocal.lpSurfMore = &pSurface->ddsSurfaceMore;

    puLockData->lpDD = (PDD_DIRECTDRAW_GLOBAL)peDdGl;
    puLockData->lpDDSurface = &pSurface->ddsSurfaceLocal;
    puLockData->lpSurfData = NULL;

    RtlZeroMemory(&lockData, sizeof(lockData));
    lockData.lpDD = (PDD_DIRECTDRAW_GLOBAL)peDdGl;
    lockData.lpDDSurface = &pSurface->ddsSurfaceLocal;
    lockData.bHasRect = puLockData->bHasRect;
    lockData.rArea = puLockData->rArea;
    lockData.dwFlags = puLockData->dwFlags;
    lockData.ddRVal = puLockData->ddRVal;

    /* Map memory if it's not already mapped and driver function is provided */
    if (!peDdL->isMemoryMapped && (peDdGl->ddCallbacks.dwFlags & DDHAL_CB32_MAPMEMORY))
    {
        mapMemoryData.bMap = 1;
        mapMemoryData.hProcess = (HANDLE)-1;
        mapMemoryData.fpProcess = 0;
        mapMemoryData.lpDD = (PDD_DIRECTDRAW_GLOBAL)peDdGl;
        mapMemoryData.ddRVal = DDERR_CURRENTLYNOTAVAIL;

        peDdGl->ddCallbacks.MapMemory(&mapMemoryData);

        if (!mapMemoryData.ddRVal)
        {
            peDdL->isMemoryMapped = 1;
            peDdL->fpProcess2 = mapMemoryData.fpProcess;
        }
    }

    lockData.fpProcess = peDdL->fpProcess2;

    if ((peDdGl->ddSurfaceCallbacks.dwFlags & DDHAL_SURFCB32_LOCK) &&
        peDdGl->ddSurfaceCallbacks.Lock)
    {
        DPRINT1("DXG: DxDdLock calling miniport Lock: lpDD=%p dhpdev=%p lpDDSurface=%p lpGbl=%p fpVidMem=%lx lPitch=%ld\n",
                 lockData.lpDD,
                 lockData.lpDD ? lockData.lpDD->dhpdev : NULL,
                 lockData.lpDDSurface,
                 lockData.lpDDSurface ? lockData.lpDDSurface->lpGbl : NULL,
                 lockData.lpDDSurface && lockData.lpDDSurface->lpGbl ? (ULONG)lockData.lpDDSurface->lpGbl->fpVidMem : 0,
                 lockData.lpDDSurface && lockData.lpDDSurface->lpGbl ? lockData.lpDDSurface->lpGbl->lPitch : 0);
        retVal = peDdGl->ddSurfaceCallbacks.Lock(&lockData);
        DPRINT1("DXG: DxDdLock miniport returned retVal=0x%lx ddRVal=0x%lx lpSurfData=%p\n",
                 retVal, lockData.ddRVal, lockData.lpSurfData);
    }
    else
    {
        DPRINT1("DXG: DxDdLock missing lock callback flags=0x%lx\n",
                 peDdGl->ddSurfaceCallbacks.dwFlags);
    }

    if (retVal == DDHAL_DRIVER_HANDLED &&
        (!lockData.lpSurfData || lockData.ddRVal != DD_OK))
    {
        ULONG bytesPerPixel;
        LONG pitch;
        LONG left = 0;
        LONG top = 0;
        /*
         * Translate a FLAT fpVidMem into a CPU pointer.
         *
         * IMPORTANT: fpVidMem is relative to the video memory base, while pvPrimary
         * points at fpPrimary. So the correct base is (pvPrimary - fpPrimary).
         */
        PBYTE base = (PBYTE)peDdGl->ddHalInfo.vmiData.pvPrimary;
        HSURF primarySurface = NULL;
        FLATPTR fpPrimary = peDdGl->ddHalInfo.vmiData.fpPrimary;

        bytesPerPixel = pSurface->ddsSurfaceGlobal.ddpfSurface.dwRGBBitCount / 8;
        if (!bytesPerPixel)
            bytesPerPixel = peDdGl->ddHalInfo.vmiData.ddpfDisplay.dwRGBBitCount / 8;

        pitch = pSurface->ddsSurfaceGlobal.lPitch;
        if (!pitch)
            pitch = peDdGl->ddHalInfo.vmiData.lDisplayPitch;

        if (base && fpPrimary)
            base -= fpPrimary;

        if (!base)
        {
            primarySurface = (HSURF)gpEngFuncs.DxEngGetHdevData(peDdGl->hDev, DxEGShDevData_Surface);
            if (primarySurface)
            {
                SURFOBJ *pso = gpEngFuncs.DxEngAltLockSurface(primarySurface);
                if (pso)
                {
                    base = (PBYTE)pso->pvScan0;
                    if (!pitch)
                        pitch = pso->lDelta;
                }
            }
        }

        DPRINT1("DXG: DxDdLock fallback base=%p fp=%lx pitch=%ld bpp=%lu\n",
                 base,
                 (ULONG)pSurface->ddsSurfaceGlobal.fpVidMem,
                 pitch,
                 bytesPerPixel);

        if (base && bytesPerPixel && pitch)
        {
            if (pSurface->ddsSurfaceGlobal.fpVidMem)
                base += pSurface->ddsSurfaceGlobal.fpVidMem;

            if (lockData.bHasRect)
            {
                left = lockData.rArea.left;
                top = lockData.rArea.top;
            }

            lockData.lpSurfData = base + (top * pitch) + (left * bytesPerPixel);
            lockData.ddRVal = DD_OK;
        }
        else
        {
            DPRINT1("DXG: DxDdLock fallback failed base=%p bpp=%lu pitch=%ld\n",
                     base, bytesPerPixel, pitch);
        }
    }

    puLockData->lpSurfData = lockData.lpSurfData;
    puLockData->ddRVal = lockData.ddRVal;

    InterlockedDecrement((VOID*)&pSurface->pobj.cExclusiveLock);

    if (retVal == DDHAL_DRIVER_NOTHANDLED)
        puLockData->ddRVal = DDERR_UNSUPPORTED;
    DPRINT1("DXG: DxDdLock retVal=0x%lx ddRVal=0x%lx surfData=%p\n",
             retVal, puLockData->ddRVal, puLockData->lpSurfData);

    return retVal;
}

/*++
* @name DxDdUnlock
* @unimplemented
*
* The function DxDdUnlock releases the lock from specified surface
*
* @param HANDLE hSurface
* Handle to DirectDraw surface
*
* @param PDD_UNLOCKDATA puUnlockData
* Structure with lock details
*
* @return
* Returns DDHAL_DRIVER_HANDLED or DDHAL_DRIVER_NOTHANDLED. 
*
* @remarks.
* Stub
*--*/
DWORD
NTAPI
DxDdUnlock(HANDLE hSurface,
           PDD_UNLOCKDATA puUnlockData)
{
    PEDD_SURFACE pSurface;
    PEDD_DIRECTDRAW_LOCAL peDdL;
    PEDD_DIRECTDRAW_GLOBAL peDdGl;
    DWORD retVal = DDHAL_DRIVER_NOTHANDLED;

    if (!puUnlockData)
        return retVal;

    pSurface = (PEDD_SURFACE)DdHmgLock(hSurface, ObjType_DDSURFACE_TYPE, FALSE);
    if (!pSurface)
    {
        puUnlockData->ddRVal = DDERR_INVALIDOBJECT;
        return DDHAL_DRIVER_HANDLED;
    }

    peDdL = pSurface->peDirectDrawLocal;
    peDdGl = peDdL->peDirectDrawGlobal2;

    puUnlockData->lpDD = (PDD_DIRECTDRAW_GLOBAL)peDdGl;
    puUnlockData->lpDDSurface = &pSurface->ddsSurfaceLocal;

    if ((peDdGl->ddSurfaceCallbacks.dwFlags & DDHAL_SURFCB32_UNLOCK) &&
        peDdGl->ddSurfaceCallbacks.Unlock)
    {
        retVal = peDdGl->ddSurfaceCallbacks.Unlock(puUnlockData);
    }

    InterlockedDecrement((VOID*)&pSurface->pobj.cExclusiveLock);

    if (retVal == DDHAL_DRIVER_NOTHANDLED)
        puUnlockData->ddRVal = DDERR_UNSUPPORTED;

    return retVal;
}
