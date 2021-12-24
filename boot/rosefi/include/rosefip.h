/*
 * PROJECT:     ROSUEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI loader private header file
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once


/* Include required header files */
#include "rosefi.h"
#include "../display/eficonsolelib.h"

BOOLEAN
RefiEfiCheckCSM(EFI_BOOT_SERVICES* BootServices);
