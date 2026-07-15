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

_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoSetDevicePropertyData(
    _In_ PDEVICE_OBJECT Pdo,
    _In_ CONST DEVPROPKEY *PropertyKey,
    _In_ LCID Lcid,
    _In_ ULONG Flags,
    _In_ DEVPROPTYPE Type,
    _In_ ULONG Size,
    _In_opt_ PVOID Data)
{
    return STATUS_NOT_IMPLEMENTED;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoGetDevicePropertyData(
    _In_ PDEVICE_OBJECT Pdo,
    _In_ CONST DEVPROPKEY *PropertyKey,
    _In_ LCID Lcid,
    _Reserved_ ULONG Flags,
    _In_ ULONG Size,
    _Out_ PVOID Data,
    _Out_ PULONG RequiredSize,
    _Out_ PDEVPROPTYPE Type)
{
    return STATUS_NOT_IMPLEMENTED;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
NTKRNLVISTAAPI
NTSTATUS
IoSetDeviceInterfacePropertyData(
    _In_ PUNICODE_STRING SymbolicLinkName,
    _In_ CONST DEVPROPKEY *PropertyKey,
    _In_ LCID Lcid,
    _In_ ULONG Flags,
    _In_ DEVPROPTYPE Type,
    _In_ ULONG Size,
    _In_reads_bytes_opt_(Size) PVOID Data)
{
    return STATUS_NOT_IMPLEMENTED;
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
 * Returns the NUMA node a physical device object is attached to.
 *
 * @param[in] Pdo
 * The physical device object to query.
 *
 * @param[out] NodeNumber
 * Receives the NUMA node number.
 *
 * @return
 * STATUS_SUCCESS. ReactOS models a single NUMA node, so node 0 is always
 * reported.
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
 * Sends an already-built IRP to a driver and waits synchronously for it to
 * complete.
 *
 * @param[in] DeviceObject
 * The target device object.
 *
 * @param[in] Irp
 * The IRP to dispatch. The caller must have allocated an additional stack
 * location for the completion routine and retains ownership of the IRP.
 *
 * @return
 * The final completion status of the IRP.
 */
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
 * Requests ejection of a device, invoking an optional callback when the
 * operation completes.
 *
 * @param[in] PhysicalDeviceObject
 * The physical device object to eject.
 *
 * @param[in] Callback
 * Optional callback invoked when the eject request is resolved.
 *
 * @param[in] Context
 * Optional context passed to @p Callback.
 *
 * @param[in] DriverObject
 * Optional driver object owning the request.
 *
 * @return
 * STATUS_SUCCESS if the eject request was submitted.
 *
 * @remarks
 * ReactOS forwards the request to the legacy IoRequestDeviceEject(). The
 * completion callback is not yet invoked.
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
 * Requests removal of a device so it can be reset by its parent bus driver.
 *
 * @param[in] PhysicalDeviceObject
 * The physical device object to remove for reset.
 *
 * @param[in] Flags
 * Reset request flags.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement device reset recovery.
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
 * Unregisters a PnP notification entry and frees associated resources.
 *
 * @param[in] NotificationEntry
 * The notification entry returned by IoRegisterPlugPlayNotification().
 *
 * @return
 * STATUS_SUCCESS on success.
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
 * Reports that a device's interrupt has become active.
 *
 * @param[in] Parameters
 * Describes the interrupt whose state is being reported.
 *
 * @return
 * STATUS_SUCCESS. The active/inactive interrupt accounting used for runtime
 * power management is a no-op on ReactOS.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
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
 * Reports that a device's interrupt has become inactive.
 *
 * @param[in] Parameters
 * Describes the interrupt whose state is being reported.
 *
 * @return
 * STATUS_SUCCESS. The active/inactive interrupt accounting used for runtime
 * power management is a no-op on ReactOS.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
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
 * Registers for container (session) state change notifications.
 *
 * @param[in] NotificationClass
 * The class of container notification requested.
 *
 * @param[in] CallbackFunction
 * The callback invoked when the notification fires.
 *
 * @param[in] NotificationInformation
 * Optional class-specific notification information.
 *
 * @param[in] NotificationInformationLength
 * Size, in bytes, of @p NotificationInformation.
 *
 * @param[out] CallbackRegistration
 * Receives the registration handle.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement container notifications.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
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
 * Unregisters a container notification.
 *
 * @param[in] CallbackRegistration
 * The registration handle returned by IoRegisterContainerNotification().
 *
 * @unimplemented
 * ReactOS does not implement container notifications.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
IoUnregisterContainerNotification(
    _In_ PVOID CallbackRegistration)
{
    UNREFERENCED_PARAMETER(CallbackRegistration);
}

/*
 * Reset-recovery driver dependency tracking (Windows 8+). ReactOS does not
 * implement this subsystem; the routines below provide the exported surface.
 */

/**
 * @brief
 * Reserves a dependency object associated with a device.
 *
 * @param[in] DeviceObject
 * The device object the dependency is associated with.
 *
 * @param[out] Dependency
 * Receives the reserved dependency handle.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
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
 * Associates a dependency with a dependent device object.
 *
 * @param[in] Dependency
 * The dependency handle returned by IoReserveDependency().
 *
 * @param[in] DependentDeviceObject
 * The device object that depends on the reserved dependency.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
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
 * Resolves a previously reserved dependency.
 *
 * @param[in] Dependency
 * The dependency handle to resolve.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
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
 * The dependency handle to test.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
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
 * The dependency handle to duplicate.
 *
 * @param[out] DuplicateDependency
 * Receives the duplicated dependency handle.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
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
