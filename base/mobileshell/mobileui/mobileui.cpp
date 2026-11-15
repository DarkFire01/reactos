/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Home screen, Start menu and the out of process app host
 */

#define SHELLCOMP_EXPORTS
#include <devstatus.h>
#include <shellcomp.h>
#include <shellmsg.h>
#include <appwire.h>
#include <uixptr.h>
#include <uixxml.h>
#include <strsafe.h>
#include <math.h>
#include <string>
#include <vector>

/*
 * Skin contract:
 *   home.uix root: Title, SoftKeyLeft, SoftKeyLeftCommand, SoftKeyRight,
 *                  SoftKeyRightCommand, AppReveal ("dx,dy,seconds")
 *   home.uix elements: AppHost (the band apps are composed into)
 *   home.uix bindings: Weekday, Date, Time, UserName
 *   startmenu.uix elements: StartMenuRoot (shown while the menu is open)
 *   Commands: start, start.dismiss, home, launch:<AppId>, plus any host command
 */

#define MOBILE_UI_APP_MESSAGE 1
#define MOBILE_UI_APP_EXITED  2
#define MOBILE_UI_APP_CONNECTED 3

/* ReactOS does not know the remote client flag; the client PID check still guards the pipe */
#ifdef __REACTOS__
#define MOBILE_PIPE_LOCAL_ONLY 0
#else
#define MOBILE_PIPE_LOCAL_ONLY PIPE_REJECT_REMOTE_CLIENTS
#endif

typedef struct _MOBILE_APP_DEF
{
    std::wstring Id;
    std::wstring Exe;
    std::wstring Markup;
    std::wstring Title;
    std::wstring Args;
    BOOL Legacy;
} MOBILE_APP_DEF;

typedef struct _MOBILE_APP
{
    ULONG Id;
    MOBILE_APP_DEF Def;
    HANDLE Process;
    DWORD ProcessId;
    HANDLE Job;
    HANDLE Pipe;
    HANDLE Thread;
    volatile LONG Closing;
    UixPtr<ID3D11Texture2D> Shared;
    UixPtr<IDXGIKeyedMutex> Mutex;
    UixPtr<IDCompositionVisual> Visual;
    UixPtr<IDCompositionSurface> Surface;
    UINT Width;
    UINT Height;
    WCHAR Title[APP_TEXT_MAX];
    WCHAR SoftKeys[2][APP_TEXT_MAX];
    SHELL_CLOSE_KIND CloseKind;
    BOOL Shown;
} MOBILE_APP, *PMOBILE_APP;

typedef struct _MOBILE_APP_POST
{
    ULONG AppId;
    APP_MESSAGE Message;
} MOBILE_APP_POST, *PMOBILE_APP_POST;

class CMobileUI;

typedef struct _MOBILE_READER
{
    CMobileUI *Owner;
    IShellHost *Host;
    ULONG AppId;
    HANDLE Pipe;
    DWORD ProcessId;
} MOBILE_READER, *PMOBILE_READER;

class CMobileUI : public IShellComponent
{
public:
    CMobileUI()
        : m_Host(NULL),
          m_Endpoint(MSG_ENDPOINT_NONE),
          m_ChromeEndpoint(MSG_ENDPOINT_NONE),
          m_Active(NULL),
          m_MenuOpen(FALSE),
          m_NextAppId(1),
          m_PointerTarget(PointerNone)
    {
    }

    VOID STDMETHODCALLTYPE Release(VOID) override
    {
        delete this;
    }

    HRESULT STDMETHODCALLTYPE Initialize(
        _In_ IShellHost *Host) override;

    VOID STDMETHODCALLTYPE Layout(VOID) override;

    HRESULT STDMETHODCALLTYPE Reload(VOID) override;

    BOOL STDMETHODCALLTYPE PointerEvent(
        _In_ UIX_POINTER Kind,
        _In_ FLOAT X,
        _In_ FLOAT Y) override;

    BOOL STDMETHODCALLTYPE Wheel(
        _In_ FLOAT X,
        _In_ FLOAT Y,
        _In_ FLOAT Delta) override;

    BOOL STDMETHODCALLTYPE Command(
        _In_z_ PCWSTR Command) override;

    VOID STDMETHODCALLTYPE Tick(VOID) override;

    VOID STDMETHODCALLTYPE UiMessage(
        _In_ ULONG Code,
        _In_opt_ PVOID Data) override;

    VOID STDMETHODCALLTYPE Shutdown(VOID) override;

private:
    typedef enum _POINTER_TARGET
    {
        PointerNone,
        PointerMenu,
        PointerPopup,
        PointerStatus,
        PointerNotify,
        PointerSip,
        PointerHome,
        PointerApp
    } POINTER_TARGET;

    /* Overlay band order, bottom first */
    typedef enum _OVERLAY
    {
        OverlayMenu,
        OverlaySip,
        OverlayNotify,
        OverlayStatus,
        OverlayPopup,
        OverlayHold,
        OverlayCount
    } OVERLAY;

    typedef struct _NOTE
    {
        std::wstring Title;
        std::wstring Body;
    } NOTE;

    HRESULT
    LoadOverlay(
        _In_z_ PCWSTR File,
        _In_ OVERLAY Slot,
        _Inout_ UixPtr<IUixScene> &Scene);

    /* Popup menus for soft keys and tap and hold */
    VOID
    ShowPopup(
        _In_ ULONG AppId,
        _In_ APP_MENU_ANCHOR Anchor,
        _In_ FLOAT X,
        _In_ FLOAT Y,
        _In_z_ PCWSTR Items);

    VOID
    ClosePopup(
        _In_ LONG Pick);

    /* Notification bubble */
    VOID
    PostNote(
        _In_z_ PCWSTR Title,
        _In_z_ PCWSTR Body);

    VOID
    ShowNote(
        _In_ BOOL Show);

    VOID
    DismissNote(VOID);

    /* Soft input panel */
    VOID
    ShowSip(
        _In_ BOOL Show);

    /* Status panel opened from the title bar icons */
    VOID
    ShowStatus(
        _In_ BOOL Show);

    VOID
    RefreshStatus(VOID);

    VOID
    RefreshOwner(VOID);

    VOID
    UpdateSipKeys(VOID);

    VOID
    SipCommand(
        _In_z_ PCWSTR Command);

    VOID
    SendChar(
        _In_ WCHAR Char);

    VOID
    SendKey(
        _In_ UINT VirtualKey);

    static
    HRESULT
    CALLBACK
    OnMessage(
        _In_opt_ PVOID Context,
        _In_reads_bytes_(Size) const VOID *Payload,
        _In_ ULONG Size);

    static
    DWORD
    WINAPI
    ReaderThread(
        _In_ PVOID Parameter);

    VOID
    LoadConfig(VOID);

    VOID
    SendChrome(
        _In_ SHELL_MSG_ID Id,
        _In_ ULONG Value,
        _In_opt_z_ PCWSTR First,
        _In_opt_z_ PCWSTR Second);

    VOID
    UpdateChrome(VOID);

    VOID
    ShowMenu(
        _In_ BOOL Open);

    VOID
    GoHome(VOID);

    VOID
    SoftKey(
        _In_ SHELL_SOFTKEY Key);

    VOID
    CloseButton(VOID);

    VOID
    Navigate(
        _In_ INT Delta);

    VOID
    Select(VOID);

    /* App host */
    const MOBILE_APP_DEF *
    FindDef(
        _In_z_ PCWSTR Id) const;

    PMOBILE_APP
    FindApp(
        _In_ ULONG Id) const;

    PMOBILE_APP
    FindAppByDef(
        _In_z_ PCWSTR Id) const;

    BOOL
    Launch(
        _In_z_ PCWSTR Id,
        _In_opt_z_ PCWSTR Document = NULL);

    VOID
    ShowApp(
        _In_ PMOBILE_APP App);

    VOID
    CloseApp(
        _In_ PMOBILE_APP App);

    VOID
    PlaceApp(
        _In_ PMOBILE_APP App,
        _In_ BOOL Reveal);

    VOID
    ActivateApp(
        _In_ PMOBILE_APP App);

    BOOL
    WriteApp(
        _In_ PMOBILE_APP App,
        _In_ const APP_MESSAGE *Message);

    VOID
    AppMessage(
        _In_ PMOBILE_APP App,
        _In_ const APP_MESSAGE *Message);

    VOID
    PresentApp(
        _In_ PMOBILE_APP App);

    IShellHost *m_Host;
    MSG_ENDPOINT m_Endpoint;
    MSG_ENDPOINT m_ChromeEndpoint;
    UixPtr<IUixScene> m_Home;
    UixPtr<IUixScene> m_Menu;
    UixPtr<IDCompositionVisual> m_HomeLayer;
    UixPtr<IDCompositionVisual> m_AppLayer;
    UixPtr<IDCompositionVisual> m_Overlays[OverlayCount];
    UixPtr<IUixScene> m_Sip;
    UixPtr<IUixScene> m_Notify;
    UixPtr<IUixScene> m_Popup;
    UixPtr<IUixScene> m_Hold;
    UixPtr<IUixScene> m_StatusPanel;
    BOOL m_StatusOpen = FALSE;
    BOOL m_PopupOpen = FALSE;
    ULONG m_PopupApp = 0;
    std::vector<NOTE> m_Notes;
    BOOL m_NoteShown = FALSE;
    BOOL m_SipOpen = FALSE;
    BOOL m_Shift = FALSE;
    BOOL m_Caps = FALSE;
    BOOL m_Symbols = FALSE;
    std::vector<MOBILE_APP_DEF> m_Defs;
    std::vector<PMOBILE_APP> m_Apps;
    PMOBILE_APP m_Active;
    BOOL m_MenuOpen;
    ULONG m_NextAppId;
    POINTER_TARGET m_PointerTarget;
    UIX_RECT m_AppRect;
};

HRESULT
STDMETHODCALLTYPE
CMobileUI::Initialize(
    _In_ IShellHost *Host)
{
    UixPtr<IDCompositionDevice> Device;
    IDCompositionVisual *Content;
    HRESULT Status;

    m_Host = Host;
    ZeroMemory(&m_AppRect, sizeof(m_AppRect));

    Status = m_Host->GetSession()->CreateEndpoint(OnMessage, this, &m_Endpoint);
    if (FAILED(Status))
        return Status;

    Status = m_Host->GetSession()->PublishEndpoint(SHELL_ENDPOINT_HOST, m_Endpoint);
    if (FAILED(Status))
        return Status;

    Status = m_Host->GetEngine()->GetCompositionDevice(Device.Put());
    if (FAILED(Status))
        return Status;

    /* Home below, hosted apps above it, both inside the content band */
    Content = m_Host->GetBand(ShellBandContent);
    Status = Device->CreateVisual(m_HomeLayer.Put());
    if (SUCCEEDED(Status))
        Status = Device->CreateVisual(m_AppLayer.Put());
    if (SUCCEEDED(Status))
        Status = Content->AddVisual(m_HomeLayer.Get(), FALSE, NULL);
    if (SUCCEEDED(Status))
        Status = Content->AddVisual(m_AppLayer.Get(), TRUE, m_HomeLayer.Get());
    if (FAILED(Status))
        return Status;

    /* One container per overlay so z order never depends on load order */
    for (ULONG Slot = 0; Slot < OverlayCount; Slot++)
    {
        Status = Device->CreateVisual(m_Overlays[Slot].Put());
        if (FAILED(Status))
            return Status;
        if (Slot == 0)
            Status = m_Host->GetBand(ShellBandOverlay)->AddVisual(m_Overlays[Slot].Get(), FALSE, NULL);
        else
            Status = m_Host->GetBand(ShellBandOverlay)->AddVisual(m_Overlays[Slot].Get(), TRUE, m_Overlays[Slot - 1].Get());
        if (FAILED(Status))
            return Status;
    }

    LoadConfig();
    return Reload();
}

VOID
CMobileUI::LoadConfig(VOID)
{
    UIX_XML_NODE Root;

    m_Defs.clear();
    if (FAILED(UixXmlLoad(m_Host->GetConfigFile(), &Root)))
        return;

    for (const UIX_XML_NODE &Section : Root.Children)
    {
        if (Section.Name != L"Apps")
            continue;

        for (const UIX_XML_NODE &Node : Section.Children)
        {
            MOBILE_APP_DEF Def;

            if (Node.Name != L"App" || !Node.Attr(L"Id") || !Node.Attr(L"Exe"))
                continue;

            Def.Id = Node.Attr(L"Id");
            Def.Exe = Node.Attr(L"Exe");
            Def.Markup = Node.Attr(L"Markup", L"");
            Def.Title = Node.Attr(L"Title", Def.Id.c_str());
            Def.Args = Node.Attr(L"Args", L"");
            Def.Legacy = (_wcsicmp(Node.Attr(L"Scale", L"Native"), L"Legacy") == 0);
            m_Defs.push_back(Def);
        }
    }
}

HRESULT
CMobileUI::LoadOverlay(
    _In_z_ PCWSTR File,
    _In_ OVERLAY Slot,
    _Inout_ UixPtr<IUixScene> &Scene)
{
    UixPtr<IUixScene> Made;
    WCHAR Path[MAX_PATH];
    HRESULT Status;

    Scene.Reset();
    if (!m_Host->GetSkinFile(File, Path, ARRAYSIZE(Path)))
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    Status = m_Host->GetEngine()->CreateScene(Path, Made.Put());
    if (SUCCEEDED(Status))
        Status = Made->Attach(m_Overlays[Slot].Get());
    if (SUCCEEDED(Status))
        Scene = Made;
    return Status;
}

HRESULT
STDMETHODCALLTYPE
CMobileUI::Reload(VOID)
{
    UixPtr<IUixScene> Home;
    UixPtr<IUixScene> Menu;
    WCHAR Path[MAX_PATH];
    HRESULT Status;

    if (!m_Host->GetSkinFile(L"home.uix", Path, ARRAYSIZE(Path)))
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    Status = m_Host->GetEngine()->CreateScene(Path, Home.Put());
    if (FAILED(Status))
        return Status;

    if (!m_Host->GetSkinFile(L"startmenu.uix", Path, ARRAYSIZE(Path)))
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    Status = m_Host->GetEngine()->CreateScene(Path, Menu.Put());
    if (FAILED(Status))
        return Status;

    Status = Home->Attach(m_HomeLayer.Get());
    if (FAILED(Status))
        return Status;
    Status = Menu->Attach(m_Overlays[OverlayMenu].Get());
    if (FAILED(Status))
        return Status;

    m_Home = Home;
    m_Menu = Menu;
    m_Menu->SetVisible(L"StartMenuRoot", m_MenuOpen);

    /* Optional services: a skin without one of these simply lacks the feature */
    LoadOverlay(L"sip.uix", OverlaySip, m_Sip);
    LoadOverlay(L"notify.uix", OverlayNotify, m_Notify);
    LoadOverlay(L"popup.uix", OverlayPopup, m_Popup);
    LoadOverlay(L"status.uix", OverlayStatus, m_StatusPanel);
    LoadOverlay(L"hold.uix", OverlayHold, m_Hold);
    m_PopupOpen = FALSE;
    if (m_Sip)
    {
        m_Sip->SetVisible(L"SipRoot", m_SipOpen);
        UpdateSipKeys();
    }
    if (m_Notify)
        ShowNote(m_NoteShown);

    /* Running apps follow the theme without a restart */
    for (PMOBILE_APP App : m_Apps)
    {
        APP_MESSAGE Message;

        ZeroMemory(&Message, sizeof(Message));
        Message.Id = AppMsgTheme;
        StringCchCopyW(Message.Path, ARRAYSIZE(Message.Path), m_Host->GetThemeFile());
        WriteApp(App, &Message);
    }

    Tick();
    Layout();
    UpdateChrome();
    return S_OK;
}

VOID
STDMETHODCALLTYPE
CMobileUI::Layout(VOID)
{
    FLOAT Width;
    FLOAT Height;

    m_Host->GetCanvas(&Width, &Height);
    if (m_Home)
    {
        m_Home->Layout(Width, Height);
        if (!m_Home->GetElementRect(L"AppHost", &m_AppRect))
            m_AppRect = { 0.0f, 0.0f, Width, Height };
    }
    for (IUixScene *Scene : { m_Menu.Get(), m_Sip.Get(), m_Notify.Get(), m_Popup.Get(), m_Hold.Get(), m_StatusPanel.Get() })
    {
        if (Scene)
            Scene->Layout(Width, Height);
    }

    for (PMOBILE_APP App : m_Apps)
        ActivateApp(App);

    m_Host->RequestFrame();
}

VOID
STDMETHODCALLTYPE
CMobileUI::Tick(VOID)
{
    WCHAR Text[64];
    DWORD Length;

    if (!m_Home)
        return;

    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, NULL, L"dddd", Text, ARRAYSIZE(Text), NULL))
        m_Home->SetBinding(L"Weekday", Text);
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, NULL, L"d MMMM yyyy", Text, ARRAYSIZE(Text), NULL))
        m_Home->SetBinding(L"Date", Text);
    if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, NULL, NULL, Text, ARRAYSIZE(Text)))
        m_Home->SetBinding(L"Time", Text);

    Length = ARRAYSIZE(Text);
    if (GetUserNameW(Text, &Length))
        m_Home->SetBinding(L"UserName", Text);

    RefreshOwner();
    RefreshStatus();

    m_Host->RequestFrame();
}

VOID
CMobileUI::SendChrome(
    _In_ SHELL_MSG_ID Id,
    _In_ ULONG Value,
    _In_opt_z_ PCWSTR First,
    _In_opt_z_ PCWSTR Second)
{
    SHELL_MESSAGE Message;

    if (m_ChromeEndpoint == MSG_ENDPOINT_NONE &&
        FAILED(m_Host->GetSession()->OpenEndpoint(SHELL_ENDPOINT_CHROME, &m_ChromeEndpoint)))
    {
        return;
    }

    ZeroMemory(&Message, sizeof(Message));
    Message.Id = Id;
    Message.Value = Value;
    if (First)
        StringCchCopyW(Message.Text[0], ARRAYSIZE(Message.Text[0]), First);
    if (Second)
        StringCchCopyW(Message.Text[1], ARRAYSIZE(Message.Text[1]), Second);
    m_Host->GetSession()->Send(m_ChromeEndpoint, MsgPriorityNormal, &Message, sizeof(Message));
}

VOID
CMobileUI::UpdateChrome(VOID)
{
    PCWSTR Title;
    PCWSTR Left;
    PCWSTR Right;

    SendChrome(ShellMsgSetNotify, static_cast<ULONG>(m_Notes.size()), NULL, NULL);

    if (m_Active)
    {
        Left = m_Active->SoftKeys[0];
        Right = m_Active->SoftKeys[1];
        SendChrome(ShellMsgSetMode, ShellModeApp, NULL, NULL);
        SendChrome(ShellMsgSetTitle, 0, m_Active->Title, NULL);
        SendChrome(ShellMsgSetCloseButton, m_Active->CloseKind, NULL, NULL);
    }
    else if (m_Home)
    {
        Title = m_Home->GetAttribute(NULL, L"Title");
        Left = m_Home->GetAttribute(NULL, L"SoftKeyLeft");
        Right = m_Home->GetAttribute(NULL, L"SoftKeyRight");
        SendChrome(ShellMsgSetMode, ShellModeHome, NULL, NULL);
        SendChrome(ShellMsgSetTitle, 0, Title ? Title : L"", NULL);
        SendChrome(ShellMsgSetCloseButton, ShellCloseNone, NULL, NULL);
    }
    else
    {
        return;
    }

    /* A visible notification owns the soft keys until it goes away */
    if (m_NoteShown && m_Notify)
    {
        Left = m_Notify->GetAttribute(NULL, L"SoftKeyLeft");
        Right = m_Notify->GetAttribute(NULL, L"SoftKeyRight");
    }

    SendChrome(ShellMsgSetSoftKeys, 0, Left ? Left : L"", Right ? Right : L"");
}

VOID
CMobileUI::ShowMenu(
    _In_ BOOL Open)
{
    if (!m_Menu || m_MenuOpen == Open)
        return;

    m_MenuOpen = Open;
    m_Menu->SetSelected(NULL);
    m_Menu->SetVisible(L"StartMenuRoot", Open);
    m_Host->RequestFrame();
}

VOID
CMobileUI::GoHome(VOID)
{
    ShowMenu(FALSE);
    if (m_Active)
    {
        m_Active->Shown = FALSE;
        PlaceApp(m_Active, FALSE);
        m_Active = NULL;
    }
    UpdateChrome();
    m_Host->RequestFrame();
}

VOID
CMobileUI::SoftKey(
    _In_ SHELL_SOFTKEY Key)
{
    APP_MESSAGE Message;
    PCWSTR Command = NULL;

    ShowMenu(FALSE);

    if (Key == ShellSoftKeyCenter)
    {
        ShowSip(!m_SipOpen);
        return;
    }

    if (m_PopupOpen)
    {
        ClosePopup(-1);
        return;
    }

    if (m_NoteShown)
    {
        if (Key == ShellSoftKeyLeft)
            DismissNote();
        else
            ShowNote(FALSE);
        return;
    }

    if (m_Active)
    {
        ZeroMemory(&Message, sizeof(Message));
        Message.Id = AppMsgSoftKey;
        Message.Int[0] = Key;
        WriteApp(m_Active, &Message);
        return;
    }

    if (Key == ShellSoftKeyLeft)
        Command = m_Home->GetAttribute(NULL, L"SoftKeyLeftCommand");
    else if (Key == ShellSoftKeyRight)
        Command = m_Home->GetAttribute(NULL, L"SoftKeyRightCommand");
    else
        Command = m_Home->GetAttribute(NULL, L"SoftKeyCenterCommand");

    if (Command && Command[0])
        m_Host->ExecuteCommand(Command);
}

VOID
CMobileUI::CloseButton(VOID)
{
    APP_MESSAGE Message;

    ShowMenu(FALSE);
    if (!m_Active)
        return;

    /* X keeps the app running in the background, ok is the app's own decision */
    if (m_Active->CloseKind == ShellCloseX)
    {
        GoHome();
        return;
    }

    ZeroMemory(&Message, sizeof(Message));
    Message.Id = AppMsgCloseButton;
    Message.Int[0] = m_Active->CloseKind;
    WriteApp(m_Active, &Message);
}

VOID
CMobileUI::Navigate(
    _In_ INT Delta)
{
    APP_MESSAGE Message;

    if (m_MenuOpen)
    {
        m_Menu->MoveSelection(Delta);
    }
    else if (m_Active)
    {
        ZeroMemory(&Message, sizeof(Message));
        Message.Id = AppMsgKey;
        Message.Int[0] = (Delta < 0) ? VK_UP : VK_DOWN;
        WriteApp(m_Active, &Message);
    }
    else
    {
        m_Home->MoveSelection(Delta);
    }
    m_Host->RequestFrame();
}

VOID
CMobileUI::Select(VOID)
{
    APP_MESSAGE Message;
    PCWSTR Command;
    std::wstring Copy;

    if (m_Active && !m_MenuOpen)
    {
        ZeroMemory(&Message, sizeof(Message));
        Message.Id = AppMsgKey;
        Message.Int[0] = VK_RETURN;
        WriteApp(m_Active, &Message);
        return;
    }

    Command = m_MenuOpen ? m_Menu->GetSelectedCommand() : m_Home->GetSelectedCommand();
    if (Command && Command[0])
    {
        Copy = Command;
        m_Host->ExecuteCommand(Copy.c_str());
    }
}

BOOL
STDMETHODCALLTYPE
CMobileUI::Command(
    _In_z_ PCWSTR Command)
{
    APP_MESSAGE Message;
    FLOAT X;
    FLOAT Y;
    WCHAR Text[16];

    if (_wcsnicmp(Command, L"popup.pick:", 11) == 0)
    {
        ClosePopup(_wtoi(Command + 11));
    }
    else if (_wcsicmp(Command, L"popup.dismiss") == 0)
    {
        ClosePopup(-1);
    }
    else if (_wcsicmp(Command, L"popup.none") == 0)
    {
        /* Separators swallow the tap */
    }
    else if (_wcsicmp(Command, L"notify.dismiss") == 0)
    {
        DismissNote();
    }
    else if (_wcsicmp(Command, L"notify.hide") == 0)
    {
        ShowNote(FALSE);
    }
    else if (_wcsicmp(Command, L"notify.show") == 0)
    {
        ShowNote(!m_Notes.empty());
    }
    else if (_wcsnicmp(Command, L"notify:", 7) == 0)
    {
        std::wstring Spec = Command + 7;
        SIZE_T Bar = Spec.find(L'|');

        if (Bar == std::wstring::npos)
            PostNote(Spec.c_str(), L"");
        else
            PostNote(Spec.substr(0, Bar).c_str(), Spec.substr(Bar + 1).c_str());
    }
    else if (_wcsicmp(Command, L"status") == 0)
    {
        ShowStatus(!m_StatusOpen);
    }
    else if (_wcsicmp(Command, L"status.close") == 0)
    {
        ShowStatus(FALSE);
    }
    else if (_wcsnicmp(Command, L"volume.", 7) == 0)
    {
        if (_wcsicmp(Command + 7, L"up") == 0)
            DevChangeVolume(10, 2);
        else if (_wcsicmp(Command + 7, L"down") == 0)
            DevChangeVolume(-10, 2);
        else if (_wcsicmp(Command + 7, L"mute") == 0)
            DevChangeVolume(0, -1);

        /* The chrome icon follows right away, not on its next tick */
        m_Host->ExecuteCommand(L"status.refresh");
        RefreshStatus();
    }
    else if (_wcsicmp(Command, L"sip") == 0)
    {
        ShowSip(!m_SipOpen);
    }
    else if (_wcsnicmp(Command, L"sip.", 4) == 0)
    {
        SipCommand(Command + 4);
    }
    else if (_wcsnicmp(Command, L"hold.hint:", 10) == 0)
    {
        /* The ring is centered on the press point */
        if (m_Hold && swscanf(Command + 10, L"%f,%f", &X, &Y) == 2)
        {
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"%.1f", X);
            m_Hold->SetBinding(L"HoldX", Text);
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"%.1f", Y);
            m_Hold->SetBinding(L"HoldY", Text);
            m_Hold->SetVisible(L"HoldRoot", TRUE);
            m_Host->RequestFrame();
        }
    }
    else if (_wcsicmp(Command, L"hold.end") == 0)
    {
        if (m_Hold)
        {
            m_Hold->SetVisible(L"HoldRoot", FALSE);
            m_Host->RequestFrame();
        }
    }
    else if (_wcsnicmp(Command, L"char:", 5) == 0)
    {
        if (!m_Active)
            return FALSE;
        SendChar(static_cast<WCHAR>(_wtoi(Command + 5)));
    }
    else if (_wcsicmp(Command, L"start") == 0)
    {
        ShowMenu(!m_MenuOpen);
    }
    else if (_wcsicmp(Command, L"start.dismiss") == 0)
    {
        ShowMenu(FALSE);
    }
    else if (_wcsicmp(Command, L"home") == 0)
    {
        GoHome();
    }
    else if (_wcsnicmp(Command, L"launch:", 7) == 0)
    {
        ShowMenu(FALSE);
        ShowStatus(FALSE);
        if (!Launch(Command + 7))
            PostNote(Command + 7, L"@NotInstalled");
    }
    else if (_wcsicmp(Command, L"softkey.left") == 0)
    {
        SoftKey(ShellSoftKeyLeft);
    }
    else if (_wcsicmp(Command, L"softkey.right") == 0)
    {
        SoftKey(ShellSoftKeyRight);
    }
    else if (_wcsicmp(Command, L"softkey.center") == 0)
    {
        SoftKey(ShellSoftKeyCenter);
    }
    else if (_wcsicmp(Command, L"close") == 0 || _wcsicmp(Command, L"back") == 0)
    {
        if (m_PopupOpen)
            ClosePopup(-1);
        else if (m_StatusOpen)
            ShowStatus(FALSE);
        else if (m_MenuOpen)
            ShowMenu(FALSE);
        else if (m_SipOpen && _wcsicmp(Command, L"back") == 0)
            ShowSip(FALSE);
        else if (m_NoteShown && _wcsicmp(Command, L"back") == 0)
            ShowNote(FALSE);
        else
            CloseButton();
    }
    else if (_wcsicmp(Command, L"nav.up") == 0)
    {
        Navigate(-1);
    }
    else if (_wcsicmp(Command, L"nav.down") == 0)
    {
        Navigate(1);
    }
    else if (_wcsicmp(Command, L"nav.select") == 0)
    {
        Select();
    }
    else if (_wcsnicmp(Command, L"key:", 4) == 0)
    {
        if (!m_Active)
            return FALSE;
        ZeroMemory(&Message, sizeof(Message));
        Message.Id = AppMsgKey;
        Message.Int[0] = _wtoi(Command + 4);
        WriteApp(m_Active, &Message);
    }
    else
    {
        return FALSE;
    }
    return TRUE;
}

BOOL
STDMETHODCALLTYPE
CMobileUI::PointerEvent(
    _In_ UIX_POINTER Kind,
    _In_ FLOAT X,
    _In_ FLOAT Y)
{
    APP_MESSAGE Message;
    PCWSTR Command = NULL;
    std::wstring Copy;
    POINTER_TARGET Target = m_PointerTarget;
    BOOL Handled = FALSE;

    if (Kind == UixPointerDown)
    {
        /* Popups and the Start menu are modal, the bubble and SIP only take what they cover */
        if (m_PopupOpen && m_Popup)
        {
            Target = PointerPopup;
        }
        else if (m_MenuOpen)
        {
            Target = PointerMenu;
        }
        else if (m_StatusOpen && m_StatusPanel)
        {
            Target = PointerStatus;
        }
        else if (m_NoteShown && m_Notify && m_Notify->PointerEvent(Kind, X, Y, &Command))
        {
            Target = PointerNotify;
            Handled = TRUE;
        }
        else if (m_SipOpen && m_Sip && m_Sip->PointerEvent(Kind, X, Y, &Command))
        {
            Target = PointerSip;
            Handled = TRUE;
        }
        else if (m_Active && X >= m_AppRect.Left && X < m_AppRect.Right && Y >= m_AppRect.Top && Y < m_AppRect.Bottom)
        {
            Target = PointerApp;
        }
        else
        {
            Target = PointerHome;
        }
        m_PointerTarget = Target;
    }

    switch (Handled ? PointerNone : Target)
    {
        case PointerPopup:
            Handled = m_Popup->PointerEvent(Kind, X, Y, &Command);
            if (!Handled && Kind == UixPointerDown)
            {
                ClosePopup(-1);
                Handled = TRUE;
                m_PointerTarget = PointerNone;
            }
            break;

        case PointerNotify:
            Handled = m_Notify->PointerEvent(Kind, X, Y, &Command);
            break;

        case PointerStatus:
            Handled = m_StatusPanel->PointerEvent(Kind, X, Y, &Command);
            break;

        case PointerSip:
            Handled = m_Sip->PointerEvent(Kind, X, Y, &Command);
            break;

        case PointerMenu:
            Handled = m_Menu->PointerEvent(Kind, X, Y, &Command);

            /* The open menu is modal: taps that miss it close it */
            if (!Handled && Kind == UixPointerDown)
            {
                ShowMenu(FALSE);
                Handled = TRUE;
                m_PointerTarget = PointerNone;
            }
            break;

        case PointerApp:
            ZeroMemory(&Message, sizeof(Message));
            Message.Id = AppMsgPointer;
            Message.Int[0] = Kind;
            Message.Float[0] = X - m_AppRect.Left;
            Message.Float[1] = Y - m_AppRect.Top;
            if (m_Active)
                WriteApp(m_Active, &Message);
            Handled = TRUE;
            break;

        case PointerHome:
            if (!m_Active)
                Handled = m_Home->PointerEvent(Kind, X, Y, &Command);
            break;

        default:
            break;
    }

    if (Kind == UixPointerUp || Kind == UixPointerCancel)
        m_PointerTarget = PointerNone;
    if (!Handled && Kind == UixPointerDown)
        m_PointerTarget = PointerNone;

    if (Handled)
        m_Host->RequestFrame();

    if (Command && Command[0])
    {
        Copy = Command;
        m_Host->ExecuteCommand(Copy.c_str());
    }
    return Handled;
}

BOOL
STDMETHODCALLTYPE
CMobileUI::Wheel(
    _In_ FLOAT X,
    _In_ FLOAT Y,
    _In_ FLOAT Delta)
{
    APP_MESSAGE Message;

    if (m_PopupOpen || m_MenuOpen)
        return TRUE;

    if (m_Active && X >= m_AppRect.Left && X < m_AppRect.Right && Y >= m_AppRect.Top && Y < m_AppRect.Bottom)
    {
        ZeroMemory(&Message, sizeof(Message));
        Message.Id = AppMsgWheel;
        Message.Float[0] = X - m_AppRect.Left;
        Message.Float[1] = Y - m_AppRect.Top;
        Message.Float[2] = Delta;
        WriteApp(m_Active, &Message);
        return TRUE;
    }

    if (!m_Active && m_Home && m_Home->Wheel(X, Y, Delta))
    {
        m_Host->RequestFrame();
        return TRUE;
    }
    return FALSE;
}

VOID
CMobileUI::ShowPopup(
    _In_ ULONG AppId,
    _In_ APP_MENU_ANCHOR Anchor,
    _In_ FLOAT X,
    _In_ FLOAT Y,
    _In_z_ PCWSTR Items)
{
    static PCWSTR const Fields[] = { L"Text", L"Height", L"Fill", L"Command" };
    std::vector<std::wstring> Storage;
    std::vector<PCWSTR> Values;
    std::wstring List = Items;
    std::wstring Line;
    UIX_RECT Rect;
    WCHAR Number[24];
    FLOAT Width;
    FLOAT Height;
    FLOAT Left;
    FLOAT Top;
    ULONG Count = 0;
    ULONG Pick = 0;
    SIZE_T Start = 0;
    SIZE_T End;

    ClosePopup(-1);
    ShowMenu(FALSE);
    if (!m_Popup)
        return;

    while (Start <= List.size())
    {
        End = List.find(L'\n', Start);
        if (End == std::wstring::npos)
            End = List.size();
        Line = List.substr(Start, End - Start);
        Start = End + 1;

        if (Line.empty())
            continue;

        if (Line == APP_MENU_SEPARATOR)
        {
            Storage.insert(Storage.end(), { L"", L"@PopupSeparatorHeight", L"@PopupSeparator", L"popup.none" });
        }
        else
        {
            StringCchPrintfW(Number, ARRAYSIZE(Number), L"popup.pick:%lu", Pick++);
            Storage.insert(Storage.end(), { Line, L"@PopupItemHeight", L"", Number });
        }
        Count++;
    }

    for (const std::wstring &Value : Storage)
        Values.push_back(Value.c_str());

    m_Popup->SetItems(L"PopupItems", Count, ARRAYSIZE(Fields), Fields, Values.data());
    m_Popup->SetBinding(L"PopupX", L"0");
    m_Popup->SetBinding(L"PopupY", L"0");
    m_Popup->SetSelected(NULL);
    m_Popup->SetVisible(L"PopupRoot", TRUE);

    m_Host->GetCanvas(&Width, &Height);
    m_Popup->Layout(Width, Height);
    if (!m_Popup->GetElementRect(L"PopupMenu", &Rect))
        return;

    /* Soft key menus sit on the bar at their side, context menus right of and below the point */
    if (Anchor == AppMenuRightSoftKey)
    {
        Left = Width - (Rect.Right - Rect.Left);
        Top = m_AppRect.Bottom - (Rect.Bottom - Rect.Top);
    }
    else if (Anchor == AppMenuLeftSoftKey)
    {
        Left = 0.0f;
        Top = m_AppRect.Bottom - (Rect.Bottom - Rect.Top);
    }
    else
    {
        Left = m_AppRect.Left + X;
        Top = m_AppRect.Top + Y;
    }

    Left = max(0.0f, min(Left, Width - (Rect.Right - Rect.Left)));
    Top = max(m_AppRect.Top, min(Top, m_AppRect.Bottom - (Rect.Bottom - Rect.Top)));

    StringCchPrintfW(Number, ARRAYSIZE(Number), L"%.1f", Left);
    m_Popup->SetBinding(L"PopupX", Number);
    StringCchPrintfW(Number, ARRAYSIZE(Number), L"%.1f", Top);
    m_Popup->SetBinding(L"PopupY", Number);

    m_PopupOpen = TRUE;
    m_PopupApp = AppId;
    m_Host->RequestFrame();
}

VOID
CMobileUI::ClosePopup(
    _In_ LONG Pick)
{
    APP_MESSAGE Message;
    PMOBILE_APP App;

    if (!m_PopupOpen)
        return;

    m_PopupOpen = FALSE;
    m_Popup->SetVisible(L"PopupRoot", FALSE);

    App = FindApp(m_PopupApp);
    m_PopupApp = 0;
    if (App)
    {
        ZeroMemory(&Message, sizeof(Message));
        Message.Id = AppMsgMenuPick;
        Message.Int[0] = Pick;
        WriteApp(App, &Message);
    }
    m_Host->RequestFrame();
}

VOID
CMobileUI::PostNote(
    _In_z_ PCWSTR Title,
    _In_z_ PCWSTR Body)
{
    NOTE Note;

    Note.Title = Title;
    Note.Body = Body;
    m_Notes.push_back(Note);
    ShowNote(TRUE);
}

VOID
CMobileUI::ShowNote(
    _In_ BOOL Show)
{
    if (!m_Notify)
        return;

    m_NoteShown = Show && !m_Notes.empty();
    if (m_NoteShown)
    {
        m_Notify->SetBinding(L"NotifyTitle", m_Notes.front().Title.c_str());
        m_Notify->SetBinding(L"NotifyBody", m_Notes.front().Body.c_str());
    }
    m_Notify->SetVisible(L"NotifyRoot", m_NoteShown);
    UpdateChrome();
    m_Host->RequestFrame();
}

VOID
CMobileUI::DismissNote(VOID)
{
    if (!m_Notes.empty())
        m_Notes.erase(m_Notes.begin());

    /* Hide first so the next note replays the drop in */
    ShowNote(FALSE);
    if (!m_Notes.empty())
        ShowNote(TRUE);
}

VOID
CMobileUI::ShowStatus(
    _In_ BOOL Show)
{
    if (!m_StatusPanel)
        return;

    m_StatusOpen = Show;
    if (Show)
    {
        ShowMenu(FALSE);
        RefreshStatus();
    }
    m_StatusPanel->SetVisible(L"StatusRoot", Show);
    m_Host->RequestFrame();
}

VOID
CMobileUI::RefreshStatus(VOID)
{
    DEV_STATUS Status;
    WCHAR Text[96];
    WCHAR Name[8];
    LONG Lit;

    if (!m_StatusPanel || !m_StatusOpen)
        return;

    DevQueryAll(&Status);

    if (Status.Network == DevNetworkWireless)
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"Wi-Fi: %s (%lu%%)", Status.Ssid[0] ? Status.Ssid : Status.Adapter, Status.Quality);
    else if (Status.Network == DevNetworkWired)
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"Connected: %s", Status.Adapter);
    else
        StringCchCopyW(Text, ARRAYSIZE(Text), L"Not connected");
    m_StatusPanel->SetBinding(L"StatusNetwork", Text);

    if (Status.Volume < 0)
        StringCchCopyW(Text, ARRAYSIZE(Text), L"No sound device");
    else if (Status.Muted)
        StringCchCopyW(Text, ARRAYSIZE(Text), L"Sound off");
    else
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"Volume %ld%%", Status.Volume);
    m_StatusPanel->SetBinding(L"StatusVolume", Text);
    m_StatusPanel->SetBinding(L"MuteLabel", Status.Muted ? L"Turn sound on" : L"Turn sound off");
    m_StatusPanel->SetBinding(L"VolumeGlyph", Status.Muted || Status.Volume <= 0 ? L"\xE74F" : L"\xE767");

    /* Ten segments, lit up to the level and dimmed while muted */
    Lit = (Status.Volume < 0) ? 0 : (Status.Volume + 5) / 10;
    for (LONG Segment = 1; Segment <= 10; Segment++)
    {
        StringCchPrintfW(Name, ARRAYSIZE(Name), L"Vol%ld", Segment);
        m_StatusPanel->SetBinding(Name, (Segment <= Lit && !Status.Muted) ? L"@VolumeOn" : L"@VolumeOff");
    }

    if (!Status.HasBattery)
        StringCchCopyW(Text, ARRAYSIZE(Text), L"External power");
    else if (Status.Charging)
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"Battery %lu%%, charging", Status.BatteryPercent);
    else if (Status.OnAc)
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"Battery %lu%%, plugged in", Status.BatteryPercent);
    else
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"Battery %lu%%", Status.BatteryPercent);
    m_StatusPanel->SetBinding(L"StatusBattery", Text);
    m_Host->RequestFrame();
}

VOID
CMobileUI::RefreshOwner(VOID)
{
    WCHAR Name[64];
    WCHAR Phone[48];
    DWORD Size;
    std::wstring Line;

    if (!m_Home)
        return;

    /* Settings writes the owner card, Today shows it in place of the prompt */
    Size = sizeof(Name);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ReactOSMobile\\Owner", L"Name", RRF_RT_REG_SZ, NULL, Name, &Size) != ERROR_SUCCESS)
        Name[0] = UNICODE_NULL;
    Size = sizeof(Phone);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ReactOSMobile\\Owner", L"Phone", RRF_RT_REG_SZ, NULL, Phone, &Size) != ERROR_SUCCESS)
        Phone[0] = UNICODE_NULL;

    if (Name[0])
    {
        Line = Name;
        if (Phone[0])
            Line += std::wstring(L"  ") + Phone;
    }
    else
    {
        Line = L"@OwnerPrompt";
    }
    m_Home->SetBinding(L"OwnerLine", Line.c_str());
}

VOID
CMobileUI::ShowSip(
    _In_ BOOL Show)
{
    if (!m_Sip)
        return;

    m_SipOpen = Show;
    m_Sip->SetVisible(L"SipRoot", Show);
    m_Host->RequestFrame();
}

static PCWSTR const g_SipPlain = L"1234567890-=[];',./`\\";
static PCWSTR const g_SipShifted = L"!@#$%^&*()_+{}:\"<>?~|";

VOID
CMobileUI::UpdateSipKeys(VOID)
{
    WCHAR Name[8];
    WCHAR Label[2] = { 0, 0 };
    BOOL Upper = m_Shift != m_Caps;
    WCHAR Char;
    SIZE_T Index;

    if (!m_Sip)
        return;

    for (Char = L'a'; Char <= L'z'; Char++)
    {
        StringCchPrintfW(Name, ARRAYSIZE(Name), L"K.%c", Char);
        Label[0] = Upper ? towupper(Char) : Char;
        m_Sip->SetBinding(Name, Label);
    }

    /* Shift alone changes the digit and punctuation row, CAP does not */
    for (Index = 0; g_SipPlain[Index]; Index++)
    {
        StringCchPrintfW(Name, ARRAYSIZE(Name), L"K.%c", g_SipPlain[Index]);
        Label[0] = m_Shift ? g_SipShifted[Index] : g_SipPlain[Index];
        m_Sip->SetBinding(Name, Label);
    }

    m_Sip->SetBinding(L"ShiftFill", m_Shift ? L"@SipKeyActive" : L"@SipKey");
    m_Sip->SetBinding(L"CapsFill", m_Caps ? L"@SipKeyActive" : L"@SipKey");
    m_Sip->SetBinding(L"SymbolsFill", m_Symbols ? L"@SipKeyActive" : L"@SipKey");
    m_Sip->SetVisible(L"SipLetters", !m_Symbols);
    m_Sip->SetVisible(L"SipSymbols", m_Symbols);
}

VOID
CMobileUI::SipCommand(
    _In_z_ PCWSTR Command)
{
    PCWSTR Found;
    WCHAR Char;

    if (_wcsnicmp(Command, L"char:", 5) == 0 && Command[5])
    {
        Char = Command[5];
        if (iswalpha(Char))
        {
            Char = (m_Shift != m_Caps) ? towupper(Char) : towlower(Char);
        }
        else if (m_Shift && (Found = wcschr(g_SipPlain, Char)) != NULL)
        {
            Char = g_SipShifted[Found - g_SipPlain];
        }
        SendChar(Char);
        m_Shift = FALSE;
    }
    else if (_wcsicmp(Command, L"shift") == 0)
    {
        m_Shift = !m_Shift;
    }
    else if (_wcsicmp(Command, L"caps") == 0)
    {
        m_Caps = !m_Caps;
    }
    else if (_wcsicmp(Command, L"123") == 0)
    {
        m_Symbols = !m_Symbols;
    }
    else if (_wcsicmp(Command, L"space") == 0)
    {
        SendChar(L' ');
    }
    else if (_wcsicmp(Command, L"tab") == 0)
    {
        SendChar(L'\t');
    }
    else if (_wcsicmp(Command, L"back") == 0)
    {
        SendKey(VK_BACK);
    }
    else if (_wcsicmp(Command, L"enter") == 0)
    {
        SendKey(VK_RETURN);
    }
    else if (_wcsicmp(Command, L"left") == 0)
    {
        SendKey(VK_LEFT);
    }
    else if (_wcsicmp(Command, L"right") == 0)
    {
        SendKey(VK_RIGHT);
    }
    else if (_wcsicmp(Command, L"up") == 0)
    {
        SendKey(VK_UP);
    }
    else if (_wcsicmp(Command, L"down") == 0)
    {
        SendKey(VK_DOWN);
    }
    else if (_wcsicmp(Command, L"hide") == 0)
    {
        ShowSip(FALSE);
    }

    UpdateSipKeys();
    m_Host->RequestFrame();
}

VOID
CMobileUI::SendChar(
    _In_ WCHAR Char)
{
    APP_MESSAGE Message;

    if (!m_Active)
        return;

    ZeroMemory(&Message, sizeof(Message));
    Message.Id = AppMsgChar;
    Message.Int[0] = Char;
    WriteApp(m_Active, &Message);
}

VOID
CMobileUI::SendKey(
    _In_ UINT VirtualKey)
{
    APP_MESSAGE Message;

    if (!m_Active)
        return;

    ZeroMemory(&Message, sizeof(Message));
    Message.Id = AppMsgKey;
    Message.Int[0] = VirtualKey;
    WriteApp(m_Active, &Message);
}

HRESULT
CALLBACK
CMobileUI::OnMessage(
    _In_opt_ PVOID Context,
    _In_reads_bytes_(Size) const VOID *Payload,
    _In_ ULONG Size)
{
    CMobileUI *This = static_cast<CMobileUI *>(Context);
    const SHELL_MESSAGE *Message = static_cast<const SHELL_MESSAGE *>(Payload);

    if (!This || Size < sizeof(SHELL_MESSAGE))
        return E_INVALIDARG;

    switch (Message->Id)
    {
        case ShellMsgStartInvoked:
            This->ShowMenu(!This->m_MenuOpen);
            break;

        case ShellMsgSoftKeyInvoked:
            This->SoftKey(static_cast<SHELL_SOFTKEY>(Message->Value));
            break;

        case ShellMsgCloseInvoked:
            This->CloseButton();
            break;

        default:
            return E_NOTIMPL;
    }
    return S_OK;
}

const MOBILE_APP_DEF *
CMobileUI::FindDef(
    _In_z_ PCWSTR Id) const
{
    for (const MOBILE_APP_DEF &Def : m_Defs)
    {
        if (_wcsicmp(Def.Id.c_str(), Id) == 0)
            return &Def;
    }
    return NULL;
}

PMOBILE_APP
CMobileUI::FindApp(
    _In_ ULONG Id) const
{
    for (PMOBILE_APP App : m_Apps)
    {
        if (App->Id == Id)
            return App;
    }
    return NULL;
}

PMOBILE_APP
CMobileUI::FindAppByDef(
    _In_z_ PCWSTR Id) const
{
    for (PMOBILE_APP App : m_Apps)
    {
        if (_wcsicmp(App->Def.Id.c_str(), Id) == 0)
            return App;
    }
    return NULL;
}

BOOL
CMobileUI::Launch(
    _In_z_ PCWSTR Id,
    _In_opt_z_ PCWSTR Document)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits;
    JOBOBJECT_BASIC_UI_RESTRICTIONS Restrictions;
    UixPtr<IDCompositionDevice> Device;
    STARTUPINFOW Startup;
    PROCESS_INFORMATION Process;
    const MOBILE_APP_DEF *Def;
    PMOBILE_APP App;
    PMOBILE_READER Reader;
    WCHAR PipeName[96];
    WCHAR SkinDirectory[MAX_PATH];
    WCHAR Markup[MAX_PATH];
    std::wstring CommandLine;

    App = FindAppByDef(Id);
    if (App)
    {
        ShowApp(App);

        /* A running app gets the document over its pipe instead of a second process */
        if (Document && Document[0])
        {
            APP_MESSAGE Message;

            ZeroMemory(&Message, sizeof(Message));
            Message.Id = AppMsgOpenDocument;
            StringCchCopyW(Message.Path, ARRAYSIZE(Message.Path), Document);
            WriteApp(App, &Message);
        }
        return TRUE;
    }

    Def = FindDef(Id);
    if (!Def)
        return FALSE;

    App = new MOBILE_APP();
    App->Id = m_NextAppId++;
    App->Def = *Def;
    App->CloseKind = ShellCloseOk;
    StringCchCopyW(App->Title, ARRAYSIZE(App->Title), Def->Title.c_str());

    if (FAILED(m_Host->GetEngine()->GetCompositionDevice(Device.Put())) ||
        FAILED(Device->CreateVisual(App->Visual.Put())) ||
        FAILED(m_AppLayer->AddVisual(App->Visual.Get(), FALSE, NULL)))
    {
        delete App;
        return FALSE;
    }

    StringCchPrintfW(PipeName, ARRAYSIZE(PipeName), L"\\\\.\\pipe\\ReactOSMobile.%lu.%lu", GetCurrentProcessId(), App->Id);
    App->Pipe = CreateNamedPipeW(PipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | MOBILE_PIPE_LOCAL_ONLY,
                                 1, sizeof(APP_MESSAGE) * 8, sizeof(APP_MESSAGE) * 8, 0, NULL);
    if (App->Pipe == INVALID_HANDLE_VALUE)
    {
        m_AppLayer->RemoveVisual(App->Visual.Get());
        delete App;
        return FALSE;
    }

    m_Host->GetSkinFile(L"", SkinDirectory, ARRAYSIZE(SkinDirectory));
    m_Host->GetSkinFile(Def->Markup.c_str(), Markup, ARRAYSIZE(Markup));

    CommandLine = L"\"";
    CommandLine += m_Host->GetBinDirectory();
    CommandLine += L"\\" + Def->Exe + L"\" --pipe \"" + PipeName + L"\" --theme \"" + m_Host->GetThemeFile() +
                   L"\" --markup \"" + Markup + L"\"";
    if (!Def->Args.empty())
        CommandLine += L" " + Def->Args;
    if (Document && Document[0])
        CommandLine += std::wstring(L" --open \"") + Document + L"\"";

    /* The job is the isolation boundary for now, a container or VM can replace it */
    App->Job = CreateJobObjectW(NULL, NULL);
    ZeroMemory(&Limits, sizeof(Limits));
    Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    ZeroMemory(&Restrictions, sizeof(Restrictions));
    Restrictions.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
                                       JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS |
                                       JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                                       JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
    if (App->Job)
    {
        SetInformationJobObject(App->Job, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits));
        SetInformationJobObject(App->Job, JobObjectBasicUIRestrictions, &Restrictions, sizeof(Restrictions));
    }

    ZeroMemory(&Startup, sizeof(Startup));
    Startup.cb = sizeof(Startup);
    if (!CreateProcessW(NULL, &CommandLine[0], NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                        NULL, m_Host->GetBinDirectory(), &Startup, &Process))
    {
        CloseApp(App);
        return FALSE;
    }

    if (App->Job)
        AssignProcessToJobObject(App->Job, Process.hProcess);
    App->Process = Process.hProcess;
    App->ProcessId = Process.dwProcessId;

    Reader = new MOBILE_READER();
    Reader->Owner = this;
    Reader->Host = m_Host;
    Reader->AppId = App->Id;
    Reader->Pipe = App->Pipe;
    Reader->ProcessId = Process.dwProcessId;
    App->Thread = CreateThread(NULL, 0, ReaderThread, Reader, 0, NULL);
    if (!App->Thread)
        delete Reader;

    ResumeThread(Process.hThread);
    CloseHandle(Process.hThread);

    /* Activation waits for the app to connect, see MOBILE_UI_APP_CONNECTED */
    m_Apps.push_back(App);
    ShowApp(App);
    return TRUE;
}

DWORD
WINAPI
CMobileUI::ReaderThread(
    _In_ PVOID Parameter)
{
    PMOBILE_READER Reader = static_cast<PMOBILE_READER>(Parameter);
    PMOBILE_APP_POST Post;
    OVERLAPPED Overlapped;
    APP_MESSAGE Message;
    ULONG ClientId = 0;
    DWORD Read;
    BOOL Connected;

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Overlapped.hEvent)
        goto Exit;

    Connected = ConnectNamedPipe(Reader->Pipe, &Overlapped);
    if (!Connected)
    {
        DWORD Error = GetLastError();

        if (Error == ERROR_IO_PENDING)
            Connected = GetOverlappedResult(Reader->Pipe, &Overlapped, &Read, TRUE);
        else
            Connected = (Error == ERROR_PIPE_CONNECTED);
    }

    /* Only the process the shell started may speak for this app */
    if (!Connected || !GetNamedPipeClientProcessId(Reader->Pipe, &ClientId) || ClientId != Reader->ProcessId)
        goto Exit;

    Post = new MOBILE_APP_POST();
    ZeroMemory(Post, sizeof(*Post));
    Post->AppId = Reader->AppId;
    if (!Reader->Host->PostUiMessage(Reader->Owner, MOBILE_UI_APP_CONNECTED, Post))
        delete Post;

    for (;;)
    {
        ResetEvent(Overlapped.hEvent);
        if (!ReadFile(Reader->Pipe, &Message, sizeof(Message), &Read, &Overlapped))
        {
            if (GetLastError() != ERROR_IO_PENDING)
                break;
            if (!GetOverlappedResult(Reader->Pipe, &Overlapped, &Read, TRUE))
                break;
        }

        if (Read != sizeof(Message))
            continue;

        Post = new MOBILE_APP_POST();
        Post->AppId = Reader->AppId;
        Post->Message = Message;
        if (!Reader->Host->PostUiMessage(Reader->Owner, MOBILE_UI_APP_MESSAGE, Post))
            delete Post;
    }

Exit:
    if (Overlapped.hEvent)
        CloseHandle(Overlapped.hEvent);

    Post = new MOBILE_APP_POST();
    ZeroMemory(Post, sizeof(*Post));
    Post->AppId = Reader->AppId;
    if (!Reader->Host->PostUiMessage(Reader->Owner, MOBILE_UI_APP_EXITED, Post))
        delete Post;
    delete Reader;
    return 0;
}

BOOL
CMobileUI::WriteApp(
    _In_ PMOBILE_APP App,
    _In_ const APP_MESSAGE *Message)
{
    OVERLAPPED Overlapped;
    DWORD Written = 0;
    BOOL Result;

    if (App->Pipe == INVALID_HANDLE_VALUE || App->Closing)
        return FALSE;

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Overlapped.hEvent)
        return FALSE;

    Result = WriteFile(App->Pipe, Message, sizeof(*Message), &Written, &Overlapped);
    if (!Result && GetLastError() == ERROR_IO_PENDING)
        Result = GetOverlappedResult(App->Pipe, &Overlapped, &Written, TRUE);

    CloseHandle(Overlapped.hEvent);
    return Result && Written == sizeof(*Message);
}

VOID
CMobileUI::ActivateApp(
    _In_ PMOBILE_APP App)
{
    APP_MESSAGE Message;
    FLOAT Scale = m_Host->GetEngine()->GetScale();
    FLOAT Width = m_AppRect.Right - m_AppRect.Left;
    FLOAT Height = m_AppRect.Bottom - m_AppRect.Top;
    FLOAT RenderScale = App->Def.Legacy ? 1.0f : Scale;

    ZeroMemory(&Message, sizeof(Message));
    Message.Id = AppMsgActivate;
    Message.Int[0] = lroundf(Width * RenderScale);
    Message.Int[1] = lroundf(Height * RenderScale);
    Message.Float[0] = RenderScale;
    Message.Float[1] = Width;
    Message.Float[2] = Height;
    WriteApp(App, &Message);
    PlaceApp(App, FALSE);
}

VOID
CMobileUI::PlaceApp(
    _In_ PMOBILE_APP App,
    _In_ BOOL Reveal)
{
    UixPtr<IDCompositionAnimation> Animation;
    UixPtr<IDCompositionDevice> Device;
    FLOAT Scale = m_Host->GetEngine()->GetScale();
    FLOAT X = static_cast<FLOAT>(lroundf(m_AppRect.Left * Scale));
    FLOAT Y = static_cast<FLOAT>(lroundf(m_AppRect.Top * Scale));
    FLOAT FromX = 0.0f;
    FLOAT FromY = 0.0f;
    FLOAT Seconds = 0.0f;
    FLOAT Distance;
    PCWSTR Spec = m_Home ? m_Home->GetAttribute(NULL, L"AppReveal") : NULL;

    if (!App->Visual)
        return;

    if (!App->Shown)
    {
        App->Visual->SetClip(UixD2D::Rect(0.0f, 0.0f, 0.0f, 0.0f));
        return;
    }

    App->Visual->SetClip(static_cast<IDCompositionClip *>(NULL));
    App->Visual->SetOffsetX(X);

    /* Legacy apps render at base resolution and the compositor scales them */
    if (App->Def.Legacy)
    {
        App->Visual->SetTransform(UixD2D::Scale(Scale, Scale));
        App->Visual->SetBitmapInterpolationMode(UIX_BITMAP_INTERPOLATION_LINEAR);
    }
    else
    {
        App->Visual->SetTransform(UixD2D::Identity());
    }

    /* Ease out from the skin's offset, evaluated by the compositor */
    if (Reveal && Spec && swscanf(Spec, L"%f,%f,%f", &FromX, &FromY, &Seconds) == 3 && Seconds > 0.0f &&
        SUCCEEDED(m_Host->GetEngine()->GetCompositionDevice(Device.Put())))
    {
        Distance = -FromY * Scale;
        if (Distance != 0.0f && SUCCEEDED(Device->CreateAnimation(Animation.Put())))
        {
            Animation->AddCubic(0.0, Y - Distance, 2.0f * Distance / Seconds, -Distance / (Seconds * Seconds), 0.0f);
            Animation->End(Seconds, Y);
            App->Visual->SetOffsetY(Animation.Get());
            return;
        }
    }

    App->Visual->SetOffsetY(Y);
}

VOID
CMobileUI::ShowApp(
    _In_ PMOBILE_APP App)
{
    if (m_Active && m_Active != App)
    {
        m_Active->Shown = FALSE;
        PlaceApp(m_Active, FALSE);
    }

    m_Active = App;
    App->Shown = TRUE;
    PlaceApp(App, TRUE);
    UpdateChrome();
    m_Host->RequestFrame();
}

VOID
CMobileUI::CloseApp(
    _In_ PMOBILE_APP App)
{
    APP_MESSAGE Message;

    /* Ask nicely first, WriteApp refuses once Closing is set */
    if (App->Process)
    {
        ZeroMemory(&Message, sizeof(Message));
        Message.Id = AppMsgTerminate;
        WriteApp(App, &Message);
    }
    InterlockedExchange(&App->Closing, 1);

    if (App->Process)
        WaitForSingleObject(App->Process, 500);

    /* Closing the job kills anything still running inside it */
    if (App->Job)
        CloseHandle(App->Job);
    if (App->Pipe != INVALID_HANDLE_VALUE)
    {
        CancelIoEx(App->Pipe, NULL);
        DisconnectNamedPipe(App->Pipe);
    }
    if (App->Thread)
    {
        WaitForSingleObject(App->Thread, 2000);
        CloseHandle(App->Thread);
    }
    if (App->Pipe != INVALID_HANDLE_VALUE)
        CloseHandle(App->Pipe);
    if (App->Process)
        CloseHandle(App->Process);

    if (App->Visual)
        m_AppLayer->RemoveVisual(App->Visual.Get());

    if (m_PopupOpen && m_PopupApp == App->Id)
    {
        m_PopupApp = 0;
        ClosePopup(-1);
    }

    for (SIZE_T Index = 0; Index < m_Apps.size(); Index++)
    {
        if (m_Apps[Index] == App)
        {
            m_Apps.erase(m_Apps.begin() + Index);
            break;
        }
    }

    if (m_Active == App)
    {
        m_Active = NULL;
        UpdateChrome();
    }

    delete App;
    m_Host->RequestFrame();
}

VOID
CMobileUI::PresentApp(
    _In_ PMOBILE_APP App)
{
    HRESULT Status;

    if (!App->Shared || !App->Surface)
        return;

    if (App->Mutex)
    {
        Status = App->Mutex->AcquireSync(1, 100);
        if (Status != S_OK)
            return;
    }

    m_Host->GetEngine()->CopyToSurface(App->Surface.Get(), App->Shared.Get(), App->Width, App->Height);

    if (App->Mutex)
        App->Mutex->ReleaseSync(0);
    m_Host->RequestFrame();
}

VOID
CMobileUI::AppMessage(
    _In_ PMOBILE_APP App,
    _In_ const APP_MESSAGE *Message)
{
    UixPtr<ID3D11Device> Device;
    UixPtr<IDCompositionDevice> Composition;

    switch (Message->Id)
    {
        case AppMsgReady:
            App->Shared.Reset();
            App->Mutex.Reset();
            App->Surface.Reset();

            if (FAILED(m_Host->GetEngine()->GetDirect3DDevice(Device.Put())) ||
                FAILED(Device->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(Message->Handle)),
                                                  __uuidof(ID3D11Texture2D), App->Shared.PutVoid())))
            {
                return;
            }

            if (Message->Flags & APP_READY_KEYED_MUTEX)
                App->Shared.As(&App->Mutex);

            App->Width = static_cast<UINT>(Message->Int[0]);
            App->Height = static_cast<UINT>(Message->Int[1]);

            if (SUCCEEDED(m_Host->GetEngine()->GetCompositionDevice(Composition.Put())) &&
                SUCCEEDED(Composition->CreateSurface(App->Width, App->Height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                     DXGI_ALPHA_MODE_PREMULTIPLIED, App->Surface.Put())))
            {
                App->Visual->SetContent(App->Surface.Get());
            }
            break;

        case AppMsgPresented:
            PresentApp(App);
            break;

        case AppMsgChrome:
            StringCchCopyW(App->Title, ARRAYSIZE(App->Title), Message->Text[0]);
            StringCchCopyW(App->SoftKeys[0], ARRAYSIZE(App->SoftKeys[0]), Message->Text[1]);
            StringCchCopyW(App->SoftKeys[1], ARRAYSIZE(App->SoftKeys[1]), Message->Text[2]);
            App->CloseKind = static_cast<SHELL_CLOSE_KIND>(Message->Int[0]);
            if (App == m_Active)
                UpdateChrome();
            break;

        case AppMsgRequestHide:
            if (App == m_Active)
                GoHome();
            break;

        case AppMsgRequestClose:
            CloseApp(App);
            break;

        case AppMsgMenu:
        {
            WCHAR Items[MAX_PATH];

            StringCchCopyNW(Items, ARRAYSIZE(Items), Message->Path, ARRAYSIZE(Message->Path));
            if (App == m_Active)
            {
                ShowPopup(App->Id, static_cast<APP_MENU_ANCHOR>(Message->Int[0]), Message->Float[0], Message->Float[1], Items);
            }
            else
            {
                APP_MESSAGE Pick;

                ZeroMemory(&Pick, sizeof(Pick));
                Pick.Id = AppMsgMenuPick;
                Pick.Int[0] = -1;
                WriteApp(App, &Pick);
            }
            break;
        }

        case AppMsgNotify:
        {
            WCHAR Title[APP_TEXT_MAX];
            WCHAR Body[MAX_PATH];

            StringCchCopyNW(Title, ARRAYSIZE(Title), Message->Text[0], APP_TEXT_MAX);
            StringCchCopyNW(Body, ARRAYSIZE(Body), Message->Path, ARRAYSIZE(Message->Path));
            PostNote(Title, Body);
            break;
        }

        case AppMsgInputPanel:
            if (App == m_Active)
                ShowSip(Message->Int[0] != 0);
            break;

        case AppMsgLaunch:
        {
            WCHAR Id[APP_TEXT_MAX];
            WCHAR Document[MAX_PATH];

            /* Apps may only ask for other registered apps, optionally with a document */
            StringCchCopyNW(Id, ARRAYSIZE(Id), Message->Text[0], APP_TEXT_MAX);
            StringCchCopyNW(Document, ARRAYSIZE(Document), Message->Path, ARRAYSIZE(Message->Path));
            if (!Launch(Id, Document))
                PostNote(Id, L"@NotInstalled");
            break;
        }

        case AppMsgRequest:
        {
            static PCWSTR const Allowed[] = { L"theme:", L"theme.next", L"status.refresh", L"home" };
            WCHAR Request[APP_TEXT_MAX];

            /* Only a short list of shell commands is open to apps */
            StringCchCopyNW(Request, ARRAYSIZE(Request), Message->Text[0], APP_TEXT_MAX);
            for (PCWSTR Prefix : Allowed)
            {
                if (_wcsnicmp(Request, Prefix, wcslen(Prefix)) == 0)
                {
                    m_Host->ExecuteCommand(Request);
                    break;
                }
            }
            break;
        }

        default:
            break;
    }
}

VOID
STDMETHODCALLTYPE
CMobileUI::UiMessage(
    _In_ ULONG Code,
    _In_opt_ PVOID Data)
{
    PMOBILE_APP_POST Post = static_cast<PMOBILE_APP_POST>(Data);
    PMOBILE_APP App;

    if (!Post)
        return;

    App = FindApp(Post->AppId);
    if (App)
    {
        if (Code == MOBILE_UI_APP_MESSAGE)
            AppMessage(App, &Post->Message);
        else if (Code == MOBILE_UI_APP_CONNECTED)
            ActivateApp(App);
        else if (Code == MOBILE_UI_APP_EXITED && !App->Closing)
            CloseApp(App);
    }
    delete Post;
}

VOID
STDMETHODCALLTYPE
CMobileUI::Shutdown(VOID)
{
    while (!m_Apps.empty())
        CloseApp(m_Apps.back());

    m_Sip.Reset();
    m_Notify.Reset();
    m_Popup.Reset();
    m_Hold.Reset();

    if (m_Endpoint != MSG_ENDPOINT_NONE)
        m_Host->GetSession()->CloseEndpoint(m_Endpoint);
    m_Endpoint = MSG_ENDPOINT_NONE;
    m_Home.Reset();
    m_Menu.Reset();
}

EXTERN_C
SHELLCOMP_API
HRESULT
WINAPI
ShellCreateComponent(
    _Outptr_ IShellComponent **Component)
{
    *Component = new CMobileUI();
    return S_OK;
}

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE Instance,
    _In_ DWORD Reason,
    _In_opt_ PVOID Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);

    if (Reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(Instance);
    return TRUE;
}
