/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Office Mobile: Writer, Sheets, Slides and Notes over one document list with find
 */

#include <appclient.h>
#include <records.h>
#include <shellapi.h>
#include <strsafe.h>
#include <math.h>
#include <algorithm>

/*
 * The markup root picks the program: Mode="Writer", "Sheets", "Slides" or "Notes".
 * Markup contract:
 *   Pages: PageList, PageEdit (Writer, Notes), PageSheet, PageShow; elements FindHint, ListEmpty, FindBar
 *   Repeaters: Docs (Id, Name, Detail), Cells (Text, W, Fill, Align, Command), Bullets (Text)
 *   Bindings: FindText, ListHeader, SortLabel, DocIcon, DocText, DocFind, DocFindText, DocFindCount,
 *             CellRef, CellEdit, SlideTitle, SlideBody, SlideNumber
 *   Commands: find.focus, find.clear, open:<Id>, hold:<Id>, cell:<Row>,<Column>, slide.next
 */

#define SHEET_ROWS 40
#define SHEET_COLUMNS 5
#define TEXT_LIMIT (1024 * 1024)

typedef enum _OFFICE_MODE
{
    ModeWriter,
    ModeSheets,
    ModeSlides,
    ModeNotes
} OFFICE_MODE;

typedef enum _OFFICE_PAGE
{
    PageList,
    PageEdit,
    PageSheet,
    PageShow
} OFFICE_PAGE;

typedef enum _OFFICE_MENU
{
    MenuNone,
    MenuList,
    MenuHold,
    MenuEdit,
    MenuSheet,
    MenuShow
} OFFICE_MENU;

typedef struct _OFFICE_DOC
{
    std::wstring Path;
    std::wstring Name;
    std::wstring Folder;
    FILETIME Modified;
    ULONGLONG Size;
    BOOL ReadOnly;
} OFFICE_DOC;

typedef struct _OFFICE_SLIDE
{
    std::wstring Title;
    std::wstring Body;
    std::vector<std::wstring> Bullets;
} OFFICE_SLIDE;

/* Text files */

static
std::wstring
ReadTextFile(
    _In_ const std::wstring &Path)
{
    std::string Bytes;
    std::wstring Text;
    HANDLE File;
    DWORD Size;
    DWORD Read = 0;
    INT Length;

    File = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return Text;

    Size = min(GetFileSize(File, NULL), static_cast<DWORD>(TEXT_LIMIT));
    if (Size != INVALID_FILE_SIZE && Size)
    {
        Bytes.resize(Size);
        ReadFile(File, &Bytes[0], Size, &Read, NULL);
        Bytes.resize(Read);
    }
    CloseHandle(File);

    /* UTF-16 and UTF-8 by their marks, then strict UTF-8, then the ANSI code page */
    if (Bytes.size() >= 2 && static_cast<BYTE>(Bytes[0]) == 0xFF && static_cast<BYTE>(Bytes[1]) == 0xFE)
    {
        Text.assign(reinterpret_cast<const WCHAR *>(Bytes.data() + 2), (Bytes.size() - 2) / sizeof(WCHAR));
    }
    else
    {
        SIZE_T Skip = (Bytes.size() >= 3 && Bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) ? 3 : 0;
        UINT CodePage = CP_UTF8;

        Length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Bytes.data() + Skip, static_cast<INT>(Bytes.size() - Skip), NULL, 0);
        if (Length <= 0 && Bytes.size() > Skip)
        {
            CodePage = CP_ACP;
            Length = MultiByteToWideChar(CP_ACP, 0, Bytes.data() + Skip, static_cast<INT>(Bytes.size() - Skip), NULL, 0);
        }
        if (Length > 0)
        {
            Text.resize(Length);
            MultiByteToWideChar(CodePage, 0, Bytes.data() + Skip, static_cast<INT>(Bytes.size() - Skip), &Text[0], Length);
        }
    }

    Text.erase(std::remove(Text.begin(), Text.end(), L'\r'), Text.end());
    return Text;
}

static
BOOL
WriteTextFile(
    _In_ const std::wstring &Path,
    _In_ const std::wstring &Text)
{
    std::wstring Lines;
    std::string Bytes = "\xEF\xBB\xBF";
    HANDLE File;
    DWORD Written = 0;
    BOOL Result;
    INT Length;

    for (WCHAR Char : Text)
    {
        if (Char == L'\n')
            Lines += L'\r';
        Lines += Char;
    }

    Length = WideCharToMultiByte(CP_UTF8, 0, Lines.c_str(), static_cast<INT>(Lines.size()), NULL, 0, NULL, NULL);
    if (Length > 0)
    {
        SIZE_T Start = Bytes.size();

        Bytes.resize(Start + Length);
        WideCharToMultiByte(CP_UTF8, 0, Lines.c_str(), static_cast<INT>(Lines.size()), &Bytes[Start], Length, NULL, NULL);
    }

    File = CreateFileW(Path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    Result = WriteFile(File, Bytes.data(), static_cast<DWORD>(Bytes.size()), &Written, NULL);
    CloseHandle(File);
    return Result;
}

/* Plain text out of RTF: control words dropped, \par and \line kept as breaks, \'hh decoded */
static
std::wstring
StripRtf(
    _In_ const std::wstring &Rtf)
{
    static PCWSTR const Skipped[] = { L"fonttbl", L"colortbl", L"stylesheet", L"info", L"pict", L"header", L"footer" };
    std::vector<BOOL> Hidden(1, FALSE);
    std::wstring Text;
    SIZE_T Index = 0;

    while (Index < Rtf.size())
    {
        WCHAR Char = Rtf[Index];

        if (Char == L'{')
        {
            Hidden.push_back(Hidden.back());
            Index++;
            continue;
        }
        if (Char == L'}')
        {
            if (Hidden.size() > 1)
                Hidden.pop_back();
            Index++;
            continue;
        }
        if (Char == L'\\' && Index + 1 < Rtf.size())
        {
            WCHAR Next = Rtf[Index + 1];
            std::wstring Word;

            if (Next == L'\'' && Index + 3 < Rtf.size())
            {
                if (!Hidden.back())
                    Text += static_cast<WCHAR>(wcstoul(Rtf.substr(Index + 2, 2).c_str(), NULL, 16));
                Index += 4;
                continue;
            }
            if (Next == L'*')
            {
                Hidden.back() = TRUE;
                Index += 2;
                continue;
            }
            if (!iswalpha(Next))
            {
                if (!Hidden.back() && (Next == L'\\' || Next == L'{' || Next == L'}'))
                    Text += Next;
                Index += 2;
                continue;
            }

            Index++;
            while (Index < Rtf.size() && iswalpha(Rtf[Index]))
                Word += Rtf[Index++];
            while (Index < Rtf.size() && (iswdigit(Rtf[Index]) || Rtf[Index] == L'-'))
                Index++;
            if (Index < Rtf.size() && Rtf[Index] == L' ')
                Index++;

            for (PCWSTR Name : Skipped)
            {
                if (Word == Name)
                    Hidden.back() = TRUE;
            }
            if (!Hidden.back() && (Word == L"par" || Word == L"line"))
                Text += L'\n';
            else if (!Hidden.back() && Word == L"tab")
                Text += L'\t';
            continue;
        }

        if (!Hidden.back() && Char != L'\n')
            Text += Char;
        Index++;
    }
    return Text;
}

static
BOOL
EndsWith(
    _In_ const std::wstring &Text,
    _In_z_ PCWSTR Suffix)
{
    SIZE_T Length = wcslen(Suffix);

    return Text.size() >= Length && _wcsicmp(Text.c_str() + Text.size() - Length, Suffix) == 0;
}

static
SIZE_T
FindNoCase(
    _In_ const std::wstring &Text,
    _In_ const std::wstring &Part)
{
    auto Found = std::search(Text.begin(), Text.end(), Part.begin(), Part.end(),
                             [](WCHAR A, WCHAR B) { return towlower(A) == towlower(B); });
    return (Found == Text.end()) ? std::wstring::npos : static_cast<SIZE_T>(Found - Text.begin());
}

class COfficeApp : public CAppClient
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

    VOID OnOpenDocument(
        _In_z_ PCWSTR Path) override;

private:
    /* Documents */
    std::wstring
    Folder(VOID) const;

    BOOL
    Accepts(
        _In_ const std::wstring &Name) const;

    VOID
    Seed(VOID);

    VOID
    Scan(VOID);

    VOID
    ScanFolder(
        _In_ const std::wstring &Directory,
        _In_ const std::wstring &Label,
        _In_ BOOL ReadOnly,
        _In_ ULONG Depth);

    std::wstring
    Contents(
        _In_ const OFFICE_DOC &Doc);

    VOID
    ShowList(VOID);

    VOID
    ShowPage(
        _In_ OFFICE_PAGE Page);

    VOID
    Open(
        _In_ LONG Index);

    VOID
    OpenCurrent(VOID);

    VOID
    NewDocument(VOID);

    VOID
    Delete(
        _In_ LONG Index);

    VOID
    SaveCopy(VOID);

    BOOL
    Save(VOID);

    /* Writer and Notes */
    VOID
    RefreshText(VOID);

    VOID
    Edit(
        _In_ const std::wstring &Insert,
        _In_ SIZE_T Remove);

    VOID
    MoveLine(
        _In_ INT Direction);

    /* Sheets */
    VOID
    LoadSheet(
        _In_ const std::wstring &Text);

    std::wstring
    SheetCsv(VOID) const;

    std::wstring
    Display(
        _In_ LONG Row,
        _In_ LONG Column);

    DOUBLE
    Evaluate(
        _In_ LONG Row,
        _In_ LONG Column,
        _Inout_ BOOL *Error,
        _In_ ULONG Depth);

    DOUBLE
    ParseExpression(
        _Inout_ PCWSTR *Cursor,
        _Inout_ BOOL *Error,
        _In_ ULONG Depth);

    DOUBLE
    ParseTerm(
        _Inout_ PCWSTR *Cursor,
        _Inout_ BOOL *Error,
        _In_ ULONG Depth);

    DOUBLE
    ParseFactor(
        _Inout_ PCWSTR *Cursor,
        _Inout_ BOOL *Error,
        _In_ ULONG Depth);

    VOID
    RefreshSheet(
        _In_ BOOL Rebuild);

    VOID
    SelectCell(
        _In_ LONG Row,
        _In_ LONG Column);

    VOID
    CommitCell(VOID);

    /* Slides */
    VOID
    LoadSlides(
        _In_ const std::wstring &Text);

    VOID
    ShowSlide(
        _In_ LONG Index);

    OFFICE_MODE m_Mode = ModeWriter;
    OFFICE_PAGE m_Page = PageList;
    OFFICE_MENU m_Menu = MenuNone;
    BOOL m_Started = FALSE;
    BOOL m_AllFolders = FALSE;
    BOOL m_SortByName = FALSE;
    std::vector<OFFICE_DOC> m_Docs;
    std::vector<LONG> m_Shown;
    std::vector<std::pair<std::wstring, std::wstring>> m_ContentCache;
    std::wstring m_Find;
    BOOL m_FindFocus = FALSE;
    LONG m_HoldTarget = -1;

    OFFICE_DOC m_Doc;
    BOOL m_Dirty = FALSE;
    std::wstring m_Text;
    SIZE_T m_Caret = 0;
    std::wstring m_DocFind;
    BOOL m_DocFindMode = FALSE;

    std::wstring m_Cells[SHEET_ROWS][SHEET_COLUMNS];
    LONG m_Row = 0;
    LONG m_Column = 0;
    std::wstring m_Entry;
    BOOL m_Editing = FALSE;

    std::vector<OFFICE_SLIDE> m_Slides;
    LONG m_Slide = 0;
};

std::wstring
COfficeApp::Folder(VOID) const
{
    std::wstring Path = AppDataFile(L"My Documents");

    CreateDirectoryW(Path.c_str(), NULL);
    if (m_Mode == ModeNotes)
    {
        Path += L"\\Notes";
        CreateDirectoryW(Path.c_str(), NULL);
    }
    return Path;
}

BOOL
COfficeApp::Accepts(
    _In_ const std::wstring &Name) const
{
    switch (m_Mode)
    {
        case ModeWriter:
            return EndsWith(Name, L".txt") || EndsWith(Name, L".rtf");
        case ModeSheets:
            return EndsWith(Name, L".csv");
        case ModeSlides:
            return EndsWith(Name, L".slides");
        default:
            return EndsWith(Name, L".txt");
    }
}

VOID
COfficeApp::Seed(VOID)
{
    std::wstring Base = Folder() + L"\\";
    std::wstring Marker = Base + ((m_Mode == ModeWriter) ? L".writer" : (m_Mode == ModeSheets) ? L".sheets" :
                                                          (m_Mode == ModeSlides) ? L".slides-seeded" : L".notes");
    HANDLE File;

    /* Only once per program, so deleting the samples keeps them deleted */
    if (GetFileAttributesW(Marker.c_str()) != INVALID_FILE_ATTRIBUTES)
        return;
    File = CreateFileW(Marker.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, NULL);
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);

    switch (m_Mode)
    {
        case ModeWriter:
            WriteTextFile(Base + L"Welcome to Writer Mobile.txt",
                L"Welcome to Writer Mobile\n\n"
                L"This is ReactOS Office Mobile, the productivity suite that fits in your shirt pocket "
                L"and runs on an operating system you can actually read the source of.\n\n"
                L"Tap New to start a document. Tap ok to save, it saves on its own just like the real thing did. "
                L"Use Find to light up every match in a document, or type in the list's find box to search "
                L"inside every document at once.\n\n"
                L"Known limitations: no clip art, no paper clip, no tears.");
            WriteTextFile(Base + L"Office Memo.txt",
                L"MEMO\n\n"
                L"To: Everyone\nFrom: Facilities\nRe: The coffee machine\n\n"
                L"The coffee machine now runs ReactOS Mobile. It boots in under a minute and it will not reboot "
                L"for updates during your espresso. Please stop asking it to sync with ActiveSync.\n\n"
                L"Reminder: the stylus is not a stirring stick.");
            WriteTextFile(Base + L"Quarterly Report.rtf",
                L"{\\rtf1\\ansi{\\fonttbl{\\f0 Tahoma;}}{\\colortbl;\\red0\\green0\\blue0;}"
                L"\\f0\\fs20{\\b Quarterly Report}\\par\\par "
                L"Revenue: up, mostly from selling styluses back to people who lost theirs.\\par "
                L"Costs: down, the fax machine was finally retired.\\par "
                L"Outlook: sunny, with a chance of Pocket Outlook.\\par }");
            break;

        case ModeSheets:
            WriteTextFile(Base + L"Office Budget.csv",
                L"Item,Qty,Price,Total\n"
                L"Coffee beans,12,9.5,=B2*C2\n"
                L"Spare styluses,40,1.25,=B3*C3\n"
                L"Sync cables,6,7,=B4*C4\n"
                L"Screen protectors,25,2.2,=B5*C5\n"
                L"Fax paper,1,30,=B6*C6\n"
                L"Total,=SUM(B2:B6),,=SUM(D2:D6)\n"
                L"Average,,=AVERAGE(C2:C6),\n");
            WriteTextFile(Base + L"Mileage.csv",
                L"Day,Miles,Rate,Pay\n"
                L"Mon,14,0.67,=B2*C2\nTue,22,0.67,=B3*C3\nWed,9,0.67,=B4*C4\n"
                L"Week,=SUM(B2:B4),,=SUM(D2:D4)\nLongest,=MAX(B2:B4),,\n");
            break;

        case ModeSlides:
            WriteTextFile(Base + L"ReactOS Mobile 2026.slides",
                L"# ReactOS Mobile 2026\nThe strategy deck nobody asked for\n\n"
                L"# Where we are\n- A shell that looks like 2008\n- Built like 2016\n- Running in 2026\n\n"
                L"# What users want\n- Start menu that drops down\n- Soft keys that mean something\n- A stylus they will lose\n\n"
                L"# The plan\n- Ship Office Mobile\n- Ship it again with a W10M skin\n- Never mention the paper clip\n\n"
                L"# Questions?\nTap to end the show");
            WriteTextFile(Base + L"Stylus Safety.slides",
                L"# Stylus Safety Training\nMandatory for all staff\n\n"
                L"# Do\n- Tap gently\n- Hold to see more options\n- Put it back in the slot\n\n"
                L"# Do not\n- Stir coffee with it\n- Use it as a hair stick in meetings\n- Calibrate the screen at lunch");
            break;

        case ModeNotes:
            WriteTextFile(Base + L"Shopping list.txt", L"Shopping list\n\n- Milk\n- AA batteries\n- A new stylus (again)");
            WriteTextFile(Base + L"Ideas.txt", L"Ideas\n\nA W10M skin for this shell.\nLive tiles that actually tell you something.\nAn app that finds the lost stylus.");
            break;
    }
}

VOID
COfficeApp::ScanFolder(
    _In_ const std::wstring &Directory,
    _In_ const std::wstring &Label,
    _In_ BOOL ReadOnly,
    _In_ ULONG Depth)
{
    WIN32_FIND_DATAW Data;
    HANDLE Search;

    Search = FindFirstFileW((Directory + L"\\*").c_str(), &Data);
    if (Search == INVALID_HANDLE_VALUE)
        return;

    do
    {
        std::wstring Name = Data.cFileName;

        if (Name == L"." || Name == L".." || (Data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
            continue;

        if (Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (Depth > 0)
                ScanFolder(Directory + L"\\" + Name, Label + L"\\" + Name, ReadOnly, Depth - 1);
            continue;
        }

        if (!Accepts(Name))
            continue;

        OFFICE_DOC Doc;
        Doc.Path = Directory + L"\\" + Name;
        Doc.Name = Name;
        Doc.Folder = Label;
        Doc.Modified = Data.ftLastWriteTime;
        Doc.Size = (static_cast<ULONGLONG>(Data.nFileSizeHigh) << 32) | Data.nFileSizeLow;
        Doc.ReadOnly = ReadOnly || EndsWith(Name, L".rtf") || m_Mode == ModeSlides;
        m_Docs.push_back(Doc);
    } while (FindNextFileW(Search, &Data));
    FindClose(Search);
}

VOID
COfficeApp::Scan(VOID)
{
    PWSTR Documents = NULL;

    m_Docs.clear();
    ScanFolder(Folder(), (m_Mode == ModeNotes) ? L"Notes" : L"My Documents", FALSE, 0);

    /* "All Folders" also looks through the PC's own Documents, read only */
    if (m_AllFolders && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &Documents)))
        ScanFolder(Documents, L"Documents", TRUE, 1);
    CoTaskMemFree(Documents);

    std::sort(m_Docs.begin(), m_Docs.end(), [this](const OFFICE_DOC &A, const OFFICE_DOC &B)
    {
        if (m_SortByName)
            return _wcsicmp(A.Name.c_str(), B.Name.c_str()) < 0;
        return CompareFileTime(&A.Modified, &B.Modified) > 0;
    });
}

std::wstring
COfficeApp::Contents(
    _In_ const OFFICE_DOC &Doc)
{
    std::wstring Text;

    for (const auto &Entry : m_ContentCache)
    {
        if (Entry.first == Doc.Path)
            return Entry.second;
    }

    Text = ReadTextFile(Doc.Path);
    if (EndsWith(Doc.Name, L".rtf"))
        Text = StripRtf(Text);
    m_ContentCache.emplace_back(Doc.Path, Text);
    return Text;
}

VOID
COfficeApp::ShowList(VOID)
{
    static PCWSTR const Fields[] = { L"Id", L"Name", L"Detail" };
    std::vector<std::wstring> Storage;
    std::vector<PCWSTR> Values;
    SYSTEMTIME Utc;
    SYSTEMTIME Local;
    WCHAR Date[48];
    WCHAR Id[16];

    m_Shown.clear();
    for (LONG Index = 0; Index < static_cast<LONG>(m_Docs.size()); Index++)
    {
        const OFFICE_DOC &Doc = m_Docs[Index];
        std::wstring Detail;
        SIZE_T Match = std::wstring::npos;

        /* Find matches the name first, then looks inside the document */
        if (!m_Find.empty() && FindNoCase(Doc.Name, m_Find) == std::wstring::npos)
        {
            std::wstring Text = Contents(Doc);

            Match = FindNoCase(Text, m_Find);
            if (Match == std::wstring::npos)
                continue;

            SIZE_T Start = (Match > 12) ? Match - 12 : 0;
            Detail = Text.substr(Start, 40);
            std::replace(Detail.begin(), Detail.end(), L'\n', L' ');
            Detail = ((Start > 0) ? L"..." : L"") + Detail + L"...";
        }
        else
        {
            FileTimeToSystemTime(&Doc.Modified, &Utc);
            SystemTimeToTzSpecificLocalTime(NULL, &Utc, &Local);
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &Local, NULL, Date, ARRAYSIZE(Date), NULL);
            Detail = std::wstring(Date) + L"   " + std::to_wstring(max(1ULL, (Doc.Size + 1023) / 1024)) + L" KB   " + Doc.Folder;
        }

        StringCchPrintfW(Id, ARRAYSIZE(Id), L"%ld", Index);
        Storage.insert(Storage.end(), { Id, Doc.Name.substr(0, Doc.Name.find_last_of(L'.')), Detail });
        m_Shown.push_back(Index);
    }

    for (const std::wstring &Value : Storage)
        Values.push_back(Value.c_str());

    Scene()->SetItems(L"Docs", static_cast<ULONG>(m_Shown.size()), ARRAYSIZE(Fields), Fields, Values.data());
    Scene()->SetBinding(L"FindText", (m_Find + (m_FindFocus ? L"|" : L"")).c_str());
    Scene()->SetVisible(L"FindHint", m_Find.empty() && !m_FindFocus);
    Scene()->SetVisible(L"ListEmpty", m_Shown.empty());
    Scene()->SetBinding(L"ListHeader", m_AllFolders ? L"All Folders" : ((m_Mode == ModeNotes) ? L"Notes" : L"My Documents"));
    Scene()->SetBinding(L"SortLabel", m_SortByName ? L"Name" : L"Date");
}

VOID
COfficeApp::ShowPage(
    _In_ OFFICE_PAGE Page)
{
    static PCWSTR const Titles[] = { L"Writer Mobile", L"Sheets Mobile", L"Slides Mobile", L"Notes" };
    std::wstring Title = Titles[m_Mode];

    m_Page = Page;
    Scene()->SetVisible(L"PageList", Page == PageList);
    Scene()->SetVisible(L"PageEdit", Page == PageEdit);
    Scene()->SetVisible(L"PageSheet", Page == PageSheet);
    Scene()->SetVisible(L"PageShow", Page == PageShow);
    Scene()->SetSelected(NULL);

    if (Page != PageList)
        Title = m_Doc.Name.substr(0, m_Doc.Name.find_last_of(L'.')) + (m_Doc.ReadOnly ? L" (read only)" : L"");

    switch (Page)
    {
        case PageList:
            ShowList();
            SetChrome(Title.c_str(), (m_Mode == ModeSlides) ? L"" : L"New", L"Menu", ShellCloseOk);
            break;
        case PageEdit:
            SetChrome(Title.c_str(), L"Find", L"Menu", ShellCloseOk);
            break;
        case PageSheet:
            SetChrome(Title.c_str(), L"Edit", L"Menu", ShellCloseOk);
            break;
        case PageShow:
            SetChrome(Title.c_str(), L"Previous", L"Next", ShellCloseOk);
            break;
    }
}

VOID
COfficeApp::OnActivate(VOID)
{
    PCWSTR Mode;

    if (!m_Started)
    {
        m_Started = TRUE;
        Mode = Scene()->GetAttribute(NULL, L"Mode");
        if (Mode && _wcsicmp(Mode, L"Sheets") == 0)
            m_Mode = ModeSheets;
        else if (Mode && _wcsicmp(Mode, L"Slides") == 0)
            m_Mode = ModeSlides;
        else if (Mode && _wcsicmp(Mode, L"Notes") == 0)
            m_Mode = ModeNotes;

        Seed();
        Scan();
    }
    ShowPage(m_Page);
}

VOID
COfficeApp::Open(
    _In_ LONG Index)
{
    if (Index < 0 || Index >= static_cast<LONG>(m_Docs.size()))
        return;

    m_Doc = m_Docs[Index];
    OpenCurrent();
}

VOID
COfficeApp::OnOpenDocument(
    _In_z_ PCWSTR Path)
{
    WIN32_FILE_ATTRIBUTE_DATA Data;
    std::wstring Full = Path;
    std::wstring Home = Folder();
    SIZE_T Slash = Full.find_last_of(L"\\/");

    if (!GetFileAttributesExW(Path, GetFileExInfoStandard, &Data))
        return;

    /* Documents from other apps are writable only when they live in our own folder */
    m_Doc = OFFICE_DOC();
    m_Doc.Path = Full;
    m_Doc.Name = (Slash == std::wstring::npos) ? Full : Full.substr(Slash + 1);
    m_Doc.Folder = (Slash == std::wstring::npos) ? L"" : Full.substr(0, Slash);
    m_Doc.Modified = Data.ftLastWriteTime;
    m_Doc.Size = (static_cast<ULONGLONG>(Data.nFileSizeHigh) << 32) | Data.nFileSizeLow;
    m_Doc.ReadOnly = _wcsnicmp(Full.c_str(), Home.c_str(), Home.size()) != 0 ||
                     EndsWith(m_Doc.Name, L".rtf") || m_Mode == ModeSlides;
    m_ContentCache.clear();
    OpenCurrent();
}

VOID
COfficeApp::OpenCurrent(VOID)
{
    std::wstring Text;

    m_Dirty = FALSE;
    m_FindFocus = FALSE;
    Text = Contents(m_Doc);

    if (m_Mode == ModeSheets)
    {
        LoadSheet(Text);
        ShowPage(PageSheet);
        SelectCell(0, 0);
    }
    else if (m_Mode == ModeSlides)
    {
        LoadSlides(Text);
        ShowPage(PageShow);
        ShowSlide(0);
    }
    else
    {
        m_Text = Text;
        m_Caret = m_Text.size();
        m_DocFind.clear();
        m_DocFindMode = FALSE;
        ShowPage(PageEdit);
        RefreshText();
        Scene()->ResetScroll(L"DocScroll");
    }
}

VOID
COfficeApp::NewDocument(VOID)
{
    std::wstring Base = (m_Mode == ModeNotes) ? L"Note" : (m_Mode == ModeSheets) ? L"Book" : L"Document";
    PCWSTR Extension = (m_Mode == ModeSheets) ? L".csv" : L".txt";
    std::wstring Path;

    /* Document1, Document2... like the device named them */
    for (ULONG Number = 1; Number < 1000; Number++)
    {
        Path = Folder() + L"\\" + Base + std::to_wstring(Number) + Extension;
        if (GetFileAttributesW(Path.c_str()) == INVALID_FILE_ATTRIBUTES)
            break;
    }

    m_Doc = OFFICE_DOC();
    m_Doc.Path = Path;
    m_Doc.Name = Path.substr(Path.find_last_of(L'\\') + 1);
    m_Doc.Folder = (m_Mode == ModeNotes) ? L"Notes" : L"My Documents";
    m_Dirty = TRUE;

    if (m_Mode == ModeSheets)
    {
        LoadSheet(L"");
        ShowPage(PageSheet);
        SelectCell(0, 0);
    }
    else
    {
        m_Text.clear();
        m_Caret = 0;
        m_DocFind.clear();
        m_DocFindMode = FALSE;
        ShowPage(PageEdit);
        RefreshText();
        ShowInputPanel(TRUE);
    }
}

BOOL
COfficeApp::Save(VOID)
{
    BOOL Result;

    if (m_Doc.ReadOnly || !m_Dirty)
        return TRUE;

    Result = WriteTextFile(m_Doc.Path, (m_Mode == ModeSheets) ? SheetCsv() : m_Text);
    if (Result)
    {
        m_Dirty = FALSE;
        m_ContentCache.clear();
    }
    return Result;
}

VOID
COfficeApp::SaveCopy(VOID)
{
    std::wstring Stem = m_Doc.Name.substr(0, m_Doc.Name.find_last_of(L'.'));
    PCWSTR Extension = (m_Mode == ModeSheets) ? L".csv" : L".txt";

    /* A read only document becomes an editable copy in My Documents */
    m_Doc.Path = Folder() + L"\\" + Stem + L" copy" + Extension;
    m_Doc.Name = Stem + L" copy" + Extension;
    m_Doc.Folder = L"My Documents";
    m_Doc.ReadOnly = FALSE;
    m_Dirty = TRUE;
    Save();
    ShowPage(m_Page);
    if (m_Page == PageEdit)
        RefreshText();
    Notify(L"Saved a copy", (m_Doc.Name + L" is in My Documents and can be edited.").c_str());
}

VOID
COfficeApp::Delete(
    _In_ LONG Index)
{
    if (Index < 0 || Index >= static_cast<LONG>(m_Docs.size()))
        return;

    if (m_Docs[Index].ReadOnly && m_Docs[Index].Folder != L"My Documents" && m_Docs[Index].Folder != L"Notes")
    {
        Notify(L"Cannot delete", L"Documents outside My Documents are only shown, never changed.");
        return;
    }

    DeleteFileW(m_Docs[Index].Path.c_str());
    m_ContentCache.clear();
    Scan();
    ShowList();
}

/* Writer and Notes */

VOID
COfficeApp::RefreshText(VOID)
{
    std::wstring Shown = m_Text;
    ULONG Count = 0;
    SIZE_T Found = 0;

    if (!m_Doc.ReadOnly && !m_DocFindMode)
        Shown.insert(min(m_Caret, Shown.size()), 1, L'|');
    if (Shown.empty())
        Shown = L" ";
    Scene()->SetBinding(L"DocText", Shown.c_str());
    Scene()->SetBinding(L"DocFind", m_DocFind.c_str());

    /* Count every match for the find bar */
    if (!m_DocFind.empty())
    {
        for (SIZE_T Start = 0; (Found = FindNoCase(m_Text.substr(Start), m_DocFind)) != std::wstring::npos; Start += Found + m_DocFind.size())
            Count++;
    }

    Scene()->SetBinding(L"DocFindText", (m_DocFind + (m_DocFindMode ? L"|" : L"")).c_str());
    Scene()->SetBinding(L"DocFindCount", m_DocFind.empty() ? L"" : (std::to_wstring(Count) + ((Count == 1) ? L" match" : L" matches")).c_str());
    Scene()->SetVisible(L"FindBar", m_DocFindMode || !m_DocFind.empty());
}

VOID
COfficeApp::Edit(
    _In_ const std::wstring &Insert,
    _In_ SIZE_T Remove)
{
    if (m_Doc.ReadOnly)
        return;

    m_Caret = min(m_Caret, m_Text.size());
    Remove = min(Remove, m_Caret);
    m_Text.erase(m_Caret - Remove, Remove);
    m_Caret -= Remove;
    m_Text.insert(m_Caret, Insert);
    m_Caret += Insert.size();
    m_Dirty = TRUE;
    RefreshText();

    /* Typing at the end keeps the end in view */
    if (m_Caret == m_Text.size())
        Scene()->ScrollTo(L"DocScroll", 1.0e6f);
}

VOID
COfficeApp::MoveLine(
    _In_ INT Direction)
{
    /* Start of the line holding Position: just past the newline before it */
    auto LineBegin = [this](SIZE_T Position) -> SIZE_T
    {
        SIZE_T Newline = (Position == 0) ? std::wstring::npos : m_Text.rfind(L'\n', Position - 1);

        return (Newline == std::wstring::npos) ? 0 : Newline + 1;
    };
    SIZE_T LineStart = LineBegin(m_Caret);
    SIZE_T Column = m_Caret - LineStart;
    SIZE_T Next;
    SIZE_T NextEnd;

    /* Keep the column, clamped to the length of the line we land on */
    if (Direction < 0)
    {
        if (LineStart == 0)
            m_Caret = 0;
        else
            m_Caret = min(LineBegin(LineStart - 1) + Column, LineStart - 1);
        return;
    }

    Next = m_Text.find(L'\n', m_Caret);
    if (Next == std::wstring::npos)
    {
        m_Caret = m_Text.size();
        return;
    }
    NextEnd = m_Text.find(L'\n', Next + 1);
    m_Caret = min(Next + 1 + Column, (NextEnd == std::wstring::npos) ? m_Text.size() : NextEnd);
}

/* Sheets */

static
std::wstring
CellName(
    _In_ LONG Row,
    _In_ LONG Column)
{
    return std::wstring(1, static_cast<WCHAR>(L'A' + Column)) + std::to_wstring(Row + 1);
}

VOID
COfficeApp::LoadSheet(
    _In_ const std::wstring &Text)
{
    LONG Row = 0;
    LONG Column = 0;
    BOOL Quoted = FALSE;
    BOOL Wide = FALSE;
    std::wstring Field;

    for (auto &Line : m_Cells)
    {
        for (auto &Cell : Line)
            Cell.clear();
    }

    /* CSV with quotes; anything past the grid makes the sheet read only so nothing is lost */
    for (SIZE_T Index = 0; Index <= Text.size(); Index++)
    {
        WCHAR Char = (Index < Text.size()) ? Text[Index] : L'\n';

        if (Quoted)
        {
            if (Char == L'"' && Index + 1 < Text.size() && Text[Index + 1] == L'"')
            {
                Field += L'"';
                Index++;
            }
            else if (Char == L'"')
            {
                Quoted = FALSE;
            }
            else
            {
                Field += Char;
            }
            continue;
        }

        if (Char == L'"')
        {
            Quoted = TRUE;
        }
        else if (Char == L',' || Char == L'\n')
        {
            if (Row < SHEET_ROWS && Column < SHEET_COLUMNS)
                m_Cells[Row][Column] = Field;
            else if (!Field.empty())
                Wide = TRUE;
            Field.clear();

            if (Char == L',')
            {
                Column++;
            }
            else
            {
                Column = 0;
                Row++;
            }
        }
        else
        {
            Field += Char;
        }
    }

    if (Wide)
        m_Doc.ReadOnly = TRUE;
    m_Row = 0;
    m_Column = 0;
    m_Editing = FALSE;
    RefreshSheet(TRUE);
}

std::wstring
COfficeApp::SheetCsv(VOID) const
{
    std::wstring Text;
    LONG LastRow = -1;

    for (LONG Row = 0; Row < SHEET_ROWS; Row++)
    {
        for (LONG Column = 0; Column < SHEET_COLUMNS; Column++)
        {
            if (!m_Cells[Row][Column].empty())
                LastRow = Row;
        }
    }

    for (LONG Row = 0; Row <= LastRow; Row++)
    {
        for (LONG Column = 0; Column < SHEET_COLUMNS; Column++)
        {
            const std::wstring &Cell = m_Cells[Row][Column];

            if (Column)
                Text += L',';
            if (Cell.find_first_of(L",\"\n") != std::wstring::npos)
            {
                Text += L'"';
                for (WCHAR Char : Cell)
                    Text += (Char == L'"') ? std::wstring(L"\"\"") : std::wstring(1, Char);
                Text += L'"';
            }
            else
            {
                Text += Cell;
            }
        }
        Text += L'\n';
    }
    return Text;
}

static
BOOL
ParseCell(
    _Inout_ PCWSTR *Cursor,
    _Out_ LONG *Row,
    _Out_ LONG *Column)
{
    PCWSTR Walk = *Cursor;
    LONG Number = 0;

    if (!iswalpha(*Walk))
        return FALSE;
    *Column = towupper(*Walk) - L'A';
    Walk++;
    if (!iswdigit(*Walk))
        return FALSE;
    while (iswdigit(*Walk))
        Number = Number * 10 + (*Walk++ - L'0');

    *Row = Number - 1;
    *Cursor = Walk;
    return TRUE;
}

DOUBLE
COfficeApp::Evaluate(
    _In_ LONG Row,
    _In_ LONG Column,
    _Inout_ BOOL *Error,
    _In_ ULONG Depth)
{
    PCWSTR Cursor;
    PWSTR End;
    DOUBLE Value;

    if (Row < 0 || Row >= SHEET_ROWS || Column < 0 || Column >= SHEET_COLUMNS || Depth > 32)
    {
        *Error = TRUE;
        return 0.0;
    }

    const std::wstring &Cell = m_Cells[Row][Column];
    if (Cell.empty())
        return 0.0;

    if (Cell[0] == L'=')
    {
        Cursor = Cell.c_str() + 1;
        Value = ParseExpression(&Cursor, Error, Depth + 1);
        while (iswspace(*Cursor))
            Cursor++;
        if (*Cursor)
            *Error = TRUE;
        return Value;
    }

    /* Text counts as zero in arithmetic */
    Value = wcstod(Cell.c_str(), &End);
    return (End != Cell.c_str()) ? Value : 0.0;
}

DOUBLE
COfficeApp::ParseFactor(
    _Inout_ PCWSTR *Cursor,
    _Inout_ BOOL *Error,
    _In_ ULONG Depth)
{
    static PCWSTR const Functions[] = { L"SUM", L"AVERAGE", L"MIN", L"MAX", L"COUNT" };
    PCWSTR Walk = *Cursor;
    PWSTR End;
    DOUBLE Value = 0.0;
    LONG Row;
    LONG Column;

    while (iswspace(*Walk))
        Walk++;

    if (*Walk == L'-')
    {
        Walk++;
        *Cursor = Walk;
        return -ParseFactor(Cursor, Error, Depth);
    }

    if (*Walk == L'(')
    {
        Walk++;
        Value = ParseExpression(&Walk, Error, Depth);
        while (iswspace(*Walk))
            Walk++;
        if (*Walk != L')')
            *Error = TRUE;
        else
            Walk++;
        *Cursor = Walk;
        return Value;
    }

    for (ULONG Function = 0; Function < ARRAYSIZE(Functions); Function++)
    {
        SIZE_T Length = wcslen(Functions[Function]);
        std::vector<DOUBLE> Numbers;
        LONG Row2;
        LONG Column2;

        if (_wcsnicmp(Walk, Functions[Function], Length) != 0 || Walk[Length] != L'(')
            continue;

        /* Arguments are ranges like A1:B3 or expressions, separated by commas */
        Walk += Length + 1;
        for (;;)
        {
            PCWSTR Save = Walk;

            while (iswspace(*Walk))
                Walk++;
            if (*Walk == L')')
                break;

            if (ParseCell(&Walk, &Row, &Column) && *Walk == L':' && (Walk++, ParseCell(&Walk, &Row2, &Column2)))
            {
                for (LONG R = min(Row, Row2); R <= max(Row, Row2); R++)
                {
                    for (LONG C = min(Column, Column2); C <= max(Column, Column2); C++)
                    {
                        if (R >= 0 && R < SHEET_ROWS && C >= 0 && C < SHEET_COLUMNS && !m_Cells[R][C].empty())
                        {
                            PWSTR Stop;
                            wcstod(m_Cells[R][C].c_str(), &Stop);
                            if (m_Cells[R][C][0] == L'=' || Stop != m_Cells[R][C].c_str())
                                Numbers.push_back(Evaluate(R, C, Error, Depth + 1));
                        }
                    }
                }
            }
            else
            {
                Walk = Save;
                Numbers.push_back(ParseExpression(&Walk, Error, Depth));
            }

            while (iswspace(*Walk))
                Walk++;
            if (*Walk == L',')
            {
                Walk++;
                continue;
            }
            if (*Walk != L')')
            {
                *Error = TRUE;
                *Cursor = Walk;
                return 0.0;
            }
            break;
        }
        Walk++;
        *Cursor = Walk;

        switch (Function)
        {
            case 0:
                for (DOUBLE Number : Numbers)
                    Value += Number;
                return Value;
            case 1:
                for (DOUBLE Number : Numbers)
                    Value += Number;
                return Numbers.empty() ? 0.0 : Value / Numbers.size();
            case 2:
                return Numbers.empty() ? 0.0 : *std::min_element(Numbers.begin(), Numbers.end());
            case 3:
                return Numbers.empty() ? 0.0 : *std::max_element(Numbers.begin(), Numbers.end());
            default:
                return static_cast<DOUBLE>(Numbers.size());
        }
    }

    if (ParseCell(&Walk, &Row, &Column))
    {
        *Cursor = Walk;
        return Evaluate(Row, Column, Error, Depth + 1);
    }

    Value = wcstod(Walk, &End);
    if (End == Walk)
        *Error = TRUE;
    *Cursor = End;
    return Value;
}

DOUBLE
COfficeApp::ParseTerm(
    _Inout_ PCWSTR *Cursor,
    _Inout_ BOOL *Error,
    _In_ ULONG Depth)
{
    DOUBLE Value = ParseFactor(Cursor, Error, Depth);
    DOUBLE Right;

    for (;;)
    {
        while (iswspace(**Cursor))
            (*Cursor)++;
        if (**Cursor == L'*')
        {
            (*Cursor)++;
            Value *= ParseFactor(Cursor, Error, Depth);
        }
        else if (**Cursor == L'/')
        {
            (*Cursor)++;
            Right = ParseFactor(Cursor, Error, Depth);
            if (Right == 0.0)
                *Error = TRUE;
            else
                Value /= Right;
        }
        else if (**Cursor == L'^')
        {
            (*Cursor)++;
            Value = pow(Value, ParseFactor(Cursor, Error, Depth));
        }
        else
        {
            return Value;
        }
    }
}

DOUBLE
COfficeApp::ParseExpression(
    _Inout_ PCWSTR *Cursor,
    _Inout_ BOOL *Error,
    _In_ ULONG Depth)
{
    DOUBLE Value = ParseTerm(Cursor, Error, Depth);

    for (;;)
    {
        while (iswspace(**Cursor))
            (*Cursor)++;
        if (**Cursor == L'+')
        {
            (*Cursor)++;
            Value += ParseTerm(Cursor, Error, Depth);
        }
        else if (**Cursor == L'-')
        {
            (*Cursor)++;
            Value -= ParseTerm(Cursor, Error, Depth);
        }
        else
        {
            return Value;
        }
    }
}

std::wstring
COfficeApp::Display(
    _In_ LONG Row,
    _In_ LONG Column)
{
    const std::wstring &Cell = m_Cells[Row][Column];
    BOOL Error = FALSE;
    DOUBLE Value;
    WCHAR Text[40];

    if (Cell.empty() || Cell[0] != L'=')
        return Cell;

    Value = Evaluate(Row, Column, &Error, 0);
    if (Error || !_finite(Value))
        return L"#ERR";
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%.10g", Value);
    return Text;
}

VOID
COfficeApp::RefreshSheet(
    _In_ BOOL Rebuild)
{
    static PCWSTR const Fields[] = { L"Id", L"Text", L"W", L"Fill", L"Align", L"Command" };
    std::vector<std::wstring> Storage;
    std::vector<PCWSTR> Values;
    WCHAR Text[24];

    if (Rebuild)
    {
        /* One flat wrap of row header plus five cells per row; selection is the scene's own */
        for (LONG Row = 0; Row < SHEET_ROWS; Row++)
        {
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"h%ld", Row);
            Storage.insert(Storage.end(), { Text, std::to_wstring(Row + 1), L"@SheetHeaderWidth", L"@IndexBar", L"Center",
                                            L"cell:" + std::to_wstring(Row) + L",0" });

            for (LONG Column = 0; Column < SHEET_COLUMNS; Column++)
            {
                std::wstring Shown = Display(Row, Column);
                PWSTR End;

                wcstod(Shown.c_str(), &End);
                StringCchPrintfW(Text, ARRAYSIZE(Text), L"c%ld_%ld", Row, Column);
                Storage.insert(Storage.end(), { Text, Shown, L"@SheetCellWidth", L"",
                                                (!Shown.empty() && *End == 0) ? L"Right" : L"Left",
                                                L"cell:" + std::to_wstring(Row) + L"," + std::to_wstring(Column) });
            }
        }

        for (const std::wstring &Value : Storage)
            Values.push_back(Value.c_str());
        Scene()->SetItems(L"Cells", SHEET_ROWS * (SHEET_COLUMNS + 1), ARRAYSIZE(Fields), Fields, Values.data());
    }

    StringCchPrintfW(Text, ARRAYSIZE(Text), L"c%ld_%ld", m_Row, m_Column);
    Scene()->SetSelected(Text);
    Scene()->SetBinding(L"CellRef", CellName(m_Row, m_Column).c_str());
    Scene()->SetBinding(L"CellEdit", m_Editing ? (m_Entry + L"|").c_str() : m_Cells[m_Row][m_Column].c_str());
}

VOID
COfficeApp::CommitCell(VOID)
{
    if (!m_Editing)
        return;

    m_Editing = FALSE;
    if (m_Cells[m_Row][m_Column] != m_Entry)
    {
        m_Cells[m_Row][m_Column] = m_Entry;
        m_Dirty = TRUE;
        RefreshSheet(TRUE);
    }
}

VOID
COfficeApp::SelectCell(
    _In_ LONG Row,
    _In_ LONG Column)
{
    CommitCell();
    m_Row = max(0L, min(Row, SHEET_ROWS - 1L));
    m_Column = max(0L, min(Column, SHEET_COLUMNS - 1L));
    RefreshSheet(FALSE);
}

/* Slides */

VOID
COfficeApp::LoadSlides(
    _In_ const std::wstring &Text)
{
    SIZE_T Start = 0;

    m_Slides.clear();
    while (Start <= Text.size())
    {
        SIZE_T End = Text.find(L'\n', Start);
        std::wstring Line = Text.substr(Start, (End == std::wstring::npos) ? std::wstring::npos : End - Start);

        Start = (End == std::wstring::npos) ? Text.size() + 1 : End + 1;
        if (Line.empty())
            continue;

        /* "# " opens a slide, "- " is a bullet, anything else is body text */
        if (Line.compare(0, 2, L"# ") == 0 || m_Slides.empty())
        {
            m_Slides.emplace_back();
            if (Line.compare(0, 2, L"# ") == 0)
            {
                m_Slides.back().Title = Line.substr(2);
                continue;
            }
        }

        if (Line.compare(0, 2, L"- ") == 0)
            m_Slides.back().Bullets.push_back(L"\x2022  " + Line.substr(2));
        else
            m_Slides.back().Body += (m_Slides.back().Body.empty() ? L"" : L"\n") + Line;
    }

    OFFICE_SLIDE End;
    End.Title = L"End of slide show";
    End.Body = L"Tap to exit.";
    m_Slides.push_back(End);
}

VOID
COfficeApp::ShowSlide(
    _In_ LONG Index)
{
    static PCWSTR const Fields[] = { L"Text" };
    std::vector<PCWSTR> Values;

    if (Index < 0)
        Index = 0;
    if (Index >= static_cast<LONG>(m_Slides.size()))
    {
        ShowPage(PageList);
        return;
    }

    m_Slide = Index;
    const OFFICE_SLIDE &Slide = m_Slides[Index];
    for (const std::wstring &Bullet : Slide.Bullets)
        Values.push_back(Bullet.c_str());

    Scene()->SetBinding(L"SlideTitle", Slide.Title.c_str());
    Scene()->SetBinding(L"SlideBody", Slide.Body.c_str());
    Scene()->SetItems(L"Bullets", static_cast<ULONG>(Slide.Bullets.size()), 1, Fields, Values.data());
    Scene()->SetBinding(L"SlideNumber", (Index + 1 < static_cast<LONG>(m_Slides.size()))
                                        ? (std::to_wstring(Index + 1) + L" / " + std::to_wstring(m_Slides.size() - 1)).c_str() : L"");
}

/* Input */

VOID
COfficeApp::OnCommand(
    _In_z_ PCWSTR Command)
{
    LONG Row;
    LONG Column;

    if (_wcsicmp(Command, L"find.focus") == 0)
    {
        m_FindFocus = TRUE;
        ShowList();
        ShowInputPanel(TRUE);
    }
    else if (_wcsicmp(Command, L"find.clear") == 0)
    {
        m_Find.clear();
        m_FindFocus = FALSE;
        ShowList();
    }
    else if (_wcsnicmp(Command, L"open:", 5) == 0)
    {
        Open(_wtol(Command + 5));
    }
    else if (_wcsnicmp(Command, L"cell:", 5) == 0)
    {
        if (swscanf(Command + 5, L"%ld,%ld", &Row, &Column) == 2)
            SelectCell(Row, Column);
    }
    else if (_wcsicmp(Command, L"slide.next") == 0)
    {
        ShowSlide(m_Slide + 1);
    }
    else
    {
        CAppClient::OnCommand(Command);
    }
}

VOID
COfficeApp::OnHold(
    _In_z_ PCWSTR Command,
    _In_ FLOAT X,
    _In_ FLOAT Y)
{
    if (_wcsnicmp(Command, L"hold:", 5) != 0)
        return;

    m_HoldTarget = _wtol(Command + 5);
    m_Menu = MenuHold;
    ShowMenu(AppMenuPoint, X, Y, L"Open\n-\nDelete");
}

VOID
COfficeApp::OnSoftKey(
    _In_ SHELL_SOFTKEY Key)
{
    std::wstring Items;

    switch (m_Page)
    {
        case PageList:
            if (Key == ShellSoftKeyLeft && m_Mode != ModeSlides)
            {
                NewDocument();
            }
            else if (Key == ShellSoftKeyRight)
            {
                m_Menu = MenuList;
                Items = (m_Mode == ModeSlides) ? L"Open\n-\n" : L"New\nDelete\n-\n";
                Items += m_SortByName ? L"Sort by Date\n" : L"Sort by Name\n";
                Items += m_AllFolders ? L"Show My Documents\n" : L"Show All Folders\n";
                Items += L"-\nAbout Office Mobile";
                ShowMenu(AppMenuRightSoftKey, 0.0f, 0.0f, Items.c_str());
            }
            break;

        case PageEdit:
            if (Key == ShellSoftKeyLeft)
            {
                m_DocFindMode = !m_DocFindMode;
                RefreshText();
                ShowInputPanel(m_DocFindMode);
            }
            else if (Key == ShellSoftKeyRight)
            {
                m_Menu = MenuEdit;
                ShowMenu(AppMenuRightSoftKey, 0.0f, 0.0f,
                         m_Doc.ReadOnly ? L"Save a Copy\nWord Count\n-\nClear Find" : L"Save\nWord Count\n-\nClear Find\n-\nUndo All Changes");
            }
            break;

        case PageSheet:
            if (Key == ShellSoftKeyLeft && !m_Doc.ReadOnly)
            {
                m_Editing = TRUE;
                m_Entry = m_Cells[m_Row][m_Column];
                RefreshSheet(FALSE);
                ShowInputPanel(TRUE);
            }
            else if (Key == ShellSoftKeyRight)
            {
                m_Menu = MenuSheet;
                ShowMenu(AppMenuRightSoftKey, 0.0f, 0.0f,
                         m_Doc.ReadOnly ? L"Save a Copy\n-\nGo to A1" : L"Save\nInsert SUM Above\nClear Cell\n-\nGo to A1");
            }
            break;

        case PageShow:
            ShowSlide((Key == ShellSoftKeyLeft) ? m_Slide - 1 : m_Slide + 1);
            break;
    }
}

VOID
COfficeApp::OnMenuPick(
    _In_ LONG Index)
{
    OFFICE_MENU Menu = m_Menu;
    PCWSTR Selected = Scene()->GetSelectedCommand();
    LONG Target = (Selected && _wcsnicmp(Selected, L"open:", 5) == 0) ? _wtol(Selected + 5) : -1;
    ULONG Words = 0;
    BOOL InWord = FALSE;

    m_Menu = MenuNone;
    if (Index < 0)
        return;

    switch (Menu)
    {
        case MenuList:
            if (m_Mode == ModeSlides)
                Index += 1;

            if (Index == 0)
                NewDocument();
            else if (Index == 1)
                (m_Mode == ModeSlides) ? Open(Target) : Delete(Target);
            else if (Index == 2)
            {
                m_SortByName = !m_SortByName;
                Scan();
                ShowList();
            }
            else if (Index == 3)
            {
                m_AllFolders = !m_AllFolders;
                Scan();
                ShowList();
            }
            else if (Index == 4)
            {
                Notify(L"ReactOS Office Mobile",
                       L"Writer, Sheets, Slides and Notes. Free as in freedom, light as in 240 by 320. "
                       L"No paper clips were harmed.");
            }
            break;

        case MenuHold:
            if (Index == 0)
                Open(m_HoldTarget);
            else
                Delete(m_HoldTarget);
            break;

        case MenuEdit:
            if (Index == 0)
            {
                if (m_Doc.ReadOnly)
                    SaveCopy();
                else if (Save())
                    Notify(L"Saved", m_Doc.Name.c_str());
            }
            else if (Index == 1)
            {
                for (WCHAR Char : m_Text)
                {
                    if (iswspace(Char))
                        InWord = FALSE;
                    else if (!InWord)
                    {
                        InWord = TRUE;
                        Words++;
                    }
                }
                Notify(L"Word Count", (std::to_wstring(Words) + L" words, " + std::to_wstring(m_Text.size()) + L" characters").c_str());
            }
            else if (Index == 2)
            {
                m_DocFind.clear();
                m_DocFindMode = FALSE;
                RefreshText();
            }
            else if (Index == 3)
            {
                m_ContentCache.clear();
                m_Text = Contents(m_Doc);
                m_Caret = m_Text.size();
                m_Dirty = FALSE;
                RefreshText();
            }
            break;

        case MenuSheet:
            if (m_Doc.ReadOnly)
            {
                if (Index == 0)
                    SaveCopy();
                else
                    SelectCell(0, 0);
                break;
            }

            if (Index == 0)
            {
                CommitCell();
                if (Save())
                    Notify(L"Saved", m_Doc.Name.c_str());
            }
            else if (Index == 1 && m_Row > 0)
            {
                /* Sum the numbers straight above, up to the first gap */
                LONG Top = m_Row - 1;

                while (Top > 0 && !m_Cells[Top - 1][m_Column].empty())
                    Top--;
                m_Cells[m_Row][m_Column] = L"=SUM(" + CellName(Top, m_Column) + L":" + CellName(m_Row - 1, m_Column) + L")";
                m_Dirty = TRUE;
                RefreshSheet(TRUE);
            }
            else if (Index == 2)
            {
                m_Cells[m_Row][m_Column].clear();
                m_Dirty = TRUE;
                RefreshSheet(TRUE);
            }
            else if (Index == 3)
            {
                SelectCell(0, 0);
                Scene()->ResetScroll(L"SheetScroll");
            }
            break;

        default:
            break;
    }
}

VOID
COfficeApp::OnCloseButton(VOID)
{
    /* ok saves and goes back to the list, like the device did without asking */
    switch (m_Page)
    {
        case PageList:
            RequestClose();
            return;

        case PageEdit:
            if (m_DocFindMode)
            {
                m_DocFindMode = FALSE;
                RefreshText();
                ShowInputPanel(FALSE);
                return;
            }
            Save();
            break;

        case PageSheet:
            CommitCell();
            Save();
            break;

        default:
            break;
    }

    ShowInputPanel(FALSE);
    Scan();
    ShowPage(PageList);
}

VOID
COfficeApp::OnChar(
    _In_ WCHAR Char)
{
    if (Char < 0x20)
        return;

    switch (m_Page)
    {
        case PageList:
            m_Find += Char;
            m_FindFocus = TRUE;
            Scene()->ResetScroll(L"ListScroll");
            ShowList();
            break;

        case PageEdit:
            if (m_DocFindMode)
            {
                m_DocFind += Char;
                RefreshText();
            }
            else
            {
                Edit(std::wstring(1, Char), 0);
            }
            break;

        case PageSheet:
            /* Typing replaces the cell, like the desktop */
            if (m_Doc.ReadOnly)
                break;
            if (!m_Editing)
            {
                m_Editing = TRUE;
                m_Entry.clear();
            }
            m_Entry += Char;
            RefreshSheet(FALSE);
            break;

        default:
            break;
    }
}

VOID
COfficeApp::OnKey(
    _In_ UINT VirtualKey)
{
    switch (m_Page)
    {
        case PageList:
            if (VirtualKey == VK_BACK && !m_Find.empty())
            {
                m_Find.pop_back();
                ShowList();
                Present();
                return;
            }
            break;

        case PageEdit:
            if (m_DocFindMode)
            {
                if (VirtualKey == VK_BACK && !m_DocFind.empty())
                    m_DocFind.pop_back();
                else if (VirtualKey == VK_RETURN)
                    m_DocFindMode = FALSE;
                RefreshText();
            }
            else if (VirtualKey == VK_BACK)
                Edit(L"", 1);
            else if (VirtualKey == VK_DELETE && m_Caret < m_Text.size() && !m_Doc.ReadOnly)
            {
                m_Text.erase(m_Caret, 1);
                m_Dirty = TRUE;
                RefreshText();
            }
            else if (VirtualKey == VK_RETURN)
                Edit(L"\n", 0);
            else if (VirtualKey == VK_LEFT && m_Caret > 0)
                m_Caret--, RefreshText();
            else if (VirtualKey == VK_RIGHT && m_Caret < m_Text.size())
                m_Caret++, RefreshText();
            else if (VirtualKey == VK_UP || VirtualKey == VK_DOWN)
            {
                MoveLine((VirtualKey == VK_UP) ? -1 : 1);
                RefreshText();
            }
            Present();
            return;

        case PageSheet:
            if (m_Editing && VirtualKey == VK_BACK)
            {
                if (!m_Entry.empty())
                    m_Entry.pop_back();
                RefreshSheet(FALSE);
            }
            else if (VirtualKey == VK_RETURN || VirtualKey == VK_DOWN)
                SelectCell(m_Row + 1, m_Column);
            else if (VirtualKey == VK_UP)
                SelectCell(m_Row - 1, m_Column);
            else if (VirtualKey == VK_LEFT && !m_Editing)
                SelectCell(m_Row, m_Column - 1);
            else if (VirtualKey == VK_RIGHT || VirtualKey == VK_TAB)
                SelectCell(m_Row, m_Column + 1);
            else if (VirtualKey == VK_DELETE && !m_Doc.ReadOnly)
            {
                m_Cells[m_Row][m_Column].clear();
                m_Dirty = TRUE;
                RefreshSheet(TRUE);
            }
            Present();
            return;

        case PageShow:
            if (VirtualKey == VK_LEFT || VirtualKey == VK_UP)
                ShowSlide(m_Slide - 1);
            else if (VirtualKey == VK_RIGHT || VirtualKey == VK_DOWN || VirtualKey == VK_RETURN || VirtualKey == VK_SPACE)
                ShowSlide(m_Slide + 1);
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
    COfficeApp App;
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
