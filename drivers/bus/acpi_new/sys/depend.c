/*
 * PROJECT:     ReactOS ACPI driver (uACPI)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Start ordering from _DEP and from the controllers a device's resources name
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

    if (!Target || Target == Pdo->Node)
        return;

    for (Index = 0; Index < Pdo->DependCount; Index++)
    {
        if (Pdo->Depends[Index] == Target)
            return;
    }

    if (Pdo->DependCount >= UACPINT_MAX_DEPENDS)
    {
        DPRINT1("uACPI-NT: %s names more than %u providers, ignoring the rest\n", Pdo->Name, UACPINT_MAX_DEPENDS);
        return;
    }

    Pdo->Depends[Pdo->DependCount++] = Target;
}

/* Package entries are names that stay unresolved until looked up from the scope */
static
uacpi_namespace_node *
NTAPI
UacpiNtResolvePackageName(
    _In_ uacpi_namespace_node *Scope,
    _In_ uacpi_object *Entry)
{
    uacpi_namespace_node *Target = NULL;
    uacpi_data_view Text;

    if (uacpi_object_is_aml_namepath(Entry))
        uacpi_object_resolve_as_aml_namepath(Entry, Scope, &Target);
    else if (uacpi_likely_success(uacpi_object_get_string(Entry, &Text)))
        uacpi_namespace_node_resolve_from_aml_namepath(Scope, Text.text, &Target);

    return Target;
}

/**
 * @brief Adds every entry of the _DEP under Scope.
 *
 * @remarks An entry that does not name a Device object is a firmware error and stops the machine.
 */
static
VOID
NTAPI
UacpiNtPdoAddDepList(
    _Inout_ PUACPINT_PDO Pdo,
    _In_ uacpi_namespace_node *Scope)
{
    uacpi_namespace_node *Target;
    uacpi_object_type Type;
    uacpi_object_array Package;
    uacpi_object *Result = NULL;
    ULONG Index;

    if (uacpi_unlikely_error(uacpi_eval(Scope, "_DEP", NULL, &Result)) || !Result)
        return;

    if (uacpi_unlikely_error(uacpi_object_get_package(Result, &Package)))
    {
        DPRINT1("uACPI-NT: _DEP of %s is not a package, ignoring it\n", Pdo->Name);
        uacpi_object_unref(Result);
        return;
    }

    for (Index = 0; Index < Package.count; Index++)
    {
        Target = UacpiNtResolvePackageName(Scope, Package.objects[Index]);
        if (!Target)
        {
            DPRINT1("uACPI-NT: _DEP entry %lu of %s does not resolve\n", Index, Pdo->Name);
            KeBugCheckEx(ACPI_BIOS_ERROR, 0x18, (ULONG_PTR)Scope, Index, 0);
        }

        if (uacpi_unlikely_error(uacpi_namespace_node_type(Target, &Type)) || Type != UACPI_OBJECT_DEVICE)
        {
            DPRINT1("uACPI-NT: _DEP entry %lu of %s is not a device\n", Index, Pdo->Name);
            KeBugCheckEx(ACPI_BIOS_ERROR, 0x19, (ULONG_PTR)Scope, Index, (ULONG_PTR)Target);
        }

        UacpiNtPdoAddDependency(Pdo, Target);
    }

    uacpi_object_unref(Result);
}

/* The first power resource a _PRx or _PRW lists brings its own _DEP along */
static
VOID
NTAPI
UacpiNtPdoAddPowerResourceDeps(
    _Inout_ PUACPINT_PDO Pdo,
    _In_z_ const char *Method,
    _In_ ULONG FirstIndex)
{
    uacpi_namespace_node *Resource;
    uacpi_object_type Type;
    uacpi_object_array Package;
    uacpi_object *Result = NULL;

    if (uacpi_unlikely_error(uacpi_eval(Pdo->Node, Method, NULL, &Result)) || !Result)
        return;

    if (uacpi_likely_success(uacpi_object_get_package(Result, &Package)) && Package.count > FirstIndex)
    {
        Resource = UacpiNtResolvePackageName(Pdo->Node, Package.objects[FirstIndex]);
        if (Resource &&
            uacpi_likely_success(uacpi_namespace_node_type(Resource, &Type)) &&
            Type == UACPI_OBJECT_POWER_RESOURCE)
        {
            UacpiNtPdoAddDepList(Pdo, Resource);
        }
    }

    uacpi_object_unref(Result);
}

static
uacpi_iteration_decision
UacpiNtConnectionSourceDependency(
    _In_ void *User,
    _In_ uacpi_resource *Resource)
{
    PUACPINT_PDO Pdo = User;
    uacpi_resource_source *Source;
    uacpi_namespace_node *Target = NULL;

    /* GPIO, serial bus and pin descriptors are served by the controller they name */
    switch (Resource->type)
    {
        case UACPI_RESOURCE_TYPE_GPIO_CONNECTION:
            Source = &Resource->gpio_connection.source;
            break;

        case UACPI_RESOURCE_TYPE_SERIAL_I2C_CONNECTION:
        case UACPI_RESOURCE_TYPE_SERIAL_SPI_CONNECTION:
        case UACPI_RESOURCE_TYPE_SERIAL_UART_CONNECTION:
        case UACPI_RESOURCE_TYPE_SERIAL_CSI2_CONNECTION:
            Source = &Resource->i2c_connection.common.source;
            break;

        case UACPI_RESOURCE_TYPE_PIN_FUNCTION:
            Source = &Resource->pin_function.source;
            break;

        case UACPI_RESOURCE_TYPE_PIN_CONFIGURATION:
            Source = &Resource->pin_configuration.source;
            break;

        case UACPI_RESOURCE_TYPE_PIN_GROUP_FUNCTION:
            Source = &Resource->pin_group_function.source;
            break;

        case UACPI_RESOURCE_TYPE_PIN_GROUP_CONFIGURATION:
            Source = &Resource->pin_group_configuration.source;
            break;

        default:
            return UACPI_ITERATION_DECISION_CONTINUE;
    }

    if (Source->string != NULL && Source->length != 0)
    {
        uacpi_namespace_node_resolve_from_aml_namepath(Pdo->Node, Source->string, &Target);
        UacpiNtPdoAddDependency(Pdo, Target);
    }

    return UACPI_ITERATION_DECISION_CONTINUE;
}

/**
 * @brief Records what the device waits for: _DEP, the _DEP of its power resources and the providers of its connection resources.
 */
VOID
NTAPI
UacpiNtPdoReadDependencies(
    _Inout_ PUACPINT_PDO Pdo)
{
    PAGED_CODE();

    Pdo->DependCount = 0;
    if (!Pdo->Node)
        return;

    UacpiNtPdoAddDepList(Pdo, Pdo->Node);

    UacpiNtPdoAddPowerResourceDeps(Pdo, "_PRW", 2);
    UacpiNtPdoAddPowerResourceDeps(Pdo, "_PR0", 0);
    UacpiNtPdoAddPowerResourceDeps(Pdo, "_PR1", 0);
    UacpiNtPdoAddPowerResourceDeps(Pdo, "_PR2", 0);
    UacpiNtPdoAddPowerResourceDeps(Pdo, "_PR3", 0);
    UacpiNtPdoAddPowerResourceDeps(Pdo, "_PRR", 0);

    uacpi_for_each_device_resource(Pdo->Node, "_CRS", UacpiNtConnectionSourceDependency, Pdo);

    if (Pdo->DependCount)
        DPRINT("uACPI-NT: %s waits for %lu device(s)\n", Pdo->Name, Pdo->DependCount);
}

/**
 * @brief Tells whether every provider the PDO waits for has a started stack. Caller holds ChildLock.
 *
 * @remarks A provider with no device of ours, or one that is gone, keeps the PDO waiting.
 */
BOOLEAN
NTAPI
UacpiNtPdoDependenciesMet(
    _In_ PUACPINT_FDO Fdo,
    _In_ PUACPINT_PDO Pdo)
{
    PUACPINT_PDO Other;
    PUACPINT_FLT Filter;
    PLIST_ENTRY Entry;
    uacpi_object_name Name;
    BOOLEAN Found, Started;
    ULONG Index;

    for (Index = 0; Index < Pdo->DependCount; Index++)
    {
        Found = FALSE;
        Started = FALSE;

        for (Entry = Fdo->ChildList.Flink; Entry != &Fdo->ChildList; Entry = Entry->Flink)
        {
            Other = CONTAINING_RECORD(Entry, UACPINT_PDO, Link);
            if (Other->Node == Pdo->Depends[Index])
            {
                Found = TRUE;
                Started = Other->Present && Other->StackStarted;
                break;
            }
        }

        for (Entry = Fdo->FilterList.Flink; !Found && Entry != &Fdo->FilterList; Entry = Entry->Flink)
        {
            Filter = CONTAINING_RECORD(Entry, UACPINT_FLT, Link);
            if (Filter->Node == Pdo->Depends[Index])
            {
                Found = TRUE;
                Started = Filter->StackStarted;
            }
        }

        if (!Started)
        {
            if (!Pdo->HeldBack)
            {
                Name = uacpi_namespace_node_name(Pdo->Depends[Index]);
                DPRINT1("uACPI-NT: %s waits for %.4s to %s\n", Pdo->Name, Name.text,
                        Found ? "start" : "be enumerated");
            }
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * @brief Asks PnP to enumerate again every bus that holds a device back, now that one more provider started.
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
    PUACPINT_FLT Filter;
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

            /* A child of a PCI device is enumerated through the filter over its PDO */
            for (Inner = Fdo->FilterList.Flink; !Bus && Inner != &Fdo->FilterList; Inner = Inner->Flink)
            {
                Filter = CONTAINING_RECORD(Inner, UACPINT_FLT, Link);
                if (Filter->Node == Pdo->ScopeNode)
                    Bus = Filter->ForeignPdo;
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
