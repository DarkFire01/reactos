/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Configuration space accessor: one outstanding read or write request at a time
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4hr.h"

#define NDEBUG
#include <debug.h>

/* Route string bits that carry hops: all of the low dword, levels 5 and 6 of the high one */
#define CONFIG_ROUTE_HIGH_HOPS      0x0000FFFF

/* A timer that fires this close to the deadline belongs to the current attempt */
#define CONFIG_DEADLINE_SLACK       10000ULL

/** One read or write, from an IOCTL or a synchronous caller. */
struct Usb4HrConfigRequest
{
    LIST_ENTRY Link;
    WDFREQUEST Request;         /**< NULL for a synchronous caller */
    Usb4HrConfigKind Kind;
    Usb4HrConfigTarget Target;
    ULONG DwordOffset;
    ULONG DwordCount;
    PULONG Buffer;              /**< read destination */
    PVOID Output;               /**< IOCTL output buffer */
    ULONG Attempts;
    USB4HR_STATUS Status;
    ULONG ResponseAdapter;
    PKEVENT Done;               /**< synchronous callers wait on it */
    ULONG Data[USB4HR_MAX_CONFIG_DWORDS];   /**< write source or read response */
};

/** Links the accessor's timer and DPC back to it. */
struct Usb4HrAccessorLink
{
    Usb4HrConfigAccessor* Accessor;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Usb4HrAccessorLink, Usb4HrGetAccessorLink);

static
BOOLEAN
NTAPI
Usb4HrIsWrite(
    _In_ const Usb4HrConfigRequest* Entry)
{
    return Entry->Kind == Usb4HrConfigKind::Write;
}

/* Writes the USB4 status to the output buffer and completes the IOCTL; frees the entry */
static
VOID
NTAPI
Usb4HrCompleteIoctlEntry(
    _In_ __drv_freesMem(Mem) Usb4HrConfigRequest* Entry)
{
    ULONG_PTR Information;
    BOOLEAN Success = (Entry->Status == USB4HR_STATUS_SUCCESS);

    if (Entry->Kind == Usb4HrConfigKind::ReadEx)
    {
        PUSB4HR_CONFIG_EX_OUTPUT Output = (PUSB4HR_CONFIG_EX_OUTPUT)Entry->Output;

        Output->Status = Entry->Status;
        Output->AdapterNumber = Success ? Entry->ResponseAdapter : 0;
        Information = sizeof(*Output);
    }
    else
    {
        PUSB4HR_CONFIG_OUTPUT Output = (PUSB4HR_CONFIG_OUTPUT)Entry->Output;

        Output->Status = Entry->Status;
        Information = sizeof(*Output);
    }

    if (Success && !Usb4HrIsWrite(Entry))
        RtlCopyMemory(Entry->Buffer, Entry->Data, Entry->DwordCount * sizeof(ULONG));

    /* QUIRK: failures, timeouts and cancellation all complete with STATUS_SUCCESS; the USB4 status tells them apart */
    WdfRequestCompleteWithInformation(Entry->Request, STATUS_SUCCESS, Information);
    ExFreePoolWithTag(Entry, USB4HR_TAG_CONFIG);
}

NTSTATUS
Usb4HrConfigAccessor::Create(
    _In_ Usb4HrHostRouter* HostRouter)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_TIMER_CONFIG TimerConfig;
    WDF_DPC_CONFIG DpcConfig;
    Usb4HrRingZero* Ring = HostRouter->Ring();
    NTSTATUS Status;

    m_HostRouter = HostRouter;
    KeInitializeSpinLock(&m_Lock);
    InitializeListHead(&m_Pending);
    InitializeListHead(&m_Cancelling);
    InitializeListHead(&m_Finished);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, Usb4HrAccessorLink);
    Attributes.ParentObject = HostRouter->Device();

    WDF_TIMER_CONFIG_INIT(&TimerConfig, OnResponseTimeout);
    TimerConfig.AutomaticSerialization = FALSE;
    Status = WdfTimerCreate(&TimerConfig, &Attributes, &m_Timer);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Config response timer creation failed 0x%lx\n", Status);
        return Status;
    }
    Usb4HrGetAccessorLink(m_Timer)->Accessor = this;

    WDF_DPC_CONFIG_INIT(&DpcConfig, OnCompletionDpc);
    DpcConfig.AutomaticSerialization = FALSE;
    Status = WdfDpcCreate(&DpcConfig, &Attributes, &m_Dpc);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Config completion DPC creation failed 0x%lx\n", Status);
        return Status;
    }
    Usb4HrGetAccessorLink(m_Dpc)->Accessor = this;

    Status = Ring->RegisterHandler(USB4HR_PDF_READ, OnPacket, this);
    if (NT_SUCCESS(Status))
        Status = Ring->RegisterHandler(USB4HR_PDF_WRITE, OnPacket, this);
    if (NT_SUCCESS(Status))
        Status = Ring->RegisterHandler(USB4HR_PDF_NOTIFICATION, OnPacket, this);
    if (!NT_SUCCESS(Status))
        DPRINT1("Config accessor packet handler registration failed 0x%lx\n", Status);

    return Status;
}

VOID Usb4HrConfigAccessor::Resume()
{
    Usb4HrSpinLockGuard Guard(&m_Lock);

    m_Running = TRUE;
    StartNext();
}

VOID Usb4HrConfigAccessor::Pause()
{
    PLIST_ENTRY Link;
    Usb4HrConfigRequest* Entry;
    ULONG Waited;

    {
        Usb4HrSpinLockGuard Guard(&m_Lock);

        m_Running = FALSE;

        while (!IsListEmpty(&m_Pending))
        {
            Link = RemoveHeadList(&m_Pending);
            Entry = CONTAINING_RECORD(Link, Usb4HrConfigRequest, Link);
            if (Entry->Request != NULL && !ClaimFromCancel(Entry))
                continue;
            Finish(Entry, USB4HR_STATUS_FAILURE);
        }

        /* The request on the wire is failed too; a late response finds nothing to match */
        if (m_Current != NULL)
        {
            WdfTimerStop(m_Timer, FALSE);
            Entry = m_Current;
            m_Current = NULL;
            Finish(Entry, USB4HR_STATUS_FAILURE);
        }
    }

    WdfTimerStop(m_Timer, TRUE);

    /* Give every failed request time to reach its owner before the rings stop */
    for (Waited = 0; m_Owned != 0 && Waited < USB4HR_CONFIG_DRAIN_MS; Waited += USB4HR_CONFIG_DRAIN_STEP_MS)
        Usb4HrSleepMs(USB4HR_CONFIG_DRAIN_STEP_MS);

    if (m_Owned != 0)
        DPRINT1("Config accessor paused with %ld requests not completed\n", m_Owned);
}

BOOLEAN
Usb4HrConfigAccessor::Enqueue(
    _Inout_ Usb4HrConfigRequest* Entry)
{
    NTSTATUS Status;

    Usb4HrSpinLockGuard Guard(&m_Lock);

    if (!m_Running)
    {
        DPRINT("Config access while the accessor is paused\n");
        return FALSE;
    }

    InsertTailList(&m_Pending, &Entry->Link);

    if (Entry->Request != NULL)
    {
        Status = WdfRequestMarkCancelableEx(Entry->Request, OnCancel);
        if (!NT_SUCCESS(Status))
        {
            DPRINT("Config IOCTL canceled before it was queued\n");
            RemoveEntryList(&Entry->Link);
            return FALSE;
        }
    }

    InterlockedIncrement(&m_Owned);
    StartNext();
    return TRUE;
}

BOOLEAN
Usb4HrConfigAccessor::ClaimFromCancel(
    _Inout_ Usb4HrConfigRequest* Entry)
{
    if (NT_SUCCESS(WdfRequestUnmarkCancelable(Entry->Request)))
        return TRUE;

    /* The cancel routine is on its way and completes the request */
    InsertTailList(&m_Cancelling, &Entry->Link);
    return FALSE;
}

VOID Usb4HrConfigAccessor::StartNext()
{
    PLIST_ENTRY Link;
    Usb4HrConfigRequest* Entry;

    while (m_Running && m_Current == NULL && !IsListEmpty(&m_Pending))
    {
        Link = RemoveHeadList(&m_Pending);
        Entry = CONTAINING_RECORD(Link, Usb4HrConfigRequest, Link);

        if (Entry->Request != NULL && !ClaimFromCancel(Entry))
            continue;

        if (!NT_SUCCESS(Transmit(Entry)))
            Finish(Entry, USB4HR_STATUS_FAILURE);
    }
}

NTSTATUS
Usb4HrConfigAccessor::Transmit(
    _Inout_ Usb4HrConfigRequest* Entry)
{
    ULONG Packet[USB4HR_PACKET_HEADER_DWORDS + USB4HR_MAX_CONFIG_DWORDS];
    ULONG DwordCount = USB4HR_PACKET_HEADER_DWORDS;
    ULONG Pdf = USB4HR_PDF_READ;
    NTSTATUS Status;

    Usb4HrRouteToPacket(&Entry->Target.Route, Packet);
    Packet[2] = Usb4HrConfigRequestHeader(Entry->DwordOffset,
                                          Entry->DwordCount,
                                          Entry->Target.Adapter,
                                          Entry->Target.Space,
                                          Entry->Target.Sequence);

    if (Usb4HrIsWrite(Entry))
    {
        Pdf = USB4HR_PDF_WRITE;
        RtlCopyMemory(&Packet[USB4HR_PACKET_HEADER_DWORDS], Entry->Data, Entry->DwordCount * sizeof(ULONG));
        DwordCount += Entry->DwordCount;
    }

    Status = m_HostRouter->Ring()->Send(Pdf, Packet, DwordCount);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Config %s of adapter %u space %lu not sent 0x%lx\n",
                Usb4HrIsWrite(Entry) ? "write" : "read",
                Entry->Target.Adapter,
                Entry->Target.Space,
                Status);
        return Status;
    }

    Entry->Attempts++;
    m_Current = Entry;
    m_Deadline = KeQueryInterruptTime() + (ULONGLONG)USB4HR_CONFIG_TIMEOUT_MS * 10000;
    WdfTimerStart(m_Timer, WDF_REL_TIMEOUT_IN_MS(USB4HR_CONFIG_TIMEOUT_MS));
    return STATUS_SUCCESS;
}

VOID
Usb4HrConfigAccessor::Finish(
    _Inout_ Usb4HrConfigRequest* Entry,
    _In_ USB4HR_STATUS Status)
{
    Entry->Status = Status;
    InsertTailList(&m_Finished, &Entry->Link);
    WdfDpcEnqueue(m_Dpc);
}

BOOLEAN
Usb4HrConfigAccessor::MatchResponse(
    _In_ const Usb4HrConfigRequest* Entry,
    _In_ const Usb4HrRxPacket* Packet)
{
    ULONG Expected[2];
    ULONG Header = Packet->Dword[2];
    ULONG Space = (Header & USB4HR_CFG_SPACE_MASK) >> USB4HR_CFG_SPACE_SHIFT;
    ULONG Adapter = (Header & USB4HR_CFG_ADAPTER_MASK) >> USB4HR_CFG_ADAPTER_SHIFT;
    ULONG Sequence = (Header & USB4HR_CFG_SEQUENCE_MASK) >> USB4HR_CFG_SEQUENCE_SHIFT;

    if (Packet->Pdf != (Usb4HrIsWrite(Entry) ? (ULONG)USB4HR_PDF_WRITE : (ULONG)USB4HR_PDF_READ))
    {
        DPRINT1("Config response PDF %lu does not match the request\n", Packet->Pdf);
        return FALSE;
    }

    Usb4HrRouteToPacket(&Entry->Target.Route, Expected);
    if (Packet->Dword[USB4HR_ROUTE_LOW] != Expected[USB4HR_ROUTE_LOW] ||
        (Packet->Dword[USB4HR_ROUTE_HIGH] & CONFIG_ROUTE_HIGH_HOPS) != Expected[USB4HR_ROUTE_HIGH])
    {
        DPRINT1("Config response from route %08lx%08lx dropped\n",
                Packet->Dword[USB4HR_ROUTE_HIGH], Packet->Dword[USB4HR_ROUTE_LOW]);
        return FALSE;
    }

    if (Sequence != (ULONG)(Entry->Target.Sequence & 3))
    {
        DPRINT1("Config response sequence %lu, expected %u\n", Sequence, Entry->Target.Sequence & 3);
        return FALSE;
    }

    /* Windows only reports these two; the response still completes the request */
    if (Space != Entry->Target.Space)
        DPRINT1("Config response space %lu, requested %lu\n", Space, Entry->Target.Space);
    if (Space != USB4HR_SPACE_ROUTER && Adapter != Entry->Target.Adapter)
        DPRINT1("Config response adapter %lu, requested %u\n", Adapter, Entry->Target.Adapter);

    return TRUE;
}

BOOLEAN
Usb4HrConfigAccessor::ShouldFailOnNotification(
    _In_ const Usb4HrConfigRequest* Entry,
    _In_ const Usb4HrRxPacket* Packet)
{
    ULONG Event = Packet->Dword[2] & USB4HR_NOTIFY_EVENT_MASK;
    UCHAR Adapter = (UCHAR)((Packet->Dword[2] & USB4HR_NOTIFY_ADAPTER_MASK) >> USB4HR_NOTIFY_ADAPTER_SHIFT);
    const Usb4HrRoute* Target = &Entry->Target.Route;
    Usb4HrRoute Source;
    ULONG Hop;

    switch (Event)
    {
        /* Errors a router answers a bad request with; Windows does not check where they came from */
        case USB4HR_STATUS_ERR_ADDR:
        case USB4HR_STATUS_ERR_ADP:
        case USB4HR_STATUS_ERR_ENUM:
        case USB4HR_STATUS_ERR_NUA:
        case USB4HR_STATUS_ERR_LEN:
            return TRUE;

        /* A router on the way reports the next hop has no link */
        case USB4HR_STATUS_ERR_CONN:
            Usb4HrRouteFromPacket(Packet->Dword, &Source);
            if (Source.Depth >= Target->Depth)
                return FALSE;

            for (Hop = 0; Hop < Source.Depth; Hop++)
            {
                if (Source.Port[Hop] != Target->Port[Hop])
                    return FALSE;
            }
            return Adapter == Target->Port[Source.Depth];

        case USB4HR_STATUS_ERR_LINK:
        case USB4HR_STATUS_ERR_HEC:
        case USB4HR_STATUS_ERR_FC:
        case USB4HR_STATUS_ERR_PLUG:
        case USB4HR_STATUS_ERR_LOCK:
            return FALSE;

        default:
            if (Event < USB4HR_STATUS_DP_BW || Event > USB4HR_STATUS_ASYM_LINK)
                DPRINT1("Unknown notification event %lu\n", Event);
            return FALSE;
    }
}

VOID
Usb4HrConfigAccessor::ProcessPacket(
    _In_ const Usb4HrRxPacket* Packet)
{
    Usb4HrConfigRequest* Entry;
    USB4HR_STATUS Status = USB4HR_STATUS_SUCCESS;
    ULONG Count;

    Usb4HrSpinLockGuard Guard(&m_Lock);

    Entry = m_Current;
    if (Entry == NULL)
    {
        if (Packet->Pdf != USB4HR_PDF_NOTIFICATION)
            DPRINT1("Config response PDF %lu without a request\n", Packet->Pdf);
        return;
    }

    if (Packet->Pdf == USB4HR_PDF_NOTIFICATION)
    {
        if (!ShouldFailOnNotification(Entry, Packet))
            return;

        Status = Packet->Dword[2] & USB4HR_NOTIFY_EVENT_MASK;
        DPRINT1("Config request failed by notification event %lu\n", Status);
    }
    else
    {
        if (!MatchResponse(Entry, Packet))
            return;

        if (Packet->Pdf == USB4HR_PDF_READ)
        {
            Count = (Packet->Dword[2] & USB4HR_CFG_LENGTH_MASK) >> USB4HR_CFG_LENGTH_SHIFT;
            if (Count != Entry->DwordCount || Packet->DwordCount < USB4HR_PACKET_HEADER_DWORDS + Count)
            {
                DPRINT1("Config read returned %lu dwords, requested %lu\n", Count, Entry->DwordCount);
                Status = USB4HR_STATUS_FAILURE;
            }
            else
            {
                RtlCopyMemory(Entry->Data, &Packet->Dword[USB4HR_PACKET_HEADER_DWORDS], Count * sizeof(ULONG));
                Entry->ResponseAdapter = (Packet->Dword[2] & USB4HR_CFG_ADAPTER_MASK) >> USB4HR_CFG_ADAPTER_SHIFT;
            }
        }
    }

    WdfTimerStop(m_Timer, FALSE);
    m_Current = NULL;
    Finish(Entry, Status);
    StartNext();
}

VOID
NTAPI
Usb4HrConfigAccessor::OnPacket(
    _In_ PVOID Context,
    _In_ const Usb4HrRxPacket* Packet)
{
    static_cast<Usb4HrConfigAccessor*>(Context)->ProcessPacket(Packet);
}

VOID Usb4HrConfigAccessor::ProcessTimeout()
{
    Usb4HrConfigRequest* Entry;

    /* A response may sit in the receive ring with its interrupt still pending */
    m_HostRouter->Ring()->ProcessReceived();

    Usb4HrSpinLockGuard Guard(&m_Lock);

    Entry = m_Current;
    if (Entry == NULL)
        return;

    /* The timer was re-armed for a newer attempt after this expiry was queued */
    if (KeQueryInterruptTime() + CONFIG_DEADLINE_SLACK < m_Deadline)
        return;

    m_Current = NULL;

    if (Entry->Attempts < USB4HR_CONFIG_ATTEMPTS)
    {
        DPRINT("Config request timed out, attempt %lu, sending again\n", Entry->Attempts);
        if (!NT_SUCCESS(Transmit(Entry)))
            Finish(Entry, USB4HR_STATUS_FAILURE);
    }
    else
    {
        DPRINT1("Config request to adapter %u space %lu offset %lu got no response\n",
                Entry->Target.Adapter, Entry->Target.Space, Entry->DwordOffset);
        Finish(Entry, USB4HR_STATUS_FAILURE);
    }

    StartNext();
}

VOID
Usb4HrConfigAccessor::OnResponseTimeout(
    _In_ WDFTIMER Timer)
{
    Usb4HrGetAccessorLink(Timer)->Accessor->ProcessTimeout();
}

VOID Usb4HrConfigAccessor::CompleteFinished()
{
    PLIST_ENTRY Link;
    Usb4HrConfigRequest* Entry;
    PKEVENT Done;

    for (;;)
    {
        {
            Usb4HrSpinLockGuard Guard(&m_Lock);

            if (IsListEmpty(&m_Finished))
                return;
            Link = RemoveHeadList(&m_Finished);
        }

        Entry = CONTAINING_RECORD(Link, Usb4HrConfigRequest, Link);
        if (Entry->Request != NULL)
        {
            Usb4HrCompleteIoctlEntry(Entry);
            InterlockedDecrement(&m_Owned);
            continue;
        }

        if (Entry->Status == USB4HR_STATUS_SUCCESS && !Usb4HrIsWrite(Entry))
            RtlCopyMemory(Entry->Buffer, Entry->Data, Entry->DwordCount * sizeof(ULONG));

        /* The entry lives on the waiter's stack; the event is the last touch */
        Done = Entry->Done;
        InterlockedDecrement(&m_Owned);
        KeSetEvent(Done, IO_NO_INCREMENT, FALSE);
    }
}

VOID
Usb4HrConfigAccessor::OnCompletionDpc(
    _In_ WDFDPC Dpc)
{
    Usb4HrGetAccessorLink(Dpc)->Accessor->CompleteFinished();
}

VOID
Usb4HrConfigAccessor::CancelIoctl(
    _In_ WDFREQUEST Request)
{
    LIST_ENTRY* Lists[] = { &m_Pending, &m_Cancelling };
    Usb4HrConfigRequest* Found = NULL;
    PLIST_ENTRY Link;
    ULONG List;

    {
        Usb4HrSpinLockGuard Guard(&m_Lock);

        for (List = 0; List < RTL_NUMBER_OF(Lists) && Found == NULL; List++)
        {
            for (Link = Lists[List]->Flink; Link != Lists[List]; Link = Link->Flink)
            {
                Usb4HrConfigRequest* Entry = CONTAINING_RECORD(Link, Usb4HrConfigRequest, Link);

                if (Entry->Request == Request)
                {
                    RemoveEntryList(Link);
                    Found = Entry;
                    break;
                }
            }
        }
    }

    if (Found == NULL)
    {
        DPRINT1("Canceled config IOCTL %p is not queued\n", Request);
        return;
    }

    Found->Status = USB4HR_STATUS_FAILURE;
    Usb4HrCompleteIoctlEntry(Found);
    InterlockedDecrement(&m_Owned);
}

VOID
Usb4HrConfigAccessor::OnCancel(
    _In_ WDFREQUEST Request)
{
    WDFDEVICE Device = WdfIoQueueGetDevice(WdfRequestGetIoQueue(Request));

    Usb4HrHostRouter::FromDevice(Device)->ConfigAccessor()->CancelIoctl(Request);
}

VOID
Usb4HrConfigAccessor::SubmitIoctl(
    _In_ WDFREQUEST Request,
    _In_ Usb4HrConfigKind Kind,
    _In_ const Usb4HrConfigTarget* Target,
    _In_ const USB4HR_CONFIG_INPUT* Input)
{
    Usb4HrConfigRequest* Entry;
    PVOID Output;
    size_t OutputSize;
    NTSTATUS Status;

    if (Kind == Usb4HrConfigKind::ReadEx)
        OutputSize = sizeof(USB4HR_CONFIG_EX_OUTPUT);
    else
        OutputSize = sizeof(USB4HR_CONFIG_OUTPUT);

    Status = WdfRequestRetrieveOutputBuffer(Request, OutputSize, &Output, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Config IOCTL output buffer failed 0x%lx\n", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    if (Input->DwordCount == 0 || Input->DwordCount > USB4HR_MAX_CONFIG_DWORDS || Input->Buffer == NULL)
    {
        DPRINT1("Config IOCTL with %lu dwords at %p\n", Input->DwordCount, Input->Buffer);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    Entry = (Usb4HrConfigRequest*)ExAllocatePoolWithTag(NonPagedPool, sizeof(*Entry), USB4HR_TAG_CONFIG);
    if (Entry == NULL)
    {
        DPRINT1("No memory for a config request\n");
        WdfRequestComplete(Request, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }

    RtlZeroMemory(Entry, sizeof(*Entry));
    Entry->Request = Request;
    Entry->Kind = Kind;
    Entry->Target = *Target;
    Entry->DwordOffset = Input->DwordOffset;
    Entry->DwordCount = Input->DwordCount;
    Entry->Buffer = Input->Buffer;
    Entry->Output = Output;
    if (Kind == Usb4HrConfigKind::Write)
        RtlCopyMemory(Entry->Data, Input->Buffer, Input->DwordCount * sizeof(ULONG));

    if (!Enqueue(Entry))
    {
        Entry->Status = USB4HR_STATUS_FAILURE;
        Usb4HrCompleteIoctlEntry(Entry);
    }
}

NTSTATUS
Usb4HrConfigAccessor::RunSynchronous(
    _Inout_ Usb4HrConfigRequest* Entry,
    _Out_ PUSB4HR_STATUS Status)
{
    KEVENT Done;

    KeInitializeEvent(&Done, NotificationEvent, FALSE);
    Entry->Done = &Done;

    if (!Enqueue(Entry))
    {
        *Status = USB4HR_STATUS_FAILURE;
        return STATUS_DEVICE_NOT_READY;
    }

    KeWaitForSingleObject(&Done, Executive, KernelMode, FALSE, NULL);
    *Status = Entry->Status;
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrConfigAccessor::Read(
    _In_ const Usb4HrConfigTarget* Target,
    _In_ ULONG DwordOffset,
    _In_ ULONG DwordCount,
    _Out_writes_(DwordCount) PULONG Buffer,
    _Out_ PUSB4HR_STATUS Status)
{
    Usb4HrConfigRequest Entry;

    *Status = USB4HR_STATUS_FAILURE;
    if (DwordCount == 0 || DwordCount > USB4HR_MAX_CONFIG_DWORDS)
    {
        DPRINT1("Config read of %lu dwords\n", DwordCount);
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(Buffer, DwordCount * sizeof(*Buffer));
    RtlZeroMemory(&Entry, sizeof(Entry));
    Entry.Kind = Usb4HrConfigKind::Read;
    Entry.Target = *Target;
    Entry.DwordOffset = DwordOffset;
    Entry.DwordCount = DwordCount;
    Entry.Buffer = Buffer;

    return RunSynchronous(&Entry, Status);
}

NTSTATUS
Usb4HrConfigAccessor::Write(
    _In_ const Usb4HrConfigTarget* Target,
    _In_ ULONG DwordOffset,
    _In_ ULONG DwordCount,
    _In_reads_(DwordCount) const ULONG* Buffer,
    _Out_ PUSB4HR_STATUS Status)
{
    Usb4HrConfigRequest Entry;

    *Status = USB4HR_STATUS_FAILURE;
    if (DwordCount == 0 || DwordCount > USB4HR_MAX_CONFIG_DWORDS)
    {
        DPRINT1("Config write of %lu dwords\n", DwordCount);
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&Entry, sizeof(Entry));
    Entry.Kind = Usb4HrConfigKind::Write;
    Entry.Target = *Target;
    Entry.DwordOffset = DwordOffset;
    Entry.DwordCount = DwordCount;
    RtlCopyMemory(Entry.Data, Buffer, DwordCount * sizeof(ULONG));

    return RunSynchronous(&Entry, Status);
}
