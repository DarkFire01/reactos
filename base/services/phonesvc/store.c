/*
 * PROJECT:     ReactOS Phone Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Persistent message store
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "phonesvc.h"

#define NDEBUG
#include <debug.h>

#define PH_STORE_MAGIC              0x534D4850
#define PH_STORE_VERSION            1
#define PH_STORE_MAX_RECORDS        2000
#define PH_STORE_GROW               64

typedef struct _PH_STORE_HEADER
{
    DWORD Magic;
    DWORD Version;
    DWORD RecordSize;
    DWORD NextId;
} PH_STORE_HEADER;

typedef struct _PH_STORE_RECORD
{
    DWORD InUse;
    PHONEOM_MESSAGE Message;
} PH_STORE_RECORD, *PPH_STORE_RECORD;

static CRITICAL_SECTION PhStoreLock;
static HANDLE PhStoreFile = INVALID_HANDLE_VALUE;
static PH_STORE_HEADER PhStoreHeader;
static PPH_STORE_RECORD PhRecords;
static DWORD PhRecordCount;
static DWORD PhRecordCapacity;

/* The folder that holds messages.dat, empty when the store is not persistent */
static WCHAR PhStoreFolder[MAX_PATH];

static
BOOL
PhStoreWriteAt(
    _In_ ULONGLONG Offset,
    _In_reads_bytes_(Size) const VOID *Data,
    _In_ DWORD Size)
{
    LARGE_INTEGER Position;
    DWORD Written;

    if (PhStoreFile == INVALID_HANDLE_VALUE)
        return TRUE;

    Position.QuadPart = Offset;
    if (!SetFilePointerEx(PhStoreFile, Position, NULL, FILE_BEGIN))
        return FALSE;

    return WriteFile(PhStoreFile, Data, Size, &Written, NULL) && Written == Size;
}

static
BOOL
PhStoreFlushRecord(
    _In_ DWORD Slot)
{
    ULONGLONG Offset = sizeof(PhStoreHeader) + (ULONGLONG)Slot * sizeof(*PhRecords);

    return PhStoreWriteAt(Offset, &PhRecords[Slot], sizeof(PhRecords[Slot])) &&
           PhStoreWriteAt(0, &PhStoreHeader, sizeof(PhStoreHeader));
}

static
BOOL
PhStoreGrow(
    _In_ DWORD Needed)
{
    PPH_STORE_RECORD NewRecords;
    DWORD NewCapacity;

    if (Needed <= PhRecordCapacity)
        return TRUE;

    NewCapacity = min(max(Needed, PhRecordCapacity + PH_STORE_GROW), PH_STORE_MAX_RECORDS);
    if (Needed > NewCapacity)
        return FALSE;

    if (PhRecords)
        NewRecords = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PhRecords, NewCapacity * sizeof(*PhRecords));
    else
        NewRecords = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, NewCapacity * sizeof(*PhRecords));

    if (!NewRecords)
        return FALSE;

    PhRecords = NewRecords;
    PhRecordCapacity = NewCapacity;
    return TRUE;
}

static
LONG
PhStoreFind(
    _In_ DWORD MessageId)
{
    DWORD i;

    for (i = 0; i < PhRecordCount; i++)
    {
        if (PhRecords[i].InUse && PhRecords[i].Message.MessageId == MessageId)
            return (LONG)i;
    }

    return -1;
}

/* The ProgramData root, read from the profile list so the service never loads shell32 or user32 */
static
BOOL
PhStoreGetDataRoot(
    _Out_writes_z_(Count) PWSTR Path,
    _In_ DWORD Count)
{
    WCHAR Raw[MAX_PATH];
    DWORD Size = sizeof(Raw) - sizeof(WCHAR);
    DWORD Type, Length;
    HKEY Key;
    LONG Error;

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList",
                          0,
                          KEY_QUERY_VALUE,
                          &Key);
    if (Error != ERROR_SUCCESS)
        return FALSE;

    ZeroMemory(Raw, sizeof(Raw));
    Error = RegQueryValueExW(Key, L"ProgramData", NULL, &Type, (LPBYTE)Raw, &Size);
    RegCloseKey(Key);
    if (Error != ERROR_SUCCESS || (Type != REG_SZ && Type != REG_EXPAND_SZ))
        return FALSE;

    Length = ExpandEnvironmentStringsW(Raw, Path, Count);
    if (Length == 0 || Length > Count)
        return FALSE;

    CreateDirectoryW(Path, NULL);
    return TRUE;
}

static
BOOL
PhStoreOpenFile(VOID)
{
    WCHAR Path[MAX_PATH];
    LARGE_INTEGER FileSize;
    DWORD Read, Count;

    if (!PhStoreGetDataRoot(Path, _countof(Path)))
        return FALSE;

    if (FAILED(StringCchCatW(Path, _countof(Path), L"\\ReactOS")))
        return FALSE;
    CreateDirectoryW(Path, NULL);

    if (FAILED(StringCchCatW(Path, _countof(Path), L"\\Phone")))
        return FALSE;
    CreateDirectoryW(Path, NULL);
    StringCchCopyW(PhStoreFolder, _countof(PhStoreFolder), Path);

    if (FAILED(StringCchCatW(Path, _countof(Path), L"\\messages.dat")))
        return FALSE;

    PhStoreFile = CreateFileW(Path,
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ,
                              NULL,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              NULL);
    if (PhStoreFile == INVALID_HANDLE_VALUE)
        return FALSE;

    if (ReadFile(PhStoreFile, &PhStoreHeader, sizeof(PhStoreHeader), &Read, NULL) &&
        Read == sizeof(PhStoreHeader) &&
        PhStoreHeader.Magic == PH_STORE_MAGIC &&
        PhStoreHeader.Version == PH_STORE_VERSION &&
        PhStoreHeader.RecordSize == sizeof(*PhRecords) &&
        GetFileSizeEx(PhStoreFile, &FileSize))
    {
        Count = (DWORD)min((FileSize.QuadPart - sizeof(PhStoreHeader)) / sizeof(*PhRecords), PH_STORE_MAX_RECORDS);
        if (Count && PhStoreGrow(Count) &&
            ReadFile(PhStoreFile, PhRecords, Count * sizeof(*PhRecords), &Read, NULL))
        {
            PhRecordCount = Read / sizeof(*PhRecords);
        }

        return TRUE;
    }

    /* Missing or foreign contents, start an empty store */
    PhStoreHeader.Magic = PH_STORE_MAGIC;
    PhStoreHeader.Version = PH_STORE_VERSION;
    PhStoreHeader.RecordSize = sizeof(*PhRecords);
    PhStoreHeader.NextId = 1;

    SetFilePointer(PhStoreFile, 0, NULL, FILE_BEGIN);
    SetEndOfFile(PhStoreFile);
    return PhStoreWriteAt(0, &PhStoreHeader, sizeof(PhStoreHeader));
}

BOOL
PhStoreInitialize(VOID)
{
    InitializeCriticalSection(&PhStoreLock);

    PhStoreHeader.NextId = 1;

    /* Without a file the store still works for the lifetime of the service */
    if (!PhStoreOpenFile())
    {
        DPRINT1("Message store is not persistent (%lu)\n", GetLastError());
        PhStoreFolder[0] = UNICODE_NULL;
        if (PhStoreFile != INVALID_HANDLE_VALUE)
        {
            CloseHandle(PhStoreFile);
            PhStoreFile = INVALID_HANDLE_VALUE;
        }
        PhStoreHeader.Magic = PH_STORE_MAGIC;
        PhStoreHeader.Version = PH_STORE_VERSION;
        PhStoreHeader.RecordSize = sizeof(*PhRecords);
        PhStoreHeader.NextId = 1;
    }

    return TRUE;
}

DWORD
PhStoreAdd(
    _Inout_ PPHONEOM_MESSAGE Message)
{
    DWORD Slot, Id = 0;

    EnterCriticalSection(&PhStoreLock);

    for (Slot = 0; Slot < PhRecordCount; Slot++)
    {
        if (!PhRecords[Slot].InUse)
            break;
    }

    if (Slot == PhRecordCount)
    {
        if (!PhStoreGrow(PhRecordCount + 1))
            goto Exit;
        PhRecordCount++;
    }

    Message->MessageId = PhStoreHeader.NextId++;
    if (PhStoreHeader.NextId == 0)
        PhStoreHeader.NextId = 1;

    PhRecords[Slot].InUse = TRUE;
    CopyMemory(&PhRecords[Slot].Message, Message, sizeof(*Message));

    if (!PhStoreFlushRecord(Slot))
        DPRINT1("Writing message %lu failed (%lu)\n", Message->MessageId, GetLastError());

    Id = Message->MessageId;

Exit:
    LeaveCriticalSection(&PhStoreLock);
    return Id;
}

BOOL
PhStoreUpdateFlags(
    _In_ DWORD MessageId,
    _In_ DWORD SetFlags,
    _In_ DWORD ClearFlags,
    _Out_opt_ PPHONEOM_MESSAGE Message)
{
    LONG Slot;

    EnterCriticalSection(&PhStoreLock);

    Slot = PhStoreFind(MessageId);
    if (Slot >= 0)
    {
        PhRecords[Slot].Message.Flags = (PhRecords[Slot].Message.Flags & ~ClearFlags) | SetFlags;
        PhStoreFlushRecord(Slot);
        if (Message)
            CopyMemory(Message, &PhRecords[Slot].Message, sizeof(*Message));
    }

    LeaveCriticalSection(&PhStoreLock);
    return Slot >= 0;
}

BOOL
PhStoreGetAttachmentFolder(
    _In_ DWORD MessageId,
    _In_ BOOL Create,
    _Out_writes_z_(Count) PWSTR Path,
    _In_ DWORD Count)
{
    DWORD Attributes;

    Path[0] = UNICODE_NULL;
    if (!PhStoreFolder[0])
        return FALSE;

    if (FAILED(StringCchPrintfW(Path, Count, L"%s\\Attachments", PhStoreFolder)))
        return FALSE;
    if (Create)
        CreateDirectoryW(Path, NULL);

    if (FAILED(StringCchPrintfW(Path, Count, L"%s\\Attachments\\%lu", PhStoreFolder, MessageId)))
        return FALSE;
    if (Create)
        CreateDirectoryW(Path, NULL);

    Attributes = GetFileAttributesW(Path);
    return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/* Called for every file in the attachment folder of a message, FALSE stops the walk */
typedef BOOL
(*PPH_ATTACHMENT_VISIT)(
    _In_z_ LPCWSTR Path,
    _In_ const WIN32_FIND_DATAW *Find,
    _Inout_opt_ PVOID Context);

static
VOID
PhStoreWalkAttachments(
    _In_ DWORD MessageId,
    _In_ PPH_ATTACHMENT_VISIT Visit,
    _Inout_opt_ PVOID Context)
{
    WCHAR Folder[MAX_PATH], Path[MAX_PATH];
    WIN32_FIND_DATAW Find;
    HANDLE Search;

    if (!PhStoreGetAttachmentFolder(MessageId, FALSE, Folder, _countof(Folder)))
        return;

    if (FAILED(StringCchPrintfW(Path, _countof(Path), L"%s\\*", Folder)))
        return;

    Search = FindFirstFileW(Path, &Find);
    if (Search == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if (Find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        if (SUCCEEDED(StringCchPrintfW(Path, _countof(Path), L"%s\\%s", Folder, Find.cFileName)) &&
            !Visit(Path, &Find, Context))
        {
            break;
        }
    } while (FindNextFileW(Search, &Find));

    FindClose(Search);
}

static
BOOL
PhStoreDeleteVisit(
    _In_z_ LPCWSTR Path,
    _In_ const WIN32_FIND_DATAW *Find,
    _Inout_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Find);
    UNREFERENCED_PARAMETER(Context);

    if (!DeleteFileW(Path))
        DPRINT1("Deleting attachment %S failed (%lu)\n", Path, GetLastError());
    return TRUE;
}

typedef struct _PH_ATTACHMENT_LIST
{
    PPHONEOM_ATTACHMENT Entries;
    DWORD Max;
    DWORD Count;
} PH_ATTACHMENT_LIST, *PPH_ATTACHMENT_LIST;

/* Keeps the list sorted by path, the file names start with the part order */
static
BOOL
PhStoreListVisit(
    _In_z_ LPCWSTR Path,
    _In_ const WIN32_FIND_DATAW *Find,
    _Inout_opt_ PVOID Context)
{
    PPH_ATTACHMENT_LIST List = Context;
    DWORD i;

    if (!List || List->Count >= List->Max)
        return FALSE;

    i = List->Count;
    while (i > 0 && _wcsicmp(List->Entries[i - 1].Path, Path) > 0)
    {
        List->Entries[i] = List->Entries[i - 1];
        i--;
    }

    ZeroMemory(&List->Entries[i], sizeof(List->Entries[i]));
    List->Entries[i].Size = Find->nFileSizeLow;
    StringCchCopyW(List->Entries[i].Path, _countof(List->Entries[i].Path), Path);
    List->Count++;
    return TRUE;
}

DWORD
PhStoreGetAttachments(
    _In_ DWORD MessageId,
    _Out_writes_to_(MaxAttachments, return) PPHONEOM_ATTACHMENT Attachments,
    _In_ DWORD MaxAttachments)
{
    PH_ATTACHMENT_LIST List;

    List.Entries = Attachments;
    List.Max = MaxAttachments;
    List.Count = 0;
    PhStoreWalkAttachments(MessageId, PhStoreListVisit, &List);
    return List.Count;
}

/* Removes the attachment folder of a message and everything in it */
VOID
PhStoreRemoveAttachments(
    _In_ DWORD MessageId)
{
    WCHAR Folder[MAX_PATH];

    if (!PhStoreGetAttachmentFolder(MessageId, FALSE, Folder, _countof(Folder)))
        return;

    PhStoreWalkAttachments(MessageId, PhStoreDeleteVisit, NULL);
    RemoveDirectoryW(Folder);
}

BOOL
PhStoreDelete(
    _In_ DWORD MessageId)
{
    DWORD Flags = 0;
    LONG Slot;

    EnterCriticalSection(&PhStoreLock);

    Slot = PhStoreFind(MessageId);
    if (Slot >= 0)
    {
        Flags = PhRecords[Slot].Message.Flags;
        ZeroMemory(&PhRecords[Slot], sizeof(PhRecords[Slot]));
        PhStoreFlushRecord(Slot);
    }

    LeaveCriticalSection(&PhStoreLock);

    if (Flags & PHONEOM_MESSAGE_MMS)
        PhStoreRemoveAttachments(MessageId);

    return Slot >= 0;
}

/* Returns messages with ids at or after FirstId in ascending id order */
DWORD
PhStoreEnumerate(
    _In_ DWORD FirstId,
    _Out_writes_to_(MaxMessages, return) PPHONEOM_MESSAGE Messages,
    _In_ DWORD MaxMessages)
{
    DWORD Count = 0, Cursor = FirstId, i;
    LONG Best;

    EnterCriticalSection(&PhStoreLock);

    while (Count < MaxMessages)
    {
        Best = -1;
        for (i = 0; i < PhRecordCount; i++)
        {
            if (!PhRecords[i].InUse || PhRecords[i].Message.MessageId < Cursor)
                continue;

            if (Best < 0 || PhRecords[i].Message.MessageId < PhRecords[Best].Message.MessageId)
                Best = (LONG)i;
        }

        if (Best < 0)
            break;

        CopyMemory(&Messages[Count++], &PhRecords[Best].Message, sizeof(Messages[0]));
        Cursor = PhRecords[Best].Message.MessageId + 1;
        if (Cursor == 0)
            break;
    }

    LeaveCriticalSection(&PhStoreLock);
    return Count;
}
