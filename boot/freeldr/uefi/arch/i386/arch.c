/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     I386 Architecture specifc UEFI loader code
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

/**** Includes ***********************************************************************************/

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

void _reloadsegment(VOID);

extern PVOID EndOfStack;
extern PVOID gdtptr;
extern PVOID i386idtptr;

VOID
ArchSpecificExitUefi()
{
    TRACE("Launching Arch Specific code");

    _disable();

    __writeeflags(0);

              /* Re-initialize EFLAGS */
    Ke386SetGlobalDescriptorTable(&gdtptr);
    __lidt(&i386idtptr);
    #if defined(__GNUC__) || defined(__clang__)
        asm("ljmp    $0x08, $1f\n"
            "1:\n");
    #elif defined(_MSC_VER)
        /* We cannot express the above in MASM so we use this far return instead */
        __asm
        {
            push 8
            push offset resume
            retf
            resume:
        };
    #else
    #error
    #endif
    _reloadsegment();
}