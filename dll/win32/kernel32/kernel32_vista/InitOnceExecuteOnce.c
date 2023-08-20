
#include "k32_vista.h"

#include <ndk/exfuncs.h>
#include <wine/config.h>
#include <wine/port.h>

/*
 * @implemented
 */
BOOL
WINAPI
InitOnceExecuteOnce(
    _Inout_ PINIT_ONCE InitOnce,
    _In_ __callback PINIT_ONCE_FN InitFn,
    _Inout_opt_ PVOID Parameter,
    _Outptr_opt_result_maybenull_ LPVOID *Context)
{
    return NT_SUCCESS(RtlRunOnceExecuteOnce(InitOnce,
                                            (PRTL_RUN_ONCE_INIT_FN)InitFn,
                                            Parameter,
                                            Context));
}
