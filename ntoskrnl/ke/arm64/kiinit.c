

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#undef KeAcquireSpinLock
#undef KeReleaseSpinLock

MMPTE ValidKernelPte = {0};


/* FUNCTIONS *****************************************************************/
VOID
NTAPI
KeFlushEntireTb(IN BOOLEAN Invalid,
                IN BOOLEAN AllProcessors)
{
}

VOID
NTAPI
KeFlushCurrentTb(VOID)
{
    //
}

ULONG
NTAPI
KeGetRecommendedSharedDataAlignment(VOID)
{
    /* Return the global variable */
    return 0;
}

VOID
NTAPI
KeFlushIoBuffers(
    _In_ PMDL Mdl,
    _In_ BOOLEAN ReadOperation,
    _In_ BOOLEAN DmaOperation)
{
    DbgBreakPoint();
}

BOOLEAN
NTAPI
KeConnectInterrupt(IN PKINTERRUPT Interrupt)
{
    return 0;
}

BOOLEAN
NTAPI
KeDisconnectInterrupt(IN PKINTERRUPT Interrupt)
{
    return 0;
}


/*
 * @implemented
 */
KIRQL
KeAcquireSpinLockRaiseToSynch(PKSPIN_LOCK SpinLock)
{
    KIRQL OldIrql;

    /* Raise to sync */
    KeRaiseIrql(SYNCH_LEVEL, &OldIrql);

    /* Acquire the lock and return */
    KxAcquireSpinLock(SpinLock);
    return OldIrql;
}

/*
 * @implemented
 */
KIRQL
NTAPI
KeAcquireSpinLockRaiseToDpc(PKSPIN_LOCK SpinLock)
{
    KIRQL OldIrql;

    /* Raise to dispatch */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    /* Acquire the lock and return */
    KxAcquireSpinLock(SpinLock);
    return OldIrql;
}

/*
 * @implemented
 */
VOID
NTAPI
KeReleaseSpinLock(PKSPIN_LOCK SpinLock,
                  KIRQL OldIrql)
{
    /* Release the lock and lower IRQL back */
    KxReleaseSpinLock(SpinLock);
    KeLowerIrql(OldIrql);
}

/*
 * @implemented
 */
KIRQL
KeAcquireQueuedSpinLock(IN KSPIN_LOCK_QUEUE_NUMBER LockNumber)
{
    KIRQL OldIrql;

    /* Raise to dispatch */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    /* Acquire the lock */
    KxAcquireSpinLock(KeGetCurrentPrcb()->LockQueue[LockNumber].Lock); // HACK
    return OldIrql;
}

/*
 * @implemented
 */
KIRQL
KeAcquireQueuedSpinLockRaiseToSynch(IN KSPIN_LOCK_QUEUE_NUMBER LockNumber)
{
    KIRQL OldIrql;

    /* Raise to synch */
    KeRaiseIrql(SYNCH_LEVEL, &OldIrql);

    /* Acquire the lock */
    KxAcquireSpinLock(KeGetCurrentPrcb()->LockQueue[LockNumber].Lock); // HACK
    return OldIrql;
}

/*
 * @implemented
 */
VOID
KeAcquireInStackQueuedSpinLock(IN PKSPIN_LOCK SpinLock,
                               IN PKLOCK_QUEUE_HANDLE LockHandle)
{
    /* Set up the lock */
    LockHandle->LockQueue.Next = NULL;
    LockHandle->LockQueue.Lock = SpinLock;

    /* Raise to dispatch */
    KeRaiseIrql(DISPATCH_LEVEL, &LockHandle->OldIrql);

    /* Acquire the lock */
    KxAcquireSpinLock(LockHandle->LockQueue.Lock); // HACK
}

BOOLEAN
NTAPI
KeInvalidateAllCaches(VOID)
{
    return FALSE;
}

NTSTATUS
NTAPI
NtVdmControl(IN ULONG ControlCode,
             IN PVOID ControlData)
{
    /* Not supported */
    return STATUS_NOT_IMPLEMENTED;
}

VOID
__cdecl
KeSaveStateForHibernate(IN PKPROCESSOR_STATE State)
{
}

NTSTATUS
NTAPI
KeUserModeCallback(
    IN ULONG RoutineIndex,
    IN PVOID Argument,
    IN ULONG ArgumentLength,
    OUT PVOID *Result,
    OUT PULONG ResultLength)
{
    return 1;
}

BOOLEAN
NTAPI
KeSynchronizeExecution(IN OUT PKINTERRUPT Interrupt,
                       IN PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
                       IN PVOID SynchronizeContext OPTIONAL)
{
    return 0;
}


VOID
NTAPI
KeSetDmaIoCoherency(IN ULONG Coherency)
{
    /* Save the coherency globally */
    KiDmaIoCoherency = Coherency;
}


NTSTATUS
NTAPI
KeRaiseUserException(IN NTSTATUS ExceptionCode)
{
    UNIMPLEMENTED;
    return STATUS_UNSUCCESSFUL;
}

KAFFINITY
NTAPI
KeQueryActiveProcessors(VOID)
{
    PAGED_CODE();

    /* Simply return the number of active processors */
    return KeActiveProcessors;
}

VOID
NTAPI
KeInitializeInterrupt(
    IN PKINTERRUPT Interrupt,
    IN PKSERVICE_ROUTINE ServiceRoutine,
    IN PVOID ServiceContext,
    IN PKSPIN_LOCK SpinLock,
    IN ULONG Vector,
    IN KIRQL Irql,
    IN KIRQL SynchronizeIrql,
    IN KINTERRUPT_MODE InterruptMode,
    IN BOOLEAN ShareVector,
    IN CHAR ProcessorNumber,
    IN BOOLEAN FloatingSave)
{

}

/*
 * @implemented
 */
VOID
KeAcquireInStackQueuedSpinLockRaiseToSynch(IN PKSPIN_LOCK SpinLock,
                                           IN PKLOCK_QUEUE_HANDLE LockHandle)
{
    /* Set up the lock */
    LockHandle->LockQueue.Next = NULL;
    LockHandle->LockQueue.Lock = SpinLock;

    /* Raise to synch */
    KeRaiseIrql(SYNCH_LEVEL, &LockHandle->OldIrql);

    /* Acquire the lock */
    KxAcquireSpinLock(LockHandle->LockQueue.Lock); // HACK
}


/*
 * @implemented
 */
VOID
KeReleaseQueuedSpinLock(IN KSPIN_LOCK_QUEUE_NUMBER LockNumber,
                        IN KIRQL OldIrql)
{
    /* Release the lock */
    KxReleaseSpinLock(KeGetCurrentPrcb()->LockQueue[LockNumber].Lock); // HACK

    /* Lower IRQL back */
    KeLowerIrql(OldIrql);
}


/*
 * @implemented
 */
VOID
KeReleaseInStackQueuedSpinLock(IN PKLOCK_QUEUE_HANDLE LockHandle)
{
    /* Simply lower IRQL back */
    KxReleaseSpinLock(LockHandle->LockQueue.Lock); // HACK
    KeLowerIrql(LockHandle->OldIrql);
}


/*
 * @implemented
 */
BOOLEAN
KeTryToAcquireQueuedSpinLockRaiseToSynch(IN KSPIN_LOCK_QUEUE_NUMBER LockNumber,
                                         IN PKIRQL OldIrql)
{
    /* Raise to synch level */
    KeRaiseIrql(SYNCH_LEVEL, OldIrql);

#ifdef CONFIG_SMP
    // HACK
    return KeTryToAcquireSpinLockAtDpcLevel(KeGetCurrentPrcb()->LockQueue[LockNumber].Lock);
#else
    /* Add an explicit memory barrier to prevent the compiler from reordering
       memory accesses across the borders of spinlocks */
    KeMemoryBarrierWithoutFence();

    /* Always return true on UP Machines */
    return TRUE;
#endif
}

/*
 * @implemented
 */
LOGICAL
KeTryToAcquireQueuedSpinLock(IN KSPIN_LOCK_QUEUE_NUMBER LockNumber,
                             OUT PKIRQL OldIrql)
{
    /* Raise to dispatch level */
    KeRaiseIrql(DISPATCH_LEVEL, OldIrql);

#ifdef CONFIG_SMP
    // HACK
    return KeTryToAcquireSpinLockAtDpcLevel(KeGetCurrentPrcb()->LockQueue[LockNumber].Lock);
#else

    /* Add an explicit memory barrier to prevent the compiler from reordering
       memory accesses across the borders of spinlocks */
    KeMemoryBarrierWithoutFence();

    /* Always return true on UP Machines */
    return TRUE;
#endif
}

/* EOF */


CODE_SEG("INIT")
DECLSPEC_NORETURN
VOID
NTAPI
KiSystemStartup(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    for(;;)
    {

    }
}
