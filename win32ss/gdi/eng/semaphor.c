#include <win32k.h>

#define NDEBUG
#include <debug.h>

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

/**
 * @brief
 * Acquires a semaphore exclusively, but only if nobody holds it.
 *
 * @param[in,out] hsem
 * The semaphore.
 *
 * @return
 * TRUE when it was acquired.
 */
BOOL
APIENTRY
EngAcquireSemaphoreNoWait(
    _Inout_ HSEMAPHORE hsem)
{
    PTHREADINFO W32Thread;

    /* On Windows a NULL hsem is ignored */
    if (hsem == NULL)
        return FALSE;

    KeEnterCriticalRegion();
    if (!ExAcquireResourceExclusiveLite((PERESOURCE)hsem, FALSE))
    {
        KeLeaveCriticalRegion();
        return FALSE;
    }

    W32Thread = PsGetThreadWin32Thread(PsGetCurrentThread());
    if (W32Thread) W32Thread->dwEngAcquireCount++;
    return TRUE;
}

/**
 * @brief
 * Acquires a semaphore for shared access, but only if nobody holds it exclusively.
 *
 * @param[in,out] hsem
 * The semaphore.
 *
 * @return
 * TRUE when it was acquired.
 */
BOOL
APIENTRY
EngAcquireSemaphoreSharedNoWait(
    _Inout_ HSEMAPHORE hsem)
{
    PTHREADINFO W32Thread;

    if (hsem == NULL)
        return FALSE;

    KeEnterCriticalRegion();
    if (!ExAcquireResourceSharedLite((PERESOURCE)hsem, FALSE))
    {
        KeLeaveCriticalRegion();
        return FALSE;
    }

    W32Thread = PsGetThreadWin32Thread(PsGetCurrentThread());
    if (W32Thread) W32Thread->dwEngAcquireCount++;
    return TRUE;
}

/**
 * @brief
 * Tells whether the calling thread holds a semaphore for shared access.
 *
 * @param[in] hsem
 * The semaphore.
 *
 * @return
 * TRUE when this thread holds it shared.
 */
BOOL
APIENTRY
EngIsSemaphoreSharedByCurrentThread(
    _In_ HSEMAPHORE hsem)
{
    ASSERT(hsem);
    return (ExIsResourceAcquiredSharedLite((PERESOURCE)hsem) != 0);
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

    if (InterlockedIncrement(&Semaphore->lCount) == 1)
    {
        /* Create the semaphore */
        hSem = EngCreateSemaphore();
        if (hSem == 0)
        {
            InterlockedDecrement(&Semaphore->lCount);
            return FALSE;
        }
        /* FIXME: Not thread-safe! Check result of InterlockedCompareExchangePointer
                  and delete semaphore if already initialized! */
        (void)InterlockedExchangePointer((volatile PVOID *)&Semaphore->hsem, hSem);
    }
    else
    {
        /* Wait for the other thread to create the semaphore */
        ASSERT(Semaphore->lCount > 1);
        ASSERT_IRQL_LESS_OR_EQUAL(PASSIVE_LEVEL);
        while (Semaphore->hsem == NULL);
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
        /* FIXME: Not thread-safe! Use result of InterlockedCompareExchangePointer! */
        EngDeleteSemaphore(pssem->hsem);
        (void)InterlockedExchangePointer((volatile PVOID *)&pssem->hsem, NULL);
    }
}

/* EOF */
