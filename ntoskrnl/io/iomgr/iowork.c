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

    /* Call the work routine */
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
    ExInitializeWorkItem(&IoWorkItem->Item, IopWorkItemCallback, IoWorkItem);

    /* Return it */
    return IoWorkItem;
}

/**
 * @brief
 * Returns the size, in bytes, of an I/O work item structure.
 *
 * @return
 * The number of bytes a caller must allocate before calling
 * IoInitializeWorkItem().
 *
 * @remarks
 * Use this together with IoInitializeWorkItem() when embedding a work item in
 * a caller-owned allocation instead of allocating one with
 * IoAllocateWorkItem().
 *
 * @implemented
 */
ULONG
NTAPI
IoSizeofWorkItem(VOID)
{
    return sizeof(IO_WORKITEM);
}

/**
 * @brief
 * Initializes a caller-allocated I/O work item.
 *
 * @param[in] IoObject
 * The device or driver object the work item is associated with. A reference is
 * taken on this object each time the work item is queued.
 *
 * @param[out] IoWorkItem
 * A caller-owned buffer of at least IoSizeofWorkItem() bytes to initialize.
 *
 * @remarks
 * A work item initialized with this routine must be released with
 * IoUninitializeWorkItem() rather than IoFreeWorkItem().
 *
 * @implemented
 */
VOID
NTAPI
IoInitializeWorkItem(IN PVOID IoObject,
                     OUT PIO_WORKITEM IoWorkItem)
{
    /* Initialize the caller-provided work item */
    IoWorkItem->DeviceObject = IoObject;
    ExInitializeWorkItem(&IoWorkItem->Item, IopWorkItemCallback, IoWorkItem);
}

/**
 * @brief
 * Releases an I/O work item that was initialized with IoInitializeWorkItem().
 *
 * @param[in,out] IoWorkItem
 * The work item to uninitialize.
 *
 * @remarks
 * The caller remains responsible for freeing the memory backing the work item.
 *
 * @implemented
 */
VOID
NTAPI
IoUninitializeWorkItem(IN OUT PIO_WORKITEM IoWorkItem)
{
    /* Nothing to release: the memory is owned by the caller */
    IoWorkItem->DeviceObject = NULL;
    IoWorkItem->WorkerRoutine = NULL;
    IoWorkItem->Context = NULL;
}

/* EOF */
