/*
 * PROJECT:     ReactOS build system
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Force-included C++ compatibility shim for modern (UCRT-based)
 *              mingw-w64 toolchains (e.g. GCC 16).
 *
 * A UCRT-based libstdc++ <cstdlib> unconditionally does
 *     using ::quick_exit;
 *     using ::at_quick_exit;
 * but ReactOS builds in msvcrt mode, where mingw's <stdlib.h> only declares
 * those C11 functions under #ifdef _UCRT. Because <cstdlib> reaches <stdlib.h>
 * through #include_next, ReactOS's own stdlib.h is bypassed and cannot supply
 * the declarations. Declaring them here (before any header is processed) lets
 * those using-directives resolve. No definition is provided: a translation
 * unit that never actually calls std::quick_exit needs only the declaration.
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _CRT_QUICK_EXIT_DEFINED
#define _CRT_QUICK_EXIT_DEFINED
void __cdecl quick_exit(int _Code);
int __cdecl at_quick_exit(void (__cdecl *)(void));
#endif

#ifdef __cplusplus
}
#endif
