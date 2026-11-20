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
 * wave miniport.
 */

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <qcom/aprif.h>

#define QCDBG_COMPONENT "qcomq6"
#include <qcom/qcomdbg.h>

#include "q6graph.h"

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

typedef struct _Q6_DEVICE
{
    WDFDEVICE WdfDevice;
    APR_HANDLE Core;
    KEVENT Replied;
    volatile LONG Answered;
    volatile LONG Up;
    PKTHREAD Worker;
    volatile LONG Stop;
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
        if (Length >= sizeof(APR_HEADER) + sizeof(ULONG))
        {
            ULONG Count = *(const ULONG *)((const UCHAR *)Packet + sizeof(APR_HEADER));
            QC_LOG("adsp core replied, %lu service(s) reported", Count);
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
        /* Older firmware rejects these probes; an answer to the state query then means ready */
        if (Length >= sizeof(APR_HEADER) + sizeof(ULONG) &&
            *(const ULONG *)((const UCHAR *)Packet + sizeof(APR_HEADER)) == AVCS_CMD_GET_STATE)
        {
            InterlockedExchange(&Device->Up, TRUE);
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
        if (InterlockedCompareExchange(&Device->Stop, 0, 0) != 0)
            return;

        if (!Device->Answered)
            Q6CoreAsk(Device, AVCS_CMD_GET_FWK_VERSION);

        if (Device->Answered && Q6CoreAsk(Device, AVCS_CMD_GET_STATE) && Device->Up)
        {
            QC_LOG("q6 audio core up after %lu probe(s)", i + 1);
            return;
        }

        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }

    QC_LOG("q6: adsp core %s", Device->Answered ? "answers but never reported ready" : "did not answer the probes");
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
    PQ6_DEVICE Device = &Q6Global;
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

    RtlZeroMemory(Device, sizeof(*Device));
    Device->WdfDevice = WdfDevice;
    KeInitializeEvent(&Device->Replied, NotificationEvent, FALSE);

    WdfControlFinishInitializing(WdfDevice);
    Q6ControlDevice = WdfDevice;

    /* Register the AFE/ADM/ASM services so playback can be driven once up */
    QcQ6GraphInit();

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
