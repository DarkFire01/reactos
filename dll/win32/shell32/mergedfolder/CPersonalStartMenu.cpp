/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Start menu item filter that hides what policies and settings remove
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include "mergedfolder.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

EXTERN_C VOID WINAPI SHReValidateDarwinCache(VOID);

EXTERN_C const CLSID CLSID_StartMenuGroupPolicyFilter =
    { 0x23613363, 0x0028, 0x431d, { 0xa4, 0x9e, 0xa3, 0xcd, 0x48, 0x2d, 0x39, 0x26 } };

static const GUID POLID_NoStartMenuSubFolders =
    { 0x78627e11, 0xec80, 0x49a3, { 0xb3, 0xb7, 0x6a, 0xbe, 0x73, 0x96, 0x93, 0xfc } };
static const GUID POLID_NoWindowsUpdate =
    { 0x68f894e4, 0x3c9d, 0x4c68, { 0x82, 0x95, 0xc8, 0x38, 0x0e, 0x22, 0x76, 0x55 } };
static const GUID POLID_NoSMConfigurePrograms =
    { 0xffd2f653, 0x7f76, 0x4eba, { 0x9b, 0x49, 0xb7, 0x39, 0x24, 0xa7, 0xef, 0xae } };
static const GUID POLID_NoRun =
    { 0xa9ea41a7, 0x4ea4, 0x4776, { 0x8c, 0x1e, 0xaa, 0x06, 0xa2, 0x51, 0x60, 0xf9 } };
static const GUID POLID_StartMenuLogoff =
    { 0xef2a45cf, 0xd2c9, 0x4ef8, { 0xa8, 0x4b, 0x12, 0xd7, 0xf1, 0x15, 0x0c, 0xa8 } };
static const GUID POLID_NoClose =
    { 0x29b0cc43, 0x2f2b, 0x4d0c, { 0xa0, 0x81, 0xa5, 0x28, 0xdd, 0x34, 0x96, 0x31 } };
static const GUID POLID_TurnOffSidebar =
    { 0x13a1260e, 0x41c7, 0x462f, { 0xa3, 0x97, 0xe7, 0xfb, 0x03, 0xbe, 0x94, 0x7f } };
static const GUID POLID_NoMobilityCenter =
    { 0xa55f91dc, 0xaadc, 0x4038, { 0x82, 0xfc, 0x84, 0x80, 0x6d, 0x92, 0x89, 0x78 } };

struct DECLSPEC_UUID("8CC5BAF5-11E9-4DD2-9810-45214134C800") DECLSPEC_NOVTABLE
IStartMenuGroupPolicyFilter : public IUnknown
{
    STDMETHOD(IsFileRestrictedByPolicyOrSystemMetrics)(
        _In_ LPCWSTR pszPath) PURE;
};

struct DECLSPEC_UUID("F1763F2A-6E44-426D-AC5B-641C866DCD63") DECLSPEC_NOVTABLE
IStartMenuMSIAds : public IUnknown
{
    STDMETHOD(IsMSIAds)(
        _In_ PCIDLIST_ABSOLUTE pidl) PURE;
};

static const IID IID_IStartMenuGroupPolicyFilter = __uuidof(IStartMenuGroupPolicyFilter);
static const IID IID_IStartMenuMSIAds = __uuidof(IStartMenuMSIAds);

static BOOL
IsTargetPathSame(
    _In_ LPCWSTR pszPath,
    _In_ LPCWSTR pszTarget)
{
    return StrCmpIW(pszPath, pszTarget) == 0;
}

static BOOL
IsPathUnderFolder(
    _In_ LPCWSTR pszPath,
    _In_ LPCWSTR pszFolder)
{
    return PathIsPrefixW(pszFolder, pszPath);
}

/* Mobility Center only exists on portable machines outside safe mode */
static BOOL
IsMobilityCenterEnabled(VOID)
{
    static BOOL s_fChecked = FALSE;
    static BOOL s_fEnabled = FALSE;
    SYSTEM_POWER_CAPABILITIES spc;
    POWER_PLATFORM_ROLE role;
    HKEY hKey;

    if (s_fChecked)
        return s_fEnabled;

    s_fChecked = TRUE;
    s_fEnabled = FALSE;
    if (GetSystemMetrics(SM_CLEANBOOT))
        return s_fEnabled;

    role = PowerDeterminePlatformRole();
    if (role != PlatformRoleMobile &&
        (role != PlatformRoleDesktop || !GetPwrCapabilities(&spc) || !spc.SystemBatteriesPresent ||
         spc.BatteriesAreShortTerm))
    {
        return s_fEnabled;
    }

    if (SHWindowsPolicyEx(POLID_NoMobilityCenter, 0))
        return s_fEnabled;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\explorer\\ControlPanel\\"
                      L"NameSpace\\{5ea4f148-308c-46d7-98a9-49041b1dd468}",
                      0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
        s_fEnabled = TRUE;
    }
    return s_fEnabled;
}

static BOOL
IsMobilityCenterRestricted(VOID)
{
    return !IsMobilityCenterEnabled();
}

/* A start menu item that a policy, a system metric or a check takes away */
struct STARTMENU_RESTRICTION
{
    const GUID *pPolicy;
    int iSystemMetric;
    BOOL (*pfnRestricted)(VOID);
    BOOL (*pfnMatch)(LPCWSTR pszPath, LPCWSTR pszTarget);
    const KNOWNFOLDERID *pFolder;
    LPCWSTR pszSubPath;
};

static const STARTMENU_RESTRICTION c_StartMenuRestrictions[] =
{
    { &POLID_NoStartMenuSubFolders, -1, NULL, IsPathUnderFolder, &FOLDERID_StartMenu, NULL },
    { &POLID_NoWindowsUpdate, -1, NULL, IsTargetPathSame, &FOLDERID_CommonStartMenu,
      L"Windows Update.lnk" },
    { &POLID_NoSMConfigurePrograms, -1, NULL, IsTargetPathSame, &FOLDERID_CommonStartMenu,
      L"Default Programs.lnk" },
    { &POLID_NoRun, -1, NULL, IsTargetPathSame, &FOLDERID_Programs, L"Accessories\\Run.lnk" },
    { &POLID_StartMenuLogoff, -1, NULL, IsTargetPathSame, &FOLDERID_Programs,
      L"Accessories\\System Tools\\logoff.lnk" },
    { &POLID_NoClose, -1, NULL, IsTargetPathSame, &FOLDERID_Programs,
      L"Accessories\\System Tools\\power.lnk" },
    { &POLID_TurnOffSidebar, -1, NULL, IsTargetPathSame, &FOLDERID_CommonPrograms,
      L"Accessories\\Sidebar.lnk" },
    { NULL, -1, IsMobilityCenterRestricted, IsTargetPathSame, &FOLDERID_CommonPrograms,
      L"Accessories\\Mobility Center.lnk" },
};

class CStartMenuGroupPolicyFilter : public IStartMenuGroupPolicyFilter
{
public:
    CStartMenuGroupPolicyFilter() :
        m_cRef(1)
    {
        ZeroMemory(m_apszPaths, sizeof(m_apszPaths));
    }

    STDMETHODIMP
    QueryInterface(
        _In_ REFIID riid,
        _Outptr_ void **ppv) override
    {
        static const QITAB qit[] =
        {
            QITABENT(CStartMenuGroupPolicyFilter, IStartMenuGroupPolicyFilter),
            { NULL, 0 }
        };

        return QISearch(this, qit, riid, ppv);
    }

    STDMETHODIMP_(ULONG)
    AddRef() override
    {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG)
    Release() override
    {
        LONG cRef = InterlockedDecrement(&m_cRef);

        if (!cRef)
            delete this;
        return cRef;
    }

    /* S_OK when the item is taken away, S_FALSE when it stays */
    STDMETHODIMP
    IsFileRestrictedByPolicyOrSystemMetrics(
        _In_ LPCWSTR pszPath) override
    {
        UINT i;

        for (i = 0; i < _countof(c_StartMenuRestrictions); i++)
        {
            const STARTMENU_RESTRICTION *pEntry = &c_StartMenuRestrictions[i];

            if (!(pEntry->pPolicy && SHWindowsPolicyEx(*pEntry->pPolicy, 0)) &&
                !(pEntry->iSystemMetric != -1 && !GetSystemMetrics(pEntry->iSystemMetric)) &&
                !(pEntry->pfnRestricted && pEntry->pfnRestricted()))
            {
                continue;
            }

            if (!m_apszPaths[i])
            {
                WCHAR szPath[MAX_PATH];

                if (SUCCEEDED(SHGetFolderPathEx(*pEntry->pFolder, KF_FLAG_DONT_VERIFY, NULL, szPath,
                                                _countof(szPath))))
                {
                    if (pEntry->pszSubPath)
                        PathAppendW(szPath, pEntry->pszSubPath);
                    Str_SetPtrW(&m_apszPaths[i], szPath);
                }
            }

            if (m_apszPaths[i] && pEntry->pfnMatch(pszPath, m_apszPaths[i]))
                return S_OK;
        }
        return S_FALSE;
    }

private:
    ~CStartMenuGroupPolicyFilter()
    {
        UINT i;

        for (i = 0; i < _countof(m_apszPaths); i++)
            Str_SetPtrW(&m_apszPaths[i], NULL);
    }

    LONG m_cRef;
    LPWSTR m_apszPaths[_countof(c_StartMenuRestrictions)];
};

HRESULT
CStartMenuGroupPolicyFilter_CreateInstance(
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    CStartMenuGroupPolicyFilter *pFilter = new CStartMenuGroupPolicyFilter();
    HRESULT hr;

    *ppv = NULL;
    if (!pFilter)
        return E_OUTOFMEMORY;

    hr = pFilter->QueryInterface(riid, ppv);
    pFilter->Release();
    return hr;
}

/* Shared part of the start menu filters: the locations that never show as items */
class CStartMenuCallbackBase : public CObjectWithSiteBase
{
public:
    CStartMenuCallbackBase() :
        m_cRef(1),
        m_pszPrograms(NULL),
        m_pszAdminTools(NULL),
        m_pszCommonPrograms(NULL),
        m_pszCommonAdminTools(NULL),
        m_fStartMenuAdminTools(FALSE),
        m_fPathsKnown(FALSE),
        m_pPolicyFilter(NULL)
    {
    }

    virtual ~CStartMenuCallbackBase()
    {
        Str_SetPtrW(&m_pszAdminTools, NULL);
        Str_SetPtrW(&m_pszCommonAdminTools, NULL);
        Str_SetPtrW(&m_pszPrograms, NULL);
        Str_SetPtrW(&m_pszCommonPrograms, NULL);
        if (m_pPolicyFilter)
        {
            m_pPolicyFilter->Release();
            m_pPolicyFilter = NULL;
        }
    }

protected:
    BOOL
    FilterPidl(
        _In_ IShellFolder *psf,
        _In_ PCUITEMID_CHILD pidl)
    {
        WCHAR szPath[MAX_PATH];

        if (FAILED(DisplayNameOfW(psf, pidl, SHGDN_FORPARSING, szPath, _countof(szPath))))
            return TRUE;

        if (m_pPolicyFilter &&
            m_pPolicyFilter->IsFileRestrictedByPolicyOrSystemMetrics(szPath) == S_OK)
        {
            return FALSE;
        }

        /* The Programs folders are merged into the list, they are not items of it */
        if ((m_pszPrograms && !lstrcmpiW(szPath, m_pszPrograms)) ||
            (m_pszCommonPrograms && !lstrcmpiW(szPath, m_pszCommonPrograms)))
        {
            return FALSE;
        }

        if (!m_fStartMenuAdminTools &&
            ((m_pszAdminTools && !lstrcmpiW(szPath, m_pszAdminTools)) ||
             (m_pszCommonAdminTools && !lstrcmpiW(szPath, m_pszCommonAdminTools))))
        {
            return FALSE;
        }
        return TRUE;
    }

    /* There is no cache of advertised installer shortcuts to look in */
    BOOL
    IsDarwinAdvertisement(
        _In_ PCIDLIST_ABSOLUTE pidl)
    {
        return FALSE;
    }

    LONG m_cRef;
    LPWSTR m_pszPrograms;
    LPWSTR m_pszAdminTools;
    LPWSTR m_pszCommonPrograms;
    LPWSTR m_pszCommonAdminTools;
    BOOL m_fStartMenuAdminTools;
    BOOL m_fPathsKnown;
    IStartMenuGroupPolicyFilter *m_pPolicyFilter;
};

class CPersonalProgramsMenuCallback :
    public CStartMenuCallbackBase,
    public IShellItemFilter,
    public IStartMenuMSIAds
{
public:
    CPersonalProgramsMenuCallback()
    {
        m_fStartMenuAdminTools = SHRegGetBoolUSValueW(
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            L"StartMenuAdminTools", FALSE, FALSE);
        CoCreateInstance(CLSID_StartMenuGroupPolicyFilter, NULL,
                         CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER,
                         IID_IStartMenuGroupPolicyFilter, (void **)&m_pPolicyFilter);
        SHReValidateDarwinCache();
    }

    // IUnknown
    STDMETHODIMP
    QueryInterface(
        _In_ REFIID riid,
        _Outptr_ void **ppv) override
    {
        static const QITAB qit[] =
        {
            QITABENT(CPersonalProgramsMenuCallback, IShellItemFilter),
            QITABENT(CPersonalProgramsMenuCallback, IObjectWithSite),
            QITABENT(CPersonalProgramsMenuCallback, IStartMenuMSIAds),
            { NULL, 0 }
        };

        return QISearch(this, qit, riid, ppv);
    }

    STDMETHODIMP_(ULONG)
    AddRef() override
    {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG)
    Release() override
    {
        LONG cRef = InterlockedDecrement(&m_cRef);

        if (!cRef)
            delete this;
        return cRef;
    }

    // IShellItemFilter
    STDMETHODIMP
    IncludeItem(
        _In_ IShellItem *psi) override;

    STDMETHODIMP
    GetEnumFlagsForItem(
        _In_ IShellItem *psi,
        _Out_ SHCONTF *pgrfFlags) override
    {
        return E_NOTIMPL;
    }

    // IStartMenuMSIAds
    STDMETHODIMP
    IsMSIAds(
        _In_ PCIDLIST_ABSOLUTE pidl) override
    {
        return IsDarwinAdvertisement(pidl) ? S_OK : S_FALSE;
    }
};

STDMETHODIMP
CPersonalProgramsMenuCallback::IncludeItem(
    _In_ IShellItem *psi)
{
    IParentAndItem *ppai;
    HRESULT hr;

    /* The locations are read once, when the first item is asked about */
    if (!m_fPathsKnown)
    {
        WCHAR szPath[MAX_PATH];

        szPath[0] = UNICODE_NULL;
        SHGetFolderPathEx(FOLDERID_Programs, 0, NULL, szPath, _countof(szPath));
        Str_SetPtrW(&m_pszPrograms, szPath);

        szPath[0] = UNICODE_NULL;
        SHGetFolderPathEx(FOLDERID_CommonPrograms, 0, NULL, szPath, _countof(szPath));
        Str_SetPtrW(&m_pszCommonPrograms, szPath);

        szPath[0] = UNICODE_NULL;
        SHGetFolderPathEx(FOLDERID_CommonAdminTools, 0, NULL, szPath, _countof(szPath));
        Str_SetPtrW(&m_pszCommonAdminTools, szPath);

        szPath[0] = UNICODE_NULL;
        SHGetFolderPathEx(FOLDERID_AdminTools, 0, NULL, szPath, _countof(szPath));
        Str_SetPtrW(&m_pszAdminTools, szPath);

        m_fPathsKnown = TRUE;
    }

    hr = psi->QueryInterface(IID_PPV_ARG(IParentAndItem, &ppai));
    if (SUCCEEDED(hr))
    {
        IShellFolder *psf;
        PITEMID_CHILD pidl;

        hr = ppai->GetParentAndItem(NULL, &psf, &pidl);
        if (SUCCEEDED(hr))
        {
            if (!FilterPidl(psf, pidl))
                hr = S_FALSE;
            psf->Release();
            ILFree(pidl);
        }
        ppai->Release();
    }
    return hr;
}

HRESULT
CPersonalStartMenu_CreateInstance(
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    CPersonalProgramsMenuCallback *pCallback = new CPersonalProgramsMenuCallback();
    HRESULT hr;

    *ppv = NULL;
    if (!pCallback)
        return E_OUTOFMEMORY;

    hr = pCallback->QueryInterface(riid, ppv);
    pCallback->Release();
    return hr;
}
