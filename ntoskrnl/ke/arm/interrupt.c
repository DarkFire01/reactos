/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/ke/arm/interrupt.c
 * PURPOSE:         Implements interrupt related routines for ARM machines
 * PROGRAMMERS:
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

NTSTATUS
NTAPI
KeSaveFloatingPointState(
    _Out_ PKFLOATING_SAVE Save)
{

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Restores the original FPU state context that has
 * been saved by a API call of KeSaveFloatingPointState.
 * Callers are expected to restore the floating point
 * state by calling this function when they've finished
 * doing FPU operations.
 *
 * @param[in] Save
 * The saved floating point context that is to be given
 * to the function to restore the FPU state.
 *
 * @return
 * Returns STATUS_SUCCESS indicating the function
 * has fully completed its operations.
 */

NTSTATUS
NTAPI
KeRestoreFloatingPointState(
    _In_ PKFLOATING_SAVE Save)
{
    return STATUS_SUCCESS;
}

VOID
NTAPI
KeInitializeInterrupt(IN PKINTERRUPT Interrupt,
                      IN PKSERVICE_ROUTINE ServiceRoutine,
                      IN PVOID ServiceContext,
                      IN PKSPIN_LOCK SpinLock,
                      IN ULONG Vector,
                      IN KIRQL Irql,
                      IN KIRQL SynchronizeIrql,
                      IN KINTERRUPT_MODE InterruptMode,
                      IN BOOLEAN ShareVector,
                      IN CHAR ProcessorNumber,
                      IN BOOLEAN FloatingSave)
{
    ASSERT(FALSE);
}

BOOLEAN
NTAPI
KeConnectInterrupt(IN PKINTERRUPT Interrupt)
{
    ASSERT(FALSE);
    return FALSE;
}

BOOLEAN
NTAPI
KeDisconnectInterrupt(IN PKINTERRUPT Interrupt)
{
    ASSERT(FALSE);
    return FALSE;
}

VOID
KiUnexpectedInterrupt(VOID)
{
    /* Crash the machine */
    KeBugCheck(TRAP_CAUSE_UNKNOWN);
}

BOOLEAN
NTAPI
KeSynchronizeExecution(
    IN OUT PKINTERRUPT Interrupt,
    IN PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
    IN PVOID SynchronizeContext OPTIONAL)
{
    ASSERT(FALSE);
    return FALSE;
}

