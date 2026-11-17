/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Base for out of process apps: pipe, shared texture and UIX scene
 */

#pragma once

#include <uix.h>
#include <uixptr.h>
#include <appwire.h>
#include <shellmsg.h>
#include <string>
#include <vector>

class CAppClient
{
public:
    CAppClient();
    virtual ~CAppClient();

    /* Parses --pipe, --theme and --markup, connects and loads the scene */
    HRESULT
    Initialize(
        _In_ INT Count,
        _In_reads_(Count) PWSTR *Arguments);

    INT
    Run(VOID);

protected:
    virtual VOID
    OnActivate(VOID)
    {
    }

    /* The base handles launch:<AppId> and close, derived apps forward what they do not know */
    virtual VOID
    OnCommand(
        _In_z_ PCWSTR Command);

    virtual VOID
    OnHold(
        _In_z_ PCWSTR Command,
        _In_ FLOAT X,
        _In_ FLOAT Y)
    {
        UNREFERENCED_PARAMETER(X);
        UNREFERENCED_PARAMETER(Y);
        OnCommand(Command);
    }

    virtual VOID
    OnChar(
        _In_ WCHAR Char)
    {
        UNREFERENCED_PARAMETER(Char);
    }

    /* A document handed over by another app, at launch (--open) or while running */
    virtual VOID
    OnOpenDocument(
        _In_z_ PCWSTR Path)
    {
        UNREFERENCED_PARAMETER(Path);
    }

    /* Index among the non separator items, -1 when the menu was dismissed */
    virtual VOID
    OnMenuPick(
        _In_ LONG Index)
    {
        UNREFERENCED_PARAMETER(Index);
    }

    virtual VOID
    OnSoftKey(
        _In_ SHELL_SOFTKEY Key)
    {
        UNREFERENCED_PARAMETER(Key);
    }

    virtual VOID
    OnCloseButton(VOID)
    {
        RequestClose();
    }

    virtual VOID
    OnKey(
        _In_ UINT VirtualKey);

    /* Runs on the app thread after Wake, the scene is presented afterwards */
    virtual VOID
    OnWake(VOID)
    {
    }

    /* Safe from any thread, several wakes before the app thread runs fold into one OnWake */
    VOID
    Wake(VOID);

    VOID
    Present(VOID);

    VOID
    SetChrome(
        _In_z_ PCWSTR Title,
        _In_z_ PCWSTR LeftSoftKey,
        _In_z_ PCWSTR RightSoftKey,
        _In_ SHELL_CLOSE_KIND CloseKind);

    VOID
    RequestClose(VOID);

    VOID
    RequestHide(VOID);

    /* Items separated by '\n', "-" for a separator */
    VOID
    ShowMenu(
        _In_ APP_MENU_ANCHOR Anchor,
        _In_ FLOAT X,
        _In_ FLOAT Y,
        _In_z_ PCWSTR Items);

    VOID
    Notify(
        _In_z_ PCWSTR Title,
        _In_z_ PCWSTR Body);

    VOID
    ShowInputPanel(
        _In_ BOOL Show);

    VOID
    Launch(
        _In_z_ PCWSTR AppId,
        _In_opt_z_ PCWSTR Document = NULL);

    /* A shell command from the short list the shell lets apps use, such as theme:<Name> */
    VOID
    Request(
        _In_z_ PCWSTR Command);

    /* Value after a --Name switch on the command line, or NULL */
    PCWSTR
    Argument(
        _In_z_ PCWSTR Name) const;

    PCWSTR
    MarkupFile(VOID) const
    {
        return m_MarkupFile.c_str();
    }

    PCWSTR
    ThemeFile(VOID) const
    {
        return m_ThemeFile.c_str();
    }

    /* Where the last pointer event landed, in app logical units */
    FLOAT m_PointerX = 0.0f;
    FLOAT m_PointerY = 0.0f;

    IUixScene *
    Scene(VOID) const
    {
        return m_Scene.Get();
    }

private:
    BOOL
    Write(
        _In_ const APP_MESSAGE *Message);

    HRESULT
    Activate(
        _In_ const APP_MESSAGE *Message);

    VOID
    Pointer(
        _In_ const APP_MESSAGE *Message);

    BOOL
    ReadMessage(
        _Out_ APP_MESSAGE *Message);

    HANDLE m_Pipe;
    HANDLE m_ReadEvent = NULL;
    HANDLE m_WriteEvent = NULL;
    HANDLE m_WakeEvent = NULL;
    BOOL m_Running;
    BOOL m_Opened = FALSE;
    std::vector<std::wstring> m_Arguments;
    std::wstring m_ThemeFile;
    std::wstring m_MarkupFile;
    UixPtr<IUixEngine> m_Engine;
    UixPtr<IUixScene> m_Scene;
    UixPtr<ID3D11Device> m_Device;
    UixPtr<ID3D11DeviceContext> m_Immediate;
    UixPtr<ID2D1DeviceContext> m_Context;
    UixPtr<ID3D11Texture2D> m_Texture;
    UixPtr<IDXGIKeyedMutex> m_Mutex;
    UixPtr<ID2D1Bitmap1> m_Target;
    FLOAT m_Scale;
};
