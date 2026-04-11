/*
 * PROJECT:     ReactOS Runtime Library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Minimal RtlUnwindEx / RtlRestoreContext for ARM64 bring-up
 * COPYRIGHT:   Copyright 2025 ReactOS Contributors
 */

#include <rtl.h>

#define NDEBUG
#include <debug.h>

VOID
NTAPI
RtlRestoreContext(
    _In_ PCONTEXT ContextRecord,
    _In_opt_ PEXCEPTION_RECORD ExceptionRecord)
{
    if (ExceptionRecord != NULL &&
        ExceptionRecord->ExceptionCode == STATUS_UNWIND_CONSOLIDATE &&
        ExceptionRecord->NumberParameters >= 1)
    {
        PVOID (*Consolidate)(PEXCEPTION_RECORD) =
            (PVOID)ExceptionRecord->ExceptionInformation[0];
        ContextRecord->Pc = (ULONG64)Consolidate(ExceptionRecord);
    }

    ZwContinue(ContextRecord, FALSE);
}

VOID
NTAPI
RtlUnwindEx(
    _In_opt_ PVOID TargetFrame,
    _In_opt_ PVOID TargetIp,
    _In_opt_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PVOID ReturnValue,
    _In_ PCONTEXT ContextRecord,
    _In_opt_ struct _UNWIND_HISTORY_TABLE *HistoryTable)
{
    EXCEPTION_RECORD LocalExceptionRecord;

    (void)TargetFrame;
    (void)HistoryTable;

    RtlCaptureContext(ContextRecord);

    if (ExceptionRecord == NULL)
    {
        LocalExceptionRecord.ExceptionCode = STATUS_UNWIND;
        LocalExceptionRecord.ExceptionAddress = (PVOID)ContextRecord->Pc;
        LocalExceptionRecord.ExceptionRecord = NULL;
        LocalExceptionRecord.NumberParameters = 0;
        ExceptionRecord = &LocalExceptionRecord;
    }

    ExceptionRecord->ExceptionFlags = EXCEPTION_UNWINDING;
    if (TargetFrame == NULL)
        ExceptionRecord->ExceptionFlags |= EXCEPTION_EXIT_UNWIND;

    ContextRecord->X0 = (ULONG64)ReturnValue;
    if (ExceptionRecord->ExceptionCode != STATUS_UNWIND_CONSOLIDATE)
        ContextRecord->Pc = (ULONG64)TargetIp;

    RtlRestoreContext(ContextRecord, ExceptionRecord);
}
