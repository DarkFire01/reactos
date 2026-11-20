/*
 * PROJECT:     ReactOS Settings
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Power page, the active plan's processor settings and power actions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "settings.h"
#include <winsvc.h>

#define ST_PWR_POWER_KEY            L"SYSTEM\\CurrentControlSet\\Control\\Power"
#define ST_PWR_SCHEMES_KEY          ST_PWR_POWER_KEY L"\\User\\PowerSchemes"
#define ST_PWR_SETTINGS_KEY         ST_PWR_POWER_KEY L"\\PowerSettings"
#define ST_PWR_BALANCED             L"381b4222-f694-41f0-9685-ff5bb260df2e"
#define ST_PWR_PROCESSOR_SUBGROUP   L"54533251-82be-4824-96c1-47b60b740d00"
#define ST_PWR_GUID_CHARS           36

/* A setting of the active plan the page can change */
typedef struct _ST_PWR_SETTING
{
    LPCWSTR Name;
    LPCWSTR Subgroup;
    LPCWSTR Setting;
    LPCWSTR Unit;
    DWORD Minimum;
    DWORD Maximum;
    DWORD Step;
} ST_PWR_SETTING, *PST_PWR_SETTING;

static const ST_PWR_SETTING StPwrSettings[] =
{
    { L"Maximum processor state", ST_PWR_PROCESSOR_SUBGROUP, L"bc5038f7-23e0-4960-96da-33abaf5935ec", L"%", 5, 100, 5 },
    { L"Minimum processor state", ST_PWR_PROCESSOR_SUBGROUP, L"893dee8e-2bef-41e0-89c6-b55d0929964c", L"%", 0, 100, 5 },
    { L"Performance increase threshold", ST_PWR_PROCESSOR_SUBGROUP, L"06cadf0e-64ed-448a-8927-ce7bf90eb35d", L"%", 0, 100, 5 },
    { L"Performance decrease threshold", ST_PWR_PROCESSOR_SUBGROUP, L"12a0ab44-fe28-4fa9-b3bd-4b64f44960a6", L"%", 0, 100, 5 },
    { L"Performance check interval", ST_PWR_PROCESSOR_SUBGROUP, L"4d2b0152-7d5c-498b-88e2-34345392a2c5", L" ms", 1, 5000, 5 },
    { L"Processor boost mode", ST_PWR_PROCESSOR_SUBGROUP, L"be337238-0d82-4146-a960-4f3749d470c7", L"", 0, 4, 1 },
};

const UINT StPwrControls[] =
{
    IDC_PWR_SETTING, IDC_PWR_SOURCE, IDC_PWR_LOWER, IDC_PWR_HIGHER, IDC_PWR_RESET,
    IDC_PWR_SLEEP, IDC_PWR_RESTART, IDC_PWR_SHUTDOWN
};
const DWORD StPwrControlCount = _countof(StPwrControls);

static WCHAR StPwrScheme[ST_PWR_GUID_CHARS + 1];
static UINT StPwrSelected;
static BOOL StPwrEditDc;
static DWORD StPwrValue;
static BOOL StPwrValueKnown;
static BOOL StPwrCustom;
static DWORD StPwrServiceState;
static SYSTEM_POWER_STATUS StPwrStatus;
static UINT StPwrConfirm;

static
VOID
StPwrReadScheme(VOID)
{
    DWORD Size = sizeof(StPwrScheme);

    if (RegGetValueW(HKEY_LOCAL_MACHINE, ST_PWR_SCHEMES_KEY, L"ActivePowerScheme", RRF_RT_REG_SZ, NULL,
                     StPwrScheme, &Size) != ERROR_SUCCESS ||
        wcslen(StPwrScheme) != ST_PWR_GUID_CHARS)
    {
        StringCchCopyW(StPwrScheme, _countof(StPwrScheme), ST_PWR_BALANCED);
    }
}

static
LPCWSTR
StPwrIndexName(VOID)
{
    return StPwrEditDc ? L"DCSettingIndex" : L"ACSettingIndex";
}

static
VOID
StPwrUserKey(
    _In_ const ST_PWR_SETTING *Setting,
    _Out_writes_(Count) LPWSTR Path,
    _In_ DWORD Count)
{
    StringCchPrintfW(Path, Count, ST_PWR_SCHEMES_KEY L"\\%s\\%s\\%s", StPwrScheme, Setting->Subgroup, Setting->Setting);
}

/* The value the power service hands out: the user's choice first, the plan default after it */
static
VOID
StPwrReadValue(VOID)
{
    const ST_PWR_SETTING *Setting = &StPwrSettings[StPwrSelected];
    WCHAR Path[MAX_PATH];
    DWORD Size = sizeof(StPwrValue);

    StPwrUserKey(Setting, Path, _countof(Path));
    StPwrCustom = (RegGetValueW(HKEY_LOCAL_MACHINE, Path, StPwrIndexName(), RRF_RT_REG_DWORD, NULL,
                                &StPwrValue, &Size) == ERROR_SUCCESS);
    if (StPwrCustom)
    {
        StPwrValueKnown = TRUE;
        return;
    }

    StringCchPrintfW(Path, _countof(Path), ST_PWR_SETTINGS_KEY L"\\%s\\%s\\DefaultPowerSchemeValues\\%s",
                     Setting->Subgroup, Setting->Setting, StPwrScheme);
    Size = sizeof(StPwrValue);
    StPwrValueKnown = (RegGetValueW(HKEY_LOCAL_MACHINE, Path, StPwrIndexName(), RRF_RT_REG_DWORD, NULL,
                                    &StPwrValue, &Size) == ERROR_SUCCESS);
}

/* The power service watches the plan and hands the new value to the kernel */
static
BOOL
StPwrWriteValue(
    _In_ DWORD Value)
{
    WCHAR Path[MAX_PATH];
    HKEY Key;
    LONG Error;

    StPwrUserKey(&StPwrSettings[StPwrSelected], Path, _countof(Path));
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &Key, NULL);
    if (Error == ERROR_SUCCESS)
    {
        Error = RegSetValueExW(Key, StPwrIndexName(), 0, REG_DWORD, (const BYTE *)&Value, sizeof(Value));
        RegCloseKey(Key);
    }

    if (Error != ERROR_SUCCESS)
    {
        WCHAR Text[128];

        StErrorText(Error, Text, _countof(Text));
        StSetStatus(L"The plan could not be changed: %s", Text);
        return FALSE;
    }

    return TRUE;
}

static
VOID
StPwrResetValue(VOID)
{
    WCHAR Path[MAX_PATH];
    HKEY Key;

    StPwrUserKey(&StPwrSettings[StPwrSelected], Path, _countof(Path));
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_SET_VALUE, &Key) != ERROR_SUCCESS)
        return;

    RegDeleteValueW(Key, StPwrIndexName());
    RegCloseKey(Key);
}

static
DWORD
StPwrQueryService(VOID)
{
    SC_HANDLE Manager, Service;
    SERVICE_STATUS Status;
    DWORD State = 0;

    Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!Manager)
        return 0;

    Service = OpenServiceW(Manager, L"PowerSvc", SERVICE_QUERY_STATUS);
    if (Service)
    {
        if (QueryServiceStatus(Service, &Status))
            State = Status.dwCurrentState;
        CloseServiceHandle(Service);
    }

    CloseServiceHandle(Manager);
    return State;
}

static
VOID
StPwrUpdateControls(VOID)
{
    const ST_PWR_SETTING *Setting = &StPwrSettings[StPwrSelected];
    WCHAR Text[96];

    if (StPage != ST_PAGE_POWER)
        return;

    StShowGroup(StPwrControls, StPwrControlCount, TRUE);
    SetDlgItemTextW(StWindow, IDC_PWR_SETTING, Setting->Name);
    SetDlgItemTextW(StWindow, IDC_PWR_SOURCE, StPwrEditDc ? L"Editing: on battery" : L"Editing: plugged in");

    EnableWindow(GetDlgItem(StWindow, IDC_PWR_LOWER), StPwrValueKnown && StPwrValue > Setting->Minimum);
    EnableWindow(GetDlgItem(StWindow, IDC_PWR_HIGHER), StPwrValueKnown && StPwrValue < Setting->Maximum);
    EnableWindow(GetDlgItem(StWindow, IDC_PWR_RESET), StPwrCustom);

    StringCchCopyW(Text, _countof(Text), (StPwrConfirm == IDC_PWR_RESTART) ? L"Tap again to restart" : L"Restart");
    SetDlgItemTextW(StWindow, IDC_PWR_RESTART, Text);
    StringCchCopyW(Text, _countof(Text), (StPwrConfirm == IDC_PWR_SHUTDOWN) ? L"Tap again to shut down" : L"Shut down");
    SetDlgItemTextW(StWindow, IDC_PWR_SHUTDOWN, Text);
}

static
VOID
StPwrRefresh(VOID)
{
    StPwrReadScheme();
    StPwrReadValue();
    StPwrServiceState = StPwrQueryService();
    if (!GetSystemPowerStatus(&StPwrStatus))
        ZeroMemory(&StPwrStatus, sizeof(StPwrStatus));

    StPwrUpdateControls();
    InvalidateRect(StWindow, NULL, FALSE);
}

/* Restart and shut down need the shutdown privilege turned on in the token */
static
BOOL
StPwrEnableShutdown(VOID)
{
    TOKEN_PRIVILEGES Privileges;
    HANDLE Token;
    BOOL Done;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Token))
        return FALSE;

    Privileges.PrivilegeCount = 1;
    Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    Done = LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &Privileges.Privileges[0].Luid) &&
           AdjustTokenPrivileges(Token, FALSE, &Privileges, 0, NULL, NULL) &&
           GetLastError() == ERROR_SUCCESS;

    CloseHandle(Token);
    return Done;
}

static
VOID
StPwrAction(
    _In_ UINT Id)
{
    WCHAR Text[128];

    if (!StPwrEnableShutdown())
    {
        StSetStatus(L"This account may not change the power state");
        return;
    }

    if (Id == IDC_PWR_SLEEP)
    {
        /* On Modern Standby the system stays on with the display off, the power key wakes it */
        if (SetSystemPowerState(TRUE, FALSE))
            StSetStatus(L"Asleep, press the power key to wake");
        else
        {
            StErrorText(GetLastError(), Text, _countof(Text));
            StSetStatus(L"Sleep failed: %s", Text);
        }
        return;
    }

    if (!ExitWindowsEx(((Id == IDC_PWR_RESTART) ? EWX_REBOOT : EWX_POWEROFF) | EWX_FORCEIFHUNG,
                       SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED))
    {
        StErrorText(GetLastError(), Text, _countof(Text));
        StSetStatus(L"The system did not accept it: %s", Text);
    }
}

VOID
StPwrCreateControls(VOID)
{
    StButton(L"Setting", WS_TABSTOP, IDC_PWR_SETTING);
    StButton(L"Editing", WS_TABSTOP, IDC_PWR_SOURCE);
    StButton(L"-", WS_TABSTOP, IDC_PWR_LOWER);
    StButton(L"+", WS_TABSTOP, IDC_PWR_HIGHER);
    StButton(L"Reset to plan default", WS_TABSTOP, IDC_PWR_RESET);
    StButton(L"Sleep", WS_TABSTOP, IDC_PWR_SLEEP);
    StButton(L"Restart", WS_TABSTOP, IDC_PWR_RESTART);
    StButton(L"Shut down", WS_TABSTOP, IDC_PWR_SHUTDOWN);
}

VOID
StPwrShow(VOID)
{
    StPwrConfirm = 0;
    StPwrRefresh();
}

VOID
StPwrHeading(
    _Out_writes_(Count) LPWSTR Text,
    _In_ DWORD Count)
{
    StringCchCopyW(Text, Count, L"Power");
}

/* Rows below the card: setting, source, value, reset, sleep, restart and shut down */
static
int
StPwrRowTop(
    _In_ const RECT *Area,
    _In_ UINT Row)
{
    return Area->top + ST_CARD_H + ST_GAP + (int)Row * (ST_ROW + ST_GAP);
}

VOID
StPwrLayout(
    _In_ const RECT *Area)
{
    int Width = Area->right - Area->left;
    int Half = (Width - ST_GAP) / 2;
    int Left = Area->left;
    int Step = ST_ROW + ST_ROW / 2;

    StMove(IDC_PWR_SETTING, Left, StPwrRowTop(Area, 0), Width, ST_ROW);
    StMove(IDC_PWR_SOURCE, Left, StPwrRowTop(Area, 1), Width, ST_ROW);
    StMove(IDC_PWR_LOWER, Left, StPwrRowTop(Area, 2), Step, ST_ROW);
    StMove(IDC_PWR_HIGHER, Area->right - Step, StPwrRowTop(Area, 2), Step, ST_ROW);
    StMove(IDC_PWR_RESET, Left, StPwrRowTop(Area, 3), Width, ST_ROW);
    StMove(IDC_PWR_SLEEP, Left, StPwrRowTop(Area, 4), Width, ST_ROW);
    StMove(IDC_PWR_RESTART, Left, StPwrRowTop(Area, 5), Half, ST_ROW);
    StMove(IDC_PWR_SHUTDOWN, Left + Half + ST_GAP, StPwrRowTop(Area, 5), Half, ST_ROW);
}

VOID
StPwrPaint(
    _In_ HDC Dc,
    _In_ const RECT *Area)
{
    const ST_PWR_SETTING *Setting = &StPwrSettings[StPwrSelected];
    WCHAR Title[96];
    WCHAR Detail[256];
    WCHAR Battery[48];
    LPCWSTR Service;
    RECT Card, Label;
    int Step = ST_ROW + ST_ROW / 2;

    switch (StPwrServiceState)
    {
        case SERVICE_RUNNING:
            Service = L"running";
            break;
        case SERVICE_START_PENDING:
            Service = L"starting";
            break;
        case 0:
            Service = L"not installed";
            break;
        default:
            Service = L"stopped";
            break;
    }

    if (StPwrStatus.BatteryLifePercent <= 100)
        StringCchPrintfW(Battery, _countof(Battery), L", battery %u%%", StPwrStatus.BatteryLifePercent);
    else
        Battery[0] = UNICODE_NULL;

    StringCchPrintfW(Title, _countof(Title), L"%s%s",
                     (StPwrStatus.ACLineStatus == 1) ? L"Plugged in" :
                     (StPwrStatus.ACLineStatus == 0) ? L"On battery" : L"Power source unknown",
                     Battery);
    StringCchPrintfW(Detail, _countof(Detail),
                     L"Plan %s%s. The power service is %s, changes reach drivers through it.",
                     !_wcsicmp(StPwrScheme, ST_PWR_BALANCED) ? L"Balanced" : L"",
                     !_wcsicmp(StPwrScheme, ST_PWR_BALANCED) ? L"" : StPwrScheme,
                     Service);

    Card = *Area;
    Card.bottom = Card.top + ST_CARD_H;
    StDrawCard(Dc, &Card, Title, Detail, StPwrServiceState == SERVICE_RUNNING);

    /* The value sits between the lower and higher buttons */
    Label.left = Area->left + Step + ST_GAP;
    Label.right = Area->right - Step - ST_GAP;
    Label.top = StPwrRowTop(Area, 2);
    Label.bottom = Label.top + ST_ROW;

    if (StPwrValueKnown)
    {
        StringCchPrintfW(Detail, _countof(Detail), L"%lu%s%s", StPwrValue, Setting->Unit,
                         StPwrCustom ? L"" : L" (plan default)");
    }
    else
    {
        StringCchCopyW(Detail, _countof(Detail), L"Not in this plan");
    }

    SetBkMode(Dc, TRANSPARENT);
    SelectObject(Dc, StFont);
    SetTextColor(Dc, StPwrCustom ? ST_ACCENT : ST_TEXT);
    DrawTextW(Dc, Detail, -1, &Label, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
}

VOID
StPwrCommand(
    _In_ UINT Id,
    _In_ UINT Code)
{
    const ST_PWR_SETTING *Setting = &StPwrSettings[StPwrSelected];
    DWORD Value;

    if (Code != BN_CLICKED)
        return;

    /* Restart and shut down take a second tap, any other button cancels that */
    if ((Id == IDC_PWR_RESTART || Id == IDC_PWR_SHUTDOWN) && StPwrConfirm == Id)
    {
        StPwrConfirm = 0;
        StPwrAction(Id);
        StPwrUpdateControls();
        return;
    }
    StPwrConfirm = 0;

    switch (Id)
    {
        case IDC_PWR_SETTING:
            StPwrSelected = (StPwrSelected + 1) % _countof(StPwrSettings);
            break;

        case IDC_PWR_SOURCE:
            StPwrEditDc = !StPwrEditDc;
            break;

        case IDC_PWR_LOWER:
        case IDC_PWR_HIGHER:
            if (!StPwrValueKnown)
                break;

            if (Id == IDC_PWR_HIGHER)
                Value = min(StPwrValue + Setting->Step, Setting->Maximum);
            else
                Value = (StPwrValue > Setting->Minimum + Setting->Step) ? StPwrValue - Setting->Step : Setting->Minimum;

            if (StPwrWriteValue(Value))
                StSetStatus(L"%s set to %lu%s %s", Setting->Name, Value, Setting->Unit,
                            StPwrEditDc ? L"on battery" : L"plugged in");
            break;

        case IDC_PWR_RESET:
            StPwrResetValue();
            StSetStatus(L"%s is back to the plan default", Setting->Name);
            break;

        case IDC_PWR_SLEEP:
            StPwrAction(Id);
            break;

        case IDC_PWR_RESTART:
        case IDC_PWR_SHUTDOWN:
            StPwrConfirm = Id;
            break;
    }

    StPwrReadValue();
    StPwrUpdateControls();
    InvalidateRect(StWindow, NULL, FALSE);
}

/* The battery and the service change on their own, keep the card current */
VOID
StPwrTimer(VOID)
{
    StPwrServiceState = StPwrQueryService();
    if (!GetSystemPowerStatus(&StPwrStatus))
        ZeroMemory(&StPwrStatus, sizeof(StPwrStatus));
    InvalidateRect(StWindow, NULL, FALSE);
}

BOOL
StPwrIsActive(
    _In_ UINT Id)
{
    return (Id == IDC_PWR_RESTART || Id == IDC_PWR_SHUTDOWN) && StPwrConfirm == Id;
}
