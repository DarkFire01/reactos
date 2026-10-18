/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Server silo queries
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Every process runs in the host silo, which callers see as a NULL silo.
 * No other silo can be obtained, so attaching never changes anything.
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

/**
 * @brief Makes a silo the one the current thread acts in.
 *
 * @param[in] Silo
 * Silo to attach to, NULL for the host silo.
 *
 * @return The silo the thread was attached to before.
 */
PVOID
NTAPI
PsAttachSiloToCurrentThread(
    _In_opt_ PVOID Silo)
{
    ASSERT(Silo == NULL);
    return NULL;
}

/**
 * @brief Puts back the silo PsAttachSiloToCurrentThread returned.
 */
VOID
NTAPI
PsDetachSiloFromCurrentThread(
    _In_opt_ PVOID PreviousSilo)
{
    ASSERT(PreviousSilo == NULL);
}

/**
 * @brief Gets the host silo.
 * @return NULL, the host silo is never a silo object.
 */
PVOID
NTAPI
PsGetHostSilo(VOID)
{
    return NULL;
}

/**
 * @brief Tells whether a silo is the host silo.
 */
BOOLEAN
NTAPI
PsIsHostSilo(
    _In_opt_ PVOID Silo)
{
    return (BOOLEAN)(Silo == NULL);
}

/**
 * @brief Gets the server silo the current thread runs in.
 * @return NULL, no server silo is ever created.
 */
PVOID
NTAPI
PsGetCurrentServerSilo(VOID)
{
    return NULL;
}

/**
 * @brief Gets the server silo a process belongs to.
 * @return NULL, no server silo is ever created.
 */
PVOID
NTAPI
PsGetProcessServerSilo(
    _In_ PEPROCESS Process)
{
    UNREFERENCED_PARAMETER(Process);
    return NULL;
}

/**
 * @brief Tells whether the current thread runs in a server silo.
 */
BOOLEAN
NTAPI
PsIsCurrentThreadInServerSilo(VOID)
{
    return FALSE;
}

/**
 * @brief Gets the session the services of a server silo run in.
 *
 * @param[in] Silo
 * Server silo, NULL for the host.
 *
 * @return Session 0, where the host runs its services.
 */
ULONG
NTAPI
PsGetServerSiloServiceSessionId(
    _In_opt_ PVOID Silo)
{
    UNREFERENCED_PARAMETER(Silo);
    return 0;
}
