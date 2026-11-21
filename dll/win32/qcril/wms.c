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
#define QCRIL_WMS_SEND_ACK              0x0037
#define QCRIL_WMS_SET_PRIMARY_CLIENT    0x0045
#define QCRIL_WMS_INDICATION_REGISTER   0x0047
#define QCRIL_WMS_BIND_SUBSCRIPTION     0x004C
#define QCRIL_WMS_SMSC_ADDRESS_IND      0x0046
#define QCRIL_WMS_SERVICE_READY_IND     0x005D

#define QCRIL_WMS_TLV_RAW_MESSAGE       0x01
#define QCRIL_WMS_TLV_REPORT_MT         0x10
#define QCRIL_WMS_TLV_REPORT_MWI        0x12
#define QCRIL_WMS_TLV_MT_MESSAGE        0x10
#define QCRIL_WMS_TLV_ROUTE_LIST        0x01
#define QCRIL_WMS_TLV_TRANSFER_ROUTE    0x11
#define QCRIL_WMS_TLV_ACK_INFO          0x01
#define QCRIL_WMS_TLV_ACK_3GPP_FAILURE  0x11
#define QCRIL_WMS_TLV_ACK_OVER_IMS      0x12

/* Ack protocol and the RP/TP causes reported when a message is refused */
#define QCRIL_WMS_PROTOCOL_3GPP         0x01
#define QCRIL_RP_MEMORY_EXCEEDED        0x16
#define QCRIL_RP_PROTOCOL_ERROR         0x6F
#define QCRIL_TP_MEMORY_EXCEEDED        0xD3
#define QCRIL_TP_UNSPECIFIED            0xB0

/* wms_message_format_enum_v01, GSM/WCDMA point to point */
#define QCRIL_WMS_FORMAT_GW_PP          0x06

/* wms_receipt_action and storage of the routes */
#define QCRIL_WMS_ACTION_STORE_NOTIFY   0x01
#define QCRIL_WMS_ACTION_TRANSFER_ONLY  0x02
#define QCRIL_WMS_ROUTE_STORAGE_UIM     0x00
#define QCRIL_WMS_ROUTE_STORAGE_NONE    0xFF

/* wms_message_class_enum_v01, NONE and CDMA follow the GSM classes */
#define QCRIL_WMS_CLASS_SIM             0x02
#define QCRIL_WMS_CLASS_COUNT           6

#define QCRIL_EXECUTOR                  QC_EXECUTOR

/* SMS-SUBMIT first octet and the defaults of its fields */
#define QCRIL_TP_SUBMIT                 0x01
#define QCRIL_TP_REJECT_DUPS            0x04
#define QCRIL_TP_VPF_NONE               0x00
#define QCRIL_TP_VPF_ENHANCED           0x08
#define QCRIL_TP_VPF_RELATIVE           0x10
#define QCRIL_TP_VPF_ABSOLUTE           0x18
#define QCRIL_TP_STATUS_REPORT          0x20
#define QCRIL_TP_UDHI                   0x40
#define QCRIL_TP_REPLY_PATH             0x80
#define QCRIL_TP_VP_DEFAULT             0xAA

/* RIL_MESSAGE.u.Submit.ValidityFormat */
#define QCRIL_VP_NONE                   0
#define QCRIL_VP_RELATIVE               1
#define QCRIL_VP_ENHANCED               2
#define QCRIL_VP_ABSOLUTE               3

static
UCHAR
QcBcd(
    _In_ UCHAR Byte)
{
    return (UCHAR)((Byte & 0x0F) * 10 + (Byte >> 4));
}

/* The service centre stamps its local time and its offset from UTC, keep the UTC time */
static
VOID
QcSctsToUtc(
    _Inout_ SYSTEMTIME *Time,
    _In_ UCHAR Zone)
{
    LONGLONG Quarters;
    ULARGE_INTEGER Value;
    FILETIME File;

    /* Quarter hours in swapped BCD, bit 3 of the first digit is the sign */
    Quarters = (Zone & 0x07) * 10 + (Zone >> 4);
    if (Zone & 0x08)
        Quarters = -Quarters;

    if (!Quarters || !SystemTimeToFileTime(Time, &File))
        return;

    Value.LowPart = File.dwLowDateTime;
    Value.HighPart = File.dwHighDateTime;
    Value.QuadPart -= Quarters * 15 * 60 * 10000000LL;
    File.dwLowDateTime = Value.LowPart;
    File.dwHighDateTime = Value.HighPart;
    FileTimeToSystemTime(&File, Time);
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

/* Encodes a dialed number as a TP address, returns total bytes written */
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
    UCHAR Type, First, Vpf;
    DWORD Alphabet = RIL_ALPHABET_GSM7;

    if (OutMax < 16)
        return 0;

    if ((Message->Params & RIL_MSGPARAM_DCS) && (Message->Dcs.Params & RIL_DCSPARAM_ALPHABET))
        Alphabet = Message->Dcs.Alphabet;

    Type = (Message->Address.dwType == RILADDRESSTYPE_INTERNATIONAL) ? 0x91 : 0x81;

    /* The service center the message names, the default one otherwise */
    if ((Message->Params & RIL_MSGPARAM_SERVICECENTER) && Message->ServiceCenter.wszAddress[0])
    {
        UCHAR CenterType = (Message->ServiceCenter.dwType == RILADDRESSTYPE_INTERNATIONAL ||
                            Message->ServiceCenter.wszAddress[0] == L'+') ? 0x91 : 0x81;

        AddrLen = QcEncodeAddress(Message->ServiceCenter.wszAddress, CenterType, Pdu, OutMax);
        if (AddrLen == 0)
            return 0;

        /* The center length counts octets after itself, not digits */
        Pdu[0] = (BYTE)(AddrLen - 1);
        Offset = AddrLen;
    }
    else
    {
        Pdu[Offset++] = 0x00;
    }

    First = QCRIL_TP_SUBMIT;
    if (Message->Params & RIL_MSGPARAM_FLAGS)
    {
        if (Message->Flags & RIL_MSGFLAG_REPLYPATH)
            First |= QCRIL_TP_REPLY_PATH;
        if (Message->Flags & RIL_MSGFLAG_STATUSREPORT)
            First |= QCRIL_TP_STATUS_REPORT;
        if (Message->Flags & RIL_MSGFLAG_REJECTDUPS)
            First |= QCRIL_TP_REJECT_DUPS;
    }

    switch (Message->u.Submit.ValidityFormat)
    {
        case QCRIL_VP_NONE: Vpf = QCRIL_TP_VPF_NONE; break;
        case QCRIL_VP_ENHANCED: Vpf = QCRIL_TP_VPF_ENHANCED; break;
        case QCRIL_VP_ABSOLUTE: Vpf = QCRIL_TP_VPF_ABSOLUTE; break;
        default: Vpf = QCRIL_TP_VPF_RELATIVE; break;
    }

    if (OutMax < Offset + 3)
        return 0;

    Pdu[Offset++] = First | Vpf;
    Pdu[Offset++] = 0x00;            /* message reference */

    AddrLen = QcEncodeAddress(Message->Address.wszAddress, Type, Pdu + Offset, OutMax - Offset);
    if (AddrLen == 0)
        return 0;
    Offset += AddrLen;

    if (Offset + 2 + 7 > OutMax)
        return 0;

    Pdu[Offset++] = (Message->Params & RIL_MSGPARAM_PROTOCOLID) ? (BYTE)Message->ProtocolId : 0x00;
    Pdu[Offset++] = (Alphabet == RIL_ALPHABET_UCS2) ? 0x08 : 0x00;  /* data coding */

    if (Vpf == QCRIL_TP_VPF_RELATIVE)
    {
        Pdu[Offset++] = Message->u.Submit.Validity[0] ? Message->u.Submit.Validity[0] : QCRIL_TP_VP_DEFAULT;
    }
    else if (Vpf != QCRIL_TP_VPF_NONE)
    {
        memcpy(Pdu + Offset, Message->u.Submit.Validity, 7);
        Offset += 7;
    }

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

/* Parses a received SMS-DELIVER TPDU into a RIL message. The modem's transfer route
   carries the TPDU alone, with no service center address in front of it. */
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
        QcSctsToUtc(Time, Pdu[Offset + 6]);
        Message->Params |= RIL_MSGPARAM_TIMESTAMP;
        Offset += 7;
    }

    if (Offset + 1 > PduLen)
        return FALSE;

    Udl = Pdu[Offset++];
    Ud = Pdu + Offset;

    /* 3GPP 23.038: 0 GSM7, 1 8 bit, 2 UCS2. Groups 00xx and 01xx carry the alphabet in bits 2-3,
       1111 picks 8 bit with bit 2, 1110 is UCS2 waiting indication, the rest is GSM7 */
    if ((Dcs & 0x80) == 0)
        Alphabet = (Dcs >> 2) & 0x03;
    else if ((Dcs & 0xF0) == 0xF0)
        Alphabet = (Dcs & 0x04) ? 1 : 0;
    else if ((Dcs & 0xF0) == 0xE0)
        Alphabet = 2;
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

    QcTrace("sms submit: pdu %lu bytes, first octet 0x%02x\n", PduLen, Pdu[1 + Pdu[0]]);
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

/* In: size, executor, reserved, ack id, status, protocol, over IMS, all DWORDs */
HRESULT
QcWmsAck(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(InSize) const VOID *In,
    _In_ ULONG InSize)
{
    const DWORD *Args = In;
    QMI_ENCODER Encoder;
    BYTE Request[64];
    BYTE Response[64];
    BYTE AckInfo[6];
    BYTE Failure[2];
    ULONG Length, ResponseLen;
    DWORD Transaction;
    UCHAR OverIms;
    BOOL Success;

    if (InSize < 7 * sizeof(DWORD))
        return E_INVALIDARG;

    Success = (Args[4] == RIL_MSGACK_OK);
    OverIms = Args[6] ? 1 : 0;

    /* Ack ids are the modem transaction plus one so zero never names a message */
    if (Args[3] == 0)
        return E_INVALIDARG;
    Transaction = Args[3] - 1;
    QcTrace("sms ack: transaction %lu status %lu over ims %u\n", Transaction, Args[4], OverIms);

    /* ack_information: transaction id u32, protocol u8, success u8 */
    memcpy(AckInfo, &Transaction, sizeof(DWORD));
    AckInfo[4] = QCRIL_WMS_PROTOCOL_3GPP;
    AckInfo[5] = Success ? 1 : 0;

    QmiEncodeBegin(&Encoder, Request, sizeof(Request), QMI_TYPE_REQUEST, 0, QCRIL_WMS_SEND_ACK);
    if (!QmiEncodeTlv(&Encoder, QCRIL_WMS_TLV_ACK_INFO, AckInfo, sizeof(AckInfo)))
        return E_FAIL;

    if (!Success)
    {
        Failure[0] = (Args[4] == RIL_MSGACK_MEMORYFULL) ? QCRIL_RP_MEMORY_EXCEEDED : QCRIL_RP_PROTOCOL_ERROR;
        Failure[1] = (Args[4] == RIL_MSGACK_MEMORYFULL) ? QCRIL_TP_MEMORY_EXCEEDED : QCRIL_TP_UNSPECIFIED;
        if (!QmiEncodeTlv(&Encoder, QCRIL_WMS_TLV_ACK_3GPP_FAILURE, Failure, sizeof(Failure)))
            return E_FAIL;
    }

    if (OverIms && !QmiEncodeTlv(&Encoder, QCRIL_WMS_TLV_ACK_OVER_IMS, &OverIms, sizeof(OverIms)))
        return E_FAIL;

    Length = QmiEncodeEnd(&Encoder);
    return QcQmiRequest(Modem, QcSvcWms, Request, Length, Response, sizeof(Response), &ResponseLen);
}

VOID
QcWmsSetup(
    _In_ PQC_MODEM Modem)
{
    static const UCHAR Classes[QCRIL_WMS_CLASS_COUNT] = { 0, 1, 2, 3, 4, 5 };
    QMI_ENCODER Encoder;
    BYTE Message[96];
    BYTE Routes[2 + QCRIL_WMS_CLASS_COUNT * 4];
    USHORT Count = QCRIL_WMS_CLASS_COUNT;
    UCHAR One = 1, Primary = 0;
    ULONG Length, i;
    HRESULT hr;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WMS_BIND_SUBSCRIPTION);
    QmiEncodeTlv(&Encoder, 0x01, &Primary, sizeof(Primary));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiCommand(Modem, QcSvcWms, Message, Length);
    QcTrace("wms: bind subscription 0x%08lx\n", hr);

    /* Class 2 is stored on the card and announced, everything else comes straight to us */
    memcpy(Routes, &Count, sizeof(Count));
    for (i = 0; i < QCRIL_WMS_CLASS_COUNT; i++)
    {
        PUCHAR Tuple = &Routes[2 + i * 4];

        Tuple[0] = 0x00;
        Tuple[1] = Classes[i];
        if (Classes[i] == QCRIL_WMS_CLASS_SIM)
        {
            Tuple[2] = QCRIL_WMS_ROUTE_STORAGE_UIM;
            Tuple[3] = QCRIL_WMS_ACTION_STORE_NOTIFY;
        }
        else
        {
            Tuple[2] = QCRIL_WMS_ROUTE_STORAGE_NONE;
            Tuple[3] = QCRIL_WMS_ACTION_TRANSFER_ONLY;
        }
    }

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WMS_SET_ROUTES);
    QmiEncodeTlv(&Encoder, QCRIL_WMS_TLV_ROUTE_LIST, Routes, sizeof(Routes));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiCommand(Modem, QcSvcWms, Message, Length);
    QcTrace("wms: routes 0x%08lx\n", hr);

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WMS_SET_EVENT_REPORT);
    QmiEncodeTlv(&Encoder, QCRIL_WMS_TLV_REPORT_MT, &One, sizeof(One));
    QmiEncodeTlv(&Encoder, QCRIL_WMS_TLV_REPORT_MWI, &One, sizeof(One));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiCommand(Modem, QcSvcWms, Message, Length);
    QcTrace("wms: event report 0x%08lx\n", hr);

    /* Another client may own the primary role already, that is not fatal */
    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WMS_SET_PRIMARY_CLIENT);
    QmiEncodeTlv(&Encoder, 0x01, &One, sizeof(One));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiCommand(Modem, QcSvcWms, Message, Length);
    QcTrace("wms: primary client 0x%08lx\n", hr);

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WMS_INDICATION_REGISTER);
    QmiEncodeTlv(&Encoder, 0x10, &One, sizeof(One));
    QmiEncodeTlv(&Encoder, 0x13, &One, sizeof(One));
    QmiEncodeTlv(&Encoder, 0x15, &One, sizeof(One));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiCommand(Modem, QcSvcWms, Message, Length);
    QcTrace("wms: indication registration 0x%08lx\n", hr);
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

    /* Which transports can carry messages: 0 none, 1 3GPP, 2 3GPP2, 3 both */
    if (MessageId == QCRIL_WMS_SERVICE_READY_IND)
    {
        if (QmiFindTlv(Message, Length, 0x01, &Value, &ValueLen) && ValueLen >= sizeof(ULONG))
            QcTrace("sms service ready status %lu\n", *(const ULONG UNALIGNED *)Value);
        return;
    }

    /* Address type (three characters), length, then the digits */
    if (MessageId == QCRIL_WMS_SMSC_ADDRESS_IND)
    {
        CHAR Smsc[24];
        ULONG Count;

        if (QmiFindTlv(Message, Length, 0x01, &Value, &ValueLen) && ValueLen >= 4)
        {
            Route = Value;
            Count = min((ULONG)Route[3], (ULONG)ValueLen - 4);
            Count = min(Count, (ULONG)sizeof(Smsc) - 1);
            memcpy(Smsc, Route + 4, Count);
            Smsc[Count] = '\0';
            QcTrace("smsc address '%s' type %.3s\n", Smsc, (const CHAR *)Route);
        }
        return;
    }

    if (MessageId != QCRIL_WMS_EVENT_REPORT_IND)
    {
        QcTrace("ind WMS 0x%04X not handled\n", MessageId);
        return;
    }

    if (QmiFindTlv(Message, Length, QCRIL_WMS_TLV_MT_MESSAGE, &Value, &ValueLen) && ValueLen >= 5)
    {
        Route = Value;
        QcTrace("sms stored: storage %u index %lu\n", Route[0], Route[1] | (Route[2] << 8) | (Route[3] << 16) | ((ULONG)Route[4] << 24));
    }

    /* Only the direct transfer route carries the message bytes */
    if (!QmiFindTlv(Message, Length, QCRIL_WMS_TLV_TRANSFER_ROUTE, &Value, &ValueLen) || ValueLen < 8)
        return;

    Route = Value;
    /* ack_indicator u8, transaction_id u32, format u8, data_len u16, data[] */
    memcpy(&TransactionId, Route + 1, sizeof(TransactionId));
    memcpy(&DataLen, Route + 6, sizeof(DataLen));
    if (Route[5] != QCRIL_WMS_FORMAT_GW_PP || (ULONG)8 + DataLen > ValueLen)
    {
        QcTrace("sms: format %u, %u bytes, not a GSM point to point message\n", Route[5], DataLen);
        return;
    }

    QcTrace("sms arrived: transaction %lu ack %u, %u bytes\n", TransactionId, Route[0], DataLen);

    Notification = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Notification));
    if (!Notification)
        return;

    Notification->Size = sizeof(*Notification);
    Notification->Executor = QCRIL_EXECUTOR;
    Notification->AckId = TransactionId + 1;
    Notification->OverIms = 0;

    if (QcParseDeliverPdu(Route + 8, DataLen, &Notification->Message))
    {
        QcNotifyAll(Modem, RIL_NOTIFY_MESSAGE, Notification, sizeof(*Notification));
    }
    else
    {
        static const CHAR Digits[] = "0123456789abcdef";
        CHAR Hex[3 * 64 + 1];
        ULONG i, Shown = min((ULONG)DataLen, 64UL);

        for (i = 0; i < Shown; i++)
        {
            Hex[i * 3] = Digits[Route[8 + i] >> 4];
            Hex[i * 3 + 1] = Digits[Route[8 + i] & 0x0F];
            Hex[i * 3 + 2] = ' ';
        }
        Hex[Shown * 3] = '\0';
        QcTrace("sms: deliver pdu did not parse, %u bytes: %s\n", DataLen, Hex);
    }

    HeapFree(GetProcessHeap(), 0, Notification);
}
