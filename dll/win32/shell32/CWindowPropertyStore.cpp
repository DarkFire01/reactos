/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Application IDs of windows and of the current process
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include <propvarutil.h>

WINE_DEFAULT_DEBUG_CHANNEL(shell);

/* Window property value that stands for an explicit VT_NULL rather than an atom */
#define WINDOW_PROP_NULL    ((HANDLE)(LONG_PTR)(LONG)0xF0000000)

/* Taskband message that makes explorer resolve the window's application again */
#define TASKBAND_APPID_CHANGED  (WM_USER + 0x4B)

/* Long enough for "{fmtid} pid" */
#define PROPKEY_STRING_CCH  50

/* Every property is a window property named after its key, holding a global atom of its text */
class CWindowPropertyStore :
    public CComObjectRootEx<CComMultiThreadModel>,
    public IPropertyStore
{
public:
    CWindowPropertyStore();
    virtual ~CWindowPropertyStore();

    HRESULT Initialize(_In_ HWND hwnd);

    // *** IPropertyStore methods ***
    STDMETHODIMP GetCount(_Out_ DWORD *cProps) override;
    STDMETHODIMP GetAt(_In_ DWORD iProp, _Out_ PROPERTYKEY *pkey) override;
    STDMETHODIMP GetValue(_In_ REFPROPERTYKEY key, _Out_ PROPVARIANT *pv) override;
    STDMETHODIMP SetValue(_In_ REFPROPERTYKEY key, _In_ REFPROPVARIANT propvar) override;
    STDMETHODIMP Commit() override;

    DECLARE_NOT_AGGREGATABLE(CWindowPropertyStore)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CWindowPropertyStore)
        COM_INTERFACE_ENTRY_IID(IID_IPropertyStore, IPropertyStore)
    END_COM_MAP()

private:
    static BOOL CALLBACK EnumPropProc(
        _In_ HWND hwnd,
        _In_ PWSTR pszName,
        _In_ HANDLE hData,
        _In_ ULONG_PTR lParam);

    HWND GetTaskbandWindow();
    VOID ForgetKeys();

    HWND m_hwnd;
    HWND m_hwndTaskband;
    HDSA m_hdsaKeys;
    CComAutoCriticalSection m_csKeys;
};

/* The schema decides whether a property holds several values and what type it has */
static
HRESULT
GetPropertyDescription(
    _In_ REFPROPERTYKEY key,
    _Outptr_ IPropertyDescription **ppdesc)
{
    return PSGetPropertyDescription(key, IID_PPV_ARG(IPropertyDescription, ppdesc));
}

/* Splits "a; b; c" into a string vector */
static
HRESULT
InitVectorFromDelimitedString(
    _In_ PCWSTR pszList,
    _Out_ PROPVARIANT *ppv)
{
    PWSTR rgpsz[64];
    PWSTR pszCopy;
    PWSTR pszNext;
    ULONG cItems = 0;
    HRESULT hr;

    hr = SHStrDupW(pszList, &pszCopy);
    if (FAILED(hr))
        return hr;

    for (PWSTR psz = pszCopy; psz && cItems < _countof(rgpsz); psz = pszNext)
    {
        pszNext = StrChrW(psz, L';');
        if (pszNext)
            *pszNext++ = UNICODE_NULL;
        StrTrimW(psz, L" ");
        if (*psz)
            rgpsz[cItems++] = psz;
    }

    hr = InitPropVariantFromStringVector((PCWSTR *)rgpsz, cItems, ppv);
    CoTaskMemFree(pszCopy);
    return hr;
}

CWindowPropertyStore::CWindowPropertyStore() :
    m_hwnd(NULL),
    m_hwndTaskband(NULL),
    m_hdsaKeys(NULL)
{
}

CWindowPropertyStore::~CWindowPropertyStore()
{
    ForgetKeys();
}

HRESULT
CWindowPropertyStore::Initialize(
    _In_ HWND hwnd)
{
    if (m_hwnd)
        return E_UNEXPECTED;
    if (!hwnd)
        return E_INVALIDARG;

    m_hwnd = hwnd;
    return S_OK;
}

VOID
CWindowPropertyStore::ForgetKeys()
{
    m_csKeys.Lock();
    if (m_hdsaKeys)
    {
        DSA_Destroy(m_hdsaKeys);
        m_hdsaKeys = NULL;
    }
    m_csKeys.Unlock();
}

/* Window properties named by an atom are not ours; the rest that parse as keys are */
BOOL
CALLBACK
CWindowPropertyStore::EnumPropProc(
    _In_ HWND hwnd,
    _In_ PWSTR pszName,
    _In_ HANDLE hData,
    _In_ ULONG_PTR lParam)
{
    HDSA hdsa = (HDSA)lParam;
    PROPERTYKEY key;

    if (IS_INTRESOURCE(pszName))
        return TRUE;

    if (SUCCEEDED(PSPropertyKeyFromString(pszName, &key)))
        return DSA_AppendItem(hdsa, &key) != DSA_ERR;
    return TRUE;
}

STDMETHODIMP
CWindowPropertyStore::GetCount(
    _Out_ DWORD *cProps)
{
    HDSA hdsa;

    *cProps = 0;

    m_csKeys.Lock();
    hdsa = m_hdsaKeys;
    m_csKeys.Unlock();

    if (!hdsa)
    {
        hdsa = DSA_Create(sizeof(PROPERTYKEY), 8);
        if (!hdsa)
            return E_FAIL;

        if (EnumPropsExW(m_hwnd, (PROPENUMPROCEXW)EnumPropProc, (LPARAM)hdsa) != TRUE)
        {
            DSA_Destroy(hdsa);
            return E_FAIL;
        }

        m_csKeys.Lock();
        if (m_hdsaKeys)
            DSA_Destroy(hdsa);
        else
            m_hdsaKeys = hdsa;
        m_csKeys.Unlock();
    }

    m_csKeys.Lock();
    if (m_hdsaKeys)
        *cProps = DSA_GetItemCount(m_hdsaKeys);
    m_csKeys.Unlock();
    return S_OK;
}

STDMETHODIMP
CWindowPropertyStore::GetAt(
    _In_ DWORD iProp,
    _Out_ PROPERTYKEY *pkey)
{
    DWORD cProps;
    HRESULT hr;

    hr = GetCount(&cProps);
    if (FAILED(hr))
        return hr;
    if (iProp >= cProps)
        return E_INVALIDARG;

    m_csKeys.Lock();
    hr = DSA_GetItem(m_hdsaKeys, iProp, pkey) ? S_OK : E_INVALIDARG;
    m_csKeys.Unlock();
    return hr;
}

/* A property the window does not have reads as VT_EMPTY */
STDMETHODIMP
CWindowPropertyStore::GetValue(
    _In_ REFPROPERTYKEY key,
    _Out_ PROPVARIANT *pv)
{
    CComPtr<IPropertyDescription> pdesc;
    WCHAR szKey[PROPKEY_STRING_CCH];
    WCHAR szValue[MAX_PATH];
    PROPDESC_TYPE_FLAGS fFlags = PDTF_DEFAULT;
    HANDLE hValue;
    HRESULT hr;

    if (!pv)
        return E_POINTER;
    PropVariantInit(pv);

    hr = PSStringFromPropertyKey(key, szKey, _countof(szKey));
    if (FAILED(hr))
        return hr;

    hValue = GetPropW(m_hwnd, szKey);
    if (hValue == WINDOW_PROP_NULL)
    {
        pv->vt = VT_NULL;
        return S_OK;
    }
    if (!LOWORD(hValue) || !GlobalGetAtomNameW(LOWORD(hValue), szValue, _countof(szValue)))
        return S_OK;

    if (SUCCEEDED(GetPropertyDescription(key, &pdesc)))
        pdesc->GetTypeFlags(PDTF_MULTIPLEVALUES, &fFlags);

    if (fFlags & PDTF_MULTIPLEVALUES)
    {
        hr = InitVectorFromDelimitedString(szValue, pv);
    }
    else
    {
        hr = SHStrDupW(szValue, &pv->pwszVal);
        if (SUCCEEDED(hr))
            pv->vt = VT_LPWSTR;
    }
    if (FAILED(hr))
    {
        PropVariantInit(pv);
        return S_OK;
    }

    /* Properties the schema knows come back in their own type */
    if (pdesc)
        hr = pdesc->CoerceToCanonicalValue(pv);
    return hr;
}

STDMETHODIMP
CWindowPropertyStore::SetValue(
    _In_ REFPROPERTYKEY key,
    _In_ REFPROPVARIANT propvar)
{
    WCHAR szKey[PROPKEY_STRING_CCH];
    WCHAR szValue[MAX_PATH] = L"";
    HANDLE hOld;
    HANDLE hNew;
    BOOL fHadValue = FALSE;
    BOOL fAppID;
    HRESULT hr;

    ForgetKeys();

    hr = PSStringFromPropertyKey(key, szKey, _countof(szKey));
    if (FAILED(hr))
        return hr;
    if (propvar.vt != VT_EMPTY && propvar.vt != VT_NULL)
    {
        hr = PropVariantToString(propvar, szValue, _countof(szValue));
        if (FAILED(hr))
            return hr;
    }

    hOld = RemovePropW(m_hwnd, szKey);
    if (hOld != WINDOW_PROP_NULL && LOWORD(hOld))
    {
        GlobalDeleteAtom(LOWORD(hOld));
        fHadValue = TRUE;
    }

    fAppID = IsEqualPropertyKey(key, PKEY_AppUserModel_ID);

    /* VT_EMPTY only removes the property */
    if (propvar.vt == VT_EMPTY)
    {
        if (fAppID && fHadValue)
            PostMessageW(GetTaskbandWindow(), TASKBAND_APPID_CHANGED, (WPARAM)m_hwnd, 0);
        return S_OK;
    }

    if (propvar.vt == VT_NULL)
    {
        hNew = WINDOW_PROP_NULL;
    }
    else
    {
        ATOM atom = GlobalAddAtomW(szValue);

        if (!atom)
            return HRESULT_FROM_WIN32(GetLastError());
        hNew = (HANDLE)(ULONG_PTR)atom;
    }

    if (!SetPropW(m_hwnd, szKey, hNew))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        if (hNew != WINDOW_PROP_NULL)
            GlobalDeleteAtom(LOWORD(hNew));
        return FAILED(hr) ? hr : E_FAIL;
    }

    if (fAppID && propvar.vt != VT_NULL)
        PostMessageW(GetTaskbandWindow(), TASKBAND_APPID_CHANGED, (WPARAM)m_hwnd, 0);
    return S_OK;
}

STDMETHODIMP
CWindowPropertyStore::Commit()
{
    return S_OK;
}

HWND
CWindowPropertyStore::GetTaskbandWindow()
{
    HWND hwndTray;

    if (m_hwndTaskband && IsWindow(m_hwndTaskband))
        return m_hwndTaskband;

    hwndTray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (hwndTray)
        m_hwndTaskband = (HWND)GetPropW(hwndTray, L"TaskbandHWND");
    return m_hwndTaskband;
}

/*************************************************************************
 * SHGetPropertyStoreForWindow [SHELL32.@]
 */
EXTERN_C
HRESULT
WINAPI
SHGetPropertyStoreForWindow(
    _In_ HWND hwnd,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    if (!ppv)
        return E_INVALIDARG;
    *ppv = NULL;

    return ShellObjectCreatorInit<CWindowPropertyStore>(hwnd, riid, ppv);
}

/* Set once the window title in the process parameters is a copy this module made */
static BOOL g_fOwnWindowTitle = FALSE;

/*************************************************************************
 * SetCurrentProcessExplicitAppUserModelID [SHELL32.@]
 *
 * The ID replaces the startup window title, where the shell reads it back for this process.
 */
EXTERN_C
HRESULT
WINAPI
SetCurrentProcessExplicitAppUserModelID(
    _In_ PCWSTR AppID)
{
    PRTL_USER_PROCESS_PARAMETERS ProcessParameters;
    PWSTR pszCopy;
    SIZE_T cch;

    cch = wcslen(AppID);
    if (cch >= 128)
        return E_INVALIDARG;

    ProcessParameters = NtCurrentPeb()->ProcessParameters;
    if (!ProcessParameters)
        return E_FAIL;

    if ((ProcessParameters->WindowFlags & STARTF_TITLEISAPPID) && g_fOwnWindowTitle)
        LocalFree(ProcessParameters->WindowTitle.Buffer);

    pszCopy = (PWSTR)LocalAlloc(LMEM_FIXED, (cch + 1) * sizeof(WCHAR));
    if (!pszCopy)
        return E_OUTOFMEMORY;
    CopyMemory(pszCopy, AppID, (cch + 1) * sizeof(WCHAR));

    ProcessParameters->WindowFlags &= ~STARTF_TITLEISLINKNAME;
    ProcessParameters->WindowFlags |= STARTF_TITLEISAPPID;
    g_fOwnWindowTitle = TRUE;
    RtlInitUnicodeString(&ProcessParameters->WindowTitle, pszCopy);
    return S_OK;
}

/*************************************************************************
 * GetCurrentProcessExplicitAppUserModelID [SHELL32.@]
 */
EXTERN_C
HRESULT
WINAPI
GetCurrentProcessExplicitAppUserModelID(
    _Outptr_ PWSTR *AppID)
{
    STARTUPINFOW si;

    *AppID = NULL;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    GetStartupInfoW(&si);

    if (!(si.dwFlags & STARTF_TITLEISAPPID) || !si.lpTitle || !*si.lpTitle)
        return E_FAIL;
    return SHStrDupW(si.lpTitle, AppID);
}
