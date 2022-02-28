/*
 * PROJECT:     ROSUEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI loader private header file
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

typedef struct _ROSEFI_FRAMEBUFFER_DATA
{
    ULONG_PTR    BaseAddress;
    ULONG64      BufferSize;
    UINT32       ScreenWidth;
    UINT32       ScreenHeight;
    UINT32       PixelsPerScanLine;
    UINT32       PixelFormat;
} ROSEFI_FRAMEBUFFER_DATA, *PROSEFI_FRAMEBUFFER_DATA;

/* Include required header files */
#include "rosefi.h"

#include "../ntldr/winldr.h"
#include "../lib/comm/debug.h"
#include "../lib/mm/mm.h"
#include "../lib/ui/ui.h"
#include "../lib/fs/fs.h"
#include "../lib/input/input.h"

#define ORANGE 0xffffa500
#define CYAN   0xff00ffff
#define RED    0xffff0000
#define GREEN  0xff00ff00
#define BLUE   0xff0000ff
#define GRAY   0xff888888
#define WHITE  0xffffffff
#define BLACK  0xff000000

VOID
RefiColSetCursor(EFI_SYSTEM_TABLE* SystemTable, UINT32 Col, UINT32 Row);

VOID
RefiColPrint(EFI_SYSTEM_TABLE* SystemTable, CHAR16* str);

VOID
RefiColClearScreen(EFI_SYSTEM_TABLE* SystemTable);

VOID
RefiColSetColor(EFI_SYSTEM_TABLE* SystemTable, UINTN Attribute);


BOOLEAN
RefiEfiCheckCSM(EFI_BOOT_SERVICES* BootServices);

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

/* UTILS */

VOID
RefiTrollBSoD(_In_ EFI_SYSTEM_TABLE *SystemTable);

int 
memcmp(const void* aptr, const void* bptr, size_t n);

VOID
RefiStallProcessor(EFI_SYSTEM_TABLE* SystemTable, UINTN d);

VOID
RefiItoa(unsigned long int n, unsigned short int* buffer, int basenumber);

UCHAR* hex_to_str(UCHAR* s, UINT32 v);

/* DEBUG */
unsigned short int* CheckStandardEFIError(unsigned long long s);

ULONG32 
RefiStrlen(PUCHAR str);