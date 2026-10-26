/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Views button of the command bar and its view slider popup
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

DirectUI::IClassInfo *ViewControl::Class = NULL;

EXTERN_C const GUID PROPID_ACC_VALUEMAP;

/* Local value store of an element property */
#define PROPERTY_STORE_LOCAL        1

/* DShellFolderViewEvents ids the ReactOS shdispid.h lacks */
#ifndef DISPID_VIEWMODECHANGED
#define DISPID_VIEWMODECHANGED      205
#endif
#ifndef DISPID_CTRLMOUSEWHEEL
#define DISPID_CTRLMOUSEWHEEL       213
#endif
#ifndef DISPID_ICONSIZECHANGED
#define DISPID_ICONSIZECHANGED      215
#endif

/* Vista trackbar additions the ReactOS commctrl.h does not carry yet */
#ifndef TBS_NOTIFYBEFOREMOVE
#define TBS_NOTIFYBEFOREMOVE        0x0800
#endif
#ifndef TRBN_THUMBPOSCHANGING
#define TRBN_FIRST                  (0U - 1501U)
#define TRBN_THUMBPOSCHANGING       (TRBN_FIRST - 1)
typedef struct tagTRBTHUMBPOSCHANGING
{
    NMHDR hdr;
    DWORD dwPos;
    int nReason;
} NMTRBTHUMBPOSCHANGING;
#endif
#ifndef CCS_NOHILITE
#define CCS_NOHILITE                0x0010
#endif

/*
 * The slider runs from 0 at the top to SLIDER_RANGE at the bottom. Positions below are measured
 * the other way, from content at 0 up to extra large icons at SLIDER_RANGE; each fixed row sits
 * a set share of the range away from the top.
 */
#define SLIDER_RANGE                160
#define SLIDER_POS_ICONS            (SLIDER_RANGE - SLIDER_RANGE * 49 / 100)
#define SLIDER_POS_LIST             (SLIDER_RANGE - SLIDER_RANGE * 62 / 100)
#define SLIDER_POS_DETAILS          (SLIDER_RANGE - SLIDER_RANGE * 75 / 100)
#define SLIDER_POS_TILES            (SLIDER_RANGE - SLIDER_RANGE * 88 / 100)
#define SLIDER_POS_CONTENT          0

#define ICON_SIZE_MIN               16
#define ICON_SIZE_MAX               256

/* Slider move reasons with bit 0 or bit 2 set snap to the rows: line and page down, thumb moves, top, bottom */
#define SLIDER_SNAP_REASONS         5

static const WCHAR c_szViewSliderClass[] = L"ViewControlClass";

/* Rows of the slider popup, top to bottom */
enum VIEWSLIDERROW
{
    VSR_EXTRALARGE,
    VSR_LARGE,
    VSR_MEDIUM,
    VSR_SMALL,
    VSR_LIST,
    VSR_DETAILS,
    VSR_TILES,
    VSR_CONTENT,
    VSR_COUNT
};

struct VIEWSLIDERROWINFO
{
    FOLDERVIEWMODE fvm;
    int iIconSize;
    UINT idIcon;
    UINT idCommand;
};

static const VIEWSLIDERROWINFO c_rgRows[VSR_COUNT] =
{
    { FVM_THUMBNAIL, 256, IDI_VIEW_EXTRALARGE, 603 },
    { FVM_THUMBNAIL, 96, IDI_VIEW_LARGE, 604 },
    { FVM_THUMBNAIL, 48, IDI_VIEW_MEDIUM, 605 },
    { FVM_THUMBNAIL, 16, IDI_VIEW_SMALL, 606 },
    { FVM_LIST, 16, IDI_VIEW_LIST, 602 },
    { FVM_DETAILS, 16, IDI_VIEW_DETAILS, 600 },
    { FVM_TILE, 48, IDI_VIEW_TILES, 601 },
    { FVM_CONTENT, 32, IDI_VIEW_CONTENT, 607 },
};

/* Popup metrics in 96 DPI units, scaled once when the first popup opens */
static SIZE g_sizeButton = { 125, 30 };
static SIZE g_sizeSlider = { 27, 255 };
static SIZE g_sizeDropDown = { 158, 265 };
static int g_cxButtonPadding = 8;
static int g_xStart = 7;
static int g_ySliderTop = 5;
static int g_rgiSnapSpots[VSR_COUNT];
static BOOL g_fMetricsCalculated;
static INIT_ONCE g_ioMetrics = INIT_ONCE_STATIC_INIT;

static
BOOL
IsIconViewMode(
    _In_ FOLDERVIEWMODE fvm)
{
    return fvm == FVM_ICON || fvm == FVM_SMALLICON || fvm == FVM_THUMBNAIL || fvm == FVM_THUMBSTRIP;
}

/* All icon modes look alike; only their size tells them apart */
static
BOOL
IsSameViewLayout(
    _In_ FOLDERVIEWMODE fvm1,
    _In_ FOLDERVIEWMODE fvm2)
{
    return fvm1 == fvm2 || (IsIconViewMode(fvm1) && IsIconViewMode(fvm2));
}

/* Icon sizes sit on a logarithmic scale between the small icon row and the top */
static
int
SliderPosFromViewMode(
    _In_ int iIconSize,
    _In_ int iPosDefault,
    _In_ FOLDERVIEWMODE fvm)
{
    switch (fvm)
    {
        case FVM_LIST:
            return SLIDER_POS_LIST;
        case FVM_DETAILS:
            return SLIDER_POS_DETAILS;
        case FVM_TILE:
            return SLIDER_POS_TILES;
        case FVM_CONTENT:
            return SLIDER_POS_CONTENT;
        case FVM_ICON:
        case FVM_SMALLICON:
        case FVM_THUMBNAIL:
        case FVM_THUMBSTRIP:
            break;
        default:
            return iPosDefault;
    }

    /* The row sizes land exactly on their rows */
    if (g_fMetricsCalculated)
    {
        for (int iRow = VSR_EXTRALARGE; iRow <= VSR_SMALL; iRow++)
        {
            if (iIconSize == c_rgRows[iRow].iIconSize)
                return g_rgiSnapSpots[iRow];
        }
    }

    float flLogMin = logf((float)ICON_SIZE_MIN);
    float flLogRange = logf((float)ICON_SIZE_MAX) - flLogMin;
    float flShare = (float)((log((double)iIconSize) - flLogMin) / flLogRange);
    int iPos = (int)floorf(flShare * (float)(SLIDER_RANGE - SLIDER_POS_ICONS) + (float)SLIDER_POS_ICONS);

    return min(max(iPos, 0), SLIDER_RANGE);
}

/*
 * Works out the view a slider position stands for. With fSnap the position is pulled onto a row
 * close to it and the returned position is where the slider should rest.
 */
static
int
ViewModeFromSliderPos(
    _In_ int iPos,
    _Out_ FOLDERVIEWMODE *pfvm,
    _Out_ int *piIconSize,
    _In_ BOOL fSnap)
{
    int iSnapRow = -1;

    if (fSnap)
    {
        /* A row catches the position when it is within 7 pixels of the slider track */
        for (int iRow = 0; iRow < VSR_COUNT; iRow++)
        {
            if (7.0f / (float)g_sizeSlider.cy >= (float)abs(iPos - g_rgiSnapSpots[iRow]) / (float)SLIDER_RANGE)
            {
                iSnapRow = iRow;
                iPos = g_rgiSnapSpots[iRow];
                break;
            }
        }
    }

    if (iPos > (SLIDER_POS_ICONS + SLIDER_POS_LIST) / 2)
    {
        int iIconPos = max(iPos, SLIDER_POS_ICONS);
        float flLogMin = logf((float)ICON_SIZE_MIN);
        float flLogRange = logf((float)ICON_SIZE_MAX) - flLogMin;
        float flShare = (float)(iIconPos - SLIDER_POS_ICONS) / (float)(SLIDER_RANGE - SLIDER_POS_ICONS);
        int iSize = (int)ceilf(expf(flLogRange * flShare + flLogMin));

        *pfvm = FVM_THUMBNAIL;
        *piIconSize = min(max(iSize, ICON_SIZE_MIN), ICON_SIZE_MAX);
        if (!fSnap)
            return iPos;

        if (iSnapRow >= VSR_EXTRALARGE && iSnapRow <= VSR_SMALL)
            *piIconSize = c_rgRows[iSnapRow].iIconSize;
        return iIconPos;
    }

    int iRow;

    if (iPos > (SLIDER_POS_LIST + SLIDER_POS_DETAILS) / 2)
        iRow = VSR_LIST;
    else if (iPos > (SLIDER_POS_DETAILS + SLIDER_POS_TILES) / 2)
        iRow = VSR_DETAILS;
    else if (iPos > (SLIDER_POS_TILES + SLIDER_POS_CONTENT) / 2)
        iRow = VSR_TILES;
    else
        iRow = VSR_CONTENT;

    *pfvm = c_rgRows[iRow].fvm;
    *piIconSize = c_rgRows[iRow].iIconSize;
    return fSnap ? SliderPosFromViewMode(*piIconSize, 0, *pfvm) : iPos;
}

static
BOOL
CALLBACK
InitSliderMetrics(
    _Inout_ PINIT_ONCE pInitOnce,
    _Inout_opt_ PVOID pvParameter,
    _Outptr_opt_ PVOID *ppvContext)
{
    SHLogicalToPhysicalDPI(&g_sizeButton);
    SHLogicalToPhysicalDPI(&g_sizeSlider);
    SHLogicalToPhysicalDPI(&g_sizeDropDown);
    SHLogicalToPhysicalDPI(&g_cxButtonPadding, NULL);
    SHLogicalToPhysicalDPI(&g_xStart, NULL);
    SHLogicalToPhysicalDPI(NULL, &g_ySliderTop);

    for (int iRow = 0; iRow < VSR_COUNT; iRow++)
        g_rgiSnapSpots[iRow] = SliderPosFromViewMode(c_rgRows[iRow].iIconSize, 0, c_rgRows[iRow].fvm);
    g_fMetricsCalculated = TRUE;
    return TRUE;
}

/*
 * Clicking the button walks the views: icons, list, details, tiles, content, then large icons
 * and round again.
 */
static
void
ViewControlClick(
    _In_ IFolderView2 *pfv2)
{
    FOLDERVIEWMODE fvm;
    int iIconSize;

    if (FAILED(pfv2->GetViewModeAndIconSize(&fvm, &iIconSize)))
        return;

    switch (fvm)
    {
        case FVM_ICON:
        case FVM_SMALLICON:
        case FVM_THUMBNAIL:
        case FVM_THUMBSTRIP:
            fvm = FVM_LIST;
            iIconSize = 16;
            break;
        case FVM_DETAILS:
            fvm = FVM_TILE;
            iIconSize = 48;
            break;
        case FVM_TILE:
            fvm = FVM_CONTENT;
            iIconSize = 32;
            break;
        case FVM_CONTENT:
            fvm = FVM_THUMBNAIL;
            iIconSize = 96;
            break;
        default:
            fvm = FVM_DETAILS;
            iIconSize = 16;
            break;
    }
    pfv2->SetViewModeAndIconSize(fvm, iIconSize);
}

/* Moves a window back onto the monitor under a point when it hangs over an edge */
static
void
KeepWindowOnMonitor(
    _In_ HWND hwnd,
    _In_ POINT pt)
{
    MONITORINFO mi = { sizeof(mi) };
    RECT rc;

    if (!GetWindowRect(hwnd, &rc) || !GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi))
        return;

    int x = rc.left;
    int y = rc.top;

    if (rc.left < mi.rcMonitor.left)
        x = mi.rcMonitor.left;
    if (rc.right > mi.rcMonitor.right)
        x = mi.rcMonitor.right - (rc.right - rc.left) - 1;
    if (rc.top < mi.rcMonitor.top)
        y = mi.rcMonitor.top;
    if (rc.bottom > mi.rcMonitor.bottom)
        y = mi.rcMonitor.bottom - (rc.bottom - rc.top) - 1;

    if (x != rc.left || y != rc.top)
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Reads a boolean event argument; anything that does not convert counts as false */
static
BOOL
VariantToBool(
    _In_ const VARIANT *pvar)
{
    VARIANT varBool;
    BOOL fValue = FALSE;

    VariantInit(&varBool);
    if (SUCCEEDED(VariantChangeType(&varBool, pvar, 0, VT_BOOL)))
        fValue = (V_BOOL(&varBool) != VARIANT_FALSE);
    return fValue;
}

static
DirectUI::Button *
FindPart(
    _In_ DirectUI::Element *pe,
    _In_ PCWSTR pszID)
{
    return static_cast<DirectUI::Button *>(pe->FindDescendent(StrToID(pszID)));
}

/* Interaction states the button holds still while its slider is up */
static
BOOL
IsInteractionProp(
    _In_ const DirectUI::PropertyInfo *ppi)
{
    return ppi == DirectUI::Element::MouseWithinProp() || ppi == DirectUI::Element::KeyFocusedProp() ||
           ppi == DirectUI::Element::KeyWithinProp() || ppi == DirectUI::Element::MouseFocusedProp() ||
           ppi == DirectUI::Button::PressedProp() || ppi == DirectUI::Element::SelectedProp();
}

/* The popup with the view rows and the slider beside them */
class CViewSlider : public CDUIFrameZeroAlloc
{
public:
    CViewSlider() : _cRef(1) {}

    ULONG AddRef()
    {
        return InterlockedIncrement(&_cRef);
    }

    ULONG Release()
    {
        ULONG cRef = InterlockedDecrement(&_cRef);

        if (!cRef)
            delete this;
        return cRef;
    }

    BOOL Show(
        _In_ IFolderView2 *pfv2,
        _In_ const RECT *prcButton,
        _In_ HWND hwndOwner);

    ViewControl *_pViewControl;

private:
    virtual ~CViewSlider();

    static LRESULT CALLBACK s_WndProc(
        _In_ HWND hwnd,
        _In_ UINT uMsg,
        _In_ WPARAM wParam,
        _In_ LPARAM lParam);
    static LRESULT CALLBACK s_SliderWndProc(
        _In_ HWND hwnd,
        _In_ UINT uMsg,
        _In_ WPARAM wParam,
        _In_ LPARAM lParam,
        _In_ UINT_PTR uIdSubclass,
        _In_ DWORD_PTR dwRefData);

    LRESULT _WndProc(
        _In_ HWND hwnd,
        _In_ UINT uMsg,
        _In_ WPARAM wParam,
        _In_ LPARAM lParam);
    LRESULT _SliderWndProc(
        _In_ HWND hwnd,
        _In_ UINT uMsg,
        _In_ WPARAM wParam,
        _In_ LPARAM lParam);
    LRESULT _OnCreate(
        _In_ HWND hwnd);
    LRESULT _OnNotify(
        _In_ NMHDR *pnmh);
    LRESULT _OnCommand(
        _In_ WPARAM wParam);
    void _PaintBackground(
        _In_ HDC hdc,
        _In_ const RECT *prcClip,
        _In_opt_ HWND hwndTarget);
    BOOL _UpdatePosition(
        _In_ int iPos,
        _In_ BOOL fSnap,
        _Out_opt_ BOOL *pfViewChanged);
    void _OnPageKey(
        _In_ BOOL fUp);
    BOOL _OnArrowKeyOrMouseWheel(
        _In_ BOOL fUp);
    void _MoveTo(
        _In_ int iPos);
    void _ShowKeyboardCues();
    void _MakeAccessible();
    void _Close();

    LONG _cRef;
    IFolderView2 *_pfv2;
    RECT _rcButton;
    HWND _hwndOwner;
    HWND _hwnd;
    HWND _hwndSlider;
    HWND _rghwndRow[VSR_COUNT];
    HIMAGELIST _rghiml[VSR_COUNT];
    HTHEME _hTheme;
    BOOL _fClosing;
    BOOL _fViewChanged;
    int _iIconSize;
    int _rgyRow[VSR_COUNT];
    FOLDERVIEWMODE _fvm;
    LPARAM _lParamLastMouse;
    BOOL _fKeyboardCues;
    IAccPropServices *_pAccPropServices;
};

CViewSlider::~CViewSlider()
{
    if (_pAccPropServices)
    {
        _pAccPropServices->ClearHwndProps(_hwndSlider, OBJID_CLIENT, CHILDID_SELF, &PROPID_ACC_VALUEMAP, 1);
        IUnknown_SafeReleaseAndNullPtr(&_pAccPropServices);
    }

    if (_hTheme)
        CloseThemeData(_hTheme);

    for (int iRow = 0; iRow < VSR_COUNT; iRow++)
    {
        if (_rghiml[iRow])
        {
            ImageList_Destroy(_rghiml[iRow]);
            _rghiml[iRow] = NULL;
        }
    }

    IUnknown_SafeReleaseAndNullPtr(&_pfv2);
}

/* Opens the popup under the button; the window keeps its own reference until it goes away */
BOOL
CViewSlider::Show(
    _In_ IFolderView2 *pfv2,
    _In_ const RECT *prcButton,
    _In_ HWND hwndOwner)
{
    WNDCLASSEXW wcx = { sizeof(wcx) };
    WCHAR szTitle[64] = L"";
    DWORD dwExStyle = WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;

    _pfv2 = pfv2;
    _pfv2->AddRef();
    _rcButton = *prcButton;
    _hwndOwner = hwndOwner;

    if (!GetClassInfoExW(DUIFRAME_HINST, c_szViewSliderClass, &wcx))
    {
        wcx.cbSize = sizeof(wcx);
        wcx.style = CS_DROPSHADOW | CS_OWNDC;
        wcx.lpfnWndProc = s_WndProc;
        wcx.cbWndExtra = sizeof(CViewSlider *);
        wcx.hInstance = DUIFRAME_HINST;
        wcx.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wcx.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wcx.lpszClassName = c_szViewSliderClass;
        RegisterClassExW(&wcx);
    }

    LoadStringW(DUIFRAME_HINST, IDS_VIEWSLIDER_TITLE, szTitle, _countof(szTitle));

    if ((GetWindowLongW(hwndOwner, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) || IsBiDiLocalizedSystem())
        dwExStyle |= WS_EX_LAYOUTRTL;

    return SHFusionCreateWindowEx(dwExStyle, c_szViewSliderClass, szTitle,
                                  WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_BORDER | WS_TABSTOP,
                                  prcButton->left, prcButton->bottom, g_sizeDropDown.cx, g_sizeDropDown.cy,
                                  hwndOwner, NULL, DUIFRAME_HINST, this) != NULL;
}

LRESULT
CALLBACK
CViewSlider::s_WndProc(
    _In_ HWND hwnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    CViewSlider *pvs;

    if (uMsg == WM_NCCREATE)
    {
        pvs = (CViewSlider *)((CREATESTRUCTW *)lParam)->lpCreateParams;
        if (!pvs)
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);

        pvs->AddRef();
        SetWindowLongPtrW(hwnd, 0, (LONG_PTR)pvs);
    }
    else
    {
        pvs = (CViewSlider *)GetWindowLongPtrW(hwnd, 0);
    }

    if (!pvs)
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);

    pvs->AddRef();
    LRESULT lres = pvs->_WndProc(hwnd, uMsg, wParam, lParam);
    pvs->Release();
    return lres;
}

LRESULT
CALLBACK
CViewSlider::s_SliderWndProc(
    _In_ HWND hwnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam,
    _In_ UINT_PTR uIdSubclass,
    _In_ DWORD_PTR dwRefData)
{
    CViewSlider *pvs = (CViewSlider *)GetWindowLongPtrW((HWND)dwRefData, 0);

    if (!pvs)
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    return pvs->_SliderWndProc(hwnd, uMsg, wParam, lParam);
}

void
CViewSlider::_Close()
{
    _fClosing = TRUE;
    DestroyWindow(_hwnd);
}

/* The first key press shows the focus rectangle and the access keys */
void
CViewSlider::_ShowKeyboardCues()
{
    if (_fKeyboardCues)
        return;

    SendMessageW(_hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);
    _fKeyboardCues = TRUE;
}

LRESULT
CViewSlider::_WndProc(
    _In_ HWND hwnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_CREATE:
            return _OnCreate(hwnd);

        case WM_DESTROY:
            RemoveWindowSubclass(_hwndSlider, s_SliderWndProc, 1);
            if (_pViewControl)
            {
                _pViewControl->UnFreeze();
                _pViewControl = NULL;
            }
            Release();
            SetWindowLongPtrW(hwnd, 0, 0);
            return 0;

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && !_fClosing)
                _Close();
            return 0;

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            RECT rcClip;
            HDC hdc = BeginPaint(hwnd, &ps);

            if (GetClipBox(hdc, &rcClip) == NULLREGION)
                GetClientRect(hwnd, &rcClip);
            _PaintBackground(hdc, &rcClip, NULL);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_NOTIFY:
            return _OnNotify((NMHDR *)lParam);

        case WM_KEYDOWN:
            _ShowKeyboardCues();
            if (wParam == VK_RETURN || wParam == VK_ESCAPE)
            {
                _Close();
                return 0;
            }
            break;

        case WM_COMMAND:
            return _OnCommand(wParam);

        case WM_THEMECHANGED:
            if (_hTheme)
            {
                CloseThemeData(_hTheme);
                _hTheme = NULL;
            }
            if (wParam)
                _hTheme = OpenThemeData(_hwnd, L"Menu");
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

/* The slider takes over the keys and the wheel so every step lands on a view */
LRESULT
CViewSlider::_SliderWndProc(
    _In_ HWND hwnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_GETOBJECT:
            _MakeAccessible();
            break;

        case WM_KEYDOWN:
            _ShowKeyboardCues();
            switch (wParam)
            {
                case VK_RETURN:
                case VK_ESCAPE:
                    _Close();
                    return 0;

                case VK_PRIOR:
                case VK_NEXT:
                    _OnPageKey(wParam == VK_PRIOR);
                    return 0;

                case VK_HOME:
                case VK_END:
                    _MoveTo((wParam == VK_HOME) ? 0 : SLIDER_RANGE);
                    return 0;

                case VK_LEFT:
                case VK_UP:
                case VK_RIGHT:
                case VK_DOWN:
                    if (_OnArrowKeyOrMouseWheel(wParam == VK_UP || wParam == VK_RIGHT))
                        return 0;
                    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
            }
            return 0;

        case WM_MOUSEMOVE:
        {
            LRESULT lres = 0;

            /* Only real moves reach the slider, so a popup opening under a still mouse leaves it alone */
            if (lParam != _lParamLastMouse)
                lres = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            _lParamLastMouse = lParam;
            return lres;
        }

        case WM_LBUTTONUP:
            if (!_fViewChanged)
                break;
            _Close();
            return 0;

        case WM_MOUSEWHEEL:
            wParam &= ~(WPARAM)MK_CONTROL;
            if (_OnArrowKeyOrMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam) > 0))
                return 0;
            break;

        case WM_CAPTURECHANGED:
            if (!lParam)
                return 0;
            break;
    }

    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

LRESULT
CViewSlider::_OnCreate(
    _In_ HWND hwnd)
{
    WCHAR szText[64];
    RECT rcChannel = { 0 };
    RECT rcThumb = { 0 };

    _hwnd = hwnd;
    _hTheme = OpenThemeData(hwnd, L"Menu");

    /* A hidden label names the slider for accessibility tools */
    if (LoadStringW(DUIFRAME_HINST, IDS_VIEWSLIDER_SLIDER, szText, _countof(szText)))
        SHFusionCreateWindowEx(0, WC_STATICW, szText, WS_CHILD, 0, 0, 0, 0, _hwnd, NULL, DUIFRAME_HINST, NULL);

    _hwndSlider = SHFusionCreateWindowEx(0, TRACKBAR_CLASSW, NULL,
                                         WS_CHILD | TBS_VERT | TBS_NOTICKS | TBS_DOWNISLEFT | TBS_NOTIFYBEFOREMOVE,
                                         g_xStart, g_ySliderTop, g_sizeSlider.cx, g_sizeSlider.cy, _hwnd, NULL,
                                         DUIFRAME_HINST, NULL);
    if (!_hwndSlider)
    {
        _fClosing = TRUE;
        return -1;
    }

    SetWindowSubclass(_hwndSlider, s_SliderWndProc, 1, (DWORD_PTR)_hwnd);
    SetFocus(_hwndSlider);
    SendMessageW(_hwndSlider, TBM_SETRANGE, FALSE, MAKELPARAM(0, SLIDER_RANGE));
    SendMessageW(_hwndSlider, TBM_SETTHUMBLENGTH, g_sizeSlider.cx - 8, 0);

    /*
     * Row centers: the ends of the channel for the first and last rows, the tics for the others.
     * The channel of a vertical trackbar comes back as if it lay horizontally.
     */
    SendMessageW(_hwndSlider, TBM_GETCHANNELRECT, 0, (LPARAM)&rcChannel);
    SendMessageW(_hwndSlider, TBM_GETTHUMBRECT, 0, (LPARAM)&rcThumb);
    int cyHalfThumb = (rcThumb.bottom - rcThumb.top) / 2;

    _rgyRow[VSR_EXTRALARGE] = rcChannel.left + g_ySliderTop + cyHalfThumb;
    _rgyRow[VSR_CONTENT] = rcChannel.right + g_ySliderTop - cyHalfThumb;
    for (int iRow = VSR_LARGE; iRow < VSR_CONTENT; iRow++)
    {
        SendMessageW(_hwndSlider, TBM_SETTIC, 0, SLIDER_RANGE - g_rgiSnapSpots[iRow]);
        _rgyRow[iRow] = g_ySliderTop + (int)SendMessageW(_hwndSlider, TBM_GETTICPOS, iRow - VSR_LARGE, 0);
    }

    if (SUCCEEDED(_pfv2->GetViewModeAndIconSize(&_fvm, &_iIconSize)))
    {
        int iPos = _fvm ? SliderPosFromViewMode(_iIconSize, 0, _fvm) : 0;

        SendMessageW(_hwndSlider, TBM_SETPOS, TRUE, SLIDER_RANGE - iPos);
    }
    SendMessageW(_hwndSlider, TBM_SETLINESIZE, 0, 2);
    ShowWindow(_hwndSlider, SW_SHOW);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    /* One single button toolbar per row, centered on the row */
    int cxIcon = GetSystemMetrics(SM_CXSMICON);
    int cyIcon = GetSystemMetrics(SM_CYSMICON);

    for (int iRow = 0; iRow < VSR_COUNT; iRow++)
    {
        HWND hwndRow = SHFusionCreateWindowEx(0, TOOLBARCLASSNAMEW, NULL,
                                              WS_CHILD | TBSTYLE_LIST | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS |
                                              CCS_NODIVIDER | CCS_NOHILITE | CCS_NOPARENTALIGN | CCS_NORESIZE,
                                              g_sizeSlider.cx + 2 * g_xStart,
                                              _rgyRow[iRow] - g_sizeButton.cy / 2 + 2,
                                              g_sizeButton.cx, g_sizeButton.cy, _hwnd, NULL, DUIFRAME_HINST, NULL);

        _rghwndRow[iRow] = hwndRow;
        if (!hwndRow)
            continue;

        SendMessageW(hwndRow, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
        SendMessageW(hwndRow, TB_SETBITMAPSIZE, 0, MAKELPARAM(cxIcon, cyIcon));
        SendMessageW(hwndRow, TB_SETPADDING, 0, MAKELPARAM(g_cxButtonPadding, g_cxButtonPadding));

        _rghiml[iRow] = ImageList_Create(cxIcon, cyIcon, ILC_COLOR32 | ILC_MASK, 1, 0);
        ImageList_SetBkColor(_rghiml[iRow], CLR_NONE);
        if (_rghiml[iRow])
        {
            HICON hicon;

            if (SUCCEEDED(LoadIconWithScaleDown(DUIFRAME_HINST, MAKEINTRESOURCEW(c_rgRows[iRow].idIcon), cxIcon,
                                                cyIcon, &hicon)))
            {
                ImageList_ReplaceIcon(_rghiml[iRow], -1, hicon);
                DestroyIcon(hicon);
            }
            SendMessageW(hwndRow, TB_SETIMAGELIST, 0, (LPARAM)_rghiml[iRow]);
        }

        /* Tiles read "Extended Tiles" when the view shows the longer tiles */
        UINT idString = IDS_VIEWSLIDER_FIRSTROW + iRow;
        DWORD dwFolderFlags;

        if (iRow == VSR_TILES && SUCCEEDED(_pfv2->GetCurrentFolderFlags(&dwFolderFlags)) &&
            (dwFolderFlags & FWF_EXTENDEDTILES))
            idString = IDS_VIEWSLIDER_EXTENDEDTILES;
        SendMessageW(hwndRow, TB_ADDSTRINGW, (WPARAM)DUIFRAME_HINST, idString);

        TBBUTTON tbb = { 0 };

        tbb.idCommand = c_rgRows[iRow].idCommand;
        tbb.fsState = TBSTATE_ENABLED;
        SendMessageW(hwndRow, TB_ADDBUTTONSW, 1, (LPARAM)&tbb);
        ShowWindow(hwndRow, SW_SHOW);
    }

    for (int iRow = 0; iRow < VSR_COUNT; iRow++)
    {
        if (!_rghwndRow[iRow])
        {
            _fClosing = TRUE;
            return -1;
        }
    }

    /* Every row is as wide as the widest label, and the popup grows to fit them */
    int cxRow = g_sizeButton.cx;

    for (int iRow = 0; iRow < VSR_COUNT; iRow++)
    {
        RECT rcButton;

        SendMessageW(_rghwndRow[iRow], TB_GETRECT, c_rgRows[iRow].idCommand, (LPARAM)&rcButton);
        cxRow = max(cxRow, (int)(rcButton.right - rcButton.left));
    }

    int cxWindow = g_sizeDropDown.cx + cxRow + 2 * g_xStart - g_sizeButton.cx;

    SetWindowPos(_hwnd, NULL, 0, 0, cxWindow, g_sizeDropDown.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    for (int iRow = 0; iRow < VSR_COUNT; iRow++)
    {
        SetWindowPos(_rghwndRow[iRow], NULL, 0, 0, cxRow, g_sizeButton.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(_rghwndRow[iRow], TB_SETBUTTONWIDTH, 0, MAKELPARAM(cxRow, cxRow));
    }

    /* The thumb opens under the mouse when the button was clicked, else under the button's center */
    POINT pt;
    BOOL fMouseOnButton = GetCursorPos(&pt) && PtInRect(&_rcButton, pt);

    SendMessageW(_hwndSlider, TBM_GETTHUMBRECT, 0, (LPARAM)&rcThumb);
    int xThumb = g_xStart + (rcThumb.left + rcThumb.right) / 2;

    if (GetWindowLongW(_hwnd, GWL_EXSTYLE) & WS_EX_LAYOUTRTL)
        xThumb = cxWindow - xThumb;

    if (!fMouseOnButton)
    {
        pt.x = (_rcButton.left + _rcButton.right) / 2;
        pt.y = (_rcButton.top + _rcButton.bottom) / 2;
    }

    SetWindowPos(_hwnd, HWND_TOPMOST, pt.x - xThumb, pt.y - (rcThumb.top + rcThumb.bottom) / 2 - g_ySliderTop, 0, 0,
                 SWP_NOSIZE);
    KeepWindowOnMonitor(_hwnd, pt);

    /* A press still held on the button carries on as a drag of the thumb */
    int vkPrimary = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;

    if (GetAsyncKeyState(vkPrimary) < 0 && GetCursorPos(&pt) && PtInRect(&_rcButton, pt))
    {
        SendMessageW(_hwndSlider, TBM_GETTHUMBRECT, 0, (LPARAM)&rcThumb);
        ScreenToClient(_hwndSlider, &pt);
        if (PtInRect(&rcThumb, pt))
            SendMessageW(_hwndSlider, WM_LBUTTONDOWN, 0, MAKELPARAM(pt.x, pt.y));
    }

    SendMessageW(_hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_INITIALIZE, 0), 0);
    return 0;
}

/* Separator lines go between the rows below the icon sizes */
void
CViewSlider::_PaintBackground(
    _In_ HDC hdc,
    _In_ const RECT *prcClip,
    _In_opt_ HWND hwndTarget)
{
    RECT rcClient;
    RECT rcGutter;
    RECT rcItems;
    RECT rcLine;
    int cyLine;

    GetClientRect(_hwnd, &rcClient);
    rcGutter = rcClient;
    rcGutter.right = rcClient.left + g_xStart + g_sizeSlider.cx;
    rcItems = rcClient;
    rcItems.left = rcGutter.right;
    rcLine = rcItems;
    InflateRect(&rcLine, -GetSystemMetrics(SM_CXEDGE), 0);

    if (_hTheme)
    {
        SIZE sizeSeparator;

        if (hwndTarget)
            MapWindowPoints(_hwnd, hwndTarget, (POINT *)&rcGutter, 2);
        DrawThemeBackground(_hTheme, hdc, MENU_POPUPGUTTER, 0, &rcGutter, prcClip);
        if (!hwndTarget)
            DrawThemeBackground(_hTheme, hdc, MENU_POPUPBACKGROUND, 0, &rcItems, prcClip);

        if (FAILED(GetThemePartSize(_hTheme, hdc, MENU_POPUPSEPARATOR, 0, NULL, TS_TRUE, &sizeSeparator)))
            return;
        cyLine = sizeSeparator.cy;
    }
    else
    {
        SHFillRectClr(hdc, prcClip, GetSysColor(COLOR_MENU));
        cyLine = GetSystemMetrics(SM_CYEDGE);
    }

    for (int iRow = VSR_SMALL; iRow < VSR_CONTENT; iRow++)
    {
        RECT rc = rcLine;

        rc.top = (int)((UINT)(_rgyRow[iRow] + _rgyRow[iRow + 1]) >> 1) - cyLine / 2 - 1;
        rc.bottom = rc.top + cyLine;
        if (hwndTarget)
            MapWindowPoints(_hwnd, hwndTarget, (POINT *)&rc, 2);

        if (_hTheme)
            DrawThemeBackground(_hTheme, hdc, MENU_POPUPSEPARATOR, 0, &rc, prcClip);
        else
            DrawEdge(hdc, &rc, EDGE_ETCHED, BF_TOP);
    }
}

LRESULT
CViewSlider::_OnNotify(
    _In_ NMHDR *pnmh)
{
    if (pnmh->code == TRBN_THUMBPOSCHANGING)
    {
        NMTRBTHUMBPOSCHANGING *pnmtp = (NMTRBTHUMBPOSCHANGING *)pnmh;

        return _UpdatePosition(pnmtp->dwPos, (pnmtp->nReason & SLIDER_SNAP_REASONS) != 0, NULL);
    }

    /* The slider sits on the menu gutter, so it gets the same background behind it */
    if (pnmh->code == NM_CUSTOMDRAW)
    {
        NMCUSTOMDRAW *pnmcd = (NMCUSTOMDRAW *)pnmh;

        if (pnmcd->dwDrawStage == CDDS_PREPAINT && !pnmcd->dwItemSpec)
        {
            RECT rcClip;

            if (GetClipBox(pnmcd->hdc, &rcClip) == NULLREGION)
                rcClip = pnmcd->rc;
            _PaintBackground(pnmcd->hdc, &rcClip, pnmh->hwndFrom);
        }
    }
    return 0;
}

/* A row was clicked: the view takes that mode and the popup closes */
LRESULT
CViewSlider::_OnCommand(
    _In_ WPARAM wParam)
{
    _fClosing = TRUE;
    ShowWindow(_hwnd, SW_HIDE);
    InvalidateRect(_hwnd, NULL, TRUE);

    for (int iRow = 0; iRow < VSR_COUNT; iRow++)
    {
        if (c_rgRows[iRow].idCommand == LOWORD(wParam))
        {
            _pfv2->SetViewModeAndIconSize(c_rgRows[iRow].fvm, c_rgRows[iRow].iIconSize);
            break;
        }
    }

    DestroyWindow(_hwnd);
    return 0;
}

/*
 * Applies the view for a slider position. Returns TRUE when the slider had to move to the row it
 * snapped to; *pfViewChanged tells whether the view now looks different.
 */
BOOL
CViewSlider::_UpdatePosition(
    _In_ int iPos,
    _In_ BOOL fSnap,
    _Out_opt_ BOOL *pfViewChanged)
{
    FOLDERVIEWMODE fvmOld = FVM_ICON;
    int iIconSizeOld = -1;
    BOOL fMoved = FALSE;
    BOOL fViewChanged = FALSE;
    int iRestPos = SLIDER_RANGE - ViewModeFromSliderPos(SLIDER_RANGE - iPos, &_fvm, &_iIconSize, fSnap);

    if (iRestPos != iPos)
    {
        fMoved = TRUE;
        SendMessageW(_hwndSlider, TBM_SETPOS, TRUE, iRestPos);
    }

    _pfv2->GetViewModeAndIconSize(&fvmOld, &iIconSizeOld);
    _pfv2->SetViewModeAndIconSize(_fvm, _iIconSize);
    if (!IsSameViewLayout(fvmOld, _fvm) || iIconSizeOld != _iIconSize)
    {
        _fViewChanged = TRUE;
        fViewChanged = TRUE;
    }

    if (pfViewChanged)
        *pfViewChanged = fViewChanged;
    return fMoved;
}

/* Puts the slider at a position measured from the top and applies its view */
void
CViewSlider::_MoveTo(
    _In_ int iPos)
{
    SendMessageW(_hwndSlider, TBM_SETPOS, TRUE, iPos);
    _UpdatePosition(iPos, FALSE, NULL);
}

/* Page up and down jump to the next row above or below */
void
CViewSlider::_OnPageKey(
    _In_ BOOL fUp)
{
    int iPos = SLIDER_RANGE - (int)SendMessageW(_hwndSlider, TBM_GETPOS, 0, 0);
    int iTarget = -1;

    for (int iRow = 0; iRow < VSR_COUNT - 1 && iTarget == -1; iRow++)
    {
        int iAbove = g_rgiSnapSpots[iRow];
        int iBelow = g_rgiSnapSpots[iRow + 1];

        if (iPos < iAbove && iPos > iBelow)
            iTarget = fUp ? iAbove : iBelow;
        else if (fUp && iPos == iBelow)
            iTarget = iAbove;
        else if (!fUp && iPos == iAbove)
            iTarget = iBelow;
    }

    if (iTarget != -1)
        _MoveTo(SLIDER_RANGE - iTarget);
}

/*
 * Below the icon sizes every step goes to the next row. Among the icon sizes the slider creeps
 * along until the view looks different.
 */
BOOL
CViewSlider::_OnArrowKeyOrMouseWheel(
    _In_ BOOL fUp)
{
    int iPos = SLIDER_RANGE - (int)SendMessageW(_hwndSlider, TBM_GETPOS, 0, 0);
    int iTarget = -1;

    if (iPos == g_rgiSnapSpots[VSR_CONTENT])
        iTarget = g_rgiSnapSpots[fUp ? VSR_TILES : VSR_CONTENT];
    else if (iPos == g_rgiSnapSpots[VSR_TILES])
        iTarget = g_rgiSnapSpots[fUp ? VSR_DETAILS : VSR_CONTENT];
    else if (iPos == g_rgiSnapSpots[VSR_DETAILS])
        iTarget = g_rgiSnapSpots[fUp ? VSR_LIST : VSR_TILES];
    else if (iPos == g_rgiSnapSpots[VSR_LIST])
        iTarget = g_rgiSnapSpots[fUp ? VSR_SMALL : VSR_DETAILS];
    else if (iPos <= g_rgiSnapSpots[VSR_SMALL] && !fUp)
        iTarget = g_rgiSnapSpots[VSR_LIST];

    if (iTarget != -1)
    {
        _MoveTo(SLIDER_RANGE - iTarget);
        return TRUE;
    }

    BOOL fViewChanged = FALSE;
    int iStep = fUp ? 1 : -1;
    int iNext;

    for (iNext = iPos + iStep; iNext >= 0 && iNext <= SLIDER_RANGE; iNext += iStep)
    {
        _UpdatePosition(SLIDER_RANGE - iNext, FALSE, &fViewChanged);
        if (fViewChanged)
            break;
    }
    iNext = min(max(iNext, 0), SLIDER_RANGE);

    if (iNext == iPos)
        return FALSE;

    SendMessageW(_hwndSlider, TBM_SETPOS, TRUE, SLIDER_RANGE - iNext);
    return TRUE;
}

/* Screen readers get the row names for the slider positions */
void
CViewSlider::_MakeAccessible()
{
    WCHAR szValueMap[256] = L"";

    if (_pAccPropServices)
        return;

    LoadStringW(DUIFRAME_HINST, IDS_VIEWSLIDER_VALUEMAP, szValueMap, _countof(szValueMap));
    if (SUCCEEDED(CoCreateInstance(CLSID_AccPropServices, NULL, CLSCTX_SERVER, IID_PPV_ARGS(&_pAccPropServices))))
        _pAccPropServices->SetHwndPropStr(_hwndSlider, OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_VALUEMAP, szValueMap);
}

/* ViewControl */

HRESULT
WINAPI
ViewControl::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    ViewControl *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<ViewControl>();
    if (!pe)
        return E_OUTOFMEMORY;

    hr = pe->Initialize(0, peParent, pdwDeferCookie);
    if (FAILED(hr))
    {
        pe->Destroy(false);
        return hr;
    }

    pe->SetActive(0);
    *ppElement = pe;
    return S_OK;
}

HRESULT
WINAPI
ViewControl::Register()
{
    return DirectUI::ClassInfo<ViewControl, CElementWithSite, DirectUI::StandardCreator<ViewControl>>::Register(
        DUIFRAME_HINST, L"ViewControl", NULL, 0, false);
}

STDMETHODIMP
ViewControl::QueryInterface(
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(ViewControl, IDispatch),
        {0},
    };
    HRESULT hr = QISearch(this, qit, riid, ppv);

    if (FAILED(hr))
        hr = CElementWithSite::QueryInterface(riid, ppv);
    return hr;
}

/* Following the view's events keeps the icon in step with the view mode */
STDMETHODIMP
ViewControl::SetSite(
    _In_opt_ IUnknown *punkSite)
{
    DirectUI::Element *peAction;

    if (_punkSite)
        _ConnectToView(FALSE);
    IUnknown_Set(&_punkSite, punkSite);

    peAction = FindPart(this, L"ActionButton");
    if (punkSite)
    {
        _ConnectToView(TRUE);
        _UpdateIcon();
        if (peAction)
            peAction->AddListener(this);
    }
    else if (peAction)
    {
        peAction->RemoveListener(this);
    }
    return S_OK;
}

HRESULT
ViewControl::_ConnectToView(
    _In_ BOOL fConnect)
{
    HRESULT hr;

    if (!fConnect)
    {
        hr = ConnectToConnectionPoint(static_cast<IDispatch *>(this), DIID_DShellFolderViewEvents, FALSE, _pdispView,
                                      &_dwViewEventsCookie, NULL);
        IUnknown_SafeReleaseAndNullPtr(&_pdispView);
        _dwViewEventsCookie = 0;
        return hr;
    }

    IShellView *psv;

    hr = IUnknown_QueryService(_punkSite, SID_SFolderView, IID_PPV_ARGS(&psv));
    if (SUCCEEDED(hr))
    {
        IDispatch *pdisp;

        hr = psv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&pdisp));
        if (SUCCEEDED(hr))
        {
            hr = ConnectToConnectionPoint(static_cast<IDispatch *>(this), DIID_DShellFolderViewEvents, TRUE, pdisp,
                                          &_dwViewEventsCookie, NULL);
            if (SUCCEEDED(hr))
                IUnknown_Set((IUnknown **)&_pdispView, pdisp);
            pdisp->Release();
        }
        psv->Release();
    }
    return hr;
}

/* The button shows the icon of the current view: the row icon, or the nearest icon size */
HRESULT
ViewControl::_UpdateIcon()
{
    IFolderView2 *pfv2;
    FOLDERVIEWMODE fvm;
    int iIconSize;
    HRESULT hr;

    hr = IUnknown_QueryService(_punkSite, SID_SFolderView, IID_PPV_ARGS(&pfv2));
    if (FAILED(hr))
        return hr;

    hr = pfv2->GetViewModeAndIconSize(&fvm, &iIconSize);
    if (SUCCEEDED(hr))
    {
        UINT idIcon = IDI_VIEW_EXTRALARGE;

        switch (fvm)
        {
            case FVM_ICON:
            case FVM_SMALLICON:
            case FVM_THUMBNAIL:
            case FVM_THUMBSTRIP:
                if (iIconSize >= 96)
                    idIcon = (iIconSize == 256) ? IDI_VIEW_EXTRALARGE : IDI_VIEW_LARGE;
                else
                    idIcon = (iIconSize >= 48) ? IDI_VIEW_MEDIUM : IDI_VIEW_SMALL;
                break;
            case FVM_LIST:
                idIcon = IDI_VIEW_LIST;
                break;
            case FVM_DETAILS:
                idIcon = IDI_VIEW_DETAILS;
                break;
            case FVM_TILE:
                idIcon = IDI_VIEW_TILES;
                break;
            case FVM_CONTENT:
                idIcon = IDI_VIEW_CONTENT;
                break;
            default:
                break;
        }

        if (idIcon != _idIcon)
        {
            DirectUI::Element *peIcon = FindPart(this, L"ActionButtonIcon");
            HICON hicon;

            _idIcon = idIcon;
            hr = LoadIconWithScaleDown(DUIFRAME_HINST, MAKEINTRESOURCEW(idIcon), GetSystemMetrics(SM_CXSMICON),
                                       GetSystemMetrics(SM_CYSMICON), &hicon);
            if (SUCCEEDED(hr))
            {
                DirectUI::Value *pv = peIcon ? DirectUI::Value::CreateGraphic(hicon, true, false, false) : NULL;

                if (pv)
                {
                    peIcon->SetValue(DirectUI::Element::ContentProp, PROPERTY_STORE_LOCAL, pv);
                    pv->Release();
                }
                else
                {
                    DestroyIcon(hicon);
                }
            }
        }
    }

    pfv2->Release();
    return hr;
}

/* Ctrl+wheel over the view steps through the views the way the slider does */
HRESULT
ViewControl::_MouseWheel(
    _In_ BOOL fUp)
{
    IFolderView2 *pfv2;
    FOLDERVIEWMODE fvmCurrent;
    int iIconSizeCurrent;
    HRESULT hr;

    hr = IUnknown_QueryService(_punkSite, SID_SFolderView, IID_PPV_ARGS(&pfv2));
    if (FAILED(hr))
        return hr;

    hr = pfv2->GetViewModeAndIconSize(&fvmCurrent, &iIconSizeCurrent);
    if (SUCCEEDED(hr))
    {
        int iPos = fvmCurrent ? SliderPosFromViewMode(iIconSizeCurrent, 0, fvmCurrent) : 0;
        FOLDERVIEWMODE fvm;
        int iIconSize;

        do
        {
            iPos = fUp ? min(iPos + 2, SLIDER_RANGE) : max(iPos - 2, 0);
            ViewModeFromSliderPos(iPos, &fvm, &iIconSize, FALSE);
        } while (iIconSize == iIconSizeCurrent && IsSameViewLayout(fvm, fvmCurrent) &&
                 iPos != SLIDER_RANGE && iPos != 0);

        hr = pfv2->SetViewModeAndIconSize(fvm, iIconSize);
    }

    pfv2->Release();
    return hr;
}

STDMETHODIMP
ViewControl::GetTypeInfoCount(
    _Out_ UINT *pctinfo)
{
    return E_NOTIMPL;
}

STDMETHODIMP
ViewControl::GetTypeInfo(
    _In_ UINT iTInfo,
    _In_ LCID lcid,
    _Outptr_ ITypeInfo **ppTInfo)
{
    return E_NOTIMPL;
}

STDMETHODIMP
ViewControl::GetIDsOfNames(
    _In_ REFIID riid,
    _In_reads_(cNames) LPOLESTR *rgszNames,
    _In_ UINT cNames,
    _In_ LCID lcid,
    _Out_writes_(cNames) DISPID *rgDispId)
{
    return E_NOTIMPL;
}

/* DShellFolderViewEvents of the view */
STDMETHODIMP
ViewControl::Invoke(
    _In_ DISPID dispIdMember,
    _In_ REFIID riid,
    _In_ LCID lcid,
    _In_ WORD wFlags,
    _In_ DISPPARAMS *pDispParams,
    _Out_opt_ VARIANT *pVarResult,
    _Out_opt_ EXCEPINFO *pExcepInfo,
    _Out_opt_ UINT *puArgErr)
{
    if (pVarResult)
        VariantInit(pVarResult);

    switch (dispIdMember)
    {
        case DISPID_VIEWMODECHANGED:
        case DISPID_ICONSIZECHANGED:
            return _UpdateIcon();

        case DISPID_CTRLMOUSEWHEEL:
        {
            BOOL fUp = FALSE;

            if (pDispParams && pDispParams->cArgs)
                fUp = VariantToBool(&pDispParams->rgvarg[0]);
            return _MouseWheel(fUp);
        }
    }
    return E_INVALIDARG;
}

/* The main half cycles the view, the arrow half opens the slider */
void
ViewControl::_DoAction(
    _In_ BOOL fShowSlider)
{
    if (!fShowSlider)
    {
        IFolderView2 *pfv2;

        if (SUCCEEDED(IUnknown_QueryService(_punkSite, SID_SFolderView, IID_PPV_ARGS(&pfv2))))
        {
            ViewControlClick(pfv2);
            pfv2->Release();
        }
    }
    else if (!_fFrozen)
    {
        _fFrozen = TRUE;
        _ShowViewSlider(FindPart(this, L"SplitMenuButton"));
    }
}

/* The button stays frozen in its pressed look until the slider goes away */
HRESULT
ViewControl::_ShowViewSlider(
    _In_ DirectUI::Element *peAnchor)
{
    IFolderView2 *pfv2;
    HRESULT hr;

    hr = IUnknown_QueryService(_punkSite, SID_SFolderView, IID_PPV_ARGS(&pfv2));
    if (SUCCEEDED(hr))
    {
        RECT rcAnchor;
        HWND hwndRoot;

        hr = DUI_GetElementScreenBounds(peAnchor, &rcAnchor);
        if (SUCCEEDED(hr))
            hr = DUI_GetElementRootHWND(peAnchor, &hwndRoot);
        if (SUCCEEDED(hr))
        {
            CViewSlider *pSlider = new CViewSlider();

            InitOnceExecuteOnce(&g_ioMetrics, InitSliderMetrics, NULL, NULL);
            _pSlider = pSlider;
            if (pSlider)
            {
                pSlider->_pViewControl = this;

                /* A popup that never came up cannot unfreeze the button itself */
                if (!pSlider->Show(pfv2, &rcAnchor, hwndRoot) && _pSlider == pSlider)
                    hr = E_FAIL;
            }
            else
            {
                hr = E_OUTOFMEMORY;
            }
        }
        pfv2->Release();
    }

    if (FAILED(hr))
        UnFreeze();
    return hr;
}

void
ViewControl::UnFreeze()
{
    ULONG dwDeferCookie = 0;
    DirectUI::Button *pbMenu = FindPart(this, L"SplitMenuButton");
    DirectUI::Button *pbAction = FindPart(this, L"ActionButton");

    _fFrozen = FALSE;
    if (_pSlider)
    {
        CViewSlider *pSlider = _pSlider;

        _pSlider = NULL;
        pSlider->Release();
    }

    StartDefer(&dwDeferCookie);
    if (pbMenu)
        pbMenu->SetPressed(false);
    if (pbAction)
        pbAction->SetPressed(false);

    /* The arrow half stays lit when the mouse came back over the button meanwhile */
    if (_fMouseEnteredWhileFrozen)
        _fMouseEnteredWhileFrozen = FALSE;
    else if (pbMenu)
        pbMenu->SetSelected(false);

    if (pbAction)
        pbAction->SetSelected(false);
    if (dwDeferCookie)
        EndDefer(dwDeferCookie);
}

void
ViewControl::OnDestroy()
{
    if (_pSlider)
    {
        CViewSlider *pSlider = _pSlider;

        pSlider->_pViewControl = NULL;
        _pSlider = NULL;
        pSlider->Release();
    }

    /* The view must not call into an element that is gone */
    if (_pdispView)
        _ConnectToView(FALSE);

    CElementWithSite::OnDestroy();
}

HRESULT
ViewControl::DefaultAction()
{
    _DoAction(FALSE);
    return S_OK;
}

void
ViewControl::OnEvent(
    _In_ DirectUI::Event *pev)
{
    if (pev->nStage == GMF_BUBBLED && pev->uidType.m_Value == DirectUI::Button::Click().m_Value)
    {
        _DoAction(pev->peTarget == FindPart(this, L"SplitMenuButton"));
        pev->fHandled = true;
    }

    if (!pev->fHandled)
        DirectUI::Element::OnEvent(pev);
}

/*
 * A left press on the arrow opens the slider at once, so the press can go on as a drag of the
 * thumb. Space and enter press the main half, down the arrow; the action runs on key up.
 */
void
ViewControl::OnInput(
    _Inout_ DirectUI::InputEvent *pie)
{
    DirectUI::Button *pbMenu = FindPart(this, L"SplitMenuButton");

    if (pie->nDevice == GINPUT_MOUSE && pie->nCode == GMOUSE_DOWN && pie->nStage == GMF_ROUTED &&
        pie->peTarget == pbMenu)
    {
        if (static_cast<DirectUI::MouseEvent *>(pie)->bButton == GBUTTON_LEFT)
        {
            _fFrozen = TRUE;
            _ShowViewSlider(pie->peTarget);
            pie->fHandled = true;
        }
    }
    else if (pie->nStage == GMF_DIRECT && pie->nDevice == GINPUT_KEYBOARD)
    {
        WCHAR ch = static_cast<DirectUI::KeyboardEvent *>(pie)->ch;
        DirectUI::Button *pb = NULL;

        if (ch == VK_SPACE || ch == VK_RETURN)
            pb = FindPart(this, L"ActionButton");
        else if (ch == VK_DOWN)
            pb = pbMenu;

        if (pb)
        {
            if (pie->nCode == GKEY_DOWN)
            {
                _pbKeyPressed = pb;
                pb->SetPressed(true);
                pie->fHandled = true;
            }
            else if (pie->nCode == GKEY_UP && pb == _pbKeyPressed)
            {
                if (pb == pbMenu)
                {
                    _fFrozen = TRUE;
                    _ShowViewSlider(pbMenu);
                }
                else
                {
                    pb->SetPressed(false);
                    pb->DefaultAction();
                }
                pie->fHandled = true;
            }
        }
    }

    if (!pie->fHandled)
        DirectUI::Element::OnInput(pie);
}

bool
ViewControl::OnPropertyChanging(
    _In_ const DirectUI::PropertyInfo *ppi,
    _In_ int iIndex,
    _In_ DirectUI::Value *pvOld,
    _In_ DirectUI::Value *pvNew)
{
    if (_fFrozen && IsInteractionProp(ppi) && (int)(ppi->Flags & 3) == iIndex)
        return false;
    return DirectUI::Element::OnPropertyChanging(ppi, iIndex, pvOld, pvNew);
}

/* Hovering lights both halves; losing focus lets go of a key press unless the slider is up */
void
ViewControl::OnPropertyChanged(
    _In_ const DirectUI::PropertyInfo *ppi,
    _In_ int iIndex,
    _In_ DirectUI::Value *pvOld,
    _In_ DirectUI::Value *pvNew)
{
    if (ppi == DirectUI::Element::MouseWithinProp() && (int)(ppi->Flags & 3) == iIndex)
    {
        ULONG dwDeferCookie = 0;
        bool fWithin = pvNew->GetBool();
        DirectUI::Element *pe;

        StartDefer(&dwDeferCookie);
        if (_fFrozen && fWithin)
            _fMouseEnteredWhileFrozen = TRUE;

        pe = FindPart(this, L"SplitMenuButton");
        if (pe)
            pe->SetSelected(fWithin);
        pe = FindPart(this, L"ActionButton");
        if (pe)
            pe->SetSelected(fWithin);
        if (dwDeferCookie)
            EndDefer(dwDeferCookie);
    }
    else if (ppi == DirectUI::Element::KeyFocusedProp() && (int)(ppi->Flags & 3) == iIndex)
    {
        if (_pbKeyPressed && !_fFrozen)
        {
            _pbKeyPressed->SetPressed(false);
            _pbKeyPressed = NULL;
        }
    }
    DirectUI::Element::OnPropertyChanged(ppi, iIndex, pvOld, pvNew);
}

/* While frozen the main half keeps its look, except that the arrow may light up again on hover */
bool
ViewControl::OnListenedPropertyChanging(
    _In_ DirectUI::Element *peFrom,
    _In_ const DirectUI::PropertyInfo *ppi,
    _In_ int iIndex,
    _In_ DirectUI::Value *pvOld,
    _In_ DirectUI::Value *pvNew)
{
    BOOL fHeldProp = ppi == DirectUI::Element::MouseWithinProp() || ppi == DirectUI::Element::KeyFocusedProp() ||
                     ppi == DirectUI::Element::MouseFocusedProp() || ppi == DirectUI::Element::SelectedProp() ||
                     (ppi == DirectUI::Button::PressedProp() && (int)(ppi->Flags & 3) == iIndex);

    if (!fHeldProp || !_fFrozen)
        return true;

    return peFrom == FindPart(this, L"SplitMenuButton") && _fMouseEnteredWhileFrozen &&
           ppi == DirectUI::Element::SelectedProp() && pvNew->GetBool();
}

/* Pressing the main half presses the arrow half with it */
void
ViewControl::OnListenedPropertyChanged(
    _In_ DirectUI::Element *peFrom,
    _In_ const DirectUI::PropertyInfo *ppi,
    _In_ int iIndex,
    _In_ DirectUI::Value *pvOld,
    _In_ DirectUI::Value *pvNew)
{
    if (ppi == DirectUI::Button::PressedProp() && (int)(ppi->Flags & 3) == iIndex &&
        peFrom == FindPart(this, L"ActionButton"))
    {
        DirectUI::Button *pbMenu = FindPart(this, L"SplitMenuButton");

        if (pbMenu)
            pbMenu->SetPressed(pvNew->GetBool());
    }
}
