/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI Debug functions
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>
#include <debug.h>

extern PREACTOS_INTERNAL_BGCONTEXT refiFbData;
VOID
UefiPrintF(PUCHAR String, unsigned X, unsigned Y, ULONG FgColor, ULONG BgColor)
{
    for (UINT32 i = 0; i < UefiStrlen(String); i++)
    {
        UefiVideoOutputChar(String[i], X + i, Y, FgColor, BgColor);
    }
}

VOID
RefiDrawRectangle(UINT32 x, UINT32 y, UINT32 width, UINT32 height, UINT32 Color)
{
    for(int  Newx = 0; Newx < width; Newx++)
    {
        for(int Newy = 0; Newy < height; Newy++)
        {
           *((UINT32*)(refiFbData->BaseAddress + 4 * refiFbData->PixelsPerScanLine * (Newy + y) + 4 * (Newx + x))) = Color;     
        } 
    }
}


VOID
RefiDrawUIBackground()
{
  //  RefiClearScreen(0x000000);
    RefiDrawRectangle(32, 32, refiFbData->ScreenWidth - 64, 64, 0x555555);
    RefiDrawRectangle(32, refiFbData->ScreenHeight - 96, (refiFbData->ScreenWidth - 64) , 64, 0x555555);
}

/* private functions */
VOID
UefiDbg_PrintUEFIFramebuf()
{
#if 0
    UefiVideoClearScreen(0);
    UefiConsSetCursor(0, 0);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"refiFbData->BufferSize:");
    UefiItoa(refiFbData->BufferSize, buffer, 10);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, buffer);
    UefiConsSetCursor(0, 1);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"refiFbData->ScreenWidth:");
    UefiItoa(refiFbData->ScreenWidth, buffer, 10);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, buffer);
    UefiConsSetCursor(0, 2);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"refiFbData->ScreenHeight:");
    UefiItoa(refiFbData->ScreenHeight, buffer, 10);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, buffer);
    UefiConsSetCursor(0, 3);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"refiFbData->PixelsPerScanLine:");
    UefiItoa(refiFbData->PixelsPerScanLine, buffer, 10);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, buffer);
    UefiConsSetCursor(0, 4);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"refiFbData->PixelFormat:");
    UefiItoa(refiFbData->PixelFormat, buffer, 10);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, buffer);
    UefiConsSetCursor(0, 5);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"refiFbData->BaseAddress:");
    UefiItoa(refiFbData->BaseAddress, buffer, 16);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, buffer);
#endif
}