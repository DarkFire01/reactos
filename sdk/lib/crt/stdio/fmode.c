/* Copyright (C) 1994 DJ Delorie, see COPYING.DJ for details */
#include <precomp.h>

int _fmode = 0;

/*
 * @implemented
 */
int *__p__fmode(void)
{
   return &_fmode;
}

/***********************************************************************
 *              _set_fmode (MSVCRT.@)
 */
int CDECL _set_fmode(int mode)
{
    /* TODO: support _O_WTEXT */
    if(!MSVCRT_CHECK_PMT(mode==_O_TEXT || mode==_O_BINARY))
        return EINVAL;

    _fmode = mode;
    return 0;
}

/***********************************************************************
 *              _get_fmode (MSVCRT.@)
 */
int CDECL _get_fmode(int *mode)
{
    if(!MSVCRT_CHECK_PMT(mode))
        return EINVAL;

    *mode = _fmode;
    return 0;
}
