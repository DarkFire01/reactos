/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Template and preview pane background elements
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

/* Details pane heights in logical pixels for the three size commands */
#define PREVIEW_SIZE_SMALL          52
#define PREVIEW_SIZE_MEDIUM         70
#define PREVIEW_SIZE_LARGE          106

/* CGID_DefView command that reports the folder type's template class name */
#define DVID_GETFOLDERTYPECLASS     2

DirectUI::IClassInfo *CTemplateBackground::Class = NULL;
DirectUI::IClassInfo *CPreviewBackground::Class = NULL;

static DirectUI::PropCapability s_pcBool = { DirectUI::ValueType::Bool, 0, UINT_MAX };
static DirectUI::PropertyInfoData s_pidVertical;

static const DirectUI::PropertyInfo s_piVertical =
    { L"Vertical", 2, 0, &s_pcBool, NULL, DirectUI::Value::GetBoolFalse, &s_pidVertical };

static const DirectUI::PropertyInfo *const c_rgpiTemplateBackground[] =
{
    &s_piVertical,
};

const DirectUI::PropertyInfo *const CTemplateBackground::VerticalProp = &s_piVertical;

template <class T>
static
HRESULT
WINAPI
CreateSiteElement(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    T *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<T>();
    if (!pe)
        return E_OUTOFMEMORY;

    hr = pe->Initialize(0, peParent, pdwDeferCookie);
    if (SUCCEEDED(hr))
        hr = pe->SetActive(1);
    if (FAILED(hr))
        pe->Destroy(false);
    else
        *ppElement = pe;
    return hr;
}

/* CTemplateBackground */

HRESULT
WINAPI
CTemplateBackground::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    return CreateSiteElement<CTemplateBackground>(peParent, pdwDeferCookie, ppElement);
}

HRESULT
WINAPI
CTemplateBackground::Register()
{
    return DirectUI::ClassInfo<CTemplateBackground, CElementWithSite,
                               DirectUI::StandardCreator<CTemplateBackground>>::Register(
        DUIFRAME_HINST, L"TemplateBackground", c_rgpiTemplateBackground,
        _countof(c_rgpiTemplateBackground), false);
}

STDMETHODIMP
CTemplateBackground::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CTemplateBackground, IFrameNotificationClient),
        QITABENT(CTemplateBackground, IDispatch),
        {0},
    };
    HRESULT hr = QISearch(this, qit, riid, ppv);

    if (FAILED(hr))
        hr = CElementWithSite::QueryInterface(riid, ppv);
    return hr;
}

/* Follows the shell view's events so the template class tracks the folder type */
STDMETHODIMP
CTemplateBackground::SetSite(
    IUnknown *punkSite)
{
    IUnknown_Set(&_punkSite, punkSite);

    if (punkSite)
    {
        IShellView *psv;
        IDispatch *pdisp;

        if (SUCCEEDED(IUnknown_QueryService(_punkSite, IID_IFolderView, IID_PPV_ARGS(&psv))))
        {
            if (SUCCEEDED(psv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&pdisp))))
            {
                if (SUCCEEDED(ConnectToConnectionPoint(static_cast<IDispatch *>(this),
                                                       DIID_DShellFolderViewEvents, TRUE, pdisp,
                                                       &_dwCookie, NULL)))
                {
                    IUnknown_Set((IUnknown **)&_psv, psv);
                    IUnknown_Set((IUnknown **)&_pdispView, pdisp);
                }
                pdisp->Release();
            }
            psv->Release();
        }
    }
    else
    {
        if (_dwCookie)
        {
            ConnectToConnectionPoint(static_cast<IDispatch *>(this), DIID_DShellFolderViewEvents, FALSE,
                                     _pdispView, &_dwCookie, NULL);
            _dwCookie = 0;
        }
        IUnknown_SafeReleaseAndNullPtr(&_psv);
        IUnknown_SafeReleaseAndNullPtr(&_pdispView);
    }

    _SetFolderType();
    return S_OK;
}

HRESULT
CTemplateBackground::_SetFolderType()
{
    VARIANT var;
    HRESULT hr;

    VariantInit(&var);
    hr = IUnknown_Exec(_psv, &CGID_DefView, DVID_GETFOLDERTYPECLASS, 0, NULL, &var);
    if (SUCCEEDED(hr))
    {
        PCWSTR pszClass = VariantToStringWithDefault(&var, NULL);

        if (pszClass)
            SetClass(pszClass);
        VariantClear(&var);
        hr = S_FALSE;
    }
    return hr;
}

STDMETHODIMP
CTemplateBackground::GetTypeInfoCount(
    UINT *pctinfo)
{
    *pctinfo = 0;
    return E_NOTIMPL;
}

STDMETHODIMP
CTemplateBackground::GetTypeInfo(
    UINT iTInfo,
    LCID lcid,
    ITypeInfo **ppTInfo)
{
    *ppTInfo = NULL;
    return E_NOTIMPL;
}

STDMETHODIMP
CTemplateBackground::GetIDsOfNames(
    REFIID riid,
    LPOLESTR *rgszNames,
    UINT cNames,
    LCID lcid,
    DISPID *rgDispId)
{
    return E_NOTIMPL;
}

STDMETHODIMP
CTemplateBackground::Invoke(
    DISPID dispIdMember,
    REFIID riid,
    LCID lcid,
    WORD wFlags,
    DISPPARAMS *pDispParams,
    VARIANT *pVarResult,
    EXCEPINFO *pExcepInfo,
    UINT *puArgErr)
{
    if (pVarResult)
        VariantInit(pVarResult);

    if (dispIdMember != DISPID_VIEW_FOLDERTYPECHANGED)
        return E_INVALIDARG;

    _SetFolderType();
    return S_OK;
}

/* CPreviewBackground */

HRESULT
WINAPI
CPreviewBackground::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    return CreateSiteElement<CPreviewBackground>(peParent, pdwDeferCookie, ppElement);
}

HRESULT
WINAPI
CPreviewBackground::Register()
{
    return DirectUI::ClassInfo<CPreviewBackground, CElementWithSite,
                               DirectUI::StandardCreator<CPreviewBackground>>::Register(
        DUIFRAME_HINST, L"PreviewBackground", NULL, 0, false);
}

void
CPreviewBackground::OnInput(
    DirectUI::InputEvent *pie)
{
    /* A right button release bubbling up through the pane opens its menu */
    if (pie->nStage == GMF_BUBBLED && pie->nDevice == GINPUT_MOUSE && pie->nCode == GMOUSE_UP &&
        static_cast<DirectUI::MouseEvent *>(pie)->bButton == GBUTTON_RIGHT)
    {
        _LaunchContextMenu(static_cast<DirectUI::MouseEvent *>(pie));
    }
    DirectUI::Element::OnInput(pie);
}

HRESULT
CPreviewBackground::_GetSelection(
    _COM_Outptr_ IShellItemArray **ppsia)
{
    IFolderView *pfv;
    HRESULT hr;

    *ppsia = NULL;
    hr = IUnknown_QueryService(_punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv));
    if (SUCCEEDED(hr))
    {
        hr = pfv->Items(SVGIO_SELECTION, IID_PPV_ARGS(ppsia));
        pfv->Release();
    }
    return hr;
}

/* Finds pszVerb among the canonical verbs the selection's context menu offers */
static
HRESULT
FindSelectionVerb(
    _In_ IShellItemArray *psia,
    _In_ PCWSTR pszVerb,
    _COM_Outptr_opt_ IContextMenu **ppcm)
{
    IContextMenu *pcm;
    HMENU hmenu;
    HRESULT hr;
    BOOL fFound = FALSE;

    if (ppcm)
        *ppcm = NULL;

    hr = psia->BindToHandler(NULL, BHID_SFUIObject, IID_PPV_ARGS(&pcm));
    if (FAILED(hr))
        return hr;

    hmenu = CreatePopupMenu();
    if (hmenu)
    {
        if (SUCCEEDED(pcm->QueryContextMenu(hmenu, 0, 1, 0x7FFF, CMF_EXTENDEDVERBS | CMF_NODEFAULT)))
        {
            int cItems = GetMenuItemCount(hmenu);
            int i;

            for (i = 0; i < cItems && !fFound; i++)
            {
                UINT idCmd = GetMenuItemID(hmenu, i);
                WCHAR szVerb[64];

                if (idCmd != (UINT)-1 && idCmd >= 1 &&
                    SUCCEEDED(pcm->GetCommandString(idCmd - 1, GCS_VERBW, NULL, (LPSTR)szVerb,
                                                    _countof(szVerb))))
                {
                    fFound = !StrCmpICW(szVerb, pszVerb);
                }
            }
        }
        DestroyMenu(hmenu);
    }

    if (fFound && ppcm)
    {
        *ppcm = pcm;
        pcm = NULL;
    }
    if (pcm)
        pcm->Release();
    return fFound ? S_OK : E_FAIL;
}

void
CPreviewBackground::_RemoveProperties()
{
    IShellItemArray *psia;
    IContextMenu *pcm;
    HWND hwnd;

    if (FAILED(DUI_GetElementRootHWND(this, &hwnd)) || FAILED(_GetSelection(&psia)))
        return;

    if (SUCCEEDED(FindSelectionVerb(psia, L"removeproperties", &pcm)))
    {
        CMINVOKECOMMANDINFOEX ici = { sizeof(ici) };

        IUnknown_SetSite(pcm, _punkSite);
        ici.fMask = CMIC_MASK_UNICODE;
        ici.hwnd = hwnd;
        ici.lpVerb = "removeproperties";
        ici.lpVerbW = L"removeproperties";
        ici.nShow = SW_SHOWNORMAL;
        pcm->InvokeCommand((CMINVOKECOMMANDINFO *)&ici);
        IUnknown_SetSite(pcm, NULL);
        pcm->Release();
    }
    psia->Release();
}

/* The menu offers property removal for the selection and the three pane heights */
void
CPreviewBackground::_LaunchContextMenu(
    _In_ DirectUI::MouseEvent *pme)
{
    IPropertyBag *ppb;
    IShellItemArray *psia;
    HMENU hmenu;
    POINT ptScreen;
    HWND hwnd;
    int cySize;
    BOOL fCanRemove = FALSE;
    int idCmd;

    if (FAILED(IUnknown_QueryService(_punkSite, SID_PerBrowserPropertyBag, IID_PPV_ARGS(&ppb))))
        return;

    if (FAILED(SHPropertyBag_ReadInt(ppb, L"PreviewPaneSizer_Size", &cySize)))
        goto Cleanup;

    hmenu = SHLoadMenuPopup(DUIFRAME_HINST, IDM_PREVIEWPANE);
    if (!hmenu)
        goto Cleanup;

    if (SUCCEEDED(_GetSelection(&psia)))
    {
        fCanRemove = SUCCEEDED(FindSelectionVerb(psia, L"removeproperties", NULL));
        psia->Release();
    }

    EnableMenuItem(hmenu, IDC_PREVIEW_REMOVEPROPS, fCanRemove ? MF_ENABLED : MF_GRAYED);
    CheckMenuItem(hmenu, IDC_PREVIEW_SIZE_SMALL, cySize == PREVIEW_SIZE_SMALL ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(hmenu, IDC_PREVIEW_SIZE_MEDIUM, cySize == PREVIEW_SIZE_MEDIUM ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(hmenu, IDC_PREVIEW_SIZE_LARGE, cySize == PREVIEW_SIZE_LARGE ? MF_CHECKED : MF_UNCHECKED);

    if (SUCCEEDED(DUI_GetMouseEventAbsolutePos(pme->peTarget, &pme->ptClientPxl, &ptScreen)) &&
        SUCCEEDED(DUI_GetElementRootHWND(this, &hwnd)))
    {
        idCmd = DUI_TrackPopupMenu(hmenu, TPM_RETURNCMD | TPM_NONOTIFY, ptScreen.x, ptScreen.y, hwnd,
                                   NULL, IsRTL());
        switch (idCmd)
        {
            case IDC_PREVIEW_REMOVEPROPS:
                _RemoveProperties();
                break;
            case IDC_PREVIEW_SIZE_SMALL:
                SHPropertyBag_WriteInt(ppb, L"PreviewPaneSizer_Size", PREVIEW_SIZE_SMALL);
                break;
            case IDC_PREVIEW_SIZE_MEDIUM:
                SHPropertyBag_WriteInt(ppb, L"PreviewPaneSizer_Size", PREVIEW_SIZE_MEDIUM);
                break;
            case IDC_PREVIEW_SIZE_LARGE:
                SHPropertyBag_WriteInt(ppb, L"PreviewPaneSizer_Size", PREVIEW_SIZE_LARGE);
                break;
        }
    }
    DestroyMenu(hmenu);

Cleanup:
    ppb->Release();
}
