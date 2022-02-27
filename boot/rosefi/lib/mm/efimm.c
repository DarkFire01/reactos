/*
 * PROJECT:     ROSUEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ROSEFI Utility File
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include "mm.h"

EFI_STATUS
RefiInitMemoryManager(_In_ EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS refiCheck;

    RefiColPrint(SystemTable, L"RefiInitMemoryManager: ");
    RefiColPrint(SystemTable, L"Initalizing Memory Manager\r\n");
    refiCheck = EFI_ACCESS_DENIED;
    /* Get UEFI memory map */

    return refiCheck;
}