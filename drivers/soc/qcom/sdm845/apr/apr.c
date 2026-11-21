/*
 * PROJECT:     ReactOS Qualcomm audio support (SDM845)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Asynchronous Packet Router over glink to the audio DSP
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * APR carries the audio command protocol between the applications processor
 * and the services on the ADSP (core, AFE, ASM, ADM). It rides one glink
 * channel, apr_audio_svc, on the lpass edge of the platform glink driver.
 * This driver opens that channel, routes inbound packets to whichever
 * in-kernel service registered for the destination service id, and fills the
 * APR header on the way out. Audio miniports consume the exported QcApr* calls.
 *
 * Received packets are copied out of the glink intent and handed to the
 * services from the worker thread, so every receive callback runs at
 * PASSIVE_LEVEL and may send.
 */

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <qcom/glinkif.h>
#include <qcom/aprif.h>

#define QCDBG_COMPONENT "qcomapr"
#include <qcom/qcomdbg.h>

#define APR_TAG                     'rpAQ'
#define APR_CHANNEL_NAME            "apr_audio_svc"
#define APR_MAX_SERVICES            16
#define APR_APPS_PORT_BASE          0x0100
#define APR_INTENT_SIZE             4096
#define APR_INTENT_COUNT            8
#define APR_RETRY_DELAY_MS          500

/* One registered local service and its receive callback */
typedef struct _APR_SERVICE
{
    BOOLEAN InUse;
    UCHAR Service;
    UCHAR Domain;
    USHORT Port;
    PQCOM_APR_RECEIVE Receive;
    PVOID Context;
} APR_SERVICE, *PAPR_SERVICE;

/* A packet copied out of glink, waiting for the worker */
typedef struct _APR_RX_PACKET
{
    LIST_ENTRY Link;
    ULONG Length;
    UCHAR Data[ANYSIZE_ARRAY];
} APR_RX_PACKET, *PAPR_RX_PACKET;

typedef struct _APR_DEVICE
{
    WDFDEVICE WdfDevice;
    WDFIOTARGET GlinkTarget;
    QCOM_GLINK_INTERFACE Glink;
    BOOLEAN HaveGlink;
    QCOM_GLINK_HANDLE Channel;
    volatile LONG Connected;
    volatile LONG NeedIntents;
    volatile LONG RemoteGone;
    volatile LONG PendingIntent;
    KSPIN_LOCK Lock;
    APR_SERVICE Services[APR_MAX_SERVICES];
    KSPIN_LOCK RxLock;
    LIST_ENTRY RxList;
    KEVENT Wake;
    PKTHREAD Worker;
    volatile LONG Stop;
} APR_DEVICE, *PAPR_DEVICE;

/* Left zeroed by the loader, a client may register before DriverEntry runs */
static APR_DEVICE AprGlobal;
static WDFDEVICE AprControlDevice;

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD AprEvtDriverUnload;

/* How many packets each direction traces before only the unusual ones are logged */
#define APR_TRACE_PACKETS           256

static volatile LONG AprRxTraced;
static volatile LONG AprTxTraced;

/* One line per packet: header field, routing, token, opcode and the first payload words */
static
VOID
AprTraceRx(
    _In_reads_bytes_(Length) const VOID *Packet,
    _In_ ULONG Length)
{
    const APR_HEADER *Header = Packet;
    const ULONG *Words = (const ULONG *)((const UCHAR *)Packet + sizeof(APR_HEADER));
    ULONG HeaderBytes = ((Header->HdrField >> 4) & 0xF) * 4;
    ULONG Count = (Length - sizeof(APR_HEADER)) / sizeof(ULONG);
    ULONG Value[4] = { 0 };
    ULONG i;

    if (HeaderBytes == sizeof(APR_HEADER) && InterlockedIncrement(&AprRxTraced) > APR_TRACE_PACKETS)
        return;

    for (i = 0; i < RTL_NUMBER_OF(Value) && i < Count; i++)
        Value[i] = Words[i];

    QC_LOG("apr rx: field %04x (%lu byte header) size %u len %lu src %u:%u:%u dst %u:%u:%u token %08lx opcode %08lx payload %08lx %08lx %08lx %08lx",
           Header->HdrField, HeaderBytes, Header->PktSize, Length,
           Header->SrcDomain, Header->SrcSvc, Header->SrcPort,
           Header->DestDomain, Header->DestSvc, Header->DestPort,
           Header->Token, Header->Opcode, Value[0], Value[1], Value[2], Value[3]);
}

/* Hands a received packet to the service that owns its destination id */
static
VOID
AprDispatch(
    _In_ PAPR_DEVICE Device,
    _Inout_updates_bytes_(Length) PVOID Packet,
    _In_ ULONG Length)
{
    PAPR_HEADER Header = Packet;
    PQCOM_APR_RECEIVE Receive = NULL;
    PVOID Context = NULL;
    ULONG HeaderBytes;
    PUCHAR Bytes;
    KIRQL OldIrql;
    ULONG i;

    if (Length < sizeof(APR_HEADER))
    {
        QC_LOG("apr rx: runt packet of %lu bytes", Length);
        return;
    }

    AprTraceRx(Packet, Length);

    /* The DSP may send a longer header; services expect the payload right after the basic one */
    HeaderBytes = ((Header->HdrField >> 4) & 0xF) * 4;
    if (HeaderBytes > sizeof(APR_HEADER) && HeaderBytes <= Length)
    {
        Bytes = (PUCHAR)Packet;
        RtlMoveMemory(Bytes + sizeof(APR_HEADER), Bytes + HeaderBytes, Length - HeaderBytes);
        Length -= HeaderBytes - sizeof(APR_HEADER);
        Header->HdrField = (USHORT)((Header->HdrField & ~0xF0) | (APR_HDR_SIZE_WORDS << 4));
        Header->PktSize = (USHORT)Length;
    }

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    for (i = 0; i < APR_MAX_SERVICES; i++)
    {
        if (Device->Services[i].InUse && Device->Services[i].Service == Header->DestSvc)
        {
            Receive = Device->Services[i].Receive;
            Context = Device->Services[i].Context;
            break;
        }
    }
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    if (Receive)
        Receive(Context, Packet, Length);
    else
        QC_LOG("apr: no service for svc %u opcode 0x%08lx", Header->DestSvc, Header->Opcode);
}

/* Delivers every queued packet, called on the worker only */
static
VOID
AprDrain(
    _In_ PAPR_DEVICE Device)
{
    PLIST_ENTRY Entry;
    PAPR_RX_PACKET Rx;

    while ((Entry = ExInterlockedRemoveHeadList(&Device->RxList, &Device->RxLock)) != NULL)
    {
        Rx = CONTAINING_RECORD(Entry, APR_RX_PACKET, Link);
        AprDispatch(Device, Rx->Data, Rx->Length);
        ExFreePoolWithTag(Rx, APR_TAG);
    }
}

static
VOID
AprNotifyRx(
    _In_ QCOM_GLINK_HANDLE Handle,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID PacketContext,
    _In_reads_bytes_(Size) const VOID *Data,
    _In_ SIZE_T Size,
    _In_ SIZE_T IntentSize)
{
    PAPR_DEVICE Device = Context;
    PAPR_RX_PACKET Rx = NULL;

    UNREFERENCED_PARAMETER(PacketContext);
    UNREFERENCED_PARAMETER(IntentSize);

    if (Size && Size <= MAXUSHORT)
    {
        Rx = ExAllocatePoolZero(NonPagedPool, FIELD_OFFSET(APR_RX_PACKET, Data) + Size, APR_TAG);
        if (Rx)
        {
            Rx->Length = (ULONG)Size;
            RtlCopyMemory(Rx->Data, Data, Size);
        }
    }

    /* Hand the intent straight back so the remote can reuse it */
    Device->Glink.RxDone(Handle, Data, TRUE);

    if (!Rx)
        return;

    ExInterlockedInsertTailList(&Device->RxList, &Rx->Link, &Device->RxLock);
    KeSetEvent(&Device->Wake, IO_NO_INCREMENT, FALSE);
}

static
VOID
AprNotifyTxDone(
    _In_ QCOM_GLINK_HANDLE Handle,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID PacketContext,
    _In_ const VOID *Data,
    _In_ SIZE_T Size)
{
    UNREFERENCED_PARAMETER(Handle);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(Size);

    if (PacketContext)
        ExFreePoolWithTag(PacketContext, APR_TAG);
}

static
VOID
AprNotifyState(
    _In_ QCOM_GLINK_HANDLE Handle,
    _In_opt_ PVOID Context,
    _In_ ULONG Event)
{
    PAPR_DEVICE Device = Context;

    UNREFERENCED_PARAMETER(Handle);

    if (Event == QCOM_GLINK_CONNECTED)
    {
        InterlockedExchange(&Device->NeedIntents, TRUE);
        InterlockedExchange(&Device->Connected, TRUE);
    }
    else if (Event == QCOM_GLINK_REMOTE_DISCONNECTED)
    {
        InterlockedExchange(&Device->Connected, FALSE);
        InterlockedExchange(&Device->RemoteGone, TRUE);
    }

    KeSetEvent(&Device->Wake, IO_NO_INCREMENT, FALSE);
}

/* The worker queues the intent, glink grants the request on our return */
static
BOOLEAN
AprNotifyRxIntentRequest(
    _In_ QCOM_GLINK_HANDLE Handle,
    _In_opt_ PVOID Context,
    _In_ SIZE_T Size)
{
    PAPR_DEVICE Device = Context;

    UNREFERENCED_PARAMETER(Handle);

    if (Size == 0 || Size > MAXUSHORT)
        return FALSE;

    InterlockedExchange(&Device->PendingIntent, (LONG)Size);
    KeSetEvent(&Device->Wake, IO_NO_INCREMENT, FALSE);
    return TRUE;
}

/* Opens \Device\GLINK and takes its client table */
static
NTSTATUS
AprAcquireGlink(
    _Inout_ PAPR_DEVICE Device)
{
    WDF_IO_TARGET_OPEN_PARAMS OpenParams;
    DECLARE_CONST_UNICODE_STRING(GlinkName, QCOM_GLINK_DEVICE_NAME);
    NTSTATUS Status;

    if (!Device->GlinkTarget)
    {
        Status = WdfIoTargetCreate(Device->WdfDevice, WDF_NO_OBJECT_ATTRIBUTES, &Device->GlinkTarget);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&OpenParams, &GlinkName, STANDARD_RIGHTS_ALL);
    OpenParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;

    Status = WdfIoTargetOpen(Device->GlinkTarget, &OpenParams);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfIoTargetQueryForInterface(Device->GlinkTarget, &GUID_QCOM_GLINK_INTERFACE,
                                          (PINTERFACE)&Device->Glink, sizeof(Device->Glink),
                                          QCOM_GLINK_INTERFACE_VERSION, NULL);
    if (!NT_SUCCESS(Status))
    {
        WdfIoTargetClose(Device->GlinkTarget);
        return Status;
    }

    Device->HaveGlink = TRUE;
    return STATUS_SUCCESS;
}

static
LONG
AprOpenChannel(
    _Inout_ PAPR_DEVICE Device)
{
    QCOM_GLINK_OPEN_CONFIG Config;

    RtlZeroMemory(&Config, sizeof(Config));
    Config.Remote = QCOM_GLINK_REMOTE_ADSP;
    Config.Name = APR_CHANNEL_NAME;
    Config.Context = Device;
    Config.NotifyRx = AprNotifyRx;
    Config.NotifyTxDone = AprNotifyTxDone;
    Config.NotifyState = AprNotifyState;
    Config.NotifyRxIntentRequest = AprNotifyRxIntentRequest;

    Device->Channel = NULL;
    return Device->Glink.Open(&Config, &Device->Channel);
}

static
VOID
AprQueueIntent(
    _In_ PAPR_DEVICE Device,
    _In_ SIZE_T Size)
{
    LONG Result;

    Result = Device->Glink.QueueRxIntent(Device->Channel, NULL, Size);
    if (Result != 0)
        QC_LOG("apr: queueing a %lu byte intent failed %ld", (ULONG)Size, Result);
}

/* Brings the channel up, then delivers packets and services intent requests */
static
VOID
AprWorker(
    _In_ PVOID Context)
{
    PAPR_DEVICE Device = Context;
    LARGE_INTEGER Delay;
    BOOLEAN Open = FALSE, Logged = FALSE;
    LONG Result, Size;
    ULONG i;

    Delay.QuadPart = -10LL * 1000 * APR_RETRY_DELAY_MS;

    while (InterlockedCompareExchange(&Device->Stop, 0, 0) == 0)
    {
        if (!Device->HaveGlink)
        {
            if (!NT_SUCCESS(AprAcquireGlink(Device)))
            {
                KeDelayExecutionThread(KernelMode, FALSE, &Delay);
                continue;
            }
            QC_LOG("apr: glink client table acquired");
        }

        if (!Open)
        {
            Result = AprOpenChannel(Device);
            if (Result != 0)
            {
                if (!Logged)
                    QC_LOG("apr: glink open of '%s' on %s failed %ld, retrying",
                           APR_CHANNEL_NAME, QCOM_GLINK_REMOTE_ADSP, Result);
                Logged = TRUE;
                KeDelayExecutionThread(KernelMode, FALSE, &Delay);
                continue;
            }
            Open = TRUE;
            QC_LOG("apr channel '%s' opened on %s, waiting for the dsp",
                   APR_CHANNEL_NAME, QCOM_GLINK_REMOTE_ADSP);
        }

        KeWaitForSingleObject(&Device->Wake, Executive, KernelMode, FALSE, &Delay);

        if (InterlockedExchange(&Device->NeedIntents, FALSE))
        {
            for (i = 0; i < APR_INTENT_COUNT; i++)
                AprQueueIntent(Device, APR_INTENT_SIZE);
            QC_LOG("apr channel '%s' connected to the adsp", APR_CHANNEL_NAME);
        }

        Size = InterlockedExchange(&Device->PendingIntent, 0);
        if (Size)
            AprQueueIntent(Device, (SIZE_T)Size);

        AprDrain(Device);

        if (InterlockedExchange(&Device->RemoteGone, FALSE))
        {
            QC_LOG("apr: adsp closed '%s', reopening", APR_CHANNEL_NAME);
            Device->Glink.Close(Device->Channel);
            Device->Channel = NULL;
            Open = FALSE;
            Logged = FALSE;
        }
    }

    if (Open)
    {
        InterlockedExchange(&Device->Connected, FALSE);
        Device->Glink.Close(Device->Channel);
        Device->Channel = NULL;
    }
    AprDrain(Device);
}

/* ---- exported interface for audio miniports ---- */

APR_HANDLE
NTAPI
QcAprRegister(
    _In_ ULONG Domain,
    _In_ ULONG Service,
    _In_ PQCOM_APR_RECEIVE Receive,
    _In_opt_ PVOID Context)
{
    PAPR_DEVICE Device = &AprGlobal;
    PAPR_SERVICE Slot = NULL;
    KIRQL OldIrql;
    ULONG i;

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    for (i = 0; i < APR_MAX_SERVICES; i++)
    {
        if (!Device->Services[i].InUse)
        {
            Slot = &Device->Services[i];
            Slot->InUse = TRUE;
            Slot->Service = (UCHAR)Service;
            Slot->Domain = (UCHAR)Domain;
            Slot->Port = (USHORT)(i + 1);
            Slot->Receive = Receive;
            Slot->Context = Context;
            break;
        }
    }
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    return Slot;
}

VOID
NTAPI
QcAprUnregister(
    _In_ APR_HANDLE Handle)
{
    PAPR_DEVICE Device = &AprGlobal;
    PAPR_SERVICE Slot = Handle;
    KIRQL OldIrql;

    if (!Slot)
        return;

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    Slot->InUse = FALSE;
    KeReleaseSpinLock(&Device->Lock, OldIrql);
}

BOOLEAN
NTAPI
QcAprIsConnected(VOID)
{
    return InterlockedCompareExchange(&AprGlobal.Connected, 0, 0) != 0;
}

/* Glink may wait for a remote intent, so this is PASSIVE_LEVEL only */
NTSTATUS
NTAPI
QcAprSendTo(
    _In_ APR_HANDLE Handle,
    _In_ ULONG DestService,
    _In_ ULONG DestPort,
    _In_ ULONG SrcPort,
    _In_ ULONG Opcode,
    _In_ ULONG Token,
    _In_reads_bytes_opt_(Length) const VOID *Payload,
    _In_ ULONG Length)
{
    PAPR_DEVICE Device = &AprGlobal;
    PAPR_SERVICE Slot = Handle;
    PUCHAR Packet;
    PAPR_HEADER Header;
    ULONG Total;
    LONG Result;

    if (!Slot || !InterlockedCompareExchange(&Device->Connected, 0, 0))
        return STATUS_DEVICE_NOT_READY;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;
    if (Length > MAXUSHORT - sizeof(APR_HEADER))
        return STATUS_INVALID_PARAMETER;

    Total = sizeof(APR_HEADER) + Length;
    Packet = ExAllocatePoolZero(NonPagedPool, Total, APR_TAG);
    if (!Packet)
        return STATUS_INSUFFICIENT_RESOURCES;

    Header = (PAPR_HEADER)Packet;
    Header->HdrField = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, APR_HDR_SIZE_WORDS, APR_PKT_VERSION);
    Header->PktSize = (USHORT)Total;
    Header->SrcSvc = Slot->Service;
    Header->SrcDomain = APR_DOMAIN_APPS;
    Header->SrcPort = (USHORT)SrcPort;
    Header->DestSvc = (UCHAR)DestService;
    Header->DestDomain = Slot->Domain;
    Header->DestPort = (USHORT)DestPort;
    Header->Token = Token;
    Header->Opcode = Opcode;

    if (Length && Payload)
        RtlCopyMemory(Packet + sizeof(APR_HEADER), Payload, Length);

    if (InterlockedIncrement(&AprTxTraced) <= APR_TRACE_PACKETS)
    {
        QC_LOG("apr tx: svc %lu port %lu from port %lu opcode %08lx token %08lx payload %lu bytes",
               DestService, DestPort, SrcPort, Opcode, Token, Length);
    }

    /* The packet is freed in the tx done callback once glink is finished with it */
    Result = Device->Glink.Tx(Device->Channel, Packet, Packet, Total, QCOM_GLINK_TX_REQUEST_INTENT);
    if (Result != 0)
    {
        ExFreePoolWithTag(Packet, APR_TAG);
        QC_LOG("apr: tx of opcode 0x%08lx failed %ld", Opcode, Result);
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
QcAprSend(
    _In_ APR_HANDLE Handle,
    _In_ ULONG DestService,
    _In_ ULONG Opcode,
    _In_ ULONG Token,
    _In_reads_bytes_opt_(Length) const VOID *Payload,
    _In_ ULONG Length)
{
    PAPR_SERVICE Slot = Handle;

    if (!Slot)
        return STATUS_DEVICE_NOT_READY;

    return QcAprSendTo(Handle, DestService, 0, APR_APPS_PORT_BASE + Slot->Port,
                       Opcode, Token, Payload, Length);
}

static
NTSTATUS
AprCreateControlDevice(
    _In_ WDFDRIVER Driver)
{
    PWDFDEVICE_INIT DeviceInit;
    WDF_OBJECT_ATTRIBUTES Attributes;
    DECLARE_CONST_UNICODE_STRING(DeviceName, L"\\Device\\QcApr");
    DECLARE_CONST_UNICODE_STRING(Sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    PAPR_DEVICE Device = &AprGlobal;
    WDFDEVICE WdfDevice;
    HANDLE Thread;
    NTSTATUS Status;

    DeviceInit = WdfControlDeviceInitAllocate(Driver, &Sddl);
    if (!DeviceInit)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = WdfDeviceInitAssignName(DeviceInit, &DeviceName);
    if (!NT_SUCCESS(Status))
    {
        WdfDeviceInitFree(DeviceInit);
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &WdfDevice);
    if (!NT_SUCCESS(Status))
        return Status;

    Device->WdfDevice = WdfDevice;
    KeInitializeSpinLock(&Device->RxLock);
    InitializeListHead(&Device->RxList);
    KeInitializeEvent(&Device->Wake, SynchronizationEvent, FALSE);

    WdfControlFinishInitializing(WdfDevice);
    AprControlDevice = WdfDevice;

    /* Glink comes up with PnP well after this driver, the worker waits for it */
    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, AprWorker, Device);
    if (!NT_SUCCESS(Status))
        return Status;

    ObReferenceObjectByHandle(Thread, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&Device->Worker, NULL);
    ZwClose(Thread);

    QC_LOG("apr transport up, waiting for glink and the adsp channel");
    return STATUS_SUCCESS;
}

static
VOID
AprEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    PAPR_DEVICE Device = &AprGlobal;

    UNREFERENCED_PARAMETER(Driver);

    InterlockedExchange(&Device->Stop, TRUE);
    KeSetEvent(&Device->Wake, IO_NO_INCREMENT, FALSE);
    if (Device->Worker)
    {
        KeWaitForSingleObject(Device->Worker, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Device->Worker);
        Device->Worker = NULL;
    }

    if (Device->GlinkTarget)
    {
        WdfIoTargetClose(Device->GlinkTarget);
        WdfObjectDelete(Device->GlinkTarget);
        Device->GlinkTarget = NULL;
    }
    Device->HaveGlink = FALSE;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    WDFDRIVER Driver;
    NTSTATUS Status;

    WDF_DRIVER_CONFIG_INIT(&Config, WDF_NO_EVENT_CALLBACK);
    Config.DriverInitFlags |= WdfDriverInitNonPnpDriver;
    Config.EvtDriverUnload = AprEvtDriverUnload;

    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, &Driver);
    if (!NT_SUCCESS(Status))
        return Status;

    return AprCreateControlDevice(Driver);
}
