/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Base for out of process apps: pipe, shared texture and UIX scene
 */

#include "appclient.h"
#include <strsafe.h>

CAppClient::CAppClient()
    : m_Pipe(INVALID_HANDLE_VALUE),
      m_Running(TRUE),
      m_Scale(1.0f)
{
}

CAppClient::~CAppClient()
{
    if (m_Pipe != INVALID_HANDLE_VALUE)
        CloseHandle(m_Pipe);
}

HRESULT
CAppClient::Initialize(
    _In_ INT Count,
    _In_reads_(Count) PWSTR *Arguments)
{
    PCWSTR PipeName = NULL;
    DWORD Mode = PIPE_READMODE_MESSAGE;
    HRESULT Status;
    INT Index;

    m_Arguments.assign(Arguments, Arguments + Count);
    for (Index = 1; Index + 1 < Count; Index++)
    {
        if (_wcsicmp(Arguments[Index], L"--pipe") == 0)
            PipeName = Arguments[++Index];
        else if (_wcsicmp(Arguments[Index], L"--theme") == 0)
            m_ThemeFile = Arguments[++Index];
        else if (_wcsicmp(Arguments[Index], L"--markup") == 0)
            m_MarkupFile = Arguments[++Index];
    }

    if (!PipeName || m_MarkupFile.empty())
        return E_INVALIDARG;

    for (Index = 0; Index < 50; Index++)
    {
        m_Pipe = CreateFileW(PipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (m_Pipe != INVALID_HANDLE_VALUE)
            break;
        if (GetLastError() != ERROR_PIPE_BUSY && GetLastError() != ERROR_FILE_NOT_FOUND)
            return HRESULT_FROM_WIN32(GetLastError());
        WaitNamedPipeW(PipeName, 100);
    }

    if (m_Pipe == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(ERROR_PIPE_NOT_CONNECTED);
    SetNamedPipeHandleState(m_Pipe, &Mode, NULL, NULL);

    Status = UixCreateEngine(m_Engine.Put());
    if (FAILED(Status))
        return Status;

    Status = m_Engine->Startup(NULL);
    if (FAILED(Status))
        return Status;

    if (!m_ThemeFile.empty())
        m_Engine->LoadTheme(m_ThemeFile.c_str());

    Status = m_Engine->CreateScene(m_MarkupFile.c_str(), m_Scene.Put());
    if (FAILED(Status))
        return Status;

    Status = m_Engine->GetDirect3DDevice(m_Device.Put());
    if (FAILED(Status))
        return Status;
    m_Device->GetImmediateContext(m_Immediate.Put());

    return m_Engine->GetDirect2DContext(m_Context.Put());
}

BOOL
CAppClient::Write(
    _In_ const APP_MESSAGE *Message)
{
    DWORD Written = 0;

    return WriteFile(m_Pipe, Message, sizeof(*Message), &Written, NULL) && Written == sizeof(*Message);
}

HRESULT
CAppClient::Activate(
    _In_ const APP_MESSAGE *Message)
{
    UixPtr<IDXGIResource> Resource;
    UixPtr<IDXGISurface> Surface;
    D3D11_TEXTURE2D_DESC Desc;
    D2D1_BITMAP_PROPERTIES1 Properties;
    APP_MESSAGE Ready;
    HANDLE Shared = NULL;
    HRESULT Status;

    if (Message->Int[0] <= 0 || Message->Int[1] <= 0)
        return E_INVALIDARG;

    m_Target.Reset();
    m_Mutex.Reset();
    m_Texture.Reset();
    m_Scale = Message->Float[0];
    m_Engine->SetScale(m_Scale);

    ZeroMemory(&Desc, sizeof(Desc));
    Desc.Width = static_cast<UINT>(Message->Int[0]);
    Desc.Height = static_cast<UINT>(Message->Int[1]);
    Desc.MipLevels = 1;
    Desc.ArraySize = 1;
    Desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Desc.SampleDesc.Count = 1;
    Desc.Usage = D3D11_USAGE_DEFAULT;
    Desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    /* Fall back to a plain shared texture where keyed mutexes are missing */
    Status = m_Device->CreateTexture2D(&Desc, NULL, m_Texture.Put());
    if (FAILED(Status))
    {
        Desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        Status = m_Device->CreateTexture2D(&Desc, NULL, m_Texture.Put());
        if (FAILED(Status))
            return Status;
    }
    else
    {
        m_Texture.As(&m_Mutex);
    }

    Status = m_Texture.As(&Resource);
    if (SUCCEEDED(Status))
        Status = Resource->GetSharedHandle(&Shared);
    if (SUCCEEDED(Status))
        Status = m_Texture.As(&Surface);
    if (FAILED(Status))
        return Status;

    Properties = UixD2D::TargetProperties();
    Status = m_Context->CreateBitmapFromDxgiSurface(Surface.Get(), &Properties, m_Target.Put());
    if (FAILED(Status))
        return Status;

    m_Scene->Layout(Message->Float[1], Message->Float[2]);

    ZeroMemory(&Ready, sizeof(Ready));
    Ready.Id = AppMsgReady;
    Ready.Flags = m_Mutex ? APP_READY_KEYED_MUTEX : 0;
    Ready.Handle = reinterpret_cast<ULONG_PTR>(Shared);
    Ready.Int[0] = static_cast<LONG>(Desc.Width);
    Ready.Int[1] = static_cast<LONG>(Desc.Height);
    Write(&Ready);

    OnActivate();

    /* A document from the launch command line opens once, after the first activation */
    if (!m_Opened)
    {
        m_Opened = TRUE;
        if (Argument(L"open"))
            OnOpenDocument(Argument(L"open"));
    }
    Present();
    return S_OK;
}

VOID
CAppClient::Present(VOID)
{
    APP_MESSAGE Presented;
    D2D1_COLOR_F Paper;

    if (!m_Target)
        return;

    if (m_Mutex && m_Mutex->AcquireSync(0, 1000) != S_OK)
        return;

    m_Context->SetTarget(m_Target.Get());
    m_Context->BeginDraw();
    Paper = UixD2D::Color(1.0f, 1.0f, 1.0f);
    static_cast<ID2D1RenderTarget *>(m_Context.Get())->Clear(&Paper);
    m_Scene->RenderTo(m_Context.Get(), m_Scale);
    m_Context->EndDraw(NULL, NULL);
    m_Context->SetTarget(NULL);
    m_Immediate->Flush();

    if (m_Mutex)
        m_Mutex->ReleaseSync(1);

    ZeroMemory(&Presented, sizeof(Presented));
    Presented.Id = AppMsgPresented;
    Write(&Presented);
}

VOID
CAppClient::SetChrome(
    _In_z_ PCWSTR Title,
    _In_z_ PCWSTR LeftSoftKey,
    _In_z_ PCWSTR RightSoftKey,
    _In_ SHELL_CLOSE_KIND CloseKind)
{
    APP_MESSAGE Message;

    ZeroMemory(&Message, sizeof(Message));
    Message.Id = AppMsgChrome;
    Message.Int[0] = CloseKind;
    StringCchCopyW(Message.Text[0], APP_TEXT_MAX, Title);
    StringCchCopyW(Message.Text[1], APP_TEXT_MAX, LeftSoftKey);
    StringCchCopyW(Message.Text[2], APP_TEXT_MAX, RightSoftKey);
    Write(&Message);
}

VOID
CAppClient::RequestClose(VOID)
{
    APP_MESSAGE Message;

    ZeroMemory(&Message, sizeof(Message));
    Message.Id = AppMsgRequestClose;
    Write(&Message);
}

VOID
CAppClient::RequestHide(VOID)
{
    APP_MESSAGE Message;

    ZeroMemory(&Message, sizeof(Message));
    Message.Id = AppMsgRequestHide;
    Write(&Message);
}

VOID
CAppClient::OnCommand(
    _In_z_ PCWSTR Command)
{
    if (_wcsnicmp(Command, L"launch:", 7) == 0)
        Launch(Command + 7);
    else if (_wcsicmp(Command, L"close") == 0)
        RequestClose();
}

VOID
CAppClient::ShowMenu(
    _In_ APP_MENU_ANCHOR Anchor,
    _In_ FLOAT X,
    _In_ FLOAT Y,
    _In_z_ PCWSTR Items)
{
    APP_MESSAGE Message;

    ZeroMemory(&Message, sizeof(Message));
    Message.Id = AppMsgMenu;
    Message.Int[0] = Anchor;
    Message.Float[0] = X;
    Message.Float[1] = Y;
    StringCchCopyW(Message.Path, ARRAYSIZE(Message.Path), Items);
    Write(&Message);
}

VOID
CAppClient::Notify(
    _In_z_ PCWSTR Title,
    _In_z_ PCWSTR Body)
{
    APP_MESSAGE Message;

    ZeroMemory(&Message, sizeof(Message));
    Message.Id = AppMsgNotify;
    StringCchCopyW(Message.Text[0], APP_TEXT_MAX, Title);
    StringCchCopyW(Message.Path, ARRAYSIZE(Message.Path), Body);
    Write(&Message);
}

VOID
CAppClient::ShowInputPanel(
    _In_ BOOL Show)
{
    APP_MESSAGE Message;

    ZeroMemory(&Message, sizeof(Message));
    Message.Id = AppMsgInputPanel;
    Message.Int[0] = Show ? 1 : 0;
    Write(&Message);
}

VOID
CAppClient::Launch(
    _In_z_ PCWSTR AppId,
    _In_opt_z_ PCWSTR Document)
{
    APP_MESSAGE Message;

    ZeroMemory(&Message, sizeof(Message));
    Message.Id = AppMsgLaunch;
    StringCchCopyW(Message.Text[0], APP_TEXT_MAX, AppId);
    if (Document)
        StringCchCopyW(Message.Path, ARRAYSIZE(Message.Path), Document);
    Write(&Message);
}

VOID
CAppClient::Request(
    _In_z_ PCWSTR Command)
{
    APP_MESSAGE Message;

    ZeroMemory(&Message, sizeof(Message));
    Message.Id = AppMsgRequest;
    StringCchCopyW(Message.Text[0], APP_TEXT_MAX, Command);
    Write(&Message);
}

PCWSTR
CAppClient::Argument(
    _In_z_ PCWSTR Name) const
{
    for (SIZE_T Index = 1; Index + 1 < m_Arguments.size(); Index++)
    {
        if (m_Arguments[Index].size() > 2 && _wcsicmp(m_Arguments[Index].c_str() + 2, Name) == 0)
            return m_Arguments[Index + 1].c_str();
    }
    return NULL;
}

VOID
CAppClient::OnKey(
    _In_ UINT VirtualKey)
{
    PCWSTR Command;
    std::wstring Copy;

    if (VirtualKey == VK_UP || VirtualKey == VK_LEFT)
    {
        m_Scene->MoveSelection(-1);
    }
    else if (VirtualKey == VK_DOWN || VirtualKey == VK_RIGHT)
    {
        m_Scene->MoveSelection(1);
    }
    else if (VirtualKey == VK_RETURN)
    {
        Command = m_Scene->GetSelectedCommand();
        if (Command)
        {
            Copy = Command;
            OnCommand(Copy.c_str());
        }
    }
    Present();
}

VOID
CAppClient::Pointer(
    _In_ const APP_MESSAGE *Message)
{
    UIX_POINTER Kind = static_cast<UIX_POINTER>(Message->Int[0]);
    PCWSTR Command = NULL;
    std::wstring Copy;

    m_PointerX = Message->Float[0];
    m_PointerY = Message->Float[1];
    if (!m_Scene->PointerEvent(Kind, m_PointerX, m_PointerY, &Command))
        return;

    if (Command && Command[0])
    {
        Copy = Command;
        if (Kind == UixPointerHold)
            OnHold(Copy.c_str(), m_PointerX, m_PointerY);
        else
            OnCommand(Copy.c_str());
    }
    Present();
}

INT
CAppClient::Run(VOID)
{
    APP_MESSAGE Message;
    DWORD Read;

    while (m_Running)
    {
        if (!ReadFile(m_Pipe, &Message, sizeof(Message), &Read, NULL) || Read != sizeof(Message))
            break;

        switch (Message.Id)
        {
            case AppMsgActivate:
                Activate(&Message);
                break;

            case AppMsgPointer:
                Pointer(&Message);
                break;

            case AppMsgKey:
                OnKey(static_cast<UINT>(Message.Int[0]));
                break;

            case AppMsgSoftKey:
                OnSoftKey(static_cast<SHELL_SOFTKEY>(Message.Int[0]));
                Present();
                break;

            case AppMsgCloseButton:
                OnCloseButton();
                Present();
                break;

            case AppMsgTheme:
                Message.Path[MAX_PATH - 1] = UNICODE_NULL;
                if (SUCCEEDED(m_Engine->LoadTheme(Message.Path)))
                    Present();
                break;

            case AppMsgTerminate:
                m_Running = FALSE;
                break;

            case AppMsgChar:
                OnChar(static_cast<WCHAR>(Message.Int[0]));
                Present();
                break;

            case AppMsgWheel:
                if (m_Scene->Wheel(Message.Float[0], Message.Float[1], Message.Float[2]))
                    Present();
                break;

            case AppMsgMenuPick:
                OnMenuPick(Message.Int[0]);
                Present();
                break;

            case AppMsgOpenDocument:
                Message.Path[MAX_PATH - 1] = UNICODE_NULL;
                OnOpenDocument(Message.Path);
                Present();
                break;

            default:
                break;
        }
    }
    return 0;
}
