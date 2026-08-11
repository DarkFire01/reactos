
#define TRACE() \
    DbgPrint("DXG: %s\n", __FUNCTION__)

DWORD
NTAPI
DxD3dContextCreate(
    PVOID p1,
    PVOID p2,
    PVOID p3,
    PVOID p4)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxD3dContextDestroy(
    PVOID p1)
{
    TRACE();
    return 0;
}


DWORD
NTAPI
DxD3dContextDestroyAll(
    PVOID p1)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxD3dValidateTextureStageState(
    PVOID p1)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxD3dDrawPrimitives2(
    PVOID p1,
    PVOID p2,
    PVOID p3,
    PVOID p4,
    PVOID p5,
    PVOID p6,
    PVOID p7)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdGetDriverState(
    PVOID p1)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdAddAttachedSurface(
    PVOID p1,
    PVOID p2,
    PVOID p3)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdAlphaBlt(
    PVOID p1,
    PVOID p2,
    PVOID p3)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdAttachSurface(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdBeginMoCompFrame(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdBlt(
    HANDLE hSurfaceDest,
    HANDLE hSurfaceSrc,
    PDD_BLTDATA puBltData)
{
    PEDD_SURFACE pSurfaceDest = NULL;
    PEDD_SURFACE pSurfaceSrc = NULL;
    PEDD_DIRECTDRAW_LOCAL peDdL = NULL;
    PEDD_DIRECTDRAW_GLOBAL peDdGl = NULL;
    DWORD retVal = DDHAL_DRIVER_NOTHANDLED;

    if (!puBltData)
        return retVal;

    pSurfaceDest = (PEDD_SURFACE)DdHmgLock(hSurfaceDest, ObjType_DDSURFACE_TYPE, FALSE);
    if (!pSurfaceDest)
    {
        puBltData->ddRVal = DDERR_INVALIDOBJECT;
        return DDHAL_DRIVER_HANDLED;
    }

    peDdL = pSurfaceDest->peDirectDrawLocal;
    peDdGl = peDdL->peDirectDrawGlobal2;

    puBltData->lpDD = (PDD_DIRECTDRAW_GLOBAL)peDdGl;
    puBltData->lpDDDestSurface = &pSurfaceDest->ddsSurfaceLocal;

    if (hSurfaceSrc)
    {
        pSurfaceSrc = (PEDD_SURFACE)DdHmgLock(hSurfaceSrc, ObjType_DDSURFACE_TYPE, FALSE);
        if (!pSurfaceSrc)
        {
            puBltData->ddRVal = DDERR_INVALIDOBJECT;
            InterlockedDecrement((VOID*)&pSurfaceDest->pobj.cExclusiveLock);
            return DDHAL_DRIVER_HANDLED;
        }

        puBltData->lpDDSrcSurface = &pSurfaceSrc->ddsSurfaceLocal;
    }

    if ((peDdGl->ddSurfaceCallbacks.dwFlags & DDHAL_SURFCB32_BLT) &&
        peDdGl->ddSurfaceCallbacks.Blt)
    {
        retVal = peDdGl->ddSurfaceCallbacks.Blt(puBltData);
    }

    if (pSurfaceSrc)
        InterlockedDecrement((VOID*)&pSurfaceSrc->pobj.cExclusiveLock);
    InterlockedDecrement((VOID*)&pSurfaceDest->pobj.cExclusiveLock);

    if (retVal == DDHAL_DRIVER_NOTHANDLED)
        puBltData->ddRVal = DDERR_UNSUPPORTED;

    return retVal;
}

DWORD
NTAPI
DxDdColorControl(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdCreateMoComp(
    PVOID p1,
    PVOID p2)
{
    return 0;
}

DWORD
NTAPI
DxDdDeleteDirectDrawObject(
    PVOID p1)
{
    return 0;
}

DWORD
NTAPI
DxDdDeleteSurfaceObject(
    PVOID p1)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdDestroyMoComp(
    PVOID p1,
    PVOID p2)
{
    return 0;
}

DWORD
NTAPI
DxDdDestroySurface(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdDestroyD3DBuffer(
    PVOID p1)
{
    return 0;
}

DWORD
NTAPI
DxDdEndMoCompFrame(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdFlip(
    HANDLE hSurfaceCurrent,
    HANDLE hSurfaceTarget,
    HANDLE hSurfaceCurrentLeft,
    HANDLE hSurfaceTargetLeft,
    PDD_FLIPDATA puFlipData)
{
    PEDD_SURFACE pSurfaceCurrent = NULL;
    PEDD_SURFACE pSurfaceTarget = NULL;
    PEDD_SURFACE pSurfaceCurrentLeft = NULL;
    PEDD_SURFACE pSurfaceTargetLeft = NULL;
    PEDD_DIRECTDRAW_LOCAL peDdL = NULL;
    PEDD_DIRECTDRAW_GLOBAL peDdGl = NULL;
    DWORD retVal = DDHAL_DRIVER_NOTHANDLED;

    if (!puFlipData)
        return retVal;

    pSurfaceCurrent = (PEDD_SURFACE)DdHmgLock(hSurfaceCurrent, ObjType_DDSURFACE_TYPE, FALSE);
    if (!pSurfaceCurrent)
    {
        puFlipData->ddRVal = DDERR_INVALIDOBJECT;
        return DDHAL_DRIVER_HANDLED;
    }

    peDdL = pSurfaceCurrent->peDirectDrawLocal;
    peDdGl = peDdL->peDirectDrawGlobal2;

    puFlipData->lpDD = (PDD_DIRECTDRAW_GLOBAL)peDdGl;
    puFlipData->lpSurfCurr = &pSurfaceCurrent->ddsSurfaceLocal;

    if (hSurfaceTarget)
    {
        pSurfaceTarget = (PEDD_SURFACE)DdHmgLock(hSurfaceTarget, ObjType_DDSURFACE_TYPE, FALSE);
        if (!pSurfaceTarget)
        {
            puFlipData->ddRVal = DDERR_INVALIDOBJECT;
            InterlockedDecrement((VOID*)&pSurfaceCurrent->pobj.cExclusiveLock);
            return DDHAL_DRIVER_HANDLED;
        }
        puFlipData->lpSurfTarg = &pSurfaceTarget->ddsSurfaceLocal;
    }

    if (hSurfaceCurrentLeft)
    {
        pSurfaceCurrentLeft = (PEDD_SURFACE)DdHmgLock(hSurfaceCurrentLeft, ObjType_DDSURFACE_TYPE, FALSE);
        if (!pSurfaceCurrentLeft)
        {
            puFlipData->ddRVal = DDERR_INVALIDOBJECT;
            if (pSurfaceTarget)
                InterlockedDecrement((VOID*)&pSurfaceTarget->pobj.cExclusiveLock);
            InterlockedDecrement((VOID*)&pSurfaceCurrent->pobj.cExclusiveLock);
            return DDHAL_DRIVER_HANDLED;
        }
        puFlipData->lpSurfCurrLeft = &pSurfaceCurrentLeft->ddsSurfaceLocal;
    }

    if (hSurfaceTargetLeft)
    {
        pSurfaceTargetLeft = (PEDD_SURFACE)DdHmgLock(hSurfaceTargetLeft, ObjType_DDSURFACE_TYPE, FALSE);
        if (!pSurfaceTargetLeft)
        {
            puFlipData->ddRVal = DDERR_INVALIDOBJECT;
            if (pSurfaceCurrentLeft)
                InterlockedDecrement((VOID*)&pSurfaceCurrentLeft->pobj.cExclusiveLock);
            if (pSurfaceTarget)
                InterlockedDecrement((VOID*)&pSurfaceTarget->pobj.cExclusiveLock);
            InterlockedDecrement((VOID*)&pSurfaceCurrent->pobj.cExclusiveLock);
            return DDHAL_DRIVER_HANDLED;
        }
        puFlipData->lpSurfTargLeft = &pSurfaceTargetLeft->ddsSurfaceLocal;
    }

    if ((peDdGl->ddSurfaceCallbacks.dwFlags & DDHAL_SURFCB32_FLIP) &&
        peDdGl->ddSurfaceCallbacks.Flip)
    {
        retVal = peDdGl->ddSurfaceCallbacks.Flip(puFlipData);
    }

    if (pSurfaceTargetLeft)
        InterlockedDecrement((VOID*)&pSurfaceTargetLeft->pobj.cExclusiveLock);
    if (pSurfaceCurrentLeft)
        InterlockedDecrement((VOID*)&pSurfaceCurrentLeft->pobj.cExclusiveLock);
    if (pSurfaceTarget)
        InterlockedDecrement((VOID*)&pSurfaceTarget->pobj.cExclusiveLock);
    InterlockedDecrement((VOID*)&pSurfaceCurrent->pobj.cExclusiveLock);

    if (retVal == DDHAL_DRIVER_NOTHANDLED)
        puFlipData->ddRVal = DDERR_UNSUPPORTED;

    return retVal;
}

DWORD
NTAPI
DxDdFlipToGDISurface(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdGetAvailDriverMemory(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdGetBltStatus(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

#ifndef GDI_OBJ_HMGR_POWNED
/* win32ss/include/ntgdihdl.h */
#define GDI_OBJ_HMGR_POWNED     0x80000002
#endif

static inline ULONG
DxgBppToBitmapFormat(ULONG bpp)
{
    switch (bpp)
    {
        case 1:  return BMF_1BPP;
        case 4:  return BMF_4BPP;
        case 8:  return BMF_8BPP;
        case 16: return BMF_16BPP;
        case 24: return BMF_24BPP;
        case 32: return BMF_32BPP;
        default: return 0;
    }
}

HDC
NTAPI
DxDdGetDC(
    HANDLE hSurface,
    PALETTEENTRY *puColorTable)
{
    PEDD_SURFACE SurfaceObj;
    PEDD_DIRECTDRAW_LOCAL DdLocal;
    PEDD_DIRECTDRAW_GLOBAL DdGlobal;
    HDC DeviceContext = NULL;
    LONG SurfacePitch;
    ULONG BitsPerPixel;
    ULONG BitmapFormat;
    SIZEL SurfaceSize;
    HBITMAP BitmapHandle;
    PBYTE SurfaceMemory;
    LONG MemoryStride;
    ULONG BitmapFlags;
    PBYTE VideoMemoryBase;
    HSURF PrimaryGdiSurface;
    SURFOBJ *SurfaceObject;
    FLATPTR PrimaryOffset;
    ULONG PaletteColorCount = 0;
    HPALETTE PaletteHandle = NULL;
    ULONG PaletteMode;
    BOOLEAN Success = FALSE;

    DbgPrint("DXG: DxDdGetDC enter hSurface=%p\n", hSurface);

    SurfaceObj = (PEDD_SURFACE)DdHmgLock(hSurface, ObjType_DDSURFACE_TYPE, FALSE);
    if (!SurfaceObj)
    {
        DbgPrint("DXG: DxDdGetDC DdHmgLock failed for hSurface=%p\n", hSurface);
        return NULL;
    }

    /* Enforce single DC per surface restriction */
    if (SurfaceObj->hdc)
    {
        DbgPrint("DXG: DxDdGetDC surface already has DC hdc=%p, returning NULL\n",
                 (PVOID)SurfaceObj->hdc);
        DeviceContext = NULL;
        goto Cleanup;
    }

    if (!SurfaceObj->peDirectDrawLocal ||
        !SurfaceObj->peDirectDrawLocal->peDirectDrawGlobal2)
    {
        goto Cleanup;
    }

    DdLocal = SurfaceObj->peDirectDrawLocal;
    DdGlobal = DdLocal->peDirectDrawGlobal2;

    /* Acquire device lock for shared data access */
    gpEngFuncs.DxEngLockHdev(DdGlobal->hDev);

    /* Calculate video memory base address for framebuffer access */
    VideoMemoryBase = (PBYTE)DdGlobal->ddHalInfo.vmiData.pvPrimary;
    PrimaryOffset = DdGlobal->ddHalInfo.vmiData.fpPrimary;
    if (VideoMemoryBase && PrimaryOffset)
    {
        VideoMemoryBase -= PrimaryOffset;
    }

    /* Try to get base from primary surface if calculation failed */
    if (!VideoMemoryBase)
    {
        PrimaryGdiSurface = (HSURF)gpEngFuncs.DxEngGetHdevData(DdGlobal->hDev, DxEGShDevData_Surface);
        if (PrimaryGdiSurface)
        {
            SurfaceObject = gpEngFuncs.DxEngAltLockSurface(PrimaryGdiSurface);
            if (SurfaceObject)
            {
                VideoMemoryBase = (PBYTE)SurfaceObject->pvScan0;
                if (!DdGlobal->ddHalInfo.vmiData.lDisplayPitch)
                {
                    DdGlobal->ddHalInfo.vmiData.lDisplayPitch = SurfaceObject->lDelta;
                }
            }
        }
    }

    if (!VideoMemoryBase)
    {
        DbgPrint("DXG: DxDdGetDC failed to compute aperture base (pvPrimary=%p fpPrimary=%lx)\n",
                 DdGlobal->ddHalInfo.vmiData.pvPrimary,
                 (ULONG)DdGlobal->ddHalInfo.vmiData.fpPrimary);
        goto UnlockDevice;
    }

    /* Determine pixel format and dimensions */
    BitsPerPixel = SurfaceObj->ddsSurfaceGlobal.ddpfSurface.dwRGBBitCount;
    if (!BitsPerPixel)
    {
        BitsPerPixel = DdGlobal->ddHalInfo.vmiData.ddpfDisplay.dwRGBBitCount;
    }

    BitmapFormat = DxgBppToBitmapFormat(BitsPerPixel);
    if (!BitmapFormat)
    {
        goto UnlockDevice;
    }

    SurfaceSize.cx = (LONG)SurfaceObj->ddsSurfaceGlobal.wWidth;
    SurfaceSize.cy = (LONG)SurfaceObj->ddsSurfaceGlobal.wHeight;
    if (!SurfaceSize.cx)
    {
        SurfaceSize.cx = (LONG)DdGlobal->ddHalInfo.vmiData.dwDisplayWidth;
    }
    if (!SurfaceSize.cy)
    {
        SurfaceSize.cy = (LONG)DdGlobal->ddHalInfo.vmiData.dwDisplayHeight;
    }
    if (!SurfaceSize.cx || !SurfaceSize.cy)
    {
        goto UnlockDevice;
    }

    /* Get surface pitch from various sources */
    SurfacePitch = SurfaceObj->ddsSurfaceGlobal.lPitch;
    if (!SurfacePitch && SurfaceObj->ddsSurfaceLocal.lpGbl)
    {
        SurfacePitch = SurfaceObj->ddsSurfaceLocal.lpGbl->lPitch;
    }
    if (!SurfacePitch)
    {
        SurfacePitch = DdGlobal->ddHalInfo.vmiData.lDisplayPitch;
    }
    if (!SurfacePitch)
    {
        goto UnlockDevice;
    }

    /* Calculate actual surface memory pointer */
    if (SurfaceObj->ddsSurfaceLocal.ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)
    {
        SurfaceMemory = (PBYTE)(ULONG_PTR)SurfaceObj->ddsSurfaceGlobal.fpVidMem;
    }
    else
    {
        SurfaceMemory = VideoMemoryBase + SurfaceObj->ddsSurfaceGlobal.fpVidMem;
    }

    /* Normalize stride for bitmap creation */
    MemoryStride = (SurfacePitch < 0) ? -SurfacePitch : SurfacePitch;
    if (MemoryStride <= 0)
    {
        goto UnlockDevice;
    }

    /* Handle bottom-up vs top-down memory layout */
    BitmapFlags = BMF_TOPDOWN;
    if (SurfacePitch < 0)
    {
        BitmapFlags = 0;
        SurfaceMemory = SurfaceMemory - ((SurfaceSize.cy - 1) * (ULONG)MemoryStride);
    }

    /* Setup palette for indexed color modes */
    if (BitsPerPixel <= 8)
    {
        PaletteColorCount = 1 << BitsPerPixel;

        if (puColorTable != NULL)
        {
            /* Validate color table pointer - ProbeForRead will raise exception if invalid */
            /* Note: Exception handling removed as it doesn't work in header files with PSEH3 */
            /* The caller should handle any exceptions from ProbeForRead */
            ProbeForRead(puColorTable, PaletteColorCount * sizeof(PALETTEENTRY), 1);
        }
        else if (BitsPerPixel == 8)
        {
            if (DdGlobal->ddHalInfo.vmiData.ddpfDisplay.dwRGBBitCount != 8)
            {
                DbgPrint("DXG: DxDdGetDC 8bpp surface needs color table or 8bpp primary\n");
                goto UnlockDevice;
            }
        }
    }

    /* Create or reuse GDI bitmap for surface */
    BitmapHandle = (HBITMAP)SurfaceObj->hbmGdi;
    if (!BitmapHandle)
    {
        BitmapHandle = EngCreateBitmap(SurfaceSize, MemoryStride, BitmapFormat, BitmapFlags, SurfaceMemory);
        if (!BitmapHandle)
        {
            goto UnlockDevice;
        }
        SurfaceObj->hbmGdi = BitmapHandle;
        DbgPrint("DXG: DxDdGetDC created new bitmap hbmp=%p\n", BitmapHandle);
    }
    else
    {
        DbgPrint("DXG: DxDdGetDC reusing existing bitmap hbmp=%p\n", BitmapHandle);
    }

    /* Configure palette for indexed color surfaces */
    if (PaletteColorCount > 0)
    {
        SurfaceObject = gpEngFuncs.DxEngAltLockSurface((HSURF)BitmapHandle);
        if (!SurfaceObject)
        {
            DbgPrint("DXG: DxDdGetDC failed to lock bitmap surface\n");
            goto UnlockDevice;
        }

        if (puColorTable != NULL)
        {
            PaletteMode = (BitmapFormat <= BMF_8BPP) ? PAL_INDEXED : PAL_BITFIELDS;
            PaletteHandle = EngCreatePalette(PaletteMode,
                                            PaletteColorCount,
                                            (ULONG*)puColorTable,
                                            SurfaceObj->ddsSurfaceGlobal.ddpfSurface.dwRBitMask,
                                            SurfaceObj->ddsSurfaceGlobal.ddpfSurface.dwGBitMask,
                                            SurfaceObj->ddsSurfaceGlobal.ddpfSurface.dwBBitMask);
            if (!PaletteHandle)
            {
                DbgPrint("DXG: DxDdGetDC EngCreatePalette failed\n");
                EngUnlockSurface(SurfaceObject);
                goto UnlockDevice;
            }

            if (gpEngFuncs.DxEngSelectPaletteToSurface)
            {
                gpEngFuncs.DxEngSelectPaletteToSurface((DWORD_PTR)SurfaceObject, (DWORD_PTR)PaletteHandle);
            }
            else if (gpEngFuncs.DxEngUploadPaletteEntryToSurface)
            {
                gpEngFuncs.DxEngUploadPaletteEntryToSurface((DWORD_PTR)DdGlobal->hDev,
                                                             (DWORD_PTR)SurfaceObject,
                                                             (DWORD_PTR)puColorTable,
                                                             PaletteColorCount);
            }
            EngDeletePalette(PaletteHandle);
        }
        else if (PaletteColorCount == 256 && BitsPerPixel == 8)
        {
            if (gpEngFuncs.DxEngSyncPaletteTableWithDevice)
            {
                /* Default palette should be set by EngCreateBitmap */
            }
        }

        EngUnlockSurface(SurfaceObject);
    }

    /* Create memory DC for surface access */
    DeviceContext = gpEngFuncs.DxEngCreateMemoryDC(DdGlobal->hDev);
    if (!DeviceContext)
    {
        DbgPrint("DXG: DxDdGetDC DxEngCreateMemoryDC failed\n");
        goto UnlockDevice;
    }

    /* Mark objects as process-owned for usermode access */
    gpEngFuncs.DxEngSetDCOwner(DeviceContext, GDI_OBJ_HMGR_POWNED);
    gpEngFuncs.DxEngSetBitmapOwner(BitmapHandle, GDI_OBJ_HMGR_POWNED);

    gpEngFuncs.DxEngSelectBitmap(DeviceContext, BitmapHandle);
    SurfaceObj->hdc = DeviceContext;
    Success = TRUE;

    DbgPrint("DXG: DxDdGetDC success hSurface=%p hdc=%p base=%p fpVidMem=%lx pitch=%ld bpp=%lu size=%ldx%ld\n",
             hSurface,
             DeviceContext,
             VideoMemoryBase,
             (ULONG)SurfaceObj->ddsSurfaceGlobal.fpVidMem,
             SurfacePitch,
             BitsPerPixel,
             SurfaceSize.cx,
             SurfaceSize.cy);

UnlockDevice:
    gpEngFuncs.DxEngUnlockHdev(DdGlobal->hDev);

Cleanup:
    InterlockedDecrement((VOID*)&SurfaceObj->pobj.cExclusiveLock);
    return Success ? DeviceContext : NULL;
}

HANDLE
NTAPI
DxDdGetDxHandle(
    HANDLE hDirectDraw,
    HANDLE hSurface,
    BOOL bRelease)
{
    (void)bRelease; /* ReactOS doesn't implement DxApi surface refcounting yet */

    if (hDirectDraw)
    {
        PEDD_DIRECTDRAW_LOCAL peDdL = (PEDD_DIRECTDRAW_LOCAL)DdHmgLock(hDirectDraw, ObjType_DDLOCAL_TYPE, FALSE);
        if (!peDdL)
            return NULL;
        InterlockedDecrement((VOID*)&peDdL->pobj.cExclusiveLock);
        return hDirectDraw;
    }

    if (hSurface)
    {
        PEDD_SURFACE pSurface = (PEDD_SURFACE)DdHmgLock(hSurface, ObjType_DDSURFACE_TYPE, FALSE);
        if (!pSurface)
            return NULL;
        InterlockedDecrement((VOID*)&pSurface->pobj.cExclusiveLock);
        return hSurface;
    }

    return NULL;
}

DWORD
NTAPI
DxDdGetFlipStatus(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdGetInternalMoCompInfo(
    PVOID p1,
    PVOID p2)
{
    return 0;
}

DWORD
NTAPI
DxDdGetMoCompBuffInfo(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdGetMoCompGuids(
    PVOID p1,
    PVOID p2)
{
    return 0;
}

DWORD
NTAPI
DxDdGetMoCompFormats(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdGetScanLine(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdLockD3D(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdQueryMoCompStatus(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdReleaseDC(
    HANDLE hSurface)
{
    PEDD_SURFACE SurfaceObj;
    PEDD_DIRECTDRAW_GLOBAL DdGlobal;
    HDC DeviceContext;

    SurfaceObj = (PEDD_SURFACE)DdHmgLock(hSurface, ObjType_DDSURFACE_TYPE, FALSE);
    if (!SurfaceObj)
    {
        return FALSE;
    }

    DeviceContext = (HDC)SurfaceObj->hdc;
    if (DeviceContext)
    {
        DdGlobal = SurfaceObj->peDirectDrawLocal ?
                   SurfaceObj->peDirectDrawLocal->peDirectDrawGlobal2 : NULL;

        /* Reset DC state before deletion */
        gpEngFuncs.DxEngCleanDC(DeviceContext);

        /* Future: Handle surface unlock if DD_SURFACE_FLAG_BITMAP_NEEDS_LOCKING is implemented */
        if (DdGlobal)
        {
            /* Surface unlock would go here when flag support is added */
        }

        gpEngFuncs.DxEngDeleteDC(DeviceContext, TRUE);
        SurfaceObj->hdc = NULL;

        InterlockedDecrement((VOID*)&SurfaceObj->pobj.cExclusiveLock);
        return TRUE;
    }

    InterlockedDecrement((VOID*)&SurfaceObj->pobj.cExclusiveLock);
    return FALSE;
}

DWORD
NTAPI
DxDdRenderMoComp(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdResetVisrgn(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdSetColorKey(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdSetExclusiveMode(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdSetGammaRamp(
    PVOID p1,
    PVOID p2,
    PVOID p3)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdCreateSurfaceEx(
    PVOID p1,
    PVOID p2,
    PVOID p3)
{
    TRACE();
    return 0;
}

FLATPTR
NTAPI
DxDdHeapVidMemAllocAligned(
    LPVIDMEM DdrawVidMem,
    DWORD Width,
    DWORD Height,
    LPSURFACEALIGNMENT Alignment,
    LPDWORD ResolvedPitch);

DWORD
NTAPI
DxDdCreateSurface(
    HANDLE hDirectDrawLocal,
    HANDLE *hSurface,
    DDSURFACEDESC *puSurfaceDescription,
    DD_SURFACE_GLOBAL *puSurfaceGlobalData,
    DD_SURFACE_LOCAL *puSurfaceLocalData,
    DD_SURFACE_MORE *puSurfaceMoreData,
    PDD_CREATESURFACEDATA puCreateSurfaceData,
    HANDLE *puhSurface)
{
    PEDD_DIRECTDRAW_LOCAL peDdL;
    PEDD_DIRECTDRAW_GLOBAL peDdGl;
    PDD_SURFACE_LOCAL *surfaceList = NULL;
    PDD_SURFACE_LOCAL *userSurfaceList = NULL;
    DDSURFACEDESC *userSurfaceDesc = NULL;
    DWORD retVal = DDHAL_DRIVER_NOTHANDLED;
    ULONG i;
    DWORD requestedCaps;

    if (!puCreateSurfaceData || !puSurfaceLocalData || !puSurfaceGlobalData || !puSurfaceMoreData)
        return retVal;

    if (!puCreateSurfaceData->dwSCnt)
    {
        puCreateSurfaceData->ddRVal = DDERR_INVALIDPARAMS;
        return DDHAL_DRIVER_HANDLED;
    }

    peDdL = (PEDD_DIRECTDRAW_LOCAL)DdHmgLock(hDirectDrawLocal, ObjType_DDLOCAL_TYPE, FALSE);
    if (!peDdL)
    {
        puCreateSurfaceData->ddRVal = DDERR_INVALIDOBJECT;
        return DDHAL_DRIVER_HANDLED;
    }

    peDdGl = peDdL->peDirectDrawGlobal2;
    puCreateSurfaceData->lpDD = (PDD_DIRECTDRAW_GLOBAL)peDdGl;

    surfaceList = EngAllocMem(FL_ZERO_MEMORY,
                              puCreateSurfaceData->dwSCnt * sizeof(PDD_SURFACE_LOCAL),
                              TAG_GDDP);
    if (!surfaceList)
    {
        puCreateSurfaceData->ddRVal = DDERR_OUTOFMEMORY;
        InterlockedDecrement((VOID*)&peDdL->pobj.cExclusiveLock);
        return DDHAL_DRIVER_HANDLED;
    }

    for (i = 0; i < puCreateSurfaceData->dwSCnt; i++)
    {
        puSurfaceLocalData[i].lpGbl = &puSurfaceGlobalData[i];
        puSurfaceLocalData[i].lpSurfMore = &puSurfaceMoreData[i];
        surfaceList[i] = &puSurfaceLocalData[i];
    }

    userSurfaceList = puCreateSurfaceData->lplpSList;
    puCreateSurfaceData->lplpSList = surfaceList;

    /*
     * Windows' usermode ddraw expects the kernel to pass a safe pointer to a
     * kernel-mode DDSURFACEDESC buffer into the miniport CreateSurface callback.
     * We already have `puSurfaceDescription` as a separate argument for this.
     */
    userSurfaceDesc = puCreateSurfaceData->lpDDSurfaceDesc;
    puCreateSurfaceData->lpDDSurfaceDesc = puSurfaceDescription;

    if ((peDdGl->ddCallbacks.dwFlags & DDHAL_CB32_CREATESURFACE) &&
        peDdGl->ddCallbacks.CreateSurface)
    {
        retVal = peDdGl->ddCallbacks.CreateSurface(puCreateSurfaceData);
    }
    else
    {
        puCreateSurfaceData->ddRVal = DDERR_UNSUPPORTED;
        retVal = DDHAL_DRIVER_HANDLED;
        DbgPrint("DXG: DxDdCreateSurface missing CreateSurface callback\n");
    }

    puCreateSurfaceData->lplpSList = userSurfaceList;
    puCreateSurfaceData->lpDDSurfaceDesc = userSurfaceDesc;

    DbgPrint("DXG: DxDdCreateSurface retVal=0x%lx ddRVal=0x%lx scnt=%lu\n",
             retVal, puCreateSurfaceData->ddRVal, puCreateSurfaceData->dwSCnt);

    if (retVal == DDHAL_DRIVER_HANDLED && puCreateSurfaceData->ddRVal == DD_OK && puhSurface)
    {
        for (i = 0; i < puCreateSurfaceData->dwSCnt; i++)
        {
            HANDLE curHandle = NULL;

            if (puSurfaceGlobalData[i].fpVidMem == DDHAL_PLEASEALLOC_BLOCKSIZE)
            {
                VIDEOMEMORY *heap = NULL;
                DWORD heapIndex;
                DWORD bytesPerPixel;
                DWORD allocWidth;
                DWORD allocHeight;
                DWORD pitch = 0;

                requestedCaps = puSurfaceLocalData[i].ddsCaps.dwCaps;

                if (peDdGl->pvmList && peDdGl->dwNumHeaps)
                {
                    for (heapIndex = 0; heapIndex < peDdGl->dwNumHeaps; heapIndex++)
                    {
                        VIDEOMEMORY *candidate = &peDdGl->pvmList[heapIndex];
                        if (candidate->ddsCaps.dwCaps & requestedCaps)
                        {
                            heap = candidate;
                            break;
                        }
                    }
                    if (!heap)
                        heap = &peDdGl->pvmList[0];
                }

                bytesPerPixel = puSurfaceGlobalData[i].ddpfSurface.dwRGBBitCount / 8;
                if (bytesPerPixel == 0)
                    bytesPerPixel = peDdGl->ddHalInfo.vmiData.ddpfDisplay.dwRGBBitCount / 8;

                if (!bytesPerPixel)
                {
                    puCreateSurfaceData->ddRVal = DDERR_INVALIDPIXELFORMAT;
                    retVal = DDHAL_DRIVER_HANDLED;
                    break;
                }

                if (heap && heap->dwFlags & VIDMEM_ISLINEAR)
                {
                    allocWidth = puSurfaceGlobalData[i].dwBlockSizeX;
                    allocHeight = 1;
                }
                else
                {
                    allocWidth = puSurfaceGlobalData[i].wWidth * bytesPerPixel;
                    allocHeight = puSurfaceGlobalData[i].wHeight;
                }

                puSurfaceGlobalData[i].fpVidMem = DxDdHeapVidMemAllocAligned((LPVIDMEM)heap,
                                                                              allocWidth,
                                                                              allocHeight,
                                                                              NULL,
                                                                              &pitch);
                if (!puSurfaceGlobalData[i].fpVidMem)
                {
                    DbgPrint("DXG: DxDdCreateSurface alloc failed w=%lu h=%lu caps=0x%lx\n",
                             allocWidth, allocHeight, requestedCaps);
                    puCreateSurfaceData->ddRVal = DDERR_OUTOFMEMORY;
                    retVal = DDHAL_DRIVER_HANDLED;
                    break;
                }

                if (!puSurfaceGlobalData[i].lPitch && pitch)
                    puSurfaceGlobalData[i].lPitch = pitch;
            }

            if (hSurface)
                curHandle = hSurface[i];

            puhSurface[i] = DxDdCreateSurfaceObject(hDirectDrawLocal,
                                                    curHandle,
                                                    &puSurfaceLocalData[i],
                                                    &puSurfaceMoreData[i],
                                                    &puSurfaceGlobalData[i],
                                                    TRUE);
        }
    }

    if (surfaceList)
        EngFreeMem(surfaceList);

    InterlockedDecrement((VOID*)&peDdL->pobj.cExclusiveLock);

    if (retVal == DDHAL_DRIVER_NOTHANDLED)
        puCreateSurfaceData->ddRVal = DDERR_UNSUPPORTED;

    return retVal;
}

DWORD
NTAPI
DxDdSetOverlayPosition(
    PVOID p1,
    PVOID p2,
    PVOID p3)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdUnattachSurface(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdUnlockD3D(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdUpdateOverlay(
    PVOID p1,
    PVOID p2,
    PVOID p3)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdWaitForVerticalBlank(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDvpCanCreateVideoPort(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDvpColorControl(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDvpCreateVideoPort(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDvpDestroyVideoPort(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDvpFlipVideoPort(
    PVOID p1,
    PVOID p2,
    PVOID p3,
    PVOID p4)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDvpGetVideoPortBandwidth(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDvpGetVideoPortField(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDvpGetVideoPortFlipStatus(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDvpGetVideoPortInputFormats(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDvpGetVideoPortLine(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDvpGetVideoPortOutputFormats(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDvpGetVideoPortConnectInfo(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDvpGetVideoSignalStatus(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDvpUpdateVideoPort(
    PVOID p1,
    PVOID p2,
    PVOID p3,
    PVOID p4)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDvpWaitForVideoPortSync(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDvpAcquireNotification(
    PVOID p1,
    PVOID p2,
    PVOID p3)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDvpReleaseNotification(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

FLATPTR
NTAPI
DxDdHeapVidMemAllocAligned(
    LPVIDMEM DdrawVidMem,
    DWORD Width,
    DWORD Height,
    LPSURFACEALIGNMENT Alignment,
    LPDWORD ResolvedPitch);

DWORD
NTAPI
DxDdHeapVidMemFree(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdDisableDirectDraw(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdSuspendDirectDraw(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdResumeDirectDraw(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdDynamicModeChange(
    PVOID p1,
    PVOID p2,
    PVOID p3)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdCloseProcess(
    PVOID p1)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdGetDirectDrawBound(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdEnableDirectDrawRedirection(
    PVOID p1,
    PVOID p2)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdAllocPrivateUserMem(
    PVOID p1,
    PVOID p2,
    PVOID p3)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdFreePrivateUserMem(
    PVOID p1,
    PVOID p2)
{
    return 0;
}

DWORD
NTAPI
DxDdSetAccelLevel(
    PVOID p1,
    PVOID p2,
    PVOID p3)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdGetSurfaceLock(
    PVOID p1)
{
    TRACE();
    return 0;
}

DWORD
NTAPI
DxDdEnumLockedSurfaceRect(
    PVOID p1,
    PVOID p2,
    PVOID p3)
{
    TRACE();
    return 0;
}

DRVFN gaDxgFuncs [] =
{
    {DXG_INDEX_DxDxgGenericThunk, (PFN)DxDxgGenericThunk},
    {DXG_INDEX_DxD3dContextCreate, (PFN)DxD3dContextCreate},
    {DXG_INDEX_DxD3dContextDestroy, (PFN)DxD3dContextDestroy},
    {DXG_INDEX_DxD3dContextDestroyAll, (PFN)DxD3dContextDestroyAll},
    {DXG_INDEX_DxD3dValidateTextureStageState, (PFN)DxD3dValidateTextureStageState},
    {DXG_INDEX_DxD3dDrawPrimitives2, (PFN)DxD3dDrawPrimitives2},
    {DXG_INDEX_DxDdGetDriverState, (PFN)DxDdGetDriverState},
    {DXG_INDEX_DxDdAddAttachedSurface, (PFN)DxDdAddAttachedSurface},
    {DXG_INDEX_DxDdAlphaBlt, (PFN)DxDdAlphaBlt},
    {DXG_INDEX_DxDdAttachSurface, (PFN)DxDdAttachSurface},
    {DXG_INDEX_DxDdBeginMoCompFrame, (PFN)DxDdBeginMoCompFrame},
    {DXG_INDEX_DxDdBlt, (PFN)DxDdBlt},
    {DXG_INDEX_DxDdCanCreateSurface, (PFN)DxDdCanCreateSurface},
    {DXG_INDEX_DxDdCanCreateD3DBuffer, (PFN)DxDdCanCreateD3DBuffer},
    {DXG_INDEX_DxDdColorControl, (PFN)DxDdColorControl},
    {DXG_INDEX_DxDdCreateDirectDrawObject, (PFN)DxDdCreateDirectDrawObject},
    {DXG_INDEX_DxDdCreateSurface, (PFN)DxDdCreateSurface},
    {DXG_INDEX_DxDdCreateD3DBuffer, (PFN)DxDdCreateD3DBuffer},
    {DXG_INDEX_DxDdCreateMoComp, (PFN)DxDdCreateMoComp},
    {DXG_INDEX_DxDdCreateSurfaceObject, (PFN)DxDdCreateSurfaceObject},
    {DXG_INDEX_DxDdDeleteDirectDrawObject, (PFN)DxDdDeleteDirectDrawObject},
    {DXG_INDEX_DxDdDeleteSurfaceObject, (PFN)DxDdDeleteSurfaceObject},
    {DXG_INDEX_DxDdDestroyMoComp, (PFN)DxDdDestroyMoComp},
    {DXG_INDEX_DxDdDestroySurface, (PFN)DxDdDestroySurface},
    {DXG_INDEX_DxDdDestroyD3DBuffer, (PFN)DxDdDestroyD3DBuffer},
    {DXG_INDEX_DxDdEndMoCompFrame, (PFN)DxDdEndMoCompFrame},
    {DXG_INDEX_DxDdFlip, (PFN)DxDdFlip},
    {DXG_INDEX_DxDdFlipToGDISurface, (PFN)DxDdFlipToGDISurface},
    {DXG_INDEX_DxDdGetAvailDriverMemory, (PFN)DxDdGetAvailDriverMemory},
    {DXG_INDEX_DxDdGetBltStatus, (PFN)DxDdGetBltStatus},
    {DXG_INDEX_DxDdGetDC, (PFN)DxDdGetDC},
    {DXG_INDEX_DxDdGetDriverInfo, (PFN)DxDdGetDriverInfo},
    {DXG_INDEX_DxDdGetDxHandle, (PFN)DxDdGetDxHandle},
    {DXG_INDEX_DxDdGetFlipStatus, (PFN)DxDdGetFlipStatus},
    {DXG_INDEX_DxDdGetInternalMoCompInfo, (PFN)DxDdGetInternalMoCompInfo},
    {DXG_INDEX_DxDdGetMoCompBuffInfo, (PFN)DxDdGetMoCompBuffInfo},
    {DXG_INDEX_DxDdGetMoCompGuids, (PFN)DxDdGetMoCompGuids},
    {DXG_INDEX_DxDdGetMoCompFormats, (PFN)DxDdGetMoCompFormats},
    {DXG_INDEX_DxDdGetScanLine, (PFN)DxDdGetScanLine},
    {DXG_INDEX_DxDdLock, (PFN)DxDdLock},
    {DXG_INDEX_DxDdLockD3D, (PFN)DxDdLockD3D},
    {DXG_INDEX_DxDdQueryDirectDrawObject, (PFN)DxDdQueryDirectDrawObject},
    {DXG_INDEX_DxDdQueryMoCompStatus, (PFN)DxDdQueryMoCompStatus},
    {DXG_INDEX_DxDdReenableDirectDrawObject, (PFN)DxDdReenableDirectDrawObject},
    {DXG_INDEX_DxDdReleaseDC, (PFN)DxDdReleaseDC},
    {DXG_INDEX_DxDdRenderMoComp, (PFN)DxDdRenderMoComp},
    {DXG_INDEX_DxDdResetVisrgn, (PFN)DxDdResetVisrgn},
    {DXG_INDEX_DxDdSetColorKey, (PFN)DxDdSetColorKey},
    {DXG_INDEX_DxDdSetExclusiveMode, (PFN)DxDdSetExclusiveMode},
    {DXG_INDEX_DxDdSetGammaRamp, (PFN)DxDdSetGammaRamp},
    {DXG_INDEX_DxDdCreateSurfaceEx, (PFN)DxDdCreateSurfaceEx},
    {DXG_INDEX_DxDdSetOverlayPosition, (PFN)DxDdSetOverlayPosition},
    {DXG_INDEX_DxDdUnattachSurface, (PFN)DxDdUnattachSurface},
    {DXG_INDEX_DxDdUnlock, (PFN)DxDdUnlock},
    {DXG_INDEX_DxDdUnlockD3D, (PFN)DxDdUnlockD3D},
    {DXG_INDEX_DxDdUpdateOverlay, (PFN)DxDdUpdateOverlay},
    {DXG_INDEX_DxDdWaitForVerticalBlank, (PFN)DxDdWaitForVerticalBlank},
    {DXG_INDEX_DxDvpCanCreateVideoPort, (PFN)DxDvpCanCreateVideoPort},
    {DXG_INDEX_DxDvpColorControl, (PFN)DxDvpColorControl},
    {DXG_INDEX_DxDvpCreateVideoPort, (PFN)DxDvpCreateVideoPort},
    {DXG_INDEX_DxDvpDestroyVideoPort, (PFN)DxDvpDestroyVideoPort},
    {DXG_INDEX_DxDvpFlipVideoPort, (PFN)DxDvpFlipVideoPort},
    {DXG_INDEX_DxDvpGetVideoPortBandwidth, (PFN)DxDvpGetVideoPortBandwidth},
    {DXG_INDEX_DxDvpGetVideoPortField, (PFN)DxDvpGetVideoPortField},
    {DXG_INDEX_DxDvpGetVideoPortFlipStatus, (PFN)DxDvpGetVideoPortFlipStatus},
    {DXG_INDEX_DxDvpGetVideoPortInputFormats, (PFN)DxDvpGetVideoPortInputFormats},
    {DXG_INDEX_DxDvpGetVideoPortLine, (PFN)DxDvpGetVideoPortLine},
    {DXG_INDEX_DxDvpGetVideoPortOutputFormats, (PFN)DxDvpGetVideoPortOutputFormats},
    {DXG_INDEX_DxDvpGetVideoPortConnectInfo, (PFN)DxDvpGetVideoPortConnectInfo},
    {DXG_INDEX_DxDvpGetVideoSignalStatus, (PFN)DxDvpGetVideoSignalStatus},
    {DXG_INDEX_DxDvpUpdateVideoPort, (PFN)DxDvpUpdateVideoPort},
    {DXG_INDEX_DxDvpWaitForVideoPortSync, (PFN)DxDvpWaitForVideoPortSync},
    {DXG_INDEX_DxDvpAcquireNotification, (PFN)DxDvpAcquireNotification},
    {DXG_INDEX_DxDvpReleaseNotification, (PFN)DxDvpReleaseNotification},
    {DXG_INDEX_DxDdHeapVidMemAllocAligned, (PFN)DxDdHeapVidMemAllocAligned},
    {DXG_INDEX_DxDdHeapVidMemFree, (PFN)DxDdHeapVidMemFree},
    {DXG_INDEX_DxDdEnableDirectDraw, (PFN)DxDdEnableDirectDraw},
    {DXG_INDEX_DxDdDisableDirectDraw, (PFN)DxDdDisableDirectDraw},
    {DXG_INDEX_DxDdSuspendDirectDraw, (PFN)DxDdSuspendDirectDraw},
    {DXG_INDEX_DxDdResumeDirectDraw, (PFN)DxDdResumeDirectDraw},
    {DXG_INDEX_DxDdDynamicModeChange, (PFN)DxDdDynamicModeChange},
    {DXG_INDEX_DxDdCloseProcess, (PFN)DxDdCloseProcess},
    {DXG_INDEX_DxDdGetDirectDrawBound, (PFN)DxDdGetDirectDrawBound},
    {DXG_INDEX_DxDdEnableDirectDrawRedirection, (PFN)DxDdEnableDirectDrawRedirection},
    {DXG_INDEX_DxDdAllocPrivateUserMem, (PFN)DxDdAllocPrivateUserMem},
    {DXG_INDEX_DxDdFreePrivateUserMem, (PFN)DxDdFreePrivateUserMem},
    {DXG_INDEX_DxDdLockDirectDrawSurface, (PFN)DxDdLockDirectDrawSurface},
    {DXG_INDEX_DxDdUnlockDirectDrawSurface, (PFN)DxDdUnlockDirectDrawSurface},
    {DXG_INDEX_DxDdSetAccelLevel, (PFN)DxDdSetAccelLevel},
    {DXG_INDEX_DxDdGetSurfaceLock, (PFN)DxDdGetSurfaceLock},
    {DXG_INDEX_DxDdEnumLockedSurfaceRect, (PFN)DxDdEnumLockedSurfaceRect},
    {DXG_INDEX_DxDdIoctl, (PFN)DxDdIoctl}
};
