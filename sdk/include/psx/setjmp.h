/* POSIX <setjmp.h> -- plain setjmp/longjmp (impl in libpsxcrt setjmp.c), NOT the
 * MSVC _setjmp3 macro (we don't link the MSVC CRT). MIT. */
#pragma once
typedef int jmp_buf[16];
int  setjmp(jmp_buf Env);
void longjmp(jmp_buf Env, int Value);
