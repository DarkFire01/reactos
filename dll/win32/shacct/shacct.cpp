/*
 * PROJECT:     ReactOS Shell Account Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Module entry points and registration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define USER_PICTURES_FOLDER L"Microsoft\\User Account Pictures"

EXTERN_C const CLSID CLSID_LocalUserAccounts =
    { 0x4f6bcd94, 0xc2a5, 0x42ce, { 0x8d, 0xbc, 0x31, 0xe7, 0x94, 0xbe, 0x46, 0x30 } };
EXTERN_C const IID IID_ILocalUserAccounts =
    { 0x3c708557, 0xc99d, 0x4fa3, { 0x92, 0x31, 0x56, 0x51, 0x84, 0x18, 0xb4, 0xe4 } };

#define SAM_FMTID { 0x705d8364, 0x7547, 0x468c, { 0x8c, 0x88, 0x84, 0x86, 0x0b, 0xcb, 0xed, 0x4c } }

EXTERN_C const PROPERTYKEY PKEY_SAM_Name = { SAM_FMTID, 2 };
EXTERN_C const PROPERTYKEY PKEY_SAM_FullName = { SAM_FMTID, 9 };
EXTERN_C const PROPERTYKEY PKEY_SAM_AdminComment = { SAM_FMTID, 14 };
EXTERN_C const PROPERTYKEY PKEY_SAM_UserComment = { SAM_FMTID, 16 };
EXTERN_C const PROPERTYKEY PKEY_SAM_SecurityID = { SAM_FMTID, 18 };
EXTERN_C const PROPERTYKEY PKEY_SAM_UserAccountControl = { SAM_FMTID, 19 };
EXTERN_C const PROPERTYKEY PKEY_SAM_UserPicture = { SAM_FMTID, 24 };
EXTERN_C const PROPERTYKEY PKEY_SAM_Type = { SAM_FMTID, 32 };
EXTERN_C const PROPERTYKEY PKEY_SAM_ResidualID = { SAM_FMTID, 103 };

typedef struct DEFAULT_PICTURE
{
    WORD idResource;
    PCWSTR pszFileName;
} DEFAULT_PICTURE;

/* The tiles explorer and the logon screen fall back to for accounts without a picture */
static const DEFAULT_PICTURE s_rgDefaultPictures[] =
{
    { IDR_DEFAULT_USER_PICTURE, L"user.bmp" },
    { IDR_DEFAULT_GUEST_PICTURE, L"guest.bmp" },
};

BEGIN_OBJECT_MAP(ObjectMap)
    OBJECT_ENTRY(CLSID_LocalUserAccounts, CLocalUserAccounts)
END_OBJECT_MAP()

CComModule gModule;

/* Existing files are kept so a picture the user picked survives a reinstall */
static
HRESULT
WINAPI
WriteDefaultPicture(
    _In_ PCWSTR pszDir,
    _In_ const DEFAULT_PICTURE *pPicture)
{
    WCHAR szPath[MAX_PATH];
    HINSTANCE hInstance = _AtlBaseModule.GetResourceInstance();
    HRSRC hResource;
    HGLOBAL hData;
    const void *pvData;
    DWORD cbData;
    DWORD cbWritten;
    HANDLE hFile;
    HRESULT hr;

    hr = StringCchPrintfW(szPath, ARRAYSIZE(szPath), L"%s\\%s", pszDir, pPicture->pszFileName);
    if (FAILED(hr))
        return hr;

    hResource = FindResourceW(hInstance, MAKEINTRESOURCEW(pPicture->idResource), RT_RCDATA);
    if (!hResource)
        return HRESULT_FROM_WIN32(GetLastError());

    cbData = SizeofResource(hInstance, hResource);
    hData = LoadResource(hInstance, hResource);
    pvData = hData ? LockResource(hData) : NULL;
    if (!pvData || !cbData)
        return E_FAIL;

    hFile = CreateFileW(szPath,
                        GENERIC_WRITE,
                        0,
                        NULL,
                        CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL,
                        NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() == ERROR_FILE_EXISTS)
            return S_FALSE;
        return HRESULT_FROM_WIN32(GetLastError());
    }

    if (!WriteFile(hFile, pvData, cbData, &cbWritten, NULL))
        hr = HRESULT_FROM_WIN32(GetLastError());
    else if (cbWritten != cbData)
        hr = E_FAIL;

    CloseHandle(hFile);

    if (FAILED(hr))
        DeleteFileW(szPath);

    return hr;
}

/* Setup runs this before any user logs on, so %ProgramData% may not be set yet */
static
HRESULT
WINAPI
InstallDefaultPictures(VOID)
{
    WCHAR szDir[MAX_PATH];
    DWORD cchDir = ARRAYSIZE(szDir);
    HRESULT hr;
    HRESULT hrPicture;
    UINT i;
    int iError;

    if (!GetAllUsersProfileDirectoryW(szDir, &cchDir))
        return HRESULT_FROM_WIN32(GetLastError());

    hr = StringCchCatW(szDir, ARRAYSIZE(szDir), L"\\" USER_PICTURES_FOLDER);
    if (FAILED(hr))
        return hr;

    iError = SHCreateDirectoryExW(NULL, szDir, NULL);
    if (iError != ERROR_SUCCESS && iError != ERROR_ALREADY_EXISTS && iError != ERROR_FILE_EXISTS)
        return HRESULT_FROM_WIN32(iError);

    for (i = 0; i < ARRAYSIZE(s_rgDefaultPictures); i++)
    {
        hrPicture = WriteDefaultPicture(szDir, &s_rgDefaultPictures[i]);
        if (FAILED(hrPicture) && SUCCEEDED(hr))
            hr = hrPicture;
    }

    return hr;
}

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

STDAPI
DllInstall(
    _In_ BOOL bInstall,
    _In_opt_ PCWSTR pszCmdLine)
{
    UNREFERENCED_PARAMETER(pszCmdLine);

    if (!bInstall)
        return S_OK;

    return InstallDefaultPictures();
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
