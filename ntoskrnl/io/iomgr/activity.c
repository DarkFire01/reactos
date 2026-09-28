/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     What a thread says its work belongs to
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/*
 * A thread can name the piece of work it is doing, so that anything tracing it
 * can tell one piece from another across every component the work passes
 * through. Setting one gives back the one that was there, and putting that
 * back is how a caller leaves the thread as it found it.
 */

/* FUNCTIONS ******************************************************************/

/*
 * @implemented
 */
LPCGUID
NTAPI
IoGetActivityIdThread(VOID)
{
    return PsGetCurrentThread()->ActivityId;
}

/*
 * @implemented
 */
LPCGUID
NTAPI
IoSetActivityIdThread(
    _In_opt_ LPCGUID ActivityId)
{
    PETHREAD Thread = PsGetCurrentThread();
    LPCGUID Previous = Thread->ActivityId;

    Thread->ActivityId = (PGUID)ActivityId;

    return Previous;
}

/*
 * @implemented
 */
VOID
NTAPI
IoClearActivityIdThread(
    _In_opt_ LPCGUID PreviousActivityId)
{
    /* What the caller was given when it set its own goes back here */
    PsGetCurrentThread()->ActivityId = (PGUID)PreviousActivityId;
}

/* EOF */
