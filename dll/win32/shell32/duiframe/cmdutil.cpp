/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Command bar helpers: selections, context menus of a selection, panes, registry
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define PROPERTY_STORE_LOCAL    1

/* Items() flag that keeps the view order */
#define SVGIO_VIEWORDER         SVGIO_FLAG_VIEWORDER

/* Explorer pane state bit set when the frame owns the pane's visibility */
#define EPS_FORCE               0x20000

EXTERN_C
HRESULT
WINAPI
SHCreatePropertyBagOnRegKey(
    _In_ HKEY hkey,
    _In_opt_ PCWSTR pszSubKey,
    _In_ DWORD grfMode,
    _In_ REFIID riid,
    _Outptr_ void **ppv);

HRESULT
WINAPI
IShellItemArray_GetEstimatedCount(
    _In_ IShellItemArray *psia,
    _Out_ DWORD *pcItems)
{
    *pcItems = 0;
    return psia->GetCount(pcItems);
}

HRESULT
WINAPI
IUnknown_QueryCurrentView(
    _In_opt_ IUnknown *punkSite,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    IShellBrowser *psb;
    HRESULT hr;

    hr = IUnknown_QueryService(punkSite, IID_IFolderView, riid, ppv);
    if (SUCCEEDED(hr))
        return hr;

    hr = IUnknown_QueryService(punkSite, SID_STopLevelBrowser, IID_PPV_ARGS(&psb));
    if (SUCCEEDED(hr))
    {
        IShellView *psv;

        hr = psb->QueryActiveShellView(&psv);
        if (SUCCEEDED(hr))
        {
            hr = psv->QueryInterface(riid, ppv);
            psv->Release();
        }
        psb->Release();
    }
    return hr;
}

HRESULT
WINAPI
GetSelectionFromSite(
    _In_opt_ IUnknown *punkSite,
    _COM_Outptr_result_maybenull_ IShellItemArray **ppsia)
{
    IFolderView *pfv;
    HRESULT hr;

    *ppsia = NULL;
    hr = IUnknown_QueryService(punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv));
    if (SUCCEEDED(hr))
    {
        hr = pfv->Items(SVGIO_SELECTION | SVGIO_VIEWORDER, IID_PPV_ARGS(ppsia));
        pfv->Release();
    }
    return hr;
}

/*
 * Turns a command's implied selection model into items. The low nibble picks the
 * folder itself, every item or the background; the high bits prefer the selection.
 */
HRESULT
WINAPI
SHGetItemArrayOfSelection(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ DWORD dwModel,
    _COM_Outptr_ IShellItemArray **ppsia)
{
    IFolderView *pfv = NULL;
    DWORD cItems = 0;
    HRESULT hr;

    *ppsia = NULL;
    if (psia && SUCCEEDED(IShellItemArray_GetEstimatedCount(psia, &cItems)) && cItems)
    {
        if (!(dwModel & SELMODEL_FOLDERIFSELECTION))
            return psia->QueryInterface(IID_PPV_ARGS(ppsia));

        hr = IUnknown_QueryCurrentView(punkSite, IID_PPV_ARGS(&pfv));
        if (SUCCEEDED(hr))
        {
            hr = pfv->GetFolder(IID_PPV_ARGS(ppsia));
            pfv->Release();
        }
        return hr;
    }

    hr = IUnknown_QueryCurrentView(punkSite, IID_PPV_ARGS(&pfv));
    if (FAILED(hr))
        return hr;

    int cSelected = 0;
    UINT uItems;

    if ((dwModel & SELMODEL_PREFERSELECTION) && SUCCEEDED(pfv->ItemCount(SVGIO_SELECTION, &cSelected)) &&
        cSelected)
    {
        uItems = SVGIO_SELECTION | SVGIO_VIEWORDER;
        hr = pfv->Items(uItems, IID_PPV_ARGS(ppsia));
    }
    else
    {
        switch (dwModel & SELMODEL_TYPEMASK)
        {
            case SELMODEL_NONE:
                hr = E_FAIL;
                break;
            case SELMODEL_FOLDER:
                hr = pfv->GetFolder(IID_PPV_ARGS(ppsia));
                break;
            case SELMODEL_ALLITEMS:
                uItems = SVGIO_ALLVIEW | SVGIO_VIEWORDER;
                hr = pfv->Items(uItems, IID_PPV_ARGS(ppsia));
                break;
            default:
                hr = E_INVALIDARG;
                break;
        }
    }

    pfv->Release();
    return hr;
}

HRESULT
WINAPI
SHGetContextMenuOfSelection(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ DWORD dwModel,
    _COM_Outptr_ IContextMenu **ppcm)
{
    IShellView *psv = NULL;
    DWORD cItems = 0;
    HRESULT hr;

    *ppcm = NULL;
    if (psia && SUCCEEDED(IShellItemArray_GetEstimatedCount(psia, &cItems)) && cItems)
    {
        /* The view hands out the menu of its own selection; other items bind for one */
        if (SUCCEEDED(IUnknown_QueryCurrentView(punkSite, IID_PPV_ARGS(&psv))))
        {
            hr = psv->GetItemObject(SVGIO_SELECTION | SVGIO_VIEWORDER, IID_PPV_ARGS(ppcm));
            psv->Release();
        }
        else
        {
            hr = psia->BindToHandler(NULL, BHID_SFUIObject, IID_PPV_ARGS(ppcm));
        }
        return hr;
    }

    hr = IUnknown_QueryCurrentView(punkSite, IID_PPV_ARGS(&psv));
    if (FAILED(hr))
        return hr;

    IFolderView2 *pfv2 = NULL;
    int cSelected = 0;
    UINT uItem;

    if ((dwModel & SELMODEL_PREFERSELECTION) && SUCCEEDED(psv->QueryInterface(IID_PPV_ARGS(&pfv2))) &&
        SUCCEEDED(pfv2->ItemCount(SVGIO_SELECTION, &cSelected)) && cSelected)
    {
        uItem = SVGIO_SELECTION | SVGIO_VIEWORDER;
        hr = psv->GetItemObject(uItem, IID_PPV_ARGS(ppcm));
    }
    else
    {
        switch (dwModel & SELMODEL_TYPEMASK)
        {
            case SELMODEL_NONE:
                hr = E_FAIL;
                break;
            case SELMODEL_FOLDER:
            {
                IShellItem *psi;

                hr = SHGetItemFromObject(psv, IID_PPV_ARGS(&psi));
                if (SUCCEEDED(hr))
                {
                    hr = psi->BindToHandler(NULL, BHID_SFUIObject, IID_PPV_ARGS(ppcm));
                    psi->Release();
                }
                break;
            }
            case SELMODEL_ALLITEMS:
                uItem = SVGIO_ALLVIEW | SVGIO_VIEWORDER;
                hr = psv->GetItemObject(uItem, IID_PPV_ARGS(ppcm));
                break;
            case SELMODEL_BACKGROUND:
                hr = psv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(ppcm));
                break;
            default:
                hr = E_INVALIDARG;
                break;
        }
    }

    if (pfv2)
        pfv2->Release();
    psv->Release();
    return hr;
}

HWND
WINAPI
SHOwnerWindowFromSite(
    _In_opt_ IUnknown *punkSite)
{
    HWND hwnd = NULL;

    IUnknown_GetWindow(punkSite, &hwnd);
    return hwnd;
}

HRESULT
WINAPI
SHInvokeCommandOnSelection(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ DWORD dwModel,
    _In_ PCSTR pszVerb)
{
    IContextMenu *pcm = NULL;
    HRESULT hr;

    hr = SHGetContextMenuOfSelection(punkSite, psia, dwModel, &pcm);
    if (SUCCEEDED(hr))
        hr = SHInvokeCommandOnContextMenu(SHOwnerWindowFromSite(punkSite), punkSite, pcm, CMIC_MASK_FLAG_LOG_USAGE,
                                          pszVerb);
    if (pcm)
        pcm->Release();
    return hr;
}

/* Whether the menu answers to the canonical verb at all */
static
HRESULT
DoesCanonicalVerbExist(
    _In_ IContextMenu *pcm,
    _In_ PCSTR pszVerb,
    _Out_ BOOL *pfExists)
{
    CHAR szVerb[MAX_PATH];
    HRESULT hr;

    *pfExists = FALSE;
    hr = pcm->GetCommandString((UINT_PTR)pszVerb, GCS_VERBA, NULL, szVerb, _countof(szVerb));
    if (SUCCEEDED(hr))
        *pfExists = !StrCmpICA(pszVerb, szVerb);
    return hr;
}

/* Position of the menu item whose canonical verb is pszVerb, or -1 */
static
UINT
FindCanonicalVerbMenuItem(
    _In_ HMENU hmenu,
    _In_ IContextMenu *pcm,
    _In_ UINT idCmdFirst,
    _In_ PCWSTR pszVerb)
{
    int cItems = GetMenuItemCount(hmenu);

    for (int i = 0; i < cItems; i++)
    {
        MENUITEMINFOW mii = { sizeof(mii) };
        WCHAR szVerb[MAX_PATH];

        mii.fMask = MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(hmenu, i, TRUE, &mii) || mii.hSubMenu || mii.wID < idCmdFirst)
            continue;

        if (SUCCEEDED(pcm->GetCommandString(mii.wID - idCmdFirst, GCS_VERBW, NULL, (PSTR)szVerb,
                                            _countof(szVerb))) &&
            !StrCmpICW(szVerb, pszVerb))
        {
            return i;
        }
    }
    return (UINT)-1;
}

HRESULT
WINAPI
SHIsVerbAvailableOnSelection(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ DWORD dwModel,
    _In_ UINT uFlags,
    _In_ BOOL fAnyState,
    _In_ PCSTR pszVerb,
    _Out_ BOOL *pfAvailable)
{
    IContextMenu *pcm;
    HMENU hmenu;
    HRESULT hr;

    *pfAvailable = FALSE;
    hr = SHGetContextMenuOfSelection(punkSite, psia, dwModel, &pcm);
    if (FAILED(hr))
        return hr;

    hmenu = CreatePopupMenu();
    if (!hmenu)
    {
        pcm->Release();
        return E_OUTOFMEMORY;
    }

    /* uFlags is for menus that take their verb filter through a private interface ReactOS lacks */
    UNREFERENCED_PARAMETER(uFlags);
    IUnknown_SetSite(pcm, punkSite);
    hr = pcm->QueryContextMenu(hmenu, 0, 1, 0x7FFF, CMF_NORMAL);
    if (SUCCEEDED(hr))
    {
        BOOL fExists;

        hr = DoesCanonicalVerbExist(pcm, pszVerb, &fExists);
        if (SUCCEEDED(hr) && fExists)
        {
            if (fAnyState)
            {
                *pfAvailable = TRUE;
            }
            else
            {
                WCHAR szVerb[MAX_PATH];
                UINT iItem;

                SHAnsiToUnicode(pszVerb, szVerb, _countof(szVerb));
                iItem = FindCanonicalVerbMenuItem(hmenu, pcm, 1, szVerb);
                if (iItem != (UINT)-1 && !(GetMenuState(hmenu, iItem, MF_BYPOSITION) & (MF_GRAYED | MF_DISABLED)))
                    *pfAvailable = TRUE;
            }
        }
    }
    IUnknown_SetSite(pcm, NULL);

    DestroyMenu(hmenu);
    pcm->Release();
    return hr;
}

HRESULT
WINAPI
IUnknown_SetSelection(
    _In_opt_ IUnknown *punk,
    _In_opt_ IShellItemArray *psia)
{
    IObjectWithSelection *pows;
    HRESULT hr = E_FAIL;

    if (punk)
    {
        hr = punk->QueryInterface(IID_PPV_ARGS(&pows));
        if (SUCCEEDED(hr))
        {
            hr = pows->SetSelection(psia);
            pows->Release();
        }
    }
    return hr;
}

HRESULT
WINAPI
IInitializeCommand_InitializeWithHKEY(
    _In_ IUnknown *punk,
    _In_ PCWSTR pszCommandName,
    _In_ HKEY hkey)
{
    IInitializeCommand *pic;
    HRESULT hr = S_FALSE;

    if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARGS(&pic))))
    {
        IPropertyBag *ppb;

        hr = SHCreatePropertyBagOnRegKey(hkey, NULL, STGM_READ, IID_PPV_ARGS(&ppb));
        if (SUCCEEDED(hr))
        {
            hr = pic->Initialize(pszCommandName, ppb);
            ppb->Release();
        }
        pic->Release();
    }
    return hr;
}

static
BOOL
IsPolicySet(
    _In_ REFGUID rguidPolicy)
{
    DWORD dwValue = 0;
    DWORD cbValue = sizeof(dwValue);

    return SUCCEEDED(SHWindowsPolicyGetValue(rguidPolicy, &dwValue, &cbValue)) && dwValue;
}

/*
 * Pane state for the pane toggles: 2 when the pane cannot show, otherwise 4 when its
 * visibility is known and 8 more when it is open.
 */
HRESULT
WINAPI
CanShowPaneAux(
    _In_ PCWSTR pszVisibleProp,
    _In_ REFGUID rguidPolicy,
    _In_ REFEXPLORERPANE ep,
    _In_ REFGUID rguidBagService,
    _In_opt_ IUnknown *punkSite,
    _Out_ DWORD *pdwState)
{
    EXPLORERPANESTATE eps = EPS_DONTCARE;
    IPropertyBag *ppb;

    *pdwState = ECS_HIDDEN;
    IUnknown_GetExplorerPaneState(punkSite, ep, &eps);
    if (eps & EPS_FORCE)
        return S_OK;

    if (!IsEqualGUID(rguidPolicy, GUID_NULL) && IsPolicySet(rguidPolicy))
        return S_OK;

    if (SUCCEEDED(IUnknown_QueryService(punkSite, rguidBagService, IID_PPV_ARGS(&ppb))))
    {
        BOOL fVisible;

        if (SUCCEEDED(SHPropertyBag_ReadBOOL(ppb, pszVisibleProp, &fVisible)))
            *pdwState = fVisible ? (ECS_CHECKBOX | ECS_CHECKED) : ECS_CHECKBOX;
        ppb->Release();
    }
    return S_OK;
}

/* 2 for nothing selected, 3 for one file, 4 for one folder, 5 for several items */
UINT
WINAPI
GetSelectionStateFromItemArray(
    _In_opt_ IShellItemArray *psia)
{
    UINT uState = SELSTATE_NONE;
    DWORD cItems;

    if (psia && SUCCEEDED(IShellItemArray_GetEstimatedCount(psia, &cItems)) && cItems)
    {
        if (cItems == 1)
        {
            SFGAOF sfgao;

            if (SUCCEEDED(psia->GetAttributes(SIATTRIBFLAGS_AND, SFGAO_FOLDER | SFGAO_STREAM, &sfgao)))
                uState = (sfgao == SFGAO_FOLDER) ? SELSTATE_FOLDER : SELSTATE_FILE;
        }
        else
        {
            uState = SELSTATE_MULTIPLE;
        }
    }
    return uState;
}

HRESULT
WINAPI
SHRegGetGUID(
    _In_ HKEY hkey,
    _In_opt_ PCWSTR pszSubKey,
    _In_ PCWSTR pszValue,
    _Out_ GUID *pguid)
{
    WCHAR szGuid[39];
    DWORD cbData = sizeof(szGuid);
    LSTATUS lError;

    *pguid = GUID_NULL;
    lError = RegGetValueW(hkey, pszSubKey, pszValue, RRF_RT_REG_SZ, NULL, szGuid, &cbData);
    if (lError != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(lError);

    return GUIDFromStringW(szGuid, pguid) ? S_OK : E_INVALIDARG;
}

HRESULT
WINAPI
SHRegAllocString(
    _In_ HKEY hkey,
    _In_opt_ PCWSTR pszSubKey,
    _In_opt_ PCWSTR pszValue,
    _Outptr_ PWSTR *ppsz)
{
    DWORD cbData = 0;
    LSTATUS lError;
    PWSTR psz;

    *ppsz = NULL;
    lError = RegGetValueW(hkey, pszSubKey, pszValue, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, NULL,
                          &cbData);
    if (lError != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(lError);

    psz = (PWSTR)CoTaskMemAlloc(cbData + sizeof(WCHAR));
    if (!psz)
        return E_OUTOFMEMORY;

    lError = RegGetValueW(hkey, pszSubKey, pszValue, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, psz,
                          &cbData);
    if (lError != ERROR_SUCCESS)
    {
        CoTaskMemFree(psz);
        return HRESULT_FROM_WIN32(lError);
    }

    *ppsz = psz;
    return S_OK;
}

BOOL
WINAPI
SHRegValueExists(
    _In_ HKEY hkey,
    _In_opt_ PCWSTR pszSubKey,
    _In_ PCWSTR pszValue)
{
    return RegGetValueW(hkey, pszSubKey, pszValue, RRF_RT_ANY, NULL, NULL, NULL) == ERROR_SUCCESS;
}

HRESULT
WINAPI
DUI_GetElementScreenBounds(
    _In_ DirectUI::Element *pe,
    _Out_ RECT *prc)
{
    DirectUI::HWNDElement *pheRoot;
    HRESULT hr;

    hr = pe->GetRootRelativeBounds(prc);
    if (SUCCEEDED(hr))
    {
        hr = DUI_GetElementRootHWNDElement(pe, &pheRoot);
        if (SUCCEEDED(hr))
            MapWindowPoints(pheRoot->GetHWND(), NULL, (POINT *)prc, 2);
    }
    return hr;
}

/* Gives an element an icon as its content; the icon belongs to the element afterwards */
HRESULT
WINAPI
DUI_SetElementIcon(
    _In_ DirectUI::Element *pe,
    _In_ HICON hicon)
{
    DirectUI::Value *pv = DirectUI::Value::CreateGraphic(hicon, false, false, false);
    HRESULT hr;

    if (!pv)
    {
        DestroyIcon(hicon);
        return E_OUTOFMEMORY;
    }

    hr = pe->SetValue(DirectUI::Element::ContentProp, PROPERTY_STORE_LOCAL, pv);
    pv->Release();
    return hr;
}

HRESULT
WINAPI
CommandButtonData_Init(
    _Out_ COMMANDBUTTONDATA *pData,
    _In_ IExplorerCommand *pec,
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ BOOL fOkToBeSlow)
{
    HRESULT hr;

    IUnknown_SetSite(pec, punkSite);
    IUnknown_SetSelection(pec, psia);

    hr = pec->GetCanonicalName(&pData->guidCanonical);
    if (SUCCEEDED(hr))
        hr = pec->GetFlags(&pData->dwFlags);
    if (SUCCEEDED(hr))
        hr = pec->GetState(psia, fOkToBeSlow, &pData->dwState);

    if (SUCCEEDED(hr))
    {
        PWSTR pszIcon = NULL;

        if (SUCCEEDED(pec->GetTitle(psia, &pData->pszTitle)) && pData->pszTitle)
            SHStripMneumonicW(pData->pszTitle);
        pec->GetToolTip(psia, &pData->pszToolTip);

        if (pec->GetIcon(psia, &pszIcon) == S_OK)
        {
            UINT uFlags = (pData->dwFlags & ECF_HASLUASHIELD) ? GIL_SHIELD : GIL_FORCENOSHIELD;
            int iIndex = PathParseIconLocationW(pszIcon);

            pData->iImage = Shell_GetCachedImageIndexW(pszIcon, iIndex, uFlags);
        }
        else
        {
            pData->iImage = -1;
        }
        CoTaskMemFree(pszIcon);
    }

    IUnknown_SetSite(pec, NULL);
    return hr;
}

VOID
WINAPI
CommandButtonData_Free(
    _Inout_ COMMANDBUTTONDATA *pData)
{
    CoTaskMemFree(pData->pszTitle);
    pData->pszTitle = NULL;
    CoTaskMemFree(pData->pszToolTip);
    pData->pszToolTip = NULL;
}
