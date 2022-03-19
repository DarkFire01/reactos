/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS NT User-Mode DLL
 * FILE:            lib/ntdll/rtl/uilist.c
 * PURPOSE:         RTL UI to API network computers list conversion.
 *                  Helper for NETAPI32.DLL
 * PROGRAMMERS:     Hermes Belusca-Maito
 */

/* INCLUDES *****************************************************************/

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ***************************************************************/

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
RtlConvertUiListToApiList(
    IN PUNICODE_STRING UiList,
    OUT PUNICODE_STRING ApiList,
    IN BOOLEAN SpaceAsSeparator)
{
    DPRINT1("RtlConvertUiListToApiList(%wZ, 0x%p, %s) called\n",
            UiList, &ApiList, SpaceAsSeparator ? "true" : "false");
    UNIMPLEMENTED;
    /*
     * Experiments show that returning a success code but setting the
     * ApiList length to zero is better than returning a failure code.
     */
    RtlInitEmptyUnicodeString(ApiList, NULL, 0);
    return STATUS_SUCCESS;
}

/**********************************************************************
 *           RtlGetCurrentProcessorNumberEx [NTDLL.@]
 */
VOID WINAPI RtlGetCurrentProcessorNumberEx(PROCESSOR_NUMBER *Processor)
{
    DPRINT1("RtlGetCurrentProcessorNumberEx: processor number (%p)\n", Processor);
    Processor->Group = 0;
    Processor->Number = NtGetCurrentProcessorNumber();
    Processor->Reserved = 0;
}

NTSTATUS
WINAPI
WinSqmEndSession(HANDLE session)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

VOID
WINAPI
WinSqmIncrementDWORD(DWORD dwUnk1, DWORD dwUnk2, DWORD dwUnk3)
{
    UNIMPLEMENTED;
}

BOOL
WINAPI
WinSqmIsOptedIn(VOID)
{
    UNIMPLEMENTED;
    return FALSE;
}

VOID
WINAPI
WinSqmSetDWORD(HANDLE hSession, DWORD DatapointID, DWORD DatapointValue)
{
    UNIMPLEMENTED;
}

VOID
WINAPI
WinSqmSetString(VOID)
{
    UNIMPLEMENTED;
}

HANDLE
WINAPI
WinSqmStartSession(GUID *sessionguid, DWORD sessionid, DWORD unknown1)
{
    UNIMPLEMENTED;
    return INVALID_HANDLE_VALUE;
}

VOID
NTAPI
NtFlushProcessWriteBuffers(VOID)
{
    UNIMPLEMENTED;
}

/* EOF */
