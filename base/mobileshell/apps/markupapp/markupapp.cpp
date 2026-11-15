/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Hosts a markup only app (Programs, Help): chrome from root attributes
 */

#include <appclient.h>
#include <shellapi.h>

/*
 * Root attributes: Title, Close (ok, x, none), SoftKeyLeft, SoftKeyLeftCommand,
 * SoftKeyRight, SoftKeyRightCommand. Commands are launch:<AppId> and close.
 */

class CMarkupApp : public CAppClient
{
protected:
    VOID OnActivate(VOID) override
    {
        PCWSTR Close = Attribute(L"Close", L"ok");
        SHELL_CLOSE_KIND Kind = ShellCloseOk;

        if (_wcsicmp(Close, L"x") == 0)
            Kind = ShellCloseX;
        else if (_wcsicmp(Close, L"none") == 0)
            Kind = ShellCloseNone;

        SetChrome(Attribute(L"Title", L""), Attribute(L"SoftKeyLeft", L""), Attribute(L"SoftKeyRight", L""), Kind);
    }

    VOID OnSoftKey(
        _In_ SHELL_SOFTKEY Key) override
    {
        PCWSTR Command = NULL;

        if (Key == ShellSoftKeyLeft)
            Command = Scene()->GetAttribute(NULL, L"SoftKeyLeftCommand");
        else if (Key == ShellSoftKeyRight)
            Command = Scene()->GetAttribute(NULL, L"SoftKeyRightCommand");

        if (Command && Command[0])
            OnCommand(Command);
    }

private:
    PCWSTR
    Attribute(
        _In_z_ PCWSTR Name,
        _In_z_ PCWSTR Default)
    {
        PCWSTR Value = Scene()->GetAttribute(NULL, Name);

        return Value ? Value : Default;
    }
};

INT
WINAPI
wWinMain(
    _In_ HINSTANCE Instance,
    _In_opt_ HINSTANCE Previous,
    _In_ PWSTR CommandLine,
    _In_ INT ShowCommand)
{
    CMarkupApp App;
    PWSTR *Arguments;
    INT Count = 0;
    INT Result = 1;

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CommandLine);
    UNREFERENCED_PARAMETER(ShowCommand);

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    Arguments = CommandLineToArgvW(GetCommandLineW(), &Count);
    if (Arguments && SUCCEEDED(App.Initialize(Count, Arguments)))
        Result = App.Run();
    LocalFree(Arguments);
    CoUninitialize();
    return Result;
}
