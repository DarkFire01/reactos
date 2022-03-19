
#include "k32_vista.h"

#include <ndk/exfuncs.h>
#include <wine/config.h>
#include <wine/port.h>

DWORD WINAPI RtlRunOnceExecuteOnce( RTL_RUN_ONCE *once, PRTL_RUN_ONCE_INIT_FN func,
                                           void *param, void **context );

DWORD WINAPI RtlRunOnceBeginInitialize( RTL_RUN_ONCE *once, DWORD flags, void **context );

DWORD WINAPI RtlRunOnceComplete( RTL_RUN_ONCE *once, DWORD flags, void **context );

/* Taken from Wine kernel32/sync.c */

/*
 * @implemented
 */
BOOL NTAPI InitOnceExecuteOnce( INIT_ONCE *once, PINIT_ONCE_FN func, void *param, void **context )
{
    return !RtlRunOnceExecuteOnce( once, (PRTL_RUN_ONCE_INIT_FN)func, param, context );
}

/***********************************************************************
 *           InitOnceBeginInitialize    (KERNEL32.@)
 */
BOOL WINAPI InitOnceBeginInitialize( INIT_ONCE *once, DWORD flags, BOOL *pending, void **context )
{
    NTSTATUS status = RtlRunOnceBeginInitialize( once, flags, context );
    if (status >= 0) *pending = (status == STATUS_PENDING);
    else SetLastError( RtlNtStatusToDosError(status) );
    return status >= 0;
}

/***********************************************************************
 *           InitOnceComplete    (KERNEL32.@)
 */
BOOL WINAPI InitOnceComplete( INIT_ONCE *once, DWORD flags, void *context )
{
    NTSTATUS status = RtlRunOnceComplete( once, flags, context );
    if (status != STATUS_SUCCESS) SetLastError( RtlNtStatusToDosError(status) );
    return !status;
}