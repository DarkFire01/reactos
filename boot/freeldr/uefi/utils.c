/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI Utils source
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

/* GLOBALS ********************************************************************/

extern EFI_SYSTEM_TABLE * GlobalSystemTable;
extern EFI_HANDLE GlobalImageHandle;

EFI_INPUT_KEY Key;
BOOLEAN scancode;

/* FUNCTIONS ******************************************************************/

TIMEINFO*
UefiGetTime(VOID)
{
    static TIMEINFO Test;
    EFI_STATUS Status;
    EFI_TIME time;

    Status = GlobalSystemTable->RuntimeServices->GetTime (&time, NULL);
    if (Status != EFI_SUCCESS)
    {
        ERR("UefiGetTime: Error cannot GetTime: %d", Status);
    }
    Test.Year = time.Year;
    Test.Month = time.Month;
    Test.Day = time.Day;
    Test.Hour = time.Hour;
    Test.Minute = time.Minute;
    Test.Second = time.Second;
    return &Test;
}

BOOLEAN
UefiConsKbHit(VOID)
{
    Key.UnicodeChar = 0;
    Key.ScanCode = 0;
    GlobalSystemTable->ConIn->ReadKeyStroke(GlobalSystemTable->ConIn, &Key);

    if(Key.UnicodeChar != 0)
    {
        //TRACE("keyboard kit %d", Key.UnicodeChar);
        scancode = FALSE;
        return TRUE;

    }
    else if(Key.ScanCode != 0)
    {
        //TRACE("keyboard kit %d", Key.ScanCode);
        scancode = TRUE;
        return TRUE;
    }
    else
    {
        return FALSE;
    }

    return 0;
}

int
UefiConsGetCh(void)
{
    if (scancode == TRUE)
    {
        switch(Key.ScanCode)
        {
            case 18:
                return KEY_F8;
            case 1:
                return KEY_UP;
            case 2:
                return KEY_DOWN;
            case 3:
                return KEY_RIGHT;
            case 4:
                return KEY_LEFT;
            case 23:
                return KEY_ESC;
        }
        return Key.ScanCode;
    }
    else
    {
        return Key.UnicodeChar;
    }
}

VOID
UefiHwIdle(VOID)
{
    /* Nothing to do here ... */
}

VOID
UefiPcBeep(VOID)
{
    /* Sadly no UEFI audio, yet */
}
