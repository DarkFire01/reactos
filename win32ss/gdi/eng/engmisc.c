/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS Win32k subsystem
 * PURPOSE:           ENG misc Functions
 * FILE:              win32ss/gdi/eng/engmisc.c
 * PROGRAMER:         ReactOS Team
 */

#include <win32k.h>

#define NDEBUG
#include <debug.h>

typedef struct _INTENG_TEMP_BITMAP_CACHE
{
  volatile LONG Busy;
  HBITMAP hbm;
  SIZEL sizl;
  LONG width;
  ULONG format;
} INTENG_TEMP_BITMAP_CACHE;

static INTENG_TEMP_BITMAP_CACHE s_IntEngTempBitmapCache = {0};

BOOL APIENTRY
IntEngEnterEx(PINTENG_ENTER_LEAVE EnterLeave,
              SURFOBJ *psoDest,
              RECTL *DestRect,
              BOOL ReadOnly,
              BOOL CopyDest,
              POINTL *Translate,
              SURFOBJ **ppsoOutput)
{
  LONG Exchange;
  SIZEL BitmapSize;
  POINTL SrcPoint;
  LONG Width;
  RECTL ClippedDestRect;
  BOOL UseCachedBitmap = FALSE;
  BOOL CacheLockHeld = FALSE;

  /* Normalize */
  if (DestRect->right < DestRect->left)
    {
    Exchange = DestRect->left;
    DestRect->left = DestRect->right;
    DestRect->right = Exchange;
    }
  if (DestRect->bottom < DestRect->top)
    {
    Exchange = DestRect->top;
    DestRect->top = DestRect->bottom;
    DestRect->bottom = Exchange;
    }

  if (NULL != psoDest && STYPE_BITMAP != psoDest->iType &&
      (NULL == psoDest->pvScan0 || 0 == psoDest->lDelta))
    {
    /* Driver needs to support DrvCopyBits, else we can't do anything */
    SURFACE *psurfDest = CONTAINING_RECORD(psoDest, SURFACE, SurfObj);
    if (!(psurfDest->flags & HOOK_COPYBITS))
    {
      return FALSE;
    }

    /* Allocate a temporary bitmap */
    BitmapSize.cx = DestRect->right - DestRect->left;
    BitmapSize.cy = DestRect->bottom - DestRect->top;
    Width = WIDTH_BYTES_ALIGN32(BitmapSize.cx, BitsPerFormat(psoDest->iBitmapFormat));
    if (BitmapSize.cx > 0 && BitmapSize.cy > 0 &&
        InterlockedCompareExchange(&s_IntEngTempBitmapCache.Busy, 1, 0) == 0)
    {
      CacheLockHeld = TRUE;

      if (s_IntEngTempBitmapCache.hbm != NULL &&
          s_IntEngTempBitmapCache.sizl.cx == BitmapSize.cx &&
          s_IntEngTempBitmapCache.sizl.cy == BitmapSize.cy &&
          s_IntEngTempBitmapCache.width == Width &&
          s_IntEngTempBitmapCache.format == psoDest->iBitmapFormat)
      {
        EnterLeave->OutputBitmap = s_IntEngTempBitmapCache.hbm;
        UseCachedBitmap = TRUE;
      }
      else
      {
        if (s_IntEngTempBitmapCache.hbm != NULL)
        {
          EngDeleteSurface((HSURF)s_IntEngTempBitmapCache.hbm);
          s_IntEngTempBitmapCache.hbm = NULL;
        }

        s_IntEngTempBitmapCache.hbm = EngCreateBitmap(BitmapSize, Width,
                                                      psoDest->iBitmapFormat,
                                                      BMF_TOPDOWN | BMF_NOZEROINIT, NULL);
        if (s_IntEngTempBitmapCache.hbm != NULL)
        {
          s_IntEngTempBitmapCache.sizl = BitmapSize;
          s_IntEngTempBitmapCache.width = Width;
          s_IntEngTempBitmapCache.format = psoDest->iBitmapFormat;

          EnterLeave->OutputBitmap = s_IntEngTempBitmapCache.hbm;
          UseCachedBitmap = TRUE;
        }
      }
    }

    if (!UseCachedBitmap)
    {
      EnterLeave->OutputBitmap = EngCreateBitmap(BitmapSize, Width,
                                                 psoDest->iBitmapFormat,
                                                 BMF_TOPDOWN | BMF_NOZEROINIT, NULL);

      if (!EnterLeave->OutputBitmap)
        {
        if (CacheLockHeld)
        {
          InterlockedExchange(&s_IntEngTempBitmapCache.Busy, 0);
        }
        DPRINT1("EngCreateBitmap() failed\n");
        return FALSE;
        }
    }

    *ppsoOutput = EngLockSurface((HSURF)EnterLeave->OutputBitmap);
    if (*ppsoOutput == NULL)
    {
      if (!UseCachedBitmap)
      {
        EngDeleteSurface((HSURF)EnterLeave->OutputBitmap);
      }
      if (CacheLockHeld)
      {
        InterlockedExchange(&s_IntEngTempBitmapCache.Busy, 0);
      }
      return FALSE;
    }

    EnterLeave->DestRect.left = 0;
    EnterLeave->DestRect.top = 0;
    EnterLeave->DestRect.right = BitmapSize.cx;
    EnterLeave->DestRect.bottom = BitmapSize.cy;
    SrcPoint.x = DestRect->left;
    SrcPoint.y = DestRect->top;
    ClippedDestRect = EnterLeave->DestRect;
    if (SrcPoint.x < 0)
      {
        ClippedDestRect.left -= SrcPoint.x;
        SrcPoint.x = 0;
      }
    if (psoDest->sizlBitmap.cx < SrcPoint.x + ClippedDestRect.right - ClippedDestRect.left)
      {
        ClippedDestRect.right = ClippedDestRect.left + psoDest->sizlBitmap.cx - SrcPoint.x;
      }
    if (SrcPoint.y < 0)
      {
        ClippedDestRect.top -= SrcPoint.y;
        SrcPoint.y = 0;
      }
    if (psoDest->sizlBitmap.cy < SrcPoint.y + ClippedDestRect.bottom - ClippedDestRect.top)
      {
        ClippedDestRect.bottom = ClippedDestRect.top + psoDest->sizlBitmap.cy - SrcPoint.y;
      }
    EnterLeave->TrivialClipObj = EngCreateClip();
    if (EnterLeave->TrivialClipObj == NULL)
    {
      EngUnlockSurface(*ppsoOutput);
      if (!UseCachedBitmap)
      {
        EngDeleteSurface((HSURF)EnterLeave->OutputBitmap);
      }
      if (CacheLockHeld)
      {
        InterlockedExchange(&s_IntEngTempBitmapCache.Busy, 0);
      }
      return FALSE;
    }
    EnterLeave->TrivialClipObj->iDComplexity = DC_TRIVIAL;
    if (CopyDest)
    {
      if (ClippedDestRect.left < (*ppsoOutput)->sizlBitmap.cx &&
        0 <= ClippedDestRect.right &&
        SrcPoint.x < psoDest->sizlBitmap.cx &&
        ClippedDestRect.top <= (*ppsoOutput)->sizlBitmap.cy &&
        0 <= ClippedDestRect.bottom &&
        SrcPoint.y < psoDest->sizlBitmap.cy &&
        ! GDIDEVFUNCS(psoDest).CopyBits(
                        *ppsoOutput, psoDest,
                        EnterLeave->TrivialClipObj, NULL,
                        &ClippedDestRect, &SrcPoint))
      {
        EngDeleteClip(EnterLeave->TrivialClipObj);
        EngUnlockSurface(*ppsoOutput);
        EngDeleteSurface((HSURF)EnterLeave->OutputBitmap);
        return FALSE;
      }
    }
    EnterLeave->DestRect.left = DestRect->left;
    EnterLeave->DestRect.top = DestRect->top;
    EnterLeave->DestRect.right = DestRect->right;
    EnterLeave->DestRect.bottom = DestRect->bottom;
    Translate->x = - DestRect->left;
    Translate->y = - DestRect->top;
    }
  else
    {
    Translate->x = 0;
    Translate->y = 0;
    *ppsoOutput = psoDest;
    }

  if (NULL != *ppsoOutput)
  {
    SURFACE* psurfOutput = CONTAINING_RECORD(*ppsoOutput, SURFACE, SurfObj);
    if (0 != (psurfOutput->flags & HOOK_SYNCHRONIZE))
    {
      if (NULL != GDIDEVFUNCS(*ppsoOutput).SynchronizeSurface)
        {
          GDIDEVFUNCS(*ppsoOutput).SynchronizeSurface(*ppsoOutput, DestRect, 0);
        }
      else if (STYPE_BITMAP == (*ppsoOutput)->iType
               && NULL != GDIDEVFUNCS(*ppsoOutput).Synchronize)
        {
          GDIDEVFUNCS(*ppsoOutput).Synchronize((*ppsoOutput)->dhpdev, DestRect);
        }
    }
  }
  else return FALSE;

  EnterLeave->DestObj = psoDest;
  EnterLeave->OutputObj = *ppsoOutput;
  EnterLeave->ReadOnly = ReadOnly;
  EnterLeave->UseCachedBitmap = UseCachedBitmap;

  return TRUE;
}

BOOL APIENTRY
IntEngEnter(PINTENG_ENTER_LEAVE EnterLeave,
            SURFOBJ *psoDest,
            RECTL *DestRect,
            BOOL ReadOnly,
            POINTL *Translate,
            SURFOBJ **ppsoOutput)
{
  return IntEngEnterEx(EnterLeave, psoDest, DestRect, ReadOnly, TRUE, Translate, ppsoOutput);
}

BOOL APIENTRY
IntEngLeave(PINTENG_ENTER_LEAVE EnterLeave)
{
  POINTL SrcPoint;
  BOOL Result = TRUE;

  if (EnterLeave->OutputObj != EnterLeave->DestObj && NULL != EnterLeave->OutputObj)
    {
    if (! EnterLeave->ReadOnly)
      {
      SrcPoint.x = 0;
      SrcPoint.y = 0;
      if (EnterLeave->DestRect.left < 0)
        {
          SrcPoint.x = - EnterLeave->DestRect.left;
          EnterLeave->DestRect.left = 0;
        }
      if (EnterLeave->DestObj->sizlBitmap.cx < EnterLeave->DestRect.right)
        {
          EnterLeave->DestRect.right = EnterLeave->DestObj->sizlBitmap.cx;
        }
      if (EnterLeave->DestRect.top < 0)
        {
          SrcPoint.y = - EnterLeave->DestRect.top;
          EnterLeave->DestRect.top = 0;
        }
      if (EnterLeave->DestObj->sizlBitmap.cy < EnterLeave->DestRect.bottom)
        {
          EnterLeave->DestRect.bottom = EnterLeave->DestObj->sizlBitmap.cy;
        }
      if (SrcPoint.x < EnterLeave->OutputObj->sizlBitmap.cx &&
          EnterLeave->DestRect.left <= EnterLeave->DestRect.right &&
          EnterLeave->DestRect.left < EnterLeave->DestObj->sizlBitmap.cx &&
          SrcPoint.y < EnterLeave->OutputObj->sizlBitmap.cy &&
          EnterLeave->DestRect.top <= EnterLeave->DestRect.bottom &&
          EnterLeave->DestRect.top < EnterLeave->DestObj->sizlBitmap.cy)
        {
          Result = GDIDEVFUNCS(EnterLeave->DestObj).CopyBits(
                                                 EnterLeave->DestObj,
                                                 EnterLeave->OutputObj,
                                                 EnterLeave->TrivialClipObj, NULL,
                                                 &EnterLeave->DestRect, &SrcPoint);
        }
      else
        {
          Result = TRUE;
        }
      }
    EngUnlockSurface(EnterLeave->OutputObj);
    if (!EnterLeave->UseCachedBitmap)
    {
      EngDeleteSurface((HSURF)EnterLeave->OutputBitmap);
    }
    EngDeleteClip(EnterLeave->TrivialClipObj);

    if (EnterLeave->UseCachedBitmap)
    {
      InterlockedExchange(&s_IntEngTempBitmapCache.Busy, 0);
    }
    }
  else
    {
    Result = TRUE;
    }

  return Result;
}

HANDLE APIENTRY
EngGetProcessHandle(VOID)
{
  /* http://www.osr.com/ddk/graphics/gdifncs_3tif.htm
     In Windows 2000 and later, the EngGetProcessHandle function always returns NULL.
     FIXME: What does NT4 return? */
  return NULL;
}

VOID
APIENTRY
EngGetCurrentCodePage(
    _Out_ PUSHORT OemCodePage,
    _Out_ PUSHORT AnsiCodePage)
{
    /* Forward to kernel */
    RtlGetDefaultCodePage(AnsiCodePage, OemCodePage);
}

BOOL
APIENTRY
EngQuerySystemAttribute(
   _In_ ENG_SYSTEM_ATTRIBUTE CapNum,
   _Out_ PDWORD pCapability)
{
    SYSTEM_BASIC_INFORMATION sbi;
    SYSTEM_PROCESSOR_INFORMATION spi;
    NTSTATUS status;

    switch (CapNum)
    {
        case EngNumberOfProcessors:
            status = NtQuerySystemInformation(SystemBasicInformation,
                                              &sbi,
                                              sizeof(SYSTEM_BASIC_INFORMATION),
                                              NULL);
            if (!NT_SUCCESS(status))
            {
                DPRINT1("Failed to query basic information: 0x%lx\n", status);
                return FALSE;
            }

            *pCapability = sbi.NumberOfProcessors;
            return TRUE;

        case EngProcessorFeature:
            status = NtQuerySystemInformation(SystemProcessorInformation,
                                              &spi,
                                              sizeof(SYSTEM_PROCESSOR_INFORMATION),
                                              NULL);
            if (!NT_SUCCESS(status))
            {
                DPRINT1("Failed to query processor information: 0x%lx\n", status);
                return FALSE;
            }
            *pCapability = spi.ProcessorFeatureBits;
            return TRUE;

        default:
            break;
    }

    return FALSE;
}

ULONGLONG
APIENTRY
EngGetTickCount(VOID)
{
    ULONG Multiplier;
    LARGE_INTEGER TickCount;

    /* Get the multiplier and current tick count */
    KeQueryTickCount(&TickCount);
    Multiplier = SharedUserData->TickCountMultiplier;

    /* Convert to milliseconds and return */
    return (Int64ShrlMod32(UInt32x32To64(Multiplier, TickCount.LowPart), 24) +
            (Multiplier * (TickCount.HighPart << 8)));
}

/* EOF */
