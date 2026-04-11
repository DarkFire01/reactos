/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS Run-Time Library
 * PURPOSE:           User-mode exception support for ARM64
 */

#include <rtl.h>
#define NDEBUG
#include <debug.h>

VOID
NTAPI
RtlGetCallersAddress(
    _Out_ PVOID *CallersAddress,
    _Out_ PVOID *CallersCaller)
{
    *CallersAddress = NULL;
    *CallersCaller = NULL;
}

BOOLEAN
NTAPI
RtlDispatchException(
    _In_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PCONTEXT Context)
{
    UNREFERENCED_PARAMETER(ExceptionRecord);
    UNREFERENCED_PARAMETER(Context);
    return FALSE;
}

VOID
NTAPI
RtlUnwind(
    _In_opt_ PVOID TargetFrame,
    _In_opt_ PVOID TargetIp,
    _In_opt_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PVOID ReturnValue)
{
    UNREFERENCED_PARAMETER(TargetFrame);
    UNREFERENCED_PARAMETER(TargetIp);
    UNREFERENCED_PARAMETER(ExceptionRecord);
    UNREFERENCED_PARAMETER(ReturnValue);
}

VOID
NTAPI
RtlInitializeContext(
    IN HANDLE ProcessHandle,
    OUT PCONTEXT ThreadContext,
    IN PVOID ThreadStartParam OPTIONAL,
    IN PTHREAD_START_ROUTINE ThreadStartAddress,
    IN PINITIAL_TEB InitialTeb)
{
    UNREFERENCED_PARAMETER(ProcessHandle);
    RtlZeroMemory(ThreadContext, sizeof(*ThreadContext));
    ThreadContext->Pc = (ULONG64)(ULONG_PTR)ThreadStartAddress;
    ThreadContext->Sp = (ULONG64)(ULONG_PTR)InitialTeb->StackBase;
    ThreadContext->X0 = (ULONG64)(ULONG_PTR)ThreadStartParam;
    ThreadContext->ContextFlags = CONTEXT_FULL;
}

VOID
NTAPI
RtlCaptureContext(
    OUT PCONTEXT ContextRecord)
{
    RtlZeroMemory(ContextRecord, sizeof(*ContextRecord));
    ContextRecord->ContextFlags = CONTEXT_FULL;
}

EXCEPTION_DISPOSITION
__cdecl
__C_specific_handler(
    struct _EXCEPTION_RECORD *ExceptionRecord,
    void *EstablisherFrame,
    struct _CONTEXT *ContextRecord,
    struct _DISPATCHER_CONTEXT *DispatcherContext)
{
    UNREFERENCED_PARAMETER(ExceptionRecord);
    UNREFERENCED_PARAMETER(EstablisherFrame);
    UNREFERENCED_PARAMETER(ContextRecord);
    UNREFERENCED_PARAMETER(DispatcherContext);
    return ExceptionContinueSearch;
}
