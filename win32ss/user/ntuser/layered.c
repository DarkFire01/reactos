/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS Win32k subsystem
 * PURPOSE:          Layered window support
 * FILE:             win32ss/user/ntuser/layered.c
 * PROGRAMER:
 */

#include <win32k.h>
#include <reactos/dwmport.h>
DBG_DEFAULT_CHANNEL(UserMisc);


typedef struct _LRD_PROP
{ 
   COLORREF Key;
   BYTE     Alpha;
   BYTE     is_Layered;
   BYTE     NoUsed[2];
   DWORD    Flags;
   HBITMAP  hbmLayer;          /* UpdateLayeredWindow's image, 32bpp with premultiplied alpha */
   SIZE     LayerSize;
   DWM_LAYERED_ATTRIBUTES LayerAttributes;
} LRD_PROP, *PLRD_PROP;

static
PLRD_PROP
IntGetLayeredProp(
   _In_ PWND pWnd,
   _In_ BOOL Create)
{
   PLRD_PROP pLrdProp = UserGetProp(pWnd, AtomLayer, TRUE);

   if (!pLrdProp && Create)
   {
      pLrdProp = ExAllocatePoolZero(PagedPool, sizeof(*pLrdProp), USERTAG_REDIRECT);
      if (pLrdProp)
         UserSetProp(pWnd, AtomLayer, (HANDLE)pLrdProp, TRUE);
   }
   return pLrdProp;
}

VOID
FASTCALL
IntDestroyLayeredWindow(
   _In_ PWND pWnd)
{
   PLRD_PROP pLrdProp = UserGetProp(pWnd, AtomLayer, TRUE);

   if (!pLrdProp)
      return;

   UserRemoveProp(pWnd, AtomLayer, TRUE);
   if (pLrdProp->hbmLayer)
      GreDeleteObject(pLrdProp->hbmLayer);
   ExFreePoolWithTag(pLrdProp, USERTAG_REDIRECT);
}

/* A window UpdateLayeredWindow gave an image of its own, and how that image blends */
BOOL
FASTCALL
IntGetLayeredWindowLayer(
   _In_ PWND pWnd,
   _Out_opt_ PDWM_LAYERED_ATTRIBUTES pAttributes)
{
   PLRD_PROP pLrdProp = UserGetProp(pWnd, AtomLayer, TRUE);

   if (!pLrdProp || !pLrdProp->hbmLayer)
      return FALSE;

   if (pAttributes)
      *pAttributes = pLrdProp->LayerAttributes;
   return TRUE;
}

static
VOID
IntCopyPixels32(
   _Inout_ SURFOBJ *psoDst,
   _In_ SURFOBJ *psoSrc,
   _In_ const RECTL *prcl)
{
   LONG y;
   SIZE_T cjRow = (prcl->right - prcl->left) * sizeof(ULONG);

   for (y = prcl->top; y < prcl->bottom; y++)
   {
      RtlCopyMemory((PBYTE)psoDst->pvScan0 + y * psoDst->lDelta + prcl->left * sizeof(ULONG),
                    (PBYTE)psoSrc->pvScan0 + y * psoSrc->lDelta + prcl->left * sizeof(ULONG),
                    cjRow);
   }
}

/* Copies the layer into a 32bpp surface the size of the window, within prclDirty when given */
BOOL
FASTCALL
IntCopyLayeredWindowLayer(
   _In_ PWND pWnd,
   _Inout_ PSURFACE psurfDst,
   _In_opt_ const RECTL *prclDirty)
{
   PLRD_PROP pLrdProp = UserGetProp(pWnd, AtomLayer, TRUE);
   PSURFACE psurfLayer;
   RECTL rcl;

   if (!pLrdProp || !pLrdProp->hbmLayer || psurfDst->SurfObj.iBitmapFormat != BMF_32BPP)
      return FALSE;

   psurfLayer = SURFACE_ShareLockSurface(pLrdProp->hbmLayer);
   if (!psurfLayer)
      return FALSE;

   rcl.left = 0;
   rcl.top = 0;
   rcl.right = min(psurfLayer->SurfObj.sizlBitmap.cx, psurfDst->SurfObj.sizlBitmap.cx);
   rcl.bottom = min(psurfLayer->SurfObj.sizlBitmap.cy, psurfDst->SurfObj.sizlBitmap.cy);
   if (!prclDirty || RECTL_bIntersectRect(&rcl, &rcl, prclDirty))
      IntCopyPixels32(&psurfDst->SurfObj, &psurfLayer->SurfObj, &rcl);

   SURFACE_ShareUnlockSurface(psurfLayer);
   return TRUE;
}

/* Only a 32bpp source with AC_SRC_ALPHA brings alpha of its own; the color key makes pixels clear */
static
VOID
IntFinishLayerAlpha(
   _Inout_ SURFOBJ *pso,
   _In_ const RECTL *prcl,
   _In_ const DWM_LAYERED_ATTRIBUTES *pAttributes,
   _In_ BOOL fSourceAlpha)
{
   ULONG Key = CLR_INVALID;
   LONG x, y;

   if (fSourceAlpha &&
       (pAttributes->Flags & ULW_ALPHA) &&
       (pAttributes->Blend.AlphaFormat & AC_SRC_ALPHA) &&
       !(pAttributes->Flags & ULW_OPAQUE))
   {
      return;
   }

   if (pAttributes->Flags & ULW_COLORKEY)
   {
      Key = (GetRValue(pAttributes->Key) << 16) |
            (GetGValue(pAttributes->Key) << 8) |
            GetBValue(pAttributes->Key);
   }

   for (y = prcl->top; y < prcl->bottom; y++)
   {
      PULONG Row = (PULONG)((PBYTE)pso->pvScan0 + y * pso->lDelta);

      for (x = prcl->left; x < prcl->right; x++)
      {
         ULONG Color = Row[x] & 0x00FFFFFF;
         Row[x] = (Color == Key) ? 0 : (Color | 0xFF000000);
      }
   }
}

/* Takes the source bits into the layer, a new one when the size changed */
static
BOOL
IntCaptureLayer(
   _Inout_ PLRD_PROP pLrdProp,
   _In_ LONG cx,
   _In_ LONG cy,
   _In_ HDC hdcSrc,
   _In_opt_ const POINT *pptSrc,
   _In_opt_ const RECT *prcDirty,
   _Out_ RECTL *prclChanged)
{
   PSURFACE psurfLayer, psurfSrc;
   PDC pdcSrc;
   EXLATEOBJ exlo;
   POINTL ptlSrc;
   RECTL rcl, rclSrc;
   BOOL Ret = FALSE;
   BOOL fNew = FALSE;

   RtlZeroMemory(prclChanged, sizeof(*prclChanged));
   if (!pLrdProp->hbmLayer ||
       pLrdProp->LayerSize.cx != cx ||
       pLrdProp->LayerSize.cy != cy)
   {
      HBITMAP hbm = GreCreateBitmapEx(cx, cy, 0, BMF_32BPP, BMF_TOPDOWN, 0, NULL, 0);
      if (!hbm)
         return FALSE;

      GreSetObjectOwner(hbm, GDI_OBJ_HMGR_PUBLIC);
      if (pLrdProp->hbmLayer)
         GreDeleteObject(pLrdProp->hbmLayer);
      pLrdProp->hbmLayer = hbm;
      pLrdProp->LayerSize.cx = cx;
      pLrdProp->LayerSize.cy = cy;
      fNew = TRUE;
   }

   rcl.left = 0;
   rcl.top = 0;
   rcl.right = cx;
   rcl.bottom = cy;
   if (prcDirty && !fNew && !RECTL_bIntersectRect(&rcl, &rcl, (const RECTL *)prcDirty))
      return TRUE;

   pdcSrc = DC_LockDc(hdcSrc);
   if (!pdcSrc)
      return FALSE;

   psurfSrc = pdcSrc->dclevel.pSurface;
   psurfLayer = SURFACE_ShareLockSurface(pLrdProp->hbmLayer);
   if (psurfSrc && psurfLayer && pdcSrc->dctype != DCTYPE_DIRECT)
   {
      ptlSrc.x = pptSrc ? pptSrc->x : 0;
      ptlSrc.y = pptSrc ? pptSrc->y : 0;
      if (fNew)
         RtlZeroMemory(psurfLayer->SurfObj.pvBits, psurfLayer->SurfObj.cjBits);

      /* What the source has of the rectangle; the rest stays clear */
      rclSrc.left = -ptlSrc.x;
      rclSrc.top = -ptlSrc.y;
      rclSrc.right = psurfSrc->SurfObj.sizlBitmap.cx - ptlSrc.x;
      rclSrc.bottom = psurfSrc->SurfObj.sizlBitmap.cy - ptlSrc.y;
      if (RECTL_bIntersectRect(&rclSrc, &rclSrc, &rcl))
      {
         ptlSrc.x += rclSrc.left;
         ptlSrc.y += rclSrc.top;
         EXLATEOBJ_vInitialize(&exlo, psurfSrc->ppal, psurfLayer->ppal, 0, 0, 0);
         EngCopyBits(&psurfLayer->SurfObj, &psurfSrc->SurfObj, NULL, &exlo.xlo, &rclSrc, &ptlSrc);
         EXLATEOBJ_vCleanup(&exlo);
         IntFinishLayerAlpha(&psurfLayer->SurfObj, &rclSrc, &pLrdProp->LayerAttributes,
                             psurfSrc->SurfObj.iBitmapFormat == BMF_32BPP);
      }

      *prclChanged = rcl;
      Ret = TRUE;
   }
   else if (pdcSrc->dctype == DCTYPE_DIRECT)
   {
      ERR("UpdateLayeredWindow from a screen DC is not supported\n");
   }

   if (psurfLayer)
      SURFACE_ShareUnlockSurface(psurfLayer);
   DC_UnlockDc(pdcSrc);
   return Ret;
}

BOOL FASTCALL
GetLayeredStatus(PWND pWnd)
{
   PLRD_PROP pLrdProp = UserGetProp(pWnd, AtomLayer, TRUE);
   if (pLrdProp)
   {
      return pLrdProp->is_Layered;
   }
   return FALSE;
}

BOOL FASTCALL
SetLayeredStatus(PWND pWnd, BYTE set)
{
   PLRD_PROP pLrdProp = UserGetProp(pWnd, AtomLayer, TRUE);
   if (pLrdProp)
   {
      pLrdProp->is_Layered = set;
      return TRUE;
   }
   return FALSE;
}

BOOL FASTCALL
IntSetLayeredWindowAttributes(PWND pWnd,
                              COLORREF crKey,
                              BYTE bAlpha,
                              DWORD dwFlags)
{
   PLRD_PROP pLrdProp;
   INT was_Layered;

   if (!(pWnd->ExStyle & WS_EX_LAYERED) )
   {
      ERR("Not a Layered Window!\n");
      return FALSE;
   }

   pLrdProp = IntGetLayeredProp(pWnd, TRUE);
   if (!pLrdProp)
   {
      ERR("failed to allocate LRD_PROP\n");
      return FALSE;
   }

   if (pLrdProp)
   {
      was_Layered = pLrdProp->is_Layered;

      pLrdProp->Key = crKey;

      if (dwFlags & LWA_ALPHA)
      {
         pLrdProp->Alpha = bAlpha;
      }
      else
      {
         if (!pLrdProp->is_Layered) pLrdProp->Alpha = 0;
      }

      pLrdProp->Flags = dwFlags;

      pLrdProp->is_Layered = 1;
  
      if (!was_Layered)
         co_UserRedrawWindow(pWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME );
   }
   // FIXME: Now set some bits to the Window DC!!!!
   return TRUE;
}

BOOL FASTCALL
IntUpdateLayeredWindowI( PWND pWnd,
                         UPDATELAYEREDWINDOWINFO *info )
{
   static const BLENDFUNCTION DefaultBlend = { AC_SRC_OVER, 0, 255, 0 };
   PWND Parent;
   PLRD_PROP pLrdProp;
   RECT Window, Client;
   RECTL rclChanged;
   SIZE Offset;
   BOOL ret = FALSE;
   BOOL fComposed;
   DWORD flags = SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW;

   Window = pWnd->rcWindow;
   Client = pWnd->rcClient;

   Parent = pWnd->spwndParent;
   if (pWnd->style & WS_CHILD && Parent)
   {
      RECTL_vOffsetRect(&Window, - Parent->rcClient.left, - Parent->rcClient.top);
      RECTL_vOffsetRect(&Client, - Parent->rcClient.left, - Parent->rcClient.top);
   }

   if (info->pptDst)
   {
      Offset.cx = info->pptDst->x - Window.left;
      Offset.cy = info->pptDst->y - Window.top;
      RECTL_vOffsetRect( &Client, Offset.cx, Offset.cy );
      RECTL_vOffsetRect( &Window, Offset.cx, Offset.cy );
      flags &= ~SWP_NOMOVE;
   }
   if (info->psize)
   {
      Offset.cx = info->psize->cx - (Window.right  - Window.left);
      Offset.cy = info->psize->cy - (Window.bottom - Window.top);
      if (info->psize->cx <= 0 || info->psize->cy <= 0)
      {
          ERR("Update Layered Invalid Parameters\n");
          EngSetLastError( ERROR_INVALID_PARAMETER );
          return FALSE;
      }
      if ((info->dwFlags & ULW_EX_NORESIZE) && (Offset.cx || Offset.cy))
      {
          ERR("Wrong Size\n");
          EngSetLastError( ERROR_INCORRECT_SIZE );
          return FALSE;
      }
      Client.right  += Offset.cx;
      Client.bottom += Offset.cy;
      Window.right  += Offset.cx;
      Window.bottom += Offset.cy;
      flags &= ~SWP_NOSIZE;
   }

   pLrdProp = IntGetLayeredProp(pWnd, TRUE);
   if (!pLrdProp)
   {
      EngSetLastError(ERROR_NOT_ENOUGH_MEMORY);
      return FALSE;
   }

   pLrdProp->LayerAttributes.Key = info->crKey;
   pLrdProp->LayerAttributes.Flags = info->dwFlags & (ULW_COLORKEY | ULW_ALPHA | ULW_OPAQUE);
   if ((info->dwFlags & ULW_ALPHA) && info->pblend)
      pLrdProp->LayerAttributes.Blend = *info->pblend;
   else
      pLrdProp->LayerAttributes.Blend = DefaultBlend;

   /* The window keeps the image, so it can be shown again without asking the application */
   if (info->hdcSrc &&
       !IntCaptureLayer(pLrdProp,
                        Window.right - Window.left,
                        Window.bottom - Window.top,
                        info->hdcSrc,
                        info->pptSrc,
                        info->prcDirty,
                        &rclChanged))
   {
      EngSetLastError(ERROR_INVALID_PARAMETER);
      return FALSE;
   }

   /* The DWM composes the layer itself; only an uncomposed window draws it here */
   fComposed = IntDwmIsRedirected(pWnd);
   if (info->hdcSrc && !fComposed)
   {
      HDC hdc, hdcBuffer;
      RECT Rect;
      BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, 0 };
      COLORREF color_key = (info->dwFlags & ULW_COLORKEY) ? info->crKey : CLR_INVALID;
      HBITMAP hOldBitmap, hOldBitmap1, hbmSrc, hbmDst;
      DIBSECTION dibs;

      Rect = Window;

      RECTL_vOffsetRect( &Rect, -Window.left, -Window.top );

      TRACE("H %d W %d\n",Rect.bottom - Rect.top,Rect.right - Rect.left);

      if (!info->hdcDst) hdc = UserGetDCEx(pWnd, NULL, DCX_USESTYLE);
      else hdc = info->hdcDst;

      hbmSrc = NtGdiCreateCompatibleBitmap(info->hdcSrc, Rect.right - Rect.left, Rect.bottom - Rect.top);
      hbmDst = NtGdiCreateCompatibleBitmap(info->hdcSrc, Rect.right - Rect.left, Rect.bottom - Rect.top);

      GreGetObject(hbmSrc, sizeof(DIBSECTION), &dibs);

      TRACE("Source Bitmap bc %d\n",dibs.dsBmih.biBitCount);

      hdcBuffer = NtGdiCreateCompatibleDC(hdc);

      hOldBitmap = (HBITMAP)NtGdiSelectBitmap(hdcBuffer, hbmSrc);
      hOldBitmap1 = (HBITMAP)NtGdiSelectBitmap(hdc, hbmDst);

      NtGdiStretchBlt( hdcBuffer,
                       Rect.left,
                       Rect.top,
                       Rect.right - Rect.left,
                       Rect.bottom - Rect.top,
                       info->hdcSrc,
                       Rect.left + (info->pptSrc ? info->pptSrc->x : 0),
                       Rect.top  + (info->pptSrc ? info->pptSrc->y : 0),
                       Rect.right - Rect.left,
                       Rect.bottom - Rect.top,
                       SRCCOPY,
                       color_key );

      // Need to test this, Dirty before or after StretchBlt?
      if (info->prcDirty)
      {
         ERR("prcDirty\n");
         RECTL_bIntersectRect( &Rect, &Rect, info->prcDirty );
         NtGdiPatBlt( hdc, Rect.left, Rect.top, Rect.right - Rect.left, Rect.bottom - Rect.top, BLACKNESS );
      }

      if (info->dwFlags & ULW_ALPHA)
      {
         blend = *info->pblend;
         TRACE("ULW_ALPHA bop %d Alpha %d aF %d\n", blend.BlendOp, blend.SourceConstantAlpha, blend.AlphaFormat);
      }

      ret = NtGdiAlphaBlend( hdc,
                             Rect.left,
                             Rect.top,
                             Rect.right - Rect.left,
                             Rect.bottom - Rect.top,
                             hdcBuffer,
                             Rect.left + (info->pptSrc ? info->pptSrc->x : 0),
                             Rect.top  + (info->pptSrc ? info->pptSrc->y : 0),
                             Rect.right - Rect.left,
                             Rect.bottom - Rect.top,
                             blend,
                             0);

      NtGdiSelectBitmap(hdc, hOldBitmap1);
      NtGdiSelectBitmap(hdcBuffer, hOldBitmap);
      if (hbmSrc) GreDeleteObject(hbmSrc);
      if (hbmDst) GreDeleteObject(hbmDst);
      if (hdcBuffer) IntGdiDeleteDC(hdcBuffer, FALSE);
      if (!info->hdcDst) UserReleaseDC(pWnd, hdc, FALSE);
   }
   else
      ret = TRUE;

   co_WinPosSetWindowPos(pWnd, 0, Window.left, Window.top, Window.right - Window.left, Window.bottom - Window.top, flags);

   /* After the move, so a new size already has its surface */
   if (fComposed)
      IntDwmLayeredWindowUpdated(pWnd, info->hdcSrc ? &rclChanged : NULL);
   return ret;
}


/*
 * @implemented
 */
BOOL
APIENTRY
NtUserGetLayeredWindowAttributes(
    HWND hwnd,
    COLORREF *pcrKey,
    BYTE *pbAlpha,
    DWORD *pdwFlags)
{
   PLRD_PROP pLrdProp;
   PWND pWnd;
   BOOL Ret = FALSE;

   TRACE("Enter NtUserGetLayeredWindowAttributes\n");
   UserEnterShared();

   if (!(pWnd = UserGetWindowObject(hwnd)) ||
       !(pWnd->ExStyle & WS_EX_LAYERED) )
   {
      ERR("Not a Layered Window!\n");
      goto Exit;
   }

   pLrdProp = UserGetProp(pWnd, AtomLayer, TRUE);

   if (!pLrdProp)
   {
      TRACE("No Prop!\n");
      goto Exit;
   }

   if (pLrdProp->is_Layered == 0)
   {
      goto Exit;
   }

   _SEH2_TRY
   {
      if (pcrKey)
      {
          ProbeForWrite(pcrKey, sizeof(*pcrKey), 1);
          *pcrKey = pLrdProp->Key;
      }
      if (pbAlpha)
      {
          ProbeForWrite(pbAlpha, sizeof(*pbAlpha), 1);
          *pbAlpha = pLrdProp->Alpha;
      }
      if (pdwFlags)
      {
          ProbeForWrite(pdwFlags, sizeof(*pdwFlags), 1);
          *pdwFlags = pLrdProp->Flags;
      }
   }
   _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
   {
      SetLastNtError(_SEH2_GetExceptionCode());
      _SEH2_YIELD(goto Exit);
   }
   _SEH2_END;

   Ret = TRUE;

Exit:
   TRACE("Leave NtUserGetLayeredWindowAttributes, ret=%i\n", Ret);
   UserLeave();
   return Ret;
}

/*
 * @implemented
 */
BOOL APIENTRY
NtUserSetLayeredWindowAttributes(HWND hwnd,
			   COLORREF crKey,
			   BYTE bAlpha,
			   DWORD dwFlags)
{
   PWND pWnd;
   BOOL Ret = FALSE;

   TRACE("Enter NtUserSetLayeredWindowAttributes\n");
   UserEnterExclusive();

   if (!(pWnd = UserGetWindowObject(hwnd)) ||
       !(pWnd->ExStyle & WS_EX_LAYERED) )
   {
      ERR("Not a Layered Window!\n");
      goto Exit;
   }

   Ret = IntSetLayeredWindowAttributes(pWnd, crKey, bAlpha, dwFlags);
Exit:
   TRACE("Leave NtUserSetLayeredWindowAttributes, ret=%i\n", Ret);
   UserLeave();
   return Ret;
}

/*
 * @implemented
 */
BOOL
APIENTRY
NtUserUpdateLayeredWindow(
   HWND hwnd,
   HDC hdcDst,
   POINT *pptDst,
   SIZE *psize,
   HDC hdcSrc,
   POINT *pptSrc,
   COLORREF crKey,
   BLENDFUNCTION *pblend,
   DWORD dwFlags,
   RECT *prcDirty)
{
   UPDATELAYEREDWINDOWINFO info;
   POINT Dst, Src; 
   SIZE Size;
   RECT Dirty;
   BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, 0 };
   PWND pWnd;
   BOOL Ret = FALSE;

   TRACE("Enter NtUserUpdateLayeredWindow\n");
   UserEnterExclusive();

   if (!(pWnd = UserGetWindowObject(hwnd)))
   {
      goto Exit;
   }

   _SEH2_TRY
   {
      if (pptDst)
      {
         ProbeForRead(pptDst, sizeof(POINT), 1);
         Dst = *pptDst;
      }
      if (pptSrc)
      {
         ProbeForRead(pptSrc, sizeof(POINT), 1);
         Src = *pptSrc;
      }
      if (psize)
      {
         ProbeForRead(psize, sizeof(SIZE), 1);
         Size = *psize;
      }
      if (pblend)
      {
         ProbeForRead(pblend, sizeof(BLENDFUNCTION), 1);
         blend = *pblend;
      }
      if (prcDirty)
      {
         ProbeForRead(prcDirty, sizeof(RECT), 1);
         Dirty = *prcDirty;
      }
   }
   _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
   {
      EngSetLastError( ERROR_INVALID_PARAMETER );
      _SEH2_YIELD(goto Exit);
   }
   _SEH2_END;

   if ( GetLayeredStatus(pWnd) ||
        dwFlags & ~(ULW_COLORKEY | ULW_ALPHA | ULW_OPAQUE | ULW_EX_NORESIZE) ||
       !(pWnd->ExStyle & WS_EX_LAYERED) )
   {
      ERR("Layered Window Invalid Parameters\n");
      EngSetLastError( ERROR_INVALID_PARAMETER );
      goto Exit;
   }

   info.cbSize   = sizeof(info);
   info.hdcDst   = hdcDst;
   info.pptDst   = pptDst? &Dst : NULL;
   info.psize    = psize ? &Size : NULL;
   info.hdcSrc   = hdcSrc;
   info.pptSrc   = pptSrc ? &Src : NULL;
   info.crKey    = crKey;
   info.pblend   = &blend;
   info.dwFlags  = dwFlags;
   info.prcDirty = prcDirty ? &Dirty : NULL;
   Ret = IntUpdateLayeredWindowI( pWnd, &info );
Exit:
   TRACE("Leave NtUserUpdateLayeredWindow, ret=%i\n", Ret);
   UserLeave();
   return Ret;
}

/* EOF */
