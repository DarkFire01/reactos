/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Calculator: standard keypad with memory, C/CE, sign, percent and square root
 */

#include <appclient.h>
#include <shellapi.h>
#include <strsafe.h>
#include <math.h>

/*
 * Markup contract (calculator.uix):
 *   Bindings: Display, Memory
 *   Commands: calc:<key> where key is a digit, ".", "+", "-", "*", "/", "=", "C", "CE",
 *             "neg", "sqrt", "%", "MC", "MR", "M+"
 */

#define CALC_DIGITS 12

class CCalculatorApp : public CAppClient
{
protected:
    VOID OnActivate(VOID) override
    {
        SetChrome(L"Calculator", L"", L"", ShellCloseOk);
        Refresh();
    }

    VOID OnCommand(
        _In_z_ PCWSTR Command) override;

    VOID OnChar(
        _In_ WCHAR Char) override;

    VOID OnKey(
        _In_ UINT VirtualKey) override;

private:
    VOID
    Key(
        _In_z_ PCWSTR Key);

    VOID
    Apply(VOID);

    VOID
    Show(
        _In_ DOUBLE Value);

    VOID
    Refresh(VOID);

    DOUBLE
    Entry(VOID) const
    {
        return _wtof(m_Entry.c_str());
    }

    std::wstring m_Entry = L"0";
    DOUBLE m_Accumulator = 0.0;
    DOUBLE m_Memory = 0.0;
    WCHAR m_Operator = 0;
    BOOL m_Fresh = TRUE;
    BOOL m_Error = FALSE;
};

VOID
CCalculatorApp::Show(
    _In_ DOUBLE Value)
{
    WCHAR Text[32];

    if (!_finite(Value))
    {
        m_Error = TRUE;
        m_Entry = L"0";
        return;
    }

    /* Enough precision for the display without float noise */
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%.10g", Value);
    m_Entry = Text;
}

VOID
CCalculatorApp::Apply(VOID)
{
    DOUBLE Right = Entry();

    switch (m_Operator)
    {
        case L'+':
            m_Accumulator += Right;
            break;
        case L'-':
            m_Accumulator -= Right;
            break;
        case L'*':
            m_Accumulator *= Right;
            break;
        case L'/':
            if (Right == 0.0)
            {
                m_Error = TRUE;
                return;
            }
            m_Accumulator /= Right;
            break;
        default:
            m_Accumulator = Right;
            break;
    }
    Show(m_Accumulator);
}

VOID
CCalculatorApp::Key(
    _In_z_ PCWSTR Key)
{
    WCHAR First = Key[0];

    if (m_Error && _wcsicmp(Key, L"C") != 0)
        return;

    if (iswdigit(First))
    {
        if (m_Fresh || m_Entry == L"0")
            m_Entry.clear();
        if (m_Entry.size() < CALC_DIGITS)
            m_Entry += First;
        m_Fresh = FALSE;
    }
    else if (First == L'.')
    {
        if (m_Fresh)
            m_Entry = L"0";
        if (m_Entry.find(L'.') == std::wstring::npos)
            m_Entry += L'.';
        m_Fresh = FALSE;
    }
    else if (wcschr(L"+-*/", First) && !Key[1])
    {
        /* Chained operators evaluate left to right, like the device */
        if (!m_Fresh || !m_Operator)
            Apply();
        m_Operator = First;
        m_Fresh = TRUE;
    }
    else if (First == L'=')
    {
        Apply();
        m_Operator = 0;
        m_Fresh = TRUE;
    }
    else if (_wcsicmp(Key, L"C") == 0)
    {
        m_Entry = L"0";
        m_Accumulator = 0.0;
        m_Operator = 0;
        m_Fresh = TRUE;
        m_Error = FALSE;
    }
    else if (_wcsicmp(Key, L"CE") == 0)
    {
        m_Entry = L"0";
        m_Fresh = TRUE;
    }
    else if (_wcsicmp(Key, L"back") == 0)
    {
        if (!m_Fresh && m_Entry.size() > 1)
            m_Entry.pop_back();
        else
            m_Entry = L"0";
    }
    else if (_wcsicmp(Key, L"neg") == 0)
    {
        Show(-Entry());
    }
    else if (_wcsicmp(Key, L"sqrt") == 0)
    {
        if (Entry() < 0.0)
            m_Error = TRUE;
        else
            Show(sqrt(Entry()));
        m_Fresh = TRUE;
    }
    else if (First == L'%')
    {
        Show(m_Accumulator * Entry() / 100.0);
        m_Fresh = TRUE;
    }
    else if (_wcsicmp(Key, L"MC") == 0)
    {
        m_Memory = 0.0;
    }
    else if (_wcsicmp(Key, L"MR") == 0)
    {
        Show(m_Memory);
        m_Fresh = TRUE;
    }
    else if (_wcsicmp(Key, L"M+") == 0)
    {
        m_Memory += Entry();
        m_Fresh = TRUE;
    }

    Refresh();
}

VOID
CCalculatorApp::Refresh(VOID)
{
    Scene()->SetBinding(L"Display", m_Error ? L"Error" : m_Entry.c_str());
    Scene()->SetBinding(L"Memory", (m_Memory != 0.0) ? L"M" : L"");
}

VOID
CCalculatorApp::OnCommand(
    _In_z_ PCWSTR Command)
{
    if (_wcsnicmp(Command, L"calc:", 5) == 0)
        Key(Command + 5);
    else
        CAppClient::OnCommand(Command);
}

VOID
CCalculatorApp::OnChar(
    _In_ WCHAR Char)
{
    WCHAR Text[2] = { Char, 0 };

    if (Char == L'x' || Char == L'X')
        Text[0] = L'*';
    if (iswdigit(Text[0]) || wcschr(L".+-*/=%", Text[0]))
        Key(Text);
}

VOID
CCalculatorApp::OnKey(
    _In_ UINT VirtualKey)
{
    if (VirtualKey == VK_BACK)
    {
        Key(L"back");
        Present();
    }
    else if (VirtualKey == VK_RETURN)
    {
        Key(L"=");
        Present();
    }
    else if (VirtualKey == VK_DELETE)
    {
        Key(L"C");
        Present();
    }
    else
    {
        CAppClient::OnKey(VirtualKey);
    }
}

INT
WINAPI
wWinMain(
    _In_ HINSTANCE Instance,
    _In_opt_ HINSTANCE Previous,
    _In_ PWSTR CommandLine,
    _In_ INT ShowCommand)
{
    CCalculatorApp App;
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
