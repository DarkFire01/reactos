/*
 * PROJECT:     ReactOS Multimedia Device API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Class factory, registration and module entry points
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define MMD_ENUMERATOR_KEY  L"CLSID\\{BCDE0395-E52F-467C-8E3D-C4579291692E}"

HINSTANCE MmdInstance;
LONG MmdObjects;
static LONG MmdLocks;

class CClassFactory
    : public CMmdObject
    , public IClassFactory
{
public:
    STDMETHODIMP
    QueryInterface(
        _In_ REFIID Riid,
        _Outptr_ VOID **Object) override
    {
        if (!Object)
            return E_POINTER;
        if (!IsEqualIID(Riid, IID_IUnknown) && !IsEqualIID(Riid, IID_IClassFactory))
        {
            *Object = NULL;
            return E_NOINTERFACE;
        }
        *Object = static_cast<IClassFactory *>(this);
        AddRef();
        return S_OK;
    }

    MMD_IMPLEMENT_REFCOUNT()

    STDMETHODIMP
    CreateInstance(
        _In_opt_ IUnknown *Outer,
        _In_ REFIID Riid,
        _Outptr_ VOID **Object) override
    {
        if (!Object)
            return E_POINTER;
        *Object = NULL;
        if (Outer)
            return CLASS_E_NOAGGREGATION;
        return MmdCreateEnumerator(Riid, Object);
    }

    STDMETHODIMP
    LockServer(
        _In_ BOOL Lock) override
    {
        if (Lock)
            InterlockedIncrement(&MmdLocks);
        else
            InterlockedDecrement(&MmdLocks);
        return S_OK;
    }
};

EXTERN_C
HRESULT
WINAPI
DllGetClassObject(
    _In_ REFCLSID Clsid,
    _In_ REFIID Riid,
    _Outptr_ LPVOID *Object)
{
    CClassFactory *Factory;
    HRESULT Hr;

    if (!Object)
        return E_POINTER;
    *Object = NULL;
    if (!IsEqualCLSID(Clsid, CLSID_MMDeviceEnumerator))
        return CLASS_E_CLASSNOTAVAILABLE;

    Factory = new (std::nothrow) CClassFactory();
    if (!Factory)
        return E_OUTOFMEMORY;
    Hr = Factory->QueryInterface(Riid, Object);
    Factory->Release();
    return Hr;
}

EXTERN_C
HRESULT
WINAPI
DllCanUnloadNow(VOID)
{
    return (!MmdObjects && !MmdLocks) ? S_OK : S_FALSE;
}

static
LONG
MmdSetString(
    _In_ HKEY Key,
    _In_opt_ PCWSTR Name,
    _In_ DWORD Type,
    _In_ PCWSTR Value)
{
    return RegSetValueExW(Key, Name, 0, Type, (const BYTE *)Value, (DWORD)((wcslen(Value) + 1) * sizeof(WCHAR)));
}

EXTERN_C
HRESULT
WINAPI
DllRegisterServer(VOID)
{
    HKEY Class, Server;
    LONG Error;

    Error = RegCreateKeyExW(HKEY_CLASSES_ROOT, MMD_ENUMERATOR_KEY, 0, NULL, 0, KEY_WRITE, NULL, &Class, NULL);
    if (Error != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(Error);

    Error = MmdSetString(Class, NULL, REG_SZ, L"MMDeviceEnumerator class");
    if (Error == ERROR_SUCCESS)
        Error = RegCreateKeyExW(Class, L"InprocServer32", 0, NULL, 0, KEY_WRITE, NULL, &Server, NULL);
    if (Error == ERROR_SUCCESS)
    {
        Error = MmdSetString(Server, NULL, REG_EXPAND_SZ, L"%SystemRoot%\\System32\\mmdevapi.dll");
        if (Error == ERROR_SUCCESS)
            Error = MmdSetString(Server, L"ThreadingModel", REG_SZ, L"Both");
        RegCloseKey(Server);
    }
    RegCloseKey(Class);
    return HRESULT_FROM_WIN32(Error);
}

EXTERN_C
HRESULT
WINAPI
DllUnregisterServer(VOID)
{
    LONG Error = RegDeleteTreeW(HKEY_CLASSES_ROOT, MMD_ENUMERATOR_KEY);

    return (Error == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(Error);
}

EXTERN_C
BOOL
WINAPI
DllMain(
    _In_ HINSTANCE Instance,
    _In_ DWORD Reason,
    _In_opt_ LPVOID Reserved)
{
    switch (Reason)
    {
        case DLL_PROCESS_ATTACH:
            MmdInstance = Instance;
            DisableThreadLibraryCalls(Instance);
            MmdInitialize();
            break;

        case DLL_PROCESS_DETACH:
            /* At process exit other threads are gone mid-call, so their locks are left alone */
            if (!Reserved)
                MmdUninitialize();
            break;
    }
    return TRUE;
}
