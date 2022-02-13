/*
 * PROJECT:     ROSUEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI entry point
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include "include/rosefip.h"

EFI_GUID EfiGraphicsOutputProtocol = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

EFI_STATUS
RefiEntry(
    _In_ EFI_HANDLE ImageHandle,
    _In_ EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
    EFI_STATUS refiCheck;

    RefiDebugInit(0);
    RefiColPrint(SystemTable, L"RefiEntry: Starting ROSEFI\r\n");

    /* Grab GOP pointer and initalize UI */
    SystemTable->BootServices->LocateProtocol(&EfiGraphicsOutputProtocol, 0, (void**)&gop);
    RefiInitUI(SystemTable, gop);

    RefiBaseClearScreen(0x000000);
    RefiBaseDrawBox(32, 32, 64, 64, 0x423E41);

    /* Start FS support */
    for(;;)
    {

    }
    refiCheck = RefiInitMemoryManager(SystemTable);
    if (refiCheck != EFI_SUCCESS)
    {
        RefiColPrint(SystemTable, L"RefiEntry: Initalizing MemoryManager has failed\r\n");
        RefiColPrint(SystemTable, L"RefiEntry: Rebooting...\r\n");
        RefiStallProcessor(SystemTable, 2000);
        SystemTable->RuntimeServices->ResetSystem(EfiResetCold, refiCheck, 0, 0);
    }

    return refiCheck;
}

VOID
RefiStartLoader()
{
    /* Here we begin real loader setup */
}