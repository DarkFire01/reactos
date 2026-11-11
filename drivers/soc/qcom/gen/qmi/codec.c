/*
 * PROJECT:     ReactOS Qualcomm QMI framework
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Message framing and type length value coding
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntddk.h>
#include <qcom/qmi.h>

VOID
QmiEncodeBegin(
    _Out_ PQMI_ENCODER Encoder,
    _Out_writes_bytes_(Capacity) PVOID Buffer,
    _In_ ULONG Capacity,
    _In_ UCHAR Type,
    _In_ USHORT TransactionId,
    _In_ USHORT MessageId)
{
    Encoder->Buffer = Buffer;
    Encoder->Capacity = Capacity;
    Encoder->Offset = sizeof(QMI_HEADER);
    Encoder->Header = Buffer;

    Encoder->Header->Type = Type;
    Encoder->Header->TransactionId = TransactionId;
    Encoder->Header->MessageId = MessageId;
    Encoder->Header->Length = 0;
}

_Success_(return != FALSE)
BOOLEAN
QmiEncodeTlv(
    _Inout_ PQMI_ENCODER Encoder,
    _In_ UCHAR Type,
    _In_reads_bytes_(Length) const VOID *Value,
    _In_ USHORT Length)
{
    QMI_TLV_HEADER Tlv;

    if (Encoder->Offset + sizeof(Tlv) + Length > Encoder->Capacity)
        return FALSE;

    Tlv.Type = Type;
    Tlv.Length = Length;
    RtlCopyMemory(Encoder->Buffer + Encoder->Offset, &Tlv, sizeof(Tlv));
    Encoder->Offset += sizeof(Tlv);

    RtlCopyMemory(Encoder->Buffer + Encoder->Offset, Value, Length);
    Encoder->Offset += Length;
    return TRUE;
}

_Success_(return != FALSE)
BOOLEAN
QmiEncodeResult(
    _Inout_ PQMI_ENCODER Encoder,
    _In_ USHORT Result,
    _In_ USHORT Error)
{
    QMI_RESULT Payload;

    Payload.Result = Result;
    Payload.Error = Error;
    return QmiEncodeTlv(Encoder, QMI_TLV_RESULT, &Payload, sizeof(Payload));
}

ULONG
QmiEncodeEnd(
    _Inout_ PQMI_ENCODER Encoder)
{
    Encoder->Header->Length = (USHORT)(Encoder->Offset - sizeof(QMI_HEADER));
    return Encoder->Offset;
}

/* A message is valid when its records sit exactly inside the declared length */
_Success_(return != FALSE)
BOOLEAN
QmiMessageValid(
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    const QMI_HEADER *Header = Message;
    const UCHAR *Body;
    ULONG Remaining;
    QMI_TLV_HEADER Tlv;

    if (Length < sizeof(QMI_HEADER))
        return FALSE;

    if ((ULONG)Header->Length + sizeof(QMI_HEADER) > Length)
        return FALSE;

    Body = (const UCHAR *)Message + sizeof(QMI_HEADER);
    Remaining = Header->Length;

    while (Remaining != 0)
    {
        if (Remaining < sizeof(QMI_TLV_HEADER))
            return FALSE;

        RtlCopyMemory(&Tlv, Body, sizeof(Tlv));
        if ((ULONG)Tlv.Length + sizeof(QMI_TLV_HEADER) > Remaining)
            return FALSE;

        Body += sizeof(QMI_TLV_HEADER) + Tlv.Length;
        Remaining -= sizeof(QMI_TLV_HEADER) + Tlv.Length;
    }

    return TRUE;
}

_Success_(return != FALSE)
BOOLEAN
QmiFindTlv(
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length,
    _In_ UCHAR Type,
    _Outptr_ const VOID **Value,
    _Out_ PUSHORT ValueLength)
{
    const QMI_HEADER *Header = Message;
    const UCHAR *Body;
    ULONG Remaining;
    QMI_TLV_HEADER Tlv;

    *Value = NULL;
    *ValueLength = 0;

    if (!QmiMessageValid(Message, Length))
        return FALSE;

    Body = (const UCHAR *)Message + sizeof(QMI_HEADER);
    Remaining = Header->Length;

    while (Remaining != 0)
    {
        RtlCopyMemory(&Tlv, Body, sizeof(Tlv));
        if (Tlv.Type == Type)
        {
            *Value = Body + sizeof(QMI_TLV_HEADER);
            *ValueLength = Tlv.Length;
            return TRUE;
        }

        Body += sizeof(QMI_TLV_HEADER) + Tlv.Length;
        Remaining -= sizeof(QMI_TLV_HEADER) + Tlv.Length;
    }

    return FALSE;
}

_Success_(return != FALSE)
BOOLEAN
QmiGetResult(
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length,
    _Out_ PQMI_RESULT Result)
{
    const VOID *Value;
    USHORT ValueLength;

    if (!QmiFindTlv(Message, Length, QMI_TLV_RESULT, &Value, &ValueLength))
        return FALSE;

    if (ValueLength != sizeof(QMI_RESULT))
        return FALSE;

    RtlCopyMemory(Result, Value, sizeof(QMI_RESULT));
    return TRUE;
}
