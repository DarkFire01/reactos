/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Settings: tabbed icon pages with live system pages, owner card, sounds and themes
 */

#include <devstatus.h>
#include <appclient.h>
#include <uixxml.h>
#include <strsafe.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include "sysinfo.h"

/*
 * Markup contract (settings.uix):
 *   Pages: PagePersonal, PageSystem, PageConnections, PageInfo, PageOwner, PageSounds,
 *          PageToday, PageDetail, PageAbout; element Tabs
 *   Repeaters: InfoRows and AboutRows (Label, Value), Themes (Name, Fill, Command)
 *   Bindings: TabPersonal/System/Connections, InfoTitle, DetailTitle, Owner0..4, OwnerFill0..4,
 *             SoundVolume, SoundDevice, MuteLabel, Vol1..Vol10, AboutProduct, AboutEdition,
 *             AboutTabVersion/Device/Copyrights
 *   Commands: tab:<Name>, open:<Page>, owner:<n>, volume.up, volume.down, volume.mute, theme:<Name>, about:<Tab>
 */

#define OWNER_FIELDS 5
#define OWNER_KEY L"Software\\ReactOSMobile\\Owner"

static PCWSTR const g_Tabs[] = { L"Personal", L"System", L"Connections" };
static PCWSTR const g_OwnerValues[OWNER_FIELDS] = { L"Name", L"Company", L"Phone", L"Email", L"Notes" };

typedef enum _SETTINGS_PAGE
{
    PageTabs,
    PageInfo,
    PageOwner,
    PageSounds,
    PageToday,
    PageDetail,
    PageAbout
} SETTINGS_PAGE;

typedef struct _INFO_ROW
{
    std::wstring Label;
    std::wstring Value;
} INFO_ROW;

static
std::wstring
Format(
    _In_z_ _Printf_format_string_ PCWSTR Pattern,
    ...)
{
    WCHAR Text[256];
    va_list Arguments;

    va_start(Arguments, Pattern);
    StringCchVPrintfW(Text, ARRAYSIZE(Text), Pattern, Arguments);
    va_end(Arguments);
    return Text;
}

static
std::wstring
Megabytes(
    _In_ ULONGLONG Bytes)
{
    if (Bytes >= 10ULL * 1024 * 1024 * 1024)
        return Format(L"%.1f GB", Bytes / (1024.0 * 1024.0 * 1024.0));
    return Format(L"%llu MB", Bytes / (1024 * 1024));
}

class CSettingsApp : public CAppClient
{
protected:
    VOID OnActivate(VOID) override;

    VOID OnCommand(
        _In_z_ PCWSTR Command) override;

    VOID OnCloseButton(VOID) override;

    VOID OnSoftKey(
        _In_ SHELL_SOFTKEY Key) override;

    VOID OnChar(
        _In_ WCHAR Char) override;

    VOID OnKey(
        _In_ UINT VirtualKey) override;

private:
    VOID
    ShowTab(
        _In_ ULONG Tab);

    VOID
    ShowPage(
        _In_ SETTINGS_PAGE Page,
        _In_z_ PCWSTR Title);

    VOID
    Open(
        _In_z_ PCWSTR Page);

    VOID
    ShowInfo(
        _In_z_ PCWSTR Title,
        _In_ const std::vector<INFO_ROW> &Rows);

    std::wstring
    SkinDirectory(VOID) const;

    /* Info page builders */
    VOID ShowAbout(_In_z_ PCWSTR Tab);
    VOID InfoPower(_Inout_ std::vector<INFO_ROW> &Rows);
    VOID InfoClock(_Inout_ std::vector<INFO_ROW> &Rows);
    VOID InfoMemory(_Inout_ std::vector<INFO_ROW> &Rows);
    VOID InfoRegional(_Inout_ std::vector<INFO_ROW> &Rows);
    VOID InfoScreen(_Inout_ std::vector<INFO_ROW> &Rows);
    VOID InfoTasks(_Inout_ std::vector<INFO_ROW> &Rows);
    VOID InfoNetwork(_Inout_ std::vector<INFO_ROW> &Rows);
    VOID InfoPrograms(_Inout_ std::vector<INFO_ROW> &Rows);
    VOID InfoButtons(_Inout_ std::vector<INFO_ROW> &Rows);

    VOID
    LoadOwner(VOID);

    VOID
    SaveOwner(VOID);

    VOID
    RefreshOwner(VOID);

    VOID
    RefreshSounds(VOID);

    VOID
    RefreshThemes(VOID);

    ULONG m_Tab = 0;
    SETTINGS_PAGE m_Page = PageTabs;
    std::wstring m_Owner[OWNER_FIELDS];
    LONG m_Field = 0;
    std::wstring m_Theme;
    std::wstring m_AboutTab = L"Version";
    BOOL m_Started = FALSE;
    BOOL m_Direct = FALSE;
};

VOID
CSettingsApp::OnActivate(VOID)
{
    PCWSTR Page;

    /* A shortcut can open one page directly; ok then closes the app instead of going back */
    if (!m_Started)
    {
        m_Started = TRUE;
        Page = Argument(L"page");
        if (Page)
        {
            m_Direct = TRUE;
            Open(Page);
            return;
        }
        ShowTab(0);
        return;
    }

    if (m_Page == PageTabs)
        ShowTab(m_Tab);
}

VOID
CSettingsApp::ShowPage(
    _In_ SETTINGS_PAGE Page,
    _In_z_ PCWSTR Title)
{
    static PCWSTR const Pages[] = { L"PageInfo", L"PageOwner", L"PageSounds", L"PageToday", L"PageDetail", L"PageAbout" };
    WCHAR Name[32];

    m_Page = Page;
    for (ULONG Index = 0; Index < ARRAYSIZE(g_Tabs); Index++)
    {
        StringCchPrintfW(Name, ARRAYSIZE(Name), L"Page%s", g_Tabs[Index]);
        Scene()->SetVisible(Name, Page == PageTabs && Index == m_Tab);
    }
    for (ULONG Index = 0; Index < ARRAYSIZE(Pages); Index++)
        Scene()->SetVisible(Pages[Index], static_cast<ULONG>(Page) == Index + 1);

    Scene()->SetVisible(L"Tabs", Page == PageTabs);
    Scene()->SetSelected(NULL);
    Scene()->ResetScroll(L"InfoScroll");

    if (Page == PageOwner)
        SetChrome(Title, L"Done", L"", ShellCloseOk);
    else
        SetChrome(Title, L"", L"", ShellCloseOk);
}

VOID
CSettingsApp::ShowTab(
    _In_ ULONG Tab)
{
    WCHAR Binding[32];

    m_Tab = Tab;
    for (ULONG Index = 0; Index < ARRAYSIZE(g_Tabs); Index++)
    {
        StringCchPrintfW(Binding, ARRAYSIZE(Binding), L"Tab%s", g_Tabs[Index]);
        Scene()->SetBinding(Binding, (Index == Tab) ? L"@TabActive" : L"@TabInactive");
    }
    ShowPage(PageTabs, L"Settings");
}

VOID
CSettingsApp::ShowInfo(
    _In_z_ PCWSTR Title,
    _In_ const std::vector<INFO_ROW> &Rows)
{
    static PCWSTR const Fields[] = { L"Label", L"Value" };
    std::vector<PCWSTR> Values;

    for (const INFO_ROW &Row : Rows)
    {
        Values.push_back(Row.Label.c_str());
        Values.push_back(Row.Value.c_str());
    }

    Scene()->SetBinding(L"InfoTitle", Title);
    Scene()->SetItems(L"InfoRows", static_cast<ULONG>(Rows.size()), ARRAYSIZE(Fields), Fields, Values.data());
    ShowPage(PageInfo, Title);
}

std::wstring
CSettingsApp::SkinDirectory(VOID) const
{
    std::wstring Path = MarkupFile();
    SIZE_T Slash;

    /* The markup lives in <skin>\apps\settings.uix */
    for (ULONG Level = 0; Level < 2; Level++)
    {
        Slash = Path.find_last_of(L"\\/");
        if (Slash == std::wstring::npos)
            return L".";
        Path.erase(Slash);
    }
    return Path;
}

VOID
CSettingsApp::ShowAbout(
    _In_z_ PCWSTR Tab)
{
    static PCWSTR const Fields[] = { L"Label", L"Value" };
    static PCWSTR const BiosKey = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    std::vector<INFO_ROW> Rows;
    std::vector<PCWSTR> Values;
    std::vector<SYS_VOLUME> Volumes;
    std::wstring Slot = L"Empty";
    std::wstring Text;
    SYS_OS Os;
    SYS_CPU Cpu;
    SYS_MEMORY Memory;
    UIX_XML_NODE Skin;
    WCHAR Name[256];
    DWORD Size;

    m_AboutTab = Tab;
    SysQueryOs(&Os);

    /* The header reads like the device's: the shell's brand, then what it runs on */
    Scene()->SetBinding(L"AboutProduct", L"ReactOS Mobile 6.1");
    Scene()->SetBinding(L"AboutEdition", (Os.Product + (Os.Release.empty() ? L"" : L" " + Os.Release)).c_str());

    if (_wcsicmp(Tab, L"Device") == 0)
    {
        Size = ARRAYSIZE(Name);
        if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, Name, &Size))
            Rows.push_back({ L"Device name", Name });

        Text = SysRegString(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\LanmanServer\\Parameters", L"srvcomment");
        Rows.push_back({ L"Description", Text.empty() ? L"ReactOS Mobile device" : Text });

        /* SMBIOS strings, which on a phone come from the UEFI firmware */
        Text = SysRegString(HKEY_LOCAL_MACHINE, BiosKey, L"SystemManufacturer");
        if (!Text.empty())
            Rows.push_back({ L"Manufacturer", Text });
        Text = SysRegString(HKEY_LOCAL_MACHINE, BiosKey, L"SystemProductName");
        if (!Text.empty())
            Rows.push_back({ L"Model", Text });
        Text = SysRegString(HKEY_LOCAL_MACHINE, BiosKey, L"BIOSVendor");
        if (!Text.empty())
            Rows.push_back({ L"Firmware", Text + L" " + SysRegString(HKEY_LOCAL_MACHINE, BiosKey, L"BIOSVersion") });
    }
    else if (_wcsicmp(Tab, L"Copyrights") == 0)
    {
        Rows.push_back({ L"ReactOS Mobile", L"Phone shell built on the Windows 10 Mobile shell architecture, wearing a Windows Mobile 6.1 skin." });
        if (Os.IsReactOS)
            Rows.push_back({ L"ReactOS", L"Copyright the ReactOS Team and contributors. Licensed under the GNU GPL." });
        else
            Rows.push_back({ L"Host system", L"Running on " + Os.Product + L". Its own copyright notices apply to it." });
        Rows.push_back({ L"Icons", L"Windows Mobile 6.5 icon set, used as reference artwork." });
        Rows.push_back({ L"Fonts", L"Tahoma and Segoe MDL2 Assets, from the system." });
    }
    else
    {
        SysQueryCpu(&Cpu);
        SysQueryMemory(&Memory);
        SysQueryVolumes(Volumes);

        /* ProductName already carries the edition; EditionID is an internal code name like "Core" */
        Rows.push_back({ L"Operating system", Os.Product });
        if (!Os.Release.empty())
            Rows.push_back({ L"Version", Os.Release + (Os.ServicePack.empty() ? L"" : L", " + Os.ServicePack) });
        if (!Os.Build.empty())
            Rows.push_back({ L"Build", Os.Build });
        Rows.push_back({ L"Kernel", Os.Kernel });

        Rows.push_back({ L"Processor", Cpu.Name.empty() ? Cpu.Architecture : Cpu.Name });
        Text = Cpu.Architecture;
        if (Cpu.Cores)
            Text += Format(L", %lu cores", Cpu.Cores);
        Text += Format(L", %lu threads", Cpu.Logical);
        if (Cpu.MaxMhz)
            Text += Format(L", %lu MHz", Cpu.MaxMhz);
        Rows.push_back({ L"Processor details", Text });

        Text = Megabytes(Memory.Installed) + L" RAM";
        if (Memory.Installed > Memory.Usable + 16ULL * 1024 * 1024)
            Text += L" (" + Megabytes(Memory.Usable) + L" usable)";
        Rows.push_back({ L"Memory", Text });

        /* A removable volume plays the part of the storage card slot */
        for (const SYS_VOLUME &Volume : Volumes)
        {
            if (Volume.Type == DRIVE_REMOVABLE)
            {
                Slot = L"In use, " + Volume.Root + L" " + Megabytes(Volume.Total);
                break;
            }
        }
        Rows.push_back({ L"Expansion slot", Slot });

        LoadOwner();
        Rows.push_back({ L"Owner", m_Owner[0].empty() ? L"Not set" : m_Owner[0] });
        if (SUCCEEDED(UixXmlLoad((SkinDirectory() + L"\\skin.xml").c_str(), &Skin)))
            Rows.push_back({ L"Shell skin", Skin.Attr(L"Name", L"") });
    }

    for (const INFO_ROW &Row : Rows)
    {
        Values.push_back(Row.Label.c_str());
        Values.push_back(Row.Value.c_str());
    }
    Scene()->SetItems(L"AboutRows", static_cast<ULONG>(Rows.size()), ARRAYSIZE(Fields), Fields, Values.data());
    Scene()->SetBinding(L"AboutTabVersion", (_wcsicmp(Tab, L"Version") == 0) ? L"@TabActive" : L"@TabInactive");
    Scene()->SetBinding(L"AboutTabDevice", (_wcsicmp(Tab, L"Device") == 0) ? L"@TabActive" : L"@TabInactive");
    Scene()->SetBinding(L"AboutTabCopyrights", (_wcsicmp(Tab, L"Copyrights") == 0) ? L"@TabActive" : L"@TabInactive");
    Scene()->SetVisible(L"AboutBrand", _wcsicmp(Tab, L"Version") == 0);

    if (m_Page != PageAbout)
        ShowPage(PageAbout, L"About");
    Scene()->ResetScroll(L"AboutScroll");
}

VOID
CSettingsApp::InfoPower(
    _Inout_ std::vector<INFO_ROW> &Rows)
{
    DEV_STATUS Status = {};

    DevQueryPower(&Status);
    Rows.push_back({ L"Main battery", Status.HasBattery ? Format(L"%lu%%", Status.BatteryPercent) : L"Not present" });
    if (Status.HasBattery)
        Rows.push_back({ L"Status", Status.Charging ? L"Charging" : (Status.OnAc ? L"Plugged in, not charging" : L"Discharging") });
    Rows.push_back({ L"Power source", Status.OnAc ? L"External power" : L"Battery" });
    if (Status.HasBattery && !Status.OnAc && Status.SecondsLeft != (ULONG)-1)
        Rows.push_back({ L"Time remaining", Format(L"%lu h %02lu min", Status.SecondsLeft / 3600, (Status.SecondsLeft / 60) % 60) });
}

VOID
CSettingsApp::InfoClock(
    _Inout_ std::vector<INFO_ROW> &Rows)
{
    DYNAMIC_TIME_ZONE_INFORMATION Zone;
    WCHAR Text[96];
    ULONGLONG Uptime = GetTickCount64() / 1000;
    LONG Bias;

    if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, NULL, NULL, Text, ARRAYSIZE(Text)))
        Rows.push_back({ L"Time", Text });
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, NULL, NULL, Text, ARRAYSIZE(Text), NULL))
        Rows.push_back({ L"Date", Text });

    if (GetDynamicTimeZoneInformation(&Zone) != TIME_ZONE_ID_INVALID)
    {
        Bias = -Zone.Bias;
        Rows.push_back({ L"Time zone", Zone.TimeZoneKeyName[0] ? Zone.TimeZoneKeyName : Zone.StandardName });
        Rows.push_back({ L"Offset", Format(L"GMT%c%02ld:%02ld", (Bias < 0) ? L'-' : L'+', labs(Bias) / 60, labs(Bias) % 60) });
    }
    Rows.push_back({ L"Up time", Format(L"%llu d %llu h %llu min", Uptime / 86400, (Uptime / 3600) % 24, (Uptime / 60) % 60) });
    Rows.push_back({ L"Alarms", L"None set" });
}

VOID
CSettingsApp::InfoMemory(
    _Inout_ std::vector<INFO_ROW> &Rows)
{
    std::vector<SYS_VOLUME> Volumes;
    SYS_MEMORY Memory;
    std::wstring Name;

    SysQueryMemory(&Memory);
    Rows.push_back({ L"Installed", Megabytes(Memory.Installed) });
    Rows.push_back({ L"Program memory", Format(L"%s usable", Megabytes(Memory.Usable).c_str()) });
    Rows.push_back({ L"In use", Format(L"%s (%lu%%)", Megabytes(Memory.Usable - Memory.Available).c_str(), Memory.Load) });
    Rows.push_back({ L"Free", Megabytes(Memory.Available) });
    Rows.push_back({ L"Committed", Format(L"%s of %s", Megabytes(Memory.CommitUsed).c_str(), Megabytes(Memory.CommitLimit).c_str()) });

    /* Every fixed and removable volume; removable ones are the storage card */
    SysQueryVolumes(Volumes);
    for (const SYS_VOLUME &Volume : Volumes)
    {
        Name = (Volume.Type == DRIVE_REMOVABLE) ? L"Storage card " : L"Storage ";
        Name += Volume.Root;
        if (!Volume.Label.empty())
            Name += L" " + Volume.Label;
        Rows.push_back({ Name, Format(L"%s free of %s, %s", Megabytes(Volume.Free).c_str(), Megabytes(Volume.Total).c_str(),
                                      Volume.FileSystem.c_str()) });
    }
}

VOID
CSettingsApp::InfoRegional(
    _Inout_ std::vector<INFO_ROW> &Rows)
{
    WCHAR Locale[LOCALE_NAME_MAX_LENGTH];
    WCHAR Text[96];

    if (!GetUserDefaultLocaleName(Locale, ARRAYSIZE(Locale)))
        return;

    if (GetLocaleInfoEx(Locale, LOCALE_SLOCALIZEDDISPLAYNAME, Text, ARRAYSIZE(Text)))
        Rows.push_back({ L"Region", Text });
    Rows.push_back({ L"Locale", Locale });
    if (GetDateFormatEx(Locale, DATE_SHORTDATE, NULL, NULL, Text, ARRAYSIZE(Text), NULL))
        Rows.push_back({ L"Short date", Text });
    if (GetDateFormatEx(Locale, DATE_LONGDATE, NULL, NULL, Text, ARRAYSIZE(Text), NULL))
        Rows.push_back({ L"Long date", Text });
    if (GetTimeFormatEx(Locale, 0, NULL, NULL, Text, ARRAYSIZE(Text)))
        Rows.push_back({ L"Time", Text });
    if (GetNumberFormatEx(Locale, 0, L"123456789.00", NULL, Text, ARRAYSIZE(Text)))
        Rows.push_back({ L"Number", Text });
    if (GetCurrencyFormatEx(Locale, 0, L"123456789.00", NULL, Text, ARRAYSIZE(Text)))
        Rows.push_back({ L"Currency", Text });
}

VOID
CSettingsApp::InfoScreen(
    _Inout_ std::vector<INFO_ROW> &Rows)
{
    DEVMODEW Mode = { 0 };

    Mode.dmSize = sizeof(Mode);
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &Mode))
    {
        Rows.push_back({ L"Display", Format(L"%lu x %lu, %lu Hz", Mode.dmPelsWidth, Mode.dmPelsHeight, Mode.dmDisplayFrequency) });
        Rows.push_back({ L"Color", Format(L"%lu bit", Mode.dmBitsPerPel) });
    }
    Rows.push_back({ L"Shell canvas", L"240 x 320 logical, portrait" });
    Rows.push_back({ L"Text smoothing", L"Grayscale, composed by DirectComposition" });
}

VOID
CSettingsApp::InfoTasks(
    _Inout_ std::vector<INFO_ROW> &Rows)
{
    static PCWSTR const Ours[] = { L"mobileshell.exe", L"mobsettings.exe", L"mobcontacts.exe", L"mobcalendar.exe",
                                   L"mobcalc.exe", L"mobmarkup.exe", L"moboffice.exe", L"mobfiles.exe" };
    PROCESS_MEMORY_COUNTERS Counters;
    PROCESSENTRY32W Entry = { sizeof(Entry) };
    HANDLE Snapshot;
    HANDLE Process;
    std::wstring Value;

    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Snapshot == INVALID_HANDLE_VALUE)
        return;

    /* Only the shell and its apps, like the device's running programs list */
    if (Process32FirstW(Snapshot, &Entry))
    {
        do
        {
            for (PCWSTR Name : Ours)
            {
                if (_wcsicmp(Entry.szExeFile, Name) != 0)
                    continue;

                Value = Format(L"PID %lu", Entry.th32ProcessID);
                Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Entry.th32ProcessID);
                if (Process)
                {
                    if (GetProcessMemoryInfo(Process, &Counters, sizeof(Counters)))
                        Value += L", " + Megabytes(Counters.WorkingSetSize);
                    CloseHandle(Process);
                }
                Rows.push_back({ Entry.szExeFile, Value });
            }
        } while (Process32NextW(Snapshot, &Entry));
    }
    CloseHandle(Snapshot);
}

VOID
CSettingsApp::InfoNetwork(
    _Inout_ std::vector<INFO_ROW> &Rows)
{
    PIP_ADAPTER_ADDRESSES Addresses;
    PIP_ADAPTER_ADDRESSES Walk;
    DEV_STATUS Status = {};
    ULONG Size = 32 * 1024;
    WCHAR Address[64];
    DWORD Length;

    DevQueryNetwork(&Status);
    if (Status.Network == DevNetworkWireless)
        Rows.push_back({ L"Wi-Fi", Format(L"Connected to %s, signal %lu%%", Status.Ssid[0] ? Status.Ssid : L"network", Status.Quality) });
    else if (Status.Network == DevNetworkWired)
        Rows.push_back({ L"Network", Format(L"Connected through %s", Status.Adapter) });
    else
        Rows.push_back({ L"Network", L"Not connected" });

    Addresses = static_cast<PIP_ADAPTER_ADDRESSES>(HeapAlloc(GetProcessHeap(), 0, Size));
    if (!Addresses)
        return;

    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST,
                             NULL, Addresses, &Size) == ERROR_SUCCESS)
    {
        for (Walk = Addresses; Walk; Walk = Walk->Next)
        {
            std::wstring Value = (Walk->OperStatus == IfOperStatusUp) ? L"Up" : L"Down";

            if (Walk->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            if (Walk->IfType == IF_TYPE_IEEE80211)
                Value += L", wireless";

            Length = ARRAYSIZE(Address);
            if (Walk->FirstUnicastAddress &&
                WSAAddressToStringW(Walk->FirstUnicastAddress->Address.lpSockaddr,
                                    Walk->FirstUnicastAddress->Address.iSockaddrLength, NULL, Address, &Length) == 0)
            {
                Value += std::wstring(L", ") + Address;
            }
            Rows.push_back({ Walk->FriendlyName, Value });
        }
    }
    HeapFree(GetProcessHeap(), 0, Addresses);
}

VOID
CSettingsApp::InfoPrograms(
    _Inout_ std::vector<INFO_ROW> &Rows)
{
    UIX_XML_NODE Shell;
    std::wstring Config = SkinDirectory();

    /* shell.xml sits two levels above the skin: bin\skins\<skin> */
    for (ULONG Level = 0; Level < 2; Level++)
        Config.erase(Config.find_last_of(L"\\/"));

    if (FAILED(UixXmlLoad((Config + L"\\shell.xml").c_str(), &Shell)))
        return;

    for (const UIX_XML_NODE &Section : Shell.Children)
    {
        if (Section.Name != L"Apps")
            continue;
        for (const UIX_XML_NODE &App : Section.Children)
        {
            if (App.Attr(L"Args"))
                continue;
            Rows.push_back({ App.Attr(L"Title", App.Attr(L"Id", L"")),
                             Format(L"%s, %s", App.Attr(L"Exe", L""), App.Attr(L"Scale", L"Native")) });
        }
    }
    Rows.push_back({ L"Removing", L"Built in programs cannot be removed" });
}

VOID
CSettingsApp::InfoButtons(
    _Inout_ std::vector<INFO_ROW> &Rows)
{
    UIX_XML_NODE Shell;
    std::wstring Config = SkinDirectory();

    for (ULONG Level = 0; Level < 2; Level++)
        Config.erase(Config.find_last_of(L"\\/"));

    if (FAILED(UixXmlLoad((Config + L"\\shell.xml").c_str(), &Shell)))
        return;

    /* The key remap is the button configuration */
    for (const UIX_XML_NODE &Section : Shell.Children)
    {
        if (Section.Name != L"Keys")
            continue;
        for (const UIX_XML_NODE &Key : Section.Children)
            Rows.push_back({ Key.Attr(L"Vk", L""), Key.Attr(L"Command", L"") });
    }
    Rows.push_back({ L"Remap", L"Edit shell.xml and press F5 in the shell" });
}

VOID
CSettingsApp::LoadOwner(VOID)
{
    WCHAR Text[256];
    DWORD Size;

    for (ULONG Index = 0; Index < OWNER_FIELDS; Index++)
    {
        Size = sizeof(Text);
        if (RegGetValueW(HKEY_CURRENT_USER, OWNER_KEY, g_OwnerValues[Index], RRF_RT_REG_SZ, NULL, Text, &Size) == ERROR_SUCCESS)
            m_Owner[Index] = Text;
        else
            m_Owner[Index].clear();
    }
}

VOID
CSettingsApp::SaveOwner(VOID)
{
    HKEY Key;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, OWNER_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &Key, NULL) != ERROR_SUCCESS)
        return;

    for (ULONG Index = 0; Index < OWNER_FIELDS; Index++)
    {
        RegSetValueExW(Key, g_OwnerValues[Index], 0, REG_SZ, reinterpret_cast<const BYTE *>(m_Owner[Index].c_str()),
                       static_cast<DWORD>((m_Owner[Index].size() + 1) * sizeof(WCHAR)));
    }
    RegCloseKey(Key);
}

VOID
CSettingsApp::RefreshOwner(VOID)
{
    WCHAR Name[16];

    for (LONG Index = 0; Index < OWNER_FIELDS; Index++)
    {
        StringCchPrintfW(Name, ARRAYSIZE(Name), L"Owner%ld", Index);
        Scene()->SetBinding(Name, (m_Owner[Index] + ((Index == m_Field) ? L"|" : L"")).c_str());
        StringCchPrintfW(Name, ARRAYSIZE(Name), L"OwnerFill%ld", Index);
        Scene()->SetBinding(Name, (Index == m_Field) ? L"@FieldActive" : L"@FieldNormal");
    }
}

VOID
CSettingsApp::RefreshSounds(VOID)
{
    DEV_STATUS Status = {};
    WCHAR Name[8];
    LONG Lit;

    DevQueryVolume(&Status);
    if (Status.Volume < 0)
        Scene()->SetBinding(L"SoundVolume", L"No sound device");
    else if (Status.Muted)
        Scene()->SetBinding(L"SoundVolume", L"Sound off");
    else
        Scene()->SetBinding(L"SoundVolume", Format(L"Volume %ld%%", Status.Volume).c_str());
    Scene()->SetBinding(L"MuteLabel", Status.Muted ? L"Turn sound on" : L"Turn sound off");

    Lit = (Status.Volume < 0) ? 0 : (Status.Volume + 5) / 10;
    for (LONG Segment = 1; Segment <= 10; Segment++)
    {
        StringCchPrintfW(Name, ARRAYSIZE(Name), L"Vol%ld", Segment);
        Scene()->SetBinding(Name, (Segment <= Lit && !Status.Muted) ? L"@ItemSelected" : L"@ScrollTrack");
    }
}

VOID
CSettingsApp::RefreshThemes(VOID)
{
    static PCWSTR const Fields[] = { L"Name", L"Fill", L"Command" };
    std::vector<std::wstring> Storage;
    std::vector<PCWSTR> Values;
    UIX_XML_NODE Skin;
    ULONG Count = 0;

    if (FAILED(UixXmlLoad((SkinDirectory() + L"\\skin.xml").c_str(), &Skin)))
        return;

    if (m_Theme.empty())
        m_Theme = ThemeFile();

    /* Themes are listed by the skin, the current one is matched by its file */
    for (const UIX_XML_NODE &Theme : Skin.Children)
    {
        std::wstring File;
        PCWSTR Name = Theme.Attr(L"Name");

        if (Theme.Name != L"Theme" || !Name)
            continue;

        File = Theme.Attr(L"File", L"");
        BOOL Current = (m_Theme.size() >= File.size() &&
                        _wcsicmp(m_Theme.c_str() + m_Theme.size() - File.size(), File.c_str()) == 0) ||
                       _wcsicmp(m_Theme.c_str(), Name) == 0;

        Storage.insert(Storage.end(), { Name, Current ? L"@ItemSelected" : L"", std::wstring(L"theme:") + Name });
        Count++;
    }

    for (const std::wstring &Value : Storage)
        Values.push_back(Value.c_str());
    Scene()->SetItems(L"Themes", Count, ARRAYSIZE(Fields), Fields, Values.data());
}

VOID
CSettingsApp::Open(
    _In_z_ PCWSTR Page)
{
    std::vector<INFO_ROW> Rows;

    if (_wcsicmp(Page, L"Owner") == 0)
    {
        LoadOwner();
        m_Field = 0;
        RefreshOwner();
        ShowPage(PageOwner, L"Owner Information");
        ShowInputPanel(TRUE);
    }
    else if (_wcsicmp(Page, L"Sounds") == 0)
    {
        RefreshSounds();
        ShowPage(PageSounds, L"Sounds");
    }
    else if (_wcsicmp(Page, L"Today") == 0)
    {
        RefreshThemes();
        ShowPage(PageToday, L"Today");
    }
    else if (_wcsicmp(Page, L"About") == 0)
    {
        ShowAbout(L"Version");
    }
    else if (_wcsicmp(Page, L"Power") == 0 || _wcsicmp(Page, L"Backlight") == 0)
    {
        InfoPower(Rows);
        ShowInfo(L"Power", Rows);
    }
    else if (_wcsicmp(Page, L"Clock") == 0)
    {
        InfoClock(Rows);
        ShowInfo(L"Clock & Alarms", Rows);
    }
    else if (_wcsicmp(Page, L"Memory") == 0)
    {
        InfoMemory(Rows);
        ShowInfo(L"Memory", Rows);
    }
    else if (_wcsicmp(Page, L"Regional") == 0)
    {
        InfoRegional(Rows);
        ShowInfo(L"Regional Settings", Rows);
    }
    else if (_wcsicmp(Page, L"Screen") == 0)
    {
        InfoScreen(Rows);
        ShowInfo(L"Screen", Rows);
    }
    else if (_wcsicmp(Page, L"TaskManager") == 0)
    {
        InfoTasks(Rows);
        ShowInfo(L"Task Manager", Rows);
    }
    else if (_wcsicmp(Page, L"WiFi") == 0 || _wcsicmp(Page, L"Connections") == 0 || _wcsicmp(Page, L"Wireless") == 0)
    {
        InfoNetwork(Rows);
        ShowInfo(L"Connections", Rows);
    }
    else if (_wcsicmp(Page, L"Programs") == 0)
    {
        InfoPrograms(Rows);
        ShowInfo(L"Remove Programs", Rows);
    }
    else if (_wcsicmp(Page, L"Buttons") == 0)
    {
        InfoButtons(Rows);
        ShowInfo(L"Buttons", Rows);
    }
    else if (_wcsicmp(Page, L"Input") == 0)
    {
        Rows.push_back({ L"Input method", L"Keyboard" });
        Rows.push_back({ L"Layout", L"QWERTY, Shift for one key, CAP stays on, 123 for symbols" });
        Rows.push_back({ L"Hardware keyboard", L"Typing goes to the active program" });
        Rows.push_back({ L"Show input panel", L"Center soft key or F3" });
        ShowInfo(L"Input", Rows);
    }
    else if (_wcsicmp(Page, L"Lock") == 0)
    {
        Rows.push_back({ L"Device lock", L"Off" });
        Rows.push_back({ L"Password", L"Not set" });
        Rows.push_back({ L"Note", L"Locking is not available in this build" });
        ShowInfo(L"Lock", Rows);
    }
    else
    {
        Scene()->SetBinding(L"DetailTitle", Page);
        ShowPage(PageDetail, Page);
    }
}

VOID
CSettingsApp::OnCommand(
    _In_z_ PCWSTR Command)
{
    if (_wcsnicmp(Command, L"tab:", 4) == 0)
    {
        for (ULONG Index = 0; Index < ARRAYSIZE(g_Tabs); Index++)
        {
            if (_wcsicmp(Command + 4, g_Tabs[Index]) == 0)
                ShowTab(Index);
        }
    }
    else if (_wcsnicmp(Command, L"open:", 5) == 0)
    {
        Open(Command + 5);
    }
    else if (_wcsnicmp(Command, L"about:", 6) == 0)
    {
        ShowAbout(Command + 6);
    }
    else if (_wcsnicmp(Command, L"owner:", 6) == 0)
    {
        m_Field = min(max(_wtol(Command + 6), 0L), OWNER_FIELDS - 1L);
        RefreshOwner();
        ShowInputPanel(TRUE);
    }
    else if (_wcsnicmp(Command, L"volume.", 7) == 0)
    {
        if (_wcsicmp(Command + 7, L"up") == 0)
            DevChangeVolume(10, 2);
        else if (_wcsicmp(Command + 7, L"down") == 0)
            DevChangeVolume(-10, 2);
        else
            DevChangeVolume(0, -1);
        RefreshSounds();
        Request(L"status.refresh");
    }
    else if (_wcsnicmp(Command, L"theme:", 6) == 0)
    {
        /* The shell switches the theme and sends it back to every app, this one included */
        m_Theme = Command + 6;
        RefreshThemes();
        Request(Command);
    }
    else
    {
        CAppClient::OnCommand(Command);
    }
}

VOID
CSettingsApp::OnSoftKey(
    _In_ SHELL_SOFTKEY Key)
{
    if (m_Page == PageOwner && Key == ShellSoftKeyLeft)
        OnCloseButton();
}

VOID
CSettingsApp::OnCloseButton(VOID)
{
    if (m_Page == PageOwner)
    {
        SaveOwner();
        ShowInputPanel(FALSE);
        Request(L"status.refresh");
    }

    /* ok backs out of a page to its tab, or closes when the page was opened directly */
    if (m_Page == PageTabs || m_Direct)
        RequestClose();
    else
        ShowTab(m_Tab);
}

VOID
CSettingsApp::OnChar(
    _In_ WCHAR Char)
{
    if (m_Page != PageOwner || Char < 0x20)
        return;

    m_Owner[m_Field] += Char;
    RefreshOwner();
}

VOID
CSettingsApp::OnKey(
    _In_ UINT VirtualKey)
{
    if (m_Page == PageOwner)
    {
        if (VirtualKey == VK_BACK && !m_Owner[m_Field].empty())
            m_Owner[m_Field].pop_back();
        else if (VirtualKey == VK_RETURN || VirtualKey == VK_DOWN)
            m_Field = (m_Field + 1) % OWNER_FIELDS;
        else if (VirtualKey == VK_UP)
            m_Field = (m_Field + OWNER_FIELDS - 1) % OWNER_FIELDS;
        RefreshOwner();
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
    CSettingsApp App;
    WSADATA Winsock;
    PWSTR *Arguments;
    INT Count = 0;
    INT Result = 1;

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CommandLine);
    UNREFERENCED_PARAMETER(ShowCommand);

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    WSAStartup(MAKEWORD(2, 2), &Winsock);
    Arguments = CommandLineToArgvW(GetCommandLineW(), &Count);
    if (Arguments && SUCCEEDED(App.Initialize(Count, Arguments)))
        Result = App.Run();
    LocalFree(Arguments);
    WSACleanup();
    CoUninitialize();
    return Result;
}
