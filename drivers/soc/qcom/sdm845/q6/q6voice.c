/*
 * PROJECT:     ReactOS Qualcomm audio support (SDM845)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Q6 circuit switched voice call path (MVM, CVS, CVP) over APR
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The apps side joins the modem's voice session and supplies a vocproc bound to
 * the Rx and Tx AFE ports. Nothing here hands the DSP a host address, since its
 * SMMU mapping does not exist yet, so calibration is left out.
 */

#include <ntddk.h>
#include <qcom/aprif.h>

#define QCDBG_COMPONENT "qcomq6"
#include <qcom/qcomdbg.h>

#include "q6graph.h"
#include "q6voice.h"

/* Voice services on the ADSP */
#define Q6V_SVC_MVM                 0x09
#define Q6V_SVC_CVS                 0x0A
#define Q6V_SVC_CVP                 0x0B

/* Generic */
#define Q6V_ACK                     0x000110E8UL
#define Q6V_DESTROY_SESSION         0x0001003CUL
#define Q6V_SET_PARAM_V2            0x0001133DUL
#define Q6V_VERSION_QUERY           0x00011378UL
#define Q6V_VERSION_REPLY           0x00011379UL

/* MVM */
#define Q6V_MVM_JOIN                0x000110FFUL
#define Q6V_MVM_DUAL_CONTROL        0x00011327UL
#define Q6V_MVM_ATTACH_CVP          0x0001123EUL
#define Q6V_MVM_DETACH_CVP          0x0001123FUL
#define Q6V_MVM_SET_TTY             0x00011196UL
#define Q6V_MVM_START               0x00011190UL
#define Q6V_MVM_STOP                0x00011192UL

/* CVS */
#define Q6V_CVS_JOIN                0x00011140UL
#define Q6V_MUTE                    0x0001138BUL

/* CVP */
#define Q6V_CVP_CREATE_V2           0x000112BFUL
#define Q6V_CVP_CREATE_V3           0x00013169UL
#define Q6V_CVP_DEV_CHANNELS        0x00013199UL
#define Q6V_CVP_COMMIT              0x00013198UL
#define Q6V_CVP_ENABLE              0x000100C6UL
#define Q6V_VOLUME_STEP             0x000112C2UL

/* In band parameters */
#define Q6V_MOD_CVD                 0x0001316EUL
#define Q6V_PID_TX_CHANNELS         0x0001328EUL
#define Q6V_PID_RX_CHANNELS         0x0001328FUL
#define Q6V_PID_EC_CHANNELS         0x00013290UL
#define Q6V_PID_TX_ENDPOINT         0x00013253UL
#define Q6V_PID_RX_ENDPOINT         0x00013254UL
#define Q6V_MOD_MFC                 0x00010912UL
#define Q6V_PID_MFC_FORMAT          0x00010913UL
#define Q6V_PID_MIXER_COEFF         0x00010342UL

/* Vocproc setup values */
#define Q6V_DIR_RX_TX               2
#define Q6V_TOPO_RX                 0x00010F77UL
#define Q6V_TOPO_TX                 0x00010F89UL
#define Q6V_PROFILE_NONE            0x0001135EUL
#define Q6V_EC_INTERNAL             0x00010F7CUL
#define Q6V_PORT_NONE               0xFFFF
#define Q6V_DIRECTION_TX            0
#define Q6V_DIRECTION_RX            1
#define Q6V_UNITY_Q14               (1 << 14)
#define Q6V_MAX_RAMP_MS             5000
#define Q6V_VOLUME_RAMP_MS          20

/* The CVP api version that takes channel info params instead of dev channels */
#define Q6V_CVP_API_CHANNEL_INFO    2
#define Q6V_CORE_ID_CVP             0x0B

/* SET_PARAM tokens the CVP answers to */
#define Q6V_TOKEN_GENERIC           0
#define Q6V_TOKEN_MEDIA_FORMAT      2

/* Session index the apps side uses as its APR port for the modem call */
#define Q6V_APPS_PORT               0
#define Q6V_TIMEOUT_MS              300
#define Q6V_NAME_BYTES              20
#define Q6V_VERSION_BYTES           31
#define Q6V_MAX_CHANNELS            8
#define Q6V_PARAM_MAX               24

/* Backend format, the one q6graph clocks the MI2S ports at */
#define Q6V_RATE                    48000
#define Q6V_BITS                    16
#define Q6V_CHANNELS                2

/* CVD version strings known to change the command set, in the order they appeared */
typedef enum _Q6V_CVD_LEVEL
{
    Q6vCvdUnknown,
    Q6vCvd00,
    Q6vCvd21,
    Q6vCvd22,
    Q6vCvd23
} Q6V_CVD_LEVEL;

#include <pshpack1.h>
typedef struct _Q6V_NAME
{
    CHAR Text[Q6V_NAME_BYTES];
} Q6V_NAME;

typedef struct _Q6V_CVP_SETUP
{
    USHORT Direction;
    USHORT TxPort;
    ULONG TxTopology;
    USHORT RxPort;
    ULONG RxTopology;
    ULONG Profile;
    ULONG Mode;
    USHORT EcRefPort;
    CHAR Name[Q6V_NAME_BYTES];
} Q6V_CVP_SETUP;

typedef struct _Q6V_STEP
{
    USHORT Direction;
    ULONG Value;
    USHORT RampMs;
} Q6V_STEP;

typedef struct _Q6V_MUTE_ARGS
{
    USHORT Direction;
    USHORT Mute;
    USHORT RampMs;
} Q6V_MUTE_ARGS;

typedef struct _Q6V_DEV_CHANNELS
{
    USHORT TxChannels;
    USHORT RxChannels;
} Q6V_DEV_CHANNELS;

/* In band SET_PARAM_V2: no handle, no address, the param follows */
typedef struct _Q6V_PARAM
{
    ULONG MemHandle;
    ULONGLONG MemAddress;
    ULONG MemSize;
    ULONG ModuleId;
    ULONG ParamId;
    USHORT ParamSize;
    USHORT Reserved;
    UCHAR Data[Q6V_PARAM_MAX];
} Q6V_PARAM;

typedef struct _Q6V_CHANNEL_INFO
{
    ULONG Channels;
    ULONG Bits;
    UCHAR Map[Q6V_MAX_CHANNELS];
} Q6V_CHANNEL_INFO;

typedef struct _Q6V_ENDPOINT
{
    ULONG PortId;
    USHORT Channels;
    USHORT Bits;
    ULONG Rate;
    UCHAR Map[Q6V_MAX_CHANNELS];
} Q6V_ENDPOINT;

typedef struct _Q6V_MIXER
{
    ULONG Index;
    USHORT Outputs;
    USHORT Inputs;
    USHORT OutMap[2];
    USHORT InMap[1];
    USHORT Coeff[2][1];
    USHORT Reserved;
} Q6V_MIXER;

typedef struct _Q6V_MFC
{
    ULONG Rate;
    USHORT Bits;
    USHORT Channels;
    USHORT Type[Q6V_MAX_CHANNELS];
} Q6V_MFC;
#include <poppack.h>

C_ASSERT(sizeof(Q6V_CVP_SETUP) == 44);
C_ASSERT(sizeof(Q6V_STEP) == 8);
C_ASSERT(sizeof(Q6V_MUTE_ARGS) == 6);
C_ASSERT(FIELD_OFFSET(Q6V_PARAM, Data) == 28);
C_ASSERT(sizeof(Q6V_CHANNEL_INFO) == 16);
C_ASSERT(sizeof(Q6V_ENDPOINT) == 20);
C_ASSERT(sizeof(Q6V_MIXER) == 20);
C_ASSERT(sizeof(Q6V_MFC) == 24);

/* Format of one side of the call as the backend runs it */
typedef struct _Q6V_PATH
{
    USHORT PortId;
    USHORT Channels;
    USHORT Bits;
    ULONG Rate;
    UCHAR Map[Q6V_MAX_CHANNELS];
    BOOLEAN LinkOpen;
} Q6V_PATH, *PQ6V_PATH;

typedef struct _Q6_VOICE
{
    KMUTEX Lock;
    APR_HANDLE Mvm;
    APR_HANDLE Cvs;
    APR_HANDLE Cvp;

    /* One command in flight; the receive matches its ack by opcode */
    KEVENT Answered;
    volatile ULONG Pending;
    volatile ULONG Result;
    volatile USHORT ReplyPort;

    CHAR CvdVersion[Q6V_VERSION_BYTES];
    BOOLEAN CvpApiKnown;
    ULONG CvpApi;

    USHORT MvmHandle;
    USHORT CvsHandle;
    USHORT CvpHandle;
    BOOLEAN Active;

    Q6V_PATH Rx;
    Q6V_PATH Tx;

    ULONG RxStep;
    USHORT RxStepRampMs;
    USHORT TxMute;
    USHORT TxMuteRampMs;
} Q6_VOICE, *PQ6_VOICE;

static Q6_VOICE Q6Voice;

static
PCSTR
Q6vServiceName(
    _In_ ULONG Service)
{
    if (Service == Q6V_SVC_MVM)
        return "mvm";
    if (Service == Q6V_SVC_CVS)
        return "cvs";
    if (Service == Q6V_SVC_CVP)
        return "cvp";
    return "?";
}

static
VOID
NTAPI
Q6vReceive(
    _In_ PVOID Context,
    _In_reads_bytes_(Length) const VOID *Packet,
    _In_ ULONG Length)
{
    PQ6_VOICE Voice = Context;
    const APR_HEADER *Hdr = Packet;
    const UCHAR *Body = (const UCHAR *)Packet + sizeof(APR_HEADER);
    ULONG BodyLength;

    if (Length < sizeof(APR_HEADER))
        return;
    BodyLength = Length - sizeof(APR_HEADER);

    if (Hdr->Opcode == Q6V_ACK && BodyLength >= 2 * sizeof(ULONG))
    {
        ULONG Command = ((const ULONG *)Body)[0];
        ULONG Status = ((const ULONG *)Body)[1];

        QC_LOG("voice < %s ack %08lx status 0x%lx port %u",
               Q6vServiceName(Hdr->SrcSvc), Command, Status, Hdr->SrcPort);

        if (Command == Voice->Pending)
        {
            Voice->Result = Status;
            Voice->ReplyPort = Hdr->SrcPort;
            KeSetEvent(&Voice->Answered, IO_NO_INCREMENT, FALSE);
        }
        return;
    }

    if (Hdr->Opcode == Q6V_VERSION_REPLY && BodyLength)
    {
        ULONG Count = min(BodyLength, (ULONG)Q6V_VERSION_BYTES);

        RtlCopyMemory(Voice->CvdVersion, Body, Count);
        Voice->CvdVersion[Count - 1] = ANSI_NULL;
        QC_LOG("voice < mvm version reply \"%s\"", Voice->CvdVersion);

        if (Voice->Pending == Q6V_VERSION_QUERY)
        {
            Voice->Result = 0;
            KeSetEvent(&Voice->Answered, IO_NO_INCREMENT, FALSE);
        }
        return;
    }

    QC_LOG("voice < %s opcode %08lx (%lu bytes) not handled",
           Q6vServiceName(Hdr->SrcSvc), Hdr->Opcode, BodyLength);
}

/* Sends one voice command and waits for the DSP to answer it */
static
NTSTATUS
Q6vCall(
    _In_ PQ6_VOICE Voice,
    _In_ ULONG Service,
    _In_ USHORT DestPort,
    _In_ ULONG Opcode,
    _In_ ULONG Token,
    _In_reads_bytes_opt_(Length) const VOID *Payload,
    _In_ ULONG Length)
{
    LARGE_INTEGER Timeout;
    APR_HANDLE Handle;
    NTSTATUS Status;

    if (Service == Q6V_SVC_MVM)
        Handle = Voice->Mvm;
    else if (Service == Q6V_SVC_CVS)
        Handle = Voice->Cvs;
    else
        Handle = Voice->Cvp;

    QC_LOG("voice > %s op %08lx port %u token %lu len %lu",
           Q6vServiceName(Service), Opcode, DestPort, Token, Length);

    KeClearEvent(&Voice->Answered);
    Voice->Result = MAXULONG;
    Voice->ReplyPort = 0;
    Voice->Pending = Opcode;

    Status = QcAprSendTo(Handle, Service, DestPort, Q6V_APPS_PORT, Opcode, Token, Payload, Length);
    if (!NT_SUCCESS(Status))
    {
        Voice->Pending = 0;
        QC_LOG("voice: op %08lx not sent 0x%08lx", Opcode, Status);
        return Status;
    }

    Timeout.QuadPart = -10LL * 1000 * Q6V_TIMEOUT_MS;
    Status = KeWaitForSingleObject(&Voice->Answered, Executive, KernelMode, FALSE, &Timeout);
    Voice->Pending = 0;
    if (Status != STATUS_SUCCESS)
    {
        QC_LOG("voice: op %08lx no answer in %u ms", Opcode, Q6V_TIMEOUT_MS);
        return STATUS_IO_TIMEOUT;
    }

    if (Voice->Result != 0)
        return STATUS_UNSUCCESSFUL;
    return STATUS_SUCCESS;
}

static
NTSTATUS
Q6vSetParam(
    _In_ PQ6_VOICE Voice,
    _In_ ULONG Token,
    _In_ ULONG ModuleId,
    _In_ ULONG ParamId,
    _In_reads_bytes_(Size) const VOID *Data,
    _In_ USHORT Size)
{
    Q6V_PARAM Param;

    if (Size > sizeof(Param.Data))
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(&Param, sizeof(Param));
    Param.MemSize = FIELD_OFFSET(Q6V_PARAM, Data) - FIELD_OFFSET(Q6V_PARAM, ModuleId) + Size;
    Param.ModuleId = ModuleId;
    Param.ParamId = ParamId;
    Param.ParamSize = Size;
    RtlCopyMemory(Param.Data, Data, Size);

    return Q6vCall(Voice, Q6V_SVC_CVP, Voice->CvpHandle, Q6V_SET_PARAM_V2, Token,
                   &Param, FIELD_OFFSET(Q6V_PARAM, Data) + Size);
}

static
Q6V_CVD_LEVEL
Q6vCvdLevel(
    _In_z_ PCSTR Version)
{
    static const PCSTR Known[] = { "", "0.0", "2.1", "2.2", "2.3" };
    ULONG i;

    /* A version newer than the table gets the oldest command set */
    for (i = 0; i < RTL_NUMBER_OF(Known); i++)
    {
        if (!strcmp(Version, Known[i]))
            return (Q6V_CVD_LEVEL)i;
    }
    return Q6vCvdUnknown;
}

static
VOID
Q6vInitPath(
    _Out_ PQ6V_PATH Path,
    _In_ USHORT PortId)
{
    RtlZeroMemory(Path, sizeof(*Path));
    Path->PortId = PortId;
    Path->Channels = Q6V_CHANNELS;
    Path->Bits = Q6V_BITS;
    Path->Rate = Q6V_RATE;

    /* Voice routing marks only the first channel */
    Path->Map[0] = 1;
}

static
NTSTATUS
Q6vRegister(
    _In_ PQ6_VOICE Voice)
{
    if (!Voice->Mvm)
        Voice->Mvm = QcAprRegister(APR_DOMAIN_ADSP, Q6V_SVC_MVM, Q6vReceive, Voice);
    if (Voice->Mvm && !Voice->Cvs)
        Voice->Cvs = QcAprRegister(APR_DOMAIN_ADSP, Q6V_SVC_CVS, Q6vReceive, Voice);
    if (Voice->Cvs && !Voice->Cvp)
        Voice->Cvp = QcAprRegister(APR_DOMAIN_ADSP, Q6V_SVC_CVP, Q6vReceive, Voice);

    if (Voice->Mvm && Voice->Cvs && Voice->Cvp)
        return STATUS_SUCCESS;

    QC_LOG("voice: could not register the voice services with apr");
    if (Voice->Cvs)
    {
        QcAprUnregister(Voice->Cvs);
        Voice->Cvs = NULL;
    }
    if (Voice->Mvm)
    {
        QcAprUnregister(Voice->Mvm);
        Voice->Mvm = NULL;
    }
    return STATUS_INSUFFICIENT_RESOURCES;
}

/* Asked once; a DSP that does not answer is treated as the oldest CVD */
static
VOID
Q6vQueryCvd(
    _In_ PQ6_VOICE Voice)
{
    if (Voice->CvdVersion[0])
        return;

    if (!NT_SUCCESS(Q6vCall(Voice, Q6V_SVC_MVM, 0, Q6V_VERSION_QUERY, 0, NULL, 0)))
        RtlCopyMemory(Voice->CvdVersion, "0.0", sizeof("0.0"));

    QC_LOG("voice: cvd version \"%s\", level %u", Voice->CvdVersion, Q6vCvdLevel(Voice->CvdVersion));
}

/* Joins the modem's default voice session; the passive handles outlive a call */
static
NTSTATUS
Q6vJoinModemSession(
    _In_ PQ6_VOICE Voice)
{
    Q6V_NAME Name;
    NTSTATUS Status;

    RtlZeroMemory(&Name, sizeof(Name));
    RtlCopyMemory(Name.Text, "default modem voice", sizeof("default modem voice"));

    if (!Voice->MvmHandle)
    {
        Status = Q6vCall(Voice, Q6V_SVC_MVM, 0, Q6V_MVM_JOIN, 0, &Name, sizeof(Name));
        if (!NT_SUCCESS(Status))
            return Status;
        Voice->MvmHandle = Voice->ReplyPort;
    }

    if (!Voice->CvsHandle)
    {
        Status = Q6vCall(Voice, Q6V_SVC_CVS, 0, Q6V_CVS_JOIN, 0, &Name, sizeof(Name));
        if (!NT_SUCCESS(Status))
            return Status;
        Voice->CvsHandle = Voice->ReplyPort;
    }

    QC_LOG("voice: modem session mvm %u cvs %u", Voice->MvmHandle, Voice->CvsHandle);
    return STATUS_SUCCESS;
}

static
NTSTATUS
Q6vCreateVocproc(
    _In_ PQ6_VOICE Voice)
{
    Q6V_CVP_SETUP Setup;
    ULONG Opcode;
    NTSTATUS Status;

    /* With no topology calibration the default Rx and single mic Tx topologies apply */
    RtlZeroMemory(&Setup, sizeof(Setup));
    Setup.Direction = Q6V_DIR_RX_TX;
    Setup.TxPort = Voice->Tx.PortId;
    Setup.TxTopology = Q6V_TOPO_TX;
    Setup.RxPort = Voice->Rx.PortId;
    Setup.RxTopology = Q6V_TOPO_RX;
    Setup.Profile = Q6V_PROFILE_NONE;
    Setup.Mode = Q6V_EC_INTERNAL;
    Setup.EcRefPort = Q6V_PORT_NONE;

    Opcode = (Q6vCvdLevel(Voice->CvdVersion) >= Q6vCvd22) ? Q6V_CVP_CREATE_V3 : Q6V_CVP_CREATE_V2;
    Status = Q6vCall(Voice, Q6V_SVC_CVP, 0, Opcode, 0, &Setup, sizeof(Setup));
    if (!NT_SUCCESS(Status))
        return Status;

    Voice->CvpHandle = Voice->ReplyPort;
    QC_LOG("voice: vocproc %u on rx 0x%x tx 0x%x", Voice->CvpHandle, Setup.RxPort, Setup.TxPort);
    return STATUS_SUCCESS;
}

static
NTSTATUS
Q6vChannelInfo(
    _In_ PQ6_VOICE Voice,
    _In_ ULONG ParamId,
    _In_ PQ6V_PATH Path)
{
    Q6V_CHANNEL_INFO Info;

    RtlZeroMemory(&Info, sizeof(Info));
    Info.Channels = Path->Channels;
    Info.Bits = Path->Bits;
    RtlCopyMemory(Info.Map, Path->Map, sizeof(Info.Map));

    return Q6vSetParam(Voice, Q6V_TOKEN_GENERIC, Q6V_MOD_CVD, ParamId, &Info, sizeof(Info));
}

static
NTSTATUS
Q6vEndpoint(
    _In_ PQ6_VOICE Voice,
    _In_ ULONG ParamId,
    _In_ PQ6V_PATH Path)
{
    Q6V_ENDPOINT Endpoint;

    RtlZeroMemory(&Endpoint, sizeof(Endpoint));
    Endpoint.PortId = Path->PortId;
    Endpoint.Channels = Path->Channels;
    Endpoint.Bits = Path->Bits;
    Endpoint.Rate = Path->Rate;
    RtlCopyMemory(Endpoint.Map, Path->Map, sizeof(Endpoint.Map));

    return Q6vSetParam(Voice, Q6V_TOKEN_MEDIA_FORMAT, Q6V_MOD_CVD, ParamId, &Endpoint, sizeof(Endpoint));
}

/* Tells the vocproc the channel layout and the endpoint formats of both ports */
static
NTSTATUS
Q6vSendFormats(
    _In_ PQ6_VOICE Voice)
{
    Q6V_CVD_LEVEL Level = Q6vCvdLevel(Voice->CvdVersion);
    Q6V_DEV_CHANNELS Channels;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Voice->CvpApi < Q6V_CVP_API_CHANNEL_INFO)
    {
        if (Level >= Q6vCvd22)
        {
            Channels.TxChannels = Voice->Tx.Channels;
            Channels.RxChannels = 1;
            Status = Q6vCall(Voice, Q6V_SVC_CVP, Voice->CvpHandle, Q6V_CVP_DEV_CHANNELS, 0,
                             &Channels, sizeof(Channels));
        }
    }
    else
    {
        /* The echo reference follows the Rx device */
        Status = Q6vChannelInfo(Voice, Q6V_PID_RX_CHANNELS, &Voice->Rx);
        if (NT_SUCCESS(Status))
            Status = Q6vChannelInfo(Voice, Q6V_PID_TX_CHANNELS, &Voice->Tx);
        if (NT_SUCCESS(Status))
            Status = Q6vChannelInfo(Voice, Q6V_PID_EC_CHANNELS, &Voice->Rx);
    }

    if (NT_SUCCESS(Status) && Level >= Q6vCvd23)
    {
        Status = Q6vEndpoint(Voice, Q6V_PID_RX_ENDPOINT, &Voice->Rx);
        if (NT_SUCCESS(Status))
            Status = Q6vEndpoint(Voice, Q6V_PID_TX_ENDPOINT, &Voice->Tx);
    }

    return Status;
}

/* The vocproc renders mono; spread it over every Rx channel */
static
VOID
Q6vRxUpmix(
    _In_ PQ6_VOICE Voice)
{
    Q6V_MIXER Mixer;
    Q6V_MFC Mfc;
    ULONG i;

    if (Voice->CvpApi < Q6V_CVP_API_CHANNEL_INFO)
    {
        QC_LOG("voice: cvp api %lu has no rx channel mixer", Voice->CvpApi);
        return;
    }

    RtlZeroMemory(&Mixer, sizeof(Mixer));
    Mixer.Outputs = Voice->Rx.Channels;
    Mixer.Inputs = 1;
    Mixer.OutMap[0] = 1;
    Mixer.OutMap[1] = 2;
    Mixer.InMap[0] = 1;
    Mixer.Coeff[0][0] = Q6V_UNITY_Q14;
    Mixer.Coeff[1][0] = Q6V_UNITY_Q14;
    if (!NT_SUCCESS(Q6vSetParam(Voice, Q6V_TOKEN_GENERIC, Q6V_MOD_MFC, Q6V_PID_MIXER_COEFF,
                                &Mixer, sizeof(Mixer))))
    {
        QC_LOG("voice: rx channel mixer not taken");
    }

    RtlZeroMemory(&Mfc, sizeof(Mfc));
    Mfc.Rate = Voice->Rx.Rate;
    Mfc.Bits = Q6V_BITS;
    Mfc.Channels = Voice->Rx.Channels;
    for (i = 0; i < Q6V_MAX_CHANNELS; i++)
        Mfc.Type[i] = Voice->Rx.Map[i];
    if (!NT_SUCCESS(Q6vSetParam(Voice, Q6V_TOKEN_GENERIC, Q6V_MOD_MFC, Q6V_PID_MFC_FORMAT,
                                &Mfc, sizeof(Mfc))))
    {
        QC_LOG("voice: rx mfc format not taken");
    }
}

static
NTSTATUS
Q6vSetupVocproc(
    _In_ PQ6_VOICE Voice)
{
    ULONG Mode = 0;
    NTSTATUS Status;

    Status = Q6vCreateVocproc(Voice);
    if (!NT_SUCCESS(Status))
        return Status;

    if (!Voice->CvpApiKnown)
    {
        Status = QcQ6CoreServiceVersion(Q6V_CORE_ID_CVP, &Voice->CvpApi);
        if (!NT_SUCCESS(Status))
        {
            QC_LOG("voice: no cvp api version 0x%08lx", Status);
            return Status;
        }
        Voice->CvpApiKnown = TRUE;
    }

    Status = Q6vSendFormats(Voice);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Q6vCvdLevel(Voice->CvdVersion) >= Q6vCvd22)
    {
        Status = Q6vCall(Voice, Q6V_SVC_CVP, Voice->CvpHandle, Q6V_CVP_COMMIT, 0, NULL, 0);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    if (Voice->Rx.Channels > 1)
        Q6vRxUpmix(Voice);

    /*
     * Stream, device config, vocproc and volume calibration register here once
     * the DSP can map host memory; each of those commands carries a map handle.
     */

    Status = Q6vCall(Voice, Q6V_SVC_CVP, Voice->CvpHandle, Q6V_CVP_ENABLE, 0, NULL, 0);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = Q6vCall(Voice, Q6V_SVC_MVM, Voice->MvmHandle, Q6V_MVM_ATTACH_CVP, 0,
                     &Voice->CvpHandle, sizeof(Voice->CvpHandle));
    if (!NT_SUCCESS(Status))
        return Status;

    /* TTY off; the answer does not gate the call */
    Q6vCall(Voice, Q6V_SVC_MVM, Voice->MvmHandle, Q6V_MVM_SET_TTY, 0, &Mode, sizeof(Mode));
    return STATUS_SUCCESS;
}

static
NTSTATUS
Q6vSendRxStep(
    _In_ PQ6_VOICE Voice)
{
    Q6V_STEP Step;

    RtlZeroMemory(&Step, sizeof(Step));
    Step.Direction = Q6V_DIRECTION_RX;
    Step.Value = Voice->RxStep;
    Step.RampMs = Voice->RxStepRampMs;
    return Q6vCall(Voice, Q6V_SVC_CVP, Voice->CvpHandle, Q6V_VOLUME_STEP, 0, &Step, sizeof(Step));
}

static
NTSTATUS
Q6vSendTxMute(
    _In_ PQ6_VOICE Voice)
{
    Q6V_MUTE_ARGS Mute;

    RtlZeroMemory(&Mute, sizeof(Mute));
    Mute.Direction = Q6V_DIRECTION_TX;
    Mute.Mute = Voice->TxMute;
    Mute.RampMs = Voice->TxMuteRampMs;
    return Q6vCall(Voice, Q6V_SVC_CVS, Voice->CvsHandle, Q6V_MUTE, 0, &Mute, sizeof(Mute));
}

/* Stops voice, detaches and destroys the vocproc; the modem session stays */
static
VOID
Q6vTeardownVocproc(
    _In_ PQ6_VOICE Voice)
{
    Q6vCall(Voice, Q6V_SVC_MVM, Voice->MvmHandle, Q6V_MVM_STOP, 0, NULL, 0);

    if (!NT_SUCCESS(Q6vCall(Voice, Q6V_SVC_MVM, Voice->MvmHandle, Q6V_MVM_DETACH_CVP, 0,
                            &Voice->CvpHandle, sizeof(Voice->CvpHandle))))
    {
        return;
    }

    /* Calibration deregistration goes here along with the registration above */

    if (NT_SUCCESS(Q6vCall(Voice, Q6V_SVC_CVP, Voice->CvpHandle, Q6V_DESTROY_SESSION, 0, NULL, 0)))
        Voice->CvpHandle = 0;
}

/* The backend links come up before the voice session, Rx first */
static
NTSTATUS
Q6vOpenLinks(
    _In_ PQ6_VOICE Voice)
{
    NTSTATUS Status;

    Status = QcQ6GraphLinkOpen(Voice->Rx.PortId);
    if (!NT_SUCCESS(Status))
        return Status;
    Voice->Rx.LinkOpen = TRUE;

    Status = QcQ6GraphLinkOpen(Voice->Tx.PortId);
    if (!NT_SUCCESS(Status))
        return Status;
    Voice->Tx.LinkOpen = TRUE;
    return STATUS_SUCCESS;
}

static
VOID
Q6vCloseLinks(
    _In_ PQ6_VOICE Voice)
{
    if (Voice->Rx.LinkOpen)
    {
        QcQ6GraphLinkClose(Voice->Rx.PortId);
        Voice->Rx.LinkOpen = FALSE;
    }

    if (Voice->Tx.LinkOpen)
    {
        QcQ6GraphLinkClose(Voice->Tx.PortId);
        Voice->Tx.LinkOpen = FALSE;
    }
}

VOID
NTAPI
QcQ6VoiceInit(
    _In_ USHORT RxPort,
    _In_ USHORT TxPort)
{
    PQ6_VOICE Voice = &Q6Voice;

    RtlZeroMemory(Voice, sizeof(*Voice));
    KeInitializeMutex(&Voice->Lock, 0);
    KeInitializeEvent(&Voice->Answered, NotificationEvent, FALSE);

    Q6vInitPath(&Voice->Rx, RxPort);
    Q6vInitPath(&Voice->Tx, TxPort);
    Voice->RxStepRampMs = Q6V_VOLUME_RAMP_MS;
}

NTSTATUS
NTAPI
QcQ6VoiceStart(VOID)
{
    PQ6_VOICE Voice = &Q6Voice;
    NTSTATUS Status;

    PAGED_CODE();

    if (Voice->Rx.PortId == MAXUSHORT || Voice->Tx.PortId == MAXUSHORT)
    {
        QC_LOG("voice: no board ports, set VoiceRxPort and VoiceTxPort");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    KeWaitForSingleObject(&Voice->Lock, Executive, KernelMode, FALSE, NULL);
    if (Voice->Active)
    {
        KeReleaseMutex(&Voice->Lock, FALSE);
        return STATUS_DEVICE_BUSY;
    }

    QC_LOG("voice: call start, rx 0x%x tx 0x%x", Voice->Rx.PortId, Voice->Tx.PortId);

    Status = Q6vOpenLinks(Voice);
    if (NT_SUCCESS(Status))
        Status = Q6vRegister(Voice);
    if (NT_SUCCESS(Status))
    {
        Q6vQueryCvd(Voice);
        Status = Q6vJoinModemSession(Voice);
    }
    if (NT_SUCCESS(Status))
    {
        BOOLEAN Enable = TRUE;

        Status = Q6vCall(Voice, Q6V_SVC_MVM, Voice->MvmHandle, Q6V_MVM_DUAL_CONTROL, 0,
                         &Enable, sizeof(Enable));
    }
    if (NT_SUCCESS(Status))
        Status = Q6vSetupVocproc(Voice);
    if (NT_SUCCESS(Status))
    {
        if (!NT_SUCCESS(Q6vSendRxStep(Voice)))
            QC_LOG("voice: rx volume step not taken");
        if (!NT_SUCCESS(Q6vSendTxMute(Voice)))
            QC_LOG("voice: tx mute not taken");

        Status = Q6vCall(Voice, Q6V_SVC_MVM, Voice->MvmHandle, Q6V_MVM_START, 0, NULL, 0);
    }

    /* A failed start leaves the DSP sessions as they are, only the links close */
    if (NT_SUCCESS(Status))
        Voice->Active = TRUE;
    else
        Q6vCloseLinks(Voice);

    QC_LOG("voice: call start 0x%08lx", Status);
    KeReleaseMutex(&Voice->Lock, FALSE);
    return Status;
}

VOID
NTAPI
QcQ6VoiceStop(VOID)
{
    PQ6_VOICE Voice = &Q6Voice;

    PAGED_CODE();

    KeWaitForSingleObject(&Voice->Lock, Executive, KernelMode, FALSE, NULL);

    /* The backend links shut before the voice session ends */
    Q6vCloseLinks(Voice);

    if (Voice->Active)
    {
        Q6vTeardownVocproc(Voice);
        Voice->TxMute = 0;
        Voice->Active = FALSE;
        QC_LOG("voice: call stopped, modem session mvm %u cvs %u kept",
               Voice->MvmHandle, Voice->CvsHandle);
    }

    KeReleaseMutex(&Voice->Lock, FALSE);
}

NTSTATUS
NTAPI
QcQ6VoiceSetTxMute(
    _In_ BOOLEAN Mute,
    _In_ ULONG RampMs)
{
    PQ6_VOICE Voice = &Q6Voice;
    NTSTATUS Status = STATUS_SUCCESS;

    PAGED_CODE();

    if (RampMs > Q6V_MAX_RAMP_MS)
        return STATUS_INVALID_PARAMETER;

    KeWaitForSingleObject(&Voice->Lock, Executive, KernelMode, FALSE, NULL);

    Voice->TxMute = Mute ? 1 : 0;
    Voice->TxMuteRampMs = (USHORT)RampMs;
    if (Voice->Active)
        Status = Q6vSendTxMute(Voice);

    KeReleaseMutex(&Voice->Lock, FALSE);
    return Status;
}
