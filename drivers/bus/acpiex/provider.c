/*
 * PROJECT:     ReactOS ACPI Platform Extensions
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Resource Hub providers and their controller targets
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * A provider is one ACPI ResourceSource name, the bus controller a Connection()
 * descriptor points at. Its target data is the controller device as currently
 * resolved, kept separate because the provider outlives it, so finding a
 * provider with no target is normal and one is connected on demand from its PDO.
 */

#include "rhpriv.h"

/* Three list heads seeded to themselves, reference count 1 */
NTSTATUS
RhpAllocateProvider(
    _Outptr_ PRH_PROVIDER *Provider)
{
    PRH_PROVIDER NewProvider;

    *Provider = NULL;

    NewProvider = ExAllocatePoolWithTag(NonPagedPool,
                                        sizeof(*NewProvider),
                                        RH_POOL_TAG);
    if (NewProvider == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(NewProvider, sizeof(*NewProvider));

    InitializeListHead(&NewProvider->Link);
    InitializeListHead(&NewProvider->ConnectionList);
    InitializeListHead(&NewProvider->TargetList);
    NewProvider->ReferenceCount = 1;

    *Provider = NewProvider;
    return STATUS_SUCCESS;
}

VOID
RhpFreeProvider(
    _In_ PRH_PROVIDER Provider)
{
    if (Provider->DeviceObject != NULL)
        ObDereferenceObject(Provider->DeviceObject);

    RhpFreeUnicodeString(&Provider->BiosName);
    ExFreePoolWithTag(Provider, RH_POOL_TAG);
}

VOID
RhpDereferenceProvider(
    _In_ PRH_PROVIDER Provider)
{
    if (InterlockedDecrement(&Provider->ReferenceCount) == 0)
        RhpFreeProvider(Provider);
}

VOID
RhpDereferenceTargetData(
    _In_ PRH_TARGET_DATA TargetData)
{
    if (InterlockedDecrement(&TargetData->ReferenceCount) == 0)
    {
        if (TargetData->IoTarget != NULL)
            WdfObjectDelete(TargetData->IoTarget);

        RhpFreeUnicodeString(&TargetData->ControllerName);
        ExFreePoolWithTag(TargetData, RH_POOL_TAG);
    }
}

/*
 * Takes a reference on the provider's current target data, if it has one.
 *
 * A provider with no target is not an error: it means ACPI has named the
 * controller in a Connection() descriptor but has not yet told us which device
 * object it is, so there is nothing to reparse to. Target data appears when ACPI
 * calls AssociateBiosName (translate.c), and is replaced wholesale each
 * time it does, so a controller that goes away and comes back resolves to its
 * new device object rather than a stale name.
 *
 * The reference additionally retries the connect inline here, from the PDO it
 * already holds on the provider. Ours does not: the same work happens on ACPI's
 * registration call, and doing it again on the create path would mean querying a
 * device property while holding a create IRP.
 */
PRH_TARGET_DATA
RhpReferenceProviderTargetData(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ PRH_PROVIDER Provider)
{
    PRH_TARGET_DATA TargetData;
    KIRQL OldIrql;

    KeAcquireSpinLock(&DeviceContext->Lock, &OldIrql);

    TargetData = Provider->TargetData;
    if (TargetData != NULL)
        InterlockedIncrement(&TargetData->ReferenceCount);

    KeReleaseSpinLock(&DeviceContext->Lock, OldIrql);
    return TargetData;
}
