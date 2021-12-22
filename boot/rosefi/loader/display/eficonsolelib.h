/*
 * PROJECT:     ROSEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ROSEFI console header file
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

VOID
RefiColSetCursor(UINT32 Col, UINT32 Row, EFI_SYSTEM_TABLE* SystemTable);

VOID
RefiColPrint(CHAR16* str,  EFI_SYSTEM_TABLE* SystemTable);

VOID
RefiClearScreen(EFI_SYSTEM_TABLE* SystemTable);

VOID
RefiSetColor(UINTN Attribute, EFI_SYSTEM_TABLE* SystemTable);
