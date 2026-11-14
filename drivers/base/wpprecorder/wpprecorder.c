/*
 * PROJECT:     ReactOS WPP In-flight Trace Recorder
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     In memory trace logs for drivers built with the WPP recorder
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntddk.h>
#include <ntstrsafe.h>

#define NDEBUG
#include <debug.h>

#define WPP_CONTEXT_TAG             'gLpW'
#define WPP_RECORD_SIGNATURE        0x524C
#define WPP_LOG_IDENTIFIER_SIZE     16
#define WPP_DRIVER_NAME_SIZE        64
#define WPP_PDB_NAME_SIZE           64
#define WPP_MAX_ARGUMENT_BYTES      0x100
#define WPP_LEVEL_ERROR             2

/* Registry defaults and limits, read from the driver's Parameters key */
#define WPP_DEFAULT_LOG_PAGES       1
#define WPP_MAX_LOG_PAGES           16
#define WPP_DEFAULT_MIN_BYTES       0x100
#define WPP_DEFAULT_MAX_BYTES       0x2000
#define WPP_LIMIT_BUFFER_BYTES      0x10000
#define WPP_DEFAULT_RETAINED_LOGS   2
#define WPP_VERIFIER_RETAINED_LOGS  4
#define WPP_MAX_RETAINED_LOGS       100

/* The parts of a driver's WPP control block the recorder reads and fills */
typedef struct _WPP_RECORDER_CONTROL
{
    PVOID Callback;
    LPCGUID ControlGuid;
    UCHAR Reserved[48];
    PVOID AutoLogContext;
    USHORT AutoLogVerboseEnabled;
    USHORT AutoLogAttachToMiniDump;
} WPP_RECORDER_CONTROL, *PWPP_RECORDER_CONTROL;

typedef struct _WPP_PARTITION
{
    PUCHAR Buffer;
    ULONG Size;
    volatile LONG State;
} WPP_PARTITION, *PWPP_PARTITION;

typedef struct _WPP_DRIVER_CONTEXT WPP_DRIVER_CONTEXT, *PWPP_DRIVER_CONTEXT;

/* Log header; debuggers find it through the triage offsets, so the layout is fixed */
typedef struct _WPP_LOG
{
    CHAR Identifier[WPP_LOG_IDENTIFIER_SIZE];
    ULONG_PTR IdentifierAppendValue;
    BOOLEAN IdentifierAppendValueSet;
    ULONG Tag;
    LIST_ENTRY Link;
    UCHAR DumpRecord[48];
    USHORT Index;
    BOOLEAN Deleted;
    LIST_ENTRY DeletedLink;
    PWPP_DRIVER_CONTEXT Driver;
    WPP_PARTITION ErrorPartition;
    WPP_PARTITION MainPartition;
    ULONG TotalSize;
    ULONG Reserved;
} WPP_LOG, *PWPP_LOG;

struct _WPP_DRIVER_CONTEXT
{
    volatile LONG Sequence;
    PVOID ImageBase;
    ULONG ImageSize;
    BOOLEAN Reserved1;
    GUID ControlGuid;
    BOOLEAN AttachToMiniDump;
    ULONG MinBufferBytes;
    ULONG MaxBufferBytes;
    BOOLEAN Configured;
    UCHAR DumpRecord[48];
    CHAR DriverName[WPP_DRIVER_NAME_SIZE + 4];
    USHORT LogsCreated;
    USHORT LogIndex;
    KSPIN_LOCK Lock;
    LIST_ENTRY Logs;
    PWPP_LOG DefaultLog;
    volatile LONG *SequencePointer;
    volatile LONG LinkCount;
    BOOLEAN ReplayEnabled;
    BOOLEAN Asserted;
    BOOLEAN DebugPrint;
    GUID PdbGuid;
    ULONG PdbAge;
    CHAR PdbName[WPP_PDB_NAME_SIZE];
    ULONG Reserved2;
    LIST_ENTRY DeletedLogs;
    ULONG DeletedCount;
    ULONG RetainDeleted;
};

typedef struct _WPP_RECORD_HEADER
{
    USHORT Signature;
    USHORT Size;
    ULONG Sequence;
    USHORT WrapStamp;
    USHORT MessageNumber;
    GUID MessageGuid;
} WPP_RECORD_HEADER, *PWPP_RECORD_HEADER;

typedef struct _WPP_TRIAGE_INFO
{
    ULONG WppAutoLogHeaderSize;
    ULONG WppDriverContextOffset;
    ULONG WppAutoLogHeaderSizeOffset;
    ULONG WppSizeOfAutoLogHeaderSizeField;
    ULONG WppDriverContextSize;
} WPP_TRIAGE_INFO, *PWPP_TRIAGE_INFO;

typedef struct _WPP_RECORDER_LOG_CREATE_PARAMS
{
    ULONG Size;
    ULONG LogTag;
    ULONG TotalBufferSize;
    ULONG ErrorPartitionSize;
    ULONG_PTR LogIdentifierAppendValue;
    BOOLEAN LogIdentifierAppendValueSet;
    ULONG LogIdentifierSize;
    CHAR LogIdentifier[WPP_LOG_IDENTIFIER_SIZE];
} WPP_RECORDER_LOG_CREATE_PARAMS, *PWPP_RECORDER_LOG_CREATE_PARAMS;

typedef struct _WPP_RECORDER_CONFIGURE_PARAMS
{
    ULONG Size;
    BOOLEAN CreateDefaultLog;
} WPP_RECORDER_CONFIGURE_PARAMS, *PWPP_RECORDER_CONFIGURE_PARAMS;

C_ASSERT(sizeof(WPP_RECORD_HEADER) == 28);
#ifdef _WIN64
C_ASSERT(FIELD_OFFSET(WPP_LOG, Driver) == 120);
C_ASSERT(FIELD_OFFSET(WPP_LOG, TotalSize) == 160);
C_ASSERT(sizeof(WPP_LOG) == 168);
C_ASSERT(FIELD_OFFSET(WPP_RECORDER_CONTROL, AutoLogContext) == 64);
#endif

static
PWPP_DRIVER_CONTEXT
NTAPI
WppGetDriverContext(
    _In_ PVOID WppCb)
{
    PWPP_LOG Log = ((PWPP_RECORDER_CONTROL)WppCb)->AutoLogContext;

    return Log ? Log->Driver : NULL;
}

static
ULONG
NTAPI
WppReadDword(
    _In_ HANDLE Key,
    _In_ PCWSTR Name,
    _In_ ULONG Default)
{
    UCHAR Buffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    UNICODE_STRING ValueName;
    ULONG Length;

    if (!Key)
        return Default;

    RtlInitUnicodeString(&ValueName, Name);
    if (!NT_SUCCESS(ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Info, sizeof(Buffer), &Length)) ||
        Info->Type != REG_DWORD || Info->DataLength != sizeof(ULONG))
    {
        return Default;
    }

    return *(PULONG)Info->Data;
}

/* Reads the tuning values; a zero means the value is not set */
static
VOID
NTAPI
WppReadSettings(
    _In_ PCUNICODE_STRING RegistryPath,
    _In_ PDRIVER_OBJECT DriverObject,
    _Inout_ PWPP_RECORDER_CONTROL Control,
    _Inout_ PWPP_DRIVER_CONTEXT Context,
    _Out_ PULONG DefaultLogBytes)
{
    UNICODE_STRING ParametersName = RTL_CONSTANT_STRING(L"Parameters");
    OBJECT_ATTRIBUTES Attributes;
    HANDLE ServiceKey = NULL;
    HANDLE Key = NULL;
    ULONG Pages;
    ULONG MinBytes;
    ULONG MaxBytes;
    ULONG Retain;

    InitializeObjectAttributes(&Attributes, (PUNICODE_STRING)RegistryPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&ServiceKey, KEY_READ, &Attributes)))
    {
        InitializeObjectAttributes(&Attributes, &ParametersName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, ServiceKey, NULL);
        if (!NT_SUCCESS(ZwOpenKey(&Key, KEY_READ, &Attributes)))
            Key = NULL;
    }

    Control->AutoLogVerboseEnabled = (USHORT)WppReadDword(Key, L"VerboseOn", 0);
    Control->AutoLogAttachToMiniDump = (USHORT)WppReadDword(Key, L"ForceLogsInMiniDump", 0);
    Context->ReplayEnabled = WppReadDword(Key, L"WppRecorder_ReplayEnabled", 1) != 0;
    Context->DebugPrint = WppReadDword(Key, L"WppRecorder_DebugPrint", 0) != 0;

    Pages = WppReadDword(Key, L"LogPages", WPP_DEFAULT_LOG_PAGES);
    Pages = min(max(Pages, 1), WPP_MAX_LOG_PAGES);

    /* An unset bound gives way to the one that was set */
    MinBytes = WppReadDword(Key, L"WppRecorder_PerBufferMinBytes", 0);
    MaxBytes = WppReadDword(Key, L"WppRecorder_PerBufferMaxBytes", 0);
    if (MinBytes && MaxBytes && MinBytes > MaxBytes)
        MaxBytes = MinBytes;
    else if (MinBytes && !MaxBytes && MinBytes > WPP_DEFAULT_MAX_BYTES)
        MaxBytes = MinBytes;
    else if (!MinBytes && MaxBytes && MaxBytes < WPP_DEFAULT_MIN_BYTES)
        MinBytes = MaxBytes;
    if (!MinBytes)
        MinBytes = WPP_DEFAULT_MIN_BYTES;
    if (!MaxBytes)
        MaxBytes = WPP_DEFAULT_MAX_BYTES;
    Context->MinBufferBytes = min(MinBytes, WPP_LIMIT_BUFFER_BYTES);
    Context->MaxBufferBytes = min(MaxBytes, WPP_LIMIT_BUFFER_BYTES);

    Retain = WppReadDword(Key, L"WppRecorder_RetainLastNDeletedLogs", WPP_DEFAULT_RETAINED_LOGS);
    if (DriverObject && MmIsDriverVerifying(DriverObject) && Retain == WPP_DEFAULT_RETAINED_LOGS)
        Retain = WPP_VERIFIER_RETAINED_LOGS;
    Context->RetainDeleted = min(Retain, WPP_MAX_RETAINED_LOGS);

    *DefaultLogBytes = Pages * PAGE_SIZE;

    if (Key)
        ZwClose(Key);
    if (ServiceKey)
        ZwClose(ServiceKey);
}

static
PWPP_LOG
NTAPI
WppCreateLog(
    _In_ PWPP_DRIVER_CONTEXT Context,
    _In_ ULONG DataSize,
    _In_ ULONG ErrorSize,
    _In_ ULONG Tag)
{
    PWPP_LOG Log;
    KIRQL OldIrql;

    if (!Tag)
        Tag = WPP_CONTEXT_TAG;

    Log = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*Log) + DataSize, Tag);
    if (!Log)
        return NULL;

    RtlZeroMemory(Log, sizeof(*Log) + DataSize);
    Log->Driver = Context;
    Log->Tag = Tag;
    Log->TotalSize = DataSize;

    /* The error partition, when there is one, sits at the end of the data */
    Log->MainPartition.Buffer = (PUCHAR)(Log + 1);
    Log->MainPartition.Size = DataSize - ErrorSize;
    Log->ErrorPartition.Buffer = (PUCHAR)(Log + 1) + DataSize - ErrorSize;
    Log->ErrorPartition.Size = ErrorSize;

    KeAcquireSpinLock(&Context->Lock, &OldIrql);
    InsertTailList(&Context->Logs, &Log->Link);
    Context->LogsCreated++;
    Log->Index = ++Context->LogIndex;
    KeReleaseSpinLock(&Context->Lock, OldIrql);

    return Log;
}

static
VOID
NTAPI
WppFreeLog(
    _In_ PWPP_LOG Log)
{
    PWPP_DRIVER_CONTEXT Context = Log->Driver;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Context->Lock, &OldIrql);
    RemoveEntryList(&Log->Link);
    KeReleaseSpinLock(&Context->Lock, OldIrql);

    ExFreePoolWithTag(Log, Log->Tag);
}

/* Deleted logs are kept a while so a dump still shows what led up to it */
static
VOID
NTAPI
WppRetireLog(
    _In_ PWPP_LOG Log)
{
    PWPP_DRIVER_CONTEXT Context = Log->Driver;
    PWPP_LOG Evicted = NULL;
    KIRQL OldIrql;

    Log->Deleted = TRUE;
    if (!Context->RetainDeleted)
    {
        WppFreeLog(Log);
        return;
    }

    KeAcquireSpinLock(&Context->Lock, &OldIrql);
    InsertTailList(&Context->DeletedLogs, &Log->DeletedLink);
    if (Context->DeletedCount < Context->RetainDeleted)
        Context->DeletedCount++;
    else
        Evicted = CONTAINING_RECORD(RemoveHeadList(&Context->DeletedLogs), WPP_LOG, DeletedLink);
    KeReleaseSpinLock(&Context->Lock, OldIrql);

    if (Evicted)
        WppFreeLog(Evicted);
}

static
VOID
NTAPI
WppDeleteDriverContext(
    _In_ PWPP_DRIVER_CONTEXT Context)
{
    while (!IsListEmpty(&Context->Logs))
        WppFreeLog(CONTAINING_RECORD(Context->Logs.Flink, WPP_LOG, Link));

    ExFreePoolWithTag(Context, WPP_CONTEXT_TAG);
}

/* The service name, the last part of the registry path */
static
VOID
NTAPI
WppCopyDriverName(
    _In_opt_ PCUNICODE_STRING RegistryPath,
    _Out_writes_(WPP_DRIVER_NAME_SIZE) PCHAR Name)
{
    USHORT Count;
    USHORT Start;
    USHORT Index;

    if (!RegistryPath || !RegistryPath->Buffer)
        return;

    Count = RegistryPath->Length / sizeof(WCHAR);
    Start = Count;
    while (Start && RegistryPath->Buffer[Start - 1] != L'\\')
        Start--;

    for (Index = 0; Index < Count - Start && Index < WPP_DRIVER_NAME_SIZE - 1; Index++)
        Name[Index] = (CHAR)RegistryPath->Buffer[Start + Index];
}

VOID
NTAPI
WppAutoLogStart(
    _In_ PVOID WppCb,
    _In_ PDRIVER_OBJECT DrvObj,
    _In_ PCUNICODE_STRING RegPath)
{
    PWPP_RECORDER_CONTROL Control = WppCb;
    PWPP_DRIVER_CONTEXT Context;
    PWPP_LOG Log;
    ULONG DefaultLogBytes;

    Control->AutoLogContext = NULL;
    Control->AutoLogVerboseEnabled = 0;
    Control->AutoLogAttachToMiniDump = 0;

    Context = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*Context), WPP_CONTEXT_TAG);
    if (!Context)
    {
        DbgPrint("Error: failed to allocate memory for autoLogContext\n");
        return;
    }

    RtlZeroMemory(Context, sizeof(*Context));
    Context->SequencePointer = &Context->Sequence;
    KeInitializeSpinLock(&Context->Lock);
    InitializeListHead(&Context->Logs);
    InitializeListHead(&Context->DeletedLogs);

    WppReadSettings(RegPath, DrvObj, Control, Context, &DefaultLogBytes);
    if (Control->ControlGuid)
        Context->ControlGuid = *Control->ControlGuid;
    Context->AttachToMiniDump = (BOOLEAN)Control->AutoLogAttachToMiniDump;
    WppCopyDriverName(RegPath, Context->DriverName);

    if (DrvObj)
    {
        Context->ImageBase = DrvObj->DriverStart;
        Context->ImageSize = DrvObj->DriverSize;
    }

    Log = WppCreateLog(Context, DefaultLogBytes - sizeof(WPP_LOG), 0, 0);
    if (!Log)
    {
        DbgPrint("Error: failed to allocate memory for WPP recorder\n");
        ExFreePoolWithTag(Context, WPP_CONTEXT_TAG);
        return;
    }

    RtlStringCbCopyA(Log->Identifier, sizeof(Log->Identifier), "DriverLog");
    Context->DefaultLog = Log;
    Control->AutoLogContext = Log;

    DPRINT("WPP: recorder started for %s, %lu byte log\n", Context->DriverName, DefaultLogBytes);
}

VOID
NTAPI
WppAutoLogStop(
    _In_ PVOID WppCb,
    _In_ PDRIVER_OBJECT DrvObj)
{
    PWPP_RECORDER_CONTROL Control = WppCb;
    PWPP_DRIVER_CONTEXT Context = WppGetDriverContext(WppCb);
    PLIST_ENTRY Entry;
    PWPP_LOG Log;

    UNREFERENCED_PARAMETER(DrvObj);

    if (!Context)
        return;

    Control->AutoLogContext = NULL;

    for (Entry = Context->Logs.Flink; Entry != &Context->Logs; Entry = Entry->Flink)
    {
        Log = CONTAINING_RECORD(Entry, WPP_LOG, Link);
        Log->Deleted = TRUE;
    }

    WppDeleteDriverContext(Context);
}

static
VOID
NTAPI
WppEchoRecord(
    _In_ PWPP_LOG Log,
    _In_ UCHAR Level,
    _In_ PWPP_RECORD_HEADER Record)
{
    CHAR Line[3 * 32 + 1];
    PUCHAR Data = (PUCHAR)(Record + 1);
    ULONG Length = Record->Size - sizeof(*Record);
    ULONG Index;

    Line[0] = ANSI_NULL;
    for (Index = 0; Index < Length && Index < 32; Index++)
        RtlStringCbPrintfA(Line + Index * 3, sizeof(Line) - Index * 3, "%02x ", Data[Index]);

    DbgPrint("WPP %s {%08lx-%04x} #%u L%u: %s\n",
             Log->Driver->DriverName,
             Record->MessageGuid.Data1,
             Record->MessageGuid.Data2,
             Record->MessageNumber,
             Level,
             Line);
}

/* Arguments arrive as (pointer, length) pairs ended by a NULL pointer */
NTSTATUS
__cdecl
WppAutoLogTrace(
    _In_opt_ PVOID AutoLogContext,
    _In_ UCHAR MessageLevel,
    _In_ ULONG MessageFlags,
    _In_ LPGUID MessageGuid,
    _In_ USHORT MessageNumber,
    ...)
{
    PWPP_LOG Log = AutoLogContext;
    PWPP_PARTITION Partition;
    PWPP_RECORD_HEADER Record;
    va_list Arguments;
    PVOID Data;
    SIZE_T Length;
    SIZE_T Payload = 0;
    ULONG RecordSize;
    LONG Old;
    LONG New;
    ULONG Offset;
    PUCHAR Cursor;

    UNREFERENCED_PARAMETER(MessageFlags);

    if (!Log || Log->Deleted)
        return STATUS_UNSUCCESSFUL;

    if (MessageLevel == WPP_LEVEL_ERROR && Log->ErrorPartition.Size)
        Partition = &Log->ErrorPartition;
    else
        Partition = &Log->MainPartition;

    va_start(Arguments, MessageNumber);
    while ((Data = va_arg(Arguments, PVOID)) != NULL)
    {
        Length = va_arg(Arguments, SIZE_T);
        if (Length > WPP_MAX_ARGUMENT_BYTES)
        {
            Payload = MAXULONG;
            break;
        }
        Payload += (USHORT)Length;
    }
    va_end(Arguments);

    Payload = (Payload + 3) & ~(SIZE_T)3;
    if (Payload > WPP_MAX_ARGUMENT_BYTES || Payload + sizeof(*Record) > Partition->Size)
    {
        /* A dropped message still takes a sequence number, the gap shows it */
        InterlockedIncrement(Log->Driver->SequencePointer);
        return STATUS_UNSUCCESSFUL;
    }

    RecordSize = (ULONG)(Payload + sizeof(*Record));

    /* Reserve space: low word is the write offset, high word counts wraps */
    do
    {
        Old = Partition->State;
        Offset = (USHORT)Old;
        if (Offset + RecordSize > Partition->Size)
        {
            New = (LONG)((((ULONG)Old & 0xFFFF0000) + 0x10000) | RecordSize);
            Offset = 0;
        }
        else
        {
            New = (LONG)(((ULONG)Old & 0xFFFF0000) | (Offset + RecordSize));
        }
    } while (InterlockedCompareExchange(&Partition->State, New, Old) != Old);

    Record = (PWPP_RECORD_HEADER)(Partition->Buffer + Offset);
    Record->Signature = WPP_RECORD_SIGNATURE;
    Record->Size = (USHORT)RecordSize;
    Record->WrapStamp = (USHORT)((ULONG)New >> 16);
    Record->MessageNumber = MessageNumber;
    Record->Sequence = (ULONG)InterlockedIncrement(Log->Driver->SequencePointer);
    Record->MessageGuid = *MessageGuid;

    Cursor = (PUCHAR)(Record + 1);
    va_start(Arguments, MessageNumber);
    while ((Data = va_arg(Arguments, PVOID)) != NULL)
    {
        Length = va_arg(Arguments, SIZE_T);
        if (Length)
        {
            RtlCopyMemory(Cursor, Data, Length);
            Cursor += Length;
        }
    }
    va_end(Arguments);

    if (Log->Driver->DebugPrint)
        WppEchoRecord(Log, MessageLevel, Record);

    return STATUS_SUCCESS;
}

/* Without a default log the driver logs only into the ones it creates */
VOID
NTAPI
imp_WppRecorderConfigure(
    _In_ PVOID WppCb,
    _In_ PWPP_RECORDER_CONFIGURE_PARAMS ConfigureParams)
{
    PWPP_RECORDER_CONTROL Control = WppCb;
    PWPP_DRIVER_CONTEXT Context = WppGetDriverContext(WppCb);
    PWPP_LOG Log;

    if (!Context || !Context->DefaultLog)
        return;

    Context->Configured = TRUE;
    if (!ConfigureParams || ConfigureParams->CreateDefaultLog)
        return;

    Context->DefaultLog->Deleted = TRUE;
    WppFreeLog(Context->DefaultLog);

    Log = WppCreateLog(Context, 0, 0, 0);
    if (!Log)
    {
        DbgPrint("Error: failed to re-allocate memory for WPP recorder\n");
        WppDeleteDriverContext(Context);
    }
    else
    {
        Context->DefaultLog = Log;
    }

    Control->AutoLogContext = Log;
}

/* Replay feeds recorded messages to a live WPP session; there is no ETW consumer to feed */
VOID
NTAPI
imp_WppRecorderReplay(
    _In_ PVOID WppCb,
    _In_ ULONG64 WppTraceHandle,
    _In_ ULONG EnableFlags,
    _In_ UCHAR EnableLevel)
{
    UNREFERENCED_PARAMETER(WppCb);
    UNREFERENCED_PARAMETER(WppTraceHandle);
    UNREFERENCED_PARAMETER(EnableFlags);
    UNREFERENCED_PARAMETER(EnableLevel);
}

NTSTATUS
NTAPI
imp_WppRecorderLogCreate(
    _In_ PVOID WppCb,
    _In_ PWPP_RECORDER_LOG_CREATE_PARAMS CreateParams,
    _Out_ PVOID *RecorderLog)
{
    PWPP_DRIVER_CONTEXT Context = WppGetDriverContext(WppCb);
    PWPP_LOG Log;
    ULONG Size;
    ULONG ErrorSize;

    *RecorderLog = NULL;

    if (!Context)
        return STATUS_INSUFFICIENT_RESOURCES;

    Size = min(max(CreateParams->TotalBufferSize, Context->MinBufferBytes), Context->MaxBufferBytes);
    ErrorSize = min(CreateParams->ErrorPartitionSize, Size / 2);

    Log = WppCreateLog(Context, Size, ErrorSize, CreateParams->LogTag);
    if (!Log)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlStringCbCopyNA(Log->Identifier,
                      sizeof(Log->Identifier),
                      CreateParams->LogIdentifier,
                      min(CreateParams->LogIdentifierSize, sizeof(CreateParams->LogIdentifier)));
    Log->IdentifierAppendValueSet = CreateParams->LogIdentifierAppendValueSet;
    Log->IdentifierAppendValue = CreateParams->LogIdentifierAppendValue;

    *RecorderLog = Log;
    return STATUS_SUCCESS;
}

VOID
NTAPI
imp_WppRecorderLogDelete(
    _In_ PVOID WppCb,
    _In_opt_ PVOID RecorderLog)
{
    PWPP_LOG Log = RecorderLog;

    UNREFERENCED_PARAMETER(WppCb);

    if (Log && !Log->Deleted && Log != Log->Driver->DefaultLog)
        WppRetireLog(Log);
}

VOID
NTAPI
imp_WppRecorderLogSetIdentifier(
    _In_ PVOID WppCb,
    _In_opt_ PVOID RecorderLog,
    _In_opt_ PCSTR LogIdentifier)
{
    PWPP_LOG Log = RecorderLog;

    UNREFERENCED_PARAMETER(WppCb);

    if (Log && LogIdentifier && Log != Log->Driver->DefaultLog)
        RtlStringCbCopyA(Log->Identifier, sizeof(Log->Identifier), LogIdentifier);
}

PVOID
NTAPI
imp_WppRecorderLogGetDefault(
    _In_ PVOID WppCb)
{
    PWPP_DRIVER_CONTEXT Context = WppGetDriverContext(WppCb);

    return Context ? Context->DefaultLog : NULL;
}

BOOLEAN
NTAPI
imp_WppRecorderIsDefaultLogAvailable(
    _In_ PVOID WppCb)
{
    PWPP_DRIVER_CONTEXT Context = WppGetDriverContext(WppCb);

    return Context && Context->DefaultLog && Context->DefaultLog->MainPartition.Size != 0;
}

/* The recorder has no counters of its own to hand out */
PVOID
NTAPI
imp_WppRecorderGetCounterHandle(
    _In_ PVOID WppCb)
{
    UNREFERENCED_PARAMETER(WppCb);

    return NULL;
}

/* Lets several components share one sequence so their records interleave in order */
NTSTATUS
NTAPI
imp_WppRecorderLinkCounters(
    _In_ PVOID WppCb,
    _In_opt_ PVOID CounterOwner)
{
    PWPP_DRIVER_CONTEXT Context = WppGetDriverContext(WppCb);
    volatile LONG *Counter = CounterOwner;

    if (!Counter || !Context)
        return STATUS_UNSUCCESSFUL;

    if (InterlockedIncrement(&Context->LinkCount) != 1)
        return STATUS_UNSUCCESSFUL;

    *Counter = max(Context->Sequence, *Counter) + 10;
    Context->SequencePointer = Counter;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
imp_WppRecorderGetTriageInfo(
    _In_ PVOID WppCb,
    _Out_ PWPP_TRIAGE_INFO WppTriageInfo)
{
    UNREFERENCED_PARAMETER(WppCb);

    if (!WppTriageInfo)
        return STATUS_UNSUCCESSFUL;

    WppTriageInfo->WppAutoLogHeaderSize = sizeof(WPP_LOG);
    WppTriageInfo->WppDriverContextOffset = FIELD_OFFSET(WPP_LOG, Driver);
    WppTriageInfo->WppAutoLogHeaderSizeOffset = FIELD_OFFSET(WPP_LOG, TotalSize);
    WppTriageInfo->WppSizeOfAutoLogHeaderSizeField = sizeof(ULONG);
    WppTriageInfo->WppDriverContextSize = sizeof(WPP_DRIVER_CONTEXT);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
imp_WppRecorderDumpLiveDriverData(
    _In_ PVOID WppCb,
    _Out_ PVOID *OutBuffer,
    _Out_ PULONG OutBufferLength,
    _Out_ LPGUID Guid)
{
    PWPP_DRIVER_CONTEXT Context = WppGetDriverContext(WppCb);

    *OutBuffer = NULL;
    *OutBufferLength = 0;
    if (!Context)
        return STATUS_UNSUCCESSFUL;

    *OutBuffer = Context->DriverName;
    *OutBufferLength = sizeof(Context->DriverName);
    *Guid = Context->ControlGuid;
    return STATUS_SUCCESS;
}

/* The log goes out whole, tagged with the control GUID made unique per log */
NTSTATUS
NTAPI
imp_WppRecorderLogDumpLiveData(
    _In_ PVOID WppCb,
    _In_opt_ PVOID RecorderLog,
    _Out_ PVOID *OutBuffer,
    _Out_ PULONG OutBufferLength,
    _Out_ LPGUID Guid)
{
    PWPP_LOG Log = RecorderLog;

    UNREFERENCED_PARAMETER(WppCb);

    if (!Log)
        return STATUS_UNSUCCESSFUL;

    *OutBuffer = Log;
    *OutBufferLength = Log->TotalSize + sizeof(WPP_LOG);
    *Guid = Log->Driver->ControlGuid;
    Guid->Data2 ^= Log->Index;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DllInitialize(
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DllUnload(VOID)
{
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    return STATUS_SUCCESS;
}
