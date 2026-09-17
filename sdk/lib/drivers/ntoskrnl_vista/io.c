/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Io functions of Vista+
 * COPYRIGHT:   2016 Pierre Schweitzer (pierre@reactos.org)
 *              2020 Victor Perevertkin (victor.perevertkin@reactos.org)
 *              2026 Justin Miller (justin.miller@reactos.org)
 */

#include "ntoskrnl_vista.h"

typedef struct _EX_WORKITEM_CONTEXT
{
    PIO_WORKITEM WorkItem;
    PIO_WORKITEM_ROUTINE_EX WorkItemRoutineEx;
    PVOID Context;
} EX_WORKITEM_CONTEXT, *PEX_WORKITEM_CONTEXT;

#define TAG_IOWI 'IWOI'

NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoGetIrpExtraCreateParameter(IN PIRP Irp,
                             OUT PECP_LIST *ExtraCreateParameter)
{
    /* Check we have a create operation */
    if (!BooleanFlagOn(Irp->Flags, IRP_CREATE_OPERATION))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* If so, return user buffer */
    *ExtraCreateParameter = Irp->UserBuffer;
    return STATUS_SUCCESS;
}

_Function_class_(IO_WORKITEM_ROUTINE)
static
VOID
NTAPI
IopWorkItemExCallback(
    PDEVICE_OBJECT DeviceObject,
    PVOID Ctx)
{
    PEX_WORKITEM_CONTEXT context = Ctx;

    context->WorkItemRoutineEx(DeviceObject, context->Context, context->WorkItem);
    ExFreePoolWithTag(context, TAG_IOWI);
}

NTKRNLVISTAAPI
VOID
NTAPI
IoQueueWorkItemEx(
    _Inout_ PIO_WORKITEM IoWorkItem,
    _In_ PIO_WORKITEM_ROUTINE_EX WorkerRoutine,
    _In_ WORK_QUEUE_TYPE QueueType,
    _In_opt_ __drv_aliasesMem PVOID Context)
{
    PEX_WORKITEM_CONTEXT newContext = ExAllocatePoolWithTag(NonPagedPoolMustSucceed, sizeof(*newContext), TAG_IOWI);
    newContext->WorkItem = IoWorkItem;
    newContext->WorkItemRoutineEx = WorkerRoutine;
    newContext->Context = Context;

    IoQueueWorkItem(IoWorkItem, IopWorkItemExCallback, QueueType, newContext);
}

NTKRNLVISTAAPI
IO_PRIORITY_HINT
NTAPI
IoGetIoPriorityHint(
    _In_ PIRP Irp)
{
    return IoPriorityNormal;
}

NTKRNLVISTAAPI
VOID
IoSetMasterIrpStatus(
    _Inout_ PIRP MasterIrp,
    _In_ NTSTATUS Status)
{
    NTSTATUS MasterStatus = MasterIrp->IoStatus.Status;

    if (Status == STATUS_FT_READ_FROM_COPY)
    {
        return;
    }

    if ((Status == STATUS_VERIFY_REQUIRED) ||
        (MasterStatus == STATUS_SUCCESS && !NT_SUCCESS(Status)) ||
        (!NT_SUCCESS(MasterStatus) && !NT_SUCCESS(Status) && Status > MasterStatus))
    {
        MasterIrp->IoStatus.Status = Status;
    }
}

/**
 * @brief
 * Returns the NUMA node a physical device object sits on.
 *
 * @param[in] Pdo
 * The physical device object to query.
 *
 * @param[out] NodeNumber
 * Receives the NUMA node number.
 *
 * @return
 * STATUS_SUCCESS. ReactOS models one NUMA node, so node 0 is always reported.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoGetDeviceNumaNode(
    _In_ PDEVICE_OBJECT Pdo,
    _Out_ PUSHORT NodeNumber)
{
    UNREFERENCED_PARAMETER(Pdo);

    *NodeNumber = 0;
    return STATUS_SUCCESS;
}

_Function_class_(IO_COMPLETION_ROUTINE)
static
NTSTATUS
NTAPI
IopSynchronousCallCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief
 * Hands a built IRP to a driver and waits for it to complete.
 *
 * @param[in] DeviceObject
 * The target device object.
 *
 * @param[in] Irp
 * The IRP to dispatch. The caller owns it and must have left a stack location
 * spare for the completion routine.
 *
 * @return
 * The completion status of the IRP.
 */
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoSynchronousCallDriver(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    KEVENT Event;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoSetCompletionRoutine(Irp, IopSynchronousCallCompletion, &Event, TRUE, TRUE, TRUE);

    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Irp->IoStatus.Status;
    }

    return Status;
}

/**
 * @brief
 * Asks for a device to be ejected and reports the outcome to a callback.
 *
 * @param[in] PhysicalDeviceObject
 * The physical device object to eject.
 *
 * @param[in] Callback
 * Optional callback invoked once the request is resolved.
 *
 * @param[in] Context
 * Optional context handed to @p Callback.
 *
 * @param[in] DriverObject
 * Optional driver object owning the request.
 *
 * @return
 * STATUS_SUCCESS once the request has been submitted.
 *
 * @remarks
 * The request goes through the plain IoRequestDeviceEject(), so the completion
 * callback is never invoked.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoRequestDeviceEjectEx(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_opt_ PIO_DEVICE_EJECT_CALLBACK Callback,
    _In_opt_ PVOID Context,
    _In_opt_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(Callback);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(DriverObject);

    IoRequestDeviceEject(PhysicalDeviceObject);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Asks for a device to be removed so its bus driver can reset it.
 *
 * @param[in] PhysicalDeviceObject
 * The physical device object to remove.
 *
 * @param[in] Flags
 * Reset request flags.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoRequestDeviceRemovalForReset(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);
    UNREFERENCED_PARAMETER(Flags);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Drops a PnP notification registration.
 *
 * @param[in] NotificationEntry
 * The entry returned by IoRegisterPlugPlayNotification().
 *
 * @return
 * The status returned by IoUnregisterPlugPlayNotification().
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoUnregisterPlugPlayNotificationEx(
    _In_ PVOID NotificationEntry)
{
    return IoUnregisterPlugPlayNotification(NotificationEntry);
}

/**
 * @brief
 * Reports that a device interrupt went active.
 *
 * @param[in] Parameters
 * Describes the interrupt being reported.
 *
 * @return
 * STATUS_SUCCESS. The accounting only feeds runtime power management, which
 * ReactOS does not drive from interrupt state.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoReportInterruptActive(
    _In_ PVOID Parameters)
{
    UNREFERENCED_PARAMETER(Parameters);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reports that a device interrupt went inactive.
 *
 * @param[in] Parameters
 * Describes the interrupt being reported.
 *
 * @return
 * STATUS_SUCCESS. See IoReportInterruptActive().
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoReportInterruptInactive(
    _In_ PVOID Parameters)
{
    UNREFERENCED_PARAMETER(Parameters);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Registers for container state change notifications.
 *
 * @param[in] NotificationClass
 * The class of notification being asked for.
 *
 * @param[in] CallbackFunction
 * The callback to run when the notification fires.
 *
 * @param[in] NotificationInformation
 * Optional class specific information.
 *
 * @param[in] NotificationInformationLength
 * Size of @p NotificationInformation, in bytes.
 *
 * @param[out] CallbackRegistration
 * Receives the registration handle.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoRegisterContainerNotification(
    _In_ IO_CONTAINER_NOTIFICATION_CLASS NotificationClass,
    _In_ PIO_CONTAINER_NOTIFICATION_FUNCTION CallbackFunction,
    _In_reads_bytes_opt_(NotificationInformationLength) PVOID NotificationInformation,
    _In_ ULONG NotificationInformationLength,
    _Out_ PVOID CallbackRegistration)
{
    UNREFERENCED_PARAMETER(NotificationClass);
    UNREFERENCED_PARAMETER(CallbackFunction);
    UNREFERENCED_PARAMETER(NotificationInformation);
    UNREFERENCED_PARAMETER(NotificationInformationLength);
    UNREFERENCED_PARAMETER(CallbackRegistration);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Drops a container notification registration.
 *
 * @param[in] CallbackRegistration
 * The handle returned by IoRegisterContainerNotification().
 *
 * @unimplemented
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
NTKRNLVISTAAPI
VOID
NTAPI
IoUnregisterContainerNotification(
    _In_ PVOID CallbackRegistration)
{
    UNREFERENCED_PARAMETER(CallbackRegistration);
}

/**
 * @brief
 * Reserves a dependency object for a device.
 *
 * @param[in] DeviceObject
 * The device the dependency belongs to.
 *
 * @param[out] Dependency
 * Receives the reserved dependency.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoReserveDependency(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Out_ PVOID *Dependency)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    if (Dependency != NULL)
        *Dependency = NULL;

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Ties a reserved dependency to the device that depends on it.
 *
 * @param[in] Dependency
 * The dependency returned by IoReserveDependency().
 *
 * @param[in] DependentDeviceObject
 * The device that depends on it.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoSetDependency(
    _In_ PVOID Dependency,
    _In_ PDEVICE_OBJECT DependentDeviceObject)
{
    UNREFERENCED_PARAMETER(Dependency);
    UNREFERENCED_PARAMETER(DependentDeviceObject);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Resolves a reserved dependency.
 *
 * @param[in] Dependency
 * The dependency to resolve.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoResolveDependency(
    _In_ PVOID Dependency)
{
    UNREFERENCED_PARAMETER(Dependency);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Tests whether a dependency is satisfied.
 *
 * @param[in] Dependency
 * The dependency to test.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoTestDependency(
    _In_ PVOID Dependency)
{
    UNREFERENCED_PARAMETER(Dependency);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Duplicates a dependency object.
 *
 * @param[in] Dependency
 * The dependency to duplicate.
 *
 * @param[out] DuplicateDependency
 * Receives the duplicate.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoDuplicateDependency(
    _In_ PVOID Dependency,
    _Out_ PVOID *DuplicateDependency)
{
    UNREFERENCED_PARAMETER(Dependency);

    if (DuplicateDependency != NULL)
        *DuplicateDependency = NULL;

    return STATUS_NOT_IMPLEMENTED;
}
