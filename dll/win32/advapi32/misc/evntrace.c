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

