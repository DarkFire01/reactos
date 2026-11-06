/*
 * PROJECT:     ReactOS C++ runtime library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64EC C++ runtime aliases
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kxarm64.h>

// Both the x64 name and the ARM64EC ($$h) name lead to the ARM64EC definition

    // void __cdecl `eh vector constructor iterator'(void *,unsigned __int64,int,void (__cdecl*)(void *),void (__cdecl*)(void *))
    IMPORT |??_L@YAXPEAX_KHP6AX0@Z2@Z|, WEAK |?MSVCRTEX_eh_vector_constructor_iterator@@$$hYAXPEAX_KHP6AX0@Z2@Z|
    IMPORT |??_L@$$hYAXPEAX_KHP6AX0@Z2@Z|, WEAK |?MSVCRTEX_eh_vector_constructor_iterator@@$$hYAXPEAX_KHP6AX0@Z2@Z|

    // void __cdecl `eh vector constructor iterator'(void *,unsigned __int64,unsigned __int64,void (__cdecl*)(void *),void (__cdecl*)(void *))
    IMPORT |??_L@YAXPEAX_K1P6AX0@Z2@Z|, WEAK |?MSVCRTEX_eh_vector_constructor_iterator@@$$hYAXPEAX_KHP6AX0@Z2@Z|
    IMPORT |??_L@$$hYAXPEAX_K1P6AX0@Z2@Z|, WEAK |?MSVCRTEX_eh_vector_constructor_iterator@@$$hYAXPEAX_KHP6AX0@Z2@Z|

    // void __cdecl `eh vector destructor iterator'(void *,unsigned __int64,unsigned __int64,void (__cdecl*)(void *))
    IMPORT |??_M@YAXPEAX_K1P6AX0@Z@Z|, WEAK |?MSVCRTEX_eh_vector_destructor_iterator@@$$hYAXPEAX_KHP6AX0@Z@Z|
    IMPORT |??_M@$$hYAXPEAX_K1P6AX0@Z@Z|, WEAK |?MSVCRTEX_eh_vector_destructor_iterator@@$$hYAXPEAX_KHP6AX0@Z@Z|

    // void __cdecl `eh vector destructor iterator'(void *,unsigned __int64,int,void (__cdecl*)(void *))
    IMPORT |??_M@YAXPEAX_KHP6AX0@Z@Z|, WEAK |?MSVCRTEX_eh_vector_destructor_iterator@@$$hYAXPEAX_KHP6AX0@Z@Z|
    IMPORT |??_M@$$hYAXPEAX_KHP6AX0@Z@Z|, WEAK |?MSVCRTEX_eh_vector_destructor_iterator@@$$hYAXPEAX_KHP6AX0@Z@Z|

    // void __cdecl operator delete(void *,struct std::nothrow_t const &)
    IMPORT |??3@YAXPEAXAEBUnothrow_t@std@@@Z|, WEAK |??3@$$hYAXPEAX@Z|
    IMPORT |??3@YAXPEAXAEBUnothrow_t@std@@$$h@Z|, WEAK |??3@$$hYAXPEAX@Z|

    // void __cdecl operator delete[](void *,struct std::nothrow_t const &)
    IMPORT |??_V@YAXPEAXAEBUnothrow_t@std@@@Z|, WEAK |??3@$$hYAXPEAX@Z|
    IMPORT |??_V@YAXPEAXAEBUnothrow_t@std@@$$h@Z|, WEAK |??3@$$hYAXPEAX@Z|

    // void __cdecl operator delete(void *,unsigned __int64)
    IMPORT |??3@YAXPEAX_K@Z|, WEAK |??3@$$hYAXPEAX@Z|
    IMPORT |??3@$$hYAXPEAX_K@Z|, WEAK |??3@$$hYAXPEAX@Z|

    END
