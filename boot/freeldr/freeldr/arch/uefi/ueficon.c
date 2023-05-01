/*
 * PROJECT:     FreeLoader UEFI Support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Console output
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#define CHAR_WIDTH  8
#define CHAR_HEIGHT 16

/* GLOBALS ********************************************************************/

extern EFI_SYSTEM_TABLE* GlobalSystemTable;
static unsigned CurrentCursorX = 0;
static unsigned CurrentCursorY = 0;
static unsigned CurrentAttr = 0x0f;
static EFI_INPUT_KEY Key;
static BOOLEAN scancode;

/* FUNCTIONS ******************************************************************/

VOID
UefiConsPutChar(int c)
{
    ULONG Width, Height, Unused;
    BOOLEAN NeedScroll;

    UefiVideoGetDisplaySize(&Width, &Height, &Unused);

    NeedScroll = (CurrentCursorY >= Height);
    if (NeedScroll)
    {
        UefiVideoScrollUp();
        --CurrentCursorY;
    }
    if (c == '\r')
    {
        CurrentCursorX = 0;
    }
    else if (c == '\n')
    {
        CurrentCursorX = 0;

        if (!NeedScroll)
            ++CurrentCursorY;
    }
    else if (c == '\t')
    {
        CurrentCursorX = (CurrentCursorX + 8) & ~7;
    }
    else
    {
        UefiVideoPutChar(c, CurrentAttr, CurrentCursorX, CurrentCursorY);
        CurrentCursorX++;
    }
    if (CurrentCursorX >= Width)
    {
        CurrentCursorX = 0;
        CurrentCursorY++;
    }
}

/* Read into the public Keystroke handler */
BOOLEAN
UefiConsKbHit(VOID)
{
    Key.UnicodeChar = 0;
    Key.ScanCode = 0;
    GlobalSystemTable->ConIn->ReadKeyStroke(GlobalSystemTable->ConIn, &Key);

    if (Key.UnicodeChar != 0)
    {
        scancode = FALSE;
        return TRUE;
    }
    else if (Key.ScanCode != 0)
    {
        scancode = TRUE;
        return TRUE;
    }

    return FALSE;
}

int
UefiConsGetCh(void)
{
    /* The UEFI Scan codes are in an entirely different order than BIOS. */
    if (scancode)
    {
        switch (Key.ScanCode)
        {
            case SCAN_UP:
                return KEY_UP;
            case SCAN_DOWN:
                return KEY_DOWN;
            case SCAN_RIGHT:
                return KEY_RIGHT;
            case SCAN_LEFT:
                return KEY_LEFT;
            case SCAN_F1:
                return KEY_F1;
            case SCAN_F2:
                return KEY_F2;
            case SCAN_F3:
                return KEY_F3;
            case SCAN_F4:
                return KEY_F4;
            case SCAN_F5:
                return KEY_F5;
            case SCAN_F6:
                return KEY_F6;
            case SCAN_F7:
                return KEY_F7;
            case SCAN_F8:
                return KEY_F8;
            case SCAN_F9:
                return KEY_F9;
            case SCAN_F10:
                return KEY_F10;
            case SCAN_ESC:
                return KEY_ESC;
        }
        return Key.ScanCode;
    }
    else
    {
        return Key.UnicodeChar;
    }
}
