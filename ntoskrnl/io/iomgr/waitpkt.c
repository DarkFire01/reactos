/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Turning a signalled object into a completion on a port
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A thread pool that is already reading completions off a port would rather
 * wait on an event the same way. A wait completion packet is what stands
 * between the two: it is told a port, an object and what to post, and when the
 * object is signalled the post happens with no thread of the caller's waiting
 * for it.
 *
 * Nothing here can hang a waiter off a dispatcher object without a thread, so
 * the waiting is done by threads of this file's own. Each takes as many
 * objects as one wait can hold, so the threads are counted in packets by the
 * dozen rather than one each.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* PRIVATE DEFINITIONS ********************************************************/

#define TAG_WAIT_GROUP 'GpwI'

/* One slot of a wait goes to being told that the rest of it has changed */
#define IOP_PACKETS_PER_GROUP (MAXIMUM_WAIT_OBJECTS - 1)

typedef struct _IOP_WAIT_GROUP
{
    LIST_ENTRY Link;
    LIST_ENTRY Packets;
    ULONG Count;
    KEVENT Rescan;
} IOP_WAIT_GROUP, *PIOP_WAIT_GROUP;

typedef struct _IOP_WAIT_PACKET
{
    LIST_ENTRY GroupLink;
    PIOP_WAIT_GROUP Group;
    /* Told apart from whatever takes this packet's place at the same address */
    ULONG Sequence;
    PVOID IoCompletion;
    PVOID TargetObject;
    PVOID TargetWait;
    PVOID KeyContext;
    PVOID ApcContext;
    NTSTATUS IoStatus;
    ULONG_PTR IoStatusInformation;
    BOOLEAN Associated;
    BOOLEAN Posted;
} IOP_WAIT_PACKET, *PIOP_WAIT_PACKET;

/* GLOBALS ********************************************************************/

POBJECT_TYPE IoWaitCompletionPacketType = NULL;

static LIST_ENTRY IopWaitGroupList;
static FAST_MUTEX IopWaitGroupLock;
static ULONG IopWaitPacketSequence = 0;
static BOOLEAN IopWaitGroupsReady = FALSE;

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief
 * Takes a packet off whatever is waiting for it and gives back what the
 * association was holding.
 *
 * @remarks
 * The caller holds the group lock. The reference the association took is
 * dropped last, so a packet nobody else holds goes away here.
 */
static
VOID
IopDetachWaitPacket(
    _Inout_ PIOP_WAIT_PACKET Packet)
{
    PVOID IoCompletion = Packet->IoCompletion;
    PVOID TargetObject = Packet->TargetObject;

    RemoveEntryList(&Packet->GroupLink);
    Packet->Group->Count--;

    Packet->Group = NULL;
    Packet->IoCompletion = NULL;
    Packet->TargetObject = NULL;
    Packet->TargetWait = NULL;
    Packet->Associated = FALSE;

    if (IoCompletion != NULL)
        ObDereferenceObject(IoCompletion);

    if (TargetObject != NULL)
        ObDereferenceObject(TargetObject);

    /* The last one of these can be the caller's, so it goes without the lock */
    ObDereferenceObjectDeferDelete(Packet);
}

/**
 * @brief
 * Puts what a packet was told to post onto its port, and has done with it.
 *
 * @remarks
 * The caller holds the group lock.
 */
static
VOID
IopPostWaitPacket(
    _Inout_ PIOP_WAIT_PACKET Packet)
{
    IoSetIoCompletion(Packet->IoCompletion,
                      Packet->KeyContext,
                      Packet->ApcContext,
                      Packet->IoStatus,
                      Packet->IoStatusInformation,
                      FALSE);

    Packet->Posted = TRUE;

    IopDetachWaitPacket(Packet);
}

/**
 * @brief
 * Waits on everything one group was given, and posts for whichever of them
 * comes up.
 *
 * @remarks
 * The list can change under this, so what is waited on is a copy taken while
 * the lock is held, with a reference of its own on every object in it. The
 * first slot is how the group says the copy is out of date.
 */
static
VOID
NTAPI
IopWaitPacketThread(
    _In_ PVOID Context)
{
    PIOP_WAIT_GROUP Group = Context;
    PVOID Objects[MAXIMUM_WAIT_OBJECTS];
    PVOID Waits[MAXIMUM_WAIT_OBJECTS];
    PIOP_WAIT_PACKET Packets[MAXIMUM_WAIT_OBJECTS];
    ULONG Sequences[MAXIMUM_WAIT_OBJECTS];
    KWAIT_BLOCK Blocks[MAXIMUM_WAIT_OBJECTS];
    PLIST_ENTRY Entry;
    ULONG Count = 1;
    ULONG Index;
    NTSTATUS Status;

    for (;;)
    {
        /* Let go of the last look before taking another */
        while (Count > 1)
        {
            Count--;
            ObDereferenceObject(Objects[Count]);
        }

        ExAcquireFastMutex(&IopWaitGroupLock);

        KeClearEvent(&Group->Rescan);
        Waits[0] = &Group->Rescan;
        Count = 1;

        for (Entry = Group->Packets.Flink;
             Entry != &Group->Packets;
             Entry = Entry->Flink)
        {
            PIOP_WAIT_PACKET Packet =
                CONTAINING_RECORD(Entry, IOP_WAIT_PACKET, GroupLink);

            ObReferenceObject(Packet->TargetObject);

            Objects[Count] = Packet->TargetObject;
            Waits[Count] = Packet->TargetWait;
            Packets[Count] = Packet;
            Sequences[Count] = Packet->Sequence;
            Count++;
        }

        ExReleaseFastMutex(&IopWaitGroupLock);

        Status = KeWaitForMultipleObjects(Count,
                                          Waits,
                                          WaitAny,
                                          Executive,
                                          KernelMode,
                                          FALSE,
                                          NULL,
                                          Blocks);

        /*
         * An abandoned mutant satisfies a wait as much as a signalled event
         * does, and taking it the same way is what keeps this from waiting on
         * it again the moment it goes round.
         */
        if ((ULONG)(Status - STATUS_ABANDONED_WAIT_0) < Count)
            Index = (ULONG)(Status - STATUS_ABANDONED_WAIT_0);
        else
            Index = (ULONG)(Status - STATUS_WAIT_0);

        if (Index == 0 || Index >= Count)
            continue;

        /*
         * Whoever the wait was satisfied for may have been taken away while it
         * was being waited on, so it counts only if it is still on the list
         * under the same name it was copied under.
         */
        ExAcquireFastMutex(&IopWaitGroupLock);

        for (Entry = Group->Packets.Flink;
             Entry != &Group->Packets;
             Entry = Entry->Flink)
        {
            PIOP_WAIT_PACKET Packet =
                CONTAINING_RECORD(Entry, IOP_WAIT_PACKET, GroupLink);

            if (Packet != Packets[Index] || Packet->Sequence != Sequences[Index])
                continue;

            IopPostWaitPacket(Packet);
            break;
        }

        ExReleaseFastMutex(&IopWaitGroupLock);
    }
}

/**
 * @brief
 * Finds a group with room for one more packet.
 *
 * @remarks
 * The caller holds the group lock. A group is never taken down: an empty one
 * costs a thread parked on its own event, and the next packet wants it.
 */
static
PIOP_WAIT_GROUP
IopFindWaitGroup(VOID)
{
    PLIST_ENTRY Entry;

    for (Entry = IopWaitGroupList.Flink;
         Entry != &IopWaitGroupList;
         Entry = Entry->Flink)
    {
        PIOP_WAIT_GROUP Group = CONTAINING_RECORD(Entry, IOP_WAIT_GROUP, Link);

        if (Group->Count < IOP_PACKETS_PER_GROUP)
            return Group;
    }

    return NULL;
}

/**
 * @brief
 * Starts another group and puts it where the next packet will find it.
 *
 * @remarks
 * The caller holds no lock, since making a thread cannot be done under one.
 */
static
NTSTATUS
IopAddWaitGroup(VOID)
{
    PIOP_WAIT_GROUP Group;
    HANDLE Thread;
    NTSTATUS Status;

    Group = ExAllocatePoolZero(NonPagedPool, sizeof(*Group), TAG_WAIT_GROUP);
    if (Group == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    InitializeListHead(&Group->Packets);
    KeInitializeEvent(&Group->Rescan, NotificationEvent, FALSE);

    Status = PsCreateSystemThread(&Thread,
                                  THREAD_ALL_ACCESS,
                                  NULL,
                                  NULL,
                                  NULL,
                                  IopWaitPacketThread,
                                  Group);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Group, TAG_WAIT_GROUP);
        return Status;
    }

    ObCloseHandle(Thread, KernelMode);

    ExAcquireFastMutex(&IopWaitGroupLock);
    InsertTailList(&IopWaitGroupList, &Group->Link);
    ExReleaseFastMutex(&IopWaitGroupLock);

    return STATUS_SUCCESS;
}

/* INTERNAL FUNCTIONS *********************************************************/

/**
 * @brief
 * Gives back what a packet still holds when the last handle to it goes.
 */
VOID
NTAPI
IopDeleteWaitCompletionPacket(
    _In_ PVOID Object)
{
    /*
     * A packet that is waiting for something is held by the group waiting for
     * it, so nothing that gets here is still on one.
     */
    UNREFERENCED_PARAMETER(Object);
}

/**
 * @brief
 * Prepares what the wait completion packets are kept on.
 */
CODE_SEG("INIT")
VOID
NTAPI
IopInitWaitCompletionPackets(VOID)
{
    InitializeListHead(&IopWaitGroupList);
    ExInitializeFastMutex(&IopWaitGroupLock);
    IopWaitGroupsReady = TRUE;
}

/* SYSTEM CALLS ***************************************************************/

/**
 * @brief
 * Makes something a caller can name a pending post by.
 */
NTSTATUS
NTAPI
NtCreateWaitCompletionPacket(
    _Out_ PHANDLE WaitCompletionPacketHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_opt_ POBJECT_ATTRIBUTES ObjectAttributes)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    PIOP_WAIT_PACKET Packet;
    HANDLE Handle;
    NTSTATUS Status;

    PAGED_CODE();

    if (PreviousMode != KernelMode)
    {
        _SEH2_TRY
        {
            ProbeForWriteHandle(WaitCompletionPacketHandle);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    Status = ObCreateObject(PreviousMode,
                            IoWaitCompletionPacketType,
                            ObjectAttributes,
                            PreviousMode,
                            NULL,
                            sizeof(*Packet),
                            0,
                            0,
                            (PVOID *)&Packet);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(Packet, sizeof(*Packet));

    Status = ObInsertObject(Packet,
                            NULL,
                            DesiredAccess,
                            0,
                            NULL,
                            &Handle);
    if (!NT_SUCCESS(Status))
        return Status;

    _SEH2_TRY
    {
        *WaitCompletionPacketHandle = Handle;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    return Status;
}

/**
 * @brief
 * Tells a packet what to post, where to post it, and what has to happen first.
 *
 * @param AlreadySignaled
 * Takes whether the object had already happened, in which case the post is
 * done by the time this returns.
 */
NTSTATUS
NTAPI
NtAssociateWaitCompletionPacket(
    _In_ HANDLE WaitCompletionPacketHandle,
    _In_ HANDLE IoCompletionHandle,
    _In_ HANDLE TargetObjectHandle,
    _In_opt_ PVOID KeyContext,
    _In_opt_ PVOID ApcContext,
    _In_ NTSTATUS IoStatus,
    _In_ ULONG_PTR IoStatusInformation,
    _Out_opt_ PBOOLEAN AlreadySignaled)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    PIOP_WAIT_PACKET Packet;
    PVOID IoCompletion;
    PVOID TargetObject;
    PVOID TargetWait;
    PIOP_WAIT_GROUP Group;
    LARGE_INTEGER NoWait;
    BOOLEAN Signaled = FALSE;
    NTSTATUS Status;

    PAGED_CODE();

    if (!IopWaitGroupsReady)
        return STATUS_DEVICE_NOT_READY;

    if (PreviousMode != KernelMode && AlreadySignaled != NULL)
    {
        _SEH2_TRY
        {
            ProbeForWriteBoolean(AlreadySignaled);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    Status = ObReferenceObjectByHandle(WaitCompletionPacketHandle,
                                       0,
                                       IoWaitCompletionPacketType,
                                       PreviousMode,
                                       (PVOID *)&Packet,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ObReferenceObjectByHandle(IoCompletionHandle,
                                       IO_COMPLETION_MODIFY_STATE,
                                       IoCompletionType,
                                       PreviousMode,
                                       &IoCompletion,
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(Packet);
        return Status;
    }

    Status = ObReferenceObjectByHandle(TargetObjectHandle,
                                       SYNCHRONIZE,
                                       NULL,
                                       PreviousMode,
                                       &TargetObject,
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(IoCompletion);
        ObDereferenceObject(Packet);
        return Status;
    }

    /* What of the object the kernel waits on, which for most of them is itself */
    TargetWait = OBJECT_TO_OBJECT_HEADER(TargetObject)->Type->DefaultObject;
    if (IsPointerOffset(TargetWait))
        TargetWait = (PVOID)((ULONG_PTR)TargetObject + (ULONG_PTR)TargetWait);

    /*
     * Whether it has happened already is asked here rather than under the
     * lock, since asking is a wait of no length and a wait of any kind is not
     * for a thread that has raised itself to hold something.
     */
    NoWait.QuadPart = 0;
    Signaled = (KeWaitForSingleObject(TargetWait,
                                      Executive,
                                      KernelMode,
                                      FALSE,
                                      &NoWait) == STATUS_SUCCESS);

    if (Signaled)
    {
        ExAcquireFastMutex(&IopWaitGroupLock);

        if (Packet->Associated)
        {
            ExReleaseFastMutex(&IopWaitGroupLock);
            ObDereferenceObject(TargetObject);
            ObDereferenceObject(IoCompletion);
            ObDereferenceObject(Packet);
            return STATUS_INVALID_PARAMETER_1;
        }

        Packet->Posted = TRUE;
        Packet->Sequence = ++IopWaitPacketSequence;

        IoSetIoCompletion(IoCompletion,
                          KeyContext,
                          ApcContext,
                          IoStatus,
                          IoStatusInformation,
                          FALSE);

        ExReleaseFastMutex(&IopWaitGroupLock);

        ObDereferenceObject(TargetObject);
        ObDereferenceObject(IoCompletion);
        ObDereferenceObject(Packet);
        goto Answer;
    }

    /* Somewhere to wait from, found before the packet is told anything */
    for (;;)
    {
        ExAcquireFastMutex(&IopWaitGroupLock);

        Group = IopFindWaitGroup();
        if (Group != NULL)
            break;

        ExReleaseFastMutex(&IopWaitGroupLock);

        Status = IopAddWaitGroup();
        if (!NT_SUCCESS(Status))
        {
            ObDereferenceObject(TargetObject);
            ObDereferenceObject(IoCompletion);
            ObDereferenceObject(Packet);
            return Status;
        }
    }

    if (Packet->Associated)
    {
        ExReleaseFastMutex(&IopWaitGroupLock);
        ObDereferenceObject(TargetObject);
        ObDereferenceObject(IoCompletion);
        ObDereferenceObject(Packet);
        return STATUS_INVALID_PARAMETER_1;
    }

    Packet->IoCompletion = IoCompletion;
    Packet->TargetObject = TargetObject;
    Packet->TargetWait = TargetWait;
    Packet->KeyContext = KeyContext;
    Packet->ApcContext = ApcContext;
    Packet->IoStatus = IoStatus;
    Packet->IoStatusInformation = IoStatusInformation;
    Packet->Posted = FALSE;
    Packet->Sequence = ++IopWaitPacketSequence;

    /* The group holds the packet for as long as it is waiting for it */
    ObReferenceObject(Packet);

    Packet->Group = Group;
    Packet->Associated = TRUE;
    InsertTailList(&Group->Packets, &Packet->GroupLink);
    Group->Count++;

    KeSetEvent(&Group->Rescan, IO_NO_INCREMENT, FALSE);

    ExReleaseFastMutex(&IopWaitGroupLock);

    ObDereferenceObject(Packet);

Answer:

    if (AlreadySignaled != NULL)
    {
        _SEH2_TRY
        {
            *AlreadySignaled = Signaled;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
    }

    return Status;
}

/**
 * @brief
 * Takes back what a packet was told to wait for.
 *
 * @return
 * STATUS_CANCELLED for a packet that was never told anything, and
 * STATUS_PENDING for one whose post is already on the port, since nothing here
 * takes a completion back off one.
 */
NTSTATUS
NTAPI
NtCancelWaitCompletionPacket(
    _In_ HANDLE WaitCompletionPacketHandle,
    _In_ BOOLEAN RemoveSignaledPacket)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    PIOP_WAIT_PACKET Packet;
    NTSTATUS Status;

    PAGED_CODE();

    Status = ObReferenceObjectByHandle(WaitCompletionPacketHandle,
                                       0,
                                       IoWaitCompletionPacketType,
                                       PreviousMode,
                                       (PVOID *)&Packet,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    ExAcquireFastMutex(&IopWaitGroupLock);

    if (Packet->Associated)
    {
        IopDetachWaitPacket(Packet);
        Status = STATUS_SUCCESS;
    }
    else if (Packet->Posted)
    {
        Status = RemoveSignaledPacket ? STATUS_PENDING : STATUS_SUCCESS;
    }
    else
    {
        Status = STATUS_CANCELLED;
    }

    ExReleaseFastMutex(&IopWaitGroupLock);

    ObDereferenceObject(Packet);

    return Status;
}

/* EOF */
