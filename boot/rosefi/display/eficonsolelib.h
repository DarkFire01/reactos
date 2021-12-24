/*
 * PROJECT:     ROSEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ROSEFI console header file
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

VOID
RefiColSetCursor(EFI_SYSTEM_TABLE* SystemTable, UINT32 Col, UINT32 Row);

VOID
RefiColPrint(EFI_SYSTEM_TABLE* SystemTable, CHAR16* str);

VOID
RefiClearScreen(EFI_SYSTEM_TABLE* SystemTable);

VOID
RefiSetColor(EFI_SYSTEM_TABLE* SystemTable, UINTN Attribute);
