/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     AMD64 Architecture specifc UEFI loader code
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

/**** Includes ***********************************************************************************/

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);
void __ExitUefi();

VOID
ArchSpecificExitUefi()
{
    _disable();

    __writeeflags(0);

   // __ExitUefi();

}