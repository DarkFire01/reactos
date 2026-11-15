/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Calendar: Month and Agenda views, appointment editor, reminders
 */

#include <appclient.h>
#include <records.h>
#include <shellapi.h>
#include <strsafe.h>
#include <algorithm>

/*
 * Markup contract (calendar.uix):
 *   Elements: PageMonth, PageAgenda, PageEdit, DayEmpty, AgendaEmpty
 *   Repeaters: Days (Date, Day, Fill, Ink, Mark), DayItems and AgendaItems (Id, Header, Time, Subject)
 *   Bindings: MonthTitle, DayTitle, EditSubject, EditDate, EditTime
 *   Commands: month:-1, month:1, day:<yyyy-mm-dd>, appt:<Id>, apptmenu:<Id>,
 *             edit.subject, edit.day:<n>, edit.time:<minutes>
 */

typedef struct _APPOINTMENT
{
    std::wstring Date;
    std::wstring Time;
    std::wstring Subject;
} APPOINTMENT;

typedef enum _CALENDAR_PAGE
{
    PageMonth,
    PageAgenda,
    PageEdit
} CALENDAR_PAGE;

typedef enum _CALENDAR_MENU
{
    MenuNone,
    MenuMain,
    MenuHold
} CALENDAR_MENU;

static PCWSTR const g_Store = L"calendar.txt";

static
INT
DayOfWeek(
    _In_ INT Year,
    _In_ INT Month,
    _In_ INT Day)
{
    static const INT Offsets[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };

    if (Month < 3)
        Year -= 1;
    return (Year + Year / 4 - Year / 100 + Year / 400 + Offsets[Month - 1] + Day) % 7;
}

static
INT
DaysInMonth(
    _In_ INT Year,
    _In_ INT Month)
{
    static const INT Days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (Month == 2 && ((Year % 4 == 0 && Year % 100 != 0) || Year % 400 == 0))
        return 29;
    return Days[Month - 1];
}

static
std::wstring
DateKey(
    _In_ INT Year,
    _In_ INT Month,
    _In_ INT Day)
{
    WCHAR Text[16];

    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%04d-%02d-%02d", Year, Month, Day);
    return Text;
}

static
std::wstring
FormatDate(
    _In_ const std::wstring &Key,
    _In_z_ PCWSTR Format)
{
    SYSTEMTIME Time = { 0 };
    WCHAR Text[64];

    if (swscanf(Key.c_str(), L"%hu-%hu-%hu", &Time.wYear, &Time.wMonth, &Time.wDay) != 3)
        return Key;
    Time.wDayOfWeek = static_cast<WORD>(DayOfWeek(Time.wYear, Time.wMonth, Time.wDay));
    if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &Time, Format, Text, ARRAYSIZE(Text), NULL))
        return Key;
    return Text;
}

class CCalendarApp : public CAppClient
{
protected:
    VOID OnActivate(VOID) override;

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
        _In_ CALENDAR_PAGE Page);

    VOID
    RefreshMonth(VOID);

    VOID
    RefreshAgenda(VOID);

    VOID
    RefreshEdit(VOID);

    VOID
    SelectDate(
        _In_ INT Year,
        _In_ INT Month,
        _In_ INT Day);

    VOID
    ShiftEditDate(
        _In_ INT Days);

    VOID
    Remind(VOID);

    std::vector<APPOINTMENT> m_Items;
    CALENDAR_PAGE m_Page = PageMonth;
    CALENDAR_PAGE m_View = PageMonth;
    CALENDAR_MENU m_Menu = MenuNone;
    INT m_Year = 2000;
    INT m_Month = 1;
    INT m_Day = 1;
    APPOINTMENT m_Edit;
    LONG m_HoldTarget = -1;
    BOOL m_Started = FALSE;
};

VOID
CCalendarApp::Load(VOID)
{
    std::vector<APP_RECORD> Records;

    LoadRecords(g_Store, Records);
    m_Items.clear();
    for (const APP_RECORD &Record : Records)
    {
        if (Record.size() >= 3)
            m_Items.push_back({ Record[0], Record[1], Record[2] });
    }

    std::sort(m_Items.begin(), m_Items.end(), [](const APPOINTMENT &A, const APPOINTMENT &B)
    {
        return (A.Date != B.Date) ? A.Date < B.Date : A.Time < B.Time;
    });
}

VOID
CCalendarApp::Save(VOID)
{
    std::vector<APP_RECORD> Records;

    std::sort(m_Items.begin(), m_Items.end(), [](const APPOINTMENT &A, const APPOINTMENT &B)
    {
        return (A.Date != B.Date) ? A.Date < B.Date : A.Time < B.Time;
    });

    for (const APPOINTMENT &Item : m_Items)
        Records.push_back({ Item.Date, Item.Time, Item.Subject });
    SaveRecords(g_Store, Records);
}

VOID
CCalendarApp::OnActivate(VOID)
{
    SYSTEMTIME Now;

    if (!m_Started)
    {
        m_Started = TRUE;
        GetLocalTime(&Now);
        m_Year = Now.wYear;
        m_Month = Now.wMonth;
        m_Day = Now.wDay;
        Load();
        Remind();
    }
    ShowPage(m_Page);
}

VOID
CCalendarApp::Remind(VOID)
{
    SYSTEMTIME Now;
    WCHAR Clock[8];
    std::wstring Today;

    /* The next appointment still ahead today gets a reminder bubble */
    GetLocalTime(&Now);
    Today = DateKey(Now.wYear, Now.wMonth, Now.wDay);
    StringCchPrintfW(Clock, ARRAYSIZE(Clock), L"%02u:%02u", Now.wHour, Now.wMinute);

    for (const APPOINTMENT &Item : m_Items)
    {
        if (Item.Date == Today && Item.Time >= Clock)
        {
            Notify(L"Calendar", (Item.Subject + L"\nToday at " + Item.Time).c_str());
            break;
        }
    }
}

VOID
CCalendarApp::ShowPage(
    _In_ CALENDAR_PAGE Page)
{
    m_Page = Page;
    if (Page != PageEdit)
        m_View = Page;

    Scene()->SetVisible(L"PageMonth", Page == PageMonth);
    Scene()->SetVisible(L"PageAgenda", Page == PageAgenda);
    Scene()->SetVisible(L"PageEdit", Page == PageEdit);
    Scene()->SetSelected(NULL);

    if (Page == PageMonth)
        RefreshMonth();
    else if (Page == PageAgenda)
        RefreshAgenda();
    else
        RefreshEdit();

    if (Page == PageEdit)
        SetChrome(L"Calendar", L"Done", L"Cancel", ShellCloseOk);
    else
        SetChrome(L"Calendar", L"New", L"Menu", ShellCloseOk);
}

VOID
CCalendarApp::RefreshMonth(VOID)
{
    static PCWSTR const DayFields[] = { L"Date", L"Day", L"Fill", L"Ink", L"Mark" };
    static PCWSTR const ItemFields[] = { L"Id", L"Header", L"Time", L"Subject" };
    std::vector<std::wstring> Storage;
    std::vector<PCWSTR> Values;
    SYSTEMTIME Now;
    std::wstring Today;
    std::wstring Selected = DateKey(m_Year, m_Month, m_Day);
    WCHAR Text[16];
    INT Lead = DayOfWeek(m_Year, m_Month, 1);
    INT PrevYear = (m_Month == 1) ? m_Year - 1 : m_Year;
    INT PrevMonth = (m_Month == 1) ? 12 : m_Month - 1;
    INT NextYear = (m_Month == 12) ? m_Year + 1 : m_Year;
    INT NextMonth = (m_Month == 12) ? 1 : m_Month + 1;
    ULONG Count = 0;

    GetLocalTime(&Now);
    Today = DateKey(Now.wYear, Now.wMonth, Now.wDay);

    /* Six weeks starting Sunday, spill days from the neighbor months are dimmed */
    for (INT Cell = 0; Cell < 42; Cell++)
    {
        INT Year = m_Year;
        INT Month = m_Month;
        INT Day = Cell - Lead + 1;
        BOOL Outside = FALSE;
        std::wstring Key;

        if (Day < 1)
        {
            Year = PrevYear;
            Month = PrevMonth;
            Day += DaysInMonth(PrevYear, PrevMonth);
            Outside = TRUE;
        }
        else if (Day > DaysInMonth(m_Year, m_Month))
        {
            Day -= DaysInMonth(m_Year, m_Month);
            Year = NextYear;
            Month = NextMonth;
            Outside = TRUE;
        }

        Key = DateKey(Year, Month, Day);
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%d", Day);
        Storage.push_back(Key);
        Storage.push_back(Text);
        Storage.push_back((Key == Selected) ? L"@DaySelected" : ((Key == Today) ? L"@DayToday" : L""));
        Storage.push_back(Outside ? L"@DayOtherText" : L"@DayText");
        Storage.push_back(std::any_of(m_Items.begin(), m_Items.end(),
                                      [&Key](const APPOINTMENT &Item) { return Item.Date == Key; }) ? L"@DayMark" : L"");
    }

    for (const std::wstring &Value : Storage)
        Values.push_back(Value.c_str());
    Scene()->SetItems(L"Days", 42, ARRAYSIZE(DayFields), DayFields, Values.data());

    Scene()->SetBinding(L"MonthTitle", FormatDate(DateKey(m_Year, m_Month, 1), L"MMMM yyyy").c_str());
    Scene()->SetBinding(L"DayTitle", FormatDate(Selected, L"dddd, MMMM d").c_str());

    Storage.clear();
    Values.clear();
    for (SIZE_T Index = 0; Index < m_Items.size(); Index++)
    {
        if (m_Items[Index].Date != Selected)
            continue;
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%Iu", Index);
        Storage.insert(Storage.end(), { Text, L"", m_Items[Index].Time, m_Items[Index].Subject });
        Count++;
    }
    for (const std::wstring &Value : Storage)
        Values.push_back(Value.c_str());
    Scene()->SetItems(L"DayItems", Count, ARRAYSIZE(ItemFields), ItemFields, Values.data());
    Scene()->SetVisible(L"DayEmpty", Count == 0);
}

VOID
CCalendarApp::RefreshAgenda(VOID)
{
    static PCWSTR const Fields[] = { L"Id", L"Header", L"Time", L"Subject" };
    std::vector<std::wstring> Storage;
    std::vector<PCWSTR> Values;
    SYSTEMTIME Now;
    std::wstring Today;
    std::wstring Last;
    WCHAR Text[16];
    ULONG Count = 0;

    GetLocalTime(&Now);
    Today = DateKey(Now.wYear, Now.wMonth, Now.wDay);

    /* Upcoming only, a date header on the first item of each day */
    for (SIZE_T Index = 0; Index < m_Items.size(); Index++)
    {
        const APPOINTMENT &Item = m_Items[Index];

        if (Item.Date < Today)
            continue;
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%Iu", Index);
        Storage.insert(Storage.end(), { Text, (Item.Date != Last) ? FormatDate(Item.Date, L"ddd, MMM d") : L"",
                                        Item.Time, Item.Subject });
        Last = Item.Date;
        Count++;
    }

    for (const std::wstring &Value : Storage)
        Values.push_back(Value.c_str());
    Scene()->SetItems(L"AgendaItems", Count, ARRAYSIZE(Fields), Fields, Values.data());
    Scene()->SetVisible(L"AgendaEmpty", Count == 0);
}

VOID
CCalendarApp::RefreshEdit(VOID)
{
    Scene()->SetBinding(L"EditSubject", (m_Edit.Subject + L"|").c_str());
    Scene()->SetBinding(L"EditDate", FormatDate(m_Edit.Date, L"ddd, MMM d, yyyy").c_str());
    Scene()->SetBinding(L"EditTime", m_Edit.Time.c_str());
}

VOID
CCalendarApp::SelectDate(
    _In_ INT Year,
    _In_ INT Month,
    _In_ INT Day)
{
    m_Year = Year;
    m_Month = Month;
    m_Day = min(Day, DaysInMonth(Year, Month));
    RefreshMonth();
}

VOID
CCalendarApp::ShiftEditDate(
    _In_ INT Days)
{
    INT Year;
    INT Month;
    INT Day;

    if (swscanf(m_Edit.Date.c_str(), L"%d-%d-%d", &Year, &Month, &Day) != 3)
        return;

    Day += Days;
    while (Day < 1)
    {
        Month -= 1;
        if (Month < 1)
        {
            Month = 12;
            Year -= 1;
        }
        Day += DaysInMonth(Year, Month);
    }
    while (Day > DaysInMonth(Year, Month))
    {
        Day -= DaysInMonth(Year, Month);
        Month += 1;
        if (Month > 12)
        {
            Month = 1;
            Year += 1;
        }
    }
    m_Edit.Date = DateKey(Year, Month, Day);
}

VOID
CCalendarApp::OnCommand(
    _In_z_ PCWSTR Command)
{
    INT Year;
    INT Month;
    INT Day;

    if (_wcsnicmp(Command, L"month:", 6) == 0)
    {
        Month = m_Month + _wtoi(Command + 6);
        Year = m_Year;
        if (Month < 1)
        {
            Month = 12;
            Year -= 1;
        }
        else if (Month > 12)
        {
            Month = 1;
            Year += 1;
        }
        SelectDate(Year, Month, 1);
    }
    else if (_wcsnicmp(Command, L"day:", 4) == 0)
    {
        if (swscanf(Command + 4, L"%d-%d-%d", &Year, &Month, &Day) == 3)
            SelectDate(Year, Month, Day);
    }
    else if (_wcsicmp(Command, L"edit.subject") == 0)
    {
        ShowInputPanel(TRUE);
    }
    else if (_wcsnicmp(Command, L"edit.day:", 9) == 0)
    {
        ShiftEditDate(_wtoi(Command + 9));
        RefreshEdit();
    }
    else if (_wcsnicmp(Command, L"edit.time:", 10) == 0)
    {
        INT Hour = 0;
        INT Minute = 0;
        WCHAR Text[8];

        swscanf(m_Edit.Time.c_str(), L"%d:%d", &Hour, &Minute);
        Minute = ((Hour * 60 + Minute + _wtoi(Command + 10)) % 1440 + 1440) % 1440;
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%02d:%02d", Minute / 60, Minute % 60);
        m_Edit.Time = Text;
        RefreshEdit();
    }
    else if (_wcsnicmp(Command, L"appt:", 5) == 0)
    {
        if (m_Page == PageAgenda)
        {
            LONG Index = _wtol(Command + 5);

            if (Index >= 0 && Index < static_cast<LONG>(m_Items.size()) &&
                swscanf(m_Items[Index].Date.c_str(), L"%d-%d-%d", &Year, &Month, &Day) == 3)
            {
                m_Year = Year;
                m_Month = Month;
                m_Day = Day;
                ShowPage(PageMonth);
            }
        }
    }
    else
    {
        CAppClient::OnCommand(Command);
    }
}

VOID
CCalendarApp::OnHold(
    _In_z_ PCWSTR Command,
    _In_ FLOAT X,
    _In_ FLOAT Y)
{
    if (_wcsnicmp(Command, L"apptmenu:", 9) != 0)
        return;

    m_HoldTarget = _wtol(Command + 9);
    m_Menu = MenuHold;
    ShowMenu(AppMenuPoint, X, Y, L"Delete Appointment");
}

VOID
CCalendarApp::OnSoftKey(
    _In_ SHELL_SOFTKEY Key)
{
    SYSTEMTIME Now;
    WCHAR Text[8];

    if (m_Page == PageEdit)
    {
        ShowInputPanel(FALSE);
        if (Key == ShellSoftKeyLeft)
            OnCloseButton();
        else if (Key == ShellSoftKeyRight)
            ShowPage(m_View);
        return;
    }

    if (Key == ShellSoftKeyLeft)
    {
        /* New appointments start on the selected day at the next whole hour */
        GetLocalTime(&Now);
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%02u:00", (Now.wHour + 1) % 24);
        m_Edit.Date = DateKey(m_Year, m_Month, m_Day);
        m_Edit.Time = Text;
        m_Edit.Subject.clear();
        ShowPage(PageEdit);
        ShowInputPanel(TRUE);
    }
    else if (Key == ShellSoftKeyRight)
    {
        m_Menu = MenuMain;
        ShowMenu(AppMenuRightSoftKey, 0.0f, 0.0f,
                 (m_View == PageMonth) ? L"New Appointment\n-\nAgenda View\nGo to Today"
                                       : L"New Appointment\n-\nMonth View\nGo to Today");
    }
}

VOID
CCalendarApp::OnCloseButton(VOID)
{
    if (m_Page != PageEdit)
    {
        RequestClose();
        return;
    }

    ShowInputPanel(FALSE);
    if (!m_Edit.Subject.empty())
    {
        m_Items.push_back(m_Edit);
        Save();
        swscanf(m_Edit.Date.c_str(), L"%d-%d-%d", &m_Year, &m_Month, &m_Day);
    }
    ShowPage(m_View);
}

VOID
CCalendarApp::OnMenuPick(
    _In_ LONG Index)
{
    CALENDAR_MENU Menu = m_Menu;
    SYSTEMTIME Now;

    m_Menu = MenuNone;
    if (Index < 0)
        return;

    if (Menu == MenuHold)
    {
        if (m_HoldTarget >= 0 && m_HoldTarget < static_cast<LONG>(m_Items.size()))
        {
            m_Items.erase(m_Items.begin() + m_HoldTarget);
            Save();
            ShowPage(m_Page);
        }
        return;
    }

    if (Index == 0)
    {
        OnSoftKey(ShellSoftKeyLeft);
    }
    else if (Index == 1)
    {
        ShowPage((m_View == PageMonth) ? PageAgenda : PageMonth);
    }
    else if (Index == 2)
    {
        GetLocalTime(&Now);
        m_Year = Now.wYear;
        m_Month = Now.wMonth;
        m_Day = Now.wDay;
        ShowPage(PageMonth);
    }
}

VOID
CCalendarApp::OnChar(
    _In_ WCHAR Char)
{
    if (m_Page != PageEdit || Char < 0x20)
        return;

    m_Edit.Subject += Char;
    RefreshEdit();
}

VOID
CCalendarApp::OnKey(
    _In_ UINT VirtualKey)
{
    if (m_Page == PageEdit && VirtualKey == VK_BACK)
    {
        if (!m_Edit.Subject.empty())
            m_Edit.Subject.pop_back();
        RefreshEdit();
        Present();
        return;
    }

    /* Arrow keys walk the month grid a day or a week at a time */
    if (m_Page == PageMonth && (VirtualKey == VK_LEFT || VirtualKey == VK_RIGHT || VirtualKey == VK_UP || VirtualKey == VK_DOWN))
    {
        INT Step = (VirtualKey == VK_LEFT) ? -1 : (VirtualKey == VK_RIGHT) ? 1 : (VirtualKey == VK_UP) ? -7 : 7;

        m_Edit.Date = DateKey(m_Year, m_Month, m_Day);
        ShiftEditDate(Step);
        swscanf(m_Edit.Date.c_str(), L"%d-%d-%d", &m_Year, &m_Month, &m_Day);
        RefreshMonth();
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
    CCalendarApp App;
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
