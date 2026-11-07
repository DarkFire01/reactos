/*
 * PROJECT:     ReactOS NT Library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64EC exception entry of the x64 emulator
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kxarm64.h>

/* sizeof(ARM64_NT_CONTEXT), then the CONTEXT_EX area in front of the exception record */
#define ExContextSize   0x390
#define ExContextExSize 0x20

    IMPORT |#LdrpDispatchEmulatorException|

    TEXTAREA

//
// The emulator raises exceptions of x64 code by entering here on its stack, an ARM64
// context holding the x64 registers at sp and the exception record after the CONTEXT_EX area.
//
    ARM64EC_ENTRY_THUNK A64NAME(KiUserExceptionDispatcher),2,0
    LEAF_ENTRY_COMDAT A64NAME(KiUserExceptionDispatcher)

    mov     x0, sp
    add     x1, sp, #(ExContextSize + ExContextExSize)
    bl      |#LdrpDispatchEmulatorException|
    brk     #0xF003

    LEAF_END A64NAME(KiUserExceptionDispatcher)

    END
/* EOF */
