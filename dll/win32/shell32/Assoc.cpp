/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Assoc
 * COPYRIGHT:   Copyright 2023 Justin Miller <justinmiller100@gmail.com>
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell_ad);

/*
 *
 *  UNIMPLEMENTED
 *  https://www.geoffchappell.com/studies/windows/shell/shell32/api/assocelem/createelement.htm?tx=57,63
 */
EXTERN_C
HRESULT
WINAPI
AssocCreateElement (
    REFCLSID rclsid,
    REFIID riid,
    PVOID *ppv)
{
    return E_FAIL;
}

/*
 *
 *  UNIMPLEMENTED
 *  https://www.geoffchappell.com/studies/windows/shell/shell32/api/assocutil/getkeys.htm
 */

EXTERN_C
UINT
WINAPI
SHGetAssocKeys (
    IUnknown *punkAssociationInfo,
    HKEY *aKeys,
    UINT cKeys)
{
        UNIMPLEMENTED;
    __debugbreak();
    return 0;
}

/*
 *
 *  UNIMPLEMENTED
 *  https://www.geoffchappell.com/studies/windows/shell/shell32/api/assocutil/getproplistforext.htm
 */

EXTERN_C
HRESULT
WINAPI
AssocGetPropListForExt (
    PCWSTR pszAssoc,
    PROPERTYKEY *pkey,
    REFIID riid,
    PVOID *ppv)
{
    UNIMPLEMENTED;
    __debugbreak();
    return E_FAIL;
}