/*
 * ReactOS Generic Framebuffer display driver
 *
 * Copyright (C) 2004 Filip Navara
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "framebuf.h"

/*
 * DrvEnableSurface
 *
 * Create the primary and shadow surfaces and set the video mode requested
 * when PDEV was initialized.
 *
 * Status
 *    @implemented
 */

HSURF APIENTRY
DrvEnableSurface(
   IN DHPDEV dhpdev)
{
   PPDEV ppdev = (PPDEV)dhpdev;
   HSURF hSurface = NULL;
   HSURF hShadow = NULL;
   SURFOBJ *psoShadow = NULL;
   ULONG BitmapType;
   SIZEL ScreenSize;
   ULONG FrameBufferSize;
   VIDEO_MEMORY VideoMemory;
   VIDEO_MEMORY_INFORMATION VideoMemoryInfo;
   ULONG ulTemp;

   RtlZeroMemory(&VideoMemory, sizeof(VideoMemory));
   RtlZeroMemory(&VideoMemoryInfo, sizeof(VideoMemoryInfo));

   /*
    * Set video mode of our adapter.
    */

   if (EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_SET_CURRENT_MODE,
                          &(ppdev->ModeIndex), sizeof(ULONG), NULL, 0,
                          &ulTemp))
   {
      return NULL;
   }

   /*
    * Map the framebuffer into our memory.
    */

   if (EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_MAP_VIDEO_MEMORY,
                          &VideoMemory, sizeof(VIDEO_MEMORY),
                          &VideoMemoryInfo, sizeof(VIDEO_MEMORY_INFORMATION),
                          &ulTemp))
   {
      return NULL;
   }

   if ((VideoMemoryInfo.FrameBufferBase == NULL) ||
       (ppdev->ScreenHeight == 0) ||
       (ppdev->ScreenDelta > (~0UL / ppdev->ScreenHeight)))
   {
      goto Failure;
   }

   FrameBufferSize = ppdev->ScreenDelta * ppdev->ScreenHeight;
   if (VideoMemoryInfo.FrameBufferLength < FrameBufferSize)
   {
      goto Failure;
   }

   switch (ppdev->BitsPerPixel)
   {
      case 8:
         IntSetPalette(dhpdev, ppdev->PaletteEntries, 0, 256);
         BitmapType = BMF_8BPP;
         break;

      case 16:
         BitmapType = BMF_16BPP;
         break;

      case 24:
         BitmapType = BMF_24BPP;
         break;

      case 32:
         BitmapType = BMF_32BPP;
         break;

      default:
         goto Failure;
   }

   ppdev->iDitherFormat = BitmapType;

   ScreenSize.cx = ppdev->ScreenWidth;
   ScreenSize.cy = ppdev->ScreenHeight;

   /*
    * Keep the authoritative primary surface in cached system memory. This
    * avoids reads from the framebuffer for screen-to-screen operations. The
    * hooked drawing functions copy modified rectangles to the framebuffer.
    */
   hShadow = (HSURF)EngCreateBitmap(ScreenSize,
                                    0,
                                    BitmapType,
                                    BMF_TOPDOWN,
                                    NULL);
   if ((hShadow != NULL) &&
       EngAssociateSurface(hShadow, ppdev->hDevEng, 0))
   {
      psoShadow = EngLockSurface(hShadow);
   }

   if (psoShadow != NULL)
   {
      hSurface = EngCreateDeviceSurface((DHSURF)ppdev,
                                        ScreenSize,
                                        BitmapType);
   }

   if ((hSurface != NULL) &&
       EngAssociateSurface(hSurface,
                           ppdev->hDevEng,
                           HOOK_BITBLT |
                           HOOK_COPYBITS |
                           HOOK_STRETCHBLTROP |
                           HOOK_TRANSPARENTBLT |
                           HOOK_ALPHABLEND |
                           HOOK_GRADIENTFILL |
                           HOOK_LINETO))
   {
      ppdev->hSurfEng = hSurface;
      ppdev->hSurfShadow = hShadow;
      ppdev->psoShadow = psoShadow;

      /* Publish VRAM only after every surface dependency is initialized. */
      InterlockedExchangePointer((PVOID volatile *)&ppdev->ScreenPtr,
                                 VideoMemoryInfo.FrameBufferBase);

      /* EngCreateBitmap zeroes the shadow; make VRAM agree with it. */
      IntFlushScreen(ppdev, NULL);
      return hSurface;
   }

   /* If the shadow cannot be allocated, retain the old direct-VRAM path. */
   if (hSurface != NULL)
      EngDeleteSurface(hSurface);
   if (psoShadow != NULL)
      EngUnlockSurface(psoShadow);
   if (hShadow != NULL)
      EngDeleteSurface(hShadow);

   hSurface = (HSURF)EngCreateBitmap(ScreenSize,
                                     ppdev->ScreenDelta,
                                     BitmapType,
                                     (ppdev->ScreenDelta > 0) ? BMF_TOPDOWN : 0,
                                     VideoMemoryInfo.FrameBufferBase);
   if (hSurface == NULL)
      goto Failure;

   if (!EngAssociateSurface(hSurface, ppdev->hDevEng, 0))
   {
      EngDeleteSurface(hSurface);
      goto Failure;
   }

   ppdev->hSurfEng = hSurface;
   InterlockedExchangePointer((PVOID volatile *)&ppdev->ScreenPtr,
                              VideoMemoryInfo.FrameBufferBase);
   return hSurface;

Failure:
   if (VideoMemoryInfo.FrameBufferBase != NULL)
   {
      VideoMemory.RequestedVirtualAddress = VideoMemoryInfo.FrameBufferBase;
      EngDeviceIoControl(ppdev->hDriver,
                         IOCTL_VIDEO_UNMAP_VIDEO_MEMORY,
                         &VideoMemory,
                         sizeof(VIDEO_MEMORY),
                         NULL,
                         0,
                         &ulTemp);
   }
   InterlockedExchangePointer((PVOID volatile *)&ppdev->ScreenPtr, NULL);
   return NULL;
}

/*
 * DrvDisableSurface
 *
 * Used by GDI to notify a driver that the surface created by DrvEnableSurface
 * for the current device is no longer needed.
 *
 * Status
 *    @implemented
 */

VOID APIENTRY
DrvDisableSurface(
   IN DHPDEV dhpdev)
{
   DWORD ulTemp;
   VIDEO_MEMORY VideoMemory;
   PPDEV ppdev = (PPDEV)dhpdev;
   PVOID ScreenPtr;

   /* Stop new flushes before dismantling their source and destination. */
   ScreenPtr = InterlockedExchangePointer((PVOID volatile *)&ppdev->ScreenPtr,
                                          NULL);

   if (ppdev->hSurfEng != NULL)
   {
      EngDeleteSurface(ppdev->hSurfEng);
      ppdev->hSurfEng = NULL;
   }

   if (ppdev->psoShadow != NULL)
   {
      EngUnlockSurface(ppdev->psoShadow);
      ppdev->psoShadow = NULL;
   }

   if (ppdev->hSurfShadow != NULL)
   {
      EngDeleteSurface(ppdev->hSurfShadow);
      ppdev->hSurfShadow = NULL;
   }

#ifdef EXPERIMENTAL_MOUSE_CURSOR_SUPPORT
   /* Clear all mouse pointer surfaces. */
   DrvSetPointerShape(NULL, NULL, NULL, NULL, 0, 0, 0, 0, NULL, 0);
#endif

   /*
    * Unmap the framebuffer.
    */

   VideoMemory.RequestedVirtualAddress = ScreenPtr;
   EngDeviceIoControl(((PPDEV)dhpdev)->hDriver, IOCTL_VIDEO_UNMAP_VIDEO_MEMORY,
                      &VideoMemory, sizeof(VIDEO_MEMORY), NULL, 0, &ulTemp);
}

/*
 * DrvAssertMode
 *
 * Sets the mode of the specified physical device to either the mode specified
 * when the PDEV was initialized or to the default mode of the hardware.
 *
 * Status
 *    @implemented
 */

BOOL APIENTRY
DrvAssertMode(
   IN DHPDEV dhpdev,
   IN BOOL bEnable)
{
   PPDEV ppdev = (PPDEV)dhpdev;
   ULONG ulTemp;

   if (bEnable)
   {
      /*
       * Reinitialize the device to a clean state.
       */
      if (EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_SET_CURRENT_MODE,
                             &(ppdev->ModeIndex), sizeof(ULONG), NULL, 0,
                             &ulTemp))
      {
          /* We failed, bail out */
          return FALSE;
      }
      if (ppdev->BitsPerPixel == 8)
      {
	     IntSetPalette(dhpdev, ppdev->PaletteEntries, 0, 256);
      }

      /* A mode set invalidates VRAM, but the shadow remains authoritative. */
      IntFlushScreen(ppdev, NULL);

      return TRUE;
   }
   else
   {
      /*
       * Call the miniport driver to reset the device to a known state.
       */
      return !EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_RESET_DEVICE,
                                 NULL, 0, NULL, 0, &ulTemp);
   }
}
