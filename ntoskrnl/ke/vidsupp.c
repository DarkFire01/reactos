/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Routines the virtualization stack expects of a Windows 10 kernel
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Each of these is the newer face of something the kernel already does, or a
 * question about a machine feature ReactOS does not have. They live together
 * because they arrived together, brought in by the root side of the
 * virtualization stack.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* TYPES **********************************************************************/

typedef struct _PS_PROTECTION
{
    union
    {
        UCHAR Level;
        struct
        {
            UCHAR Type:3;
            UCHAR Audit:1;
            UCHAR Signer:4;
        };
    };
} PS_PROTECTION, *PPS_PROTECTION;

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Returns the NUMA node the running processor belongs to.
 *
 * @return
 * Zero. Memory here is one node.
 */
USHORT
NTAPI
KeGetCurrentNodeNumber(VOID)
{
    return 0;
}

/**
 * @brief
 * Describes how the logical processors of the machine relate to each other.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED. Callers treat that as a machine with nothing worth
 * describing, which is what a flat single node looks like anyway.
 */
NTSTATUS
NTAPI
KeQueryLogicalProcessorRelationship(
    _In_opt_ PPROCESSOR_NUMBER ProcessorNumber,
    _In_ LOGICAL_PROCESSOR_RELATIONSHIP RelationshipType,
    _Out_writes_bytes_opt_(*Length) PVOID Information,
    _Inout_ PULONG Length)
{
    UNREFERENCED_PARAMETER(ProcessorNumber);
    UNREFERENCED_PARAMETER(RelationshipType);
    UNREFERENCED_PARAMETER(Information);
    UNREFERENCED_PARAMETER(Length);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Puts a return that was diverted for a retpoline back on its way.
 *
 * @remarks
 * ReactOS does not divert returns, so there is never one to put back.
 */
VOID
NTAPI
KeReenterRetpolinedCode(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

/**
 * @brief
 * Takes a DPC out of its queue, saying which processor's queue to look in.
 *
 * @return
 * TRUE when the DPC was queued and is now removed.
 */
BOOLEAN
NTAPI
KeRemoveQueueDpcEx(
    _Inout_ PRKDPC Dpc,
    _In_ BOOLEAN FlushQueue)
{
    UNREFERENCED_PARAMETER(FlushQueue);

    return KeRemoveQueueDpc(Dpc);
}

/**
 * @brief
 * Returns the kind of protection a process runs under.
 *
 * @remarks
 * ReactOS runs no process protected, so every process reads back as none.
 */
PS_PROTECTION
NTAPI
PsGetProcessProtection(
    _In_ PEPROCESS Process)
{
    PS_PROTECTION Protection;

    UNREFERENCED_PARAMETER(Process);

    Protection.Level = 0;

    return Protection;
}

/**
 * @brief
 * Reads a firmware table.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED, as the kernel has no firmware table provider.
 */
NTSTATUS
NTAPI
ExGetSystemFirmwareTable(
    _In_ ULONG FirmwareTableProviderSignature,
    _In_ ULONG FirmwareTableId,
    _Out_writes_bytes_opt_(BufferLength) PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_opt_ PULONG ReturnLength)
{
    UNREFERENCED_PARAMETER(FirmwareTableProviderSignature);
    UNREFERENCED_PARAMETER(FirmwareTableId);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(BufferLength);

    if (ReturnLength != NULL)
        *ReturnLength = 0;

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Reads the mailbox the secure kernel leaves for the normal one.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED. There is no secure kernel here to leave one.
 */
NTSTATUS
NTAPI
VslRetrieveMailbox(
    _In_ ULONG MailboxKey,
    _In_opt_ PVOID Context,
    _In_ ULONG Index,
    _Out_writes_bytes_opt_(*Size) PVOID Buffer,
    _Inout_opt_ PULONG64 Size)
{
    UNREFERENCED_PARAMETER(MailboxKey);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Index);
    UNREFERENCED_PARAMETER(Buffer);

    if (Size != NULL)
        *Size = 0;

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Reads how much of a file has ever been written, as the cache manager knows
 * it.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED, which leaves the caller to read the file itself.
 */
NTSTATUS
NTAPI
FsRtlQueryCachedVdl(
    _In_ PFILE_OBJECT FileObject,
    _Out_ PLONGLONG Vdl)
{
    UNREFERENCED_PARAMETER(FileObject);

    if (Vdl != NULL)
        *Vdl = 0;

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Sends a file system control straight to a file system from the kernel.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 */
NTSTATUS
NTAPI
FsRtlKernelFsControlFile(
    _In_ PFILE_OBJECT FileObject,
    _In_ ULONG FsControlCode,
    _In_reads_bytes_opt_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_opt_(OutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_opt_ PULONG RetOutputBufferSize)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FsControlCode);
    UNREFERENCED_PARAMETER(InputBuffer);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBuffer);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (RetOutputBufferSize != NULL)
        *RetOutputBufferSize = 0;

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Describes the memory the machine has, as ranges.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED. MmGetPhysicalMemoryRanges answers the same question
 * in the form ReactOS keeps it.
 */
NTSTATUS
NTAPI
MmQueryMemoryRanges(
    _Inout_ PVOID MemoryRangesInformation)
{
    UNREFERENCED_PARAMETER(MemoryRangesInformation);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
MmAllocateMemoryRanges(
    _Inout_ PVOID MemoryRangesInformation)
{
    UNREFERENCED_PARAMETER(MemoryRangesInformation);
    return STATUS_NOT_IMPLEMENTED;
}

VOID
NTAPI
MmFreeMemoryRanges(
    _In_ PVOID MemoryRangesInformation)
{
    UNREFERENCED_PARAMETER(MemoryRangesInformation);
}

/**
 * @brief
 * Describes a section object.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 */
NTSTATUS
NTAPI
MmGetSectionInformation(
    _In_ PVOID SectionObject,
    _Out_writes_bytes_(Length) PVOID SectionInformation,
    _In_ ULONG Length)
{
    UNREFERENCED_PARAMETER(SectionObject);
    UNREFERENCED_PARAMETER(SectionInformation);
    UNREFERENCED_PARAMETER(Length);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Takes a token that covers a run of non volatile memory, so that writes to
 * it can be flushed to where they survive a reset.
 *
 * @return
 * STATUS_NOT_SUPPORTED. No memory here is non volatile.
 */
NTSTATUS
NTAPI
RtlGetNonVolatileToken(
    _In_ PVOID Buffer,
    _In_ SIZE_T Size,
    _Out_ PVOID *NonVolatileToken)
{
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Size);

    *NonVolatileToken = NULL;
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
RtlFreeNonVolatileToken(
    _In_ PVOID NonVolatileToken)
{
    UNREFERENCED_PARAMETER(NonVolatileToken);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
RtlFlushNonVolatileMemory(
    _In_ PVOID NonVolatileToken,
    _In_ PVOID Buffer,
    _In_ SIZE_T Size,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(NonVolatileToken);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(Flags);

    return STATUS_NOT_SUPPORTED;
}

/**
 * @brief
 * Reserves or commits memory, taking the extended parameters of a newer
 * caller.
 *
 * @param[in] ExtendedParameters
 * Attributes for the memory, none of which ReactOS acts on. A caller that
 * asked for one gets told so rather than silently getting memory without it.
 */
NTSTATUS
NTAPI
ZwAllocateVirtualMemoryEx(
    _In_ HANDLE ProcessHandle,
    _Inout_ PVOID *BaseAddress,
    _Inout_ PSIZE_T RegionSize,
    _In_ ULONG AllocationType,
    _In_ ULONG PageProtection,
    _Inout_updates_opt_(ParameterCount) PVOID ExtendedParameters,
    _In_ ULONG ParameterCount)
{
    if ((ExtendedParameters != NULL) && (ParameterCount != 0))
        return STATUS_NOT_SUPPORTED;

    return ZwAllocateVirtualMemory(ProcessHandle,
                                   BaseAddress,
                                   0,
                                   RegionSize,
                                   AllocationType,
                                   PageProtection);
}

/**
 * @brief
 * Reads the data behind a notification name.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED. ReactOS publishes no notification state.
 */
NTSTATUS
NTAPI
ZwQueryWnfStateData(
    _In_ PVOID StateName,
    _In_opt_ PVOID TypeId,
    _In_opt_ PVOID ExplicitScope,
    _Out_ PULONG ChangeStamp,
    _Out_writes_bytes_opt_(*BufferSize) PVOID Buffer,
    _Inout_ PULONG BufferSize)
{
    UNREFERENCED_PARAMETER(StateName);
    UNREFERENCED_PARAMETER(TypeId);
    UNREFERENCED_PARAMETER(ExplicitScope);
    UNREFERENCED_PARAMETER(Buffer);

    if (ChangeStamp != NULL)
        *ChangeStamp = 0;

    if (BufferSize != NULL)
        *BufferSize = 0;

    return STATUS_NOT_IMPLEMENTED;
}

/* EOF */
