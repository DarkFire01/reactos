/*
 * PROJECT:     ReactOS Qualcomm SoC support
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     QMI message framing and type length value coding
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* QMI carries one service message as a small header followed by a run of
 * type length value records. The format is the same on every service, so the
 * codec here is generic and the per service message ids live with each client. */

#include <pshpack1.h>
typedef struct _QMI_HEADER
{
    UCHAR Type;
    USHORT TransactionId;
    USHORT MessageId;
    USHORT Length;
} QMI_HEADER, *PQMI_HEADER;

typedef struct _QMI_TLV_HEADER
{
    UCHAR Type;
    USHORT Length;
} QMI_TLV_HEADER, *PQMI_TLV_HEADER;
#include <poppack.h>

C_ASSERT(sizeof(QMI_HEADER) == 7);
C_ASSERT(sizeof(QMI_TLV_HEADER) == 3);

/* Message type in the header */
#define QMI_TYPE_REQUEST            0x00
#define QMI_TYPE_RESPONSE           0x02
#define QMI_TYPE_INDICATION         0x04

/* The result record every response carries, type 2 */
#define QMI_TLV_RESULT              0x02
#define QMI_RESULT_SUCCESS          0x0000
#define QMI_RESULT_FAILURE          0x0001

typedef struct _QMI_RESULT
{
    USHORT Result;
    USHORT Error;
} QMI_RESULT, *PQMI_RESULT;

/* Walks a message while it is being built */
typedef struct _QMI_ENCODER
{
    PUCHAR Buffer;
    ULONG Capacity;
    ULONG Offset;
    PQMI_HEADER Header;
} QMI_ENCODER, *PQMI_ENCODER;

/* codec.c */
VOID
QmiEncodeBegin(
    _Out_ PQMI_ENCODER Encoder,
    _Out_writes_bytes_(Capacity) PVOID Buffer,
    _In_ ULONG Capacity,
    _In_ UCHAR Type,
    _In_ USHORT TransactionId,
    _In_ USHORT MessageId);

_Success_(return != FALSE)
BOOLEAN
QmiEncodeTlv(
    _Inout_ PQMI_ENCODER Encoder,
    _In_ UCHAR Type,
    _In_reads_bytes_(Length) const VOID *Value,
    _In_ USHORT Length);

_Success_(return != FALSE)
BOOLEAN
QmiEncodeResult(
    _Inout_ PQMI_ENCODER Encoder,
    _In_ USHORT Result,
    _In_ USHORT Error);

/* Total message length, header included, after the records are added */
ULONG
QmiEncodeEnd(
    _Inout_ PQMI_ENCODER Encoder);

_Success_(return != FALSE)
BOOLEAN
QmiMessageValid(
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length);

_Success_(return != FALSE)
BOOLEAN
QmiFindTlv(
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length,
    _In_ UCHAR Type,
    _Outptr_ const VOID **Value,
    _Out_ PUSHORT ValueLength);

_Success_(return != FALSE)
BOOLEAN
QmiGetResult(
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length,
    _Out_ PQMI_RESULT Result);
