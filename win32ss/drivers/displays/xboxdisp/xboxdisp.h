/*
 * PROJECT:     Xbox NV2A accelerated GDI display driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Header — PDEV and shared decls
 *
 * The driver pairs with xboxvmp.sys.  It clones the generic framebuf driver
 * but adds:
 *   - DrvBitBlt / DrvCopyBits hooks that intercept solid-colour rectangle
 *     fills and screen-to-screen blits and route them through the miniport's
 *     IOCTL_VIDEO_NV2A_FILL_RECT / IOCTL_VIDEO_NV2A_SCREEN_BLT.
 *   - DrvEscape OPENGL_GETINFO that publishes the xboxogl ICD.
 */

#ifndef _XBOXDISP_PCH_
#define _XBOXDISP_PCH_

#include <stdarg.h>
#include <windef.h>
#include <wingdi.h>
#include <winddi.h>
#include <winioctl.h>
#include <ntddvdeo.h>

#include "../../miniport/xboxvmp/nv2a_accel.h"

#define DEVICE_NAME L"xboxdisp"
#define ALLOC_TAG   'DxbX'

typedef struct _PDEV
{
    HANDLE hDriver;
    HDEV   hDevEng;
    HSURF  hSurfEng;
    ULONG  ModeIndex;
    ULONG  ScreenWidth;
    ULONG  ScreenHeight;
    ULONG  ScreenDelta;
    BYTE   BitsPerPixel;
    ULONG  RedMask;
    ULONG  GreenMask;
    ULONG  BlueMask;
    BYTE   PaletteShift;
    PVOID  ScreenPtr;
    HPALETTE     DefaultPalette;
    PALETTEENTRY *PaletteEntries;

    /* TRUE once we have successfully queried the miniport for accel caps. */
    BOOL   AccelAvailable;
    BOOL   AccelHardware;     /* TRUE iff miniport reports HardwareAccelEnabled */
    DWORD  iDitherFormat;

    /* Offscreen device-bitmap support.  VRAM-resident GDI bitmaps live in a slice
     * of video memory above the visible framebuffer; copies between them and the
     * screen go through the NV2A 2D engine (IOCTL_VIDEO_NV2A_BLT_EX), copies to a
     * system DIB go through the CPU.  The heap is a small first-fit free-list. */
    PVOID  VramBase;          /* CPU base of the mapped video memory (== ScreenPtr) */
    ULONG  VramLen;           /* mapped video-memory length in bytes */
    ULONG  FbGpuOffset;       /* GPU offset of VramBase */
#define XBOXDISP_HEAP_MAX_SPANS 64
    struct { ULONG Off; ULONG Size; } HeapFree[XBOXDISP_HEAP_MAX_SPANS]; /* free spans, GPU-absolute Off */
    ULONG  HeapSpanCount;
} PDEV, *PPDEV;

/* Per-device-bitmap driver handle (DHSURF) for EngCreateDeviceSurface bitmaps. */
typedef struct _XBOXDISP_DEVBMP
{
    struct _PDEV *ppdev; /* owning device (needed to free the heap span on delete) */
    HSURF  hsurf;
    PVOID  CpuPtr;       /* CPU address of the bitmap's pixels in mapped VRAM */
    ULONG  GpuOffset;    /* absolute GPU offset of the pixels */
    ULONG  Pitch;        /* stride in bytes */
    ULONG  HeapOff;      /* GPU-absolute offset returned by the allocator (for free) */
    ULONG  HeapSize;     /* allocation size (for free) */
    LONG   Width;
    LONG   Height;
} XBOXDISP_DEVBMP, *PXBOXDISP_DEVBMP;

/* enable.c */
BOOL APIENTRY DrvEnableDriver(ULONG, ULONG, PDRVENABLEDATA);
DHPDEV APIENTRY DrvEnablePDEV(DEVMODEW*, LPWSTR, ULONG, HSURF*, ULONG, ULONG*,
                              ULONG, DEVINFO*, HDEV, LPWSTR, HANDLE);
VOID  APIENTRY DrvCompletePDEV(DHPDEV, HDEV);
VOID  APIENTRY DrvDisablePDEV(DHPDEV);

/* surface.c */
HSURF APIENTRY DrvEnableSurface(DHPDEV);
VOID  APIENTRY DrvDisableSurface(DHPDEV);
BOOL  APIENTRY DrvAssertMode(DHPDEV, BOOL);

/* screen.c */
ULONG APIENTRY DrvGetModes(HANDLE, ULONG, DEVMODEW*);
BOOL  IntInitScreenInfo(PPDEV, LPDEVMODEW, PGDIINFO, PDEVINFO);

/* palette.c */
BOOL  APIENTRY DrvSetPalette(DHPDEV, PALOBJ*, FLONG, ULONG, ULONG);
BOOL  IntInitDefaultPalette(PPDEV, PDEVINFO);
BOOL  APIENTRY IntSetPalette(DHPDEV, PPALETTEENTRY, ULONG, ULONG);

/* pointer.c */
ULONG APIENTRY DrvSetPointerShape(SURFOBJ*, SURFOBJ*, SURFOBJ*, XLATEOBJ*,
                                  LONG, LONG, LONG, LONG, RECTL*, FLONG);
VOID  APIENTRY DrvMovePointer(SURFOBJ*, LONG, LONG, RECTL*);

/* accel.c */
BOOL  APIENTRY DrvBitBlt(SURFOBJ*, SURFOBJ*, SURFOBJ*, CLIPOBJ*, XLATEOBJ*,
                         RECTL*, POINTL*, POINTL*, BRUSHOBJ*, POINTL*, ROP4);
BOOL  APIENTRY DrvCopyBits(SURFOBJ*, SURFOBJ*, CLIPOBJ*, XLATEOBJ*,
                           RECTL*, POINTL*);
ULONG APIENTRY DrvEscape(SURFOBJ*, ULONG, ULONG, PVOID, ULONG, PVOID);
HBITMAP APIENTRY DrvCreateDeviceBitmap(DHPDEV, SIZEL, ULONG);
VOID  APIENTRY DrvDeleteDeviceBitmap(DHSURF);

/* dd.c (DirectDraw stubs) */
BOOL  APIENTRY DrvEnableDirectDraw(DHPDEV, DD_CALLBACKS*, DD_SURFACECALLBACKS*,
                                   DD_PALETTECALLBACKS*);
VOID  APIENTRY DrvDisableDirectDraw(DHPDEV);

#endif /* _XBOXDISP_PCH_ */
