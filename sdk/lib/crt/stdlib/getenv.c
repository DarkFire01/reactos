/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS system libraries
 * FILE:        lib/sdk/crt/stdlib/getenv.c
 * PURPOSE:     Unknown
 * PROGRAMER:   Unknown
 * UPDATE HISTORY:
 *              25/11/05: Added license header
 */

#include <precomp.h>
//#undef environ

/*
 * @implemented
 */
char *getenv(const char *name)
{
   char **env;
   size_t length = strlen(name);

   for (env = *__p__environ(); *env; env++)
   {
      char *str = *env;
      char *pos = strchr(str,'=');
      if (pos && ((unsigned int)(pos - str) == length) && !_strnicmp(str, name, length))
         return pos + 1;
   }
   return NULL;
}

/*
 * @implemented
 */
wchar_t *_wgetenv(const wchar_t *name)
{
   wchar_t **env;
   size_t length = wcslen(name);

   for (env = *__p__wenviron(); *env; env++)
   {
      wchar_t *str = *env;
      wchar_t *pos = wcschr(str, L'=');
      if (pos && ((unsigned int)(pos - str) == length) && !_wcsnicmp(str, name, length))
         return pos + 1;
   }
   return NULL;
}

/******************************************************************
 *		getenv_s (MSVCRT.@)
 */
int CDECL getenv_s(size_t *pReturnValue, char* buffer, size_t numberOfElements, const char *varname)
{
    char *e;

    if (!MSVCRT_CHECK_PMT(pReturnValue != NULL)) return EINVAL;
    if (!MSVCRT_CHECK_PMT(!(buffer == NULL && numberOfElements > 0))) return EINVAL;
    if (!MSVCRT_CHECK_PMT(varname != NULL)) return EINVAL;

    if (!(e = getenv(varname)))
    {
        *pReturnValue = 0;
        return *_errno() = EINVAL;
    }
    *pReturnValue = strlen(e) + 1;
    if (numberOfElements < *pReturnValue)
    {
        return *_errno() = ERANGE;
    }
    strcpy(buffer, e);
    return 0;
}

/******************************************************************
 *		_wgetenv_s (MSVCRT.@)
 */
int CDECL _wgetenv_s(size_t *pReturnValue, wchar_t *buffer, size_t numberOfElements,
                     const wchar_t *varname)
{
    wchar_t *e;

    if (!MSVCRT_CHECK_PMT(pReturnValue != NULL)) return EINVAL;
    if (!MSVCRT_CHECK_PMT(!(buffer == NULL && numberOfElements > 0))) return EINVAL;
    if (!MSVCRT_CHECK_PMT(varname != NULL)) return EINVAL;

    if (!(e = _wgetenv(varname)))
    {
        *pReturnValue = 0;
        return *_errno() = EINVAL;
    }
    *pReturnValue = wcslen(e) + 1;
    if (numberOfElements < *pReturnValue)
    {
        return *_errno() = ERANGE;
    }
    wcscpy(buffer, e);
    return 0;
}
