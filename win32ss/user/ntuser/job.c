/*
 * PROJECT:     ReactOS Win32k Subsystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Job object UI restrictions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A job object may restrict what the processes it contains are allowed to do
 * to the user interface (JOB_OBJECT_UILIMIT_*). The kernel stores the mask,
 * but it is win32k that has to honour it, so the kernel hands each restricted
 * job over to us through a callout (see PspInvokeW32JobCallout).
 *
 * For every such job we keep a JOBINFO, which owns the list of USER-connected
 * processes of the job, the handles that have been explicitly granted to it
 * (see NtUserUserHandleGrantAccess) and, for JOB_OBJECT_UILIMIT_GLOBALATOMS,
 * a private global atom table.
 *
 * Locking: the kernel acquires the job lock before invoking a callout, and the
 * callout then takes the USER lock. To keep that the only order in which the
 * two are ever held, win32k never acquires the job lock itself: gJobInfoList
 * and everything hanging off it is protected by the USER lock alone.
 */

#include <win32k.h>

DBG_DEFAULT_CHANNEL(UserProcess);

/* GLOBALS ********************************************************************/

static PJOBINFO gJobInfoList = NULL;

/* PRIVATE FUNCTIONS **********************************************************/

/*
 * Looks up the JOBINFO of a kernel job object.
 * The USER lock must be held.
 */
static
PJOBINFO
IntFindJobInfo(_In_ PEJOB pEJob)
{
    PJOBINFO pJobInfo;

    for (pJobInfo = gJobInfoList; pJobInfo != NULL; pJobInfo = pJobInfo->Next)
    {
        if (pJobInfo->pEJob == pEJob)
            return pJobInfo;
    }

    return NULL;
}

/*
 * Creates the JOBINFO of a kernel job object and links it into the global list.
 * The USER lock must be held.
 */
static
PJOBINFO
IntCreateJobInfo(_In_ PEJOB pEJob)
{
    PJOBINFO pJobInfo;

    pJobInfo = ExAllocatePoolZero(PagedPool, sizeof(JOBINFO), USERTAG_W32JOB);
    if (pJobInfo == NULL)
    {
        ERR("Failed to allocate a JOBINFO for job %p\n", pEJob);
        return NULL;
    }

    pJobInfo->pEJob = pEJob;

    pJobInfo->Next = gJobInfoList;
    gJobInfoList = pJobInfo;

    TRACE("Created JOBINFO %p for job %p\n", pJobInfo, pEJob);
    return pJobInfo;
}

/*
 * Marks or unmarks a process, and every thread it has, as belonging to a job
 * that restricts the UI.
 *
 * Enforcement sites test these instead of walking back to the job on every
 * call, and user32 sees the thread flag through the CLIENTINFO it shares with
 * us, so a restricted call can be refused without a syscall at all.
 *
 * The USER lock must be held.
 */
static
VOID
IntSetProcessRestricted(
    _In_ PPROCESSINFO ppi,
    _In_ BOOL bRestricted)
{
    PTHREADINFO pti;
    KAPC_STATE ApcState;
    BOOL bAttached = FALSE;

    ASSERT(UserIsEnteredExclusive());

    if (bRestricted)
        ppi->W32PF_flags |= W32PF_JOBRESTRICTED;
    else
        ppi->W32PF_flags &= ~W32PF_JOBRESTRICTED;

    /* A process on its way out is losing its threads and its address space,
       so there is nothing left that marking them could be good for */
    if (ppi->W32PF_flags & W32PF_TERMINATED)
        return;

    /* The CLIENTINFO of a thread lives in the address space of its own
       process, which is not necessarily the one we are running in */
    if (ppi->peProcess != NULL && ppi != PsGetCurrentProcessWin32Process())
    {
        KeStackAttachProcess(&ppi->peProcess->Pcb, &ApcState);
        bAttached = TRUE;
    }

    for (pti = ppi->ptiList; pti != NULL; pti = pti->ptiSibling)
    {
        if (bRestricted)
            pti->TIF_flags |= TIF_JOBRESTRICTED;
        else
            pti->TIF_flags &= ~TIF_JOBRESTRICTED;

        _SEH2_TRY
        {
            if (pti->pClientInfo != NULL)
                pti->pClientInfo->dwTIFlags = pti->TIF_flags;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            /* The thread is going away; what it can no longer do does not
               matter, and the kernel side copy of the flags still stands */
            (void)0;
        }
        _SEH2_END;
    }

    if (bAttached)
        KeUnstackDetachProcess(&ApcState);
}

/*
 * Unlinks and frees a JOBINFO, detaching any process still pointing at it.
 * The USER lock must be held.
 */
static
VOID
IntDestroyJobInfo(_In_ PJOBINFO pJobInfo)
{
    PJOBINFO *ppJobInfo;
    ULONG i;

    /* Unlink it first, so that nothing can find it any more */
    for (ppJobInfo = &gJobInfoList; *ppJobInfo != NULL; ppJobInfo = &(*ppJobInfo)->Next)
    {
        if (*ppJobInfo == pJobInfo)
        {
            *ppJobInfo = pJobInfo->Next;
            break;
        }
    }

    /* Detach the processes that are still around */
    for (i = 0; i < pJobInfo->ProcessCount; i++)
    {
        if (pJobInfo->pProcesses[i] != NULL)
        {
            IntSetProcessRestricted(pJobInfo->pProcesses[i], FALSE);
            pJobInfo->pProcesses[i]->pJobInfo = NULL;
        }
    }

    if (pJobInfo->pProcesses != NULL)
        ExFreePoolWithTag(pJobInfo->pProcesses, USERTAG_W32JOBEXTRA);

    if (pJobInfo->pGrantedHandles != NULL)
        ExFreePoolWithTag(pJobInfo->pGrantedHandles, USERTAG_W32JOBEXTRA);

    if (pJobInfo->pAtomTable != NULL)
        RtlDestroyAtomTable(pJobInfo->pAtomTable);

    TRACE("Destroyed JOBINFO %p\n", pJobInfo);
    ExFreePoolWithTag(pJobInfo, USERTAG_W32JOB);
}

/*
 * Grows one of the arrays of a job geometrically.
 * Returns the new array, or NULL when out of memory (the old one is kept).
 */
static
PVOID
IntGrowJobArray(
    _In_opt_ PVOID pOldArray,
    _In_ ULONG Used,
    _Inout_ PULONG Max,
    _In_ ULONG EntrySize)
{
    PVOID pNewArray;
    ULONG NewMax;

    NewMax = (*Max == 0) ? 4 : (*Max * 2);

    pNewArray = ExAllocatePoolZero(PagedPool, NewMax * EntrySize, USERTAG_W32JOBEXTRA);
    if (pNewArray == NULL)
        return NULL;

    if (pOldArray != NULL)
    {
        RtlCopyMemory(pNewArray, pOldArray, Used * EntrySize);
        ExFreePoolWithTag(pOldArray, USERTAG_W32JOBEXTRA);
    }

    *Max = NewMax;
    return pNewArray;
}

/*
 * Adds a USER-connected process to a job.
 * The USER lock must be held.
 */
static
NTSTATUS
IntAddProcessToJobInfo(
    _In_ PJOBINFO pJobInfo,
    _In_ PPROCESSINFO ppi)
{
    ULONG i;

    /* Already a member? */
    for (i = 0; i < pJobInfo->ProcessCount; i++)
    {
        if (pJobInfo->pProcesses[i] == ppi)
            return STATUS_SUCCESS;
    }

    if (pJobInfo->ProcessCount >= pJobInfo->ProcessCountMax)
    {
        PPROCESSINFO *pProcesses;

        pProcesses = IntGrowJobArray(pJobInfo->pProcesses,
                                     pJobInfo->ProcessCount,
                                     &pJobInfo->ProcessCountMax,
                                     sizeof(PPROCESSINFO));
        if (pProcesses == NULL)
            return STATUS_NO_MEMORY;

        pJobInfo->pProcesses = pProcesses;
    }

    pJobInfo->pProcesses[pJobInfo->ProcessCount++] = ppi;
    ppi->pJobInfo = pJobInfo;
    IntSetProcessRestricted(ppi, TRUE);

    TRACE("Process %p joined JOBINFO %p (restrictions 0x%lx)\n",
          ppi, pJobInfo, pJobInfo->UIRestrictions);

    return STATUS_SUCCESS;
}

/*
 * Applies a new restriction mask to a job, creating its JOBINFO on first use.
 */
static
NTSTATUS
IntJobSetRestrictions(
    _In_ PEJOB pEJob,
    _In_ ULONG UIRestrictions)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PJOBINFO pJobInfo;
    BOOL Created = FALSE;

    UserEnterExclusive();

    pJobInfo = IntFindJobInfo(pEJob);

    /* Dropping all restrictions: the job no longer needs any state here */
    if (UIRestrictions == 0)
    {
        if (pJobInfo != NULL)
            IntDestroyJobInfo(pJobInfo);

        goto Quit;
    }

    if (pJobInfo == NULL)
    {
        PPROCESSINFO ppi;

        pJobInfo = IntCreateJobInfo(pEJob);
        if (pJobInfo == NULL)
        {
            Status = STATUS_NO_MEMORY;
            goto Quit;
        }

        Created = TRUE;

        /* The job may already contain processes that connected to USER before
           the restrictions were applied. Collect them now. */
        for (ppi = gppiList; ppi != NULL; ppi = ppi->ppiNext)
        {
            if (ppi->peProcess != NULL &&
                PsGetProcessJob(ppi->peProcess) == pEJob)
            {
                Status = IntAddProcessToJobInfo(pJobInfo, ppi);
                if (!NT_SUCCESS(Status))
                    goto Quit;
            }
        }
    }

    /* A private atom table is only needed while GLOBALATOMS is restricted */
    if (UIRestrictions & JOB_OBJECT_UILIMIT_GLOBALATOMS)
    {
        if (pJobInfo->pAtomTable == NULL)
        {
            Status = RtlCreateAtomTable(37, &pJobInfo->pAtomTable);
            if (!NT_SUCCESS(Status))
            {
                ERR("Failed to create the atom table of job %p: 0x%lx\n", pEJob, Status);
                goto Quit;
            }
        }
    }
    else if (pJobInfo->pAtomTable != NULL)
    {
        RtlDestroyAtomTable(pJobInfo->pAtomTable);
        pJobInfo->pAtomTable = NULL;
    }

    pJobInfo->UIRestrictions = UIRestrictions;

Quit:
    /* Leave nothing behind if we could not finish what we just started */
    if (!NT_SUCCESS(Status) && Created)
        IntDestroyJobInfo(pJobInfo);

    UserLeave();
    return Status;
}

/*
 * Handles PsW32JobCalloutAddProcess.
 */
static
NTSTATUS
IntJobAddProcess(
    _In_ PEJOB pEJob,
    _In_ PPROCESSINFO ppi)
{
    NTSTATUS Status;
    PJOBINFO pJobInfo;

    UserEnterExclusive();

    pJobInfo = IntFindJobInfo(pEJob);
    if (pJobInfo == NULL)
    {
        /* The kernel only calls us for jobs that have restrictions, so we
           should have been told about this job already. */
        ERR("No JOBINFO for job %p\n", pEJob);
        Status = STATUS_UNSUCCESSFUL;
    }
    else
    {
        Status = IntAddProcessToJobInfo(pJobInfo, ppi);
    }

    UserLeave();
    return Status;
}

/*
 * Handles PsW32JobCalloutTerminate. The job object is going away.
 */
static
NTSTATUS
IntJobTerminate(_In_ PEJOB pEJob)
{
    PJOBINFO pJobInfo;

    UserEnterExclusive();

    pJobInfo = IntFindJobInfo(pEJob);
    if (pJobInfo != NULL)
        IntDestroyJobInfo(pJobInfo);

    UserLeave();
    return STATUS_SUCCESS;
}

/*
 * Tests whether a USER handle has been explicitly granted to a job.
 * The USER lock must be held.
 */
static
BOOL
IntIsHandleGrantedToJob(
    _In_ PJOBINFO pJobInfo,
    _In_ HANDLE hUserHandle)
{
    ULONG i;

    for (i = 0; i < pJobInfo->GrantedHandleCount; i++)
    {
        if (pJobInfo->pGrantedHandles[i] == hUserHandle)
            return TRUE;
    }

    return FALSE;
}

/*
 * Grants or revokes access to a USER handle for the processes of a job that is
 * otherwise restricted by JOB_OBJECT_UILIMIT_HANDLES.
 * The USER lock must be held.
 */
static
BOOL
IntGrantHandleToJob(
    _In_ PJOBINFO pJobInfo,
    _In_ HANDLE hUserHandle,
    _In_ BOOL bGrant)
{
    ULONG i;

    if (!bGrant)
    {
        for (i = 0; i < pJobInfo->GrantedHandleCount; i++)
        {
            if (pJobInfo->pGrantedHandles[i] == hUserHandle)
            {
                /* Keep the array dense */
                pJobInfo->pGrantedHandles[i] =
                    pJobInfo->pGrantedHandles[pJobInfo->GrantedHandleCount - 1];
                pJobInfo->pGrantedHandles[pJobInfo->GrantedHandleCount - 1] = NULL;
                pJobInfo->GrantedHandleCount--;
                return TRUE;
            }
        }

        /* Revoking an access that was never granted is not an error. Note
           that the handle is deliberately not validated here: a handle that
           has since been destroyed must still be removable from the list. */
        return TRUE;
    }

    /* Only a live USER handle can be granted. Without this the caller could
       seed the list with handle values it does not own and wait for one of
       them to be handed out to somebody else. */
    if (!UserMarkHandleGranted(hUserHandle))
    {
        EngSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (IntIsHandleGrantedToJob(pJobInfo, hUserHandle))
        return TRUE;

    if (pJobInfo->GrantedHandleCount >= pJobInfo->GrantedHandleCountMax)
    {
        HANDLE *pGrantedHandles;

        pGrantedHandles = IntGrowJobArray(pJobInfo->pGrantedHandles,
                                          pJobInfo->GrantedHandleCount,
                                          &pJobInfo->GrantedHandleCountMax,
                                          sizeof(HANDLE));
        if (pGrantedHandles == NULL)
        {
            EngSetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }

        pJobInfo->pGrantedHandles = pGrantedHandles;
    }

    pJobInfo->pGrantedHandles[pJobInfo->GrantedHandleCount++] = hUserHandle;
    return TRUE;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * The entry point of the kernel into the job support of win32k.
 * Called with the job lock held and without the USER lock.
 */
NTSTATUS
NTAPI
Win32kJobCallout(_In_ PWIN32_JOBCALLOUT_PARAMETERS Parameters)
{
    switch (Parameters->CalloutType)
    {
        case PsW32JobCalloutSetInformation:
            return IntJobSetRestrictions((PEJOB)Parameters->Job,
                                         PtrToUlong(Parameters->Data));

        case PsW32JobCalloutAddProcess:
            return IntJobAddProcess((PEJOB)Parameters->Job,
                                    (PPROCESSINFO)Parameters->Data);

        case PsW32JobCalloutTerminate:
            return IntJobTerminate((PEJOB)Parameters->Job);

        default:
            ERR("Unknown job callout %d\n", Parameters->CalloutType);
            return STATUS_INVALID_PARAMETER;
    }
}

/*
 * Called when a process connects to USER. A process is usually assigned to its
 * job while it is still suspended, long before it ever calls into win32k, so
 * this is where restricted processes normally join their job.
 *
 * The USER lock must be held. We deliberately do not take the job lock here,
 * as that would invert the order the callout path establishes; the caller has
 * already linked ppi into gppiList, so restrictions applied concurrently pick
 * this process up in IntJobSetRestrictions instead.
 */
NTSTATUS
FASTCALL
IntJobConnectProcess(_In_ PPROCESSINFO ppi)
{
    PJOBINFO pJobInfo;
    PEJOB pEJob;

    ASSERT(UserIsEnteredExclusive());

    pEJob = PsGetProcessJob(ppi->peProcess);
    if (pEJob == NULL)
        return STATUS_SUCCESS;

    /* Unrestricted jobs have no state in win32k */
    if (PsGetJobUIRestrictionsClass(pEJob) == 0)
        return STATUS_SUCCESS;

    pJobInfo = IntFindJobInfo(pEJob);
    if (pJobInfo == NULL)
    {
        /* Either the restrictions have not reached us yet, or they are being
           torn down. The callout handles both, so there is nothing to do. */
        return STATUS_SUCCESS;
    }

    return IntAddProcessToJobInfo(pJobInfo, ppi);
}

/*
 * Called when a process disconnects from USER.
 * The USER lock must be held.
 */
VOID
FASTCALL
IntJobDisconnectProcess(_In_ PPROCESSINFO ppi)
{
    PJOBINFO pJobInfo;
    ULONG i;

    ASSERT(UserIsEnteredExclusive());

    pJobInfo = ppi->pJobInfo;
    if (pJobInfo == NULL)
        return;

    for (i = 0; i < pJobInfo->ProcessCount; i++)
    {
        if (pJobInfo->pProcesses[i] == ppi)
        {
            /* Keep the array dense */
            pJobInfo->pProcesses[i] = pJobInfo->pProcesses[pJobInfo->ProcessCount - 1];
            pJobInfo->pProcesses[pJobInfo->ProcessCount - 1] = NULL;
            pJobInfo->ProcessCount--;
            break;
        }
    }

    IntSetProcessRestricted(ppi, FALSE);
    ppi->pJobInfo = NULL;
}

/*
 * Grants or revokes access to a USER handle for a job whose processes are
 * restricted by JOB_OBJECT_UILIMIT_HANDLES.
 */
BOOL
APIENTRY
NtUserUserHandleGrantAccess(
    IN HANDLE hUserHandle,
    IN HANDLE hJob,
    IN BOOL bGrant)
{
    PEJOB pEJob;
    PJOBINFO pJobInfo;
    NTSTATUS Status;
    BOOL Ret = FALSE;

    if (hUserHandle == NULL)
    {
        EngSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    Status = ObReferenceObjectByHandle(hJob,
                                       JOB_OBJECT_SET_ATTRIBUTES,
                                       PsJobType,
                                       UserMode,
                                       (PVOID*)&pEJob,
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        SetLastNtError(Status);
        return FALSE;
    }

    UserEnterExclusive();

    pJobInfo = IntFindJobInfo(pEJob);
    if (pJobInfo == NULL)
    {
        /* Only a job that restricts USER handles keeps a granted list */
        EngSetLastError(ERROR_INVALID_PARAMETER);
    }
    else
    {
        Ret = IntGrantHandleToJob(pJobInfo, hUserHandle, bGrant);
    }

    UserLeave();

    ObDereferenceObject(pEJob);
    return Ret;
}

/* EOF */
