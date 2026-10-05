/*
 * PROJECT:     ReactOS USB Host Controller Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     KMDF class library registration and client binding
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "ucx01000.h"

#define NDEBUG
#include <debug.h>

extern "C"
{
WDF_CLASS_LIBRARY_INFO UcxClassLibraryInfo =
{
    sizeof(WDF_CLASS_LIBRARY_INFO),
    { UCX_CLASS_MAJOR_VERSION, UCX_CLASS_MINOR_VERSION, 0 },
    UcxClassLibraryInitialize,
    UcxClassLibraryDeinitialize,
    UcxClassLibraryBindClient,
    UcxClassLibraryUnbindClient
};
}

/** Number of table slots a client built against the given minor expects. */
static
ULONG
NTAPI
UcxTableEntriesForMinor(
    _In_ ULONG Minor)
{
    if (Minor >= 4)
        return UcxFunctionTableNumEntries;

    /* SetIdStrings arrived in 1.3 */
    if (Minor == 3)
        return UcxControllerSetIdStringsTableIndex + 1;

    return UcxInitializeDeviceInitTableIndex + 1;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
UcxClassLibraryInitialize(VOID)
{
    PAGED_CODE();

    UcxBuildExportTable();
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
UcxClassLibraryDeinitialize(VOID)
{
    PAGED_CODE();
}

/*
 * ComponentGlobals is the client's PWDF_DRIVER_GLOBALS passed by value, not
 * the address of it. That is what the Windows client stub hands over.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
UcxClassLibraryBindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _Out_ PWDF_COMPONENT_GLOBALS *ComponentGlobals)
{
    PUCX_DRIVER_GLOBALS *ClientGlobals;
    PUCX_DRIVER_GLOBALS Globals;
    ULONG Minor;

    PAGED_CODE();

    ClientGlobals = (PUCX_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo;
    if (ClientGlobals == NULL)
        return STATUS_INVALID_PARAMETER;

    /* KMDF unbinds even after a failed bind, so clear the slot up front */
    *ClientGlobals = NULL;

    Minor = ClassBindInfo->Version.Minor;
    if (ClassBindInfo->Version.Major != UCX_CLASS_MAJOR_VERSION || Minor == 0 || Minor > UCX_CLASS_MINOR_VERSION)
    {
        DPRINT1("Client wants UCX %lu.%lu, have %u.%u\n",
                ClassBindInfo->Version.Major,
                Minor,
                UCX_CLASS_MAJOR_VERSION,
                UCX_CLASS_MINOR_VERSION);
        return STATUS_INVALID_PARAMETER;
    }

    if (ClassBindInfo->FunctionTableCount != UcxTableEntriesForMinor(Minor))
    {
        DPRINT1("UCX 1.%lu client has %lu table slots\n", Minor, ClassBindInfo->FunctionTableCount);
        return STATUS_INVALID_PARAMETER;
    }

    /* Filled even if the allocation below fails, as on Windows */
    RtlCopyMemory(ClassBindInfo->FunctionTable,
                  UcxExportTable,
                  ClassBindInfo->FunctionTableCount * sizeof(UcxExportTable[0]));

    Globals = (PUCX_DRIVER_GLOBALS)ExAllocatePoolZero(NonPagedPool, sizeof(*Globals), UCX_POOL_TAG);
    if (Globals == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Globals->Size = sizeof(*Globals);
    Globals->WdfDriverGlobals = (PWDF_DRIVER_GLOBALS)ComponentGlobals;

    *ClientGlobals = Globals;
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
UcxClassLibraryUnbindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS *ComponentGlobals)
{
    PUCX_DRIVER_GLOBALS *ClientGlobals;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(ComponentGlobals);

    ClientGlobals = (PUCX_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo;
    if (ClientGlobals == NULL || *ClientGlobals == NULL)
        return;

    ExFreePoolWithTag(*ClientGlobals, UCX_POOL_TAG);
    *ClientGlobals = NULL;
}
