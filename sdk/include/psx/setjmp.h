/* POSIX <setjmp.h> -- plain setjmp/longjmp (impl in libpsxcrt setjmp.c), NOT the
 * MSVC _setjmp3 macro (we don't link the MSVC CRT). jmp_buf uses the MSVC
 * _JUMP_BUFFER register order [ebp ebx edi esi esp eip] -- psxdll's siglongjmp
 * (sigjmp.c) reads the same slots. sigjmp_buf adds the MSTOOLS 2-slot tail
 * ([16] mask-saved flag, [17] mask); sigsetjmp must be a macro over setjmp
 * (a wrapper function's frame would be gone at siglongjmp time). MIT. */
#pragma once
typedef int jmp_buf[16];
int  setjmp(jmp_buf Env);
void longjmp(jmp_buf Env, int Value);

typedef int sigjmp_buf[16 + 2];
void *_sigjmp_store_mask(sigjmp_buf Env, int SaveMask);   /* psxdll ordinal 19 */
void  siglongjmp(sigjmp_buf Env, int Value);              /* psxdll ordinal 93 */
#define sigsetjmp(Env, SaveMask) setjmp((int *)_sigjmp_store_mask((Env), (SaveMask)))
