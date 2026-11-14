/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Voice service, call control and call state reporting
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcril.h"
#include <string.h>

#define QCRIL_VOICE_DIAL_CALL           0x0020
#define QCRIL_VOICE_END_CALL            0x0021
#define QCRIL_VOICE_ANSWER_CALL         0x0022
#define QCRIL_VOICE_ALL_CALL_STATUS_IND 0x002E
#define QCRIL_VOICE_GET_ALL_CALL_INFO   0x002F

#define QCRIL_VOICE_TLV_CALLING_NUMBER  0x01
#define QCRIL_VOICE_TLV_CALL_ID         0x01
#define QCRIL_VOICE_TLV_DIAL_CALL_ID    0x10
#define QCRIL_VOICE_TLV_CALL_INFO       0x01
#define QCRIL_VOICE_TLV_REMOTE_NUMBER   0x10

/* The phone service drives a single executor */
#define QCRIL_EXECUTOR                  0

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

/* The dialed address lives in the RILADDRESS at offset 4 of the dial block */
#include <pshpack4.h>
typedef struct _QCRIL_DIAL_IN
{
    DWORD Executor;
    RILADDRESS Address;
} QCRIL_DIAL_IN;

typedef struct _QCRIL_MANAGE_IN
{
    DWORD Executor;
    DWORD Action;
    DWORD CallId;
} QCRIL_MANAGE_IN;
#include <poppack.h>

static
ULONG
QcWideToAscii(
    _In_ PCWSTR Wide,
    _In_ ULONG MaxChars,
    _Out_writes_(OutMax) PSTR Out,
    _In_ ULONG OutMax)
{
    ULONG i;

    for (i = 0; i < MaxChars && i + 1 < OutMax && Wide[i]; i++)
        Out[i] = (CHAR)Wide[i];

    Out[i] = '\0';
    return i;
}

static
DWORD
QcVoiceMapState(
    _In_ UCHAR CallState)
{
    switch (CallState)
    {
        case 0x02: /* INCOMING */
        case 0x07: /* WAITING */
            return RIL_CALLSTATUS_INCOMING;
        case 0x03: /* CONVERSATION */
            return RIL_CALLSTATUS_CONNECTED;
        case 0x06: /* HOLD */
            return RIL_CALLSTATUS_HELD;
        case 0x08: /* DISCONNECTING */
        case 0x09: /* END */
            return RIL_CALLSTATUS_DISCONNECTED;
        case 0x01: /* ORIGINATION */
        case 0x04: /* CC_IN_PROGRESS */
        case 0x05: /* ALERTING */
        case 0x0A: /* SETUP */
            return RIL_CALLSTATUS_OUTGOING;
        default:
            return RIL_CALLSTATUS_IDLE;
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
    BYTE Message[128];
    BYTE Response[128];
    CHAR Number[96];
    ULONG Digits, Offset = 0, Length, ResponseLen;
    const VOID *Value;
    USHORT ValueLen;
    HRESULT hr;

    if (InSize < sizeof(QCRIL_DIAL_IN))
        return E_INVALIDARG;

    if ((Dial->Address.dwType == RILADDRESSTYPE_INTERNATIONAL) &&
        Dial->Address.wszAddress[0] != L'+')
    {
        Number[0] = '+';
        Offset = 1;
    }

    Digits = QcWideToAscii(Dial->Address.wszAddress, MAXLENGTH_ADDRESS,
                           Number + Offset, sizeof(Number) - Offset);
    Digits += Offset;
    if (Digits == 0)
        return RIL_E_INVALIDDIALSTRING;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_VOICE_DIAL_CALL);
    if (!QmiEncodeTlv(&Encoder, QCRIL_VOICE_TLV_CALLING_NUMBER, Number, (USHORT)Digits))
        return E_FAIL;
    Length = QmiEncodeEnd(&Encoder);

    hr = QcQmiRequest(Modem, QcSvcVoice, Message, Length, Response, sizeof(Response), &ResponseLen);
    if (FAILED(hr))
        return hr;

    /* The answer carries the new call id, the host returns it to the caller */
    if (QmiFindTlv(Response, ResponseLen, QCRIL_VOICE_TLV_DIAL_CALL_ID, &Value, &ValueLen) && ValueLen >= 1)
        QcRespDword(Resp, ((const UCHAR *)Value)[0]);
    else
        QcRespDword(Resp, 0);

    return S_OK;
}

HRESULT
QcVoiceManage(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(InSize) const VOID *In,
    _In_ ULONG InSize)
{
    const QCRIL_MANAGE_IN *Manage = In;
    QMI_ENCODER Encoder;
    BYTE Message[32];
    UCHAR CallId;
    USHORT MessageId;
    ULONG Length;

    if (InSize < sizeof(QCRIL_MANAGE_IN))
        return E_INVALIDARG;

    CallId = (UCHAR)Manage->CallId;

    switch (Manage->Action)
    {
        case RIL_CALLACTION_ANSWER:
            MessageId = QCRIL_VOICE_ANSWER_CALL;
            break;

        case RIL_CALLACTION_RELEASE_CALL:
        case RIL_CALLACTION_HANGUP_OUTGOING:
        case RIL_CALLACTION_RELEASE_ALL:
        case RIL_CALLACTION_RELEASE_HELD:
            MessageId = QCRIL_VOICE_END_CALL;
            break;

        default:
            /* Hold, swap and conference use supplementary flows added later */
            return S_OK;
    }

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, MessageId);
    if (!QmiEncodeTlv(&Encoder, QCRIL_VOICE_TLV_CALL_ID, &CallId, sizeof(CallId)))
        return E_FAIL;
    Length = QmiEncodeEnd(&Encoder);

    return QcQmiCommand(Modem, QcSvcVoice, Message, Length);
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

VOID
QcVoiceIndication(
    _In_ PQC_MODEM Modem,
    _In_ USHORT MessageId,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    const VOID *Info, *Numbers = NULL;
    USHORT InfoLen, NumbersLen = 0;
    const UCHAR *Bytes;
    UCHAR Count, i;
    ULONG Offset = 1;

    if (MessageId != QCRIL_VOICE_ALL_CALL_STATUS_IND)
        return;

    if (!QmiFindTlv(Message, Length, QCRIL_VOICE_TLV_CALL_INFO, &Info, &InfoLen) || InfoLen < 1)
        return;

    QmiFindTlv(Message, Length, QCRIL_VOICE_TLV_REMOTE_NUMBER, &Numbers, &NumbersLen);

    Bytes = Info;
    Count = Bytes[0];

    for (i = 0; i < Count; i++)
    {
        QCRIL_CALL_INFO Call;
        RIL_CALL_RECORD Record;
        CHAR Number[96];

        if (Offset + sizeof(Call) > InfoLen)
            break;

        memcpy(&Call, &Bytes[Offset], sizeof(Call));
        Offset += sizeof(Call);

        ZeroMemory(&Record, sizeof(Record));
        Record.Size = sizeof(Record);
        Record.Executor = QCRIL_EXECUTOR;
        Record.CallId = Call.CallId;
        Record.Status = QcVoiceMapState(Call.CallState);
        Record.Direction = (Call.Direction == QCRIL_VOICE_DIR_MT) ?
                           RIL_CALLDIR_INCOMING : RIL_CALLDIR_OUTGOING;
        Record.Domain = RIL_CALL_DOMAIN_CS;
        Record.Multiparty = Call.IsMultiparty;

        Number[0] = '\0';
        if (Numbers)
            QcVoiceFindNumber(Numbers, NumbersLen, Call.CallId, Number, sizeof(Number));

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

VOID
QcVoiceSeed(
    _In_ PQC_MODEM Modem)
{
    QMI_ENCODER Encoder;
    BYTE Message[16];
    BYTE Response[256];
    ULONG Length, ResponseLen;

    if (!QcServiceEnsure(Modem, QcSvcVoice))
        return;

    /* Querying the call list registers this port for status indications */
    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_VOICE_GET_ALL_CALL_INFO);
    Length = QmiEncodeEnd(&Encoder);
    QcQmiRequest(Modem, QcSvcVoice, Message, Length, Response, sizeof(Response), &ResponseLen);
}
