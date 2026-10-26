/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Command folder: the commands of the view in its site, as a shell namespace
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

/* Menu of the command items: one default "Invoke" entry */
#define IDM_COMMANDITEM         392
#define IDC_COMMANDITEM_INVOKE  16

/* Explorer pane state bit for a pane that is off by default */
#define EPS_DEFAULT_OFF_BIT     0x2

/* Commands on the right of the bar, next to the preview pane button */
static const GUID *const c_rgpguidRightSideCommands[] =
{
    &UICID_ViewControl,
};

BOOL
WINAPI
IsRightSideCommand(
    _In_ REFGUID rguidCanonical)
{
    for (UINT i = 0; i < _countof(c_rgpguidRightSideCommands); i++)
    {
        if (IsEqualGUID(*c_rgpguidRightSideCommands[i], rguidCanonical))
            return TRUE;
    }
    return FALSE;
}

HRESULT
WINAPI
_GetCommandFolder(
    _COM_Outptr_ IShellFolder **ppsf)
{
    PIDLIST_ABSOLUTE pidl;
    HRESULT hr;

    *ppsf = NULL;
    hr = SHParseDisplayName(COMMANDFOLDER_PARSENAME, NULL, &pidl, 0, NULL);
    if (SUCCEEDED(hr))
    {
        hr = SHBindToObject(NULL, pidl, NULL, IID_PPV_ARGS(ppsf));
        CoTaskMemFree(pidl);
    }
    return hr;
}

/*
 * Folder type of a location. Windows also reads desktop.ini and the item's properties; the
 * shell folder's registered FolderType is what ReactOS folders can name today.
 */
static
HRESULT
GetShellFolderType(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _Out_ FOLDERTYPEID *pftid)
{
    IShellFolder *psf;
    CLSID clsid;
    HRESULT hr;

    *pftid = FOLDERTYPEID_Generic;
    hr = SHBindToObject(NULL, pidl, NULL, IID_PPV_ARGS(&psf));
    if (SUCCEEDED(hr))
    {
        hr = IUnknown_GetClassID(psf, &clsid);
        if (SUCCEEDED(hr))
        {
            WCHAR szKey[MAX_PATH];
            WCHAR szClsid[39];

            SHStringFromGUIDW(clsid, szClsid, _countof(szClsid));
            hr = StringCchPrintfW(szKey, _countof(szKey), L"CLSID\\%s\\ShellFolder", szClsid);
            if (SUCCEEDED(hr) && FAILED(SHRegGetGUID(HKEY_CLASSES_ROOT, szKey, L"FolderType", pftid)))
                *pftid = FOLDERTYPEID_Generic;
        }
        psf->Release();
    }
    return S_OK;
}

/* The command item id inside a pidl, or NULL when the pidl is not one */
static
const COMMANDITEMID *
CommandItemFromPidl(
    _In_opt_ PCUIDLIST_RELATIVE pidl)
{
    const COMMANDITEMID *pItem = (const COMMANDITEMID *)pidl;

    if (!pItem || pItem->cb < sizeof(*pItem) || pItem->dwSignature != COMMANDITEM_SIGNATURE)
        return NULL;
    return pItem;
}

/* Adds a menu template's items to a QCMINFO menu */
static
HRESULT
MergeMenuResource(
    _In_ UINT idMenu,
    _Inout_ QCMINFO *pqcm)
{
    HMENU hmenu = LoadMenuW(DUIFRAME_HINST, MAKEINTRESOURCEW(idMenu));
    HMENU hmenuPopup;

    if (!hmenu)
        return HRESULT_FROM_WIN32(GetLastError());

    hmenuPopup = GetSubMenu(hmenu, 0);
    if (hmenuPopup)
    {
        pqcm->idCmdFirst = Shell_MergeMenus(pqcm->hmenu, hmenuPopup, pqcm->indexMenu, pqcm->idCmdFirst,
                                            pqcm->idCmdLast, MM_SUBMENUSHAVEIDS);
    }
    DestroyMenu(hmenu);
    return S_OK;
}

static
HRESULT
StringToStrRet(
    _In_ PCWSTR psz,
    _Out_ STRRET *psr)
{
    psr->uType = STRRET_WSTR;
    return SHStrDupW(psz, &psr->pOleStr);
}

/* CCommandFolder */

CCommandFolder::CCommandFolder() :
    _cRef(1)
{
}

CCommandFolder::~CCommandFolder()
{
    CoTaskMemFree(_pidl);
    for (UINT i = 0; i < _cEntries; i++)
    {
        if (_pEntries[i].pec)
            _pEntries[i].pec->Release();
    }
    CoTaskMemFree(_pEntries);
}

HRESULT
WINAPI
CCommandFolder::CreateInstance(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CCommandFolder *pFolder;
    HRESULT hr;

    *ppv = NULL;
    pFolder = new CCommandFolder();
    if (!pFolder)
        return E_OUTOFMEMORY;

    hr = pFolder->QueryInterface(riid, ppv);
    pFolder->Release();
    return hr;
}

STDMETHODIMP
CCommandFolder::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CCommandFolder, IShellFolder2),
        QITABENTMULTI(CCommandFolder, IShellFolder, IShellFolder2),
        QITABENTMULTI(CCommandFolder, IPersistFolder, IPersistFolder2),
        QITABENTMULTI(CCommandFolder, IPersist, IPersistFolder2),
        QITABENT(CCommandFolder, IPersistFolder2),
        QITABENT(CCommandFolder, IObjectWithSite),
        QITABENT(CCommandFolder, IContextMenuCB),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CCommandFolder::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CCommandFolder::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

HRESULT
CCommandFolder::_InsertEntry(
    _In_ UINT iEntry,
    _In_ const COMMANDFOLDERENTRY *pEntry)
{
    if (_cEntries == _cAlloc)
    {
        UINT cAlloc = _cAlloc ? _cAlloc * 2 : 5;
        COMMANDFOLDERENTRY *pEntries;

        pEntries = (COMMANDFOLDERENTRY *)CoTaskMemRealloc(_pEntries, cAlloc * sizeof(*pEntries));
        if (!pEntries)
            return E_OUTOFMEMORY;
        _pEntries = pEntries;
        _cAlloc = cAlloc;
    }

    if (iEntry > _cEntries)
        iEntry = _cEntries;
    MoveMemory(&_pEntries[iEntry + 1], &_pEntries[iEntry], (_cEntries - iEntry) * sizeof(*_pEntries));
    _pEntries[iEntry] = *pEntry;
    if (pEntry->pec)
        pEntry->pec->AddRef();
    _cEntries++;
    return S_OK;
}

VOID
CCommandFolder::_RemoveEntry(
    _In_ UINT iEntry)
{
    if (iEntry >= _cEntries)
        return;

    if (_pEntries[iEntry].pec)
        _pEntries[iEntry].pec->Release();
    MoveMemory(&_pEntries[iEntry], &_pEntries[iEntry + 1], (_cEntries - iEntry - 1) * sizeof(*_pEntries));
    _cEntries -= 1;
}

HRESULT
CCommandFolder::_AddEntry(
    _In_ IExplorerCommand *pec,
    _In_ DWORD dwEntryFlags)
{
    COMMANDFOLDERENTRY entry = { pec, dwEntryFlags, FALSE };

    return _InsertEntry(_cEntries, &entry);
}

HRESULT
CCommandFolder::GetEntry(
    _In_ UINT iEntry,
    _Out_ COMMANDFOLDERENTRY *pEntry)
{
    if (iEntry >= _cEntries)
        return E_INVALIDARG;
    *pEntry = _pEntries[iEntry];
    return S_OK;
}

HRESULT
CCommandFolder::CreateItemID(
    _In_ REFGUID rguidCanonical,
    _In_ UINT iCommand,
    _Outptr_ PITEMID_CHILD *ppidl)
{
    COMMANDITEMID *pItem;

    /* Room for the terminating zero length id */
    pItem = (COMMANDITEMID *)CoTaskMemAlloc(sizeof(*pItem) + sizeof(USHORT));
    *ppidl = (PITEMID_CHILD)pItem;
    if (!pItem)
        return E_OUTOFMEMORY;

    ZeroMemory(pItem, sizeof(*pItem) + sizeof(USHORT));
    pItem->cb = sizeof(*pItem);
    pItem->dwSignature = COMMANDITEM_SIGNATURE;
    pItem->guidCanonical = rguidCanonical;
    pItem->iCommand = iCommand;
    return S_OK;
}

/* The command list is built once, from the first site that asks */
HRESULT
CCommandFolder::GetCommandsFromSite(
    _In_opt_ IUnknown *punkSite)
{
    IUnknown *punkDialog;

    if (_pEntries)
        return S_OK;

    /* File dialogs have a fixed bar: organize, new folder and the view commands */
    if (SUCCEEDED(IUnknown_QueryService(punkSite, IID_IFileDialogPrivate, IID_PPV_ARGS(&punkDialog))))
    {
        IEnumUICommand *penum;

        punkDialog->Release();
        _AppendOrganizeCommand(punkSite);
        if (SUCCEEDED(LegacyEnumSpecialTasksByType(7, &penum)))
        {
            _AppendLegacyCommands(penum);
            penum->Release();
        }
        _AppendRightSideCommands(punkSite);
        return S_OK;
    }

    return _EnumFolderCommandsForExplorer(punkSite);
}

HRESULT
CCommandFolder::_GetView(
    _In_opt_ IUnknown *punkSite,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    IShellBrowser *psb = NULL;
    IShellView *psv = NULL;
    HRESULT hr;

    *ppv = NULL;
    if (SUCCEEDED(IUnknown_QueryService(punkSite, SID_STopLevelBrowser, IID_PPV_ARGS(&psb))))
        hr = psb->QueryActiveShellView(&psv);
    else
        hr = IUnknown_QueryService(punkSite, IID_IFolderView, IID_PPV_ARGS(&psv));

    if (SUCCEEDED(hr))
        hr = psv->QueryInterface(riid, ppv);

    if (psb)
        psb->Release();
    if (psv)
        psv->Release();
    return hr;
}

/*
 * Order of the bar: organize, the view's legacy tasks, the folder type's tasks, the folder's
 * own commands, then the right side commands. Later duplicates of a command are dropped.
 */
HRESULT
CCommandFolder::_EnumFolderCommandsForExplorer(
    _In_opt_ IUnknown *punkSite)
{
    IShellFolderViewCB *psfvcb = NULL;
    IEnumExplorerCommand *penumFolder = NULL;
    IFolderView *pfv = NULL;

    if (SUCCEEDED(_GetView(punkSite, IID_PPV_ARGS(&pfv))))
    {
        IShellFolder *psf;

        IUnknown_QueryService(pfv, IID_IShellFolderViewCB, IID_IShellFolderViewCB, (void **)&psfvcb);
        if (SUCCEEDED(pfv->GetFolder(IID_PPV_ARGS(&psf))))
        {
            IExplorerCommandProvider *pecp;

            if (SUCCEEDED(psf->CreateViewObject(NULL, IID_PPV_ARGS(&pecp))))
            {
                pecp->GetCommands(punkSite, IID_PPV_ARGS(&penumFolder));
                pecp->Release();
            }
            psf->Release();
        }
    }

    _AppendOrganizeCommand(punkSite);
    if (psfvcb)
        _EnumLegacyCommands(psfvcb);

    IUnknown *punkView = NULL;

    if (SUCCEEDED(_GetView(punkSite, IID_PPV_ARGS(&punkView))))
    {
        PIDLIST_ABSOLUTE pidlView;

        if (SUCCEEDED(SHGetIDListFromObject(punkView, &pidlView)))
        {
            FOLDERTYPEID ftid;

            if (SUCCEEDED(GetShellFolderType(pidlView, &ftid)))
            {
                CFolderTypeCommandProvider *pProvider = new CFolderTypeCommandProvider(ftid);

                if (pProvider)
                {
                    IEnumExplorerCommand *penumType = NULL;

                    if (SUCCEEDED(pProvider->GetCommands(punkSite, IID_PPV_ARGS(&penumType))))
                        _AppendCommands(penumType);
                    if (penumType)
                        penumType->Release();
                    pProvider->Release();
                }
            }
            CoTaskMemFree(pidlView);
        }
        punkView->Release();
    }

    if (penumFolder)
        _AppendCommands(penumFolder);

    /* A command listed twice stays where it first appeared */
    for (UINT i = 0; i < _cEntries; i++)
    {
        GUID guidFirst;

        if (!_pEntries[i].pec || FAILED(_pEntries[i].pec->GetCanonicalName(&guidFirst)) ||
            IsEqualGUID(guidFirst, GUID_NULL))
        {
            continue;
        }

        for (UINT j = _cEntries - 1; j > i; j -= 1)
        {
            GUID guidLater;

            if (_pEntries[j].pec && SUCCEEDED(_pEntries[j].pec->GetCanonicalName(&guidLater)) &&
                IsEqualGUID(guidFirst, guidLater))
            {
                _RemoveEntry(j);
            }
        }
    }

    _AdjustPosition();
    _AppendRightSideCommands(punkSite);

    if (pfv)
        pfv->Release();
    if (penumFolder)
        penumFolder->Release();
    if (psfvcb)
        psfvcb->Release();
    return _cEntries ? S_OK : E_FAIL;
}

/* Moves the commands that ask for it last, or before or after another command */
VOID
CCommandFolder::_AdjustPosition()
{
    UINT cEntries = _cEntries;
    UINT i = 0;

    while (i < cEntries && i < _cEntries)
    {
        COMMANDFOLDERENTRY *pEntry = &_pEntries[i];
        IRegDataDrivenCommand *prddc;
        COMMANDPOSITION position;
        GUID guidRelative;
        UINT iTarget = (UINT)-1;

        if (pEntry->fPositioned || !pEntry->pec ||
            FAILED(pEntry->pec->QueryInterface(IID_PPV_ARGS(&prddc))))
        {
            i++;
            continue;
        }

        if (SUCCEEDED(prddc->GetPosition(&position, &guidRelative)) && position != CMDPOSITION_DEFAULT)
        {
            UINT iRelative;

            if (position == CMDPOSITION_LAST)
                iTarget = cEntries;
            else if (SUCCEEDED(_FindByGUID(guidRelative, NULL, &iRelative)))
                iTarget = (position == CMDPOSITION_BEFORE) ? iRelative : iRelative + 1;
        }
        prddc->Release();

        if (iTarget != (UINT)-1 && iTarget != i && iTarget != i + 1)
        {
            COMMANDFOLDERENTRY entry = *pEntry;

            entry.pec->AddRef();
            _RemoveEntry(i);
            entry.fPositioned = TRUE;
            _InsertEntry((iTarget > i) ? iTarget - 1 : iTarget, &entry);
            entry.pec->Release();

            /* The next command moved into this slot */
            if (iTarget > i)
                continue;
        }
        i++;
    }
}

HRESULT
CCommandFolder::_AppendOrganizeCommand(
    _In_opt_ IUnknown *punkSite)
{
    EXPLORERPANE ep = EP_Commands_Organize;
    EXPLORERPANESTATE eps = EPS_DONTCARE;
    IExplorerCommand *pec;
    HRESULT hr = S_OK;

    IUnknown_GetExplorerPaneState(punkSite, &ep, &eps);
    if (eps & EPS_DEFAULT_OFF_BIT)
        return hr;

    hr = GetCanonicalCommand(L"Windows.organize", IID_PPV_ARGS(&pec));
    if (SUCCEEDED(hr))
    {
        hr = _AddEntry(pec, 0);
        pec->Release();
    }
    return hr;
}

HRESULT
CCommandFolder::_AppendRightSideCommands(
    _In_opt_ IUnknown *punkSite)
{
    EXPLORERPANE ep = EP_Commands_View;
    EXPLORERPANESTATE eps = EPS_DONTCARE;
    IEnumUICommand *penum = NULL;
    HRESULT hr;

    IUnknown_GetExplorerPaneState(punkSite, &ep, &eps);
    if (eps & EPS_DEFAULT_OFF_BIT)
        return S_OK;

    /* Windows adds the library arrangement command here too, hidden from the bar; ReactOS has no top views */
    hr = LegacyEnumSpecialTasksByType(6, &penum);
    if (SUCCEEDED(hr))
        hr = _AppendLegacyCommands(penum);
    if (penum)
        penum->Release();
    return hr;
}

/* Web view tasks of the view callback: the folder tasks, then the special tasks */
HRESULT
CCommandFolder::_EnumLegacyCommands(
    _In_ IShellFolderViewCB *psfvcb)
{
    WEBVIEWTASKS tasks = { NULL, NULL, 0 };
    HRESULT hr;

    hr = psfvcb->MessageSFVCB(SFVM_GET_WEBVIEW_TASKS, 0, (LPARAM)&tasks);
    if (SUCCEEDED(hr))
    {
        if (tasks.penumFolderTasks)
            hr = _AppendLegacyCommands(tasks.penumFolderTasks);
        if (tasks.penumSpecialTasks)
            hr = _AppendLegacyCommands(tasks.penumSpecialTasks);
    }

    if (tasks.penumFolderTasks)
        tasks.penumFolderTasks->Release();
    if (tasks.penumSpecialTasks)
        tasks.penumSpecialTasks->Release();
    return hr;
}

HRESULT
CCommandFolder::_AppendLegacyCommands(
    _In_ IEnumUICommand *penum)
{
    IEnumExplorerCommand *penumCommands = NULL;
    HRESULT hr;

    hr = penum->QueryInterface(IID_PPV_ARGS(&penumCommands));
    if (FAILED(hr))
        hr = CEnumExplorerCommandOnEnumUICommand_CreateInstance(penum, &penumCommands);
    if (SUCCEEDED(hr))
        hr = _AppendCommands(penumCommands);

    if (penumCommands)
        penumCommands->Release();
    return hr;
}

HRESULT
CCommandFolder::_AppendCommands(
    _In_ IEnumExplorerCommand *penum)
{
    IExplorerCommand *pec;

    while (penum->Next(1, &pec, NULL) == S_OK)
    {
        _AddEntry(pec, 0);
        pec->Release();
    }
    return S_OK;
}

HRESULT
CCommandFolder::_FindByGUID(
    _In_ REFGUID rguidCanonical,
    _COM_Outptr_opt_ IExplorerCommand **ppec,
    _Out_opt_ UINT *piEntry)
{
    HRESULT hr = E_FAIL;

    if (ppec)
        *ppec = NULL;
    if (piEntry)
        *piEntry = (UINT)-1;

    for (UINT i = 0; i < _cEntries && FAILED(hr); i++)
    {
        GUID guid;

        if (!_pEntries[i].pec || FAILED(_pEntries[i].pec->GetCanonicalName(&guid)) ||
            !IsEqualGUID(guid, rguidCanonical))
        {
            continue;
        }

        hr = ppec ? _pEntries[i].pec->QueryInterface(IID_PPV_ARGS(ppec)) : S_OK;
        if (SUCCEEDED(hr) && piEntry)
            *piEntry = i;
    }
    return hr;
}

/* The command an item id names; the slot is tried first, then the canonical name */
HRESULT
CCommandFolder::_GetCommandFromItemID(
    _In_ PCUITEMID_CHILD pidl,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    const COMMANDITEMID *pItem = CommandItemFromPidl(pidl);
    IExplorerCommand *pec = NULL;
    HRESULT hr;

    *ppv = NULL;
    if (!pItem)
        return E_INVALIDARG;

    if (SUCCEEDED(GetCommandsFromSite(_punkSite)) && pItem->iCommand < _cEntries)
    {
        GUID guid;

        pec = _pEntries[pItem->iCommand].pec;
        if (pec)
        {
            pec->AddRef();
            if (FAILED(pec->GetCanonicalName(&guid)) || !IsEqualGUID(guid, pItem->guidCanonical))
            {
                pec->Release();
                pec = NULL;
            }
        }
    }

    hr = pec ? S_OK : _FindByGUID(pItem->guidCanonical, &pec, NULL);
    if (SUCCEEDED(hr))
        hr = pec->QueryInterface(riid, ppv);

    if (pec)
        pec->Release();
    return hr;
}

/* Commands with sub commands are folders of those sub commands */
HRESULT
CCommandFolder::_CreateFolder(
    _In_ PCUITEMID_CHILD pidl,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    IExplorerCommand *pec;
    EXPCMDFLAGS dwFlags;
    HRESULT hr;

    *ppv = NULL;
    hr = _GetCommandFromItemID(pidl, IID_PPV_ARGS(&pec));
    if (FAILED(hr))
        return hr;

    hr = pec->GetFlags(&dwFlags);
    if (SUCCEEDED(hr) && !(dwFlags & ECF_HASSUBCOMMANDS))
        hr = E_NOINTERFACE;

    if (SUCCEEDED(hr))
    {
        IEnumExplorerCommand *penum;

        hr = pec->EnumSubCommands(&penum);
        if (SUCCEEDED(hr))
        {
            PIDLIST_ABSOLUTE pidlFolder = ILCombine(_pidl, pidl);

            hr = pidlFolder ? S_OK : E_OUTOFMEMORY;
            if (SUCCEEDED(hr))
            {
                CCommandFolder *pFolder = new CCommandFolder();

                if (pFolder)
                {
                    hr = pFolder->Initialize(pidlFolder);
                    if (SUCCEEDED(hr))
                        pFolder->_AppendCommands(penum);
                    if (SUCCEEDED(hr))
                        hr = pFolder->QueryInterface(riid, ppv);
                    pFolder->Release();
                }
                else
                {
                    hr = E_OUTOFMEMORY;
                }
                CoTaskMemFree(pidlFolder);
            }
            penum->Release();
        }
    }
    pec->Release();
    return hr;
}

HRESULT
CCommandFolder::_InvokeItem(
    _In_ IDataObject *pdtobj)
{
    IExplorerCommand *pec;
    STGMEDIUM medium;
    FORMATETC fmte = { (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_SHELLIDLIST), NULL, DVASPECT_CONTENT, -1,
                       TYMED_HGLOBAL };
    HRESULT hr;

    /* The data object holds the folder and the one command item */
    hr = pdtobj->GetData(&fmte, &medium);
    if (FAILED(hr))
        return hr;

    CIDA *pcida = (CIDA *)GlobalLock(medium.hGlobal);
    PCUITEMID_CHILD pidl = NULL;

    if (pcida && pcida->cidl)
        pidl = (PCUITEMID_CHILD)((BYTE *)pcida + pcida->aoffset[1]);

    hr = pidl ? _GetCommandFromItemID(pidl, IID_PPV_ARGS(&pec)) : E_FAIL;
    if (SUCCEEDED(hr))
    {
        IShellItemArray *psia = NULL;

        hr = GetSelectionFromSite(_punkSite, &psia);
        if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        {
            IUnknown_SetSite(pec, _punkSite);
            hr = pec->Invoke(psia, NULL);
            IUnknown_SetSite(pec, NULL);
        }
        if (psia)
            psia->Release();
        pec->Release();
    }

    if (pcida)
        GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
    return hr;
}

STDMETHODIMP
CCommandFolder::ParseDisplayName(
    HWND hwnd,
    IBindCtx *pbc,
    LPWSTR pszDisplayName,
    ULONG *pchEaten,
    PIDLIST_RELATIVE *ppidl,
    ULONG *pdwAttributes)
{
    GUID guidCanonical;
    UINT iCommand;
    HRESULT hr;

    *ppidl = NULL;
    if (!GUIDFromStringW(pszDisplayName, &guidCanonical))
        return E_FAIL;

    hr = GetCommandsFromSite(_punkSite);
    if (SUCCEEDED(hr))
        hr = _FindByGUID(guidCanonical, NULL, &iCommand);
    if (SUCCEEDED(hr))
        hr = CreateItemID(guidCanonical, iCommand, (PITEMID_CHILD *)ppidl);

    if (SUCCEEDED(hr) && pdwAttributes)
        GetAttributesOf(1, (PCUITEMID_CHILD_ARRAY)ppidl, pdwAttributes);
    return hr;
}

STDMETHODIMP
CCommandFolder::EnumObjects(
    HWND hwnd,
    SHCONTF grfFlags,
    IEnumIDList **ppenumIDList)
{
    CCommandFolderEnum *penum;
    HRESULT hr;

    *ppenumIDList = NULL;
    penum = new CCommandFolderEnum(this, grfFlags);
    if (!penum)
        return E_OUTOFMEMORY;

    hr = penum->QueryInterface(IID_PPV_ARGS(ppenumIDList));
    penum->Release();
    return hr;
}

STDMETHODIMP
CCommandFolder::BindToObject(
    PCUIDLIST_RELATIVE pidl,
    IBindCtx *pbc,
    REFIID riid,
    void **ppv)
{
    PITEMID_CHILD pidlFirst;
    PCUIDLIST_RELATIVE pidlNext;
    HRESULT hr;

    *ppv = NULL;
    hr = SHILCloneFirst(pidl, &pidlFirst);
    if (FAILED(hr))
        return hr;

    pidlNext = ILGetNext(pidl);
    if (pidlNext && pidlNext->mkid.cb)
    {
        IShellFolder *psf;

        hr = _CreateFolder(pidlFirst, IID_PPV_ARGS(&psf));
        if (SUCCEEDED(hr))
        {
            hr = psf->BindToObject(pidlNext, pbc, riid, ppv);
            psf->Release();
        }
    }
    else if (IsEqualIID(riid, IID_IShellFolder))
    {
        hr = _CreateFolder(pidlFirst, riid, ppv);
    }
    else if (IsEqualIID(riid, IID_IExplorerCommand))
    {
        hr = _GetCommandFromItemID(pidlFirst, riid, ppv);
    }
    else
    {
        hr = E_NOINTERFACE;
    }

    CoTaskMemFree(pidlFirst);
    return hr;
}

STDMETHODIMP
CCommandFolder::BindToStorage(
    PCUIDLIST_RELATIVE pidl,
    IBindCtx *pbc,
    REFIID riid,
    void **ppv)
{
    *ppv = NULL;
    return E_NOTIMPL;
}

/* Same canonical name compares equal; otherwise the slot order decides */
STDMETHODIMP
CCommandFolder::CompareIDs(
    LPARAM lParam,
    PCUIDLIST_RELATIVE pidl1,
    PCUIDLIST_RELATIVE pidl2)
{
    const COMMANDITEMID *pItem1 = CommandItemFromPidl(pidl1);
    const COMMANDITEMID *pItem2 = CommandItemFromPidl(pidl2);

    if (lParam != SHCIDS_CANONICALONLY)
    {
        if (!pItem1 || !pItem2)
            return E_INVALIDARG;
        if (pItem1->iCommand == pItem2->iCommand)
            return ResultFromShort(0);
        return ResultFromShort(pItem1->iCommand < pItem2->iCommand ? -1 : 1);
    }

    if (!pItem1 || !pItem2)
        return E_INVALIDARG;
    if (IsEqualGUID(pItem1->guidCanonical, pItem2->guidCanonical))
        return S_OK;
    if (pItem2->iCommand == pItem1->iCommand)
        return E_INVALIDARG;
    return (pItem2->iCommand < pItem1->iCommand) ? 1 : (HRESULT)-1;
}

STDMETHODIMP
CCommandFolder::CreateViewObject(
    HWND hwndOwner,
    REFIID riid,
    void **ppv)
{
    *ppv = NULL;
    if (IsEqualIID(riid, IID_IShellView))
    {
        SFV_CREATE sfvc = { sizeof(sfvc), static_cast<IShellFolder *>(this), NULL, NULL };

        return SHCreateShellFolderView(&sfvc, (IShellView **)ppv);
    }

    if (IsEqualIID(riid, IID_IContextMenu))
    {
        DEFCONTEXTMENU dcm = { hwndOwner, NULL, NULL, static_cast<IShellFolder *>(this), 0, NULL, NULL, 0, NULL };

        return SHCreateDefaultContextMenu(&dcm, riid, ppv);
    }
    return E_NOINTERFACE;
}

STDMETHODIMP
CCommandFolder::GetAttributesOf(
    UINT cidl,
    PCUITEMID_CHILD_ARRAY apidl,
    SFGAOF *rgfInOut)
{
    SFGAOF sfgao = *rgfInOut;

    if (!cidl || !apidl)
        return E_INVALIDARG;

    for (UINT i = 0; i < cidl; i++)
    {
        SFGAOF sfgaoItem = 0;
        IExplorerCommand *pec;

        /* A command is a folder when it has sub commands */
        if ((*rgfInOut & SFGAO_FOLDER) && SUCCEEDED(_GetCommandFromItemID(apidl[i], IID_PPV_ARGS(&pec))))
        {
            EXPCMDFLAGS dwFlags;

            if (SUCCEEDED(pec->GetFlags(&dwFlags)) && (dwFlags & ECF_HASSUBCOMMANDS))
                sfgaoItem = SFGAO_FOLDER;
            pec->Release();
        }
        sfgao &= sfgaoItem;
    }

    *rgfInOut = sfgao;
    return S_OK;
}

STDMETHODIMP
CCommandFolder::GetUIObjectOf(
    HWND hwndOwner,
    UINT cidl,
    PCUITEMID_CHILD_ARRAY apidl,
    REFIID riid,
    UINT *rgfReserved,
    void **ppv)
{
    HRESULT hr = E_NOTIMPL;

    *ppv = NULL;
    if (IsEqualIID(riid, IID_IDataObject))
        return SHCreateDataObject(_pidl, cidl, apidl, NULL, riid, ppv);

    if (IsEqualIID(riid, IID_IContextMenu))
    {
        DEFCONTEXTMENU dcm = { hwndOwner, static_cast<IContextMenuCB *>(this), NULL,
                               static_cast<IShellFolder *>(this), cidl, apidl, NULL, 0, NULL };

        return SHCreateDefaultContextMenu(&dcm, riid, ppv);
    }

    if (cidl == 1 && (IsEqualIID(riid, IID_IExtractIconA) || IsEqualIID(riid, IID_IExtractIconW)))
    {
        IExplorerCommand *pec;

        hr = _GetCommandFromItemID(apidl[0], IID_PPV_ARGS(&pec));
        if (SUCCEEDED(hr))
        {
            PWSTR pszIcon = NULL;

            hr = pec->GetIcon(NULL, &pszIcon);
            if (hr == S_OK)
            {
                IDefaultExtractIconInit *pdxi = NULL;
                EXPCMDFLAGS dwFlags;
                UINT uFlags = GIL_PERINSTANCE;

                if (SUCCEEDED(pec->GetFlags(&dwFlags)))
                    uFlags |= (dwFlags & ECF_HASLUASHIELD) ? GIL_SHIELD : GIL_FORCENOSHIELD;

                hr = SHCreateDefaultExtractIcon(IID_PPV_ARGS(&pdxi));
                if (SUCCEEDED(hr))
                    hr = pdxi->SetFlags(uFlags);
                if (SUCCEEDED(hr))
                {
                    int iIcon = PathParseIconLocationW(pszIcon);

                    hr = pdxi->SetNormalIcon(pszIcon, iIcon);
                }
                if (SUCCEEDED(hr))
                    hr = pdxi->QueryInterface(riid, ppv);
                if (pdxi)
                    pdxi->Release();
            }
            else if (SUCCEEDED(hr))
            {
                hr = E_FAIL;
            }
            CoTaskMemFree(pszIcon);
            pec->Release();
        }
    }
    return hr;
}

STDMETHODIMP
CCommandFolder::GetDisplayNameOf(
    PCUITEMID_CHILD pidl,
    SHGDNF uFlags,
    STRRET *pName)
{
    IExplorerCommand *pec;
    HRESULT hr;

    ZeroMemory(pName, sizeof(*pName));
    hr = _GetCommandFromItemID(pidl, IID_PPV_ARGS(&pec));
    if (FAILED(hr))
        return hr;

    IUnknown_SetSite(pec, _punkSite);
    if (!(uFlags & SHGDN_FORPARSING))
    {
        IShellItemArray *psia = NULL;

        hr = GetSelectionFromSite(_punkSite, &psia);
        if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        {
            hr = pec->GetTitle(psia, &pName->pOleStr);
            if (SUCCEEDED(hr))
                pName->uType = STRRET_WSTR;
        }
        if (psia)
            psia->Release();
    }
    else
    {
        GUID guidCanonical;

        hr = pec->GetCanonicalName(&guidCanonical);
        if (SUCCEEDED(hr))
        {
            WCHAR szGuid[39];
            WCHAR szPath[MAX_PATH];

            SHStringFromGUIDW(guidCanonical, szGuid, _countof(szGuid));
            if (uFlags & SHGDN_INFOLDER)
            {
                hr = StringToStrRet(szGuid, pName);
            }
            else
            {
                hr = SHGetNameAndFlagsW(_pidl, SHGDN_FORPARSING, szPath, _countof(szPath), NULL);
                if (SUCCEEDED(hr))
                    hr = PathAppendW(szPath, szGuid) ? S_OK : E_FAIL;
                if (SUCCEEDED(hr))
                    hr = StringToStrRet(szPath, pName);
            }
        }
    }
    IUnknown_SetSite(pec, NULL);
    pec->Release();
    return hr;
}

STDMETHODIMP
CCommandFolder::SetNameOf(
    HWND hwnd,
    PCUITEMID_CHILD pidl,
    LPCWSTR pszName,
    SHGDNF uFlags,
    PITEMID_CHILD *ppidlOut)
{
    if (ppidlOut)
        *ppidlOut = NULL;
    return E_NOTIMPL;
}

STDMETHODIMP
CCommandFolder::GetDefaultSearchGUID(
    GUID *pguid)
{
    return E_NOTIMPL;
}

STDMETHODIMP
CCommandFolder::EnumSearches(
    IEnumExtraSearch **ppenum)
{
    *ppenum = NULL;
    return E_NOTIMPL;
}

STDMETHODIMP
CCommandFolder::GetDefaultColumn(
    DWORD dwRes,
    ULONG *pSort,
    ULONG *pDisplay)
{
    return E_NOTIMPL;
}

/* One column: the command's name */
STDMETHODIMP
CCommandFolder::GetDefaultColumnState(
    UINT iColumn,
    SHCOLSTATEF *pcsFlags)
{
    if (iColumn)
        return E_INVALIDARG;
    *pcsFlags = SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT;
    return S_OK;
}

/* Item ids hold no properties, so no detail can be read from one */
STDMETHODIMP
CCommandFolder::GetDetailsEx(
    PCUITEMID_CHILD pidl,
    const SHCOLUMNID *pscid,
    VARIANT *pv)
{
    VariantInit(pv);
    return E_INVALIDARG;
}

STDMETHODIMP
CCommandFolder::GetDetailsOf(
    PCUITEMID_CHILD pidl,
    UINT iColumn,
    SHELLDETAILS *psd)
{
    if (iColumn)
        return E_INVALIDARG;

    psd->fmt = LVCFMT_LEFT;
    psd->cxChar = 20;
    if (!pidl)
        return StringToStrRet(L"Name", &psd->str);
    return GetDisplayNameOf(pidl, SHGDN_INFOLDER, &psd->str);
}

STDMETHODIMP
CCommandFolder::MapColumnToSCID(
    UINT iColumn,
    SHCOLUMNID *pscid)
{
    if (iColumn)
        return E_INVALIDARG;
    pscid->fmtid = PKEY_ItemNameDisplay.fmtid;
    pscid->pid = PKEY_ItemNameDisplay.pid;
    return S_OK;
}

STDMETHODIMP
CCommandFolder::GetClassID(
    CLSID *pClassID)
{
    *pClassID = CLSID_CommandFolder;
    return S_OK;
}

STDMETHODIMP
CCommandFolder::Initialize(
    PCIDLIST_ABSOLUTE pidl)
{
    CoTaskMemFree(_pidl);
    _pidl = NULL;
    if (!pidl)
        return E_INVALIDARG;

    _pidl = ILClone(pidl);
    return _pidl ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP
CCommandFolder::GetCurFolder(
    PIDLIST_ABSOLUTE *ppidl)
{
    *ppidl = NULL;
    if (!_pidl)
        return E_FAIL;
    *ppidl = ILClone(_pidl);
    return *ppidl ? S_OK : E_OUTOFMEMORY;
}

/* The item menu only has "Invoke", which runs the command on the view's selection */
STDMETHODIMP
CCommandFolder::CallBack(
    IShellFolder *psf,
    HWND hwndOwner,
    IDataObject *pdtobj,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (uMsg)
    {
        case DFM_MERGECONTEXTMENU:
            MergeMenuResource(IDM_COMMANDITEM, (QCMINFO *)lParam);
            return S_OK;
        case DFM_INVOKECOMMAND:
            if (wParam == IDC_COMMANDITEM_INVOKE)
                _InvokeItem(pdtobj);
            return S_FALSE;
    }
    return E_NOTIMPL;
}

/* CCommandFolderEnum */

CCommandFolderEnum::CCommandFolderEnum(
    _In_ CCommandFolder *pFolder,
    _In_ SHCONTF grfFlags) :
    _cRef(1),
    _pFolder(pFolder),
    _iNext(0),
    _grfFlags(grfFlags)
{
    _pFolder->AddRef();
}

CCommandFolderEnum::~CCommandFolderEnum()
{
    _pFolder->Release();
}

STDMETHODIMP
CCommandFolderEnum::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CCommandFolderEnum, IEnumIDList),
        QITABENT(CCommandFolderEnum, IObjectWithSite),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CCommandFolderEnum::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CCommandFolderEnum::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

/* Lists the folder's commands as built for the enumerator's site; hidden entries need SHCONTF_INCLUDEHIDDEN */
BOOL
CCommandFolderEnum::_Next(
    _Outptr_ PITEMID_CHILD *ppidl)
{
    BOOL fFound = FALSE;

    *ppidl = NULL;
    if (FAILED(_pFolder->GetCommandsFromSite(_punkSite)))
        return FALSE;

    while (!fFound && _iNext < _pFolder->GetEntryCount())
    {
        COMMANDFOLDERENTRY entry;

        _pFolder->GetEntry(_iNext, &entry);
        if (!(entry.dwEntryFlags & CFEF_HIDDEN) || (_grfFlags & (SHCONTF_INCLUDEHIDDEN | SHCONTF_INCLUDESUPERHIDDEN)))
        {
            IExplorerCommand *pec;

            if (entry.pec && SUCCEEDED(entry.pec->QueryInterface(IID_PPV_ARGS(&pec))))
            {
                GUID guidCanonical = GUID_NULL;

                if (SUCCEEDED(pec->GetCanonicalName(&guidCanonical)) &&
                    SUCCEEDED(_pFolder->CreateItemID(guidCanonical, _iNext, ppidl)))
                {
                    fFound = TRUE;
                }
                pec->Release();
            }
        }
        _iNext++;
    }
    return fFound;
}

STDMETHODIMP
CCommandFolderEnum::Next(
    ULONG celt,
    PITEMID_CHILD *rgelt,
    ULONG *pceltFetched)
{
    ULONG cFetched = 0;

    while (cFetched < celt && _Next(&rgelt[cFetched]))
        cFetched++;

    if (pceltFetched)
        *pceltFetched = cFetched;
    return (cFetched == celt) ? S_OK : S_FALSE;
}

STDMETHODIMP
CCommandFolderEnum::Skip(
    ULONG celt)
{
    for (ULONG i = 0; i < celt; i++)
    {
        PITEMID_CHILD pidl;

        if (!_Next(&pidl))
            return S_FALSE;
        CoTaskMemFree(pidl);
    }
    return S_OK;
}

STDMETHODIMP
CCommandFolderEnum::Reset()
{
    _iNext = 0;
    return S_OK;
}

STDMETHODIMP
CCommandFolderEnum::Clone(
    IEnumIDList **ppenum)
{
    *ppenum = NULL;
    return E_NOTIMPL;
}
