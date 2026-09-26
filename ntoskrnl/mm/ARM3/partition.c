/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Memory partitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A memory partition owns physical pages that the rest of the system can no
 * longer have. Memory is moved into one from another partition, and what a
 * partition holds is handed out through MmAllocatePartitionNodePagesForMdlEx.
 *
 * Pages are moved in by taking them out of the source for good, so a partition
 * is a reservation that the system cannot page, trim or hand to anyone else.
 * That is the property a virtual machine's memory needs, and it is why the
 * virtualization stack asks for a partition in the first place.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#include "miarm.h"

/* GLOBALS ********************************************************************/

POBJECT_TYPE PsPartitionType;
PVOID MmSystemPartition;

static ULONG MmpNextPartitionId;

static GENERIC_MAPPING MmpPartitionMapping =
{
    STANDARD_RIGHTS_READ | MEMORY_PARTITION_QUERY_ACCESS,
    STANDARD_RIGHTS_WRITE | MEMORY_PARTITION_MODIFY_ACCESS,
    STANDARD_RIGHTS_EXECUTE,
    MEMORY_PARTITION_ALL_ACCESS
};

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief
 * Gives the pages a partition still holds back to the system.
 *
 * @remarks
 * Only the pages that were never handed out. Whatever was handed out belongs
 * to whoever asked for it, and comes back through the ordinary MDL free.
 */
static
VOID
NTAPI
MmpEmptyPartition(
    _Inout_ PMM_PARTITION Partition)
{
    PLIST_ENTRY Entry;
    PMM_PARTITION_BLOCK Block;
    PPFN_NUMBER Pages;
    PMDL Remainder;
    PFN_NUMBER Left;

    while (!IsListEmpty(&Partition->BlockListHead))
    {
        Entry = RemoveHeadList(&Partition->BlockListHead);
        Block = CONTAINING_RECORD(Entry, MM_PARTITION_BLOCK, ListEntry);

        Left = Block->PageCount - Block->HandedOut;
        if (Left != 0)
        {
            /*
             * The block's own MDL still names the pages that were handed out,
             * so the remainder is freed through one of its own.
             */
            Remainder = MmCreateMdl(NULL, NULL, Left * PAGE_SIZE);
            if (Remainder != NULL)
            {
                Pages = MmGetMdlPfnArray(Block->Mdl);
                RtlCopyMemory(MmGetMdlPfnArray(Remainder),
                              &Pages[Block->HandedOut],
                              Left * sizeof(PFN_NUMBER));

                Remainder->MdlFlags |= MDL_PAGES_LOCKED;
                MmFreePagesFromMdl(Remainder);
                ExFreePool(Remainder);
            }
            else
            {
                DPRINT1("Mm: %I64u pages of partition %lu are stranded\n",
                        (ULONG64)Left, Partition->PartitionId);
            }
        }

        ExFreePoolWithTag(Block->Mdl, TAG_MM_PARTITION);
        ExFreePoolWithTag(Block, TAG_MM_PARTITION);
    }

    Partition->TotalPages = 0;
    Partition->AvailablePages = 0;
}

/**
 * @brief
 * Runs when the last reference to a partition goes away.
 */
static
VOID
NTAPI
MmpDeletePartition(
    _In_ PVOID Object)
{
    PMM_PARTITION Partition = Object;

    DPRINT("Mm: deleting partition %lu\n", Partition->PartitionId);

    MmpEmptyPartition(Partition);
}

/**
 * @brief
 * Creates a partition object.
 */
static
NTSTATUS
NTAPI
MmpCreatePartitionObject(
    _In_ KPROCESSOR_MODE AccessMode,
    _In_opt_ POBJECT_ATTRIBUTES ObjectAttributes,
    _Out_ PMM_PARTITION *PartitionOut)
{
    PMM_PARTITION Partition;
    NTSTATUS Status;

    Status = ObCreateObject(AccessMode,
                            PsPartitionType,
                            ObjectAttributes,
                            AccessMode,
                            NULL,
                            sizeof(MM_PARTITION),
                            0,
                            0,
                            (PVOID *)&Partition);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(Partition, sizeof(*Partition));
    InitializeListHead(&Partition->BlockListHead);
    KeInitializeSpinLock(&Partition->Lock);
    Partition->PartitionId = InterlockedIncrement((PLONG)&MmpNextPartitionId);

    *PartitionOut = Partition;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Takes pages out of one partition and gives them to another.
 *
 * @remarks
 * Only the system partition can be a source. Pages of a child partition are
 * already spoken for, and passing them on would need the page that was handed
 * out to be found again, which nothing here tracks.
 */
static
NTSTATUS
NTAPI
MmpMovePartitionMemory(
    _Inout_ PMM_PARTITION Target,
    _In_ PMM_PARTITION Source,
    _In_ PFN_NUMBER NumberOfPages)
{
    PMM_PARTITION_BLOCK Block;
    PHYSICAL_ADDRESS Low, High, Skip;
    KIRQL OldIrql;

    if (Source != MmSystemPartition)
        return STATUS_NOT_SUPPORTED;

    if (Target == Source)
        return STATUS_INVALID_PARAMETER;

    if (NumberOfPages == 0)
        return STATUS_INVALID_PARAMETER;

    Block = ExAllocatePoolWithTag(NonPagedPool,
                                  sizeof(MM_PARTITION_BLOCK),
                                  TAG_MM_PARTITION);
    if (Block == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Low.QuadPart = 0;
    High.QuadPart = -1;
    Skip.QuadPart = 0;

    /* Out of the system for good, which is what moving memory means */
    Block->Mdl = MmAllocatePagesForMdlEx(Low,
                                         High,
                                         Skip,
                                         NumberOfPages * PAGE_SIZE,
                                         MmCached,
                                         MM_DONT_ZERO_ALLOCATION);
    if (Block->Mdl == NULL)
    {
        ExFreePoolWithTag(Block, TAG_MM_PARTITION);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Block->PageCount = Block->Mdl->ByteCount / PAGE_SIZE;
    Block->HandedOut = 0;

    if (Block->PageCount == 0)
    {
        MmFreePagesFromMdl(Block->Mdl);
        ExFreePool(Block->Mdl);
        ExFreePoolWithTag(Block, TAG_MM_PARTITION);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeAcquireSpinLock(&Target->Lock, &OldIrql);
    InsertTailList(&Target->BlockListHead, &Block->ListEntry);
    Target->TotalPages += Block->PageCount;
    Target->AvailablePages += Block->PageCount;
    KeReleaseSpinLock(&Target->Lock, OldIrql);

    DPRINT("Mm: moved %I64u pages into partition %lu\n",
           (ULONG64)Block->PageCount, Target->PartitionId);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Describes what a partition holds.
 */
static
VOID
NTAPI
MmpQueryPartitionConfiguration(
    _In_ PMM_PARTITION Partition,
    _Out_ PMEMORY_PARTITION_CONFIGURATION_INFORMATION Information)
{
    KIRQL OldIrql;

    RtlZeroMemory(Information, sizeof(*Information));

    KeAcquireSpinLock(&Partition->Lock, &OldIrql);

    Information->PartitionId = Partition->PartitionId;
    Information->Flags = Partition->Flags;
    Information->NumberOfNumaNodes = 1;
    Information->TotalNumberOfPages = Partition->TotalPages;
    Information->AvailablePages = Partition->AvailablePages;
    Information->FreePages = Partition->AvailablePages;
    Information->ResidentAvailablePages = Partition->AvailablePages;

    KeReleaseSpinLock(&Partition->Lock, OldIrql);

    /* The system partition answers for the machine, not for a reservation */
    if (Partition == MmSystemPartition)
    {
        Information->TotalNumberOfPages = MmNumberOfPhysicalPages;
        Information->AvailablePages = MmAvailablePages;
        Information->FreePages = MmAvailablePages;
        Information->ResidentAvailablePages = MmResidentAvailablePages;
        Information->CommitLimit = MmTotalCommitLimit;
        Information->CommittedPages = MmTotalCommittedPages;
    }
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Creates the partition object type and the partition the system itself is.
 */
CODE_SEG("INIT")
NTSTATUS
NTAPI
MmInitPartitionImplementation(VOID)
{
    OBJECT_TYPE_INITIALIZER ObjectTypeInitializer;
    UNICODE_STRING TypeName = RTL_CONSTANT_STRING(L"Partition");
    UNICODE_STRING SystemName = RTL_CONSTANT_STRING(L"\\KernelObjects\\MemoryPartition0");
    OBJECT_ATTRIBUTES ObjectAttributes;
    PMM_PARTITION Partition;
    HANDLE Handle;
    NTSTATUS Status;

    RtlZeroMemory(&ObjectTypeInitializer, sizeof(ObjectTypeInitializer));
    ObjectTypeInitializer.Length = sizeof(ObjectTypeInitializer);
    ObjectTypeInitializer.DefaultNonPagedPoolCharge = sizeof(MM_PARTITION);
    ObjectTypeInitializer.PoolType = NonPagedPool;
    ObjectTypeInitializer.UseDefaultObject = TRUE;
    ObjectTypeInitializer.GenericMapping = MmpPartitionMapping;
    ObjectTypeInitializer.DeleteProcedure = MmpDeletePartition;
    ObjectTypeInitializer.ValidAccessMask = MEMORY_PARTITION_ALL_ACCESS;
    ObjectTypeInitializer.InvalidAttributes = OBJ_OPENLINK;

    Status = ObCreateObjectType(&TypeName,
                                &ObjectTypeInitializer,
                                NULL,
                                &PsPartitionType);
    if (!NT_SUCCESS(Status))
        return Status;

    InitializeObjectAttributes(&ObjectAttributes,
                               &SystemName,
                               OBJ_CASE_INSENSITIVE | OBJ_PERMANENT | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    Status = MmpCreatePartitionObject(KernelMode, &ObjectAttributes, &Partition);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ObInsertObject(Partition,
                            NULL,
                            MEMORY_PARTITION_ALL_ACCESS,
                            0,
                            NULL,
                            &Handle);
    if (!NT_SUCCESS(Status))
        return Status;

    /* The system partition outlives every handle to it */
    Status = ObReferenceObjectByHandle(Handle,
                                       MEMORY_PARTITION_ALL_ACCESS,
                                       PsPartitionType,
                                       KernelMode,
                                       &MmSystemPartition,
                                       NULL);
    ObCloseHandle(Handle, KernelMode);

    if (!NT_SUCCESS(Status))
        return Status;

    DPRINT("Mm: the system is partition %lu\n", Partition->PartitionId);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Creates a memory partition.
 *
 * @param[in] ParentPartitionHandle
 * The partition the new one is created under. Only the system partition can
 * be a parent here.
 *
 * @param[out] PartitionHandle
 * Receives a handle to the new partition.
 *
 * @param[in] PreferredNode
 * The node its memory should come from. A value that is not a node the machine
 * has means no preference, and this machine has one node.
 */
NTSTATUS
NTAPI
ZwCreatePartition(
    _In_ HANDLE ParentPartitionHandle,
    _Out_ PHANDLE PartitionHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_opt_ POBJECT_ATTRIBUTES ObjectAttributes,
    _In_ ULONG PreferredNode)
{
    PMM_PARTITION Partition;
    PVOID Parent = NULL;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(PreferredNode);

    if (ParentPartitionHandle != NULL)
    {
        Status = ObReferenceObjectByHandle(ParentPartitionHandle,
                                           MEMORY_PARTITION_MODIFY_ACCESS,
                                           PsPartitionType,
                                           KernelMode,
                                           &Parent,
                                           NULL);
        if (!NT_SUCCESS(Status))
            return Status;

        if (Parent != MmSystemPartition)
        {
            ObDereferenceObject(Parent);
            return STATUS_NOT_SUPPORTED;
        }
    }

    Status = MmpCreatePartitionObject(KernelMode, ObjectAttributes, &Partition);

    if (Parent != NULL)
        ObDereferenceObject(Parent);

    if (!NT_SUCCESS(Status))
        return Status;

    return ObInsertObject(Partition,
                          NULL,
                          DesiredAccess,
                          0,
                          NULL,
                          PartitionHandle);
}

/**
 * @brief
 * Opens a memory partition by name.
 */
NTSTATUS
NTAPI
ZwOpenPartition(
    _Out_ PHANDLE PartitionHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ POBJECT_ATTRIBUTES ObjectAttributes)
{
    return ObOpenObjectByName(ObjectAttributes,
                              PsPartitionType,
                              KernelMode,
                              NULL,
                              DesiredAccess,
                              NULL,
                              PartitionHandle);
}

/**
 * @brief
 * Asks about a partition, or moves memory into one.
 *
 * @param[in] TargetHandle
 * The partition being asked about or given memory.
 *
 * @param[in] SourceHandle
 * Where the memory comes from, for the classes that move memory.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_NOT_SUPPORTED for a class this does not implement.
 */
NTSTATUS
NTAPI
ZwManagePartition(
    _In_ HANDLE TargetHandle,
    _In_opt_ HANDLE SourceHandle,
    _In_ MEMORY_PARTITION_INFORMATION_CLASS PartitionInformationClass,
    _Inout_updates_bytes_(PartitionInformationLength) PVOID PartitionInformation,
    _In_ ULONG PartitionInformationLength)
{
    PMM_PARTITION Target = NULL;
    PMM_PARTITION Source = NULL;
    PMEMORY_PARTITION_TRANSFER_INFORMATION Transfer;
    NTSTATUS Status;

    if (PartitionInformation == NULL)
        return STATUS_INVALID_PARAMETER;

    Status = ObReferenceObjectByHandle(TargetHandle,
                                       MEMORY_PARTITION_QUERY_ACCESS,
                                       PsPartitionType,
                                       KernelMode,
                                       (PVOID *)&Target,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    if (SourceHandle != NULL)
    {
        Status = ObReferenceObjectByHandle(SourceHandle,
                                           MEMORY_PARTITION_MODIFY_ACCESS,
                                           PsPartitionType,
                                           KernelMode,
                                           (PVOID *)&Source,
                                           NULL);
        if (!NT_SUCCESS(Status))
        {
            ObDereferenceObject(Target);
            return Status;
        }
    }

    switch (PartitionInformationClass)
    {
        case SystemMemoryPartitionInformation:

            if (PartitionInformationLength < sizeof(MEMORY_PARTITION_CONFIGURATION_INFORMATION))
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            MmpQueryPartitionConfiguration(Target, PartitionInformation);
            Status = STATUS_SUCCESS;
            break;

        case SystemMemoryPartitionMoveMemory:

            if (PartitionInformationLength < sizeof(MEMORY_PARTITION_TRANSFER_INFORMATION))
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            if (Source == NULL)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            Transfer = PartitionInformation;
            Status = MmpMovePartitionMemory(Target, Source, Transfer->NumberOfPages);
            break;

        default:

            DPRINT1("Mm: partition information class %lu is not implemented\n",
                    (ULONG)PartitionInformationClass);
            Status = STATUS_NOT_SUPPORTED;
            break;
    }

    if (Source != NULL)
        ObDereferenceObject(Source);

    ObDereferenceObject(Target);

    return Status;
}

/**
 * @brief
 * Hands out pages a partition holds, described by an MDL.
 *
 * @return
 * The MDL, or NULL when the partition does not hold that many pages.
 *
 * @remarks
 * Pages leave the partition for good. Whoever gets them frees them the
 * ordinary way, and the partition's count goes down to match.
 */
PMDL
NTAPI
MmpAllocatePartitionPages(
    _Inout_ PMM_PARTITION Partition,
    _In_ SIZE_T TotalBytes)
{
    PFN_NUMBER NumberOfPages, Taken = 0;
    PLIST_ENTRY Entry;
    PMM_PARTITION_BLOCK Block;
    PPFN_NUMBER Source, Destination;
    PMDL Mdl;
    KIRQL OldIrql;

    NumberOfPages = (PFN_NUMBER)BYTES_TO_PAGES(TotalBytes);
    if (NumberOfPages == 0)
        return NULL;

    Mdl = MmCreateMdl(NULL, NULL, NumberOfPages * PAGE_SIZE);
    if (Mdl == NULL)
        return NULL;

    Destination = MmGetMdlPfnArray(Mdl);

    KeAcquireSpinLock(&Partition->Lock, &OldIrql);

    if (Partition->AvailablePages < NumberOfPages)
    {
        KeReleaseSpinLock(&Partition->Lock, OldIrql);
        ExFreePool(Mdl);
        return NULL;
    }

    for (Entry = Partition->BlockListHead.Flink;
         (Entry != &Partition->BlockListHead) && (Taken < NumberOfPages);
         Entry = Entry->Flink)
    {
        PFN_NUMBER Left, Wanted;

        Block = CONTAINING_RECORD(Entry, MM_PARTITION_BLOCK, ListEntry);

        Left = Block->PageCount - Block->HandedOut;
        if (Left == 0)
            continue;

        Wanted = min(Left, NumberOfPages - Taken);

        Source = MmGetMdlPfnArray(Block->Mdl);
        RtlCopyMemory(&Destination[Taken],
                      &Source[Block->HandedOut],
                      Wanted * sizeof(PFN_NUMBER));

        Block->HandedOut += Wanted;
        Taken += Wanted;
    }

    ASSERT(Taken == NumberOfPages);
    Partition->AvailablePages -= Taken;

    KeReleaseSpinLock(&Partition->Lock, OldIrql);

    Mdl->MdlFlags |= MDL_PAGES_LOCKED;

    return Mdl;
}

/* EOF */
