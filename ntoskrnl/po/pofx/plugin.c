/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Power engine plug-in (PEP) host
 * COPYRIGHT:   Copyright 2023 George Bișoc <george.bisoc@reactos.org>
 *              Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#include <devpkey.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Devices a plug-in accepted in PEP_DPM_PREPARE_DEVICE, guarded by PopFxPluginLock */
static LIST_ENTRY PopFxPreparedList = { &PopFxPreparedList, &PopFxPreparedList };

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief Returns the plug-in registered after Plugin, or the first one when Plugin is NULL.
 *
 * @remarks Plug-ins are never unregistered, so the returned pointer stays valid.
 */
PPOP_FX_PLUGIN
NTAPI
PopFxNextPlugin(
    _In_opt_ PPOP_FX_PLUGIN Plugin)
{
    PLIST_ENTRY Entry;
    PPOP_FX_PLUGIN Next = NULL;

    KeAcquireGuardedMutex(&PopFxPluginLock);
    Entry = Plugin ? Plugin->Link.Flink : PopFxPluginList.Flink;
    if (Entry != &PopFxPluginList)
        Next = CONTAINING_RECORD(Entry, POP_FX_PLUGIN, Link);
    KeReleaseGuardedMutex(&PopFxPluginLock);

    return Next;
}

/**
 * @brief Hands one queued PEP_DPM_WORK request to the plug-in and carries out what it asks for.
 */
static
VOID
PopFxRunPluginWork(
    _In_ PPOP_FX_PLUGIN Plugin)
{
    PEP_WORK_INFORMATION Information;
    PEP_WORK Work;

    RtlZeroMemory(&Information, sizeof(Information));
    Work.WorkInformation = &Information;
    Work.NeedWork = FALSE;

    if (!PopFxNotifyPlugin(Plugin, PEP_DPM_WORK, &Work) || !Work.NeedWork)
        return;

    switch (Information.WorkType)
    {
        case PepWorkRequestPowerControl:
            PopFxRunPepPowerControl((PPOP_FX_DEVICE)Information.PowerControl.DeviceHandle,
                                    &Information.PowerControl);
            break;

        case PepWorkCompleteIdleState:
            PopFxCompletePepIdleState((PPOP_FX_DEVICE)Information.CompleteIdleState.DeviceHandle,
                                      Information.CompleteIdleState.Component);
            break;

        case PepWorkCompletePerfState:
            PopFxCompletePepPerfState((PPOP_FX_DEVICE)Information.CompletePerfState.DeviceHandle,
                                      Information.CompletePerfState.Component,
                                      Information.CompletePerfState.Succeeded);
            break;

        default:
            DPRINT1("PEP %p asked for unsupported work type %lu\n", Plugin, Information.WorkType);
            break;
    }
}

/**
 * @brief Worker thread body, serves RequestWorker calls one PEP_DPM_WORK at a time.
 */
static
VOID
NTAPI
PopFxPluginWorker(
    _In_ PVOID Parameter)
{
    PPOP_FX_PLUGIN_WORKER Worker = Parameter;
    PPOP_FX_PLUGIN Plugin = Worker->Plugin;
    LONG Slot = Worker->Slot;
    LONG Pending;

    for (;;)
    {
        /* Take one request, every RequestWorker call is answered by exactly one PEP_DPM_WORK */
        Pending = Plugin->WorkRequests;
        while (Pending > 0)
        {
            LONG Seen = InterlockedCompareExchange(&Plugin->WorkRequests, Pending - 1, Pending);
            if (Seen == Pending)
                break;
            Pending = Seen;
        }

        if (Pending > 0)
        {
            PopFxRunPluginWork(Plugin);
            continue;
        }

        /* Release the slot, then look again so a request raced in after the check is not lost */
        InterlockedAnd(&Plugin->WorkersBusy, ~(1L << Slot));
        if (Plugin->WorkRequests <= 0 ||
            (InterlockedOr(&Plugin->WorkersBusy, 1L << Slot) & (1L << Slot)))
        {
            return;
        }
    }
}

/**
 * @brief Queues a worker for the plug-in unless enough of them are already running.
 */
static
VOID
PopFxStartPluginWorker(
    _In_ PPOP_FX_PLUGIN Plugin)
{
    LONG Slots;
    LONG Slot;

    Slots = (Plugin->Flags & PEP_FLAG_WORKER_CONCURRENCY) ? POP_FX_PLUGIN_WORKERS : 1;
    for (Slot = 0; Slot < Slots; Slot++)
    {
        if (InterlockedOr(&Plugin->WorkersBusy, 1L << Slot) & (1L << Slot))
            continue;

        ExQueueWorkItem(&Plugin->Workers[Slot].WorkItem, DelayedWorkQueue);
        return;
    }
}

/* Kernel services the plug-in gets back from PoFxRegisterPluginEx */

static POFXCALLBACKREQUESTWORKER PopFxRequestWorker;
static POFXCALLBACKENUMERATEUNMASKEDINTERRUPTS PopFxEnumerateUnmaskedInterrupts;
static POFXCALLBACKPROCESSORHALT PopFxProcessorHalt;
static POFXCALLBACKREQUESTINTERRUPT PopFxRequestInterrupt;
static POFXCALLBACKCRITICALRESOURCE PopFxTransitionCriticalResource;
static POFXCALLBACKPROCESSORIDLEVETO PopFxProcessorIdleVeto;
static POFXCALLBACKPLATFORMIDLEVETO PopFxPlatformIdleVeto;
static POFXCALLBACKUPDATEPROCESSORIDLESTATE PopFxUpdateProcessorIdleState;
static POFXCALLBACKUPDATEPLATFORMIDLESTATE PopFxUpdatePlatformIdleState;
static POFXCALLBACKREQUESTCOMMON PopFxRequestCommon;

static
VOID
PopFxRequestWorker(
    _In_ POHANDLE PluginHandle)
{
    PPOP_FX_PLUGIN Plugin = (PPOP_FX_PLUGIN)PluginHandle;

    if (!Plugin)
        return;

    InterlockedIncrement(&Plugin->WorkRequests);
    PopFxStartPluginWorker(Plugin);
}

/**
 * @brief No interrupt controller hands its unmasked lines to plug-ins.
 */
static
NTSTATUS
PopFxEnumerateUnmaskedInterrupts(
    _In_opt_ POHANDLE PluginHandle,
    _In_opt_ ULONG EnumerateFlags,
    _In_ PPO_ENUMERATE_INTERRUPT_SOURCE_CALLBACK Callback,
    _In_ PVOID CallbackContext,
    _Inout_ PPEP_UNMASKED_INTERRUPT_INFORMATION InterruptInformation)
{
    UNREFERENCED_PARAMETER(PluginHandle);
    UNREFERENCED_PARAMETER(EnumerateFlags);
    UNREFERENCED_PARAMETER(Callback);
    UNREFERENCED_PARAMETER(CallbackContext);
    UNREFERENCED_PARAMETER(InterruptInformation);

    return STATUS_NOT_SUPPORTED;
}

/**
 * @brief Processor idle is not driven through plug-ins, so there is no halt to hand out.
 */
static
NTSTATUS
PopFxProcessorHalt(
    _In_ ULONG Flags,
    _Inout_opt_ PVOID Context,
    _In_ PPROCESSOR_HALT_ROUTINE Halt)
{
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Halt);

    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
PopFxRequestInterrupt(
    _In_ ULONG Gsiv)
{
    UNREFERENCED_PARAMETER(Gsiv);

    return STATUS_NOT_SUPPORTED;
}

static
VOID
PopFxTransitionCriticalResource(
    _In_ POHANDLE DeviceHandle,
    _In_ ULONG Component,
    _In_ BOOLEAN Active)
{
    POFXTRACE(POFX_PEP_DEBUG, "Critical resource %p/%lu active %u\n", DeviceHandle, Component, Active);
}

static
NTSTATUS
PopFxProcessorIdleVeto(
    _In_ POHANDLE ProcessorHandle,
    _In_ ULONG ProcessorState,
    _In_ ULONG VetoReason,
    _In_ BOOLEAN Increment)
{
    UNREFERENCED_PARAMETER(ProcessorHandle);
    UNREFERENCED_PARAMETER(ProcessorState);
    UNREFERENCED_PARAMETER(VetoReason);
    UNREFERENCED_PARAMETER(Increment);

    return STATUS_SUCCESS;
}

static
NTSTATUS
PopFxPlatformIdleVeto(
    _In_ POHANDLE ProcessorHandle,
    _In_ ULONG PlatformState,
    _In_ ULONG VetoReason,
    _In_ BOOLEAN Increment)
{
    UNREFERENCED_PARAMETER(ProcessorHandle);
    UNREFERENCED_PARAMETER(PlatformState);
    UNREFERENCED_PARAMETER(VetoReason);
    UNREFERENCED_PARAMETER(Increment);

    return STATUS_SUCCESS;
}

static
NTSTATUS
PopFxUpdateProcessorIdleState(
    _In_ POHANDLE ProcessorHandle,
    _In_ ULONG ProcessorState,
    _In_ PPEP_PROCESSOR_IDLE_STATE_UPDATE Update)
{
    UNREFERENCED_PARAMETER(ProcessorHandle);
    UNREFERENCED_PARAMETER(ProcessorState);
    UNREFERENCED_PARAMETER(Update);

    return STATUS_SUCCESS;
}

static
NTSTATUS
PopFxUpdatePlatformIdleState(
    _In_ POHANDLE ProcessorHandle,
    _In_ ULONG PlatformState,
    _In_ PPEP_PLATFORM_IDLE_STATE_UPDATE Update)
{
    UNREFERENCED_PARAMETER(ProcessorHandle);
    UNREFERENCED_PARAMETER(PlatformState);
    UNREFERENCED_PARAMETER(Update);

    return STATUS_SUCCESS;
}

static
NTSTATUS
PopFxRequestCommon(
    _In_ ULONG RequestId,
    _Inout_opt_ PVOID Data)
{
    UNREFERENCED_PARAMETER(Data);

    DPRINT1("PEP common request %lu is not supported\n", RequestId);
    return STATUS_NOT_SUPPORTED;
}

/**
 * @brief Offers every device already in the tree to a plug-in that registered after it was enumerated.
 */
static
VOID
PopFxPrepareExistingDevices(VOID)
{
    PDEVICE_OBJECT *Pdos;
    PDEVICE_NODE Node;
    ULONG Count = 0, Capacity = 64, Index;
    KIRQL OldIrql;

    for (;;)
    {
        Pdos = ExAllocatePoolWithTag(NonPagedPool, Capacity * sizeof(*Pdos), TAG_PO_FX_PLUGIN);
        if (!Pdos)
            return;

        /* Walk the tree depth first, taking a reference on each PDO so it outlives the lock */
        Count = 0;
        KeAcquireSpinLock(&IopDeviceTreeLock, &OldIrql);
        Node = IopRootDeviceNode;
        while (Node)
        {
            if (Count < Capacity && Node->PhysicalDeviceObject)
                ObReferenceObject(Node->PhysicalDeviceObject);
            if (Node->PhysicalDeviceObject)
            {
                if (Count < Capacity)
                    Pdos[Count] = Node->PhysicalDeviceObject;
                Count++;
            }

            if (Node->Child)
            {
                Node = Node->Child;
                continue;
            }
            while (Node && !Node->Sibling)
                Node = Node->Parent;
            if (Node)
                Node = Node->Sibling;
        }
        KeReleaseSpinLock(&IopDeviceTreeLock, OldIrql);

        if (Count <= Capacity)
            break;

        /* The tree outgrew the buffer, drop what was taken and retry with room for all of it */
        for (Index = 0; Index < Capacity; Index++)
            ObDereferenceObject(Pdos[Index]);
        ExFreePoolWithTag(Pdos, TAG_PO_FX_PLUGIN);
        Capacity = Count + 16;
    }

    for (Index = 0; Index < Count; Index++)
    {
        PopFxPrepareDevice(Pdos[Index]);
        ObDereferenceObject(Pdos[Index]);
    }

    ExFreePoolWithTag(Pdos, TAG_PO_FX_PLUGIN);
}

/* INTERNAL FUNCTIONS *********************************************************/

/**
 * @brief Sends a device power management notification to a plug-in.
 *
 * @return TRUE when the plug-in handled the notification.
 */
BOOLEAN
NTAPI
PopFxNotifyPlugin(
    _In_ PPOP_FX_PLUGIN Plugin,
    _In_ ULONG Notification,
    _Inout_ PVOID Data)
{
    if (!Plugin || !Plugin->AcceptDeviceNotification)
        return FALSE;

    return Plugin->AcceptDeviceNotification(Notification, Data);
}

/**
 * @brief Gets the id plug-ins know a device by: its ACPI namespace path, else its instance path.
 */
NTSTATUS
NTAPI
PopFxQueryDeviceId(
    _In_ PDEVICE_OBJECT Pdo,
    _Out_ PUNICODE_STRING DeviceId)
{
    PDEVICE_NODE DeviceNode;
    DEVPROPTYPE Type;
    ULONG Size = 0;
    PWSTR Buffer;
    NTSTATUS Status;

    PAGED_CODE();

    RtlInitEmptyUnicodeString(DeviceId, NULL, 0);

    Status = IoGetDevicePropertyData(Pdo, &DEVPKEY_Device_BiosDeviceName, LOCALE_NEUTRAL, 0,
                                     0, NULL, &Size, &Type);
    if (Status == STATUS_BUFFER_TOO_SMALL && Type == DEVPROP_TYPE_STRING &&
        Size > sizeof(UNICODE_NULL) && Size <= UNICODE_STRING_MAX_BYTES)
    {
        Buffer = ExAllocatePoolWithTag(PagedPool, Size, TAG_PO_FX_PLUGIN);
        if (!Buffer)
            return STATUS_INSUFFICIENT_RESOURCES;

        Status = IoGetDevicePropertyData(Pdo, &DEVPKEY_Device_BiosDeviceName, LOCALE_NEUTRAL, 0,
                                         Size, Buffer, &Size, &Type);
        if (NT_SUCCESS(Status))
        {
            Buffer[Size / sizeof(WCHAR) - 1] = UNICODE_NULL;
            RtlInitUnicodeString(DeviceId, Buffer);
            DeviceId->MaximumLength = (USHORT)Size;
            return STATUS_SUCCESS;
        }

        ExFreePoolWithTag(Buffer, TAG_PO_FX_PLUGIN);
    }

    DeviceNode = IopGetDeviceNode(Pdo);
    if (!DeviceNode || !DeviceNode->InstancePath.Length)
        return STATUS_NOT_FOUND;

    Size = DeviceNode->InstancePath.Length + sizeof(UNICODE_NULL);
    Buffer = ExAllocatePoolWithTag(PagedPool, Size, TAG_PO_FX_PLUGIN);
    if (!Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlInitEmptyUnicodeString(DeviceId, Buffer, (USHORT)Size);
    RtlCopyUnicodeString(DeviceId, &DeviceNode->InstancePath);
    return STATUS_SUCCESS;
}

/**
 * @brief Finds the plug-in that accepted a device in PEP_DPM_PREPARE_DEVICE.
 *
 * @param[out] DeviceId
 * Receives a copy of the id the plug-in accepted, the caller frees it.
 */
PPOP_FX_PLUGIN
NTAPI
PopFxFindPreparedDevice(
    _In_ PDEVICE_OBJECT Pdo,
    _Out_ PUNICODE_STRING DeviceId)
{
    PPOP_FX_PREPARED_DEVICE Prepared;
    PPOP_FX_PLUGIN Plugin = NULL;
    PLIST_ENTRY Entry;
    PWSTR Buffer;

    PAGED_CODE();

    RtlInitEmptyUnicodeString(DeviceId, NULL, 0);

    KeAcquireGuardedMutex(&PopFxPluginLock);
    for (Entry = PopFxPreparedList.Flink; Entry != &PopFxPreparedList; Entry = Entry->Flink)
    {
        Prepared = CONTAINING_RECORD(Entry, POP_FX_PREPARED_DEVICE, Link);
        if (Prepared->Pdo != Pdo)
            continue;

        Buffer = ExAllocatePoolWithTag(PagedPool, Prepared->DeviceId.MaximumLength, TAG_PO_FX_PLUGIN);
        if (Buffer)
        {
            RtlInitEmptyUnicodeString(DeviceId, Buffer, Prepared->DeviceId.MaximumLength);
            RtlCopyUnicodeString(DeviceId, &Prepared->DeviceId);
            Plugin = Prepared->Plugin;
        }
        break;
    }
    KeReleaseGuardedMutex(&PopFxPluginLock);

    return Plugin;
}

/**
 * @brief Offers a device about to get its drivers to the plug-ins, the first that accepts powers it up.
 */
VOID
NTAPI
PopFxPrepareDevice(
    _In_ PDEVICE_OBJECT Pdo)
{
    PPOP_FX_PREPARED_DEVICE Prepared;
    PEP_PREPARE_DEVICE Prepare;
    PPOP_FX_PLUGIN Plugin;
    UNICODE_STRING DeviceId;
    PLIST_ENTRY Entry;

    PAGED_CODE();

    if (!PopFxNextPlugin(NULL))
        return;

    /* A device is prepared once, until its PDO goes away */
    KeAcquireGuardedMutex(&PopFxPluginLock);
    for (Entry = PopFxPreparedList.Flink; Entry != &PopFxPreparedList; Entry = Entry->Flink)
    {
        if (CONTAINING_RECORD(Entry, POP_FX_PREPARED_DEVICE, Link)->Pdo == Pdo)
        {
            KeReleaseGuardedMutex(&PopFxPluginLock);
            return;
        }
    }
    KeReleaseGuardedMutex(&PopFxPluginLock);

    if (!NT_SUCCESS(PopFxQueryDeviceId(Pdo, &DeviceId)))
        return;

    for (Plugin = PopFxNextPlugin(NULL); Plugin; Plugin = PopFxNextPlugin(Plugin))
    {
        Prepare.DeviceId = &DeviceId;
        Prepare.DeviceAccepted = FALSE;
        if (!PopFxNotifyPlugin(Plugin, PEP_DPM_PREPARE_DEVICE, &Prepare) || !Prepare.DeviceAccepted)
            continue;

        Prepared = ExAllocatePoolWithTag(PagedPool, sizeof(*Prepared), TAG_PO_FX_PLUGIN);
        if (!Prepared)
        {
            DPRINT1("No memory to track %wZ, its plug-in keeps it prepared\n", &DeviceId);
            break;
        }

        DPRINT("PEP %p prepared %wZ\n", Plugin, &DeviceId);
        Prepared->Pdo = Pdo;
        Prepared->Plugin = Plugin;
        Prepared->DeviceId = DeviceId;

        KeAcquireGuardedMutex(&PopFxPluginLock);
        InsertTailList(&PopFxPreparedList, &Prepared->Link);
        KeReleaseGuardedMutex(&PopFxPluginLock);
        return;
    }

    RtlFreeUnicodeString(&DeviceId);
}

/**
 * @brief Tells the plug-in that prepared a device that its PDO is gone.
 */
VOID
NTAPI
PopFxAbandonDevice(
    _In_ PDEVICE_OBJECT Pdo)
{
    PPOP_FX_PREPARED_DEVICE Prepared = NULL;
    PEP_ABANDON_DEVICE Abandon;
    PLIST_ENTRY Entry;

    PAGED_CODE();

    KeAcquireGuardedMutex(&PopFxPluginLock);
    for (Entry = PopFxPreparedList.Flink; Entry != &PopFxPreparedList; Entry = Entry->Flink)
    {
        if (CONTAINING_RECORD(Entry, POP_FX_PREPARED_DEVICE, Link)->Pdo == Pdo)
        {
            Prepared = CONTAINING_RECORD(Entry, POP_FX_PREPARED_DEVICE, Link);
            RemoveEntryList(&Prepared->Link);
            break;
        }
    }
    KeReleaseGuardedMutex(&PopFxPluginLock);

    if (!Prepared)
        return;

    Abandon.DeviceId = &Prepared->DeviceId;
    Abandon.DeviceAccepted = FALSE;
    PopFxNotifyPlugin(Prepared->Plugin, PEP_DPM_ABANDON_DEVICE, &Abandon);

    RtlFreeUnicodeString(&Prepared->DeviceId);
    ExFreePoolWithTag(Prepared, TAG_PO_FX_PLUGIN);
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief Registers a power engine plug-in and hands it the kernel services it calls back into.
 *
 * @param[in] PepInformation
 * The plug-in's notification handlers.
 *
 * @param[in] Flags
 * PEP_FLAG_WORKER_CONCURRENCY lets several PEP_DPM_WORK requests run at once.
 *
 * @param[in,out] KernelInformation
 * Version and size on input, the plug-in handle and kernel services on output.
 *
 * @return
 * STATUS_INVALID_KERNEL_INFO_VERSION or STATUS_INVALID_PEP_INFO_VERSION for versions
 * past 3, STATUS_INVALID_PARAMETER for a structure too small or missing its handlers.
 */
NTSTATUS
NTAPI
PoFxRegisterPluginEx(
    _In_ PPEP_INFORMATION PepInformation,
    _In_ ULONGLONG Flags,
    _Inout_ PPEP_KERNEL_INFORMATION KernelInformation)
{
    PPOP_FX_PLUGIN Plugin;
    ULONG Slot;

    PAGED_CODE();

    /* Each kernel information version adds services, so its size has a floor */
    if (KernelInformation->Version > PEP_KERNEL_INFORMATION_V3)
        return STATUS_INVALID_KERNEL_INFO_VERSION;
    if (KernelInformation->Version == 0 ||
        (KernelInformation->Version == PEP_KERNEL_INFORMATION_V3 &&
         KernelInformation->Size < RTL_SIZEOF_THROUGH_FIELD(PEP_KERNEL_INFORMATION, RequestCommon)) ||
        (KernelInformation->Version == PEP_KERNEL_INFORMATION_V2 &&
         KernelInformation->Size < RTL_SIZEOF_THROUGH_FIELD(PEP_KERNEL_INFORMATION, UpdatePlatformIdleState)) ||
        KernelInformation->Size < RTL_SIZEOF_THROUGH_FIELD(PEP_KERNEL_INFORMATION, TransitionCriticalResource))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Version 3 may be an ACPI only plug-in, version 2 must handle device notifications */
    if (PepInformation->Version > PEP_INFORMATION_V3)
        return STATUS_INVALID_PEP_INFO_VERSION;
    if (PepInformation->Version == PEP_INFORMATION_V3)
    {
        if (PepInformation->Size < RTL_SIZEOF_THROUGH_FIELD(PEP_INFORMATION, AcceptAcpiNotification) ||
            (!PepInformation->AcceptDeviceNotification && !PepInformation->AcceptAcpiNotification))
        {
            return STATUS_INVALID_PARAMETER;
        }
    }
    else if (PepInformation->Version != PEP_INFORMATION_V2 ||
             PepInformation->Size < RTL_SIZEOF_THROUGH_FIELD(PEP_INFORMATION, AcceptProcessorNotification) ||
             !PepInformation->AcceptDeviceNotification)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Plugin = ExAllocatePoolZero(NonPagedPool, sizeof(*Plugin), TAG_PO_FX_PLUGIN);
    if (!Plugin)
        return STATUS_INSUFFICIENT_RESOURCES;

    Plugin->Version = PepInformation->Version;
    Plugin->Flags = Flags;
    Plugin->AcceptDeviceNotification = PepInformation->AcceptDeviceNotification;
    Plugin->AcceptProcessorNotification = PepInformation->AcceptProcessorNotification;
    if (PepInformation->Version >= PEP_INFORMATION_V3)
        Plugin->AcceptAcpiNotification = PepInformation->AcceptAcpiNotification;
    for (Slot = 0; Slot < POP_FX_PLUGIN_WORKERS; Slot++)
    {
        Plugin->Workers[Slot].Plugin = Plugin;
        Plugin->Workers[Slot].Slot = (LONG)Slot;
        ExInitializeWorkItem(&Plugin->Workers[Slot].WorkItem, PopFxPluginWorker, &Plugin->Workers[Slot]);
    }

    KeAcquireGuardedMutex(&PopFxPluginLock);
    InsertTailList(&PopFxPluginList, &Plugin->Link);
    KeReleaseGuardedMutex(&PopFxPluginLock);

    KernelInformation->Plugin = (POHANDLE)Plugin;
    KernelInformation->RequestWorker = PopFxRequestWorker;
    KernelInformation->EnumerateUnmaskedInterrupts = PopFxEnumerateUnmaskedInterrupts;
    KernelInformation->ProcessorHalt = PopFxProcessorHalt;
    KernelInformation->RequestInterrupt = PopFxRequestInterrupt;
    KernelInformation->TransitionCriticalResource = PopFxTransitionCriticalResource;
    if (KernelInformation->Version >= PEP_KERNEL_INFORMATION_V2)
    {
        KernelInformation->ProcessorIdleVeto = PopFxProcessorIdleVeto;
        KernelInformation->PlatformIdleVeto = PopFxPlatformIdleVeto;
        KernelInformation->UpdateProcessorIdleState = PopFxUpdateProcessorIdleState;
        KernelInformation->UpdatePlatformIdleState = PopFxUpdatePlatformIdleState;
    }
    if (PepInformation->Version >= PEP_INFORMATION_V3)
        KernelInformation->RequestCommon = PopFxRequestCommon;

    DPRINT1("Registered PEP %p, version %u, flags 0x%I64x\n", Plugin, Plugin->Version, Flags);

    /* Devices enumerated before the plug-in loaded get their prepare now */
    PopFxPrepareExistingDevices();
    return STATUS_SUCCESS;
}

/**
 * @brief Registers a power engine plug-in without flags.
 */
NTSTATUS
NTAPI
PoFxRegisterPlugin(
    _In_ PPEP_INFORMATION PepInformation,
    _Inout_ PPEP_KERNEL_INFORMATION KernelInformation)
{
    return PoFxRegisterPluginEx(PepInformation, 0, KernelInformation);
}

/* EOF */
