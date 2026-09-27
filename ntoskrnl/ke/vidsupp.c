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

/*
 * The request the memory range routines read and answer in. A range is
 * counted in blocks of BlockSize bytes, which is also what an entry of the
 * answer is scaled by: an entry is the first block of a run of them times the
 * block size, plus one less than the number of blocks the run covers.
 */
typedef struct _MM_RANGE_REQUEST
{
    ULONG Version;
    ULONG Flags;
    ULONG_PTR Partition;
    ULONGLONG BlockCount;
    ULONGLONG BlockSize;
    ULONG NodeNumber;
    ULONG Reserved;
    ULONGLONG RangeCount;
    PULONGLONG Ranges;
} MM_RANGE_REQUEST, *PMM_RANGE_REQUEST;

#ifdef _WIN64
C_ASSERT(FIELD_OFFSET(MM_RANGE_REQUEST, BlockCount) == 0x10);
C_ASSERT(FIELD_OFFSET(MM_RANGE_REQUEST, BlockSize) == 0x18);
C_ASSERT(FIELD_OFFSET(MM_RANGE_REQUEST, NodeNumber) == 0x20);
C_ASSERT(FIELD_OFFSET(MM_RANGE_REQUEST, RangeCount) == 0x28);
C_ASSERT(FIELD_OFFSET(MM_RANGE_REQUEST, Ranges) == 0x30);
C_ASSERT(sizeof(MM_RANGE_REQUEST) == 0x38);
#endif

/* GLOBALS ********************************************************************/

#define MM_RANGE_VERSION            1

/* The one block size a request may ask to be answered in */
#define MM_RANGE_BLOCK_SIZE         0x40000000ULL

/* Every request carries this, and then one bit for each kind of memory */
#define MM_RANGE_FLAG_PRESENT       0x01
#define MM_RANGE_FLAGS_KIND         0x3E
#define MM_RANGE_FLAGS_KNOWN        0x3F

/* A node number reads as any node with this set */
#define MM_RANGE_ANY_NODE           0x80000000

#define MM_RANGE_CURRENT_PARTITION  ((ULONG_PTR)-1)
#define MM_RANGE_ANY_PARTITION      ((ULONG_PTR)-2)

#define MM_RANGE_TAG                'nRmM'

/* FUNCTIONS ******************************************************************/

/* Reads a range request over, before anything is done about it */
static
NTSTATUS
MiValidateRangeRequest(
    _In_ PMM_RANGE_REQUEST Request)
{
    if (Request->Version != MM_RANGE_VERSION)
        return STATUS_INVALID_PARAMETER;

    /* The answer goes in these two, so a request arrives with them empty */
    if (Request->RangeCount != 0)
        return STATUS_INVALID_PARAMETER;

    if ((Request->Flags > MM_RANGE_FLAGS_KNOWN) ||
        ((Request->Flags & MM_RANGE_FLAG_PRESENT) == 0))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (Request->BlockSize != MM_RANGE_BLOCK_SIZE)
        return STATUS_INVALID_PARAMETER;

    if ((Request->NodeNumber & ~MM_RANGE_ANY_NODE) >= KeNumberNodes)
        return STATUS_INVALID_PARAMETER;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_PARAMETER;

    /* Memory only ever belongs to the one partition that owns all of it */
    if ((Request->Partition != MM_RANGE_ANY_PARTITION) &&
        (Request->Partition != MM_RANGE_CURRENT_PARTITION) &&
        (Request->Partition != 0) &&
        ((PVOID)Request->Partition != MmSystemPartition))
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

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
 * Describes the memory the machine has, in blocks of the size the request
 * asks for.
 *
 * @param[in,out] MemoryRangesInformation
 * The request. It receives a count of ranges and an array of them, which the
 * caller frees from pool.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_PARAMETER when the request does not read
 * back as one, or STATUS_INSUFFICIENT_RESOURCES.
 *
 * @remarks
 * A block that holds any memory at all is reported, so the ranges cover more
 * than the memory that is really there whenever a block is only partly
 * filled. That is what a block sized answer means.
 */
NTSTATUS
NTAPI
MmQueryMemoryRanges(
    _Inout_ PVOID MemoryRangesInformation)
{
    PMM_RANGE_REQUEST Request = (PMM_RANGE_REQUEST)MemoryRangesInformation;
    PPHYSICAL_MEMORY_RANGE Physical;
    PULONGLONG Ranges;
    ULONGLONG First, Last, Start = 0, End = 0;
    ULONG Index, Number, Count = 0;
    NTSTATUS Status;

    Status = MiValidateRangeRequest(Request);
    if (!NT_SUCCESS(Status))
        return Status;

    Request->RangeCount = 0;
    Request->Ranges = NULL;

    /* All memory here is of the one kind, so asking for no kind of it answers nothing */
    if ((Request->Flags & MM_RANGE_FLAGS_KIND) == 0)
        return STATUS_SUCCESS;

    Physical = MmGetPhysicalMemoryRanges();
    if (Physical == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (Number = 0; Physical[Number].NumberOfBytes.QuadPart != 0; Number++)
        ;

    if (Number == 0)
    {
        ExFreePoolWithTag(Physical, 'hPmM');
        return STATUS_SUCCESS;
    }

    /* Ranges only ever run together, so one per memory range is always enough */
    Ranges = ExAllocatePoolZero(NonPagedPool,
                                Number * sizeof(*Ranges),
                                MM_RANGE_TAG);
    if (Ranges == NULL)
    {
        ExFreePoolWithTag(Physical, 'hPmM');
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (Index = 0; Index < Number; Index++)
    {
        First = (ULONGLONG)Physical[Index].BaseAddress.QuadPart / MM_RANGE_BLOCK_SIZE;
        Last = ((ULONGLONG)Physical[Index].BaseAddress.QuadPart +
                (ULONGLONG)Physical[Index].NumberOfBytes.QuadPart - 1) /
               MM_RANGE_BLOCK_SIZE;

        /* Carry the run being built on when this memory lands in it or next to it */
        if ((Count != 0) && (First <= End + 1))
        {
            if (Last <= End)
                continue;

            End = Last;
        }
        else
        {
            Start = First;
            End = Last;
            Count++;
        }

        Ranges[Count - 1] = Start * MM_RANGE_BLOCK_SIZE + (End - Start);
    }

    Request->RangeCount = Count;
    Request->Ranges = Ranges;

    ExFreePoolWithTag(Physical, 'hPmM');

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Takes memory for a caller that wants it whole blocks at a time.
 *
 * @return
 * STATUS_INVALID_PARAMETER for a request this kernel cannot read, otherwise
 * STATUS_INSUFFICIENT_RESOURCES. Nothing here hands out memory a gigabyte at
 * a time.
 */
NTSTATUS
NTAPI
MmAllocateMemoryRanges(
    _Inout_ PVOID MemoryRangesInformation)
{
    PMM_RANGE_REQUEST Request = (PMM_RANGE_REQUEST)MemoryRangesInformation;
    NTSTATUS Status;

    Status = MiValidateRangeRequest(Request);
    if (!NT_SUCCESS(Status))
        return Status;

    return STATUS_INSUFFICIENT_RESOURCES;
}

/**
 * @brief
 * Gives back memory that was taken in blocks.
 *
 * @return
 * STATUS_INVALID_PARAMETER, since nothing was ever taken to give back.
 */
NTSTATUS
NTAPI
MmFreeMemoryRanges(
    _Inout_ PVOID MemoryRangesInformation)
{
    UNREFERENCED_PARAMETER(MemoryRangesInformation);

    return STATUS_INVALID_PARAMETER;
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
