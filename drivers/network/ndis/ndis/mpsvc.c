/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NDIS 6 services for miniports
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ndissys.h"

/* Handles */

/* A miniport adapter handle, or NULL when the handle is something else */
static
PLOGICAL_ADAPTER
CoreAdapterFromHandle(
    _In_ NDIS_HANDLE NdisHandle)
{
    PLOGICAL_ADAPTER Found = NULL;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&AdapterListLock, &OldIrql);
    for (Entry = AdapterListHead.Flink; Entry != &AdapterListHead; Entry = Entry->Flink)
    {
        if (CONTAINING_RECORD(Entry, LOGICAL_ADAPTER, ListEntry) == (PLOGICAL_ADAPTER)NdisHandle)
        {
            Found = (PLOGICAL_ADAPTER)NdisHandle;
            break;
        }
    }
    KeReleaseSpinLock(&AdapterListLock, OldIrql);

    return Found;
}

/* A miniport driver handle, or NULL when the handle is something else */
static
PNDIS_M_DRIVER_BLOCK
CoreDriverFromHandle(
    _In_ NDIS_HANDLE NdisHandle)
{
    PNDIS_M_DRIVER_BLOCK Found = NULL;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&MiniportListLock, &OldIrql);
    for (Entry = MiniportListHead.Flink; Entry != &MiniportListHead; Entry = Entry->Flink)
    {
        if (CONTAINING_RECORD(Entry, NDIS_M_DRIVER_BLOCK, ListEntry) == (PNDIS_M_DRIVER_BLOCK)NdisHandle)
        {
            Found = (PNDIS_M_DRIVER_BLOCK)NdisHandle;
            break;
        }
    }
    KeReleaseSpinLock(&MiniportListLock, OldIrql);

    return Found;
}

/* The driver object behind an adapter or driver handle */
static
PDRIVER_OBJECT
CoreDriverObjectFromHandle(
    _In_ NDIS_HANDLE NdisHandle)
{
    PLOGICAL_ADAPTER Adapter = CoreAdapterFromHandle(NdisHandle);
    PNDIS_M_DRIVER_BLOCK Driver;

    if (Adapter != NULL)
        return Adapter->NdisMiniportBlock.DriverHandle->DriverObject;

    Driver = CoreDriverFromHandle(NdisHandle);
    return (Driver != NULL) ? Driver->DriverObject : NULL;
}

/* Timer objects */

typedef struct _CORE_TIMER_OBJECT
{
    KTIMER Timer;
    KDPC Dpc;
    NDIS_HANDLE NdisHandle;
    PNDIS_TIMER_FUNCTION TimerFunction;
    PVOID DefaultContext;
    PVOID CurrentContext;
    BOOLEAN Periodic;
} CORE_TIMER_OBJECT, *PCORE_TIMER_OBJECT;

static KDEFERRED_ROUTINE CoreTimerObjectDpc;

static
VOID
NTAPI
CoreTimerObjectDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PCORE_TIMER_OBJECT TimerObject = DeferredContext;

    TimerObject->TimerFunction(Dpc, TimerObject->CurrentContext, SystemArgument1, SystemArgument2);
}

/**
 * @brief
 * Creates a timer whose function runs as a DPC.
 *
 * @param[in] NdisHandle
 * The owner, usually a miniport adapter handle.
 *
 * @param[in] TimerCharacteristics
 * The function, its default context and the pool tag.
 *
 * @param[out] pTimerObject
 * The timer.
 *
 * @return
 * NDIS_STATUS_SUCCESS, NDIS_STATUS_BAD_CHARACTERISTICS or NDIS_STATUS_RESOURCES.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisAllocateTimerObject(
    NDIS_HANDLE NdisHandle,
    PNDIS_TIMER_CHARACTERISTICS TimerCharacteristics,
    PNDIS_HANDLE pTimerObject)
{
    PCORE_TIMER_OBJECT TimerObject;

    *pTimerObject = NULL;

    if (TimerCharacteristics->Header.Type != NDIS_OBJECT_TYPE_TIMER_CHARACTERISTICS ||
        TimerCharacteristics->Header.Size < NDIS_SIZEOF_TIMER_CHARACTERISTICS_REVISION_1)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Bad timer characteristics type 0x%x size %u.\n",
                                  TimerCharacteristics->Header.Type, TimerCharacteristics->Header.Size));
        return NDIS_STATUS_BAD_CHARACTERISTICS;
    }

    TimerObject = ExAllocatePoolWithTag(NonPagedPool, sizeof(*TimerObject), TimerCharacteristics->AllocationTag);
    if (TimerObject == NULL)
        return NDIS_STATUS_RESOURCES;

    RtlZeroMemory(TimerObject, sizeof(*TimerObject));
    TimerObject->NdisHandle = NdisHandle;
    TimerObject->TimerFunction = TimerCharacteristics->TimerFunction;
    TimerObject->DefaultContext = TimerCharacteristics->FunctionContext;
    TimerObject->CurrentContext = TimerCharacteristics->FunctionContext;

    KeInitializeTimer(&TimerObject->Timer);
    KeInitializeDpc(&TimerObject->Dpc, CoreTimerObjectDpc, TimerObject);

    *pTimerObject = TimerObject;
    return NDIS_STATUS_SUCCESS;
}

/**
 * @brief
 * Starts a timer object, once or periodically.
 *
 * @param[in] TimerObject
 * The timer.
 *
 * @param[in] DueTime
 * When it first fires, in KeSetTimer units.
 *
 * @param[in] MillisecondsPeriod
 * The period, or 0 for a one shot timer.
 *
 * @param[in] FunctionContext
 * The context for this run, or NULL for the one it was created with.
 *
 * @return
 * TRUE if the timer was already set.
 */
_Use_decl_annotations_
BOOLEAN
NTAPI
NdisSetTimerObject(
    NDIS_HANDLE TimerObject,
    LARGE_INTEGER DueTime,
    LONG MillisecondsPeriod,
    PVOID FunctionContext)
{
    PCORE_TIMER_OBJECT Timer = TimerObject;

    Timer->CurrentContext = (FunctionContext != NULL) ? FunctionContext : Timer->DefaultContext;

    if (MillisecondsPeriod != 0)
        Timer->Periodic = TRUE;

    return KeSetTimerEx(&Timer->Timer, DueTime, MillisecondsPeriod, &Timer->Dpc);
}

/**
 * @brief
 * Sets a timer object the kernel may fire late, within a tolerance, so it can
 * batch it with other timers.
 *
 * @return
 * TRUE if the timer was already set.
 */
_Use_decl_annotations_
BOOLEAN
NTAPI
NdisSetCoalescableTimerObject(
    NDIS_HANDLE TimerObject,
    LARGE_INTEGER DueTime,
    LONG MillisecondsPeriod,
    PVOID FunctionContext,
    ULONG TolerableDelay)
{
    PCORE_TIMER_OBJECT Timer = TimerObject;

    Timer->CurrentContext = (FunctionContext != NULL) ? FunctionContext : Timer->DefaultContext;

    if (MillisecondsPeriod != 0)
        Timer->Periodic = TRUE;

    if (TolerableDelay != 0)
        return KeSetCoalescableTimer(&Timer->Timer, DueTime, MillisecondsPeriod, TolerableDelay, &Timer->Dpc);

    return KeSetTimerEx(&Timer->Timer, DueTime, MillisecondsPeriod, &Timer->Dpc);
}

/**
 * @brief
 * Stops a timer object. A periodic timer's function has also finished running
 * when this returns.
 *
 * @param[in] TimerObject
 * The timer.
 *
 * @return
 * TRUE if the timer was set.
 */
_Use_decl_annotations_
BOOLEAN
NTAPI
NdisCancelTimerObject(
    NDIS_HANDLE TimerObject)
{
    PCORE_TIMER_OBJECT Timer = TimerObject;
    BOOLEAN Cancelled;

    Cancelled = KeCancelTimer(&Timer->Timer);

    if (Timer->Periodic)
        KeFlushQueuedDpcs();

    return Cancelled;
}

/**
 * @brief
 * Frees a timer object.
 *
 * @param[in] TimerObject
 * The timer, no longer set.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisFreeTimerObject(
    NDIS_HANDLE TimerObject)
{
    PCORE_TIMER_OBJECT Timer = TimerObject;

    KeCancelTimer(&Timer->Timer);
    ExFreePool(Timer);
}

/* I/O work items */

typedef struct _CORE_IO_WORKITEM
{
    PIO_WORKITEM IoWorkItem;
    NDIS_HANDLE NdisObjectHandle;
    NDIS_IO_WORKITEM_ROUTINE Routine;
    PVOID Context;
} CORE_IO_WORKITEM, *PCORE_IO_WORKITEM;

static IO_WORKITEM_ROUTINE CoreIoWorkItemRoutine;

static
VOID
NTAPI
CoreIoWorkItemRoutine(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PCORE_IO_WORKITEM WorkItem = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    WorkItem->Routine(WorkItem->Context, WorkItem);
}

/**
 * @brief
 * Creates a work item tied to an NDIS object, so the object's driver stays
 * loaded while the item is queued.
 *
 * @param[in] NdisObjectHandle
 * A miniport adapter or driver handle.
 *
 * @return
 * The work item, or NULL.
 */
_Use_decl_annotations_
NDIS_HANDLE
NTAPI
NdisAllocateIoWorkItem(
    NDIS_HANDLE NdisObjectHandle)
{
    PLOGICAL_ADAPTER Adapter = CoreAdapterFromHandle(NdisObjectHandle);
    PDEVICE_OBJECT DeviceObject = NULL;
    PDRIVER_OBJECT DriverObject;
    PCORE_IO_WORKITEM WorkItem;

    if (Adapter != NULL)
    {
        DeviceObject = Adapter->NdisMiniportBlock.DeviceObject;
    }
    else
    {
        DriverObject = CoreDriverObjectFromHandle(NdisObjectHandle);
        if (DriverObject != NULL)
            DeviceObject = DriverObject->DeviceObject;
    }

    if (DeviceObject == NULL)
    {
        NDIS_DbgPrint(MIN_TRACE, ("No device object behind handle %p for a work item.\n", NdisObjectHandle));
        return NULL;
    }

    WorkItem = ExAllocatePoolWithTag(NonPagedPool, sizeof(*WorkItem), NDIS_TAG);
    if (WorkItem == NULL)
        return NULL;

    RtlZeroMemory(WorkItem, sizeof(*WorkItem));
    WorkItem->NdisObjectHandle = NdisObjectHandle;

    WorkItem->IoWorkItem = IoAllocateWorkItem(DeviceObject);
    if (WorkItem->IoWorkItem == NULL)
    {
        ExFreePoolWithTag(WorkItem, NDIS_TAG);
        return NULL;
    }

    return WorkItem;
}

/**
 * @brief
 * Queues a work item from NdisAllocateIoWorkItem.
 *
 * @param[in] NdisIoWorkItemHandle
 * The work item.
 *
 * @param[in] Routine
 * What to run.
 *
 * @param[in] WorkItemContext
 * Its context.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisQueueIoWorkItem(
    NDIS_HANDLE NdisIoWorkItemHandle,
    NDIS_IO_WORKITEM_ROUTINE Routine,
    PVOID WorkItemContext)
{
    PCORE_IO_WORKITEM WorkItem = NdisIoWorkItemHandle;

    WorkItem->Routine = Routine;
    WorkItem->Context = WorkItemContext;

    IoQueueWorkItem(WorkItem->IoWorkItem, CoreIoWorkItemRoutine, CriticalWorkQueue, WorkItem);
}

/**
 * @brief
 * Frees a work item from NdisAllocateIoWorkItem.
 *
 * @param[in] NdisIoWorkItemHandle
 * The work item, not queued.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisFreeIoWorkItem(
    NDIS_HANDLE NdisIoWorkItemHandle)
{
    PCORE_IO_WORKITEM WorkItem = NdisIoWorkItemHandle;

    IoFreeWorkItem(WorkItem->IoWorkItem);
    ExFreePoolWithTag(WorkItem, NDIS_TAG);
}

/* Memory */

/**
 * @brief
 * Allocates nonpaged memory with a pool priority.
 *
 * @param[in] NdisHandle
 * The caller's NDIS handle.
 *
 * @param[in] Length
 * How much.
 *
 * @param[in] Tag
 * The pool tag, or 0 for NDIS's own.
 *
 * @param[in] Priority
 * How hard to try when memory is low.
 *
 * @return
 * The memory, or NULL.
 */
_Use_decl_annotations_
PVOID
NTAPI
NdisAllocateMemoryWithTagPriority(
    NDIS_HANDLE NdisHandle,
    UINT Length,
    ULONG Tag,
    EX_POOL_PRIORITY Priority)
{
    PVOID Block;

    UNREFERENCED_PARAMETER(NdisHandle);

    Block = ExAllocatePoolWithTagPriority(NonPagedPool, Length, (Tag != 0) ? Tag : NDIS_TAG, Priority);
    if (Block == NULL)
        NDIS_DbgPrint(MIN_TRACE, ("No %u bytes of nonpaged pool.\n", Length));

    return Block;
}

/**
 * @brief
 * Frees memory from NdisAllocateMemoryWithTagPriority.
 *
 * @param[in] NdisHandle
 * The caller's NDIS handle.
 *
 * @param[in] VirtualAddress
 * The memory.
 *
 * @param[in] Tag
 * The tag it was allocated with.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisFreeMemoryWithTagPriority(
    NDIS_HANDLE NdisHandle,
    PVOID VirtualAddress,
    ULONG Tag)
{
    UNREFERENCED_PARAMETER(NdisHandle);

    ExFreePoolWithTag(VirtualAddress, (Tag != 0) ? Tag : NDIS_TAG);
}

/**
 * @brief
 * Describes nonpaged memory with an MDL.
 *
 * @param[in] NdisHandle
 * The caller's NDIS handle.
 *
 * @param[in] VirtualAddress
 * The memory, which must be nonpaged.
 *
 * @param[in] Length
 * Its size.
 *
 * @return
 * The MDL, or NULL.
 */
_Use_decl_annotations_
PMDL
NTAPI
NdisAllocateMdl(
    NDIS_HANDLE NdisHandle,
    PVOID VirtualAddress,
    ULONG Length)
{
    PMDL Mdl;

    UNREFERENCED_PARAMETER(NdisHandle);

    Mdl = IoAllocateMdl(VirtualAddress, Length, FALSE, FALSE, NULL);
    if (Mdl == NULL)
        return NULL;

    MmBuildMdlForNonPagedPool(Mdl);
    Mdl->Next = NULL;

    return Mdl;
}

/**
 * @brief
 * Frees an MDL from NdisAllocateMdl.
 *
 * @param[in] Mdl
 * The MDL.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisFreeMdl(
    PMDL Mdl)
{
    IoFreeMdl(Mdl);
}

/* Device objects */

/**
 * @brief
 * Creates a named device object with a symbolic link, whose IRPs go to the
 * dispatch routines the caller gives.
 *
 * @param[in] NdisHandle
 * A miniport driver or adapter handle.
 *
 * @param[in] DeviceObjectAttributes
 * Names, dispatch routines and the size of the caller's extension.
 *
 * @param[out] pDeviceObject
 * The device object.
 *
 * @param[out] NdisDeviceHandle
 * The handle for NdisDeregisterDeviceEx.
 *
 * @return
 * NDIS_STATUS_SUCCESS, or why the device could not be created.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisRegisterDeviceEx(
    NDIS_HANDLE NdisHandle,
    PNDIS_DEVICE_OBJECT_ATTRIBUTES DeviceObjectAttributes,
    PDEVICE_OBJECT *pDeviceObject,
    PNDIS_HANDLE NdisDeviceHandle)
{
    PDRIVER_OBJECT DriverObject = CoreDriverObjectFromHandle(NdisHandle);
    PNDIS_STRING SymbolicName = DeviceObjectAttributes->SymbolicName;
    PNDIS_M_DEVICE_BLOCK DeviceBlock;
    PDEVICE_OBJECT DeviceObject;
    ULONG BlockSize;
    NTSTATUS Status;
    ULONG i;

    *pDeviceObject = NULL;
    *NdisDeviceHandle = NULL;

    if (DriverObject == NULL)
        return NDIS_STATUS_NOT_SUPPORTED;

    /* The caller's extension follows the device block */
    BlockSize = ALIGN_UP_BY(sizeof(NDIS_M_DEVICE_BLOCK), MEMORY_ALLOCATION_ALIGNMENT);

    Status = IoCreateDevice(DriverObject,
                            BlockSize + DeviceObjectAttributes->ExtensionSize,
                            DeviceObjectAttributes->DeviceName,
                            FILE_DEVICE_NETWORK,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &DeviceObject);
    if (!NT_SUCCESS(Status))
        return Status;

    DeviceBlock = DeviceObject->DeviceExtension;
    DeviceBlock->DeviceObject = DeviceObject;
    DeviceBlock->ReservedExtension = (PUCHAR)DeviceBlock + BlockSize;

    /* The caller's name may not outlive this call */
    DeviceBlock->SymbolicLink.Buffer = ExAllocatePoolWithTag(PagedPool, SymbolicName->Length, NDIS_TAG);
    if (DeviceBlock->SymbolicLink.Buffer == NULL)
    {
        IoDeleteDevice(DeviceObject);
        return NDIS_STATUS_RESOURCES;
    }

    DeviceBlock->SymbolicLink.Length = SymbolicName->Length;
    DeviceBlock->SymbolicLink.MaximumLength = SymbolicName->Length;
    RtlCopyMemory(DeviceBlock->SymbolicLink.Buffer, SymbolicName->Buffer, SymbolicName->Length);
    DeviceBlock->SymbolicName = &DeviceBlock->SymbolicLink;

    Status = IoCreateSymbolicLink(&DeviceBlock->SymbolicLink, DeviceObjectAttributes->DeviceName);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(DeviceBlock->SymbolicLink.Buffer, NDIS_TAG);
        IoDeleteDevice(DeviceObject);
        return Status;
    }

    if (DeviceObjectAttributes->MajorFunctions != NULL)
    {
        for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
            DeviceBlock->MajorFunction[i] = DeviceObjectAttributes->MajorFunctions[i];
    }

    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    *pDeviceObject = DeviceObject;
    *NdisDeviceHandle = DeviceBlock;

    return NDIS_STATUS_SUCCESS;
}

/**
 * @brief
 * Deletes a device object from NdisRegisterDeviceEx and its symbolic link.
 *
 * @param[in] NdisDeviceHandle
 * The handle it returned.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisDeregisterDeviceEx(
    NDIS_HANDLE NdisDeviceHandle)
{
    PNDIS_M_DEVICE_BLOCK DeviceBlock = NdisDeviceHandle;

    IoDeleteSymbolicLink(&DeviceBlock->SymbolicLink);
    ExFreePoolWithTag(DeviceBlock->SymbolicLink.Buffer, NDIS_TAG);
    IoDeleteDevice(DeviceBlock->DeviceObject);
}

/**
 * @brief
 * Returns the caller's part of a device extension from NdisRegisterDeviceEx.
 *
 * @param[in] DeviceObject
 * The device object.
 *
 * @return
 * The extension.
 */
_Use_decl_annotations_
PVOID
NTAPI
NdisGetDeviceReservedExtension(
    PDEVICE_OBJECT DeviceObject)
{
    return ((PNDIS_M_DEVICE_BLOCK)DeviceObject->DeviceExtension)->ReservedExtension;
}

/* Bus configuration space */

/**
 * @brief
 * Reads a miniport's bus configuration space through the bus driver.
 *
 * @param[in] NdisMiniportHandle
 * The adapter.
 *
 * @param[in] WhichSpace
 * The configuration space, normally PCI_WHICHSPACE_CONFIG.
 *
 * @param[in] Offset
 * Where to start.
 *
 * @param[out] Buffer
 * Receives the data.
 *
 * @param[in] Length
 * How much to read.
 *
 * @return
 * The number of bytes read.
 */
_Use_decl_annotations_
ULONG
NTAPI
NdisMGetBusData(
    NDIS_HANDLE NdisMiniportHandle,
    ULONG WhichSpace,
    ULONG Offset,
    PVOID Buffer,
    ULONG Length)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)NdisMiniportHandle;
    ULONG Read;

    if (Adapter->BusInterface.GetBusData == NULL)
    {
        NDIS_DbgPrint(MIN_TRACE, ("No bus interface to read space %lu at 0x%lx.\n", WhichSpace, Offset));
        return 0;
    }

    Read = Adapter->BusInterface.GetBusData(Adapter->BusInterface.Context, WhichSpace, Buffer, Offset, Length);
    if (Read != Length)
        NDIS_DbgPrint(MIN_TRACE, ("Read %lu of %lu bytes of space %lu at 0x%lx.\n", Read, Length, WhichSpace, Offset));

    return Read;
}

/**
 * @brief
 * Writes a miniport's bus configuration space through the bus driver.
 *
 * @param[in] NdisMiniportHandle
 * The adapter.
 *
 * @param[in] WhichSpace
 * The configuration space, normally PCI_WHICHSPACE_CONFIG.
 *
 * @param[in] Offset
 * Where to start.
 *
 * @param[in] Buffer
 * The data.
 *
 * @param[in] Length
 * How much to write.
 *
 * @return
 * The number of bytes written.
 */
_Use_decl_annotations_
ULONG
NTAPI
NdisMSetBusData(
    NDIS_HANDLE NdisMiniportHandle,
    ULONG WhichSpace,
    ULONG Offset,
    PVOID Buffer,
    ULONG Length)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)NdisMiniportHandle;

    if (Adapter->BusInterface.SetBusData == NULL)
        return 0;

    return Adapter->BusInterface.SetBusData(Adapter->BusInterface.Context, WhichSpace, Buffer, Offset, Length);
}

/* Reader writer locks */

/*
 * Readers count themselves on their own processor and only look at the write
 * lock, so read acquisitions never share a cache line. A writer takes the
 * write lock and waits for every processor's count to drain.
 */
struct _NDIS_RW_LOCK_EX
{
    KSPIN_LOCK WriteLock;
    PKTHREAD Owner;
    NDIS_HANDLE SourceHandle;
    ULONG ProcessorCount;
    volatile LONG Readers[ANYSIZE_ARRAY];
};

/* LOCK_STATE_EX::LockState */
#define CORE_RWL_RELEASED           0xFF
#define CORE_RWL_WRITE_NESTED       2
#define CORE_RWL_READ               3
#define CORE_RWL_WRITE              4

/**
 * @brief
 * Creates a reader writer lock.
 *
 * @param[in] NdisHandle
 * The caller's NDIS handle.
 *
 * @return
 * The lock, or NULL.
 */
_Use_decl_annotations_
PNDIS_RW_LOCK_EX
NTAPI
NdisAllocateRWLock(
    NDIS_HANDLE NdisHandle)
{
    ULONG ProcessorCount = KeQueryMaximumProcessorCountEx(ALL_PROCESSOR_GROUPS);
    PNDIS_RW_LOCK_EX Lock;
    ULONG Size;

    Size = FIELD_OFFSET(NDIS_RW_LOCK_EX, Readers) + ProcessorCount * sizeof(Lock->Readers[0]);

    Lock = ExAllocatePoolWithTag(NonPagedPool, Size, NDIS_TAG);
    if (Lock == NULL)
        return NULL;

    RtlZeroMemory(Lock, Size);
    KeInitializeSpinLock(&Lock->WriteLock);
    Lock->SourceHandle = NdisHandle;
    Lock->ProcessorCount = ProcessorCount;

    return Lock;
}

/**
 * @brief
 * Frees a reader writer lock nobody holds.
 *
 * @param[in] Lock
 * The lock.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisFreeRWLock(
    PNDIS_RW_LOCK_EX Lock)
{
    ExFreePoolWithTag(Lock, NDIS_TAG);
}

/**
 * @brief
 * Takes a reader writer lock shared, at DISPATCH_LEVEL.
 *
 * @param[in] Lock
 * The lock.
 *
 * @param[out] LockState
 * What NdisReleaseRWLock needs.
 *
 * @param[in] Flags
 * NDIS_RWL_AT_DISPATCH_LEVEL when the caller already is.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisAcquireRWLockRead(
    PNDIS_RW_LOCK_EX Lock,
    PLOCK_STATE_EX LockState,
    UCHAR Flags)
{
    volatile LONG *Readers;

    LockState->Flags = Flags;

    if (!(Flags & NDIS_RWL_AT_DISPATCH_LEVEL))
        KeRaiseIrql(DISPATCH_LEVEL, &LockState->OldIrql);

    Readers = &Lock->Readers[KeGetCurrentProcessorIndex()];

    /*
     * A first read on this processor while someone else writes steps back and
     * waits for the writer. Nested reads, and reads by the writer, go ahead.
     */
    if (InterlockedIncrement(Readers) == 1 &&
        !KeTestSpinLock(&Lock->WriteLock) &&
        Lock->Owner != KeGetCurrentThread())
    {
        InterlockedDecrement(Readers);
        KeAcquireSpinLockAtDpcLevel(&Lock->WriteLock);
        InterlockedIncrement(Readers);
        KeReleaseSpinLockFromDpcLevel(&Lock->WriteLock);
    }

    LockState->LockState = CORE_RWL_READ;
}

/**
 * @brief
 * Takes a reader writer lock exclusive, at DISPATCH_LEVEL. The owner may take
 * it again, shared or exclusive.
 *
 * @param[in] Lock
 * The lock.
 *
 * @param[out] LockState
 * What NdisReleaseRWLock needs.
 *
 * @param[in] Flags
 * NDIS_RWL_AT_DISPATCH_LEVEL when the caller already is.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisAcquireRWLockWrite(
    PNDIS_RW_LOCK_EX Lock,
    PLOCK_STATE_EX LockState,
    UCHAR Flags)
{
    ULONG Current;
    LONG OwnReads;
    ULONG i;

    LockState->Flags = Flags;

    if (Lock->Owner == KeGetCurrentThread())
    {
        LockState->LockState = CORE_RWL_WRITE_NESTED;
        return;
    }

    if (Flags & NDIS_RWL_AT_DISPATCH_LEVEL)
        KeAcquireSpinLockAtDpcLevel(&Lock->WriteLock);
    else
        KeAcquireSpinLock(&Lock->WriteLock, &LockState->OldIrql);

    /* Reads on this processor can only be this thread's own, which never drain */
    Current = KeGetCurrentProcessorIndex();
    OwnReads = InterlockedExchange(&Lock->Readers[Current], 0);

    for (i = 0; i < Lock->ProcessorCount; i++)
    {
        while (Lock->Readers[i] != 0)
            YieldProcessor();
    }

    Lock->Readers[Current] = OwnReads;
    Lock->Owner = KeGetCurrentThread();
    LockState->LockState = CORE_RWL_WRITE;
}

/**
 * @brief
 * Releases a reader writer lock taken either way.
 *
 * @param[in] Lock
 * The lock.
 *
 * @param[in] LockState
 * What the acquisition returned.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisReleaseRWLock(
    PNDIS_RW_LOCK_EX Lock,
    PLOCK_STATE_EX LockState)
{
    switch (LockState->LockState)
    {
        case CORE_RWL_READ:
            InterlockedDecrement(&Lock->Readers[KeGetCurrentProcessorIndex()]);
            LockState->LockState = CORE_RWL_RELEASED;

            if (!(LockState->Flags & NDIS_RWL_AT_DISPATCH_LEVEL))
                KeLowerIrql(LockState->OldIrql);
            break;

        case CORE_RWL_WRITE:
            LockState->LockState = CORE_RWL_RELEASED;
            Lock->Owner = NULL;

            if (LockState->Flags & NDIS_RWL_AT_DISPATCH_LEVEL)
                KeReleaseSpinLockFromDpcLevel(&Lock->WriteLock);
            else
                KeReleaseSpinLock(&Lock->WriteLock, LockState->OldIrql);
            break;

        default:
            /* A nested write releases nothing */
            break;
    }
}

/* Configuration */

/**
 * @brief
 * Opens the registry configuration of an adapter, or the Parameters key of a
 * miniport driver.
 *
 * @param[in] ConfigObject
 * The adapter or driver handle.
 *
 * @param[out] ConfigurationHandle
 * The handle for NdisReadConfiguration and NdisCloseConfiguration.
 *
 * @return
 * NDIS_STATUS_SUCCESS, or why the key could not be opened.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisOpenConfigurationEx(
    PNDIS_CONFIGURATION_OBJECT ConfigObject,
    PNDIS_HANDLE ConfigurationHandle)
{
    NDIS_WRAPPER_CONTEXT WrapperContext;
    PNDIS_M_DRIVER_BLOCK Driver;
    PLOGICAL_ADAPTER Adapter;
    NDIS_STATUS Status;

    if (ConfigObject->Header.Type != NDIS_OBJECT_TYPE_CONFIGURATION_OBJECT ||
        ConfigObject->Header.Size < NDIS_SIZEOF_CONFIGURATION_OBJECT_REVISION_1 ||
        ConfigObject->Header.Revision == 0)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Bad configuration object type 0x%x revision %u size %u.\n",
                                  ConfigObject->Header.Type, ConfigObject->Header.Revision,
                                  ConfigObject->Header.Size));
        return STATUS_INVALID_PARAMETER;
    }

    *ConfigurationHandle = NULL;

    Adapter = CoreAdapterFromHandle(ConfigObject->NdisHandle);
    if (Adapter != NULL)
    {
        Status = IoOpenDeviceRegistryKey(Adapter->NdisMiniportBlock.PhysicalDeviceObject,
                                         PLUGPLAY_REGKEY_DRIVER,
                                         KEY_ALL_ACCESS,
                                         &WrapperContext.RegistryHandle);
        if (!NT_SUCCESS(Status))
        {
            NDIS_DbgPrint(MIN_TRACE, ("No driver key for %wZ (0x%lx).\n",
                                      &Adapter->NdisMiniportBlock.MiniportName, Status));
            return NDIS_STATUS_FAILURE;
        }

        WrapperContext.DeviceObject = Adapter->NdisMiniportBlock.DeviceObject;
        WrapperContext.BusNumber = Adapter->NdisMiniportBlock.BusNumber;
        WrapperContext.SlotNumber = Adapter->NdisMiniportBlock.SlotNumber;

        NdisOpenConfiguration(&Status, ConfigurationHandle, &WrapperContext);
        if (Status != NDIS_STATUS_SUCCESS)
            NDIS_DbgPrint(MIN_TRACE, ("Opening the configuration failed (0x%x).\n", Status));

        ZwClose(WrapperContext.RegistryHandle);
        return Status;
    }

    Driver = CoreDriverFromHandle(ConfigObject->NdisHandle);
    if (Driver != NULL && Driver->Ndis6Driver)
    {
        NdisOpenProtocolConfiguration(&Status, ConfigurationHandle, Driver->RegistryPath);
        return Status;
    }

    NDIS_DbgPrint(MIN_TRACE, ("Configuration asked for with unknown handle %p.\n", ConfigObject->NdisHandle));
    return NDIS_STATUS_FAILURE;
}

/* Processors */

/**
 * @brief
 * The number of active processors in a group.
 *
 * @param[in] Group
 * The group, or ALL_PROCESSOR_GROUPS.
 *
 * @return
 * The count.
 */
_Use_decl_annotations_
ULONG
NTAPI
NdisGroupActiveProcessorCount(
    USHORT Group)
{
    return KeQueryActiveProcessorCountEx(Group);
}

/**
 * @brief
 * Describes the system's processors. Every processor is reported as its own
 * core in socket 0 and node 0.
 *
 * @param[in] NdisHandle
 * The caller's NDIS handle.
 *
 * @param[out] SystemProcessorInfo
 * Receives the description.
 *
 * @param[in,out] Size
 * The buffer size in, the size needed out when it is too small.
 *
 * @return
 * NDIS_STATUS_SUCCESS or NDIS_STATUS_BUFFER_TOO_SHORT.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisGetProcessorInformationEx(
    NDIS_HANDLE NdisHandle,
    PNDIS_SYSTEM_PROCESSOR_INFO_EX SystemProcessorInfo,
    PSIZE_T Size)
{
    ULONG Count = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    PNDIS_PROCESSOR_INFO_EX Processors;
    SIZE_T Needed;
    ULONG i;

    UNREFERENCED_PARAMETER(NdisHandle);

    Needed = sizeof(NDIS_SYSTEM_PROCESSOR_INFO_EX) + Count * sizeof(NDIS_PROCESSOR_INFO_EX);
    if (SystemProcessorInfo == NULL || *Size < Needed)
    {
        *Size = Needed;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }

    RtlZeroMemory(SystemProcessorInfo, Needed);
    SystemProcessorInfo->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    SystemProcessorInfo->Header.Revision = NDIS_SYSTEM_PROCESSOR_INFO_EX_REVISION_1;
    SystemProcessorInfo->Header.Size = NDIS_SIZEOF_SYSTEM_PROCESSOR_INFO_EX_REVISION_1;
    SystemProcessorInfo->ProcessorVendor = NdisProcessorVendorUnknown;
    SystemProcessorInfo->NumSockets = 1;
    SystemProcessorInfo->NumCores = Count;
    SystemProcessorInfo->NumCoresPerSocket = Count;
    SystemProcessorInfo->MaxHyperThreadingProcsPerCore = 1;
    SystemProcessorInfo->ProcessorInfoOffset = sizeof(NDIS_SYSTEM_PROCESSOR_INFO_EX);
    SystemProcessorInfo->NumberOfProcessors = Count;
    SystemProcessorInfo->ProcessorInfoEntrySize = sizeof(NDIS_PROCESSOR_INFO_EX);

    Processors = (PNDIS_PROCESSOR_INFO_EX)((PUCHAR)SystemProcessorInfo + sizeof(NDIS_SYSTEM_PROCESSOR_INFO_EX));
    for (i = 0; i < Count; i++)
    {
        KeGetProcessorNumberFromIndex(i, &Processors[i].ProcNum);
        Processors[i].CoreId = i;
    }

    *Size = Needed;
    return NDIS_STATUS_SUCCESS;
}

/* Optional handlers */

/**
 * @brief
 * Registers optional handlers of a miniport driver: its PnP handlers and its
 * selective suspend handlers.
 *
 * @param[in] NdisHandle
 * The miniport driver handle, from MiniportSetOptions.
 *
 * @param[in] OptionalHandlers
 * The handlers, identified by Header.Type.
 *
 * @return
 * NDIS_STATUS_SUCCESS, NDIS_STATUS_NOT_SUPPORTED for anything else, or
 * NDIS_STATUS_INVALID_PARAMETER for a malformed structure.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisSetOptionalHandlers(
    NDIS_HANDLE NdisHandle,
    PNDIS_DRIVER_OPTIONAL_HANDLERS OptionalHandlers)
{
    PNDIS_M_DRIVER_BLOCK Driver = CoreDriverFromHandle(NdisHandle);

    if (Driver == NULL || !Driver->Ndis6Driver)
        return NDIS_STATUS_NOT_SUPPORTED;

    switch (OptionalHandlers->Header.Type)
    {
        case NDIS_OBJECT_TYPE_MINIPORT_PNP_CHARACTERISTICS:
            if (OptionalHandlers->Header.Revision == 0 ||
                OptionalHandlers->Header.Size < NDIS_SIZEOF_MINIPORT_PNP_CHARACTERISTICS_REVISION_1)
            {
                return NDIS_STATUS_INVALID_PARAMETER;
            }

            RtlCopyMemory(&Driver->PnpCharacteristics, OptionalHandlers, sizeof(Driver->PnpCharacteristics));
            return NDIS_STATUS_SUCCESS;

        case NDIS_OBJECT_TYPE_MINIPORT_SS_CHARACTERISTICS:
            if (OptionalHandlers->Header.Revision == 0 ||
                OptionalHandlers->Header.Size < NDIS_SIZEOF_MINIPORT_SS_CHARACTERISTICS_REVISION_1)
            {
                return NDIS_STATUS_INVALID_PARAMETER;
            }

            RtlCopyMemory(&Driver->SsCharacteristics, OptionalHandlers, sizeof(Driver->SsCharacteristics));
            return NDIS_STATUS_SUCCESS;

        default:
            NDIS_DbgPrint(MIN_TRACE, ("Optional handlers of type 0x%x not supported.\n",
                                      OptionalHandlers->Header.Type));
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

/* Selective suspend: NDIS never sends an idle notification, so these have nothing to complete */

/**
 * @brief
 * A miniport confirms an idle notification.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 *
 * @param[in] IdlePowerState
 * The power state it can go to.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisMIdleNotificationConfirm(
    NDIS_HANDLE MiniportAdapterHandle,
    NDIS_DEVICE_POWER_STATE IdlePowerState)
{
    UNREFERENCED_PARAMETER(MiniportAdapterHandle);
    UNREFERENCED_PARAMETER(IdlePowerState);

    NDIS_DbgPrint(MIN_TRACE, ("Idle confirmation with no idle notification outstanding.\n"));
}

/**
 * @brief
 * A miniport completes an idle notification.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisMIdleNotificationComplete(
    NDIS_HANDLE MiniportAdapterHandle)
{
    UNREFERENCED_PARAMETER(MiniportAdapterHandle);

    NDIS_DbgPrint(MIN_TRACE, ("Idle completion with no idle notification outstanding.\n"));
}

/* SR-IOV: the bus offers no virtualization interface, so there are no virtual functions */

_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisMEnableVirtualization(
    NDIS_HANDLE NdisMiniportHandle,
    USHORT NumVFs,
    BOOLEAN EnableVFMigration,
    BOOLEAN EnableMigrationInterrupt,
    BOOLEAN EnableVirtualization)
{
    UNREFERENCED_PARAMETER(NdisMiniportHandle);
    UNREFERENCED_PARAMETER(NumVFs);
    UNREFERENCED_PARAMETER(EnableVFMigration);
    UNREFERENCED_PARAMETER(EnableMigrationInterrupt);
    UNREFERENCED_PARAMETER(EnableVirtualization);

    return NDIS_STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
ULONG
NTAPI
NdisMGetVirtualFunctionBusData(
    NDIS_HANDLE NdisMiniportHandle,
    NDIS_SRIOV_FUNCTION_ID VFId,
    PVOID Buffer,
    ULONG Offset,
    ULONG Length)
{
    UNREFERENCED_PARAMETER(NdisMiniportHandle);
    UNREFERENCED_PARAMETER(VFId);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Offset);
    UNREFERENCED_PARAMETER(Length);

    return 0;
}

_Use_decl_annotations_
ULONG
NTAPI
NdisMSetVirtualFunctionBusData(
    NDIS_HANDLE NdisMiniportHandle,
    NDIS_SRIOV_FUNCTION_ID VFId,
    PVOID Buffer,
    ULONG Offset,
    ULONG Length)
{
    UNREFERENCED_PARAMETER(NdisMiniportHandle);
    UNREFERENCED_PARAMETER(VFId);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Offset);
    UNREFERENCED_PARAMETER(Length);

    return 0;
}

_Use_decl_annotations_
VOID
NTAPI
NdisMGetVirtualFunctionLocation(
    NDIS_HANDLE NdisMiniportHandle,
    NDIS_SRIOV_FUNCTION_ID VFId,
    PUSHORT SegmentNumber,
    PUCHAR BusNumber,
    PUCHAR FunctionNumber)
{
    UNREFERENCED_PARAMETER(NdisMiniportHandle);
    UNREFERENCED_PARAMETER(VFId);
    UNREFERENCED_PARAMETER(SegmentNumber);
    UNREFERENCED_PARAMETER(BusNumber);
    UNREFERENCED_PARAMETER(FunctionNumber);
}

_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisMQueryProbedBars(
    NDIS_HANDLE NdisMiniportHandle,
    PULONG BaseRegisterValues)
{
    UNREFERENCED_PARAMETER(NdisMiniportHandle);
    UNREFERENCED_PARAMETER(BaseRegisterValues);

    return NDIS_STATUS_NOT_SUPPORTED;
}
