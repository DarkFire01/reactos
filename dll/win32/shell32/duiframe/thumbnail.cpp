/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Details and reading pane thumbnail: item images and their host element
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include <DUser.h>

#ifndef DISPID_UPDATEIMAGE
#define DISPID_UPDATEIMAGE 222
#endif

/* Image extraction interfaces the ReactOS SDK headers do not declare yet */
MIDL_INTERFACE("BB2E617C-0920-11D1-9A0B-00C04FC2D6C1")
IThumbExtractImage : public IUnknown
{
    STDMETHOD(GetLocation)(
        _Out_writes_(cch) PWSTR pszPathBuffer,
        _In_ DWORD cch,
        _Inout_opt_ DWORD *pdwPriority,
        _In_ const SIZE *prgSize,
        _In_ DWORD dwRecClrDepth,
        _Inout_ DWORD *pdwFlags) PURE;
    STDMETHOD(Extract)(
        _Out_ HBITMAP *phBmpThumbnail) PURE;
};

MIDL_INTERFACE("BCC18B79-BA16-442F-80C4-8A59C30C463B")
IThumbItemImageFactory : public IUnknown
{
    STDMETHOD(GetImage)(
        _In_ SIZE size,
        _In_ DWORD dwFlags,
        _Out_ HBITMAP *phbm) PURE;
};

MIDL_INTERFACE("E357FCCD-A995-4576-B01F-234630154E96")
IThumbProvider : public IUnknown
{
    STDMETHOD(GetThumbnail)(
        _In_ UINT cx,
        _Out_ HBITMAP *phbmp,
        _Out_ DWORD *pdwAlpha) PURE;
};

MIDL_INTERFACE("0811AEBE-0B87-4C54-9E72-548CF649016B")
IThumbContextMenuSite : public IUnknown
{
    STDMETHOD(DoContextMenuPopup)(
        _In_ IUnknown *punkContextMenu,
        _In_ UINT fFlags,
        _In_ POINT pt) PURE;
};

#ifndef CMF_ITEMMENU
#define CMF_ITEMMENU 0x00000080
#endif

/* IExtractImage flags and IShellItemImageFactory flags */
#define THUMB_IEIFLAG_ORIGSIZE      0x0040
#define THUMB_IEIFLAG_QUALITY       0x0200
#define THUMB_SIIGBF_THUMBNAILONLY  0x0008

/* Shell extension key of thumbnail providers */
static const WCHAR c_szThumbnailProviderKey[] = L"{e357fccd-a995-4576-b01f-234630154e96}";

/* Task ids of the thumbnail work items, one per pane orientation */
static const GUID TOID_HorizontalPanePrefetchTask =
    { 0xd7a7571f, 0xdc97, 0x4da1, { 0xb0, 0xfc, 0x6e, 0x63, 0xa6, 0x9d, 0x97, 0x27 } };
static const GUID TOID_VerticalPanePrefetchTask =
    { 0x33c301e6, 0xc545, 0x43b1, { 0xbc, 0x39, 0xd5, 0xbb, 0x09, 0x29, 0x69, 0x9f } };
static const GUID TOID_HorizontalDevirtualizeSelectionTask =
    { 0x3ce2abd2, 0x759b, 0x4581, { 0xb9, 0xeb, 0xb7, 0x62, 0x50, 0x72, 0x82, 0x20 } };
static const GUID TOID_VerticalDevirtualizeSelectionTask =
    { 0xe8b56a60, 0x53bb, 0x4617, { 0x92, 0x5f, 0x4d, 0x3a, 0x1b, 0x7b, 0x7f, 0x63 } };

static const PROPERTYKEY c_pkeyItemType =
    { { 0x28636aa6, 0x953d, 0x11d2, { 0xb5, 0xd6, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0xd0 } }, 11 };
static const PROPERTYKEY c_pkeyDateModified =
    { { 0xb725f130, 0x47ef, 0x101a, { 0xa5, 0xf1, 0x02, 0x60, 0x8c, 0x9e, 0xeb, 0xac } }, 14 };

/* Item thumbnail defaults: largest image, stacking step out of 256, most items drawn */
#define ITEMTHUMB_MAXSIZE       256
#define ITEMTHUMB_STEPX         7
#define ITEMTHUMB_STEPY         9
#define ITEMTHUMB_MAXITEMS      7

/* Below this size an item shows its icon unless its type says otherwise */
#define ITEMTHUMB_DEFAULTCUTOFF 20

/* Thumbnail size for each ThumbnailCutoff registry value */
static const int c_rgcxThumbnailCutoff[] = { 256, 96, 48, 32, 16 };

static const WCHAR c_szPreviewPaneVisible[] = L"PreviewPaneSizer_Visible";
static const WCHAR c_szReadingPaneVisible[] = L"ReadingPaneSizer_Visible";

/* Shared helpers */

HRESULT
WINAPI
DetailsPane_GetTaskManager(
    _In_opt_ IUnknown *punkSite,
    _COM_Outptr_ IFrameTaskManager **ppftm)
{
    IObjectProvider *pop;
    HRESULT hr;

    *ppftm = NULL;
    hr = IUnknown_QueryService(punkSite, IID_IFrameManager, IID_PPV_ARGS(&pop));
    if (SUCCEEDED(hr))
    {
        hr = pop->QueryObject(OID_FrameTaskManager, IID_PPV_ARGS(ppftm));
        pop->Release();
    }
    return hr;
}

static
BOOL
DetailsPane_ReadBrowserBOOL(
    _In_opt_ IUnknown *punkSite,
    _In_ PCWSTR pszName,
    _In_ BOOL fDefault)
{
    IPropertyBag *ppb;
    BOOL fValue = fDefault;

    if (SUCCEEDED(IUnknown_QueryService(punkSite, SID_PerBrowserPropertyBag, IID_PPV_ARGS(&ppb))))
    {
        fValue = PropertyBag_ReadBOOLDefault(ppb, pszName, fDefault);
        ppb->Release();
    }
    return fValue;
}

BOOL
WINAPI
DetailsPane_IsPreviewPaneVisible(
    _In_opt_ IUnknown *punkSite)
{
    return DetailsPane_ReadBrowserBOOL(punkSite, c_szPreviewPaneVisible, TRUE);
}

BOOL
WINAPI
DetailsPane_IsReadingPaneVisible(
    _In_opt_ IUnknown *punkSite)
{
    return DetailsPane_ReadBrowserBOOL(punkSite, c_szReadingPaneVisible, FALSE);
}

VOID
WINAPI
DetailsPane_FitToSize(
    _In_ const SIZE *psizeMax,
    _Inout_ RECT *prc)
{
    int cx = prc->right - prc->left;
    int cy = prc->bottom - prc->top;

    if (cx <= 0 || cy <= 0 || psizeMax->cx <= 0 || psizeMax->cy <= 0)
        return;

    /* Whichever side is relatively longer fills its bound */
    if ((LONGLONG)cx * psizeMax->cy > (LONGLONG)cy * psizeMax->cx)
    {
        cy = max(MulDiv(cy, psizeMax->cx, cx), 1);
        cx = psizeMax->cx;
    }
    else
    {
        cx = max(MulDiv(cx, psizeMax->cy, cy), 1);
        cy = psizeMax->cy;
    }
    prc->right = prc->left + cx;
    prc->bottom = prc->top + cy;
}

/* ThumbnailCutoff of the file type, cached per type is not needed at this rate */
static
int
ItemThumbnail_GetCutoffFromType(
    _In_opt_ PCWSTR pszType)
{
    DWORD dwCutoff;
    DWORD cbData = sizeof(dwCutoff);
    HKEY hkey;

    if (!pszType || !*pszType)
        return ITEMTHUMB_DEFAULTCUTOFF;

    if (FAILED(AssocQueryKeyW(ASSOCF_NONE, ASSOCKEY_CLASS, pszType, NULL, &hkey)))
        return ITEMTHUMB_DEFAULTCUTOFF;

    if (SHGetValueW(hkey, NULL, L"ThumbnailCutoff", NULL, &dwCutoff, &cbData) != ERROR_SUCCESS ||
        dwCutoff >= _countof(c_rgcxThumbnailCutoff))
    {
        RegCloseKey(hkey);
        return ITEMTHUMB_DEFAULTCUTOFF;
    }
    RegCloseKey(hkey);
    return c_rgcxThumbnailCutoff[dwCutoff];
}

/* Copies any bitmap into a top-down 32bpp DIB with premultiplied alpha, opaque when it had none */
static
HBITMAP
ItemThumbnail_NormalizeBitmap(
    _In_ HBITMAP hbmSource,
    _Out_ SIZE *psize)
{
    BITMAPINFO bmi = { 0 };
    BITMAP bm;
    HBITMAP hbm;
    BYTE *pbBits;
    HDC hdc;
    BOOL fAlpha = FALSE;
    int cPixels;

    psize->cx = psize->cy = 0;
    if (!GetObjectW(hbmSource, sizeof(bm), &bm) || bm.bmWidth <= 0 || !bm.bmHeight)
        return NULL;

    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = bm.bmWidth;
    bmi.bmiHeader.biHeight = -abs(bm.bmHeight);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hbm = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, (void **)&pbBits, NULL, 0);
    if (!hbm)
        return NULL;

    hdc = GetDC(NULL);
    if (!GetDIBits(hdc, hbmSource, 0, abs(bm.bmHeight), pbBits, &bmi, DIB_RGB_COLORS))
    {
        ReleaseDC(NULL, hdc);
        DeleteObject(hbm);
        return NULL;
    }
    ReleaseDC(NULL, hdc);

    cPixels = bm.bmWidth * abs(bm.bmHeight);
    if (bm.bmBitsPixel == 32)
    {
        for (int i = 0; i < cPixels && !fAlpha; i++)
            fAlpha = pbBits[i * 4 + 3] != 0;
    }

    for (int i = 0; i < cPixels; i++)
    {
        BYTE *pb = &pbBits[i * 4];

        if (!fAlpha)
        {
            pb[3] = 0xFF;
        }
        else if (pb[3] != 0xFF)
        {
            pb[0] = (BYTE)(pb[0] * pb[3] / 255);
            pb[1] = (BYTE)(pb[1] * pb[3] / 255);
            pb[2] = (BYTE)(pb[2] * pb[3] / 255);
        }
    }

    psize->cx = bm.bmWidth;
    psize->cy = abs(bm.bmHeight);
    return hbm;
}

/* The thumbnail provider registered for the item's type, initialized on the item */
static
HRESULT
ItemThumbnail_GetProviderImage(
    _In_ IShellFolder *psf,
    _In_ PCUITEMID_CHILD pidl,
    _In_ int cx,
    _Out_ HBITMAP *phbm)
{
    IThumbProvider *ptp = NULL;
    IShellItem *psi;
    WCHAR szClsid[40];
    DWORD cchClsid = _countof(szClsid);
    PWSTR pszPath = NULL;
    PWSTR pszExt;
    DWORD dwAlpha;
    CLSID clsid;
    HRESULT hr;

    *phbm = NULL;
    hr = SHCreateItemWithParent(NULL, psf, pidl, IID_PPV_ARGS(&psi));
    if (FAILED(hr))
        return hr;

    hr = psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
    if (SUCCEEDED(hr))
    {
        pszExt = PathFindExtensionW(pszPath);
        hr = AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_SHELLEXTENSION, pszExt, c_szThumbnailProviderKey,
                               szClsid, &cchClsid);
        if (SUCCEEDED(hr))
            hr = CLSIDFromString(szClsid, &clsid);
        if (SUCCEEDED(hr))
            hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ptp));
    }

    if (SUCCEEDED(hr))
    {
        IInitializeWithStream *piws;
        IInitializeWithItem *piwi;
        IInitializeWithFile *piwf;

        if (SUCCEEDED(ptp->QueryInterface(IID_PPV_ARGS(&piws))))
        {
            IStream *pstm;

            hr = SHCreateStreamOnFileEx(pszPath, STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, NULL, &pstm);
            if (SUCCEEDED(hr))
            {
                hr = piws->Initialize(pstm, STGM_READ);
                pstm->Release();
            }
            piws->Release();
        }
        else if (SUCCEEDED(ptp->QueryInterface(IID_PPV_ARGS(&piwi))))
        {
            hr = piwi->Initialize(psi, STGM_READ);
            piwi->Release();
        }
        else if (SUCCEEDED(ptp->QueryInterface(IID_PPV_ARGS(&piwf))))
        {
            hr = piwf->Initialize(pszPath, STGM_READ);
            piwf->Release();
        }
        else
        {
            hr = E_NOINTERFACE;
        }

        if (SUCCEEDED(hr))
            hr = ptp->GetThumbnail(cx, phbm, &dwAlpha);
        ptp->Release();
    }

    CoTaskMemFree(pszPath);
    psi->Release();
    return hr;
}

/* A real thumbnail: the item's image factory, its extractor, then the type's thumbnail provider */
static
HBITMAP
ItemThumbnail_ExtractThumbnail(
    _In_ IShellFolder *psf,
    _In_ PCUITEMID_CHILD pidl,
    _In_ int cx)
{
    IThumbItemImageFactory *pif;
    IThumbExtractImage *pei;
    IShellItem *psi;
    HBITMAP hbm = NULL;
    SIZE size = { cx, cx };

    if (SUCCEEDED(SHCreateItemWithParent(NULL, psf, pidl, IID_PPV_ARGS(&psi))))
    {
        if (SUCCEEDED(psi->QueryInterface(IID_PPV_ARGS(&pif))))
        {
            if (FAILED(pif->GetImage(size, THUMB_SIIGBF_THUMBNAILONLY, &hbm)))
                hbm = NULL;
            pif->Release();
        }
        psi->Release();
    }

    if (!hbm && SUCCEEDED(psf->GetUIObjectOf(NULL, 1, &pidl, __uuidof(IThumbExtractImage), NULL, (void **)&pei)))
    {
        WCHAR szLocation[MAX_PATH];
        DWORD dwPriority = 0;
        DWORD dwFlags = THUMB_IEIFLAG_ORIGSIZE | THUMB_IEIFLAG_QUALITY;

        if (SUCCEEDED(pei->GetLocation(szLocation, _countof(szLocation), &dwPriority, &size, 32, &dwFlags)))
        {
            if (FAILED(pei->Extract(&hbm)))
                hbm = NULL;
        }
        pei->Release();
    }

    if (!hbm && FAILED(ItemThumbnail_GetProviderImage(psf, pidl, cx, &hbm)))
        hbm = NULL;
    return hbm;
}

/* The item's icon at the requested size, from its icon extractor */
static
HICON
ItemThumbnail_ExtractIcon(
    _In_ IShellFolder *psf,
    _In_ PCUITEMID_CHILD pidl,
    _In_ int cx)
{
    IExtractIconW *pxi;
    WCHAR szFile[MAX_PATH];
    HICON hicon = NULL;
    HICON hiconSmall = NULL;
    UINT uFlags = 0;
    int iIndex = 0;
    HRESULT hr;

    if (FAILED(psf->GetUIObjectOf(NULL, 1, &pidl, IID_IExtractIconW, NULL, (void **)&pxi)))
        return NULL;

    hr = pxi->GetIconLocation(GIL_FORSHELL, szFile, _countof(szFile), &iIndex, &uFlags);
    if (SUCCEEDED(hr))
    {
        hr = pxi->Extract(szFile, iIndex, &hicon, &hiconSmall, MAKELONG(cx, GetSystemMetrics(SM_CXSMICON)));
        if (hiconSmall)
            DestroyIcon(hiconSmall);

        /* S_FALSE asks the caller to extract the location itself */
        if (hr != S_OK)
        {
            hicon = NULL;
            if (!(uFlags & GIL_NOTFILENAME) &&
                PrivateExtractIconsW(szFile, iIndex, cx, cx, &hicon, NULL, 1, LR_DEFAULTCOLOR) != 1)
            {
                hicon = NULL;
            }
        }
    }
    pxi->Release();
    return hicon;
}

static
VOID
ItemThumbnail_FreeImage(
    _Inout_ ITEMTHUMBNAILENTRY *pEntry)
{
    if (pEntry->hbmImage)
    {
        DeleteObject(pEntry->hbmImage);
        pEntry->hbmImage = NULL;
    }
    if (pEntry->hiconImage)
    {
        DestroyIcon(pEntry->hiconImage);
        pEntry->hiconImage = NULL;
    }
    pEntry->sizeImage.cx = pEntry->sizeImage.cy = 0;
}

static
int
CALLBACK
ItemThumbnail_FreeImageCB(
    _In_ void *p,
    _In_opt_ void *pData)
{
    UNREFERENCED_PARAMETER(pData);
    ItemThumbnail_FreeImage((ITEMTHUMBNAILENTRY *)p);
    ILFree(((ITEMTHUMBNAILENTRY *)p)->pidl);
    return TRUE;
}

/* CItemImageFetchWorkItem: extracts the item images off the frame thread */

class CItemImageFetchWorkItem : public CFrameWorkItem
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_ CItemThumbnail *pThumbnail,
        _In_opt_ IItemThumbnailCallback *pCallback,
        _In_ int cxIcon,
        _In_ int cxThumbnail,
        _COM_Outptr_ IFrameWorkItem **ppfwi);

    STDMETHODIMP DoWork() override;
    STDMETHODIMP Dispatch() override;

protected:
    virtual ~CItemImageFetchWorkItem();

private:
    CItemThumbnail *_pThumbnail;
    IItemThumbnailCallback *_pCallback;
    PIDLIST_ABSOLUTE _pidlParent;
    HDSA _hdsaImages;
    UINT _uGeneration;
    DWORD _dwFlags;
    int _cxIcon;
    int _cxThumbnail;
};

HRESULT
WINAPI
CItemImageFetchWorkItem::CreateInstance(
    _In_ CItemThumbnail *pThumbnail,
    _In_opt_ IItemThumbnailCallback *pCallback,
    _In_ int cxIcon,
    _In_ int cxThumbnail,
    _COM_Outptr_ IFrameWorkItem **ppfwi)
{
    CItemImageFetchWorkItem *pwi;
    int cItems = DSA_GetItemCount(pThumbnail->_hdsaItems);
    HRESULT hr;

    *ppfwi = NULL;
    pwi = new CItemImageFetchWorkItem();
    if (!pwi)
        return E_OUTOFMEMORY;

    pwi->_pThumbnail = pThumbnail;
    pThumbnail->AddRef();
    IUnknown_Set((IUnknown **)&pwi->_pCallback, pCallback);
    pwi->_uGeneration = pThumbnail->_uGeneration;
    pwi->_dwFlags = pThumbnail->_dwFlags;
    pwi->_cxIcon = cxIcon;
    pwi->_cxThumbnail = cxThumbnail;
    pwi->_pidlParent = ILClone(pThumbnail->_pidlParent);
    pwi->_hdsaImages = DSA_Create(sizeof(ITEMTHUMBNAILENTRY), max(cItems, 1));

    hr = (pwi->_pidlParent && pwi->_hdsaImages) ? S_OK : E_OUTOFMEMORY;
    for (int i = 0; i < cItems && SUCCEEDED(hr); i++)
    {
        ITEMTHUMBNAILENTRY entry = *(ITEMTHUMBNAILENTRY *)DSA_GetItemPtr(pThumbnail->_hdsaItems, i);

        entry.hbmImage = NULL;
        entry.hiconImage = NULL;
        entry.sizeImage.cx = entry.sizeImage.cy = 0;
        entry.pidl = ILClone(entry.pidl);
        if (!entry.pidl || DSA_AppendItem(pwi->_hdsaImages, &entry) == DSA_ERR)
        {
            ILFree(entry.pidl);
            hr = E_OUTOFMEMORY;
        }
    }

    if (SUCCEEDED(hr))
        hr = pwi->QueryInterface(IID_PPV_ARGS(ppfwi));
    pwi->Release();
    return hr;
}

CItemImageFetchWorkItem::~CItemImageFetchWorkItem()
{
    if (_hdsaImages)
        DSA_DestroyCallback(_hdsaImages, ItemThumbnail_FreeImageCB, NULL);
    ILFree(_pidlParent);
    IUnknown_SafeReleaseAndNullPtr(&_pCallback);
    if (_pThumbnail)
        _pThumbnail->Release();
}

/* Thumbnails for items whose type allows them at this size, icons for the rest */
STDMETHODIMP
CItemImageFetchWorkItem::DoWork()
{
    IShellFolder *psf;
    HRESULT hr;

    hr = SHBindToObject(NULL, _pidlParent, NULL, IID_PPV_ARGS(&psf));
    if (FAILED(hr))
        return hr;

    for (int i = 0; i < DSA_GetItemCount(_hdsaImages) && _ShouldContinue(); i++)
    {
        ITEMTHUMBNAILENTRY *pEntry = (ITEMTHUMBNAILENTRY *)DSA_GetItemPtr(_hdsaImages, i);

        if (!(_dwFlags & ITSIF_ICONSONLY) && _cxThumbnail >= pEntry->iCutoff)
        {
            HBITMAP hbm = ItemThumbnail_ExtractThumbnail(psf, pEntry->pidl, _cxThumbnail);

            if (hbm)
            {
                pEntry->hbmImage = ItemThumbnail_NormalizeBitmap(hbm, &pEntry->sizeImage);
                DeleteObject(hbm);
            }
        }

        if (!pEntry->hbmImage)
        {
            pEntry->hiconImage = ItemThumbnail_ExtractIcon(psf, pEntry->pidl, _cxIcon);
            if (pEntry->hiconImage)
            {
                pEntry->sizeImage.cx = _cxIcon;
                pEntry->sizeImage.cy = _cxIcon;
            }
        }
    }

    psf->Release();
    return S_OK;
}

STDMETHODIMP
CItemImageFetchWorkItem::Dispatch()
{
    HRESULT hr;

    hr = _pThumbnail->NotifyFetchComplete(_uGeneration, _cxIcon, _hdsaImages);
    if (hr == S_OK)
    {
        /* The thumbnail took the images */
        DSA_Destroy(_hdsaImages);
        _hdsaImages = NULL;
        if (_pCallback)
            _pCallback->OnNewImageAvailable();
    }
    return hr;
}

/* CItemThumbnail */

CItemThumbnail::CItemThumbnail() :
    _cRef(1),
    _cMaxItems(ITEMTHUMB_MAXITEMS),
    _dxStep(ITEMTHUMB_STEPX),
    _dyStep(ITEMTHUMB_STEPY)
{
}

CItemThumbnail::~CItemThumbnail()
{
    _ReleaseItems();
}

HRESULT
WINAPI
CItemThumbnail::CreateInstance(
    _In_ int cxMax,
    _In_ int dxStep,
    _In_ int dyStep,
    _In_ int cMaxItems,
    _In_ BOOL fAsync,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CItemThumbnail *pit;
    HRESULT hr;

    *ppv = NULL;
    pit = new CItemThumbnail();
    if (!pit)
        return E_OUTOFMEMORY;

    hr = pit->SetMultiItemLayout(dxStep, dyStep, cMaxItems);
    if (SUCCEEDED(hr))
    {
        pit->_fAsync = fAsync;
        pit->_cxMax = cxMax;
        hr = pit->QueryInterface(riid, ppv);
    }
    pit->Release();
    return hr;
}

STDMETHODIMP
CItemThumbnail::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        { &__uuidof(IItemThumbnail), OFFSETOFCLASS(IItemThumbnail, CItemThumbnail) },
        {0},
    };

    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CItemThumbnail::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CItemThumbnail::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

STDMETHODIMP
CItemThumbnail::SetAsync(
    BOOL fAsync)
{
    _fAsync = fAsync;
    return S_OK;
}

/* The stack offsets are in 1/256 of the drawn size, so they must leave room for the last image */
STDMETHODIMP
CItemThumbnail::SetMultiItemLayout(
    int dxStep,
    int dyStep,
    int cMaxItems)
{
    if ((cMaxItems - 1) * max(dxStep, dyStep) >= 256)
        return E_INVALIDARG;

    _dxStep = dxStep;
    _dyStep = dyStep;
    _cMaxItems = cMaxItems;
    return S_OK;
}

int
CALLBACK
CItemThumbnail::s_FreeEntryCB(
    _In_ void *p,
    _In_opt_ void *pData)
{
    return ItemThumbnail_FreeImageCB(p, pData);
}

void
CItemThumbnail::_ReleaseItems()
{
    if (_hdsaItems)
    {
        DSA_DestroyCallback(_hdsaItems, s_FreeEntryCB, NULL);
        _hdsaItems = NULL;
    }
    ILFree(_pidlParent);
    _pidlParent = NULL;
    IUnknown_SafeReleaseAndNullPtr(&_psfParent);
    _cxFetched = 0;
}

/* Up to the item limit, every item keyed by its child ID under the first item's parent */
HRESULT
CItemThumbnail::_CreateEntries(
    _In_ IShellItemArray *psia,
    _COM_Outptr_ IShellFolder **ppsf,
    _Outptr_ PIDLIST_ABSOLUTE *ppidlParent,
    _Out_ HDSA *phdsa)
{
    DWORD cItems = 0;
    HDSA hdsa;
    HRESULT hr;

    *ppsf = NULL;
    *ppidlParent = NULL;
    *phdsa = NULL;

    hr = psia->GetCount(&cItems);
    if (FAILED(hr))
        return hr;
    cItems = min(cItems, (DWORD)_cMaxItems);
    if (!cItems)
        return E_FAIL;

    hdsa = DSA_Create(sizeof(ITEMTHUMBNAILENTRY), cItems);
    if (!hdsa)
        return E_OUTOFMEMORY;

    for (DWORD i = 0; i < cItems && SUCCEEDED(hr); i++)
    {
        PIDLIST_ABSOLUTE pidlFull = NULL;
        ITEMTHUMBNAILENTRY entry = { 0 };
        IShellItem *psi;

        hr = psia->GetItemAt(i, &psi);
        if (SUCCEEDED(hr))
        {
            hr = SHGetIDListFromObject(psi, &pidlFull);
            psi->Release();
        }

        if (SUCCEEDED(hr) && !*ppsf)
        {
            hr = SHBindToParent(pidlFull, IID_PPV_ARGS(ppsf), NULL);
            if (SUCCEEDED(hr))
            {
                *ppidlParent = ILClone(pidlFull);
                if (*ppidlParent)
                    ILRemoveLastID(*ppidlParent);
                else
                    hr = E_OUTOFMEMORY;
            }
        }

        if (SUCCEEDED(hr))
        {
            entry.pidl = ILClone(ILFindLastID(pidlFull));
            entry.iSysImage = -1;
            entry.iCutoff = ITEMTHUMB_DEFAULTCUTOFF;
            if (!entry.pidl || DSA_AppendItem(hdsa, &entry) == DSA_ERR)
            {
                ILFree(entry.pidl);
                hr = E_OUTOFMEMORY;
            }
        }
        ILFree(pidlFull);
    }

    if (FAILED(hr))
    {
        DSA_DestroyCallback(hdsa, s_FreeEntryCB, NULL);
        IUnknown_SafeReleaseAndNullPtr(ppsf);
        ILFree(*ppidlParent);
        *ppidlParent = NULL;
        return hr;
    }

    *phdsa = hdsa;
    return S_OK;
}

/* Same folder, same items in the same order, none modified since */
BOOL
CItemThumbnail::_NewItemsEqualToCurrent(
    _In_ IShellFolder *psf,
    _In_ PCIDLIST_ABSOLUTE pidlParent,
    _In_ HDSA hdsa)
{
    IShellFolder2 *psf2;
    BOOL fEqual = TRUE;
    int cItems = DSA_GetItemCount(hdsa);

    if (!_hdsaItems || DSA_GetItemCount(_hdsaItems) != cItems || !_pidlParent ||
        !ILIsEqual(_pidlParent, pidlParent))
    {
        return FALSE;
    }

    if (FAILED(psf->QueryInterface(IID_PPV_ARGS(&psf2))))
        return FALSE;

    for (int i = 0; i < cItems && fEqual; i++)
    {
        ITEMTHUMBNAILENTRY *pOld = (ITEMTHUMBNAILENTRY *)DSA_GetItemPtr(_hdsaItems, i);
        ITEMTHUMBNAILENTRY *pNew = (ITEMTHUMBNAILENTRY *)DSA_GetItemPtr(hdsa, i);
        VARIANT varOld;
        VARIANT varNew;

        fEqual = FALSE;
        if (HRESULT_CODE(psf2->CompareIDs(SHCIDS_CANONICALONLY, pOld->pidl, pNew->pidl)) != 0)
            break;

        VariantInit(&varOld);
        VariantInit(&varNew);
        if (SUCCEEDED(psf2->GetDetailsEx(pOld->pidl, (const SHCOLUMNID *)&c_pkeyDateModified, &varOld)) &&
            SUCCEEDED(psf2->GetDetailsEx(pNew->pidl, (const SHCOLUMNID *)&c_pkeyDateModified, &varNew)))
        {
            fEqual = VarCmp(&varOld, &varNew, LOCALE_USER_DEFAULT, 0) == VARCMP_EQ;
        }
        VariantClear(&varOld);
        VariantClear(&varNew);
    }

    psf2->Release();
    return fEqual;
}

/* S_FALSE means the same items are already showing and nothing needs fetching */
STDMETHODIMP
CItemThumbnail::SetItems(
    IShellItemArray *psia,
    DWORD dwFlags)
{
    PIDLIST_ABSOLUTE pidlParent;
    IShellFolder *psf;
    SHELLSTATE ss = { 0 };
    HDSA hdsa;
    DWORD dwOldFlags;
    HRESULT hr;

    _fSingleColumn = (dwFlags & ITSIF_SINGLECOLUMN) != 0;
    hr = _CreateEntries(psia, &psf, &pidlParent, &hdsa);
    if (FAILED(hr))
        return hr;

    SHGetSetSettings(&ss, SSF_ICONSONLY, FALSE);
    if (ss.fIconsOnly)
        dwFlags = (dwFlags & ~ITSIF_HIDEPENDINGICON) | ITSIF_ICONSONLY;

    dwOldFlags = _dwFlags;
    _dwFlags = dwFlags;
    if ((dwFlags & ITSIF_ICONSONLY) == (dwOldFlags & ITSIF_ICONSONLY) && !(dwFlags & ITSIF_FORCEREFRESH) &&
        _NewItemsEqualToCurrent(psf, pidlParent, hdsa))
    {
        DSA_DestroyCallback(hdsa, s_FreeEntryCB, NULL);
        ILFree(pidlParent);
        psf->Release();
        return S_FALSE;
    }

    _ReleaseItems();
    _uGeneration++;
    for (int i = 0; i < DSA_GetItemCount(hdsa); i++)
    {
        ITEMTHUMBNAILENTRY *pEntry = (ITEMTHUMBNAILENTRY *)DSA_GetItemPtr(hdsa, i);

        pEntry->iSysImage = SHMapPIDLToSystemImageListIndex(psf, pEntry->pidl, NULL);
        if (!(dwFlags & ITSIF_ICONSONLY))
        {
            IShellItem2 *psi2;

            if (SUCCEEDED(SHCreateItemWithParent(NULL, psf, pEntry->pidl, IID_PPV_ARGS(&psi2))))
            {
                PWSTR pszType = NULL;

                if (SUCCEEDED(psi2->GetString(c_pkeyItemType, &pszType)))
                    pEntry->iCutoff = ItemThumbnail_GetCutoffFromType(pszType);
                CoTaskMemFree(pszType);
                psi2->Release();
            }
        }
    }

    _hdsaItems = hdsa;
    _pidlParent = pidlParent;
    _psfParent = psf;
    return S_OK;
}

/* Icons come at the smaller side, thumbnails at the larger one up to the size limit */
STDMETHODIMP
CItemThumbnail::GetPrefetchTask(
    int cx,
    int cy,
    IItemThumbnailCallback *pCallback,
    IFrameWorkItem **ppfwi)
{
    *ppfwi = NULL;
    if (cx <= 0 || cy <= 0)
        return E_INVALIDARG;
    if (!_hdsaItems)
        return E_FAIL;

    _uGeneration++;
    return CItemImageFetchWorkItem::CreateInstance(this, pCallback, min(min(cx, cy), _cxMax),
                                                   min(max(cx, cy), _cxMax), ppfwi);
}

/* Images of an older generation are dropped: the items or the size changed meanwhile */
HRESULT
CItemThumbnail::NotifyFetchComplete(
    _In_ UINT uGeneration,
    _In_ int cxRequested,
    _In_ HDSA hdsaImages)
{
    int cItems;

    if (uGeneration != _uGeneration || !_hdsaItems)
        return S_FALSE;

    cItems = min(DSA_GetItemCount(_hdsaItems), DSA_GetItemCount(hdsaImages));
    for (int i = 0; i < cItems; i++)
    {
        ITEMTHUMBNAILENTRY *pEntry = (ITEMTHUMBNAILENTRY *)DSA_GetItemPtr(_hdsaItems, i);
        ITEMTHUMBNAILENTRY *pResult = (ITEMTHUMBNAILENTRY *)DSA_GetItemPtr(hdsaImages, i);

        ItemThumbnail_FreeImage(pEntry);
        pEntry->hbmImage = pResult->hbmImage;
        pEntry->hiconImage = pResult->hiconImage;
        pEntry->sizeImage = pResult->sizeImage;
        pResult->hbmImage = NULL;
        pResult->hiconImage = NULL;
    }

    /* Results without a matching item give back what they own */
    for (int i = 0; i < DSA_GetItemCount(hdsaImages); i++)
        ItemThumbnail_FreeImageCB(DSA_GetItemPtr(hdsaImages, i), NULL);

    _cxFetched = cxRequested;
    return S_OK;
}

/* An item still waiting for its image is drawn with its large system icon */
HRESULT
CItemThumbnail::_GetMaxImageDimensions(
    _Out_ SIZE *psize)
{
    int cxIcon = GetSystemMetrics(SM_CXICON);

    psize->cx = psize->cy = 0;
    if (!_hdsaItems)
        return E_FAIL;

    for (int i = 0; i < DSA_GetItemCount(_hdsaItems); i++)
    {
        ITEMTHUMBNAILENTRY *pEntry = (ITEMTHUMBNAILENTRY *)DSA_GetItemPtr(_hdsaItems, i);
        SIZE size = pEntry->sizeImage;

        if (!size.cx || !size.cy)
            size.cx = size.cy = _cxFetched ? _cxFetched : cxIcon;

        psize->cx = max(psize->cx, size.cx);
        psize->cy = max(psize->cy, size.cy);
    }
    return S_OK;
}

STDMETHODIMP
CItemThumbnail::GetContentBounds(
    const RECT *prcMax,
    RECT *prcContent)
{
    RECT rc = { 0, 0, 0, 0 };
    SIZE sizeMax = { prcMax->right - prcMax->left, prcMax->bottom - prcMax->top };
    SIZE sizeImages;
    HRESULT hr;

    SetRectEmpty(prcContent);
    if (!_hdsaItems)
        return E_FAIL;

    /* A synchronous thumbnail extracts right away instead of waiting for the work item */
    if (!_fAsync)
    {
        IFrameWorkItem *pfwi;

        hr = GetPrefetchTask(sizeMax.cx, sizeMax.cy, NULL, &pfwi);
        if (SUCCEEDED(hr))
        {
            hr = pfwi->DoWork();
            if (SUCCEEDED(hr))
                hr = pfwi->Dispatch();
            pfwi->Release();
        }
        if (FAILED(hr))
            return hr;
    }

    hr = _GetMaxImageDimensions(&sizeImages);
    if (FAILED(hr))
        return hr;

    rc.right = sizeImages.cx;
    rc.bottom = sizeImages.cy;
    DetailsPane_FitToSize(&sizeMax, &rc);
    SetRect(prcContent, 0, 0, rc.right - rc.left, rc.bottom - rc.top);
    return S_OK;
}

void
CItemThumbnail::_GetImageScaledRect(
    _In_ const ITEMTHUMBNAILENTRY *pEntry,
    _In_ SIZE sizeBox,
    _Out_ RECT *prc)
{
    if (pEntry->sizeImage.cx > 0 && pEntry->sizeImage.cy > 0)
    {
        SetRect(prc, 0, 0, pEntry->sizeImage.cx, pEntry->sizeImage.cy);
        DetailsPane_FitToSize(&sizeBox, prc);
    }
    else
    {
        SetRect(prc, 0, 0, min(sizeBox.cx, sizeBox.cy), min(sizeBox.cx, sizeBox.cy));
    }
}

/* Each further item moves one step right and down; the images share what the steps leave */
STDMETHODIMP
CItemThumbnail::Draw(
    HDC hdc,
    const RECT *prc,
    BOOL fRTL)
{
    IImageList *piml = NULL;
    RECT rcContent;
    SIZE sizeBox;
    int cItems;
    int dxStep;
    int dyStep;
    int xOffset = 0;
    int yOffset = 0;
    HRESULT hr;

    hr = GetContentBounds(prc, &rcContent);
    if (FAILED(hr))
        return hr;

    cItems = DSA_GetItemCount(_hdsaItems);
    dxStep = MulDiv(rcContent.right - rcContent.left, _dxStep, 256);
    dyStep = MulDiv(rcContent.bottom - rcContent.top, _dyStep, 256);
    sizeBox.cx = (rcContent.right - rcContent.left) - dxStep * (cItems - 1);
    sizeBox.cy = (rcContent.bottom - rcContent.top) - dyStep * (cItems - 1);

    SHGetImageList(SHIL_LARGE, IID_PPV_ARGS(&piml));
    for (int i = 0; i < cItems; i++)
    {
        ITEMTHUMBNAILENTRY *pEntry = (ITEMTHUMBNAILENTRY *)DSA_GetItemPtr(_hdsaItems, i);
        BOOL fPending = !pEntry->hbmImage && !pEntry->hiconImage;
        RECT rc;

        if (!fPending || !(_dwFlags & ITSIF_HIDEPENDINGICON))
        {
            _GetImageScaledRect(pEntry, sizeBox, &rc);
            if (fRTL && !_fSingleColumn)
                OffsetRect(&rc, sizeBox.cx - rc.right, 0);
            if (!_fSingleColumn)
                OffsetRect(&rc, xOffset, sizeBox.cy - rc.bottom + yOffset);
            OffsetRect(&rc, prc->left, prc->top);

            if (pEntry->hbmImage)
            {
                HDC hdcMem = CreateCompatibleDC(hdc);
                HGDIOBJ hbmOld = SelectObject(hdcMem, pEntry->hbmImage);
                BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

                SetStretchBltMode(hdc, HALFTONE);
                GdiAlphaBlend(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, hdcMem, 0, 0,
                              pEntry->sizeImage.cx, pEntry->sizeImage.cy, bf);
                SelectObject(hdcMem, hbmOld);
                DeleteDC(hdcMem);
            }
            else if (pEntry->hiconImage)
            {
                DrawIconEx(hdc, rc.left, rc.top, pEntry->hiconImage, rc.right - rc.left, rc.bottom - rc.top, 0,
                           NULL, DI_NORMAL);
            }
            else if (piml && pEntry->iSysImage >= 0)
            {
                HICON hicon;

                if (SUCCEEDED(piml->GetIcon(pEntry->iSysImage, ILD_TRANSPARENT, &hicon)))
                {
                    DrawIconEx(hdc, rc.left, rc.top, hicon, rc.right - rc.left, rc.bottom - rc.top, 0, NULL,
                               DI_NORMAL);
                    DestroyIcon(hicon);
                }
            }
        }

        xOffset += dxStep;
        yOffset += dyStep;
    }

    if (piml)
        piml->Release();
    return S_OK;
}

STDMETHODIMP
CItemThumbnail::GetMaxImageCount(
    UINT *pcMax)
{
    *pcMax = _cMaxItems;
    return S_OK;
}

/* Drops the extracted images; the items show their system icons until the next fetch */
STDMETHODIMP
CItemThumbnail::Reset()
{
    _uGeneration++;
    if (_hdsaItems)
    {
        for (int i = 0; i < DSA_GetItemCount(_hdsaItems); i++)
            ItemThumbnail_FreeImage((ITEMTHUMBNAILENTRY *)DSA_GetItemPtr(_hdsaItems, i));
    }
    return S_OK;
}

/* Shows pcm as a popup at pt, or with fDefaultOnly runs its default command */
static
HRESULT
DetailsPane_RunContextMenu(
    _In_ IContextMenu *pcm,
    _In_opt_ HWND hwnd,
    _In_ POINT pt,
    _In_ UINT uFlags,
    _In_ BOOL fDefaultOnly)
{
    CMINVOKECOMMANDINFOEX ici = { sizeof(ici) };
    HMENU hmenu = CreatePopupMenu();
    UINT idCmd;
    HRESULT hr;

    if (!hmenu)
        return E_OUTOFMEMORY;

    hr = pcm->QueryContextMenu(hmenu, 0, 1, 0x7FFF, fDefaultOnly ? CMF_DEFAULTONLY : uFlags);
    if (SUCCEEDED(hr))
    {
        if (fDefaultOnly)
            idCmd = GetMenuDefaultItem(hmenu, FALSE, 0);
        else
            idCmd = TrackPopupMenuEx(hmenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd, NULL);

        hr = S_FALSE;
        if (idCmd && idCmd != (UINT)-1)
        {
            ici.fMask = CMIC_MASK_PTINVOKE | CMIC_MASK_UNICODE;
            ici.hwnd = hwnd;
            ici.lpVerb = MAKEINTRESOURCEA(idCmd - 1);
            ici.lpVerbW = MAKEINTRESOURCEW(idCmd - 1);
            ici.nShow = SW_SHOWNORMAL;
            ici.ptInvoke = pt;
            hr = pcm->InvokeCommand((CMINVOKECOMMANDINFO *)&ici);
        }
    }
    DestroyMenu(hmenu);
    return hr;
}

/* CThumbnailControlCallback: tells the host element a new image arrived */

class CThumbnailControlCallback : public IItemThumbnailCallback, public CDUIFrameZeroAlloc
{
public:
    CThumbnailControlCallback(_In_ CThumbnailControlHost *pHost) : _cRef(1), _pHost(pHost) {}

    STDMETHODIMP
    QueryInterface(
        REFIID riid,
        void **ppv) override
    {
        static const QITAB qit[] =
        {
            { &__uuidof(IItemThumbnailCallback), OFFSETOFCLASS(IItemThumbnailCallback, CThumbnailControlCallback) },
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
    OnNewImageAvailable() override
    {
        if (_pHost)
            _pHost->Update();
        return S_OK;
    }

    void Detach() { _pHost = NULL; }

private:
    LONG _cRef;
    CThumbnailControlHost *_pHost;
};

/* CThumbnailControlHost */

DirectUI::IClassInfo *CThumbnailControlHost::Class = NULL;

HRESULT
WINAPI
CThumbnailControlHost::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    CThumbnailControlHost *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<CThumbnailControlHost>();
    if (!pe)
        return E_OUTOFMEMORY;

    hr = pe->Initialize(0, peParent, pdwDeferCookie);
    if (FAILED(hr))
        pe->Destroy(false);
    else
        *ppElement = pe;
    return hr;
}

HRESULT
WINAPI
CThumbnailControlHost::Register()
{
    return DirectUI::ClassInfo<CThumbnailControlHost, DirectUI::Element,
                               DirectUI::StandardCreator<CThumbnailControlHost>>::Register(
        DUIFRAME_HINST, L"ThumbnailControlHost", NULL, 0, false);
}

void
CThumbnailControlHost::Paint(
    HDC hdc,
    const RECT *prcBounds,
    const RECT *prcInvalid,
    RECT *prcSkipBorder,
    RECT *prcSkipContent)
{
    RECT rcContent;

    UNREFERENCED_PARAMETER(prcSkipContent);
    DirectUI::Element::Paint(hdc, prcBounds, prcInvalid, prcSkipBorder, &rcContent);
    if (_pit)
        _pit->Draw(hdc, &rcContent, GetDirection() == 1);
}

/* Extent follows the size the layout gives the element */
void
CThumbnailControlHost::OnPropertyChanged(
    const DirectUI::PropertyInfo *ppi,
    int iIndex,
    DirectUI::Value *pvOld,
    DirectUI::Value *pvNew)
{
    if (ppi == DirectUI::Element::ExtentProp() && (int)(ppi->Flags & 3) == iIndex)
        InvalidateGadget(GetDisplayNode());
    DirectUI::Element::OnPropertyChanged(ppi, iIndex, pvOld, pvNew);
}

void
CThumbnailControlHost::OnDestroy()
{
    if (_pCallback)
    {
        _pCallback->Detach();
        _pCallback->Release();
        _pCallback = NULL;
    }
    IUnknown_SafeReleaseAndNullPtr(&_pit);
    DirectUI::Element::OnDestroy();
}

HRESULT
CThumbnailControlHost::EnsureItemThumbnail()
{
    if (_pit)
        return S_OK;

    return CItemThumbnail::CreateInstance(ITEMTHUMB_MAXSIZE, ITEMTHUMB_STEPX, ITEMTHUMB_STEPY, ITEMTHUMB_MAXITEMS,
                                          TRUE, IID_PPV_ARGS(&_pit));
}

/* S_FALSE while the host is resizing itself, so the resize does not start another fetch */
HRESULT
CThumbnailControlHost::SetMaxSize(
    _In_ SIZE sizeMax,
    _COM_Outptr_result_maybenull_ IFrameWorkItem **ppfwi)
{
    HRESULT hr;

    *ppfwi = NULL;
    if (_fUpdating)
        return S_FALSE;

    hr = EnsureItemThumbnail();
    if (FAILED(hr))
        return hr;

    if (!_pCallback)
    {
        _pCallback = new CThumbnailControlCallback(this);
        if (!_pCallback)
            return E_OUTOFMEMORY;
    }

    hr = _pit->GetPrefetchTask(sizeMax.cx, sizeMax.cy, _pCallback, ppfwi);
    if (SUCCEEDED(hr))
        _sizeMax = sizeMax;
    return hr;
}

/* Sizes the element to the images inside the last maximum size and repaints */
HRESULT
CThumbnailControlHost::Update()
{
    RECT rcMax = { 0, 0, _sizeMax.cx, _sizeMax.cy };
    RECT rcContent;
    ULONG dwDeferCookie = 0;
    HRESULT hr;

    if (!_pit)
        return E_FAIL;

    hr = _pit->GetContentBounds(&rcMax, &rcContent);
    if (FAILED(hr))
        return hr;

    _fUpdating = TRUE;
    StartDefer(&dwDeferCookie);
    SetWidth(rcContent.right - rcContent.left);
    SetHeight(rcContent.bottom - rcContent.top);
    if (dwDeferCookie)
    {
        EndDefer(dwDeferCookie);
        dwDeferCookie = 0;
    }
    InvalidateGadget(GetDisplayNode());
    _fUpdating = FALSE;
    return hr;
}

/* CPreviewThumbnailDevirtualizeSelectionWorkItem: walks the selection off the frame thread */

class CPreviewThumbnailDevirtualizeSelectionWorkItem : public CFrameWorkItem
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_ CPreviewThumbnailInner *pInner,
        _In_ IShellItemArray *psia,
        _In_ BOOL fFirstItemOnly,
        _In_ BOOL fForce,
        _In_ BOOL fRefetch,
        _In_ UINT cMaxItems,
        _COM_Outptr_ IFrameWorkItem **ppfwi);

    STDMETHODIMP DoWork() override;
    STDMETHODIMP Dispatch() override;

    /* The inner element goes away before the item: it must not be called back */
    void ClearThreadState() { _pInner = NULL; }

protected:
    virtual ~CPreviewThumbnailDevirtualizeSelectionWorkItem();

private:
    CPreviewThumbnailInner *_pInner;
    IShellItemArray *_psiaSelection;
    IShellItemArray *_psiaResult;
    BOOL _fFirstItemOnly;
    BOOL _fForce;
    BOOL _fRefetch;
    UINT _cMaxItems;
};

HRESULT
WINAPI
CPreviewThumbnailDevirtualizeSelectionWorkItem::CreateInstance(
    _In_ CPreviewThumbnailInner *pInner,
    _In_ IShellItemArray *psia,
    _In_ BOOL fFirstItemOnly,
    _In_ BOOL fForce,
    _In_ BOOL fRefetch,
    _In_ UINT cMaxItems,
    _COM_Outptr_ IFrameWorkItem **ppfwi)
{
    CPreviewThumbnailDevirtualizeSelectionWorkItem *pwi;
    HRESULT hr;

    *ppfwi = NULL;
    pwi = new CPreviewThumbnailDevirtualizeSelectionWorkItem();
    if (!pwi)
        return E_OUTOFMEMORY;

    pwi->_pInner = pInner;
    pwi->_fFirstItemOnly = fFirstItemOnly;
    pwi->_fForce = fForce;
    pwi->_fRefetch = fRefetch;
    pwi->_cMaxItems = cMaxItems ? cMaxItems : 1;
    /* Shell item arrays are not tied to a thread, and ReactOS may lack their proxies */
    pwi->_psiaSelection = psia;
    psia->AddRef();
    hr = pwi->QueryInterface(IID_PPV_ARGS(ppfwi));
    pwi->Release();
    return hr;
}

CPreviewThumbnailDevirtualizeSelectionWorkItem::~CPreviewThumbnailDevirtualizeSelectionWorkItem()
{
    IUnknown_SafeReleaseAndNullPtr(&_psiaSelection);
    IUnknown_SafeReleaseAndNullPtr(&_psiaResult);
}

/* Only the first items are looked at: the thumbnail never shows more */
STDMETHODIMP
CPreviewThumbnailDevirtualizeSelectionWorkItem::DoWork()
{
    IShellItemArray *psia = _psiaSelection;
    PIDLIST_ABSOLUTE rgpidl[ITEMTHUMB_MAXITEMS] = { 0 };
    DWORD cItems = 0;
    UINT cTake;
    HRESULT hr;

    hr = psia->GetCount(&cItems);
    cTake = _fFirstItemOnly ? 1 : min(min((UINT)cItems, _cMaxItems), (UINT)ITEMTHUMB_MAXITEMS);
    for (UINT i = 0; i < cTake && SUCCEEDED(hr) && _ShouldContinue(); i++)
    {
        IShellItem *psi;

        hr = psia->GetItemAt(i, &psi);
        if (SUCCEEDED(hr))
        {
            hr = SHGetIDListFromObject(psi, &rgpidl[i]);
            psi->Release();
        }
    }

    if (SUCCEEDED(hr) && cTake)
        hr = SHCreateShellItemArrayFromIDLists(cTake, (PCIDLIST_ABSOLUTE_ARRAY)rgpidl, &_psiaResult);

    for (UINT i = 0; i < _countof(rgpidl); i++)
        ILFree(rgpidl[i]);
    return hr;
}

STDMETHODIMP
CPreviewThumbnailDevirtualizeSelectionWorkItem::Dispatch()
{
    if (!_pInner || !_psiaResult)
        return E_FAIL;

    return _pInner->SetAndFetch(_psiaResult, FALSE, _fForce, _fRefetch);
}

/* CPreviewThumbnailInner */

DirectUI::IClassInfo *CPreviewThumbnailInner::Class = NULL;

HRESULT
WINAPI
CPreviewThumbnailInner::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    CPreviewThumbnailInner *pe;
    DirectUI::Element *peMarkup;
    WCHAR szName[64];
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<CPreviewThumbnailInner>();
    if (!pe)
        return E_OUTOFMEMORY;

    pe->_cxMaxThumb = -1;
    hr = pe->Initialize(2, peParent, pdwDeferCookie);
    if (SUCCEEDED(hr))
        hr = pe->SetActive(1);
    if (SUCCEEDED(hr))
        hr = DUI_CreateElementFromResource(DUIFRAME_HINST, 7, L"PreviewThumbnail", pe, NULL, NULL, &peMarkup, NULL);
    if (FAILED(hr))
    {
        pe->Destroy(false);
        return hr;
    }

    LoadStringW(DUIFRAME_HINST, IDS_DUI_THUMBNAILMODULE, szName, _countof(szName));
    pe->SetAccessible(true);
    pe->SetAccName(szName);
    pe->SetAccRole(ROLE_SYSTEM_GRAPHIC);
    *ppElement = pe;
    return S_OK;
}

HRESULT
WINAPI
CPreviewThumbnailInner::Register()
{
    return DirectUI::ClassInfo<CPreviewThumbnailInner, CElementWithSite,
                               DirectUI::StandardCreator<CPreviewThumbnailInner>>::Register(
        DUIFRAME_HINST, L"PreviewThumbnailInner", NULL, 0, false);
}

STDMETHODIMP
CPreviewThumbnailInner::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CPreviewThumbnailInner, IDispatch),
        QITABENT(CPreviewThumbnailInner, IFrameNotificationClient),
        QITABENT(CPreviewThumbnailInner, IFrameShellViewClient),
        {0},
    };
    HRESULT hr = QISearch(this, qit, riid, ppv);

    if (FAILED(hr))
        hr = CElementWithSite::QueryInterface(riid, ppv);
    return hr;
}

STDMETHODIMP
CPreviewThumbnailInner::SetSite(
    IUnknown *punkSite)
{
    CElementWithSite::SetSite(punkSite);
    _ConnectToView(punkSite != NULL);
    return S_OK;
}

void
CPreviewThumbnailInner::OnDestroy()
{
    _CancelPendingWorkItem();
    _ConnectToView(FALSE);
    CElementWithSite::OnDestroy();
}

/* The view's events carry image updates; the frame relays everything else */
HRESULT
CPreviewThumbnailInner::_ConnectToView(
    _In_ BOOL fConnect)
{
    IShellView *psv;
    IDispatch *pdisp;
    HRESULT hr;

    if (!fConnect)
    {
        if (_pdispView && _dwViewCookie)
        {
            ConnectToConnectionPoint(static_cast<IDispatch *>(this), DIID_DShellFolderViewEvents, FALSE,
                                     _pdispView, &_dwViewCookie, NULL);
        }
        _dwViewCookie = 0;
        IUnknown_SafeReleaseAndNullPtr(&_pdispView);
        return S_OK;
    }

    hr = IUnknown_QueryService(_punkSite, IID_IFolderView, IID_PPV_ARGS(&psv));
    if (FAILED(hr))
        return hr;

    hr = psv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&pdisp));
    if (SUCCEEDED(hr))
    {
        hr = ConnectToConnectionPoint(static_cast<IDispatch *>(this), DIID_DShellFolderViewEvents, TRUE, pdisp,
                                      &_dwViewCookie, NULL);
        if (SUCCEEDED(hr))
            IUnknown_Set((IUnknown **)&_pdispView, pdisp);
        pdisp->Release();
    }
    psv->Release();
    return hr;
}

STDMETHODIMP
CPreviewThumbnailInner::GetTypeInfoCount(
    UINT *pctinfo)
{
    *pctinfo = 0;
    return E_NOTIMPL;
}

STDMETHODIMP
CPreviewThumbnailInner::GetTypeInfo(
    UINT iTInfo,
    LCID lcid,
    ITypeInfo **ppTInfo)
{
    *ppTInfo = NULL;
    return E_NOTIMPL;
}

STDMETHODIMP
CPreviewThumbnailInner::GetIDsOfNames(
    REFIID riid,
    LPOLESTR *rgszNames,
    UINT cNames,
    LCID lcid,
    DISPID *rgDispId)
{
    return E_NOTIMPL;
}

STDMETHODIMP
CPreviewThumbnailInner::Invoke(
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

    if (dispIdMember != DISPID_UPDATEIMAGE)
        return DISP_E_MEMBERNOTFOUND;
    return _UpdateItems(TRUE, FALSE);
}

/* Settings come from the outer module element the markup created */
STDMETHODIMP
CPreviewThumbnailInner::LayoutInitialized()
{
    DirectUI::Element *peModule = GetParent();
    DirectUI::Value *pv;

    if (!peModule)
        return S_OK;

    pv = peModule->GetValue(CPreviewThumbnail::VerticalProp, 2, NULL);
    _fVertical = pv->GetBool();
    pv->Release();
    SetLayoutPos(_fVertical ? ROSUI_LP_CLIENT : ROSUI_LP_LEFT);

    pv = peModule->GetValue(CPreviewThumbnail::AlwaysGetIconFromViewProp, 2, NULL);
    _fAlwaysGetIconFromView = pv->GetBool();
    pv->Release();

    pv = peModule->GetValue(CPreviewThumbnail::ForceUseIconProp, 2, NULL);
    _fForceUseIcon = pv->GetBool();
    pv->Release();

    pv = peModule->GetValue(CPreviewThumbnail::MaxThumbSizeProp, 2, NULL);
    _cxMaxThumb = pv->GetInt();
    pv->Release();

    pv = peModule->GetValue(CPreviewThumbnail::ThumbnailIdProp, 2, NULL);
    if (!pv->GetString() || !GUIDFromStringW(pv->GetString(), &_guidFetchTask))
        _guidFetchTask = _fVertical ? TOID_VerticalPanePrefetchTask : TOID_HorizontalPanePrefetchTask;
    pv->Release();

    _UpdateItems(FALSE, TRUE);
    return S_OK;
}

/* A pane that was hidden when the selection changed catches up when it shows again */
STDMETHODIMP
CPreviewThumbnailInner::Notify(
    PCWSTR pszPropertyName)
{
    BOOL fVisible;

    if (_fVertical)
    {
        if (StrCmpICW(pszPropertyName, c_szReadingPaneVisible) || !_fUpdatePending)
            return S_OK;
        fVisible = DetailsPane_IsReadingPaneVisible(_punkSite);
    }
    else
    {
        if (StrCmpICW(pszPropertyName, c_szPreviewPaneVisible) || !_fUpdatePending)
            return S_OK;
        fVisible = DetailsPane_IsPreviewPaneVisible(_punkSite);
    }

    if (fVisible)
    {
        _UpdateItems(FALSE, TRUE);
        _fUpdatePending = FALSE;
    }
    return S_OK;
}

STDMETHODIMP
CPreviewThumbnailInner::OnSelectedItemChanged()
{
    CThumbnailControlHost *pHost;

    if (_fVertical && !DetailsPane_IsReadingPaneVisible(_punkSite) &&
        SUCCEEDED(_GetThumbnailControlHost(&pHost)) && pHost->GetItemThumbnail())
    {
        pHost->GetItemThumbnail()->Reset();
    }
    return _UpdateItems(TRUE, FALSE);
}

STDMETHODIMP
CPreviewThumbnailInner::OnContentsChanged()
{
    if (_HasSelection())
        return S_OK;
    return _UpdateItems(FALSE, FALSE);
}

STDMETHODIMP
CPreviewThumbnailInner::OnFolderChanged()
{
    return _UpdateItems(FALSE, FALSE);
}

HRESULT
CPreviewThumbnailInner::_GetThumbnailControlHost(
    _Outptr_ CThumbnailControlHost **ppHost)
{
    DirectUI::Element *pe = FindDescendent(StrToID(L"Thumbnail"));

    *ppHost = (CThumbnailControlHost *)pe;
    return pe ? S_OK : E_FAIL;
}

BOOL
CPreviewThumbnailInner::_HasSelection()
{
    IFolderView *pfv;
    int cSelected = 0;
    BOOL fHasSelection = FALSE;

    if (SUCCEEDED(IUnknown_QueryService(_punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv))))
    {
        if (SUCCEEDED(pfv->ItemCount(SVGIO_SELECTION | SVGIO_FLAG_VIEWORDER, &cSelected)) && cSelected > 0)
            fHasSelection = TRUE;
        pfv->Release();
    }
    return fHasSelection;
}

void
CPreviewThumbnailInner::_CancelPendingWorkItem()
{
    if (!_pfwiDevirtualize)
        return;

    static_cast<CPreviewThumbnailDevirtualizeSelectionWorkItem *>(_pfwiDevirtualize)->ClearThreadState();
    _pfwiDevirtualize->SetDispatchable(FALSE);
    IUnknown_SafeReleaseAndNullPtr(&_pfwiDevirtualize);
}

/* The selection, or the folder itself when nothing is selected */
HRESULT
CPreviewThumbnailInner::_UpdateItems(
    _In_ BOOL fForce,
    _In_ BOOL fRefetch)
{
    IShellItemArray *psia = NULL;
    BOOL fQueued = FALSE;
    BOOL fFolderItem = FALSE;
    IFolderView *pfv;
    HRESULT hr;

    if (!_fAlwaysGetIconFromView &&
        !(_fVertical ? DetailsPane_IsReadingPaneVisible(_punkSite) : DetailsPane_IsPreviewPaneVisible(_punkSite)))
    {
        _fUpdatePending = TRUE;
        return S_OK;
    }

    _CancelPendingWorkItem();

    hr = IUnknown_QueryService(_punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv));
    if (SUCCEEDED(hr))
    {
        UINT uFlags = _fVertical ? SVGIO_SELECTION : (SVGIO_SELECTION | SVGIO_FLAG_VIEWORDER);
        int cSelected;

        if (SUCCEEDED(pfv->ItemCount(uFlags, &cSelected)) &&
            SUCCEEDED(pfv->Items(uFlags, IID_PPV_ARGS(&psia))))
        {
            CThumbnailControlHost *pHost;
            UINT cMaxItems = 1;

            if (SUCCEEDED(_GetThumbnailControlHost(&pHost)) && SUCCEEDED(pHost->EnsureItemThumbnail()))
                pHost->GetItemThumbnail()->GetMaxImageCount(&cMaxItems);

            hr = _QueueUpFetchWorkItem(psia, fForce, fRefetch, cMaxItems);
            if (SUCCEEDED(hr))
                fQueued = TRUE;
        }
        else
        {
            PIDLIST_ABSOLUTE pidlFolder;

            fFolderItem = TRUE;
            hr = SHGetIDListFromObject(pfv, &pidlFolder);
            if (SUCCEEDED(hr))
            {
                hr = SHCreateShellItemArrayFromIDLists(1, (PCIDLIST_ABSOLUTE_ARRAY)&pidlFolder, &psia);
                ILFree(pidlFolder);
            }
        }
        pfv->Release();
    }

    if (SUCCEEDED(hr) && !fQueued)
        hr = SetAndFetch(psia, fFolderItem, fForce, fRefetch);
    if (psia)
        psia->Release();
    return hr;
}

HRESULT
CPreviewThumbnailInner::_QueueUpFetchWorkItem(
    _In_ IShellItemArray *psia,
    _In_ BOOL fForce,
    _In_ BOOL fRefetch,
    _In_ UINT cMaxItems)
{
    IFrameTaskManager *pftm;
    IFrameWorkItem *pfwi;
    HRESULT hr;

    hr = DetailsPane_GetTaskManager(_punkSite, &pftm);
    if (FAILED(hr))
        return hr;

    hr = CPreviewThumbnailDevirtualizeSelectionWorkItem::CreateInstance(this, psia, _fVertical, fForce, fRefetch,
                                                                       cMaxItems, &pfwi);
    if (SUCCEEDED(hr))
    {
        IUnknown_Set((IUnknown **)&_pfwiDevirtualize, pfwi);
        hr = pftm->StartWorkItem(pfwi,
                                 _fVertical ? TOID_VerticalDevirtualizeSelectionTask
                                            : TOID_HorizontalDevirtualizeSelectionTask,
                                 FTMF_REPLACE);
        pfwi->Release();
    }
    pftm->Release();
    return hr;
}

/* The reading pane hides an item's icon until its preview image is ready, unless told otherwise */
HRESULT
CPreviewThumbnailInner::SetAndFetch(
    _In_ IShellItemArray *psia,
    _In_ BOOL fFolderItem,
    _In_ BOOL fForce,
    _In_ BOOL fRefetch)
{
    CThumbnailControlHost *pHost;
    DWORD dwFlags = 0;
    HRESULT hr;

    hr = _GetThumbnailControlHost(&pHost);
    if (FAILED(hr))
        return hr;

    if (_fVertical && !_fForceUseIcon)
        dwFlags = ITSIF_HIDEPENDINGICON;
    if (_fVertical)
        dwFlags |= ITSIF_SINGLECOLUMN;
    if (fFolderItem)
        dwFlags |= ITSIF_ICONSONLY;
    if (fForce)
        dwFlags |= ITSIF_FORCEREFRESH;

    hr = pHost->EnsureItemThumbnail();
    if (SUCCEEDED(hr))
        hr = pHost->GetItemThumbnail()->SetItems(psia, dwFlags);
    if (fRefetch && hr == S_FALSE)
        hr = S_OK;
    if (hr == S_OK)
    {
        hr = _FetchThumbnail(pHost, _sizeFetch);

        /* Items can arrive before the first layout; that layout fetches them */
        _fFetchPending = (hr == S_FALSE);
    }
    return hr;
}

/* The maximum thumbnail size bounds the side the pane does not stretch */
HRESULT
CPreviewThumbnailInner::_FetchThumbnail(
    _In_opt_ CThumbnailControlHost *pHost,
    _In_ SIZE size)
{
    IFrameTaskManager *pftm;
    IFrameWorkItem *pfwi = NULL;
    HRESULT hr;

    if (size.cx <= 0 || size.cy <= 0)
        return S_FALSE;

    if (!pHost)
    {
        hr = _GetThumbnailControlHost(&pHost);
        if (FAILED(hr))
            return hr;
    }

    hr = DetailsPane_GetTaskManager(_punkSite, &pftm);
    if (FAILED(hr))
        return hr;

    if (_cxMaxThumb != -1)
    {
        if (_fVertical)
            size.cy = _cxMaxThumb;
        else
            size.cx = _cxMaxThumb;
    }

    hr = pHost->SetMaxSize(size, &pfwi);
    if (hr == S_OK)
        hr = pftm->StartWorkItem(pfwi, _guidFetchTask, FTMF_REPLACE);
    if (pfwi)
        pfwi->Release();
    pftm->Release();
    return hr;
}

/* The details pane refetches when its height changes, the reading pane when its width does */
void
CPreviewThumbnailInner::OnPropertyChanged(
    const DirectUI::PropertyInfo *ppi,
    int iIndex,
    DirectUI::Value *pvOld,
    DirectUI::Value *pvNew)
{
    if (ppi == DirectUI::Element::ExtentProp() && (int)(ppi->Flags & 3) == iIndex &&
        pvNew->GetType() == (int)DirectUI::ValueType::Size)
    {
        SIZE sizeOld = { 0, 0 };
        SIZE sizeNew = *pvNew->GetSize();
        BOOL fFetch;

        if (pvOld && pvOld->GetType() == (int)DirectUI::ValueType::Size)
            sizeOld = *pvOld->GetSize();

        if (_fVertical)
            fFetch = sizeNew.cx != sizeOld.cx || !sizeOld.cy;
        else
            fFetch = sizeNew.cy != sizeOld.cy || !sizeOld.cx;

        if (fFetch)
            _FetchThumbnail(NULL, sizeNew);
    }
    CElementWithSite::OnPropertyChanged(ppi, iIndex, pvOld, pvNew);
}

SIZE
CPreviewThumbnailInner::_SelfLayoutUpdateDesiredSize(
    int cxConstraint,
    int cyConstraint,
    DirectUI::Surface *psrf)
{
    SIZE size = { cxConstraint, cyConstraint };
    CThumbnailControlHost *pHost;

    if (cxConstraint <= 0 || cyConstraint <= 0)
        return size;

    if (_cxMaxThumb != -1)
    {
        if (_fVertical)
            cyConstraint = min(cyConstraint, _cxMaxThumb);
        else
            cxConstraint = min(cxConstraint, _cxMaxThumb);
    }

    if (SUCCEEDED(_GetThumbnailControlHost(&pHost)))
    {
        size = pHost->_UpdateDesiredSize(INT_MAX, INT_MAX, psrf);
        if (size.cx > 0 && size.cy > 0)
        {
            SIZE sizeFit = { cxConstraint, cyConstraint };
            RECT rc = { 0, 0, size.cx, size.cy };

            DetailsPane_FitToSize(&sizeFit, &rc);
            size.cx = rc.right - rc.left;
            size.cy = rc.bottom - rc.top;
        }
    }

    _sizeFetch.cx = cxConstraint;
    _sizeFetch.cy = cyConstraint;
    if (_fFetchPending)
    {
        _fFetchPending = FALSE;
        _FetchThumbnail(NULL, _sizeFetch);
    }
    return size;
}

void
CPreviewThumbnailInner::_SelfLayoutDoLayout(
    int cx,
    int cy)
{
    CThumbnailControlHost *pHost;
    const SIZE *psizeDesired;

    if (cx <= 0 || cy <= 0 || FAILED(_GetThumbnailControlHost(&pHost)))
        return;

    psizeDesired = pHost->GetDesiredSize();
    if (psizeDesired->cx > 0 && psizeDesired->cy > 0)
    {
        SIZE sizeFit = { cx, cy };
        RECT rc = { 0, 0, psizeDesired->cx, psizeDesired->cy };

        DetailsPane_FitToSize(&sizeFit, &rc);
        pHost->_UpdateLayoutPosition(0, 0);
        pHost->_UpdateLayoutSize(rc.right - rc.left, rc.bottom - rc.top);
    }
}

/* Right click opens the selection's menu, or the folder's; a double click opens the selection */
void
CPreviewThumbnailInner::OnInput(
    DirectUI::InputEvent *pie)
{
    if (pie->nStage == GMF_DIRECT && pie->nDevice == GINPUT_MOUSE)
    {
        DirectUI::MouseEvent *pme = (DirectUI::MouseEvent *)pie;

        if (pie->nCode == GMOUSE_UP && pme->bButton == GBUTTON_RIGHT)
        {
            pie->fHandled = SUCCEEDED(_LaunchContextMenu(pme));
        }
        else if (pie->nCode == GMOUSE_DOWN && pme->bButton == GBUTTON_LEFT &&
                 ((DirectUI::MouseClickEvent *)pme)->cClicks == 1)
        {
            HRESULT hr = E_FAIL;

            if (_HasSelection())
            {
                IFolderView2 *pfv2;
                IShellView *psv;

                hr = IUnknown_QueryService(_punkSite, IID_IFolderView, IID_PPV_ARGS(&pfv2));
                if (SUCCEEDED(hr))
                {
                    hr = pfv2->InvokeVerbOnSelection(NULL);
                    pfv2->Release();
                }
                else if (SUCCEEDED(IUnknown_QueryService(_punkSite, IID_IFolderView, IID_PPV_ARGS(&psv))))
                {
                    IContextMenu *pcm;
                    POINT pt = { 0, 0 };
                    HWND hwnd = NULL;

                    /* Views without IFolderView2 still open the selection through its default verb */
                    hr = psv->GetItemObject(SVGIO_SELECTION, IID_PPV_ARGS(&pcm));
                    if (SUCCEEDED(hr))
                    {
                        DUI_GetElementRootHWND(this, &hwnd);
                        hr = DetailsPane_RunContextMenu(pcm, hwnd, pt, CMF_DEFAULTONLY, TRUE);
                        pcm->Release();
                    }
                    psv->Release();
                }
            }
            pie->fHandled = SUCCEEDED(hr);
        }
    }
    CElementWithSite::OnInput(pie);
}

HRESULT
CPreviewThumbnailInner::_LaunchContextMenu(
    _In_ DirectUI::MouseEvent *pme)
{
    IThumbContextMenuSite *pcms;
    IContextMenu *pcm;
    IShellView *psv;
    BOOL fSelection;
    POINT pt;
    HRESULT hr;

    hr = DUI_GetMouseEventAbsolutePos(pme->peTarget, &pme->ptClientPxl, &pt);
    if (FAILED(hr))
        return hr;

    hr = IUnknown_QueryService(_punkSite, IID_IFolderView, IID_PPV_ARGS(&psv));
    if (FAILED(hr))
        return hr;

    fSelection = _HasSelection();
    hr = psv->GetItemObject(fSelection ? SVGIO_SELECTION : SVGIO_BACKGROUND, IID_PPV_ARGS(&pcm));
    if (SUCCEEDED(hr))
    {
        UINT uFlags = fSelection ? (CMF_ITEMMENU | CMF_CANRENAME) : CMF_NODEFAULT;

        if (SUCCEEDED(IUnknown_QueryService(_punkSite, IID_IFolderView, __uuidof(IThumbContextMenuSite),
                                            (void **)&pcms)))
        {
            hr = pcms->DoContextMenuPopup(pcm, uFlags, pt);
            pcms->Release();
        }
        else
        {
            HWND hwnd = NULL;

            /* No browser menu site: the pane tracks the menu itself */
            DUI_GetElementRootHWND(this, &hwnd);
            hr = DetailsPane_RunContextMenu(pcm, hwnd, pt, uFlags, FALSE);
        }
        pcm->Release();
    }
    psv->Release();
    return hr;
}
