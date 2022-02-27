/*
 * PROJECT:     ReactOS UEFI Bootloader
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI keyboard core source file
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */


#include "../../include/rosefip.h"

VOID
RefiUefiHitAnyKey(EFI_SYSTEM_TABLE* SystemTable)
{
    // This clears the keyboard buffer.
    SystemTable->ConIn->Reset(SystemTable->ConIn, 1);

    // We setup the struct to take keyboard input.
    EFI_INPUT_KEY Key;

    // In a while loop to see if the keyboard has a key stroke in the buffer.
    while((SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key)) == EFI_NOT_READY);
}

VOID
RefiUefiResetKeyboard(EFI_SYSTEM_TABLE* SystemTable)
{
    SystemTable->ConIn->Reset(SystemTable->ConIn, 1);
}

BOOLEAN 
RefiUefiGetKey(CHAR16 key, EFI_INPUT_KEY CheckKeystroke)
{
    if(CheckKeystroke.UnicodeChar == key)
    {
        return 1;
    } 
    else 
    {
        return 0;
    }
}

EFI_STATUS 
RefiUefiCheckKey(EFI_SYSTEM_TABLE* SystemTable, EFI_INPUT_KEY CheckKeystroke)
{
    return SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &CheckKeystroke);
}
