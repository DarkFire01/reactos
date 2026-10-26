/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Drop-down menus of the command bar, built from command folders
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

static const WCHAR c_szTrackContextMenuClass[] = L"TrackContextMenuClass";

/* Removes leading, trailing and doubled separators */
static
VOID
SHPrettyMenu(
    _In_ HMENU hmenu)
{
    BOOL fLastWasSeparator = TRUE;

    for (int i = 0; i < GetMenuItemCount(hmenu); )
    {
        MENUITEMINFOW mii = { sizeof(mii) };

        mii.fMask = MIIM_FTYPE;
        if (GetMenuItemInfoW(hmenu, i, TRUE, &mii) && (mii.fType & MFT_SEPARATOR))
        {
            if (fLastWasSeparator)
            {
                DeleteMenu(hmenu, i, MF_BYPOSITION);
                continue;
            }
            fLastWasSeparator = TRUE;
        }
        else
        {
            fLastWasSeparator = FALSE;
        }
        i++;
    }

    int cItems = GetMenuItemCount(hmenu);

    if (cItems > 0 && fLastWasSeparator)
        DeleteMenu(hmenu, cItems - 1, MF_BYPOSITION);
}

static
VOID
SetICIKeyModifiers(
    _Inout_ DWORD *pfMask)
{
    if (GetKeyState(VK_SHIFT) < 0)
        *pfMask |= CMIC_MASK_SHIFT_DOWN;
    if (GetKeyState(VK_CONTROL) < 0)
        *pfMask |= CMIC_MASK_CONTROL_DOWN;
}

/* Turns a system image list icon into a 32 bit bitmap a menu item can show */
static
HRESULT
CreateBitmapFromIcon(
    _In_ int iImage,
    _In_ const SIZE *psize,
    _Out_ HBITMAP *phbmp)
{
    BITMAPINFO bmi = { { sizeof(bmi.bmiHeader) } };
    HIMAGELIST himl = NULL;
    HBITMAP hbmp;
    HDC hdc;
    void *pvBits;
    HRESULT hr = E_FAIL;

    *phbmp = NULL;
    if (!Shell_GetImageLists(NULL, &himl) || !himl)
        return hr;

    bmi.bmiHeader.biWidth = psize->cx;
    bmi.bmiHeader.biHeight = -psize->cy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hdc = CreateCompatibleDC(NULL);
    if (!hdc)
        return hr;

    hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pvBits, NULL, 0);
    if (hbmp)
    {
        HGDIOBJ hbmpOld = SelectObject(hdc, hbmp);
        HICON hicon = ImageList_GetIcon(himl, iImage, ILD_TRANSPARENT);

        ZeroMemory(pvBits, psize->cx * psize->cy * 4);
        if (hicon)
        {
            DrawIconEx(hdc, 0, 0, hicon, psize->cx, psize->cy, 0, NULL, DI_NORMAL);
            DestroyIcon(hicon);
            hr = S_OK;
        }
        SelectObject(hdc, hbmpOld);

        if (SUCCEEDED(hr))
            *phbmp = hbmp;
        else
            DeleteObject(hbmp);
    }
    DeleteDC(hdc);
    return hr;
}

static
BOOL
SHEnableMenuCheckMarkOrBmp(
    _In_ HMENU hmenu,
    _In_ BOOL fEnable)
{
    MENUINFO mi = { sizeof(mi) };

    mi.fMask = MIM_STYLE;
    if (!GetMenuInfo(hmenu, &mi))
        return FALSE;

    if (fEnable)
        mi.dwStyle |= MNS_CHECKORBMP;
    else
        mi.dwStyle &= ~MNS_CHECKORBMP;
    return SetMenuInfo(hmenu, &mi);
}

/* Whether a menu text's mnemonic, or its first letter when it has none, is chKey */
static
BOOL
MenuCharMatch(
    _In_ PCWSTR pszText,
    _In_ WCHAR chKey)
{
    PCWSTR psz = pszText;

    while ((psz = StrChrW(psz, L'&')) != NULL)
    {
        if (psz[1] != L'&')
            return psz[1] && ChrCmpIW(psz[1], chKey) == 0;
        psz += 2;
    }
    return pszText[0] && ChrCmpIW(pszText[0], chKey) == 0;
}

/* Hidden owner of a tracked menu; passes the menu's messages to the IContextMenu3 behind it */
static
LRESULT
CALLBACK
TrackContextMenuWndProc(
    HWND hwnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    IContextMenu *pcm;

    if (uMsg == WM_NCCREATE)
    {
        pcm = (IContextMenu *)((CREATESTRUCTW *)lParam)->lpCreateParams;
        if (!pcm)
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
        pcm->AddRef();
        SetWindowLongPtrW(hwnd, 0, (LONG_PTR)pcm);
    }
    else
    {
        pcm = (IContextMenu *)GetWindowLongPtrW(hwnd, 0);
    }

    if (!pcm)
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);

    switch (uMsg)
    {
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, 0, 0);
            pcm->Release();
            break;

        case WM_DRAWITEM:
        case WM_MEASUREITEM:
        case WM_INITMENUPOPUP:
        case WM_MENUCHAR:
        {
            LRESULT lResult;

            if (SUCCEEDED(SHForwardContextMenuMsg(pcm, uMsg, wParam, lParam, &lResult, FALSE)))
                return lResult;
            break;
        }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static
HWND
CreateWindowForContextMenu(
    _In_opt_ IContextMenu *pcm)
{
    WNDCLASSW wc = { 0 };

    wc.lpfnWndProc = TrackContextMenuWndProc;
    wc.cbWndExtra = sizeof(LONG_PTR);
    wc.hInstance = DUIFRAME_HINST;
    wc.lpszClassName = c_szTrackContextMenuClass;
    RegisterClassW(&wc);

    return SHFusionCreateWindowEx(IsBiDiLocalizedSystem() ? WS_EX_LAYOUTRTL : 0, c_szTrackContextMenuClass, L"",
                                  WS_POPUP, 0, 0, 0, 0, NULL, NULL, NULL, pcm);
}

static
HRESULT
SHTrackContextMenu(
    _In_ IContextMenu *pcm,
    _In_opt_ HWND hwndOwner,
    _In_ UINT uFlags,
    _In_ int x,
    _In_ int y,
    _In_opt_ TPMPARAMS *ptpm)
{
    HMENU hmenu = CreatePopupMenu();
    HWND hwnd = hwndOwner;
    HRESULT hr;

    if (!hmenu)
        return HRESULT_FROM_WIN32(GetLastError());

    hr = pcm->QueryContextMenu(hmenu, 0, 1, 0x7FFF, CMF_NORMAL);
    if (SUCCEEDED(hr) && !hwnd)
    {
        hwnd = CreateWindowForContextMenu(pcm);
        if (!hwnd)
            hr = HRESULT_FROM_WIN32(GetLastError());
    }

    if (SUCCEEDED(hr))
    {
        UINT uTrack = (uFlags & ~TPM_NONOTIFY) | TPM_RETURNCMD;
        UINT idCmd;

        if (IsBiDiLocalizedSystem())
            uTrack |= TPM_LAYOUTRTL;

        SHPrettyMenu(hmenu);
        idCmd = TrackPopupMenuEx(hmenu, uTrack, x, y, hwnd, ptpm);
        if (idCmd)
        {
            CMINVOKECOMMANDINFOEX ici = { sizeof(ici) };

            ici.fMask = CMIC_MASK_FLAG_LOG_USAGE;
            ici.hwnd = hwnd;
            ici.lpVerb = MAKEINTRESOURCEA(idCmd - 1);
            ici.nShow = SW_SHOWNORMAL;
            SetICIKeyModifiers(&ici.fMask);
            pcm->InvokeCommand((CMINVOKECOMMANDINFO *)&ici);
            hr = S_OK;
        }
        else
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            if (SUCCEEDED(hr))
                hr = E_FAIL;
        }

        if (!hwndOwner)
            DestroyWindow(hwnd);
    }

    DestroyMenu(hmenu);
    return hr;
}

/* Drops the menu of a command folder below prcExclude; cSkip commands are already on the bar */
HRESULT
WINAPI
ShowFolderMenu(
    _In_ IShellFolder *psf,
    _In_opt_ IUnknown *punkSite,
    _In_ const RECT *prcExclude,
    _In_ UINT cSkip,
    _In_ BOOL fOverflow)
{
    IContextMenu3 *pcm;
    HWND hwnd;
    HRESULT hr;

    hwnd = CreateWindowForContextMenu(NULL);
    if (!hwnd)
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        return SUCCEEDED(hr) ? E_FAIL : hr;
    }

    hr = CFolderMenu::CreateInstance(psf, cSkip, fOverflow, hwnd, IID_PPV_ARGS(&pcm));
    if (SUCCEEDED(hr))
    {
        BOOL fDropRight = GetSystemMetrics(SM_MENUDROPALIGNMENT);
        MONITORINFO mi = { sizeof(mi) };
        TPMPARAMS tpm = { sizeof(tpm) };
        int x;

        pcm->AddRef();
        SetWindowLongPtrW(hwnd, 0, (LONG_PTR)pcm);
        IUnknown_SetSite(pcm, punkSite);

        CopyRect(&tpm.rcExclude, prcExclude);
        x = (IsBiDiLocalizedSystem() || fDropRight) ? prcExclude->right : prcExclude->left;
        if (GetMonitorInfoW(MonitorFromRect(prcExclude, MONITOR_DEFAULTTONEAREST), &mi))
        {
            if (x < mi.rcMonitor.left)
                x = mi.rcMonitor.left;
            else if (x > mi.rcMonitor.right)
                x = mi.rcMonitor.right;
        }

        hr = SHTrackContextMenu(pcm, hwnd, (fDropRight ? TPM_RIGHTALIGN : 0) | TPM_VERTICAL, x,
                                prcExclude->bottom, &tpm);
        IUnknown_SetSite(pcm, NULL);
        pcm->Release();
    }
    DestroyWindow(hwnd);
    return hr;
}

/* CFolderMenu */

CFolderMenu::CFolderMenu() :
    _cRef(1)
{
}

CFolderMenu::~CFolderMenu()
{
    if (_hdpaItems)
        DPA_DestroyCallback(_hdpaItems, s_FreeMenuItemData, NULL);
    if (_psiaSelection)
        _psiaSelection->Release();
    if (_pdtobj)
        _pdtobj->Release();
    if (_psf)
        _psf->Release();
    if (_hbmShield)
        DeleteObject(_hbmShield);
    if (_penumCommands)
        _penumCommands->Release();
}

HRESULT
WINAPI
CFolderMenu::CreateInstance(
    _In_opt_ IShellFolder *psf,
    _In_ UINT cSkip,
    _In_ BOOL fOverflow,
    _In_opt_ HWND hwnd,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CFolderMenu *pMenu;
    HRESULT hr;

    *ppv = NULL;
    pMenu = new CFolderMenu();
    if (!pMenu)
        return E_OUTOFMEMORY;

    pMenu->_psf = psf;
    if (psf)
        psf->AddRef();
    pMenu->_hwnd = hwnd;
    pMenu->_cSkip = cSkip;
    pMenu->_fOverflow = fOverflow;
    pMenu->_hdpaItems = DPA_Create(4);

    hr = pMenu->QueryInterface(riid, ppv);
    pMenu->Release();
    return hr;
}

STDMETHODIMP
CFolderMenu::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CFolderMenu, IShellExtInit),
        QITABENTMULTI(CFolderMenu, IContextMenu3, IContextMenu3),
        QITABENTMULTI(CFolderMenu, IContextMenu2, IContextMenu3),
        QITABENTMULTI(CFolderMenu, IContextMenu, IContextMenu3),
        QITABENT(CFolderMenu, IObjectWithSite),
        QITABENT(CFolderMenu, IServiceProvider),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CFolderMenu::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CFolderMenu::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

STDMETHODIMP
CFolderMenu::Initialize(
    PCIDLIST_ABSOLUTE pidlFolder,
    IDataObject *pdtobj,
    HKEY hkeyProgID)
{
    IUnknown_Set((IUnknown **)&_pdtobj, pdtobj);
    return S_OK;
}

STDMETHODIMP
CFolderMenu::QueryService(
    REFGUID guidService,
    REFIID riid,
    void **ppv)
{
    return IUnknown_QueryService(_punkSite, guidService, riid, ppv);
}

HRESULT
CFolderMenu::SetEnumExplorerCommand(
    _In_opt_ IEnumExplorerCommand *penum)
{
    IUnknown_Set((IUnknown **)&_penumCommands, penum);
    return S_OK;
}

int
CALLBACK
CFolderMenu::s_FreeMenuItemData(
    _In_ void *p,
    _In_opt_ void *pData)
{
    FOLDERMENUITEM *pItem = (FOLDERMENUITEM *)p;

    if (pItem->pec)
    {
        pItem->pec->Release();
        pItem->pec = NULL;
    }
    if (pItem->hbmp)
        DeleteObject(pItem->hbmp);
    CoTaskMemFree(pItem->pszTitle);
    CoTaskMemFree(pItem->pszToolTip);
    LocalFree(pItem);
    return TRUE;
}

/* WM_MENUCHAR: the next item after the highlight whose mnemonic is chKey */
LRESULT
CFolderMenu::_MatchMenuItem(
    _In_ HMENU hmenu,
    _In_ WCHAR chKey)
{
    int cItems = GetMenuItemCount(hmenu);
    int iHilite = -1;
    int iFound = -1;
    BOOL fMore = FALSE;

    for (int i = 0; i < cItems; i++)
    {
        MENUITEMINFOW mii = { sizeof(mii) };

        mii.fMask = MIIM_STATE;
        if (GetMenuItemInfoW(hmenu, i, TRUE, &mii) && (mii.fState & MFS_HILITE))
        {
            iHilite = i;
            break;
        }
    }

    for (int i = 0; i < cItems; i++)
    {
        MENUITEMINFOW mii = { sizeof(mii) };
        int iItem = (iHilite + 1 + i) % cItems;
        FOLDERMENUITEM *pItem;

        mii.fMask = MIIM_DATA;
        if (!GetMenuItemInfoW(hmenu, iItem, TRUE, &mii) || !mii.dwItemData)
            continue;

        pItem = (FOLDERMENUITEM *)mii.dwItemData;
        if (!pItem->pszTitle || !MenuCharMatch(pItem->pszTitle, chKey))
            continue;

        if (iFound != -1)
        {
            fMore = TRUE;
            break;
        }
        iFound = iItem;
    }

    if (iFound == -1)
        return 0;
    return MAKELRESULT(iFound, fMore ? MNC_SELECT : MNC_EXECUTE);
}

HRESULT
CFolderMenu::_AllocFolderMenuItemData(
    _In_ IExplorerCommand *pec,
    _In_opt_ IShellItemArray *psia,
    _In_ BOOL fWantIcon,
    _Outptr_ FOLDERMENUITEM **ppItem)
{
    FOLDERMENUITEM *pItem;

    *ppItem = NULL;
    pItem = (FOLDERMENUITEM *)LocalAlloc(LPTR, sizeof(*pItem));
    if (!pItem)
        return E_OUTOFMEMORY;

    pec->GetFlags(&pItem->dwFlags);
    pec->GetCanonicalName(&pItem->guidCanonical);
    pec->GetToolTip(psia, &pItem->pszToolTip);
    if (SUCCEEDED(pec->GetTitle(psia, &pItem->pszTitle)))
    {
        pItem->dwSignature = FOLDERMENUITEM_SIGNATURE;
        if (StrChrW(pItem->pszTitle, L'&'))
        {
            pItem->pszMnemonicText = pItem->pszTitle;
        }
        else
        {
            pItem->szMnemonicPrefix[0] = L'&';
            pItem->pszMnemonicText = pItem->szMnemonicPrefix;
        }
        pItem->cchMnemonicText = lstrlenW(pItem->pszMnemonicText);
    }

    /* Overflow menus only show the icon of the default verb */
    if (fWantIcon || IsEqualGUID(pItem->guidCanonical, UICID_DefaultVerb))
    {
        PWSTR pszIcon = NULL;

        if (pec->GetIcon(psia, &pszIcon) == S_OK)
        {
            UINT uFlags = (pItem->dwFlags & ECF_HASLUASHIELD) ? GIL_SHIELD : GIL_FORCENOSHIELD;
            int iImage = Shell_GetCachedImageIndexW(pszIcon, PathParseIconLocationW(pszIcon), uFlags);

            if (iImage != -1)
            {
                SIZE size = { GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON) };

                CreateBitmapFromIcon(iImage, &size, &pItem->hbmp);
            }
        }
        CoTaskMemFree(pszIcon);
    }

    pItem->pec = pec;
    pec->AddRef();
    *ppItem = pItem;
    return S_OK;
}

/* Counts the visible right side commands so the overflow menu can skip what the bar shows */
HRESULT
CFolderMenu::_GetRightSideAndTotalCommand(
    _In_ IEnumIDList *penum,
    _In_ IShellFolder *psf,
    _In_opt_ IShellItemArray *psia,
    _Out_ UINT *pcRightSide,
    _Out_ UINT *pcTotal)
{
    PITEMID_CHILD pidl;
    UINT cRightSide = 0;
    UINT cTotal = 0;

    while (penum->Next(1, &pidl, NULL) == S_OK)
    {
        IExplorerCommand *pec;

        if (SUCCEEDED(psf->BindToObject(pidl, NULL, IID_PPV_ARGS(&pec))))
        {
            GUID guidCanonical;

            if (SUCCEEDED(pec->GetCanonicalName(&guidCanonical)) && IsRightSideCommand(guidCanonical))
            {
                EXPCMDSTATE state = ECS_ENABLED;

                IUnknown_SetSite(pec, _punkSite);
                IUnknown_SetSelection(pec, psia);
                pec->GetState(psia, FALSE, &state);
                IUnknown_SetSite(pec, NULL);
                if (!(state & ECS_HIDDEN))
                    cRightSide++;
            }
            pec->Release();
        }
        cTotal++;
        CoTaskMemFree(pidl);
    }
    penum->Reset();

    *pcRightSide = cRightSide;
    *pcTotal = cTotal;
    return S_OK;
}

/*
 * Adds one command. Sub menus take ids right after their parent's, and *pidCmdBase moves past
 * them, so every item of the tree keeps a distinct id.
 */
HRESULT
CFolderMenu::_InsertCommandToMenu(
    _In_ IExplorerCommand *pec,
    _In_opt_ IShellItemArray *psia,
    _In_ HMENU hmenu,
    _In_ UINT indexMenu,
    _Inout_ UINT *pidCmdBase,
    _In_ UINT idCmdLast,
    _Inout_ UINT *pidCmdMax,
    _Inout_ UINT *pcInserted,
    _Inout_ BOOL *pfSeparatorPending,
    _Inout_ BOOL *pfRightSideSeparated,
    _Inout_ BOOL *pfHasBitmaps,
    _In_ BOOL fWantIcon)
{
    MENUITEMINFOW mii = { sizeof(mii) };
    FOLDERMENUITEM *pItem;
    EXPCMDSTATE state = ECS_ENABLED;
    HRESULT hrState;
    HRESULT hr;

    *pidCmdMax = *pidCmdBase;
    IUnknown_SetSite(pec, _punkSite);
    IUnknown_SetSelection(pec, psia);

    hr = _AllocFolderMenuItemData(pec, psia, fWantIcon, &pItem);
    if (FAILED(hr))
        goto Exit;

    mii.fMask = MIIM_STATE | MIIM_ID | MIIM_DATA | MIIM_STRING | MIIM_FTYPE;
    mii.wID = *pidCmdBase + *pcInserted;
    mii.dwItemData = (ULONG_PTR)pItem;
    mii.dwTypeData = pItem->pszTitle;
    if (pItem->hbmp)
    {
        mii.fMask |= MIIM_BITMAP;
        mii.hbmpItem = pItem->hbmp;
        *pfHasBitmaps = TRUE;
    }

    /* A separator only shows between two commands */
    if (pItem->dwFlags & ECF_ISSEPARATOR)
    {
        if (*pcInserted)
            *pfSeparatorPending = TRUE;
        s_FreeMenuItemData(pItem, NULL);
        goto Exit;
    }

    hrState = pec->GetState(psia, FALSE, &state);
    if (state & ECS_HIDDEN)
    {
        s_FreeMenuItemData(pItem, NULL);
        goto Exit;
    }

    if (state & ECS_DISABLED)
        mii.fState = MFS_DISABLED;
    if (state & ECS_CHECKBOX)
    {
        *pfHasBitmaps = TRUE;
        if (state & ECS_CHECKED)
            mii.fState |= MFS_CHECKED;
    }
    if (state & ECS_RADIOCHECK)
    {
        mii.fType |= MFT_RADIOCHECK;
        mii.fState |= MFS_CHECKED;
        *pfHasBitmaps = TRUE;
    }

    if (*pfSeparatorPending)
    {
        *pfSeparatorPending = FALSE;
        AppendMenuW(hmenu, MF_SEPARATOR, 0, NULL);
        (*pcInserted)++;
    }

    if (pItem->dwFlags & ECF_HASSUBCOMMANDS)
    {
        IEnumExplorerCommand *penumSub;

        hr = pec->EnumSubCommands(&penumSub);
        if (SUCCEEDED(hr))
        {
            mii.fMask |= MIIM_SUBMENU;
            mii.hSubMenu = CreateMenu();
            hr = _ComposeMenuFromEnumCommand(penumSub, psia, mii.hSubMenu, 0, *pcInserted + *pidCmdBase + 1,
                                             idCmdLast, pidCmdBase);
            penumSub->Release();
        }
    }

    {
        BOOL fRightSide = IsRightSideCommand(pItem->guidCanonical);
        UINT iPosition = fRightSide ? GetMenuItemCount(hmenu) : *pcInserted + indexMenu;
        int iItem;

        iItem = DPA_AppendPtr(_hdpaItems, pItem);
        if (iItem == -1 || !InsertMenuItemW(hmenu, iPosition, TRUE, &mii))
        {
            if (iItem != -1)
                DPA_DeletePtr(_hdpaItems, iItem);
            if (mii.hSubMenu)
                DestroyMenu(mii.hSubMenu);
            s_FreeMenuItemData(pItem, NULL);
            goto Exit;
        }

        /* The right side commands are set apart from the rest */
        BOOL fSeparateRightSide = !*pfRightSideSeparated && fRightSide;

        if (iPosition && (fSeparateRightSide || (pItem->dwFlags & ECF_SEPARATORBEFORE)))
        {
            InsertMenuW(hmenu, iPosition, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
            (*pcInserted)++;
            if (fSeparateRightSide)
                *pfRightSideSeparated = TRUE;
        }
    }

    if (pItem->dwFlags & ECF_SEPARATORAFTER)
        *pfSeparatorPending = TRUE;

    /* A state that is slow to work out stays as first shown; Windows updates it from a task */
    UNREFERENCED_PARAMETER(hrState);

    *pidCmdMax = mii.wID;
    (*pcInserted)++;

Exit:
    IUnknown_SetSite(pec, NULL);
    return hr;
}

HRESULT
CFolderMenu::_ComposeMenuFromCommandFolder(
    _In_ IShellFolder *psf,
    _In_opt_ IShellItemArray *psia,
    _In_ HMENU hmenu,
    _In_ UINT indexMenu,
    _In_ UINT idCmdFirst,
    _In_ UINT idCmdLast,
    _Inout_ UINT *pidCmdMax,
    _In_ UINT cSkip)
{
    IEnumIDList *penum;
    UINT cRightSide;
    UINT cTotal;
    UINT cToSkip = 0;
    UINT cHiddenSkipped = 0;
    UINT cShownSkipped = 0;
    PITEMID_CHILD pidl;
    HRESULT hr;

    hr = psf->EnumObjects(NULL, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &penum);
    if (hr != S_OK)
        return hr;

    IUnknown_SetSite(penum, _punkSite);
    _GetRightSideAndTotalCommand(penum, psf, psia, &cRightSide, &cTotal);

    /* The overflow menu starts after the commands the bar has room for */
    if (cSkip > cRightSide)
        cToSkip = cSkip - cRightSide;

    while (hr == S_OK && cShownSkipped < cToSkip)
    {
        hr = penum->Next(1, &pidl, NULL);
        if (hr == S_OK)
        {
            IExplorerCommand *pec;
            EXPCMDSTATE state = ECS_ENABLED;

            if (SUCCEEDED(psf->BindToObject(pidl, NULL, IID_PPV_ARGS(&pec))))
            {
                IUnknown_SetSite(pec, _punkSite);
                IUnknown_SetSelection(pec, psia);
                pec->GetState(psia, FALSE, &state);
                IUnknown_SetSite(pec, NULL);
                pec->Release();
            }

            if (state & ECS_HIDDEN)
                cHiddenSkipped++;
            else
                cShownSkipped++;
            CoTaskMemFree(pidl);
        }
    }

    UINT cInserted = 0;
    BOOL fSeparatorPending = FALSE;
    BOOL fRightSideSeparated = FALSE;
    BOOL fHasBitmaps = FALSE;
    UINT idCmdBase = idCmdFirst;

    for (UINT i = cHiddenSkipped + cSkip; i < cTotal; i++)
    {
        IExplorerCommand *pec;

        if (*pidCmdMax > idCmdLast || penum->Next(1, &pidl, NULL) != S_OK)
            break;

        IUnknown_SetSite(psf, _punkSite);
        hr = psf->BindToObject(pidl, NULL, IID_PPV_ARGS(&pec));
        IUnknown_SetSite(psf, NULL);
        if (SUCCEEDED(hr))
        {
            hr = _InsertCommandToMenu(pec, psia, hmenu, indexMenu, &idCmdBase, idCmdLast, pidCmdMax, &cInserted,
                                      &fSeparatorPending, &fRightSideSeparated, &fHasBitmaps, !_fOverflow);
            pec->Release();
        }
        CoTaskMemFree(pidl);
    }

    IUnknown_SetSite(penum, NULL);
    penum->Release();

    if (fHasBitmaps)
        SHEnableMenuCheckMarkOrBmp(hmenu, TRUE);
    return hr;
}

HRESULT
CFolderMenu::_ComposeMenuFromEnumCommand(
    _In_ IEnumExplorerCommand *penum,
    _In_opt_ IShellItemArray *psia,
    _In_ HMENU hmenu,
    _In_ UINT indexMenu,
    _In_ UINT idCmdFirst,
    _In_ UINT idCmdLast,
    _Inout_ UINT *pidCmdMax)
{
    IExplorerCommand *pec;
    UINT cInserted = 0;
    BOOL fSeparatorPending = FALSE;
    BOOL fRightSideSeparated = FALSE;
    BOOL fHasBitmaps = FALSE;
    UINT idCmdBase = idCmdFirst;
    HRESULT hr = S_OK;

    while (penum->Next(1, &pec, NULL) == S_OK)
    {
        if (*pidCmdMax > idCmdLast)
        {
            pec->Release();
            break;
        }

        hr = _InsertCommandToMenu(pec, psia, hmenu, indexMenu, &idCmdBase, idCmdLast, pidCmdMax, &cInserted,
                                  &fSeparatorPending, &fRightSideSeparated, &fHasBitmaps, TRUE);
        pec->Release();
    }

    if (fHasBitmaps)
        SHEnableMenuCheckMarkOrBmp(hmenu, TRUE);
    return hr;
}

/* Commands run on the data object's items when the menu got one, else on the view's selection */
HRESULT
CFolderMenu::_GetSelection(
    _COM_Outptr_result_maybenull_ IShellItemArray **ppsia)
{
    *ppsia = NULL;
    if (!_psiaSelection && _pdtobj)
        SHCreateShellItemArrayFromDataObject(_pdtobj, IID_PPV_ARGS(&_psiaSelection));

    if (_psiaSelection)
        return _psiaSelection->QueryInterface(IID_PPV_ARGS(ppsia));
    return GetSelectionFromSite(_punkSite, ppsia);
}

STDMETHODIMP
CFolderMenu::QueryContextMenu(
    HMENU hmenu,
    UINT indexMenu,
    UINT idCmdFirst,
    UINT idCmdLast,
    UINT uFlags)
{
    IShellItemArray *psia = NULL;
    UINT idCmdMax = idCmdFirst;
    HRESULT hr;

    _hmenu = hmenu;
    _idCmdFirst = idCmdFirst;

    hr = _GetSelection(&psia);
    if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
    {
        if (_psf)
            _ComposeMenuFromCommandFolder(_psf, psia, hmenu, indexMenu, idCmdFirst, idCmdLast, &idCmdMax, _cSkip);
        else if (_penumCommands)
            _ComposeMenuFromEnumCommand(_penumCommands, psia, hmenu, indexMenu, idCmdFirst, idCmdLast, &idCmdMax);
    }

    if (psia)
        psia->Release();
    return ResultFromShort(idCmdMax - idCmdFirst + 1);
}

FOLDERMENUITEM *
CFolderMenu::_GetItemData(
    _In_ HMENU hmenu,
    _In_ UINT idCmd)
{
    MENUITEMINFOW mii = { sizeof(mii) };

    mii.fMask = MIIM_DATA;
    if (!GetMenuItemInfoW(hmenu, idCmd, FALSE, &mii))
        return NULL;
    return (FOLDERMENUITEM *)mii.dwItemData;
}

/* Registry commands take the full invoke parameters; other commands just run */
STDMETHODIMP
CFolderMenu::InvokeCommand(
    CMINVOKECOMMANDINFO *pici)
{
    FOLDERMENUITEM *pItem;
    IShellItemArray *psia = NULL;
    HRESULT hr;

    if (!IS_INTRESOURCE(pici->lpVerb))
        return E_FAIL;

    pItem = _GetItemData(_hmenu, _idCmdFirst + LOWORD(pici->lpVerb));
    if (!pItem)
        return E_FAIL;

    hr = _GetSelection(&psia);
    if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
    {
        IRegDataDrivenCommand *prddc;

        IUnknown_SetSite(pItem->pec, _punkSite);
        if (FAILED(pItem->pec->QueryInterface(IID_PPV_ARGS(&prddc))))
        {
            hr = pItem->pec->Invoke(psia, NULL);
        }
        else
        {
            CMINVOKECOMMANDINFOEX ici = { sizeof(ici) };

            CopyMemory(&ici, pici, min(pici->cbSize, sizeof(ici)));
            ici.cbSize = sizeof(ici);
            if (pici->cbSize < sizeof(ici))
                ici.fMask &= ~CMIC_MASK_UNICODE;

            hr = prddc->InvokeFromContextMenu(&ici, psia);
            if (hr == E_NOTIMPL)
                hr = pItem->pec->Invoke(psia, NULL);
            prddc->Release();
        }
        IUnknown_SetSite(pItem->pec, NULL);
    }

    if (psia)
        psia->Release();
    return hr;
}

STDMETHODIMP
CFolderMenu::GetCommandString(
    UINT_PTR idCmd,
    UINT uType,
    UINT *pReserved,
    CHAR *pszName,
    UINT cchMax)
{
    FOLDERMENUITEM *pItem = _GetItemData(_hmenu, (UINT)idCmd + _idCmdFirst);
    HRESULT hr = E_NOTIMPL;

    if (!pItem)
        return hr;

    switch (uType)
    {
        case GCS_VERBA:
        case GCS_VERBW:
        {
            IRegDataDrivenCommand *prddc;

            hr = pItem->pec->QueryInterface(IID_PPV_ARGS(&prddc));
            if (SUCCEEDED(hr))
            {
                PWSTR pszVerb;
                HKEY hkey;

                hr = prddc->GetKeyAndName(&hkey, &pszVerb);
                if (SUCCEEDED(hr))
                {
                    if (uType == GCS_VERBW)
                        hr = StringCchCopyW((PWSTR)pszName, cchMax, pszVerb);
                    else
                        hr = SHUnicodeToAnsi(pszVerb, pszName, cchMax) ? S_OK : E_FAIL;
                    RegCloseKey(hkey);
                    CoTaskMemFree(pszVerb);
                }
                prddc->Release();
            }
            break;
        }

        case GCS_HELPTEXTA:
        case GCS_HELPTEXTW:
            if (pItem->pszToolTip)
            {
                if (uType == GCS_HELPTEXTW)
                    hr = StringCchCopyW((PWSTR)pszName, cchMax, pItem->pszToolTip);
                else
                    hr = SHUnicodeToAnsi(pItem->pszToolTip, pszName, cchMax) ? S_OK : E_FAIL;
            }
            break;

        case GCS_VALIDATEA:
        case GCS_VALIDATEW:
            hr = S_OK;
            break;
    }
    return hr;
}

STDMETHODIMP
CFolderMenu::HandleMenuMsg(
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    return HandleMenuMsg2(uMsg, wParam, lParam, NULL);
}

STDMETHODIMP
CFolderMenu::HandleMenuMsg2(
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam,
    LRESULT *plResult)
{
    LRESULT lResult = 0;
    HRESULT hr = S_OK;

    switch (uMsg)
    {
        case WM_INITMENUPOPUP:
            break;
        case WM_MENUCHAR:
            lResult = _MatchMenuItem((HMENU)lParam, LOWORD(wParam));
            break;
        default:
            hr = E_NOTIMPL;
            break;
    }

    if (plResult)
        *plResult = lResult;
    return hr;
}
