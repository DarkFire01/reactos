/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Common frame layout definitions and the browser layout modifier
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

/* Folder layout markup when the layout type has no entry of its own */
#define IDUIF_FOLDERLAYOUT_DEFAULT  3

/* Browser chrome markup that wraps the folder layout, per browser layout type */
#define IDUIF_CHROME_NONE           11
#define IDUIF_CHROME_NORMAL         12
#define IDUIF_CHROME_OPENDIALOG     27
#define IDUIF_CHROME_SAVEDIALOG     28
#define IDUIF_CHROME_MINISAVEDIALOG 49
#define IDUIF_CHROME_FOLDERDIALOG   46

typedef struct _COMMONLAYOUT
{
    LAYOUTTYPE layoutType;
    USHORT uUIFile;
    BOOL fHideContentPane;
} COMMONLAYOUT;

static const COMMONLAYOUT c_rgCommonLayouts[] =
{
    { LAYOUTTYPE_PHOTO,                 5,  FALSE },
    { LAYOUTTYPE_MUSIC,                 4,  FALSE },
    { LAYOUTTYPE_DOCUMENTS,             6,  FALSE },
    { LAYOUTTYPE_CONTROLPANEL,          17, FALSE },
    { LAYOUTTYPE_CONTROLPANELHOME,      18, FALSE },
    { LAYOUTTYPE_SOFTWAREEXP,           31, FALSE },
    { LAYOUTTYPE_NOPREVIEW,             10, FALSE },
    { LAYOUTTYPE_COMPUTER,              21, FALSE },
    { LAYOUTTYPE_LEGACYSHELLVIEW,       13, FALSE },
    { LAYOUTTYPE_SEARCHPAGE,            14, FALSE },
    { LAYOUTTYPE_EMAIL,                 19, FALSE },
    { LAYOUTTYPE_CONTACTS,              20, FALSE },
    { LAYOUTTYPE_SEARCHONLYBARRIER,     15, TRUE },
    { LAYOUTTYPE_EMPTYLIBRARYBARRIER,   16, TRUE },
    { LAYOUTTYPE_HGCREATEBARRIER,       53, TRUE },
    { LAYOUTTYPE_HGJOINBARRIER,         54, TRUE },
    { LAYOUTTYPE_HGMERGEBARRIER,        55, TRUE },
    { LAYOUTTYPE_HGPASSWORDBARRIER,     56, TRUE },
    { LAYOUTTYPE_HGONLYMEMBERBARRIER,   57, TRUE },
    { (LAYOUTTYPE)0x1A,                 58, TRUE },
    { (LAYOUTTYPE)0x1B,                 62, TRUE },
};

static
const COMMONLAYOUT *
FindCommonLayout(
    _In_ LAYOUTTYPE layoutType)
{
    SIZE_T i;

    for (i = 0; i < _countof(c_rgCommonLayouts); i++)
    {
        if (c_rgCommonLayouts[i].layoutType == layoutType)
            return &c_rgCommonLayouts[i];
    }
    return NULL;
}

/* CStreamWithLayoutType: a markup stream that also says which layout it describes */

CStreamWithLayoutType::CStreamWithLayoutType(
    _In_ IStream *pstm,
    _In_ LAYOUTTYPE layoutType) :
    _cRef(1),
    _layoutType(layoutType),
    _pstm(pstm)
{
    pstm->AddRef();
}

CStreamWithLayoutType::~CStreamWithLayoutType()
{
    _pstm->Release();
}

HRESULT
WINAPI
CStreamWithLayoutType::Create(
    _In_ IStream *pstm,
    _In_ LAYOUTTYPE layoutType,
    _COM_Outptr_ IUnknown **ppunk)
{
    CStreamWithLayoutType *pswlt;
    HRESULT hr;

    *ppunk = NULL;
    pswlt = new CStreamWithLayoutType(pstm, layoutType);
    if (!pswlt)
        return E_OUTOFMEMORY;

    hr = pswlt->QueryInterface(IID_PPV_ARGS(ppunk));
    pswlt->Release();
    return hr;
}

STDMETHODIMP
CStreamWithLayoutType::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CStreamWithLayoutType, IStream),
        QITABENT(CStreamWithLayoutType, ISequentialStream),
        QITABENT(CStreamWithLayoutType, ILayoutType),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CStreamWithLayoutType::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CStreamWithLayoutType::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

STDMETHODIMP CStreamWithLayoutType::Read(void *pv, ULONG cb, ULONG *pcbRead) { return _pstm->Read(pv, cb, pcbRead); }
STDMETHODIMP CStreamWithLayoutType::Write(const void *pv, ULONG cb, ULONG *pcbWritten) { return _pstm->Write(pv, cb, pcbWritten); }
STDMETHODIMP CStreamWithLayoutType::Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER *plibNewPosition) { return _pstm->Seek(dlibMove, dwOrigin, plibNewPosition); }
STDMETHODIMP CStreamWithLayoutType::SetSize(ULARGE_INTEGER libNewSize) { return _pstm->SetSize(libNewSize); }
STDMETHODIMP CStreamWithLayoutType::CopyTo(IStream *pstm, ULARGE_INTEGER cb, ULARGE_INTEGER *pcbRead, ULARGE_INTEGER *pcbWritten) { return _pstm->CopyTo(pstm, cb, pcbRead, pcbWritten); }
STDMETHODIMP CStreamWithLayoutType::Commit(DWORD grfCommitFlags) { return _pstm->Commit(grfCommitFlags); }
STDMETHODIMP CStreamWithLayoutType::Revert() { return _pstm->Revert(); }
STDMETHODIMP CStreamWithLayoutType::LockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) { return _pstm->LockRegion(libOffset, cb, dwLockType); }
STDMETHODIMP CStreamWithLayoutType::UnlockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) { return _pstm->UnlockRegion(libOffset, cb, dwLockType); }
STDMETHODIMP CStreamWithLayoutType::Stat(STATSTG *pstatstg, DWORD grfStatFlag) { return _pstm->Stat(pstatstg, grfStatFlag); }
STDMETHODIMP CStreamWithLayoutType::Clone(IStream **ppstm) { return _pstm->Clone(ppstm); }

STDMETHODIMP
CStreamWithLayoutType::GetLayoutType(
    LAYOUTTYPE *pType)
{
    *pType = _layoutType;
    return S_OK;
}

static
LAYOUTTYPE
GetLayoutTypeOf(
    _In_ IUnknown *punk)
{
    LAYOUTTYPE layoutType = LAYOUTTYPE_NORMAL;
    ILayoutType *plt;

    if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARGS(&plt))))
    {
        plt->GetLayoutType(&layoutType);
        plt->Release();
    }
    return layoutType;
}

/* CCommonLayoutDefinition */

class CCommonLayoutDefinition :
    public ICommonLayoutDefinition,
    public IFrameLayoutDefinition,
    public ILayoutProperties,
    public CDUIFrameZeroAlloc
{
public:
    CCommonLayoutDefinition() : _cRef(1), _layoutType(LAYOUTTYPE_NORMAL) {}

    STDMETHODIMP
    QueryInterface(
        REFIID riid,
        void **ppv) override
    {
        static const QITAB qit[] =
        {
            QITABENT(CCommonLayoutDefinition, ICommonLayoutDefinition),
            QITABENT(CCommonLayoutDefinition, IFrameLayoutDefinition),
            QITABENT(CCommonLayoutDefinition, ILayoutProperties),
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
    ChooseLayout(
        LAYOUTTYPE layoutType) override
    {
        _layoutType = layoutType;
        return S_OK;
    }

    STDMETHODIMP
    GetLayoutDefinition(
        IUnknown **ppunk) override
    {
        const COMMONLAYOUT *pLayout = FindCommonLayout(_layoutType);
        PWSTR pszMarkup;
        IStream *pstm;
        HRESULT hr;

        *ppunk = NULL;
        hr = DUI_LoadUIFileFromResources(DUIFRAME_HINST,
                                         pLayout ? pLayout->uUIFile : IDUIF_FOLDERLAYOUT_DEFAULT,
                                         &pszMarkup);
        if (FAILED(hr))
            return hr;

        pstm = SHCreateMemStream((const BYTE *)pszMarkup, (lstrlenW(pszMarkup) + 1) * sizeof(WCHAR));
        if (pstm)
        {
            hr = CStreamWithLayoutType::Create(pstm, _layoutType, ppunk);
            pstm->Release();
        }
        else
        {
            hr = E_OUTOFMEMORY;
        }
        LocalFree(pszMarkup);
        return hr;
    }

    STDMETHODIMP
    GetPossibleRedirectTarget(
        REFGUID rguid,
        int *pfRedirect,
        PIDLIST_ABSOLUTE *ppidl) override
    {
        *pfRedirect = FALSE;
        return S_OK;
    }

    STDMETHODIMP
    GetLayoutProperties(
        LAYOUTPROPERTIES *plp) override
    {
        const COMMONLAYOUT *pLayout = FindCommonLayout(_layoutType);

        *plp = (pLayout && pLayout->fHideContentPane) ? LP_HIDECONTENTPANE : LP_NONE;
        return S_OK;
    }

private:
    LONG _cRef;
    LAYOUTTYPE _layoutType;
};

HRESULT
WINAPI
CommonLayoutDefinition_CreateInstance(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CCommonLayoutDefinition *pcld;
    HRESULT hr;

    *ppv = NULL;
    pcld = new CCommonLayoutDefinition();
    if (!pcld)
        return E_OUTOFMEMORY;

    hr = pcld->QueryInterface(riid, ppv);
    pcld->Release();
    return hr;
}

/* CCommonLayoutModifier: places a folder layout inside the browser chrome markup */

class CCommonLayoutModifier : public ICommonLayoutModifier, public ILayoutModifier, public CDUIFrameZeroAlloc
{
public:
    CCommonLayoutModifier() : _cRef(1), _blt(BROWSERLAYOUTTYPE_NONE) {}

    STDMETHODIMP
    QueryInterface(
        REFIID riid,
        void **ppv) override
    {
        static const QITAB qit[] =
        {
            QITABENT(CCommonLayoutModifier, ICommonLayoutModifier),
            QITABENT(CCommonLayoutModifier, ILayoutModifier),
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
    ChooseBrowserLayout(
        BROWSERLAYOUTTYPE blt) override
    {
        _blt = blt;
        return S_OK;
    }

    STDMETHODIMP AdjustLayout(IUnknown *punkFolderLayout, IUnknown **ppunkCompleteLayout) override;

private:
    UINT _GetChromeUIFile();

    LONG _cRef;
    BROWSERLAYOUTTYPE _blt;
};

UINT
CCommonLayoutModifier::_GetChromeUIFile()
{
    switch (_blt)
    {
        case BROWSERLAYOUTTYPE_NORMAL:
            return IDUIF_CHROME_NORMAL;
        case BROWSERLAYOUTTYPE_COMMONOPENDIALOG:
            return IDUIF_CHROME_OPENDIALOG;
        case BROWSERLAYOUTTYPE_COMMONSAVEDIALOG:
            return IDUIF_CHROME_SAVEDIALOG;
        case BROWSERLAYOUTTYPE_COMMONMINISAVEDIALOG:
            return IDUIF_CHROME_MINISAVEDIALOG;
        case BROWSERLAYOUTTYPE_COMMONFOLDERDIALOG:
            return IDUIF_CHROME_FOLDERDIALOG;
        default:
            return IDUIF_CHROME_NONE;
    }
}

/* The chrome markup goes in front of the folder layout's closing tag, keeping one duixml document */
STDMETHODIMP
CCommonLayoutModifier::AdjustLayout(
    IUnknown *punkFolderLayout,
    IUnknown **ppunkCompleteLayout)
{
    LAYOUTTYPE layoutType = GetLayoutTypeOf(punkFolderLayout);
    PWSTR pszChrome;
    IStream *pstmFolder;
    ULARGE_INTEGER cbFolder;
    HRESULT hr;

    *ppunkCompleteLayout = NULL;
    hr = DUI_LoadUIFileFromResources(DUIFRAME_HINST, _GetChromeUIFile(), &pszChrome);
    if (FAILED(hr))
        return hr;

    hr = punkFolderLayout->QueryInterface(IID_PPV_ARGS(&pstmFolder));
    if (SUCCEEDED(hr))
    {
        hr = IStream_Size(pstmFolder, &cbFolder);
        if (SUCCEEDED(hr))
        {
            UINT cchFolder = cbFolder.LowPart / sizeof(WCHAR);
            UINT cchTotal = cchFolder + lstrlenW(pszChrome);
            PWSTR pszMerged = (PWSTR)LocalAlloc(LPTR, cchTotal * sizeof(WCHAR));

            if (!pszMerged)
            {
                hr = E_OUTOFMEMORY;
            }
            else
            {
                UINT cchRead = min(cchFolder - 1, cchTotal);

                hr = IStream_Read(pstmFolder, pszMerged, cchRead * sizeof(WCHAR));
                if (SUCCEEDED(hr))
                {
                    PWSTR pszClose = StrRStrIW(pszMerged, pszMerged + cchRead, L"</duixml>");
                    UINT cchKeep = pszClose ? (UINT)(pszClose - pszMerged) : cchRead;
                    IStream *pstmMerged;

                    StringCchCopyW(pszMerged + cchKeep, cchTotal - cchKeep, pszChrome);
                    if (pszClose)
                        StringCchCatW(pszMerged, cchTotal, L"</duixml>");

                    pstmMerged = SHCreateMemStream((const BYTE *)pszMerged, cchTotal * sizeof(WCHAR));
                    if (pstmMerged)
                    {
                        hr = CStreamWithLayoutType::Create(pstmMerged, layoutType, ppunkCompleteLayout);
                        pstmMerged->Release();
                    }
                    else
                    {
                        hr = E_OUTOFMEMORY;
                    }
                }
                LocalFree(pszMerged);
            }
        }
        pstmFolder->Release();
    }
    LocalFree(pszChrome);
    return hr;
}

HRESULT
WINAPI
CommonLayoutModifier_CreateInstance(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CCommonLayoutModifier *pclm;
    HRESULT hr;

    *ppv = NULL;
    pclm = new CCommonLayoutModifier();
    if (!pclm)
        return E_OUTOFMEMORY;

    hr = pclm->QueryInterface(riid, ppv);
    pclm->Release();
    return hr;
}

/* Layout definition lookup for a location */

static
HRESULT
CreateCommonFrame(
    _In_ LAYOUTTYPE layoutType,
    _COM_Outptr_ IFrameLayoutDefinition **ppfld)
{
    ICommonLayoutDefinition *pcld;
    HRESULT hr;

    *ppfld = NULL;
    hr = CommonLayoutDefinition_CreateInstance(IID_PPV_ARGS(&pcld));
    if (SUCCEEDED(hr))
    {
        hr = pcld->ChooseLayout(layoutType);
        if (SUCCEEDED(hr))
            hr = pcld->QueryInterface(IID_PPV_ARGS(ppfld));
        pcld->Release();
    }
    return hr;
}

/* A folder supplies its own layout; others get the common layout of their folder type */
static
HRESULT
GetLayoutDefinitionFromItem(
    _In_ GETLAYOUTREASON reason,
    _In_ IShellItem *psi,
    _In_opt_ HWND hwnd,
    _COM_Outptr_ IFrameLayoutDefinition **ppfld)
{
    IShellFolder *psf;
    IFrameLayoutDefinitionFactory *pfldf;
    HRESULT hr;

    *ppfld = NULL;
    hr = psi->BindToHandler(NULL, BHID_SFObject, IID_PPV_ARGS(&psf));
    if (FAILED(hr))
        return hr;

    hr = psf->CreateViewObject(hwnd, IID_PPV_ARGS(&pfldf));
    if (SUCCEEDED(hr))
    {
        hr = pfldf->GetFrameLayoutDefinition(reason, IID_PPV_ARGS(ppfld));
        pfldf->Release();
    }
    else if (reason == GLR_DEFAULT)
    {
        hr = psf->CreateViewObject(hwnd, IID_PPV_ARGS(ppfld));
    }

    /* Folder type descriptions (SHGetFolderTypeDescription) are not implemented, so every folder
       type falls back to the normal layout here */
    if (FAILED(hr) && reason == GLR_DEFAULT && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        hr = CreateCommonFrame(LAYOUTTYPE_NORMAL, ppfld);

    psf->Release();
    return hr;
}

static
HRESULT
GetLayoutDefinition(
    _In_ GETLAYOUTREASON reason,
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_opt_ HWND hwnd,
    _COM_Outptr_ IFrameLayoutDefinition **ppfld)
{
    IShellItem *psi;
    HRESULT hr;

    *ppfld = NULL;
    hr = SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&psi));
    if (SUCCEEDED(hr))
    {
        hr = GetLayoutDefinitionFromItem(reason, psi, hwnd, ppfld);
        psi->Release();
    }
    return hr;
}

/* Follows layout redirects (a location that asks to be shown with another one's layout) */
HRESULT
WINAPI
GetLayoutDefinitionAllowRedirect(
    _In_ GETLAYOUTREASON reason,
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_opt_ HWND hwnd,
    _In_ REFGUID rguidRedirectKind,
    _COM_Outptr_ IFrameLayoutDefinition **ppfld)
{
    PIDLIST_ABSOLUTE pidlTarget;
    int fRedirect;
    HRESULT hr;

    hr = GetLayoutDefinition(reason, pidl, hwnd, ppfld);
    if (FAILED(hr))
        return hr;

    hr = (*ppfld)->GetPossibleRedirectTarget(rguidRedirectKind, &fRedirect, &pidlTarget);
    if (SUCCEEDED(hr) && fRedirect)
    {
        (*ppfld)->Release();
        *ppfld = NULL;
        hr = GetLayoutDefinitionAllowRedirect(reason, pidlTarget, hwnd, rguidRedirectKind, ppfld);
        ILFree(pidlTarget);
    }
    return hr;
}
