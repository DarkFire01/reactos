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

VOID
RefiFatalFailure(_In_ EFI_SYSTEM_TABLE *SystemTable, 
                 _In_ EFI_STATUS refiCheck,
                 _In_ EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
                 _In_ UINT32 x, UINT32 y);

VOID
RefiInitGOP(_In_ EFI_SYSTEM_TABLE *SystemTable,
            _In_ PROSEFI_FRAMEBUFFER_DATA refiData,
            _In_ EFI_GRAPHICS_OUTPUT_PROTOCOL* gop);

VOID
RefiItoa(ULONG32 n, UINT16* buffer, UINT32 basenumber);
