/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/po/poshtdwn.c
 * PURPOSE:         Power Manager Shutdown Code
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#ifdef NEWCC
#include <cache/newcc.h>
#endif

#define NDEBUG
#include <debug.h>

#include "inbv/logo.h"

/* GLOBALS *******************************************************************/

ULONG PopShutdownPowerOffPolicy;
KEVENT PopShutdownEvent;
PPOP_SHUTDOWN_WAIT_ENTRY PopShutdownThreadList;
LIST_ENTRY PopShutdownQueue;
KGUARDED_MUTEX PopShutdownListMutex;
BOOLEAN PopShutdownListAvailable;


/* PRIVATE FUNCTIONS *********************************************************/

VOID
NTAPI
PopInitShutdownList(VOID)
{
    PAGED_CODE();

    /* Initialize the global shutdown event */
    KeInitializeEvent(&PopShutdownEvent, NotificationEvent, FALSE);

    /* Initialize the shutdown lists */
    PopShutdownThreadList = NULL;
    InitializeListHead(&PopShutdownQueue);

    /* Initialize the shutdown list lock */
    KeInitializeGuardedMutex(&PopShutdownListMutex);

    /* The list is available now */
    PopShutdownListAvailable = TRUE;
}

NTSTATUS
NTAPI
PoRequestShutdownWait(
    _In_ PETHREAD Thread)
{
    PPOP_SHUTDOWN_WAIT_ENTRY ShutDownWaitEntry;
    NTSTATUS Status;
    PAGED_CODE();

    /* Allocate a new shutdown wait entry */
    ShutDownWaitEntry = ExAllocatePoolWithTag(PagedPool, sizeof(*ShutDownWaitEntry), 'LSoP');
    if (ShutDownWaitEntry == NULL)
    {
        return STATUS_NO_MEMORY;
    }

    /* Reference the thread and save it in the wait entry */
    ObReferenceObject(Thread);
    ShutDownWaitEntry->Thread = Thread;

    /* Acquire the shutdown list lock */
    KeAcquireGuardedMutex(&PopShutdownListMutex);

    /* Check if the list is still available */
    if (PopShutdownListAvailable)
    {
        /* Insert the item in the list */
        ShutDownWaitEntry->NextEntry = PopShutdownThreadList;
        PopShutdownThreadList = ShutDownWaitEntry;

        /* We are successful */
        Status = STATUS_SUCCESS;
    }
    else
    {
        /* We cannot proceed, cleanup and return failure */
        ObDereferenceObject(Thread);
        ExFreePoolWithTag(ShutDownWaitEntry, 'LSoP');
        Status = STATUS_UNSUCCESSFUL;
    }

    /* Release the list lock */
    KeReleaseGuardedMutex(&PopShutdownListMutex);

    /* Return the status */
    return Status;
}

VOID
NTAPI
PopProcessShutDownLists(VOID)
{
    PPOP_SHUTDOWN_WAIT_ENTRY ShutDownWaitEntry;
    PWORK_QUEUE_ITEM WorkItem;
    PLIST_ENTRY ListEntry;

    /* First signal the shutdown event */
    KeSetEvent(&PopShutdownEvent, IO_NO_INCREMENT, FALSE);

    /* Acquire the shutdown list lock */
    KeAcquireGuardedMutex(&PopShutdownListMutex);

    /* Block any further attempts to register a shutdown event */
    PopShutdownListAvailable = FALSE;

    /* Release the list lock, since we are exclusively using the lists now */
    KeReleaseGuardedMutex(&PopShutdownListMutex);

    /* Process the shutdown queue */
    while (!IsListEmpty(&PopShutdownQueue))
    {
        /* Get the head entry */
        ListEntry = RemoveHeadList(&PopShutdownQueue);
        WorkItem = CONTAINING_RECORD(ListEntry, WORK_QUEUE_ITEM, List);

        /* Call the shutdown worker routine */
        WorkItem->WorkerRoutine(WorkItem->Parameter);
    }

    /* Now process the shutdown thread list */
    while (PopShutdownThreadList != NULL)
    {
        /* Get the top entry and remove it from the list */
        ShutDownWaitEntry = PopShutdownThreadList;
        PopShutdownThreadList = PopShutdownThreadList->NextEntry;

        /* Wait for the thread to finish and dereference it */
        KeWaitForSingleObject(ShutDownWaitEntry->Thread, 0, 0, 0, 0);
        ObDereferenceObject(ShutDownWaitEntry->Thread);

        /* Finally free the entry */
        ExFreePoolWithTag(ShutDownWaitEntry, 'LSoP');
    }
}

/**
 * @brief
 * The soft off fallback when no HAL handler powers the machine off. Every
 * processor runs it, only the boot processor draws the halt screen.
 */
NTSTATUS
NTAPI
PopShutdownHandler(
    _In_opt_ PVOID Context,
    _In_opt_ PENTER_STATE_SYSTEM_HANDLER SystemHandler,
    _In_opt_ PVOID SystemContext,
    _In_ LONG NumberProcessors,
    _In_opt_ LONG volatile *Number)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(SystemHandler);
    UNREFERENCED_PARAMETER(SystemContext);
    UNREFERENCED_PARAMETER(NumberProcessors);
    UNREFERENCED_PARAMETER(Number);

    /* Stop all interrupts */
    KeRaiseIrqlToDpcLevel();
    _disable();

    if (KeGetCurrentProcessorNumber() != 0)
    {
        for (;;) HalHaltSystem();
    }

    /* Do we have boot video */
    if (InbvIsBootDriverInstalled())
    {
        /* Yes we do, cleanup for shutdown screen */
        if (!InbvCheckDisplayOwnership()) InbvAcquireDisplayOwnership();
        InbvResetDisplay();
        // InbvEnableDisplayString(TRUE);
        DisplayShutdownBitmap();
    }
    else
    {
        /* Do it in text-mode */
        DisplayShutdownText();
    }

    /* Hang the system */
    for (;;) HalHaltSystem();
}

/* Gathers every processor at DISPATCH_LEVEL so the state handler runs on all of them */
typedef struct _POP_STATE_HANDLER_GATHER
{
    PPOWER_STATE_HANDLER Handler;
    LONG NumberProcessors;
    LONG volatile Joined;
    LONG volatile Release;
    LONG volatile Finished;
    LONG volatile HandlerNumber;
} POP_STATE_HANDLER_GATHER, *PPOP_STATE_HANDLER_GATHER;

/* Only ever used by the one thread that shuts the system down */
static KDPC PopStateHandlerDpcs[MAXIMUM_PROCESSORS];

static
NTSTATUS
NTAPI
PopCallStateHandler(
    _In_ PPOP_STATE_HANDLER_GATHER Gather)
{
    return Gather->Handler->Handler(Gather->Handler->Context,
                                    NULL,
                                    NULL,
                                    Gather->NumberProcessors,
                                    &Gather->HandlerNumber);
}

static KDEFERRED_ROUTINE PopStateHandlerSecondaryDpc;

static
VOID
NTAPI
PopStateHandlerSecondaryDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PPOP_STATE_HANDLER_GATHER Gather = DeferredContext;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    InterlockedIncrement(&Gather->Joined);

    while (!Gather->Release)
        YieldProcessor();

    PopCallStateHandler(Gather);
    InterlockedIncrement(&Gather->Finished);
}

/**
 * @brief
 * Runs a registered power state handler on every processor, the boot
 * processor last. A shutdown handler that works does not return.
 *
 * @return
 * The status of the handler on the boot processor, or
 * STATUS_DEVICE_DOES_NOT_EXIST when nothing is registered for Type.
 */
NTSTATUS
NTAPI
PopInvokeSystemStateHandler(
    _In_ POWER_STATE_HANDLER_TYPE Type)
{
    POP_STATE_HANDLER_GATHER Gather;
    NTSTATUS Status;
    KIRQL OldIrql;
    ULONG Index;
    LONG Queued = 0;

    if (((ULONG)Type >= PowerStateMaximum) || (PopPowerStateHandlers[Type].Handler == NULL))
        return STATUS_DEVICE_DOES_NOT_EXIST;

    RtlZeroMemory(&Gather, sizeof(Gather));
    Gather.Handler = &PopPowerStateHandlers[Type];
    Gather.NumberProcessors = KeNumberProcessors;

    KeSetSystemAffinityThread(1);
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    for (Index = 1; Index < (ULONG)KeNumberProcessors; Index++)
    {
        if (!(KeActiveProcessors & AFFINITY_MASK(Index)))
            continue;

        KeInitializeDpc(&PopStateHandlerDpcs[Index], PopStateHandlerSecondaryDpc, &Gather);
        KeSetImportanceDpc(&PopStateHandlerDpcs[Index], HighImportance);
        KeSetTargetProcessorDpc(&PopStateHandlerDpcs[Index], (CCHAR)Index);
        KeInsertQueueDpc(&PopStateHandlerDpcs[Index], NULL, NULL);
        Queued++;
    }

    /* Nobody enters the handler until every processor is spinning in its DPC */
    while (Gather.Joined != Queued)
        YieldProcessor();

    InterlockedExchange(&Gather.Release, 1);
    Status = PopCallStateHandler(&Gather);

    while (Gather.Finished != Queued)
        YieldProcessor();

    KeLowerIrql(OldIrql);
    KeRevertToUserAffinityThread();
    return Status;
}

VOID
NTAPI
PopShutdownSystem(IN POWER_ACTION SystemAction)
{
    /* Note should notify caller of NtPowerInformation(PowerShutdownNotification) */

    /* Unload symbols */
    DPRINT("It's the final countdown...%lx\n", SystemAction);
    DbgUnLoadImageSymbols(NULL, (PVOID)-1, 0);

    /* FIXME: the devices should be in S5 before this point, as on Windows */
    PopSetSystemPowerState(PowerSystemShutdown, SystemAction);

    /* Now check what the caller wants */
    switch (SystemAction)
    {
        case PowerActionShutdownReset:
            PopInvokeSystemStateHandler(PowerStateShutdownReset);
            break;

        case PowerActionShutdown:
            /* The policy asks for the "it is now safe" screen instead of powering off */
            if (PopShutdownPowerOffPolicy)
                PopPowerStateHandlers[PowerStateShutdownOff].Handler = PopShutdownHandler;

            /* Fall through */
        case PowerActionShutdownOff:
            PopInvokeSystemStateHandler(PowerStateShutdownOff);
            HalReturnToFirmware(HalPowerDownRoutine);
            break;

        default:
            break;
    }

    /* A reset, or a power off the handler could not do */
    HalReturnToFirmware(HalRebootRoutine);

    /* Anything else should not happen */
    KeBugCheckEx(INTERNAL_POWER_ERROR, 5, 0, 0, 0);
}

VOID
NTAPI
PopGracefulShutdown(IN PVOID Context)
{
    PEPROCESS Process = NULL;

    /* Process the registered waits and work items */
    PopProcessShutDownLists();

    /* Loop every process */
    Process = PsGetNextProcess(Process);
    while (Process)
    {
        /* Make sure this isn't the idle or initial process */
        if ((Process != PsInitialSystemProcess) && (Process != PsIdleProcess))
        {
            /* Print it */
            DPRINT1("%15s is still RUNNING (%p)\n", Process->ImageFileName, Process->UniqueProcessId);
        }

        /* Get the next process */
        Process = PsGetNextProcess(Process);
    }

    /* First, the HAL handles any "end of boot" special functionality */
    DPRINT("HAL shutting down\n");
    HalEndOfBoot();

    /* Shut down the Shim cache if enabled */
    ApphelpCacheShutdown();

    /* In this step, the I/O manager does first-chance shutdown notification */
    DPRINT("I/O manager shutting down in phase 0\n");
    IoShutdownSystem(0);

    /* In this step, all workers are killed and hives are flushed */
    DPRINT("Configuration Manager shutting down\n");
    CmShutdownSystem();

    /* Shut down the Executive */
    DPRINT("Executive shutting down\n");
    ExShutdownSystem();

    /* Note that modified pages should be written here (MiShutdownSystem) */
    MmShutdownSystem(0);

    /* Flush all user files before we start shutting down IO */
    /* This is where modified pages are written back by the IO manager */
    CcShutdownSystem();

    /* In this step, the I/O manager does last-chance shutdown notification */
    DPRINT("I/O manager shutting down in phase 1\n");
    IoShutdownSystem(1);
    CcWaitForCurrentLazyWriterActivity();

    /* FIXME: Calling Mm shutdown phase 1 here to get page file dereference
     * but it shouldn't be called here. Only phase 2 should be called.
     */
    MmShutdownSystem(1);

    /* Note that here, we should broadcast the power IRP to devices */

    /* In this step, the HAL disables any wake timers */
    DPRINT("Disabling wake timers\n");
    HalSetWakeEnable(FALSE);

    /* And finally the power request is sent */
    DPRINT("Taking the system down\n");
    PopShutdownSystem(PopAction.Action);
}

VOID
NTAPI
PopReadShutdownPolicy(VOID)
{
    UNICODE_STRING KeyString;
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;
    HANDLE KeyHandle;
    ULONG Length;
    UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PVOID)Buffer;

    /* Setup object attributes */
    RtlInitUnicodeString(&KeyString,
                         L"\\Registry\\Machine\\Software\\Policies\\Microsoft\\Windows NT");
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyString,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    /* Open the key */
    Status = ZwOpenKey(&KeyHandle, KEY_READ, &ObjectAttributes);
    if (NT_SUCCESS(Status))
    {
        /* Open the policy value and query it */
        RtlInitUnicodeString(&KeyString, L"DontPowerOffAfterShutdown");
        Status = ZwQueryValueKey(KeyHandle,
                                 &KeyString,
                                 KeyValuePartialInformation,
                                 &Info,
                                 sizeof(Info),
                                 &Length);
        if ((NT_SUCCESS(Status)) && (Info->Type == REG_DWORD))
        {
            /* Read the policy */
            PopShutdownPowerOffPolicy = *Info->Data == 1;
        }

        /* Close the key */
        ZwClose(KeyHandle);
    }
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
PoQueueShutdownWorkItem(
    _In_ PWORK_QUEUE_ITEM WorkItem)
{
    NTSTATUS Status;

    /* Acquire the shutdown list lock */
    KeAcquireGuardedMutex(&PopShutdownListMutex);

    /* Check if the list is (already/still) available */
    if (PopShutdownListAvailable)
    {
        /* Insert the item into the list */
        InsertTailList(&PopShutdownQueue, &WorkItem->List);
        Status = STATUS_SUCCESS;
    }
    else
    {
        /* We are already in shutdown */
        Status = STATUS_SYSTEM_SHUTDOWN;
    }

    /* Release the list lock */
    KeReleaseGuardedMutex(&PopShutdownListMutex);

    return Status;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
PoRequestShutdownEvent(OUT PVOID *Event)
{
    NTSTATUS Status;
    PAGED_CODE();

    /* Initialize to NULL */
    if (Event) *Event = NULL;

    /* Request a shutdown wait */
    Status = PoRequestShutdownWait(PsGetCurrentThread());
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /* Return the global shutdown event */
    if (Event) *Event = &PopShutdownEvent;
    return STATUS_SUCCESS;
}

