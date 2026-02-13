/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/io/iomgr/irq.c
 * PURPOSE:         I/O Wrappers (called Completion Ports) for Kernel Queues
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

typedef struct _IOP_MESSAGE_INTERRUPT_CONTEXT
{
    PKMESSAGE_SERVICE_ROUTINE MessageServiceRoutine;
    PVOID ServiceContext;
    ULONG MessageId;
} IOP_MESSAGE_INTERRUPT_CONTEXT, *PIOP_MESSAGE_INTERRUPT_CONTEXT;

static
BOOLEAN
NTAPI
IopMessageInterruptServiceRoutine(
    _In_ struct _KINTERRUPT *Interrupt,
    _In_ PVOID ServiceContext)
{
    PIOP_MESSAGE_INTERRUPT_CONTEXT Ctx = (PIOP_MESSAGE_INTERRUPT_CONTEXT)ServiceContext;

    if (!Ctx || !Ctx->MessageServiceRoutine)
        return FALSE;

    return Ctx->MessageServiceRoutine(Interrupt, Ctx->ServiceContext, Ctx->MessageId);
}

static
NTSTATUS
IopQueryTranslatedInterrupt(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PULONG Vector,
    _Out_ PKIRQL Irql,
    _Out_ PKAFFINITY Affinity,
    _Out_ PBOOLEAN IsMessageInterrupt,
    _Out_opt_ PUSHORT OutFlags,
    _Out_opt_ PUCHAR OutShareDisposition)
{
    PDEVICE_NODE DeviceNode;
    PCM_RESOURCE_LIST Translated;

    if (!PhysicalDeviceObject || !Vector || !Irql || !Affinity || !IsMessageInterrupt)
        return STATUS_INVALID_PARAMETER;

    *Vector = 0;
    *Irql = 0;
    *Affinity = 0;
    *IsMessageInterrupt = FALSE;
    if (OutFlags) *OutFlags = 0;
    if (OutShareDisposition) *OutShareDisposition = CmResourceShareUndetermined;

    DeviceNode = IopGetDeviceNode(PhysicalDeviceObject);
    if (!DeviceNode || !DeviceNode->ResourceListTranslated)
        return STATUS_NOT_FOUND;

    Translated = DeviceNode->ResourceListTranslated;
    for (ULONG f = 0; f < Translated->Count; f++)
    {
        PCM_FULL_RESOURCE_DESCRIPTOR Full = &Translated->List[f];
        PCM_PARTIAL_RESOURCE_LIST Partial = &Full->PartialResourceList;

        for (ULONG p = 0; p < Partial->Count; p++)
        {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = &Partial->PartialDescriptors[p];
            if (Desc->Type != CmResourceTypeInterrupt)
                continue;

            if (Desc->Flags & CM_RESOURCE_INTERRUPT_MESSAGE)
            {
                *IsMessageInterrupt = TRUE;
                *Vector = Desc->u.MessageInterrupt.Translated.Vector;
                *Irql = (KIRQL)Desc->u.MessageInterrupt.Translated.Level;
                *Affinity = Desc->u.MessageInterrupt.Translated.Affinity;
            }
            else
            {
                *IsMessageInterrupt = FALSE;
                *Vector = Desc->u.Interrupt.Vector;
                *Irql = (KIRQL)Desc->u.Interrupt.Level;
                *Affinity = Desc->u.Interrupt.Affinity;
            }

            if (OutFlags) *OutFlags = Desc->Flags;
            if (OutShareDisposition) *OutShareDisposition = Desc->ShareDisposition;

            if (*Vector)
                return STATUS_SUCCESS;
        }
    }

    return STATUS_NOT_FOUND;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
IoConnectInterrupt(OUT PKINTERRUPT *InterruptObject,
                   IN PKSERVICE_ROUTINE ServiceRoutine,
                   IN PVOID ServiceContext,
                   IN PKSPIN_LOCK SpinLock,
                   IN ULONG Vector,
                   IN KIRQL Irql,
                   IN KIRQL SynchronizeIrql,
                   IN KINTERRUPT_MODE InterruptMode,
                   IN BOOLEAN ShareVector,
                   IN KAFFINITY ProcessorEnableMask,
                   IN BOOLEAN FloatingSave)
{
    PKINTERRUPT Interrupt;
    PKINTERRUPT InterruptUsed;
    PIO_INTERRUPT IoInterrupt;
    BOOLEAN FirstRun;
    CCHAR Count = 0;
    KAFFINITY Affinity;

    PAGED_CODE();

    /* Assume failure */
    *InterruptObject = NULL;

    /* Get the affinity */
    Affinity = ProcessorEnableMask & KeActiveProcessors;
    while (Affinity)
    {
        /* Increase count */
        if (Affinity & 1) Count++;
        Affinity >>= 1;
    }

    /* Make sure we have a valid CPU count */
    if (!Count) return STATUS_INVALID_PARAMETER;

    /* Allocate the array of I/O interrupts */
    IoInterrupt = ExAllocatePoolZero(NonPagedPool,
                                     (Count - 1) * sizeof(KINTERRUPT) +
                                     sizeof(IO_INTERRUPT),
                                     TAG_IO_INTERRUPT);
    if (!IoInterrupt) return STATUS_INSUFFICIENT_RESOURCES;

    /* Use the structure's spinlock, if none was provided */
    if (!SpinLock)
    {
        SpinLock = &IoInterrupt->SpinLock;
        KeInitializeSpinLock(SpinLock);
    }

    /* We first start with a built-in interrupt inside the I/O structure */
    Interrupt = (PKINTERRUPT)(IoInterrupt + 1);
    FirstRun = TRUE;

    /* Now create all the interrupts */
    Affinity = ProcessorEnableMask & KeActiveProcessors;
    for (Count = 0; Affinity; Count++, Affinity >>= 1)
    {
        /* Check if it's enabled for this CPU */
        if (!(Affinity & 1))
            continue;

        /* Check which one we will use */
        InterruptUsed = FirstRun ? &IoInterrupt->FirstInterrupt : Interrupt;

        /* Initialize it */
        KeInitializeInterrupt(InterruptUsed,
                              ServiceRoutine,
                              ServiceContext,
                              SpinLock,
                              Vector,
                              Irql,
                              SynchronizeIrql,
                              InterruptMode,
                              ShareVector,
                              Count,
                              FloatingSave);

        /* Connect it */
        if (!KeConnectInterrupt(InterruptUsed))
        {
            /* Check how far we got */
            if (FirstRun)
            {
                /* We failed early so just free this */
                ExFreePoolWithTag(IoInterrupt, TAG_IO_INTERRUPT);
            }
            else
            {
                /* Far enough, so disconnect everything */
                IoDisconnectInterrupt(&IoInterrupt->FirstInterrupt);
            }

            /* And fail */
            return STATUS_INVALID_PARAMETER;
        }

        /* Now we've used up our First Run */
        if (FirstRun)
        {
            FirstRun = FALSE;
        }
        else
        {
            /* Move on to the next one */
            IoInterrupt->Interrupt[(UCHAR)Count] = Interrupt++;
        }
    }

    /* Return success */
    *InterruptObject = &IoInterrupt->FirstInterrupt;
    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
VOID
NTAPI
IoDisconnectInterrupt(PKINTERRUPT InterruptObject)
{
    PIO_INTERRUPT IoInterrupt;
    ULONG i;

    PAGED_CODE();

    /* Get the I/O interrupt */
    IoInterrupt = CONTAINING_RECORD(InterruptObject,
                                    IO_INTERRUPT,
                                    FirstInterrupt);

    /* Disconnect the first one */
    KeDisconnectInterrupt(&IoInterrupt->FirstInterrupt);

    /* Now disconnect the others */
    for (i = 0; i < KeNumberProcessors; i++)
    {
        /* Make sure one was registered */
        if (!IoInterrupt->Interrupt[i])
            continue;

        /* Disconnect it */
        KeDisconnectInterrupt(IoInterrupt->Interrupt[i]);
    }

    /* Free the I/O interrupt */
    ExFreePoolWithTag(IoInterrupt, TAG_IO_INTERRUPT);
}

NTSTATUS
IopConnectInterruptExFullySpecific(
    _Inout_ PIO_CONNECT_INTERRUPT_PARAMETERS Parameters)
{
    NTSTATUS Status;

    PAGED_CODE();

    /* Fallback to standard IoConnectInterrupt */
    Status = IoConnectInterrupt(Parameters->FullySpecified.InterruptObject,
                                Parameters->FullySpecified.ServiceRoutine,
                                Parameters->FullySpecified.ServiceContext,
                                Parameters->FullySpecified.SpinLock,
                                Parameters->FullySpecified.Vector,
                                Parameters->FullySpecified.Irql,
                                Parameters->FullySpecified.SynchronizeIrql,
                                Parameters->FullySpecified.InterruptMode,
                                Parameters->FullySpecified.ShareVector,
                                Parameters->FullySpecified.ProcessorEnableMask,
                                Parameters->FullySpecified.FloatingSave);
    if (!NT_SUCCESS(Status))
        DPRINT1("IopConnectInterruptExFullySpecific() failed: 0x%lx\n", Status);
    return Status;
}

NTSTATUS
NTAPI
IoConnectInterruptEx(
    _Inout_ PIO_CONNECT_INTERRUPT_PARAMETERS Parameters)
{
    PAGED_CODE();

    if (!Parameters)
        return STATUS_INVALID_PARAMETER;

    switch (Parameters->Version)
    {
        case CONNECT_FULLY_SPECIFIED:
            return IopConnectInterruptExFullySpecific(Parameters);
        case CONNECT_FULLY_SPECIFIED_GROUP:
            //TODO: We don't do anything for the group type
            return IopConnectInterruptExFullySpecific(Parameters);
        case CONNECT_MESSAGE_BASED:
        {
            PIO_CONNECT_INTERRUPT_MESSAGE_BASED_PARAMETERS P = &Parameters->MessageBased;
            PIO_INTERRUPT_MESSAGE_INFO Table;
            PIOP_MESSAGE_INTERRUPT_CONTEXT Ctx;
            SIZE_T Size;
            ULONG Vector;
            KIRQL Irql;
            KAFFINITY Affinity;
            BOOLEAN IsMsg;
            USHORT Flags;
            UCHAR ShareDisposition;
            KIRQL SyncIrql;
            NTSTATUS Status;

            Status = IopQueryTranslatedInterrupt(P->PhysicalDeviceObject,
                                                 &Vector,
                                                 &Irql,
                                                 &Affinity,
                                                 &IsMsg,
                                                 &Flags,
                                                 &ShareDisposition);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("IoConnectInterruptEx(CONNECT_MESSAGE_BASED): no translated interrupt resource: 0x%lx\n", Status);
                return Status;
            }

            /*
             * Vista+ behavior: if the interrupt isn't message-signaled but the
             * caller provided a fallback ISR, connect a line-based interrupt instead.
             * KMDF relies on this behavior.
             */
            if (!IsMsg)
            {
                if (!P->FallBackServiceRoutine)
                {
                    DPRINT1("IoConnectInterruptEx(CONNECT_MESSAGE_BASED): no message interrupt and no fallback ISR\n");
                    return STATUS_NOT_FOUND;
                }

                SyncIrql = P->SynchronizeIrql ? P->SynchronizeIrql : Irql;
                Status = IoConnectInterrupt(P->ConnectionContext.InterruptObject,
                                            P->FallBackServiceRoutine,
                                            P->ServiceContext,
                                            P->SpinLock,
                                            Vector,
                                            Irql,
                                            SyncIrql,
                                            (Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? Latched : LevelSensitive,
                                            (ShareDisposition == CmResourceShareShared),
                                            Affinity ? Affinity : KeActiveProcessors,
                                            P->FloatingSave);
                return Status;
            }

            /* Minimal implementation: single-message only. */
            Size = sizeof(*Table) + sizeof(*Ctx);
            Table = ExAllocatePoolZero(NonPagedPool, Size, TAG_IO_INTERRUPT);
            if (!Table)
                return STATUS_INSUFFICIENT_RESOURCES;

            Ctx = (PIOP_MESSAGE_INTERRUPT_CONTEXT)((PUCHAR)Table + sizeof(*Table));
            Ctx->MessageServiceRoutine = P->MessageServiceRoutine;
            Ctx->ServiceContext = P->ServiceContext;
            Ctx->MessageId = 0;

            Table->UnifiedIrql = Irql;
            Table->MessageCount = 1;
            Table->MessageInfo[0].Vector = Vector;
            Table->MessageInfo[0].Irql = Irql;
            Table->MessageInfo[0].Mode = (Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? Latched : LevelSensitive;
            Table->MessageInfo[0].Polarity = InterruptPolarityUnknown;
            Table->MessageInfo[0].TargetProcessorSet = Affinity ? Affinity : KeActiveProcessors;
            Table->MessageInfo[0].MessageAddress.QuadPart = 0;
            Table->MessageInfo[0].MessageData = 0;

            SyncIrql = P->SynchronizeIrql ? P->SynchronizeIrql : Irql;

            Status = IoConnectInterrupt(&Table->MessageInfo[0].InterruptObject,
                                        IopMessageInterruptServiceRoutine,
                                        Ctx,
                                        P->SpinLock,
                                        Vector,
                                        Irql,
                                        SyncIrql,
                                        Table->MessageInfo[0].Mode,
                                        FALSE,
                                        Table->MessageInfo[0].TargetProcessorSet,
                                        P->FloatingSave);
            if (!NT_SUCCESS(Status))
            {
                ExFreePoolWithTag(Table, TAG_IO_INTERRUPT);
                return Status;
            }

            if (P->ConnectionContext.InterruptMessageTable)
                *P->ConnectionContext.InterruptMessageTable = Table;
            else if (P->ConnectionContext.Generic)
                *P->ConnectionContext.Generic = Table;
            else if (P->ConnectionContext.InterruptObject)
                *P->ConnectionContext.InterruptObject = Table->MessageInfo[0].InterruptObject;

            return STATUS_SUCCESS;
        }
        case CONNECT_LINE_BASED:
        {
            PIO_CONNECT_INTERRUPT_LINE_BASED_PARAMETERS P = &Parameters->LineBased;
            ULONG Vector;
            KIRQL Irql;
            KAFFINITY Affinity;
            BOOLEAN IsMsg;
            USHORT Flags;
            UCHAR ShareDisposition;
            KIRQL SyncIrql;
            KINTERRUPT_MODE Mode;
            BOOLEAN ShareVector;
            NTSTATUS Status;

            Status = IopQueryTranslatedInterrupt(P->PhysicalDeviceObject,
                                                 &Vector,
                                                 &Irql,
                                                 &Affinity,
                                                 &IsMsg,
                                                 &Flags,
                                                 &ShareDisposition);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("IoConnectInterruptEx(CONNECT_LINE_BASED): no translated interrupt resource: 0x%lx\n", Status);
                return Status;
            }

            /* If the device actually has MSI resources, the caller should use CONNECT_MESSAGE_BASED. */
            if (IsMsg)
            {
                DPRINT1("IoConnectInterruptEx(CONNECT_LINE_BASED): called for message interrupt, rejecting\n");
                return STATUS_INVALID_PARAMETER;
            }

            SyncIrql = P->SynchronizeIrql ? P->SynchronizeIrql : Irql;
            Mode = (Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? Latched : LevelSensitive;
            ShareVector = (ShareDisposition == CmResourceShareShared);

            Status = IoConnectInterrupt(P->InterruptObject,
                                        P->ServiceRoutine,
                                        P->ServiceContext,
                                        P->SpinLock,
                                        Vector,
                                        Irql,
                                        SyncIrql,
                                        Mode,
                                        ShareVector,
                                        Affinity ? Affinity : KeActiveProcessors,
                                        P->FloatingSave);
            return Status;
        }
    }

    return STATUS_INVALID_PARAMETER;
}

VOID
NTAPI
IoDisconnectInterruptEx(
    _In_ PIO_DISCONNECT_INTERRUPT_PARAMETERS Parameters)
{
    PAGED_CODE();

    //FIXME: This eventually will need to handle more cases
    if (Parameters->ConnectionContext.InterruptMessageTable)
    {
        PIO_INTERRUPT_MESSAGE_INFO Table = Parameters->ConnectionContext.InterruptMessageTable;
        if (Table->MessageCount >= 1 && Table->MessageInfo[0].InterruptObject)
            IoDisconnectInterrupt(Table->MessageInfo[0].InterruptObject);
        ExFreePoolWithTag(Table, TAG_IO_INTERRUPT);
        return;
    }

    if (Parameters->ConnectionContext.InterruptObject)
        IoDisconnectInterrupt(Parameters->ConnectionContext.InterruptObject);
}

/* EOF */
