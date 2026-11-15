/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     App chrome server: title bar, status area and soft key bar
 */

#define SHELLCOMP_EXPORTS
#include <devstatus.h>
#include <shellcomp.h>
#include <shellmsg.h>
#include <uixptr.h>
#include <strsafe.h>

/*
 * Skin contract (chrome.uix):
 *   Bindings: Title, SoftKeyLeft, SoftKeyRight, CloseLabel, Time, Battery, BatteryLevel,
 *             BatteryBrush, Bar1..Bar5, VolumeGlyph; root BatteryFillWidth
 *   Elements: BatteryCharging, NetActive, NetOff, NotifyIcon
 *   Elements: CloseButton (shown when the app asks for one), Clock (shown in apps)
 *   Commands: start, softkey.left, softkey.right, softkey.center, close
 */

class CShellChrome : public IShellComponent
{
public:
    CShellChrome()
        : m_Host(NULL),
          m_Endpoint(MSG_ENDPOINT_NONE),
          m_HostEndpoint(MSG_ENDPOINT_NONE),
          m_CloseKind(ShellCloseNone),
          m_Mode(ShellModeHome),
          m_Notifications(0)
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
        _In_ FLOAT Delta) override
    {
        UNREFERENCED_PARAMETER(X);
        UNREFERENCED_PARAMETER(Y);
        UNREFERENCED_PARAMETER(Delta);
        return FALSE;
    }

    BOOL STDMETHODCALLTYPE Command(
        _In_z_ PCWSTR Command) override
    {
        if (_wcsicmp(Command, L"status.refresh") != 0)
            return FALSE;
        Refresh(TRUE);
        return TRUE;
    }

    VOID STDMETHODCALLTYPE Tick(VOID) override;

    VOID STDMETHODCALLTYPE UiMessage(
        _In_ ULONG Code,
        _In_opt_ PVOID Data) override
    {
        UNREFERENCED_PARAMETER(Code);
        UNREFERENCED_PARAMETER(Data);
    }

    VOID STDMETHODCALLTYPE Shutdown(VOID) override;

private:
    static
    HRESULT
    CALLBACK
    OnMessage(
        _In_opt_ PVOID Context,
        _In_reads_bytes_(Size) const VOID *Payload,
        _In_ ULONG Size);

    VOID
    HandleMessage(
        _In_ const SHELL_MESSAGE *Message);

    VOID
    Notify(
        _In_ SHELL_MSG_ID Id,
        _In_ ULONG Value);

    VOID
    ApplyState(VOID);

    VOID
    Refresh(
        _In_ BOOL Everything);

    DEV_STATUS m_Status = {};
    ULONG m_Ticks = 0;

    IShellHost *m_Host;
    UixPtr<IUixScene> m_Scene;
    MSG_ENDPOINT m_Endpoint;
    MSG_ENDPOINT m_HostEndpoint;
    WCHAR m_Title[SHELL_TEXT_MAX];
    WCHAR m_SoftKeys[2][SHELL_TEXT_MAX];
    SHELL_CLOSE_KIND m_CloseKind;
    SHELL_MODE m_Mode;
    ULONG m_Notifications;
};

HRESULT
STDMETHODCALLTYPE
CShellChrome::Initialize(
    _In_ IShellHost *Host)
{
    HRESULT Status;

    m_Host = Host;
    m_Title[0] = UNICODE_NULL;
    m_SoftKeys[0][0] = UNICODE_NULL;
    m_SoftKeys[1][0] = UNICODE_NULL;

    Status = m_Host->GetSession()->CreateEndpoint(OnMessage, this, &m_Endpoint);
    if (FAILED(Status))
        return Status;

    Status = m_Host->GetSession()->PublishEndpoint(SHELL_ENDPOINT_CHROME, m_Endpoint);
    if (FAILED(Status))
        return Status;

    return Reload();
}

HRESULT
STDMETHODCALLTYPE
CShellChrome::Reload(VOID)
{
    UixPtr<IUixScene> Scene;
    WCHAR Path[MAX_PATH];
    HRESULT Status;

    if (!m_Host->GetSkinFile(L"chrome.uix", Path, ARRAYSIZE(Path)))
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    Status = m_Host->GetEngine()->CreateScene(Path, Scene.Put());
    if (FAILED(Status))
        return Status;

    Status = Scene->Attach(m_Host->GetBand(ShellBandChrome));
    if (FAILED(Status))
        return Status;

    m_Scene = Scene;
    ApplyState();
    Refresh(TRUE);
    Layout();
    return S_OK;
}

VOID
STDMETHODCALLTYPE
CShellChrome::Layout(VOID)
{
    FLOAT Width;
    FLOAT Height;

    if (!m_Scene)
        return;

    m_Host->GetCanvas(&Width, &Height);
    m_Scene->Layout(Width, Height);
    m_Host->RequestFrame();
}

VOID
CShellChrome::ApplyState(VOID)
{
    if (!m_Scene)
        return;

    m_Scene->SetBinding(L"Title", m_Title);
    m_Scene->SetBinding(L"SoftKeyLeft", m_SoftKeys[0]);
    m_Scene->SetBinding(L"SoftKeyRight", m_SoftKeys[1]);
    m_Scene->SetBinding(L"CloseLabel", (m_CloseKind == ShellCloseOk) ? L"@CloseOk" : L"@CloseX");
    m_Scene->SetVisible(L"CloseButton", m_CloseKind != ShellCloseNone);
    /* Today shows its own time; apps get the clock only when no close button needs the room */
    m_Scene->SetVisible(L"Clock", m_Mode == ShellModeApp && m_CloseKind == ShellCloseNone);
    m_Scene->SetVisible(L"NotifyIcon", m_Notifications != 0);
    /* Skins whose soft keys live in an app bar hide it on the home screen */
    m_Scene->SetVisible(L"AppBar", m_Mode == ShellModeApp);
    m_Host->RequestFrame();
}

VOID
STDMETHODCALLTYPE
CShellChrome::Tick(VOID)
{
    Refresh(FALSE);
}

VOID
CShellChrome::Refresh(
    _In_ BOOL Everything)
{
    WCHAR Text[32];
    WCHAR Name[8];
    ULONG Percent;
    ULONG Bars;
    PCWSTR Glyph;

    if (!m_Scene)
        return;

    if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, NULL, NULL, Text, ARRAYSIZE(Text)))
        m_Scene->SetBinding(L"Time", Text);

    /* Power and volume are cheap, the adapter walk runs every few seconds */
    DevQueryPower(&m_Status);
    DevQueryVolume(&m_Status);
    if (Everything || (m_Ticks++ % 5) == 0)
        DevQueryNetwork(&m_Status);

    Percent = (m_Status.BatteryPercent == DEV_BATTERY_UNKNOWN) ? 100 : m_Status.BatteryPercent;
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu%%", Percent);
    m_Scene->SetBinding(L"Battery", Text);
    /* The skin says how wide a full battery is, the fill is a share of that */
    PCWSTR Full = m_Scene->GetAttribute(NULL, L"BatteryFillWidth");
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%.2f", (Full ? _wtof(Full) : 10.0) * Percent / 100.0);
    m_Scene->SetBinding(L"BatteryLevel", Text);
    m_Scene->SetBinding(L"BatteryBrush", (m_Status.HasBattery && Percent <= 20 && !m_Status.OnAc) ? L"@BatteryLow" : L"@TitleText");
    m_Scene->SetVisible(L"BatteryCharging", m_Status.OnAc);

    /* Five bars from Wi-Fi quality; wired counts as full, nothing as none */
    Bars = (m_Status.Network == DevNetworkNone) ? 0 : (m_Status.Quality + 19) / 20;
    if (m_Status.Network == DevNetworkWireless && Bars == 0)
        Bars = 1;
    for (ULONG Bar = 1; Bar <= 5; Bar++)
    {
        StringCchPrintfW(Name, ARRAYSIZE(Name), L"Bar%lu", Bar);
        m_Scene->SetBinding(Name, (Bar <= Bars) ? L"@TitleText" : L"@TitleDim");
    }
    m_Scene->SetVisible(L"NetActive", m_Status.Network != DevNetworkNone);
    m_Scene->SetVisible(L"NetOff", m_Status.Network == DevNetworkNone);

    if (m_Status.Volume < 0 || m_Status.Muted)
        Glyph = L"\xE74F";
    else if (m_Status.Volume == 0)
        Glyph = L"\xE992";
    else if (m_Status.Volume < 34)
        Glyph = L"\xE993";
    else if (m_Status.Volume < 67)
        Glyph = L"\xE994";
    else
        Glyph = L"\xE995";
    m_Scene->SetBinding(L"VolumeGlyph", Glyph);

    m_Host->RequestFrame();
}

VOID
CShellChrome::Notify(
    _In_ SHELL_MSG_ID Id,
    _In_ ULONG Value)
{
    SHELL_MESSAGE Message;

    if (m_HostEndpoint == MSG_ENDPOINT_NONE &&
        FAILED(m_Host->GetSession()->OpenEndpoint(SHELL_ENDPOINT_HOST, &m_HostEndpoint)))
    {
        return;
    }

    ZeroMemory(&Message, sizeof(Message));
    Message.Id = Id;
    Message.Value = Value;
    m_Host->GetSession()->Send(m_HostEndpoint, MsgPriorityHigh, &Message, sizeof(Message));
}

BOOL
STDMETHODCALLTYPE
CShellChrome::PointerEvent(
    _In_ UIX_POINTER Kind,
    _In_ FLOAT X,
    _In_ FLOAT Y)
{
    PCWSTR Command = NULL;
    BOOL Handled;

    if (!m_Scene)
        return FALSE;

    Handled = m_Scene->PointerEvent(Kind, X, Y, &Command);
    if (Handled)
        m_Host->RequestFrame();
    if (!Command || !Command[0])
        return Handled;

    if (_wcsicmp(Command, L"start") == 0)
        Notify(ShellMsgStartInvoked, 0);
    else if (_wcsicmp(Command, L"softkey.left") == 0)
        Notify(ShellMsgSoftKeyInvoked, ShellSoftKeyLeft);
    else if (_wcsicmp(Command, L"softkey.right") == 0)
        Notify(ShellMsgSoftKeyInvoked, ShellSoftKeyRight);
    else if (_wcsicmp(Command, L"softkey.center") == 0)
        Notify(ShellMsgSoftKeyInvoked, ShellSoftKeyCenter);
    else if (_wcsicmp(Command, L"close") == 0)
        Notify(ShellMsgCloseInvoked, m_CloseKind);
    else
        m_Host->ExecuteCommand(Command);
    return TRUE;
}

HRESULT
CALLBACK
CShellChrome::OnMessage(
    _In_opt_ PVOID Context,
    _In_reads_bytes_(Size) const VOID *Payload,
    _In_ ULONG Size)
{
    CShellChrome *This = static_cast<CShellChrome *>(Context);

    if (!This || Size < sizeof(SHELL_MESSAGE))
        return E_INVALIDARG;

    This->HandleMessage(static_cast<const SHELL_MESSAGE *>(Payload));
    return S_OK;
}

VOID
CShellChrome::HandleMessage(
    _In_ const SHELL_MESSAGE *Message)
{
    switch (Message->Id)
    {
        case ShellMsgSetTitle:
            StringCchCopyW(m_Title, ARRAYSIZE(m_Title), Message->Text[0]);
            break;

        case ShellMsgSetSoftKeys:
            StringCchCopyW(m_SoftKeys[0], ARRAYSIZE(m_SoftKeys[0]), Message->Text[0]);
            StringCchCopyW(m_SoftKeys[1], ARRAYSIZE(m_SoftKeys[1]), Message->Text[1]);
            break;

        case ShellMsgSetCloseButton:
            m_CloseKind = static_cast<SHELL_CLOSE_KIND>(Message->Value);
            break;

        case ShellMsgSetMode:
            m_Mode = static_cast<SHELL_MODE>(Message->Value);
            break;

        case ShellMsgSetNotify:
            m_Notifications = Message->Value;
            break;

        default:
            return;
    }
    ApplyState();
}

VOID
STDMETHODCALLTYPE
CShellChrome::Shutdown(VOID)
{
    if (m_Endpoint != MSG_ENDPOINT_NONE)
        m_Host->GetSession()->CloseEndpoint(m_Endpoint);
    m_Endpoint = MSG_ENDPOINT_NONE;
    m_Scene.Reset();
}

EXTERN_C
SHELLCOMP_API
HRESULT
WINAPI
ShellCreateComponent(
    _Outptr_ IShellComponent **Component)
{
    *Component = new CShellChrome();
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
