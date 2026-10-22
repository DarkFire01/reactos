/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Worker factory objects, which run a user mode thread pool on an I/O completion port
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* DEFINES ******************************************************************/

#define TAG_WORKER_FACTORY                  'FkrW'

/* How long a spare worker idles before it is let go, and the range a caller may pick */
#define WORKER_FACTORY_IDLE_SECONDS         67
#define WORKER_FACTORY_MIN_IDLE_SECONDS     1
#define WORKER_FACTORY_MAX_IDLE_SECONDS     600
#define WORKER_FACTORY_SECOND               (-10000000LL)

#define WORKER_FACTORY_STACK_RESERVE        0x10000
#define WORKER_FACTORY_STACK_COMMIT         0x1000

/* Largest packet array NtWaitForWorkViaWorkerFactory takes */
#define WORKER_FACTORY_MAX_PACKETS          0x0FFFFFFF

/* GENERIC_ALL also grants two rights that have no public name */
#define WORKER_FACTORY_GENERIC_ALL          (STANDARD_RIGHTS_REQUIRED | 0xFF)

/* Deferred work flag asking for an ALPC message to be sent before the wait */
#define WORKER_FACTORY_DEFER_ALPC_SEND      0x1

/* The only bit WorkerFactoryFlags keeps */
#define WORKER_FACTORY_VALID_FLAGS          0x1

/* TYPES ********************************************************************/

typedef enum _WORKER_FACTORY_WATCH_STATE
{
    WatchIdle,
    WatchArmed,
    WatchFired
} WORKER_FACTORY_WATCH_STATE;

/* State the signal packet needs, which outlives the factory while the packet sits in the port */
typedef struct _WORKER_FACTORY_SIGNAL
{
    KSPIN_LOCK Lock;
    PKQUEUE Port;
    IOP_CALLBACK_COMPLETION_PACKET Packet;
    ULONG ReleaseCount;
    ULONG WaitingWorkerCount;
    BOOLEAN PacketQueued;
    BOOLEAN Shutdown;
    BOOLEAN Deleted;
} WORKER_FACTORY_SIGNAL, *PWORKER_FACTORY_SIGNAL;

/* Counts and settings are guarded by Signal->Lock */
typedef struct _WORKER_FACTORY
{
    PWORKER_FACTORY_SIGNAL Signal;
    PEPROCESS Process;
    HANDLE ProcessHandle;
    PTHREAD_START_ROUTINE StartRoutine;
    PVOID StartParameter;
    SIZE_T StackReserve;
    SIZE_T StackCommit;
    LARGE_INTEGER IdleTimeout;
    ULONG ThreadMinimum;
    ULONG ThreadMaximum;
    ULONG ThreadSoftMaximum;
    ULONG ReadyWorkerCount;
    ULONG TotalWorkerCount;
    ULONG PendingWorkerCount;
    ULONG CreateInProgress;
    ULONG BindingCount;
    ULONG Flags;
    KPRIORITY BasePriority;
    NTSTATUS LastThreadCreationStatus;
    EX_RUNDOWN_REF CreationRundown;
    IOP_COMPLETION_WATCH PortWatch;
    WORK_QUEUE_ITEM WatchWorkItem;
    WORKER_FACTORY_WATCH_STATE WatchState;
} WORKER_FACTORY, *PWORKER_FACTORY;

typedef struct _WORKER_FACTORY_STACK_SIZES
{
    SIZE_T StackReserve;
    SIZE_T StackCommit;
} WORKER_FACTORY_STACK_SIZES;

typedef union _WORKER_FACTORY_SET_BUFFER
{
    ULONG Value;
    LARGE_INTEGER Timeout;
    WORKER_FACTORY_STACK_SIZES Stack;
} WORKER_FACTORY_SET_BUFFER;

/* GLOBALS ******************************************************************/

static POBJECT_TYPE ExpWorkerFactoryObjectType;

static GENERIC_MAPPING ExpWorkerFactoryMapping =
{
    STANDARD_RIGHTS_READ | WORKER_FACTORY_QUERY_INFORMATION,
    STANDARD_RIGHTS_WRITE | WORKER_FACTORY_SET_INFORMATION,
    STANDARD_RIGHTS_EXECUTE | WORKER_FACTORY_RELEASE_WORKER | WORKER_FACTORY_WAIT,
    WORKER_FACTORY_GENERIC_ALL
};

/* PRIVATE FUNCTIONS ********************************************************/

static
VOID
ExpWorkerFactoryQueueSignal(
    _In_ PWORKER_FACTORY_SIGNAL Signal)
{
    KeInsertQueue(Signal->Port, &Signal->Packet.Packet.ListEntry);
}

/**
 * @brief
 * Called once the signal packet leaves the port. A taker gets one empty completion per release,
 * and after shutdown the packet keeps going round until no worker is left waiting.
 */
static
VOID
NTAPI
ExpWorkerFactorySignalReturned(
    _In_ PVOID Context,
    _In_ BOOLEAN Rundown)
{
    PWORKER_FACTORY_SIGNAL Signal = Context;
    KLOCK_QUEUE_HANDLE LockHandle;
    BOOLEAN Requeue = FALSE;
    BOOLEAN Free = FALSE;

    KeAcquireInStackQueuedSpinLock(&Signal->Lock, &LockHandle);
    if (Signal->Deleted)
    {
        Free = TRUE;
    }
    else if (!Rundown)
    {
        if (Signal->Shutdown)
        {
            Requeue = (Signal->WaitingWorkerCount != 0);
        }
        else if (Signal->ReleaseCount)
        {
            Signal->ReleaseCount--;
            Requeue = (Signal->ReleaseCount != 0);
        }
    }

    if (!Free && !Requeue)
        Signal->PacketQueued = FALSE;
    KeReleaseInStackQueuedSpinLock(&LockHandle);

    if (Requeue)
        ExpWorkerFactoryQueueSignal(Signal);

    if (Free)
        ExFreePoolWithTag(Signal, TAG_WORKER_FACTORY);
}

/* Whether there is work for a new thread and no worker free to take it. Lock held. */
static
BOOLEAN
ExpWorkerFactoryWantsThread(
    _In_ PWORKER_FACTORY Factory,
    _In_ BOOLEAN CallerIsWaiting)
{
    PWORKER_FACTORY_SIGNAL Signal = Factory->Signal;

    if (Factory->CreateInProgress)
        return FALSE;

    /* A waiting caller may be one of the ready workers itself */
    if ((Factory->ReadyWorkerCount > Factory->ThreadMaximum) ||
        ((Factory->ReadyWorkerCount == Factory->ThreadMaximum) && !CallerIsWaiting))
        return FALSE;

    if (Signal->WaitingWorkerCount > (CallerIsWaiting ? 1UL : 0UL))
        return FALSE;

    return (KeReadStateQueue(Signal->Port) > 0) || (Signal->ReleaseCount != 0);
}

/* Starts one worker in the target process. Called without the lock at PASSIVE_LEVEL. */
static
NTSTATUS
ExpWorkerFactoryCreateThread(
    _In_ PWORKER_FACTORY Factory)
{
    KLOCK_QUEUE_HANDLE LockHandle;
    CLIENT_ID ClientId;
    HANDLE ThreadHandle;
    NTSTATUS Status;

    KeEnterCriticalRegion();
    if (!ExAcquireRundownProtection(&Factory->CreationRundown))
    {
        KeLeaveCriticalRegion();
        return STATUS_ABANDONED;
    }

    KeAcquireInStackQueuedSpinLock(&Factory->Signal->Lock, &LockHandle);
    if (Factory->Signal->Shutdown)
    {
        KeReleaseInStackQueuedSpinLock(&LockHandle);
        ExReleaseRundownProtection(&Factory->CreationRundown);
        KeLeaveCriticalRegion();
        return STATUS_ABANDONED;
    }
    Factory->PendingWorkerCount++;
    KeReleaseInStackQueuedSpinLock(&LockHandle);

    Status = RtlCreateUserThread(Factory->ProcessHandle,
                                 NULL,
                                 FALSE,
                                 0,
                                 Factory->StackReserve,
                                 Factory->StackCommit,
                                 Factory->StartRoutine,
                                 Factory->StartParameter,
                                 &ThreadHandle,
                                 &ClientId);
    Factory->LastThreadCreationStatus = Status;
    if (NT_SUCCESS(Status))
    {
        if (Factory->BasePriority)
            ZwSetInformationThread(ThreadHandle,
                                   ThreadBasePriority,
                                   &Factory->BasePriority,
                                   sizeof(Factory->BasePriority));
        ObCloseHandle(ThreadHandle, KernelMode);
    }
    else
    {
        KeAcquireInStackQueuedSpinLock(&Factory->Signal->Lock, &LockHandle);
        Factory->PendingWorkerCount--;
        KeReleaseInStackQueuedSpinLock(&LockHandle);
    }

    ExReleaseRundownProtection(&Factory->CreationRundown);
    KeLeaveCriticalRegion();
    return Status;
}

/* Stops watching the port for unclaimed packets. Lock held. */
static
VOID
ExpWorkerFactoryStopWatching(
    _In_ PWORKER_FACTORY Factory)
{
    if ((Factory->WatchState == WatchArmed) && IopDisarmCompletionWatch(&Factory->PortWatch))
    {
        Factory->WatchState = WatchIdle;
        ObDereferenceObjectDeferDelete(Factory);
    }
}

/**
 * @brief
 * With no worker waiting, packets posted straight to the port by bound objects would sit there
 * unseen, so the port is watched for them. Lock held.
 *
 * @return
 * TRUE when the caller should check for a new thread right away.
 */
static
BOOLEAN
ExpWorkerFactoryWatchPort(
    _In_ PWORKER_FACTORY Factory)
{
    if (!Factory->BindingCount)
        return TRUE;

    if (Factory->WatchState != WatchIdle)
        return FALSE;

    /* The watch holds a reference until it is disarmed or its work item is done */
    ObReferenceObject(Factory);
    Factory->WatchState = WatchArmed;
    if (IopArmCompletionWatch(&Factory->PortWatch))
        return FALSE;

    Factory->WatchState = WatchIdle;
    ObDereferenceObjectDeferDelete(Factory);
    return TRUE;
}

/* Creates a worker if the factory wants one. Entered with the lock held, returns with it released. */
static
VOID
ExpWorkerFactoryCheckCreate(
    _In_ PWORKER_FACTORY Factory,
    _Inout_ PKLOCK_QUEUE_HANDLE LockHandle)
{
    NTSTATUS Status;

    if (!ExpWorkerFactoryWantsThread(Factory, FALSE) ||
        (Factory->ThreadSoftMaximum && (Factory->ReadyWorkerCount > Factory->ThreadSoftMaximum)))
    {
        KeReleaseInStackQueuedSpinLock(LockHandle);
        return;
    }

    ExpWorkerFactoryStopWatching(Factory);
    Factory->CreateInProgress++;
    KeReleaseInStackQueuedSpinLock(LockHandle);

    Status = ExpWorkerFactoryCreateThread(Factory);
    if (!NT_SUCCESS(Status))
    {
        KeAcquireInStackQueuedSpinLock(&Factory->Signal->Lock, LockHandle);
        Factory->CreateInProgress--;
        KeReleaseInStackQueuedSpinLock(LockHandle);
    }
}

/**
 * @brief
 * Creates one worker on request, unless one is already on its way. Entered with the lock held,
 * returns with it released. Quiet requests do not report a full pool or a creation failure.
 */
static
NTSTATUS
ExpWorkerFactoryCreateOne(
    _In_ PWORKER_FACTORY Factory,
    _Inout_ PKLOCK_QUEUE_HANDLE LockHandle,
    _In_ BOOLEAN Quiet)
{
    NTSTATUS Status = STATUS_SUCCESS;

    if (Factory->Signal->Shutdown)
    {
        Status = STATUS_ABANDONED;
    }
    else if (Factory->ReadyWorkerCount >= Factory->ThreadMaximum)
    {
        if (!Quiet)
            Status = STATUS_TOO_MANY_THREADS;
    }
    else if (!Factory->CreateInProgress &&
             (Factory->ReadyWorkerCount + Factory->PendingWorkerCount < Factory->ThreadMaximum))
    {
        ExpWorkerFactoryStopWatching(Factory);
        Factory->CreateInProgress++;
        KeReleaseInStackQueuedSpinLock(LockHandle);

        Status = ExpWorkerFactoryCreateThread(Factory);
        if (NT_SUCCESS(Status))
            return Status;

        KeAcquireInStackQueuedSpinLock(&Factory->Signal->Lock, LockHandle);
        Factory->CreateInProgress--;
        if (Quiet)
            Status = STATUS_SUCCESS;
    }

    KeReleaseInStackQueuedSpinLock(LockHandle);
    return Status;
}

/* Creates workers until the minimum is met or creation fails. Lock held on entry and return. */
static
NTSTATUS
ExpWorkerFactoryFillMinimum(
    _In_ PWORKER_FACTORY Factory,
    _Inout_ PKLOCK_QUEUE_HANDLE LockHandle)
{
    NTSTATUS Status = STATUS_SUCCESS;

    ExpWorkerFactoryStopWatching(Factory);
    while (Factory->ReadyWorkerCount + Factory->PendingWorkerCount < Factory->ThreadMinimum)
    {
        Factory->CreateInProgress++;
        KeReleaseInStackQueuedSpinLock(LockHandle);
        Status = ExpWorkerFactoryCreateThread(Factory);
        KeAcquireInStackQueuedSpinLock(&Factory->Signal->Lock, LockHandle);
        if (!NT_SUCCESS(Status))
        {
            Factory->CreateInProgress--;
            break;
        }
    }

    return Status;
}

/* Port watch routine, at or below DISPATCH_LEVEL */
static
VOID
NTAPI
ExpWorkerFactoryPortSignaled(
    _In_ PVOID Context)
{
    PWORKER_FACTORY Factory = Context;
    KLOCK_QUEUE_HANDLE LockHandle;

    KeAcquireInStackQueuedSpinLock(&Factory->Signal->Lock, &LockHandle);
    Factory->WatchState = WatchFired;
    KeReleaseInStackQueuedSpinLock(&LockHandle);

    /* Threads are created at PASSIVE_LEVEL; the watch reference moves to the work item */
    ExQueueWorkItem(&Factory->WatchWorkItem, DelayedWorkQueue);
}

static
VOID
NTAPI
ExpWorkerFactoryWatchWorker(
    _In_ PVOID Context)
{
    PWORKER_FACTORY Factory = Context;
    PWORKER_FACTORY_SIGNAL Signal = Factory->Signal;
    KLOCK_QUEUE_HANDLE LockHandle;

    KeAcquireInStackQueuedSpinLock(&Signal->Lock, &LockHandle);
    Factory->WatchState = WatchIdle;
    if (!Signal->Shutdown &&
        !Signal->WaitingWorkerCount &&
        (Factory->ReadyWorkerCount < Factory->ThreadMaximum) &&
        ExpWorkerFactoryWatchPort(Factory))
        ExpWorkerFactoryCheckCreate(Factory, &LockHandle);
    else
        KeReleaseInStackQueuedSpinLock(&LockHandle);

    ObDereferenceObject(Factory);
}

/* Stops all creation and wakes the waiting workers so they can see the factory is gone */
static
VOID
ExpShutdownWorkerFactory(
    _In_ PWORKER_FACTORY Factory)
{
    PWORKER_FACTORY_SIGNAL Signal = Factory->Signal;
    KLOCK_QUEUE_HANDLE LockHandle;
    BOOLEAN Wake = FALSE;

    KeAcquireInStackQueuedSpinLock(&Signal->Lock, &LockHandle);
    Signal->Shutdown = TRUE;
    Factory->ThreadMinimum = 0;
    Factory->ThreadMaximum = 0;
    ExpWorkerFactoryStopWatching(Factory);
    if (Signal->WaitingWorkerCount && !Signal->PacketQueued)
    {
        Signal->PacketQueued = TRUE;
        Wake = TRUE;
    }
    KeReleaseInStackQueuedSpinLock(&LockHandle);

    if (Wake)
        ExpWorkerFactoryQueueSignal(Signal);
}

static
VOID
NTAPI
ExpCloseWorkerFactory(
    _In_opt_ PEPROCESS Process,
    _In_ PVOID Object,
    _In_ ACCESS_MASK GrantedAccess,
    _In_ ULONG_PTR ProcessHandleCount,
    _In_ ULONG_PTR SystemHandleCount)
{
    /* The last handle going away shuts the factory down */
    if (SystemHandleCount == 1)
        ExpShutdownWorkerFactory(Object);
}

static
VOID
NTAPI
ExpDeleteWorkerFactory(
    _In_ PVOID Object)
{
    PWORKER_FACTORY Factory = Object;
    PWORKER_FACTORY_SIGNAL Signal = Factory->Signal;
    KLOCK_QUEUE_HANDLE LockHandle;
    BOOLEAN FreeSignal;
    PKQUEUE Port;

    /* A signal still in the port is freed when it leaves, even if the port goes first */
    KeAcquireInStackQueuedSpinLock(&Signal->Lock, &LockHandle);
    Signal->Deleted = TRUE;
    FreeSignal = !Signal->PacketQueued;
    Port = Signal->Port;
    KeReleaseInStackQueuedSpinLock(&LockHandle);

    ObCloseHandle(Factory->ProcessHandle, KernelMode);
    ObDereferenceObject(Factory->Process);
    ObDereferenceObject(Port);

    if (FreeSignal)
        ExFreePoolWithTag(Signal, TAG_WORKER_FACTORY);
}

CODE_SEG("INIT")
BOOLEAN
NTAPI
ExpInitializeWorkerFactoryImplementation(VOID)
{
    OBJECT_TYPE_INITIALIZER ObjectTypeInitializer;
    UNICODE_STRING TypeName = RTL_CONSTANT_STRING(L"TpWorkerFactory");
    NTSTATUS Status;

    RtlZeroMemory(&ObjectTypeInitializer, sizeof(ObjectTypeInitializer));
    ObjectTypeInitializer.Length = sizeof(ObjectTypeInitializer);
    ObjectTypeInitializer.InvalidAttributes = OBJ_OPENLINK;
    ObjectTypeInitializer.GenericMapping = ExpWorkerFactoryMapping;
    ObjectTypeInitializer.ValidAccessMask = WORKER_FACTORY_GENERIC_ALL;
    ObjectTypeInitializer.PoolType = NonPagedPool;
    ObjectTypeInitializer.DefaultNonPagedPoolCharge = sizeof(WORKER_FACTORY);
    ObjectTypeInitializer.CloseProcedure = ExpCloseWorkerFactory;
    ObjectTypeInitializer.DeleteProcedure = ExpDeleteWorkerFactory;

    Status = ObCreateObjectType(&TypeName,
                                &ObjectTypeInitializer,
                                NULL,
                                &ExpWorkerFactoryObjectType);
    return NT_SUCCESS(Status);
}

/* PUBLIC FUNCTIONS *********************************************************/

/**
 * @brief
 * Creates a worker factory serving a completion port with threads in the calling process.
 */
NTSTATUS
NTAPI
NtCreateWorkerFactory(
    _Out_ PHANDLE WorkerFactoryHandleReturn,
    _In_ ACCESS_MASK DesiredAccess,
    _In_opt_ POBJECT_ATTRIBUTES ObjectAttributes,
    _In_ HANDLE CompletionPortHandle,
    _In_ HANDLE WorkerProcessHandle,
    _In_ PVOID StartRoutine,
    _In_opt_ PVOID StartParameter,
    _In_opt_ ULONG MaxThreadCount,
    _In_opt_ SIZE_T StackReserve,
    _In_opt_ SIZE_T StackCommit)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    PWORKER_FACTORY_SIGNAL Signal;
    PWORKER_FACTORY Factory;
    HANDLE ProcessHandle;
    PEPROCESS Process;
    PKQUEUE Port;
    HANDLE Handle;
    NTSTATUS Status;
    PAGED_CODE();

    if (PreviousMode != KernelMode)
    {
        _SEH2_TRY
        {
            ProbeForWriteHandle(WorkerFactoryHandleReturn);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    Signal = ExAllocatePoolZero(NonPagedPool, sizeof(*Signal), TAG_WORKER_FACTORY);
    if (!Signal)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ObReferenceObjectByHandle(CompletionPortHandle,
                                       IO_COMPLETION_MODIFY_STATE,
                                       IoCompletionType,
                                       PreviousMode,
                                       (PVOID*)&Port,
                                       NULL);
    if (!NT_SUCCESS(Status))
        goto FreeSignal;

    Status = ObReferenceObjectByHandle(WorkerProcessHandle,
                                       PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE,
                                       PsProcessType,
                                       PreviousMode,
                                       (PVOID*)&Process,
                                       NULL);
    if (!NT_SUCCESS(Status))
        goto DereferencePort;

    /* Workers can only be made for the calling process */
    if (Process != PsGetCurrentProcess())
    {
        Status = STATUS_INVALID_PARAMETER;
        goto DereferenceProcess;
    }

    Status = ObOpenObjectByPointer(Process,
                                   OBJ_KERNEL_HANDLE,
                                   NULL,
                                   PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE,
                                   PsProcessType,
                                   KernelMode,
                                   &ProcessHandle);
    if (!NT_SUCCESS(Status))
        goto DereferenceProcess;

    Status = ObCreateObject(PreviousMode,
                            ExpWorkerFactoryObjectType,
                            ObjectAttributes,
                            PreviousMode,
                            NULL,
                            sizeof(*Factory),
                            0,
                            0,
                            (PVOID*)&Factory);
    if (!NT_SUCCESS(Status))
        goto CloseProcess;

    KeInitializeSpinLock(&Signal->Lock);
    Signal->Port = Port;
    Signal->Packet.Packet.PacketType = IopCompletionPacketCallback;
    Signal->Packet.Routine = ExpWorkerFactorySignalReturned;
    Signal->Packet.Context = Signal;

    RtlZeroMemory(Factory, sizeof(*Factory));
    Factory->Signal = Signal;
    Factory->Process = Process;
    Factory->ProcessHandle = ProcessHandle;
    Factory->StartRoutine = (PTHREAD_START_ROUTINE)StartRoutine;
    Factory->StartParameter = StartParameter;
    Factory->StackReserve = StackReserve ? StackReserve : WORKER_FACTORY_STACK_RESERVE;
    Factory->StackCommit = StackCommit ? StackCommit : WORKER_FACTORY_STACK_COMMIT;
    Factory->IdleTimeout.QuadPart = WORKER_FACTORY_IDLE_SECONDS * WORKER_FACTORY_SECOND;
    Factory->ThreadMaximum = MaxThreadCount;
    ExInitializeRundownProtection(&Factory->CreationRundown);
    Factory->PortWatch.Port = Port;
    Factory->PortWatch.Routine = ExpWorkerFactoryPortSignaled;
    Factory->PortWatch.Context = Factory;
    ExInitializeWorkItem(&Factory->WatchWorkItem, ExpWorkerFactoryWatchWorker, Factory);
    Factory->WatchState = WatchIdle;

    /* From here the delete routine owns the references and the signal */
    Status = ObInsertObject(Factory, NULL, DesiredAccess, 0, NULL, &Handle);
    if (!NT_SUCCESS(Status))
        return Status;

    _SEH2_TRY
    {
        *WorkerFactoryHandleReturn = Handle;
    }
    _SEH2_EXCEPT(ExSystemExceptionFilter())
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    return Status;

CloseProcess:
    ObCloseHandle(ProcessHandle, KernelMode);
DereferenceProcess:
    ObDereferenceObject(Process);
DereferencePort:
    ObDereferenceObject(Port);
FreeSignal:
    ExFreePoolWithTag(Signal, TAG_WORKER_FACTORY);
    return Status;
}

/**
 * @brief
 * Returns WORKER_FACTORY_BASIC_INFORMATION, the only class that can be queried.
 */
NTSTATUS
NTAPI
NtQueryInformationWorkerFactory(
    _In_ HANDLE WorkerFactoryHandle,
    _In_ WORKERFACTORYINFOCLASS WorkerFactoryInformationClass,
    _Out_writes_bytes_(WorkerFactoryInformationLength) PVOID WorkerFactoryInformation,
    _In_ ULONG WorkerFactoryInformationLength,
    _Out_opt_ PULONG ReturnLength)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    WORKER_FACTORY_BASIC_INFORMATION Info;
    PWORKER_FACTORY_SIGNAL Signal;
    KLOCK_QUEUE_HANDLE LockHandle;
    PWORKER_FACTORY Factory;
    NTSTATUS Status;
    PAGED_CODE();

    if (WorkerFactoryInformationClass != WorkerFactoryBasicInformation)
        return STATUS_INVALID_INFO_CLASS;

    /* The needed size is reported even when the buffer has the wrong size */
    _SEH2_TRY
    {
        if (PreviousMode != KernelMode)
        {
            ProbeForWrite(WorkerFactoryInformation, sizeof(Info), sizeof(ULONG));
            if (ReturnLength)
                ProbeForWriteUlong(ReturnLength);
        }

        if (ReturnLength)
            *ReturnLength = sizeof(Info);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    if (WorkerFactoryInformationLength != sizeof(Info))
        return STATUS_INFO_LENGTH_MISMATCH;

    Status = ObReferenceObjectByHandle(WorkerFactoryHandle,
                                       WORKER_FACTORY_QUERY_INFORMATION,
                                       ExpWorkerFactoryObjectType,
                                       PreviousMode,
                                       (PVOID*)&Factory,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Signal = Factory->Signal;
    RtlZeroMemory(&Info, sizeof(Info));

    KeAcquireInStackQueuedSpinLock(&Signal->Lock, &LockHandle);
    Info.IdleTimeout = Factory->IdleTimeout;
    Info.QueuedToExWorker = (Factory->WatchState == WatchFired);
    Info.MayCreate = (Signal->WaitingWorkerCount == 0);
    Info.CreateInProgress = (Factory->CreateInProgress != 0);
    Info.InsertedIntoQueue = Signal->PacketQueued;
    Info.Shutdown = Signal->Shutdown;
    Info.BindingCount = Factory->BindingCount;
    Info.ThreadMinimum = Factory->ThreadMinimum;
    Info.ThreadMaximum = Factory->ThreadMaximum;
    Info.PendingWorkerCount = Factory->PendingWorkerCount;
    Info.WaitingWorkerCount = Signal->WaitingWorkerCount;
    Info.TotalWorkerCount = Factory->TotalWorkerCount;
    Info.ReleaseCount = Signal->ReleaseCount;
    Info.StartRoutine = (PVOID)Factory->StartRoutine;
    Info.StartParameter = Factory->StartParameter;
    Info.ProcessId = PsGetProcessId(Factory->Process);
    Info.StackReserve = Factory->StackReserve;
    Info.StackCommit = Factory->StackCommit;
    Info.LastThreadCreationStatus = Factory->LastThreadCreationStatus;
    KeReleaseInStackQueuedSpinLock(&LockHandle);

    ObDereferenceObject(Factory);

    _SEH2_TRY
    {
        RtlCopyMemory(WorkerFactoryInformation, &Info, sizeof(Info));
    }
    _SEH2_EXCEPT(ExSystemExceptionFilter())
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    return Status;
}

/**
 * @brief
 * Changes a factory setting, creating or letting go of workers where the new value calls for it.
 */
NTSTATUS
NTAPI
NtSetInformationWorkerFactory(
    _In_ HANDLE WorkerFactoryHandle,
    _In_ WORKERFACTORYINFOCLASS WorkerFactoryInformationClass,
    _In_reads_bytes_(WorkerFactoryInformationLength) PVOID WorkerFactoryInformation,
    _In_ ULONG WorkerFactoryInformationLength)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    WORKER_FACTORY_SET_BUFFER Buffer;
    PWORKER_FACTORY_SIGNAL Signal;
    KLOCK_QUEUE_HANDLE LockHandle;
    PWORKER_FACTORY Factory;
    BOOLEAN LockHeld = TRUE;
    BOOLEAN CreateOne = FALSE;
    BOOLEAN Quiet = FALSE;
    ULONG Woken = 0;
    ULONG Needed;
    ULONG Old;
    NTSTATUS Status = STATUS_SUCCESS;
    PAGED_CODE();

    switch (WorkerFactoryInformationClass)
    {
        case WorkerFactoryIdleTimeout:
            Needed = sizeof(Buffer.Timeout);
            break;

        case WorkerFactoryStackInformation:
            Needed = sizeof(Buffer.Stack);
            break;

        case WorkerFactoryBindingCount:
        case WorkerFactoryThreadMinimum:
        case WorkerFactoryThreadMaximum:
        case WorkerFactoryAdjustThreadGoal:
        case WorkerFactoryCallbackType:
        case WorkerFactoryThreadBasePriority:
        case WorkerFactoryTimeoutWaiters:
        case WorkerFactoryFlags:
        case WorkerFactoryThreadSoftMaximum:
            Needed = sizeof(Buffer.Value);
            break;

        case WorkerFactoryPaused:
            return STATUS_NOT_IMPLEMENTED;

        default:
            return STATUS_INVALID_INFO_CLASS;
    }

    if (WorkerFactoryInformationLength != Needed)
        return STATUS_INFO_LENGTH_MISMATCH;

    /* TimeoutWaiters hands back how many waiters it woke */
    _SEH2_TRY
    {
        if (PreviousMode != KernelMode)
        {
            if (WorkerFactoryInformationClass == WorkerFactoryTimeoutWaiters)
                ProbeForWrite(WorkerFactoryInformation, Needed, sizeof(ULONG));
            else
                ProbeForRead(WorkerFactoryInformation, Needed, sizeof(ULONG));
        }

        RtlCopyMemory(&Buffer, WorkerFactoryInformation, Needed);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    Status = ObReferenceObjectByHandle(WorkerFactoryHandle,
                                       WORKER_FACTORY_SET_INFORMATION,
                                       ExpWorkerFactoryObjectType,
                                       PreviousMode,
                                       (PVOID*)&Factory,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Signal = Factory->Signal;

    /* The thread goal is the concurrency of the port itself */
    if (WorkerFactoryInformationClass == WorkerFactoryAdjustThreadGoal)
    {
        Signal->Port->MaximumCount = Buffer.Value ? Buffer.Value : (ULONG)KeNumberProcessors;
        ObDereferenceObject(Factory);
        return STATUS_SUCCESS;
    }

    KeAcquireInStackQueuedSpinLock(&Signal->Lock, &LockHandle);
    switch (WorkerFactoryInformationClass)
    {
        case WorkerFactoryIdleTimeout:
            if (Buffer.Timeout.QuadPart >= 0)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            /* Relative times are negative, so the shortest allowed is the largest value */
            if (Buffer.Timeout.QuadPart > WORKER_FACTORY_MIN_IDLE_SECONDS * WORKER_FACTORY_SECOND)
                Buffer.Timeout.QuadPart = WORKER_FACTORY_MIN_IDLE_SECONDS * WORKER_FACTORY_SECOND;
            else if (Buffer.Timeout.QuadPart < WORKER_FACTORY_MAX_IDLE_SECONDS * WORKER_FACTORY_SECOND)
                Buffer.Timeout.QuadPart = WORKER_FACTORY_MAX_IDLE_SECONDS * WORKER_FACTORY_SECOND;
            Factory->IdleTimeout = Buffer.Timeout;
            break;

        case WorkerFactoryBindingCount:
            /* The value is a signed adjustment that stops at zero and MAXULONG */
            Old = Factory->BindingCount;
            if ((LONG)Buffer.Value < 0)
                Factory->BindingCount = (Old > (ULONG)0 - Buffer.Value) ? Old + Buffer.Value : 0;
            else
                Factory->BindingCount = (Old + Buffer.Value > Old) ? Old + Buffer.Value : MAXULONG;

            if (Old && !Factory->BindingCount)
            {
                ExpWorkerFactoryStopWatching(Factory);
            }
            else if (!Old &&
                     Factory->BindingCount &&
                     !Signal->WaitingWorkerCount &&
                     (Factory->ReadyWorkerCount < Factory->ThreadMaximum) &&
                     ExpWorkerFactoryWatchPort(Factory))
            {
                ExpWorkerFactoryCheckCreate(Factory, &LockHandle);
                LockHeld = FALSE;
            }
            break;

        case WorkerFactoryThreadMinimum:
            if (Signal->Shutdown)
            {
                Status = STATUS_ABANDONED;
                break;
            }

            Old = Factory->ThreadMinimum;
            Factory->ThreadMinimum = Buffer.Value;
            if (Factory->ThreadMaximum < Buffer.Value)
                Factory->ThreadMaximum = Buffer.Value;
            if (Buffer.Value > Old)
                Status = ExpWorkerFactoryFillMinimum(Factory, &LockHandle);
            break;

        case WorkerFactoryThreadMaximum:
            if (Signal->Shutdown)
            {
                Status = STATUS_ABANDONED;
                break;
            }

            /* Raising the limit lets queued work get the thread it was refused */
            CreateOne = Buffer.Value &&
                        ((!Factory->ThreadMaximum && Signal->ReleaseCount) ||
                         (KeReadStateQueue(Signal->Port) > 0));
            Quiet = TRUE;
            Factory->ThreadMaximum = Buffer.Value;
            if (Buffer.Value < Factory->ThreadMinimum)
                Factory->ThreadMinimum = Buffer.Value;
            break;

        case WorkerFactoryCallbackType:
            switch (Buffer.Value)
            {
                case 0:
                    /* A worker that left the pool comes back */
                    if (Factory->ReadyWorkerCount == Factory->TotalWorkerCount)
                        Status = STATUS_UNSUCCESSFUL;
                    else
                        Factory->ReadyWorkerCount++;
                    break;

                case 1:
                    /* A callback will run long, so another thread may be wanted */
                    CreateOne = TRUE;
                    break;

                case 2:
                case 3:
                    /* A worker leaves the pool, for good when it hit an unrecoverable error */
                    if (!Factory->ReadyWorkerCount)
                    {
                        Status = STATUS_UNSUCCESSFUL;
                        break;
                    }

                    Factory->ReadyWorkerCount--;
                    if ((Buffer.Value == 3) && Factory->TotalWorkerCount)
                        Factory->TotalWorkerCount--;
                    CreateOne = (Factory->ReadyWorkerCount == 0);
                    break;

                default:
                    Status = STATUS_INVALID_PARAMETER;
                    break;
            }
            break;

        case WorkerFactoryStackInformation:
            if (Signal->Shutdown)
            {
                Status = STATUS_ABANDONED;
                break;
            }

            Factory->StackReserve = Buffer.Stack.StackReserve;
            Factory->StackCommit = Buffer.Stack.StackCommit;
            break;

        case WorkerFactoryThreadBasePriority:
            /* Windows keeps the value even after shutdown */
            Factory->BasePriority = (KPRIORITY)Buffer.Value;
            if (Signal->Shutdown)
                Status = STATUS_ABANDONED;
            break;

        case WorkerFactoryTimeoutWaiters:
            /* Spare waiters above the minimum are woken to exit now instead of after the idle time */
            if (!Factory->CreateInProgress && (Factory->ReadyWorkerCount > Factory->ThreadMinimum))
                Woken = Factory->ReadyWorkerCount - Factory->ThreadMinimum;
            break;

        case WorkerFactoryFlags:
            Factory->Flags = Buffer.Value & WORKER_FACTORY_VALID_FLAGS;
            break;

        case WorkerFactoryThreadSoftMaximum:
            if (Signal->Shutdown)
            {
                Status = STATUS_ABANDONED;
                break;
            }

            Factory->ThreadSoftMaximum = Buffer.Value;
            if (Buffer.Value && (Buffer.Value < Factory->ThreadMinimum))
                Factory->ThreadSoftMaximum = Factory->ThreadMinimum;
            else if (Buffer.Value > Factory->ThreadMaximum)
                Factory->ThreadSoftMaximum = Factory->ThreadMaximum;
            break;

        default:
            ASSERT(FALSE);
            break;
    }

    if (CreateOne && NT_SUCCESS(Status))
        Status = ExpWorkerFactoryCreateOne(Factory, &LockHandle, Quiet);
    else if (LockHeld)
        KeReleaseInStackQueuedSpinLock(&LockHandle);

    if (WorkerFactoryInformationClass == WorkerFactoryTimeoutWaiters)
    {
        if (Woken)
            Woken = KiTimeOutQueueWaiters(Signal->Port, Woken);

        _SEH2_TRY
        {
            *(PULONG)WorkerFactoryInformation = Woken;
        }
        _SEH2_EXCEPT(ExSystemExceptionFilter())
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
    }

    ObDereferenceObject(Factory);
    return Status;
}

/**
 * @brief
 * Called by a new worker once it runs, turning it from pending into a ready worker.
 */
NTSTATUS
NTAPI
NtWorkerFactoryWorkerReady(
    _In_ HANDLE WorkerFactoryHandle)
{
    PWORKER_FACTORY_SIGNAL Signal;
    KLOCK_QUEUE_HANDLE LockHandle;
    PWORKER_FACTORY Factory;
    NTSTATUS Status;
    PAGED_CODE();

    Status = ObReferenceObjectByHandle(WorkerFactoryHandle,
                                       WORKER_FACTORY_READY_WORKER,
                                       ExpWorkerFactoryObjectType,
                                       ExGetPreviousMode(),
                                       (PVOID*)&Factory,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Signal = Factory->Signal;
    KeAcquireInStackQueuedSpinLock(&Signal->Lock, &LockHandle);
    if (!Factory->CreateInProgress)
    {
        Status = STATUS_UNSUCCESSFUL;
    }
    else
    {
        Factory->CreateInProgress--;

        /* Shutdown already gave the pending workers to the caller of NtShutdownWorkerFactory */
        if (Factory->PendingWorkerCount)
        {
            Factory->PendingWorkerCount--;
            Factory->ReadyWorkerCount++;
            Factory->TotalWorkerCount++;
        }
        else
        {
            Status = STATUS_PROCESS_IS_TERMINATING;
        }
    }
    KeReleaseInStackQueuedSpinLock(&LockHandle);

    ObDereferenceObject(Factory);
    return Status;
}

/**
 * @brief
 * Releases a worker for queued work: a waiting worker gets an empty completion, and a new thread
 * is made when nobody is waiting.
 */
NTSTATUS
NTAPI
NtReleaseWorkerFactoryWorker(
    _In_ HANDLE WorkerFactoryHandle)
{
    PWORKER_FACTORY_SIGNAL Signal;
    KLOCK_QUEUE_HANDLE LockHandle;
    PWORKER_FACTORY Factory;
    BOOLEAN Queue = FALSE;
    NTSTATUS Status;
    PAGED_CODE();

    Status = ObReferenceObjectByHandle(WorkerFactoryHandle,
                                       WORKER_FACTORY_RELEASE_WORKER,
                                       ExpWorkerFactoryObjectType,
                                       ExGetPreviousMode(),
                                       (PVOID*)&Factory,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Signal = Factory->Signal;
    KeAcquireInStackQueuedSpinLock(&Signal->Lock, &LockHandle);
    if (Signal->Shutdown)
    {
        Status = STATUS_ABANDONED;
    }
    else if (Signal->ReleaseCount == MAXULONG)
    {
        Status = STATUS_UNSUCCESSFUL;
    }
    else
    {
        /* Releases pile up on the one signal packet, which is queued again for each */
        Signal->ReleaseCount++;
        if (!Signal->PacketQueued)
        {
            Signal->PacketQueued = TRUE;
            ExpWorkerFactoryStopWatching(Factory);
            Queue = TRUE;
        }
    }
    KeReleaseInStackQueuedSpinLock(&LockHandle);

    if (Queue)
    {
        ExpWorkerFactoryQueueSignal(Signal);
        KeAcquireInStackQueuedSpinLock(&Signal->Lock, &LockHandle);
        ExpWorkerFactoryCheckCreate(Factory, &LockHandle);
    }

    ObDereferenceObject(Factory);
    return Status;
}

/**
 * @brief
 * Waits for completions on the factory's port. A worker gets STATUS_TIMEOUT when it should exit,
 * and STATUS_ABANDONED once the factory has been shut down.
 */
NTSTATUS
NTAPI
NtWaitForWorkViaWorkerFactory(
    _In_ HANDLE WorkerFactoryHandle,
    _Out_writes_to_(Count, *PacketsReturned) PFILE_IO_COMPLETION_INFORMATION MiniPackets,
    _In_ ULONG Count,
    _Out_ PULONG PacketsReturned,
    _In_ PWORKER_FACTORY_DEFERRED_WORK DeferredWork)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    WORKER_FACTORY_DEFERRED_WORK Deferred;
    PWORKER_FACTORY_SIGNAL Signal;
    KLOCK_QUEUE_HANDLE LockHandle;
    PWORKER_FACTORY Factory;
    LARGE_INTEGER Timeout;
    BOOLEAN Leaving = FALSE;
    ULONG Removed = 0;
    NTSTATUS Status;
    PAGED_CODE();

    if (!Count || (Count > WORKER_FACTORY_MAX_PACKETS))
        return STATUS_INVALID_PARAMETER;

    _SEH2_TRY
    {
        if (PreviousMode != KernelMode)
        {
            ProbeForWrite(MiniPackets, Count * sizeof(*MiniPackets), sizeof(ULONG));
            ProbeForWriteUlong(PacketsReturned);
            ProbeForRead(DeferredWork, sizeof(*DeferredWork), sizeof(ULONG));
        }

        Deferred = *DeferredWork;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    /* There is no ALPC to send the deferred message through */
    if (Deferred.Flags & WORKER_FACTORY_DEFER_ALPC_SEND)
        DPRINT1("Deferred ALPC send to %p is not supported\n", Deferred.AlpcSendMessagePort);

    Status = ObReferenceObjectByHandle(WorkerFactoryHandle,
                                       WORKER_FACTORY_WAIT,
                                       ExpWorkerFactoryObjectType,
                                       PreviousMode,
                                       (PVOID*)&Factory,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Signal = Factory->Signal;
    KeAcquireInStackQueuedSpinLock(&Signal->Lock, &LockHandle);
    if (Signal->Shutdown)
    {
        KeReleaseInStackQueuedSpinLock(&LockHandle);
        ObDereferenceObject(Factory);
        return STATUS_ABANDONED;
    }

    /* A waiter will see new packets, so nothing has to watch for them */
    ExpWorkerFactoryStopWatching(Factory);
    Signal->WaitingWorkerCount++;

    for (;;)
    {
        /* Lowering the maximum or shutting down sends extra workers home */
        if ((Factory->ReadyWorkerCount > Factory->ThreadMaximum) || Signal->Shutdown)
        {
            Status = STATUS_TIMEOUT;
            Leaving = TRUE;
            break;
        }

        Timeout = Factory->IdleTimeout;
        KeReleaseInStackQueuedSpinLock(&LockHandle);

        Status = IopRemoveCompletionEntries(Signal->Port,
                                            MiniPackets,
                                            Count,
                                            &Removed,
                                            PreviousMode,
                                            &Timeout,
                                            TRUE);

        KeAcquireInStackQueuedSpinLock(&Signal->Lock, &LockHandle);
        if (Status != STATUS_TIMEOUT)
            break;

        /* Idle long enough: leave if the pool can spare this worker and it owns no pending I/O */
        if ((Factory->ReadyWorkerCount > Factory->ThreadMinimum) &&
            !ExpWorkerFactoryWantsThread(Factory, TRUE) &&
            IsListEmpty(&PsGetCurrentThread()->IrpList))
        {
            Leaving = TRUE;
            break;
        }
    }

    Signal->WaitingWorkerCount--;
    if (Leaving)
    {
        if (Factory->ReadyWorkerCount)
            Factory->ReadyWorkerCount--;
        if (Factory->TotalWorkerCount)
            Factory->TotalWorkerCount--;
    }

    /* The last waiter is gone, so queued work may need a thread of its own */
    if (!Signal->WaitingWorkerCount &&
        (Factory->ReadyWorkerCount < Factory->ThreadMaximum) &&
        ExpWorkerFactoryWatchPort(Factory))
        ExpWorkerFactoryCheckCreate(Factory, &LockHandle);
    else
        KeReleaseInStackQueuedSpinLock(&LockHandle);

    if (Status == STATUS_SUCCESS)
    {
        _SEH2_TRY
        {
            *PacketsReturned = Removed;
        }
        _SEH2_EXCEPT(ExSystemExceptionFilter())
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
    }

    ObDereferenceObject(Factory);
    return Status;
}

/**
 * @brief
 * Shuts a factory down for good. Workers still starting up are added to PendingWorkerCount,
 * since they will exit as soon as they report ready.
 */
NTSTATUS
NTAPI
NtShutdownWorkerFactory(
    _In_ HANDLE WorkerFactoryHandle,
    _Inout_ volatile LONG *PendingWorkerCount)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    KLOCK_QUEUE_HANDLE LockHandle;
    PWORKER_FACTORY Factory;
    NTSTATUS Status;
    PAGED_CODE();

    if (PreviousMode != KernelMode)
    {
        _SEH2_TRY
        {
            ProbeForRead((PVOID)PendingWorkerCount, sizeof(*PendingWorkerCount), sizeof(LONG));
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    Status = ObReferenceObjectByHandle(WorkerFactoryHandle,
                                       WORKER_FACTORY_SHUTDOWN,
                                       ExpWorkerFactoryObjectType,
                                       PreviousMode,
                                       (PVOID*)&Factory,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    ExpShutdownWorkerFactory(Factory);

    /* Once no creation is in flight the pending count can only shrink */
    ExWaitForRundownProtectionRelease(&Factory->CreationRundown);

    _SEH2_TRY
    {
        InterlockedExchangeAdd(PendingWorkerCount, (LONG)Factory->PendingWorkerCount);
    }
    _SEH2_EXCEPT(ExSystemExceptionFilter())
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    KeAcquireInStackQueuedSpinLock(&Factory->Signal->Lock, &LockHandle);
    Factory->PendingWorkerCount = 0;
    KeReleaseInStackQueuedSpinLock(&LockHandle);

    ObDereferenceObject(Factory);
    return Status;
}

/* EOF */
