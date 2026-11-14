/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Objects attached to a thread by key
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define TAG_PROPERTY_ENTRY  'rPsP'

typedef struct _PSP_PROPERTY_ENTRY
{
    LIST_ENTRY Link;
    ULONG_PTR Key;
    PVOID Object;
} PSP_PROPERTY_ENTRY, *PPSP_PROPERTY_ENTRY;

static
PPSP_PROPERTY_ENTRY
PspFindProperty(
    _In_ PPS_PROPERTY_SET PropertySet,
    _In_ ULONG_PTR Key)
{
    PLIST_ENTRY Entry;
    PPSP_PROPERTY_ENTRY Property;

    for (Entry = PropertySet->ListHead.Flink; Entry != &PropertySet->ListHead; Entry = Entry->Flink)
    {
        Property = CONTAINING_RECORD(Entry, PSP_PROPERTY_ENTRY, Link);
        if (Property->Key == Key)
            return Property;
    }

    return NULL;
}

VOID
NTAPI
PspInitializePropertySet(
    _Out_ PPS_PROPERTY_SET PropertySet)
{
    InitializeListHead(&PropertySet->ListHead);
    KeInitializeSpinLock(&PropertySet->Lock);
}

/**
 * @brief Drops every object still attached, the owner is going away.
 */
VOID
NTAPI
PspDeletePropertySet(
    _Inout_ PPS_PROPERTY_SET PropertySet)
{
    PPSP_PROPERTY_ENTRY Property;
    PLIST_ENTRY Entry;

    /* A thread that never got initialized has a zeroed set */
    if (PropertySet->ListHead.Flink == NULL)
        return;

    while (!IsListEmpty(&PropertySet->ListHead))
    {
        Entry = RemoveHeadList(&PropertySet->ListHead);
        Property = CONTAINING_RECORD(Entry, PSP_PROPERTY_ENTRY, Link);
        ObDereferenceObject(Property->Object);
        ExFreePoolWithTag(Property, TAG_PROPERTY_ENTRY);
    }
}

/**
 * @brief Returns the object attached to a thread under Key, referenced.
 *
 * @param[in] Flags
 * Only bit 0 is defined. Without it Windows also looks in the thread's job,
 * which only server silos fill, so there is nothing more to find here.
 *
 * @return The object, which the caller dereferences, or NULL.
 */
PVOID
NTAPI
PsGetThreadProperty(
    _In_ PETHREAD Thread,
    _In_ ULONG_PTR Key,
    _In_ ULONG Flags)
{
    PPSP_PROPERTY_ENTRY Property;
    PVOID Object = NULL;
    KIRQL OldIrql;

    if (Flags & ~1UL)
        return NULL;

    KeAcquireSpinLock(&Thread->PropertySet.Lock, &OldIrql);

    Property = PspFindProperty(&Thread->PropertySet, Key);
    if (Property)
    {
        Object = Property->Object;
        ObReferenceObject(Object);
    }

    KeReleaseSpinLock(&Thread->PropertySet.Lock, OldIrql);
    return Object;
}

/**
 * @brief Attaches an object to a thread under Key, or detaches it when Object is NULL.
 *
 * @return STATUS_OBJECT_NAME_COLLISION when Key is already in use.
 */
NTSTATUS
NTAPI
PsSetThreadProperty(
    _In_ PETHREAD Thread,
    _In_ ULONG_PTR Key,
    _In_opt_ PVOID Object)
{
    PPSP_PROPERTY_ENTRY Property;
    PPSP_PROPERTY_ENTRY NewProperty;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    if (Object == NULL)
    {
        KeAcquireSpinLock(&Thread->PropertySet.Lock, &OldIrql);
        Property = PspFindProperty(&Thread->PropertySet, Key);
        if (Property)
            RemoveEntryList(&Property->Link);
        KeReleaseSpinLock(&Thread->PropertySet.Lock, OldIrql);

        if (Property)
        {
            ObDereferenceObject(Property->Object);
            ExFreePoolWithTag(Property, TAG_PROPERTY_ENTRY);
        }

        return STATUS_SUCCESS;
    }

    NewProperty = ExAllocatePoolZero(NonPagedPool, sizeof(*NewProperty), TAG_PROPERTY_ENTRY);
    if (NewProperty == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    NewProperty->Key = Key;
    NewProperty->Object = Object;

    KeAcquireSpinLock(&Thread->PropertySet.Lock, &OldIrql);

    if (PspFindProperty(&Thread->PropertySet, Key))
    {
        Status = STATUS_OBJECT_NAME_COLLISION;
    }
    else
    {
        InsertHeadList(&Thread->PropertySet.ListHead, &NewProperty->Link);
        ObReferenceObject(Object);
    }

    KeReleaseSpinLock(&Thread->PropertySet.Lock, OldIrql);

    if (!NT_SUCCESS(Status))
        ExFreePoolWithTag(NewProperty, TAG_PROPERTY_ENTRY);

    return Status;
}
