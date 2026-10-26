/*
 * PROJECT:     ReactOS Authentication UI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Module entry points and registration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

EXTERN_C const CLSID CLSID_ShutdownChoices =
    { 0x14ce31dc, 0xabc2, 0x484c, { 0xb0, 0x61, 0xcf, 0x34, 0x16, 0xae, 0xd8, 0xff } };
EXTERN_C const IID IID_IShutdownChoices =
    { 0xf678fcde, 0xeb44, 0x4b6e, { 0x9b, 0x75, 0xcc, 0x4a, 0x66, 0x1f, 0x52, 0x63 } };
EXTERN_C const IID IID_IEnumShutdownChoices =
    { 0xa0b16477, 0x52f1, 0x4cfe, { 0xb1, 0xcc, 0x38, 0x8c, 0xd0, 0xe3, 0xe2, 0x3a } };

BEGIN_OBJECT_MAP(ObjectMap)
    OBJECT_ENTRY(CLSID_ShutdownChoices, CShutdownChoices)
END_OBJECT_MAP()

CComModule gModule;

STDAPI
DllCanUnloadNow(VOID)
{
    return gModule.DllCanUnloadNow();
}

STDAPI
DllGetClassObject(
    _In_ REFCLSID rclsid,
    _In_ REFIID riid,
    _Outptr_ LPVOID *ppv)
{
    return gModule.DllGetClassObject(rclsid, riid, ppv);
}

STDAPI
DllRegisterServer(VOID)
{
    return gModule.DllRegisterServer(FALSE);
}

STDAPI
DllUnregisterServer(VOID)
{
    return gModule.DllUnregisterServer(FALSE);
}

STDAPI_(BOOL)
DllMain(
    _In_ HINSTANCE hInstance,
    _In_ DWORD dwReason,
    _In_opt_ LPVOID pvReserved)
{
    UNREFERENCED_PARAMETER(pvReserved);

    if (dwReason == DLL_PROCESS_ATTACH)
    {
        gModule.Init(ObjectMap, hInstance, NULL);
        DisableThreadLibraryCalls(hInstance);
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        gModule.Term();
    }

    return TRUE;
}
