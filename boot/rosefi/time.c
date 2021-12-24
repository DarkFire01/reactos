/*
 * PROJECT:     ROSUEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI Timing source file
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include "include/rosefip.h"

VOID
RefiStallProcessor(EFI_SYSTEM_TABLE* SystemTable, UINTN d)
{
    SystemTable->BootServices->Stall(d * 1000);
}
