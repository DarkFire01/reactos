/*
 * PROJECT:     ReactOS ACPI Platform Extensions
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Resource Hub connection store
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * A connection is refcounted because a reparse walks from it to its provider and
 * on to an I/O target, all while ACPI could be tearing the device down. The
 * allocator seeds the count at 1, and that reference belongs to the list.
 */

#include "rhpriv.h"

/* Three list heads seeded to themselves, reference count 1 */
NTSTATUS
RhpAllocateConnection(
    _Outptr_ PRH_CONNECTION *Connection)
{
    PRH_CONNECTION NewConnection;

    *Connection = NULL;

    NewConnection = ExAllocatePoolWithTag(NonPagedPool,
                                          sizeof(*NewConnection),
                                          RH_POOL_TAG);
    if (NewConnection == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(NewConnection, sizeof(*NewConnection));

    InitializeListHead(&NewConnection->Link);
    InitializeListHead(&NewConnection->ProviderLink);
    InitializeListHead(&NewConnection->ContextList);
    NewConnection->ReferenceCount = 1;

    *Connection = NewConnection;
    return STATUS_SUCCESS;
}

VOID
RhpFreeConnection(
    _In_ PRH_CONNECTION Connection)
{
    if (Connection->ConnectionProperties != NULL)
        ExFreePoolWithTag(Connection->ConnectionProperties, RH_POOL_TAG);

    ExFreePoolWithTag(Connection, RH_POOL_TAG);
}

/*
 * Walks the hub's list for a matching id and takes a reference before dropping
 * the lock, because the caller is about to touch the provider behind it.
 */
PRH_CONNECTION
RhpFindAndReferenceConnectionById(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ LARGE_INTEGER Id)
{
    PRH_CONNECTION Connection = NULL;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&DeviceContext->Lock, &OldIrql);

    for (Entry = DeviceContext->ConnectionList.Flink;
         Entry != &DeviceContext->ConnectionList;
         Entry = Entry->Flink)
    {
        PRH_CONNECTION Candidate = CONTAINING_RECORD(Entry, RH_CONNECTION, Link);

        if (Candidate->Id.QuadPart == Id.QuadPart)
        {
            InterlockedIncrement(&Candidate->ReferenceCount);
            Connection = Candidate;
            break;
        }
    }

    KeReleaseSpinLock(&DeviceContext->Lock, OldIrql);
    return Connection;
}

/*
 * The same walk, keyed by the synthetic vector a GpioInt was given instead of
 * by a connection id.
 *
 * A GPIO interrupt is the one connection class that never reaches its consumer
 * as a connection id: it becomes an ordinary CmResourceTypeInterrupt and the
 * vector is all that survives. So the class extension that owns the pin has
 * nothing else to ask by, which is why the reference lets its
 * IOCTL_RH_QUERY_CONNECTION_PROPERTIES be keyed either way
 * (RH_QUERY_CONNECTION_PROPERTIES_INPUT_TYPE).
 */
PRH_CONNECTION
RhpFindAndReferenceConnectionByVector(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ ULONG InterruptVector)
{
    PRH_CONNECTION Connection = NULL;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    if (InterruptVector == 0)
        return NULL;

    KeAcquireSpinLock(&DeviceContext->Lock, &OldIrql);

    for (Entry = DeviceContext->ConnectionList.Flink;
         Entry != &DeviceContext->ConnectionList;
         Entry = Entry->Flink)
    {
        PRH_CONNECTION Candidate = CONTAINING_RECORD(Entry, RH_CONNECTION, Link);

        if (Candidate->InterruptVector == InterruptVector &&
            Candidate->Class == CM_RESOURCE_CONNECTION_CLASS_GPIO)
        {
            InterlockedIncrement(&Candidate->ReferenceCount);
            Connection = Candidate;
            break;
        }
    }

    KeReleaseSpinLock(&DeviceContext->Lock, OldIrql);
    return Connection;
}

VOID
RhpDereferenceConnection(
    _In_ PRH_CONNECTION Connection)
{
    if (InterlockedDecrement(&Connection->ReferenceCount) == 0)
        RhpFreeConnection(Connection);
}

/*
 * Ids are handed out as the lowest free positive integer, and the list is kept
 * sorted by id so the search is a single walk looking for the first gap. That
 * matters because ids are reused: a connection freed when a device disappears
 * leaves a hole the next one fills. Zero is never issued, because an id of zero
 * in a CmResourceTypeConnection descriptor means "not translated".
 *
 * Caller holds the device lock.
 */
NTSTATUS
RhpInsertConnectionLocked(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ PRH_CONNECTION Connection)
{
    PLIST_ENTRY Entry;
    PRH_CONNECTION Previous = NULL;
    LONGLONG NextId = 1;

    for (Entry = DeviceContext->ConnectionList.Flink;
         Entry != &DeviceContext->ConnectionList;
         Entry = Entry->Flink)
    {
        PRH_CONNECTION Candidate = CONTAINING_RECORD(Entry, RH_CONNECTION, Link);

        if (Candidate->Id.QuadPart > NextId)
            break;

        /* Every id below is taken, so the gap starts one past this one */
        NextId = Candidate->Id.QuadPart + 1;
        Previous = Candidate;

        if (NextId <= 0)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    Connection->Id.QuadPart = NextId;

    if (Previous != NULL)
        InsertHeadList(&Previous->Link, &Connection->Link);
    else
        InsertHeadList(&DeviceContext->ConnectionList, &Connection->Link);

    return STATUS_SUCCESS;
}

/*
 * Matches on the tuple that makes a connection unique to the firmware: the
 * provider it names, its class and type, and the descriptor bytes themselves.
 * Two devices declaring the identical Connection() share one id, which is what
 * lets a shared bus target be opened twice.
 *
 * Caller holds the device lock.
 */
PRH_CONNECTION
RhpFindConnectionLocked(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ PRH_PROVIDER Provider,
    _In_ UCHAR Class,
    _In_ UCHAR Type,
    _In_reads_bytes_(DescriptorLength) PVOID Descriptor,
    _In_ ULONG DescriptorLength)
{
    PLIST_ENTRY Entry;

    for (Entry = DeviceContext->ConnectionList.Flink;
         Entry != &DeviceContext->ConnectionList;
         Entry = Entry->Flink)
    {
        PRH_CONNECTION Candidate = CONTAINING_RECORD(Entry, RH_CONNECTION, Link);

        if (Candidate->Provider == Provider &&
            Candidate->Class == Class &&
            Candidate->Type == Type &&
            Candidate->PropertiesLength == DescriptorLength &&
            Candidate->ConnectionProperties != NULL &&
            RtlCompareMemory(Candidate->ConnectionProperties,
                             Descriptor,
                             DescriptorLength) == DescriptorLength)
        {
            return Candidate;
        }
    }

    return NULL;
}
