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



typedef HANDLE REGHANDLE;

typedef struct _EVENT_DESCRIPTOR {
  USHORT    Id;
  UCHAR     Version;
  UCHAR     Channel;
  UCHAR     Level;
  UCHAR     Opcode;
  USHORT    Task;
  ULONGLONG Keyword;
} EVENT_DESCRIPTOR, *PEVENT_DESCRIPTOR;

typedef struct _EVENT_DATA_DESCRIPTOR {
  ULONGLONG Ptr;
  ULONG     Size;
  union {
    ULONG Reserved;
    struct {
      UCHAR  Type;
      UCHAR  Reserved1;
      USHORT Reserved2;
    } DUMMYSTRUCTNAME;
  } DUMMYUNIONNAME;
} EVENT_DATA_DESCRIPTOR, *PEVENT_DATA_DESCRIPTOR;



typedef void (WINAPI *PENABLECALLBACK)(
  LPCGUID SourceId,
  ULONG IsEnabled,
  UCHAR Level,
  ULONGLONG MatchAnyKeyword,
  ULONGLONG MatchAllKeyword,
  PEVENT_FILTER_DESCRIPTOR FilterData,
  PVOID CallbackContext
);

ULONG 
WINAPI 
EventEnabled(
  REGHANDLE              RegHandle,
  PEVENT_DESCRIPTOR     EventDescriptor
)
{
	UNIMPLEMENTED;
    return ERROR_SUCCESS;
};

ULONG 
WINAPI 
EventWrite(
  REGHANDLE              RegHandle,
  PEVENT_DESCRIPTOR     EventDescriptor,
  ULONG                  UserDataCount,
  PEVENT_DATA_DESCRIPTOR UserData
)
{
	UNIMPLEMENTED;
    return ERROR_SUCCESS;
};

ULONG 
WINAPI 
EventWriteTransfer(
  REGHANDLE              RegHandle,
  PEVENT_DESCRIPTOR     EventDescriptor,
  LPCGUID                ActivityId,
  LPCGUID                RelatedActivityId,
  ULONG                  UserDataCount,
  PEVENT_DATA_DESCRIPTOR UserData
)
{
	UNIMPLEMENTED;
    return ERROR_SUCCESS;
};

ULONG WINAPI EventRegister(
  LPCGUID         ProviderId,
  PENABLECALLBACK EnableCallback,
  PVOID           CallbackContext,
  REGHANDLE*      RegHandle
  )
{
	UNIMPLEMENTED;
    return ERROR_SUCCESS;
};
  
ULONG WINAPI EventUnregister(REGHANDLE RegHandle)
{
	UNIMPLEMENTED;
    return ERROR_SUCCESS;
};  