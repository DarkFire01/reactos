/*
 * PROJECT:     ReactOS WPP In-flight Trace Recorder
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Recorder entry points that WPP traced drivers import
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Drivers built with the WPP recorder start it from DriverEntry and send
 * every trace message to it. ReactOS has no tracing back end to replay the
 * messages into, so the recorder keeps none. Logs a driver creates are real
 * handles that can be named and deleted, they just stay empty, and there is
 * never a default log.
 */

#include <ntddk.h>

#define TAG_WPP_RECORDER        'rRpW'
#define WPP_LOG_MAGIC           'gLpW'
#define WPP_LOG_IDENTIFIER_SIZE 16

typedef struct _WPP_RECORDER_LOG
{
    ULONG Magic;
    CHAR Identifier[WPP_LOG_IDENTIFIER_SIZE + 1];
} WPP_RECORDER_LOG, *PWPP_RECORDER_LOG;

typedef struct _WPP_TRIAGE_INFO
{
    ULONG WppAutoLogHeaderSize;
    ULONG WppDriverContextOffset;
    ULONG WppAutoLogHeaderSizeOffset;
    ULONG WppSizeOfAutoLogHeaderSizeField;
    ULONG WppDriverContextSize;
} WPP_TRIAGE_INFO, *PWPP_TRIAGE_INFO;

/* The identifier is the only part of the creation parameters an empty log keeps */
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

static
VOID
WppRecorderSetName(
    _Inout_ PWPP_RECORDER_LOG Log,
    _In_reads_opt_(MaximumLength) PCSTR Identifier,
    _In_ SIZE_T MaximumLength)
{
    SIZE_T Length = 0;

    RtlZeroMemory(Log->Identifier, sizeof(Log->Identifier));
    if (Identifier == NULL)
        return;

    while (Length < min(MaximumLength, WPP_LOG_IDENTIFIER_SIZE) && Identifier[Length] != ANSI_NULL)
        Length++;
    RtlCopyMemory(Log->Identifier, Identifier, Length);
}

VOID
NTAPI
WppAutoLogStart(
    _In_ PVOID WppCb,
    _In_ PDRIVER_OBJECT DrvObj,
    _In_ PCUNICODE_STRING RegPath)
{
    UNREFERENCED_PARAMETER(WppCb);
    UNREFERENCED_PARAMETER(DrvObj);
    UNREFERENCED_PARAMETER(RegPath);
}

VOID
NTAPI
WppAutoLogStop(
    _In_ PVOID WppCb,
    _In_ PDRIVER_OBJECT DrvObj)
{
    UNREFERENCED_PARAMETER(WppCb);
    UNREFERENCED_PARAMETER(DrvObj);
}

/* The message is dropped, which is what a recorder with no room for it does too */
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
    UNREFERENCED_PARAMETER(AutoLogContext);
    UNREFERENCED_PARAMETER(MessageLevel);
    UNREFERENCED_PARAMETER(MessageFlags);
    UNREFERENCED_PARAMETER(MessageGuid);
    UNREFERENCED_PARAMETER(MessageNumber);

    return STATUS_SUCCESS;
}

VOID
NTAPI
imp_WppRecorderConfigure(
    _In_ PVOID WppCb,
    _In_ PVOID ConfigureParams)
{
    UNREFERENCED_PARAMETER(WppCb);
    UNREFERENCED_PARAMETER(ConfigureParams);
}

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
    PWPP_RECORDER_LOG Log;

    UNREFERENCED_PARAMETER(WppCb);

    if (CreateParams == NULL || RecorderLog == NULL)
        return STATUS_INVALID_PARAMETER;

    *RecorderLog = NULL;

    Log = ExAllocatePoolZero(NonPagedPool, sizeof(*Log), TAG_WPP_RECORDER);
    if (Log == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Log->Magic = WPP_LOG_MAGIC;
    if (CreateParams->Size >= RTL_SIZEOF_THROUGH_FIELD(WPP_RECORDER_LOG_CREATE_PARAMS, LogIdentifier))
        WppRecorderSetName(Log, CreateParams->LogIdentifier, CreateParams->LogIdentifierSize);

    *RecorderLog = Log;
    return STATUS_SUCCESS;
}

VOID
NTAPI
imp_WppRecorderLogDelete(
    _In_ PVOID WppCb,
    _In_opt_ PVOID RecorderLog)
{
    PWPP_RECORDER_LOG Log = RecorderLog;

    UNREFERENCED_PARAMETER(WppCb);

    if (Log == NULL || Log->Magic != WPP_LOG_MAGIC)
        return;

    Log->Magic = 0;
    ExFreePoolWithTag(Log, TAG_WPP_RECORDER);
}

VOID
NTAPI
imp_WppRecorderLogSetIdentifier(
    _In_ PVOID WppCb,
    _In_opt_ PVOID RecorderLog,
    _In_opt_ PCSTR LogIdentifier)
{
    PWPP_RECORDER_LOG Log = RecorderLog;

    UNREFERENCED_PARAMETER(WppCb);

    if (Log != NULL && Log->Magic == WPP_LOG_MAGIC)
        WppRecorderSetName(Log, LogIdentifier, WPP_LOG_IDENTIFIER_SIZE);
}

PVOID
NTAPI
imp_WppRecorderLogGetDefault(
    _In_ PVOID WppCb)
{
    UNREFERENCED_PARAMETER(WppCb);

    return NULL;
}

BOOLEAN
NTAPI
imp_WppRecorderIsDefaultLogAvailable(
    _In_ PVOID WppCb)
{
    UNREFERENCED_PARAMETER(WppCb);

    return FALSE;
}

PVOID
NTAPI
imp_WppRecorderGetCounterHandle(
    _In_ PVOID WppCb)
{
    UNREFERENCED_PARAMETER(WppCb);

    return NULL;
}

NTSTATUS
NTAPI
imp_WppRecorderLinkCounters(
    _In_ PVOID WppCb,
    _In_opt_ PVOID CounterOwner)
{
    UNREFERENCED_PARAMETER(WppCb);
    UNREFERENCED_PARAMETER(CounterOwner);

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
imp_WppRecorderGetTriageInfo(
    _In_ PVOID WppCb,
    _Out_ PWPP_TRIAGE_INFO WppTriageInfo)
{
    UNREFERENCED_PARAMETER(WppCb);

    if (WppTriageInfo == NULL)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(WppTriageInfo, sizeof(*WppTriageInfo));
    return STATUS_SUCCESS;
}

/* There is never anything recorded to hand to a debugger or dump */
NTSTATUS
NTAPI
imp_WppRecorderDumpLiveDriverData(
    _In_ PVOID WppCb,
    _Out_ PVOID *OutBuffer,
    _Out_ PULONG OutBufferLength,
    _Out_ LPGUID Guid)
{
    UNREFERENCED_PARAMETER(WppCb);

    *OutBuffer = NULL;
    *OutBufferLength = 0;
    RtlZeroMemory(Guid, sizeof(*Guid));
    return STATUS_NOT_FOUND;
}

NTSTATUS
NTAPI
imp_WppRecorderLogDumpLiveData(
    _In_ PVOID WppCb,
    _In_opt_ PVOID RecorderLog,
    _Out_ PVOID *OutBuffer,
    _Out_ PULONG OutBufferLength,
    _Out_ LPGUID Guid)
{
    UNREFERENCED_PARAMETER(RecorderLog);

    return imp_WppRecorderDumpLiveDriverData(WppCb, OutBuffer, OutBufferLength, Guid);
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
