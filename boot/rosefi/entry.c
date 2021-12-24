/*
 * PROJECT:     ROSUEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI entry point
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include "include/rosefip.h"

EFI_STATUS
RefiEntry(
    _In_ EFI_HANDLE ImageHandle,
    _In_ EFI_SYSTEM_TABLE *SystemTable)
{
    RefiSetColor(SystemTable, EFI_MAGENTA);
    RefiColPrint(SystemTable, L"Starting ROSEFI:");
    for(;;)
    {
        
    }
    return EFI_SUCCESS;
}
