/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Device Manager: the device tree by class, problem codes, enable, disable and restart
 */

#include <appclient.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <shellapi.h>
#include <strsafe.h>
#include <algorithm>

/*
 * Markup contract (devices.uix):
 *   Elements: PageList, PageDevice, SearchHint, ListEmpty; repeater DeviceList
 *             (fields Id, Name, Detail, DetailBrush, Glyph, Fill)
 *   Bindings: SearchText, EmptyText, ProblemsLabel, TabAll, TabProblems, DevName, DevGlyph,
 *             DevFill, DevStatus, DevStatusBrush, DevClass, DevMfg, DevLocation, DevDriver,
 *             DevService, DevHardwareId, DevInstance
 *   Commands: filter:all, filter:problems, search.focus, search.clear, open:<Id>, hold:<Id>
 */

#define DEVICE_PROBLEM_FILL L"#FFB900"
#define DEVICE_OFF_FILL     L"@BaseLow"

typedef struct _DEVICE
{
    std::wstring Name;
    std::wstring Class;
    std::wstring Instance;
    std::wstring Manufacturer;
    std::wstring Location;
    std::wstring Service;
    std::wstring HardwareId;
    std::wstring Driver;
    ULONG Status = 0;
    ULONG Problem = 0;
    BOOL Present = FALSE;
} DEVICE;

typedef enum _DEVICE_PAGE
{
    PageList,
    PageDevice
} DEVICE_PAGE;

typedef enum _DEVICE_MENU
{
    MenuNone,
    MenuList,
    MenuDevice
} DEVICE_MENU;

typedef enum _DEVICE_ACTION
{
    ActionEnable,
    ActionDisable,
    ActionRestart
} DEVICE_ACTION;

typedef struct _PROBLEM_TEXT
{
    ULONG Code;
    PCWSTR Text;
} PROBLEM_TEXT;

/* Device Manager wording for the problem codes a device can report */
static const PROBLEM_TEXT g_Problems[] =
{
    { 1, L"This device is not configured correctly." },
    { 3, L"The driver for this device might be corrupted, or the system is low on memory." },
    { 10, L"This device cannot start." },
    { 12, L"This device cannot find enough free resources that it can use." },
    { 14, L"This device cannot work properly until you restart the computer." },
    { 16, L"The system cannot identify all the resources this device uses." },
    { 18, L"Reinstall the drivers for this device." },
    { 19, L"The configuration information for this device in the registry is incomplete or damaged." },
    { 21, L"The system is removing this device." },
    { 22, L"This device is disabled." },
    { 24, L"This device is not present, is not working properly, or does not have all its drivers installed." },
    { 28, L"The drivers for this device are not installed." },
    { 29, L"This device is disabled because the firmware of the device did not give it the required resources." },
    { 31, L"This device is not working properly because the system cannot load the drivers required for this device." },
    { 32, L"A driver service for this device has been disabled." },
    { 37, L"The driver for this device returned a failure when it was loaded." },
    { 38, L"The driver cannot load because a previous instance of it is still in memory." },
    { 39, L"The driver for this device might be corrupted or missing." },
    { 40, L"The service key information for the driver in the registry is missing or recorded incorrectly." },
    { 41, L"The driver loaded but cannot find the hardware device." },
    { 43, L"The device reported a problem and was stopped." },
    { 45, L"This hardware device is not connected to the computer." },
    { 47, L"This device is prepared for safe removal." },
    { 48, L"The driver for this device has been blocked from starting." },
    { 51, L"This device is waiting on another device or set of devices to start." },
    { 52, L"The system cannot verify the digital signature for the drivers of this device." },
};

typedef struct _CLASS_GLYPH
{
    PCWSTR Class;
    PCWSTR Glyph;
} CLASS_GLYPH;

static const CLASS_GLYPH g_Glyphs[] =
{
    { L"Display", L"\xE7F4" },
    { L"Monitor", L"\xE7F4" },
    { L"Net", L"\xE839" },
    { L"USB", L"\xE88E" },
    { L"HIDClass", L"\xE765" },
    { L"Keyboard", L"\xE765" },
    { L"Mouse", L"\xE962" },
    { L"Battery", L"\xE83F" },
    { L"Bluetooth", L"\xE702" },
    { L"Camera", L"\xE722" },
    { L"Image", L"\xE722" },
    { L"MEDIA", L"\xE767" },
    { L"AudioEndpoint", L"\xE767" },
    { L"DiskDrive", L"\xEDA2" },
    { L"Volume", L"\xEDA2" },
    { L"SCSIAdapter", L"\xEDA2" },
    { L"Processor", L"\xE950" },
    { L"Sensor", L"\xE957" },
    { L"Ports", L"\xE88E" },
    { L"Modem", L"\xE717" },
    { L"System", L"\xE770" },
};

static
PCWSTR
ClassGlyph(
    _In_ const std::wstring &Class)
{
    for (const CLASS_GLYPH &Entry : g_Glyphs)
    {
        if (_wcsicmp(Entry.Class, Class.c_str()) == 0)
            return Entry.Glyph;
    }
    return L"\xE772";
}

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

/* A REG_SZ property, or the first string of a REG_MULTI_SZ one */
static
std::wstring
DeviceProperty(
    _In_ HDEVINFO Set,
    _In_ PSP_DEVINFO_DATA Data,
    _In_ DWORD Property)
{
    WCHAR Buffer[512];
    DWORD Type = 0;

    ZeroMemory(Buffer, sizeof(Buffer));
    if (!SetupDiGetDeviceRegistryPropertyW(Set, Data, Property, &Type, reinterpret_cast<PBYTE>(Buffer),
                                           sizeof(Buffer) - sizeof(WCHAR), NULL) ||
        (Type != REG_SZ && Type != REG_MULTI_SZ && Type != REG_EXPAND_SZ))
    {
        return std::wstring();
    }
    return Buffer;
}

static
std::wstring
DriverValue(
    _In_ HKEY Key,
    _In_z_ PCWSTR Name)
{
    WCHAR Buffer[256];
    DWORD Size = sizeof(Buffer) - sizeof(WCHAR);
    DWORD Type = 0;

    ZeroMemory(Buffer, sizeof(Buffer));
    if (RegQueryValueExW(Key, Name, NULL, &Type, reinterpret_cast<PBYTE>(Buffer), &Size) != ERROR_SUCCESS || Type != REG_SZ)
        return std::wstring();
    return Buffer;
}

/* Provider, version, date and INF from the driver key, on one line */
static
std::wstring
DriverSummary(
    _In_ HDEVINFO Set,
    _In_ PSP_DEVINFO_DATA Data)
{
    std::wstring Text;
    std::wstring Part;
    HKEY Key;

    Key = SetupDiOpenDevRegKey(Set, Data, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
    if (Key == INVALID_HANDLE_VALUE)
        return std::wstring();

    Text = DriverValue(Key, L"ProviderName");
    if (!(Part = DriverValue(Key, L"DriverVersion")).empty())
        Text += (Text.empty() ? L"" : L", ") + Part;
    if (!(Part = DriverValue(Key, L"DriverDate")).empty())
        Text += (Text.empty() ? L"" : L", ") + Part;
    if (!(Part = DriverValue(Key, L"InfPath")).empty())
        Text += (Text.empty() ? L"" : L", ") + Part;
    RegCloseKey(Key);
    return Text;
}

class CDevicesApp : public CAppClient
{
protected:
    VOID OnActivate(VOID) override
    {
        Enumerate();
        ShowPage(PageList);
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
    Enumerate(VOID);

    VOID
    ShowPage(
        _In_ DEVICE_PAGE Page);

    VOID
    RefreshList(VOID);

    VOID
    RefreshDevice(VOID);

    VOID
    Act(
        _In_ LONG Index,
        _In_ DEVICE_ACTION Action);

    VOID
    Rescan(VOID);

    static
    BOOL
    HasProblem(
        _In_ const DEVICE &Device)
    {
        return Device.Problem != 0 && Device.Problem != CM_PROB_DISABLED;
    }

    static
    BOOL
    IsDisabled(
        _In_ const DEVICE &Device)
    {
        return Device.Problem == CM_PROB_DISABLED;
    }

    static
    std::wstring
    StatusText(
        _In_ const DEVICE &Device);

    static
    std::wstring
    ShortStatus(
        _In_ const DEVICE &Device);

    static
    PCWSTR
    Fill(
        _In_ const DEVICE &Device);

    std::vector<DEVICE> m_Devices;
    std::vector<LONG> m_Shown;
    std::wstring m_Search;
    DEVICE_PAGE m_Page = PageList;
    DEVICE_MENU m_Menu = MenuNone;
    LONG m_Current = -1;
    LONG m_HoldTarget = -1;
    BOOL m_ProblemsOnly = FALSE;
    BOOL m_ShowHidden = FALSE;
    BOOL m_SearchFocus = FALSE;
};

std::wstring
CDevicesApp::StatusText(
    _In_ const DEVICE &Device)
{
    WCHAR Text[256];

    if (!Device.Present)
        return L"This hardware device is not connected to the computer. (Code 45)";

    if (Device.Problem)
    {
        for (const PROBLEM_TEXT &Entry : g_Problems)
        {
            if (Entry.Code == Device.Problem)
            {
                StringCchPrintfW(Text, ARRAYSIZE(Text), L"%s (Code %lu)", Entry.Text, Device.Problem);
                return Text;
            }
        }
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"This device has a problem. (Code %lu)", Device.Problem);
        return Text;
    }

    if (!(Device.Status & DN_STARTED))
        return L"This device is not started.";
    return L"This device is working properly.";
}

std::wstring
CDevicesApp::ShortStatus(
    _In_ const DEVICE &Device)
{
    WCHAR Text[64];

    if (!Device.Present)
        return L"Not connected";
    if (IsDisabled(Device))
        return L"Disabled";
    if (Device.Problem)
    {
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"Problem, code %lu", Device.Problem);
        return Text;
    }
    if (!(Device.Status & DN_STARTED))
        return L"Not started";
    return std::wstring();
}

PCWSTR
CDevicesApp::Fill(
    _In_ const DEVICE &Device)
{
    if (HasProblem(Device))
        return DEVICE_PROBLEM_FILL;
    if (IsDisabled(Device) || !Device.Present)
        return DEVICE_OFF_FILL;
    return L"@TileFill";
}

/**
 * @brief Reads the device tree the way Device Manager shows it.
 *
 * Without hidden devices it lists present devices that do not ask to stay out of the
 * list, keeping any with a problem.
 */
VOID
CDevicesApp::Enumerate(VOID)
{
    SP_DEVINFO_DATA Data;
    HDEVINFO Set;
    WCHAR Instance[MAX_DEVICE_ID_LEN];
    DWORD Index;

    m_Devices.clear();

    Set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | (m_ShowHidden ? 0 : DIGCF_PRESENT));
    if (Set == INVALID_HANDLE_VALUE)
        return;

    Data.cbSize = sizeof(Data);
    for (Index = 0; SetupDiEnumDeviceInfo(Set, Index, &Data); Index++)
    {
        DEVICE Device;
        ULONG Status = 0;
        ULONG Problem = 0;

        if (!SetupDiGetDeviceInstanceIdW(Set, &Data, Instance, ARRAYSIZE(Instance), NULL))
            continue;

        Device.Present = (CM_Get_DevNode_Status(&Status, &Problem, Data.DevInst, 0) == CR_SUCCESS);
        Device.Status = Status;
        Device.Problem = Device.Present ? Problem : 0;

        if (!m_ShowHidden && (Status & DN_NO_SHOW_IN_DM) && !Problem)
            continue;

        Device.Instance = Instance;
        Device.Name = DeviceProperty(Set, &Data, SPDRP_FRIENDLYNAME);
        if (Device.Name.empty())
            Device.Name = DeviceProperty(Set, &Data, SPDRP_DEVICEDESC);
        if (Device.Name.empty())
            Device.Name = L"Unknown device";
        Device.Class = DeviceProperty(Set, &Data, SPDRP_CLASS);
        if (Device.Class.empty())
            Device.Class = L"Other devices";
        Device.Manufacturer = DeviceProperty(Set, &Data, SPDRP_MFG);
        Device.Location = DeviceProperty(Set, &Data, SPDRP_LOCATION_INFORMATION);
        Device.Service = DeviceProperty(Set, &Data, SPDRP_SERVICE);
        Device.HardwareId = DeviceProperty(Set, &Data, SPDRP_HARDWAREID);
        Device.Driver = DriverSummary(Set, &Data);
        m_Devices.push_back(Device);
    }
    SetupDiDestroyDeviceInfoList(Set);

    std::sort(m_Devices.begin(), m_Devices.end(), [](const DEVICE &A, const DEVICE &B)
    {
        INT Order = _wcsicmp(A.Class.c_str(), B.Class.c_str());
        return Order ? (Order < 0) : (_wcsicmp(A.Name.c_str(), B.Name.c_str()) < 0);
    });
}

VOID
CDevicesApp::ShowPage(
    _In_ DEVICE_PAGE Page)
{
    m_Page = Page;
    Scene()->SetVisible(L"PageList", Page == PageList);
    Scene()->SetVisible(L"PageDevice", Page == PageDevice);
    Scene()->SetSelected(NULL);

    if (Page == PageList)
    {
        RefreshList();
        SetChrome(L"Device Manager", L"Scan", L"Menu", ShellCloseOk);
    }
    else
    {
        RefreshDevice();
        SetChrome(L"Device Manager", IsDisabled(m_Devices[m_Current]) ? L"Enable" : L"Disable", L"Menu", ShellCloseOk);
    }
}

VOID
CDevicesApp::RefreshList(VOID)
{
    static PCWSTR const Fields[] = { L"Id", L"Name", L"Detail", L"DetailBrush", L"Glyph", L"Fill" };
    std::vector<std::wstring> Storage;
    std::vector<PCWSTR> Values;
    WCHAR Text[64];
    ULONG Problems = 0;

    m_Shown.clear();
    for (LONG Index = 0; Index < static_cast<LONG>(m_Devices.size()); Index++)
    {
        const DEVICE &Device = m_Devices[Index];
        std::wstring Status = ShortStatus(Device);

        if (Device.Problem || !Device.Present)
            Problems++;
        if (m_ProblemsOnly && !Device.Problem && Device.Present)
            continue;
        if (!m_Search.empty() && !ContainsNoCase(Device.Name, m_Search) &&
            !ContainsNoCase(Device.Class, m_Search) && !ContainsNoCase(Device.HardwareId, m_Search))
        {
            continue;
        }

        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%ld", Index);
        Storage.push_back(Text);
        Storage.push_back(Device.Name);
        Storage.push_back(Status.empty() ? Device.Class : Device.Class + L", " + Status);
        Storage.push_back(HasProblem(Device) ? DEVICE_PROBLEM_FILL : L"@AppSubText");
        Storage.push_back(HasProblem(Device) ? L"\xE7BA" : ClassGlyph(Device.Class));
        Storage.push_back(Fill(Device));
        m_Shown.push_back(Index);
    }

    for (const std::wstring &Value : Storage)
        Values.push_back(Value.c_str());

    Scene()->SetItems(L"DeviceList", static_cast<ULONG>(m_Shown.size()), ARRAYSIZE(Fields), Fields, Values.data());

    StringCchPrintfW(Text, ARRAYSIZE(Text), Problems ? L"problems (%lu)" : L"problems", Problems);
    Scene()->SetBinding(L"ProblemsLabel", Text);
    Scene()->SetBinding(L"TabAll", m_ProblemsOnly ? L"@PivotOff" : L"@AppText");
    Scene()->SetBinding(L"TabProblems", m_ProblemsOnly ? L"@AppText" : L"@PivotOff");
    Scene()->SetBinding(L"SearchText", (m_Search + (m_SearchFocus ? L"|" : L"")).c_str());
    Scene()->SetBinding(L"EmptyText", m_ProblemsOnly && m_Search.empty() ? L"Every device is working properly." : L"No devices match.");
    Scene()->SetVisible(L"SearchHint", m_Search.empty() && !m_SearchFocus);
    Scene()->SetVisible(L"ListEmpty", m_Shown.empty());
}

VOID
CDevicesApp::RefreshDevice(VOID)
{
    const DEVICE &Device = m_Devices[m_Current];

    Scene()->SetBinding(L"DevName", Device.Name.c_str());
    Scene()->SetBinding(L"DevGlyph", HasProblem(Device) ? L"\xE7BA" : ClassGlyph(Device.Class));
    Scene()->SetBinding(L"DevFill", Fill(Device));
    Scene()->SetBinding(L"DevStatus", StatusText(Device).c_str());
    Scene()->SetBinding(L"DevStatusBrush", HasProblem(Device) ? DEVICE_PROBLEM_FILL : L"@AppText");
    Scene()->SetBinding(L"DevClass", Device.Class.c_str());
    Scene()->SetBinding(L"DevMfg", Device.Manufacturer.empty() ? L"Unknown" : Device.Manufacturer.c_str());
    Scene()->SetBinding(L"DevLocation", Device.Location.empty() ? L"Unknown" : Device.Location.c_str());
    Scene()->SetBinding(L"DevDriver", Device.Driver.empty() ? L"No driver installed" : Device.Driver.c_str());
    Scene()->SetBinding(L"DevService", Device.Service.empty() ? L"None" : Device.Service.c_str());
    Scene()->SetBinding(L"DevHardwareId", Device.HardwareId.empty() ? L"None" : Device.HardwareId.c_str());
    Scene()->SetBinding(L"DevInstance", Device.Instance.c_str());
}

/**
 * @brief Enables, disables or restarts a device through its class installer.
 *
 * The change needs administrator rights; a failure or a needed reboot is reported as a toast.
 */
VOID
CDevicesApp::Act(
    _In_ LONG Index,
    _In_ DEVICE_ACTION Action)
{
    static const DWORD States[] = { DICS_ENABLE, DICS_DISABLE, DICS_PROPCHANGE };
    static PCWSTR const Verbs[] = { L"enable", L"disable", L"restart" };
    SP_PROPCHANGE_PARAMS Params;
    SP_DEVINSTALL_PARAMS_W Install;
    SP_DEVINFO_DATA Data;
    std::wstring Instance;
    HDEVINFO Set;
    WCHAR Text[160];
    DWORD Error = ERROR_SUCCESS;

    if (Index < 0 || Index >= static_cast<LONG>(m_Devices.size()))
        return;
    Instance = m_Devices[Index].Instance;

    Set = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (Set == INVALID_HANDLE_VALUE)
        return;

    Data.cbSize = sizeof(Data);
    ZeroMemory(&Params, sizeof(Params));
    Params.ClassInstallHeader.cbSize = sizeof(Params.ClassInstallHeader);
    Params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    Params.StateChange = States[Action];
    Params.Scope = DICS_FLAG_GLOBAL;

    if (!SetupDiOpenDeviceInfoW(Set, Instance.c_str(), NULL, 0, &Data) ||
        !SetupDiSetClassInstallParamsW(Set, &Data, &Params.ClassInstallHeader, sizeof(Params)) ||
        !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, Set, &Data))
    {
        Error = GetLastError();
    }

    if (Error != ERROR_SUCCESS)
    {
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"Could not %s the device, error %lu.%s", Verbs[Action], Error,
                         (Error == ERROR_ACCESS_DENIED) ? L" This needs administrator rights." : L"");
        Notify(L"Device Manager", Text);
    }
    else
    {
        Install.cbSize = sizeof(Install);
        if (SetupDiGetDeviceInstallParamsW(Set, &Data, &Install) && (Install.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)))
            Notify(L"Device Manager", L"Restart the device to finish this change.");
    }
    SetupDiDestroyDeviceInfoList(Set);

    /* The list order may change, so the device is found again by its instance path */
    Enumerate();
    m_Current = -1;
    for (LONG Search = 0; Search < static_cast<LONG>(m_Devices.size()); Search++)
    {
        if (m_Devices[Search].Instance == Instance)
        {
            m_Current = Search;
            break;
        }
    }
    ShowPage((m_Page == PageDevice && m_Current >= 0) ? PageDevice : PageList);
}

VOID
CDevicesApp::Rescan(VOID)
{
    DEVINST Root;

    if (CM_Locate_DevNodeW(&Root, NULL, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
        CM_Reenumerate_DevNode(Root, CM_REENUMERATE_SYNCHRONOUS);

    Enumerate();
    ShowPage(PageList);
}

VOID
CDevicesApp::OnCommand(
    _In_z_ PCWSTR Command)
{
    if (_wcsicmp(Command, L"filter:all") == 0 || _wcsicmp(Command, L"filter:problems") == 0)
    {
        m_ProblemsOnly = (_wcsicmp(Command + 7, L"problems") == 0);
        Scene()->ResetScroll(L"ListScroll");
        RefreshList();
    }
    else if (_wcsicmp(Command, L"search.focus") == 0)
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
    else if (_wcsnicmp(Command, L"open:", 5) == 0)
    {
        m_Current = _wtol(Command + 5);
        if (m_Current >= 0 && m_Current < static_cast<LONG>(m_Devices.size()))
        {
            m_SearchFocus = FALSE;
            ShowInputPanel(FALSE);
            ShowPage(PageDevice);
        }
    }
    else
    {
        CAppClient::OnCommand(Command);
    }
}

VOID
CDevicesApp::OnHold(
    _In_z_ PCWSTR Command,
    _In_ FLOAT X,
    _In_ FLOAT Y)
{
    LONG Index;

    if (_wcsnicmp(Command, L"hold:", 5) != 0)
        return;

    Index = _wtol(Command + 5);
    if (Index < 0 || Index >= static_cast<LONG>(m_Devices.size()))
        return;

    m_HoldTarget = Index;
    m_Menu = MenuDevice;
    ShowMenu(AppMenuPoint, X, Y, IsDisabled(m_Devices[Index]) ? L"Enable device\nRestart device\n-\nProperties"
                                                               : L"Disable device\nRestart device\n-\nProperties");
}

VOID
CDevicesApp::OnSoftKey(
    _In_ SHELL_SOFTKEY Key)
{
    if (m_Page == PageList)
    {
        if (Key == ShellSoftKeyLeft)
        {
            Rescan();
        }
        else if (Key == ShellSoftKeyRight)
        {
            m_Menu = MenuList;
            ShowMenu(AppMenuRightSoftKey, 0.0f, 0.0f,
                     m_ShowHidden ? L"Scan for hardware changes\nHide hidden devices\n-\nClear search"
                                  : L"Scan for hardware changes\nShow hidden devices\n-\nClear search");
        }
        return;
    }

    if (Key == ShellSoftKeyLeft)
    {
        Act(m_Current, IsDisabled(m_Devices[m_Current]) ? ActionEnable : ActionDisable);
    }
    else if (Key == ShellSoftKeyRight)
    {
        m_HoldTarget = m_Current;
        m_Menu = MenuDevice;
        ShowMenu(AppMenuRightSoftKey, 0.0f, 0.0f, IsDisabled(m_Devices[m_Current]) ? L"Enable device\nRestart device"
                                                                                   : L"Disable device\nRestart device");
    }
}

VOID
CDevicesApp::OnCloseButton(VOID)
{
    if (m_Page == PageDevice)
        ShowPage(PageList);
    else
        RequestClose();
}

VOID
CDevicesApp::OnMenuPick(
    _In_ LONG Index)
{
    DEVICE_MENU Menu = m_Menu;

    m_Menu = MenuNone;
    if (Index < 0)
        return;

    if (Menu == MenuList)
    {
        if (Index == 0)
        {
            Rescan();
        }
        else if (Index == 1)
        {
            m_ShowHidden = !m_ShowHidden;
            Enumerate();
            ShowPage(PageList);
        }
        else if (Index == 2)
        {
            OnCommand(L"search.clear");
        }
    }
    else if (Menu == MenuDevice && m_HoldTarget >= 0 && m_HoldTarget < static_cast<LONG>(m_Devices.size()))
    {
        if (Index == 0)
        {
            Act(m_HoldTarget, IsDisabled(m_Devices[m_HoldTarget]) ? ActionEnable : ActionDisable);
        }
        else if (Index == 1)
        {
            Act(m_HoldTarget, ActionRestart);
        }
        else if (Index == 2)
        {
            m_Current = m_HoldTarget;
            ShowPage(PageDevice);
        }
    }
}

VOID
CDevicesApp::OnChar(
    _In_ WCHAR Char)
{
    if (m_Page != PageList || Char < 0x20)
        return;

    /* Typing on the list goes to the search box */
    m_Search += Char;
    m_SearchFocus = TRUE;
    Scene()->ResetScroll(L"ListScroll");
    RefreshList();
}

VOID
CDevicesApp::OnKey(
    _In_ UINT VirtualKey)
{
    if (VirtualKey == VK_BACK && m_Page == PageList)
    {
        if (!m_Search.empty())
            m_Search.pop_back();
        RefreshList();
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
    CDevicesApp App;
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
