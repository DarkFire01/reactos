/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shortcuts for sending different IRP_MJ_PNP requests
 * COPYRIGHT:   Copyright 2010 Sir Richard <sir_richard@svn.reactos.org>
 *              Copyright 2020 Victor Perevertkin <victor.perevertkin@reactos.org>
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* The interrupt mask the debug output reports: EFLAGS on x86, DAIF on ARM64 */
#if defined(_M_ARM64)
#define IopReadInterruptFlags() ((ULONG_PTR)_ReadStatusReg(ARM64_SYSREG(3, 3, 4, 2, 1)))
#else
#define IopReadInterruptFlags() __readeflags()
#endif

/* The user IOSB is not written for an error that was not pended, so the result is taken here */
static
NTSTATUS
NTAPI
IopSynchronousCallCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    *(PIO_STATUS_BLOCK)Context = Irp->IoStatus;
    return STATUS_CONTINUE_COMPLETION;
}

/*
 * PnP watchdog, for bringing up binary drivers without a kernel debugger: every
 * synchronous PnP request is tracked, and one still running after a while is
 * reported with its device and the code addresses found on the waiting thread's
 * stack, as module+offset. One stuck request blocks all device enumeration after it.
 */
#define PNP_WATCH_SLOTS         16
#define PNP_WATCH_REPORT_AFTER  (15LL * 10000000)   /* 15 s */
#define PNP_WATCH_PERIOD        (-5LL * 10000000)   /* 5 s */

typedef struct _PNP_WATCH_SLOT
{
    PETHREAD volatile Thread;
    PDEVICE_OBJECT DeviceObject;
    UCHAR MajorFunction;
    UCHAR MinorFunction;
    BOOLEAN Reported;
    LARGE_INTEGER StartTime;
} PNP_WATCH_SLOT, *PPNP_WATCH_SLOT;

static PNP_WATCH_SLOT PnpWatchSlots[PNP_WATCH_SLOTS];
static LONG PnpWatchStarted;

/* Print the code addresses found between a blocked thread's saved stack pointer and its stack base */
static VOID
PnpWatchScanStack(
    _In_ PKTHREAD Thread)
{
    PULONG_PTR Sp = Thread->KernelStack, Base = Thread->InitialStack;
    PLIST_ENTRY Entry;
    PLDR_DATA_TABLE_ENTRY LdrEntry;
    ULONG Hits = 0;

    if (!Sp || !Base || Sp >= Base || (ULONG_PTR)(Base - Sp) > KERNEL_LARGE_STACK_SIZE / sizeof(ULONG_PTR))
        return;

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&PsLoadedModuleResource, TRUE);
    for (; Sp < Base && Hits < 48; Sp++)
    {
        for (Entry = PsLoadedModuleList.Flink; Entry != &PsLoadedModuleList; Entry = Entry->Flink)
        {
            LdrEntry = CONTAINING_RECORD(Entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
            if (*Sp > (ULONG_PTR)LdrEntry->DllBase + PAGE_SIZE &&
                *Sp < (ULONG_PTR)LdrEntry->DllBase + LdrEntry->SizeOfImage)
            {
                DbgPrint("PNPWATCH:   %wZ+0x%Ix\n", &LdrEntry->BaseDllName, *Sp - (ULONG_PTR)LdrEntry->DllBase);
                Hits++;
                break;
            }
        }
    }
    ExReleaseResourceLite(&PsLoadedModuleResource);
    KeLeaveCriticalRegion();
}

static VOID
NTAPI
PnpWatchThread(
    _In_ PVOID Context)
{
    LARGE_INTEGER Period, Now;
    PPNP_WATCH_SLOT Slot;
    PDEVICE_NODE DeviceNode;
    PETHREAD Thread;
    ULONG i;

    UNREFERENCED_PARAMETER(Context);
    Period.QuadPart = PNP_WATCH_PERIOD;

    for (;;)
    {
        KeDelayExecutionThread(KernelMode, FALSE, &Period);
        /* Not the system time, which boot moves by the time zone bias */
        Now.QuadPart = KeQueryInterruptTime();

        for (i = 0; i < PNP_WATCH_SLOTS; i++)
        {
            Slot = &PnpWatchSlots[i];
            Thread = Slot->Thread;
            if (!Thread || Slot->Reported || Now.QuadPart - Slot->StartTime.QuadPart < PNP_WATCH_REPORT_AFTER)
                continue;

            /* Reported once per request; the slot is reused only after the request returns */
            Slot->Reported = TRUE;
            DeviceNode = IopGetDeviceNode(Slot->DeviceObject);
            DbgPrint("PNPWATCH: IRP %02x/%02x to %wZ still running after %I64d s, thread %p\n",
                     Slot->MajorFunction, Slot->MinorFunction,
                     DeviceNode ? &DeviceNode->InstancePath : NULL,
                     (Now.QuadPart - Slot->StartTime.QuadPart) / 10000000, Thread);
            PnpWatchScanStack(&Thread->Tcb);
        }
    }
}

static PPNP_WATCH_SLOT
PnpWatchBegin(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIO_STACK_LOCATION IoStackLocation)
{
    HANDLE ThreadHandle;
    ULONG i;

    if (!InterlockedCompareExchange(&PnpWatchStarted, 1, 0))
    {
        if (NT_SUCCESS(PsCreateSystemThread(&ThreadHandle, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                            PnpWatchThread, NULL)))
        {
            ZwClose(ThreadHandle);
        }
    }

    for (i = 0; i < PNP_WATCH_SLOTS; i++)
    {
        if (InterlockedCompareExchangePointer((PVOID *)&PnpWatchSlots[i].Thread, PsGetCurrentThread(), NULL) == NULL)
        {
            PnpWatchSlots[i].DeviceObject = DeviceObject;
            PnpWatchSlots[i].MajorFunction = IoStackLocation->MajorFunction;
            PnpWatchSlots[i].MinorFunction = IoStackLocation->MinorFunction;
            PnpWatchSlots[i].Reported = FALSE;
            PnpWatchSlots[i].StartTime.QuadPart = KeQueryInterruptTime();
            return &PnpWatchSlots[i];
        }
    }
    return NULL;
}

static VOID
PnpWatchEnd(
    _In_opt_ PPNP_WATCH_SLOT Slot,
    _In_ NTSTATUS Status)
{
    PDEVICE_NODE DeviceNode;

    if (!Slot)
        return;

    if (Slot->Reported)
    {
        DeviceNode = IopGetDeviceNode(Slot->DeviceObject);
        DbgPrint("PNPWATCH: IRP %02x/%02x to %wZ finished late with 0x%lx\n",
                 Slot->MajorFunction, Slot->MinorFunction,
                 DeviceNode ? &DeviceNode->InstancePath : NULL, Status);
    }
    InterlockedExchangePointer((PVOID *)&Slot->Thread, NULL);
}

NTSTATUS
IopSynchronousCall(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIO_STACK_LOCATION IoStackLocation,
    _Out_ PVOID *Information)
{
    PIRP Irp;
    PIO_STACK_LOCATION IrpStack;
    IO_STATUS_BLOCK IoStatusBlock;
    IO_STATUS_BLOCK Result;
    KEVENT Event;
    NTSTATUS Status;
    PDEVICE_OBJECT TopDeviceObject;
    PPNP_WATCH_SLOT WatchSlot;
    PAGED_CODE();

    WatchSlot = PnpWatchBegin(DeviceObject, IoStackLocation);

    /* Call the top of the device stack */
    TopDeviceObject = IoGetAttachedDeviceReference(DeviceObject);

    /* Allocate an IRP */
    Irp = IoAllocateIrp(TopDeviceObject->StackSize, FALSE);
    if (!Irp)
    {
        ObDereferenceObject(TopDeviceObject);
        PnpWatchEnd(WatchSlot, STATUS_INSUFFICIENT_RESOURCES);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Initialize to failure */
    Irp->IoStatus.Status = IoStatusBlock.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = IoStatusBlock.Information = 0;

    /* Special case for IRP_MN_FILTER_RESOURCE_REQUIREMENTS */
    if ((IoStackLocation->MajorFunction == IRP_MJ_PNP) &&
        (IoStackLocation->MinorFunction == IRP_MN_FILTER_RESOURCE_REQUIREMENTS))
    {
        /* Copy the resource requirements list into the IOSB */
        Irp->IoStatus.Information =
        IoStatusBlock.Information = (ULONG_PTR)IoStackLocation->Parameters.FilterResourceRequirements.IoResourceRequirementList;
    }

    Result = IoStatusBlock;

    /* Initialize the event */
    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

    /* Set them up */
    Irp->UserIosb = &IoStatusBlock;
    Irp->UserEvent = &Event;

    /* Queue the IRP */
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    IoQueueThreadIrp(Irp);

    /* Copy-in the stack */
    IrpStack = IoGetNextIrpStackLocation(Irp);
    *IrpStack = *IoStackLocation;
    IoSetCompletionRoutine(Irp, IopSynchronousCallCompletion, &Result, TRUE, TRUE, TRUE);

    /* Call the driver */
    Status = IoCallDriver(TopDeviceObject, Irp);
    /* Otherwise we may get stuck here or have IoStatusBlock not populated */
    ASSERT(!KeAreAllApcsDisabled());
#if DBG
    /*
     * A request completed in this thread has run its completion APC by now. Other kernel APCs
     * can stay queued while the thread is in a critical region, as file systems are, so only
     * this IRP's own APC counts. Its address is compared, the IRP may already be freed.
     */
    if (Status != STATUS_PENDING)
    {
        PLIST_ENTRY ApcListHead = &KeGetCurrentThread()->ApcState.ApcListHead[KernelMode];
        PLIST_ENTRY ApcEntry;

        for (ApcEntry = ApcListHead->Flink; ApcEntry != ApcListHead; ApcEntry = ApcEntry->Flink)
        {
            if (ApcEntry != &Irp->Tail.Apc.ApcListEntry)
                continue;

            DPRINT1("Completion APC of IRP %p still queued: Irql %u Pending %u KernelApcDisable %d SpecialApcDisable %d Flags 0x%Ix\n",
                    Irp,
                    KeGetCurrentIrql(),
                    KeGetCurrentThread()->ApcState.KernelApcPending,
                    KeGetCurrentThread()->KernelApcDisable,
                    KeGetCurrentThread()->SpecialApcDisable,
                    IopReadInterruptFlags());
            DbgBreakPoint();
            break;
        }
    }
#endif
    if (Status == STATUS_PENDING)
    {
        /* Wait for it */
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }

    Status = Result.Status;

    /* Remove the reference */
    ObDereferenceObject(TopDeviceObject);
    PnpWatchEnd(WatchSlot, Status);

    /* Return the information */
    *Information = (PVOID)Result.Information;
    return Status;
}

// IRP_MN_START_DEVICE (0x00)
NTSTATUS
PiIrpStartDevice(
    _In_ PDEVICE_NODE DeviceNode)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeResourcesAssigned);

    /* A device added before its plug-in registered gets the offer here */
    PopFxPrepareDevice(DeviceNode->PhysicalDeviceObject, TRUE);

    PVOID info;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_START_DEVICE,
        .Parameters.StartDevice.AllocatedResources = DeviceNode->ResourceList,
        .Parameters.StartDevice.AllocatedResourcesTranslated = DeviceNode->ResourceListTranslated
    };

    // Vista+ does an asynchronous call
    NTSTATUS status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, &info);
    DeviceNode->CompletionStatus = status;
    DPRINT1("Start %wZ -> 0x%lx\n", &DeviceNode->InstancePath, status);
    return status;
}

// IRP_MN_STOP_DEVICE (0x04)
NTSTATUS
PiIrpStopDevice(
    _In_ PDEVICE_NODE DeviceNode)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeQueryStopped);

    PVOID info;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_STOP_DEVICE
    };

    DPRINT1("Stopping %wZ\n", &DeviceNode->InstancePath);

    // Drivers should never fail a IRP_MN_STOP_DEVICE request
    NTSTATUS status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, &info);
    ASSERT(NT_SUCCESS(status));
    return status;
}

// IRP_MN_QUERY_STOP_DEVICE (0x05)
NTSTATUS
PiIrpQueryStopDevice(
    _In_ PDEVICE_NODE DeviceNode)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeStarted);

    PVOID info;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_STOP_DEVICE
    };

    NTSTATUS status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, &info);
    DeviceNode->CompletionStatus = status;
    return status;
}

// IRP_MN_CANCEL_STOP_DEVICE (0x06)
NTSTATUS
PiIrpCancelStopDevice(
    _In_ PDEVICE_NODE DeviceNode)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeQueryStopped);

    PVOID info;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_CANCEL_STOP_DEVICE
    };

    // in fact we don't care which status is returned here
    NTSTATUS status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, &info);
    ASSERT(NT_SUCCESS(status));
    return status;
}

// IRP_MN_QUERY_DEVICE_RELATIONS (0x07)
NTSTATUS
PiIrpQueryDeviceRelations(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ DEVICE_RELATION_TYPE Type)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeStarted);

    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_DEVICE_RELATIONS,
        .Parameters.QueryDeviceRelations.Type = Type
    };

    // Vista+ does an asynchronous call
    NTSTATUS status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject,
                                         &stack,
                                         (PVOID)&DeviceNode->OverUsed1.PendingDeviceRelations);
    DeviceNode->CompletionStatus = status;
    return status;
}

// IRP_MN_QUERY_RESOURCES (0x0A)
NTSTATUS
PiIrpQueryResources(
    _In_ PDEVICE_NODE DeviceNode,
    _Out_ PCM_RESOURCE_LIST *Resources)
{
    PAGED_CODE();

    ASSERT(DeviceNode);

    ULONG_PTR longRes;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_RESOURCES
    };

    NTSTATUS status;
    status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, (PVOID)&longRes);
    if (NT_SUCCESS(status))
    {
        *Resources = (PVOID)longRes;
    }

    return status;
}

// IRP_MN_QUERY_RESOURCE_REQUIREMENTS (0x0B)
NTSTATUS
PiIrpQueryResourceRequirements(
    _In_ PDEVICE_NODE DeviceNode,
    _Out_ PIO_RESOURCE_REQUIREMENTS_LIST *Resources)
{
    PAGED_CODE();

    ASSERT(DeviceNode);

    ULONG_PTR longRes;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_RESOURCE_REQUIREMENTS
    };

    NTSTATUS status;
    status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, (PVOID)&longRes);
    if (NT_SUCCESS(status))
    {
        *Resources = (PVOID)longRes;
    }

    return status;
}

// IRP_MN_QUERY_DEVICE_TEXT (0x0C)
NTSTATUS
PiIrpQueryDeviceText(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ LCID LocaleId,
    _In_ DEVICE_TEXT_TYPE Type,
    _Out_ PWSTR *DeviceText)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeUninitialized);

    ULONG_PTR longText;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_DEVICE_TEXT,
        .Parameters.QueryDeviceText.DeviceTextType = Type,
        .Parameters.QueryDeviceText.LocaleId = LocaleId
    };

    NTSTATUS status;
    status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, (PVOID)&longText);
    if (NT_SUCCESS(status))
    {
        *DeviceText = (PVOID)longText;
    }

    return status;
}

// IRP_MN_QUERY_PNP_DEVICE_STATE (0x14)
NTSTATUS
PiIrpQueryPnPDeviceState(
    _In_ PDEVICE_NODE DeviceNode,
    _Out_ PPNP_DEVICE_STATE DeviceState)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeStartPostWork ||
           DeviceNode->State == DeviceNodeStarted);

    ULONG_PTR longState;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_PNP_DEVICE_STATE
    };

    NTSTATUS status;
    status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, (PVOID)&longState);
    if (NT_SUCCESS(status))
    {
        *DeviceState = longState;
    }

    return status;
}
