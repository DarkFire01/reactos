/*
 * PROJECT:     ROSUEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI loader private header file
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once


/* Include required header files */
#include "rosefi.h"

#include "../display/eficonsolelib.h"
#include "../lib/mm/mm.h"
#include "../ntldr/winldr.h"
#include "../lib/comm/debug.h"
#include "../display/gui.h"

typedef struct _ROSEFI_FRAMEBUFFER_DATA
{
    ULONG_PTR    BaseAddress;
    ULONG64      BufferSize;
    UINT32       ScreenWidth;
    UINT32       ScreenHeight;
    UINT32       PixelsPerScanLine;
} ROSEFI_FRAMEBUFFER_DATA, *PROSEFI_FRAMEBUFFER_DATA;

BOOLEAN
RefiEfiCheckCSM(EFI_BOOT_SERVICES* BootServices);

VOID
RefiStallProcessor(EFI_SYSTEM_TABLE* SystemTable, UINTN d);

#if 0
VOID
RefiTrollBSoD(_In_ EFI_SYSTEM_TABLE *SystemTable, 
                 _In_ EFI_STATUS refiCheck,
                 _In_ EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
                 _In_ UINT32 x, UINT32 y);
#endif

VOID
RefiInitUI(_In_ EFI_SYSTEM_TABLE *SystemTable,
            _In_ EFI_GRAPHICS_OUTPUT_PROTOCOL* gop);

// PLACE HOLDER ______________________
VOID
RefiHitAnyKey(EFI_SYSTEM_TABLE* SystemTable);

VOID
RefiResetKeyboard(EFI_SYSTEM_TABLE* SystemTable);

BOOLEAN 
RefiGetKey(CHAR16 key, EFI_INPUT_KEY CheckKeystroke);

EFI_STATUS 
RefiCheckKey(EFI_SYSTEM_TABLE* SystemTable, EFI_INPUT_KEY CheckKeystroke);

VOID
RefiTrollBSoD(_In_ EFI_SYSTEM_TABLE *SystemTable, 
              _In_ EFI_STATUS refiCheck,
              _In_ EFI_GRAPHICS_OUTPUT_PROTOCOL* gop, _In_ UINT32 x, UINT32 y);

VOID 
InitializeFILESYSTEM(_In_ EFI_HANDLE ImageHandle,
                     _In_ EFI_SYSTEM_TABLE *SystemTable);
void 
readFile(EFI_SYSTEM_TABLE *SystemTable, CHAR16* FileName);

void closeFile(EFI_SYSTEM_TABLE *SystemTable, EFI_FILE_PROTOCOL* FileHandle);
EFI_FILE_PROTOCOL* openFile(EFI_SYSTEM_TABLE *SystemTable, CHAR16* FileName);