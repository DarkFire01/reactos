#include "gui.h"

PROSEFI_FRAMEBUFFER_DATA refiFbData;

VOID
RefiInitUI(_In_ EFI_SYSTEM_TABLE *SystemTable,
           _In_ EFI_GRAPHICS_OUTPUT_PROTOCOL* gop)
{
    refiFbData->BaseAddress        = (ULONG_PTR)gop->Mode->FrameBufferBase;
    refiFbData->BufferSize         = gop->Mode->FrameBufferSize;
    refiFbData->ScreenWidth        = gop->Mode->Info->HorizontalResolution;
    refiFbData->ScreenHeight       = gop->Mode->Info->VerticalResolution;
    refiFbData->PixelsPerScanLine  = gop->Mode->Info->PixelsPerScanLine;
}

VOID
RosEFIAdvPutChar(PPSF1_FONT psf1_font, unsigned int colour, char chr, unsigned int xOff, unsigned int yOff)
{
    unsigned int* pixPtr = (unsigned int*)refiFbData->BaseAddress;
    char* fontPtr = (PUCHAR)psf1_font->glyphBuffer + (chr * psf1_font->psf1_header.charsize);
    for (unsigned long y = yOff; y < yOff + 16; y++){
        for (unsigned long x = xOff; x < xOff+8; x++){
            if ((*fontPtr & (0b10000000 >> (x - xOff))) > 0){
                    *(unsigned int*)(pixPtr + x + (y * refiFbData->PixelsPerScanLine)) = colour;
                }

        }
        fontPtr++;
    }
}

VOID
RefiBaseDrawBox(UINT32 Thisx, UINT32 Thisy, UINT32 width, UINT32 height, UINT32 Color)
{
    for(int y = Thisy; y < height; y++)
    {
        for(int x = Thisx; x < width; x++)
        {
            *((UINT32*)(refiFbData->BaseAddress + 4 * refiFbData->PixelsPerScanLine * (y) + 4 * (x))) = Color;
        }
    }
}

VOID
RefiBaseDrawRandomShit(UINT32 Color)
{
    for(int y = 0; y < 100; y++)
    {
        for(int x = 0; x < 100; x++)
        {
            *((UINT32*)(refiFbData->BaseAddress + 4 * refiFbData->PixelsPerScanLine * ((y-1) * (y-1)) + 4 * ((x-2) * (x-2)))) = Color;
        }
    }
}

VOID
RefiBaseClearScreen(UINT32 Color)
{
    for(int y = 0; y < refiFbData->ScreenHeight; y++)
    {
        for(int x = 0; x < refiFbData->ScreenWidth; x++)
        {
            *((UINT32*)(refiFbData->BaseAddress + 4 * refiFbData->PixelsPerScanLine * (y) + 4 * (x))) = Color;
        }
    }
}

/*
 * Set a pixel without factoring in the UI
 */
VOID
RefiBaseSetPixel(UINT32 x, UINT32 y, UINT32 Color)
{
    *((UINT32*)(refiFbData->BaseAddress + 4 * refiFbData->PixelsPerScanLine * (y) + 4 * (x))) = Color;
        
}
#if 0
VOID
RefiAssignPixel(_In_ ROSEFI_FRAMEBUFFER_DATA refiFbData,
                _In_ UINT32 x, UINT32 y)
{
    //CreateFilledBox(x, y, 32, 32, SetGraphicsColor(0x00FFFF), gop);
}

VOID
RefiDrawBox(_In_ ROSEFI_FRAMEBUFFER_DATA refiFbData,
            _In_ UINT32 x, UINT32 y,
            _In_ UINT32 width, UINT32 height,
            _In_ ULONG32 Color)
{

}

VOID
RefiClearScreenUI(_In_ ROSEFI_FRAMEBUFFER_DATA refiFbData,
                  _In_ ULONG32 Color)
{
    
}
#endif
