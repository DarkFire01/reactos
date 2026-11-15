/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Shell host: window, boot order, co-render frame loop, input and key mapping
 */

#include <shellcomp.h>
#include <uixptr.h>
#include <uixxml.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <strsafe.h>
#include <math.h>
#include <string>
#include <vector>

#define WM_APP_FRAME     (WM_APP + 1)
#define WM_APP_DISPATCH  (WM_APP + 2)
#define WM_APP_COMPONENT (WM_APP + 3)

#define TIMER_TICK   1
#define TIMER_SCRIPT 2
#define TIMER_HOLD_HINT 3
#define TIMER_HOLD 4

/* The dots appear shortly after the press and take about half a second to circle */
#define HOLD_HINT_MS 150
#define HOLD_MS 650

typedef struct _SHELL_KEY
{
    UINT VirtualKey;
    std::wstring Command;
} SHELL_KEY;

typedef struct _SHELL_THEME
{
    std::wstring Name;
    std::wstring File;
} SHELL_THEME;

typedef struct _SHELL_MODULE
{
    HMODULE Module;
    IShellComponent *Component;
} SHELL_MODULE;

class CShellHost : public IShellHost
{
public:
    CShellHost();

    HRESULT
    Run(
        _In_ INT ShowCommand);

    /* IShellHost */
    IMessageSession *STDMETHODCALLTYPE GetSession(VOID) override
    {
        return m_Session.Get();
    }

    IUixEngine *STDMETHODCALLTYPE GetEngine(VOID) override
    {
        return m_Engine.Get();
    }

    IDCompositionVisual *STDMETHODCALLTYPE GetBand(
        _In_ SHELL_BAND Band) override
    {
        return (Band < ShellBandCount) ? m_Bands[Band].Get() : NULL;
    }

    BOOL STDMETHODCALLTYPE GetSkinFile(
        _In_z_ PCWSTR Name,
        _Out_writes_z_(Count) PWSTR Path,
        _In_ SIZE_T Count) override;

    PCWSTR STDMETHODCALLTYPE GetThemeFile(VOID) override
    {
        return m_ThemeFile.c_str();
    }

    PCWSTR STDMETHODCALLTYPE GetConfigFile(VOID) override
    {
        return m_ConfigFile.c_str();
    }

    PCWSTR STDMETHODCALLTYPE GetBinDirectory(VOID) override
    {
        return m_BinDirectory.c_str();
    }

    VOID STDMETHODCALLTYPE GetCanvas(
        _Out_ FLOAT *Width,
        _Out_ FLOAT *Height) override
    {
        *Width = m_CanvasWidth;
        *Height = m_CanvasHeight;
    }

    VOID STDMETHODCALLTYPE RequestFrame(VOID) override;

    VOID STDMETHODCALLTYPE ExecuteCommand(
        _In_z_ PCWSTR Command) override;

    BOOL STDMETHODCALLTYPE PostUiMessage(
        _In_ IShellComponent *Component,
        _In_ ULONG Code,
        _In_opt_ PVOID Data) override;

    /* Command line */
    std::wstring m_Script;
    std::wstring m_InitialTheme;
    FLOAT m_InitialScale;
    BOOL m_Windowed = FALSE;

private:
    static
    LRESULT
    CALLBACK
    WindowProc(
        _In_ HWND Window,
        _In_ UINT Message,
        _In_ WPARAM WParam,
        _In_ LPARAM LParam);

    static
    VOID
    CALLBACK
    Wake(
        _In_opt_ PVOID Context);

    LRESULT
    HandleMessage(
        _In_ UINT Message,
        _In_ WPARAM WParam,
        _In_ LPARAM LParam);

    HRESULT
    LoadConfig(VOID);

    HRESULT
    LoadSkin(VOID);

    HRESULT
    CreateBands(VOID);

    HRESULT
    LoadComponents(VOID);

    VOID
    Resize(VOID);

    VOID
    Frame(VOID);

    VOID
    Pointer(
        _In_ UIX_POINTER Kind,
        _In_ LONG X,
        _In_ LONG Y);

    VOID
    EndHold(VOID);

    VOID
    HoldTimer(
        _In_ UINT_PTR Timer);

    VOID
    Wheel(
        _In_ LONG X,
        _In_ LONG Y,
        _In_ FLOAT Delta);

    VOID
    KeyDown(
        _In_ UINT VirtualKey);

    BOOL
    SelectTheme(
        _In_z_ PCWSTR Name);

    VOID
    ReloadAll(VOID);

    VOID
    ScriptStep(VOID);

    HRESULT
    Capture(
        _In_z_ PCWSTR Path);

    HWND m_Window;
    UixPtr<IMessageSession> m_Session;
    UixPtr<IUixEngine> m_Engine;
    UixPtr<IDCompositionVisual> m_Root;
    UixPtr<IDCompositionVisual> m_Backdrop;
    UixPtr<IDCompositionSurface> m_BackdropSurface;
    UixPtr<IDCompositionVisual> m_Bands[ShellBandCount];
    std::vector<SHELL_MODULE> m_Modules;
    std::vector<SHELL_KEY> m_Keys;
    std::vector<SHELL_THEME> m_Themes;
    std::wstring m_BinDirectory;
    std::wstring m_ConfigFile;
    std::wstring m_SkinDirectory;
    std::wstring m_ThemeName;
    std::wstring m_ThemeFile;
    FLOAT m_CanvasWidth;
    FLOAT m_CanvasHeight;
    FLOAT m_SkinWidth = 240.0f;
    FLOAT m_SkinHeight = 320.0f;
    BOOL m_FullScreen = FALSE;
    LONG m_OriginX;
    LONG m_OriginY;
    IShellComponent *m_Capture;
    LONG m_DownX = 0;
    LONG m_DownY = 0;
    BOOL m_Holding = FALSE;
    BOOL m_HoldHint = FALSE;
    BOOL m_FramePending;
    SIZE_T m_ScriptCursor;
};

static
const struct
{
    PCWSTR Name;
    UINT VirtualKey;
} g_KeyNames[] =
{
    { L"F1", VK_F1 }, { L"F2", VK_F2 }, { L"F3", VK_F3 }, { L"F4", VK_F4 },
    { L"F5", VK_F5 }, { L"F6", VK_F6 }, { L"F7", VK_F7 }, { L"F8", VK_F8 },
    { L"F9", VK_F9 }, { L"F10", VK_F10 }, { L"F11", VK_F11 }, { L"F12", VK_F12 },
    { L"Escape", VK_ESCAPE }, { L"Back", VK_BACK }, { L"Enter", VK_RETURN },
    { L"Up", VK_UP }, { L"Down", VK_DOWN }, { L"Left", VK_LEFT }, { L"Right", VK_RIGHT },
    { L"Home", VK_HOME }, { L"End", VK_END }, { L"LWin", VK_LWIN }, { L"Space", VK_SPACE },
    { L"Tab", VK_TAB }
};

static
UINT
ParseKey(
    _In_z_ PCWSTR Name)
{
    for (SIZE_T Index = 0; Index < ARRAYSIZE(g_KeyNames); Index++)
    {
        if (_wcsicmp(g_KeyNames[Index].Name, Name) == 0)
            return g_KeyNames[Index].VirtualKey;
    }

    if (Name[0] && !Name[1])
        return towupper(Name[0]);
    return 0;
}

static
std::wstring
JoinPath(
    _In_ const std::wstring &Directory,
    _In_z_ PCWSTR Relative)
{
    WCHAR Full[MAX_PATH];
    std::wstring Joined = PathIsRelativeW(Relative) ? (Directory + L"\\" + Relative) : std::wstring(Relative);

    if (GetFullPathNameW(Joined.c_str(), MAX_PATH, Full, NULL))
        return Full;
    return Joined;
}

CShellHost::CShellHost()
    : m_InitialScale(2.0f),
      m_Window(NULL),
      m_CanvasWidth(240.0f),
      m_CanvasHeight(320.0f),
      m_OriginX(0),
      m_OriginY(0),
      m_Capture(NULL),
      m_FramePending(FALSE),
      m_ScriptCursor(0)
{
}

BOOL
STDMETHODCALLTYPE
CShellHost::GetSkinFile(
    _In_z_ PCWSTR Name,
    _Out_writes_z_(Count) PWSTR Path,
    _In_ SIZE_T Count)
{
    std::wstring Full = Name[0] ? JoinPath(m_SkinDirectory, Name) : m_SkinDirectory;

    if (FAILED(StringCchCopyW(Path, Count, Full.c_str())))
        return FALSE;
    return GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}

HRESULT
CShellHost::LoadConfig(VOID)
{
    UIX_XML_NODE Root;
    HRESULT Status;

    Status = UixXmlLoad(m_ConfigFile.c_str(), &Root);
    if (FAILED(Status))
        return Status;

    m_SkinDirectory = JoinPath(m_BinDirectory, Root.Attr(L"Skin", L"skins\\WM61"));
    if (m_InitialTheme.empty())
        m_InitialTheme = Root.Attr(L"Theme", L"");

    /* A phone fills its screen; --windowed keeps a desktop window for testing */
    m_FullScreen = !m_Windowed && _wcsicmp(Root.Attr(L"FullScreen", L"false"), L"true") == 0;

    m_Keys.clear();
    for (const UIX_XML_NODE &Section : Root.Children)
    {
        if (Section.Name != L"Keys")
            continue;

        for (const UIX_XML_NODE &Node : Section.Children)
        {
            SHELL_KEY Key;

            if (Node.Name != L"Key" || !Node.Attr(L"Vk") || !Node.Attr(L"Command"))
                continue;
            Key.VirtualKey = ParseKey(Node.Attr(L"Vk"));
            Key.Command = Node.Attr(L"Command");
            if (Key.VirtualKey)
                m_Keys.push_back(Key);
        }
    }
    return S_OK;
}

HRESULT
CShellHost::LoadSkin(VOID)
{
    UIX_XML_NODE Root;
    HRESULT Status;

    Status = UixXmlLoad(JoinPath(m_SkinDirectory, L"skin.xml").c_str(), &Root);
    if (FAILED(Status))
        return Status;

    m_CanvasWidth = static_cast<FLOAT>(_wtof(Root.Attr(L"Width", L"240")));
    m_CanvasHeight = static_cast<FLOAT>(_wtof(Root.Attr(L"Height", L"320")));
    if (m_CanvasWidth <= 0.0f || m_CanvasHeight <= 0.0f)
        return E_INVALIDARG;
    m_SkinWidth = m_CanvasWidth;
    m_SkinHeight = m_CanvasHeight;

    m_Themes.clear();
    for (const UIX_XML_NODE &Node : Root.Children)
    {
        SHELL_THEME Theme;

        if (Node.Name != L"Theme" || !Node.Attr(L"Name") || !Node.Attr(L"File"))
            continue;
        Theme.Name = Node.Attr(L"Name");
        Theme.File = JoinPath(m_SkinDirectory, Node.Attr(L"File"));
        m_Themes.push_back(Theme);
    }

    if (m_Themes.empty())
        return E_FAIL;

    if (m_InitialTheme.empty() || !SelectTheme(m_InitialTheme.c_str()))
    {
        if (!SelectTheme(Root.Attr(L"DefaultTheme", m_Themes[0].Name.c_str())))
            return SelectTheme(m_Themes[0].Name.c_str()) ? S_OK : E_FAIL;
    }
    return S_OK;
}

BOOL
CShellHost::SelectTheme(
    _In_z_ PCWSTR Name)
{
    for (const SHELL_THEME &Theme : m_Themes)
    {
        if (_wcsicmp(Theme.Name.c_str(), Name) == 0 && SUCCEEDED(m_Engine->LoadTheme(Theme.File.c_str())))
        {
            m_ThemeName = Theme.Name;
            m_ThemeFile = Theme.File;
            return TRUE;
        }
    }
    return FALSE;
}

HRESULT
CShellHost::CreateBands(VOID)
{
    UixPtr<IDCompositionDevice> Device;
    UixPtr<ID3D11Device> Direct3D;
    UixPtr<ID3D11Texture2D> Black;
    D3D11_TEXTURE2D_DESC Desc;
    D3D11_SUBRESOURCE_DATA Data;
    IDCompositionVisual *Previous = NULL;
    ULONG Pixel = 0xFF000000;
    HRESULT Status;
    ULONG Band;

    Status = m_Engine->GetCompositionDevice(Device.Put());
    if (FAILED(Status))
        return Status;
    Status = m_Engine->GetRootVisual(m_Root.Put());
    if (FAILED(Status))
        return Status;

    /* A one pixel black surface stretched under everything gives the letterbox */
    ZeroMemory(&Desc, sizeof(Desc));
    Desc.Width = 1;
    Desc.Height = 1;
    Desc.MipLevels = 1;
    Desc.ArraySize = 1;
    Desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Desc.SampleDesc.Count = 1;
    Desc.Usage = D3D11_USAGE_DEFAULT;
    Data.pSysMem = &Pixel;
    Data.SysMemPitch = sizeof(Pixel);
    Data.SysMemSlicePitch = 0;

    Status = m_Engine->GetDirect3DDevice(Direct3D.Put());
    if (SUCCEEDED(Status))
        Status = Direct3D->CreateTexture2D(&Desc, &Data, Black.Put());
    if (SUCCEEDED(Status))
        Status = Device->CreateSurface(1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_IGNORE, m_BackdropSurface.Put());
    if (SUCCEEDED(Status))
        Status = m_Engine->CopyToSurface(m_BackdropSurface.Get(), Black.Get(), 1, 1);
    if (SUCCEEDED(Status))
        Status = Device->CreateVisual(m_Backdrop.Put());
    if (FAILED(Status))
        return Status;

    m_Backdrop->SetContent(m_BackdropSurface.Get());
    m_Root->AddVisual(m_Backdrop.Get(), FALSE, NULL);
    Previous = m_Backdrop.Get();

    for (Band = 0; Band < ShellBandCount; Band++)
    {
        Status = Device->CreateVisual(m_Bands[Band].Put());
        if (FAILED(Status))
            return Status;
        Status = m_Root->AddVisual(m_Bands[Band].Get(), TRUE, Previous);
        if (FAILED(Status))
            return Status;
        Previous = m_Bands[Band].Get();
    }
    return S_OK;
}

HRESULT
CShellHost::LoadComponents(VOID)
{
    /* Boot order matters: the chrome server publishes its endpoint before MobileUI talks to it */
    static PCWSTR const Names[] = { L"ShellChrome.dll", L"MobileUI.dll" };
    PSHELL_CREATE_COMPONENT Create;
    SHELL_MODULE Module;
    HRESULT Status;

    for (SIZE_T Index = 0; Index < ARRAYSIZE(Names); Index++)
    {
        Module.Module = LoadLibraryW(JoinPath(m_BinDirectory, Names[Index]).c_str());
        if (!Module.Module)
            return HRESULT_FROM_WIN32(GetLastError());

        Create = reinterpret_cast<PSHELL_CREATE_COMPONENT>(GetProcAddress(Module.Module, SHELL_CREATE_COMPONENT_NAME));
        if (!Create)
            return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

        Status = Create(&Module.Component);
        if (FAILED(Status))
            return Status;

        Status = Module.Component->Initialize(this);
        if (FAILED(Status))
            return Status;

        m_Modules.push_back(Module);
    }
    return S_OK;
}

VOID
STDMETHODCALLTYPE
CShellHost::RequestFrame(VOID)
{
    if (!m_FramePending && m_Window)
    {
        m_FramePending = TRUE;
        PostMessageW(m_Window, WM_APP_FRAME, 0, 0);
    }
}

BOOL
STDMETHODCALLTYPE
CShellHost::PostUiMessage(
    _In_ IShellComponent *Component,
    _In_ ULONG Code,
    _In_opt_ PVOID Data)
{
    /* WParam carries Code, LParam a heap pair of Component and Data */
    PVOID *Packet = new PVOID[2];

    Packet[0] = Component;
    Packet[1] = Data;
    if (!PostMessageW(m_Window, WM_APP_COMPONENT, Code, reinterpret_cast<LPARAM>(Packet)))
    {
        delete[] Packet;
        return FALSE;
    }
    return TRUE;
}

VOID
CALLBACK
CShellHost::Wake(
    _In_opt_ PVOID Context)
{
    CShellHost *This = static_cast<CShellHost *>(Context);

    PostMessageW(This->m_Window, WM_APP_DISPATCH, 0, 0);
}

VOID
CShellHost::Frame(VOID)
{
    m_FramePending = FALSE;

    /* Co-render order, the host stands where the system compositor would */
    m_Engine->PreRender();
    if (FAILED(m_Engine->BeginRender()))
        return;
    m_Engine->Render();
    m_Engine->EndRender();
    m_Engine->PostPresent();
}

VOID
CShellHost::Resize(VOID)
{
    RECT Client;
    FLOAT Scale;
    LONG Width;
    LONG Height;

    if (!m_Backdrop)
        return;

    GetClientRect(m_Window, &Client);
    Width = Client.right - Client.left;
    Height = Client.bottom - Client.top;
    if (Width <= 0 || Height <= 0)
        return;

    /*
     * Full screen keeps the skin's logical width and gives it the screen's full height, so a tall
     * phone gets a tall canvas instead of bars. A window, or a screen shorter than the skin, letterboxes.
     */
    m_CanvasWidth = m_SkinWidth;
    m_CanvasHeight = m_SkinHeight;
    Scale = min(Width / m_SkinWidth, Height / m_SkinHeight);
    if (m_FullScreen && Height / (Width / m_SkinWidth) >= m_SkinHeight)
    {
        Scale = Width / m_SkinWidth;
        m_CanvasHeight = Height / Scale;
    }
    m_OriginX = (Width - lroundf(m_CanvasWidth * Scale)) / 2;
    m_OriginY = (Height - lroundf(m_CanvasHeight * Scale)) / 2;

    m_Engine->SetScale(Scale);
    m_Backdrop->SetTransform(UixD2D::Scale(static_cast<FLOAT>(Width), static_cast<FLOAT>(Height)));
    for (ULONG Band = 0; Band < ShellBandCount; Band++)
    {
        m_Bands[Band]->SetOffsetX(static_cast<FLOAT>(m_OriginX));
        m_Bands[Band]->SetOffsetY(static_cast<FLOAT>(m_OriginY));
    }

    for (const SHELL_MODULE &Module : m_Modules)
        Module.Component->Layout();
    RequestFrame();
}

VOID
CShellHost::Pointer(
    _In_ UIX_POINTER Kind,
    _In_ LONG X,
    _In_ LONG Y)
{
    FLOAT Scale = m_Engine->GetScale();
    FLOAT LogicalX = (X - m_OriginX) / Scale;
    FLOAT LogicalY = (Y - m_OriginY) / Scale;
    SIZE_T Index;

    if (Kind != UixPointerDown)
    {
        /* Moving past the slop or letting go ends the tap and hold wait */
        if (Kind != UixPointerMove || labs(X - m_DownX) + labs(Y - m_DownY) > lroundf(6.0f * Scale))
            EndHold();

        if (m_Capture)
            m_Capture->PointerEvent(Kind, LogicalX, LogicalY);
        if (Kind == UixPointerUp || Kind == UixPointerCancel)
            m_Capture = NULL;
        return;
    }

    /* Topmost component first: MobileUI owns the overlay band */
    EndHold();
    m_Capture = NULL;
    for (Index = m_Modules.size(); Index > 0; Index -= 1)
    {
        if (m_Modules[Index - 1].Component->PointerEvent(Kind, LogicalX, LogicalY))
        {
            m_Capture = m_Modules[Index - 1].Component;
            break;
        }
    }

    if (m_Capture)
    {
        m_DownX = X;
        m_DownY = Y;
        m_Holding = TRUE;
        SetTimer(m_Window, TIMER_HOLD_HINT, HOLD_HINT_MS, NULL);
        SetTimer(m_Window, TIMER_HOLD, HOLD_MS, NULL);
    }
}

VOID
CShellHost::EndHold(VOID)
{
    if (!m_Holding)
        return;

    m_Holding = FALSE;
    KillTimer(m_Window, TIMER_HOLD_HINT);
    KillTimer(m_Window, TIMER_HOLD);
    if (m_HoldHint)
    {
        m_HoldHint = FALSE;
        ExecuteCommand(L"hold.end");
    }
}

VOID
CShellHost::HoldTimer(
    _In_ UINT_PTR Timer)
{
    FLOAT Scale = m_Engine->GetScale();
    FLOAT LogicalX = (m_DownX - m_OriginX) / Scale;
    FLOAT LogicalY = (m_DownY - m_OriginY) / Scale;
    WCHAR Command[48];

    if (!m_Holding)
        return;

    if (Timer == TIMER_HOLD_HINT)
    {
        /* The circling dots, the menu follows when they complete */
        KillTimer(m_Window, TIMER_HOLD_HINT);
        m_HoldHint = TRUE;
        StringCchPrintfW(Command, ARRAYSIZE(Command), L"hold.hint:%.1f,%.1f", LogicalX, LogicalY);
        ExecuteCommand(Command);
        return;
    }

    EndHold();
    if (m_Capture)
        m_Capture->PointerEvent(UixPointerHold, LogicalX, LogicalY);
}

VOID
CShellHost::Wheel(
    _In_ LONG X,
    _In_ LONG Y,
    _In_ FLOAT Delta)
{
    FLOAT Scale = m_Engine->GetScale();
    SIZE_T Index;

    for (Index = m_Modules.size(); Index > 0; Index -= 1)
    {
        if (m_Modules[Index - 1].Component->Wheel((X - m_OriginX) / Scale, (Y - m_OriginY) / Scale, Delta))
            break;
    }
}

VOID
CShellHost::KeyDown(
    _In_ UINT VirtualKey)
{
    WCHAR Command[16];

    for (const SHELL_KEY &Key : m_Keys)
    {
        if (Key.VirtualKey == VirtualKey)
        {
            ExecuteCommand(Key.Command.c_str());
            return;
        }
    }

    StringCchPrintfW(Command, ARRAYSIZE(Command), L"key:%u", VirtualKey);
    ExecuteCommand(Command);
}

VOID
CShellHost::ReloadAll(VOID)
{
    LoadConfig();
    for (const SHELL_MODULE &Module : m_Modules)
        Module.Component->Reload();
    Resize();
}

VOID
STDMETHODCALLTYPE
CShellHost::ExecuteCommand(
    _In_z_ PCWSTR Command)
{
    SIZE_T Index;

    if (_wcsicmp(Command, L"exit") == 0)
    {
        DestroyWindow(m_Window);
        return;
    }

    if (_wcsicmp(Command, L"reload") == 0)
    {
        ReloadAll();
        return;
    }

    if (_wcsicmp(Command, L"theme.next") == 0)
    {
        for (Index = 0; Index < m_Themes.size(); Index++)
        {
            if (m_Themes[Index].Name == m_ThemeName)
                break;
        }
        Command = m_Themes[(Index + 1) % m_Themes.size()].Name.c_str();
        if (SelectTheme(Command))
            ReloadAll();
        return;
    }

    if (_wcsnicmp(Command, L"theme:", 6) == 0)
    {
        if (SelectTheme(Command + 6))
            ReloadAll();
        return;
    }

    for (const SHELL_MODULE &Module : m_Modules)
    {
        if (Module.Component->Command(Command))
            return;
    }
}

HRESULT
CShellHost::Capture(
    _In_z_ PCWSTR Path)
{
    UixPtr<IWICImagingFactory> Factory;
    UixPtr<IWICBitmap> Bitmap;
    UixPtr<IWICStream> Stream;
    UixPtr<IWICBitmapEncoder> Encoder;
    UixPtr<IWICBitmapFrameEncode> Frame;
    WICPixelFormatGUID Format = GUID_WICPixelFormat32bppBGR;
    POINT Origin = { 0, 0 };
    RECT Client;
    HBITMAP Section;
    HDC Screen;
    HDC Memory;
    HGDIOBJ Old;
    HRESULT Status;
    LONG Width;
    LONG Height;

    /* The composed frame only exists on screen, so read it back from there */
    DwmFlush();
    GetClientRect(m_Window, &Client);
    ClientToScreen(m_Window, &Origin);
    Width = Client.right;
    Height = Client.bottom;

    Screen = GetDC(NULL);
    Memory = CreateCompatibleDC(Screen);
    Section = CreateCompatibleBitmap(Screen, Width, Height);
    Old = SelectObject(Memory, Section);
    BitBlt(Memory, 0, 0, Width, Height, Screen, Origin.x, Origin.y, SRCCOPY | CAPTUREBLT);
    SelectObject(Memory, Old);
    DeleteDC(Memory);
    ReleaseDC(NULL, Screen);

    Status = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), Factory.PutVoid());
    if (SUCCEEDED(Status))
        Status = Factory->CreateBitmapFromHBITMAP(Section, NULL, WICBitmapIgnoreAlpha, Bitmap.Put());
    if (SUCCEEDED(Status))
        Status = Factory->CreateStream(Stream.Put());
    if (SUCCEEDED(Status))
        Status = Stream->InitializeFromFilename(Path, GENERIC_WRITE);
    if (SUCCEEDED(Status))
        Status = Factory->CreateEncoder(GUID_ContainerFormatPng, NULL, Encoder.Put());
    if (SUCCEEDED(Status))
        Status = Encoder->Initialize(Stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(Status))
        Status = Encoder->CreateNewFrame(Frame.Put(), NULL);
    if (SUCCEEDED(Status))
        Status = Frame->Initialize(NULL);
    if (SUCCEEDED(Status))
        Status = Frame->SetSize(Width, Height);
    if (SUCCEEDED(Status))
        Status = Frame->SetPixelFormat(&Format);
    if (SUCCEEDED(Status))
        Status = Frame->WriteSource(Bitmap.Get(), NULL);
    if (SUCCEEDED(Status))
        Status = Frame->Commit();
    if (SUCCEEDED(Status))
        Status = Encoder->Commit();

    DeleteObject(Section);
    return Status;
}

VOID
CShellHost::ScriptStep(VOID)
{
    std::wstring Step;
    SIZE_T End;
    FLOAT X;
    FLOAT Y;
    FLOAT Scale;

    if (m_ScriptCursor >= m_Script.size())
    {
        KillTimer(m_Window, TIMER_SCRIPT);
        return;
    }

    End = m_Script.find(L';', m_ScriptCursor);
    if (End == std::wstring::npos)
        End = m_Script.size();
    Step = m_Script.substr(m_ScriptCursor, End - m_ScriptCursor);
    m_ScriptCursor = End + 1;

    if (_wcsnicmp(Step.c_str(), L"tap:", 4) == 0 && swscanf(Step.c_str() + 4, L"%f,%f", &X, &Y) == 2)
    {
        Scale = m_Engine->GetScale();
        Pointer(UixPointerDown, lroundf(X * Scale) + m_OriginX, lroundf(Y * Scale) + m_OriginY);
        Pointer(UixPointerUp, lroundf(X * Scale) + m_OriginX, lroundf(Y * Scale) + m_OriginY);
    }
    else if ((_wcsnicmp(Step.c_str(), L"press:", 6) == 0 || _wcsnicmp(Step.c_str(), L"release:", 8) == 0) &&
             swscanf(wcschr(Step.c_str(), L':') + 1, L"%f,%f", &X, &Y) == 2)
    {
        /* Split taps let the hold timers run between steps */
        Scale = m_Engine->GetScale();
        Pointer((Step[0] == L'p') ? UixPointerDown : UixPointerUp,
                lroundf(X * Scale) + m_OriginX, lroundf(Y * Scale) + m_OriginY);
    }
    else if (_wcsnicmp(Step.c_str(), L"wheel:", 6) == 0)
    {
        FLOAT Delta;

        Scale = m_Engine->GetScale();
        if (swscanf(Step.c_str() + 6, L"%f,%f,%f", &X, &Y, &Delta) == 3)
            Wheel(lroundf(X * Scale) + m_OriginX, lroundf(Y * Scale) + m_OriginY, Delta);
    }
    else if (_wcsnicmp(Step.c_str(), L"type:", 5) == 0)
    {
        WCHAR Command[16];

        for (SIZE_T Index = 5; Index < Step.size(); Index++)
        {
            StringCchPrintfW(Command, ARRAYSIZE(Command), L"char:%u", static_cast<UINT>(Step[Index]));
            ExecuteCommand(Command);
        }
    }
    else if (_wcsnicmp(Step.c_str(), L"key:", 4) == 0)
    {
        KeyDown(ParseKey(Step.c_str() + 4));
    }
    else if (_wcsnicmp(Step.c_str(), L"shot:", 5) == 0)
    {
        Capture(Step.c_str() + 5);
    }
    else if (_wcsicmp(Step.c_str(), L"wait") != 0 && !Step.empty())
    {
        ExecuteCommand(Step.c_str());
    }
}

LRESULT
CShellHost::HandleMessage(
    _In_ UINT Message,
    _In_ WPARAM WParam,
    _In_ LPARAM LParam)
{
    switch (Message)
    {
        case WM_APP_FRAME:
            Frame();
            return 0;

        case WM_APP_DISPATCH:
            m_Session->Dispatch();
            return 0;

        case WM_APP_COMPONENT:
        {
            PVOID *Packet = reinterpret_cast<PVOID *>(LParam);
            IShellComponent *Component = static_cast<IShellComponent *>(Packet[0]);

            for (const SHELL_MODULE &Module : m_Modules)
            {
                if (Module.Component == Component)
                {
                    Component->UiMessage(static_cast<ULONG>(WParam), Packet[1]);
                    Packet[1] = NULL;
                    break;
                }
            }
            delete[] Packet;
            return 0;
        }

        case WM_SIZE:
            Resize();
            return 0;

        case WM_DPICHANGED:
        {
            const RECT *Suggested = reinterpret_cast<const RECT *>(LParam);

            /* Full screen already covers the monitor; only a window follows the suggested size */
            if (m_FullScreen)
            {
                Resize();
                return 0;
            }
            SetWindowPos(m_Window, NULL, Suggested->left, Suggested->top, Suggested->right - Suggested->left,
                         Suggested->bottom - Suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_TIMER:
            if (WParam == TIMER_TICK)
            {
                for (const SHELL_MODULE &Module : m_Modules)
                    Module.Component->Tick();
            }
            else if (WParam == TIMER_SCRIPT)
            {
                ScriptStep();
            }
            else if (WParam == TIMER_HOLD_HINT || WParam == TIMER_HOLD)
            {
                HoldTimer(WParam);
            }
            return 0;

        case WM_MOUSEWHEEL:
        {
            POINT Point = { GET_X_LPARAM(LParam), GET_Y_LPARAM(LParam) };

            ScreenToClient(m_Window, &Point);
            Wheel(Point.x, Point.y, GET_WHEEL_DELTA_WPARAM(WParam) / static_cast<FLOAT>(WHEEL_DELTA));
            return 0;
        }

        case WM_CHAR:
        {
            WCHAR Command[16];

            /* Control characters arrive as keys, only printable text goes this way */
            if (WParam >= 0x20 && WParam != 0x7F)
            {
                StringCchPrintfW(Command, ARRAYSIZE(Command), L"char:%u", static_cast<UINT>(WParam));
                ExecuteCommand(Command);
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
            SetCapture(m_Window);
            Pointer(UixPointerDown, GET_X_LPARAM(LParam), GET_Y_LPARAM(LParam));
            return 0;

        case WM_MOUSEMOVE:
            if (WParam & MK_LBUTTON)
                Pointer(UixPointerMove, GET_X_LPARAM(LParam), GET_Y_LPARAM(LParam));
            return 0;

        case WM_LBUTTONUP:
            /* Deliver the release before giving up capture, or WM_CAPTURECHANGED cancels it */
            Pointer(UixPointerUp, GET_X_LPARAM(LParam), GET_Y_LPARAM(LParam));
            ReleaseCapture();
            return 0;

        case WM_CAPTURECHANGED:
            if (m_Capture)
                Pointer(UixPointerCancel, -1, -1);
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (WParam == VK_F4 && (GetKeyState(VK_MENU) & 0x8000))
                break;
            KeyDown(static_cast<UINT>(WParam));
            return 0;

        case WM_DESTROY:
            KillTimer(m_Window, TIMER_TICK);
            KillTimer(m_Window, TIMER_SCRIPT);
            EndHold();
            for (SIZE_T Index = m_Modules.size(); Index > 0; Index -= 1)
            {
                m_Modules[Index - 1].Component->Shutdown();
                m_Modules[Index - 1].Component->Release();
            }
            m_Modules.clear();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(m_Window, Message, WParam, LParam);
}

LRESULT
CALLBACK
CShellHost::WindowProc(
    _In_ HWND Window,
    _In_ UINT Message,
    _In_ WPARAM WParam,
    _In_ LPARAM LParam)
{
    CShellHost *This;

    if (Message == WM_NCCREATE)
    {
        This = static_cast<CShellHost *>(reinterpret_cast<LPCREATESTRUCTW>(LParam)->lpCreateParams);
        This->m_Window = Window;
        SetWindowLongPtrW(Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(This));
    }

    This = reinterpret_cast<CShellHost *>(GetWindowLongPtrW(Window, GWLP_USERDATA));
    if (!This || !This->m_Engine)
        return DefWindowProcW(Window, Message, WParam, LParam);
    return This->HandleMessage(Message, WParam, LParam);
}

HRESULT
CShellHost::Run(
    _In_ INT ShowCommand)
{
    WNDCLASSEXW Class;
    WCHAR Module[MAX_PATH];
    RECT Frame;
    UINT Dpi;
    MSG Message;
    HRESULT Status;
    FLOAT Scale;
    PWSTR Slash;

    GetModuleFileNameW(NULL, Module, ARRAYSIZE(Module));
    Slash = wcsrchr(Module, L'\\');
    if (Slash)
        *Slash = UNICODE_NULL;
    m_BinDirectory = Module;
    m_ConfigFile = JoinPath(m_BinDirectory, L"shell.xml");

    Status = LoadConfig();
    if (FAILED(Status))
        return Status;

    ZeroMemory(&Class, sizeof(Class));
    Class.cbSize = sizeof(Class);
    Class.lpfnWndProc = WindowProc;
    Class.hInstance = GetModuleHandleW(NULL);
    Class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    Class.lpszClassName = L"ReactOSMobileShell";
    RegisterClassExW(&Class);

    m_Window = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, Class.lpszClassName, L"ReactOS Mobile",
                               m_FullScreen ? WS_POPUP : WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                               NULL, NULL, Class.hInstance, this);
    if (!m_Window)
        return HRESULT_FROM_WIN32(GetLastError());

    Status = CoreMsgCreateSession(m_Session.Put());
    if (FAILED(Status))
        return Status;
    m_Session->SetWake(Wake, this);

    Status = UixCreateEngine(m_Engine.Put());
    if (FAILED(Status))
        return Status;
    Status = m_Engine->Startup(m_Window);
    if (FAILED(Status))
        return Status;

    Status = LoadSkin();
    if (FAILED(Status))
        return Status;

    if (m_FullScreen)
    {
        MONITORINFO Monitor = { sizeof(Monitor) };

        /* The whole monitor, title bar and borders included: the shell is the screen */
        GetMonitorInfoW(MonitorFromWindow(m_Window, MONITOR_DEFAULTTOPRIMARY), &Monitor);
        SetWindowPos(m_Window, NULL, Monitor.rcMonitor.left, Monitor.rcMonitor.top,
                     Monitor.rcMonitor.right - Monitor.rcMonitor.left, Monitor.rcMonitor.bottom - Monitor.rcMonitor.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    else
    {
        /* Logical canvas times the requested scale, in this monitor's pixels */
        Dpi = GetDpiForWindow(m_Window);
        Scale = m_InitialScale * Dpi / 96.0f;
        Frame = { 0, 0, lroundf(m_CanvasWidth * Scale), lroundf(m_CanvasHeight * Scale) };
        AdjustWindowRectExForDpi(&Frame, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_NOREDIRECTIONBITMAP, Dpi);
        SetWindowPos(m_Window, NULL, 0, 0, Frame.right - Frame.left, Frame.bottom - Frame.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    Status = CreateBands();
    if (FAILED(Status))
        return Status;

    Status = LoadComponents();
    if (FAILED(Status))
        return Status;

    m_Session->Dispatch();
    Resize();
    ShowWindow(m_Window, ShowCommand);
    SetTimer(m_Window, TIMER_TICK, 1000, NULL);
    if (!m_Script.empty())
    {
        /* Shots read the screen, so keep the window above everything while scripted */
        SetWindowPos(m_Window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(m_Window);
        SetTimer(m_Window, TIMER_SCRIPT, 400, NULL);
    }

    while (GetMessageW(&Message, NULL, 0, 0) > 0)
    {
        TranslateMessage(&Message);
        DispatchMessageW(&Message);
    }
    return S_OK;
}

INT
WINAPI
wWinMain(
    _In_ HINSTANCE Instance,
    _In_opt_ HINSTANCE Previous,
    _In_ PWSTR CommandLine,
    _In_ INT ShowCommand)
{
    CShellHost Host;
    PWSTR *Arguments;
    INT Count;
    INT Index;
    HRESULT Status;

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CommandLine);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    Arguments = CommandLineToArgvW(GetCommandLineW(), &Count);
    for (Index = 1; Arguments && Index + 1 < Count; Index++)
    {
        if (_wcsicmp(Arguments[Index], L"--run") == 0)
            Host.m_Script = Arguments[++Index];
        else if (_wcsicmp(Arguments[Index], L"--theme") == 0)
            Host.m_InitialTheme = Arguments[++Index];
        else if (_wcsicmp(Arguments[Index], L"--scale") == 0)
            Host.m_InitialScale = static_cast<FLOAT>(_wtof(Arguments[++Index]));
    }

    for (Index = 1; Arguments && Index < Count; Index++)
    {
        if (_wcsicmp(Arguments[Index], L"--windowed") == 0)
            Host.m_Windowed = TRUE;
    }
    LocalFree(Arguments);

    if (Host.m_InitialScale <= 0.0f)
        Host.m_InitialScale = 2.0f;

    Status = Host.Run(ShowCommand);
    if (FAILED(Status))
    {
        WCHAR Text[96];

        StringCchPrintfW(Text, ARRAYSIZE(Text), L"The shell failed to start (0x%08lX).", Status);
        MessageBoxW(NULL, Text, L"ReactOS Mobile", MB_ICONERROR);
    }

    CoUninitialize();
    return SUCCEEDED(Status) ? 0 : 1;
}
