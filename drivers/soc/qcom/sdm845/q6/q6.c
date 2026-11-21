/*
 * PROJECT:     ReactOS Qualcomm audio support (SDM845)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Q6 audio core bring-up over the APR transport
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The audio DSP exposes its services (core, AFE, ASM, ADM) over APR once the
 * subsystem driver has booted it. This driver registers for the core service,
 * waits for the APR channel, probes the framework version and then polls the
 * core until it reports the audio services ready. The playback graph (AFE
 * port, ASM session, ADM route) builds on top of this and is driven from the
 * wave miniport. The voice call path uses the service api versions the core
 * reports and the board ports from the Parameters key.
 */

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <qcom/aprif.h>

#define QCDBG_COMPONENT "qcomq6"
#include <qcom/qcomdbg.h>

#include "q6graph.h"
#include "q6smmu.h"
#include "q6voice.h"

#define Q6_CORE_PROBE_TRIES         60
#define Q6_CORE_PROBE_DELAY_MS      500
#define Q6_PROBE_TOKEN              'VrE6'

/* Core service (AVCS) version handshake, header only request */
#define AVCS_CMD_GET_FWK_VERSION    0x0001292CUL
#define AVCS_CMDRSP_GET_FWK_VERSION 0x0001292DUL
#define AVCS_CMD_GET_VERSIONS       0x00012905UL
#define AVCS_CMDRSP_GET_VERSIONS    0x00012906UL
#define AVCS_CMD_GET_STATE          0x0001290CUL
#define AVCS_CMDRSP_GET_STATE       0x0001290DUL
#define AVCS_STATE_READY            1
#define APR_BASIC_RSP_RESULT        0x000110E8UL
#define Q6_DSP_EUNSUPPORTED         3
#define Q6_MAX_SERVICES             25

/* I2S data line mask the speaker link used before the board key existed */
#define Q6_DEFAULT_RX_LINE          2

/* SDM845 apps SMMU, and the stream id the IORT gives the ADSP audio memory client */
#define Q6_APPS_SMMU_BASE           0x15000000UL
#define Q6_AUDIO_STREAM_ID          0x1821

/* Without these bits above bit 31 the ADSP reads host memory as its own stream 0x1820 */
#define Q6_AUDIO_SID_MASK           0xF

/* The IP accelerator streams 0x720 and 0x722 are left to qcipa: with our entries in
   place it never loads its firmware. The registry can still set them for a test */
#define Q6_IPA_STREAM_ID            MAXULONG
#define Q6_IPA_STREAM_ID2           MAXULONG

typedef enum _Q6_VERSION_STATE
{
    Q6VersionNotAsked,
    Q6VersionKnown,
    Q6VersionUnsupported
} Q6_VERSION_STATE;

typedef struct _Q6_SERVICE_VERSION
{
    ULONG ServiceId;
    ULONG ApiVersion;
    ULONG BranchVersion;
} Q6_SERVICE_VERSION;

typedef struct _Q6_DEVICE
{
    WDFDEVICE WdfDevice;
    APR_HANDLE Core;
    KEVENT Replied;
    volatile LONG Answered;
    volatile LONG Up;
    PKTHREAD Worker;
    volatile LONG Stop;

    /* Serializes core queries between the probe worker and the voice path */
    KMUTEX AskLock;
    Q6_VERSION_STATE VersionState;
    ULONG ServiceCount;
    Q6_SERVICE_VERSION Services[Q6_MAX_SERVICES];
} Q6_DEVICE, *PQ6_DEVICE;

static Q6_DEVICE Q6Global;
static WDFDEVICE Q6ControlDevice;

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD Q6EvtDriverUnload;

/* Exported so the audio miniport can gate on the DSP being up */
BOOLEAN
NTAPI
QcQ6CoreIsUp(VOID)
{
    return InterlockedCompareExchange(&Q6Global.Up, 0, 0) != 0;
}

static
VOID
NTAPI
Q6CoreReceive(
    _In_ PVOID Context,
    _In_reads_bytes_(Length) const VOID *Packet,
    _In_ ULONG Length)
{
    PQ6_DEVICE Device = Context;
    const APR_HEADER *Hdr = Packet;

    if (Length < sizeof(APR_HEADER))
        return;

    if (Hdr->Opcode == AVCS_CMDRSP_GET_FWK_VERSION || Hdr->Opcode == AVCS_CMDRSP_GET_VERSIONS)
    {
        /* Build major, minor, branch and subbranch, then the service count */
        if (Hdr->Opcode == AVCS_CMDRSP_GET_FWK_VERSION && Length >= sizeof(APR_HEADER) + 5 * sizeof(ULONG))
        {
            const ULONG *Version = (const ULONG *)((const UCHAR *)Packet + sizeof(APR_HEADER));
            ULONG Count = Version[4];

            QC_LOG("adsp framework %lu.%lu.%lu.%lu, %lu service(s)",
                   Version[0], Version[1], Version[2], Version[3], Count);

            /* The service table follows the build numbers and the count */
            if (Count <= Q6_MAX_SERVICES &&
                Length >= sizeof(APR_HEADER) + 5 * sizeof(ULONG) + Count * sizeof(Q6_SERVICE_VERSION))
            {
                RtlCopyMemory(Device->Services, &Version[5], Count * sizeof(Q6_SERVICE_VERSION));
                Device->ServiceCount = Count;
                Device->VersionState = Q6VersionKnown;
            }
        }
        else
        {
            QC_LOG("adsp core replied to version probe");
        }

        InterlockedExchange(&Device->Answered, TRUE);
        KeSetEvent(&Device->Replied, IO_NO_INCREMENT, FALSE);
    }
    else if (Hdr->Opcode == AVCS_CMDRSP_GET_STATE)
    {
        if (Length >= sizeof(APR_HEADER) + sizeof(ULONG) &&
            *(const ULONG *)((const UCHAR *)Packet + sizeof(APR_HEADER)) == AVCS_STATE_READY)
        {
            InterlockedExchange(&Device->Up, TRUE);
        }
        InterlockedExchange(&Device->Answered, TRUE);
        KeSetEvent(&Device->Replied, IO_NO_INCREMENT, FALSE);
    }
    else if (Hdr->Opcode == APR_BASIC_RSP_RESULT)
    {
        const ULONG *Rsp = (const ULONG *)((const UCHAR *)Packet + sizeof(APR_HEADER));

        /* Older firmware rejects these probes; an answer to the state query then means ready */
        if (Length >= sizeof(APR_HEADER) + sizeof(ULONG) && Rsp[0] == AVCS_CMD_GET_STATE)
            InterlockedExchange(&Device->Up, TRUE);

        /* Only an explicit unsupported answer stops later version queries */
        if (Length >= sizeof(APR_HEADER) + 2 * sizeof(ULONG) &&
            Rsp[0] == AVCS_CMD_GET_FWK_VERSION && Rsp[1] == Q6_DSP_EUNSUPPORTED)
        {
            Device->VersionState = Q6VersionUnsupported;
        }
        InterlockedExchange(&Device->Answered, TRUE);
        KeSetEvent(&Device->Replied, IO_NO_INCREMENT, FALSE);
    }
}

/* Sends one header-only core command and waits for any answer */
static
BOOLEAN
Q6CoreAsk(
    _In_ PQ6_DEVICE Device,
    _In_ ULONG Opcode)
{
    LARGE_INTEGER Timeout;

    Timeout.QuadPart = -10LL * 1000 * Q6_CORE_PROBE_DELAY_MS;
    KeClearEvent(&Device->Replied);

    if (!NT_SUCCESS(QcAprSend(Device->Core, APR_SVC_ADSP_CORE, Opcode, Q6_PROBE_TOKEN, NULL, 0)))
        return FALSE;

    return KeWaitForSingleObject(&Device->Replied, Executive, KernelMode, FALSE, &Timeout) == STATUS_SUCCESS;
}

static
VOID
Q6Worker(
    _In_ PVOID Context)
{
    PQ6_DEVICE Device = Context;
    LARGE_INTEGER Delay;
    ULONG i;

    Device->Core = QcAprRegister(APR_DOMAIN_ADSP, APR_SVC_ADSP_CORE, Q6CoreReceive, Device);
    if (!Device->Core)
    {
        QC_LOG("q6: could not register the core service with apr");
        return;
    }

    Delay.QuadPart = -10LL * 1000 * Q6_CORE_PROBE_DELAY_MS;

    /* The channel only opens once qcsubsys has the adsp running */
    while (!QcAprIsConnected())
    {
        if (InterlockedCompareExchange(&Device->Stop, 0, 0) != 0)
            return;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }

    for (i = 0; i < Q6_CORE_PROBE_TRIES; i++)
    {
        BOOLEAN Ready;

        if (InterlockedCompareExchange(&Device->Stop, 0, 0) != 0)
            return;

        KeWaitForSingleObject(&Device->AskLock, Executive, KernelMode, FALSE, NULL);
        if (!Device->Answered)
            Q6CoreAsk(Device, AVCS_CMD_GET_FWK_VERSION);

        Ready = Device->Answered && Q6CoreAsk(Device, AVCS_CMD_GET_STATE) && Device->Up;
        KeReleaseMutex(&Device->AskLock, FALSE);

        if (Ready)
        {
            QC_LOG("q6 audio core up after %lu probe(s)", i + 1);
            return;
        }

        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }

    QC_LOG("q6: adsp core %s", Device->Answered ? "answers but never reported ready" : "did not answer the probes");
}

NTSTATUS
NTAPI
QcQ6CoreServiceVersion(
    _In_ ULONG ServiceId,
    _Out_ PULONG ApiVersion)
{
    PQ6_DEVICE Device = &Q6Global;
    NTSTATUS Status = STATUS_NOT_FOUND;
    ULONG i;

    PAGED_CODE();

    *ApiVersion = 0;
    KeWaitForSingleObject(&Device->AskLock, Executive, KernelMode, FALSE, NULL);

    /* A query that timed out earlier is tried again, like the first one */
    if (Device->VersionState == Q6VersionNotAsked)
    {
        if (QcQ6CoreIsUp())
            Q6CoreAsk(Device, AVCS_CMD_GET_FWK_VERSION);
        else
            Status = STATUS_DEVICE_NOT_READY;
    }

    if (Device->VersionState == Q6VersionKnown)
    {
        for (i = 0; i < Device->ServiceCount; i++)
        {
            if (Device->Services[i].ServiceId == ServiceId)
            {
                *ApiVersion = Device->Services[i].ApiVersion;
                Status = STATUS_SUCCESS;
                break;
            }
        }
    }
    else if (Device->VersionState == Q6VersionUnsupported)
    {
        Status = STATUS_NOT_SUPPORTED;
    }
    else if (Status == STATUS_NOT_FOUND)
    {
        Status = STATUS_IO_TIMEOUT;
    }

    KeReleaseMutex(&Device->AskLock, FALSE);

    QC_LOG("core service 0x%lx api version %lu: 0x%08lx", ServiceId, *ApiVersion, Status);
    return Status;
}

/* Reads one REG_DWORD of the Parameters key, keeping the default when absent */
static
ULONG
Q6ReadParameter(
    _In_opt_ WDFKEY Key,
    _In_ PCUNICODE_STRING Name,
    _In_ ULONG Default)
{
    ULONG Value;

    if (Key && NT_SUCCESS(WdfRegistryQueryULong(Key, Name, &Value)))
        return Value;
    return Default;
}

static
NTSTATUS
Q6CreateControlDevice(
    _In_ WDFDRIVER Driver)
{
    PWDFDEVICE_INIT DeviceInit;
    WDF_OBJECT_ATTRIBUTES Attributes;
    DECLARE_CONST_UNICODE_STRING(DeviceName, L"\\Device\\QcQ6");
    DECLARE_CONST_UNICODE_STRING(Sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    DECLARE_CONST_UNICODE_STRING(RxPortName, L"VoiceRxPort");
    DECLARE_CONST_UNICODE_STRING(TxPortName, L"VoiceTxPort");
    DECLARE_CONST_UNICODE_STRING(RxLineName, L"QuatMi2sRxLine");
    DECLARE_CONST_UNICODE_STRING(TxLineName, L"QuatMi2sTxLine");
    DECLARE_CONST_UNICODE_STRING(SmmuBaseName, L"AudioSmmuBase");
    DECLARE_CONST_UNICODE_STRING(StreamIdName, L"AudioStreamId");
    DECLARE_CONST_UNICODE_STRING(HostBuffersName, L"AllowUnroutedHostBuffers");
    DECLARE_CONST_UNICODE_STRING(RouteName, L"RouteAudioStream");
    DECLARE_CONST_UNICODE_STRING(SidMaskName, L"AudioStreamSidMask");
    DECLARE_CONST_UNICODE_STRING(IpaStreamName, L"IpaStreamId");
    DECLARE_CONST_UNICODE_STRING(IpaStream2Name, L"IpaStreamId2");
    PQ6_DEVICE Device = &Q6Global;
    WDFDEVICE WdfDevice;
    WDFKEY Key = NULL;
    ULONG RxPort, TxPort, RxLine, TxLine;
    ULONG SmmuBase, StreamId, HostBuffers, Route, SidMask, IpaStream, IpaStream2;
    ULONGLONG AddressTag = 0;
    BOOLEAN Routed;
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

    RtlZeroMemory(Device, sizeof(*Device));
    Device->WdfDevice = WdfDevice;
    KeInitializeEvent(&Device->Replied, NotificationEvent, FALSE);
    KeInitializeMutex(&Device->AskLock, 0);

    WdfControlFinishInitializing(WdfDevice);
    Q6ControlDevice = WdfDevice;

    /* Board wiring: which I2S lines carry each MI2S port, which ports a call uses */
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(Driver, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &Key)))
        Key = NULL;
    RxLine = Q6ReadParameter(Key, &RxLineName, Q6_DEFAULT_RX_LINE);
    TxLine = Q6ReadParameter(Key, &TxLineName, 0);
    RxPort = Q6ReadParameter(Key, &RxPortName, MAXUSHORT);
    TxPort = Q6ReadParameter(Key, &TxPortName, MAXUSHORT);
    SmmuBase = Q6ReadParameter(Key, &SmmuBaseName, Q6_APPS_SMMU_BASE);
    StreamId = Q6ReadParameter(Key, &StreamIdName, Q6_AUDIO_STREAM_ID);
    HostBuffers = Q6ReadParameter(Key, &HostBuffersName, 0);
    Route = Q6ReadParameter(Key, &RouteName, 1);
    SidMask = Q6ReadParameter(Key, &SidMaskName, Q6_AUDIO_SID_MASK);
    IpaStream = Q6ReadParameter(Key, &IpaStreamName, Q6_IPA_STREAM_ID);
    IpaStream2 = Q6ReadParameter(Key, &IpaStream2Name, Q6_IPA_STREAM_ID2);
    if (Key)
        WdfRegistryClose(Key);

    /* Host buffer addresses only go to the dsp once its stream reaches memory */
    Routed = QcQ6SmmuRouteStream(SmmuBase, (USHORT)StreamId, 0, Route != 0);

    /* The IP accelerator reads and writes packet buffers on its own streams. No SMMU
       driver here routes them yet, so they get the same untranslated route */
    if (IpaStream <= 0x7FFF)
        QcQ6SmmuRouteStream(SmmuBase, (USHORT)IpaStream, 0, Route != 0);
    if (IpaStream2 <= 0x7FFF)
        QcQ6SmmuRouteStream(SmmuBase, (USHORT)IpaStream2, 0, Route != 0);
    if (Routed)
        AddressTag = (ULONGLONG)(StreamId & SidMask) << 32;
    QC_LOG("q6: host buffer addresses tagged 0x%I64x for stream 0x%lx", AddressTag, StreamId);
    QcQ6GraphAllowHostBuffers(Routed || HostBuffers != 0, AddressTag);

    QC_LOG("board: mi2s lines rx 0x%lx tx 0x%lx, voice ports rx 0x%lx tx 0x%lx",
           RxLine, TxLine, RxPort, TxPort);

    /* Register the AFE/ADM/ASM services so playback can be driven once up */
    QcQ6GraphInit(RxLine, TxLine);
    QcQ6VoiceInit((USHORT)RxPort, (USHORT)TxPort);

    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, Q6Worker, Device);
    if (NT_SUCCESS(Status))
    {
        ObReferenceObjectByHandle(Thread, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&Device->Worker, NULL);
        ZwClose(Thread);
    }

    QC_LOG("q6 core loaded, waiting for the apr channel to probe the audio dsp");
    return STATUS_SUCCESS;
}

static
VOID
Q6EvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    PQ6_DEVICE Device = &Q6Global;

    UNREFERENCED_PARAMETER(Driver);

    InterlockedExchange(&Device->Stop, TRUE);
    if (Device->Worker)
    {
        KeWaitForSingleObject(Device->Worker, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Device->Worker);
        Device->Worker = NULL;
    }

    if (Device->Core)
    {
        QcAprUnregister(Device->Core);
        Device->Core = NULL;
    }
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
    Config.EvtDriverUnload = Q6EvtDriverUnload;

    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, &Driver);
    if (!NT_SUCCESS(Status))
        return Status;

    return Q6CreateControlDevice(Driver);
}
