/*
 * PROJECT:     ROSUEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI entry point
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include "include/rosefip.h"

EFI_GUID EfiGraphicsOutputProtocol = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
PROSEFI_FRAMEBUFFER_DATA refiFbData;

VOID
WriteStringThing(CHAR16* str)
{
    UINT32 fred;

    for(fred = 0; fred < 15; fred++)
    {
        Rs232PortPutByte(str[fred]);
    }
}

EFI_STATUS
RefiEntry(
    _In_ EFI_HANDLE ImageHandle,
    _In_ EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
    EFI_STATUS refiCheck;
    //UINT32 x, y, color;

    /* Just print a string to show we are alive on system with standard GOP */
    RefiSetColor(SystemTable, EFI_GREEN);
    RefiColPrint(SystemTable, L"RefiEntry: ");
    RefiSetColor(SystemTable, EFI_WHITE);
    RefiColPrint(SystemTable, L"Starting ROSEFI\r\n");


    SystemTable->BootServices->LocateProtocol(&EfiGraphicsOutputProtocol, 0, (void**)&gop);
    RefiInitGOP(SystemTable, refiFbData, gop);
    /* Initalize UEFI loader memory managment */
    refiCheck = RefiInitMemoryManager(SystemTable);

    Rs232PortInitialize(0, 115200);
    WriteStringThing(L"FredIsSweetMeow");

    RefiStallProcessor(SystemTable, 5000);
    RefiFatalFailure(SystemTable, refiCheck, gop, refiFbData->ScreenWidth, refiFbData->ScreenHeight);
    if(refiCheck != EFI_SUCCESS)
    {
    }
    for(;;)
    {

    }
    return EFI_SUCCESS;
}
