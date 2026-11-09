/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/io/iomgr/iowork.c
 * PURPOSE:         I/O Wrappers for the Executive Work Item Functions
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 *                  Robert Dickenson (odin@pnc.com.au)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* PRIVATE FUNCTIONS *********************************************************/

VOID
NTAPI
IopWorkItemCallback(IN PVOID Parameter)
{
    PIO_WORKITEM IoWorkItem = (PIO_WORKITEM)Parameter;
    PDEVICE_OBJECT DeviceObject = IoWorkItem->DeviceObject;
    PAGED_CODE();

    /* Call the work routine, the extended form also gets the work item */
    if (IoWorkItem->WorkerRoutineEx)
        IoWorkItem->WorkerRoutineEx(DeviceObject, IoWorkItem->Context, IoWorkItem);
    else
        IoWorkItem->WorkerRoutine(DeviceObject, IoWorkItem->Context);

    /* Dereference the device object */
    ObDereferenceObject(DeviceObject);
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
VOID
NTAPI
IoQueueWorkItem(IN PIO_WORKITEM IoWorkItem,
                IN PIO_WORKITEM_ROUTINE WorkerRoutine,
                IN WORK_QUEUE_TYPE QueueType,
                IN PVOID Context)
{
    /* Make sure we're called at DISPATCH or lower */
    ASSERT_IRQL_LESS_OR_EQUAL(DISPATCH_LEVEL);

    /* Reference the device object */
    ObReferenceObject(IoWorkItem->DeviceObject);

    /* Setup the work item */
    IoWorkItem->WorkerRoutine = WorkerRoutine;
    IoWorkItem->WorkerRoutineEx = NULL;
    IoWorkItem->Context = Context;

    /* Queue the work item */
    ExQueueWorkItem(&IoWorkItem->Item, QueueType);
}

/*
 * @implemented
 */
VOID
NTAPI
IoFreeWorkItem(IN PIO_WORKITEM IoWorkItem)
{
    /* Free the work item */
    ExFreePoolWithTag(IoWorkItem, TAG_IOWI);
}

/*
 * @implemented
 */
PIO_WORKITEM
NTAPI
IoAllocateWorkItem(IN PDEVICE_OBJECT DeviceObject)
{
    PIO_WORKITEM IoWorkItem;

    /* Allocate the work item */
    IoWorkItem = ExAllocatePoolWithTag(NonPagedPool,
                                       sizeof(IO_WORKITEM),
                                       TAG_IOWI);
    if (!IoWorkItem) return NULL;

    /* Initialize it */
    IoWorkItem->DeviceObject = DeviceObject;
    IoWorkItem->WorkerRoutineEx = NULL;
    ExInitializeWorkItem(&IoWorkItem->Item, IopWorkItemCallback, IoWorkItem);

    /* Return it */
    return IoWorkItem;
}

/**
 * @brief Queues a work item whose routine also receives the work item itself.
 */
VOID
NTAPI
IoQueueWorkItemEx(
    _Inout_ PIO_WORKITEM IoWorkItem,
    _In_ PIO_WORKITEM_ROUTINE_EX WorkerRoutine,
    _In_ WORK_QUEUE_TYPE QueueType,
    _In_opt_ PVOID Context)
{
    ASSERT_IRQL_LESS_OR_EQUAL(DISPATCH_LEVEL);

    ObReferenceObject(IoWorkItem->DeviceObject);

    IoWorkItem->WorkerRoutine = NULL;
    IoWorkItem->WorkerRoutineEx = WorkerRoutine;
    IoWorkItem->Context = Context;

    ExQueueWorkItem(&IoWorkItem->Item, QueueType);
}

/**
 * @brief Size of the storage a caller provides to IoInitializeWorkItem.
 */
ULONG
NTAPI
IoSizeofWorkItem(VOID)
{
    return sizeof(IO_WORKITEM);
}

/**
 * @brief Prepares caller allocated work item storage, IoObject may be a device or driver object.
 */
VOID
NTAPI
IoInitializeWorkItem(
    _In_ PVOID IoObject,
    _Out_ PIO_WORKITEM IoWorkItem)
{
    RtlZeroMemory(IoWorkItem, sizeof(*IoWorkItem));
    IoWorkItem->DeviceObject = IoObject;
    ExInitializeWorkItem(&IoWorkItem->Item, IopWorkItemCallback, IoWorkItem);
}

/**
 * @brief Releases a work item set up with IoInitializeWorkItem, the storage stays with the caller.
 */
VOID
NTAPI
IoUninitializeWorkItem(
    _Inout_ PIO_WORKITEM IoWorkItem)
{
    UNREFERENCED_PARAMETER(IoWorkItem);
}

/* EOF */
