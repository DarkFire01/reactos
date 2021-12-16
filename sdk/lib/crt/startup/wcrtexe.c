/**
 * This file has no copyright assigned and is placed in the Public Domain.
 * This file is part of the w64 mingw-runtime package.
 * No warranty is given; refer to the file DISCLAIMER.PD within this package.
 */

void* __cdecl __RTC_memset(void* src, int val, size_t count)
{
    char *char_src = (char *)src;..........................

    while(count>0) {
        *char_src = val;
        char_src++;
        count--;
    }
    return src;
}


#define WPRFLAG
#define UNICODE
#define _UNICODE
#define mainCRTStartup wmainCRTStartup
#define WinMainCRTStartup wWinMainCRTStartup
#define mingw_pcinit mingw_pwcinit
#define mingw_pcppinit mingw_pwcppinit

#include "crtexe.c"
