/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     advapi32.dll Event tracing stubs
 * COPYRIGHT:   Copyright 2017 Mark Jansen (mark.jansen@reactos.org)
 */

#include <advapi32.h>
#include <wmistr.h>
#include <evntrace.h>

/*
 * The provider side of event tracing arrived in Vista, and EventSetInformation
 * with it in Windows 8, so <evntprov.h> hides those declarations at the WINVER
 * this module is built for. We are the ones exporting them, so ask for them,
 * and for the definitions rather than the imports.
 */
#undef WINVER
#define WINVER _WIN32_WINNT_WIN8
#define _EVNT_SOURCE_
#include <evntprov.h>

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

/*
 * The provider side of event tracing. There is no session to trace to here, so
 * these do what Windows does when nobody is listening: registration succeeds,
 * the provider is never enabled, and writing an event is quietly dropped.
 *
 * They cannot be forwarded to ntdll's EtwEventRegister and friends, the way the
 * consumer side is, because those are stub entries that raise when called, and
 * a provider registers as it starts up. Failing here is not an option either:
 * callers treat it as fatal.
 */

ULONG
EVNTAPI
EventRegister(
    _In_ LPCGUID ProviderId,
    _In_opt_ PENABLECALLBACK EnableCallback,
    _In_opt_ PVOID CallbackContext,
    _Out_ PREGHANDLE RegHandle)
{
    UNREFERENCED_PARAMETER(ProviderId);
    UNREFERENCED_PARAMETER(EnableCallback);
    UNREFERENCED_PARAMETER(CallbackContext);

    if (RegHandle == NULL)
        return ERROR_INVALID_PARAMETER;

    /* No session, so nothing to hand back but a handle that does nothing */
    *RegHandle = 0;
    return ERROR_SUCCESS;
}

ULONG
EVNTAPI
EventUnregister(
    _In_ REGHANDLE RegHandle)
{
    UNREFERENCED_PARAMETER(RegHandle);
    return ERROR_SUCCESS;
}

ULONG
EVNTAPI
EventSetInformation(
    _In_ REGHANDLE RegHandle,
    _In_ EVENT_INFO_CLASS InformationClass,
    _In_reads_bytes_(InformationLength) PVOID EventInformation,
    _In_ ULONG InformationLength)
{
    UNREFERENCED_PARAMETER(RegHandle);

    if (EventInformation == NULL && InformationLength != 0)
        return ERROR_INVALID_PARAMETER;

    switch (InformationClass)
    {
        case EventProviderBinaryTrackInfo:
        case EventProviderSetTraits:
        case EventProviderUseDescriptorType:
            /* All of these describe the provider to a session, which is
               where the description would be read back. There is no session,
               so there is nothing to tell and nothing to keep. A caller
               registers its traits as it starts up and treats a failure here
               as fatal, so take them. */
            return ERROR_SUCCESS;

        default:
            return ERROR_INVALID_PARAMETER;
    }
}

BOOLEAN
EVNTAPI
EventEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(EventDescriptor);

    /* Nothing is collecting, so no event is worth building */
    return FALSE;
}

ULONG
EVNTAPI
EventWrite(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(UserDataCount);
    UNREFERENCED_PARAMETER(UserData);

    if (EventDescriptor == NULL)
        return ERROR_INVALID_PARAMETER;

    return ERROR_SUCCESS;
}

ULONG
EVNTAPI
EventWriteTransfer(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    /* The activity and the one it relates to only mean something to a session
       reading the events back, and there is none, so this is EventWrite with
       two more arguments to ignore. */
    UNREFERENCED_PARAMETER(ActivityId);
    UNREFERENCED_PARAMETER(RelatedActivityId);

    return EventWrite(RegHandle, EventDescriptor, UserDataCount, UserData);
}


/*
 * The rest of the provider surface. These arrived alongside the ones above and
 * are reached through api-ms-win-eventing-provider-l1-1-0, which names the
 * whole set - an apiset resolves to one module, so a set that is only half
 * exported here leaves the other half unresolvable no matter which module the
 * table points at.
 *
 * They answer the way the others do: there is no session, so nothing is
 * enabled and nothing is written, and every caller is told so truthfully.
 */

/*
 * An activity id has to be unique within the trace and nothing more - no
 * session ever reads these back here. RPC's UuidCreate would be the usual
 * source, but advapi32 does not import rpcrt4 and should not start over this,
 * so the id is built from the clock and a per-process counter.
 */
static
VOID
EtwpMakeActivityId(
    _Out_ LPGUID ActivityId)
{
    static LONG Sequence = 0;
    FILETIME Now;
    ULARGE_INTEGER Time;
    ULONG Seed;

    GetSystemTimeAsFileTime(&Now);
    Time.LowPart = Now.dwLowDateTime;
    Time.HighPart = Now.dwHighDateTime;

    Seed = Time.LowPart ^ GetCurrentThreadId();

    ActivityId->Data1 = Time.LowPart;
    ActivityId->Data2 = (USHORT)(Time.HighPart & 0xFFFF);
    ActivityId->Data3 = (USHORT)(InterlockedIncrement(&Sequence) & 0xFFFF);
    *(PULONG)&ActivityId->Data4[0] = RtlRandom(&Seed);
    *(PULONG)&ActivityId->Data4[4] = RtlRandom(&Seed);
}

ULONG
EVNTAPI
EventActivityIdControl(
    _In_ ULONG ControlCode,
    _Inout_ LPGUID ActivityId)
{
    if (ActivityId == NULL)
        return ERROR_INVALID_PARAMETER;

    switch (ControlCode)
    {
        case EVENT_ACTIVITY_CTRL_GET_ID:
        case EVENT_ACTIVITY_CTRL_CREATE_ID:
        case EVENT_ACTIVITY_CTRL_GET_SET_ID:
        case EVENT_ACTIVITY_CTRL_CREATE_SET_ID:
            /*
             * An activity id only has to be unique, not meaningful, and a
             * caller that asks for one and is refused often gives up on
             * tracing altogether. Handing back a fresh GUID costs nothing and
             * is what a caller does with the answer anyway - tags events that
             * nothing here collects.
             */
            if (ControlCode != EVENT_ACTIVITY_CTRL_SET_ID)
                EtwpMakeActivityId(ActivityId);
            return ERROR_SUCCESS;

        case EVENT_ACTIVITY_CTRL_SET_ID:
            return ERROR_SUCCESS;

        default:
            return ERROR_INVALID_PARAMETER;
    }
}

BOOLEAN
EVNTAPI
EventProviderEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Keyword);

    /* Nothing is collecting, so no level or keyword is ever enabled */
    return FALSE;
}

ULONG
EVNTAPI
EventWriteEx(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG64 Filter,
    _In_ ULONG Flags,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(Flags);

    return EventWriteTransfer(RegHandle, EventDescriptor, ActivityId,
                              RelatedActivityId, UserDataCount, UserData);
}

ULONG
EVNTAPI
EventWriteString(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword,
    _In_ PCWSTR String)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Keyword);

    if (String == NULL)
        return ERROR_INVALID_PARAMETER;

    return ERROR_SUCCESS;
}

/*
 * The controller side. EnableTraceEx2 is how a session turns a provider on,
 * and CloseTrace ends a consumer session opened by OpenTrace - which here
 * always fails, so there is never a handle to close.
 */
ULONG
WINAPI
EnableTraceEx2(
    _In_ TRACEHANDLE TraceHandle,
    _In_ LPCGUID ProviderId,
    _In_ ULONG ControlCode,
    _In_ UCHAR Level,
    _In_ ULONGLONG MatchAnyKeyword,
    _In_ ULONGLONG MatchAllKeyword,
    _In_ ULONG Timeout,
    _In_opt_ PVOID EnableParameters)
{
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(MatchAnyKeyword);
    UNREFERENCED_PARAMETER(MatchAllKeyword);
    UNREFERENCED_PARAMETER(Timeout);
    UNREFERENCED_PARAMETER(EnableParameters);

    if (ProviderId == NULL)
        return ERROR_INVALID_PARAMETER;

    if (ControlCode != EVENT_CONTROL_CODE_DISABLE_PROVIDER &&
        ControlCode != EVENT_CONTROL_CODE_ENABLE_PROVIDER &&
        ControlCode != EVENT_CONTROL_CODE_CAPTURE_STATE)
    {
        return ERROR_INVALID_PARAMETER;
    }

    /*
     * There is no session behind the handle, so say so rather than reporting
     * that a provider was enabled. A caller that gets this back stops trying;
     * one that is told ERROR_SUCCESS waits for events that never arrive.
     */
    if (TraceHandle == 0)
        return ERROR_INVALID_HANDLE;

    return ERROR_WMI_INSTANCE_NOT_FOUND;
}

ULONG
WINAPI
CloseTrace(
    _In_ TRACEHANDLE TraceHandle)
{
    /* OpenTrace never hands out a handle, so there is never one to close */
    UNREFERENCED_PARAMETER(TraceHandle);

    return ERROR_INVALID_HANDLE;
}

/* EOF */
