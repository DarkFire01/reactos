
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
    
    for(int  Newx = 0; Newx < width; Newx++)
    {
        for(int Newy = 0; Newy < height; Newy++)
        {
           // RefiSetPixel(Newx + x, Newy + y, Color);
           *((UINT32*)(refiFbData->BaseAddress + 4 * refiFbData->PixelsPerScanLine * (Newy + y) + 4 * (Newx + x))) = Color;     
        } 
    }
    #if 0
    UCHAR *where = (PUCHAR)refiFbData->BaseAddress;
    int i, j;
 
    for (i = 0; i < width; i++) {
        for (j = 0; j < height; j++) {
            //putpixel(vram, 64 + j, 64 + i, (r << 16) + (g << 8) + b);
           // where[j*refiFbData->PixelFormat] = Color;
           RefiSetPixel(i, j, Color);
        }
        where += (4 * refiFbData->PixelsPerScanLine);
    }
     #endif
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
    RefiClearScreen(0x000000);
    RefiDrawRectangle(32, 32, refiFbData->ScreenWidth - 64, 64, 0x555555);
    RefiDrawRectangle(32, refiFbData->ScreenHeight - 96, (refiFbData->ScreenWidth - 64) , 64, 0x555555);
    RefiDrawRectangle(64, 128, (refiFbData->ScreenWidth - 128), (refiFbData->ScreenHeight - 256), 0x0000FF);
   // RefiDrawRectangle(32, 32, 32, 3200, 0x555555);
   // RefiDrawRectangle(32, refiFbData-256>ScreenHeight - 32, refiFbData->ScreenWidth - 32, 96, 0x555555);

}
