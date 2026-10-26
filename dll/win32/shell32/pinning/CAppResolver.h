/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Start menu program cache and application resolver
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "pinning.h"

class CStartMenuCacheAndAppResolver :
    public CPinZeroAlloc,
    public CComCoClass<CStartMenuCacheAndAppResolver, &CLSID_StartMenuCacheAndAppResolver>,
    public CComObjectRootEx<CComMultiThreadModel>,
    public IStartMenuItemsCache,
    public IApplicationResolver
{
public:
    CStartMenuCacheAndAppResolver();
    virtual ~CStartMenuCacheAndAppResolver();

    HRESULT FinalConstruct();

    // *** IStartMenuItemsCache methods ***
    STDMETHODIMP OnChangeNotify(UINT uId, LONG lEvent, PCIDLIST_ABSOLUTE pidl1, PCIDLIST_ABSOLUTE pidl2) override;
    STDMETHODIMP PinListChanged() override;
    STDMETHODIMP GetPinnedItemsCount(int *pcPinned) override;
    STDMETHODIMP GetStartMenuMFUList(UINT cMax, IEnumStartMenuItems **ppenumItems, IEnumString **ppenumNewApps,
                                     FILETIME *pftNewest) override;
    STDMETHODIMP RegisterSMNotify(IStartMenuItemsCacheNotify *pNotify) override;
    STDMETHODIMP RegisterARNotify(IAppResolverNotify *pNotify) override;
    STDMETHODIMP SetAltName(PCUIDLIST_RELATIVE pidl, PCWSTR pszName, PIDLIST_RELATIVE *ppidlNew) override;
    STDMETHODIMP GetAltName(PCUIDLIST_RELATIVE pidl, PWSTR *ppszName) override;

    // *** IApplicationResolver methods ***
    STDMETHODIMP GetAppIDForShortcut(IShellItem *psi, PWSTR *ppszAppID) override;
    STDMETHODIMP GetAppIDForWindow(HWND hwnd, PWSTR *ppszAppID, BOOL *pfPinningPrevented, BOOL *pfExplicitAppID,
                                   BOOL *pfEmbeddedShortcutValid) override;
    STDMETHODIMP GetAppIDForProcess(DWORD dwProcessId, PWSTR *ppszAppID, BOOL *pfPinningPrevented,
                                    BOOL *pfExplicitAppID, BOOL *pfEmbeddedShortcutValid) override;
    STDMETHODIMP GetShortcutForProcess(DWORD dwProcessId, IShellItem **ppsi) override;
    STDMETHODIMP GetBestShortcutForAppID(PCWSTR pszAppID, IShellItem **ppsi) override;
    STDMETHODIMP GetBestShortcutAndAppIDForAppPath(PCWSTR pszAppPath, IShellItem **ppsi, PWSTR *ppszAppID) override;
    STDMETHODIMP CanPinApp(IShellItem *psi) override;
    STDMETHODIMP GetRelaunchProperties(HWND hwnd, PWSTR *ppszAppID, PWSTR *ppszCmdLine, PWSTR *ppszIconResource,
                                       PWSTR *ppszDisplayNameResource, BOOL *pfPinnable) override;
    STDMETHODIMP GenerateShortcutFromWindowProperties(HWND hwnd, IShellItem **ppsi) override;
    STDMETHODIMP GenerateShortcutFromItemProperties(IShellItem2 *psi, IShellItem **ppsi) override;

    DECLARE_REGISTRY_RESOURCEID(IDR_STARTMENUCACHEANDAPPRESOLVER)
    DECLARE_NOT_AGGREGATABLE(CStartMenuCacheAndAppResolver)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CStartMenuCacheAndAppResolver)
        COM_INTERFACE_ENTRY_IID(IID_IStartMenuItemsCache, IStartMenuItemsCache)
        COM_INTERFACE_ENTRY_IID(IID_IApplicationResolver, IApplicationResolver)
    END_COM_MAP()

private:
    HRESULT GetShortcutPathOrAppIDFromPid(
        _In_ DWORD dwProcessId,
        _Out_writes_(MAX_PATH) PWSTR pszValue,
        _Out_ BOOL *pfAppID,
        _Out_opt_ BOOL *pfPinningPrevented);
    HRESULT GetBestAppIDForProcess(
        _In_ PCWSTR pszExePath,
        _In_ HANDLE hProcess,
        _Outptr_ PWSTR *ppszAppID);
    HRESULT GetRelaunchPropertiesFromStore(
        _In_ IPropertyStore *pps,
        _Outptr_opt_ PWSTR *ppszAppID,
        _Outptr_opt_ PWSTR *ppszCmdLine,
        _Outptr_opt_ PWSTR *ppszIconResource,
        _Outptr_opt_ PWSTR *ppszDisplayNameResource,
        _Out_opt_ BOOL *pfPinnable);
    HRESULT CreateShortcutFromProperties(
        _In_ PCWSTR pszAppID,
        _In_ PCWSTR pszCmdLine,
        _In_opt_ PCWSTR pszIconResource,
        _In_ PCWSTR pszDisplayNameResource,
        _Outptr_ IShellItem **ppsi);
    BOOL IsShortcutInteresting(
        _In_ IShellItem *psi,
        _In_ PCWSTR pszTarget,
        _In_ BOOL fHasArgs);

    IPinnedList *m_pplStartMenu;
    IPinnedList *m_pplTaskband;
};
