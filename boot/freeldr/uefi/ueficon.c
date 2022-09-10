/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI Console output
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */


#include <uefildr.h>

#include <debug.h>

extern EFI_SYSTEM_TABLE * GlobalSystemTable;

VOID
UefiConsPutChar(int Ch)
{
    GlobalSystemTable->ConOut->OutputString(GlobalSystemTable->ConOut, (CHAR16*)&Ch);
}

VOID
UefiConsSetCursor(UINT32 Col, UINT32 Row)
{
    GlobalSystemTable->ConOut->SetCursorPosition(GlobalSystemTable->ConOut, Col * 8, Row * 16);
}
