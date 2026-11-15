/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Contacts: searchable list, contact card and editor
 */

#include <appclient.h>
#include <records.h>
#include <shellapi.h>
#include <strsafe.h>
#include <algorithm>

/*
 * Markup contract (contacts.uix):
 *   Elements: PageList, PageCard, PageEdit, SearchHint, ListEmpty, CardPhoneRow,
 *             CardEmailRow, CardCompanyRow; repeater ContactList (fields Id, Name, Detail)
 *   Bindings: SearchText, CardName, CardPhone, CardEmail, CardCompany, EditTitle,
 *             Edit0..Edit3 (field text), Fill0..Fill3 (field brush)
 *   Commands: search.focus, search.clear, jump:<letters>, open:<Id>, hold:<Id>, field:<n>
 */

#define CONTACT_FIELDS 4

typedef struct _CONTACT
{
    std::vector<std::wstring> Field = std::vector<std::wstring>(CONTACT_FIELDS);
} CONTACT;

typedef enum _CONTACT_PAGE
{
    PageList,
    PageCard,
    PageEdit
} CONTACT_PAGE;

typedef enum _CONTACT_MENU
{
    MenuNone,
    MenuList,
    MenuCard,
    MenuHold
} CONTACT_MENU;

static PCWSTR const g_Store = L"contacts.txt";

class CContactsApp : public CAppClient
{
protected:
    VOID OnActivate(VOID) override
    {
        Load();
        ShowPage(m_Page);
    }

    VOID OnCommand(
        _In_z_ PCWSTR Command) override;

    VOID OnHold(
        _In_z_ PCWSTR Command,
        _In_ FLOAT X,
        _In_ FLOAT Y) override;

    VOID OnSoftKey(
        _In_ SHELL_SOFTKEY Key) override;

    VOID OnCloseButton(VOID) override;

    VOID OnChar(
        _In_ WCHAR Char) override;

    VOID OnKey(
        _In_ UINT VirtualKey) override;

    VOID OnMenuPick(
        _In_ LONG Index) override;

private:
    VOID
    Load(VOID);

    VOID
    Save(VOID);

    VOID
    ShowPage(
        _In_ CONTACT_PAGE Page);

    VOID
    RefreshList(VOID);

    VOID
    RefreshCard(VOID);

    VOID
    RefreshEdit(VOID);

    VOID
    BeginEdit(
        _In_ LONG Index);

    VOID
    Delete(
        _In_ LONG Index);

    std::wstring *
    FocusedText(VOID);

    std::vector<CONTACT> m_Contacts;
    std::vector<LONG> m_Shown;
    std::wstring m_Search;
    CONTACT_PAGE m_Page = PageList;
    CONTACT_MENU m_Menu = MenuNone;
    CONTACT m_Edit;
    LONG m_Current = -1;
    LONG m_Field = 0;
    LONG m_HoldTarget = -1;
    BOOL m_SearchFocus = FALSE;
    BOOL m_Loaded = FALSE;
};

static
BOOL
ContainsNoCase(
    _In_ const std::wstring &Text,
    _In_ const std::wstring &Part)
{
    auto Found = std::search(Text.begin(), Text.end(), Part.begin(), Part.end(),
                             [](WCHAR A, WCHAR B) { return towlower(A) == towlower(B); });
    return Found != Text.end();
}

VOID
CContactsApp::Load(VOID)
{
    std::vector<APP_RECORD> Records;

    if (m_Loaded)
        return;
    m_Loaded = TRUE;

    /* First run gets a few entries so the list is not empty */
    if (!LoadRecords(g_Store, Records))
    {
        Records = {
            { L"Alex Carter", L"(425) 555-0142", L"alex@example.com", L"Fabrikam" },
            { L"Bianca Ortiz", L"(206) 555-0187", L"bianca@example.com", L"" },
            { L"Chen Wei", L"(425) 555-0110", L"", L"Contoso" },
            { L"Dana Brooks", L"(360) 555-0133", L"dana@example.com", L"" },
            { L"Elliot Park", L"(253) 555-0199", L"", L"" },
            { L"Farah Haddad", L"(425) 555-0166", L"farah@example.com", L"Northwind" },
            { L"Grace Kim", L"(206) 555-0121", L"", L"" },
            { L"Hugo Martin", L"(509) 555-0158", L"hugo@example.com", L"" },
            { L"Ines Duarte", L"(425) 555-0104", L"", L"Tailspin" },
            { L"Jonah Reed", L"(206) 555-0177", L"jonah@example.com", L"" },
            { L"Kira Novak", L"(360) 555-0145", L"", L"" },
            { L"Liam O'Neil", L"(425) 555-0190", L"liam@example.com", L"Adventure Works" },
            { L"Maya Singh", L"(206) 555-0114", L"", L"" },
            { L"Noah Fischer", L"(253) 555-0129", L"noah@example.com", L"" },
            { L"Olivia Grant", L"(425) 555-0163", L"", L"Litware" },
        };
        SaveRecords(g_Store, Records);
    }

    for (const APP_RECORD &Record : Records)
    {
        CONTACT Contact;

        for (SIZE_T Index = 0; Index < CONTACT_FIELDS && Index < Record.size(); Index++)
            Contact.Field[Index] = Record[Index];
        if (!Contact.Field[0].empty())
            m_Contacts.push_back(Contact);
    }
}

VOID
CContactsApp::Save(VOID)
{
    std::vector<APP_RECORD> Records;

    std::sort(m_Contacts.begin(), m_Contacts.end(),
              [](const CONTACT &A, const CONTACT &B) { return _wcsicmp(A.Field[0].c_str(), B.Field[0].c_str()) < 0; });

    for (const CONTACT &Contact : m_Contacts)
        Records.push_back(Contact.Field);
    SaveRecords(g_Store, Records);
}

VOID
CContactsApp::ShowPage(
    _In_ CONTACT_PAGE Page)
{
    m_Page = Page;
    Scene()->SetVisible(L"PageList", Page == PageList);
    Scene()->SetVisible(L"PageCard", Page == PageCard);
    Scene()->SetVisible(L"PageEdit", Page == PageEdit);
    Scene()->SetSelected(NULL);

    switch (Page)
    {
        case PageList:
            RefreshList();
            SetChrome(L"Contacts", L"New", L"Menu", ShellCloseOk);
            break;

        case PageCard:
            RefreshCard();
            SetChrome(L"Contacts", L"Edit", L"Menu", ShellCloseOk);
            break;

        case PageEdit:
            RefreshEdit();
            SetChrome(L"Contacts", L"Done", L"Cancel", ShellCloseOk);
            break;
    }
}

VOID
CContactsApp::RefreshList(VOID)
{
    static PCWSTR const Fields[] = { L"Id", L"Name", L"Detail" };
    std::vector<std::wstring> Storage;
    std::vector<PCWSTR> Values;
    WCHAR Id[16];

    std::sort(m_Contacts.begin(), m_Contacts.end(),
              [](const CONTACT &A, const CONTACT &B) { return _wcsicmp(A.Field[0].c_str(), B.Field[0].c_str()) < 0; });

    m_Shown.clear();
    for (LONG Index = 0; Index < static_cast<LONG>(m_Contacts.size()); Index++)
    {
        const CONTACT &Contact = m_Contacts[Index];

        if (!m_Search.empty() && !ContainsNoCase(Contact.Field[0], m_Search) && !ContainsNoCase(Contact.Field[1], m_Search))
            continue;

        /* Second line shows the phone number with WM's "m" for mobile */
        StringCchPrintfW(Id, ARRAYSIZE(Id), L"%ld", Index);
        Storage.push_back(Id);
        Storage.push_back(Contact.Field[0]);
        Storage.push_back(Contact.Field[1].empty() ? Contact.Field[2] : Contact.Field[1] + L"  m");
        m_Shown.push_back(Index);
    }

    for (const std::wstring &Value : Storage)
        Values.push_back(Value.c_str());

    Scene()->SetItems(L"ContactList", static_cast<ULONG>(m_Shown.size()), ARRAYSIZE(Fields), Fields, Values.data());
    Scene()->SetBinding(L"SearchText", (m_Search + (m_SearchFocus ? L"|" : L"")).c_str());
    Scene()->SetVisible(L"SearchHint", m_Search.empty() && !m_SearchFocus);
    Scene()->SetVisible(L"ListEmpty", m_Shown.empty());
}

VOID
CContactsApp::RefreshCard(VOID)
{
    static PCWSTR const Rows[] = { NULL, L"CardPhoneRow", L"CardEmailRow", L"CardCompanyRow" };
    static PCWSTR const Bindings[] = { L"CardName", L"CardPhone", L"CardEmail", L"CardCompany" };

    if (m_Current < 0 || m_Current >= static_cast<LONG>(m_Contacts.size()))
        return;

    for (ULONG Index = 0; Index < CONTACT_FIELDS; Index++)
    {
        Scene()->SetBinding(Bindings[Index], m_Contacts[m_Current].Field[Index].c_str());
        if (Rows[Index])
            Scene()->SetVisible(Rows[Index], !m_Contacts[m_Current].Field[Index].empty());
    }
}

VOID
CContactsApp::RefreshEdit(VOID)
{
    WCHAR Name[16];

    Scene()->SetBinding(L"EditTitle", (m_Current < 0) ? L"New Contact" : L"Edit Contact");
    for (LONG Index = 0; Index < CONTACT_FIELDS; Index++)
    {
        StringCchPrintfW(Name, ARRAYSIZE(Name), L"Edit%ld", Index);
        Scene()->SetBinding(Name, (m_Edit.Field[Index] + ((Index == m_Field) ? L"|" : L"")).c_str());
        StringCchPrintfW(Name, ARRAYSIZE(Name), L"Fill%ld", Index);
        Scene()->SetBinding(Name, (Index == m_Field) ? L"@FieldActive" : L"@FieldNormal");
    }
}

VOID
CContactsApp::BeginEdit(
    _In_ LONG Index)
{
    m_Current = Index;
    m_Edit = (Index >= 0) ? m_Contacts[Index] : CONTACT();
    m_Field = 0;
    ShowPage(PageEdit);
    ShowInputPanel(TRUE);
}

VOID
CContactsApp::Delete(
    _In_ LONG Index)
{
    if (Index < 0 || Index >= static_cast<LONG>(m_Contacts.size()))
        return;

    m_Contacts.erase(m_Contacts.begin() + Index);
    Save();
    m_Current = -1;
    ShowPage(PageList);
}

std::wstring *
CContactsApp::FocusedText(VOID)
{
    if (m_Page == PageEdit)
        return &m_Edit.Field[m_Field];
    if (m_Page == PageList)
        return &m_Search;
    return NULL;
}

VOID
CContactsApp::OnCommand(
    _In_z_ PCWSTR Command)
{
    if (_wcsicmp(Command, L"search.focus") == 0)
    {
        m_SearchFocus = TRUE;
        RefreshList();
        ShowInputPanel(TRUE);
    }
    else if (_wcsicmp(Command, L"search.clear") == 0)
    {
        m_Search.clear();
        m_SearchFocus = FALSE;
        Scene()->ResetScroll(L"ListScroll");
        RefreshList();
    }
    else if (_wcsnicmp(Command, L"jump:", 5) == 0)
    {
        WCHAR Name[16];

        /* Jump to the first shown name whose initial is in the group, '#' meaning not a letter */
        for (LONG Index : m_Shown)
        {
            WCHAR Initial = towlower(m_Contacts[Index].Field[0][0]);

            if ((iswalpha(Initial) && wcschr(Command + 5, Initial)) || (!iswalpha(Initial) && wcschr(Command + 5, L'#')))
            {
                StringCchPrintfW(Name, ARRAYSIZE(Name), L"c%ld", Index);
                Scene()->SetSelected(Name);
                break;
            }
        }
    }
    else if (_wcsnicmp(Command, L"open:", 5) == 0)
    {
        m_Current = _wtol(Command + 5);
        m_SearchFocus = FALSE;
        ShowPage(PageCard);
    }
    else if (_wcsnicmp(Command, L"field:", 6) == 0)
    {
        m_Field = min(max(_wtol(Command + 6), 0L), CONTACT_FIELDS - 1L);
        RefreshEdit();
        ShowInputPanel(TRUE);
    }
    else
    {
        CAppClient::OnCommand(Command);
    }
}

VOID
CContactsApp::OnHold(
    _In_z_ PCWSTR Command,
    _In_ FLOAT X,
    _In_ FLOAT Y)
{
    if (_wcsnicmp(Command, L"hold:", 5) != 0)
        return;

    m_HoldTarget = _wtol(Command + 5);
    m_Menu = MenuHold;
    ShowMenu(AppMenuPoint, X, Y, L"Edit Contact\nDelete Contact");
}

VOID
CContactsApp::OnSoftKey(
    _In_ SHELL_SOFTKEY Key)
{
    switch (m_Page)
    {
        case PageList:
            if (Key == ShellSoftKeyLeft)
            {
                BeginEdit(-1);
            }
            else if (Key == ShellSoftKeyRight)
            {
                m_Menu = MenuList;
                ShowMenu(AppMenuRightSoftKey, 0.0f, 0.0f, L"New Contact\nDelete Contact\n-\nClear Search");
            }
            break;

        case PageCard:
            if (Key == ShellSoftKeyLeft)
            {
                BeginEdit(m_Current);
            }
            else if (Key == ShellSoftKeyRight)
            {
                m_Menu = MenuCard;
                ShowMenu(AppMenuRightSoftKey, 0.0f, 0.0f, L"Edit Contact\n-\nDelete Contact");
            }
            break;

        case PageEdit:
            if (Key == ShellSoftKeyLeft)
                OnCloseButton();
            else if (Key == ShellSoftKeyRight)
                ShowPage((m_Current < 0) ? PageList : PageCard);
            ShowInputPanel(FALSE);
            break;
    }
}

VOID
CContactsApp::OnCloseButton(VOID)
{
    switch (m_Page)
    {
        case PageList:
            RequestClose();
            break;

        case PageCard:
            ShowPage(PageList);
            break;

        case PageEdit:
            /* ok saves, an empty name throws the entry away */
            ShowInputPanel(FALSE);
            if (!m_Edit.Field[0].empty())
            {
                if (m_Current < 0)
                    m_Contacts.push_back(m_Edit);
                else
                    m_Contacts[m_Current] = m_Edit;

                /* Saving sorts, so find the entry again */
                Save();
                for (m_Current = 0; m_Current < static_cast<LONG>(m_Contacts.size()); m_Current++)
                {
                    if (m_Edit.Field == m_Contacts[m_Current].Field)
                        break;
                }
                ShowPage(PageCard);
            }
            else
            {
                ShowPage(PageList);
            }
            break;
    }
}

VOID
CContactsApp::OnMenuPick(
    _In_ LONG Index)
{
    CONTACT_MENU Menu = m_Menu;
    LONG Selected = -1;
    PCWSTR Command = Scene()->GetSelectedCommand();

    m_Menu = MenuNone;
    if (Index < 0)
        return;

    if (Command && _wcsnicmp(Command, L"open:", 5) == 0)
        Selected = _wtol(Command + 5);

    switch (Menu)
    {
        case MenuList:
            if (Index == 0)
                BeginEdit(-1);
            else if (Index == 1)
                Delete(Selected);
            else if (Index == 2)
                OnCommand(L"search.clear");
            break;

        case MenuCard:
            if (Index == 0)
                BeginEdit(m_Current);
            else if (Index == 1)
                Delete(m_Current);
            break;

        case MenuHold:
            if (Index == 0)
                BeginEdit(m_HoldTarget);
            else if (Index == 1)
                Delete(m_HoldTarget);
            break;

        default:
            break;
    }
}

VOID
CContactsApp::OnChar(
    _In_ WCHAR Char)
{
    std::wstring *Text = FocusedText();

    if (!Text || Char < 0x20)
        return;

    *Text += Char;
    if (m_Page == PageList)
    {
        /* Typing on the list goes to the Find box, like the device */
        m_SearchFocus = TRUE;
        Scene()->ResetScroll(L"ListScroll");
        RefreshList();
    }
    else
    {
        RefreshEdit();
    }
}

VOID
CContactsApp::OnKey(
    _In_ UINT VirtualKey)
{
    std::wstring *Text = FocusedText();

    if (VirtualKey == VK_BACK && Text)
    {
        if (!Text->empty())
            Text->pop_back();
        if (m_Page == PageList)
            RefreshList();
        else
            RefreshEdit();
        Present();
        return;
    }

    if (m_Page == PageEdit && (VirtualKey == VK_RETURN || VirtualKey == VK_DOWN || VirtualKey == VK_UP))
    {
        m_Field = (m_Field + ((VirtualKey == VK_UP) ? CONTACT_FIELDS - 1 : 1)) % CONTACT_FIELDS;
        RefreshEdit();
        Present();
        return;
    }

    CAppClient::OnKey(VirtualKey);
}

INT
WINAPI
wWinMain(
    _In_ HINSTANCE Instance,
    _In_opt_ HINSTANCE Previous,
    _In_ PWSTR CommandLine,
    _In_ INT ShowCommand)
{
    CContactsApp App;
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
