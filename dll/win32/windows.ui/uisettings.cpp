/*
 * PROJECT:     ReactOS Windows.UI
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Windows.UI.ViewManagement.UISettings, the user's interface settings for apps
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#define WIN32_NO_STATUS
#define COBJMACROS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#include <objbase.h>
#include <inspectable.h>
#include <activation.h>
#include <winstring.h>
#include <new>

#define UI_SETTINGS_CLASS L"Windows.UI.ViewManagement.UISettings"

#ifndef SPI_GETMESSAGEDURATION
#define SPI_GETMESSAGEDURATION 0x2016
#endif

/* Windows.UI.Color and Windows.Foundation.Size as the ABI passes them */
typedef struct _UI_COLOR
{
    BYTE A;
    BYTE R;
    BYTE G;
    BYTE B;
} UI_COLOR;

typedef struct _UI_SIZE
{
    FLOAT Width;
    FLOAT Height;
} UI_SIZE;

/* Windows.UI.ViewManagement.UIColorType */
enum
{
    UiColorBackground,
    UiColorForeground,
    UiColorAccentDark3,
    UiColorAccentDark2,
    UiColorAccentDark1,
    UiColorAccent,
    UiColorAccentLight1,
    UiColorAccentLight2,
    UiColorAccentLight3,
    UiColorComplement
};

/* Windows.UI.ViewManagement.UIElementType values past the system colors */
#define UI_ELEMENT_LAST_SYSTEM_COLOR    12
#define UI_ELEMENT_ACCENT_COLOR         1000

/* Windows.Foundation.EventRegistrationToken */
typedef struct _UI_EVENT_TOKEN
{
    INT64 value;
} UI_EVENT_TOKEN;

static const IID UiIidInspectable = { 0xaf86e2e0, 0xb12d, 0x4c6a, { 0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90 } };
static const IID UiIidActivationFactory = { 0x00000035, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const IID UiIidAgileObject = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };
static const IID IID_IUISettings = { 0x85361600, 0x1c63, 0x4627, { 0xbc, 0xb1, 0x3a, 0x89, 0xe0, 0xbc, 0x9c, 0x55 } };
static const IID IID_IUISettings2 = { 0xbad82401, 0x2721, 0x44f9, { 0xbb, 0x91, 0x2b, 0xb2, 0x28, 0xbe, 0x44, 0x2f } };
static const IID IID_IUISettings3 = { 0x03021be4, 0x5254, 0x4781, { 0x81, 0x94, 0x51, 0x68, 0xf7, 0xd0, 0x6d, 0x7b } };

/* The interfaces, in the order the ABI lays their methods out */
struct IUISettings : public IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_HandPreference(INT32 *Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CursorSize(UI_SIZE *Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ScrollBarSize(UI_SIZE *Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ScrollBarArrowSize(UI_SIZE *Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ScrollBarThumbBoxSize(UI_SIZE *Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MessageDuration(UINT32 *Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AnimationsEnabled(BOOLEAN *Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CaretBrowsingEnabled(BOOLEAN *Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CaretBlinkRate(UINT32 *Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CaretWidth(UINT32 *Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DoubleClickTime(UINT32 *Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MouseHoverTime(UINT32 *Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE UIElementColor(INT32 Element, UI_COLOR *Value) = 0;
};

struct IUISettings2 : public IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_TextScaleFactor(DOUBLE *Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_TextScaleFactorChanged(IUnknown *Handler, UI_EVENT_TOKEN *Cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_TextScaleFactorChanged(UI_EVENT_TOKEN Cookie) = 0;
};

struct IUISettings3 : public IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE GetColorValue(INT32 Type, UI_COLOR *Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ColorValuesChanged(IUnknown *Handler, UI_EVENT_TOKEN *Cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ColorValuesChanged(UI_EVENT_TOKEN Cookie) = 0;
};

static LONG UiObjectCount;
static volatile LONG64 UiNextToken;

static
UI_COLOR
UiColorFromColorref(
    _In_ COLORREF Color)
{
    UI_COLOR Result = { 0xFF, GetRValue(Color), GetGValue(Color), GetBValue(Color) };
    return Result;
}

/* Blends a color toward another; Amount is out of 256 */
static
UI_COLOR
UiBlend(
    _In_ UI_COLOR From,
    _In_ UI_COLOR To,
    _In_ ULONG Amount)
{
    UI_COLOR Result;

    Result.A = 0xFF;
    Result.R = (BYTE)(From.R + ((To.R - From.R) * (LONG)Amount) / 256);
    Result.G = (BYTE)(From.G + ((To.G - From.G) * (LONG)Amount) / 256);
    Result.B = (BYTE)(From.B + ((To.B - From.B) * (LONG)Amount) / 256);
    return Result;
}

static
BOOL
UiReadDword(
    _In_ PCWSTR SubKey,
    _In_ PCWSTR Name,
    _Out_ PDWORD Value)
{
    DWORD Size = sizeof(*Value);
    return RegGetValueW(HKEY_CURRENT_USER, SubKey, Name, RRF_RT_REG_DWORD, NULL, Value, &Size) == ERROR_SUCCESS;
}

/* Apps are dark when the user turned their light theme off */
static
BOOL
UiAppsUseDarkTheme(VOID)
{
    DWORD Light;

    return UiReadDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", &Light) &&
           !Light;
}

/*
 * The accent and its shades. Explorer keeps them in AccentPalette, lightest first, as red,
 * green, blue and a spare byte; without it the shades are made from the DWM accent or the
 * selection color.
 */
static
UI_COLOR
UiAccentColor(
    _In_ INT32 Type)
{
    static const UI_COLOR White = { 0xFF, 0xFF, 0xFF, 0xFF }, Black = { 0xFF, 0, 0, 0 };
    BYTE Palette[32];
    DWORD Size = sizeof(Palette), Accent;
    UI_COLOR Base;
    ULONG Index;

    if (Type <= UiColorAccentLight3 &&
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, NULL, Palette, &Size) == ERROR_SUCCESS &&
        Size == sizeof(Palette))
    {
        Index = (ULONG)(UiColorAccentLight3 - Type) * 4;
        Base.A = 0xFF;
        Base.R = Palette[Index];
        Base.G = Palette[Index + 1];
        Base.B = Palette[Index + 2];
        return Base;
    }

    /* AccentColor is ABGR */
    if (UiReadDword(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", &Accent))
        Base = UiColorFromColorref(Accent & 0x00FFFFFF);
    else
        Base = UiColorFromColorref(GetSysColor(COLOR_HIGHLIGHT));

    switch (Type)
    {
        case UiColorAccentDark3:
            return UiBlend(Base, Black, 192);
        case UiColorAccentDark2:
            return UiBlend(Base, Black, 128);
        case UiColorAccentDark1:
            return UiBlend(Base, Black, 64);
        case UiColorAccentLight1:
            return UiBlend(Base, White, 64);
        case UiColorAccentLight2:
            return UiBlend(Base, White, 128);
        case UiColorAccentLight3:
            return UiBlend(Base, White, 192);
        default:
            return Base;
    }
}

class CUISettings : public IUISettings, public IUISettings2, public IUISettings3
{
public:
    CUISettings() : m_Refs(1)
    {
        InterlockedIncrement(&UiObjectCount);
    }

    virtual ~CUISettings()
    {
        InterlockedDecrement(&UiObjectCount);
    }

    /* IUnknown */
    STDMETHODIMP
    QueryInterface(
        _In_ REFIID Riid,
        _Outptr_ VOID **Object) override
    {
        if (!Object)
            return E_POINTER;

        if (IsEqualIID(Riid, IID_IUnknown) || IsEqualIID(Riid, UiIidInspectable) || IsEqualIID(Riid, UiIidAgileObject) ||
            IsEqualIID(Riid, IID_IUISettings))
        {
            *Object = static_cast<IUISettings *>(this);
        }
        else if (IsEqualIID(Riid, IID_IUISettings2))
        {
            *Object = static_cast<IUISettings2 *>(this);
        }
        else if (IsEqualIID(Riid, IID_IUISettings3))
        {
            *Object = static_cast<IUISettings3 *>(this);
        }
        else
        {
            *Object = NULL;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG)
    AddRef() override
    {
        return InterlockedIncrement(&m_Refs);
    }

    STDMETHODIMP_(ULONG)
    Release() override
    {
        ULONG Refs = InterlockedDecrement(&m_Refs);

        if (!Refs)
            delete this;
        return Refs;
    }

    /* IInspectable */
    STDMETHODIMP
    GetIids(
        _Out_ ULONG *Count,
        _Outptr_ IID **Iids) override
    {
        if (!Count || !Iids)
            return E_POINTER;

        *Iids = (IID *)CoTaskMemAlloc(3 * sizeof(IID));
        if (!*Iids)
            return E_OUTOFMEMORY;
        (*Iids)[0] = IID_IUISettings;
        (*Iids)[1] = IID_IUISettings2;
        (*Iids)[2] = IID_IUISettings3;
        *Count = 3;
        return S_OK;
    }

    STDMETHODIMP
    GetRuntimeClassName(
        _Out_ HSTRING *ClassName) override
    {
        if (!ClassName)
            return E_POINTER;
        return WindowsCreateString(UI_SETTINGS_CLASS, ARRAYSIZE(UI_SETTINGS_CLASS) - 1, ClassName);
    }

    STDMETHODIMP
    GetTrustLevel(
        _Out_ TrustLevel *Level) override
    {
        if (!Level)
            return E_POINTER;
        *Level = BaseTrust;
        return S_OK;
    }

    /* IUISettings */
    STDMETHODIMP
    get_HandPreference(
        _Out_ INT32 *Value) override
    {
        if (!Value)
            return E_POINTER;
        *Value = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? 0 : 1;
        return S_OK;
    }

    STDMETHODIMP
    get_CursorSize(
        _Out_ UI_SIZE *Value) override
    {
        return GetSize(SM_CXCURSOR, SM_CYCURSOR, Value);
    }

    STDMETHODIMP
    get_ScrollBarSize(
        _Out_ UI_SIZE *Value) override
    {
        return GetSize(SM_CXVSCROLL, SM_CYHSCROLL, Value);
    }

    STDMETHODIMP
    get_ScrollBarArrowSize(
        _Out_ UI_SIZE *Value) override
    {
        return GetSize(SM_CXHSCROLL, SM_CYVSCROLL, Value);
    }

    STDMETHODIMP
    get_ScrollBarThumbBoxSize(
        _Out_ UI_SIZE *Value) override
    {
        return GetSize(SM_CXHTHUMB, SM_CYVTHUMB, Value);
    }

    STDMETHODIMP
    get_MessageDuration(
        _Out_ UINT32 *Value) override
    {
        ULONG Seconds = 5;

        if (!Value)
            return E_POINTER;
        SystemParametersInfoW(SPI_GETMESSAGEDURATION, 0, &Seconds, 0);
        *Value = Seconds;
        return S_OK;
    }

    STDMETHODIMP
    get_AnimationsEnabled(
        _Out_ BOOLEAN *Value) override
    {
        BOOL Enabled = TRUE;

        if (!Value)
            return E_POINTER;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &Enabled, 0);
        *Value = Enabled ? TRUE : FALSE;
        return S_OK;
    }

    STDMETHODIMP
    get_CaretBrowsingEnabled(
        _Out_ BOOLEAN *Value) override
    {
        if (!Value)
            return E_POINTER;
        *Value = FALSE;
        return S_OK;
    }

    STDMETHODIMP
    get_CaretBlinkRate(
        _Out_ UINT32 *Value) override
    {
        if (!Value)
            return E_POINTER;
        *Value = GetCaretBlinkTime();
        return S_OK;
    }

    STDMETHODIMP
    get_CaretWidth(
        _Out_ UINT32 *Value) override
    {
        DWORD Width = 1;

        if (!Value)
            return E_POINTER;
        SystemParametersInfoW(SPI_GETCARETWIDTH, 0, &Width, 0);
        *Value = Width;
        return S_OK;
    }

    STDMETHODIMP
    get_DoubleClickTime(
        _Out_ UINT32 *Value) override
    {
        if (!Value)
            return E_POINTER;
        *Value = GetDoubleClickTime();
        return S_OK;
    }

    STDMETHODIMP
    get_MouseHoverTime(
        _Out_ UINT32 *Value) override
    {
        UINT Time = HOVER_DEFAULT;

        if (!Value)
            return E_POINTER;
        SystemParametersInfoW(SPI_GETMOUSEHOVERTIME, 0, &Time, 0);
        *Value = Time;
        return S_OK;
    }

    /* The classic element types are the system colors, in the order of the enumeration */
    STDMETHODIMP
    UIElementColor(
        _In_ INT32 Element,
        _Out_ UI_COLOR *Value) override
    {
        static const INT SystemColors[] =
        {
            COLOR_ACTIVECAPTION, COLOR_BACKGROUND, COLOR_BTNFACE, COLOR_BTNTEXT, COLOR_CAPTIONTEXT,
            COLOR_GRAYTEXT, COLOR_HIGHLIGHT, COLOR_HIGHLIGHTTEXT, COLOR_HOTLIGHT, COLOR_INACTIVECAPTION,
            COLOR_INACTIVECAPTIONTEXT, COLOR_WINDOW, COLOR_WINDOWTEXT
        };

        if (!Value)
            return E_POINTER;
        if (Element >= 0 && Element <= UI_ELEMENT_LAST_SYSTEM_COLOR)
            *Value = UiColorFromColorref(GetSysColor(SystemColors[Element]));
        else if (Element == UI_ELEMENT_ACCENT_COLOR)
            *Value = UiAccentColor(UiColorAccent);
        else
            return E_INVALIDARG;
        return S_OK;
    }

    /* IUISettings2 */
    STDMETHODIMP
    get_TextScaleFactor(
        _Out_ DOUBLE *Value) override
    {
        DWORD Percent;

        if (!Value)
            return E_POINTER;
        *Value = 1.0;
        if (UiReadDword(L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", &Percent) && Percent >= 100 && Percent <= 225)
            *Value = Percent / 100.0;
        return S_OK;
    }

    STDMETHODIMP
    add_TextScaleFactorChanged(
        _In_ IUnknown *Handler,
        _Out_ UI_EVENT_TOKEN *Cookie) override
    {
        return AddHandler(Handler, Cookie);
    }

    STDMETHODIMP
    remove_TextScaleFactorChanged(
        _In_ UI_EVENT_TOKEN Cookie) override
    {
        UNREFERENCED_PARAMETER(Cookie);
        return S_OK;
    }

    /* IUISettings3 */
    STDMETHODIMP
    GetColorValue(
        _In_ INT32 Type,
        _Out_ UI_COLOR *Value) override
    {
        static const UI_COLOR White = { 0xFF, 0xFF, 0xFF, 0xFF }, Black = { 0xFF, 0, 0, 0 };

        if (!Value)
            return E_POINTER;

        switch (Type)
        {
            case UiColorBackground:
                *Value = UiAppsUseDarkTheme() ? Black : White;
                return S_OK;
            case UiColorForeground:
                *Value = UiAppsUseDarkTheme() ? White : Black;
                return S_OK;
            case UiColorAccentDark3:
            case UiColorAccentDark2:
            case UiColorAccentDark1:
            case UiColorAccent:
            case UiColorAccentLight1:
            case UiColorAccentLight2:
            case UiColorAccentLight3:
                *Value = UiAccentColor(Type);
                return S_OK;
            case UiColorComplement:
                /* Windows gives no complement, all four bytes are zero */
                ZeroMemory(Value, sizeof(*Value));
                return S_OK;
            default:
                return E_INVALIDARG;
        }
    }

    STDMETHODIMP
    add_ColorValuesChanged(
        _In_ IUnknown *Handler,
        _Out_ UI_EVENT_TOKEN *Cookie) override
    {
        return AddHandler(Handler, Cookie);
    }

    STDMETHODIMP
    remove_ColorValuesChanged(
        _In_ UI_EVENT_TOKEN Cookie) override
    {
        UNREFERENCED_PARAMETER(Cookie);
        return S_OK;
    }

private:
    LONG m_Refs;

    static
    HRESULT
    GetSize(
        _In_ INT XMetric,
        _In_ INT YMetric,
        _Out_ UI_SIZE *Value)
    {
        if (!Value)
            return E_POINTER;
        Value->Width = (FLOAT)GetSystemMetrics(XMetric);
        Value->Height = (FLOAT)GetSystemMetrics(YMetric);
        return S_OK;
    }

    /* The settings do not change under a running app here, so a handler is never called */
    static
    HRESULT
    AddHandler(
        _In_ IUnknown *Handler,
        _Out_ UI_EVENT_TOKEN *Cookie)
    {
        if (!Handler || !Cookie)
            return E_POINTER;
        Cookie->value = InterlockedIncrement64(&UiNextToken);
        return S_OK;
    }
};

class CUISettingsFactory : public IActivationFactory
{
public:
    STDMETHODIMP
    QueryInterface(
        _In_ REFIID Riid,
        _Outptr_ VOID **Object) override
    {
        if (!Object)
            return E_POINTER;
        if (!IsEqualIID(Riid, IID_IUnknown) && !IsEqualIID(Riid, UiIidInspectable) &&
            !IsEqualIID(Riid, UiIidActivationFactory) && !IsEqualIID(Riid, UiIidAgileObject))
        {
            *Object = NULL;
            return E_NOINTERFACE;
        }
        *Object = static_cast<IActivationFactory *>(this);
        return S_OK;
    }

    /* The factory is a single static object, alive as long as the module */
    STDMETHODIMP_(ULONG)
    AddRef() override
    {
        return 2;
    }

    STDMETHODIMP_(ULONG)
    Release() override
    {
        return 1;
    }

    STDMETHODIMP
    GetIids(
        _Out_ ULONG *Count,
        _Outptr_ IID **Iids) override
    {
        if (!Count || !Iids)
            return E_POINTER;
        *Count = 0;
        *Iids = NULL;
        return S_OK;
    }

    STDMETHODIMP
    GetRuntimeClassName(
        _Out_ HSTRING *ClassName) override
    {
        if (!ClassName)
            return E_POINTER;
        return WindowsCreateString(UI_SETTINGS_CLASS, ARRAYSIZE(UI_SETTINGS_CLASS) - 1, ClassName);
    }

    STDMETHODIMP
    GetTrustLevel(
        _Out_ TrustLevel *Level) override
    {
        if (!Level)
            return E_POINTER;
        *Level = BaseTrust;
        return S_OK;
    }

    STDMETHODIMP
    ActivateInstance(
        _Outptr_ IInspectable **Instance) override
    {
        CUISettings *Settings;

        if (!Instance)
            return E_POINTER;
        Settings = new (std::nothrow) CUISettings();
        if (!Settings)
        {
            *Instance = NULL;
            return E_OUTOFMEMORY;
        }
        *Instance = static_cast<IUISettings *>(Settings);
        return S_OK;
    }
};

static CUISettingsFactory UiSettingsFactory;

/**
 * @brief
 * Hands out the factory of a runtime class this module implements.
 */
EXTERN_C
HRESULT
WINAPI
DllGetActivationFactory(
    _In_ HSTRING ClassId,
    _Outptr_ IActivationFactory **Factory)
{
    if (!Factory)
        return E_POINTER;
    *Factory = NULL;

    if (wcscmp(WindowsGetStringRawBuffer(ClassId, NULL), UI_SETTINGS_CLASS))
        return CLASS_E_CLASSNOTAVAILABLE;

    *Factory = &UiSettingsFactory;
    return S_OK;
}

EXTERN_C
HRESULT
WINAPI
DllCanUnloadNow(VOID)
{
    return UiObjectCount ? S_FALSE : S_OK;
}
