/*
 * PROJECT:     ReactOS Phone Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Persistent message store
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "phonesvc.h"
#include <shlobj.h>

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

static
BOOL
PhStoreOpenFile(VOID)
{
    WCHAR Path[MAX_PATH];
    LARGE_INTEGER FileSize;
    DWORD Read, Count;
    HRESULT hr;

    hr = SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, Path);
    if (FAILED(hr))
        return FALSE;

    if (FAILED(StringCchCatW(Path, _countof(Path), L"\\ReactOS")))
        return FALSE;
    CreateDirectoryW(Path, NULL);

    if (FAILED(StringCchCatW(Path, _countof(Path), L"\\Phone")))
        return FALSE;
    CreateDirectoryW(Path, NULL);

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
PhStoreDelete(
    _In_ DWORD MessageId)
{
    LONG Slot;

    EnterCriticalSection(&PhStoreLock);

    Slot = PhStoreFind(MessageId);
    if (Slot >= 0)
    {
        ZeroMemory(&PhRecords[Slot], sizeof(PhRecords[Slot]));
        PhStoreFlushRecord(Slot);
    }

    LeaveCriticalSection(&PhStoreLock);
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
