/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Messaging: conversations over the phone service, one thread view and compose
 */

#include <appclient.h>
#include <records.h>
#include <shellapi.h>
#include <strsafe.h>
#include <telephony/phoneom.h>
#include <wincodec.h>
#include <algorithm>
#include <map>

/*
 * Markup contract (messaging.uix):
 *   Elements: PageThreads, PageThread, ServiceDown, ThreadsEmpty, ThreadEmpty, ToRow, ToHint, ComposeHint
 *   Repeaters: ThreadList (Id, Initial, Name, Time, Preview, Tone)
 *              MessageList (Id, Text, Detail, Side, Pad, Fill, Ink, Note, Picture, PictureWidth,
 *                           PictureHeight)
 *   Scrollers: ThreadsScroll, MessageScroll
 *   Bindings: ServiceText, ThreadName, ThreadNumber, ToText, ToFill, ComposeText, ComposeFill,
 *             ComposeCount, SendTone
 *   Commands: open:<n>, hold:<n>, bubble:<n>, bubblehold:<n>, to.focus, compose.focus, send
 */

/* Ids at or above this mark messages still waiting in the outbox */
#define OUTBOX_ID_BASE      0xF0000000UL
#define LINK_PERIOD_MS      5000
#define LINK_PAGE           16
#define PICTURE_MAX_WIDTH   220.0f
#define PICTURE_MAX_HEIGHT  280.0f

typedef struct _SMS_ITEM
{
    DWORD Id;
    DWORD Flags;
    FILETIME Time;
    std::wstring Address;
    std::wstring Text;
    std::wstring Picture;
    UINT PictureWidth = 0;
    UINT PictureHeight = 0;
} SMS_ITEM;

/* First picture of a multimedia message and its pixel size, empty when it has none */
typedef struct _SMS_PICTURE
{
    std::wstring Path;
    UINT Width = 0;
    UINT Height = 0;
} SMS_PICTURE;

typedef struct _SMS_SNAPSHOT
{
    BOOL ServiceUp = FALSE;
    HRESULT Error = S_OK;
    ULONG Generation = 0;
    std::vector<SMS_ITEM> Items;
} SMS_SNAPSHOT;

typedef enum _LINK_OP_KIND
{
    LinkSend,
    LinkMarkRead,
    LinkDelete
} LINK_OP_KIND;

typedef struct _LINK_OP
{
    LINK_OP_KIND Kind;
    DWORD Id;
    std::wstring Address;
    std::wstring Text;
} LINK_OP;

typedef VOID
(*PLINK_WAKE)(
    _In_opt_ PVOID Context);

/* Owns the phone service session on its own thread and wakes the app with snapshots */
class CSmsLink
{
public:
    ~CSmsLink()
    {
        Stop();
    }

    BOOL
    Start(
        _In_ PLINK_WAKE WakeRoutine,
        _In_opt_ PVOID Context)
    {
        if (m_Thread)
            return TRUE;

        m_Wake = WakeRoutine;
        m_WakeContext = Context;
        m_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        m_KickEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!m_StopEvent || !m_KickEvent)
            return FALSE;

        m_Thread = CreateThread(NULL, 0, ThreadProc, this, 0, NULL);
        return m_Thread != NULL;
    }

    VOID
    Stop(VOID)
    {
        if (m_Thread)
        {
            SetEvent(m_StopEvent);
            WaitForSingleObject(m_Thread, INFINITE);
            CloseHandle(m_Thread);
            m_Thread = NULL;
        }
        if (m_StopEvent)
            CloseHandle(m_StopEvent);
        if (m_KickEvent)
            CloseHandle(m_KickEvent);
        m_StopEvent = m_KickEvent = NULL;
    }

    VOID
    Queue(
        _In_ const LINK_OP &Op)
    {
        AcquireSRWLockExclusive(&m_Lock);
        m_Ops.push_back(Op);

        /* A send shows in the conversation right away, before the service has it */
        if (Op.Kind == LinkSend)
            m_Snapshot.Generation++;
        ReleaseSRWLockExclusive(&m_Lock);

        if (Op.Kind == LinkSend && m_Wake)
            m_Wake(m_WakeContext);
        Kick();
    }

    VOID
    Kick(VOID)
    {
        if (m_KickEvent)
            SetEvent(m_KickEvent);
    }

    /* Copies the snapshot when it changed since Generation, the outbox is folded in */
    BOOL
    Snapshot(
        _Inout_ SMS_SNAPSHOT &Out)
    {
        BOOL Changed;

        AcquireSRWLockShared(&m_Lock);
        Changed = (Out.Generation != m_Snapshot.Generation);
        if (Changed)
        {
            Out = m_Snapshot;
            for (SIZE_T Index = 0; Index < m_Ops.size(); Index++)
            {
                if (m_Ops[Index].Kind != LinkSend)
                    continue;

                SMS_ITEM Item;
                Item.Id = OUTBOX_ID_BASE + static_cast<DWORD>(Index);
                Item.Flags = 0;
                GetSystemTimeAsFileTime(&Item.Time);
                Item.Address = m_Ops[Index].Address;
                Item.Text = m_Ops[Index].Text;
                Out.Items.push_back(Item);
            }
        }
        ReleaseSRWLockShared(&m_Lock);
        return Changed;
    }

private:
    typedef HRESULT (WINAPI *PFN_OPEN)(PPHONEOM_EVENT_CALLBACK, PVOID, HPHONEOM *);
    typedef VOID (WINAPI *PFN_CLOSE)(HPHONEOM);
    typedef HRESULT (WINAPI *PFN_SEND)(HPHONEOM, LPCWSTR, LPCWSTR, PDWORD);
    typedef HRESULT (WINAPI *PFN_LIST)(HPHONEOM, DWORD, PPHONEOM_MESSAGE, DWORD, PDWORD);
    typedef HRESULT (WINAPI *PFN_ID)(HPHONEOM, DWORD);
    typedef HRESULT (WINAPI *PFN_ATTACHMENTS)(HPHONEOM, DWORD, PPHONEOM_ATTACHMENT, DWORD, PDWORD);

    static
    BOOL
    IsPicture(
        _In_z_ PCWSTR Path)
    {
        static PCWSTR const Extensions[] = { L".jpg", L".jpeg", L".png", L".gif", L".bmp" };
        PCWSTR Dot = wcsrchr(Path, L'.');

        if (!Dot)
            return FALSE;
        for (PCWSTR Extension : Extensions)
        {
            if (!_wcsicmp(Dot, Extension))
                return TRUE;
        }
        return FALSE;
    }

    /* Reads only the frame header, the bubble needs the aspect before the engine decodes it */
    VOID
    MeasurePicture(
        _Inout_ SMS_PICTURE &Picture)
    {
        IWICBitmapDecoder *Decoder = NULL;
        IWICBitmapFrameDecode *Frame = NULL;

        if (!m_Imaging &&
            FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&m_Imaging))))
        {
            return;
        }

        if (SUCCEEDED(m_Imaging->CreateDecoderFromFilename(Picture.Path.c_str(), NULL, GENERIC_READ,
                                                           WICDecodeMetadataCacheOnDemand, &Decoder)) &&
            SUCCEEDED(Decoder->GetFrame(0, &Frame)))
        {
            Frame->GetSize(&Picture.Width, &Picture.Height);
        }

        if (Frame)
            Frame->Release();
        if (Decoder)
            Decoder->Release();
    }

    /* Looks a message's picture up once, the service never changes the attachments of a stored message */
    const SMS_PICTURE &
    PictureFor(
        _In_ DWORD MessageId)
    {
        auto Found = m_Pictures.find(MessageId);
        PHONEOM_ATTACHMENT *Files;
        DWORD Count = 0;
        SMS_PICTURE Picture;

        if (Found != m_Pictures.end())
            return Found->second;

        Files = new PHONEOM_ATTACHMENT[PHONEOM_MAX_ATTACHMENTS];
        if (m_Attachments && SUCCEEDED(m_Attachments(m_Phone, MessageId, Files, PHONEOM_MAX_ATTACHMENTS, &Count)))
        {
            for (DWORD Index = 0; Index < Count && Index < PHONEOM_MAX_ATTACHMENTS; Index++)
            {
                Files[Index].Path[PHONEOM_MAX_PATH - 1] = UNICODE_NULL;
                if (!IsPicture(Files[Index].Path))
                    continue;

                Picture.Path = Files[Index].Path;
                MeasurePicture(Picture);
                break;
            }
        }
        delete[] Files;

        return m_Pictures[MessageId] = Picture;
    }

    static
    VOID
    CALLBACK
    OnPhoneEvent(
        _In_ HPHONEOM Phone,
        _In_ DWORD Event,
        _In_reads_bytes_opt_(DataSize) const VOID *Data,
        _In_ DWORD DataSize,
        _In_opt_ PVOID Context)
    {
        CSmsLink *This = static_cast<CSmsLink *>(Context);

        UNREFERENCED_PARAMETER(Phone);
        UNREFERENCED_PARAMETER(Data);
        UNREFERENCED_PARAMETER(DataSize);

        /* The session is closed on the link thread, never from its own callback */
        if (Event == PhoneEventDisconnected)
            InterlockedExchange(&This->m_Lost, TRUE);
        if (Event == PhoneEventMessage || Event == PhoneEventMessageStatus || Event == PhoneEventDisconnected)
            This->Kick();
    }

    BOOL
    Load(VOID)
    {
        if (m_Module)
            return TRUE;

        m_Module = LoadLibraryW(L"phoneom.dll");
        if (!m_Module)
            return FALSE;

        m_Open = reinterpret_cast<PFN_OPEN>(GetProcAddress(m_Module, "PhoneOmOpen"));
        m_Close = reinterpret_cast<PFN_CLOSE>(GetProcAddress(m_Module, "PhoneOmClose"));
        m_Send = reinterpret_cast<PFN_SEND>(GetProcAddress(m_Module, "PhoneOmSendMessage"));
        m_List = reinterpret_cast<PFN_LIST>(GetProcAddress(m_Module, "PhoneOmGetMessages"));
        m_MarkRead = reinterpret_cast<PFN_ID>(GetProcAddress(m_Module, "PhoneOmMarkMessageRead"));
        m_Delete = reinterpret_cast<PFN_ID>(GetProcAddress(m_Module, "PhoneOmDeleteMessage"));
        m_Attachments = reinterpret_cast<PFN_ATTACHMENTS>(GetProcAddress(m_Module, "PhoneOmGetAttachments"));

        if (!m_Open || !m_Close || !m_Send || !m_List || !m_MarkRead || !m_Delete)
        {
            FreeLibrary(m_Module);
            m_Module = NULL;
            SetLastError(ERROR_PROC_NOT_FOUND);
            return FALSE;
        }
        return TRUE;
    }

    /* Runs queued work oldest first, a send leaves the outbox only once the service answered */
    VOID
    Drain(VOID)
    {
        LINK_OP Op;

        for (;;)
        {
            AcquireSRWLockShared(&m_Lock);
            if (m_Ops.empty())
            {
                ReleaseSRWLockShared(&m_Lock);
                return;
            }
            Op = m_Ops.front();
            ReleaseSRWLockShared(&m_Lock);

            switch (Op.Kind)
            {
                case LinkSend:
                    m_Send(m_Phone, Op.Address.c_str(), Op.Text.c_str(), NULL);
                    break;

                case LinkMarkRead:
                    m_MarkRead(m_Phone, Op.Id);
                    break;

                case LinkDelete:
                    m_Delete(m_Phone, Op.Id);
                    break;
            }

            AcquireSRWLockExclusive(&m_Lock);
            m_Ops.erase(m_Ops.begin());
            ReleaseSRWLockExclusive(&m_Lock);
        }
    }

    VOID
    Poll(VOID)
    {
        SMS_SNAPSHOT Next;
        PHONEOM_MESSAGE *Page;
        DWORD First = 0, Count, Index;
        HRESULT hr = S_OK;

        if (InterlockedExchange(&m_Lost, FALSE) && m_Phone)
        {
            m_Close(m_Phone);
            m_Phone = NULL;
        }

        if (!Load())
            hr = HRESULT_FROM_WIN32(GetLastError());
        else if (!m_Phone && FAILED(hr = m_Open(OnPhoneEvent, this, &m_Phone)))
            m_Phone = NULL;

        if (m_Phone)
        {
            Next.ServiceUp = TRUE;
            Drain();

            Page = new PHONEOM_MESSAGE[LINK_PAGE];
            for (;;)
            {
                hr = m_List(m_Phone, First, Page, LINK_PAGE, &Count);
                if (FAILED(hr) || Count == 0)
                    break;

                for (Index = 0; Index < Count; Index++)
                {
                    SMS_ITEM Item;

                    Page[Index].Address[PHONEOM_MAX_NUMBER - 1] = UNICODE_NULL;
                    Page[Index].Text[PHONEOM_MAX_TEXT - 1] = UNICODE_NULL;
                    Item.Id = Page[Index].MessageId;
                    Item.Flags = Page[Index].Flags;
                    Item.Time = Page[Index].Timestamp;
                    Item.Address = Page[Index].Address;
                    Item.Text = Page[Index].Text;
                    if (Item.Flags & PHONEOM_MESSAGE_ATTACHMENTS)
                    {
                        const SMS_PICTURE &Picture = PictureFor(Item.Id);

                        Item.Picture = Picture.Path;
                        Item.PictureWidth = Picture.Width;
                        Item.PictureHeight = Picture.Height;
                    }
                    Next.Items.push_back(Item);
                }

                First = Page[Count - 1].MessageId + 1;
                if (Count < LINK_PAGE || First == 0)
                    break;
            }
            delete[] Page;
        }
        Next.Error = hr;

        AcquireSRWLockExclusive(&m_Lock);
        Next.Generation = m_Snapshot.Generation + 1;
        m_Snapshot = Next;
        ReleaseSRWLockExclusive(&m_Lock);

        m_Wake(m_WakeContext);
    }

    static
    DWORD
    WINAPI
    ThreadProc(
        _In_ LPVOID Parameter)
    {
        CSmsLink *This = static_cast<CSmsLink *>(Parameter);
        HANDLE Events[2] = { This->m_StopEvent, This->m_KickEvent };
        HRESULT ComStatus = CoInitializeEx(NULL, COINIT_MULTITHREADED);

        do
        {
            This->Poll();
        } while (WaitForMultipleObjects(ARRAYSIZE(Events), Events, FALSE, LINK_PERIOD_MS) != WAIT_OBJECT_0);

        if (This->m_Phone)
            This->m_Close(This->m_Phone);
        This->m_Phone = NULL;
        if (This->m_Module)
            FreeLibrary(This->m_Module);
        This->m_Module = NULL;
        if (This->m_Imaging)
            This->m_Imaging->Release();
        This->m_Imaging = NULL;
        This->m_Pictures.clear();
        if (SUCCEEDED(ComStatus))
            CoUninitialize();
        return 0;
    }

    PLINK_WAKE m_Wake = NULL;
    PVOID m_WakeContext = NULL;
    SRWLOCK m_Lock = SRWLOCK_INIT;
    HANDLE m_Thread = NULL;
    HANDLE m_StopEvent = NULL;
    HANDLE m_KickEvent = NULL;
    volatile LONG m_Lost = FALSE;
    SMS_SNAPSHOT m_Snapshot;
    std::vector<LINK_OP> m_Ops;

    HMODULE m_Module = NULL;
    HPHONEOM m_Phone = NULL;
    PFN_OPEN m_Open = NULL;
    PFN_CLOSE m_Close = NULL;
    PFN_SEND m_Send = NULL;
    PFN_LIST m_List = NULL;
    PFN_ID m_MarkRead = NULL;
    PFN_ID m_Delete = NULL;
    PFN_ATTACHMENTS m_Attachments = NULL;
    IWICImagingFactory *m_Imaging = NULL;
    std::map<DWORD, SMS_PICTURE> m_Pictures;
};

typedef struct _SMS_THREAD
{
    std::wstring Key;
    std::wstring Address;
    std::wstring Name;
    std::vector<SIZE_T> Items;
    BOOL Unread = FALSE;
} SMS_THREAD;

typedef enum _SMS_PAGE
{
    PageThreads,
    PageThread
} SMS_PAGE;

typedef enum _SMS_MENU
{
    MenuNone,
    MenuThreads,
    MenuThread,
    MenuThreadHold,
    MenuBubbleHold
} SMS_MENU;

typedef enum _SMS_FIELD
{
    FieldTo,
    FieldCompose
} SMS_FIELD;

/* Numbers match on their last digits so +1 425... and 425... land in one conversation */
static
std::wstring
NumberKey(
    _In_ const std::wstring &Number)
{
    std::wstring Digits;

    for (WCHAR Char : Number)
    {
        if (iswdigit(Char))
            Digits += Char;
    }

    if (Digits.empty())
        return Number;
    if (Digits.size() > 10)
        Digits.erase(0, Digits.size() - 10);
    return Digits;
}

/* GSM 7 bit fits 160 characters in one part, 153 per part once split; UCS-2 fits 70 and 67 */
static
std::wstring
PartCounter(
    _In_ const std::wstring &Text)
{
    static PCWSTR const Gsm =
        L"@\x00A3$\x00A5\x00E8\x00E9\x00F9\x00EC\x00F2\x00C7\n\x00D8\x00F8\r\x00C5\x00E5"
        L"\x0394_\x03A6\x0393\x039B\x03A9\x03A0\x03A8\x03A3\x0398\x039E\x00C6\x00E6\x00DF\x00C9"
        L" !\"#\x00A4%&'()*+,-./0123456789:;<=>?"
        L"\x00A1" L"ABCDEFGHIJKLMNOPQRSTUVWXYZ\x00C4\x00D6\x00D1\x00DC\x00A7\x00BF"
        L"abcdefghijklmnopqrstuvwxyz\x00E4\x00F6\x00F1\x00FC\x00E0^{}\\[~]|\x20AC";
    BOOL Wide = FALSE;
    SIZE_T Units = Text.size();
    SIZE_T Single, Multi, Parts;
    WCHAR Buffer[32];

    for (WCHAR Char : Text)
    {
        if (!wcschr(Gsm, Char))
        {
            Wide = TRUE;
            break;
        }
    }

    Single = Wide ? 70 : 160;
    Multi = Wide ? 67 : 153;

    /* Only show the counter once the message gets close to splitting, like the device */
    if (Units < Single - 20)
        return std::wstring();

    Parts = (Units <= Single) ? 1 : (Units + Multi - 1) / Multi;
    StringCchPrintfW(Buffer, ARRAYSIZE(Buffer), L"%Iu/%Iu (%Iu)",
                     Units, (Parts == 1) ? Single : Parts * Multi, Parts);
    return Buffer;
}

class CMessagingApp : public CAppClient
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

    VOID OnWake(VOID) override;

private:
    static
    VOID
    LinkWake(
        _In_opt_ PVOID Context)
    {
        static_cast<CMessagingApp *>(Context)->Wake();
    }

    VOID
    LoadNames(VOID);

    std::wstring
    NameFor(
        _In_ const std::wstring &Address) const;

    VOID
    BuildThreads(VOID);

    LONG
    FindThread(
        _In_ const std::wstring &Key) const;

    VOID
    ShowPage(
        _In_ SMS_PAGE Page);

    VOID
    RefreshThreads(VOID);

    VOID
    RefreshThread(
        _In_ BOOL ScrollToEnd);

    VOID
    RefreshCompose(VOID);

    VOID
    OpenThread(
        _In_ LONG Index);

    VOID
    StartNew(
        _In_opt_z_ PCWSTR Address);

    VOID
    MarkThreadRead(VOID);

    VOID
    SendCompose(VOID);

    VOID
    DeleteThread(
        _In_ LONG Index);

    VOID
    DeleteItem(
        _In_ SIZE_T Item);

    VOID
    Retry(
        _In_ SIZE_T Item);

    LONG
    ItemFromCommand(
        _In_z_ PCWSTR Number) const;

    std::wstring *
    FocusedText(VOID);

    CSmsLink m_Link;
    SMS_SNAPSHOT m_Snapshot;
    std::vector<SMS_THREAD> m_Threads;
    std::vector<std::pair<std::wstring, std::wstring>> m_Names;
    SMS_PAGE m_Page = PageThreads;
    SMS_MENU m_Menu = MenuNone;
    SMS_FIELD m_Field = FieldCompose;
    std::wstring m_Address;
    std::wstring m_To;
    std::wstring m_Compose;
    LONG m_HoldThread = -1;
    LONG m_HoldItem = -1;
    DWORD m_SeenId = 0;
    SIZE_T m_ShownCount = 0;
    BOOL m_Started = FALSE;
    BOOL m_Primed = FALSE;
};

static
std::wstring
FormatWhen(
    _In_ const FILETIME &Utc,
    _In_ BOOL Short)
{
    FILETIME Local;
    SYSTEMTIME When, Now;
    WCHAR Time[32] = L"";
    WCHAR Date[64] = L"";

    if (!Utc.dwLowDateTime && !Utc.dwHighDateTime)
        return std::wstring();

    FileTimeToLocalFileTime(&Utc, &Local);
    FileTimeToSystemTime(&Local, &When);
    GetLocalTime(&Now);

    GetTimeFormatEx(NULL, TIME_NOSECONDS, &When, NULL, Time, ARRAYSIZE(Time));
    if (When.wYear == Now.wYear && When.wMonth == Now.wMonth && When.wDay == Now.wDay)
        return Time;

    GetDateFormatEx(NULL, DATE_SHORTDATE, &When, NULL, Date, ARRAYSIZE(Date), NULL);
    if (Short)
        return Date;
    return std::wstring(Date) + L" " + Time;
}

VOID
CMessagingApp::LoadNames(VOID)
{
    std::vector<APP_RECORD> Records;

    /* People keeps name and mobile number as its first two fields */
    m_Names.clear();
    if (!LoadRecords(L"contacts.txt", Records))
        return;

    for (const APP_RECORD &Record : Records)
    {
        if (Record.size() >= 2 && !Record[0].empty() && !Record[1].empty())
            m_Names.emplace_back(NumberKey(Record[1]), Record[0]);
    }
}

std::wstring
CMessagingApp::NameFor(
    _In_ const std::wstring &Address) const
{
    std::wstring Key = NumberKey(Address);

    for (const auto &Entry : m_Names)
    {
        if (Entry.first == Key)
            return Entry.second;
    }
    return Address;
}

LONG
CMessagingApp::FindThread(
    _In_ const std::wstring &Key) const
{
    for (LONG Index = 0; Index < static_cast<LONG>(m_Threads.size()); Index++)
    {
        if (m_Threads[Index].Key == Key)
            return Index;
    }
    return -1;
}

VOID
CMessagingApp::BuildThreads(VOID)
{
    std::vector<SMS_ITEM> &Items = m_Snapshot.Items;

    std::stable_sort(Items.begin(), Items.end(), [](const SMS_ITEM &A, const SMS_ITEM &B)
    {
        return CompareFileTime(&A.Time, &B.Time) < 0;
    });

    m_Threads.clear();
    for (SIZE_T Index = 0; Index < Items.size(); Index++)
    {
        std::wstring Key = NumberKey(Items[Index].Address);
        LONG Thread = FindThread(Key);

        if (Thread < 0)
        {
            SMS_THREAD New;

            New.Key = Key;
            New.Address = Items[Index].Address;
            New.Name = NameFor(Items[Index].Address);
            m_Threads.push_back(New);
            Thread = static_cast<LONG>(m_Threads.size()) - 1;
        }

        m_Threads[Thread].Items.push_back(Index);
        if ((Items[Index].Flags & PHONEOM_MESSAGE_INCOMING) && !(Items[Index].Flags & PHONEOM_MESSAGE_READ))
            m_Threads[Thread].Unread = TRUE;
    }

    /* Newest conversation first */
    std::stable_sort(m_Threads.begin(), m_Threads.end(), [&Items](const SMS_THREAD &A, const SMS_THREAD &B)
    {
        return CompareFileTime(&Items[A.Items.back()].Time, &Items[B.Items.back()].Time) > 0;
    });
}

VOID
CMessagingApp::ShowPage(
    _In_ SMS_PAGE Page)
{
    m_Page = Page;
    Scene()->SetVisible(L"PageThreads", Page == PageThreads);
    Scene()->SetVisible(L"PageThread", Page == PageThread);
    Scene()->SetSelected(NULL);

    if (Page == PageThreads)
    {
        ShowInputPanel(FALSE);
        RefreshThreads();
        SetChrome(L"Messaging", L"New", L"Menu", ShellCloseOk);
    }
    else
    {
        RefreshThread(TRUE);
        SetChrome(L"Messaging", L"Send", L"Menu", ShellCloseOk);
    }
}

VOID
CMessagingApp::RefreshThreads(VOID)
{
    static PCWSTR const Fields[] = { L"Id", L"Initial", L"Glyph", L"Name", L"Time", L"Preview", L"Tone" };
    std::vector<std::wstring> Storage;
    std::vector<PCWSTR> Values;
    WCHAR Id[16];

    for (LONG Index = 0; Index < static_cast<LONG>(m_Threads.size()); Index++)
    {
        const SMS_THREAD &Thread = m_Threads[Index];
        const SMS_ITEM &Last = m_Snapshot.Items[Thread.Items.back()];
        std::wstring Preview = Last.Text;
        std::wstring Initial;
        std::wstring Glyph;

        if (Preview.empty() && !Last.Picture.empty())
            Preview = L"Picture";

        std::replace(Preview.begin(), Preview.end(), L'\n', L' ');
        if (Last.Flags & PHONEOM_MESSAGE_FAILED)
            Preview = L"Not sent: " + Preview;

        /* A saved name shows its initial, a bare number shows the person glyph */
        if (Thread.Name != Thread.Address && !Thread.Name.empty() && iswalpha(Thread.Name[0]))
            Initial.assign(1, towupper(Thread.Name[0]));
        else
            Glyph = L"\xE77B";

        StringCchPrintfW(Id, ARRAYSIZE(Id), L"%ld", Index);
        Storage.push_back(Id);
        Storage.push_back(Initial);
        Storage.push_back(Glyph);
        Storage.push_back(Thread.Name);
        Storage.push_back(FormatWhen(Last.Time, TRUE));
        Storage.push_back(Preview);
        Storage.push_back(Thread.Unread ? L"@AccentText" : L"@AppSubText");
    }

    for (const std::wstring &Value : Storage)
        Values.push_back(Value.c_str());

    Scene()->SetItems(L"ThreadList", static_cast<ULONG>(m_Threads.size()), ARRAYSIZE(Fields), Fields, Values.data());
    Scene()->SetVisible(L"ThreadsEmpty", m_Threads.empty() && m_Snapshot.ServiceUp);

    if (!m_Snapshot.ServiceUp)
    {
        Scene()->SetBinding(L"ServiceText", m_Primed ?
                            L"The phone service isn't running, so messages can't be sent or received right now." :
                            L"Connecting to the phone service...");
    }
    Scene()->SetVisible(L"ServiceDown", !m_Snapshot.ServiceUp);
}

VOID
CMessagingApp::RefreshThread(
    _In_ BOOL ScrollToEnd)
{
    static PCWSTR const Fields[] = { L"Id", L"Text", L"Detail", L"Side", L"Pad", L"Fill", L"Ink", L"Note",
                                     L"Picture", L"PictureWidth", L"PictureHeight" };
    std::vector<std::wstring> Storage;
    std::vector<PCWSTR> Values;
    LONG Thread = FindThread(NumberKey(m_Address));
    SIZE_T Count = 0;
    WCHAR Id[16];

    if (Thread >= 0 && !m_Address.empty())
    {
        for (SIZE_T Item : m_Threads[Thread].Items)
        {
            const SMS_ITEM &Message = m_Snapshot.Items[Item];
            BOOL Incoming = (Message.Flags & PHONEOM_MESSAGE_INCOMING) != 0;
            std::wstring Detail;
            FLOAT Fit = 0.0f;
            WCHAR Width[16];
            WCHAR Height[16];

            if (Message.Id >= OUTBOX_ID_BASE || (!Incoming && !(Message.Flags & (PHONEOM_MESSAGE_SENT | PHONEOM_MESSAGE_FAILED))))
                Detail = L"Sending...";
            else if (Message.Flags & PHONEOM_MESSAGE_FAILED)
                Detail = L"Not sent. Tap to try again.";
            else
                Detail = FormatWhen(Message.Time, FALSE);

            StringCchPrintfW(Id, ARRAYSIZE(Id), L"%Iu", Item);
            Storage.push_back(Id);
            Storage.push_back(Message.Text);
            Storage.push_back(Detail);
            Storage.push_back(Incoming ? L"Left" : L"Right");
            Storage.push_back(Incoming ? L"12,4,64,6" : L"64,4,12,6");
            Storage.push_back(Incoming ? L"@ChromeMediumLow" : L"@Accent");
            Storage.push_back(Incoming ? L"@AppText" : L"@BaseWhite");
            Storage.push_back((Message.Flags & PHONEOM_MESSAGE_FAILED) ? L"@AccentText" : L"@AppSubText");

            /* The picture keeps its aspect inside the bubble and never grows past its own size */
            if (Message.PictureWidth && Message.PictureHeight)
            {
                Fit = min(PICTURE_MAX_WIDTH / Message.PictureWidth, PICTURE_MAX_HEIGHT / Message.PictureHeight);
                Fit = min(Fit, 1.0f);
            }
            StringCchPrintfW(Width, ARRAYSIZE(Width), L"%.0f", Message.PictureWidth * Fit);
            StringCchPrintfW(Height, ARRAYSIZE(Height), L"%.0f", Message.PictureHeight * Fit);
            Storage.push_back(Fit > 0.0f ? L"file:" + Message.Picture : std::wstring());
            Storage.push_back(Width);
            Storage.push_back(Height);
            Count++;
        }
    }

    for (const std::wstring &Value : Storage)
        Values.push_back(Value.c_str());

    Scene()->SetItems(L"MessageList", static_cast<ULONG>(Count), ARRAYSIZE(Fields), Fields, Values.data());
    Scene()->SetVisible(L"ThreadEmpty", Count == 0);
    Scene()->SetVisible(L"ToRow", m_Address.empty());

    if (m_Address.empty())
    {
        Scene()->SetBinding(L"ThreadName", L"New message");
        Scene()->SetBinding(L"ThreadNumber", L"");
    }
    else
    {
        std::wstring Name = NameFor(m_Address);

        Scene()->SetBinding(L"ThreadName", Name.c_str());
        Scene()->SetBinding(L"ThreadNumber", (Name != m_Address) ? m_Address.c_str() : L"");
    }

    RefreshCompose();

    /* New bubbles bring the view to the newest message */
    if (ScrollToEnd || Count != m_ShownCount)
        Scene()->ScrollTo(L"MessageScroll", 1.0e9f);
    m_ShownCount = Count;
}

VOID
CMessagingApp::RefreshCompose(VOID)
{
    BOOL CanSend = !m_Compose.empty() && !(m_Address.empty() && m_To.empty());

    Scene()->SetBinding(L"ToText", (m_To + ((m_Field == FieldTo) ? L"|" : L"")).c_str());
    Scene()->SetBinding(L"ToFill", (m_Field == FieldTo) ? L"@FieldActive" : L"@FieldNormal");
    Scene()->SetVisible(L"ToHint", m_To.empty() && m_Field != FieldTo);

    Scene()->SetBinding(L"ComposeText", (m_Compose + ((m_Field == FieldCompose) ? L"|" : L"")).c_str());
    Scene()->SetBinding(L"ComposeFill", (m_Field == FieldCompose) ? L"@FieldActive" : L"@FieldNormal");
    Scene()->SetVisible(L"ComposeHint", m_Compose.empty() && m_Field != FieldCompose);
    Scene()->SetBinding(L"ComposeCount", PartCounter(m_Compose).c_str());
    Scene()->SetBinding(L"SendTone", CanSend ? L"@AccentText" : L"@AppSubText");
}

VOID
CMessagingApp::MarkThreadRead(VOID)
{
    LONG Thread = FindThread(NumberKey(m_Address));
    LINK_OP Op;

    if (Thread < 0 || m_Address.empty())
        return;

    for (SIZE_T Item : m_Threads[Thread].Items)
    {
        SMS_ITEM &Message = m_Snapshot.Items[Item];

        if ((Message.Flags & PHONEOM_MESSAGE_INCOMING) && !(Message.Flags & PHONEOM_MESSAGE_READ))
        {
            Message.Flags |= PHONEOM_MESSAGE_READ;
            Op.Kind = LinkMarkRead;
            Op.Id = Message.Id;
            m_Link.Queue(Op);
        }
    }
    m_Threads[Thread].Unread = FALSE;
}

VOID
CMessagingApp::OpenThread(
    _In_ LONG Index)
{
    if (Index < 0 || Index >= static_cast<LONG>(m_Threads.size()))
        return;

    m_Address = m_Threads[Index].Address;
    m_To.clear();
    m_Field = FieldCompose;
    MarkThreadRead();
    ShowPage(PageThread);
}

VOID
CMessagingApp::StartNew(
    _In_opt_z_ PCWSTR Address)
{
    LONG Thread;

    m_To.clear();
    m_Compose.clear();
    m_Address = (Address && Address[0]) ? Address : L"";

    /* A number that already has a conversation opens that one */
    Thread = m_Address.empty() ? -1 : FindThread(NumberKey(m_Address));
    if (Thread >= 0)
        m_Address = m_Threads[Thread].Address;

    m_Field = m_Address.empty() ? FieldTo : FieldCompose;
    MarkThreadRead();
    ShowPage(PageThread);
    ShowInputPanel(TRUE);
}

VOID
CMessagingApp::SendCompose(VOID)
{
    LINK_OP Op;

    if (m_Address.empty())
    {
        if (m_To.empty())
        {
            m_Field = FieldTo;
            RefreshCompose();
            return;
        }
        m_Address = m_To;
        m_To.clear();
    }

    if (m_Compose.empty())
        return;

    Op.Kind = LinkSend;
    Op.Id = 0;
    Op.Address = m_Address;
    Op.Text = m_Compose;
    m_Link.Queue(Op);

    m_Compose.clear();
    m_Field = FieldCompose;
    RefreshThread(TRUE);
}

VOID
CMessagingApp::DeleteThread(
    _In_ LONG Index)
{
    LINK_OP Op;

    if (Index < 0 || Index >= static_cast<LONG>(m_Threads.size()))
        return;

    Op.Kind = LinkDelete;
    for (SIZE_T Item : m_Threads[Index].Items)
    {
        if (m_Snapshot.Items[Item].Id >= OUTBOX_ID_BASE)
            continue;
        Op.Id = m_Snapshot.Items[Item].Id;
        m_Link.Queue(Op);
    }

    if (m_Page == PageThread)
        ShowPage(PageThreads);
}

VOID
CMessagingApp::DeleteItem(
    _In_ SIZE_T Item)
{
    LINK_OP Op;

    if (Item >= m_Snapshot.Items.size() || m_Snapshot.Items[Item].Id >= OUTBOX_ID_BASE)
        return;

    Op.Kind = LinkDelete;
    Op.Id = m_Snapshot.Items[Item].Id;
    m_Link.Queue(Op);
}

VOID
CMessagingApp::Retry(
    _In_ SIZE_T Item)
{
    LINK_OP Op;

    if (Item >= m_Snapshot.Items.size() || !(m_Snapshot.Items[Item].Flags & PHONEOM_MESSAGE_FAILED))
        return;

    /* The failed copy goes away and the text is sent again as a new message */
    Op.Kind = LinkDelete;
    Op.Id = m_Snapshot.Items[Item].Id;
    m_Link.Queue(Op);

    Op.Kind = LinkSend;
    Op.Address = m_Snapshot.Items[Item].Address;
    Op.Text = m_Snapshot.Items[Item].Text;
    m_Link.Queue(Op);
}

LONG
CMessagingApp::ItemFromCommand(
    _In_z_ PCWSTR Number) const
{
    LONG Item = _wtol(Number);

    if (Item < 0 || Item >= static_cast<LONG>(m_Snapshot.Items.size()))
        return -1;
    return Item;
}

std::wstring *
CMessagingApp::FocusedText(VOID)
{
    if (m_Page != PageThread)
        return NULL;
    if (m_Field == FieldTo && m_Address.empty())
        return &m_To;
    return &m_Compose;
}

VOID
CMessagingApp::OnActivate(VOID)
{
    PCWSTR To = Argument(L"to");

    LoadNames();
    if (!m_Started)
        m_Started = m_Link.Start(LinkWake, this);

    if (To && !m_Primed)
        StartNew(To);
    else
        ShowPage(m_Page);
}

VOID
CMessagingApp::OnWake(VOID)
{
    BOOL First = !m_Primed;
    std::wstring Notice, NoticeFrom;

    if (!m_Link.Snapshot(m_Snapshot))
        return;
    m_Primed = TRUE;

    /* Messages that arrived while the app is open and not looking at their conversation */
    for (const SMS_ITEM &Item : m_Snapshot.Items)
    {
        if (Item.Id >= OUTBOX_ID_BASE || Item.Id <= m_SeenId)
            continue;

        if (!First && (Item.Flags & PHONEOM_MESSAGE_INCOMING) && !(Item.Flags & PHONEOM_MESSAGE_READ) &&
            !(m_Page == PageThread && NumberKey(Item.Address) == NumberKey(m_Address)))
        {
            NoticeFrom = Item.Address;
            Notice = Item.Text;
        }
        m_SeenId = max(m_SeenId, Item.Id);
    }

    BuildThreads();
    if (m_Page == PageThread)
    {
        MarkThreadRead();
        RefreshThread(FALSE);
    }
    else
    {
        RefreshThreads();
    }

    if (!Notice.empty())
        Notify(NameFor(NoticeFrom).c_str(), Notice.c_str());
}

VOID
CMessagingApp::OnCommand(
    _In_z_ PCWSTR Command)
{
    if (_wcsnicmp(Command, L"open:", 5) == 0)
    {
        OpenThread(_wtol(Command + 5));
    }
    else if (_wcsicmp(Command, L"to.focus") == 0)
    {
        m_Field = FieldTo;
        RefreshCompose();
        ShowInputPanel(TRUE);
    }
    else if (_wcsicmp(Command, L"compose.focus") == 0)
    {
        m_Field = FieldCompose;
        RefreshCompose();
        ShowInputPanel(TRUE);
    }
    else if (_wcsicmp(Command, L"send") == 0)
    {
        SendCompose();
    }
    else if (_wcsnicmp(Command, L"bubble:", 7) == 0)
    {
        LONG Item = ItemFromCommand(Command + 7);

        if (Item >= 0 && (m_Snapshot.Items[Item].Flags & PHONEOM_MESSAGE_FAILED))
            Retry(Item);
    }
    else
    {
        CAppClient::OnCommand(Command);
    }
}

VOID
CMessagingApp::OnHold(
    _In_z_ PCWSTR Command,
    _In_ FLOAT X,
    _In_ FLOAT Y)
{
    if (_wcsnicmp(Command, L"hold:", 5) == 0)
    {
        m_HoldThread = _wtol(Command + 5);
        m_Menu = MenuThreadHold;
        ShowMenu(AppMenuPoint, X, Y, L"Delete conversation");
    }
    else if (_wcsnicmp(Command, L"bubblehold:", 11) == 0)
    {
        m_HoldItem = ItemFromCommand(Command + 11);
        if (m_HoldItem < 0)
            return;

        m_Menu = MenuBubbleHold;
        ShowMenu(AppMenuPoint, X, Y, (m_Snapshot.Items[m_HoldItem].Flags & PHONEOM_MESSAGE_FAILED) ?
                 L"Try again\nDelete" : L"Delete");
    }
}

VOID
CMessagingApp::OnSoftKey(
    _In_ SHELL_SOFTKEY Key)
{
    if (m_Page == PageThreads)
    {
        if (Key == ShellSoftKeyLeft)
        {
            StartNew(NULL);
        }
        else if (Key == ShellSoftKeyRight)
        {
            m_Menu = MenuThreads;
            ShowMenu(AppMenuRightSoftKey, 0.0f, 0.0f, L"New message\nDelete conversation\n-\nRefresh");
        }
        return;
    }

    if (Key == ShellSoftKeyLeft)
    {
        SendCompose();
    }
    else if (Key == ShellSoftKeyRight)
    {
        m_Menu = MenuThread;
        ShowMenu(AppMenuRightSoftKey, 0.0f, 0.0f, L"Delete conversation\n-\nAll conversations");
    }
}

VOID
CMessagingApp::OnCloseButton(VOID)
{
    if (m_Page == PageThread)
        ShowPage(PageThreads);
    else
        RequestClose();
}

VOID
CMessagingApp::OnMenuPick(
    _In_ LONG Index)
{
    SMS_MENU Menu = m_Menu;
    PCWSTR Command = Scene()->GetSelectedCommand();
    LONG Selected = -1;

    m_Menu = MenuNone;
    if (Index < 0)
        return;

    if (Command && _wcsnicmp(Command, L"open:", 5) == 0)
        Selected = _wtol(Command + 5);

    switch (Menu)
    {
        case MenuThreads:
            if (Index == 0)
                StartNew(NULL);
            else if (Index == 1)
                DeleteThread(Selected);
            else if (Index == 2)
                m_Link.Kick();
            break;

        case MenuThread:
            if (Index == 0)
                DeleteThread(FindThread(NumberKey(m_Address)));
            else if (Index == 1)
                ShowPage(PageThreads);
            break;

        case MenuThreadHold:
            if (Index == 0)
                DeleteThread(m_HoldThread);
            break;

        case MenuBubbleHold:
            if (m_HoldItem < 0)
                break;
            if ((m_Snapshot.Items[m_HoldItem].Flags & PHONEOM_MESSAGE_FAILED) && Index == 0)
                Retry(m_HoldItem);
            else
                DeleteItem(m_HoldItem);
            break;

        default:
            break;
    }
}

VOID
CMessagingApp::OnChar(
    _In_ WCHAR Char)
{
    std::wstring *Text = FocusedText();

    if (!Text)
        return;

    /* Enter in the To box moves on to the message, in the message it starts a new line */
    if (Char == L'\r')
    {
        if (Text == &m_To)
            m_Field = FieldCompose;
        else
            *Text += L'\n';
        RefreshCompose();
        return;
    }

    if (Char < 0x20 || Text->size() >= PHONEOM_MAX_TEXT - 1)
        return;
    if (Text == &m_To && Text->size() >= PHONEOM_MAX_NUMBER - 1)
        return;

    *Text += Char;
    RefreshCompose();
}

VOID
CMessagingApp::OnKey(
    _In_ UINT VirtualKey)
{
    std::wstring *Text = FocusedText();

    if (VirtualKey == VK_BACK && Text)
    {
        if (!Text->empty())
            Text->pop_back();
        RefreshCompose();
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
    CMessagingApp App;
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
