/*
 * PROJECT:     ReactOS ACPI driver (uACPI)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Start ordering from _DEP and from the GPIO controllers a device's resources name
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <uacpint.h>

#define NDEBUG
#include <debug.h>

static
VOID
NTAPI
UacpiNtPdoAddDependency(
    _Inout_ PUACPINT_PDO Pdo,
    _In_opt_ uacpi_namespace_node *Target)
{
    ULONG Index;

    if (!Target || Target == Pdo->Node || Pdo->DependCount >= UACPINT_MAX_DEPENDS)
        return;

    for (Index = 0; Index < Pdo->DependCount; Index++)
    {
        if (Pdo->Depends[Index] == Target)
            return;
    }

    Pdo->Depends[Pdo->DependCount++] = Target;
}

static
uacpi_iteration_decision
UacpiNtGpioSourceDependency(
    _In_ void *User,
    _In_ uacpi_resource *Resource)
{
    PUACPINT_PDO Pdo = User;
    uacpi_namespace_node *Target = NULL;

    /* A GPIO pin cannot be connected before its controller is up */
    if (Resource->type == UACPI_RESOURCE_TYPE_GPIO_CONNECTION &&
        Resource->gpio_connection.source.string != NULL &&
        Resource->gpio_connection.source.length != 0)
    {
        uacpi_namespace_node_resolve_from_aml_namepath(Pdo->Node,
                                                       Resource->gpio_connection.source.string,
                                                       &Target);
        UacpiNtPdoAddDependency(Pdo, Target);
    }

    return UACPI_ITERATION_DECISION_CONTINUE;
}

/**
 * @brief Records what the device waits for: its _DEP list and the controllers of its GPIO resources.
 */
VOID
NTAPI
UacpiNtPdoReadDependencies(
    _Inout_ PUACPINT_PDO Pdo)
{
    uacpi_namespace_node *Target;
    uacpi_object_array Package;
    uacpi_object *Result = NULL;
    uacpi_data_view Text;
    ULONG Index;

    PAGED_CODE();

    Pdo->DependCount = 0;
    if (!Pdo->Node)
        return;

    if (uacpi_likely_success(uacpi_eval_typed(Pdo->Node, "_DEP", NULL, UACPI_OBJECT_PACKAGE_BIT, &Result)) &&
        uacpi_likely_success(uacpi_object_get_package(Result, &Package)))
    {
        for (Index = 0; Index < Package.count; Index++)
        {
            /* Names in a package stay unresolved until looked up from the device */
            Target = NULL;
            if (uacpi_object_is_aml_namepath(Package.objects[Index]))
                uacpi_object_resolve_as_aml_namepath(Package.objects[Index], Pdo->Node, &Target);
            else if (uacpi_likely_success(uacpi_object_get_string(Package.objects[Index], &Text)))
                uacpi_namespace_node_resolve_from_aml_namepath(Pdo->Node, Text.text, &Target);

            UacpiNtPdoAddDependency(Pdo, Target);
        }
    }

    if (Result)
        uacpi_object_unref(Result);

    uacpi_for_each_device_resource(Pdo->Node, "_CRS", UacpiNtGpioSourceDependency, Pdo);

    if (Pdo->DependCount)
        DPRINT("uACPI-NT: %s waits for %lu device(s)\n", Pdo->Name, Pdo->DependCount);
}

/**
 * @brief Tells whether every device the PDO waits for has started. Caller holds ChildLock.
 *
 * @remarks A dependency with no present PDO of ours can never start here, so it does not hold anything back.
 */
BOOLEAN
NTAPI
UacpiNtPdoDependenciesMet(
    _In_ PUACPINT_FDO Fdo,
    _In_ PUACPINT_PDO Pdo)
{
    PUACPINT_PDO Other;
    PLIST_ENTRY Entry;
    ULONG Index;

    for (Index = 0; Index < Pdo->DependCount; Index++)
    {
        for (Entry = Fdo->ChildList.Flink; Entry != &Fdo->ChildList; Entry = Entry->Flink)
        {
            Other = CONTAINING_RECORD(Entry, UACPINT_PDO, Link);
            if (Other->Node != Pdo->Depends[Index])
                continue;

            if (Other->Present && !Other->Started)
            {
                if (!Pdo->HeldBack)
                    DPRINT1("uACPI-NT: %s waits for %s to start\n", Pdo->Name, Other->Name);
                return FALSE;
            }
            break;
        }
    }

    return TRUE;
}

/**
 * @brief Asks PnP to enumerate again every bus that holds a device back, now that one more device started.
 */
VOID
NTAPI
UacpiNtReleaseDependents(
    _In_ PUACPINT_FDO Fdo)
{
    uacpi_namespace_node *Sb = uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_SB);
    uacpi_namespace_node *Tz = uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_TZ);
    PDEVICE_OBJECT Buses[UACPINT_MAX_DEPENDS];
    PDEVICE_OBJECT Bus;
    PUACPINT_PDO Pdo, Parent;
    PLIST_ENTRY Entry, Inner;
    ULONG Count = 0, Index;

    ExAcquireFastMutex(&Fdo->ChildLock);

    for (Entry = Fdo->ChildList.Flink; Entry != &Fdo->ChildList; Entry = Entry->Flink)
    {
        Pdo = CONTAINING_RECORD(Entry, UACPINT_PDO, Link);
        if (!Pdo->HeldBack)
            continue;

        if (!UacpiNtPdoDependenciesMet(Fdo, Pdo))
            continue;

        Bus = NULL;
        if (Pdo->ScopeNode == Sb || Pdo->ScopeNode == Tz)
        {
            Bus = Fdo->PhysicalDeviceObject;
        }
        else
        {
            for (Inner = Fdo->ChildList.Flink; Inner != &Fdo->ChildList; Inner = Inner->Flink)
            {
                Parent = CONTAINING_RECORD(Inner, UACPINT_PDO, Link);
                if (Parent->Node == Pdo->ScopeNode)
                {
                    Bus = Parent->Shared.Self;
                    break;
                }
            }
        }

        for (Index = 0; Bus && Index < Count; Index++)
        {
            if (Buses[Index] == Bus)
                Bus = NULL;
        }

        if (Bus && Count < RTL_NUMBER_OF(Buses))
        {
            ObReferenceObject(Bus);
            Buses[Count++] = Bus;
        }

        DPRINT1("uACPI-NT: %s can start now, %s\n", Pdo->Name,
                Bus ? "enumerating its bus again" : "its bus is already queued or was not found");
    }

    ExReleaseFastMutex(&Fdo->ChildLock);

    for (Index = 0; Index < Count; Index++)
    {
        IoInvalidateDeviceRelations(Buses[Index], BusRelations);
        ObDereferenceObject(Buses[Index]);
    }
}
