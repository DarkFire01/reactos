/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NT 6+ HAL exports shared by every x86 HAL image
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The routines here complete the export surface newer kernels and drivers
 * expect from a HAL: processor hot-add hooks, errata registration, the
 * hardware error bugcheck, performance-counter allocation and the UEFI
 * variable services. None of them has real hardware behind it on this HAL
 * yet, so each answers the way a HAL without the feature does.
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

#ifndef WHEA_UNCORRECTABLE_ERROR
#define WHEA_UNCORRECTABLE_ERROR 0x124
#endif

/* FUNCTIONS *****************************************************************/

/**
 * @brief
 * Returns how many processors the machine can ever have. Without hot-add
 * that is the number that came up at boot.
 */
ULONG
NTAPI
HalQueryMaximumProcessorCount(VOID)
{
    return KeNumberProcessors;
}

/**
 * @brief
 * Registers a processor that appeared after boot. Hot-add is not
 * supported, so nothing can be registered.
 */
NTSTATUS
NTAPI
HalRegisterDynamicProcessor(
    _In_ ULONG ProcessorNumber,
    _In_ ULONG ProcessorId)
{
    UNREFERENCED_PARAMETER(ProcessorNumber);
    UNREFERENCED_PARAMETER(ProcessorId);

    return STATUS_NOT_SUPPORTED;
}

/**
 * @brief
 * Registers the HAL's chipset errata callbacks with the kernel. There is
 * no errata manager to register with, so there is nothing to do.
 */
NTSTATUS
NTAPI
HalRegisterErrataCallbacks(VOID)
{
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Stops the machine on an uncorrectable hardware error report.
 *
 * @param[in] ErrorSource
 * The error source that raised the report.
 *
 * @param[in] ErrorSource
 * The error source that raised the fault, on Win7 and later.
 *
 * @param[in] ErrorRecord
 * The error record describing the fault.
 */
DECLSPEC_NORETURN
VOID
NTAPI
HalBugCheckSystem(
    _In_ PWHEA_ERROR_SOURCE_DESCRIPTOR ErrorSource,
    _In_ PWHEA_ERROR_RECORD ErrorRecord)
{
    KeBugCheckEx(WHEA_UNCORRECTABLE_ERROR,
                 0,
                 (ULONG_PTR)ErrorRecord,
                 (ULONG_PTR)ErrorSource,
                 0);
}

/**
 * @brief
 * Reserves hardware performance counters for a caller. Counter
 * management is not implemented.
 */
NTSTATUS
NTAPI
HalAllocateHardwareCounters(
    _In_reads_(GroupCount) PGROUP_AFFINITY GroupAffinty,
    _In_ ULONG GroupCount,
    _In_ PPHYSICAL_COUNTER_RESOURCE_LIST ResourceList,
    _Out_ PHANDLE CounterSetHandle)
{
    UNREFERENCED_PARAMETER(GroupAffinty);
    UNREFERENCED_PARAMETER(GroupCount);
    UNREFERENCED_PARAMETER(ResourceList);

    *CounterSetHandle = NULL;
    return STATUS_NOT_SUPPORTED;
}

/**
 * @brief
 * Releases counters taken with HalAllocateHardwareCounters.
 */
NTSTATUS
NTAPI
HalFreeHardwareCounters(
    _In_ HANDLE CounterSetHandle)
{
    UNREFERENCED_PARAMETER(CounterSetHandle);

    return STATUS_INVALID_HANDLE;
}

/*
 * The UEFI variable services need runtime services the loader does not
 * hand over yet, so they fail the same way a BIOS-booted HAL does.
 */

/**
 * @brief
 * Reads a firmware variable identified by name and vendor GUID.
 */
NTSTATUS
NTAPI
HalGetEnvironmentVariableEx(
    _In_ PCWSTR VariableName,
    _In_ LPGUID VendorGuid,
    _Out_writes_bytes_opt_(*ValueLength) PVOID Value,
    _Inout_ PULONG ValueLength,
    _Out_opt_ PULONG Attributes)
{
    UNREFERENCED_PARAMETER(VariableName);
    UNREFERENCED_PARAMETER(VendorGuid);
    UNREFERENCED_PARAMETER(Value);
    UNREFERENCED_PARAMETER(ValueLength);
    UNREFERENCED_PARAMETER(Attributes);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Writes a firmware variable identified by name and vendor GUID.
 */
NTSTATUS
NTAPI
HalSetEnvironmentVariableEx(
    _In_ PCWSTR VariableName,
    _In_ LPGUID VendorGuid,
    _In_reads_bytes_opt_(ValueLength) PVOID Value,
    _In_ ULONG ValueLength,
    _In_ ULONG Attributes)
{
    UNREFERENCED_PARAMETER(VariableName);
    UNREFERENCED_PARAMETER(VendorGuid);
    UNREFERENCED_PARAMETER(Value);
    UNREFERENCED_PARAMETER(ValueLength);
    UNREFERENCED_PARAMETER(Attributes);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Lists the firmware variables.
 */
NTSTATUS
NTAPI
HalEnumerateEnvironmentVariablesEx(
    _In_ ULONG InformationClass,
    _Out_writes_bytes_opt_(*BufferLength) PVOID Buffer,
    _Inout_ PULONG BufferLength)
{
    UNREFERENCED_PARAMETER(InformationClass);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(BufferLength);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Reports the firmware variable store capacity.
 */
NTSTATUS
NTAPI
HalQueryEnvironmentVariableInfoEx(
    _In_ ULONG Attributes,
    _Out_ PULONGLONG MaximumVariableStorageSize,
    _Out_ PULONGLONG RemainingVariableStorageSize,
    _Out_ PULONGLONG MaximumVariableSize)
{
    UNREFERENCED_PARAMETER(Attributes);
    UNREFERENCED_PARAMETER(MaximumVariableStorageSize);
    UNREFERENCED_PARAMETER(RemainingVariableStorageSize);
    UNREFERENCED_PARAMETER(MaximumVariableSize);

    return STATUS_NOT_IMPLEMENTED;
}

/* EOF */
