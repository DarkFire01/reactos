/*
 * PROJECT:     ROSEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ROSEFI default console source file
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include "../include/rosefip.h"

/* Before swapping to a true GOP set up, we can use these to quickly debug */

VOID
RefiColSetCursor(EFI_SYSTEM_TABLE* SystemTable, UINT32 Col, UINT32 Row)
{
    SystemTable->ConOut->SetCursorPosition(SystemTable->ConOut, Col, Row);
}

VOID
RefiColPrint(EFI_SYSTEM_TABLE* SystemTable, CHAR16* str)
{
    //RefiDebugPrint(str);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, str);
}

VOID
RefiClearScreen(EFI_SYSTEM_TABLE* SystemTable)
{
    SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
}

VOID
RefiSetColor(EFI_SYSTEM_TABLE* SystemTable, UINTN Attribute)
{
    SystemTable->ConOut->SetAttribute(SystemTable->ConOut, Attribute);
}
