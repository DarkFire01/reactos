/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Voice service, call control and call state reporting
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcril.h"
#include <string.h>

#define QCRIL_VOICE_INDICATION_REGISTER 0x0003
#define QCRIL_VOICE_DIAL_CALL           0x0020
#define QCRIL_VOICE_END_CALL            0x0021
#define QCRIL_VOICE_ANSWER_CALL         0x0022
#define QCRIL_VOICE_START_CONT_DTMF     0x0029
#define QCRIL_VOICE_STOP_CONT_DTMF      0x002A
#define QCRIL_VOICE_ALL_CALL_STATUS_IND 0x002E
#define QCRIL_VOICE_GET_ALL_CALL_INFO   0x002F
#define QCRIL_VOICE_MANAGE_CALLS        0x0031
#define QCRIL_VOICE_BIND_SUBSCRIPTION   0x0044

#define QCRIL_VOICE_TLV_CALLING_NUMBER  0x01
#define QCRIL_VOICE_TLV_CLIR            0x11
#define QCRIL_VOICE_TLV_SERVICE_TYPE    0x16
#define QCRIL_VOICE_TLV_CALL_ID         0x01
#define QCRIL_VOICE_TLV_DIAL_CALL_ID    0x10
#define QCRIL_VOICE_TLV_REJECT          0x15
#define QCRIL_VOICE_TLV_SUPS_TYPE       0x01
#define QCRIL_VOICE_TLV_SUPS_CALL_ID    0x10
#define QCRIL_VOICE_TLV_DTMF            0x01
#define QCRIL_VOICE_TLV_CALL_INFO       0x01
#define QCRIL_VOICE_TLV_REMOTE_NUMBER   0x10
#define QCRIL_VOICE_TLV_ALERTING        0x12
#define QCRIL_VOICE_TLV_END_REASON      0x14

/* Indication register switches */
static const UCHAR QcVoiceIndications[] = { 0x10, 0x12, 0x14, 0x15, 0x18, 0x1E, 0x1F, 0x24 };

/* call_state_enum_v02 */
#define QCRIL_CALL_ORIGINATING          0x01
#define QCRIL_CALL_INCOMING             0x02
#define QCRIL_CALL_CONVERSATION         0x03
#define QCRIL_CALL_CC_IN_PROGRESS       0x04
#define QCRIL_CALL_ALERTING             0x05
#define QCRIL_CALL_HOLD                 0x06
#define QCRIL_CALL_WAITING              0x07
#define QCRIL_CALL_DISCONNECTING        0x08
#define QCRIL_CALL_END                  0x09
#define QCRIL_CALL_SETUP                0x0A

/* sups_type_enum_v02 */
#define QCRIL_SUPS_RELEASE_HELD         0x01
#define QCRIL_SUPS_RELEASE_ACTIVE       0x02
#define QCRIL_SUPS_SWAP                 0x03
#define QCRIL_SUPS_SPLIT                0x04
#define QCRIL_SUPS_CONFERENCE           0x05
#define QCRIL_SUPS_TRANSFER             0x06

/* clir_type_enum_v02 */
#define QCRIL_CLIR_SUPPRESSION          0x01
#define QCRIL_CLIR_INVOCATION           0x02

/* voice_dial_call_service_type */
#define QCRIL_SERVICE_AUTOMATIC         0x01
#define QCRIL_SERVICE_IMS               0x06
#define QCRIL_SERVICE_CS                0x09

/* QMI errors with a dial specific meaning */
#define QCRIL_QMI_ERR_INVALID_ID        0x000D
#define QCRIL_QMI_ERR_INCOMPATIBLE      0x005A
#define QCRIL_QMI_ERR_FDN_RESTRICT      0x005F
#define QCRIL_QMI_ERR_NOT_PROVISIONED   0x0017

/* alerting_type_enum_v02, local alerting means the phone plays the ringback */
#define QCRIL_ALERTING_LOCAL            0x00

#define QCRIL_DIAL_MAX                  81

/* call_direction_enum_v02 */
#define QCRIL_VOICE_DIR_MT              0x02

/* Each call_info record on the wire, one byte fields */
#include <pshpack1.h>
typedef struct _QCRIL_CALL_INFO
{
    UCHAR CallId;
    UCHAR CallState;
    UCHAR CallType;
    UCHAR Direction;
    UCHAR Mode;
    UCHAR IsMultiparty;
    UCHAR Als;
} QCRIL_CALL_INFO;
#include <poppack.h>

#include <pshpack4.h>
typedef struct _QCRIL_DIAL_IN
{
    DWORD Executor;
    RILADDRESS Address;
    DWORD Options;
    DWORD Domain;
} QCRIL_DIAL_IN;

typedef struct _QCRIL_MANAGE_IN
{
    DWORD Executor;
    DWORD Action;
    DWORD CallId;
} QCRIL_MANAGE_IN;

typedef struct _QCRIL_RINGBACK
{
    DWORD Size;
    DWORD Params;
    DWORD Executor;
    DWORD Active;
    DWORD Reserved[2];
} QCRIL_RINGBACK;
#include <poppack.h>

static
DWORD
QcVoiceMapState(
    _In_ UCHAR CallState,
    _In_ DWORD Previous)
{
    switch (CallState)
    {
        case QCRIL_CALL_ORIGINATING:
        case QCRIL_CALL_ALERTING:
            return RIL_CALLSTATUS_OUTGOING;
        case QCRIL_CALL_INCOMING:
        case QCRIL_CALL_WAITING:
        case QCRIL_CALL_SETUP:
            return RIL_CALLSTATUS_INCOMING;
        case QCRIL_CALL_CONVERSATION:
            return RIL_CALLSTATUS_CONNECTED;
        case QCRIL_CALL_HOLD:
            return RIL_CALLSTATUS_HELD;
        case QCRIL_CALL_END:
            return RIL_CALLSTATUS_DISCONNECTED;
        case QCRIL_CALL_DISCONNECTING:
            return Previous;
        default:
            return RIL_CALLSTATUS_IDLE;
    }
}

/* Finds the call for a modem id, optionally taking a free entry for it */
static
PQC_CALL
QcVoiceFind(
    _In_ PQC_MODEM Modem,
    _In_ UCHAR ModemId,
    _In_ BOOL Create)
{
    PQC_CALL Free = NULL;
    ULONG i;

    for (i = 0; i < QC_VOICE_MAX_CALLS; i++)
    {
        if (Modem->Calls[i].Used && Modem->Calls[i].ModemId == ModemId)
            return &Modem->Calls[i];
        if (!Modem->Calls[i].Used && !Free)
            Free = &Modem->Calls[i];
    }

    if (!Create || !Free)
        return NULL;

    ZeroMemory(Free, sizeof(*Free));
    Free->Used = TRUE;
    Free->ModemId = ModemId;
    Free->RilId = Modem->NextCallId++;
    if (Modem->NextCallId == 0)
        Modem->NextCallId = 1;
    Free->Status = RIL_CALLSTATUS_IDLE;
    return Free;
}

/* The modem id of a RIL call id, FALSE when the call is unknown */
static
BOOL
QcVoiceModemId(
    _In_ PQC_MODEM Modem,
    _In_ DWORD RilId,
    _Out_ PUCHAR ModemId)
{
    BOOL Found = FALSE;
    ULONG i;

    EnterCriticalSection(&Modem->StateLock);
    for (i = 0; i < QC_VOICE_MAX_CALLS; i++)
    {
        if (Modem->Calls[i].Used && Modem->Calls[i].RilId == RilId)
        {
            *ModemId = Modem->Calls[i].ModemId;
            Found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&Modem->StateLock);

    return Found;
}

/* The modem id of the first call in a state that takes DTMF */
static
BOOL
QcVoiceActiveId(
    _In_ PQC_MODEM Modem,
    _Out_ PUCHAR ModemId)
{
    BOOL Found = FALSE;
    ULONG i;

    EnterCriticalSection(&Modem->StateLock);
    for (i = 0; i < QC_VOICE_MAX_CALLS && !Found; i++)
    {
        if (Modem->Calls[i].Used &&
            (Modem->Calls[i].Status == RIL_CALLSTATUS_CONNECTED || Modem->Calls[i].Status == RIL_CALLSTATUS_OUTGOING))
        {
            *ModemId = Modem->Calls[i].ModemId;
            Found = TRUE;
        }
    }
    LeaveCriticalSection(&Modem->StateLock);

    return Found;
}

static
HRESULT
QcVoiceSimple(
    _In_ PQC_MODEM Modem,
    _In_ USHORT MessageId,
    _In_ UCHAR Tlv,
    _In_reads_bytes_(Length) const VOID *Value,
    _In_ USHORT Length)
{
    QMI_ENCODER Encoder;
    BYTE Message[32];
    ULONG Size;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, MessageId);
    if (!QmiEncodeTlv(&Encoder, Tlv, Value, Length))
        return E_FAIL;
    Size = QmiEncodeEnd(&Encoder);

    return QcQmiCommand(Modem, QcSvcVoice, Message, Size);
}

static
HRESULT
QcVoiceSups(
    _In_ PQC_MODEM Modem,
    _In_ UCHAR Type,
    _In_ BOOL WithCall,
    _In_ UCHAR ModemId)
{
    QMI_ENCODER Encoder;
    BYTE Message[32];
    ULONG Size;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_VOICE_MANAGE_CALLS);
    QmiEncodeTlv(&Encoder, QCRIL_VOICE_TLV_SUPS_TYPE, &Type, sizeof(Type));
    if (WithCall)
        QmiEncodeTlv(&Encoder, QCRIL_VOICE_TLV_SUPS_CALL_ID, &ModemId, sizeof(ModemId));
    Size = QmiEncodeEnd(&Encoder);

    QcTrace("manage calls: sups type %u call %u\n", Type, WithCall ? ModemId : 0);
    return QcQmiCommand(Modem, QcSvcVoice, Message, Size);
}

VOID
QcVoiceSetup(
    _In_ PQC_MODEM Modem)
{
    QMI_ENCODER Encoder;
    BYTE Message[64];
    BYTE Response[QC_MESSAGE_MAX];
    UCHAR Primary = 0, One = 1;
    ULONG Length, ResponseLen, i;
    HRESULT hr;

    hr = QcVoiceSimple(Modem, QCRIL_VOICE_BIND_SUBSCRIPTION, 0x01, &Primary, sizeof(Primary));
    QcTrace("voice: bind subscription 0x%08lx\n", hr);

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_VOICE_INDICATION_REGISTER);
    for (i = 0; i < RTL_NUMBER_OF(QcVoiceIndications); i++)
        QmiEncodeTlv(&Encoder, QcVoiceIndications[i], &One, sizeof(One));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiCommand(Modem, QcSvcVoice, Message, Length);
    QcTrace("voice: indication registration 0x%08lx\n", hr);

    /* Querying the call list also registers this port for status indications */
    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_VOICE_GET_ALL_CALL_INFO);
    Length = QmiEncodeEnd(&Encoder);
    QcQmiRequest(Modem, QcSvcVoice, Message, Length, Response, sizeof(Response), &ResponseLen);
}

/* The modem restarted, every call it knew about is gone */
VOID
QcVoiceModemLost(
    _In_ PQC_MODEM Modem)
{
    RIL_CALL_RECORD Record;
    ULONG i;

    for (i = 0; i < QC_VOICE_MAX_CALLS; i++)
    {
        EnterCriticalSection(&Modem->StateLock);
        if (!Modem->Calls[i].Used)
        {
            LeaveCriticalSection(&Modem->StateLock);
            continue;
        }

        ZeroMemory(&Record, sizeof(Record));
        Record.Size = sizeof(Record);
        Record.Executor = QC_EXECUTOR;
        Record.CallId = Modem->Calls[i].RilId;
        Record.Direction = Modem->Calls[i].Direction;
        Record.Status = RIL_CALLSTATUS_DISCONNECTED;
        Record.Domain = RIL_CALL_DOMAIN_CS;
        Modem->Calls[i].Used = FALSE;
        LeaveCriticalSection(&Modem->StateLock);

        QcTrace("ssr: call %lu dropped\n", Record.CallId);
        QcNotifyAll(Modem, RIL_NOTIFY_CALLSTATE, &Record, sizeof(Record));
    }
}

static
HRESULT
QcVoiceDialError(
    _In_ USHORT Error,
    _In_ HRESULT Mapped)
{
    switch (Error)
    {
        case QCRIL_QMI_ERR_INVALID_ID: return RIL_ERROR(0x51);
        case QCRIL_QMI_ERR_FDN_RESTRICT: return RIL_ERROR(0x15);
        case QCRIL_QMI_ERR_INCOMPATIBLE: return RIL_ERROR(0x04);
        case QCRIL_QMI_ERR_NOT_PROVISIONED: return RIL_ERROR(0x77);
        default: return Mapped;
    }
}

HRESULT
QcVoiceDial(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(InSize) const VOID *In,
    _In_ ULONG InSize,
    _Inout_ PQC_RESP Resp)
{
    const QCRIL_DIAL_IN *Dial = In;
    QMI_ENCODER Encoder;
    BYTE Message[160];
    BYTE Response[256];
    CHAR Number[QCRIL_DIAL_MAX + 2];
    ULONG Digits = 0, Length, ResponseLen, i;
    DWORD Options = 0, Domain = RIL_CALL_DOMAIN_AUTO, ServiceType;
    const VOID *Value;
    USHORT ValueLen;
    UCHAR Clir = 0;
    PQC_CALL Call;
    DWORD RilId;
    HRESULT hr;

    if (InSize < FIELD_OFFSET(QCRIL_DIAL_IN, Options))
        return E_INVALIDARG;
    if (InSize >= FIELD_OFFSET(QCRIL_DIAL_IN, Domain))
        Options = Dial->Options;
    if (InSize >= sizeof(QCRIL_DIAL_IN))
        Domain = Dial->Domain;

    /* A leading plus only when the address type says the number is international */
    if ((Dial->Address.dwParams & RILADDRESS_PARAM_TYPE) &&
        Dial->Address.dwType == RILADDRESSTYPE_INTERNATIONAL &&
        Dial->Address.wszAddress[0] != L'+')
    {
        Number[Digits++] = '+';
    }

    for (i = 0; i < MAXLENGTH_ADDRESS && Dial->Address.wszAddress[i]; i++)
    {
        if (Digits >= QCRIL_DIAL_MAX)
            return RIL_E_DIALSTRINGTOOLONG;
        Number[Digits++] = (CHAR)Dial->Address.wszAddress[i];
    }
    Number[Digits] = '\0';

    if (Digits == 0)
        return RIL_E_INVALIDDIALSTRING;

    if (Options & RIL_DIAL_RESTRICT_CALLER_ID)
        Clir = QCRIL_CLIR_INVOCATION;
    else if (Options & RIL_DIAL_PRESENT_CALLER_ID)
        Clir = QCRIL_CLIR_SUPPRESSION;

    switch (Domain)
    {
        case RIL_CALL_DOMAIN_CS: ServiceType = QCRIL_SERVICE_CS; break;
        case RIL_CALL_DOMAIN_IMS: ServiceType = QCRIL_SERVICE_IMS; break;
        default: ServiceType = QCRIL_SERVICE_AUTOMATIC; break;
    }

    QcTrace("dial '%s' options 0x%lx domain %lu clir %u service %lu\n", Number, Options, Domain, Clir, ServiceType);

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_VOICE_DIAL_CALL);
    if (!QmiEncodeTlv(&Encoder, QCRIL_VOICE_TLV_CALLING_NUMBER, Number, (USHORT)Digits))
        return E_FAIL;
    if (Clir && !QmiEncodeTlv(&Encoder, QCRIL_VOICE_TLV_CLIR, &Clir, sizeof(Clir)))
        return E_FAIL;
    if (!QmiEncodeTlv(&Encoder, QCRIL_VOICE_TLV_SERVICE_TYPE, &ServiceType, sizeof(ServiceType)))
        return E_FAIL;
    Length = QmiEncodeEnd(&Encoder);

    hr = QcQmiRequestWait(Modem, QcSvcVoice, Message, Length, Response, sizeof(Response),
                          &ResponseLen, QC_CALL_SETUP_TIMEOUT_MS);
    if (FAILED(hr))
        return ResponseLen ? QcVoiceDialError(QcQmiError(Response, ResponseLen), hr) : hr;

    /* The caller gets a RIL call id, the modem id stays behind the map */
    RilId = 0;
    if (QmiFindTlv(Response, ResponseLen, QCRIL_VOICE_TLV_DIAL_CALL_ID, &Value, &ValueLen) && ValueLen >= 1)
    {
        EnterCriticalSection(&Modem->StateLock);
        Call = QcVoiceFind(Modem, *(const UCHAR *)Value, TRUE);
        if (Call)
        {
            Call->Direction = RIL_CALLDIR_OUTGOING;
            RilId = Call->RilId;
        }
        LeaveCriticalSection(&Modem->StateLock);
        QcTrace("dial: modem call %u is call %lu\n", *(const UCHAR *)Value, RilId);
    }

    QcRespDword(Resp, RilId);
    return S_OK;
}

HRESULT
QcVoiceManage(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(InSize) const VOID *In,
    _In_ ULONG InSize)
{
    const QCRIL_MANAGE_IN *Manage = In;
    UCHAR ModemId = 0, Ids[QC_VOICE_MAX_CALLS];
    ULONG Count = 0, i;
    BOOL Known;
    HRESULT hr = S_OK;

    if (InSize < sizeof(QCRIL_MANAGE_IN))
        return E_INVALIDARG;

    Known = QcVoiceModemId(Modem, Manage->CallId, &ModemId);
    QcTrace("manage calls: action %lu call %lu (modem %u%s)\n", Manage->Action, Manage->CallId, ModemId,
            Known ? "" : ", unknown");

    switch (Manage->Action)
    {
        case RIL_CALLACTION_RELEASE_HELD:
            return QcVoiceSups(Modem, QCRIL_SUPS_RELEASE_HELD, FALSE, 0);

        case RIL_CALLACTION_RELEASE_ACTIVE_ACCEPT_HELD:
            return QcVoiceSups(Modem, QCRIL_SUPS_RELEASE_ACTIVE, FALSE, 0);

        case RIL_CALLACTION_SWAP:
            return QcVoiceSups(Modem, QCRIL_SUPS_SWAP, FALSE, 0);

        case RIL_CALLACTION_SPLIT:
            if (!Known)
                return E_INVALIDARG;
            return QcVoiceSups(Modem, QCRIL_SUPS_SPLIT, TRUE, ModemId);

        case RIL_CALLACTION_CONFERENCE:
            return QcVoiceSups(Modem, QCRIL_SUPS_CONFERENCE, FALSE, 0);

        case RIL_CALLACTION_TRANSFER:
            return QcVoiceSups(Modem, QCRIL_SUPS_TRANSFER, FALSE, 0);

        case RIL_CALLACTION_ANSWER:
            if (!Known)
                return E_INVALIDARG;
            return QcVoiceSimple(Modem, QCRIL_VOICE_ANSWER_CALL, QCRIL_VOICE_TLV_CALL_ID, &ModemId, sizeof(ModemId));

        case RIL_CALLACTION_RELEASE_CALL:
        case RIL_CALLACTION_HANGUP_OUTGOING:
            if (!Known)
                return E_INVALIDARG;
            return QcVoiceSimple(Modem, QCRIL_VOICE_END_CALL, QCRIL_VOICE_TLV_CALL_ID, &ModemId, sizeof(ModemId));

        case RIL_CALLACTION_RELEASE_ALL:
            EnterCriticalSection(&Modem->StateLock);
            for (i = 0; i < QC_VOICE_MAX_CALLS; i++)
            {
                if (Modem->Calls[i].Used && Modem->Calls[i].Status != RIL_CALLSTATUS_DISCONNECTED)
                    Ids[Count++] = Modem->Calls[i].ModemId;
            }
            LeaveCriticalSection(&Modem->StateLock);

            for (i = 0; i < Count; i++)
            {
                HRESULT End = QcVoiceSimple(Modem, QCRIL_VOICE_END_CALL, QCRIL_VOICE_TLV_CALL_ID, &Ids[i], sizeof(Ids[i]));
                if (FAILED(End))
                    hr = End;
            }
            return hr;

        default:
            return RIL_E_OPNOTSUPPORTED;
    }
}

/* SendDtmf in: executor, digits[256], on and off times. Start in: executor, digit. Stop in: executor. */
HRESULT
QcVoiceDtmf(
    _In_ PQC_MODEM Modem,
    _In_ DWORD Command,
    _In_reads_bytes_(InSize) const VOID *In,
    _In_ ULONG InSize)
{
    const BYTE *Args = In;
    UCHAR Tone[2];
    UCHAR ModemId;
    DWORD OnMs = 0, OffMs = 0;
    ULONG i;
    HRESULT hr;

    if (InSize < sizeof(DWORD))
        return E_INVALIDARG;

    if (!QcVoiceActiveId(Modem, &ModemId))
        return RIL_E_OPNOTALLOWED;

    switch (Command)
    {
        case RilCmdStartDtmf:
            if (InSize < sizeof(DWORD) + 1)
                return E_INVALIDARG;
            Tone[0] = ModemId;
            Tone[1] = Args[sizeof(DWORD)];
            QcTrace("dtmf start '%c' on call %u\n", Tone[1], ModemId);
            return QcVoiceSimple(Modem, QCRIL_VOICE_START_CONT_DTMF, QCRIL_VOICE_TLV_DTMF, Tone, sizeof(Tone));

        case RilCmdStopDtmf:
            QcTrace("dtmf stop on call %u\n", ModemId);
            return QcVoiceSimple(Modem, QCRIL_VOICE_STOP_CONT_DTMF, QCRIL_VOICE_TLV_CALL_ID, &ModemId, sizeof(ModemId));

        default:
            break;
    }

    if (InSize < sizeof(DWORD) + 256)
        return E_INVALIDARG;
    if (InSize >= sizeof(DWORD) + 256 + 2 * sizeof(DWORD))
    {
        memcpy(&OnMs, &Args[sizeof(DWORD) + 256], sizeof(DWORD));
        memcpy(&OffMs, &Args[sizeof(DWORD) + 256 + sizeof(DWORD)], sizeof(DWORD));
    }

    /* Each digit is a start and a stop */
    for (i = 0; i < 256 && Args[sizeof(DWORD) + i]; i++)
    {
        Tone[0] = ModemId;
        Tone[1] = Args[sizeof(DWORD) + i];
        QcTrace("dtmf '%c' on call %u, %lu ms on %lu ms off\n", Tone[1], ModemId, OnMs, OffMs);

        hr = QcVoiceSimple(Modem, QCRIL_VOICE_START_CONT_DTMF, QCRIL_VOICE_TLV_DTMF, Tone, sizeof(Tone));
        if (FAILED(hr))
            return hr;
        if (OnMs)
            Sleep(OnMs);

        hr = QcVoiceSimple(Modem, QCRIL_VOICE_STOP_CONT_DTMF, QCRIL_VOICE_TLV_CALL_ID, &ModemId, sizeof(ModemId));
        if (FAILED(hr))
            return hr;
        if (OffMs)
            Sleep(OffMs);
    }

    return S_OK;
}

/* The modem's reason for ending one call, from the call_id and reason array */
static
BOOL
QcVoiceFindEndReason(
    _In_reads_bytes_(Length) const UCHAR *Array,
    _In_ USHORT Length,
    _In_ UCHAR CallId,
    _Out_ PUSHORT Reason)
{
    ULONG Offset;

    *Reason = 0;
    if (Length < 1)
        return FALSE;

    for (Offset = 1; Offset + 3 <= Length; Offset += 3)
    {
        if (Array[Offset] == CallId)
        {
            *Reason = Array[Offset + 1] | (Array[Offset + 2] << 8);
            return TRUE;
        }
    }

    return FALSE;
}

/* The alerting type of one call from the call_id and alerting array */
static
BOOL
QcVoiceFindAlerting(
    _In_reads_bytes_(Length) const UCHAR *Array,
    _In_ USHORT Length,
    _In_ UCHAR CallId,
    _Out_ PUCHAR Alerting)
{
    ULONG Offset;

    *Alerting = 0;
    for (Offset = 1; Offset + 2 <= Length; Offset += 2)
    {
        if (Array[Offset] == CallId)
        {
            *Alerting = Array[Offset + 1];
            return TRUE;
        }
    }

    return FALSE;
}

/* Finds the remote number record for one call in the remote party array */
static
BOOL
QcVoiceFindNumber(
    _In_reads_bytes_(Length) const UCHAR *Array,
    _In_ USHORT Length,
    _In_ UCHAR CallId,
    _Out_writes_z_(OutMax) PSTR Out,
    _In_ ULONG OutMax)
{
    ULONG Offset = 1, i;
    UCHAR Count, NumberLen;

    Out[0] = '\0';
    if (Length < 1)
        return FALSE;

    Count = Array[0];
    for (i = 0; i < Count; i++)
    {
        if (Offset + 3 > Length)
            break;

        /* call_id, presentation, number_len, number[] */
        NumberLen = Array[Offset + 2];
        if (Offset + 3 + NumberLen > Length)
            break;

        if (Array[Offset] == CallId)
        {
            ULONG Copy = min(NumberLen, OutMax - 1);
            memcpy(Out, &Array[Offset + 3], Copy);
            Out[Copy] = '\0';
            return TRUE;
        }

        Offset += 3 + NumberLen;
    }

    return FALSE;
}

static
VOID
QcVoiceRingback(
    _In_ PQC_MODEM Modem,
    _In_ BOOL Active)
{
    QCRIL_RINGBACK Ringback;

    ZeroMemory(&Ringback, sizeof(Ringback));
    Ringback.Size = sizeof(Ringback);
    Ringback.Params = 1;
    Ringback.Executor = QC_EXECUTOR;
    Ringback.Active = Active;

    QcTrace("ringback %s\n", Active ? "on" : "off");
    QcNotifyAll(Modem, RIL_NOTIFY_RINGBACK, &Ringback, sizeof(Ringback));
}

VOID
QcVoiceIndication(
    _In_ PQC_MODEM Modem,
    _In_ USHORT MessageId,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    const VOID *Info, *Numbers = NULL, *Ends = NULL, *Alerts = NULL;
    USHORT InfoLen, NumbersLen = 0, EndsLen = 0, AlertsLen = 0;
    const UCHAR *Bytes;
    UCHAR Count, i;
    ULONG Offset = 1;

    if (MessageId != QCRIL_VOICE_ALL_CALL_STATUS_IND)
    {
        QcTrace("ind VOICE 0x%04X not handled\n", MessageId);
        return;
    }

    if (!QmiFindTlv(Message, Length, QCRIL_VOICE_TLV_CALL_INFO, &Info, &InfoLen) || InfoLen < 1)
        return;

    QmiFindTlv(Message, Length, QCRIL_VOICE_TLV_REMOTE_NUMBER, &Numbers, &NumbersLen);
    QmiFindTlv(Message, Length, QCRIL_VOICE_TLV_END_REASON, &Ends, &EndsLen);
    QmiFindTlv(Message, Length, QCRIL_VOICE_TLV_ALERTING, &Alerts, &AlertsLen);

    Bytes = Info;
    Count = Bytes[0];

    for (i = 0; i < Count; i++)
    {
        QCRIL_CALL_INFO Call;
        RIL_CALL_RECORD Record;
        CHAR Number[96];
        USHORT EndReason = 0;
        UCHAR Alerting, PreviousState;
        DWORD Previous, Status;
        PQC_CALL Entry;
        BOOL Report, Ended;

        if (Offset + sizeof(Call) > InfoLen)
            break;

        memcpy(&Call, &Bytes[Offset], sizeof(Call));
        Offset += sizeof(Call);

        EnterCriticalSection(&Modem->StateLock);
        Entry = QcVoiceFind(Modem, Call.CallId, Call.CallState != QCRIL_CALL_END);
        if (!Entry)
        {
            LeaveCriticalSection(&Modem->StateLock);
            QcTrace("call state %u for untracked modem call %u\n", Call.CallState, Call.CallId);
            continue;
        }

        Previous = Entry->Status;
        PreviousState = Entry->QmiState;
        Status = QcVoiceMapState(Call.CallState, Previous);
        Entry->QmiState = Call.CallState;
        Entry->Direction = (Call.Direction == QCRIL_VOICE_DIR_MT) ? RIL_CALLDIR_INCOMING : RIL_CALLDIR_OUTGOING;

        /* Transitions that carry nothing new for the client are not reported */
        Report = Status != Previous &&
                 Call.CallState != QCRIL_CALL_DISCONNECTING &&
                 Call.CallState != QCRIL_CALL_SETUP &&
                 !(PreviousState == QCRIL_CALL_WAITING && Call.CallState == QCRIL_CALL_INCOMING) &&
                 !(PreviousState == QCRIL_CALL_ORIGINATING && Call.CallState == QCRIL_CALL_ALERTING);
        Entry->Status = Status;

        ZeroMemory(&Record, sizeof(Record));
        Record.Size = sizeof(Record);
        Record.Executor = QC_EXECUTOR;
        Record.CallId = Entry->RilId;
        Record.Status = Status;
        Record.Direction = Entry->Direction;
        Record.Domain = RIL_CALL_DOMAIN_CS;
        Record.Multiparty = Call.IsMultiparty;

        Ended = Call.CallState == QCRIL_CALL_END;
        if (Ended)
            Entry->Used = FALSE;
        LeaveCriticalSection(&Modem->StateLock);

        Number[0] = '\0';
        if (Numbers)
            QcVoiceFindNumber(Numbers, NumbersLen, Call.CallId, Number, sizeof(Number));

        if (Ends && QcVoiceFindEndReason(Ends, EndsLen, Call.CallId, &EndReason))
            Record.DisconnectDetail[0] = EndReason;

        QcTrace("call %lu (modem %u): qmi state %u -> %u, status %lu -> %lu%s, end reason %u '%s'\n",
                Record.CallId, Call.CallId, PreviousState, Call.CallState, Previous, Status,
                Report ? "" : " (not reported)", EndReason, Number);

        /* Ringback follows the alerting type of an outgoing call */
        if (Alerts && Call.CallState == QCRIL_CALL_ALERTING &&
            QcVoiceFindAlerting(Alerts, AlertsLen, Call.CallId, &Alerting))
        {
            QcVoiceRingback(Modem, Alerting == QCRIL_ALERTING_LOCAL);
        }
        else if (PreviousState == QCRIL_CALL_ALERTING && Call.CallState != QCRIL_CALL_ALERTING)
        {
            QcVoiceRingback(Modem, FALSE);
        }

        if (!Report)
            continue;

        if (Number[0])
        {
            ULONG j;

            Record.Remote.cbSize = sizeof(Record.Remote);
            Record.Remote.dwParams = RILADDRESS_PARAM_TYPE | RILADDRESS_PARAM_ADDRESS;
            Record.Remote.dwType = (Number[0] == '+') ?
                                   RILADDRESSTYPE_INTERNATIONAL : RILADDRESSTYPE_UNKNOWN;

            for (j = 0; Number[j] && j + 1 < MAXLENGTH_ADDRESS; j++)
                Record.Remote.wszAddress[j] = (WCHAR)Number[j];
            Record.Remote.wszAddress[j] = UNICODE_NULL;
        }

        QcNotifyAll(Modem, RIL_NOTIFY_CALLSTATE, &Record, sizeof(Record));
    }
}
