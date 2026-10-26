/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Start menu and taskbar pinned lists
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include "CPinnedList.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

#define STARTPAGE2_KEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartPage2"
#define TASKBAND_KEY        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband"
#define STARTPAGE_KEY       L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartPage"

/* Extended change events the start menu and taskbar listen for */
#define PIN_EVENT_STARTMENU     10
#define PIN_EVENT_TASKBAND      13
#define PIN_EVENT_REMOVED       14

/* Resolve without UI and write back what changed */
#define PIN_RESOLVE_QUIET       (SLR_NO_UI | SLR_UPDATE)

#include <pshpack1.h>
typedef struct _PIN_EVENT_IDLIST
{
    USHORT cb;
    DWORD dwEvent;
    DWORD dwParam;
    USHORT cbTerminator;
} PIN_EVENT_IDLIST;
#include <poppack.h>

static
VOID
SendPinEvent(
    _In_ DWORD dwEvent,
    _In_ UINT uFlags)
{
    PIN_EVENT_IDLIST event = { FIELD_OFFSET(PIN_EVENT_IDLIST, cbTerminator), dwEvent, 0, 0 };

    SHChangeNotify(SHCNE_EXTENDED_EVENT, uFlags, &event, NULL);
}

static
UINT
ReadKeyDword(
    _In_ HKEY hkeyRoot,
    _In_ PCWSTR pszKey,
    _In_ PCWSTR pszValue)
{
    DWORD dwValue;

    if (RegQueryDword(hkeyRoot, pszKey, pszValue, &dwValue) != ERROR_SUCCESS)
        return 0;
    return dwValue;
}

static
HRESULT
WriteKeyDword(
    _In_ PCWSTR pszKey,
    _In_ PCWSTR pszValue,
    _In_ DWORD dwValue)
{
    DWORD dwError = SHSetValueW(HKEY_CURRENT_USER, pszKey, pszValue, REG_DWORD, &dwValue, sizeof(dwValue));
    return HRESULT_FROM_WIN32(dwError);
}

/* Enumerates a list straight from its pin stream */
class CPinEnum :
    public CPinZeroAlloc,
    public IEnumFullIDList
{
public:
    CPinEnum(
        _In_ CPinnedListBase *pList) :
        m_cRef(1),
        m_pList(pList),
        m_hrDone(S_OK),
        m_pstm(NULL),
        m_fUpgrade(FALSE),
        m_uVersion(0)
    {
        static_cast<IPinnedList *>(m_pList)->AddRef();
    }

    virtual ~CPinEnum()
    {
        static_cast<IPinnedList *>(m_pList)->Release();
        if (m_pstm)
            m_pstm->Release();
    }

    // *** IUnknown methods ***
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumFullIDList))
        {
            *ppv = static_cast<IEnumFullIDList *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        LONG cRef = InterlockedDecrement(&m_cRef);
        if (!cRef)
            delete this;
        return cRef;
    }

    // *** IEnumFullIDList methods ***
    STDMETHODIMP Next(ULONG celt, PIDLIST_ABSOLUTE *rgelt, ULONG *pceltFetched) override;

    STDMETHODIMP Skip(ULONG celt) override
    {
        return E_NOTIMPL;
    }

    STDMETHODIMP Reset() override
    {
        m_hrDone = S_OK;
        ReleaseStream();
        return S_OK;
    }

    STDMETHODIMP Clone(IEnumFullIDList **ppenum) override
    {
        *ppenum = NULL;
        return E_NOTIMPL;
    }

private:
    HRESULT ReadNext(
        _Outptr_ PIDLIST_ABSOLUTE *ppidl);

    VOID ReleaseStream()
    {
        if (m_pstm)
        {
            m_pstm->Release();
            m_pstm = NULL;
        }
    }

    LONG m_cRef;
    CPinnedListBase *m_pList;
    HRESULT m_hrDone;       /* Sticks once the stream ran out or failed */
    IStream *m_pstm;
    BOOL m_fUpgrade;        /* The stream predates the current format */
    UINT m_uVersion;
};

HRESULT
CPinEnum::ReadNext(
    _Outptr_ PIDLIST_ABSOLUTE *ppidl)
{
    PIDLIST_ABSOLUTE pidlFolder;
    LPITEMIDLIST pidlRelative;
    BYTE bCsidl;
    HRESULT hr;

    hr = IStream_Read(m_pstm, &bCsidl, sizeof(bCsidl));
    if (FAILED(hr))
        return hr;
    if (bCsidl == 0xFF)
        return S_FALSE;

    hr = SHGetFolderLocation(NULL, bCsidl, NULL, 0, &pidlFolder);
    if (hr == S_FALSE)
        hr = E_FAIL;
    if (FAILED(hr))
        return hr;

    hr = IStream_ReadPidl(m_pstm, &pidlRelative);
    if (SUCCEEDED(hr))
    {
        *ppidl = ILCombine(pidlFolder, pidlRelative);
        hr = *ppidl ? S_OK : E_OUTOFMEMORY;
        if (SUCCEEDED(hr) && m_fUpgrade)
            hr = m_pList->UpgradeItem(m_uVersion, *ppidl, ppidl);
        ILFree(pidlRelative);
    }
    ILFree(pidlFolder);
    return hr;
}

STDMETHODIMP
CPinEnum::Next(
    _In_ ULONG celt,
    _Out_writes_to_(celt, *pceltFetched) PIDLIST_ABSOLUTE *rgelt,
    _Out_opt_ ULONG *pceltFetched)
{
    HRESULT hr;

    UNREFERENCED_PARAMETER(celt);

    if (pceltFetched)
        *pceltFetched = 0;
    if (m_hrDone != S_OK)
        return m_hrDone;

    if (!m_pstm)
    {
        m_pstm = m_pList->OpenPinStream(STGM_READ);
        if (m_pstm)
        {
            if (SHIsEmptyStream(m_pstm))
            {
                ReleaseStream();
            }
            else
            {
                m_uVersion = m_pList->GetPinStreamVersion();
                m_fUpgrade = (m_uVersion < m_pList->GetCurrentPinStreamVersion());
            }
        }
    }

    if (m_pstm)
    {
        rgelt[0] = NULL;
        hr = ReadNext(&rgelt[0]);
    }
    else
    {
        hr = S_FALSE;
    }

    if (pceltFetched)
        *pceltFetched = (hr == S_OK);
    m_hrDone = hr;
    if (hr != S_OK)
        ReleaseStream();
    return hr;
}

CPinnedListBase::CPinnedListBase() :
    m_fSuppressNotify(FALSE),
    m_pdtobj(NULL),
    m_uVerb(0),
    m_pidlMenuItem(NULL)
{
}

CPinnedListBase::~CPinnedListBase()
{
    ILFree(m_pidlMenuItem);
    if (m_pdtobj)
        m_pdtobj->Release();
}

/* Items written before application IDs were kept get theirs now */
HRESULT
CPinnedListBase::UpgradeItem(
    _In_ UINT uVersion,
    _In_ PIDLIST_ABSOLUTE pidl,
    _Outptr_ PIDLIST_ABSOLUTE *ppidl)
{
    if (uVersion && ILRemoveHiddenID(pidl, HIDDENID_APPID))
        ILExpungeRemovedHiddenIDs(pidl);
    return CacheAppIDInIDList(pidl, ppidl);
}

HRESULT
CPinnedListBase::GetBackupSubDirectoryName(
    _Out_writes_(cch) PWSTR pszName,
    _In_ UINT cch)
{
    return StringCchCopyW(pszName, cch, L"TaskBar");
}

/* Paths on a fixed disk, or in the folders the start menu reads anyway, are quick to reach */
BOOL
CPinnedListBase::IsLocalHardDisk(
    _In_ PCWSTR pszPath)
{
    static const KNOWNFOLDERID *const s_LocalFolders[] =
    {
        &FOLDERID_Programs,
        &FOLDERID_Desktop,
        &FOLDERID_QuickLaunch,
    };
    int iDrive = PathGetDriveNumberW(pszPath);

    if (iDrive >= 0 && RealDriveType(iDrive, FALSE) == DRIVE_FIXED)
        return TRUE;

    for (UINT i = 0; i < _countof(s_LocalFolders); i++)
    {
        PWSTR pszFolder;
        BOOL fInside;

        if (FAILED(SHGetKnownFolderPath(*s_LocalFolders[i], KF_FLAG_DONT_VERIFY | KF_FLAG_NO_ALIAS, NULL, &pszFolder)))
            continue;
        fInside = (PathComparePaths(pszFolder, pszPath) & PCP_CHILD) != 0;
        CoTaskMemFree(pszFolder);
        if (fInside)
            return TRUE;
    }
    return FALSE;
}

BOOL
CPinnedListBase::IsAcceptableTarget(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_ DWORD dwFlags,
    _Outptr_opt_ IShellItem **ppsiApplication,
    _Outptr_opt_ PWSTR *ppszAppID,
    _Outptr_ PWSTR *ppszAppPath,
    _Outptr_opt_ IShellItem **ppsiDestination)
{
    CComPtr<IShellItem> psi;
    WCHAR szPath[MAX_PATH];
    WCHAR szTarget[MAX_PATH] = L"";
    DWORD sfgao = SFGAO_FILESYSTEM | SFGAO_LINK;
    BOOL fLink;

    if (ppsiDestination)
        *ppsiDestination = NULL;
    if (ppszAppID)
        *ppszAppID = NULL;
    if (ppsiApplication)
        *ppsiApplication = NULL;
    *ppszAppPath = NULL;

    /* An item that cannot even be named is not rejected here */
    if (FAILED(SHGetNameAndFlagsW(pidl, SHGDN_FORPARSING, szPath, _countof(szPath), &sfgao)))
        return TRUE;

    if (!(sfgao & SFGAO_FILESYSTEM))
        return !(dwFlags & PINNABLE_EXEONLY);

    if (FAILED(SHCreateItemFromIDList(pidl, IID_PPV_ARG(IShellItem, &psi))))
        return FALSE;

    fLink = (sfgao & SFGAO_LINK) && SUCCEEDED(GetAppPathFromLink(psi, szTarget, _countof(szTarget)));

    if (dwFlags & PINNABLE_EXEONLY)
    {
        PCWSTR pszExt = PathFindExtensionW(fLink ? szTarget : szPath);
        if (_wcsicmp(pszExt, L".EXE") && _wcsicmp(pszExt, L".MSC"))
            return FALSE;
    }

    if (dwFlags & PINNABLE_REJECTSLOWMEDIA)
    {
        BOOL fShellTarget = (szTarget[0] == L':' && szTarget[1] == L':' && szTarget[2] == L'{' && szTarget[3]);

        if (!IsLocalHardDisk(szPath))
            return FALSE;
        if (fLink && !fShellTarget && !IsLocalHardDisk(szTarget) && !PathIsURLW(szTarget))
            return FALSE;
    }

    if (dwFlags & PINNABLE_APPLICATION_ONLY)
    {
        CComPtr<IApplicationResolver> par;

        if (FAILED(CoCreateInstance(CLSID_StartMenuCacheAndAppResolver, NULL, CLSCTX_INPROC,
                                    IID_PPV_ARG(IApplicationResolver, &par))))
        {
            return FALSE;
        }
        return SUCCEEDED(par->CanPinApp(psi));
    }
    return TRUE;
}

UINT
CPinnedListBase::GetRemovedGlobalChangeCount()
{
    return ReadKeyDword(HKEY_LOCAL_MACHINE, STARTPAGE_KEY, L"FavoritesRemovedChanges");
}

BOOL
CPinnedListBase::GetDarwinLinkInfo(
    _In_ IShellLinkW *psl,
    _Out_opt_ BOOL *pfInstalled,
    _Out_writes_opt_(cchProduct) PWSTR pszProduct,
    _In_ UINT cchProduct)
{
    CComPtr<IShellLinkDataList> pdl;
    DWORD dwFlags = 0;

    if (pszProduct && cchProduct)
        *pszProduct = UNICODE_NULL;
    if (pfInstalled)
        *pfInstalled = FALSE;

    if (FAILED(psl->QueryInterface(IID_PPV_ARG(IShellLinkDataList, &pdl))) ||
        FAILED(pdl->GetFlags(&dwFlags)) || !(dwFlags & SLDF_HAS_DARWINID))
    {
        return FALSE;
    }

    EXP_DARWIN_LINK *pDarwin;
    if (SUCCEEDED(pdl->CopyDataBlock(EXP_DARWIN_ID_SIG, (void **)&pDarwin)))
    {
        /* Without installer support every advertised product counts as present */
        if (pfInstalled)
            *pfInstalled = TRUE;
        if (pszProduct)
            StringCchCopyW(pszProduct, cchProduct, pDarwin->szwDarwinID);
        LocalFree(pDarwin);
    }
    return TRUE;
}

HRESULT
CPinnedListBase::RemoveDarwinShortcut(
    _In_ IPinnedList *ppl,
    _In_ PCWSTR pszProduct)
{
    CComPtr<IEnumFullIDList> penum;
    PIDLIST_ABSOLUTE pidlFound = NULL;
    PIDLIST_ABSOLUTE pidl;
    HRESULT hr;

    hr = ppl->EnumObjects(&penum);
    if (FAILED(hr))
        return hr;

    while (!pidlFound && penum->Next(1, &pidl, NULL) == S_OK)
    {
        CComPtr<IShellItem> psi;
        CComPtr<IShellLinkW> psl;
        WCHAR szProduct[MAX_PATH];

        if (SUCCEEDED(SHCreateItemFromIDList(pidl, IID_PPV_ARG(IShellItem, &psi))) &&
            SUCCEEDED(psi->BindToHandler(NULL, BHID_SFUIObject, IID_PPV_ARG(IShellLinkW, &psl))) &&
            GetDarwinLinkInfo(psl, NULL, szProduct, _countof(szProduct)) &&
            CompareStringOrdinal(pszProduct, -1, szProduct, -1, TRUE) == CSTR_EQUAL)
        {
            pidlFound = pidl;
            pidl = NULL;
        }
        ILFree(pidl);
    }

    if (!pidlFound)
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

    hr = ppl->Modify(pidlFound, NULL);
    ILFree(pidlFound);
    return hr;
}

/* Removes a pinned shortcut whose target is gone for good */
HRESULT
CPinnedListBase::ResolveTargetLink(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _Inout_ BOOL *pfRemoved)
{
    CComPtr<IShellItem> psi;
    CComPtr<IShellLinkW> psl;
    BOOL fInstalled;
    HRESULT hr;

    hr = SHCreateItemFromIDList(pidl, IID_PPV_ARG(IShellItem, &psi));
    if (FAILED(hr))
        return hr;

    if (FAILED(psi->BindToHandler(NULL, BHID_SFUIObject, IID_PPV_ARG(IShellLinkW, &psl))))
        return hr;

    if (GetDarwinLinkInfo(psl, &fInstalled, NULL, 0))
        hr = fInstalled ? S_OK : E_FAIL;
    else
        hr = psl->Resolve(NULL, PIN_RESOLVE_QUIET);

    if ((FAILED(hr) && hr != E_NOTIMPL) || hr == S_FALSE)
    {
        hr = Modify(pidl, NULL);
        if (SUCCEEDED(hr))
            *pfRemoved = TRUE;
    }
    return hr;
}

STDMETHODIMP
CPinnedListBase::Initialize(
    _In_opt_ PCIDLIST_ABSOLUTE pidlFolder,
    _In_opt_ IDataObject *pdtobj,
    _In_opt_ HKEY hkeyProgID)
{
    IUnknown_Set((IUnknown **)&m_pdtobj, pdtobj);
    return S_OK;
}

/* S_OK when the item the menu is for can be pinned; m_pidlMenuItem then names what to pin */
HRESULT
CPinnedListBase::ShouldAddMenu(
    _In_ UINT uFlags)
{
    CComPtr<IPersistFile> ppfLink;
    CComPtr<IShellItem> psiPin;
    DWORD dwPinFlags;
    HRESULT hr;

    if ((uFlags & CMF_DEFAULTONLY) || IsRestricted() || IsMenuHidden())
        return S_FALSE;

    /* Without the extended menu only programs are offered */
    dwPinFlags = (uFlags & CMF_EXTENDEDVERBS) ? PINNABLE_REJECTSLOWMEDIA
                                              : (PINNABLE_REJECTSLOWMEDIA | PINNABLE_APPLICATION_ONLY);

    /* A site that hosts a shortcut wants the shortcut file pinned, not what it was opened on */
    if (SUCCEEDED(IUnknown_QueryService(m_pUnkSite, IID_IShellLinkW, IID_PPV_ARG(IPersistFile, &ppfLink))))
    {
        PWSTR pszLink = NULL;

        hr = E_FAIL;
        if (ppfLink->GetCurFile(&pszLink) == S_OK && pszLink)
        {
            CComPtr<IShellItem> psiLink;
            CComPtr<IDataObject> pdo;

            hr = SHCreateItemFromParsingName(pszLink, NULL, IID_PPV_ARG(IShellItem, &psiLink));
            if (SUCCEEDED(hr))
                hr = psiLink->BindToHandler(NULL, BHID_SFUIObject, IID_PPV_ARG(IDataObject, &pdo));
            if (SUCCEEDED(hr))
                hr = GetPinnableInfo(pdo, dwPinFlags, &psiPin, NULL, NULL, NULL);
            CoTaskMemFree(pszLink);
        }
    }
    else
    {
        hr = GetPinnableInfo(m_pdtobj, dwPinFlags, &psiPin, NULL, NULL, NULL);
    }

    if (psiPin && hr == S_OK)
    {
        ILFree(m_pidlMenuItem);
        m_pidlMenuItem = NULL;
        hr = SHGetIDListFromObject(psiPin, &m_pidlMenuItem);
    }
    return hr;
}

STDMETHODIMP
CPinnedListBase::QueryContextMenu(
    _In_ HMENU hmenu,
    _In_ UINT indexMenu,
    _In_ UINT idCmdFirst,
    _In_ UINT idCmdLast,
    _In_ UINT uFlags)
{
    WCHAR szText[MAX_PATH];
    UINT uID;
    HRESULT hr;

    hr = ShouldAddMenu(uFlags);
    if (hr != S_OK)
        return hr;

    m_uVerb = (IsPinned(m_pidlMenuItem) == S_OK) ? 1 : 0;
    if (SUCCEEDED(GetMenuStringID(&uID)) && LoadStringW(shell32_hInstance, uID, szText, _countof(szText)))
        InsertMenuW(hmenu, indexMenu, MF_BYPOSITION, idCmdFirst + m_uVerb, szText);

    /* Both verbs own a command ID, whichever one shows */
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 2);
}

STDMETHODIMP
CPinnedListBase::InvokeCommand(
    _In_ LPCMINVOKECOMMANDINFO pici)
{
    UINT uVerb;
    HRESULT hr;

    if (!IS_INTRESOURCE(pici->lpVerb))
    {
        WCHAR szVerb[128];

        if (pici->cbSize >= sizeof(CMINVOKECOMMANDINFOEX) && (pici->fMask & CMIC_MASK_UNICODE) &&
            ((LPCMINVOKECOMMANDINFOEX)pici)->lpVerbW)
        {
            StringCchCopyW(szVerb, _countof(szVerb), ((LPCMINVOKECOMMANDINFOEX)pici)->lpVerbW);
        }
        else
        {
            SHAnsiToUnicode(pici->lpVerb, szVerb, _countof(szVerb));
        }

        for (uVerb = 0; uVerb < 2; uVerb++)
        {
            if (CompareStringOrdinal(szVerb, -1, GetVerb(uVerb), -1, TRUE) == CSTR_EQUAL)
                break;
        }
    }
    else
    {
        uVerb = LOWORD(pici->lpVerb);
    }

    if (uVerb != m_uVerb)
        return E_INVALIDARG;

    if (m_uVerb)
        hr = Modify(m_pidlMenuItem, NULL);
    else
        hr = Modify(NULL, m_pidlMenuItem);
    return hr;
}

STDMETHODIMP
CPinnedListBase::GetCommandString(
    _In_ UINT_PTR idCmd,
    _In_ UINT uType,
    _Reserved_ UINT *pReserved,
    _Out_writes_bytes_(cchMax) LPSTR pszName,
    _In_ UINT cchMax)
{
    WCHAR szHelp[MAX_PATH];
    PCWSTR pszText = NULL;

    switch (uType & ~GCS_UNICODE)
    {
        case GCS_VERBA:
            if (idCmd < 2)
                pszText = GetVerb((UINT)idCmd);
            break;
        case GCS_HELPTEXTA:
            if (idCmd < 2 && GetHelpText((UINT)idCmd, szHelp, _countof(szHelp)) > 0)
                pszText = szHelp;
            break;
    }

    if (!pszText)
        return E_NOTIMPL;

    if (uType & GCS_UNICODE)
        return StringCchCopyW((PWSTR)pszName, cchMax, pszText);
    return SHUnicodeToAnsi(pszText, pszName, cchMax) ? S_OK : E_FAIL;
}

STDMETHODIMP
CPinnedListBase::EnumObjects(
    _Outptr_ IEnumFullIDList **ppenum)
{
    CPinEnum *pEnum;
    HRESULT hr;

    *ppenum = NULL;
    pEnum = new CPinEnum(this);
    if (!pEnum)
        return E_OUTOFMEMORY;

    hr = pEnum->QueryInterface(IID_IEnumFullIDList, (void **)ppenum);
    pEnum->Release();
    return hr;
}

/* Rewrites file system ID lists in the logical form the lists keep */
static
PIDLIST_ABSOLUTE
TranslateAlias(
    _In_opt_ PCIDLIST_ABSOLUTE pidl,
    _Inout_ PCIDLIST_ABSOLUTE *ppidlUse)
{
    PIDLIST_ABSOLUTE pidlLogical;

    if (!pidl || IS_INTRESOURCE(pidl))
        return NULL;

    pidlLogical = SHLogILFromFSIL(const_cast<PIDLIST_ABSOLUTE>(pidl));
    if (pidlLogical)
        *ppidlUse = pidlLogical;
    return pidlLogical;
}

/* From and to add, remove, move to a 1 based place or replace an item */
STDMETHODIMP
CPinnedListBase::Modify(
    _In_opt_ PCIDLIST_ABSOLUTE pidlFrom,
    _In_opt_ PCIDLIST_ABSOLUTE pidlTo)
{
    PIDLIST_ABSOLUTE pidlFromLogical;
    PIDLIST_ABSOLUTE pidlToLogical;
    PIDLIST_ABSOLUTE pidlChanged = NULL;
    CPinStore store;
    HRESULT hr;

    if (IsRestricted())
        return E_ACCESSDENIED;

    pidlFromLogical = TranslateAlias(pidlFrom, &pidlFrom);
    pidlToLogical = TranslateAlias(pidlTo, &pidlTo);

    if (!store.IsValid() || FAILED(store.Load(this)))
    {
        hr = E_OUTOFMEMORY;
    }
    else if (pidlFrom)
    {
        hr = store.ReplacePidl(this, pidlFrom, pidlTo, &pidlChanged);
    }
    else if (!pidlTo)
    {
        hr = E_INVALIDARG;
    }
    else
    {
        WCHAR szSubDir[MAX_PATH];
        CPinLinkFolder linkFolder;

        /* A new item is pinned through its shortcut copy */
        hr = GetBackupSubDirectoryName(szSubDir, _countof(szSubDir));
        if (SUCCEEDED(hr))
            hr = linkFolder.Initialize(szSubDir);
        if (SUCCEEDED(hr))
            hr = linkFolder.CreateBackupLink(pidlTo, &pidlChanged);
        if (SUCCEEDED(hr))
            hr = CacheAppIDInIDList(pidlChanged, &pidlChanged);
        if (SUCCEEDED(hr))
        {
            int iIndex = store.AppendPidl(pidlChanged);

            if (iIndex < 0)
            {
                hr = E_OUTOFMEMORY;
            }
            else
            {
                PinItemUpdateShellLink(store.GetItem(iIndex));
                hr = store.Save(this);
            }
        }
    }

    if (SUCCEEDED(hr))
        NotifyPinListChange(pidlChanged, pidlFrom && !pidlTo);

    ILFree(pidlChanged);
    ILFree(pidlFromLogical);
    ILFree(pidlToLogical);
    return hr;
}

STDMETHODIMP
CPinnedListBase::GetChangeCount(
    _Out_ UINT *pcChanges)
{
    return ReadChangeCount(pcChanges);
}

STDMETHODIMP
CPinnedListBase::GetPinnableInfo(
    _In_ IDataObject *pdo,
    _In_ DWORD dwFlags,
    _Outptr_opt_ IShellItem **ppsiApplication,
    _Outptr_opt_ IShellItem **ppsiDestination,
    _Outptr_opt_ PWSTR *ppszAppID,
    _Out_opt_ BOOL *pfPinnable)
{
    PIDLIST_ABSOLUTE pidl;
    HRESULT hr = S_FALSE;

    if (ppsiApplication)
        *ppsiApplication = NULL;
    if (ppsiDestination)
        *ppsiDestination = NULL;
    if (ppszAppID)
        *ppszAppID = NULL;

    if (SUCCEEDED(PinGetIDListFromDataObject(pdo, &pidl)))
    {
        PWSTR pszAppPath = NULL;

        if (IsAcceptableTarget(pidl, dwFlags, ppsiApplication, ppszAppID, &pszAppPath, ppsiDestination))
        {
            hr = S_OK;
            if (ppsiApplication)
            {
                BOOL fHaveDestination = ppsiDestination && *ppsiDestination;
                PIDLIST_ABSOLUTE pidlApp = NULL;
                PCIDLIST_ABSOLUTE pidlUse = pidl;

                if (*ppsiApplication && !fHaveDestination)
                {
                    hr = SHGetIDListFromObject(*ppsiApplication, &pidlApp);
                    pidlUse = pidlApp;
                }

                if (FAILED(hr) || !pidlUse)
                {
                    hr = E_OUTOFMEMORY;
                }
                else if ((dwFlags & PINNABLE_DETERMINE_APPLICATION) && fHaveDestination)
                {
                    /* The destination must fit the application it is pinned to */
                    if (pszAppPath && ppszAppID && *ppszAppID)
                        hr = MakeDestinationItem(*ppsiDestination, NULL, *ppszAppID, pszAppPath);
                }
                else
                {
                    if (*ppsiApplication)
                    {
                        (*ppsiApplication)->Release();
                        *ppsiApplication = NULL;
                    }
                    hr = SHCreateItemFromIDList(pidlUse, IID_PPV_ARG(IShellItem, ppsiApplication));
                }
                ILFree(pidlApp);
            }
            CoTaskMemFree(pszAppPath);
        }
        ILFree(pidl);
    }

    if (pfPinnable)
        *pfPinnable = (hr == S_OK);
    if (IsRestricted())
        hr = S_FALSE;
    return hr;
}

STDMETHODIMP
CPinnedListBase::IsPinnable(
    _In_ IDataObject *pdo,
    _In_ DWORD dwFlags)
{
    return GetPinnableInfo(pdo, dwFlags, NULL, NULL, NULL, NULL);
}

/* S_OK with a new ID list when the pinned link had to be fixed up, S_FALSE when it was fine */
STDMETHODIMP
CPinnedListBase::Resolve(
    _In_opt_ HWND hwnd,
    _In_ DWORD dwFlags,
    _In_ PCIDLIST_ABSOLUTE pidl,
    _Outptr_ PIDLIST_ABSOLUTE *ppidlResolved)
{
    PIDLIST_ABSOLUTE pidlLogical;
    CComPtr<IShellLinkW> psl;
    CPinStore store;
    PPIN_ITEM pItem;
    HRESULT hr;

    *ppidlResolved = NULL;
    if (IsRestricted())
        return E_ACCESSDENIED;

    pidlLogical = TranslateAlias(pidl, &pidl);
    if (!store.IsValid() || FAILED(store.Load(this)))
    {
        ILFree(pidlLogical);
        return E_OUTOFMEMORY;
    }

    pItem = store.FindPidlOrAppID(pidl, NULL);
    if (!pItem)
        hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    else
        hr = store.LoadShellLink(pItem, &psl);

    if (SUCCEEDED(hr))
    {
        hr = psl->Resolve(hwnd, dwFlags);
        if (hr == S_OK)
        {
            CComPtr<IPersistStream> pps;

            hr = psl->QueryInterface(IID_PPV_ARG(IPersistStream, &pps));
            if (SUCCEEDED(hr))
            {
                hr = S_FALSE;
                if (pps->IsDirty() == S_OK)
                {
                    PIDLIST_ABSOLUTE pidlTarget;

                    hr = psl->GetIDList(&pidlTarget);
                    if (hr == S_FALSE)
                        hr = E_FAIL;
                    if (SUCCEEDED(hr))
                        hr = CacheAppIDInIDList(pidlTarget, &pidlTarget);
                    if (SUCCEEDED(hr))
                    {
                        ILFree(pItem->pidl);
                        pItem->pidl = pidlTarget;
                        *ppidlResolved = ILClone(pidlTarget);
                        hr = *ppidlResolved ? S_OK : E_OUTOFMEMORY;
                    }
                }
            }
        }
        else if (SUCCEEDED(hr))
        {
            hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }
    }

    if (hr == S_OK && SUCCEEDED(store.Save(this)))
        NotifyPinListChange(NULL, FALSE);

    ILFree(pidlLogical);
    return hr;
}

STDMETHODIMP
CPinnedListBase::IsPinned(
    _In_ PCIDLIST_ABSOLUTE pidl)
{
    CPinStore store;

    if (!store.IsValid() || FAILED(store.Load(this)))
        return E_OUTOFMEMORY;
    return store.FindPidlOrAppID(pidl, NULL) ? S_OK : S_FALSE;
}

STDMETHODIMP
CPinnedListBase::GetPinnedItem(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _Outptr_ PIDLIST_ABSOLUTE *ppidlPinned)
{
    CPinStore store;
    PPIN_ITEM pItem;

    *ppidlPinned = NULL;
    if (!store.IsValid() || FAILED(store.Load(this)))
        return E_OUTOFMEMORY;

    pItem = store.FindPidlOrAppID(pidl, NULL);
    if (!pItem)
        return S_FALSE;

    *ppidlPinned = ILClone(pItem->pidl);
    return *ppidlPinned ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP
CPinnedListBase::GetAppIDForPinnedItem(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _Outptr_ PWSTR *ppszAppID)
{
    return ILGetHiddenStringAllocW(pidl, HIDDENID_APPID, ppszAppID) ? S_OK : E_FAIL;
}

/* Follows a pinned item that was renamed or moved */
STDMETHODIMP
CPinnedListBase::ItemChangeNotify(
    _In_ PCIDLIST_ABSOLUTE pidlFrom,
    _In_ PCIDLIST_ABSOLUTE pidlTo)
{
    PIDLIST_ABSOLUTE pidlFromLogical;
    PIDLIST_ABSOLUTE pidlToLogical;
    PIDLIST_ABSOLUTE pidlOld = NULL;
    PWSTR pszOldAppID = NULL;
    PWSTR pszNewAppID = NULL;
    CPinStore store;
    HRESULT hr;

    if (!pidlFrom || !pidlTo)
        return E_INVALIDARG;

    pidlFromLogical = TranslateAlias(pidlFrom, &pidlFrom);
    pidlToLogical = TranslateAlias(pidlTo, &pidlTo);

    if (!store.IsValid() || FAILED(store.Load(this)))
    {
        hr = E_OUTOFMEMORY;
    }
    else
    {
        PPIN_ITEM pItem = store.FindPidl(pidlFrom, NULL);

        if (!pItem)
        {
            hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        else
        {
            PIDLIST_ABSOLUTE pidlNew;

            hr = SimpleToRealIDList(pidlTo, &pidlNew);
            if (SUCCEEDED(hr))
                hr = CacheAppIDInIDList(pidlNew, &pidlNew);
            if (SUCCEEDED(hr))
            {
                pidlOld = ILClone(pItem->pidl);
                if (!pidlOld)
                {
                    ILFree(pidlNew);
                    hr = E_OUTOFMEMORY;
                }
                else
                {
                    ILGetHiddenStringAllocW(pItem->pidl, HIDDENID_APPID, &pszOldAppID);
                    ILFree(pItem->pidl);
                    pItem->pidl = pidlNew;
                    ILGetHiddenStringAllocW(pidlNew, HIDDENID_APPID, &pszNewAppID);
                    PinItemUpdateShellLink(pItem);
                }
            }
        }
    }

    /* Nothing to write when the same item kept the same application */
    if (SUCCEEDED(hr) &&
        (!pszOldAppID || !pszNewAppID ||
         SHCompareIDsFull(SHCIDS_CANONICALONLY, pidlFrom, pidlTo, NULL) != S_OK ||
         CompareStringOrdinal(pszOldAppID, -1, pszNewAppID, -1, TRUE) != CSTR_EQUAL))
    {
        hr = store.Save(this);
        if (SUCCEEDED(hr))
            NotifyPinListChange(pidlOld, TRUE);
    }

    CoTaskMemFree(pszNewAppID);
    CoTaskMemFree(pszOldAppID);
    ILFree(pidlOld);
    ILFree(pidlFromLogical);
    ILFree(pidlToLogical);
    return hr;
}

/* Drops items whose targets were uninstalled since Explorer last looked */
STDMETHODIMP
CPinnedListBase::UpdateForRemovedItemsAsNecessary()
{
    UINT cGlobal = GetRemovedGlobalChangeCount();
    CComPtr<IEnumFullIDList> penum;
    PIDLIST_ABSOLUTE pidl;
    BOOL fRemoved = FALSE;

    if (!IsExplorerProcess() || cGlobal == GetRemovedChangeCount())
        return S_OK;

    m_fSuppressNotify = TRUE;
    if (SUCCEEDED(EnumObjects(&penum)))
    {
        while (penum->Next(1, &pidl, NULL) == S_OK)
        {
            PIDLIST_ABSOLUTE pidlResolved = NULL;

            if (FAILED(Resolve(NULL, PIN_RESOLVE_QUIET, pidl, &pidlResolved)))
            {
                if (SUCCEEDED(Modify(pidl, NULL)))
                    fRemoved = TRUE;
            }
            else
            {
                ResolveTargetLink(pidlResolved ? pidlResolved : pidl, &fRemoved);
                ILFree(pidlResolved);
            }
            ILFree(pidl);
        }
    }
    m_fSuppressNotify = FALSE;

    if (fRemoved)
        NotifyPinListChange(NULL, TRUE);
    SetRemovedChangeCount(cGlobal);
    return S_OK;
}

/* Start menu */

HRESULT
CStartMenuPin::SetChangeCount(
    _In_ UINT cChanges)
{
    WriteKeyDword(STARTPAGE2_KEY, L"FavoritesChanges", cChanges);
    return S_OK;
}

IStream *
CStartMenuPin::OpenPinStream(
    _In_ DWORD grfMode)
{
    return SHOpenRegStream2W(HKEY_CURRENT_USER, STARTPAGE2_KEY, L"Favorites", grfMode);
}

IStream *
CStartMenuPin::OpenLinksStream(
    _In_ DWORD grfMode)
{
    return SHOpenRegStream2W(HKEY_CURRENT_USER, STARTPAGE2_KEY, L"FavoritesResolve", grfMode);
}

VOID
CStartMenuPin::NotifyPinListChange(
    _In_opt_ PCIDLIST_ABSOLUTE pidl,
    _In_ BOOL fRemoved)
{
    if (!m_fSuppressNotify)
        SendPinEvent(PIN_EVENT_STARTMENU, SHCNF_IDLIST | SHCNF_FLUSH | SHCNF_FLUSHNOWAIT);
}

UINT
CStartMenuPin::GetPinStreamVersion()
{
    return ReadKeyDword(HKEY_CURRENT_USER, STARTPAGE2_KEY, L"FavoritesVersion");
}

HRESULT
CStartMenuPin::SetPinStreamVersion(
    _In_ UINT uVersion)
{
    return WriteKeyDword(STARTPAGE2_KEY, L"FavoritesVersion", uVersion);
}

HRESULT
CStartMenuPin::GetBackupSubDirectoryName(
    _Out_writes_(cch) PWSTR pszName,
    _In_ UINT cch)
{
    return StringCchCopyW(pszName, cch, L"StartMenu");
}

BOOL
CStartMenuPin::IsRestricted()
{
    return SHRestricted(REST_NOSMPINNEDLIST) != 0;
}

HRESULT
CStartMenuPin::GetMenuStringID(
    _Out_ UINT *puID)
{
    *puID = IDS_PIN_TO_STARTMENU + m_uVerb;
    return S_OK;
}

int
CStartMenuPin::GetHelpText(
    _In_ UINT uVerb,
    _Out_writes_(cch) PWSTR pszText,
    _In_ int cch)
{
    return LoadStringW(shell32_hInstance, IDS_PIN_TO_STARTMENU_HELP + uVerb, pszText, cch);
}

HRESULT
CStartMenuPin::ReadChangeCount(
    _Out_ UINT *pcChanges)
{
    *pcChanges = ReadKeyDword(HKEY_CURRENT_USER, STARTPAGE2_KEY, L"FavoritesChanges");
    return S_OK;
}

PCWSTR
CStartMenuPin::GetVerb(
    _In_ UINT uVerb)
{
    static const PCWSTR s_Verbs[] = { L"startpin", L"startunpin" };
    return (uVerb < _countof(s_Verbs)) ? s_Verbs[uVerb] : NULL;
}

HRESULT
CStartMenuPin::SetRemovedChangeCount(
    _In_ UINT cChanges)
{
    return WriteKeyDword(STARTPAGE2_KEY, L"FavoritesRemovedChanges", cChanges);
}

UINT
CStartMenuPin::GetRemovedChangeCount()
{
    return ReadKeyDword(HKEY_CURRENT_USER, STARTPAGE2_KEY, L"FavoritesRemovedChanges");
}

/* Forgets a program everywhere: its jump lists, both pinned lists and its installer shortcut */
STDMETHODIMP
CStartMenuPin::RemoveFromList(
    _In_ IShellItem *psi)
{
    CComPtr<IApplicationResolver> par;
    CComPtr<IShellLinkW> psl;
    PIDLIST_ABSOLUTE pidl;
    HRESULT hr;

    if (SUCCEEDED(CoCreateInstance(CLSID_StartMenuCacheAndAppResolver, NULL, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARG(IApplicationResolver, &par))))
    {
        PWSTR pszAppID;

        if (SUCCEEDED(par->GetAppIDForShortcut(psi, &pszAppID)))
        {
            CComPtr<ICustomDestinationList> pcdl;

            if (SUCCEEDED(CoCreateInstance(CLSID_DestinationList, NULL, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARG(ICustomDestinationList, &pcdl))))
            {
                pcdl->DeleteList(pszAppID);
            }
            CoTaskMemFree(pszAppID);
        }
    }

    hr = SHGetIDListFromObject(psi, &pidl);
    if (SUCCEEDED(hr))
    {
        hr = Modify(pidl, NULL);
        if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            hr = S_FALSE;
        if (SUCCEEDED(hr))
        {
            CComPtr<IPinnedList> pplTaskband;

            hr = CoCreateInstance(CLSID_TaskbandPin, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IPinnedList, &pplTaskband));
            if (SUCCEEDED(hr))
            {
                hr = pplTaskband->Modify(pidl, NULL);
                if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
                    hr = S_FALSE;
            }
        }
        ILFree(pidl);
    }

    if (SUCCEEDED(psi->BindToHandler(NULL, BHID_SFUIObject, IID_PPV_ARG(IShellLinkW, &psl))))
    {
        WCHAR szProduct[MAX_PATH];

        if (GetDarwinLinkInfo(psl, NULL, szProduct, _countof(szProduct)))
        {
            hr = RemoveDarwinShortcut(this, szProduct);
            if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
                hr = S_FALSE;
            if (SUCCEEDED(hr))
            {
                CComPtr<IPinnedList> pplTaskband;

                hr = CoCreateInstance(CLSID_TaskbandPin, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IPinnedList, &pplTaskband));
                if (SUCCEEDED(hr))
                {
                    hr = RemoveDarwinShortcut(pplTaskband, szProduct);
                    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
                        hr = S_FALSE;
                }
            }
        }
    }

    /* Every Explorer prunes its lists once it sees the count move */
    DWORD cRemoved = GetRemovedGlobalChangeCount() + 1;
    if (SHSetValueW(HKEY_LOCAL_MACHINE, STARTPAGE_KEY, L"FavoritesRemovedChanges", REG_DWORD,
                    &cRemoved, sizeof(cRemoved)) == ERROR_SUCCESS)
    {
        SendPinEvent(PIN_EVENT_REMOVED, SHCNF_IDLIST | SHCNF_FLUSH | SHCNF_FLUSHNOWAIT);
    }
    return hr;
}

/* Taskbar */

HRESULT
CTaskbandPin::SetChangeCount(
    _In_ UINT cChanges)
{
    WriteKeyDword(TASKBAND_KEY, L"FavoritesChanges", cChanges);
    return S_OK;
}

IStream *
CTaskbandPin::OpenPinStream(
    _In_ DWORD grfMode)
{
    return SHOpenRegStream2W(HKEY_CURRENT_USER, TASKBAND_KEY, L"Favorites", grfMode);
}

IStream *
CTaskbandPin::OpenLinksStream(
    _In_ DWORD grfMode)
{
    return SHOpenRegStream2W(HKEY_CURRENT_USER, TASKBAND_KEY, L"FavoritesResolve", grfMode);
}

VOID
CTaskbandPin::NotifyPinListChange(
    _In_opt_ PCIDLIST_ABSOLUTE pidl,
    _In_ BOOL fRemoved)
{
    if (!m_fSuppressNotify)
        SendPinEvent(PIN_EVENT_TASKBAND, fRemoved ? (SHCNF_IDLIST | SHCNF_FLUSH | SHCNF_FLUSHNOWAIT)
                                                  : (SHCNF_IDLIST | SHCNF_FLUSH));
}

UINT
CTaskbandPin::GetPinStreamVersion()
{
    return ReadKeyDword(HKEY_CURRENT_USER, TASKBAND_KEY, L"FavoritesVersion");
}

HRESULT
CTaskbandPin::SetPinStreamVersion(
    _In_ UINT uVersion)
{
    return WriteKeyDword(TASKBAND_KEY, L"FavoritesVersion", uVersion);
}

BOOL
CTaskbandPin::IsRestricted()
{
    return PinPolicyEnabled(PIN_POLICY_TASKBARNOPINNEDLIST);
}

HRESULT
CTaskbandPin::GetMenuStringID(
    _Out_ UINT *puID)
{
    if (PinPolicyEnabled(PIN_POLICY_NOPINNINGTOTASKBAR))
        return E_ACCESSDENIED;

    *puID = IDS_PIN_TO_TASKBAR + m_uVerb;
    return S_OK;
}

int
CTaskbandPin::GetHelpText(
    _In_ UINT uVerb,
    _Out_writes_(cch) PWSTR pszText,
    _In_ int cch)
{
    return LoadStringW(shell32_hInstance, IDS_PIN_TO_TASKBAR_HELP + uVerb, pszText, cch);
}

HRESULT
CTaskbandPin::ReadChangeCount(
    _Out_ UINT *pcChanges)
{
    *pcChanges = ReadKeyDword(HKEY_CURRENT_USER, TASKBAND_KEY, L"FavoritesChanges");
    return S_OK;
}

PCWSTR
CTaskbandPin::GetVerb(
    _In_ UINT uVerb)
{
    static const PCWSTR s_Verbs[] = { L"taskbarpin", L"taskbarunpin" };
    return (uVerb < _countof(s_Verbs)) ? s_Verbs[uVerb] : NULL;
}

HRESULT
CTaskbandPin::SetRemovedChangeCount(
    _In_ UINT cChanges)
{
    return WriteKeyDword(TASKBAND_KEY, L"FavoritesRemovedChanges", cChanges);
}

UINT
CTaskbandPin::GetRemovedChangeCount()
{
    return ReadKeyDword(HKEY_CURRENT_USER, TASKBAND_KEY, L"FavoritesRemovedChanges");
}

/* With pinning turned off by policy only what is pinned already may stay */
BOOL
CTaskbandPin::IsAllowedWhilePinningDisabled(
    _In_opt_ IShellItem *psiApplication)
{
    PIDLIST_ABSOLUTE pidl;
    BOOL fPinned;

    if (!PinPolicyEnabled(PIN_POLICY_NOPINNINGTOTASKBAR))
        return TRUE;
    if (!psiApplication || FAILED(SHGetIDListFromObject(psiApplication, &pidl)))
        return FALSE;

    fPinned = (IsPinned(pidl) == S_OK);
    ILFree(pidl);
    return fPinned;
}

HRESULT
CTaskbandPin::GetApplication(
    _In_ IApplicationResolver *par,
    _In_ PCWSTR pszPath,
    _Outptr_opt_ IShellItem **ppsiApplication,
    _Outptr_opt_ PWSTR *ppszAppID,
    _Outptr_opt_ PWSTR *ppszAppPath)
{
    WCHAR szPath[MAX_PATH];
    HRESULT hr;

    if (ppsiApplication)
        *ppsiApplication = NULL;
    if (ppszAppID)
        *ppszAppID = NULL;
    if (ppszAppPath)
        *ppszAppPath = NULL;

    if (!IsLocalHardDisk(pszPath))
        return E_FAIL;

    hr = StringCchCopyW(szPath, _countof(szPath), pszPath);
    if (FAILED(hr))
        return hr;
    if (ppszAppPath)
        PathGetLongPath(szPath, szPath, _countof(szPath));

    /* A program with a shortcut is pinned through that shortcut */
    hr = par->GetBestShortcutAndAppIDForAppPath(szPath, ppsiApplication, ppszAppID);
    if (FAILED(hr))
    {
        CComPtr<IShellItem> psi;
        CComPtr<IBindCtx> pbc;

        hr = CreateBindCtxWithParam(PIN_BIND_NO_ALIAS, &pbc);
        if (SUCCEEDED(hr))
            hr = SHCreateItemFromParsingName(szPath, pbc, IID_PPV_ARG(IShellItem, &psi));
        if (SUCCEEDED(hr))
            hr = par->CanPinApp(psi);
        if (FAILED(hr))
            return hr;

        if (ppsiApplication)
            *ppsiApplication = psi.Detach();
        if (ppszAppID)
        {
            hr = GenerateAppID(szPath, NULL, NULL, ppszAppID);
            if (FAILED(hr))
            {
                if (ppsiApplication && *ppsiApplication)
                {
                    (*ppsiApplication)->Release();
                    *ppsiApplication = NULL;
                }
                return hr;
            }
        }
    }

    if (ppszAppPath)
        hr = SHStrDupW(szPath, ppszAppPath);
    return hr;
}

HRESULT
CTaskbandPin::ProcessDestinationFolder(
    _In_ IApplicationResolver *par,
    _In_ PCIDLIST_ABSOLUTE pidl,
    _Outptr_opt_ IShellItem **ppsiApplication,
    _Outptr_opt_ PWSTR *ppszAppID)
{
    CComPtr<IShellItem> psi;
    PWSTR pszAppID = NULL;
    HRESULT hr;

    if (ppszAppID)
        *ppszAppID = NULL;
    if (ppsiApplication)
        *ppsiApplication = NULL;

    hr = SHCreateItemFromIDList(pidl, IID_PPV_ARG(IShellItem, &psi));
    if (SUCCEEDED(hr))
        hr = DetermineFolderDestinationParentAppID(par, psi, ppsiApplication, &pszAppID, NULL);
    if (FAILED(hr))
        return hr;

    if (!IsAllowedWhilePinningDisabled(ppsiApplication ? *ppsiApplication : NULL))
    {
        if (ppsiApplication && *ppsiApplication)
        {
            (*ppsiApplication)->Release();
            *ppsiApplication = NULL;
        }
        CoTaskMemFree(pszAppID);
        return E_ACCESSDENIED;
    }

    if (ppszAppID)
        *ppszAppID = pszAppID;
    else
        CoTaskMemFree(pszAppID);
    return S_OK;
}

/* A file is pinned to the jump list of the program that opens it */
HRESULT
CTaskbandPin::ProcessDestinationFile(
    _In_ IApplicationResolver *par,
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_ SFGAOF sfgao,
    _Outptr_opt_ IShellItem **ppsiApplication,
    _Outptr_opt_ PWSTR *ppszAppID,
    _Outptr_ PWSTR *ppszAppPath)
{
    WCHAR szName[MAX_PATH];
    WCHAR szHandler[MAX_PATH];
    DWORD cchHandler = _countof(szHandler);
    PWSTR pszAppPath = NULL;
    PCWSTR pszExt;
    HRESULT hr;

    *ppszAppPath = NULL;
    if (ppszAppID)
        *ppszAppID = NULL;
    if (ppsiApplication)
        *ppsiApplication = NULL;

    hr = SHGetNameAndFlagsW(pidl, SHGDN_INFOLDER | SHGDN_FORPARSING, szName, _countof(szName), NULL);
    if (FAILED(hr))
        return hr;

    /* Programs are pinned themselves, never into a jump list */
    pszExt = PathFindExtensionW(szName);
    if (!*pszExt || GetExeOrMscExtension(szName))
        return E_FAIL;

    hr = AssocQueryStringW(ASSOCF_VERIFY, ASSOCSTR_EXECUTABLE, pszExt, NULL, szHandler, &cchHandler);
    if (FAILED(hr))
        return hr;

    if ((sfgao & SFGAO_FOLDER) &&
        CompareStringOrdinal(PathFindFileNameW(szHandler), -1, L"explorer.exe", -1, TRUE) == CSTR_EQUAL)
    {
        return ProcessDestinationFolder(par, pidl, ppsiApplication, ppszAppID);
    }

    hr = GetApplication(par, szHandler, ppsiApplication, ppszAppID, &pszAppPath);
    if (FAILED(hr))
        return hr;

    if (!IsAllowedWhilePinningDisabled(ppsiApplication ? *ppsiApplication : NULL))
    {
        CoTaskMemFree(pszAppPath);
        return E_ACCESSDENIED;
    }

    *ppszAppPath = pszAppPath;
    return S_OK;
}

BOOL
CTaskbandPin::IsAcceptableTarget(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_ DWORD dwFlags,
    _Outptr_opt_ IShellItem **ppsiApplication,
    _Outptr_opt_ PWSTR *ppszAppID,
    _Outptr_ PWSTR *ppszAppPath,
    _Outptr_opt_ IShellItem **ppsiDestination)
{
    BOOL fDetermine = (dwFlags & PINNABLE_DETERMINE_APPLICATION) != 0;
    BOOL fRejectSlow = (dwFlags & PINNABLE_REJECTSLOWMEDIA) != 0;
    CComPtr<IApplicationResolver> par;
    CComPtr<IShellItem> psi;
    PIDLIST_ABSOLUTE pidlLinkTarget = NULL;
    WCHAR szPath[MAX_PATH];
    WCHAR szTarget[MAX_PATH] = L"";
    BOOL fLink = FALSE;
    DWORD sfgao;
    HRESULT hr;

    if (ppsiDestination)
        *ppsiDestination = NULL;
    if (ppszAppID)
        *ppszAppID = NULL;
    if (ppsiApplication)
        *ppsiApplication = NULL;
    *ppszAppPath = NULL;

    sfgao = fDetermine ? (SFGAO_FILESYSTEM | SFGAO_FOLDER | SFGAO_STREAM) : SFGAO_FILESYSTEM;
    if (FAILED(SHGetNameAndFlagsW(pidl, SHGDN_FORPARSING, szPath, _countof(szPath), &sfgao)))
        return FALSE;

    if (fRejectSlow && !fDetermine && (sfgao & SFGAO_FILESYSTEM) && !IsLocalHardDisk(szPath))
        return FALSE;

    hr = SHCreateItemFromIDList(pidl, IID_PPV_ARG(IShellItem, &psi));
    if (SUCCEEDED(hr))
    {
        hr = CoCreateInstance(CLSID_StartMenuCacheAndAppResolver, NULL, CLSCTX_INPROC,
                              IID_PPV_ARG(IApplicationResolver, &par));
    }
    if (FAILED(hr))
        return FALSE;

    {
        CComPtr<IShellLinkW> psl;

        if (SUCCEEDED(psi->BindToHandler(NULL, BHID_SFUIObject, IID_PPV_ARG(IShellLinkW, &psl))))
        {
            fLink = TRUE;
            if (SUCCEEDED(GetAppPathFromShortcut(psl, pidl, szTarget, _countof(szTarget))) && fRejectSlow)
            {
                BOOL fShellTarget = (szTarget[0] == L':' && szTarget[1] == L':' &&
                                     szTarget[2] == L'{' && szTarget[3]);

                hr = E_FAIL;
                if (fDetermine || IsLocalHardDisk(szTarget) || fShellTarget)
                    hr = psl->GetIDList(&pidlLinkTarget);
            }
        }
    }

    if (SUCCEEDED(hr))
    {
        hr = par->CanPinApp(psi);
        if (fDetermine || (dwFlags & PINNABLE_APPLICATION_ONLY))
        {
            if (SUCCEEDED(hr))
            {
                if (PinPolicyEnabled(PIN_POLICY_NOPINNINGTOTASKBAR))
                {
                    hr = E_ACCESSDENIED;
                }
                else if (fLink)
                {
                    /* A shortcut is pinned as it is when both it and its program are local */
                    if ((!(sfgao & SFGAO_FILESYSTEM) || IsLocalHardDisk(szPath)) && IsLocalHardDisk(szTarget))
                    {
                        hr = S_OK;
                        if (ppsiApplication)
                            hr = psi->QueryInterface(IID_PPV_ARG(IShellItem, ppsiApplication));
                        if (SUCCEEDED(hr) && ppszAppID)
                        {
                            hr = par->GetAppIDForShortcut(psi, ppszAppID);
                            if (FAILED(hr) && ppsiApplication && *ppsiApplication)
                            {
                                (*ppsiApplication)->Release();
                                *ppsiApplication = NULL;
                            }
                        }
                    }
                    else
                    {
                        hr = E_FAIL;
                    }
                }
                else
                {
                    hr = GetApplication(par, szPath, ppsiApplication, ppszAppID, NULL);
                }
            }
            else if (!(dwFlags & PINNABLE_APPLICATION_ONLY))
            {
                /* Not a program, so it may still go into a program's jump list */
                if (PinPolicyEnabled(PIN_POLICY_NOPINNINGTODESTINATIONS))
                {
                    hr = E_ACCESSDENIED;
                }
                else if (!fLink || szTarget[0])
                {
                    PCIDLIST_ABSOLUTE pidlUse = pidlLinkTarget ? pidlLinkTarget : pidl;

                    if (!ppsiDestination)
                    {
                        hr = E_UNEXPECTED;
                    }
                    else
                    {
                        if (!(sfgao & SFGAO_FOLDER) || (sfgao & SFGAO_STREAM))
                            hr = ProcessDestinationFile(par, pidlUse, sfgao, ppsiApplication, ppszAppID, ppszAppPath);
                        else
                            hr = ProcessDestinationFolder(par, pidlUse, ppsiApplication, ppszAppID);
                        if (SUCCEEDED(hr))
                            hr = SHCreateItemFromIDList(pidlUse, IID_PPV_ARG(IShellItem, ppsiDestination));
                    }
                }
            }
        }
    }

    ILFree(pidlLinkTarget);
    return SUCCEEDED(hr);
}
