/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Element base classes of the view frame: IUnknown, site and module elements
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

DirectUI::IClassInfo *CElementWithIUnknown::Class = NULL;
DirectUI::IClassInfo *CElementWithSite::Class = NULL;
DirectUI::IClassInfo *CFrameModule::Class = NULL;

static DirectUI::PropCapability s_pcString = { DirectUI::ValueType::String, 0, UINT_MAX };
static DirectUI::PropertyInfoData s_pidModuleID;

static const DirectUI::PropertyInfo s_piModuleID =
{
    L"ModuleID", 2, 0, &s_pcString, NULL, DirectUI::Value::GetStringNull, &s_pidModuleID
};

static const DirectUI::PropertyInfo *const c_rgpiFrameModule[] =
{
    &s_piModuleID,
};

const DirectUI::PropertyInfo *const CFrameModule::ModuleIDProp = &s_piModuleID;

/* Lets the frame hand an element around as a COM object without owning it */
class CElementWrapper : public IElementWrapper, public CDUIFrameZeroAlloc
{
public:
    CElementWrapper(_In_ DirectUI::Element *pe) : _cRef(1), _pe(pe) {}

    STDMETHODIMP
    QueryInterface(
        REFIID riid,
        void **ppv) override
    {
        static const QITAB qit[] =
        {
            QITABENT(CElementWrapper, IElementWrapper),
            {0},
        };
        return QISearch(this, qit, riid, ppv);
    }

    STDMETHODIMP_(ULONG)
    AddRef() override
    {
        return InterlockedIncrement(&_cRef);
    }

    STDMETHODIMP_(ULONG)
    Release() override
    {
        ULONG cRef = InterlockedDecrement(&_cRef);

        if (!cRef)
            delete this;
        return cRef;
    }

    STDMETHODIMP
    GetElement(
        DirectUI::Element **ppe) override
    {
        *ppe = _pe;
        return S_OK;
    }

private:
    LONG _cRef;
    DirectUI::Element *_pe;
};

HRESULT
WINAPI
CreateWrapperForElement(
    _In_ DirectUI::Element *pe,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CElementWrapper *pew;
    HRESULT hr;

    *ppv = NULL;
    pew = new CElementWrapper(pe);
    if (!pew)
        return E_OUTOFMEMORY;

    hr = pew->QueryInterface(riid, ppv);
    pew->Release();
    return hr;
}

/* CElementWithIUnknown */

HRESULT
WINAPI
CElementWithIUnknown::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    *ppElement = NULL;
    return E_NOTIMPL;
}

HRESULT
WINAPI
CElementWithIUnknown::Register()
{
    return DirectUI::ClassInfo<CElementWithIUnknown, DirectUI::Element,
                               DirectUI::StandardCreator<CElementWithIUnknown>>::Register(
        DUIFRAME_HINST, L"CElementWithIUnknown", NULL, 0, false);
}

DirectUI::IClassInfo *
CElementWithIUnknown::GetClassInfoW()
{
    return Class;
}

/* Elements expose their IUnknown when CElementWithIUnknown is anywhere in their class chain */
IUnknown *
WINAPI
CElementWithIUnknown::GetUnknownFromElement(
    _In_ DirectUI::Element *pe)
{
    DirectUI::IClassInfo *pci;
    PCWSTR pszName = Class ? Class->GetName() : NULL;

    if (!pszName)
        return NULL;

    for (pci = pe->GetClassInfoW(); pci; pci = pci->GetBaseClass())
    {
        if (CompareStringOrdinal(pci->GetName(), -1, pszName, -1, FALSE) == CSTR_EQUAL)
            return static_cast<IUnknown *>(static_cast<CElementWithIUnknown *>(pe));
    }
    return NULL;
}

STDMETHODIMP
CElementWithIUnknown::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

/* The element tree owns these objects, so the COM reference count does not matter */
STDMETHODIMP_(ULONG)
CElementWithIUnknown::AddRef()
{
    return 1;
}

STDMETHODIMP_(ULONG)
CElementWithIUnknown::Release()
{
    return 1;
}

/* CElementWithSite */

HRESULT
WINAPI
CElementWithSite::Register()
{
    return DirectUI::ClassInfo<CElementWithSite, CElementWithIUnknown,
                               DirectUI::StandardCreator<CElementWithSite>>::Register(
        DUIFRAME_HINST, L"CElementWithSite", NULL, 0, false);
}

DirectUI::IClassInfo *
CElementWithSite::GetClassInfoW()
{
    return Class;
}

STDMETHODIMP
CElementWithSite::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CElementWithSite, IObjectWithSite),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CElementWithSite::AddRef()
{
    return 1;
}

STDMETHODIMP_(ULONG)
CElementWithSite::Release()
{
    return 1;
}

STDMETHODIMP
CElementWithSite::SetSite(
    IUnknown *punkSite)
{
    IUnknown_Set(&_punkSite, punkSite);
    return S_OK;
}

STDMETHODIMP
CElementWithSite::GetSite(
    REFIID riid,
    void **ppvSite)
{
    *ppvSite = NULL;
    return _punkSite ? _punkSite->QueryInterface(riid, ppvSite) : E_FAIL;
}

/* Settings persist per element: the class name and the element ID name the stream */
HRESULT
CElementWithSite::GetPersistStream(
    _In_ UINT uPersistType,
    _In_ DWORD grfMode,
    _COM_Outptr_ IStream **ppstm)
{
    IFrameManager *pfm;
    WCHAR szID[MAX_PATH];
    HRESULT hr;

    *ppstm = NULL;
    hr = IUnknown_QueryService(_punkSite, IID_IFrameManager, IID_PPV_ARGS(&pfm));
    if (FAILED(hr))
        return hr;

    if (GetAtomNameW(GetID(), szID, _countof(szID)))
        hr = pfm->GetPersistStream(uPersistType, GetClassInfoW()->GetName(), szID, grfMode, ppstm);
    else
        hr = HRESULT_FROM_WIN32(GetLastError());

    pfm->Release();
    return hr;
}

/* CFrameModule */

HRESULT
WINAPI
CFrameModule::Register()
{
    return DirectUI::ClassInfo<CFrameModule, CElementWithSite,
                               DirectUI::StandardCreator<CFrameModule>>::Register(
        DUIFRAME_HINST, L"FrameModule", c_rgpiFrameModule, _countof(c_rgpiFrameModule), false);
}

DirectUI::IClassInfo *
CFrameModule::GetClassInfoW()
{
    return Class;
}

STDMETHODIMP
CFrameModule::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CFrameModule, IFrameModule),
        {0},
    };
    HRESULT hr = QISearch(this, qit, riid, ppv);

    if (FAILED(hr))
        hr = CElementWithSite::QueryInterface(riid, ppv);
    return hr;
}

STDMETHODIMP_(ULONG)
CFrameModule::AddRef()
{
    return 1;
}

STDMETHODIMP_(ULONG)
CFrameModule::Release()
{
    return 1;
}

STDMETHODIMP
CFrameModule::CreateInnerObject(
    IUnknown **ppunkInner)
{
    *ppunkInner = NULL;
    return E_NOTIMPL;
}

/* The inner element replaces whatever the module held; NULL just empties it */
STDMETHODIMP
CFrameModule::SetInnerObject(
    IUnknown *punkInner)
{
    IElementWrapper *pew;
    DirectUI::Element *peInner;
    HRESULT hr;

    RemoveAll();
    if (!punkInner)
        return E_FAIL;

    hr = punkInner->QueryInterface(IID_PPV_ARGS(&pew));
    if (SUCCEEDED(hr))
    {
        hr = pew->GetElement(&peInner);
        if (SUCCEEDED(hr))
            Add(peInner);
        pew->Release();
    }
    return hr;
}

STDMETHODIMP
CFrameModule::GetModuleID(
    PWSTR *ppszModuleID)
{
    DirectUI::Value *pv = GetValue(ModuleIDProp, 2, NULL);
    HRESULT hr = SHStrDupW(pv->GetString(), ppszModuleID);

    pv->Release();
    return hr;
}

HRESULT
WINAPI
FrameModule_CreateInner(
    _In_ PFNCREATEINNERELEMENT pfnCreate,
    _In_ PCWSTR pszInnerID,
    _COM_Outptr_ IUnknown **ppunkInner)
{
    DirectUI::Element *peInner;
    ULONG dwDeferCookie;
    HRESULT hr;

    *ppunkInner = NULL;
    hr = pfnCreate(NULL, &dwDeferCookie, &peInner);
    if (FAILED(hr))
        return hr;

    peInner->SetID(pszInnerID);
    peInner->SetLayoutPos(ROSUI_LP_CLIENT);
    peInner->EndDefer(dwDeferCookie);

    hr = CreateWrapperForElement(peInner, IID_PPV_ARGS(ppunkInner));
    if (FAILED(hr))
        peInner->Destroy(true);
    return hr;
}

/* Class registration for the frame layouts */

HRESULT
WINAPI
InitializeDUIFrameClasses(VOID)
{
    static HRESULT (WINAPI *const s_rgpfnRegister[])() =
    {
        CFrameModule::Register,
        CHWNDView::Register,
        CDUIViewHost::Register,
        CDUIView::Register,
        CDUISizerElement::Register,
        CPreviewBackground::Register,
        CTemplateBackground::Register,
        CPreviewThumbnail::Register,
        CPreviewMetadata::Register,
        CReadingPaneModule::Register,
        CProperTreeModule::Register,
        CCommandModule::Register,
        CInfoBarHostModule::Register,
        CLibraryPaneModule::Register,
        CInfoPane::Register,
        CProperTreeModuleInner::Register,
        Button3d::Register,
        CCommandModuleInner::Register,
        CSplitButton::Register,
        ViewControl::Register,
        CProperTreeHost::Register,
        CCIDLabel::Register,
        CInfoPaneInner::Register,
        CLibraryPaneModuleInner::Register,
        CReadingPaneModuleInner::Register,
        CReadingPaneModuleHost::Register,
        CThumbnailControlHost::Register,
        CPreviewThumbnailInner::Register,
        CMetadataLabel::Register,
        CMetadataElement::Register,
        CDisplayElement::Register,
        CPreviewMetadataInner::Register,
    };
    HRESULT hr = S_OK;
    SIZE_T i;

    for (i = 0; i < _countof(s_rgpfnRegister) && SUCCEEDED(hr); i++)
        hr = s_rgpfnRegister[i]();
    return hr;
}
