/*
 * PROJECT:     ReactOS Messages
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Text messaging on top of the phone service
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <wingdi.h>
#include <stdlib.h>
#include <strsafe.h>
#include <wchar.h>
#include <objbase.h>
#include <commdlg.h>
#include <gdiplus.h>

#include <telephony/phoneom.h>

#define WM_PHONE_EVENT              (WM_APP + 1)
#define MG_RETRY_TIMER              1
#define MG_RETRY_MS                 3000

/* Dark theme shared with the mobile shell */
#define MG_BG                       RGB(0x12, 0x12, 0x14)
#define MG_TILE                     RGB(0x24, 0x25, 0x2A)
#define MG_ACCENT                   RGB(0x3A, 0x96, 0xDD)
#define MG_TEXT                     RGB(0xF2, 0xF2, 0xF4)
#define MG_SUBTEXT                  RGB(0x9A, 0x9B, 0xA3)
#define MG_FIELD                    RGB(0x1A, 0x1A, 0x1E)
#define MG_FAILED                   RGB(0xC8, 0x3C, 0x3C)
#define MG_TILE_ARGB                0xFF24252A

/* Touch sized layout in pixels, the top row stays clear of the camera cutout */
#define MG_MARGIN                   28
#define MG_SAFE_TOP                 120
#define MG_GAP                      16
#define MG_ROW                      88
#define MG_HEAD_H                   72
#define MG_ITEM_H                   132
#define MG_BUBBLE_PAD               20
#define MG_BUBBLE_WIDTH_PERCENT     72
#define MG_THUMB_MAX_H              120
#define MG_ATTACH_LINE_H            34
#define MG_ITEM_MAX_H               255

enum
{
    IDC_LIST = 100,
    IDC_NEW,
    IDC_EXIT,
    IDC_BACK,
    IDC_TO,
    IDC_THREAD,
    IDC_COMPOSE,
    IDC_SEND,
    IDC_DELETE,
    IDC_STATUS,
    IDC_ATTACH,
    IDC_VIEWER
};

typedef struct _MG_EVENT
{
    DWORD Event;
    DWORD Size;
    BYTE Data[ANYSIZE_ARRAY];
} MG_EVENT, *PMG_EVENT;

/* One conversation, the messages exchanged with one address */
typedef struct _MG_THREAD
{
    WCHAR Address[PHONEOM_MAX_NUMBER];
    DWORD Last;
    DWORD Unread;
    DWORD Count;
} MG_THREAD, *PMG_THREAD;

static HINSTANCE MgInstance;
static HWND MgWindow;
static HPHONEOM MgPhone;
static HFONT MgFont;
static HFONT MgSmallFont;
static HFONT MgHeadFont;
static HFONT MgButtonFont;
static HBRUSH MgBgBrush;
static HBRUSH MgFieldBrush;

static PPHONEOM_MESSAGE MgMessages;
static DWORD MgMessageCount;
static DWORD MgMessageCapacity;
static PMG_THREAD MgThreads;
static DWORD MgThreadCount;

/* Attachments of one multimedia message, loaded the first time it is drawn */
typedef struct _MG_MEDIA
{
    DWORD MessageId;
    DWORD Count;
    HBITMAP Thumb;
    SIZE ThumbSize;
    WCHAR Picture[PHONEOM_MAX_PATH];
    WCHAR Summary[128];
} MG_MEDIA, *PMG_MEDIA;

static PMG_MEDIA MgMedia;
static DWORD MgMediaCount;
static DWORD MgMediaCapacity;
static ULONG_PTR MgGdiplusToken;
static WNDPROC MgThreadListProc;
static WCHAR MgAttachPath[PHONEOM_MAX_PATH];

/* The conversation on screen, empty while the list is shown */
static BOOL MgInThread;
static BOOL MgComposingNew;
static WCHAR MgAddress[PHONEOM_MAX_NUMBER];

static
VOID
MgFillRound(
    _In_ HDC Dc,
    _In_ const RECT *Rect,
    _In_ int Radius,
    _In_ COLORREF Color)
{
    HBRUSH Brush = CreateSolidBrush(Color);
    HGDIOBJ OldBrush = SelectObject(Dc, Brush);
    HGDIOBJ OldPen = SelectObject(Dc, GetStockObject(NULL_PEN));

    RoundRect(Dc, Rect->left, Rect->top, Rect->right + 1, Rect->bottom + 1, Radius, Radius);

    SelectObject(Dc, OldPen);
    SelectObject(Dc, OldBrush);
    DeleteObject(Brush);
}

static
VOID
MgSetStatus(
    _In_ LPCWSTR Format,
    ...)
{
    WCHAR Text[256];
    va_list Args;

    va_start(Args, Format);
    StringCchVPrintfW(Text, _countof(Text), Format, Args);
    va_end(Args);

    SetDlgItemTextW(MgWindow, IDC_STATUS, Text);
}

/* Numbers match when their digits do, so +1 555 0100 and 15550100 are one conversation */
static
BOOL
MgSameAddress(
    _In_ LPCWSTR A,
    _In_ LPCWSTR B)
{
    for (;;)
    {
        while (*A && !iswdigit(*A))
            A++;
        while (*B && !iswdigit(*B))
            B++;
        if (*A != *B)
            return FALSE;
        if (*A == UNICODE_NULL)
            return TRUE;
        A++;
        B++;
    }
}

static
LONG
MgCompareTime(
    _In_ const FILETIME *A,
    _In_ const FILETIME *B)
{
    return CompareFileTime(A, B);
}

static
VOID
MgFormatTime(
    _In_ const FILETIME *Time,
    _Out_writes_(Count) LPWSTR Text,
    _In_ DWORD Count)
{
    FILETIME Local;
    SYSTEMTIME System;

    Text[0] = UNICODE_NULL;
    if (!Time->dwLowDateTime && !Time->dwHighDateTime)
        return;

    if (!FileTimeToLocalFileTime(Time, &Local) || !FileTimeToSystemTime(&Local, &System))
        return;

    StringCchPrintfW(Text, Count, L"%u/%u %02u:%02u", System.wMonth, System.wDay, System.wHour, System.wMinute);
}

static
BOOL
MgIsPicture(
    _In_z_ LPCWSTR Path)
{
    static const LPCWSTR Extensions[] = { L".jpg", L".jpeg", L".png", L".gif", L".bmp" };
    LPCWSTR Dot = wcsrchr(Path, L'.');
    DWORD i;

    for (i = 0; Dot && i < _countof(Extensions); i++)
    {
        if (!_wcsicmp(Dot, Extensions[i]))
            return TRUE;
    }

    return FALSE;
}

/* Scales a picture file to fit the box, keeping its aspect */
static
HBITMAP
MgLoadPicture(
    _In_z_ LPCWSTR Path,
    _In_ int MaxWidth,
    _In_ int MaxHeight,
    _Out_ SIZE *Size)
{
    GpBitmap *Bitmap = NULL;
    GpImage *Scaled = NULL;
    HBITMAP Result = NULL;
    UINT Width = 0, Height = 0;

    Size->cx = Size->cy = 0;
    if (!MgGdiplusToken || MaxWidth <= 0 || MaxHeight <= 0)
        return NULL;

    if (GdipCreateBitmapFromFile(Path, &Bitmap) != Ok)
        return NULL;

    GdipGetImageWidth((GpImage *)Bitmap, &Width);
    GdipGetImageHeight((GpImage *)Bitmap, &Height);
    if (Width && Height)
    {
        Size->cx = MaxWidth;
        Size->cy = (LONG)((ULONGLONG)Height * MaxWidth / Width);
        if (Size->cy > MaxHeight)
        {
            Size->cy = MaxHeight;
            Size->cx = (LONG)((ULONGLONG)Width * MaxHeight / Height);
        }
        Size->cx = max(Size->cx, 1);
        Size->cy = max(Size->cy, 1);

        if (GdipGetImageThumbnail((GpImage *)Bitmap, Size->cx, Size->cy, &Scaled, NULL, NULL) == Ok)
        {
            GdipCreateHBITMAPFromBitmap((GpBitmap *)Scaled, &Result, MG_TILE_ARGB);
            GdipDisposeImage(Scaled);
        }
    }

    GdipDisposeImage((GpImage *)Bitmap);
    if (!Result)
        Size->cx = Size->cy = 0;
    return Result;
}

static
LPCWSTR
MgFileName(
    _In_z_ LPCWSTR Path)
{
    LPCWSTR Slash = wcsrchr(Path, L'\\');
    LPCWSTR Name = Slash ? Slash + 1 : Path;

    /* The service prefixes the part order */
    if (wcslen(Name) > 3 && iswdigit(Name[0]) && iswdigit(Name[1]) && Name[2] == L'_')
        Name += 3;
    return Name;
}

/* Media for a message with attachments, NULL for anything else */
static
PMG_MEDIA
MgGetMedia(
    _In_ const PHONEOM_MESSAGE *Message)
{
    PHONEOM_ATTACHMENT *Files;
    PMG_MEDIA Media;
    DWORD Count = 0, i;

    if (!(Message->Flags & PHONEOM_MESSAGE_ATTACHMENTS))
        return NULL;

    for (i = 0; i < MgMediaCount; i++)
    {
        if (MgMedia[i].MessageId == Message->MessageId)
            return &MgMedia[i];
    }

    if (!MgPhone)
        return NULL;

    if (MgMediaCount == MgMediaCapacity)
    {
        DWORD Capacity = MgMediaCapacity ? MgMediaCapacity * 2 : 16;
        PVOID Grown;

        Grown = MgMedia ? HeapReAlloc(GetProcessHeap(), 0, MgMedia, Capacity * sizeof(*MgMedia))
                        : HeapAlloc(GetProcessHeap(), 0, Capacity * sizeof(*MgMedia));
        if (!Grown)
            return NULL;

        MgMedia = Grown;
        MgMediaCapacity = Capacity;
    }

    Files = HeapAlloc(GetProcessHeap(), 0, PHONEOM_MAX_ATTACHMENTS * sizeof(*Files));
    if (!Files)
        return NULL;

    if (FAILED(PhoneOmGetAttachments(MgPhone, Message->MessageId, Files, PHONEOM_MAX_ATTACHMENTS, &Count)))
        Count = 0;

    Media = &MgMedia[MgMediaCount++];
    ZeroMemory(Media, sizeof(*Media));
    Media->MessageId = Message->MessageId;

    for (i = 0; i < Count; i++)
    {
        LPCWSTR Dot = wcsrchr(Files[i].Path, L'.');

        /* The layout file is not something to show */
        if (Dot && !_wcsicmp(Dot, L".smil"))
            continue;

        if (!Media->Picture[0] && MgIsPicture(Files[i].Path))
            StringCchCopyW(Media->Picture, _countof(Media->Picture), Files[i].Path);

        if (Media->Summary[0])
            StringCchCatW(Media->Summary, _countof(Media->Summary), L", ");
        StringCchCatW(Media->Summary, _countof(Media->Summary), MgFileName(Files[i].Path));
        Media->Count++;
    }

    HeapFree(GetProcessHeap(), 0, Files);
    return Media;
}

static
VOID
MgFreeMedia(VOID)
{
    DWORD i;

    for (i = 0; i < MgMediaCount; i++)
    {
        if (MgMedia[i].Thumb)
            DeleteObject(MgMedia[i].Thumb);
    }

    HeapFree(GetProcessHeap(), 0, MgMedia);
    MgMedia = NULL;
    MgMediaCount = MgMediaCapacity = 0;
}

/* Text for the conversation list, a picture message may have none */
static
LPCWSTR
MgPreviewText(
    _In_ const PHONEOM_MESSAGE *Message)
{
    if (Message->Text[0] || !(Message->Flags & PHONEOM_MESSAGE_MMS))
        return Message->Text;
    return L"Picture message";
}

static
VOID
MgBuildThreads(VOID)
{
    DWORD i, t;

    HeapFree(GetProcessHeap(), 0, MgThreads);
    MgThreads = NULL;
    MgThreadCount = 0;

    if (!MgMessageCount)
        return;

    MgThreads = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, MgMessageCount * sizeof(*MgThreads));
    if (!MgThreads)
        return;

    for (i = 0; i < MgMessageCount; i++)
    {
        const PHONEOM_MESSAGE *Message = &MgMessages[i];

        for (t = 0; t < MgThreadCount; t++)
        {
            if (MgSameAddress(MgThreads[t].Address, Message->Address))
                break;
        }

        if (t == MgThreadCount)
        {
            StringCchCopyW(MgThreads[t].Address, _countof(MgThreads[t].Address), Message->Address);
            MgThreads[t].Last = i;
            MgThreadCount++;
        }

        MgThreads[t].Count++;
        if (MgCompareTime(&Message->Timestamp, &MgMessages[MgThreads[t].Last].Timestamp) >= 0)
            MgThreads[t].Last = i;
        if ((Message->Flags & PHONEOM_MESSAGE_INCOMING) && !(Message->Flags & PHONEOM_MESSAGE_READ))
            MgThreads[t].Unread++;
    }

    /* Newest conversation first */
    for (i = 1; i < MgThreadCount; i++)
    {
        MG_THREAD Key = MgThreads[i];

        t = i;
        while (t > 0 &&
               MgCompareTime(&MgMessages[MgThreads[t - 1].Last].Timestamp, &MgMessages[Key.Last].Timestamp) < 0)
        {
            MgThreads[t] = MgThreads[t - 1];
            t--;
        }
        MgThreads[t] = Key;
    }
}

static
BOOL
MgAddMessage(
    _In_ const PHONEOM_MESSAGE *Message)
{
    DWORD i;

    for (i = 0; i < MgMessageCount; i++)
    {
        if (MgMessages[i].MessageId == Message->MessageId)
        {
            MgMessages[i] = *Message;
            return TRUE;
        }
    }

    if (MgMessageCount == MgMessageCapacity)
    {
        DWORD Capacity = MgMessageCapacity ? MgMessageCapacity * 2 : 64;
        PVOID Grown;

        Grown = MgMessages ? HeapReAlloc(GetProcessHeap(), 0, MgMessages, Capacity * sizeof(*MgMessages))
                           : HeapAlloc(GetProcessHeap(), 0, Capacity * sizeof(*MgMessages));
        if (!Grown)
            return FALSE;

        MgMessages = Grown;
        MgMessageCapacity = Capacity;
    }

    MgMessages[MgMessageCount++] = *Message;
    return TRUE;
}

static
VOID
MgLoadMessages(VOID)
{
    PHONEOM_MESSAGE Batch[16];
    DWORD Count, Next = 1, i;

    MgMessageCount = 0;
    if (!MgPhone)
        return;

    for (;;)
    {
        if (FAILED(PhoneOmGetMessages(MgPhone, Next, Batch, _countof(Batch), &Count)) || Count == 0)
            break;

        for (i = 0; i < Count; i++)
            MgAddMessage(&Batch[i]);

        Next = Batch[Count - 1].MessageId + 1;
        if (Count < _countof(Batch))
            break;
    }
}

static
VOID
MgFillList(VOID)
{
    HWND List = GetDlgItem(MgWindow, IDC_LIST);
    DWORD i;

    SendMessageW(List, WM_SETREDRAW, FALSE, 0);
    SendMessageW(List, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < MgThreadCount; i++)
        SendMessageW(List, LB_ADDSTRING, 0, (LPARAM)i);
    SendMessageW(List, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(List, NULL, TRUE);

    if (!MgPhone)
        MgSetStatus(L"Waiting for the phone service");
    else if (!MgThreadCount)
        MgSetStatus(L"No messages yet");
    else
        MgSetStatus(L"%lu conversation(s)", MgThreadCount);
}

static
VOID
MgFillThread(VOID)
{
    HWND Thread = GetDlgItem(MgWindow, IDC_THREAD);
    DWORD *Order;
    DWORD Count = 0, i, j;
    LRESULT Index = LB_ERR;

    SendMessageW(Thread, WM_SETREDRAW, FALSE, 0);
    SendMessageW(Thread, LB_RESETCONTENT, 0, 0);

    Order = HeapAlloc(GetProcessHeap(), 0, (MgMessageCount + 1) * sizeof(*Order));
    if (Order)
    {
        for (i = 0; i < MgMessageCount; i++)
        {
            if (MgAddress[0] && MgSameAddress(MgMessages[i].Address, MgAddress))
                Order[Count++] = i;
        }

        /* Oldest at the top, as a chat reads */
        for (i = 1; i < Count; i++)
        {
            DWORD Key = Order[i];

            j = i;
            while (j > 0 && MgCompareTime(&MgMessages[Order[j - 1]].Timestamp, &MgMessages[Key].Timestamp) > 0)
            {
                Order[j] = Order[j - 1];
                j--;
            }
            Order[j] = Key;
        }

        for (i = 0; i < Count; i++)
            Index = SendMessageW(Thread, LB_ADDSTRING, 0, (LPARAM)Order[i]);

        HeapFree(GetProcessHeap(), 0, Order);
    }

    SendMessageW(Thread, WM_SETREDRAW, TRUE, 0);
    if (Index != LB_ERR)
        SendMessageW(Thread, LB_SETTOPINDEX, Index, 0);
    InvalidateRect(Thread, NULL, TRUE);
}

/* Reading a conversation marks what came in as read, here and in the service store */
static
VOID
MgMarkThreadRead(VOID)
{
    DWORD i;

    if (!MgPhone || !MgAddress[0])
        return;

    for (i = 0; i < MgMessageCount; i++)
    {
        PPHONEOM_MESSAGE Message = &MgMessages[i];

        if ((Message->Flags & PHONEOM_MESSAGE_INCOMING) && !(Message->Flags & PHONEOM_MESSAGE_READ) &&
            MgSameAddress(Message->Address, MgAddress) &&
            SUCCEEDED(PhoneOmMarkMessageRead(MgPhone, Message->MessageId)))
        {
            Message->Flags |= PHONEOM_MESSAGE_READ;
        }
    }
}

static
VOID
MgLayout(VOID);

static
VOID
MgHidePicture(VOID);

static
VOID
MgShowView(
    _In_ BOOL Thread,
    _In_ BOOL ComposeNew)
{
    static const UINT ListIds[] = { IDC_LIST, IDC_NEW, IDC_EXIT };
    static const UINT ThreadIds[] = { IDC_BACK, IDC_THREAD, IDC_COMPOSE, IDC_SEND, IDC_DELETE, IDC_ATTACH };
    DWORD i;

    MgInThread = Thread;
    MgComposingNew = ComposeNew;
    MgHidePicture();

    for (i = 0; i < _countof(ListIds); i++)
        ShowWindow(GetDlgItem(MgWindow, ListIds[i]), Thread ? SW_HIDE : SW_SHOW);
    for (i = 0; i < _countof(ThreadIds); i++)
        ShowWindow(GetDlgItem(MgWindow, ThreadIds[i]), Thread ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(MgWindow, IDC_TO), (Thread && ComposeNew) ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(MgWindow, IDC_DELETE), (Thread && !ComposeNew) ? SW_SHOW : SW_HIDE);

    MgLayout();
    InvalidateRect(MgWindow, NULL, FALSE);

    if (Thread)
    {
        MgMarkThreadRead();
        MgBuildThreads();
        MgFillThread();
        SetFocus(GetDlgItem(MgWindow, ComposeNew ? IDC_TO : IDC_COMPOSE));
    }
    else
    {
        MgAddress[0] = UNICODE_NULL;
        MgBuildThreads();
        MgFillList();
    }
}

static
VOID
MgRefresh(VOID)
{
    MgBuildThreads();
    if (MgInThread)
        MgFillThread();
    else
        MgFillList();
}

static
VOID
CALLBACK
MgPhoneEvent(
    _In_ HPHONEOM Phone,
    _In_ DWORD Event,
    _In_reads_bytes_opt_(DataSize) const VOID *Data,
    _In_ DWORD DataSize,
    _In_opt_ PVOID Context)
{
    PMG_EVENT Copy;

    UNREFERENCED_PARAMETER(Phone);
    UNREFERENCED_PARAMETER(Context);

    Copy = HeapAlloc(GetProcessHeap(), 0, FIELD_OFFSET(MG_EVENT, Data) + DataSize);
    if (!Copy)
        return;

    Copy->Event = Event;
    Copy->Size = DataSize;
    if (Data && DataSize)
        CopyMemory(Copy->Data, Data, DataSize);

    if (!PostMessageW(MgWindow, WM_PHONE_EVENT, 0, (LPARAM)Copy))
        HeapFree(GetProcessHeap(), 0, Copy);
}

static
VOID
MgOpenPhone(VOID)
{
    if (MgPhone)
        return;

    if (FAILED(PhoneOmOpen(MgPhoneEvent, NULL, &MgPhone)))
    {
        MgPhone = NULL;
        MgSetStatus(L"Waiting for the phone service");
        SetTimer(MgWindow, MG_RETRY_TIMER, MG_RETRY_MS, NULL);
        return;
    }

    KillTimer(MgWindow, MG_RETRY_TIMER);
    MgLoadMessages();
    MgRefresh();
}

static
VOID
MgHandleEvent(
    _In_ const MG_EVENT *Event)
{
    const PHONEOM_MESSAGE *Message = (const PHONEOM_MESSAGE *)Event->Data;

    switch (Event->Event)
    {
        case PhoneEventMessage:
            if (Event->Size < sizeof(*Message))
                break;

            MgAddMessage(Message);
            if (MgInThread && MgSameAddress(Message->Address, MgAddress))
                MgMarkThreadRead();
            MessageBeep(MB_OK);
            MgRefresh();
            break;

        case PhoneEventMessageStatus:
            if (Event->Size < sizeof(*Message))
                break;

            MgAddMessage(Message);
            if (Message->Flags & PHONEOM_MESSAGE_FAILED)
                MgSetStatus(L"Message to %s failed", Message->Address);
            MgRefresh();
            break;

        case PhoneEventDisconnected:
            PhoneOmClose(MgPhone);
            MgPhone = NULL;
            MgSetStatus(L"Phone service went away");
            SetTimer(MgWindow, MG_RETRY_TIMER, MG_RETRY_MS, NULL);
            break;
    }
}

static
VOID
MgHidePicture(VOID)
{
    HWND Viewer = GetDlgItem(MgWindow, IDC_VIEWER);
    HBITMAP Old;

    ShowWindow(Viewer, SW_HIDE);
    Old = (HBITMAP)SendMessageW(Viewer, STM_SETIMAGE, IMAGE_BITMAP, 0);
    if (Old)
        DeleteObject(Old);
}

/* Full view of a picture over the conversation, a tap closes it */
static
VOID
MgShowPicture(
    _In_z_ LPCWSTR Path)
{
    HWND Viewer = GetDlgItem(MgWindow, IDC_VIEWER);
    HWND Thread = GetDlgItem(MgWindow, IDC_THREAD);
    HBITMAP Bitmap, Old;
    RECT Area;
    SIZE Size;

    GetWindowRect(Thread, &Area);
    MapWindowPoints(NULL, MgWindow, (LPPOINT)&Area, 2);

    Bitmap = MgLoadPicture(Path, Area.right - Area.left, Area.bottom - Area.top, &Size);
    if (!Bitmap)
    {
        MgSetStatus(L"Cannot show %s", MgFileName(Path));
        return;
    }

    Old = (HBITMAP)SendMessageW(Viewer, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)Bitmap);
    if (Old)
        DeleteObject(Old);

    SetWindowPos(Viewer, HWND_TOP, Area.left, Area.top, Area.right - Area.left, Area.bottom - Area.top, SWP_SHOWWINDOW);
    MgSetStatus(L"%s, tap to close", MgFileName(Path));
}

/* Taps on a picture message open the picture */
static
LRESULT
CALLBACK
MgThreadListWndProc(
    _In_ HWND Window,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    if (Message == WM_LBUTTONUP)
    {
        LRESULT Hit = CallWindowProcW(MgThreadListProc, Window, LB_ITEMFROMPOINT, 0, lParam);
        LRESULT Data;
        PMG_MEDIA Media;

        if (HIWORD(Hit) == 0)
        {
            Data = CallWindowProcW(MgThreadListProc, Window, LB_GETITEMDATA, LOWORD(Hit), 0);
            if (Data != LB_ERR && (DWORD)Data < MgMessageCount)
            {
                Media = MgGetMedia(&MgMessages[Data]);
                if (Media && Media->Picture[0])
                    MgShowPicture(Media->Picture);
            }
        }
    }

    return CallWindowProcW(MgThreadListProc, Window, Message, wParam, lParam);
}

static
VOID
MgPickAttachment(VOID)
{
    OPENFILENAMEW Dialog;
    WCHAR Path[PHONEOM_MAX_PATH] = L"";

    /* A second tap drops the picture */
    if (MgAttachPath[0])
    {
        MgAttachPath[0] = UNICODE_NULL;
        SetDlgItemTextW(MgWindow, IDC_ATTACH, L"Picture");
        MgSetStatus(L"Picture removed");
        return;
    }

    ZeroMemory(&Dialog, sizeof(Dialog));
    Dialog.lStructSize = sizeof(Dialog);
    Dialog.hwndOwner = MgWindow;
    Dialog.lpstrFilter = L"Pictures\0*.jpg;*.jpeg;*.png;*.gif\0All files\0*.*\0";
    Dialog.lpstrFile = Path;
    Dialog.nMaxFile = _countof(Path);
    Dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&Dialog))
        return;

    StringCchCopyW(MgAttachPath, _countof(MgAttachPath), Path);
    SetDlgItemTextW(MgWindow, IDC_ATTACH, L"Remove");
    MgSetStatus(L"Picture attached: %s", MgFileName(Path));
}

static
VOID
MgSend(VOID)
{
    WCHAR Text[PHONEOM_MAX_TEXT];
    PHONEOM_MESSAGE Message;
    DWORD Id;
    HRESULT hr;

    if (MgComposingNew)
        GetDlgItemTextW(MgWindow, IDC_TO, MgAddress, _countof(MgAddress));
    GetDlgItemTextW(MgWindow, IDC_COMPOSE, Text, _countof(Text));

    if (!MgAddress[0] || (!Text[0] && !MgAttachPath[0]))
    {
        MgSetStatus(L"Enter a number and a message");
        return;
    }

    if (!MgPhone)
    {
        MgSetStatus(L"The phone service is not running");
        return;
    }

    if (MgAttachPath[0])
        hr = PhoneOmSendMms(MgPhone, MgAddress, Text, MgAttachPath, &Id);
    else
        hr = PhoneOmSendMessage(MgPhone, MgAddress, Text, &Id);
    if (FAILED(hr))
    {
        MgSetStatus(L"Send failed 0x%08lx", hr);
        return;
    }

    /* Shown right away as sending, the status event updates it */
    ZeroMemory(&Message, sizeof(Message));
    Message.MessageId = Id;
    if (MgAttachPath[0])
        Message.Flags = PHONEOM_MESSAGE_MMS | PHONEOM_MESSAGE_ATTACHMENTS;
    GetSystemTimeAsFileTime(&Message.Timestamp);
    StringCchCopyW(Message.Address, _countof(Message.Address), MgAddress);
    StringCchCopyW(Message.Text, _countof(Message.Text), Text);
    MgAddMessage(&Message);

    SetDlgItemTextW(MgWindow, IDC_COMPOSE, L"");
    MgSetStatus(L"Sending to %s", MgAddress);
    MgAttachPath[0] = UNICODE_NULL;
    SetDlgItemTextW(MgWindow, IDC_ATTACH, L"Picture");

    if (MgComposingNew)
        MgShowView(TRUE, FALSE);
    else
        MgRefresh();
}

static
VOID
MgDeleteThread(VOID)
{
    DWORD i = 0, Deleted = 0;

    if (!MgPhone || !MgAddress[0])
        return;

    if (MessageBoxW(MgWindow, L"Delete this conversation?", L"Messages", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    while (i < MgMessageCount)
    {
        if (MgSameAddress(MgMessages[i].Address, MgAddress) &&
            SUCCEEDED(PhoneOmDeleteMessage(MgPhone, MgMessages[i].MessageId)))
        {
            MgMessages[i] = MgMessages[--MgMessageCount];
            Deleted++;
            continue;
        }
        i++;
    }

    MgShowView(FALSE, FALSE);
    MgSetStatus(L"Deleted %lu message(s)", Deleted);
}

static
VOID
MgDrawButton(
    _In_ const DRAWITEMSTRUCT *Item)
{
    WCHAR Text[64];
    RECT Rect = Item->rcItem;
    BOOL Pressed = (Item->itemState & ODS_SELECTED) != 0;
    BOOL Accent = (Item->CtlID == IDC_SEND || Item->CtlID == IDC_NEW);

    FillRect(Item->hDC, &Rect, MgBgBrush);
    MgFillRound(Item->hDC, &Rect, (Rect.bottom - Rect.top) / 3, (Pressed || Accent) ? MG_ACCENT : MG_TILE);

    GetWindowTextW(Item->hwndItem, Text, _countof(Text));
    SetBkMode(Item->hDC, TRANSPARENT);
    SetTextColor(Item->hDC, MG_TEXT);
    SelectObject(Item->hDC, MgButtonFont);
    DrawTextW(Item->hDC, Text, -1, &Rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
}

static
VOID
MgDrawConversation(
    _In_ const DRAWITEMSTRUCT *Item)
{
    const MG_THREAD *Thread;
    const PHONEOM_MESSAGE *Last;
    WCHAR Line[128];
    RECT Card = Item->rcItem, Text;

    FillRect(Item->hDC, &Item->rcItem, MgBgBrush);
    if (Item->itemID == (UINT)-1 || (DWORD)Item->itemData >= MgThreadCount)
        return;

    Thread = &MgThreads[Item->itemData];
    Last = &MgMessages[Thread->Last];

    InflateRect(&Card, 0, -MG_GAP / 2);
    MgFillRound(Item->hDC, &Card, 24, (Item->itemState & ODS_SELECTED) ? MG_ACCENT : MG_TILE);
    SetBkMode(Item->hDC, TRANSPARENT);

    Text = Card;
    InflateRect(&Text, -MG_BUBBLE_PAD, -MG_BUBBLE_PAD / 2);
    Text.bottom = Text.top + (Card.bottom - Card.top) / 2;
    SelectObject(Item->hDC, MgFont);
    SetTextColor(Item->hDC, MG_TEXT);
    if (Thread->Unread)
        StringCchPrintfW(Line, _countof(Line), L"%s  (%lu new)", Thread->Address, Thread->Unread);
    else
        StringCchCopyW(Line, _countof(Line), Thread->Address);
    DrawTextW(Item->hDC, Line, -1, &Text, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    MgFormatTime(&Last->Timestamp, Line, _countof(Line));
    SelectObject(Item->hDC, MgSmallFont);
    SetTextColor(Item->hDC, MG_SUBTEXT);
    DrawTextW(Item->hDC, Line, -1, &Text, DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX);

    Text.top = Text.bottom;
    Text.bottom = Card.bottom - MG_BUBBLE_PAD / 2;
    DrawTextW(Item->hDC, MgPreviewText(Last), -1, &Text, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

/* The bubble rectangle of one message, inside the given row of the thread */
static
VOID
MgBubbleRect(
    _In_ HDC Dc,
    _In_ const PHONEOM_MESSAGE *Message,
    _In_ int Width,
    _Out_ RECT *Bubble,
    _Out_ RECT *Text)
{
    int MaxText = Width * MG_BUBBLE_WIDTH_PERCENT / 100 - 2 * MG_BUBBLE_PAD;
    RECT Measure = { 0, 0, MaxText, 0 };
    HGDIOBJ Old = SelectObject(Dc, MgFont);
    PMG_MEDIA Media = MgGetMedia(Message);
    int Footer;

    DrawTextW(Dc, Message->Text[0] ? Message->Text : L" ", -1, &Measure,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    SelectObject(Dc, Old);

    Footer = 34;
    if (Media && Media->Count)
    {
        /* The thumbnail and the file names sit between the text and the time */
        if (Media->Picture[0] && !Media->Thumb)
            Media->Thumb = MgLoadPicture(Media->Picture, MaxText, MG_THUMB_MAX_H, &Media->ThumbSize);

        Footer += MG_ATTACH_LINE_H + (Media->Thumb ? Media->ThumbSize.cy + MG_GAP / 2 : 0);
        Measure.right = max(Measure.right, Media->ThumbSize.cx);

        /* Keep the bubble inside what a variable height list item can hold */
        if (Measure.bottom + 2 * MG_BUBBLE_PAD + Footer + MG_GAP > MG_ITEM_MAX_H)
            Measure.bottom = max(MG_ITEM_MAX_H - 2 * MG_BUBBLE_PAD - Footer - MG_GAP, 0);
    }
    Bubble->top = MG_GAP / 2;
    Bubble->bottom = Bubble->top + Measure.bottom + 2 * MG_BUBBLE_PAD + Footer;
    if (Message->Flags & PHONEOM_MESSAGE_INCOMING)
    {
        Bubble->left = 0;
        Bubble->right = Measure.right + 2 * MG_BUBBLE_PAD;
    }
    else
    {
        Bubble->right = Width;
        Bubble->left = Width - Measure.right - 2 * MG_BUBBLE_PAD;
    }

    if (Bubble->right - Bubble->left < 220)
    {
        if (Message->Flags & PHONEOM_MESSAGE_INCOMING)
            Bubble->right = Bubble->left + 220;
        else
            Bubble->left = Bubble->right - 220;
    }

    *Text = *Bubble;
    InflateRect(Text, -MG_BUBBLE_PAD, -MG_BUBBLE_PAD);
    Text->bottom -= Footer;
}

static
VOID
MgMeasureBubble(
    _Inout_ MEASUREITEMSTRUCT *Item)
{
    HWND Thread = GetDlgItem(MgWindow, IDC_THREAD);
    RECT Client, Bubble, Text;
    HDC Dc;

    Item->itemHeight = MG_ITEM_H;
    if ((DWORD)Item->itemData >= MgMessageCount || !Thread)
        return;

    GetClientRect(Thread, &Client);
    Dc = GetDC(Thread);
    MgBubbleRect(Dc, &MgMessages[Item->itemData], Client.right, &Bubble, &Text);
    ReleaseDC(Thread, Dc);

    Item->itemHeight = min(Bubble.bottom + MG_GAP / 2, MG_ITEM_MAX_H);
}

/* Thumbnail and file names below the text of a picture message */
static
VOID
MgDrawMedia(
    _In_ HDC Dc,
    _In_ const PHONEOM_MESSAGE *Message,
    _In_ const RECT *Text)
{
    PMG_MEDIA Media = MgGetMedia(Message);
    WCHAR Line[160];
    RECT Rect;
    HDC Memory;
    HGDIOBJ Old;

    if (!Media)
        return;

    Rect.left = Text->left;
    Rect.right = Text->right + MG_BUBBLE_PAD;
    Rect.top = Text->bottom;

    if (Media->Thumb)
    {
        Memory = CreateCompatibleDC(Dc);
        if (Memory)
        {
            Old = SelectObject(Memory, Media->Thumb);
            BitBlt(Dc, Rect.left, Rect.top + MG_GAP / 2, Media->ThumbSize.cx, Media->ThumbSize.cy, Memory, 0, 0, SRCCOPY);
            SelectObject(Memory, Old);
            DeleteDC(Memory);
        }
        Rect.top += Media->ThumbSize.cy + MG_GAP / 2;
    }

    if (Media->Count == 0)
        StringCchCopyW(Line, _countof(Line), L"Attachment not available");
    else if (Media->Count == 1)
        StringCchPrintfW(Line, _countof(Line), L"Attachment: %s", Media->Summary);
    else
        StringCchPrintfW(Line, _countof(Line), L"%lu attachments: %s", Media->Count, Media->Summary);

    Rect.bottom = Rect.top + MG_ATTACH_LINE_H;
    SelectObject(Dc, MgSmallFont);
    SetTextColor(Dc, (Message->Flags & PHONEOM_MESSAGE_INCOMING) ? MG_SUBTEXT : MG_TEXT);
    DrawTextW(Dc, Line, -1, &Rect, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static
VOID
MgDrawBubble(
    _In_ const DRAWITEMSTRUCT *Item)
{
    const PHONEOM_MESSAGE *Message;
    WCHAR Footer[96];
    WCHAR Time[48];
    RECT Bubble, Text, Foot;
    LPCWSTR State = L"";
    COLORREF Fill;

    FillRect(Item->hDC, &Item->rcItem, MgBgBrush);
    if (Item->itemID == (UINT)-1 || (DWORD)Item->itemData >= MgMessageCount)
        return;

    Message = &MgMessages[Item->itemData];
    MgBubbleRect(Item->hDC, Message, Item->rcItem.right - Item->rcItem.left, &Bubble, &Text);
    OffsetRect(&Bubble, Item->rcItem.left, Item->rcItem.top);
    OffsetRect(&Text, Item->rcItem.left, Item->rcItem.top);

    if (Message->Flags & PHONEOM_MESSAGE_INCOMING)
        Fill = MG_TILE;
    else if (Message->Flags & PHONEOM_MESSAGE_FAILED)
        Fill = MG_FAILED;
    else
        Fill = MG_ACCENT;

    MgFillRound(Item->hDC, &Bubble, 28, Fill);
    SetBkMode(Item->hDC, TRANSPARENT);
    SetTextColor(Item->hDC, MG_TEXT);
    SelectObject(Item->hDC, MgFont);
    DrawTextW(Item->hDC, Message->Text, -1, &Text, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | DT_END_ELLIPSIS);
    MgDrawMedia(Item->hDC, Message, &Text);

    if (!(Message->Flags & PHONEOM_MESSAGE_INCOMING))
    {
        if (Message->Flags & PHONEOM_MESSAGE_FAILED)
            State = L"  failed";
        else if (Message->Flags & PHONEOM_MESSAGE_SENT)
            State = L"  sent";
        else
            State = L"  sending";
    }

    MgFormatTime(&Message->Timestamp, Time, _countof(Time));
    StringCchPrintfW(Footer, _countof(Footer), L"%s%s", Time, State);
    Foot = Bubble;
    Foot.left += MG_BUBBLE_PAD;
    Foot.right -= MG_BUBBLE_PAD;
    Foot.top = Text.bottom;
    Foot.bottom = Bubble.bottom - MG_BUBBLE_PAD / 2;
    SelectObject(Item->hDC, MgSmallFont);
    SetTextColor(Item->hDC, (Message->Flags & PHONEOM_MESSAGE_INCOMING) ? MG_SUBTEXT : MG_TEXT);
    DrawTextW(Item->hDC, Footer, -1, &Foot, DT_SINGLELINE | DT_RIGHT | DT_BOTTOM | DT_NOPREFIX);
}

static
HFONT
MgMakeFont(
    _In_ int Height,
    _In_ int Weight)
{
    return CreateFontW(Height, 0, 0, 0, Weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
}

static
HWND
MgControl(
    _In_ LPCWSTR Class,
    _In_opt_ LPCWSTR Text,
    _In_ DWORD Style,
    _In_ UINT Id,
    _In_ HFONT Font)
{
    HWND Control;

    Control = CreateWindowExW(0, Class, Text, WS_CHILD | Style, 0, 0, 10, 10,
                              MgWindow, (HMENU)(ULONG_PTR)Id, MgInstance, NULL);
    if (Control)
        SendMessageW(Control, WM_SETFONT, (WPARAM)Font, FALSE);
    return Control;
}

static
VOID
MgCreateControls(VOID)
{
    MgControl(L"LISTBOX", NULL, WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | LBS_OWNERDRAWFIXED,
              IDC_LIST, MgFont);
    MgControl(L"BUTTON", L"New message", WS_TABSTOP | BS_OWNERDRAW, IDC_NEW, MgButtonFont);
    MgControl(L"BUTTON", L"Exit", WS_TABSTOP | BS_OWNERDRAW, IDC_EXIT, MgButtonFont);

    MgControl(L"BUTTON", L"Back", WS_TABSTOP | BS_OWNERDRAW, IDC_BACK, MgButtonFont);
    MgControl(L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL, IDC_TO, MgFont);
    MgControl(L"LISTBOX", NULL, WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWVARIABLE | LBS_NOSEL,
              IDC_THREAD, MgFont);
    MgControl(L"EDIT", NULL, WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, IDC_COMPOSE, MgFont);
    MgControl(L"BUTTON", L"Send", WS_TABSTOP | BS_OWNERDRAW, IDC_SEND, MgButtonFont);
    MgControl(L"BUTTON", L"Delete", WS_TABSTOP | BS_OWNERDRAW, IDC_DELETE, MgButtonFont);
    MgControl(L"BUTTON", L"Picture", WS_TABSTOP | BS_OWNERDRAW, IDC_ATTACH, MgButtonFont);
    MgControl(L"STATIC", NULL, SS_BITMAP | SS_CENTERIMAGE | SS_NOTIFY, IDC_VIEWER, MgFont);

    MgControl(L"STATIC", L"", WS_VISIBLE | SS_CENTERIMAGE | SS_ENDELLIPSIS, IDC_STATUS, MgSmallFont);

    SendDlgItemMessageW(MgWindow, IDC_LIST, LB_SETITEMHEIGHT, 0, MG_ITEM_H);
    MgThreadListProc = (WNDPROC)SetWindowLongPtrW(GetDlgItem(MgWindow, IDC_THREAD), GWLP_WNDPROC, (LONG_PTR)MgThreadListWndProc);
    SendDlgItemMessageW(MgWindow, IDC_COMPOSE, EM_LIMITTEXT, PHONEOM_MAX_TEXT - 1, 0);
    SendDlgItemMessageW(MgWindow, IDC_TO, EM_LIMITTEXT, PHONEOM_MAX_NUMBER - 1, 0);
}

static
VOID
MgMove(
    _In_ UINT Id,
    _In_ int X,
    _In_ int Y,
    _In_ int Width,
    _In_ int Height)
{
    MoveWindow(GetDlgItem(MgWindow, Id), X, Y, Width, Height, TRUE);
}

static
VOID
MgLayout(VOID)
{
    RECT Client;
    int Width, Left = MG_MARGIN, Top, Bottom, Third;

    GetClientRect(MgWindow, &Client);
    Width = Client.right - 2 * MG_MARGIN;
    if (Width < 200)
        return;

    Third = (Width - 2 * MG_GAP) / 3;
    Top = MG_SAFE_TOP + MG_HEAD_H + MG_GAP;
    Bottom = Client.bottom - MG_MARGIN;

    MgMove(IDC_STATUS, Left, Bottom - 48, Width, 48);
    Bottom -= 48 + MG_GAP;

    if (!MgInThread)
    {
        MgMove(IDC_NEW, Left, Bottom - MG_ROW, Width - Third - MG_GAP, MG_ROW);
        MgMove(IDC_EXIT, Left + Width - Third, Bottom - MG_ROW, Third, MG_ROW);
        Bottom -= MG_ROW + MG_GAP;
        MgMove(IDC_LIST, Left, Top, Width, max(Bottom - Top, MG_ROW));
        return;
    }

    MgMove(IDC_BACK, Left, MG_SAFE_TOP, Third, MG_HEAD_H);
    MgMove(IDC_DELETE, Left + Width - Third, MG_SAFE_TOP, Third, MG_HEAD_H);

    if (MgComposingNew)
    {
        MgMove(IDC_TO, Left, Top, Width, MG_ROW);
        Top += MG_ROW + MG_GAP;
    }

    MgMove(IDC_COMPOSE, Left, Bottom - 2 * MG_ROW, Width - Third - MG_GAP, 2 * MG_ROW);
    MgMove(IDC_ATTACH, Left + Width - Third, Bottom - 2 * MG_ROW, Third, MG_ROW - MG_GAP / 2);
    MgMove(IDC_SEND, Left + Width - Third, Bottom - MG_ROW + MG_GAP / 2, Third, MG_ROW - MG_GAP / 2);
    Bottom -= 2 * MG_ROW + MG_GAP;
    MgMove(IDC_THREAD, Left, Top, Width, max(Bottom - Top, MG_ROW));
}

static
VOID
MgPaint(VOID)
{
    PAINTSTRUCT Paint;
    RECT Client, Head;
    HDC Dc = BeginPaint(MgWindow, &Paint);
    LPCWSTR Title;

    GetClientRect(MgWindow, &Client);
    FillRect(Dc, &Client, MgBgBrush);

    Head.top = MG_SAFE_TOP;
    Head.bottom = Head.top + MG_HEAD_H;
    Head.left = MG_MARGIN;
    Head.right = Client.right - MG_MARGIN;

    if (MgInThread)
    {
        /* Between the back and delete buttons */
        int Third = (Head.right - Head.left - 2 * MG_GAP) / 3;
        Head.left += Third + MG_GAP;
        Head.right -= Third + MG_GAP;
        Title = MgComposingNew ? L"New message" : MgAddress;
    }
    else
    {
        Title = L"Messages";
    }

    SetBkMode(Dc, TRANSPARENT);
    SetTextColor(Dc, MG_TEXT);
    SelectObject(Dc, MgHeadFont);
    DrawTextW(Dc, Title, -1, &Head,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | (MgInThread ? DT_CENTER : DT_LEFT));

    EndPaint(MgWindow, &Paint);
}

static
LRESULT
CALLBACK
MgWndProc(
    _In_ HWND Window,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    switch (Message)
    {
        case WM_CREATE:
            MgWindow = Window;
            MgCreateControls();
            MgShowView(FALSE, FALSE);
            MgOpenPhone();
            return 0;

        case WM_SIZE:
            MgLayout();
            InvalidateRect(Window, NULL, FALSE);
            if (MgInThread)
                MgFillThread();
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            MgPaint();
            return 0;

        case WM_MEASUREITEM:
            if (((MEASUREITEMSTRUCT *)lParam)->CtlID == IDC_THREAD)
                MgMeasureBubble((MEASUREITEMSTRUCT *)lParam);
            else
                ((MEASUREITEMSTRUCT *)lParam)->itemHeight = MG_ITEM_H;
            return TRUE;

        case WM_DRAWITEM:
        {
            const DRAWITEMSTRUCT *Item = (const DRAWITEMSTRUCT *)lParam;

            if (Item->CtlID == IDC_LIST)
                MgDrawConversation(Item);
            else if (Item->CtlID == IDC_THREAD)
                MgDrawBubble(Item);
            else
                MgDrawButton(Item);
            return TRUE;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            SetTextColor((HDC)wParam, MG_TEXT);
            SetBkColor((HDC)wParam, MG_FIELD);
            return (LRESULT)(((HWND)lParam == GetDlgItem(Window, IDC_TO) ||
                              (HWND)lParam == GetDlgItem(Window, IDC_COMPOSE)) ? MgFieldBrush : MgBgBrush);

        case WM_CTLCOLORSTATIC:
            SetTextColor((HDC)wParam, MG_SUBTEXT);
            SetBkColor((HDC)wParam, MG_BG);
            return (LRESULT)MgBgBrush;

        case WM_TIMER:
            if (wParam == MG_RETRY_TIMER)
                MgOpenPhone();
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDC_LIST:
                    if (HIWORD(wParam) == LBN_SELCHANGE)
                    {
                        LRESULT Index = SendDlgItemMessageW(Window, IDC_LIST, LB_GETCURSEL, 0, 0);

                        if (Index != LB_ERR && (DWORD)Index < MgThreadCount)
                        {
                            StringCchCopyW(MgAddress, _countof(MgAddress), MgThreads[Index].Address);
                            MgShowView(TRUE, FALSE);
                        }
                    }
                    break;

                case IDC_NEW:
                    MgAddress[0] = UNICODE_NULL;
                    SetDlgItemTextW(Window, IDC_TO, L"");
                    MgShowView(TRUE, TRUE);
                    break;

                case IDC_BACK:
                    if (IsWindowVisible(GetDlgItem(Window, IDC_VIEWER)))
                        MgHidePicture();
                    else
                        MgShowView(FALSE, FALSE);
                    break;

                case IDC_VIEWER:
                    if (HIWORD(wParam) == STN_CLICKED)
                        MgHidePicture();
                    break;

                case IDC_ATTACH:
                    MgPickAttachment();
                    break;

                case IDC_SEND:
                    MgSend();
                    break;

                case IDC_DELETE:
                    MgDeleteThread();
                    break;

                case IDC_EXIT:
                    DestroyWindow(Window);
                    break;
            }
            return 0;

        case WM_PHONE_EVENT:
            MgHandleEvent((const MG_EVENT *)lParam);
            HeapFree(GetProcessHeap(), 0, (PVOID)lParam);
            return 0;

        case WM_DESTROY:
            KillTimer(Window, MG_RETRY_TIMER);
            if (MgPhone)
            {
                PhoneOmClose(MgPhone);
                MgPhone = NULL;
            }
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(Window, Message, wParam, lParam);
}

int
WINAPI
wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nShowCmd)
{
    struct GdiplusStartupInput GdiplusInput;
    WNDCLASSEXW Class;
    HWND Window;
    MSG Msg;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nShowCmd);

    MgInstance = hInstance;

    /* Without GDI+ picture messages still show their file names */
    ZeroMemory(&GdiplusInput, sizeof(GdiplusInput));
    GdiplusInput.GdiplusVersion = 1;
    if (GdiplusStartup(&MgGdiplusToken, &GdiplusInput, NULL) != Ok)
        MgGdiplusToken = 0;

    MgFont = MgMakeFont(36, FW_NORMAL);
    MgSmallFont = MgMakeFont(26, FW_NORMAL);
    MgHeadFont = MgMakeFont(48, FW_SEMIBOLD);
    MgButtonFont = MgMakeFont(32, FW_SEMIBOLD);
    if (!MgFont)
        MgFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (!MgSmallFont)
        MgSmallFont = MgFont;
    if (!MgHeadFont)
        MgHeadFont = MgFont;
    if (!MgButtonFont)
        MgButtonFont = MgFont;

    MgBgBrush = CreateSolidBrush(MG_BG);
    MgFieldBrush = CreateSolidBrush(MG_FIELD);

    ZeroMemory(&Class, sizeof(Class));
    Class.cbSize = sizeof(Class);
    Class.lpfnWndProc = MgWndProc;
    Class.hInstance = hInstance;
    Class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    Class.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    Class.hbrBackground = MgBgBrush;
    Class.lpszClassName = L"ReactOSMessages";
    if (!RegisterClassExW(&Class))
        return 1;

    Window = CreateWindowExW(0, Class.lpszClassName, L"Messages", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                             NULL, NULL, hInstance, NULL);
    if (!Window)
        return 1;

    ShowWindow(Window, SW_SHOWMAXIMIZED);
    UpdateWindow(Window);

    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        if (!IsDialogMessageW(Window, &Msg))
        {
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
    }

    HeapFree(GetProcessHeap(), 0, MgMessages);
    HeapFree(GetProcessHeap(), 0, MgThreads);
    MgFreeMedia();
    if (MgGdiplusToken)
        GdiplusShutdown(MgGdiplusToken);
    DeleteObject(MgBgBrush);
    DeleteObject(MgFieldBrush);
    return (int)Msg.wParam;
}
