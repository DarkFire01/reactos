/*
 * PROJECT:     ReactOS Qualcomm IPC Router driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Message buffer chains and the version 1 wire codec
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ipcrouter.h"

#define NDEBUG
#include <debug.h>

const char *IpcTypeNames[8] =
{
    "Invalid", "Data", "Hello", "Bye",
    "New_server", "Rem_server", "Rem_client", "Resume_tx"
};

/* Counts fragments freed with a damaged canary, for diagnostics only */
static volatile LONG IpcMbufCorruptions;

PIPC_MBUF
IpcMbufAllocate(
    _In_ ULONG Headroom,
    _In_ ULONG Length)
{
    PIPC_MBUF Mbuf;
    ULONG Reserve = (Headroom + Length + 3) & ~3u;

    Mbuf = ExAllocatePoolWithTag(IPC_POOL, sizeof(*Mbuf) + Reserve, IPC_TAG);
    if (!Mbuf)
        return NULL;

    Mbuf->Next = NULL;
    Mbuf->Length = 0;
    Mbuf->Base = (PUCHAR)(Mbuf + 1);
    Mbuf->Data = Mbuf->Base + Headroom;
    Mbuf->Capacity = Reserve - Headroom;
    Mbuf->FreeRoutine = NULL;
    Mbuf->FreeContext = NULL;
    Mbuf->Reserved = NULL;
    Mbuf->Canary = IPC_MBUF_CANARY;
    return Mbuf;
}

VOID
IpcMbufFree(
    _In_opt_ PIPC_MBUF Mbuf)
{
    if (!Mbuf)
        return;

    if (Mbuf->Canary != IPC_MBUF_CANARY)
        InterlockedIncrement(&IpcMbufCorruptions);

    if (Mbuf->FreeRoutine)
        Mbuf->FreeRoutine(Mbuf->Base, Mbuf->FreeContext);

    Mbuf->Canary = 0;
    ExFreePoolWithTag(Mbuf, IPC_TAG);
}

VOID
IpcMbufFreeChain(
    _Inout_ PIPC_MBUF *Head)
{
    PIPC_MBUF Mbuf, Next;

    if (!Head)
        return;

    for (Mbuf = *Head; Mbuf; Mbuf = Next)
    {
        Next = Mbuf->Next;
        IpcMbufFree(Mbuf);
    }

    *Head = NULL;
}

ULONG
IpcMbufPrepend(
    _Inout_ PIPC_MBUF *Head,
    _In_reads_bytes_(Length) PVOID Source,
    _In_ ULONG Length)
{
    PIPC_MBUF Mbuf, Front;

    if (!Source || !Length)
        return 0;

    Mbuf = *Head;
    if (Mbuf && (ULONG)(Mbuf->Data - Mbuf->Base) >= Length)
    {
        Mbuf->Data -= Length;
        Mbuf->Capacity += Length;
        Mbuf->Length += Length;
        RtlCopyMemory(Mbuf->Data, Source, Length);
        return Length;
    }

    Front = IpcMbufAllocate(0, Length);
    if (!Front)
        return 0;

    RtlCopyMemory(Front->Data, Source, Length);
    Front->Length = Length;
    Front->Next = *Head;
    *Head = Front;
    return Length;
}

ULONG
IpcMbufAppend(
    _Inout_ PIPC_MBUF *Head,
    _In_reads_bytes_(Length) PVOID Source,
    _In_ ULONG Length)
{
    PIPC_MBUF Last, Tail = NULL;
    PUCHAR Bytes = Source;
    ULONG Total = Length;
    ULONG Free = 0, Chunk;

    if (!Head || !Source || !Length)
        return 0;

    Last = *Head;
    if (Last)
    {
        while (Last->Next)
            Last = Last->Next;
        Free = Last->Capacity - Last->Length;
    }

    if (Free < Length)
    {
        Tail = IpcMbufAllocate(0, Length - Free);
        if (!Tail)
            return 0;
    }

    if (Last)
    {
        Chunk = (Free >= Length) ? Length : Free;
        RtlCopyMemory(Last->Data + Last->Length, Bytes, Chunk);
        Last->Length += Chunk;
        Last->Next = Tail;
        Total -= Chunk;
        Bytes += Chunk;
    }
    else
    {
        *Head = Tail;
    }

    if (Tail)
    {
        RtlCopyMemory(Tail->Data, Bytes, Total);
        Tail->Length = Total;
    }

    return Length;
}

ULONG
IpcMbufCopyOut(
    _Inout_ PIPC_MBUF *Head,
    _Out_writes_bytes_(Length) PVOID Destination,
    _In_ ULONG Length)
{
    PIPC_MBUF Mbuf, Next;
    PUCHAR Out = Destination;
    ULONG Copied = 0, Chunk;

    if (!Head || !Destination || !Length)
        return 0;

    Mbuf = *Head;
    while (Mbuf && Copied < Length)
    {
        Chunk = (Length - Copied >= Mbuf->Length) ? Mbuf->Length : (Length - Copied);
        RtlCopyMemory(Out, Mbuf->Data, Chunk);
        Copied += Chunk;
        Out += Chunk;
        Mbuf->Length -= Chunk;
        Mbuf->Data += Chunk;

        Next = Mbuf->Next;
        if (Mbuf->Length == 0)
        {
            IpcMbufFree(Mbuf);
            *Head = Next;
        }
        Mbuf = Next;
    }

    return Copied;
}

PVOID
IpcMbufPeek(
    _In_opt_ PIPC_MBUF Head,
    _In_ ULONG Length)
{
    if (Head && Head->Length >= Length)
        return Head->Data;

    return NULL;
}

ULONG
IpcMbufChainLength(
    _In_opt_ PIPC_MBUF Head)
{
    ULONG Total = 0;

    for (; Head; Head = Head->Next)
        Total += Head->Length;

    return Total;
}

PIPC_MBUF
IpcMbufClone(
    _In_opt_ PIPC_MBUF Head)
{
    ULONG Total = IpcMbufChainLength(Head);
    PIPC_MBUF Copy;
    PUCHAR Cursor;

    /* One fragment with room for a header to be prepended later */
    Copy = IpcMbufAllocate(IPC_MBUF_HEADROOM, Total ? Total : 1);
    if (!Copy)
        return NULL;

    Cursor = Copy->Data;
    for (; Head; Head = Head->Next)
    {
        RtlCopyMemory(Cursor, Head->Data, Head->Length);
        Cursor += Head->Length;
    }

    Copy->Length = Total;
    return Copy;
}

PIPC_MBUF
IpcPacketAllocate(
    _In_reads_bytes_(Length) PVOID Payload,
    _In_ ULONG Length)
{
    PIPC_MBUF Head;
    ULONG Pad = (ULONG)(-(LONG)Length) & 3;
    ULONG Zero = 0;

    Head = IpcMbufAllocate(IPC_MBUF_HEADROOM, 128);
    if (!Head)
        return NULL;

    if (IpcMbufAppend(&Head, Payload, Length) == Length &&
        (Pad == 0 || IpcMbufAppend(&Head, &Zero, Pad) == Pad))
    {
        return Head;
    }

    IpcMbufFreeChain(&Head);
    return NULL;
}

ULONG
IpcHeaderEncode(
    _Inout_ PIPC_HEADER Header,
    _Inout_ PIPC_MBUF *Chain)
{
    if (!Header || !Chain || !*Chain)
        return 1;

    Header->Version = IPC_WIRE_VERSION;
    if (IpcMbufPrepend(Chain, Header, IPC_HEADER_SIZE) == IPC_HEADER_SIZE)
        return IPC_OK;

    return IPC_ENOMEM;
}

ULONG
IpcHeaderDecode(
    _Inout_ PIPC_MBUF *Chain,
    _Out_ PIPC_HEADER Header)
{
    PIPC_MBUF Head;
    PUCHAR Bytes;

    if (!Header || !Chain)
        return 1;

    Head = *Chain;
    if (!Head || Head->Length == 0)
        return 1;

    Bytes = Head->Data;
    if (!Bytes || Bytes[0] != IPC_WIRE_VERSION)
        return 1;

    if (IpcMbufCopyOut(Chain, Header, IPC_HEADER_SIZE) != IPC_HEADER_SIZE)
        return IPC_ENOMEM;

    Header->Version = IPC_WIRE_VERSION;
    return IPC_OK;
}
