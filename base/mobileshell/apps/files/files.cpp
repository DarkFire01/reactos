/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     File Explorer: folder and sort drop downs, file operations, properties and handoff
 */

#include <appclient.h>
#include <records.h>
#include <shellapi.h>
#include <strsafe.h>
#include <algorithm>

/*
 * Markup contract (files.uix):
 *   Pages: PageList, PageDetails, PageRename; elements ListEmpty, Preview
 *   Repeaters: Entries (Id, Name, Detail, Glyph, Ink, Icon, Command), Properties (Label, Value)
 *   Bindings: FolderName, SortName, DetailName, PreviewSource, RenameText
 *   Commands: folder.menu, sort.menu, open:<Id>, hold:<Id>, more
 *
 * Changes are allowed inside My Documents only, unless launched with --writable all.
 */

#define FILES_PAGE_SIZE 300

typedef enum _FILES_PAGE
{
    PageList,
    PageDetails,
    PageRename
} FILES_PAGE;

typedef enum _FILES_MENU
{
    MenuNone,
    MenuList,
    MenuHold,
    MenuFolder,
    MenuSort,
    MenuDetails
} FILES_MENU;

typedef enum _FILES_SORT
{
    SortName,
    SortDate,
    SortSize,
    SortType
} FILES_SORT;

typedef struct _FILE_ENTRY
{
    std::wstring Name;
    std::wstring Path;
    std::wstring Label;
    BOOL Folder;
    BOOL Drive;
    ULONGLONG Size;
    FILETIME Modified;
    DWORD Attributes;
} FILE_ENTRY;

static PCWSTR const g_SortNames[] = { L"Name", L"Date", L"Size", L"Type" };

static
std::wstring
Extension(
    _In_ const std::wstring &Name)
{
    SIZE_T Dot = Name.find_last_of(L'.');
    std::wstring Result;

    if (Dot == std::wstring::npos)
        return Result;
    for (WCHAR Char : Name.substr(Dot + 1))
        Result += towlower(Char);
    return Result;
}

static
BOOL
IsImage(
    _In_ const std::wstring &Name)
{
    std::wstring Type = Extension(Name);

    return Type == L"png" || Type == L"jpg" || Type == L"jpeg" || Type == L"bmp" || Type == L"gif" || Type == L"ico";
}

/* Which app opens a file, by extension; empty when only Properties applies */
static
PCWSTR
Handler(
    _In_ const std::wstring &Name)
{
    std::wstring Type = Extension(Name);

    if (Type == L"txt" || Type == L"rtf" || Type == L"log" || Type == L"ini" || Type == L"md")
        return L"Writer";
    if (Type == L"csv")
        return L"Sheets";
    if (Type == L"slides")
        return L"Slides";
    return NULL;
}

static
std::wstring
ShortSize(
    _In_ ULONGLONG Bytes)
{
    WCHAR Text[32];

    /* The device's compact style: 12K, 1.4M */
    if (Bytes < 1024)
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lluB", Bytes);
    else if (Bytes < 1024 * 1024)
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lluK", (Bytes + 1023) / 1024);
    else if (Bytes < 1024ULL * 1024 * 1024)
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%.1fM", Bytes / (1024.0 * 1024.0));
    else
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%.1fG", Bytes / (1024.0 * 1024.0 * 1024.0));
    return Text;
}

static
std::wstring
LocalDate(
    _In_ const FILETIME &Time,
    _In_ BOOL WithTime)
{
    SYSTEMTIME Utc;
    SYSTEMTIME Local;
    WCHAR Date[64];
    WCHAR Clock[32];

    if (!FileTimeToSystemTime(&Time, &Utc) || !SystemTimeToTzSpecificLocalTime(NULL, &Utc, &Local))
        return std::wstring();
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &Local, NULL, Date, ARRAYSIZE(Date), NULL);
    if (!WithTime)
        return Date;
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &Local, NULL, Clock, ARRAYSIZE(Clock));
    return std::wstring(Date) + L" " + Clock;
}

class CFilesApp : public CAppClient
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

    VOID OnMenuPick(
        _In_ LONG Index) override;

    VOID OnChar(
        _In_ WCHAR Char) override;

    VOID OnKey(
        _In_ UINT VirtualKey) override;

private:
    BOOL
    Writable(
        _In_ const std::wstring &Path) const;

    VOID
    Navigate(
        _In_ const std::wstring &Path);

    VOID
    Load(VOID);

    VOID
    ShowList(VOID);

    VOID
    ShowPage(
        _In_ FILES_PAGE Page);

    VOID
    Open(
        _In_ LONG Index);

    VOID
    ShowDetails(
        _In_ LONG Index);

    LONG
    SelectedIndex(VOID);

    BOOL
    Operate(
        _In_ UINT Operation,
        _In_ const std::wstring &From,
        _In_opt_ PCWSTR To);

    VOID
    NewFolder(VOID);

    VOID
    BeginRename(
        _In_ LONG Index);

    VOID
    Delete(
        _In_ LONG Index);

    VOID
    Clip(
        _In_ LONG Index,
        _In_ BOOL Cut);

    VOID
    Paste(VOID);

    VOID
    RefreshRename(VOID);

    BOOL m_RenameFresh = FALSE;
    std::wstring m_Home;
    std::wstring m_Path;
    std::vector<FILE_ENTRY> m_Entries;
    std::vector<std::wstring> m_Crumbs;
    FILES_PAGE m_Page = PageList;
    FILES_MENU m_Menu = MenuNone;
    FILES_SORT m_Sort = SortName;
    BOOL m_ShowAll = FALSE;
    BOOL m_WriteAll = FALSE;
    BOOL m_Started = FALSE;
    ULONG m_Limit = FILES_PAGE_SIZE;
    LONG m_Target = -1;
    std::wstring m_Clip;
    BOOL m_ClipCut = FALSE;
    std::wstring m_Rename;
};

BOOL
CFilesApp::Writable(
    _In_ const std::wstring &Path) const
{
    if (Path.empty())
        return FALSE;
    if (m_WriteAll)
        return TRUE;
    return _wcsnicmp(Path.c_str(), m_Home.c_str(), m_Home.size()) == 0 &&
           (Path.size() == m_Home.size() || Path[m_Home.size()] == L'\\');
}

VOID
CFilesApp::OnActivate(VOID)
{
    PCWSTR Writable;

    if (!m_Started)
    {
        m_Started = TRUE;
        m_Home = AppDataFile(L"My Documents");
        CreateDirectoryW(m_Home.c_str(), NULL);
        Writable = Argument(L"writable");
        m_WriteAll = Writable && _wcsicmp(Writable, L"all") == 0;

        /* Like the device, File Explorer starts in My Documents */
        Navigate(Argument(L"folder") ? Argument(L"folder") : m_Home.c_str());
        return;
    }
    ShowPage(m_Page);
}

VOID
CFilesApp::Navigate(
    _In_ const std::wstring &Path)
{
    m_Path = Path;
    while (m_Path.size() > 3 && m_Path.back() == L'\\')
        m_Path.pop_back();
    m_Limit = FILES_PAGE_SIZE;
    Load();
    Scene()->ResetScroll(L"ListScroll");
    ShowPage(PageList);
}

VOID
CFilesApp::Load(VOID)
{
    WIN32_FIND_DATAW Data;
    HANDLE Search;
    WCHAR Drives[512];
    WCHAR Label[MAX_PATH];
    UINT OldMode;

    m_Entries.clear();

    /* "My Device": My Documents, then every fixed, removable and optical volume */
    if (m_Path.empty())
    {
        FILE_ENTRY Home = {};

        Home.Name = L"My Documents";
        Home.Path = m_Home;
        Home.Folder = TRUE;
        m_Entries.push_back(Home);

        OldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
        if (GetLogicalDriveStringsW(ARRAYSIZE(Drives), Drives))
        {
            for (PCWSTR Root = Drives; *Root; Root += wcslen(Root) + 1)
            {
                UINT Type = GetDriveTypeW(Root);
                FILE_ENTRY Drive = {};

                if (Type != DRIVE_FIXED && Type != DRIVE_REMOVABLE && Type != DRIVE_CDROM)
                    continue;

                Label[0] = UNICODE_NULL;
                GetVolumeInformationW(Root, Label, ARRAYSIZE(Label), NULL, NULL, NULL, NULL, 0);
                Drive.Name = std::wstring((Type == DRIVE_REMOVABLE) ? L"Storage Card" : (Type == DRIVE_CDROM) ? L"Disc" : L"Storage") +
                             L" (" + std::wstring(Root, 2) + L")";
                Drive.Label = Label;
                Drive.Path = Root;
                Drive.Folder = TRUE;
                Drive.Drive = TRUE;
                m_Entries.push_back(Drive);
            }
        }
        SetErrorMode(OldMode);
        return;
    }

    Search = FindFirstFileExW((m_Path + L"\\*").c_str(), FindExInfoStandard, &Data, FindExSearchNameMatch, NULL, 0);
    if (Search == INVALID_HANDLE_VALUE)
        return;

    do
    {
        FILE_ENTRY Entry = {};

        if (wcscmp(Data.cFileName, L".") == 0 || wcscmp(Data.cFileName, L"..") == 0)
            continue;
        if (!m_ShowAll && (Data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
            continue;

        Entry.Name = Data.cFileName;
        Entry.Path = m_Path + ((m_Path.back() == L'\\') ? L"" : L"\\") + Data.cFileName;
        Entry.Folder = (Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        Entry.Size = (static_cast<ULONGLONG>(Data.nFileSizeHigh) << 32) | Data.nFileSizeLow;
        Entry.Modified = Data.ftLastWriteTime;
        Entry.Attributes = Data.dwFileAttributes;
        m_Entries.push_back(Entry);
    } while (FindNextFileW(Search, &Data));
    FindClose(Search);

    /* Folders first, then the chosen order */
    std::sort(m_Entries.begin(), m_Entries.end(), [this](const FILE_ENTRY &A, const FILE_ENTRY &B)
    {
        INT Order;

        if (A.Folder != B.Folder)
            return A.Folder > B.Folder;
        switch (m_Sort)
        {
            case SortDate:
                return CompareFileTime(&A.Modified, &B.Modified) > 0;
            case SortSize:
                if (A.Size != B.Size)
                    return A.Size > B.Size;
                break;
            case SortType:
                Order = _wcsicmp(Extension(A.Name).c_str(), Extension(B.Name).c_str());
                if (Order != 0)
                    return Order < 0;
                break;
            default:
                break;
        }
        return _wcsicmp(A.Name.c_str(), B.Name.c_str()) < 0;
    });
}

VOID
CFilesApp::ShowList(VOID)
{
    static PCWSTR const Fields[] = { L"Id", L"Name", L"Detail", L"Glyph", L"Ink", L"Icon", L"Command" };
    std::vector<std::wstring> Storage;
    std::vector<PCWSTR> Values;
    std::wstring Folder;
    ULONG Count = 0;

    for (LONG Index = 0; Index < static_cast<LONG>(m_Entries.size()) && Count < m_Limit; Index++, Count++)
    {
        const FILE_ENTRY &Entry = m_Entries[Index];
        std::wstring Type = Extension(Entry.Name);
        std::wstring Icon;
        std::wstring Detail;

        if (Entry.Drive)
            Detail = Entry.Label;
        else if (!Entry.Folder)
            Detail = LocalDate(Entry.Modified, FALSE) + L"  " + ShortSize(Entry.Size);
        else if (!m_Path.empty())
            Detail = LocalDate(Entry.Modified, FALSE);

        if (Entry.Folder && !Entry.Drive)
        {
            Icon = L"@FolderIcon";
        }
        else if (!Entry.Folder)
        {
            if (Type == L"txt" || Type == L"rtf" || Type == L"doc" || Type == L"docx")
                Icon = L"@Icon.Word";
            else if (Type == L"csv" || Type == L"xls" || Type == L"xlsx")
                Icon = L"@Icon.Excel";
            else if (Type == L"slides" || Type == L"ppt" || Type == L"pptx")
                Icon = L"@Icon.PowerPoint";
            else if (IsImage(Entry.Name))
                Icon = L"@Icon.Photos";
            else if (Type == L"mp3" || Type == L"wav" || Type == L"wma" || Type == L"mp4" || Type == L"wmv")
                Icon = L"@Icon.WMP";
        }

        /* Folders and drives are font glyphs; typed files use the app icons; the rest a page glyph */
        Storage.insert(Storage.end(), {
            std::to_wstring(Index),
            Entry.Name,
            Detail,
            Entry.Drive ? L"\xEDA2" : ((Entry.Folder || !Icon.empty()) ? L"" : L"\xE8A5"),
            Entry.Folder ? (Entry.Drive ? L"@DriveColor" : L"@FolderColor") : L"@AppSubText",
            Icon,
            L"open:" + std::to_wstring(Index) });
    }

    if (m_Entries.size() > m_Limit)
    {
        SIZE_T Rest = m_Entries.size() - m_Limit;

        Storage.insert(Storage.end(), { L"more", L"Show " + std::to_wstring((Rest < FILES_PAGE_SIZE) ? Rest : FILES_PAGE_SIZE) +
                                                 L" more of " + std::to_wstring(m_Entries.size() - m_Limit) + L"...",
                                        L"", L"\xE712", L"@AppSubText", L"", L"more" });
        Count++;
    }

    for (const std::wstring &Value : Storage)
        Values.push_back(Value.c_str());
    Scene()->SetItems(L"Entries", Count, ARRAYSIZE(Fields), Fields, Values.data());
    Scene()->SetVisible(L"ListEmpty", m_Entries.empty());

    if (m_Path.empty())
        Folder = L"My Device";
    else if (_wcsicmp(m_Path.c_str(), m_Home.c_str()) == 0)
        Folder = L"My Documents";
    else if (m_Path.size() <= 3)
        Folder = m_Path.substr(0, 2);
    else
        Folder = m_Path.substr(m_Path.find_last_of(L'\\') + 1);
    Scene()->SetBinding(L"FolderName", Folder.c_str());
    Scene()->SetBinding(L"SortName", g_SortNames[m_Sort]);
}

VOID
CFilesApp::ShowPage(
    _In_ FILES_PAGE Page)
{
    m_Page = Page;
    Scene()->SetVisible(L"PageList", Page == PageList);
    Scene()->SetVisible(L"PageDetails", Page == PageDetails);
    Scene()->SetVisible(L"PageRename", Page == PageRename);

    switch (Page)
    {
        case PageList:
            ShowList();
            SetChrome(L"File Explorer", m_Path.empty() ? L"" : L"Up", L"Menu", ShellCloseOk);
            break;
        case PageDetails:
            SetChrome(L"File Explorer", (m_Target >= 0 && Handler(m_Entries[m_Target].Name)) ? L"Open" : L"", L"Menu", ShellCloseOk);
            break;
        case PageRename:
            SetChrome(L"Rename", L"Done", L"Cancel", ShellCloseOk);
            break;
    }
}

LONG
CFilesApp::SelectedIndex(VOID)
{
    PCWSTR Command = Scene()->GetSelectedCommand();

    if (m_Page == PageDetails)
        return m_Target;
    if (Command && _wcsnicmp(Command, L"open:", 5) == 0)
        return _wtol(Command + 5);
    return -1;
}

VOID
CFilesApp::Open(
    _In_ LONG Index)
{
    PCWSTR App;

    if (Index < 0 || Index >= static_cast<LONG>(m_Entries.size()))
        return;

    const FILE_ENTRY &Entry = m_Entries[Index];
    if (Entry.Folder)
    {
        Navigate(Entry.Path);
        return;
    }

    /* Known documents go to their program through the shell, everything else to Properties */
    App = Handler(Entry.Name);
    if (App)
        Launch(App, Entry.Path.c_str());
    else
        ShowDetails(Index);
}

VOID
CFilesApp::ShowDetails(
    _In_ LONG Index)
{
    static PCWSTR const Fields[] = { L"Label", L"Value" };
    std::vector<std::pair<std::wstring, std::wstring>> Rows;
    std::vector<PCWSTR> Values;
    WIN32_FILE_ATTRIBUTE_DATA Data;
    SHFILEINFOW Info = {};
    std::wstring Attributes;

    if (Index < 0 || Index >= static_cast<LONG>(m_Entries.size()))
        return;

    m_Target = Index;
    const FILE_ENTRY &Entry = m_Entries[Index];

    if (SHGetFileInfoW(Entry.Path.c_str(), 0, &Info, sizeof(Info), SHGFI_TYPENAME) && Info.szTypeName[0])
        Rows.emplace_back(L"Type", Info.szTypeName);
    Rows.emplace_back(L"Location", Entry.Path.substr(0, Entry.Path.find_last_of(L'\\') + 1));

    if (GetFileAttributesExW(Entry.Path.c_str(), GetFileExInfoStandard, &Data))
    {
        ULONGLONG Size = (static_cast<ULONGLONG>(Data.nFileSizeHigh) << 32) | Data.nFileSizeLow;

        if (!Entry.Folder)
            Rows.emplace_back(L"Size", ShortSize(Size) + L" (" + std::to_wstring(Size) + L" bytes)");
        Rows.emplace_back(L"Created", LocalDate(Data.ftCreationTime, TRUE));
        Rows.emplace_back(L"Modified", LocalDate(Data.ftLastWriteTime, TRUE));

        if (Data.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
            Attributes += L"Read only  ";
        if (Data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)
            Attributes += L"Hidden  ";
        if (Data.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)
            Attributes += L"System  ";
        if (Data.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE)
            Attributes += L"Archive";
        if (!Attributes.empty())
            Rows.emplace_back(L"Attributes", Attributes);
    }
    Rows.emplace_back(L"Changes", Writable(Entry.Path) ? L"Allowed" : L"Read only outside My Documents");

    for (const auto &Row : Rows)
    {
        Values.push_back(Row.first.c_str());
        Values.push_back(Row.second.c_str());
    }
    Scene()->SetItems(L"Properties", static_cast<ULONG>(Rows.size()), ARRAYSIZE(Fields), Fields, Values.data());
    Scene()->SetBinding(L"DetailName", Entry.Name.c_str());

    /* Pictures get a preview, decoded by the same engine that draws the UI */
    Scene()->SetBinding(L"PreviewSource", IsImage(Entry.Name) ? (L"file:" + Entry.Path).c_str() : L"");
    Scene()->SetVisible(L"Preview", IsImage(Entry.Name));
    Scene()->ResetScroll(L"DetailScroll");
    ShowPage(PageDetails);
}

BOOL
CFilesApp::Operate(
    _In_ UINT Operation,
    _In_ const std::wstring &From,
    _In_opt_ PCWSTR To)
{
    SHFILEOPSTRUCTW Op = {};
    std::wstring Source = From + std::wstring(1, L'\0');
    std::wstring Target = To ? std::wstring(To) + std::wstring(1, L'\0') : std::wstring();

    /* Strings are double terminated; no dialogs, deletes go to the recycle bin */
    Op.wFunc = Operation;
    Op.pFrom = Source.c_str();
    Op.pTo = To ? Target.c_str() : NULL;
    Op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR;
    if (Operation == FO_DELETE)
        Op.fFlags |= FOF_ALLOWUNDO;
    if (Operation == FO_COPY || Operation == FO_MOVE)
        Op.fFlags |= FOF_RENAMEONCOLLISION;
    return SHFileOperationW(&Op) == 0 && !Op.fAnyOperationsAborted;
}

VOID
CFilesApp::NewFolder(VOID)
{
    std::wstring Path;

    if (!Writable(m_Path))
    {
        Notify(L"File Explorer", L"New folders can only be made inside My Documents.");
        return;
    }

    for (ULONG Number = 1; Number < 1000; Number++)
    {
        Path = m_Path + L"\\New Folder" + ((Number > 1) ? L" (" + std::to_wstring(Number) + L")" : L"");
        if (GetFileAttributesW(Path.c_str()) == INVALID_FILE_ATTRIBUTES)
            break;
    }

    if (!CreateDirectoryW(Path.c_str(), NULL))
        return;

    /* Like the device, a new folder goes straight into rename */
    Load();
    for (LONG Index = 0; Index < static_cast<LONG>(m_Entries.size()); Index++)
    {
        if (_wcsicmp(m_Entries[Index].Path.c_str(), Path.c_str()) == 0)
        {
            BeginRename(Index);
            return;
        }
    }
    ShowList();
}

VOID
CFilesApp::BeginRename(
    _In_ LONG Index)
{
    if (Index < 0 || Index >= static_cast<LONG>(m_Entries.size()) || m_Entries[Index].Drive)
        return;
    if (!Writable(m_Entries[Index].Path))
    {
        Notify(L"File Explorer", L"Only items in My Documents can be renamed.");
        return;
    }

    /* The old name starts selected, so the first key replaces it */
    m_Target = Index;
    m_Rename = m_Entries[Index].Name;
    m_RenameFresh = TRUE;
    RefreshRename();
    ShowPage(PageRename);
    ShowInputPanel(TRUE);
}

VOID
CFilesApp::Delete(
    _In_ LONG Index)
{
    if (Index < 0 || Index >= static_cast<LONG>(m_Entries.size()) || m_Entries[Index].Drive)
        return;
    if (!Writable(m_Entries[Index].Path) || _wcsicmp(m_Entries[Index].Path.c_str(), m_Home.c_str()) == 0)
    {
        Notify(L"File Explorer", L"Only items inside My Documents can be deleted.");
        return;
    }

    Operate(FO_DELETE, m_Entries[Index].Path, NULL);
    Load();
    ShowPage(PageList);
}

VOID
CFilesApp::Clip(
    _In_ LONG Index,
    _In_ BOOL Cut)
{
    if (Index < 0 || Index >= static_cast<LONG>(m_Entries.size()) || m_Entries[Index].Drive)
        return;
    if (Cut && !Writable(m_Entries[Index].Path))
    {
        Notify(L"File Explorer", L"This item is read only here. Use Copy instead.");
        return;
    }

    m_Clip = m_Entries[Index].Path;
    m_ClipCut = Cut;
}

VOID
CFilesApp::Paste(VOID)
{
    if (m_Clip.empty())
        return;
    if (!Writable(m_Path))
    {
        Notify(L"File Explorer", L"Paste into a folder inside My Documents.");
        return;
    }

    if (Operate(m_ClipCut ? FO_MOVE : FO_COPY, m_Clip, m_Path.c_str()) && m_ClipCut)
        m_Clip.clear();
    Load();
    ShowList();
}

VOID
CFilesApp::OnCommand(
    _In_z_ PCWSTR Command)
{
    std::wstring Items;

    if (_wcsnicmp(Command, L"open:", 5) == 0)
    {
        Open(_wtol(Command + 5));
    }
    else if (_wcsicmp(Command, L"more") == 0)
    {
        m_Limit += FILES_PAGE_SIZE;
        ShowList();
    }
    else if (_wcsicmp(Command, L"folder.menu") == 0)
    {
        /* The folder drop down lists the way back up to My Device */
        std::wstring Walk = m_Path;

        m_Crumbs.clear();
        while (!Walk.empty())
        {
            m_Crumbs.push_back(Walk);
            if (_wcsicmp(Walk.c_str(), m_Home.c_str()) == 0 || Walk.size() <= 3)
                break;
            Walk = Walk.substr(0, Walk.find_last_of(L'\\'));
            if (Walk.size() == 2)
                Walk += L'\\';
        }
        m_Crumbs.push_back(L"");

        for (const std::wstring &Crumb : m_Crumbs)
        {
            if (Crumb.empty())
                Items += L"My Device";
            else if (_wcsicmp(Crumb.c_str(), m_Home.c_str()) == 0)
                Items += L"My Documents";
            else if (Crumb.size() <= 3)
                Items += Crumb.substr(0, 2);
            else
                Items += Crumb.substr(Crumb.find_last_of(L'\\') + 1);
            Items += L'\n';
        }
        m_Menu = MenuFolder;
        ShowMenu(AppMenuPoint, 2.0f, 22.0f, Items.c_str());
    }
    else if (_wcsicmp(Command, L"sort.menu") == 0)
    {
        m_Menu = MenuSort;
        ShowMenu(AppMenuPoint, 150.0f, 22.0f, L"Name\nDate\nSize\nType");
    }
    else
    {
        CAppClient::OnCommand(Command);
    }
}

VOID
CFilesApp::OnHold(
    _In_z_ PCWSTR Command,
    _In_ FLOAT X,
    _In_ FLOAT Y)
{
    if (_wcsnicmp(Command, L"hold:", 5) != 0 || !iswdigit(Command[5]))
        return;

    m_Target = _wtol(Command + 5);
    m_Menu = MenuHold;
    ShowMenu(AppMenuPoint, X, Y, L"Cut\nCopy\nRename\nDelete\n-\nProperties");
}

VOID
CFilesApp::OnSoftKey(
    _In_ SHELL_SOFTKEY Key)
{
    switch (m_Page)
    {
        case PageList:
            if (Key == ShellSoftKeyLeft && !m_Path.empty())
            {
                if (_wcsicmp(m_Path.c_str(), m_Home.c_str()) == 0 || m_Path.size() <= 3)
                    Navigate(L"");
                else
                    Navigate(m_Path.substr(0, m_Path.find_last_of(L'\\')) + ((m_Path.find_last_of(L'\\') == 2) ? L"\\" : L""));
            }
            else if (Key == ShellSoftKeyRight)
            {
                m_Menu = MenuList;
                ShowMenu(AppMenuRightSoftKey, 0.0f, 0.0f,
                         m_ShowAll ? L"New Folder\nRename\nDelete\n-\nCopy\nCut\nPaste\n-\nHide System Files\nProperties"
                                   : L"New Folder\nRename\nDelete\n-\nCopy\nCut\nPaste\n-\nShow All Files\nProperties");
            }
            break;

        case PageDetails:
            if (Key == ShellSoftKeyLeft && m_Target >= 0 && Handler(m_Entries[m_Target].Name))
            {
                Launch(Handler(m_Entries[m_Target].Name), m_Entries[m_Target].Path.c_str());
            }
            else if (Key == ShellSoftKeyRight)
            {
                m_Menu = MenuDetails;
                ShowMenu(AppMenuRightSoftKey, 0.0f, 0.0f, L"Rename\nDelete\nCopy");
            }
            break;

        case PageRename:
            ShowInputPanel(FALSE);
            if (Key == ShellSoftKeyLeft)
                OnCloseButton();
            else
                ShowPage(PageList);
            break;
    }
}

VOID
CFilesApp::OnCloseButton(VOID)
{
    std::wstring Target;

    switch (m_Page)
    {
        case PageList:
            RequestClose();
            break;

        case PageDetails:
            ShowPage(PageList);
            break;

        case PageRename:
            /* Names cannot hold path characters; an unchanged name is not an error */
            ShowInputPanel(FALSE);
            if (!m_Rename.empty() && m_Rename.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos &&
                m_Target >= 0 && m_Rename != m_Entries[m_Target].Name)
            {
                Target = m_Path + L"\\" + m_Rename;
                if (!MoveFileW(m_Entries[m_Target].Path.c_str(), Target.c_str()))
                    Notify(L"File Explorer", L"That name is already used or not allowed.");
            }
            Load();
            ShowPage(PageList);
            break;
    }
}

VOID
CFilesApp::OnMenuPick(
    _In_ LONG Index)
{
    FILES_MENU Menu = m_Menu;
    LONG Selected = SelectedIndex();

    m_Menu = MenuNone;
    if (Index < 0)
        return;

    switch (Menu)
    {
        case MenuFolder:
            if (Index < static_cast<LONG>(m_Crumbs.size()))
                Navigate(m_Crumbs[Index]);
            break;

        case MenuSort:
            m_Sort = static_cast<FILES_SORT>(Index);
            Load();
            ShowList();
            break;

        case MenuHold:
            if (Index == 0)
                Clip(m_Target, TRUE);
            else if (Index == 1)
                Clip(m_Target, FALSE);
            else if (Index == 2)
                BeginRename(m_Target);
            else if (Index == 3)
                Delete(m_Target);
            else
                ShowDetails(m_Target);
            break;

        case MenuDetails:
            if (Index == 0)
                BeginRename(m_Target);
            else if (Index == 1)
                Delete(m_Target);
            else
                Clip(m_Target, FALSE);
            break;

        case MenuList:
            switch (Index)
            {
                case 0:
                    NewFolder();
                    break;
                case 1:
                    BeginRename(Selected);
                    break;
                case 2:
                    Delete(Selected);
                    break;
                case 3:
                    Clip(Selected, FALSE);
                    break;
                case 4:
                    Clip(Selected, TRUE);
                    break;
                case 5:
                    Paste();
                    break;
                case 6:
                    m_ShowAll = !m_ShowAll;
                    Load();
                    ShowList();
                    break;
                default:
                    ShowDetails(Selected);
                    break;
            }
            break;

        default:
            break;
    }
}

VOID
CFilesApp::OnChar(
    _In_ WCHAR Char)
{
    if (m_Page != PageRename || Char < 0x20)
        return;

    if (m_RenameFresh)
        m_Rename.clear();
    m_RenameFresh = FALSE;
    m_Rename += Char;
    RefreshRename();
}

VOID
CFilesApp::RefreshRename(VOID)
{
    Scene()->SetBinding(L"RenameText", (m_Rename + (m_RenameFresh ? L"" : L"|")).c_str());
    Scene()->SetBinding(L"RenameSelected", m_RenameFresh ? m_Rename.c_str() : L"");
}

VOID
CFilesApp::OnKey(
    _In_ UINT VirtualKey)
{
    if (m_Page == PageRename)
    {
        if (VirtualKey == VK_BACK && m_RenameFresh)
            m_Rename.clear();
        else if (VirtualKey == VK_BACK && !m_Rename.empty())
            m_Rename.pop_back();
        else if (VirtualKey == VK_RETURN)
        {
            OnCloseButton();
            Present();
            return;
        }
        m_RenameFresh = FALSE;
        RefreshRename();
        Present();
        return;
    }

    if (m_Page == PageList && VirtualKey == VK_BACK)
    {
        OnSoftKey(ShellSoftKeyLeft);
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
    CFilesApp App;
    PWSTR *Arguments;
    INT Count = 0;
    INT Result = 1;

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CommandLine);
    UNREFERENCED_PARAMETER(ShowCommand);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    Arguments = CommandLineToArgvW(GetCommandLineW(), &Count);
    if (Arguments && SUCCEEDED(App.Initialize(Count, Arguments)))
        Result = App.Run();
    LocalFree(Arguments);
    CoUninitialize();
    return Result;
}
