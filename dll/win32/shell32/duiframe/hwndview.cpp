/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Root HWND element of the view frame, the view host and the hosted view window
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

/* KeyboardNavigate direction bits */
#define NAVDIR_RELATIVE             0x1
#define NAVDIR_FORWARD              0x2
#define NAVDIR_LOGICAL              0x8

/* HWNDElement::Initialize creation flags of the frame root */
#define HWNDVIEW_CREATEFLAGS        4

/* HWNDHost::Initialize flags of the view host */
#define DUIVIEW_HOSTFLAGS           0x13
#define DUIVIEW_CREATEFLAGS         3

DirectUI::IClassInfo *CHWNDView::Class = NULL;
DirectUI::IClassInfo *CDUIViewHost::Class = NULL;
DirectUI::IClassInfo *CDUIView::Class = NULL;

/* CHWNDView */

HRESULT
WINAPI
CHWNDView::Create(
    _In_ HWND hwndParent,
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    DirectUI::Layout *pLayout;
    WCHAR szName[64];
    CHWNDView *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<CHWNDView>();
    if (!pe)
        return E_OUTOFMEMORY;

    hr = pe->Initialize(hwndParent, true, HWNDVIEW_CREATEFLAGS, NULL, pdwDeferCookie);
    if (FAILED(hr))
    {
        pe->Destroy(true);
        return hr;
    }

    LoadStringW(DUIFRAME_HINST, IDS_DUI_EXPLORERPANE, szName, _countof(szName));
    pe->SetWrapKeyboardNavigate(false);
    pe->SetAccessible(true);
    pe->SetAccRole(ROLE_SYSTEM_PANE);
    pe->SetAccName(szName);

    hr = DirectUI::BorderLayout::Create(&pLayout);
    if (SUCCEEDED(hr))
        pe->SetLayout(pLayout);

    *ppElement = pe;

    /* Tab moves between the frame's child windows */
    HWND hwnd = pe->GetHWND();
    SetWindowLongW(hwnd, GWL_EXSTYLE, GetWindowLongW(hwnd, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);
    return hr;
}

HRESULT
WINAPI
CHWNDView::Register()
{
    return DirectUI::ClassInfo<CHWNDView, DirectUI::HWNDElement,
                               DirectUI::StandardCreator<CHWNDView>>::Register(
        DUIFRAME_HINST, L"HWNDView", NULL, 0, false);
}

void
CHWNDView::ActivateTooltip(
    DirectUI::Element *pe,
    unsigned long dwFlags)
{
    ULONG_PTR ulCookie = 0;

    SHActivateContext(&ulCookie);
    DirectUI::HWNDElement::ActivateTooltip(pe, dwFlags);
    if (ulCookie)
        SHDeactivateContext(ulCookie);
}

/* Push buttons, links and expandos do their default action on Enter; check boxes and radios do not */
BOOL
CHWNDView::_ShouldDoDefaultActionOnEnter(
    _In_ DirectUI::Element *pe)
{
    DirectUI::IClassInfo *pci = pe->GetClassInfoW();
    BOOL fButtonLike;

    fButtonLike = pci->IsSubclassOf(DirectUI::CCPushButton::GetClassInfoPtr()) ||
                  pci->IsSubclassOf(DirectUI::CCSysLink::GetClassInfoPtr()) ||
                  pci == DirectUI::Button::GetClassInfoPtr() ||
                  pci->IsSubclassOf(DirectUI::ExpandoButtonGlyph::GetClassInfoPtr());

    return fButtonLike && !pci->IsSubclassOf(DirectUI::CCBaseCheckRadioButton::GetClassInfoPtr());
}

void
CHWNDView::OnInput(
    DirectUI::InputEvent *pie)
{
    if ((pie->nStage == GMF_DIRECT || pie->nStage == GMF_BUBBLED) &&
        pie->nDevice == GINPUT_KEYBOARD && pie->nCode == GKEY_DOWN &&
        static_cast<DirectUI::KeyboardEvent *>(pie)->ch == VK_RETURN)
    {
        if (pie->peTarget && _ShouldDoDefaultActionOnEnter(pie->peTarget))
            pie->peTarget->DefaultAction();
        pie->fHandled = true;
    }

    if (!pie->fHandled)
        DirectUI::HWNDElement::OnInput(pie);
}

void
CHWNDView::OnEvent(
    DirectUI::Event *pev)
{
    if (pev->nStage == GMF_DIRECT && pev->uidType.m_Value == DirectUI::HWNDElement::ThemeChange().m_Value)
    {
        DirectUI::Event evTheme = {};

        DUI_ReloadTheme(this);
        evTheme.uidType = DirectUI::HWNDElement::ThemeChange();
        BroadcastEvent(&evTheme);
    }

    DirectUI::HWNDElement::OnEvent(pev);

    /* A logical navigation nobody took has run off the end of the tree */
    if (((!pev->fHandled && pev->nStage == GMF_BUBBLED) || pev->nStage == GMF_DIRECT) &&
        pev->uidType.m_Value == DirectUI::Element::KeyboardNavigate().m_Value)
    {
        int iNavDir = static_cast<DirectUI::KeyboardNavigateEvent *>(pev)->iNavDir;

        if ((iNavDir & NAVDIR_RELATIVE) && (iNavDir & NAVDIR_LOGICAL))
            _fNavigated = TRUE;
    }
}

LRESULT
CHWNDView::WndProc(
    HWND hwnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    if (uMsg == WM_NCACTIVATE)
        return TRUE;
    return DirectUI::HWNDElement::WndProc(hwnd, uMsg, wParam, lParam);
}

/* The hosted control or view element that holds the window with focus */
DirectUI::Element *
CHWNDView::_FindAppropriateElement(
    _In_ DirectUI::Element *peParent,
    _In_ HWND hwndFocus)
{
    DirectUI::DynamicArray<DirectUI::Element *, 0> *pdaChildren;
    DirectUI::Value *pvChildren;
    DirectUI::Element *peFound = NULL;
    UINT i;

    pdaChildren = peParent->GetChildren(&pvChildren);
    if (!pdaChildren)
        return NULL;

    for (i = 0; i < pdaChildren->GetSize() && !peFound; i++)
    {
        DirectUI::Element *peChild = pdaChildren->GetItem(i);
        DirectUI::IClassInfo *pci = peChild->GetClassInfoW();

        /* An XElement hosting the focus window wins, then a view that holds it, then deeper levels */
        if (pci->IsSubclassOf(DirectUI::XElement::GetClassInfoPtr()) &&
            SHIsChildOrSelf(static_cast<DirectUI::XElement *>(peChild)->GetHWND(), hwndFocus) == S_OK)
        {
            peFound = peChild;
        }
        else if (pci->IsSubclassOf(CDUIView::Class) &&
                 SHIsChildOrSelf(static_cast<CDUIView *>(peChild)->GetHWND(), hwndFocus) == S_OK)
        {
            peFound = peChild;
        }
        else
        {
            peFound = _FindAppropriateElement(peChild, hwndFocus);
        }
    }
    pvChildren->Release();
    return peFound;
}

static
VOID
WINAPI
FindInputObjectWithFocus(
    _In_ IUnknown *punk,
    _In_ LPARAM lParam)
{
    DirectUI::Element **ppeFocus = (DirectUI::Element **)lParam;
    DirectUI::Element *pe;

    if (*ppeFocus || IUnknown_HasFocusIO(punk) != S_OK)
        return;

    /* Modules hand focus to their first child when they cannot take it themselves */
    pe = static_cast<CElementWithIUnknown *>(punk);
    *ppeFocus = pe;
    if (!(pe->GetActive() & 2))
    {
        DirectUI::Value *pvChildren;
        DirectUI::DynamicArray<DirectUI::Element *, 0> *pdaChildren = pe->GetChildren(&pvChildren);

        if (pdaChildren)
        {
            if (pdaChildren->GetSize() && (pdaChildren->GetItem(0)->GetActive() & 2))
                *ppeFocus = pdaChildren->GetItem(0);
            pvChildren->Release();
        }
    }
}

/* Tab and Shift+Tab inside the frame; FALSE when focus should leave the frame */
BOOL
CHWNDView::Navigate(
    _In_ MSG *pmsg)
{
    HWND hwnd = GetHWND();

    if (hwnd == pmsg->hwnd || IsChild(hwnd, pmsg->hwnd))
    {
        DirectUI::KeyboardNavigateEvent ev = {};
        DirectUI::Element *peFrom;

        _fNavigated = FALSE;
        peFrom = _FindAppropriateElement(this, pmsg->hwnd);
        if (!peFrom)
            peFrom = DirectUI::HWNDElement::GetKeyFocusedElement();
        if (!peFrom)
            DUI_WalkIUnknownElements(this, FindInputObjectWithFocus, (LPARAM)&peFrom);

        ev.uidType = DirectUI::Element::KeyboardNavigate();
        ev.iNavDir = NAVDIR_RELATIVE | NAVDIR_LOGICAL | ((GetKeyState(VK_SHIFT) < 0) ? 0 : NAVDIR_FORWARD);
        ev.fSetFocus = true;
        (peFrom ? peFrom : this)->FireEvent(&ev, true, false);
    }
    return !_fNavigated;
}

/* CDUIViewHost */

static DirectUI::PropCapability s_pcColor = { DirectUI::ValueType::Color, 0, UINT_MAX };
static DirectUI::PropertyInfoData s_pidSortColumn;
static DirectUI::PropertyInfoData s_pidSubText;

static const DirectUI::PropertyInfo s_piSortColumn =
    { L"SortColumn", 6, 0x20000, &s_pcColor, NULL, DirectUI::Value::GetColorTrans, &s_pidSortColumn };
static const DirectUI::PropertyInfo s_piSubText =
    { L"SubText", 6, 0x20000, &s_pcColor, NULL, DirectUI::Value::GetColorTrans, &s_pidSubText };

static const DirectUI::PropertyInfo *const c_rgpiViewHost[] =
{
    &s_piSortColumn,
    &s_piSubText,
};

const DirectUI::PropertyInfo *const CDUIViewHost::SortColumnProp = &s_piSortColumn;
const DirectUI::PropertyInfo *const CDUIViewHost::SubTextProp = &s_piSubText;

HRESULT
WINAPI
CDUIViewHost::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    CDUIViewHost *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<CDUIViewHost>();
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
CDUIViewHost::Register()
{
    return DirectUI::ClassInfo<CDUIViewHost, DirectUI::Element,
                               DirectUI::StandardCreator<CDUIViewHost>>::Register(
        DUIFRAME_HINST, L"ViewHost", c_rgpiViewHost, _countof(c_rgpiViewHost), false);
}

/* CDUIView */

HRESULT
WINAPI
CDUIView::Create(
    _In_ HWND hwndView,
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    WCHAR szName[64];
    CDUIView *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<CDUIView>();
    if (!pe)
        return E_OUTOFMEMORY;

    /* The view window exists already; CreateHWND adopts it instead of creating one */
    pe->_hwndView = hwndView;
    hr = pe->DirectUI::HWNDHost::Initialize(DUIVIEW_HOSTFLAGS, DUIVIEW_CREATEFLAGS, peParent, pdwDeferCookie);
    if (FAILED(hr))
    {
        pe->Destroy(true);
        return hr;
    }

    LoadStringW(DUIFRAME_HINST, IDS_DUI_SHELLFOLDERVIEW, szName, _countof(szName));
    pe->SetAccessible(true);
    pe->SetAccName(szName);
    *ppElement = pe;
    return S_OK;
}

HRESULT
WINAPI
CDUIView::Register()
{
    return DirectUI::ClassInfo<CDUIView, DirectUI::HWNDHost,
                               DirectUI::StandardCreator<CDUIView>>::Register(
        DUIFRAME_HINST, L"DUIListView", NULL, 0, false);
}

HWND
CDUIView::CreateHWND(
    HWND hwndParent)
{
    DWORD dwExStyle;

    _hwndHost = hwndParent;
    _hwndOriginalParent = ::GetParent(_hwndView);
    SHSetParentHwnd(_hwndView, hwndParent);

    /* The frame draws the edge around the view, so the view's own goes for as long as it is hosted */
    dwExStyle = GetWindowLongW(_hwndView, GWL_EXSTYLE);
    _fHadClientEdge = (dwExStyle & WS_EX_CLIENTEDGE) != 0;
    if (_fHadClientEdge)
        SetWindowLongW(_hwndView, GWL_EXSTYLE, dwExStyle & ~WS_EX_CLIENTEDGE);

    ShowWindow(_hwndView, SW_SHOW);
    SetWindowLongW(hwndParent, GWL_EXSTYLE, GetWindowLongW(hwndParent, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);
    return _hwndView;
}

/* Hands the view window back to the parent it had before the frame adopted it */
void
CDUIView::DetachListview()
{
    if (!_hwndView)
        return;

    Detach();
    if (_fHadClientEdge)
        SHSetWindowBits(_hwndView, GWL_EXSTYLE, WS_EX_CLIENTEDGE, WS_EX_CLIENTEDGE);
    ShowWindow(_hwndView, SW_HIDE);
    SHSetParentHwnd(_hwndView, _hwndOriginalParent);
    _hwndView = NULL;
}

void
CDUIView::OnDestroy()
{
    DetachListview();
    DirectUI::HWNDHost::OnDestroy();
}

void
CDUIView::SetKeyFocus()
{
    /* Only take focus from DirectUI when it is not already inside the view */
    if (SHIsChildOrSelf(GetHWND(), GetFocus()) != S_OK)
        DirectUI::HWNDHost::SetKeyFocus();
}

bool
CDUIView::OnMessage(
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam,
    LRESULT *plRet)
{
    if (uMsg == WM_NOTIFY && ((NMHDR *)lParam)->code == NM_SETFOCUS)
        SetKeyFocus();
    return DirectUI::HWNDHost::OnMessage(uMsg, wParam, lParam, plRet);
}

void
CDUIView::OnInput(
    DirectUI::InputEvent *pie)
{
    /* Tab key traffic belongs to the frame's navigation, everything else to the hosted window */
    if (pie->nStage == GMF_DIRECT && pie->nDevice == GINPUT_KEYBOARD &&
        (pie->nCode == GKEY_DOWN || pie->nCode == GKEY_UP || pie->nCode == GKEY_CHAR) &&
        static_cast<DirectUI::KeyboardEvent *>(pie)->ch == VK_TAB)
    {
        DirectUI::Element::OnInput(pie);
    }
    else
    {
        DirectUI::HWNDHost::OnInput(pie);
    }
}
