/*
 * PROJECT:     ROSEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ROSEFI console header file
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

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
RefiClearScreen(EFI_SYSTEM_TABLE* SystemTable);

VOID
RefiSetColor(EFI_SYSTEM_TABLE* SystemTable, UINTN Attribute);

VOID
CreateFilledBox(UINT32 xPos, UINT32 yPos, UINT32 w, UINT32 h, EFI_GRAPHICS_OUTPUT_BLT_PIXEL GraphicsColor, EFI_GRAPHICS_OUTPUT_PROTOCOL* gop);

EFI_GRAPHICS_OUTPUT_BLT_PIXEL
SetGraphicsColor(UINT32 color);

VOID
RefiAssignPixel(ULONG* screen, UINT32 x, UINT32 y, UINT64 color);
