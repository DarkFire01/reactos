#include <win32k.h>

#define NDEBUG
#include <debug.h>

/* Marker used to signal initialization/deletion in progress.
 * HSEMAPHORE is a pointer type (ERESOURCE*), so a small invalid value is safe.
 */
#define HSEM_IN_PROGRESS ((HSEMAPHORE)(ULONG_PTR)1)

static
VOID
EngpWaitForSafeSemaphoreReady(_In_ volatile HSEMAPHORE *SafeHsem)
{
    ULONG spinCount = 0;

    ASSERT_IRQL_LESS_OR_EQUAL(PASSIVE_LEVEL);

    while ((*SafeHsem == NULL) || (*SafeHsem == HSEM_IN_PROGRESS))
    {
        if (++spinCount < 1024)
        {
            YieldProcessor();
        }
        else
        {
            LARGE_INTEGER interval;

            /* Sleep 1ms to avoid pegging the CPU if another thread stalls. */
            interval.QuadPart = -(10 * 1000);
            (void)KeDelayExecutionThread(KernelMode, FALSE, &interval);
            spinCount = 0;
        }
    }
}

/*
 * @implemented
 */
__drv_allocatesMem(Mem)
_Post_writable_byte_size_(sizeof(ERESOURCE))
HSEMAPHORE
APIENTRY
EngCreateSemaphore(
    VOID)
{
    // www.osr.com/ddk/graphics/gdifncs_95lz.htm
    PERESOURCE psem = ExAllocatePoolWithTag(NonPagedPool,
                                            sizeof(ERESOURCE),
                                            GDITAG_SEMAPHORE);
    if (!psem)
        return NULL;

    if (!NT_SUCCESS(ExInitializeResourceLite(psem)))
    {
        ExFreePoolWithTag(psem, GDITAG_SEMAPHORE );
        return NULL;
    }

    return (HSEMAPHORE)psem;
}

/*
 * @implemented
 */
_Requires_lock_not_held_(*hsem)
_Acquires_exclusive_lock_(*hsem)
_Acquires_lock_(_Global_critical_region_)
VOID
APIENTRY
EngAcquireSemaphore(
    _Inout_ HSEMAPHORE hsem)
{
    // www.osr.com/ddk/graphics/gdifncs_14br.htm
    PTHREADINFO W32Thread;

    /* On Windows a NULL hsem is ignored */
    if (hsem == NULL)
    {
        DPRINT1("EngAcquireSemaphore called with hsem == NULL!\n");
        return;
    }

    ExEnterCriticalRegionAndAcquireResourceExclusive((PERESOURCE)hsem);
    W32Thread = PsGetThreadWin32Thread(PsGetCurrentThread());
    if (W32Thread) W32Thread->dwEngAcquireCount++;
}

/*
 * @implemented
 */
_Requires_lock_held_(*hsem)
_Releases_lock_(*hsem)
_Releases_lock_(_Global_critical_region_)
VOID
APIENTRY
EngReleaseSemaphore(
    _Inout_ HSEMAPHORE hsem)
{
    // www.osr.com/ddk/graphics/gdifncs_5u3r.htm
    PTHREADINFO W32Thread;

    /* On Windows a NULL hsem is ignored */
    if (hsem == NULL)
    {
        DPRINT1("EngReleaseSemaphore called with hsem == NULL!\n");
        return;
    }

    W32Thread = PsGetThreadWin32Thread(PsGetCurrentThread());
    if (W32Thread) --W32Thread->dwEngAcquireCount;
    ExReleaseResourceAndLeaveCriticalRegion((PERESOURCE)hsem);
}

_Acquires_lock_(_Global_critical_region_)
_Requires_lock_not_held_(*hsem)
_Acquires_shared_lock_(*hsem)
VOID
NTAPI
EngAcquireSemaphoreShared(
     _Inout_ HSEMAPHORE hsem)
{
    PTHREADINFO pti;

    ASSERT(hsem);
    ExEnterCriticalRegionAndAcquireResourceShared((PERESOURCE)hsem);
    pti = PsGetThreadWin32Thread(PsGetCurrentThread());
    if (pti) ++pti->dwEngAcquireCount;
}

/*
 * @implemented
 */
_Requires_lock_not_held_(*hsem)
VOID
APIENTRY
EngDeleteSemaphore(
    _Inout_ __drv_freesMem(Mem) HSEMAPHORE hsem)
{
    // www.osr.com/ddk/graphics/gdifncs_13c7.htm
    ASSERT(hsem);

    ExDeleteResourceLite((PERESOURCE)hsem);
    ExFreePoolWithTag((PVOID)hsem, GDITAG_SEMAPHORE);
}

/*
 * @implemented
 */
BOOL
APIENTRY
EngIsSemaphoreOwned(
    _In_ HSEMAPHORE hsem)
{
    // www.osr.com/ddk/graphics/gdifncs_6wmf.htm
    ASSERT(hsem);
    return (((PERESOURCE)hsem)->ActiveCount > 0);
}

/*
 * @implemented
 */
BOOL
APIENTRY
EngIsSemaphoreOwnedByCurrentThread(
    _In_ HSEMAPHORE hsem)
{
    // www.osr.com/ddk/graphics/gdifncs_9yxz.htm
    ASSERT(hsem);
    return ExIsResourceAcquiredExclusiveLite((PERESOURCE)hsem);
}

/*
 * @implemented
 */
BOOL
APIENTRY
EngInitializeSafeSemaphore(
    _Out_ ENGSAFESEMAPHORE *Semaphore)
{
    HSEMAPHORE hSem;
    HSEMAPHORE hCur;

    if (InterlockedIncrement(&Semaphore->lCount) == 1)
    {
        /* We are the initializer for this generation.
         * Acquire the initialization marker; if a previous-generation semaphore is
         * still being torn down, wait until it becomes NULL.
         */
        for (;;)
        {
            hCur = (HSEMAPHORE)InterlockedCompareExchangePointer((volatile PVOID *)&Semaphore->hsem,
                                                                (PVOID)HSEM_IN_PROGRESS,
                                                                NULL);
            if (hCur == NULL)
                break;

            /* Someone else is initializing/deleting, or an old semaphore is pending
             * deletion. Wait for it to finish.
             */
            EngpWaitForSafeSemaphoreReady((volatile HSEMAPHORE *)&Semaphore->hsem);

            /* If we got here, hsem is now a real semaphore; but since lCount is 1,
             * it must be from a previous generation that raced with deletion.
             * Wait until it is cleared and retry.
             */
            while (Semaphore->hsem != NULL)
                EngpWaitForSafeSemaphoreReady((volatile HSEMAPHORE *)&Semaphore->hsem);
        }

        /* Create the semaphore */
        hSem = EngCreateSemaphore();
        if (hSem == NULL)
        {
            (void)InterlockedExchangePointer((volatile PVOID *)&Semaphore->hsem, NULL);
            InterlockedDecrement(&Semaphore->lCount);
            return FALSE;
        }

        /* Publish it (replace the in-progress marker). */
        (void)InterlockedExchangePointer((volatile PVOID *)&Semaphore->hsem, hSem);
    }
    else
    {
        /* Wait for the other thread to create the semaphore */
        ASSERT(Semaphore->lCount > 1);
        EngpWaitForSafeSemaphoreReady((volatile HSEMAPHORE *)&Semaphore->hsem);
    }

    return TRUE;
}

/*
 * @implemented
 */
VOID
APIENTRY
EngDeleteSafeSemaphore(
    _Inout_ _Post_invalid_ ENGSAFESEMAPHORE *pssem)
{
    if (InterlockedDecrement(&pssem->lCount) == 0)
    {
        /* Block any concurrent initializers from observing a soon-to-be-deleted
         * semaphore pointer.
         */
        HSEMAPHORE hOld;

        hOld = (HSEMAPHORE)InterlockedExchangePointer((volatile PVOID *)&pssem->hsem,
                                                     (PVOID)HSEM_IN_PROGRESS);
        if ((hOld != NULL) && (hOld != HSEM_IN_PROGRESS))
        {
            EngDeleteSemaphore(hOld);
        }

        (void)InterlockedExchangePointer((volatile PVOID *)&pssem->hsem, NULL);
    }
}

/* EOF */
