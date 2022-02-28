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
PSF1_FONT SystemFont;
BOOLEAN test;

VOID
RefiInitFonts(PROSEFI_FRAMEBUFFER_DATA FbData)
{
    refiFbData = FbData;
    //test = RefiLoadPSF1Font();
    //RefiReadFile(L"\\font.psf", (PVOID)CoreFont);
    /* Initalize Fonts */
}

BOOLEAN
RefiLoadPSF1Font(EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS status;
    RefiReadFile(L"\\font.psf", (PVOID)CoreFont, SystemTable);
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
   // RefiColPrint("Unable to verify PSF1 signature")
    //RefiReadFile(L"\\font.psf", (PVOID)CoreFont);
    SystemFont.psf1_header = CoreFont->psf1_header;
    SystemFont.glyphBuffer = (PVOID)(CoreFont + sizeof(PSF1_HEADER));
    if (status == EFI_SUCCESS)
    {
        return TRUE;
    }

    return FALSE;
}

VOID
RefiPrintF(UCHAR* str, UINT32 xOff, UINT32 yOff, UINT32 Color, UINT32 Scale)
{
    for (int i = 0; i < RefiStrlen(str); i++)
    {
        RefiFontPrint(str[i], xOff + (i * 8), yOff, Color);
    }
}

VOID
RefiFontPrint(CHAR16 str, UINT32 xOff, UINT32 yOff, UINT32 Color)
{
    ULONG_PTR pixPtr = refiFbData->BaseAddress;
    //PUCHAR FontPtr = (PUCHAR)SystemFont.glyphBuffer + (chr * psf1_font->psf1_header.charsize);
    PUCHAR FontPtr = (PUCHAR)SystemFont.glyphBuffer + (str * SystemFont.psf1_header.charsize) - 59;
    UINT32 x, y;
    for (y = yOff; y < yOff + 16; y++)
    {
        for (x = xOff; x < xOff + 8; x++)
        {
            if ((*FontPtr & (0b10000000 >> (x - xOff))) > 0)
            {
                *((UINT32*)(pixPtr + 4 * refiFbData->PixelsPerScanLine * (y) + 4 * (x))) = Color;
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
}
