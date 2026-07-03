/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Nonlocal jumps (<setjmp.h>)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

//
// Plain setjmp/longjmp, not the MSVC _setjmp3 form. jmp_buf slot order is
// ebp, ebx, edi, esi, esp, eip; psxdll's siglongjmp reads the same slots.
//
typedef int jmp_buf[16];
int  setjmp(jmp_buf Env);
void longjmp(jmp_buf Env, int Value);

//
// sigjmp_buf appends two slots: [16] mask-saved flag, [17] saved mask.
// sigsetjmp must be a macro so setjmp runs in the caller's frame.
//
typedef int sigjmp_buf[16 + 2];
void *_sigjmp_store_mask(sigjmp_buf Env, int SaveMask);   /* psxdll ordinal 19 */
void  siglongjmp(sigjmp_buf Env, int Value);              /* psxdll ordinal 93 */
#define sigsetjmp(Env, SaveMask) setjmp((int *)_sigjmp_store_mask((Env), (SaveMask)))
