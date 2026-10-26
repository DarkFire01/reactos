/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     C entry points shell32 uses to create the view frame objects
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

/* shell32 is built with a native wchar_t, this library is not, so nothing C++ crosses over */

EXTERN_C
HRESULT
WINAPI
DUIFrame_CreateViewFrame(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    return DUIViewFrame_CreateInstance(riid, ppv);
}

EXTERN_C
HRESULT
WINAPI
DUIFrame_CreateCommonLayoutDefinition(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    return CommonLayoutDefinition_CreateInstance(riid, ppv);
}

EXTERN_C
HRESULT
WINAPI
DUIFrame_CreateCommandBarObject(
    _In_ REFCLSID rclsid,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    return CommandBar_CreateInstance(rclsid, riid, ppv);
}

EXTERN_C
HRESULT
WINAPI
DUIFrame_CreateCommonLayoutModifier(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    return CommonLayoutModifier_CreateInstance(riid, ppv);
}
