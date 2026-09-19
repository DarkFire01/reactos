/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SCI delivery with the uACPI handler running in the ISR
 */

#include "uacpi_sci.h"

// Translated SCI resource captured from START_DEVICE.
static UACPI_HOST_SCI_RESOURCE UacpiHostSci;
static BOOLEAN                 UacpiHostSciValid;

VOID UacpiHostSetSciResource(PUACPI_HOST_SCI_RESOURCE Resource)
{
    UacpiHostSci = *Resource;
    UacpiHostSciValid = TRUE;
}

// Recorded during namespace_load, connected by UacpiHostConnectSci.
static PUACPI_HOST_INTERRUPT UacpiHostSciBlock;
static ULONG                 UacpiHostSciGsi;

// The SCI's DIRQL once connected, DISPATCH_LEVEL before. The ISR takes the same
// uACPI locks (GPE state, registers), so a holder must not be preemptable by it.
static KIRQL UacpiHostSciLockIrql = DISPATCH_LEVEL;
// Seconds between reports while a work drain is stuck.
#define UACPI_WORK_WAIT_REPORT_SECONDS 5

// Spinlocks

// Callable at DIRQL. Raises to the SCI's IRQL, as uACPI's contract requires
// ("expected to disable interrupts").
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle h)
{
    KIRQL old;
    KIRQL target = UacpiHostSciLockIrql;

    if (KeGetCurrentIrql() > target)
    {
        target = KeGetCurrentIrql();
    }
    KeRaiseIrql(target, &old);
    KeAcquireSpinLockAtDpcLevel((PKSPIN_LOCK)h);
    return (uacpi_cpu_flags)old;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle h, uacpi_cpu_flags flags)
{
    KeReleaseSpinLockFromDpcLevel((PKSPIN_LOCK)h);
    KeLowerIrql((KIRQL)flags);
}

// Deferred work and signals

// Entries queued from the ISR come from a fixed pool and are drained by a DPC.
#define UACPI_ISR_WORK_POOL 64

typedef enum _UACPI_DEFERRED_KIND
{
    UacpiDeferredWork = 0,
    UacpiDeferredSignal
} UACPI_DEFERRED_KIND;

typedef struct _UACPI_DEFERRED
{
    SLIST_ENTRY        Link;        // must be first and 16-byte aligned on x64
    UACPI_DEFERRED_KIND Kind;
    uacpi_work_handler Handler;     // UacpiDeferredWork
    uacpi_handle       Ctx;
    uacpi_work_type    Type;
    uacpi_handle       Event;       // UacpiDeferredSignal
} UACPI_DEFERRED, *PUACPI_DEFERRED;

typedef struct _UACPI_WORK_ITEM
{
    WORK_QUEUE_ITEM    Item;
    uacpi_work_handler Handler;
    uacpi_handle       Ctx;
    uacpi_work_type    Type;
} UACPI_WORK_ITEM, *PUACPI_WORK_ITEM;

static SLIST_HEADER   UacpiIsrPending;    // ISR to DPC
static SLIST_HEADER   UacpiIsrFree;       // preallocated entry pool
static PUACPI_DEFERRED UacpiIsrPool;
static KDPC           UacpiIsrDeferDpc;

static volatile LONG  UacpiHostWorkOutstanding;
static KEVENT         UacpiHostWorkDrained;
static BOOLEAN        UacpiHostWorkInit;

// Vista+ exports, resolved at runtime for the Server 2003 build.
typedef KAFFINITY (NTAPI *PKE_SET_SYSTEM_AFFINITY_THREAD_EX)(KAFFINITY);
typedef VOID (NTAPI *PKE_REVERT_TO_USER_AFFINITY_THREAD_EX)(KAFFINITY);

static PKE_SET_SYSTEM_AFFINITY_THREAD_EX     UacpiHostKeSetSystemAffinityThreadEx;
static PKE_REVERT_TO_USER_AFFINITY_THREAD_EX UacpiHostKeRevertToUserAffinityThreadEx;
static BOOLEAN                               UacpiHostAffinityRoutinesResolved;

static VOID
UacpiHostResolveAffinityRoutines(VOID)
{
    UNICODE_STRING name;

    if (UacpiHostAffinityRoutinesResolved)
    {
        return;
    }
    RtlInitUnicodeString(&name, L"KeSetSystemAffinityThreadEx");
    UacpiHostKeSetSystemAffinityThreadEx =
        (PKE_SET_SYSTEM_AFFINITY_THREAD_EX)MmGetSystemRoutineAddress(&name);
    RtlInitUnicodeString(&name, L"KeRevertToUserAffinityThreadEx");
    UacpiHostKeRevertToUserAffinityThreadEx =
        (PKE_REVERT_TO_USER_AFFINITY_THREAD_EX)MmGetSystemRoutineAddress(&name);
    UacpiHostAffinityRoutinesResolved = TRUE;
}

static VOID
NTAPI
UacpiHostWorkRoutine(PVOID Parameter)
{
    PUACPI_WORK_ITEM w = (PUACPI_WORK_ITEM)Parameter;
    KAFFINITY prev = 0;
    BOOLEAN pinned = FALSE;

    UacpiHostResolveAffinityRoutines();
    if (w->Type == UACPI_WORK_GPE_EXECUTION &&
        UacpiHostKeSetSystemAffinityThreadEx != NULL &&
        UacpiHostKeRevertToUserAffinityThreadEx != NULL)
    {
        prev = UacpiHostKeSetSystemAffinityThreadEx((KAFFINITY)1);   // CPU0
        pinned = TRUE;
    }

    w->Handler(w->Ctx);   // PASSIVE_LEVEL; runs AML

    if (pinned)
    {
        UacpiHostKeRevertToUserAffinityThreadEx(prev);
    }

    ExFreePoolWithTag(w, UACPI_HOST_TAG);
    if (InterlockedDecrement(&UacpiHostWorkOutstanding) == 0)
    {
        KeSetEvent(&UacpiHostWorkDrained, IO_NO_INCREMENT, FALSE);
    }
}

// DISPATCH_LEVEL. Queues work items and releases semaphores for the ISR.
_Function_class_(KDEFERRED_ROUTINE)
static VOID
NTAPI
UacpiIsrDeferRoutine(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    PSLIST_ENTRY entry;
    PUACPI_DEFERRED d;
    PUACPI_WORK_ITEM w;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    while ((entry = ExInterlockedPopEntrySList(&UacpiIsrPending, NULL)) != NULL)
    {
        d = CONTAINING_RECORD(entry, UACPI_DEFERRED, Link);

        if (d->Kind == UacpiDeferredSignal)
        {
            KeReleaseSemaphore((PKSEMAPHORE)d->Event, IO_NO_INCREMENT, 1, FALSE);
        }
        else
        {
            w = (PUACPI_WORK_ITEM)ExAllocatePoolWithTag(NonPagedPool, sizeof(*w),
                                                        UACPI_HOST_TAG);
            if (w != NULL)
            {
                w->Handler = d->Handler;
                w->Ctx     = d->Ctx;
                w->Type    = d->Type;
                ExInitializeWorkItem(&w->Item, UacpiHostWorkRoutine, w);
                ExQueueWorkItem(&w->Item, DelayedWorkQueue);
            }
            else
            {
                // Drop the count so wait_for_work_completion can drain.
                DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                           "[uacpi] deferred work dropped: out of pool\n");
                if (InterlockedDecrement(&UacpiHostWorkOutstanding) == 0)
                {
                    KeSetEvent(&UacpiHostWorkDrained, IO_NO_INCREMENT, FALSE);
                }
            }
        }

        InterlockedPushEntrySList(&UacpiIsrFree, &d->Link);
    }
}

// PASSIVE_LEVEL, before the SCI is connected.
static NTSTATUS
UacpiIsrDeferInitialize(VOID)
{
    ULONG i;

    PAGED_CODE();

    if (UacpiHostWorkInit)
    {
        return STATUS_SUCCESS;
    }

    // SLIST_HEADER and every entry must be 16-byte aligned on 64-bit.
    UacpiIsrPool = (PUACPI_DEFERRED)ExAllocatePoolWithTag(
        NonPagedPool, sizeof(UACPI_DEFERRED) * UACPI_ISR_WORK_POOL,
        UACPI_HOST_TAG);
    if (UacpiIsrPool == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(UacpiIsrPool, sizeof(UACPI_DEFERRED) * UACPI_ISR_WORK_POOL);

    InitializeSListHead(&UacpiIsrPending);
    InitializeSListHead(&UacpiIsrFree);
    for (i = 0; i < UACPI_ISR_WORK_POOL; i++)
    {
        InterlockedPushEntrySList(&UacpiIsrFree, &UacpiIsrPool[i].Link);
    }

    KeInitializeDpc(&UacpiIsrDeferDpc, UacpiIsrDeferRoutine, NULL);
    KeInitializeEvent(&UacpiHostWorkDrained, NotificationEvent, TRUE);
    UacpiHostWorkInit = TRUE;
    return STATUS_SUCCESS;
}

// Take an entry from the pool. Any IRQL.
static PUACPI_DEFERRED
UacpiIsrDeferAcquire(VOID)
{
    PSLIST_ENTRY entry;

    if (!UacpiHostWorkInit)
    {
        return NULL;
    }
    entry = ExInterlockedPopEntrySList(&UacpiIsrFree, NULL);
    return entry != NULL ? CONTAINING_RECORD(entry, UACPI_DEFERRED, Link) : NULL;
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type,
                                        uacpi_work_handler handler,
                                        uacpi_handle ctx)
{
    PUACPI_DEFERRED d = UacpiIsrDeferAcquire();

    if (d == NULL)
    {
        return UACPI_STATUS_OUT_OF_MEMORY;
    }
    d->Kind    = UacpiDeferredWork;
    d->Handler = handler;
    d->Ctx     = ctx;
    d->Type    = type;

    if (InterlockedIncrement(&UacpiHostWorkOutstanding) == 1)
    {
        KeClearEvent(&UacpiHostWorkDrained);
    }
    InterlockedPushEntrySList(&UacpiIsrPending, &d->Link);
    KeInsertQueueDpc(&UacpiIsrDeferDpc, NULL, NULL);
    return UACPI_STATUS_OK;
}

void uacpi_kernel_signal_event(uacpi_handle h)
{
    PUACPI_DEFERRED d;

    // Release directly at or below DISPATCH_LEVEL.
    if (KeGetCurrentIrql() <= DISPATCH_LEVEL)
    {
        KeReleaseSemaphore((PKSEMAPHORE)h, IO_NO_INCREMENT, 1, FALSE);
        return;
    }

    d = UacpiIsrDeferAcquire();
    if (d == NULL)
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[uacpi] event signal dropped: deferral pool exhausted\n");
        return;
    }
    d->Kind  = UacpiDeferredSignal;
    d->Event = h;
    InterlockedPushEntrySList(&UacpiIsrPending, &d->Link);
    KeInsertQueueDpc(&UacpiIsrDeferDpc, NULL, NULL);
}

uacpi_status uacpi_kernel_wait_for_work_completion(void)
{
    LARGE_INTEGER to;
    ULONG waited = 0;

    // Drain the deferral DPC, then scheduled work.
    KeFlushQueuedDpcs();

    if (UacpiHostWorkInit)
    {
        // uACPI calls this from under its own locks, so a work item that needs
        // one of those locks will never drain. Report instead of hanging mute.
        while (InterlockedCompareExchange(&UacpiHostWorkOutstanding, 0, 0) != 0)
        {
            to.QuadPart =
                -((LONGLONG)UACPI_WORK_WAIT_REPORT_SECONDS * 10 * 1000 * 1000);
            if (KeWaitForSingleObject(&UacpiHostWorkDrained, Executive, KernelMode,
                                      FALSE, &to) != STATUS_TIMEOUT)
            {
                continue;
            }

            waited += UACPI_WORK_WAIT_REPORT_SECONDS;
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "[uacpi] STUCK: %ld work item(s) undrained after %lu s "
                       "(waiter thread %p, irql %u) - still waiting\n",
                       InterlockedCompareExchange(&UacpiHostWorkOutstanding, 0, 0),
                       waited, PsGetCurrentThread(), (ULONG)KeGetCurrentIrql());
        }
    }
    return UACPI_STATUS_OK;
}

// ISR

_Function_class_(KSERVICE_ROUTINE)
static BOOLEAN
NTAPI
UacpiHostIsr(PKINTERRUPT Interrupt, PVOID Context)
{
    PUACPI_HOST_INTERRUPT irq = (PUACPI_HOST_INTERRUPT)Context;
    uacpi_interrupt_ret ret;

    UNREFERENCED_PARAMETER(Interrupt);

    // The handler clears PM1 status and disables fired GPEs before returning.
    ret = irq->Handler(irq->Ctx);

    if (ret == UACPI_INTERRUPT_HANDLED)
    {
        irq->Unhandled = 0;
        return TRUE;
    }

    // Decline so a shared vector reaches its owner.
    irq->Unhandled++;
    return FALSE;
}

// Install / connect / uninstall

uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
    uacpi_handle *out_irq_handle)
{
    PUACPI_HOST_INTERRUPT block;

    block = (PUACPI_HOST_INTERRUPT)ExAllocatePoolWithTag(NonPagedPool,
                                                         sizeof(*block),
                                                         UACPI_HOST_TAG);
    if (block == NULL)
    {
        return UACPI_STATUS_OUT_OF_MEMORY;
    }
    RtlZeroMemory(block, sizeof(*block));
    block->Handler = handler;
    block->Ctx     = ctx;

    // Record only; UacpiHostConnectSci does the connect.
    UacpiHostSciBlock = block;
    UacpiHostSciGsi   = irq;
    *out_irq_handle   = block;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[uacpi] SCI handler recorded (gsi %u); connect deferred to IrqLib"
               " bring-up\n", irq);
    return UACPI_STATUS_OK;
}

NTSTATUS UacpiHostConnectSci(VOID)
{
    PUACPI_HOST_INTERRUPT block = UacpiHostSciBlock;
    ULONG     vector = 0, polarity = 0, mode = 0;
    KIRQL     irql = 0;
    KAFFINITY affinity = 0;
    NTSTATUS  status;

    if (block == NULL)
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[uacpi] UacpiHostConnectSci: no SCI handler recorded\n");
        return STATUS_SUCCESS;   // no SCI, not fatal
    }
    if (block->KInterrupt != NULL)
    {
        return STATUS_SUCCESS;   // already connected
    }

    // The deferral pool must exist before the line can fire.
    status = UacpiIsrDeferInitialize();
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = UacpiIrqLibResolveVector(UacpiHostSciGsi, &vector, &irql, &affinity,
                                      &polarity, &mode);
    if (!NT_SUCCESS(status) || vector == 0)
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[uacpi] SCI vector resolve for gsi %u failed 0x%X (vector 0x%X)\n",
                   UacpiHostSciGsi, status, vector);
        return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
    }

    block->Vector         = vector;
    block->Irql           = irql;
    block->LevelTriggered = (BOOLEAN)(mode == 0);

    // From here on the ISR can run; spinlocks must block it.
    UacpiHostSciLockIrql = (irql > DISPATCH_LEVEL) ? irql : DISPATCH_LEVEL;

    status = IoConnectInterrupt(&block->KInterrupt,
                                UacpiHostIsr,
                                block,
                                NULL,                       // uACPI has its own locks
                                vector,
                                irql,
                                irql,                       // SynchronizeIrql == DIRQL
                                (mode == 0) ? LevelSensitive : Latched,
                                TRUE,                       // shareable
                                affinity,
                                FALSE);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[uacpi] SCI connect (gsi %u -> vector 0x%X irql %u): 0x%X\n",
               UacpiHostSciGsi, vector, irql, status);
    return status;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(
    uacpi_interrupt_handler handler, uacpi_handle irq_handle)
{
    PUACPI_HOST_INTERRUPT block = (PUACPI_HOST_INTERRUPT)irq_handle;
    IO_DISCONNECT_INTERRUPT_PARAMETERS p;

    UNREFERENCED_PARAMETER(handler);

    if (block == NULL)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    if (block->KInterrupt != NULL)
    {
        RtlZeroMemory(&p, sizeof(p));
        p.Version = CONNECT_FULLY_SPECIFIED;
        p.ConnectionContext.InterruptObject = block->KInterrupt;
        IoDisconnectInterruptEx(&p);
        KeFlushQueuedDpcs();   // drain the deferral DPC before freeing
    }

    if (UacpiHostSciBlock == block)
    {
        UacpiHostSciBlock = NULL;
    }
    ExFreePoolWithTag(block, UACPI_HOST_TAG);
    return UACPI_STATUS_OK;
}
