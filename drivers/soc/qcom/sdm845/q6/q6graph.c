/*
 * PROJECT:     ReactOS Qualcomm audio support (SDM845)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Q6 playback graph (AFE, ADM, ASM) over APR
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A PCM playback stream on the dsp is three services wired together: the ASM
 * owns the stream and the shared pcm buffer, the AFE owns the backend port
 * that clocks samples out, and the ADM routes the ASM session onto the AFE
 * port through a COPP. The backend here is the quaternary MI2S port feeding
 * the speaker amp: LPASS bit clock first, then the I2S port, then the stream
 * (memory map, open, route, media format, run). The buffer is fed to the dsp
 * one period at a time and each period is handed back on its write done.
 *
 * The backend always runs 48 kHz, 16 bit, two slots; the ASM and the matrix
 * convert whatever rate and channel count the stream uses. The voice path uses
 * the same quaternary MI2S ports, so each port, the shared bit clock and the
 * amp are counted and only the first user starts them.
 */

#include <ntddk.h>
#include <qcom/aprif.h>
#include <qcom/ampif.h>

#define QCDBG_COMPONENT "qcomq6"
#include <qcom/qcomdbg.h>

#include "q6graph.h"

/* APR generic response */
#define APR_BASIC_RSP_RESULT            0x000110E8UL

/* AFE */
#define AFE_PORT_CMD_DEVICE_START       0x000100E5UL
#define AFE_PORT_CMD_DEVICE_STOP        0x000100E6UL
#define AFE_PORT_CMD_SET_PARAM_V2       0x000100EFUL
#define AFE_SVC_CMD_SET_PARAM           0x000100F3UL
#define AFE_MODULE_AUDIO_DEV_INTERFACE  0x0001020CUL
#define AFE_PARAM_ID_I2S_CONFIG         0x0001020DUL
#define AFE_MODULE_CLOCK_SET            0x0001028FUL
#define AFE_PARAM_ID_CLOCK_SET          0x00010290UL
#define AFE_PORT_ID_QUAT_MI2S_RX        0x1006
#define AFE_PORT_ID_QUAT_MI2S_TX        0x1007

/* I2S port config values */
#define Q6_I2S_CONFIG_VERSION           1
#define Q6_I2S_STEREO                   1
#define Q6_I2S_WS_INTERNAL              1

/* LPASS clock set values, quaternary MI2S internal bit clock */
#define Q6_CLOCK_SET_VERSION            1
#define Q6_CLOCK_QUAT_MI2S_IBIT         0x106
#define Q6_CLOCK_NOT_COUPLED            1
#define Q6_CLOCK_ROOT_DEFAULT           0

/* ADM */
#define ADM_CMD_DEVICE_OPEN_V5          0x00010326UL
#define ADM_CMDRSP_DEVICE_OPEN_V5       0x00010329UL
#define ADM_CMD_DEVICE_CLOSE_V5         0x00010327UL
#define ADM_CMD_MATRIX_MAP_ROUTINGS_V5  0x00010325UL
#define ADM_NULL_COPP_TOPOLOGY          0x00010312UL
#define ADM_MATRIX_ID_AUDIO_RX          0
#define ADM_LEGACY_DEVICE_SESSION       0
#define ADM_PATH_PLAYBACK               1

/* ASM */
#define ASM_STREAM_CMD_OPEN_WRITE_V3    0x00010DB3UL
#define ASM_SESSION_CMD_RUN_V2          0x00010DAAUL
#define ASM_SESSION_CMD_PAUSE           0x00010BD3UL
#define ASM_STREAM_CMD_CLOSE            0x00010BCDUL
#define ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2 0x00010D98UL
#define ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2 0x00010DA5UL
#define ASM_NULL_POPP_TOPOLOGY          0x00010C68UL
#define ASM_CMD_SHARED_MEM_MAP_REGIONS  0x00010D92UL
#define ASM_CMDRSP_SHARED_MEM_MAP_REGIONS 0x00010D93UL
#define ASM_CMD_SHARED_MEM_UNMAP_REGIONS 0x00010D94UL
#define ASM_DATA_CMD_WRITE_V2           0x00010DABUL
#define ASM_DATA_EVENT_WRITE_DONE_V2    0x00010D99UL
#define ASM_MEM_POOL_SHMEM8_4K          3
#define ASM_SINK_DEVICE_MATRIX          0

#define Q6_SESSION                      1
#define Q6_STREAM                       1
#define Q6_PCM_MAX_CHANNELS             8
#define Q6_CMD_TIMEOUT_MS               1000
#define Q6_LINK_COUNT                   2

/* Channel map codes */
#define Q6_CHANNEL_FL                   1
#define Q6_CHANNEL_FR                   2
#define Q6_CHANNEL_FC                   3

/* Backend format */
#define Q6_BACKEND_RATE                 48000
#define Q6_BACKEND_BITS                 16
#define Q6_BACKEND_CHANNELS             2

#define Q6_ASM_PORT                     (((Q6_SESSION) << 8) | (Q6_STREAM))
#define Q6_ASM_MAP_TOKEN                ((Q6_SESSION) << 8)

#include <pshpack1.h>
typedef struct _AFE_SET_PARAM_V2
{
    USHORT PortId;
    USHORT PayloadSize;
    ULONG PayloadAddrLsw;
    ULONG PayloadAddrMsw;
    ULONG MemMapHandle;
} AFE_SET_PARAM_V2;

typedef struct _AFE_SVC_SET_PARAM
{
    ULONG PayloadSize;
    ULONG PayloadAddrLsw;
    ULONG PayloadAddrMsw;
    ULONG MemMapHandle;
} AFE_SVC_SET_PARAM;

typedef struct _AFE_PARAM_DATA_V2
{
    ULONG ModuleId;
    ULONG ParamId;
    USHORT ParamSize;
    USHORT Reserved;
} AFE_PARAM_DATA_V2;

typedef struct _AFE_I2S_CFG
{
    ULONG MinorVersion;
    USHORT BitWidth;
    USHORT ChannelMode;
    USHORT MonoStereo;
    USHORT WsSource;
    ULONG SampleRate;
    USHORT DataFormat;
    USHORT Reserved;
} AFE_I2S_CFG;

typedef struct _AFE_CLOCK_SET
{
    ULONG MinorVersion;
    ULONG ClockId;
    ULONG FrequencyHz;
    USHORT Attributes;
    USHORT Root;
    ULONG Enable;
} AFE_CLOCK_SET;

typedef struct _AFE_DEVICE_START
{
    USHORT PortId;
    USHORT Reserved;
} AFE_DEVICE_START;

typedef struct _ADM_DEVICE_OPEN_V5
{
    USHORT Flags;
    USHORT ModeOfOperation;
    USHORT EndpointId1;
    USHORT EndpointId2;
    ULONG TopologyId;
    USHORT DevNumChannel;
    USHORT BitWidth;
    ULONG SampleRate;
    UCHAR DevChannelMapping[8];
} ADM_DEVICE_OPEN_V5;

typedef struct _ADM_DEVICE_OPEN_RSP
{
    ULONG Status;
    USHORT CoppId;
    USHORT Reserved;
} ADM_DEVICE_OPEN_RSP;

/* One session onto one copp; the copp list is padded to a 32 bit slot */
typedef struct _ADM_MATRIX_MAP
{
    ULONG MatrixId;
    ULONG NumSessions;
    USHORT SessionId;
    USHORT NumCopps;
    USHORT CoppId;
    USHORT Pad;
} ADM_MATRIX_MAP;

typedef struct _ASM_OPEN_WRITE_V3
{
    ULONG ModeFlags;
    USHORT SinkEndpointType;
    USHORT BitsPerSample;
    ULONG PostProcTopoId;
    ULONG DecFmtId;
} ASM_OPEN_WRITE_V3;

typedef struct _ASM_PCM_FMT_BLK
{
    ULONG FmtBlkSize;
    USHORT NumChannels;
    USHORT BitsPerSample;
    ULONG SampleRate;
    USHORT IsSigned;
    USHORT Reserved;
    UCHAR ChannelMapping[Q6_PCM_MAX_CHANNELS];
} ASM_PCM_FMT_BLK;

typedef struct _ASM_SHARED_MEM_MAP
{
    USHORT MemPoolId;
    USHORT NumRegions;
    ULONG PropertyFlag;
    ULONG ShmAddrLsw;
    ULONG ShmAddrMsw;
    ULONG MemSizeBytes;
} ASM_SHARED_MEM_MAP;

typedef struct _ASM_RUN_V2
{
    ULONG Flags;
    ULONG TimeLsw;
    ULONG TimeMsw;
} ASM_RUN_V2;

typedef struct _ASM_WRITE_V2
{
    ULONG BufAddrLsw;
    ULONG BufAddrMsw;
    ULONG MemMapHandle;
    ULONG BufSize;
    ULONG SeqId;
    ULONG TimestampLsw;
    ULONG TimestampMsw;
    ULONG Flags;
} ASM_WRITE_V2;
#include <poppack.h>

C_ASSERT(sizeof(ADM_MATRIX_MAP) == 16);
C_ASSERT(sizeof(AFE_CLOCK_SET) == 20);
C_ASSERT(sizeof(AFE_I2S_CFG) == 20);

/* One backend port, the I2S data line the board wires to it and its users */
typedef struct _Q6_AFE_LINK
{
    USHORT PortId;
    USHORT Line;
    ULONG Users;
} Q6_AFE_LINK, *PQ6_AFE_LINK;

typedef struct _Q6_GRAPH
{
    APR_HANDLE Afe;
    APR_HANDLE Adm;
    APR_HANDLE Asm;

    KEVENT Cmd;
    KMUTEX Serial;
    ULONG CmdOpcode;
    ULONG RspOpcode;
    volatile LONG CmdStatus;

    Q6_AFE_LINK Links[Q6_LINK_COUNT];
    ULONG ClockUsers;
    ULONG AmpUsers;

    USHORT CoppId;
    BOOLEAN LinkHeld;
    BOOLEAN StreamOpen;
    BOOLEAN AmpOn;
    ULONG MemMapHandle;

    PHYSICAL_ADDRESS BufferPhys;
    ULONG BufferBytes;
    ULONG PeriodBytes;
    ULONG Periods;
    ULONG Rate;
    ULONG Channels;
    ULONG Bits;

    KSPIN_LOCK NotifyLock;
    PQC_Q6_PERIOD_DONE PeriodDone;
    PVOID PeriodContext;

    volatile LONG Running;
    ULONG SeqId;
    volatile LONGLONG BytesConsumed;
} Q6_GRAPH, *PQ6_GRAPH;

static Q6_GRAPH Q6Graph;

/* An ADSP read of a host address with no SMMU route for its stream resets the phone */
static BOOLEAN Q6GraphHostBuffers;

/* Above bit 31 of a buffer address the ADSP reads the low bits of the SMMU stream to use */
static ULONGLONG Q6GraphAddressTag;

VOID
NTAPI
QcQ6GraphAllowHostBuffers(
    _In_ BOOLEAN Allow,
    _In_ ULONGLONG AddressTag)
{
    Q6GraphHostBuffers = Allow;
    Q6GraphAddressTag = AddressTag;
}

static
VOID
Q6GraphSubmitPeriod(
    _In_ PQ6_GRAPH Graph,
    _In_ ULONG PeriodIndex)
{
    ASM_WRITE_V2 Write;
    ULONGLONG Addr;

    Addr = ((ULONGLONG)Graph->BufferPhys.QuadPart + (ULONGLONG)PeriodIndex * Graph->PeriodBytes) | Q6GraphAddressTag;

    RtlZeroMemory(&Write, sizeof(Write));
    Write.BufAddrLsw = (ULONG)(Addr & 0xFFFFFFFF);
    Write.BufAddrMsw = (ULONG)(Addr >> 32);
    Write.MemMapHandle = Graph->MemMapHandle;
    Write.BufSize = Graph->PeriodBytes;
    Write.SeqId = ++Graph->SeqId;

    QcAprSendTo(Graph->Asm, APR_SVC_ASM, Q6_ASM_PORT, Q6_ASM_PORT,
                ASM_DATA_CMD_WRITE_V2, PeriodIndex, &Write, sizeof(Write));
}

/* One receive for every graph service; setup is serial so one event suffices */
static
VOID
NTAPI
Q6GraphReceive(
    _In_ PVOID Context,
    _In_reads_bytes_(Length) const VOID *Packet,
    _In_ ULONG Length)
{
    PQ6_GRAPH Graph = Context;
    const APR_HEADER *Hdr = Packet;
    const UCHAR *Payload = (const UCHAR *)Packet + sizeof(APR_HEADER);
    ULONG PayloadLen;

    if (Length < sizeof(APR_HEADER))
        return;
    PayloadLen = Length - sizeof(APR_HEADER);

    if (Hdr->Opcode == ASM_DATA_EVENT_WRITE_DONE_V2)
    {
        KIRQL OldIrql;
        BOOLEAN Running;
        ULONG Period = 0;

        /* The lock keeps the stop path from freeing the callback's context under us */
        KeAcquireSpinLock(&Graph->NotifyLock, &OldIrql);
        Running = InterlockedCompareExchange(&Graph->Running, 0, 0) && Graph->Periods;
        if (Running)
        {
            Period = Hdr->Token % Graph->Periods;
            Graph->BytesConsumed += Graph->PeriodBytes;
            if (Graph->PeriodDone)
                Graph->PeriodDone(Graph->PeriodContext, Period);
        }
        KeReleaseSpinLock(&Graph->NotifyLock, OldIrql);

        /* Resubmit this buffer so the cyclic pcm keeps flowing */
        if (Running)
            Q6GraphSubmitPeriod(Graph, Period);
        return;
    }

    if (Hdr->Opcode == APR_BASIC_RSP_RESULT)
    {
        if (PayloadLen >= 2 * sizeof(ULONG))
        {
            ULONG RspFor = ((const ULONG *)Payload)[0];
            ULONG Status = ((const ULONG *)Payload)[1];

            if (RspFor == Graph->CmdOpcode)
            {
                InterlockedExchange(&Graph->CmdStatus, (LONG)Status);
                KeSetEvent(&Graph->Cmd, IO_NO_INCREMENT, FALSE);
            }
            else if (Status != 0)
            {
                QC_LOG("q6: dsp rejected opcode 0x%08lx with 0x%lx", RspFor, Status);
            }
        }
        return;
    }

    if (Graph->RspOpcode && Hdr->Opcode == Graph->RspOpcode)
    {
        if (Hdr->Opcode == ADM_CMDRSP_DEVICE_OPEN_V5 && PayloadLen >= sizeof(ADM_DEVICE_OPEN_RSP))
        {
            const ADM_DEVICE_OPEN_RSP *Rsp = (const ADM_DEVICE_OPEN_RSP *)Payload;
            Graph->CoppId = Rsp->CoppId;
            InterlockedExchange(&Graph->CmdStatus, (LONG)Rsp->Status);
        }
        else if (Hdr->Opcode == ASM_CMDRSP_SHARED_MEM_MAP_REGIONS && PayloadLen >= sizeof(ULONG))
        {
            Graph->MemMapHandle = ((const ULONG *)Payload)[0];
            InterlockedExchange(&Graph->CmdStatus, 0);
        }
        else
        {
            InterlockedExchange(&Graph->CmdStatus, 0);
        }

        KeSetEvent(&Graph->Cmd, IO_NO_INCREMENT, FALSE);
    }
}

/* Sends one command and waits for its ack or named response */
static
NTSTATUS
Q6GraphCall(
    _In_ PQ6_GRAPH Graph,
    _In_ APR_HANDLE Handle,
    _In_ ULONG DestService,
    _In_ ULONG DestPort,
    _In_ ULONG SrcPort,
    _In_ ULONG Opcode,
    _In_ ULONG RspOpcode,
    _In_ ULONG Token,
    _In_reads_bytes_opt_(Length) const VOID *Payload,
    _In_ ULONG Length)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    Graph->CmdOpcode = Opcode;
    Graph->RspOpcode = RspOpcode;
    InterlockedExchange(&Graph->CmdStatus, -1);
    KeClearEvent(&Graph->Cmd);

    Status = QcAprSendTo(Handle, DestService, DestPort, SrcPort, Opcode, Token, Payload, Length);
    if (!NT_SUCCESS(Status))
        return Status;

    Timeout.QuadPart = -10LL * 1000 * Q6_CMD_TIMEOUT_MS;
    Status = KeWaitForSingleObject(&Graph->Cmd, Executive, KernelMode, FALSE, &Timeout);
    Graph->CmdOpcode = 0;
    Graph->RspOpcode = 0;
    if (Status != STATUS_SUCCESS)
    {
        QC_LOG("q6: opcode 0x%08lx timed out", Opcode);
        return STATUS_IO_TIMEOUT;
    }

    if (Graph->CmdStatus != 0)
    {
        QC_LOG("q6: opcode 0x%08lx failed with dsp status 0x%lx", Opcode, (ULONG)Graph->CmdStatus);
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

static
VOID
Q6FillChannelMap(
    _Out_writes_(Q6_PCM_MAX_CHANNELS) PUCHAR Map,
    _In_ ULONG Channels)
{
    RtlZeroMemory(Map, Q6_PCM_MAX_CHANNELS);

    /* A mono stream is the center channel, the matrix spreads it over both slots */
    if (Channels == 1)
    {
        Map[0] = Q6_CHANNEL_FC;
        return;
    }

    Map[0] = Q6_CHANNEL_FL;
    Map[1] = Q6_CHANNEL_FR;
}

/* Enables or disables the quaternary MI2S bit clock */
static
NTSTATUS
Q6AfeClock(
    _In_ PQ6_GRAPH Graph,
    _In_ BOOLEAN Enable)
{
    struct
    {
        AFE_SVC_SET_PARAM Param;
        AFE_PARAM_DATA_V2 Data;
        AFE_CLOCK_SET Clock;
    } SetParam;

    RtlZeroMemory(&SetParam, sizeof(SetParam));
    SetParam.Param.PayloadSize = sizeof(SetParam.Data) + sizeof(SetParam.Clock);
    SetParam.Data.ModuleId = AFE_MODULE_CLOCK_SET;
    SetParam.Data.ParamId = AFE_PARAM_ID_CLOCK_SET;
    SetParam.Data.ParamSize = sizeof(SetParam.Clock);
    SetParam.Clock.MinorVersion = Q6_CLOCK_SET_VERSION;
    SetParam.Clock.ClockId = Q6_CLOCK_QUAT_MI2S_IBIT;
    SetParam.Clock.Attributes = Q6_CLOCK_NOT_COUPLED;
    SetParam.Clock.Root = Q6_CLOCK_ROOT_DEFAULT;
    if (Enable)
    {
        SetParam.Clock.FrequencyHz = Q6_BACKEND_RATE * Q6_BACKEND_BITS * Q6_BACKEND_CHANNELS;
        SetParam.Clock.Enable = 1;
    }

    return Q6GraphCall(Graph, Graph->Afe, APR_SVC_AFE, 0, 0,
                       AFE_SVC_CMD_SET_PARAM, 0, AFE_PORT_ID_QUAT_MI2S_RX,
                       &SetParam, sizeof(SetParam));
}

static
PQ6_AFE_LINK
Q6FindLink(
    _In_ PQ6_GRAPH Graph,
    _In_ USHORT PortId)
{
    ULONG i;

    for (i = 0; i < Q6_LINK_COUNT; i++)
    {
        if (Graph->Links[i].PortId == PortId)
            return &Graph->Links[i];
    }
    return NULL;
}

/* Rx and Tx share the interface bit clock, the first user turns it on */
static
NTSTATUS
Q6AfeClockGet(
    _In_ PQ6_GRAPH Graph)
{
    NTSTATUS Status;

    if (Graph->ClockUsers++)
        return STATUS_SUCCESS;

    Status = Q6AfeClock(Graph, TRUE);
    if (!NT_SUCCESS(Status))
    {
        QC_LOG("afe quaternary mi2s bit clock failed 0x%08lx", Status);
        Graph->ClockUsers = 0;
    }
    return Status;
}

static
VOID
Q6AfeClockPut(
    _In_ PQ6_GRAPH Graph)
{
    if (!Graph->ClockUsers)
        return;

    Graph->ClockUsers -= 1;
    if (!Graph->ClockUsers)
        Q6AfeClock(Graph, FALSE);
}

static
VOID
Q6AfeDeviceStop(
    _In_ PQ6_GRAPH Graph,
    _In_ USHORT PortId)
{
    AFE_DEVICE_START Stop;

    RtlZeroMemory(&Stop, sizeof(Stop));
    Stop.PortId = PortId;
    Q6GraphCall(Graph, Graph->Afe, APR_SVC_AFE, 0, 0,
                AFE_PORT_CMD_DEVICE_STOP, 0, PortId, &Stop, sizeof(Stop));
}

/* Clocks the interface, configures the I2S line of the port and starts it once */
static
NTSTATUS
Q6AfeLinkGet(
    _In_ PQ6_GRAPH Graph,
    _In_ USHORT PortId)
{
    struct
    {
        AFE_SET_PARAM_V2 Param;
        AFE_PARAM_DATA_V2 Data;
        AFE_I2S_CFG Cfg;
    } SetParam;
    AFE_DEVICE_START Start;
    PQ6_AFE_LINK Link;
    NTSTATUS Status;

    Link = Q6FindLink(Graph, PortId);
    if (!Link || !Link->Line)
    {
        QC_LOG("afe port 0x%x has no board line", PortId);
        return STATUS_NOT_SUPPORTED;
    }

    if (Link->Users)
    {
        Link->Users++;
        return STATUS_SUCCESS;
    }

    Status = Q6AfeClockGet(Graph);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(&SetParam, sizeof(SetParam));
    SetParam.Param.PortId = PortId;
    SetParam.Param.PayloadSize = sizeof(SetParam.Data) + sizeof(SetParam.Cfg);
    SetParam.Data.ModuleId = AFE_MODULE_AUDIO_DEV_INTERFACE;
    SetParam.Data.ParamId = AFE_PARAM_ID_I2S_CONFIG;
    SetParam.Data.ParamSize = sizeof(SetParam.Cfg);
    SetParam.Cfg.MinorVersion = Q6_I2S_CONFIG_VERSION;
    SetParam.Cfg.BitWidth = Q6_BACKEND_BITS;
    SetParam.Cfg.ChannelMode = Link->Line;
    SetParam.Cfg.MonoStereo = Q6_I2S_STEREO;
    SetParam.Cfg.WsSource = Q6_I2S_WS_INTERNAL;
    SetParam.Cfg.SampleRate = Q6_BACKEND_RATE;

    Status = Q6GraphCall(Graph, Graph->Afe, APR_SVC_AFE, 0, 0,
                         AFE_PORT_CMD_SET_PARAM_V2, 0, PortId,
                         &SetParam, sizeof(SetParam));
    if (!NT_SUCCESS(Status))
    {
        QC_LOG("afe port 0x%x i2s set-param failed 0x%08lx", PortId, Status);
        Q6AfeClockPut(Graph);
        return Status;
    }

    RtlZeroMemory(&Start, sizeof(Start));
    Start.PortId = PortId;
    Status = Q6GraphCall(Graph, Graph->Afe, APR_SVC_AFE, 0, 0,
                         AFE_PORT_CMD_DEVICE_START, 0, PortId,
                         &Start, sizeof(Start));
    if (!NT_SUCCESS(Status))
    {
        QC_LOG("afe port 0x%x start failed 0x%08lx", PortId, Status);
        Q6AfeClockPut(Graph);
        return Status;
    }

    Link->Users = 1;
    QC_LOG("afe port 0x%x running on line 0x%x", PortId, Link->Line);
    return STATUS_SUCCESS;
}

static
VOID
Q6AfeLinkPut(
    _In_ PQ6_GRAPH Graph,
    _In_ USHORT PortId)
{
    PQ6_AFE_LINK Link = Q6FindLink(Graph, PortId);

    if (!Link || !Link->Users)
        return;

    Link->Users -= 1;
    if (Link->Users)
        return;

    Q6AfeDeviceStop(Graph, PortId);
    Q6AfeClockPut(Graph);
    QC_LOG("afe port 0x%x stopped", PortId);
}

/* The amp hangs off the speaker port and goes on once that port clocks */
static
VOID
Q6AmpGet(
    _In_ PQ6_GRAPH Graph)
{
    NTSTATUS Status;

    if (Graph->AmpUsers++)
        return;

    /* A failed start leaves the amp off; the stop at the last user is then a no-op */
    Status = QcAmpStart(Q6_BACKEND_RATE, Q6_BACKEND_BITS, Q6_BACKEND_CHANNELS);
    if (!NT_SUCCESS(Status))
        QC_LOG("speaker amp start failed 0x%08lx", Status);
}

static
VOID
Q6AmpPut(
    _In_ PQ6_GRAPH Graph)
{
    if (!Graph->AmpUsers)
        return;

    Graph->AmpUsers -= 1;
    if (!Graph->AmpUsers)
        QcAmpStop();
}

static
NTSTATUS
Q6AdmRoute(
    _In_ PQ6_GRAPH Graph)
{
    ADM_DEVICE_OPEN_V5 Open;
    ADM_MATRIX_MAP Map;
    NTSTATUS Status;

    RtlZeroMemory(&Open, sizeof(Open));
    Open.Flags = ADM_LEGACY_DEVICE_SESSION;
    Open.ModeOfOperation = ADM_PATH_PLAYBACK;
    Open.EndpointId1 = AFE_PORT_ID_QUAT_MI2S_RX;
    Open.TopologyId = ADM_NULL_COPP_TOPOLOGY;
    Open.DevNumChannel = Q6_BACKEND_CHANNELS;
    Open.BitWidth = Q6_BACKEND_BITS;
    Open.SampleRate = Q6_BACKEND_RATE;
    Q6FillChannelMap(Open.DevChannelMapping, Q6_BACKEND_CHANNELS);

    Graph->CoppId = 0;
    Status = Q6GraphCall(Graph, Graph->Adm, APR_SVC_ADM,
                         AFE_PORT_ID_QUAT_MI2S_RX, AFE_PORT_ID_QUAT_MI2S_RX,
                         ADM_CMD_DEVICE_OPEN_V5, ADM_CMDRSP_DEVICE_OPEN_V5,
                         (ULONG)AFE_PORT_ID_QUAT_MI2S_RX << 16,
                         &Open, sizeof(Open));
    if (!NT_SUCCESS(Status))
    {
        QC_LOG("adm device open failed 0x%08lx", Status);
        return Status;
    }

    RtlZeroMemory(&Map, sizeof(Map));
    Map.MatrixId = ADM_MATRIX_ID_AUDIO_RX;
    Map.NumSessions = 1;
    Map.SessionId = Q6_SESSION;
    Map.NumCopps = 1;
    Map.CoppId = Graph->CoppId;

    Status = Q6GraphCall(Graph, Graph->Adm, APR_SVC_ADM, 0, 0,
                         ADM_CMD_MATRIX_MAP_ROUTINGS_V5, 0, 0, &Map, sizeof(Map));
    if (!NT_SUCCESS(Status))
        QC_LOG("adm matrix map failed 0x%08lx", Status);
    else
        QC_LOG("adm copp %u routed to session %u", Graph->CoppId, Q6_SESSION);
    return Status;
}

static
NTSTATUS
Q6AsmMapBuffer(
    _In_ PQ6_GRAPH Graph)
{
    ASM_SHARED_MEM_MAP MemMap;
    NTSTATUS Status;

    RtlZeroMemory(&MemMap, sizeof(MemMap));
    MemMap.MemPoolId = ASM_MEM_POOL_SHMEM8_4K;
    MemMap.NumRegions = 1;
    MemMap.ShmAddrLsw = (ULONG)(Graph->BufferPhys.QuadPart & 0xFFFFFFFF);
    MemMap.ShmAddrMsw = (ULONG)(((ULONGLONG)Graph->BufferPhys.QuadPart | Q6GraphAddressTag) >> 32);
    MemMap.MemSizeBytes = (Graph->BufferBytes + 0xFFF) & ~0xFFFUL;

    /* The map is a service command: port 0, session and direction in the token */
    Graph->MemMapHandle = 0;
    Status = Q6GraphCall(Graph, Graph->Asm, APR_SVC_ASM, 0, 0,
                         ASM_CMD_SHARED_MEM_MAP_REGIONS, ASM_CMDRSP_SHARED_MEM_MAP_REGIONS,
                         Q6_ASM_MAP_TOKEN, &MemMap, sizeof(MemMap));
    if (!NT_SUCCESS(Status))
        QC_LOG("asm shared mem map failed 0x%08lx", Status);
    else
        QC_LOG("asm mapped %lu bytes at 0x%I64x, handle 0x%lx",
               MemMap.MemSizeBytes, Graph->BufferPhys.QuadPart, Graph->MemMapHandle);
    return Status;
}

static
VOID
Q6AsmUnmapBuffer(
    _In_ PQ6_GRAPH Graph)
{
    ULONG Handle = Graph->MemMapHandle;

    if (!Handle)
        return;

    Q6GraphCall(Graph, Graph->Asm, APR_SVC_ASM, 0, 0,
                ASM_CMD_SHARED_MEM_UNMAP_REGIONS, 0, Q6_ASM_MAP_TOKEN, &Handle, sizeof(Handle));
    Graph->MemMapHandle = 0;
}

static
NTSTATUS
Q6AsmOpen(
    _In_ PQ6_GRAPH Graph)
{
    ASM_OPEN_WRITE_V3 Open;
    NTSTATUS Status;

    RtlZeroMemory(&Open, sizeof(Open));
    Open.SinkEndpointType = ASM_SINK_DEVICE_MATRIX;
    Open.BitsPerSample = (USHORT)Graph->Bits;
    Open.PostProcTopoId = ASM_NULL_POPP_TOPOLOGY;
    Open.DecFmtId = ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2;
    Status = Q6GraphCall(Graph, Graph->Asm, APR_SVC_ASM, Q6_ASM_PORT, Q6_ASM_PORT,
                         ASM_STREAM_CMD_OPEN_WRITE_V3, 0, Q6_SESSION, &Open, sizeof(Open));
    if (!NT_SUCCESS(Status))
    {
        QC_LOG("asm open write failed 0x%08lx", Status);
        return Status;
    }

    Graph->StreamOpen = TRUE;
    return STATUS_SUCCESS;
}

static
NTSTATUS
Q6AsmRun(
    _In_ PQ6_GRAPH Graph)
{
    ASM_PCM_FMT_BLK Fmt;
    ASM_RUN_V2 Run;
    NTSTATUS Status;

    RtlZeroMemory(&Fmt, sizeof(Fmt));
    Fmt.FmtBlkSize = sizeof(Fmt) - sizeof(ULONG);
    Fmt.NumChannels = (USHORT)Graph->Channels;
    Fmt.BitsPerSample = (USHORT)Graph->Bits;
    Fmt.SampleRate = Graph->Rate;
    Fmt.IsSigned = 1;
    Q6FillChannelMap(Fmt.ChannelMapping, Graph->Channels);
    Status = Q6GraphCall(Graph, Graph->Asm, APR_SVC_ASM, Q6_ASM_PORT, Q6_ASM_PORT,
                         ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2, 0, Q6_SESSION, &Fmt, sizeof(Fmt));
    if (!NT_SUCCESS(Status))
    {
        QC_LOG("asm media format failed 0x%08lx", Status);
        return Status;
    }

    RtlZeroMemory(&Run, sizeof(Run));
    Status = Q6GraphCall(Graph, Graph->Asm, APR_SVC_ASM, Q6_ASM_PORT, Q6_ASM_PORT,
                         ASM_SESSION_CMD_RUN_V2, 0, Q6_SESSION, &Run, sizeof(Run));
    if (!NT_SUCCESS(Status))
        QC_LOG("asm run failed 0x%08lx", Status);
    return Status;
}

/* Tears down whatever part of the graph is up, in reverse order */
static
VOID
Q6GraphTeardown(
    _In_ PQ6_GRAPH Graph)
{
    if (Graph->AmpOn)
    {
        Q6AmpPut(Graph);
        Graph->AmpOn = FALSE;
    }

    if (Graph->StreamOpen)
    {
        Q6GraphCall(Graph, Graph->Asm, APR_SVC_ASM, Q6_ASM_PORT, Q6_ASM_PORT,
                    ASM_SESSION_CMD_PAUSE, 0, Q6_SESSION, NULL, 0);
        Q6GraphCall(Graph, Graph->Asm, APR_SVC_ASM, Q6_ASM_PORT, Q6_ASM_PORT,
                    ASM_STREAM_CMD_CLOSE, 0, Q6_SESSION, NULL, 0);
        Graph->StreamOpen = FALSE;
    }

    Q6AsmUnmapBuffer(Graph);

    if (Graph->CoppId)
    {
        Q6GraphCall(Graph, Graph->Adm, APR_SVC_ADM, Graph->CoppId, AFE_PORT_ID_QUAT_MI2S_RX,
                    ADM_CMD_DEVICE_CLOSE_V5, 0, (ULONG)AFE_PORT_ID_QUAT_MI2S_RX << 16, NULL, 0);
        Graph->CoppId = 0;
    }

    if (Graph->LinkHeld)
    {
        Q6AfeLinkPut(Graph, AFE_PORT_ID_QUAT_MI2S_RX);
        Graph->LinkHeld = FALSE;
    }
}

VOID
NTAPI
QcQ6GraphInit(
    _In_ ULONG RxLine,
    _In_ ULONG TxLine)
{
    PQ6_GRAPH Graph = &Q6Graph;

    RtlZeroMemory(Graph, sizeof(*Graph));
    KeInitializeEvent(&Graph->Cmd, NotificationEvent, FALSE);
    KeInitializeMutex(&Graph->Serial, 0);
    KeInitializeSpinLock(&Graph->NotifyLock);

    Graph->Links[0].PortId = AFE_PORT_ID_QUAT_MI2S_RX;
    Graph->Links[0].Line = (USHORT)RxLine;
    Graph->Links[1].PortId = AFE_PORT_ID_QUAT_MI2S_TX;
    Graph->Links[1].Line = (USHORT)TxLine;

    Graph->Afe = QcAprRegister(APR_DOMAIN_ADSP, APR_SVC_AFE, Q6GraphReceive, Graph);
    Graph->Adm = QcAprRegister(APR_DOMAIN_ADSP, APR_SVC_ADM, Q6GraphReceive, Graph);
    Graph->Asm = QcAprRegister(APR_DOMAIN_ADSP, APR_SVC_ASM, Q6GraphReceive, Graph);
}

NTSTATUS
NTAPI
QcQ6PlaybackStart(
    _In_ ULONGLONG BufferPhys,
    _In_ ULONG BufferBytes,
    _In_ ULONG PeriodBytes,
    _In_ ULONG Rate,
    _In_ ULONG Channels,
    _In_ ULONG Bits,
    _In_opt_ PQC_Q6_PERIOD_DONE PeriodDone,
    _In_opt_ PVOID Context)
{
    PQ6_GRAPH Graph = &Q6Graph;
    NTSTATUS Status;
    ULONG i;

    PAGED_CODE();

    if (!Graph->Afe || !Graph->Adm || !Graph->Asm || !QcAprIsConnected())
        return STATUS_DEVICE_NOT_READY;
    if (!PeriodBytes || !BufferBytes || PeriodBytes > BufferBytes)
        return STATUS_INVALID_PARAMETER;
    if (!Channels || Channels > 2 || Bits != 16)
        return STATUS_NOT_SUPPORTED;

    if (!Q6GraphHostBuffers)
    {
        QC_LOG("q6: playback refused, the adsp has no smmu route to host buffers yet");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    KeWaitForSingleObject(&Graph->Serial, Executive, KernelMode, FALSE, NULL);
    if (InterlockedCompareExchange(&Graph->Running, 0, 0))
    {
        KeReleaseMutex(&Graph->Serial, FALSE);
        return STATUS_DEVICE_BUSY;
    }

    Graph->BufferPhys.QuadPart = BufferPhys;
    Graph->BufferBytes = BufferBytes;
    Graph->PeriodBytes = PeriodBytes;
    Graph->Periods = BufferBytes / PeriodBytes;
    Graph->Rate = Rate;
    Graph->Channels = Channels;
    Graph->Bits = Bits;
    Graph->PeriodDone = PeriodDone;
    Graph->PeriodContext = Context;
    Graph->SeqId = 0;
    Graph->BytesConsumed = 0;
    Graph->MemMapHandle = 0;
    Graph->CoppId = 0;

    /* Pins and the amp's static setup must be in place before the link clocks */
    Status = QcAmpPrepare();
    if (!NT_SUCCESS(Status))
        QC_LOG("speaker amp not ready 0x%08lx, the dsp graph still runs", Status);

    Status = Q6AfeLinkGet(Graph, AFE_PORT_ID_QUAT_MI2S_RX);
    if (NT_SUCCESS(Status))
    {
        Graph->LinkHeld = TRUE;
        Status = Q6AsmMapBuffer(Graph);
    }
    if (NT_SUCCESS(Status))
        Status = Q6AsmOpen(Graph);
    if (NT_SUCCESS(Status))
        Status = Q6AdmRoute(Graph);
    if (NT_SUCCESS(Status))
        Status = Q6AsmRun(Graph);
    if (!NT_SUCCESS(Status))
    {
        Q6GraphTeardown(Graph);
        KeReleaseMutex(&Graph->Serial, FALSE);
        return Status;
    }

    InterlockedExchange(&Graph->Running, TRUE);

    /* Prime the dsp with every period; each is resubmitted on its write-done */
    for (i = 0; i < Graph->Periods; i++)
        Q6GraphSubmitPeriod(Graph, i);

    Q6AmpGet(Graph);
    Graph->AmpOn = TRUE;

    QC_LOG("q6 playback started: %lu hz, %lu ch, %lu bit, %lu periods of %lu bytes",
           Rate, Channels, Bits, Graph->Periods, PeriodBytes);
    KeReleaseMutex(&Graph->Serial, FALSE);
    return STATUS_SUCCESS;
}

VOID
NTAPI
QcQ6PlaybackStop(VOID)
{
    PQ6_GRAPH Graph = &Q6Graph;
    KIRQL OldIrql;
    LONG WasRunning;

    PAGED_CODE();

    KeWaitForSingleObject(&Graph->Serial, Executive, KernelMode, FALSE, NULL);

    KeAcquireSpinLock(&Graph->NotifyLock, &OldIrql);
    WasRunning = InterlockedExchange(&Graph->Running, FALSE);
    Graph->PeriodDone = NULL;
    Graph->PeriodContext = NULL;
    KeReleaseSpinLock(&Graph->NotifyLock, OldIrql);

    if (WasRunning)
    {
        Q6GraphTeardown(Graph);
        QC_LOG("q6 playback stopped after %I64u bytes", (ULONGLONG)Graph->BytesConsumed);
    }
    KeReleaseMutex(&Graph->Serial, FALSE);
}

ULONGLONG
NTAPI
QcQ6PlaybackPosition(VOID)
{
    return (ULONGLONG)Q6Graph.BytesConsumed;
}

NTSTATUS
NTAPI
QcQ6GraphLinkOpen(
    _In_ USHORT PortId)
{
    PQ6_GRAPH Graph = &Q6Graph;
    NTSTATUS Status;

    PAGED_CODE();

    if (!Graph->Afe || !QcAprIsConnected())
        return STATUS_DEVICE_NOT_READY;

    KeWaitForSingleObject(&Graph->Serial, Executive, KernelMode, FALSE, NULL);

    if (PortId == AFE_PORT_ID_QUAT_MI2S_RX)
    {
        Status = QcAmpPrepare();
        if (!NT_SUCCESS(Status))
            QC_LOG("speaker amp not ready 0x%08lx, the port still runs", Status);
    }

    Status = Q6AfeLinkGet(Graph, PortId);
    if (NT_SUCCESS(Status) && PortId == AFE_PORT_ID_QUAT_MI2S_RX)
        Q6AmpGet(Graph);

    KeReleaseMutex(&Graph->Serial, FALSE);
    return Status;
}

VOID
NTAPI
QcQ6GraphLinkClose(
    _In_ USHORT PortId)
{
    PQ6_GRAPH Graph = &Q6Graph;
    PQ6_AFE_LINK Link;

    PAGED_CODE();

    KeWaitForSingleObject(&Graph->Serial, Executive, KernelMode, FALSE, NULL);

    Link = Q6FindLink(Graph, PortId);
    if (Link && Link->Users)
    {
        if (PortId == AFE_PORT_ID_QUAT_MI2S_RX)
            Q6AmpPut(Graph);
        Q6AfeLinkPut(Graph, PortId);
    }

    KeReleaseMutex(&Graph->Serial, FALSE);
}
