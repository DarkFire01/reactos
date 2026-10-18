/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     advapi32.dll Event tracing stubs
 * COPYRIGHT:   Copyright 2017 Mark Jansen (mark.jansen@reactos.org)
 */

#include <advapi32.h>
#include <wmistr.h>
#include <evntrace.h>

WINE_DEFAULT_DEBUG_CHANNEL(advapi);


TRACEHANDLE
WINAPI
OpenTraceA(IN PEVENT_TRACE_LOGFILEA Logfile)
{
    UNIMPLEMENTED;
    SetLastError(ERROR_ACCESS_DENIED);
    return INVALID_PROCESSTRACE_HANDLE;
}

TRACEHANDLE
WINAPI
OpenTraceW(IN PEVENT_TRACE_LOGFILEW Logfile)
{
    UNIMPLEMENTED;
    SetLastError(ERROR_ACCESS_DENIED);
    return INVALID_PROCESSTRACE_HANDLE;
}

ULONG
WINAPI
ProcessTrace(IN PTRACEHANDLE HandleArray,
             IN ULONG HandleCount,
             IN LPFILETIME StartTime,
             IN LPFILETIME EndTime)
{
    UNIMPLEMENTED;
    return ERROR_NOACCESS;
}

static
ULONG
EtwpCheckTraceProperties(
    _In_opt_ PEVENT_TRACE_PROPERTIES Properties)
{
    if (!Properties)
        return ERROR_INVALID_PARAMETER;

    if (Properties->Wnode.BufferSize < sizeof(EVENT_TRACE_PROPERTIES))
        return ERROR_BAD_LENGTH;

    return ERROR_SUCCESS;
}

/**
 * @brief
 * Starts an event tracing session.
 *
 * @return
 * ERROR_ACCESS_DENIED for valid parameters, as no trace session can be
 * started here.
 */
static
ULONG
EtwpStartTrace(
    _Out_opt_ PTRACEHANDLE SessionHandle,
    _In_ BOOL HasSessionName,
    _In_opt_ PEVENT_TRACE_PROPERTIES Properties)
{
    ULONG Error;

    if (!SessionHandle || !HasSessionName)
        return ERROR_INVALID_PARAMETER;

    Error = EtwpCheckTraceProperties(Properties);
    if (Error != ERROR_SUCCESS)
        return Error;

    *SessionHandle = 0;
    return ERROR_ACCESS_DENIED;
}

/**
 * @brief
 * Queries, updates, flushes or stops an event tracing session.
 *
 * @return
 * ERROR_WMI_INSTANCE_NOT_FOUND for valid parameters, as no trace session
 * exists here.
 */
static
ULONG
EtwpControlTrace(
    _In_opt_ PEVENT_TRACE_PROPERTIES Properties,
    _In_ ULONG ControlCode)
{
    ULONG Error;

    if (ControlCode != EVENT_TRACE_CONTROL_QUERY &&
        ControlCode != EVENT_TRACE_CONTROL_STOP &&
        ControlCode != EVENT_TRACE_CONTROL_UPDATE &&
        ControlCode != EVENT_TRACE_CONTROL_FLUSH)
    {
        return ERROR_INVALID_PARAMETER;
    }

    Error = EtwpCheckTraceProperties(Properties);
    if (Error != ERROR_SUCCESS)
        return Error;

    return ERROR_WMI_INSTANCE_NOT_FOUND;
}

ULONG
WINAPI
StartTraceW(
    _Out_ PTRACEHANDLE SessionHandle,
    _In_ LPCWSTR SessionName,
    _Inout_ PEVENT_TRACE_PROPERTIES Properties)
{
    return EtwpStartTrace(SessionHandle, SessionName != NULL, Properties);
}

ULONG
WINAPI
StartTraceA(
    _Out_ PTRACEHANDLE SessionHandle,
    _In_ LPCSTR SessionName,
    _Inout_ PEVENT_TRACE_PROPERTIES Properties)
{
    return EtwpStartTrace(SessionHandle, SessionName != NULL, Properties);
}

ULONG
WINAPI
ControlTraceW(
    _In_ TRACEHANDLE SessionHandle,
    _In_opt_ LPCWSTR SessionName,
    _Inout_ PEVENT_TRACE_PROPERTIES Properties,
    _In_ ULONG ControlCode)
{
    UNREFERENCED_PARAMETER(SessionHandle);
    UNREFERENCED_PARAMETER(SessionName);

    return EtwpControlTrace(Properties, ControlCode);
}

ULONG
WINAPI
ControlTraceA(
    _In_ TRACEHANDLE SessionHandle,
    _In_opt_ LPCSTR SessionName,
    _Inout_ PEVENT_TRACE_PROPERTIES Properties,
    _In_ ULONG ControlCode)
{
    UNREFERENCED_PARAMETER(SessionHandle);
    UNREFERENCED_PARAMETER(SessionName);

    return EtwpControlTrace(Properties, ControlCode);
}

ULONG
WINAPI
StopTraceW(
    _In_ TRACEHANDLE SessionHandle,
    _In_opt_ LPCWSTR SessionName,
    _Out_ PEVENT_TRACE_PROPERTIES Properties)
{
    return ControlTraceW(SessionHandle, SessionName, Properties, EVENT_TRACE_CONTROL_STOP);
}

ULONG
WINAPI
StopTraceA(
    _In_ TRACEHANDLE SessionHandle,
    _In_opt_ LPCSTR SessionName,
    _Out_ PEVENT_TRACE_PROPERTIES Properties)
{
    return ControlTraceA(SessionHandle, SessionName, Properties, EVENT_TRACE_CONTROL_STOP);
}

ULONG
WINAPI
QueryTraceW(
    _In_ TRACEHANDLE SessionHandle,
    _In_opt_ LPCWSTR SessionName,
    _Inout_ PEVENT_TRACE_PROPERTIES Properties)
{
    return ControlTraceW(SessionHandle, SessionName, Properties, EVENT_TRACE_CONTROL_QUERY);
}

ULONG
WINAPI
QueryTraceA(
    _In_ TRACEHANDLE SessionHandle,
    _In_opt_ LPCSTR SessionName,
    _Inout_ PEVENT_TRACE_PROPERTIES Properties)
{
    return ControlTraceA(SessionHandle, SessionName, Properties, EVENT_TRACE_CONTROL_QUERY);
}

ULONG
WINAPI
FlushTraceW(
    _In_ TRACEHANDLE SessionHandle,
    _In_opt_ LPCWSTR SessionName,
    _Inout_ PEVENT_TRACE_PROPERTIES Properties)
{
    return ControlTraceW(SessionHandle, SessionName, Properties, EVENT_TRACE_CONTROL_FLUSH);
}

ULONG
WINAPI
FlushTraceA(
    _In_ TRACEHANDLE SessionHandle,
    _In_opt_ LPCSTR SessionName,
    _Inout_ PEVENT_TRACE_PROPERTIES Properties)
{
    return ControlTraceA(SessionHandle, SessionName, Properties, EVENT_TRACE_CONTROL_FLUSH);
}

