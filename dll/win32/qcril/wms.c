/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Wireless messaging service, SMS submit and delivery
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcril.h"
#include <string.h>

#define QCRIL_WMS_SET_EVENT_REPORT      0x0001
#define QCRIL_WMS_EVENT_REPORT_IND      0x0001
#define QCRIL_WMS_RAW_SEND              0x0020
#define QCRIL_WMS_SET_ROUTES            0x0032

#define QCRIL_WMS_TLV_RAW_MESSAGE       0x01
#define QCRIL_WMS_TLV_REPORT_MT         0x10
#define QCRIL_WMS_TLV_ROUTE_LIST        0x01
#define QCRIL_WMS_TLV_TRANSFER_ROUTE    0x11

/* wms_message_format_enum_v01, GSM/WCDMA point to point */
#define QCRIL_WMS_FORMAT_GW_PP          0x06

/* wms_receipt_action, transfer the message to the client without storing it */
#define QCRIL_WMS_ACTION_TRANSFER_ONLY  0x02
#define QCRIL_WMS_ROUTE_STORAGE_NONE    0xFF

#define QCRIL_EXECUTOR                  0

/* SMS-SUBMIT first octet: MTI submit, relative validity period */
#define QCRIL_TP_SUBMIT                 0x11
#define QCRIL_TP_VP_RELATIVE            0xAA
#define QCRIL_TP_UDHI                   0x40

static
UCHAR
QcBcd(
    _In_ UCHAR Byte)
{
    return (UCHAR)((Byte & 0x0F) * 10 + (Byte >> 4));
}

/* Packs 7 bit codes into octets, returns the octet count */
static
ULONG
QcGsm7Pack(
    _In_reads_(Count) const BYTE *Codes,
    _In_ ULONG Count,
    _Out_writes_(OutMax) PBYTE Out,
    _In_ ULONG OutMax)
{
    ULONG i, Length = 0, Bits = 0;
    ULONG Accumulator = 0;

    for (i = 0; i < Count; i++)
    {
        Accumulator |= (ULONG)(Codes[i] & 0x7F) << Bits;
        Bits += 7;
        while (Bits >= 8 && Length < OutMax)
        {
            Out[Length++] = (BYTE)(Accumulator & 0xFF);
            Accumulator >>= 8;
            Bits -= 8;
        }
    }

    if (Bits > 0 && Length < OutMax)
        Out[Length++] = (BYTE)(Accumulator & 0xFF);

    return Length;
}

/* Unpacks Count 7 bit codes starting BitOffset bits into the packed data */
static
VOID
QcGsm7Unpack(
    _In_reads_bytes_(PackedLen) const BYTE *Packed,
    _In_ ULONG PackedLen,
    _In_ ULONG Count,
    _In_ ULONG BitOffset,
    _Out_writes_(Count) PBYTE Out)
{
    ULONG i;

    for (i = 0; i < Count; i++)
    {
        ULONG Bit = BitOffset + i * 7;
        ULONG Index = Bit / 8;
        ULONG Shift = Bit % 8;
        ULONG Value = 0;

        if (Index < PackedLen)
            Value = Packed[Index] >> Shift;
        if (Shift > 1 && Index + 1 < PackedLen)
            Value |= (ULONG)Packed[Index + 1] << (8 - Shift);

        Out[i] = (BYTE)(Value & 0x7F);
    }
}

/* Encodes a dialled number as a TP address, returns total bytes written */
static
ULONG
QcEncodeAddress(
    _In_ PCWSTR Digits,
    _In_ UCHAR Type,
    _Out_writes_(OutMax) PBYTE Out,
    _In_ ULONG OutMax)
{
    ULONG Count = 0, i;
    BYTE Nibbles[32];

    for (i = 0; Digits[i] && Count < sizeof(Nibbles); i++)
    {
        WCHAR Ch = Digits[i];
        if (Ch == L'+')
            continue;
        if (Ch >= L'0' && Ch <= L'9')
            Nibbles[Count++] = (BYTE)(Ch - L'0');
        else if (Ch == L'*')
            Nibbles[Count++] = 0x0A;
        else if (Ch == L'#')
            Nibbles[Count++] = 0x0B;
    }

    if (OutMax < 2 + (Count + 1) / 2)
        return 0;

    Out[0] = (BYTE)Count;
    Out[1] = Type;
    for (i = 0; i < Count; i += 2)
    {
        BYTE Low = Nibbles[i];
        BYTE High = (i + 1 < Count) ? Nibbles[i + 1] : 0x0F;
        Out[2 + i / 2] = (BYTE)((High << 4) | Low);
    }

    return 2 + (Count + 1) / 2;
}

/* Decodes a TP address field, returns the number of octets consumed */
static
ULONG
QcDecodeAddress(
    _In_reads_bytes_(InLen) const BYTE *In,
    _In_ ULONG InLen,
    _Out_ PUCHAR Type,
    _Out_writes_z_(OutMax) PWSTR Out,
    _In_ ULONG OutMax)
{
    ULONG DigitCount, Octets, i, Written = 0;

    Out[0] = UNICODE_NULL;
    if (InLen < 2)
        return InLen;

    DigitCount = In[0];
    *Type = In[1];
    Octets = (DigitCount + 1) / 2;
    if (2 + Octets > InLen)
        Octets = InLen - 2;

    for (i = 0; i < Octets && Written + 2 < OutMax; i++)
    {
        BYTE Byte = In[2 + i];
        BYTE Low = Byte & 0x0F;
        BYTE High = Byte >> 4;

        if (Written < DigitCount)
            Out[Written++] = (Low <= 9) ? (WCHAR)(L'0' + Low) : (Low == 0x0A ? L'*' : L'#');
        if (Written < DigitCount)
            Out[Written++] = (High <= 9) ? (WCHAR)(L'0' + High) : (High == 0x0A ? L'*' : L'#');
    }

    Out[Written] = UNICODE_NULL;
    return 2 + Octets;
}

/* Builds a single segment SMS-SUBMIT PDU, returns the PDU length or zero */
static
ULONG
QcBuildSubmitPdu(
    _In_ const RIL_MESSAGE *Message,
    _Out_writes_(OutMax) PBYTE Pdu,
    _In_ ULONG OutMax)
{
    ULONG Offset = 0, AddrLen, i;
    UCHAR Type;
    DWORD Alphabet = RIL_ALPHABET_GSM7;

    if (OutMax < 16)
        return 0;

    if ((Message->Params & RIL_MSGPARAM_DCS) && (Message->Dcs.Params & RIL_DCSPARAM_ALPHABET))
        Alphabet = Message->Dcs.Alphabet;

    Type = (Message->Address.dwType == RILADDRESSTYPE_INTERNATIONAL) ? 0x91 : 0x81;

    Pdu[Offset++] = 0x00;            /* use the default service centre */
    Pdu[Offset++] = QCRIL_TP_SUBMIT;
    Pdu[Offset++] = 0x00;            /* message reference */

    AddrLen = QcEncodeAddress(Message->Address.wszAddress, Type, Pdu + Offset, OutMax - Offset);
    if (AddrLen == 0)
        return 0;
    Offset += AddrLen;

    if (Offset + 3 > OutMax)
        return 0;

    Pdu[Offset++] = 0x00;            /* protocol id */
    Pdu[Offset++] = (Alphabet == RIL_ALPHABET_UCS2) ? 0x08 : 0x00;  /* data coding */
    Pdu[Offset++] = QCRIL_TP_VP_RELATIVE;

    if (Alphabet == RIL_ALPHABET_UCS2)
    {
        ULONG Chars = min(Message->u.Submit.BodyLength, RIL_MESSAGE_BODY_MAX / sizeof(WCHAR));
        const WCHAR *Body = (const WCHAR *)Message->u.Submit.Body;

        if (Offset + 1 + Chars * 2 > OutMax)
            return 0;

        Pdu[Offset++] = (BYTE)(Chars * 2);
        for (i = 0; i < Chars; i++)
        {
            Pdu[Offset++] = (BYTE)(Body[i] >> 8);
            Pdu[Offset++] = (BYTE)(Body[i] & 0xFF);
        }
    }
    else
    {
        ULONG Septets = min(Message->u.Submit.BodyLength, RIL_MESSAGE_BODY_MAX);
        ULONG Packed;

        if (Offset + 1 > OutMax)
            return 0;

        Pdu[Offset++] = (BYTE)Septets;
        Packed = QcGsm7Pack(Message->u.Submit.Body, Septets, Pdu + Offset, OutMax - Offset);
        Offset += Packed;
    }

    return Offset;
}

/* Parses a received SMS-DELIVER PDU into a RIL message */
static
BOOL
QcParseDeliverPdu(
    _In_reads_bytes_(PduLen) const BYTE *Pdu,
    _In_ ULONG PduLen,
    _Out_ PRIL_MESSAGE Message)
{
    ULONG Offset = 0, AddrLen, i;
    UCHAR Flags, Type, Dcs, Udl;
    DWORD Alphabet;
    const BYTE *Ud;
    ULONG UdhOctets = 0;

    ZeroMemory(Message, sizeof(*Message));
    Message->Size = sizeof(*Message);
    Message->Type = RIL_MSGTYPE_GW_DELIVER;
    Message->Params = RIL_MSGPARAM_TYPE | RIL_MSGPARAM_ADDRESS | RIL_MSGPARAM_DCS | RIL_MSGPARAM_BODY;

    if (PduLen < 1)
        return FALSE;

    /* Service centre field, skip it */
    Offset += 1 + Pdu[0];
    if (Offset + 1 > PduLen)
        return FALSE;

    Flags = Pdu[Offset++];

    if (Offset + 1 > PduLen)
        return FALSE;

    Message->Address.cbSize = sizeof(Message->Address);
    Message->Address.dwParams = RILADDRESS_PARAM_TYPE | RILADDRESS_PARAM_ADDRESS;
    AddrLen = QcDecodeAddress(Pdu + Offset, PduLen - Offset, &Type,
                              Message->Address.wszAddress, MAXLENGTH_ADDRESS);
    Offset += AddrLen;

    switch ((Type >> 4) & 0x07)
    {
        case 1: Message->Address.dwType = RILADDRESSTYPE_INTERNATIONAL; break;
        case 2: Message->Address.dwType = RILADDRESSTYPE_NATIONAL; break;
        default: Message->Address.dwType = RILADDRESSTYPE_UNKNOWN; break;
    }

    if (Offset + 2 > PduLen)
        return FALSE;

    Offset++;                        /* protocol id */
    Dcs = Pdu[Offset++];

    /* Timestamp, seven semi octet bytes */
    if (Offset + 7 <= PduLen)
    {
        SYSTEMTIME *Time = &Message->u.Deliver.Timestamp;
        Time->wYear = (WORD)(2000 + QcBcd(Pdu[Offset]));
        Time->wMonth = QcBcd(Pdu[Offset + 1]);
        Time->wDay = QcBcd(Pdu[Offset + 2]);
        Time->wHour = QcBcd(Pdu[Offset + 3]);
        Time->wMinute = QcBcd(Pdu[Offset + 4]);
        Time->wSecond = QcBcd(Pdu[Offset + 5]);
        Message->Params |= RIL_MSGPARAM_TIMESTAMP;
        Offset += 7;
    }

    if (Offset + 1 > PduLen)
        return FALSE;

    Udl = Pdu[Offset++];
    Ud = Pdu + Offset;

    if ((Dcs & 0xC0) == 0)
        Alphabet = (Dcs >> 2) & 0x03;   /* 0 GSM7, 1 8 bit, 2 UCS2 */
    else
        Alphabet = 0;

    Message->Dcs.Size = sizeof(Message->Dcs);
    Message->Dcs.Params = RIL_DCSPARAM_ALPHABET;

    if (Flags & QCRIL_TP_UDHI)
    {
        if (Offset >= PduLen)
            return FALSE;

        UdhOctets = (ULONG)Ud[0] + 1;
        if (UdhOctets > PduLen - Offset)
            return FALSE;

        Message->Params |= RIL_MSGPARAM_HEADER;
        Message->u.Deliver.HeaderLength = UdhOctets;
        memcpy(Message->u.Deliver.Header, Ud, min(UdhOctets, RIL_MESSAGE_HEADER_MAX));
    }

    if (Alphabet == 2)
    {
        ULONG Bytes = min(Udl, PduLen - Offset);
        ULONG Chars;

        Bytes = (Bytes > UdhOctets) ? Bytes - UdhOctets : 0;
        Chars = min(Bytes / 2, RIL_MESSAGE_BODY_MAX / sizeof(WCHAR));

        Message->Dcs.Alphabet = RIL_ALPHABET_UCS2;
        for (i = 0; i < Chars; i++)
        {
            const BYTE *Pair = Ud + UdhOctets + i * 2;
            ((PWCHAR)Message->u.Deliver.Body)[i] = (WCHAR)((Pair[0] << 8) | Pair[1]);
        }
        Message->u.Deliver.BodyLength = Chars;
    }
    else if (Alphabet == 1)
    {
        ULONG Bytes = min(Udl, PduLen - Offset);

        Bytes = (Bytes > UdhOctets) ? Bytes - UdhOctets : 0;
        Bytes = min(Bytes, RIL_MESSAGE_BODY_MAX);

        Message->Dcs.Alphabet = RIL_ALPHABET_8BIT;
        memcpy(Message->u.Deliver.Body, Ud + UdhOctets, Bytes);
        Message->u.Deliver.BodyLength = Bytes;
    }
    else
    {
        ULONG UdhSeptets = (UdhOctets * 8 + 6) / 7;
        ULONG TextSeptets = (Udl > UdhSeptets) ? Udl - UdhSeptets : 0;

        TextSeptets = min(TextSeptets, RIL_MESSAGE_BODY_MAX);
        Message->Dcs.Alphabet = RIL_ALPHABET_GSM7;
        QcGsm7Unpack(Ud, PduLen - Offset, TextSeptets, UdhSeptets * 7, Message->u.Deliver.Body);
        Message->u.Deliver.BodyLength = TextSeptets;
    }

    return TRUE;
}

HRESULT
QcWmsSend(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(InSize) const VOID *In,
    _In_ ULONG InSize,
    _Inout_ PQC_RESP Resp)
{
    const RIL_MESSAGE *Message;
    QMI_ENCODER Encoder;
    BYTE Pdu[400];
    BYTE Record[420];
    BYTE Request[512];
    BYTE Response[128];
    ULONG PduLen, RecordLen, Length, ResponseLen;
    const VOID *Value;
    USHORT ValueLen, PduLen16;
    HRESULT hr;

    UNREFERENCED_PARAMETER(Resp);

    if (InSize < 8 + sizeof(RIL_MESSAGE))
        return E_INVALIDARG;

    Message = (const RIL_MESSAGE *)((const BYTE *)In + 8);

    PduLen = QcBuildSubmitPdu(Message, Pdu, sizeof(Pdu));
    if (PduLen == 0)
        return E_INVALIDARG;

    /* raw_message_data: format byte, u16 length, then the PDU */
    PduLen16 = (USHORT)PduLen;
    Record[0] = QCRIL_WMS_FORMAT_GW_PP;
    memcpy(Record + 1, &PduLen16, sizeof(PduLen16));
    memcpy(Record + 3, Pdu, PduLen);
    RecordLen = 3 + PduLen;

    QmiEncodeBegin(&Encoder, Request, sizeof(Request), QMI_TYPE_REQUEST, 0, QCRIL_WMS_RAW_SEND);
    if (!QmiEncodeTlv(&Encoder, QCRIL_WMS_TLV_RAW_MESSAGE, Record, (USHORT)RecordLen))
        return E_FAIL;
    Length = QmiEncodeEnd(&Encoder);

    hr = QcQmiRequest(Modem, QcSvcWms, Request, Length, Response, sizeof(Response), &ResponseLen);
    if (FAILED(hr))
        return hr;

    /* Hand back the network message id when the modem supplies one */
    if (QmiFindTlv(Response, ResponseLen, QCRIL_WMS_TLV_RAW_MESSAGE, &Value, &ValueLen) &&
        ValueLen >= sizeof(USHORT))
    {
        USHORT MessageId;
        memcpy(&MessageId, Value, sizeof(MessageId));
        QcRespDword(Resp, MessageId);
    }

    return S_OK;
}

VOID
QcWmsConfigureRoutes(
    _In_ PQC_MODEM Modem)
{
    QMI_ENCODER Encoder;
    BYTE Message[64];
    BYTE Route[6];
    USHORT Count = 1;
    UCHAR Enable = 1;
    ULONG Length;

    if (!QcServiceEnsure(Modem, QcSvcWms))
        return;

    /* One tuple: point to point, any class, do not store, transfer to us */
    memcpy(Route, &Count, sizeof(Count));
    Route[2] = 0x00;                             /* message type point to point */
    Route[3] = 0x04;                             /* message class none */
    Route[4] = QCRIL_WMS_ROUTE_STORAGE_NONE;
    Route[5] = QCRIL_WMS_ACTION_TRANSFER_ONLY;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WMS_SET_ROUTES);
    QmiEncodeTlv(&Encoder, QCRIL_WMS_TLV_ROUTE_LIST, Route, sizeof(Route));
    Length = QmiEncodeEnd(&Encoder);
    QcQmiCommand(Modem, QcSvcWms, Message, Length);

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WMS_SET_EVENT_REPORT);
    QmiEncodeTlv(&Encoder, QCRIL_WMS_TLV_REPORT_MT, &Enable, sizeof(Enable));
    Length = QmiEncodeEnd(&Encoder);
    QcQmiCommand(Modem, QcSvcWms, Message, Length);
}

VOID
QcWmsIndication(
    _In_ PQC_MODEM Modem,
    _In_ USHORT MessageId,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    const UCHAR *Route;
    const VOID *Value;
    USHORT ValueLen;
    RIL_MESSAGE_NOTIFICATION *Notification;
    USHORT DataLen;
    ULONG TransactionId;

    if (MessageId != QCRIL_WMS_EVENT_REPORT_IND)
        return;

    /* Only the direct transfer route carries the message bytes */
    if (!QmiFindTlv(Message, Length, QCRIL_WMS_TLV_TRANSFER_ROUTE, &Value, &ValueLen) || ValueLen < 8)
        return;

    Route = Value;
    /* ack_indicator u8, transaction_id u32, format u8, data_len u16, data[] */
    memcpy(&TransactionId, Route + 1, sizeof(TransactionId));
    memcpy(&DataLen, Route + 6, sizeof(DataLen));
    if (Route[5] != QCRIL_WMS_FORMAT_GW_PP || (ULONG)8 + DataLen > ValueLen)
        return;

    Notification = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Notification));
    if (!Notification)
        return;

    Notification->Size = sizeof(*Notification);
    Notification->Executor = QCRIL_EXECUTOR;
    Notification->AckId = TransactionId;
    Notification->OverIms = 0;

    if (QcParseDeliverPdu(Route + 8, DataLen, &Notification->Message))
        QcNotifyAll(Modem, RIL_NOTIFY_MESSAGE, Notification, sizeof(*Notification));

    HeapFree(GetProcessHeap(), 0, Notification);
}
