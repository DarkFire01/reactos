/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel mode entry points for services without a system call
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* An extended parameter of a virtual memory allocation */
typedef struct DECLSPEC_ALIGN(8) _MEM_EXTENDED_PARAMETER
{
    struct
    {
        ULONG64 Type : 8;
        ULONG64 Reserved : 56;
    };
    union
    {
        ULONG64 ULong64;
        PVOID Pointer;
        SIZE_T Size;
        HANDLE Handle;
        ULONG ULong;
    };
} MEM_EXTENDED_PARAMETER, *PMEM_EXTENDED_PARAMETER;

typedef struct _MEM_ADDRESS_REQUIREMENTS
{
    PVOID LowestStartingAddress;
    PVOID HighestEndingAddress;
    SIZE_T Alignment;
} MEM_ADDRESS_REQUIREMENTS, *PMEM_ADDRESS_REQUIREMENTS;

#define MEM_EXTENDED_PARAMETER_ADDRESS_REQUIREMENTS 1

/* FUNCTIONS *****************************************************************/

/**
 * @brief Reserves or commits virtual memory with extended parameters.
 *
 * @remarks Only address requirements are understood. A highest ending address
 * becomes the matching number of zero bits, an alignment above the allocation
 * granularity or a lowest starting address is not supported.
 */
NTSTATUS
NTAPI
ZwAllocateVirtualMemoryEx(
    _In_ HANDLE ProcessHandle,
    _Inout_ PVOID *BaseAddress,
    _Inout_ PSIZE_T RegionSize,
    _In_ ULONG AllocationType,
    _In_ ULONG PageProtection,
    _Inout_updates_opt_(ExtendedParameterCount) PMEM_EXTENDED_PARAMETER ExtendedParameters,
    _In_ ULONG ExtendedParameterCount)
{
    PMEM_ADDRESS_REQUIREMENTS Requirements;
    ULONG_PTR Highest, ZeroBits = 0;
    ULONG i;

    for (i = 0; i < ExtendedParameterCount; i++)
    {
        if (ExtendedParameters[i].Type != MEM_EXTENDED_PARAMETER_ADDRESS_REQUIREMENTS)
            return STATUS_NOT_SUPPORTED;

        Requirements = ExtendedParameters[i].Pointer;
        if (Requirements->LowestStartingAddress ||
            (Requirements->Alignment > MM_ALLOCATION_GRANULARITY))
        {
            return STATUS_NOT_SUPPORTED;
        }

        /* Take as many zero bits as keep the allocation below the highest address */
        Highest = (ULONG_PTR)Requirements->HighestEndingAddress;
        if (Highest)
        {
            while ((MAXULONG_PTR >> ZeroBits) > Highest)
                ZeroBits++;
        }
    }

    return ZwAllocateVirtualMemory(ProcessHandle,
                                   BaseAddress,
                                   ZeroBits,
                                   RegionSize,
                                   AllocationType,
                                   PageProtection);
}

/**
 * @brief Flushes the cached data of a file.
 * @remarks Only a full flush is done, so any flags still flush everything.
 */
NTSTATUS
NTAPI
ZwFlushBuffersFileEx(
    _In_ HANDLE FileHandle,
    _In_ ULONG Flags,
    _In_reads_bytes_(ParametersSize) PVOID Parameters,
    _In_ ULONG ParametersSize,
    _Out_ PIO_STATUS_BLOCK IoStatusBlock)
{
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(Parameters);
    UNREFERENCED_PARAMETER(ParametersSize);

    return ZwFlushBuffersFile(FileHandle, IoStatusBlock);
}

/**
 * @brief Creates a notification state name.
 * @return STATUS_NOT_IMPLEMENTED, there is no notification facility to name.
 */
NTSTATUS
NTAPI
ZwCreateWnfStateName(
    _Out_ PULONG64 StateName,
    _In_ ULONG NameLifetime,
    _In_ ULONG DataScope,
    _In_ BOOLEAN PersistData,
    _In_opt_ PVOID TypeId,
    _In_ ULONG MaximumStateSize,
    _In_ PSECURITY_DESCRIPTOR SecurityDescriptor)
{
    UNREFERENCED_PARAMETER(NameLifetime);
    UNREFERENCED_PARAMETER(DataScope);
    UNREFERENCED_PARAMETER(PersistData);
    UNREFERENCED_PARAMETER(TypeId);
    UNREFERENCED_PARAMETER(MaximumStateSize);
    UNREFERENCED_PARAMETER(SecurityDescriptor);

    *StateName = 0;
    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief Deletes a notification state name.
 */
NTSTATUS
NTAPI
ZwDeleteWnfStateName(
    _In_ PULONG64 StateName)
{
    UNREFERENCED_PARAMETER(StateName);
    return STATUS_NOT_FOUND;
}

/**
 * @brief Opens a memory partition.
 * @return STATUS_NOT_SUPPORTED, all memory belongs to the one system partition.
 */
NTSTATUS
NTAPI
ZwOpenPartition(
    _Out_ PHANDLE PartitionHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ POBJECT_ATTRIBUTES ObjectAttributes)
{
    UNREFERENCED_PARAMETER(DesiredAccess);
    UNREFERENCED_PARAMETER(ObjectAttributes);

    *PartitionHandle = NULL;
    return STATUS_NOT_SUPPORTED;
}

/**
 * @brief Queries or changes a memory partition.
 */
NTSTATUS
NTAPI
ZwManagePartition(
    _In_ HANDLE TargetHandle,
    _In_opt_ HANDLE SourceHandle,
    _In_ ULONG PartitionInformationClass,
    _Inout_updates_bytes_(PartitionInformationLength) PVOID PartitionInformation,
    _In_ ULONG PartitionInformationLength)
{
    UNREFERENCED_PARAMETER(TargetHandle);
    UNREFERENCED_PARAMETER(SourceHandle);
    UNREFERENCED_PARAMETER(PartitionInformationClass);
    UNREFERENCED_PARAMETER(PartitionInformation);
    UNREFERENCED_PARAMETER(PartitionInformationLength);

    return STATUS_NOT_SUPPORTED;
}

/**
 * @brief Stops stack walks of the current thread from being queued as APCs.
 * @return The state to give EtwpReenableStackWalkApc, nothing ever queues them.
 */
ULONG
NTAPI
EtwpDisableStackWalkApc(VOID)
{
    return 0;
}

/**
 * @brief Undoes EtwpDisableStackWalkApc.
 */
VOID
NTAPI
EtwpReenableStackWalkApc(
    _In_ ULONG PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);
}

/**
 * @brief Calls a routine on a stack with at least a given amount of room.
 *
 * @remarks Kernel stacks have a fixed size, the routine runs on the current one
 * when the room asked for fits in a large kernel stack.
 */
NTSTATUS
NTAPI
KeExpandKernelStackAndCalloutEx(
    _In_ PEXPAND_STACK_CALLOUT Callout,
    _In_opt_ PVOID Parameter,
    _In_ SIZE_T Size,
    _In_ BOOLEAN Wait,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(Context);

    if (Size > KERNEL_LARGE_STACK_SIZE)
        return STATUS_INVALID_PARAMETER_3;

    Callout(Parameter);
    return STATUS_SUCCESS;
}

/* Memory partitions never exist, so there is no object type for them */
POBJECT_TYPE PsPartitionType = NULL;
