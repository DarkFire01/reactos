/*
 * ntdll.dll Event Tracing Functions
 */

#include <ntdll.h>

#include <wmistr.h>
#include <evntrace.h>
#include <evntprov.h>

#define NDEBUG
#include <debug.h>

#define FIXME DPRINT1

/*
 * @unimplemented
 */
ULONG CDECL
EtwTraceMessage(
    TRACEHANDLE  SessionHandle,
    ULONG        MessageFlags,
    LPCGUID      MessageGuid,
    USHORT       MessageNumber,
    ...)
{
    FIXME("TraceMessage()\n");
    return ERROR_SUCCESS;
}

TRACEHANDLE
NTAPI
EtwGetTraceLoggerHandle(
    PVOID Buffer
)
{
    FIXME("EtwGetTraceLoggerHandle stub()\n");
    return (TRACEHANDLE)-1;
}


ULONG
NTAPI
EtwTraceEvent(
    TRACEHANDLE SessionHandle,
    PEVENT_TRACE_HEADER EventTrace
)
{
    FIXME("EtwTraceEvent stub()\n");

    if (!SessionHandle || !EventTrace)
    {
        /* invalid parameters */
        return ERROR_INVALID_PARAMETER;
    }

    if (EventTrace->Size != sizeof(EVENT_TRACE_HEADER))
    {
        /* invalid parameter */
        return ERROR_INVALID_PARAMETER;
    }

    return ERROR_SUCCESS;
}

ULONG
NTAPI
EtwGetTraceEnableFlags(
    TRACEHANDLE TraceHandle
)
{
    FIXME("EtwGetTraceEnableFlags stub()\n");
    return 0xFF;
}

UCHAR
NTAPI
EtwGetTraceEnableLevel(
    TRACEHANDLE TraceHandle
)
{
    FIXME("EtwGetTraceEnableLevel stub()\n");
    return 0xFF;
}

ULONG
NTAPI
EtwUnregisterTraceGuids(
    TRACEHANDLE RegistrationHandle
)
{
    FIXME("EtwUnregisterTraceGuids stub()\n");
    return ERROR_SUCCESS;
}

ULONG
NTAPI
EtwRegisterTraceGuidsA(
    WMIDPREQUEST RequestAddress,
    PVOID RequestContext,
    LPCGUID ControlGuid,
    ULONG GuidCount,
    PTRACE_GUID_REGISTRATION TraceGuidReg,
    LPCSTR MofImagePath,
    LPCSTR MofResourceName,
    PTRACEHANDLE RegistrationHandle
)
{
    FIXME("EtwRegisterTraceGuidsA stub()\n");
    return ERROR_SUCCESS;
}

ULONG
NTAPI
EtwRegisterTraceGuidsW(
    WMIDPREQUEST RequestAddress,
    PVOID RequestContext,
    LPCGUID ControlGuid,
    ULONG GuidCount,
    PTRACE_GUID_REGISTRATION TraceGuidReg,
    LPCWSTR MofImagePath,
    LPCWSTR MofResourceName,
    PTRACEHANDLE RegistrationHandle
)
{
    FIXME("EtwRegisterTraceGuidsW stub()\n");
    return ERROR_SUCCESS;
}

ULONG WINAPI EtwStartTraceW( PTRACEHANDLE pSessionHandle, LPCWSTR SessionName, PEVENT_TRACE_PROPERTIES Properties )
{
    FIXME("(%p, %s, %p) stub\n", pSessionHandle, SessionName, Properties);
    if (pSessionHandle) *pSessionHandle = 0xcafe4242;
    return ERROR_SUCCESS;
}

ULONG WINAPI EtwStartTraceA( PTRACEHANDLE pSessionHandle, LPCSTR SessionName, PEVENT_TRACE_PROPERTIES Properties )
{
    FIXME("(%p, %s, %p) stub\n", pSessionHandle, SessionName, Properties);
    if (pSessionHandle) *pSessionHandle = 0xcafe4242;
    return ERROR_SUCCESS;
}

/******************************************************************************
 * EtwControlTraceW [NTDLL.@]
 *
 * Control a givel event trace session
 *
 */
ULONG WINAPI EtwControlTraceW( TRACEHANDLE hSession, LPCWSTR SessionName, PEVENT_TRACE_PROPERTIES Properties, ULONG control )
{
    FIXME("(%I64x, %s, %p, %d) stub\n", hSession, SessionName, Properties, control);
    return ERROR_SUCCESS;
}

/******************************************************************************
 * EtwControlTraceA [NTDLL.@]
 *
 * See ControlTraceW.
 *
 */
ULONG WINAPI EtwControlTraceA( TRACEHANDLE hSession, LPCSTR SessionName, PEVENT_TRACE_PROPERTIES Properties, ULONG control )
{
    FIXME("(%I64x, %s, %p, %d) stub\n", hSession, SessionName, Properties, control);
    return ERROR_SUCCESS;
}

/******************************************************************************
 *                  EtwEventActivityIdControl (NTDLL.@)
 */
ULONG WINAPI EtwEventActivityIdControl(ULONG code, GUID *guid)
{
    static int once;

    if (!once++) FIXME("0x%x, %p: stub\n", code, guid);
    return ERROR_SUCCESS;
}

/******************************************************************************
 *                  EtwEventProviderEnabled (NTDLL.@)
 */
BOOLEAN WINAPI EtwEventProviderEnabled( REGHANDLE handle, UCHAR level, ULONGLONG keyword )
{
    FIXME("%s, %u, %s: stub\n", handle, level, keyword);
    return FALSE;
}

/******************************************************************************
 * EtwEnableTrace [NTDLL.@]
 */
ULONG WINAPI EtwEnableTrace( ULONG enable, ULONG flag, ULONG level, LPCGUID guid, TRACEHANDLE hSession )
{
    FIXME("(%d, 0x%x, %d, %p, %I64x): stub\n", enable, flag, level,
            guid, hSession);

    return ERROR_SUCCESS;
}

/******************************************************************************
 *                  EtwEventRegister (NTDLL.@)
 */
ULONG WINAPI EtwEventRegister( LPCGUID provider, PENABLECALLBACK callback, PVOID context, PREGHANDLE handle )
{
    FIXME("(%s, %p, %p, %p) stub.\n", provider, callback, context, handle);

    if (!handle) return ERROR_INVALID_PARAMETER;

    *handle = 0xdeadbeef;
    return ERROR_SUCCESS;
}

/******************************************************************************
 *                  EtwEventUnregister (NTDLL.@)
 */
ULONG WINAPI EtwEventUnregister( REGHANDLE handle )
{
    FIXME("(%s) stub.\n", handle);
    return ERROR_SUCCESS;
}

/******************************************************************************
 *                  EtwEventWrite (NTDLL.@)
 */
ULONG WINAPI EtwEventWrite( REGHANDLE handle, const EVENT_DESCRIPTOR *descriptor, ULONG count, EVENT_DATA_DESCRIPTOR *data )
{
    FIXME("(%s, %p, %u, %p): stub\n", handle, descriptor, count, data);
    return ERROR_SUCCESS;
}

/******************************************************************************
 *                  EtwEventWriteString   (NTDLL.@)
 */
ULONG WINAPI EtwEventWriteString( REGHANDLE handle, UCHAR level, ULONGLONG keyword, PCWSTR string )
{
    FIXME("%s, %u, %s, %s: stub\n", handle, level, keyword, string);
    return ERROR_SUCCESS;
}

/******************************************************************************
 *                  EtwEventWriteTransfer   (NTDLL.@)
 */
ULONG WINAPI EtwEventWriteTransfer( REGHANDLE handle, PCEVENT_DESCRIPTOR descriptor, LPCGUID activity,
                                    LPCGUID related, ULONG count, PEVENT_DATA_DESCRIPTOR data )
{
    FIXME("%s, %p, %s, %s, %u, %p: stub\n", handle, descriptor, activity, related, count, data);
    return ERROR_SUCCESS;
}

/******************************************************************************
 * EtwQueryAllTracesW [NTDLL.@]
 *
 * Query information for started event trace sessions
 *
 */
ULONG WINAPI EtwQueryAllTracesW( PEVENT_TRACE_PROPERTIES * parray, ULONG arraycount, PULONG psessioncount )
{
    FIXME("(%p, %d, %p) stub\n", parray, arraycount, psessioncount);

    if (psessioncount) *psessioncount = 0;
    return ERROR_SUCCESS;
}

/******************************************************************************
 * QueryAllTracesA [NTDLL.@]
 *
 * See EtwQueryAllTracesA.
 */
ULONG WINAPI EtwQueryAllTracesA( PEVENT_TRACE_PROPERTIES * parray, ULONG arraycount, PULONG psessioncount )
{
    FIXME("(%p, %d, %p) stub\n", parray, arraycount, psessioncount);

    if (psessioncount) *psessioncount = 0;
    return ERROR_SUCCESS;
}

/******************************************************************************
 * EtwFlushTraceA [NTDLL.@]
 *
 */
ULONG WINAPI EtwFlushTraceA( TRACEHANDLE hSession, LPCSTR SessionName, PEVENT_TRACE_PROPERTIES Properties )
{
    return EtwControlTraceA( hSession, SessionName, Properties, EVENT_TRACE_CONTROL_FLUSH );
}

/******************************************************************************
 * EtwFlushTraceW [NTDLL.@]
 *
 */
ULONG WINAPI EtwFlushTraceW( TRACEHANDLE hSession, LPCWSTR SessionName, PEVENT_TRACE_PROPERTIES Properties )
{
    return EtwControlTraceW( hSession, SessionName, Properties, EVENT_TRACE_CONTROL_FLUSH );
}

/******************************************************************************
 * EtwQueryTraceA [NTDLL.@]
 *
 */
ULONG WINAPI EtwQueryTraceA( TRACEHANDLE hSession, LPCSTR SessionName, PEVENT_TRACE_PROPERTIES Properties )
{
    return EtwControlTraceA( hSession, SessionName, Properties, EVENT_TRACE_CONTROL_QUERY );
}

/******************************************************************************
 * EtwQueryTraceW [NTDLL.@]
 *
 */
ULONG WINAPI EtwQueryTraceW( TRACEHANDLE hSession, LPCWSTR SessionName, PEVENT_TRACE_PROPERTIES Properties )
{
    return EtwControlTraceW( hSession, SessionName, Properties, EVENT_TRACE_CONTROL_QUERY );
}

/******************************************************************************
 * EtwStopTraceA [NTDLL.@]
 *
 */
ULONG WINAPI EtwStopTraceA( TRACEHANDLE hSession, LPCSTR SessionName, PEVENT_TRACE_PROPERTIES Properties )
{
    return EtwControlTraceA( hSession, SessionName, Properties, EVENT_TRACE_CONTROL_STOP );
}

/******************************************************************************
 * EtwStopTraceW [NTDLL.@]
 *
 */
ULONG WINAPI EtwStopTraceW( TRACEHANDLE hSession, LPCWSTR SessionName, PEVENT_TRACE_PROPERTIES Properties )
{
    return EtwControlTraceW( hSession, SessionName, Properties, EVENT_TRACE_CONTROL_STOP );
}

/******************************************************************************
 * EtwUpdateTraceA [NTDLL.@]
 *
 */
ULONG WINAPI EtwUpdateTraceA( TRACEHANDLE hSession, LPCSTR SessionName, PEVENT_TRACE_PROPERTIES Properties )
{
    return EtwControlTraceA( hSession, SessionName, Properties, EVENT_TRACE_CONTROL_UPDATE );
}

/******************************************************************************
 * EtwUpdateTraceW [NTDLL.@]
 *
 */
ULONG WINAPI EtwUpdateTraceW( TRACEHANDLE hSession, LPCWSTR SessionName, PEVENT_TRACE_PROPERTIES Properties )
{
    return EtwControlTraceW( hSession, SessionName, Properties, EVENT_TRACE_CONTROL_UPDATE );
}

ULONG
WINAPI
EtwNotificationRegister(
    LPCGUID Guid,
    ULONG Type,
    PETW_NOTIFICATION_CALLBACK Callback,
    PVOID Context,
    REGHANDLE RegHandle
)
{
    UNIMPLEMENTED;
    return 0;
}

/* EOF */
