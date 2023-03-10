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

extern EFI_SYSTEM_TABLE* GlobalSystemTable;

/* FUNCTIONS ******************************************************************/

TIMEINFO*
UefiGetTime(VOID)
{
    static TIMEINFO Test;
    EFI_STATUS Status;
    EFI_TIME time = {0};

    Status = GlobalSystemTable->RuntimeServices->GetTime(&time, NULL);
    if (Status != EFI_SUCCESS)
        ERR("UefiGetTime: cannot get time status %d\n", Status);

    Test.Year = time.Year;
    Test.Month = time.Month;
    Test.Day = time.Day;
    Test.Hour = time.Hour;
    Test.Minute = time.Minute;
    Test.Second = time.Second;
    return &Test;
}
