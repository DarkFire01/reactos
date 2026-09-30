/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/config/cmnotify.c
 * PURPOSE:         Configuration Manager - Registry Change Notification
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include "ntoskrnl.h"
#define NDEBUG
#include "debug.h"

/* GLOBALS *******************************************************************/

#define TAG_CM_NOTIFY   'bnMC'
#define TAG_CM_POST     'bpMC'

/* PRIVATE FUNCTIONS *********************************************************/

static
VOID
CmpLockNotifies(VOID)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&CmpNotifyLock);
}

static
VOID
CmpUnlockNotifies(VOID)
{
    ExReleasePushLockExclusive(&CmpNotifyLock);
    KeLeaveCriticalRegion();
}

static
VOID
NTAPI
CmpFreePostBlock(
    _In_ PCM_POST_BLOCK PostBlock)
{
    if (PostBlock->UserEvent != NULL)
        ObDereferenceObject(PostBlock->UserEvent);

    /* A thread is torn down in more than a free, so never do it under the lock */
    if (PostBlock->Thread != NULL)
        ObDereferenceObjectDeferDelete(PostBlock->Thread);

    ExFreePoolWithTag(PostBlock, TAG_CM_POST);
}

/*
 * Runs in the thread that asked for the notification, which is the only place
 * the caller's status block can be reached.
 */
static
VOID
NTAPI
CmpPostApc(
    _In_ PKAPC Apc,
    _Inout_ PKNORMAL_ROUTINE *NormalRoutine,
    _Inout_ PVOID *NormalContext,
    _Inout_ PVOID *SystemArgument1,
    _Inout_ PVOID *SystemArgument2)
{
    PCM_POST_BLOCK PostBlock;
    PIO_STATUS_BLOCK IoStatusBlock;

    UNREFERENCED_PARAMETER(NormalContext);

    PostBlock = CONTAINING_RECORD(Apc, CM_POST_BLOCK, Apc);
    IoStatusBlock = PostBlock->IoStatusBlock;

    if (IoStatusBlock != NULL)
    {
        _SEH2_TRY
        {
            IoStatusBlock->Status = PostBlock->Status;
            IoStatusBlock->Information = 0;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            /* The caller threw its own status block away, so there is nowhere to write */
        }
        _SEH2_END;
    }

    if (PostBlock->UserEvent != NULL)
        KeSetEvent(PostBlock->UserEvent, IO_NO_INCREMENT, FALSE);

    /* A user routine is handed the status block the way an I/O completion hands it over */
    if (*NormalRoutine != NULL)
    {
        *SystemArgument1 = IoStatusBlock;
        *SystemArgument2 = NULL;
    }

    CmpFreePostBlock(PostBlock);
}

static
VOID
NTAPI
CmpPostApcRunDown(
    _In_ PKAPC Apc)
{
    CmpFreePostBlock(CONTAINING_RECORD(Apc, CM_POST_BLOCK, Apc));
}

static
PCM_POST_BLOCK
NTAPI
CmpAllocatePostBlock(
    _In_ POST_BLOCK_TYPE NotifyType,
    _In_opt_ PKEVENT UserEvent,
    _In_opt_ PIO_APC_ROUTINE ApcRoutine,
    _In_opt_ PVOID ApcContext,
    _In_opt_ PIO_STATUS_BLOCK IoStatusBlock,
    _In_ KPROCESSOR_MODE ApcMode)
{
    PCM_POST_BLOCK PostBlock;

    PostBlock = ExAllocatePoolZero(NonPagedPool,
                                   sizeof(*PostBlock),
                                   TAG_CM_POST);
    if (PostBlock == NULL)
        return NULL;

    PostBlock->NotifyType = NotifyType;
    PostBlock->UserEvent = UserEvent;
    PostBlock->IoStatusBlock = IoStatusBlock;
    InitializeListHead(&PostBlock->NotifyList);
    InitializeListHead(&PostBlock->ThreadList);

    if (NotifyType == PostSynchronous)
    {
        KeInitializeEvent(&PostBlock->SystemEvent, NotificationEvent, FALSE);
        return PostBlock;
    }

    if (NotifyType == PostAsyncUser)
    {
        PostBlock->Thread = PsGetCurrentThread();
        ObReferenceObject(PostBlock->Thread);
        KeInitializeApc(&PostBlock->Apc,
                        &PostBlock->Thread->Tcb,
                        OriginalApcEnvironment,
                        CmpPostApc,
                        CmpPostApcRunDown,
                        (PKNORMAL_ROUTINE)ApcRoutine,
                        (ApcRoutine != NULL) ? ApcMode : KernelMode,
                        ApcContext);
    }

    return PostBlock;
}

/* Is the change at Kcb inside what this block asked to be told about? */
static
BOOLEAN
NTAPI
CmpNotifyCoversKey(
    _In_ PCM_NOTIFY_BLOCK NotifyBlock,
    _In_ PCM_KEY_CONTROL_BLOCK Kcb)
{
    PCM_KEY_CONTROL_BLOCK Walk;

    if (NotifyBlock->KeyControlBlock == Kcb)
        return TRUE;

    if (!NotifyBlock->WatchTree)
        return FALSE;

    /* Nothing above the change can sit deeper than it does */
    if (NotifyBlock->KeyControlBlock->TotalLevels > Kcb->TotalLevels)
        return FALSE;

    for (Walk = Kcb->ParentKcb; Walk != NULL; Walk = Walk->ParentKcb)
    {
        if (Walk == NotifyBlock->KeyControlBlock)
            return TRUE;
    }

    return FALSE;
}

/* Hands the block over and leaves it with nothing pending. The lock must be held. */
static
VOID
NTAPI
CmpPostNotifyBlock(
    _In_ PCM_NOTIFY_BLOCK NotifyBlock,
    _In_ NTSTATUS Status)
{
    PCM_POST_BLOCK PostBlock;
    PLIST_ENTRY Entry;

    NotifyBlock->NotifyPending = FALSE;

    while (!IsListEmpty(&NotifyBlock->PostList))
    {
        Entry = RemoveHeadList(&NotifyBlock->PostList);
        PostBlock = CONTAINING_RECORD(Entry, CM_POST_BLOCK, NotifyList);
        InitializeListHead(&PostBlock->NotifyList);

        /*
         * Only a block that asked to be told by an APC was ever put on the list
         * the thread keeps, and taking an entry out of no list at all is not the
         * same as taking it out of an empty one: an entry pointing at itself is
         * what this kernel treats as a list that has been trodden on.
         */
        if (!IsListEmpty(&PostBlock->ThreadList))
        {
            RemoveEntryList(&PostBlock->ThreadList);
            InitializeListHead(&PostBlock->ThreadList);
        }

        PostBlock->Status = Status;

        switch (PostBlock->NotifyType)
        {
            case PostSynchronous:

                /* The waiter owns the block from here on, so do not touch it again */
                KeSetEvent(&PostBlock->SystemEvent, IO_NO_INCREMENT, FALSE);
                break;

            case PostAsyncUser:

                if (!KeInsertQueueApc(&PostBlock->Apc, NULL, NULL, IO_NO_INCREMENT))
                {
                    /* The thread is on its way out and will never run it */
                    CmpFreePostBlock(PostBlock);
                }
                break;

            case PostAsyncKernel:

                if (PostBlock->UserEvent != NULL)
                    KeSetEvent(PostBlock->UserEvent, IO_NO_INCREMENT, FALSE);

                CmpFreePostBlock(PostBlock);
                break;
        }
    }
}

/* PUBLIC FUNCTIONS **********************************************************/

/**
 * @brief
 * Tells everyone watching a key, or a tree the key sits in, that it changed.
 */
VOID
NTAPI
CmpReportNotify(
    IN PCM_KEY_CONTROL_BLOCK Kcb,
    IN PHHIVE Hive,
    IN HCELL_INDEX Cell,
    IN ULONG Filter)
{
    PCMHIVE CmHive = CONTAINING_RECORD(Hive, CMHIVE, Hive);
    PCM_NOTIFY_BLOCK NotifyBlock;
    PLIST_ENTRY Entry;

    UNREFERENCED_PARAMETER(Cell);

    /*
     * A key appearing or going away is a change to the key above it, since that
     * is where anyone interested in the set of names is watching.
     */
    if ((Filter == REG_NOTIFY_CHANGE_NAME) && (Kcb->ParentKcb != NULL))
        Kcb = Kcb->ParentKcb;

    CmpLockNotifies();

    /* A hive nobody has ever watched has no list to walk */
    if (CmHive->NotifyList.Flink != NULL)
    {
        for (Entry = CmHive->NotifyList.Flink;
             Entry != &CmHive->NotifyList;
             Entry = Entry->Flink)
        {
            NotifyBlock = CONTAINING_RECORD(Entry, CM_NOTIFY_BLOCK, HiveList);

            if (!(NotifyBlock->Filter & Filter))
                continue;

            if (!CmpNotifyCoversKey(NotifyBlock, Kcb))
                continue;

            if (IsListEmpty(&NotifyBlock->PostList))
            {
                /* Nobody is asking yet, so hold it for whoever asks next */
                NotifyBlock->NotifyPending = TRUE;
                continue;
            }

            CmpPostNotifyBlock(NotifyBlock, STATUS_NOTIFY_ENUM_DIR);
        }
    }

    CmpUnlockNotifies();
}

/**
 * @brief
 * Drops the notification a key body carries, completing anything still waiting
 * on it with the cleanup status.
 */
VOID
NTAPI
CmpFlushNotify(
    IN PCM_KEY_BODY KeyBody,
    IN BOOLEAN LockHeld)
{
    PCM_NOTIFY_BLOCK NotifyBlock;

    UNREFERENCED_PARAMETER(LockHeld);

    CmpLockNotifies();

    NotifyBlock = KeyBody->NotifyBlock;
    if (NotifyBlock != NULL)
    {
        KeyBody->NotifyBlock = NULL;
        NotifyBlock->KeyBody = NULL;
        RemoveEntryList(&NotifyBlock->HiveList);
        CmpPostNotifyBlock(NotifyBlock, STATUS_NOTIFY_CLEANUP);
    }

    CmpUnlockNotifies();

    if (NotifyBlock != NULL)
        ExFreePoolWithTag(NotifyBlock, TAG_CM_NOTIFY);
}

/**
 * @brief
 * Throws away whatever a dying thread was still owed. An asynchronous request
 * belongs to the thread that made it unless it asked to outlive it.
 */
VOID
NTAPI
CmNotifyRunDown(
    _In_ PETHREAD Thread)
{
    PCM_POST_BLOCK PostBlock;
    PLIST_ENTRY Entry;

    if (IsListEmpty(&Thread->PostBlockList))
        return;

    CmpLockNotifies();

    while (!IsListEmpty(&Thread->PostBlockList))
    {
        Entry = RemoveHeadList(&Thread->PostBlockList);
        PostBlock = CONTAINING_RECORD(Entry, CM_POST_BLOCK, ThreadList);
        InitializeListHead(&PostBlock->ThreadList);

        /* The same the other way round, for one already handed over */
        if (!IsListEmpty(&PostBlock->NotifyList))
        {
            RemoveEntryList(&PostBlock->NotifyList);
            InitializeListHead(&PostBlock->NotifyList);
        }

        CmpFreePostBlock(PostBlock);
    }

    CmpUnlockNotifies();
}

/**
 * @brief
 * Arranges for a key, and if asked the tree under it, to report its next change.
 *
 * @param[in] KeyHandle
 * The key to watch, opened for KEY_NOTIFY.
 *
 * @param[in] EventHandle
 * Event signalled when the change happens. Required for an asynchronous request.
 *
 * @param[in] ApcRoutine
 * Routine called once the change has been reported, or NULL.
 *
 * @param[in] ApcContext
 * Passed to ApcRoutine.
 *
 * @param[out] IoStatusBlock
 * Receives the status the request completed with.
 *
 * @param[in] CompletionFilter
 * The kinds of change worth reporting.
 *
 * @param[in] WatchTree
 * Whether keys below the one being watched count as well.
 *
 * @param[in] Asynchronous
 * TRUE returns at once, FALSE blocks until something changes.
 *
 * @return
 * STATUS_PENDING for an asynchronous request, the completion status for a
 * synchronous one, or a failure code.
 */
NTSTATUS
NTAPI
CmpNotifyChangeKey(
    _In_ HANDLE KeyHandle,
    _In_opt_ HANDLE EventHandle,
    _In_opt_ PIO_APC_ROUTINE ApcRoutine,
    _In_opt_ PVOID ApcContext,
    _Out_ PIO_STATUS_BLOCK IoStatusBlock,
    _In_ ULONG CompletionFilter,
    _In_ BOOLEAN WatchTree,
    _In_ BOOLEAN Asynchronous)
{
    KPROCESSOR_MODE PreviousMode;
    POST_BLOCK_TYPE NotifyType;
    PCM_KEY_BODY KeyBody = NULL;
    PKEVENT EventObject = NULL;
    PCM_NOTIFY_BLOCK NotifyBlock;
    PCM_POST_BLOCK PostBlock;
    PCM_KEY_CONTROL_BLOCK Kcb;
    PCMHIVE CmHive;
    NTSTATUS Status;

    PAGED_CODE();

    if ((CompletionFilter == 0) ||
        (CompletionFilter & ~REG_LEGAL_CHANGE_FILTER))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Without an event there would be no way to hear about it later */
    if (Asynchronous && (EventHandle == NULL))
        return STATUS_INVALID_PARAMETER;

    /* A request that outlives its thread has to be told about by the event alone */
    if ((CompletionFilter & REG_NOTIFY_THREAD_AGNOSTIC) && !Asynchronous)
        return STATUS_INVALID_PARAMETER;

    PreviousMode = ExGetPreviousMode();
    if (PreviousMode != KernelMode)
    {
        _SEH2_TRY
        {
            ProbeForWriteIoStatusBlock(IoStatusBlock);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    Status = ObReferenceObjectByHandle(KeyHandle,
                                       KEY_NOTIFY,
                                       CmpKeyObjectType,
                                       PreviousMode,
                                       (PVOID*)&KeyBody,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    if (EventHandle != NULL)
    {
        Status = ObReferenceObjectByHandle(EventHandle,
                                           EVENT_MODIFY_STATE,
                                           ExEventObjectType,
                                           PreviousMode,
                                           (PVOID*)&EventObject,
                                           NULL);
        if (!NT_SUCCESS(Status))
        {
            ObDereferenceObject(KeyBody);
            return Status;
        }
    }

    if (!Asynchronous)
        NotifyType = PostSynchronous;
    else if (CompletionFilter & REG_NOTIFY_THREAD_AGNOSTIC)
        NotifyType = PostAsyncKernel;
    else
        NotifyType = PostAsyncUser;

    PostBlock = CmpAllocatePostBlock(NotifyType,
                                     EventObject,
                                     ApcRoutine,
                                     ApcContext,
                                     IoStatusBlock,
                                     PreviousMode);
    if (PostBlock == NULL)
    {
        if (EventObject != NULL)
            ObDereferenceObject(EventObject);

        ObDereferenceObject(KeyBody);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* The event is the post block's from here on, however this turns out */
    EventObject = NULL;

    CmpLockRegistry();
    CmpLockNotifies();

    Kcb = KeyBody->KeyControlBlock;
    NotifyBlock = KeyBody->NotifyBlock;

    if (Kcb->Delete)
    {
        Status = STATUS_KEY_DELETED;
    }
    else if (NotifyBlock != NULL)
    {
        /* The handle is already watching, so this request joins what it watches */
        Status = STATUS_SUCCESS;
    }
    else
    {
        NotifyBlock = ExAllocatePoolZero(NonPagedPool,
                                        sizeof(*NotifyBlock),
                                        TAG_CM_NOTIFY);
        if (NotifyBlock == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            NotifyBlock->KeyControlBlock = Kcb;
            NotifyBlock->KeyBody = KeyBody;
            NotifyBlock->Filter = CompletionFilter;
            NotifyBlock->WatchTree = WatchTree ? 1 : 0;
            InitializeListHead(&NotifyBlock->PostList);

            CmHive = CONTAINING_RECORD(Kcb->KeyHive, CMHIVE, Hive);
            if (CmHive->NotifyList.Flink == NULL)
                InitializeListHead(&CmHive->NotifyList);

            InsertTailList(&CmHive->NotifyList, &NotifyBlock->HiveList);
            KeyBody->NotifyBlock = NotifyBlock;
            Status = STATUS_SUCCESS;
        }
    }

    if (!NT_SUCCESS(Status))
    {
        CmpUnlockNotifies();
        CmpUnlockRegistry();
        CmpFreePostBlock(PostBlock);
        ObDereferenceObject(KeyBody);
        return Status;
    }

    InsertTailList(&NotifyBlock->PostList, &PostBlock->NotifyList);

    if (NotifyType == PostAsyncUser)
        InsertTailList(&PsGetCurrentThread()->PostBlockList, &PostBlock->ThreadList);

    /*
     * A change that arrived while nothing was waiting was held back rather than
     * dropped, so hand it over now instead of waiting for the next one.
     */
    if (NotifyBlock->NotifyPending)
        CmpPostNotifyBlock(NotifyBlock, STATUS_NOTIFY_ENUM_DIR);

    CmpUnlockNotifies();
    CmpUnlockRegistry();

    if (Asynchronous)
    {
        ObDereferenceObject(KeyBody);
        return STATUS_PENDING;
    }

    /* Nothing can change while the registry is locked, so it was let go first */
    KeWaitForSingleObject(&PostBlock->SystemEvent,
                          Executive,
                          KernelMode,
                          FALSE,
                          NULL);

    Status = PostBlock->Status;

    /* A synchronous request may still have handed in an event to signal */
    if (PostBlock->UserEvent != NULL)
        KeSetEvent(PostBlock->UserEvent, IO_NO_INCREMENT, FALSE);

    CmpFreePostBlock(PostBlock);
    ObDereferenceObject(KeyBody);

    _SEH2_TRY
    {
        IoStatusBlock->Status = Status;
        IoStatusBlock->Information = 0;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    return Status;
}

/* EOF */
