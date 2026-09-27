/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Performance counters, for drivers that publish them
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A driver publishes counters by registering a set of them and then adding an
 * instance for each thing it counts. Nothing here collects any, so a set is
 * taken and remembered by nothing at all.
 *
 * Registering still has to succeed, and has to hand back something: a driver
 * that cannot register its counters treats that as a reason not to load, and
 * says so with whatever the registration answered. So a set is a small piece of
 * memory that means only that the driver asked for one.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/*
 * The kernel builds against an older interface than these belong to, so the
 * names are written out here rather than taken from the header that hides them.
 * Nothing here looks inside one, which is why the shapes are not needed.
 */
typedef struct _PCW_REGISTRATION *PPCW_REGISTRATION;
typedef struct _PCW_REGISTRATION_INFORMATION *PPCW_REGISTRATION_INFORMATION;
typedef struct _PCW_BUFFER *PPCW_BUFFER;
typedef struct _PCW_DATA *PPCW_DATA;

/* FUNCTIONS ******************************************************************/

/* What a registered set is made of, which is nothing but the fact of it */
#define TAG_PCW 'wcpE'

/**
 * @brief
 * Registers a set of counters a driver is willing to publish.
 *
 * @remarks
 * The set is not read and the counters in it are never asked for. What matters
 * to the caller is that it was taken.
 */
NTSTATUS
NTAPI
PcwRegister(
    _Outptr_ PPCW_REGISTRATION *Registration,
    _In_ PPCW_REGISTRATION_INFORMATION Info)
{
    PVOID Set;

    UNREFERENCED_PARAMETER(Info);

    Set = ExAllocatePoolZero(NonPagedPool, sizeof(ULONG), TAG_PCW);
    if (Set == NULL)
    {
        *Registration = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *Registration = Set;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Takes back a set of counters.
 */
VOID
NTAPI
PcwUnregister(
    _In_ PPCW_REGISTRATION Registration)
{
    if (Registration != NULL)
        ExFreePoolWithTag(Registration, TAG_PCW);
}

/**
 * @brief
 * Adds one instance of a counter set to what a collector asked for.
 *
 * @return
 * STATUS_NOT_SUPPORTED, because nothing here ever asks.
 */
NTSTATUS
NTAPI
PcwAddInstance(
    _In_ PPCW_BUFFER Buffer,
    _In_ PCUNICODE_STRING Name,
    _In_ ULONG Id,
    _In_ ULONG Count,
    _In_reads_(Count) PPCW_DATA Data)
{
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Name);
    UNREFERENCED_PARAMETER(Id);
    UNREFERENCED_PARAMETER(Count);
    UNREFERENCED_PARAMETER(Data);

    return STATUS_NOT_SUPPORTED;
}

/* EOF */
