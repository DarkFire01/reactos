/*
 * PROJECT:     ReactOS GNSS Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     QMI message framing for the location client
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "gnsssvc.h"
#include <string.h>

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
    Encoder->Header = Buffer;
    Encoder->Offset = sizeof(*Encoder->Header);

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
    ULONG Needed = sizeof(Tlv) + Length;

    if (Encoder->Capacity < Encoder->Offset || Encoder->Capacity - Encoder->Offset < Needed)
        return FALSE;

    Tlv.Type = Type;
    Tlv.Length = Length;
    memcpy(Encoder->Buffer + Encoder->Offset, &Tlv, sizeof(Tlv));
    if (Length)
        memcpy(Encoder->Buffer + Encoder->Offset + sizeof(Tlv), Value, Length);

    Encoder->Offset += Needed;
    return TRUE;
}

ULONG
QmiEncodeEnd(
    _Inout_ PQMI_ENCODER Encoder)
{
    Encoder->Header->Length = (USHORT)(Encoder->Offset - sizeof(*Encoder->Header));
    return Encoder->Offset;
}

/* Walks the records, stops at Type when it is found; Type 0 walks them all */
static
BOOLEAN
QmiWalk(
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length,
    _In_ UCHAR Type,
    _Outptr_opt_result_maybenull_ const VOID **Value,
    _Out_opt_ PUSHORT ValueLength)
{
    const QMI_HEADER *Header = Message;
    const UCHAR *Cursor, *End;
    QMI_TLV_HEADER Tlv;

    if (Length < sizeof(*Header) || Header->Length > Length - sizeof(*Header))
        return FALSE;

    Cursor = (const UCHAR *)Message + sizeof(*Header);
    End = Cursor + Header->Length;

    while (Cursor < End)
    {
        if ((ULONG)(End - Cursor) < sizeof(Tlv))
            return FALSE;

        memcpy(&Tlv, Cursor, sizeof(Tlv));
        Cursor += sizeof(Tlv);
        if ((ULONG)(End - Cursor) < Tlv.Length)
            return FALSE;

        if (Type != 0 && Tlv.Type == Type)
        {
            *Value = Cursor;
            *ValueLength = Tlv.Length;
            return TRUE;
        }

        Cursor += Tlv.Length;
    }

    return Type == 0;
}

_Success_(return != FALSE)
BOOLEAN
QmiMessageValid(
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    return QmiWalk(Message, Length, 0, NULL, NULL);
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
    *Value = NULL;
    *ValueLength = 0;

    if (Type == 0)
        return FALSE;

    return QmiWalk(Message, Length, Type, Value, ValueLength);
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

    if (!QmiFindTlv(Message, Length, QMI_TLV_RESULT, &Value, &ValueLength) ||
        ValueLength != sizeof(*Result))
    {
        return FALSE;
    }

    memcpy(Result, Value, sizeof(*Result));
    return TRUE;
}
