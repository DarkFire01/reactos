/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Server silos, of which this system has none
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/*
 * A silo is the container a process can be placed in, and a server silo is one
 * that carries a whole session of its own. Nothing here creates one, so every
 * process belongs to the host silo, which is named by a null pointer. That is
 * the same answer the shipping kernel gives for a process outside a container,
 * so a driver that asks gets what it expects rather than a refusal.
 */

/* TYPES **********************************************************************/

typedef struct _PSP_SILO_MONITOR
{
    LIST_ENTRY Link;
    SILO_MONITOR_REGISTRATION Registration;
    BOOLEAN Started;
} PSP_SILO_MONITOR, *PPSP_SILO_MONITOR;

/* GLOBALS ********************************************************************/

static LIST_ENTRY PspSiloMonitorList = { &PspSiloMonitorList, &PspSiloMonitorList };
static KGUARDED_MUTEX PspSiloMonitorLock;
static BOOLEAN PspSiloMonitorLockReady = FALSE;

/* FUNCTIONS ******************************************************************/

static
VOID
NTAPI
PspEnsureSiloMonitorLock(VOID)
{
    /*
     * The list is only ever touched by a driver loading or unloading, so the
     * lock is made on the first of those rather than from phase one.
     */
    if (!PspSiloMonitorLockReady)
    {
        KeInitializeGuardedMutex(&PspSiloMonitorLock);
        PspSiloMonitorLockReady = TRUE;
    }
}

/*
 * @implemented
 */
PESILO
NTAPI
PsGetProcessServerSilo(
    _In_ PEPROCESS Process)
{
    UNREFERENCED_PARAMETER(Process);

    /* Every process here is in the host silo */
    return NULL;
}

/*
 * @implemented
 */
PESILO
NTAPI
PsGetThreadServerSilo(
    _In_ PETHREAD Thread)
{
    UNREFERENCED_PARAMETER(Thread);

    /* A thread has no silo of its own, and its process is in the host one */
    return NULL;
}

/*
 * @implemented
 */
PESILO
NTAPI
PsGetCurrentServerSilo(VOID)
{
    return NULL;
}

/*
 * @implemented
 */
PESILO
NTAPI
PsGetHostSilo(VOID)
{
    /* The host silo is the one everything is already in, and it is named by nothing */
    return NULL;
}

/*
 * @implemented
 */
PESILO
NTAPI
PsAttachSiloToCurrentThread(
    _In_ PESILO Silo)
{
    UNREFERENCED_PARAMETER(Silo);

    /*
     * Attaching gives back the silo the thread was in so the caller can put it
     * back. A thread here is never in one, so that is what it gets back.
     */
    return NULL;
}

/*
 * @implemented
 */
VOID
NTAPI
PsDetachSiloFromCurrentThread(
    _In_ PESILO Silo)
{
    UNREFERENCED_PARAMETER(Silo);
}

/*
 * @implemented
 */
PGUID
NTAPI
PsGetSiloContainerId(
    _In_ PESILO Silo)
{
    /*
     * The shipping kernel hands back a pointer into the silo rather than a
     * copy, and nothing to point at when there is no silo.
     */
    if (Silo == NULL)
        return NULL;

    return NULL;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
PsRegisterSiloMonitor(
    _In_ PSILO_MONITOR_REGISTRATION Registration,
    _Outptr_ PSERVER_SILO_MONITOR *ReturnedMonitor)
{
    PPSP_SILO_MONITOR Monitor;

    PAGED_CODE();

    if ((Registration == NULL) || (ReturnedMonitor == NULL))
        return STATUS_INVALID_PARAMETER;

    Monitor = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Monitor), TAG_PS_SILO);
    if (Monitor == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Monitor->Registration = *Registration;

    PspEnsureSiloMonitorLock();

    KeAcquireGuardedMutex(&PspSiloMonitorLock);
    InsertTailList(&PspSiloMonitorList, &Monitor->Link);
    KeReleaseGuardedMutex(&PspSiloMonitorLock);

    *ReturnedMonitor = (PSERVER_SILO_MONITOR)Monitor;
    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
PsStartSiloMonitor(
    _In_ PSERVER_SILO_MONITOR Monitor)
{
    PPSP_SILO_MONITOR Entry = (PPSP_SILO_MONITOR)Monitor;

    PAGED_CODE();

    if (Entry == NULL)
        return STATUS_INVALID_PARAMETER;

    /*
     * Starting one walks the silos that already exist and tells the monitor
     * about each. There are none, so there is nothing to tell it about, and
     * the monitor simply becomes live for silos made later.
     */
    Entry->Started = TRUE;
    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
VOID
NTAPI
PsUnregisterSiloMonitor(
    _In_ PSERVER_SILO_MONITOR Monitor)
{
    PPSP_SILO_MONITOR Entry = (PPSP_SILO_MONITOR)Monitor;

    PAGED_CODE();

    if (Entry == NULL)
        return;

    PspEnsureSiloMonitorLock();

    KeAcquireGuardedMutex(&PspSiloMonitorLock);
    RemoveEntryList(&Entry->Link);
    KeReleaseGuardedMutex(&PspSiloMonitorLock);

    ExFreePoolWithTag(Entry, TAG_PS_SILO);
}

/* EOF */
