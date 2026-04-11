/*
 * MSVC ARM64: UCRT string/arm64/*.s targets armasm64 with ksarm64 macros;
 * use simple C implementations until the assembly path is wired correctly.
 */
#ifdef _M_ARM64

#include <stddef.h>

size_t __cdecl strlen(const char *s)
{
    const char *p = s;
    while (*p)
        ++p;
    return (size_t)(p - s);
}

size_t __cdecl wcslen(const wchar_t *s)
{
    const wchar_t *p = s;
    while (*p)
        ++p;
    return (size_t)(p - s);
}

size_t __cdecl strnlen(const char *s, size_t maxlen)
{
    size_t n = 0;
    if (!s)
        return 0;
    while (n < maxlen && s[n])
        ++n;
    return n;
}

size_t __cdecl wcsnlen(const wchar_t *s, size_t maxlen)
{
    size_t n = 0;
    if (!s)
        return 0;
    while (n < maxlen && s[n])
        ++n;
    return n;
}

#endif /* _M_ARM64 */
