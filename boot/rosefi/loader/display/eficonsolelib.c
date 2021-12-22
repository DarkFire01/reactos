/*
 * PROJECT:     ROSEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ROSEFI console source file
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include "../include/uefiloader.h"

VOID
RefiColSetCursor(UINT32 Col, UINT32 Row, EFI_SYSTEM_TABLE* SystemTable)
{
    SystemTable->ConOut->SetCursorPosition(SystemTable->ConOut, Col, Row);
}

VOID
RefiColPrint(CHAR16* str,  EFI_SYSTEM_TABLE* SystemTable)
{
    SystemTable->ConOut->OutputString(SystemTable->ConOut, str);
}

VOID
RefiClearScreen(EFI_SYSTEM_TABLE* SystemTable)
{
    SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
}

VOID
RefiSetColor(UINTN Attribute, EFI_SYSTEM_TABLE* SystemTable)
{
    SystemTable->ConOut->SetAttribute(SystemTable->ConOut, Attribute);
}
