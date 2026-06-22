/*
 * PROJECT:     ReactOS msvcrt.dll
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX/large-file CRT names expected by a modern mingw-w64
 *              libstdc++ (e.g. GCC 16). The real mingw CRT (msvcrt/ucrt)
 *              exports these; ReactOS only shipped the _-prefixed Win32
 *              variants, so provide thin forwarders here. Consumed by C++
 *              modules that link the real libstdc++ (e.g. its std::basic_file).
 */

#include <stdio.h>
#include <io.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <msvcrt.h>

__int64 __cdecl lseek64(int _FileHandle, __int64 _Offset, int _Origin)
{
    return _lseeki64(_FileHandle, _Offset, _Origin);
}

int __cdecl fstat64(int _FileHandle, struct _stat64 *_Stat)
{
    return _fstat64(_FileHandle, _Stat);
}

int __cdecl fseeko64(FILE *_File, __int64 _Offset, int _Origin)
{
    return _fseeki64(_File, _Offset, _Origin);
}

__int64 __cdecl ftello64(FILE *_File)
{
    return _ftelli64(_File);
}

/*
 * wctype() maps a character-class name to a mask usable with iswctype().
 * msvcrt's ctype.c only compiles it for _MSVCR_VER >= 120, but a modern
 * libstdc++ imports it from msvcrt.dll, so provide it here for plain msvcrt.
 */
#if _MSVCR_VER < 120
unsigned short __cdecl wctype(const char *_Property)
{
    static const struct { const char *name; unsigned short mask; } properties[] =
    {
        { "alnum", _DIGIT | _ALPHA },
        { "alpha", _ALPHA },
        { "cntrl", _CONTROL },
        { "digit", _DIGIT },
        { "graph", _DIGIT | _PUNCT | _ALPHA },
        { "lower", _LOWER },
        { "print", _DIGIT | _PUNCT | _BLANK | _ALPHA },
        { "punct", _PUNCT },
        { "space", _SPACE },
        { "upper", _UPPER },
        { "xdigit", _HEX },
    };
    unsigned int i;

    for (i = 0; i < sizeof(properties) / sizeof(properties[0]); i++)
    {
        if (strcmp(_Property, properties[i].name) == 0)
            return properties[i].mask;
    }

    return 0;
}
#endif /* _MSVCR_VER < 120 */
