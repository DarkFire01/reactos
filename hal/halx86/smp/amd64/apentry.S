/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Assembly file for real mode AP code
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include <asm.inc>

PUBLIC APEntry16
PUBLIC APEntry16End
PUBLIC APEntry32
PUBLIC APEntryJump32Offset
PUBLIC APEntryJump32Segment
PUBLIC TempPageTableAddr
PUBLIC APEntryCpuState

.code
APEntry16:
    cli

    xor ax, ax
	mov ds, ax
	mov ss, ax
    mov fs, ax
    mov gs, ax

    hlt

APEntry16End:
.long HEX(0)
APEntry32:
.long HEX(0)
APEntryJump32Offset:
.long HEX(0)
APEntryJump32Segment:
.long HEX(0)
TempPageTableAddr:
.long HEX(0)
APEntryCpuState:
.long HEX(0)
END
