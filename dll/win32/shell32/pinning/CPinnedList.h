/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Start menu and taskbar pinned lists
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "pinning.h"

/* Version the pin stream is written with; older items get their application IDs again */
#define PIN_STREAM_VERSION      2

/* One pinned item while the list is loaded */
typedef struct _PIN_ITEM
{
    PIDLIST_ABSOLUTE pidl;
    IShellLinkW *psl;           /* Link to save for a new or changed item */
    ULARGE_INTEGER uliLinkPos;  /* Where the saved link starts in the old links stream */
    DWORD cbLink;               /* Size of the saved link, 0 when there is none */
} PIN_ITEM, *PPIN_ITEM;

class CPinnedListBase;

/* The pinned items of one list with the links that resolve them */
class CPinStore
{
public:
    CPinStore();
    ~CPinStore();

    HRESULT Load(
        _In_ CPinnedListBase *pList);
    HRESULT Save(
        _In_ CPinnedListBase *pList);

    PPIN_ITEM FindPidl(
        _In_ PCIDLIST_ABSOLUTE pidl,
        _Out_opt_ int *piIndex);
    PPIN_ITEM FindPidlOrAppID(
        _In_ PCIDLIST_ABSOLUTE pidl,
        _Out_opt_ int *piIndex);
    HRESULT ReplacePidl(
        _In_ CPinnedListBase *pList,
        _In_ PCIDLIST_ABSOLUTE pidlFrom,
        _In_opt_ PCIDLIST_ABSOLUTE pidlTo,
        _Outptr_opt_ PIDLIST_ABSOLUTE *ppidlOld);
    int AppendPidl(
        _In_ PCIDLIST_ABSOLUTE pidl);
    HRESULT LoadShellLink(
        _In_ PPIN_ITEM pItem,
        _Outptr_ IShellLinkW **ppsl);

    PPIN_ITEM GetItem(
        _In_ int iIndex)
    {
        return (PPIN_ITEM)DSA_GetItemPtr(m_hdsa, iIndex);
    }

    int GetCount() const
    {
        return m_hdsa ? DSA_GetItemCount(m_hdsa) : 0;
    }

    BOOL IsValid() const
    {
        return m_hdsa != NULL;
    }

private:
    friend struct PIN_SAVE_CONTEXT;

    HDSA m_hdsa;
    IStream *m_pstmLinks;
};

HRESULT
PinItemUpdateShellLink(
    _Inout_ PPIN_ITEM pItem);

/* The shortcut copies a pinned list keeps under User Pinned */
class CPinLinkFolder
{
public:
    CPinLinkFolder() : m_pszSubDir(NULL) {}
    ~CPinLinkFolder() { CoTaskMemFree(m_pszSubDir); }

    HRESULT Initialize(
        _In_ PCWSTR pszSubDir)
    {
        return SHStrDupW(pszSubDir, &m_pszSubDir);
    }

    HRESULT CreateBackupLink(
        _In_ PCIDLIST_ABSOLUTE pidl,
        _Outptr_ PIDLIST_ABSOLUTE *ppidlLink);
    HRESULT IsBackupLink(
        _In_ PCIDLIST_ABSOLUTE pidl);

private:
    HRESULT GetBackupPath(
        _Outptr_ PWSTR *ppszPath);
    HRESULT CopyShortcut(
        _In_ PCWSTR pszSource,
        _In_ PCWSTR pszFolder,
        _Outptr_ PIDLIST_ABSOLUTE *ppidl);
    HRESULT CreateShortcut(
        _In_ PCIDLIST_ABSOLUTE pidlTarget,
        _In_ PCWSTR pszTarget,
        _In_ SFGAOF sfgaoTarget,
        _In_ PCWSTR pszLink,
        _In_opt_ PCWSTR pszAppID,
        _Outptr_ PIDLIST_ABSOLUTE *ppidl);
    HRESULT GetPinnedFolderIDList(
        _In_ PCWSTR pszLink,
        _Outptr_ PIDLIST_ABSOLUTE *ppidl);

    PWSTR m_pszSubDir;
};

class CPinnedListBase :
    public CPinZeroAlloc,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IShellExtInit,
    public IContextMenu,
    public IPinnedList,
    public CObjectWithSiteBase
{
public:
    CPinnedListBase();
    virtual ~CPinnedListBase();

    /* What a concrete list stores and how it tells others about changes */
    virtual HRESULT SetChangeCount(
        _In_ UINT cChanges) = 0;
    virtual IStream *OpenPinStream(
        _In_ DWORD grfMode) = 0;
    virtual IStream *OpenLinksStream(
        _In_ DWORD grfMode) = 0;
    virtual VOID NotifyPinListChange(
        _In_opt_ PCIDLIST_ABSOLUTE pidl,
        _In_ BOOL fRemoved) = 0;
    virtual UINT GetPinStreamVersion() = 0;
    virtual HRESULT SetPinStreamVersion(
        _In_ UINT uVersion) = 0;
    virtual UINT GetCurrentPinStreamVersion()
    {
        return PIN_STREAM_VERSION;
    }
    virtual HRESULT UpgradeItem(
        _In_ UINT uVersion,
        _In_ PIDLIST_ABSOLUTE pidl,
        _Outptr_ PIDLIST_ABSOLUTE *ppidl);
    virtual HRESULT GetBackupSubDirectoryName(
        _Out_writes_(cch) PWSTR pszName,
        _In_ UINT cch);
    virtual BOOL IsAcceptableTarget(
        _In_ PCIDLIST_ABSOLUTE pidl,
        _In_ DWORD dwFlags,
        _Outptr_opt_ IShellItem **ppsiApplication,
        _Outptr_opt_ PWSTR *ppszAppID,
        _Outptr_ PWSTR *ppszAppPath,
        _Outptr_opt_ IShellItem **ppsiDestination);
    virtual BOOL IsRestricted() = 0;
    virtual BOOL IsMenuHidden()
    {
        return FALSE;
    }
    virtual HRESULT GetMenuStringID(
        _Out_ UINT *puID) = 0;
    virtual int GetHelpText(
        _In_ UINT uVerb,
        _Out_writes_(cch) PWSTR pszText,
        _In_ int cch) = 0;
    virtual HRESULT ReadChangeCount(
        _Out_ UINT *pcChanges) = 0;
    virtual PCWSTR GetVerb(
        _In_ UINT uVerb) = 0;
    virtual HRESULT SetRemovedChangeCount(
        _In_ UINT cChanges) = 0;
    virtual UINT GetRemovedChangeCount() = 0;

    static BOOL IsLocalHardDisk(
        _In_ PCWSTR pszPath);
    static BOOL GetDarwinLinkInfo(
        _In_ IShellLinkW *psl,
        _Out_opt_ BOOL *pfInstalled,
        _Out_writes_opt_(cchProduct) PWSTR pszProduct,
        _In_ UINT cchProduct);
    static HRESULT RemoveDarwinShortcut(
        _In_ IPinnedList *ppl,
        _In_ PCWSTR pszProduct);
    static UINT GetRemovedGlobalChangeCount();

    // *** IShellExtInit methods ***
    STDMETHODIMP Initialize(PCIDLIST_ABSOLUTE pidlFolder, IDataObject *pdtobj, HKEY hkeyProgID) override;

    // *** IContextMenu methods ***
    STDMETHODIMP QueryContextMenu(HMENU hmenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast, UINT uFlags) override;
    STDMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO pici) override;
    STDMETHODIMP GetCommandString(UINT_PTR idCmd, UINT uType, UINT *pReserved, LPSTR pszName, UINT cchMax) override;

    // *** IPinnedList methods ***
    STDMETHODIMP EnumObjects(IEnumFullIDList **ppenum) override;
    STDMETHODIMP Modify(PCIDLIST_ABSOLUTE pidlFrom, PCIDLIST_ABSOLUTE pidlTo) override;
    STDMETHODIMP GetChangeCount(UINT *pcChanges) override;
    STDMETHODIMP GetPinnableInfo(IDataObject *pdo, DWORD dwFlags, IShellItem **ppsiApplication,
                                 IShellItem **ppsiDestination, PWSTR *ppszAppID, BOOL *pfPinnable) override;
    STDMETHODIMP IsPinnable(IDataObject *pdo, DWORD dwFlags) override;
    STDMETHODIMP Resolve(HWND hwnd, DWORD dwFlags, PCIDLIST_ABSOLUTE pidl, PIDLIST_ABSOLUTE *ppidlResolved) override;
    STDMETHODIMP IsPinned(PCIDLIST_ABSOLUTE pidl) override;
    STDMETHODIMP GetPinnedItem(PCIDLIST_ABSOLUTE pidl, PIDLIST_ABSOLUTE *ppidlPinned) override;
    STDMETHODIMP GetAppIDForPinnedItem(PCIDLIST_ABSOLUTE pidl, PWSTR *ppszAppID) override;
    STDMETHODIMP ItemChangeNotify(PCIDLIST_ABSOLUTE pidlFrom, PCIDLIST_ABSOLUTE pidlTo) override;
    STDMETHODIMP UpdateForRemovedItemsAsNecessary() override;

protected:
    HRESULT ShouldAddMenu(
        _In_ UINT uFlags);
    HRESULT ResolveTargetLink(
        _In_ PCIDLIST_ABSOLUTE pidl,
        _Inout_ BOOL *pfRemoved);

    BOOL m_fSuppressNotify;     /* Set while stale items are pruned in one go */
    IDataObject *m_pdtobj;
    UINT m_uVerb;               /* 0 when the menu offers to pin, 1 to unpin */
    PIDLIST_ABSOLUTE m_pidlMenuItem;
};

class CStartMenuPin :
    public CPinnedListBase,
    public CComCoClass<CStartMenuPin, &CLSID_StartMenuPin>,
    public IStartMenuPinnedList
{
public:
    DECLARE_REGISTRY_RESOURCEID(IDR_STARTMENUPIN)
    DECLARE_AGGREGATABLE(CStartMenuPin)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    HRESULT SetChangeCount(UINT cChanges) override;
    IStream *OpenPinStream(DWORD grfMode) override;
    IStream *OpenLinksStream(DWORD grfMode) override;
    VOID NotifyPinListChange(PCIDLIST_ABSOLUTE pidl, BOOL fRemoved) override;
    UINT GetPinStreamVersion() override;
    HRESULT SetPinStreamVersion(UINT uVersion) override;
    HRESULT GetBackupSubDirectoryName(PWSTR pszName, UINT cch) override;
    BOOL IsRestricted() override;
    HRESULT GetMenuStringID(UINT *puID) override;
    int GetHelpText(UINT uVerb, PWSTR pszText, int cch) override;
    HRESULT ReadChangeCount(UINT *pcChanges) override;
    PCWSTR GetVerb(UINT uVerb) override;
    HRESULT SetRemovedChangeCount(UINT cChanges) override;
    UINT GetRemovedChangeCount() override;

    // *** IStartMenuPinnedList methods ***
    STDMETHODIMP RemoveFromList(IShellItem *psi) override;

    BEGIN_COM_MAP(CStartMenuPin)
        COM_INTERFACE_ENTRY_IID(IID_IShellExtInit, IShellExtInit)
        COM_INTERFACE_ENTRY_IID(IID_IContextMenu, IContextMenu)
        COM_INTERFACE_ENTRY_IID(IID_IPinnedList, IPinnedList)
        COM_INTERFACE_ENTRY_IID(IID_IObjectWithSite, IObjectWithSite)
        COM_INTERFACE_ENTRY_IID(IID_IStartMenuPinnedList, IStartMenuPinnedList)
    END_COM_MAP()
};

class CTaskbandPin :
    public CPinnedListBase,
    public CComCoClass<CTaskbandPin, &CLSID_TaskbandPin>
{
public:
    DECLARE_REGISTRY_RESOURCEID(IDR_TASKBANDPIN)
    DECLARE_AGGREGATABLE(CTaskbandPin)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    HRESULT SetChangeCount(UINT cChanges) override;
    IStream *OpenPinStream(DWORD grfMode) override;
    IStream *OpenLinksStream(DWORD grfMode) override;
    VOID NotifyPinListChange(PCIDLIST_ABSOLUTE pidl, BOOL fRemoved) override;
    UINT GetPinStreamVersion() override;
    HRESULT SetPinStreamVersion(UINT uVersion) override;
    BOOL IsAcceptableTarget(PCIDLIST_ABSOLUTE pidl, DWORD dwFlags, IShellItem **ppsiApplication,
                            PWSTR *ppszAppID, PWSTR *ppszAppPath, IShellItem **ppsiDestination) override;
    BOOL IsRestricted() override;
    HRESULT GetMenuStringID(UINT *puID) override;
    int GetHelpText(UINT uVerb, PWSTR pszText, int cch) override;
    HRESULT ReadChangeCount(UINT *pcChanges) override;
    PCWSTR GetVerb(UINT uVerb) override;
    HRESULT SetRemovedChangeCount(UINT cChanges) override;
    UINT GetRemovedChangeCount() override;

    BEGIN_COM_MAP(CTaskbandPin)
        COM_INTERFACE_ENTRY_IID(IID_IShellExtInit, IShellExtInit)
        COM_INTERFACE_ENTRY_IID(IID_IContextMenu, IContextMenu)
        COM_INTERFACE_ENTRY_IID(IID_IPinnedList, IPinnedList)
        COM_INTERFACE_ENTRY_IID(IID_IObjectWithSite, IObjectWithSite)
    END_COM_MAP()

private:
    HRESULT GetApplication(
        _In_ IApplicationResolver *par,
        _In_ PCWSTR pszPath,
        _Outptr_opt_ IShellItem **ppsiApplication,
        _Outptr_opt_ PWSTR *ppszAppID,
        _Outptr_opt_ PWSTR *ppszAppPath);
    HRESULT ProcessDestinationFile(
        _In_ IApplicationResolver *par,
        _In_ PCIDLIST_ABSOLUTE pidl,
        _In_ SFGAOF sfgao,
        _Outptr_opt_ IShellItem **ppsiApplication,
        _Outptr_opt_ PWSTR *ppszAppID,
        _Outptr_ PWSTR *ppszAppPath);
    HRESULT ProcessDestinationFolder(
        _In_ IApplicationResolver *par,
        _In_ PCIDLIST_ABSOLUTE pidl,
        _Outptr_opt_ IShellItem **ppsiApplication,
        _Outptr_opt_ PWSTR *ppszAppID);
    BOOL IsAllowedWhilePinningDisabled(
        _In_opt_ IShellItem *psiApplication);
};
