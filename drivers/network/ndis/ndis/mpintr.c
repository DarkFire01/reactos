/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NDIS 6 miniport interrupts
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ndissys.h"

/*
 * Every message, or the one line, gets a DPC for each processor plus one that
 * runs wherever it is queued. The miniport picks between them from its ISR.
 */
typedef struct _CORE_INTERRUPT
{
    PLOGICAL_ADAPTER Adapter;
    NDIS_HANDLE MiniportInterruptContext;
    MINIPORT_ISR_HANDLER InterruptHandler;
    MINIPORT_INTERRUPT_DPC_HANDLER InterruptDpcHandler;
    MINIPORT_MSI_ISR_HANDLER MessageInterruptHandler;
    MINIPORT_MSI_INTERRUPT_DPC_HANDLER MessageInterruptDpcHandler;

    /* A PKINTERRUPT when line based, a PIO_INTERRUPT_MESSAGE_INFO otherwise */
    PVOID Connection;
    BOOLEAN MessageBased;
    PIO_INTERRUPT_MESSAGE_INFO MiniportMessageInfo;
    KSPIN_LOCK MessageLock;

    /* One count for the registration and one for every queued DPC */
    LONG DpcReferences;
    BOOLEAN Draining;
    KEVENT DpcsDrained;

    ULONG MessageCount;
    ULONG ProcessorCount;
    KDPC Dpcs[ANYSIZE_ARRAY];
} CORE_INTERRUPT, *PCORE_INTERRUPT;

/* Slot ProcessorCount is the DPC that is not tied to a processor */
#define CORE_INTERRUPT_DPC(_Interrupt, _MessageId, _Slot) \
    (&(_Interrupt)->Dpcs[(_MessageId) * ((_Interrupt)->ProcessorCount + 1) + (_Slot)])

static
VOID
CoreReleaseInterruptDpc(
    _In_ PCORE_INTERRUPT Interrupt)
{
    if (InterlockedDecrement(&Interrupt->DpcReferences) == 0)
        KeSetEvent(&Interrupt->DpcsDrained, IO_NO_INCREMENT, FALSE);
}

static
BOOLEAN
CoreInsertInterruptDpc(
    _In_ PCORE_INTERRUPT Interrupt,
    _In_ PKDPC Dpc,
    _In_ ULONG MessageId,
    _In_opt_ PVOID MiniportDpcContext)
{
    InterlockedIncrement(&Interrupt->DpcReferences);

    if (KeInsertQueueDpc(Dpc, UlongToPtr(MessageId), MiniportDpcContext))
        return TRUE;

    /* Already queued: that one will do */
    CoreReleaseInterruptDpc(Interrupt);
    return FALSE;
}

static
KAFFINITY
CoreQueueTargetedDpcs(
    _In_ PCORE_INTERRUPT Interrupt,
    _In_ ULONG MessageId,
    _In_ KAFFINITY Processors,
    _In_opt_ PVOID MiniportDpcContext)
{
    KAFFINITY Queued = 0;
    ULONG Processor;

    if (MessageId >= Interrupt->MessageCount)
        return 0;

    for (Processor = 0; Processor < Interrupt->ProcessorCount && Processors != 0; Processor++)
    {
        if (!(Processors & ((KAFFINITY)1 << Processor)))
            continue;

        Processors &= ~((KAFFINITY)1 << Processor);

        if (CoreInsertInterruptDpc(Interrupt,
                                   CORE_INTERRUPT_DPC(Interrupt, MessageId, Processor),
                                   MessageId,
                                   MiniportDpcContext))
        {
            Queued |= ((KAFFINITY)1 << Processor);
        }
    }

    return Queued;
}

static
VOID
CoreQueueIsrDpcs(
    _In_ PCORE_INTERRUPT Interrupt,
    _In_ ULONG MessageId,
    _In_ BOOLEAN QueueDefaultDpc,
    _In_ ULONG TargetProcessors)
{
    if (MessageId >= Interrupt->MessageCount)
        return;

    if (QueueDefaultDpc)
    {
        CoreInsertInterruptDpc(Interrupt,
                               CORE_INTERRUPT_DPC(Interrupt, MessageId, Interrupt->ProcessorCount),
                               MessageId,
                               NULL);
        return;
    }

    if (TargetProcessors != 0)
        CoreQueueTargetedDpcs(Interrupt, MessageId, TargetProcessors, NULL);
}

static KSERVICE_ROUTINE CoreLineIsr;

static
BOOLEAN
NTAPI
CoreLineIsr(
    _In_ PKINTERRUPT InterruptObject,
    _In_ PVOID ServiceContext)
{
    PCORE_INTERRUPT Interrupt = ServiceContext;
    BOOLEAN QueueDefaultDpc = FALSE;
    ULONG TargetProcessors = 0;
    BOOLEAN Claimed;

    UNREFERENCED_PARAMETER(InterruptObject);

    Claimed = Interrupt->InterruptHandler(Interrupt->MiniportInterruptContext,
                                          &QueueDefaultDpc,
                                          &TargetProcessors);

    CoreQueueIsrDpcs(Interrupt, 0, QueueDefaultDpc, TargetProcessors);
    return Claimed;
}

static KMESSAGE_SERVICE_ROUTINE CoreMessageIsr;

static
BOOLEAN
NTAPI
CoreMessageIsr(
    _In_ PKINTERRUPT InterruptObject,
    _In_ PVOID ServiceContext,
    _In_ ULONG MessageId)
{
    PCORE_INTERRUPT Interrupt = ServiceContext;
    BOOLEAN QueueDefaultDpc = FALSE;
    ULONG TargetProcessors = 0;
    BOOLEAN Claimed;

    UNREFERENCED_PARAMETER(InterruptObject);

    Claimed = Interrupt->MessageInterruptHandler(Interrupt->MiniportInterruptContext,
                                                 MessageId,
                                                 &QueueDefaultDpc,
                                                 &TargetProcessors);

    CoreQueueIsrDpcs(Interrupt, MessageId, QueueDefaultDpc, TargetProcessors);
    return Claimed;
}

static KDEFERRED_ROUTINE CoreInterruptDpc;

static
VOID
NTAPI
CoreInterruptDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PCORE_INTERRUPT Interrupt = DeferredContext;
    NDIS_RECEIVE_THROTTLE_PARAMETERS Throttle;
    ULONG MessageId = PtrToUlong(SystemArgument1);

    /* Once the interrupt is going away the DPCs only drain */
    if (!Interrupt->Draining)
    {
        Throttle.MaxNblsToIndicate = NDIS_INDICATE_ALL_NBLS;
        Throttle.MoreNblsPending = 0;

        if (Interrupt->MessageBased)
        {
            Interrupt->MessageInterruptDpcHandler(Interrupt->MiniportInterruptContext,
                                                  MessageId,
                                                  SystemArgument2,
                                                  &Throttle,
                                                  NULL);
        }
        else
        {
            Interrupt->InterruptDpcHandler(Interrupt->MiniportInterruptContext,
                                           SystemArgument2,
                                           &Throttle,
                                           NULL);
        }

        /* The miniport stopped short and wants to be called again */
        if (Throttle.MoreNblsPending)
            CoreInsertInterruptDpc(Interrupt, Dpc, MessageId, SystemArgument2);
    }

    CoreReleaseInterruptDpc(Interrupt);
}

/* Messages the device was given, or 1 for a line interrupt */
static
ULONG
CoreCountInterruptMessages(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PCM_RESOURCE_LIST Resources = Adapter->NdisMiniportBlock.AllocatedResources;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    ULONG Messages = 0;
    ULONG i;

    if (Resources == NULL || Resources->Count == 0)
        return 1;

    for (i = 0; i < Resources->List[0].PartialResourceList.Count; i++)
    {
        Descriptor = &Resources->List[0].PartialResourceList.PartialDescriptors[i];

        if (Descriptor->Type == CmResourceTypeInterrupt &&
            (Descriptor->Flags & CM_RESOURCE_INTERRUPT_MESSAGE))
        {
            Messages += Descriptor->u.MessageInterrupt.Raw.MessageCount;
        }
    }

    return max(Messages, 1);
}

static
VOID
CoreInitializeInterruptDpcs(
    _In_ PCORE_INTERRUPT Interrupt)
{
    PROCESSOR_NUMBER ProcessorNumber;
    ULONG MessageId;
    ULONG Processor;
    PKDPC Dpc;

    for (MessageId = 0; MessageId < Interrupt->MessageCount; MessageId++)
    {
        for (Processor = 0; Processor <= Interrupt->ProcessorCount; Processor++)
        {
            Dpc = CORE_INTERRUPT_DPC(Interrupt, MessageId, Processor);

            KeInitializeDpc(Dpc, CoreInterruptDpc, Interrupt);
            KeSetImportanceDpc(Dpc, MediumHighImportance);

            if (Processor < Interrupt->ProcessorCount &&
                NT_SUCCESS(KeGetProcessorNumberFromIndex(Processor, &ProcessorNumber)))
            {
                KeSetTargetProcessorDpcEx(Dpc, &ProcessorNumber);
            }
        }
    }
}

/* The miniport gets its own copy of the message table */
static
NDIS_STATUS
CoreCopyMessageInfo(
    _In_ PCORE_INTERRUPT Interrupt,
    _In_ PIO_INTERRUPT_MESSAGE_INFO MessageInfo)
{
    ULONG Size;

    Size = FIELD_OFFSET(IO_INTERRUPT_MESSAGE_INFO, MessageInfo) +
           MessageInfo->MessageCount * sizeof(IO_INTERRUPT_MESSAGE_INFO_ENTRY);

    Interrupt->MiniportMessageInfo = ExAllocatePoolWithTag(NonPagedPool, Size, NDIS_TAG);
    if (Interrupt->MiniportMessageInfo == NULL)
        return NDIS_STATUS_RESOURCES;

    RtlCopyMemory(Interrupt->MiniportMessageInfo, MessageInfo, Size);
    return NDIS_STATUS_SUCCESS;
}

static
VOID
CoreDisconnectInterrupt(
    _In_ PCORE_INTERRUPT Interrupt)
{
    IO_DISCONNECT_INTERRUPT_PARAMETERS Parameters;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Version = Interrupt->MessageBased ? CONNECT_MESSAGE_BASED : CONNECT_LINE_BASED;
    Parameters.ConnectionContext.Generic = Interrupt->Connection;

    IoDisconnectInterruptEx(&Parameters);
}

/**
 * @brief
 * Connects a 6.x miniport's interrupt, message based when the miniport and
 * the device both can, line based otherwise.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 *
 * @param[in] MiniportInterruptContext
 * What the miniport's interrupt handlers get.
 *
 * @param[in,out] MiniportInterruptCharacteristics
 * The handlers. On success the interrupt type and message table are filled in.
 *
 * @param[out] NdisInterruptHandle
 * The handle for the other interrupt calls.
 *
 * @return
 * NDIS_STATUS_SUCCESS, NDIS_STATUS_RESOURCES or NDIS_STATUS_FAILURE.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisMRegisterInterruptEx(
    NDIS_HANDLE MiniportAdapterHandle,
    NDIS_HANDLE MiniportInterruptContext,
    PNDIS_MINIPORT_INTERRUPT_CHARACTERISTICS MiniportInterruptCharacteristics,
    PNDIS_HANDLE NdisInterruptHandle)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterHandle;
    IO_CONNECT_INTERRUPT_PARAMETERS Parameters;
    PCORE_INTERRUPT Interrupt;
    ULONG MessageCount;
    ULONG ProcessorCount;
    ULONG Size;
    NTSTATUS Status;

    *NdisInterruptHandle = NULL;
    MiniportInterruptCharacteristics->MessageInfoTable = NULL;
    MiniportInterruptCharacteristics->InterruptType = NDIS_CONNECT_LINE_BASED;

    MessageCount = CoreCountInterruptMessages(Adapter);
    ProcessorCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);

    Size = FIELD_OFFSET(CORE_INTERRUPT, Dpcs) + MessageCount * (ProcessorCount + 1) * sizeof(KDPC);
    Interrupt = ExAllocatePoolWithTag(NonPagedPool, Size, NDIS_TAG);
    if (Interrupt == NULL)
        return NDIS_STATUS_RESOURCES;

    RtlZeroMemory(Interrupt, Size);
    Interrupt->Adapter = Adapter;
    Interrupt->MiniportInterruptContext = MiniportInterruptContext;
    Interrupt->InterruptHandler = MiniportInterruptCharacteristics->InterruptHandler;
    Interrupt->InterruptDpcHandler = MiniportInterruptCharacteristics->InterruptDpcHandler;
    Interrupt->MessageInterruptHandler = MiniportInterruptCharacteristics->MessageInterruptHandler;
    Interrupt->MessageInterruptDpcHandler = MiniportInterruptCharacteristics->MessageInterruptDpcHandler;
    Interrupt->MessageCount = MessageCount;
    Interrupt->ProcessorCount = ProcessorCount;
    Interrupt->DpcReferences = 1;
    KeInitializeSpinLock(&Interrupt->MessageLock);
    KeInitializeEvent(&Interrupt->DpcsDrained, NotificationEvent, FALSE);
    CoreInitializeInterruptDpcs(Interrupt);

    RtlZeroMemory(&Parameters, sizeof(Parameters));

    /* A miniport with a message handler is offered messages, falling back to the line */
    if (Interrupt->MessageInterruptHandler != NULL)
    {
        Parameters.Version = CONNECT_MESSAGE_BASED;
        Parameters.MessageBased.PhysicalDeviceObject = Adapter->NdisMiniportBlock.PhysicalDeviceObject;
        Parameters.MessageBased.ConnectionContext.Generic = &Interrupt->Connection;
        Parameters.MessageBased.MessageServiceRoutine = CoreMessageIsr;
        Parameters.MessageBased.ServiceContext = Interrupt;
        Parameters.MessageBased.FallBackServiceRoutine = CoreLineIsr;

        if (MiniportInterruptCharacteristics->MsiSyncWithAllMessages)
            Parameters.MessageBased.SpinLock = &Interrupt->MessageLock;
    }
    else
    {
        Parameters.Version = CONNECT_LINE_BASED;
        Parameters.LineBased.PhysicalDeviceObject = Adapter->NdisMiniportBlock.PhysicalDeviceObject;
        Parameters.LineBased.InterruptObject = (PKINTERRUPT *)&Interrupt->Connection;
        Parameters.LineBased.ServiceRoutine = CoreLineIsr;
        Parameters.LineBased.ServiceContext = Interrupt;
    }

    Status = IoConnectInterruptEx(&Parameters);
    if (!NT_SUCCESS(Status))
    {
        NDIS_DbgPrint(MIN_TRACE, ("IoConnectInterruptEx failed (0x%x).\n", Status));
        ExFreePoolWithTag(Interrupt, NDIS_TAG);
        return NDIS_STATUS_FAILURE;
    }

    Interrupt->MessageBased = (Parameters.Version == CONNECT_MESSAGE_BASED);

    if (Interrupt->MessageBased)
    {
        if (CoreCopyMessageInfo(Interrupt, Interrupt->Connection) != NDIS_STATUS_SUCCESS)
        {
            NdisMDeregisterInterruptEx(Interrupt);
            return NDIS_STATUS_RESOURCES;
        }

        MiniportInterruptCharacteristics->MessageInfoTable = Interrupt->MiniportMessageInfo;
        MiniportInterruptCharacteristics->InterruptType = NDIS_CONNECT_MESSAGE_BASED;
    }

    Adapter->Core.Interrupt = Interrupt;
    *NdisInterruptHandle = Interrupt;

    return NDIS_STATUS_SUCCESS;
}

/**
 * @brief
 * Disconnects a miniport's interrupt and waits out its queued DPCs.
 *
 * @param[in] NdisInterruptHandle
 * The handle from NdisMRegisterInterruptEx.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisMDeregisterInterruptEx(
    NDIS_HANDLE NdisInterruptHandle)
{
    PCORE_INTERRUPT Interrupt = NdisInterruptHandle;

    CoreDisconnectInterrupt(Interrupt);

    if (Interrupt->MiniportMessageInfo != NULL)
    {
        ExFreePoolWithTag(Interrupt->MiniportMessageInfo, NDIS_TAG);
        Interrupt->MiniportMessageInfo = NULL;
    }

    Interrupt->Draining = TRUE;
    CoreReleaseInterruptDpc(Interrupt);
    KeWaitForSingleObject(&Interrupt->DpcsDrained, Executive, KernelMode, FALSE, NULL);

    if (Interrupt->Adapter->Core.Interrupt == Interrupt)
        Interrupt->Adapter->Core.Interrupt = NULL;

    ExFreePoolWithTag(Interrupt, NDIS_TAG);
}

/**
 * @brief
 * Runs a function synchronized with a miniport's interrupt, or with one of
 * its messages.
 *
 * @param[in] NdisInterruptHandle
 * The handle from NdisMRegisterInterruptEx.
 *
 * @param[in] MessageId
 * The message to synchronize with. Ignored for a line interrupt.
 *
 * @param[in] SynchronizeFunction
 * The function.
 *
 * @param[in] SynchronizeContext
 * Its argument.
 *
 * @return
 * What the function returned, or FALSE for a message that does not exist.
 */
_Use_decl_annotations_
BOOLEAN
NTAPI
NdisMSynchronizeWithInterruptEx(
    NDIS_HANDLE NdisInterruptHandle,
    ULONG MessageId,
    MINIPORT_SYNCHRONIZE_INTERRUPT_HANDLER SynchronizeFunction,
    PVOID SynchronizeContext)
{
    PCORE_INTERRUPT Interrupt = NdisInterruptHandle;
    PIO_INTERRUPT_MESSAGE_INFO MessageInfo;

    if (!Interrupt->MessageBased)
    {
        return KeSynchronizeExecution((PKINTERRUPT)Interrupt->Connection,
                                      (PKSYNCHRONIZE_ROUTINE)SynchronizeFunction,
                                      SynchronizeContext);
    }

    MessageInfo = Interrupt->Connection;
    if (MessageInfo == NULL || MessageId >= MessageInfo->MessageCount)
        return FALSE;

    return KeSynchronizeExecution(MessageInfo->MessageInfo[MessageId].InterruptObject,
                                  (PKSYNCHRONIZE_ROUTINE)SynchronizeFunction,
                                  SynchronizeContext);
}

/**
 * @brief
 * Queues a miniport's interrupt DPC on the processors it names.
 *
 * @param[in] NdisInterruptHandle
 * The handle from NdisMRegisterInterruptEx.
 *
 * @param[in] MessageId
 * The message whose DPC runs, 0 for a line interrupt.
 *
 * @param[in] TargetProcessors
 * The processor group and processors.
 *
 * @param[in] MiniportDpcContext
 * What the DPC handler gets.
 *
 * @return
 * The processors a DPC was queued on.
 */
_Use_decl_annotations_
KAFFINITY
NTAPI
NdisMQueueDpcEx(
    NDIS_HANDLE NdisInterruptHandle,
    ULONG MessageId,
    PGROUP_AFFINITY TargetProcessors,
    PVOID MiniportDpcContext)
{
    /* Everything runs in group 0 */
    if (TargetProcessors->Group != 0)
        return 0;

    return CoreQueueTargetedDpcs(NdisInterruptHandle, MessageId, TargetProcessors->Mask, MiniportDpcContext);
}

/**
 * @brief
 * NdisMQueueDpcEx for processor group 0 only.
 *
 * @param[in] NdisInterruptHandle
 * The handle from NdisMRegisterInterruptEx.
 *
 * @param[in] MessageId
 * The message whose DPC runs, 0 for a line interrupt.
 *
 * @param[in] TargetProcessors
 * The processors.
 *
 * @param[in] MiniportDpcContext
 * What the DPC handler gets.
 *
 * @return
 * The processors a DPC was queued on.
 */
_Use_decl_annotations_
ULONG
NTAPI
NdisMQueueDpc(
    NDIS_HANDLE NdisInterruptHandle,
    ULONG MessageId,
    ULONG TargetProcessors,
    PVOID MiniportDpcContext)
{
    return (ULONG)CoreQueueTargetedDpcs(NdisInterruptHandle, MessageId, TargetProcessors, MiniportDpcContext);
}
