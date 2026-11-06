/*
 * PROJECT:     ReactOS runtime library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64EC software exceptions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <rtl.h>

#define NDEBUG
#include <debug.h>

/* The native half of an ARM64X image provides it */
VOID
NTAPI
RtlpRaiseEcException(
    _In_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PVOID CallerFrame);

/* FUNCTIONS *****************************************************************/

/*! RtlRaiseException
 * \brief Raises a software exception on behalf of the caller.
 * ARM64EC frames unwind like ARM64 ones, so the native code raises it.
 */
VOID
NTAPI
RtlRaiseException(
    _In_ PEXCEPTION_RECORD ExceptionRecord)
{
    RtlpRaiseEcException(ExceptionRecord, _AddressOfReturnAddress());
}
