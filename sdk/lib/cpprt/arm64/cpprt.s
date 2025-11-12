#include <kxarm64.h>

    TEXTAREA

    MACRO
    DEFINE_ALIAS $FuncName, $Target
        LCLS _FuncName
        LCLS _Target
_FuncName SETS "|$FuncName|"
_Target SETS "|$Target|"
        IMPORT $_FuncName, WEAK $_Target
    MEND

    ; void __cdecl `eh vector constructor iterator'(void * __ptr64,unsigned __int64,int,void (__cdecl*)(void * __ptr64),void (__cdecl*)(void * __ptr64))
    DEFINE_ALIAS ??_L@YAXPEAX_KHP6AX0@Z2@Z, ?MSVCRTEX_eh_vector_constructor_iterator@@YAXPEAX_KHP6AX0@Z2@Z

    ; void __cdecl `eh vector constructor iterator'(void *,unsigned __int64,unsigned __int64,void (__cdecl*)(void *),void (__cdecl*)(void *))
    DEFINE_ALIAS ??_L@YAXPEAX_K1P6AX0@Z2@Z, ?MSVCRTEX_eh_vector_constructor_iterator@@YAXPEAX_KHP6AX0@Z2@Z

    ; void __cdecl `eh vector destructor iterator'(void *,unsigned __int64,unsigned __int64,void (__cdecl*)(void *))
    DEFINE_ALIAS ??_M@YAXPEAX_K1P6AX0@Z@Z, ?MSVCRTEX_eh_vector_destructor_iterator@@YAXPEAX_KHP6AX0@Z@Z

    ; void __cdecl `eh vector destructor iterator'(void * __ptr64,unsigned __int64,int,void (__cdecl*)(void * __ptr64))
    DEFINE_ALIAS ??_M@YAXPEAX_KHP6AX0@Z@Z, ?MSVCRTEX_eh_vector_destructor_iterator@@YAXPEAX_KHP6AX0@Z@Z

    DEFINE_ALIAS ??3@YAXPEAXAEBUnothrow_t@std@@@Z, ??3@YAXPEAX@Z
    DEFINE_ALIAS ??_V@YAXPEAXAEBUnothrow_t@std@@@Z, ??3@YAXPEAX@Z
    DEFINE_ALIAS ??3@YAXPEAX_K@Z, ??3@YAXPEAX@Z

    END
