/*
 * PROJECT:     FreeLoader
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 *              or MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Video support for linear framebuffers
 * COPYRIGHT:   Copyright 2025-2026 Hermès Bélusca-Maïto <hermes.belusca-maito@reactos.org>
 */

#include <freeldr.h>
#include "vidfb.h"
#include "vgafont.h"

#include <debug.h>
DBG_DEFAULT_CHANNEL(UI);
#undef TRACE
#define TRACE ERR

/* This is used to introduce artificial symmetric borders at the top and bottom */
#define TOP_BOTTOM_LINES 0 // (2 * CHAR_HEIGHT) // 0

typedef ULONG RGBQUAD; // , COLORREF;

#define RGB(r,g,b)      ((RGBQUAD)(((UCHAR)(b) | ((USHORT)((UCHAR)(g))<<8)) | (((ULONG)(UCHAR)(r))<<16)))
#define RGBA(r,g,b,a)   ((RGBQUAD)((((ULONG)(UCHAR)(a))<<24) | RGB(r,g,b)))

#define GetAValue(rgb)  ((UCHAR)((rgb)>>24))
#define GetRValue(quad) ((UCHAR)(((quad)>>16) & 0xFF))
#define GetGValue(quad) ((UCHAR)(((quad)>>8) & 0xFF))
#define GetBValue(quad) ((UCHAR)((quad) & 0xFF))

#define CONST_STR_LEN(x)  (sizeof(x)/sizeof(x[0]) - 1)


/* GLOBALS ********************************************************************/

typedef struct _FRAMEBUFFER_INFO
{
    ULONG_PTR BaseAddress;
    ULONG BufferSize;

    /* Horizontal and Vertical resolution in pixels */
    ULONG ScreenWidth;
    ULONG ScreenHeight;

    /* Number of pixel elements per video memory line */
    ULONG PixelsPerScanLine; // aka. "Pitch" or "ScreenStride", but Stride is in bytes or bits...
    ULONG BitsPerPixel;      // aka. "PixelStride".

    /* Physical format of the pixel for BPP > 8, specified by bit-mask */
    PIXEL_BITMASK PixelMasks;

/** Calculated values */

    ULONG BytesPerPixel;
    ULONG Delta;             // aka. "Pitch": actual size in bytes of a scanline.

    /* Calculated number of bits from the masks above */
    UCHAR RedMaskSize;
    UCHAR GreenMaskSize;
    UCHAR BlueMaskSize;
    UCHAR ReservedMaskSize;

    /* Calculated bit position (~ shift count) of each mask LSB */
    UCHAR RedMaskPosition;
    UCHAR GreenMaskPosition;
    UCHAR BlueMaskPosition;
    UCHAR ReservedMaskPosition;
} FRAMEBUFFER_INFO, *PFRAMEBUFFER_INFO;

static FRAMEBUFFER_INFO framebufInfo = {0};
static CM_FRAMEBUF_DEVICE_DATA FrameBufferData = {0};

static UINT8 VidpXScale = 1;
static UINT8 VidpYScale = 1;


/**
 * @brief   Standard 16-color CGA/EGA text palette.
 *
 * Corresponds to the following formulae (see also
 * https://moddingwiki.shikadi.net/wiki/EGA_Palette):
 * // - - R0 G0 B0 R1 G1 B1
 * UINT8 red   = 0x55 * (((ega >> 1) & 2) | (ega >> 5) & 1);
 * UINT8 green = 0x55 * (( ega       & 2) | (ega >> 4) & 1);
 * UINT8 blue  = 0x55 * (((ega << 1) & 2) | (ega >> 3) & 1);
 *
 * And from a 16-bit CGA index:
 * UINT8 red   = 0x55 * (((cga & 4) >> 1) | ((cga & 8) >> 3));
 * UINT8 green = 0x55 * ( (cga & 2) | ((cga & 8) >> 3));
 * UINT8 blue  = 0x55 * (((cga & 1) << 1) | ((cga & 8) >> 3));
 * if (cga == 6) green /= 2;
 **/
// NOTE: Console support.
static const RGBQUAD CgaEgaPalette[16] =
{
    RGB(0x00, 0x00, 0x00), RGB(0x00, 0x00, 0xAA), RGB(0x00, 0xAA, 0x00), RGB(0x00, 0xAA, 0xAA),
    RGB(0xAA, 0x00, 0x00), RGB(0xAA, 0x00, 0xAA), RGB(0xAA, 0x55, 0x00), RGB(0xAA, 0xAA, 0xAA),
    RGB(0x55, 0x55, 0x55), RGB(0x55, 0x55, 0xFF), RGB(0x55, 0xFF, 0x55), RGB(0x55, 0xFF, 0xFF),
    RGB(0xFF, 0x55, 0x55), RGB(0xFF, 0x55, 0xFF), RGB(0xFF, 0xFF, 0x55), RGB(0xFF, 0xFF, 0xFF)
};

/**
 * @brief
 * Default 16-color palette for foreground and background colors.
 * Taken from win32ss/user/winsrv/consrv/frontends/gui/conwnd.c
 * and win32ss/user/winsrv/concfg/settings.c
 **/
// NOTE: Console support.
static const RGBQUAD ConsPalette[16] =
{
    RGB(  0,   0,   0), /* Black */
    RGB(  0,   0, 128), /* Blue */
    RGB(  0, 128,   0), /* Green */
    RGB(  0, 128, 128), /* Cyan */
    RGB(128,   0,   0), /* Red */
    RGB(128,   0, 128), /* Magenta */
    RGB(128, 128,   0), /* Brown */
    RGB(192, 192, 192), /* Light Gray */
    RGB(128, 128, 128), /* Dark Gray */
    RGB(  0,   0, 255), /* Light Blue */
    RGB(  0, 255,   0), /* Light Green */
    RGB(  0, 255, 255), /* Light Cyan */
    RGB(255,   0,   0), /* Light Red */
    RGB(255,   0, 255), /* Light Magenta */
    RGB(255, 255,   0), /* Yellow */
    RGB(255, 255, 255), /* White */
};

// NOTE: Console support.
static const RGBQUAD* Palette = CgaEgaPalette;
static UINT8 CgaToEga[16]; // Palette to map 16-color CGA indexes to 6-bit EGA.

/**
 * @brief   Pixel handlers.
 **/
PUCHAR (*pWritePixel)(_In_ PUCHAR Addr, _In_ UINT32 Pixel);
PUCHAR (*pWritePixels)(_In_ PUCHAR Addr, _In_ UINT32 Pixel, _In_ ULONG PixelCount);


/* FUNCTIONS ******************************************************************/

static PUCHAR
WritePixel8BPP(
    _In_ PUCHAR Addr,
    _In_ UINT32 Pixel)
{
    *(PUINT8)Addr = (UINT8)Pixel;
    return ++Addr;
}

static PUCHAR
WritePixels8BPP(
    _In_ PUCHAR Addr,
    _In_ UINT32 Pixel,
    _In_ ULONG PixelCount)
{
    while (PixelCount--)
    {
        *(PUINT8)Addr = (UINT8)Pixel;
        ++Addr;
    }
    return Addr;
}

static PUCHAR
WritePixel555_565(
    _In_ PUCHAR Addr,
    _In_ UINT32 Pixel)
{
    *(PUINT16)Addr = (UINT16)Pixel;
    return (Addr + 2);
}

static PUCHAR
WritePixels555_565(
    _In_ PUCHAR Addr,
    _In_ UINT32 Pixel,
    _In_ ULONG PixelCount)
{
    while (PixelCount--)
    {
        *(PUINT16)Addr = (UINT16)Pixel;
        Addr += 2;
    }
    return Addr;
}

static PUCHAR
WritePixel888(
    _In_ PUCHAR Addr,
    _In_ UINT32 Pixel)
{
    Addr[0] = (Pixel & 0xFF);
    Addr[1] = ((Pixel >> 8) & 0xFF);
    Addr[2] = ((Pixel >> 16) & 0xFF);
    return (Addr + 3);
}

static PUCHAR
WritePixels888(
    _In_ PUCHAR Addr,
    _In_ UINT32 Pixel,
    _In_ ULONG PixelCount)
{
    while (PixelCount--)
    {
        Addr[0] = (Pixel & 0xFF);
        Addr[1] = ((Pixel >> 8) & 0xFF);
        Addr[2] = ((Pixel >> 16) & 0xFF);
        Addr += 3;
    }
    return Addr;
}

static PUCHAR
WritePixel8888(
    _In_ PUCHAR Addr,
    _In_ UINT32 Pixel)
{
    *(PUINT32)Addr = Pixel;
    return (Addr + 4);
}

static PUCHAR
WritePixels8888(
    _In_ PUCHAR Addr,
    _In_ UINT32 Pixel,
    _In_ ULONG PixelCount)
{
    while (PixelCount--)
    {
        *(PUINT32)Addr = Pixel;
        Addr += 4;
    }
    return Addr;
}


#if DBG
static VOID
VidFbPrintFramebufferInfo(VOID)
{
    TRACE("Framebuffer format:\n");
    TRACE("    BaseAddress       : 0x%X\n", framebufInfo.BaseAddress);
    TRACE("    BufferSize        : %lu\n", framebufInfo.BufferSize);
    TRACE("    ScreenWidth       : %lu\n", framebufInfo.ScreenWidth);
    TRACE("    ScreenHeight      : %lu\n", framebufInfo.ScreenHeight);
    TRACE("    PixelsPerScanLine : %lu\n", framebufInfo.PixelsPerScanLine);
    TRACE("    BitsPerPixel      : %lu\n", framebufInfo.BitsPerPixel);
    TRACE("    BytesPerPixel     : %lu\n", framebufInfo.BytesPerPixel);
    TRACE("    Delta             : %lu\n", framebufInfo.Delta);
    TRACE("    ARGB masks:       : %08x/%08x/%08x/%08x\n",
          framebufInfo.PixelMasks.ReservedMask,
          framebufInfo.PixelMasks.RedMask,
          framebufInfo.PixelMasks.GreenMask,
          framebufInfo.PixelMasks.BlueMask);
    TRACE("    ARGB number bits  : %u/%u/%u/%u\n",
          framebufInfo.ReservedMaskSize,
          framebufInfo.RedMaskSize,
          framebufInfo.GreenMaskSize,
          framebufInfo.BlueMaskSize);
    TRACE("    ARGB masks LSB pos: %u/%u/%u/%u\n",
          framebufInfo.ReservedMaskPosition,
          framebufInfo.RedMaskPosition,
          framebufInfo.GreenMaskPosition,
          framebufInfo.BlueMaskPosition);
}
#endif

/**
 * @brief
 * Initializes internal framebuffer information based on the given parameters.
 *
 * @param[in]   BaseAddress
 * The framebuffer physical base address.
 *
 * @param[in]   BufferSize
 * The framebuffer size, in bytes.
 *
 * @param[in]   ScreenWidth
 * @param[in]   ScreenHeight
 * The width and height of the visible framebuffer area, in pixels.
 *
 * @param[in]   PixelsPerScanLine
 * The size in number of pixels of a whole horizontal video memory scanline.
 *
 * @param[in]   BitsPerPixel
 * The number of usable bits (not counting the reserved ones) per pixel.
 *
 * @param[in]   PixelFormat
 * Optional pointer to a PIXEL_FORMAT structure describing the pixel
 * format used by the framebuffer.
 *
 * @param[in]   FormatByMask
 * TRUE if the pixel format is directly given by bitmasks;
 * FALSE if the format is given by mask sizes and positions.
 *
 * @return
 * TRUE if initialization is successful; FALSE if not.
 **/
BOOLEAN
VidFbInitializeVideo(
    _Out_opt_ PCM_FRAMEBUF_DEVICE_DATA* pFbData,
    _In_ ULONG_PTR BaseAddress,
    _In_ ULONG BufferSize,
    _In_ UINT32 ScreenWidth,
    _In_ UINT32 ScreenHeight,
    _In_ UINT32 PixelsPerScanLine,
    _In_ UINT32 BitsPerPixel,
    _In_opt_ PPIXEL_FORMAT PixelFormat,
    _In_ BOOLEAN FormatByMask)
{
    PPIXEL_BITMASK BitMasks = &framebufInfo.PixelMasks;
    UINT8 iPal = 0;

    if (pFbData)
        *pFbData = NULL;

    RtlZeroMemory(&framebufInfo, sizeof(framebufInfo));

    /* Verify framebuffer dimensions */
    if ((ScreenWidth < 1) || (ScreenHeight < 1))
    {
        ERR("Invalid framebuffer dimensions\n");
        return FALSE;
    }

    framebufInfo.BaseAddress  = BaseAddress;
    framebufInfo.BufferSize   = BufferSize;
    framebufInfo.ScreenWidth  = ScreenWidth;
    framebufInfo.ScreenHeight = ScreenHeight;
    framebufInfo.PixelsPerScanLine = PixelsPerScanLine;
    framebufInfo.BitsPerPixel = BitsPerPixel;

    framebufInfo.BytesPerPixel = (BitsPerPixel + 7) / 8; // Round up to nearest byte.
    framebufInfo.Delta = (PixelsPerScanLine * framebufInfo.BytesPerPixel + 3) & ~3;

    /* Verify that the framebuffer fits inside the video RAM */
    if (!(ScreenHeight * framebufInfo.Delta <= BufferSize))
    {
        ERR("Framebuffer doesn't fit inside the video RAM (FB size: %lu, VRAM size: %lu)\n",
            ScreenHeight * framebufInfo.Delta, BufferSize);
        return FALSE;
    }

    //ASSERT((BitsPerPixel <= 8 && !PixelFormat) || (BitsPerPixel > 8));
    if (!PixelFormat || FormatByMask)
    {
        if (BitsPerPixel > 8)
        {
            if (!PixelFormat ||
                (PixelFormat->PixelMasks.RedMask   == 0 &&
                 PixelFormat->PixelMasks.GreenMask == 0 &&
                 PixelFormat->PixelMasks.BlueMask  == 0 /* &&
                 PixelFormat->PixelMasks.ReservedMask == 0 */))
            {
                /* Determine pixel mask given color depth and color channel */
                switch (BitsPerPixel)
                {
                    case 32:
                    case 24: /* 8:8:8 */
                        BitMasks->RedMask   = 0x00FF0000; // 0x00FF0000;
                        BitMasks->GreenMask = 0x0000FF00; // 0x00FF0000 >> 8;
                        BitMasks->BlueMask  = 0x000000FF; // 0x00FF0000 >> 16;
                        BitMasks->ReservedMask = ((1 << (BitsPerPixel - 24)) - 1) << 24;
                        break;
                    case 16: /* 5:6:5 */
                        BitMasks->RedMask   = 0xF800; // 0xF800;
                        BitMasks->GreenMask = 0x07E0; // (0xF800 >> 5) | 0x20;
                        BitMasks->BlueMask  = 0x001F; // 0xF800 >> 11;
                        BitMasks->ReservedMask = 0;
                        break;
                    case 15: /* 5:5:5 */
                        BitMasks->RedMask   = 0x7C00; // 0x7C00;
                        BitMasks->GreenMask = 0x03E0; // 0x7C00 >> 5;
                        BitMasks->BlueMask  = 0x001F; // 0x7C00 >> 10;
                        BitMasks->ReservedMask = 0x8000;
                        break;
                    default:
                        /* Unsupported BPP */
                        UNIMPLEMENTED;
                        RtlZeroMemory(BitMasks, sizeof(*BitMasks));
                }
            }
            else
            {
                /* Copy the pixel masks */
                RtlCopyMemory(BitMasks, &PixelFormat->PixelMasks, sizeof(*BitMasks));
            }
        }
        else
        {
            /* Palettized modes don't use masks */
            RtlZeroMemory(BitMasks, sizeof(*BitMasks));
        }

        framebufInfo.RedMaskSize   = CountNumberOfBits(BitMasks->RedMask); // FindHighestSetBit
        framebufInfo.GreenMaskSize = CountNumberOfBits(BitMasks->GreenMask);
        framebufInfo.BlueMaskSize  = CountNumberOfBits(BitMasks->BlueMask);
        framebufInfo.ReservedMaskSize = CountNumberOfBits(BitMasks->ReservedMask);

        /* REMARK: If any of the FindLowestSetBit() returns 0, i.e. no bit set
         * in mask (typically the ReservedMask), then subtracting 1 will set
         * the mask position "out of the way". */
        framebufInfo.RedMaskPosition   = FindLowestSetBit(BitMasks->RedMask) - 1;
        framebufInfo.GreenMaskPosition = FindLowestSetBit(BitMasks->GreenMask) - 1;
        framebufInfo.BlueMaskPosition  = FindLowestSetBit(BitMasks->BlueMask) - 1;
        framebufInfo.ReservedMaskPosition = FindLowestSetBit(BitMasks->ReservedMask) - 1;
    }
    else
    {
        PPIXEL_BITMASK_SIZEPOS MasksBySizePos = &PixelFormat->MasksBySizePos;

        framebufInfo.RedMaskSize   = MasksBySizePos->RedMaskSize;
        framebufInfo.GreenMaskSize = MasksBySizePos->GreenMaskSize;
        framebufInfo.BlueMaskSize  = MasksBySizePos->BlueMaskSize;
        framebufInfo.ReservedMaskSize = MasksBySizePos->ReservedMaskSize;

        framebufInfo.RedMaskPosition   = MasksBySizePos->RedMaskPosition;
        framebufInfo.GreenMaskPosition = MasksBySizePos->GreenMaskPosition;
        framebufInfo.BlueMaskPosition  = MasksBySizePos->BlueMaskPosition;
        framebufInfo.ReservedMaskPosition = MasksBySizePos->ReservedMaskPosition;

        /* Calculate the corresponding masks */
        BitMasks->RedMask   = ((1 << framebufInfo.RedMaskSize)   - 1) << framebufInfo.RedMaskPosition;
        BitMasks->GreenMask = ((1 << framebufInfo.GreenMaskSize) - 1) << framebufInfo.GreenMaskPosition;
        BitMasks->BlueMask  = ((1 << framebufInfo.BlueMaskSize)  - 1) << framebufInfo.BlueMaskPosition;
        BitMasks->ReservedMask = ((1 << framebufInfo.ReservedMaskSize) - 1) << framebufInfo.ReservedMaskPosition;
    }

#if DBG
    VidFbPrintFramebufferInfo();
    {
    ULONG BppFromMasks =
        PixelBitmasksToBpp(BitMasks->RedMask,
                           BitMasks->GreenMask,
                           BitMasks->BlueMask,
                           BitMasks->ReservedMask);
    TRACE("BitsPerPixel = %lu , BppFromMasks = %lu\n", BitsPerPixel, BppFromMasks);
    //ASSERT(BitsPerPixel == BppFromMasks);
    }
#endif

    // TEMPTEMP: Investigate two possible color palettes.
    if (BootMgrInfo.VideoOptions && *BootMgrInfo.VideoOptions)
    {
        PCSTR p;

        /* Find the "pal:" palette option */
        for (p = BootMgrInfo.VideoOptions; p;)
        {
            if (_strnicmp(p, "pal:", CONST_STR_LEN("pal:")) == 0)
            {
                p += CONST_STR_LEN("pal:");
                break;
            }
            p = strchr(p, ',');
            if (p) ++p;
        }
        if (p)
            iPal = (UINT8)strtoul(p, NULL, 0);

        if (iPal == 0)
            Palette = CgaEgaPalette;
        else if (iPal == 1)
            Palette = ConsPalette;

        /* Find the "scale:" X/Y scaling option */
        for (p = BootMgrInfo.VideoOptions; p;)
        {
            if (_strnicmp(p, "scale:", CONST_STR_LEN("scale:")) == 0)
            {
                p += CONST_STR_LEN("scale:");
                break;
            }
            p = strchr(p, ',');
            if (p) ++p;
        }
        if (p)
        {
            /*
             * The parameter value has the following format:
             *   X[:Y]
             * The first value is the X scaling, the second value (if any) is
             * the Y scaling. If Y is absent, use the same X (i.e. proportional)
             * scaling.
             */
ERR("Scaling option: '%s'\n", p);
            VidpXScale = (UINT8)strtoul(p, (PSTR*)&p, 0);
            if (!VidpXScale) // Fixup if invalid.
                VidpXScale = 1;
ERR("VidpXScale: %u\n", VidpXScale);
            VidpYScale = VidpXScale;
            if (p && *p == ':')
            {
                VidpYScale = (UINT8)strtoul(++p, NULL, 0);
                if (!VidpYScale) // Fixup if invalid.
                    VidpYScale = 1;
            }
ERR("VidpYScale: %u\n", VidpYScale);
        }
    }

    if ((BitMasks->RedMask | BitMasks->GreenMask |
         BitMasks->BlueMask | BitMasks->ReservedMask) == 0)
    {
        UINT8 Cga;

        if (framebufInfo.BitsPerPixel > 8)
        {
            ERR("BitsPerPixel = %lu but no pixel masks\n", BitsPerPixel);
            return FALSE;
        }

        /* Palette mode: prepare the CGA/EGA palette */
        for (Cga = 0; Cga < RTL_NUMBER_OF(CgaToEga); ++Cga)
        {
            if (BaseAddress < 0xC0000)
            {
                /* VGA graphics mode, it already uses the correct palette
                 * (the palette's first 16 colors correspond to CGA) */
                CgaToEga[Cga] = Cga;
            }
            else
            {
                /* SVGA/VBE/... uses an EGA-like palette, so create the CGA->EGA mapping */
                // https://godbolt.org/z/x8j8xTP3f
                if (Cga != 6)
                    CgaToEga[Cga] = ((iPal == 0) ? 7 : (7*!(Cga & 7) | (Cga & 7))) * (Cga & 8) | (Cga & 7);
                else
                    CgaToEga[Cga] = 0x14; // Switch to using Brown instead of "Dark Yellow"
            }
        }
    }

    /* Set read/write pixels function pointers */
    if (framebufInfo.BitsPerPixel <= 8)
    {
        pWritePixel  = WritePixel8BPP;
        pWritePixels = WritePixels8BPP;
    }
    else if (framebufInfo.BytesPerPixel == 15/8 || framebufInfo.BytesPerPixel == 16/8)
    {
        pWritePixel  = WritePixel555_565;
        pWritePixels = WritePixels555_565;
    }
    else if (framebufInfo.BytesPerPixel == 24/8)
    {
        pWritePixel  = WritePixel888;
        pWritePixels = WritePixels888;
    }
    else if (framebufInfo.BytesPerPixel == 32/8)
    {
        pWritePixel  = WritePixel8888;
        pWritePixels = WritePixels8888;
    }

    /* Initialize the hardware device configuration data if specified */
    if (pFbData)
    {
        FrameBufferData.FrameBufferOffset = 0;
        FrameBufferData.ScreenWidth  = framebufInfo.ScreenWidth;
        FrameBufferData.ScreenHeight = framebufInfo.ScreenHeight;
        FrameBufferData.PixelsPerScanLine = framebufInfo.PixelsPerScanLine;
        FrameBufferData.BitsPerPixel = framebufInfo.BitsPerPixel;

        RtlCopyMemory(&FrameBufferData.PixelMasks,
                      &framebufInfo.PixelMasks, sizeof(framebufInfo.PixelMasks));

        *pFbData = &FrameBufferData;
    }

    return TRUE;
}

VOID
VidFbGetFbDeviceData(
    _Out_ PULONG_PTR BaseAddress,
    _Out_ PULONG BufferSize,
    _Out_ PCM_FRAMEBUF_DEVICE_DATA FbData)
{
    *BaseAddress = framebufInfo.BaseAddress;
    *BufferSize = framebufInfo.BufferSize;

    FbData->ScreenWidth  = framebufInfo.ScreenWidth;
    FbData->ScreenHeight = framebufInfo.ScreenHeight;
    FbData->PixelsPerScanLine = framebufInfo.PixelsPerScanLine;
    FbData->BitsPerPixel = framebufInfo.BitsPerPixel;

    RtlCopyMemory(&FbData->PixelMasks, &framebufInfo.PixelMasks, sizeof(framebufInfo.PixelMasks));

////////////
    // For fun: resize the framebuffer so that it can be contained
    // within the FreeLoader display menu.
    {
extern UIVTBL UiVtbl;
// extern const UIVTBL TuiVtbl;
extern VOID TuiDrawBackdrop(ULONG);
extern ULONG UiScreenWidth;
extern ULONG UiScreenHeight;
#ifndef TUI_TITLE_BOX_CHAR_HEIGHT // See tui.h
#define TUI_TITLE_BOX_CHAR_HEIGHT    5
#endif
    // if (UiVtbl == TuiVtbl)
    if (UiVtbl.DrawBackdrop == TuiDrawBackdrop)
    {
        /*
         * Put the "guest" framebuffer below the UI header
         * (0,0):(UiScreenWidth-1,TUI_TITLE_BOX_CHAR_HEIGHT-1)
         */
        FbData->ScreenWidth  = UiScreenWidth * CHAR_WIDTH * VidpXScale;
        FbData->ScreenHeight = (UiScreenHeight - TUI_TITLE_BOX_CHAR_HEIGHT) * CHAR_HEIGHT * VidpYScale;

        *BaseAddress = (ULONG_PTR)framebufInfo.BaseAddress
                        + (TUI_TITLE_BOX_CHAR_HEIGHT * CHAR_HEIGHT * VidpYScale) * framebufInfo.Delta
                        + (0 * CHAR_WIDTH * VidpXScale) * framebufInfo.BytesPerPixel;
        *BufferSize = FbData->ScreenHeight * framebufInfo.Delta;
    }
    }
////////////
}

/**
 * @brief   Scale a R/G/B (0,255) color component to fit into a given bit depth.
 * @note    Adapted _and FIXED_ from https://wiki.osdev.org/VGA_Fonts#Displaying_a_character
 **/
static inline
UINT8
color_scale_component(
    _In_ UINT8 Component,
    _In_ UCHAR bpp)
{
    if (bpp == 8) return Component;
    // Buggy version: return (Component * 255 + ((1 << bpp) - 1) / 2) / ((1 << bpp) - 1);
    // First version: return (Component * ((1 << bpp) - 1)) / 255;
    return (UINT8)(((UINT16)Component << bpp) >> 8);
}

/**
 * @brief   Convert an ARGB color to a pixel format.
 * @note    Adapted from color_scale_rgb() from https://wiki.osdev.org/VGA_Fonts#Displaying_a_character
 **/
static UINT32 // RescaleColor()
color_scale_argb(
    _In_ UINT32 Color,
    _In_ PFRAMEBUFFER_INFO FbInfo)
{
    //UINT32 scRsvd  = color_scale_component(GetAValue(Color), FbInfo->ReservedMaskSize);
    UINT32 scRed   = color_scale_component(GetRValue(Color), FbInfo->RedMaskSize);
    UINT32 scGreen = color_scale_component(GetGValue(Color), FbInfo->GreenMaskSize);
    UINT32 scBlue  = color_scale_component(GetBValue(Color), FbInfo->BlueMaskSize);
#if 0 // These formulae work, but let's test the most general ones instead...
    /* Formula when R/G/B components are packed together */
    //scRsvd  = (scRsvd << FbInfo->ReservedMaskPosition);
    scRed   = (scRed   << FbInfo->RedMaskPosition);
    scGreen = (scGreen << FbInfo->GreenMaskPosition);
    scBlue  = (scBlue  << FbInfo->BlueMaskPosition);
#else
    /* Formula when R/G/B masks can be anything (incl. interleaved) */
    //scRsvd  = ExpandBits(scRsvd, FbInfo->PixelMasks.ReservedMask);
    scRed   = ExpandBits(scRed  , FbInfo->PixelMasks.RedMask);
    scGreen = ExpandBits(scGreen, FbInfo->PixelMasks.GreenMask);
    scBlue  = ExpandBits(scBlue , FbInfo->PixelMasks.BlueMask);
#endif
    return (/*scRsvd |*/ scRed | scGreen | scBlue);
}

VOID
VidFbClearScreenColor(
    _In_ UINT32 Color,
    _In_ BOOLEAN FullScreen)
{
    PUCHAR p = (PUCHAR)framebufInfo.BaseAddress + (FullScreen ? 0 : TOP_BOTTOM_LINES) * framebufInfo.Delta;
    ULONG Line;

    /* Convert ARGB color to pixel format */
    if (framebufInfo.BitsPerPixel > 8)
        Color = color_scale_argb(Color, &framebufInfo);

    for (Line = 0; Line < framebufInfo.ScreenHeight - (FullScreen ? 0 : 2 * TOP_BOTTOM_LINES); ++Line)
    {
        (void)pWritePixels(p, Color, framebufInfo.ScreenWidth);
        p += framebufInfo.Delta;
    }
}

/**
 * @brief
 * Displays a character at a given pixel position with specific foreground
 * and background colors.
 **/
VOID
VidFbOutputChar(
    _In_ UCHAR Char,
    _In_ ULONG X,
    _In_ ULONG Y,
    _In_ UINT32 FgColor,
    _In_ UINT32 BgColor)
{
    const UCHAR* FontPtr;
    PUCHAR Pixel;
    ULONG Line, Col;

    /* Don't display outside of the screen, nor partial characters */
    if ((X + CHAR_WIDTH - 1 >= framebufInfo.ScreenWidth / VidpXScale) ||
        (Y + CHAR_HEIGHT - 1 >= (framebufInfo.ScreenHeight - 2 * TOP_BOTTOM_LINES) / VidpYScale))
    {
        return;
    }

    FontPtr = BitmapFont8x16 + Char * CHAR_HEIGHT;
    Pixel = (PUCHAR)framebufInfo.BaseAddress
            + (TOP_BOTTOM_LINES + Y * VidpYScale) * framebufInfo.Delta
            + X * VidpXScale * framebufInfo.BytesPerPixel;

    /* Convert ARGB color to pixel format */
    if (framebufInfo.BitsPerPixel > 8)
    {
        FgColor = color_scale_argb(FgColor, &framebufInfo);
        BgColor = color_scale_argb(BgColor, &framebufInfo);
    }

    for (Line = 0; Line < CHAR_HEIGHT; ++Line)
    {
        PUCHAR p = Pixel;
        UCHAR Mask = 0x80; // 1 << (CHAR_WIDTH - 1);
        for (Col = 0; Col < CHAR_WIDTH; ++Col)
        {
            // p = pWritePixel(p, ((FontPtr[Line] & Mask) ? FgColor : BgColor)); // Works only if VidpXScale == 1
            p = pWritePixels(p, ((FontPtr[Line] & Mask) ? FgColor : BgColor), VidpXScale);
            Mask >>= 1;
        }
        if (VidpYScale > 1)
        {
            /* Copy (VidpYScale - 1) times the same line as that built above */
            PUCHAR Dst = Pixel + framebufInfo.Delta;
            ULONG sub = VidpYScale;
            while (sub-- > 1)
            {
                RtlCopyMemory(Dst, Pixel, p - Pixel);
                Dst += framebufInfo.Delta;
            }
            Pixel = Dst;
        }
        else
        {
            Pixel += framebufInfo.Delta;
        }
    }
}

/**
 * @brief
 * Returns the width and height in pixels, of the whole visible area
 * of the graphics framebuffer.
 **/
VOID
VidFbGetDisplaySize(
    _Out_ PULONG Width,
    _Out_ PULONG Height,
    _Out_ PULONG Depth)
{
    *Width  = framebufInfo.ScreenWidth / VidpXScale;
    *Height = (framebufInfo.ScreenHeight - 2 * TOP_BOTTOM_LINES) / VidpYScale;
    *Depth  = framebufInfo.BitsPerPixel;
}

/**
 * @brief
 * Returns the size in bytes, of a full graphics pixel buffer rectangle
 * that can fill the whole visible area of the graphics framebuffer.
 **/
ULONG
VidFbGetBufferSize(VOID)
{
    return ((framebufInfo.ScreenHeight - 2 * TOP_BOTTOM_LINES) / VidpYScale *
            (framebufInfo.ScreenWidth / VidpXScale) * framebufInfo.BytesPerPixel);
}

VOID
VidFbScrollUp(
    _In_ UINT32 Color,
    _In_ ULONG Scroll)
{
    PUCHAR Dst, Src;
    SIZE_T Size;
    ULONG Line;

    /* Rescale scrolling */
    Scroll *= VidpYScale;

    /* Compute what to move */
    Dst = (PUCHAR)framebufInfo.BaseAddress + TOP_BOTTOM_LINES * framebufInfo.Delta;
    Src = Dst + Scroll * framebufInfo.Delta;
    Size = (framebufInfo.ScreenHeight - 2 * TOP_BOTTOM_LINES) * framebufInfo.Delta - Scroll * framebufInfo.Delta;

    /* Move up the visible contents (skipping the first character line) */
    // TODO: When scrolling a screen region that doesn't start at X = 0
    // and that isn't as wide as the visible screen contents, don't do a
    // whole move, but do it line by line.
    RtlMoveMemory(Dst, Src, Size);

    /* Convert ARGB color to pixel format */
    if (framebufInfo.BitsPerPixel > 8)
        Color = color_scale_argb(Color, &framebufInfo);

    /* Clear the bottom line, starting at (Dst + Size) */
    Dst += Size;
    for (Line = 0; Line < Scroll; ++Line)
    {
        (void)pWritePixels(Dst, Color, framebufInfo.ScreenWidth);
        Dst += framebufInfo.Delta;
    }
}

// NOTE: Console support.
#if 0
VOID
VidFbSetTextCursorPosition(UCHAR X, UCHAR Y)
{
    /* We don't have a cursor yet */
}

VOID
VidFbHideShowTextCursor(BOOLEAN Show)
{
    /* We don't have a cursor yet */
}
#endif

#if 0
BOOLEAN
VidFbIsPaletteFixed(VOID)
{
    return FALSE;
}

VOID
VidFbSetPaletteColor(
    _In_ UCHAR Color,
    _In_ UCHAR Red, _In_ UCHAR Green, _In_ UCHAR Blue)
{
    /* Not supported */
}

VOID
VidFbGetPaletteColor(
    _In_ UCHAR Color,
    _Out_ PUCHAR Red, _Out_ PUCHAR Green, _Out_ PUCHAR Blue)
{
    /* Not supported */
}
#endif



/*
 * PROJECT:     FreeLoader
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 *              or MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Linear framebuffer based console support
 * COPYRIGHT:   Copyright 2025-2026 Hermès Bélusca-Maïto <hermes.belusca-maito@reactos.org>
 */

#define VGA_CHAR_SIZE 2

#define FBCONS_WIDTH    (framebufInfo.ScreenWidth / VidpXScale / CHAR_WIDTH)
#define FBCONS_HEIGHT   ((framebufInfo.ScreenHeight - 2 * TOP_BOTTOM_LINES) / VidpYScale / CHAR_HEIGHT)


/**
 * @brief
 * Maps a text-mode CGA-style character 16-color index to a pixel
 * (if BitsPerPixel <= 8) or an ARGB color.
 **/
static inline
UINT32
FbConsAttrToSingleColor(
    _In_ UCHAR Attr)
{
    /* Index format, or others */
    if (framebufInfo.BitsPerPixel <= 1)
        return !!(Attr & 0x08); // (Attr & 0x0F) >> 3;
    if (framebufInfo.BitsPerPixel == 2)
        return (Attr & 0x0F) >> 2;
    // if (framebufInfo.BitsPerPixel == 3)
    //     return (Attr & 0x0F) >> 1; // (Attr & 0x0F) / 3;
    if (framebufInfo.BitsPerPixel <= 8)
        return (UINT32)CgaToEga[Attr & 0x0F];
    return Palette[Attr & 0x0F];
}

/**
 * @brief
 * Maps a text-mode CGA-style character attribute to separate
 * foreground and background ARGB colors.
 **/
static VOID
FbConsAttrToColors(
    _In_ UCHAR Attr,
    _Out_ PUINT32 FgColor,
    _Out_ PUINT32 BgColor)
{
    *FgColor = FbConsAttrToSingleColor(Attr & 0x0F);
    *BgColor = FbConsAttrToSingleColor((Attr >> 4) & 0x0F);
}

VOID
FbConsClearScreen(
    _In_ UCHAR Attr)
{
    UINT32 FgColor, BgColor;
    FbConsAttrToColors(Attr, &FgColor, &BgColor);
    VidFbClearScreenColor(BgColor, FALSE);
}

/**
 * @brief
 * Displays a character at a given position with specific foreground
 * and background colors.
 **/
VOID
FbConsOutputChar(
    _In_ UCHAR Char,
    _In_ ULONG Column,
    _In_ ULONG Row,
    _In_ UINT32 FgColor,
    _In_ UINT32 BgColor)
{
    /* Don't display outside of the screen */
    if ((Column >= FBCONS_WIDTH) || (Row >= FBCONS_HEIGHT))
        return;
    VidFbOutputChar(Char, Column * CHAR_WIDTH, Row * CHAR_HEIGHT, FgColor, BgColor);
}

/**
 * @brief
 * Displays a character with specific text attributes at a given position.
 **/
VOID
FbConsPutChar(
    _In_ UCHAR Char,
    _In_ UCHAR Attr,
    _In_ ULONG Column,
    _In_ ULONG Row)
{
    UINT32 FgColor, BgColor;
    FbConsAttrToColors(Attr, &FgColor, &BgColor);
    FbConsOutputChar(Char, Column, Row, FgColor, BgColor);
}

/**
 * @brief
 * Returns the width and height in number of CGA characters/attributes, of a
 * full text-mode CGA-style character buffer rectangle that can fill the whole console.
 **/
VOID
FbConsGetDisplaySize(
    _Out_ PULONG Width,
    _Out_ PULONG Height,
    _Out_ PULONG Depth)
{
    // VidFbGetDisplaySize(Width, Height, Depth);
    // *Width  /= CHAR_WIDTH;
    // *Height /= CHAR_HEIGHT;
    *Width  = FBCONS_WIDTH;
    *Height = FBCONS_HEIGHT;
    *Depth  = framebufInfo.BitsPerPixel;
}

/**
 * @brief
 * Returns the size in bytes, of a full text-mode CGA-style
 * character buffer rectangle that can fill the whole console.
 **/
ULONG
FbConsGetBufferSize(VOID)
{
    return (FBCONS_HEIGHT * FBCONS_WIDTH * VGA_CHAR_SIZE);
}

/**
 * @brief
 * Copies a full text-mode CGA-style character buffer rectangle to the console.
 **/
// TODO: Write a VidFb "BitBlt" equivalent.
VOID
FbConsCopyOffScreenBufferToVRAM(
    _In_ PVOID Buffer)
{
    PUCHAR OffScreenBuffer = (PUCHAR)Buffer;
    ULONG Row, Col;

    // ULONG Width, Height, Depth;
    // FbConsGetDisplaySize(&Width, &Height, &Depth);
    ULONG Width = FBCONS_WIDTH, Height = FBCONS_HEIGHT;

    for (Row = 0; Row < Height; ++Row)
    {
        for (Col = 0; Col < Width; ++Col)
        {
            FbConsPutChar(OffScreenBuffer[0], OffScreenBuffer[1], Col, Row);
            OffScreenBuffer += VGA_CHAR_SIZE;
        }
    }
}

VOID
FbConsScrollUp(
    _In_ UCHAR Attr)
{
    UINT32 BgColor, Dummy;
    FbConsAttrToColors(Attr, &Dummy, &BgColor);
    VidFbScrollUp(BgColor, CHAR_HEIGHT);
}
