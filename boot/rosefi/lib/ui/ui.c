
#include "ui.h"

PROSEFI_FRAMEBUFFER_DATA refiFbData;

VOID
RefiInitUI(_In_ EFI_SYSTEM_TABLE *SystemTable,
           _In_ EFI_GRAPHICS_OUTPUT_PROTOCOL* gop)
{
    EFI_STATUS Status = SystemTable->BootServices->AllocatePool(EfiLoaderData, sizeof(refiFbData), (void**)&refiFbData);
    refiFbData->BaseAddress        = (ULONG_PTR)gop->Mode->FrameBufferBase;
    refiFbData->BufferSize         = gop->Mode->FrameBufferSize;
    refiFbData->ScreenWidth        = gop->Mode->Info->HorizontalResolution;
    refiFbData->ScreenHeight       = gop->Mode->Info->VerticalResolution;
    refiFbData->PixelsPerScanLine  = gop->Mode->Info->PixelsPerScanLine;
    refiFbData->PixelFormat        = gop->Mode->Info->PixelFormat;
    Status = 0;
}

VOID
RefiPrintUI(CHAR16* str, UINT32 x, UINT32 y, UINT32 Color)
{
    /* TODO: adjust for.. literally everything */
    // For now we are passing the parameters to the font specific call 
    RefiFontPrint(str, x, y, Color);
}

VOID
RefiClearScreen(UINT32 Color)
{
    for(int y = 0; y < refiFbData->ScreenHeight; y++)
    {
        for(int x = 0; x < refiFbData->ScreenWidth; x++)
        {
            *((UINT32*)(refiFbData->BaseAddress + 4 * refiFbData->PixelsPerScanLine * (y) + 4 * (x))) = Color;
        }
    }
}

VOID
RefiDrawRectangle(UINT32 x, UINT32 y, UINT32 width, UINT32 height, UINT32 Color)
{
    for(int Newy = y; Newy < height; Newy++)
    {
        for(int  Newx = x; Newx < width; Newx++)
        {
            RefiSetPixel(Newx, Newy, Color);
        }
    }
}

/*
 * Set a pixel directly on the framebuffer
 */
VOID
RefiSetPixel(UINT32 x, UINT32 y, UINT32 Color)
{
    *((UINT32*)(refiFbData->BaseAddress + 4 * refiFbData->PixelsPerScanLine * (y) + 4 * (x))) = Color;     
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
RefiDrawUIBackground()
{
    //RefiDrawRectangle(32, 32, refiFbData->ScreenWidth - 32, 64, 0x555555);
    RefiDrawRectangle(32, 32, 640, 64, 0x555555);
  //  RefiDrawRectangle(32, refiFbData->ScreenHeight - 32 - 64, refiFbData->ScreenWidth - 32, 64, 0x555555);

}
