/*
 * PROJECT:     ReactOS USB Attached SCSI Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tags and the requests they name
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "uaspstor.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

/**
 * @brief Builds the requests, one per command that may be outstanding.
 *
 * Everything a command needs is built here and reused for the life of the
 * device, so issuing one allocates nothing.
 */
NTSTATUS
UaspCreateRequests(
    _In_ PUASP_ADAPTER_EXTENSION Adapter)
{
    PUASP_REQUEST Request;
    PUCHAR Buffer;
    ULONG PerRequest;
    NTSTATUS Status;
    USHORT Index;
    ULONG Kind;

    ASSERT(Adapter->RequestCount != 0);

    Adapter->Requests = ExAllocatePoolWithTag(NonPagedPool,
                                              Adapter->RequestCount * sizeof(UASP_REQUEST),
                                              TAG_UASP);
    if (Adapter->Requests == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Adapter->Requests, Adapter->RequestCount * sizeof(UASP_REQUEST));

    /*
     * The command going out and the two answers that may come back in. Both
     * reads are the full length because either can land in either, on a
     * device where they share the one pipe.
     */
    PerRequest = sizeof(UAS_COMMAND_IU) + 2 * UASP_STATUS_IU_LENGTH;

    Adapter->IuBufferSize = Adapter->RequestCount * PerRequest;
    Adapter->IuBuffer = ExAllocatePoolWithTag(NonPagedPool, Adapter->IuBufferSize, TAG_UASP);
    if (Adapter->IuBuffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Adapter->IuBuffer, Adapter->IuBufferSize);

    Buffer = Adapter->IuBuffer;

    for (Index = 0; Index < Adapter->RequestCount; Index++)
    {
        Request = &Adapter->Requests[Index];

        Request->Adapter = Adapter;
        Request->Tag = Index + UASP_FIRST_TAG;

        Request->CommandIu = (PUAS_COMMAND_IU)Buffer;
        Buffer += sizeof(UAS_COMMAND_IU);
        Request->StatusIu = Buffer;
        Buffer += UASP_STATUS_IU_LENGTH;
        Request->ReadyIu = Buffer;
        Buffer += UASP_STATUS_IU_LENGTH;

        for (Kind = 0; Kind < UaspTransferMax; Kind++)
        {
            PUASP_TRANSFER Transfer = &Request->Transfer[Kind];

            Transfer->Request = Request;

            Transfer->Irp = IoAllocateIrp(Adapter->LowerDeviceObject->StackSize, FALSE);
            if (Transfer->Irp == NULL)
                return STATUS_INSUFFICIENT_RESOURCES;

            Status = USBD_UrbAllocate(Adapter->UsbdHandle, &Transfer->Urb);
            if (!NT_SUCCESS(Status))
                return Status;
        }
    }

    return STATUS_SUCCESS;
}

/**
 * @brief Gives back everything UaspCreateRequests took.
 */
VOID
UaspDeleteRequests(
    _In_ PUASP_ADAPTER_EXTENSION Adapter)
{
    PUASP_REQUEST Request;
    USHORT Index;
    ULONG Kind;

    if (Adapter->Requests != NULL)
    {
        for (Index = 0; Index < Adapter->RequestCount; Index++)
        {
            Request = &Adapter->Requests[Index];

            for (Kind = 0; Kind < UaspTransferMax; Kind++)
            {
                PUASP_TRANSFER Transfer = &Request->Transfer[Kind];

                if (Transfer->Urb != NULL)
                {
                    USBD_UrbFree(Adapter->UsbdHandle, Transfer->Urb);
                    Transfer->Urb = NULL;
                }

                if (Transfer->Irp != NULL)
                {
                    IoFreeIrp(Transfer->Irp);
                    Transfer->Irp = NULL;
                }
            }
        }

        ExFreePoolWithTag(Adapter->Requests, TAG_UASP);
        Adapter->Requests = NULL;
    }

    if (Adapter->IuBuffer != NULL)
    {
        ExFreePoolWithTag(Adapter->IuBuffer, TAG_UASP);
        Adapter->IuBuffer = NULL;
    }
}

/**
 * @brief Hands out a tag, or nothing when every one of them is in use.
 *
 * The search starts where the last one was found, so a device that answers in
 * order walks straight down the array.
 */
PUASP_REQUEST
UaspAcquireRequest(
    _In_ PUASP_ADAPTER_EXTENSION Adapter)
{
    PUASP_REQUEST Request = NULL;
    KIRQL OldIrql;
    USHORT Count;
    USHORT Index;

    KeAcquireSpinLock(&Adapter->QueueLock, &OldIrql);

    if (Adapter->QueueState == UaspQueueRunning)
    {
        Index = Adapter->NextFreeTag;

        for (Count = 0; Count < Adapter->RequestCount; Count++)
        {
            if (!Adapter->Requests[Index].Busy)
            {
                Request = &Adapter->Requests[Index];
                Request->Busy = TRUE;

                Adapter->NextFreeTag = (Index + 1) % Adapter->RequestCount;
                break;
            }

            Index = (Index + 1) % Adapter->RequestCount;
        }
    }

    KeReleaseSpinLock(&Adapter->QueueLock, OldIrql);

    return Request;
}

/**
 * @brief Takes a tag back once every transfer of its command has answered.
 */
VOID
UaspReleaseRequest(
    _In_ PUASP_REQUEST Request)
{
    PUASP_ADAPTER_EXTENSION Adapter = Request->Adapter;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Adapter->QueueLock, &OldIrql);

    Request->Srb = NULL;
    Request->Busy = FALSE;

    KeReleaseSpinLock(&Adapter->QueueLock, OldIrql);
}

/**
 * @brief Stops handing out tags.
 *
 * Commands already in flight still answer. Anything arriving afterwards is
 * turned away, which is what the class layer expects while the device is on
 * its way out.
 */
VOID
UaspFreezeQueue(
    _In_ PUASP_ADAPTER_EXTENSION Adapter)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&Adapter->QueueLock, &OldIrql);
    Adapter->QueueState = UaspQueueStopped;
    KeReleaseSpinLock(&Adapter->QueueLock, OldIrql);
}
