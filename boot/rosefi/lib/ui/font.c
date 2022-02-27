/*
 * PROJECT:     ROSUEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ROSEFI Utility File
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include "ui.h"

PROSEFI_FRAMEBUFFER_DATA refiFbData;
/* In theory ill expand the font system to be more then this. But for now.. */
PPSF1_FONT CoreFont;

VOID
RefiInitFonts(PROSEFI_FRAMEBUFFER_DATA FbData)
{
    refiFbData = FbData;
    /* Initalize Fonts */
}

BOOLEAN
RefiLoadPSF1Font()
{
    EFI_STATUS status;

    if (CoreFont->psf1_header.magic[0] == 0x36)
    {
        status = EFI_SUCCESS;
    }
    else
    {
        return FALSE;
    }
    if (CoreFont->psf1_header.magic[1] == 0x04)
    {
        status = EFI_SUCCESS;
    }
    else 
    {
        return FALSE;
    }

    if (status == EFI_SUCCESS)
    {
        return TRUE;
    }

    return FALSE;
}

VOID
RefiFontPrint(CHAR16* str, UINT32 xOff, UINT32 yOff, UINT32 Color)
{
    #if 0
    ULONG_PTR pixPtr = refiFbData->BaseAddress;
    //PUCHAR FontPtr = (PUCHAR)psf1_font->glyphBuffer + (chr * psf1_font->psf1_header.charsize);
    PUCHAR FontPtr = (PUCHAR)psf1_font->glyphBuffer;
    UINT32 x, y;
    for (y = yOff; y < yOff + 16; y++)
    {
        for (x = xOff; x < xOff + 8; x++)
        {
            if ((*FontPtr & (0b10000000 >> (x - xOff))) > 0)
            {
                *((UINT32*)(pixPtr + 4 * refiFbData->PixelsPerScanLine * (y) + 4 * (x))) = colour;
               // *((UINT32*)(pixPtr + refiFbData->PixelFormat * refiFbData->PixelsPerScanLine * y + refiFbData->PixelFormat * x)) = colour;
               // *(unsigned int*)(pixPtr + x + (y * refiFbData->PixelsPerScanLine)) = colour;
            }
           
            #if 0
            if ((*FontPtr & (0b10000000 >> (x - xOff))) > 0)
            {
                *((UINT32*)(pixPtr + 4 * refiFbData->PixelsPerScanLine * (y) + 4 * (x))) = colour;
            }
            #endif
        }
        FontPtr++;
    }
    #endif

}
