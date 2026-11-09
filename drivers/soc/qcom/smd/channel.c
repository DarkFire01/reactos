/*
 * PROJECT:     ReactOS Qualcomm SMD driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SMD channels and the SMD Lite client interface
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "smd.h"

#define NDEBUG
#include <debug.h>

/* Channel type field: edge in the low byte, transfer kind above it */
#define SMD_TYPE_EDGE_MASK          0x000000FF
#define SMD_TYPE_KIND_MASK          0x00000F00
#define SMD_TYPE_KIND_STREAM        0x00000100
#define SMD_TYPE_KIND_PACKET        0x00000200
#define SMD_TYPE_RESERVED_MASK      0x0000F000

typedef struct _SMD_EDGE
{
    UCHAR HostA;
    UCHAR HostB;
} SMD_EDGE;

static const SMD_EDGE SmdEdges[QCOM_SMD_EDGE_COUNT] =
{
    { 0, 1 }, { 0, 2 }, { 1, 2 }, { 0, 3 }, { 1, 3 }, { 2, 3 }, { 0, 4 },
    { 1, 4 }, { 2, 4 }, { 3, 4 }, { 0, 5 }, { 1, 5 }, { 2, 5 }, { 3, 5 },
    { 4, 5 }, { 0, 6 }, { 1, 6 }, { 2, 6 }, { 4, 6 }, { 6, 5 }, { 6, 3 }
};

typedef enum _SMD_FIELD
{
    SmdFieldState = 0,
    SmdFieldDsr,
    SmdFieldCts,
    SmdFieldCd,
    SmdFieldRi,
    SmdFieldHeadFlag,
    SmdFieldTailFlag,
    SmdFieldStateFlag,
    SmdFieldBlockReadIntr,
    SmdFieldTail,
    SmdFieldHead
} SMD_FIELD;

static const UCHAR SmdByteOffsets[] = { 0x00, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x10 };

ULONG
SmdEdgeRemoteHost(
    _In_ ULONG Edge)
{
    if (Edge >= QCOM_SMD_EDGE_COUNT)
        return QCOM_HOST_GLOBAL;

    if (SmdEdges[Edge].HostA == QCOM_HOST_APPS)
        return SmdEdges[Edge].HostB;
    if (SmdEdges[Edge].HostB == QCOM_HOST_APPS)
        return SmdEdges[Edge].HostA;

    return QCOM_HOST_GLOBAL;
}

static
ULONG
SmdHalfGet(
    _In_ PSMD_PORT Port,
    _In_ PVOID Half,
    _In_ SMD_FIELD Field)
{
    if (Port->WordAccess)
        return READ_REGISTER_ULONG((PULONG)Half + Field);

    if (Field == SmdFieldState || Field == SmdFieldTail || Field == SmdFieldHead)
        return *(volatile ULONG *)((PUCHAR)Half + SmdByteOffsets[Field]);

    return *(volatile UCHAR *)((PUCHAR)Half + SmdByteOffsets[Field]);
}

static
VOID
SmdHalfSet(
    _In_ PSMD_PORT Port,
    _In_ PVOID Half,
    _In_ SMD_FIELD Field,
    _In_ ULONG Value)
{
    if (Port->WordAccess)
    {
        WRITE_REGISTER_ULONG((PULONG)Half + Field, Value);
        return;
    }

    if (Field == SmdFieldState || Field == SmdFieldTail || Field == SmdFieldHead)
        *(volatile ULONG *)((PUCHAR)Half + SmdByteOffsets[Field]) = Value;
    else
        *(volatile UCHAR *)((PUCHAR)Half + SmdByteOffsets[Field]) = (UCHAR)Value;
}

VOID
SmdSignalHost(
    _In_ PSMD_DEVICE Device,
    _In_ ULONG Host)
{
    PSMD_HOST Target;

    if (Host >= QCOM_HOST_COUNT)
        return;

    Target = &Device->Hosts[Host];
    if (Target->SmdIrqRegister)
    {
        KeMemoryBarrier();
        WRITE_REGISTER_ULONG(Target->SmdIrqRegister, Target->SmdIrqMask);
    }
}

static
VOID
SmdNotify(
    _In_ PSMD_PORT Port,
    _In_ ULONG Event)
{
    if (Port->Notify)
        Port->Notify(Port, Event, Port->NotifyContext);
}

/* Free bytes for the writer, the FIFO always keeps 4 bytes empty */
static
ULONG
SmdTxFree(
    _In_ PSMD_PORT Port)
{
    ULONG Head = SmdHalfGet(Port, Port->TxHalf, SmdFieldHead);
    ULONG Tail = SmdHalfGet(Port, Port->TxHalf, SmdFieldTail);

    if (Head >= Port->FifoSize || Tail >= Port->FifoSize)
        SMD_FATAL(0x30);

    if (Tail <= Head)
        return Port->FifoSize - (Head - Tail) - 4;

    return Tail - Head - 4;
}

static
ULONG
SmdRxAvailable(
    _In_ PSMD_PORT Port)
{
    ULONG Head = SmdHalfGet(Port, Port->RxHalf, SmdFieldHead);
    ULONG Tail = SmdHalfGet(Port, Port->RxHalf, SmdFieldTail);

    if (Head >= Port->FifoSize || Tail >= Port->FifoSize)
        SMD_FATAL(0x31);

    if (Head >= Tail)
        return Head - Tail;

    return Port->FifoSize - Tail + Head;
}

/* Copies into the ring at Index and returns the index after the data */
static
ULONG
SmdFifoWrite(
    _In_ PSMD_PORT Port,
    _In_ ULONG Index,
    _In_reads_bytes_(Length) const VOID *Data,
    _In_ ULONG Length)
{
    ULONG First = min(Length, Port->FifoSize - Index);

    RtlCopyMemory(Port->TxFifo + Index, Data, First);
    if (Length > First)
        RtlCopyMemory(Port->TxFifo, (const UCHAR *)Data + First, Length - First);

    Index += Length;
    if (Index >= Port->FifoSize)
        Index -= Port->FifoSize;
    return Index;
}

static
ULONG
SmdFifoRead(
    _In_ PSMD_PORT Port,
    _In_ ULONG Index,
    _Out_writes_bytes_opt_(Length) PVOID Data,
    _In_ ULONG Length)
{
    ULONG First = min(Length, Port->FifoSize - Index);

    if (Data)
    {
        RtlCopyMemory(Data, Port->RxFifo + Index, First);
        if (Length > First)
            RtlCopyMemory((PUCHAR)Data + First, Port->RxFifo, Length - First);
    }

    Index += Length;
    if (Index >= Port->FifoSize)
        Index -= Port->FifoSize;
    return Index;
}

static
VOID
SmdRaiseStateFlag(
    _In_ PSMD_PORT Port)
{
    KeMemoryBarrier();
    SmdHalfSet(Port, Port->TxHalf, SmdFieldStateFlag, 1);
    SmdSignalHost(Port->Device, Port->RemoteHost);
}

static
VOID
SmdEnterOpening(
    _Inout_ PSMD_PORT Port)
{
    RtlZeroMemory(Port->RemoteSignals, sizeof(Port->RemoteSignals));
    Port->HeaderBytes = 0;
    Port->PacketRemaining = 0;
    Port->TxBlocked = FALSE;

    SmdHalfSet(Port, Port->TxHalf, SmdFieldHead, 0);
    SmdHalfSet(Port, Port->RxHalf, SmdFieldTail, 0);
    SmdHalfSet(Port, Port->TxHalf, SmdFieldHeadFlag, 0);
    SmdHalfSet(Port, Port->TxHalf, SmdFieldTailFlag, 0);
    SmdHalfSet(Port, Port->TxHalf, SmdFieldStateFlag, 0);
    SmdHalfSet(Port, Port->TxHalf, SmdFieldState, SMD_STATE_OPENING);
    Port->LocalState = SMD_STATE_OPENING;
    DPRINT1("[smd] '%s' edge %lu cid %lu -> OPENING\n", Port->Name, Port->Edge, Port->ChannelId);
}

static
VOID
SmdUnlinkPort(
    _Inout_ PSMD_PORT Port)
{
    if (Port->Linked)
    {
        RemoveEntryList(&Port->HostLink);
        Port->Linked = FALSE;
    }
}

/* Finishes a close: the client is told and must not touch the handle afterwards */
static
VOID
SmdRetirePort(
    _Inout_ PSMD_PORT Port)
{
    SmdUnlinkPort(Port);
    SmdNotify(Port, QCOM_SMDL_EVENT_CLOSE_COMPLETE);
    ExFreePoolWithTag(Port, SMD_TAG);
}

static
BOOLEAN
SmdEnterClosed(
    _Inout_ PSMD_PORT Port)
{
    SMD_FIELD Field;

    SmdHalfSet(Port, Port->TxHalf, SmdFieldState, SMD_STATE_CLOSED);
    for (Field = SmdFieldDsr; Field <= SmdFieldBlockReadIntr; Field++)
        SmdHalfSet(Port, Port->TxHalf, Field, 0);
    Port->LocalState = SMD_STATE_CLOSED;
    SmdRaiseStateFlag(Port);
    DPRINT1("[smd] '%s' edge %lu cid %lu -> CLOSED (remote %lu)\n",
            Port->Name, Port->Edge, Port->ChannelId, SmdHalfGet(Port, Port->RxHalf, SmdFieldState));

    /* With the other side already gone there is nothing left to wait for */
    return SmdHalfGet(Port, Port->RxHalf, SmdFieldState) == SMD_STATE_CLOSED;
}

static
VOID
SmdEnterReset(
    _Inout_ PSMD_PORT Port)
{
    SMD_FIELD Field;

    for (Field = SmdFieldState; Field <= SmdFieldBlockReadIntr; Field++)
        SmdHalfSet(Port, Port->RxHalf, Field, 0);

    SmdHalfSet(Port, Port->TxHalf, SmdFieldState, SMD_STATE_RESET);
    Port->LocalState = SMD_STATE_RESET;
    Port->RemoteState = SMD_STATE_CLOSED;
    DPRINT1("[smd] '%s' edge %lu cid %lu -> RESET\n", Port->Name, Port->Edge, Port->ChannelId);
}

static
VOID
SmdEnterOpened(
    _Inout_ PSMD_PORT Port)
{
    SmdHalfSet(Port, Port->TxHalf, SmdFieldState, SMD_STATE_OPENED);
    Port->LocalState = SMD_STATE_OPENED;
    DPRINT1("[smd] '%s' edge %lu cid %lu -> OPENED\n", Port->Name, Port->Edge, Port->ChannelId);
    SmdNotify(Port, QCOM_SMDL_EVENT_OPEN);
}

/**
 * @brief One step of the channel state machine.
 *
 * @return The next input to process, SMD_INPUT_NONE when settled. *Retired is set when the port was freed.
 */
static
ULONG
SmdStep(
    _Inout_ PSMD_PORT Port,
    _In_ ULONG Input,
    _Out_ PBOOLEAN Retired)
{
    ULONG Remote;

    *Retired = FALSE;

    switch (Port->LocalState)
    {
        case SMD_STATE_CLOSED:
            if (Input == SMD_INPUT_LOCAL_OPEN)
            {
                if (!Port->Linked)
                {
                    InsertTailList(&Port->Device->Hosts[Port->RemoteHost].Ports, &Port->HostLink);
                    Port->Linked = TRUE;
                }

                Remote = SmdHalfGet(Port, Port->RxHalf, SmdFieldState);
                Port->RemoteState = Remote;

                /* A stale open from a previous session has to close before we start over */
                if (Remote == SMD_STATE_OPENED)
                {
                    Port->WaitRemoteClose = TRUE;
                    return SMD_INPUT_NONE;
                }

                SmdEnterOpening(Port);
                if (Remote == SMD_STATE_OPENING || Remote == SMD_STATE_FLUSHING || Remote == SMD_STATE_RESET_OPENING)
                    return SMD_INPUT_REMOTE_OPEN;
                return SMD_INPUT_NONE;
            }

            if (Input == SMD_INPUT_REMOTE_CLOSE)
            {
                if (Port->WaitRemoteClose)
                {
                    Port->WaitRemoteClose = FALSE;
                    return SMD_INPUT_LOCAL_OPEN;
                }

                if (Port->CloseRequested)
                {
                    SmdRetirePort(Port);
                    *Retired = TRUE;
                }
            }
            return SMD_INPUT_NONE;

        case SMD_STATE_OPENING:
            switch (Input)
            {
                case SMD_INPUT_LOCAL_CLOSE:
                    if (SmdEnterClosed(Port))
                    {
                        SmdRetirePort(Port);
                        *Retired = TRUE;
                    }
                    break;

                case SMD_INPUT_REMOTE_OPEN:
                case SMD_INPUT_REMOTE_REOPEN:
                    if (Port->FlushPending)
                    {
                        Port->FlushPending = FALSE;
                        SmdHalfSet(Port, Port->TxHalf, SmdFieldState, SMD_STATE_FLUSHING);
                        Port->LocalState = SMD_STATE_FLUSHING;
                    }
                    else
                    {
                        SmdEnterOpened(Port);
                    }
                    break;

                case SMD_INPUT_REMOTE_FLUSH:
                    Port->FlushPending = TRUE;
                    break;

                case SMD_INPUT_REMOTE_RESET:
                    SmdEnterReset(Port);
                    break;
            }
            return SMD_INPUT_NONE;

        case SMD_STATE_OPENED:
            switch (Input)
            {
                case SMD_INPUT_LOCAL_CLOSE:
                    if (SmdEnterClosed(Port))
                    {
                        SmdRetirePort(Port);
                        *Retired = TRUE;
                    }
                    break;

                case SMD_INPUT_REMOTE_CLOSE:
                    SmdHalfSet(Port, Port->TxHalf, SmdFieldState, SMD_STATE_CLOSING);
                    Port->LocalState = SMD_STATE_CLOSING;
                    break;

                case SMD_INPUT_REMOTE_FLUSH:
                    SmdHalfSet(Port, Port->TxHalf, SmdFieldState, SMD_STATE_FLUSHING);
                    SmdHalfSet(Port, Port->TxHalf, SmdFieldBlockReadIntr, 0);
                    Port->LocalState = SMD_STATE_FLUSHING;
                    break;

                case SMD_INPUT_REMOTE_RESET:
                    SmdEnterReset(Port);
                    break;
            }
            return SMD_INPUT_NONE;

        case SMD_STATE_FLUSHING:
            switch (Input)
            {
                case SMD_INPUT_LOCAL_CLOSE:
                    Port->CloseRequested = TRUE;
                    break;

                case SMD_INPUT_REMOTE_CLOSE:
                case SMD_INPUT_FLUSH_DONE:
                case SMD_INPUT_REMOTE_OPEN:
                    if (Port->CloseRequested)
                    {
                        if (SmdEnterClosed(Port))
                        {
                            SmdRetirePort(Port);
                            *Retired = TRUE;
                        }
                    }
                    else if (Input == SMD_INPUT_REMOTE_CLOSE)
                    {
                        SmdHalfSet(Port, Port->TxHalf, SmdFieldState, SMD_STATE_CLOSING);
                        Port->LocalState = SMD_STATE_CLOSING;
                    }
                    else
                    {
                        SmdEnterOpened(Port);
                    }
                    break;

                case SMD_INPUT_REMOTE_RESET:
                    SmdEnterReset(Port);
                    break;
            }
            return SMD_INPUT_NONE;

        case SMD_STATE_CLOSING:
            switch (Input)
            {
                case SMD_INPUT_LOCAL_CLOSE:
                    if (SmdEnterClosed(Port))
                    {
                        SmdRetirePort(Port);
                        *Retired = TRUE;
                    }
                    break;

                case SMD_INPUT_REMOTE_OPEN:
                case SMD_INPUT_REMOTE_REOPEN:
                    SmdEnterOpening(Port);
                    return SMD_INPUT_REMOTE_OPEN;

                case SMD_INPUT_REMOTE_RESET:
                    SmdEnterReset(Port);
                    break;
            }
            return SMD_INPUT_NONE;

        case SMD_STATE_RESET:
            switch (Input)
            {
                case SMD_INPUT_LOCAL_CLOSE:
                    if (SmdEnterClosed(Port))
                    {
                        SmdRetirePort(Port);
                        *Retired = TRUE;
                    }
                    break;

                case SMD_INPUT_REMOTE_OPEN:
                case SMD_INPUT_REMOTE_REOPEN:
                    SmdEnterOpening(Port);
                    return SMD_INPUT_REMOTE_OPEN;

                case SMD_INPUT_REMOTE_CLOSE:
                    SmdHalfSet(Port, Port->TxHalf, SmdFieldState, SMD_STATE_CLOSING);
                    Port->LocalState = SMD_STATE_CLOSING;
                    break;
            }
            return SMD_INPUT_NONE;

        case SMD_STATE_RESET_OPENING:
            if (Input == SMD_INPUT_REMOTE_OPEN || Input == SMD_INPUT_REMOTE_CLOSE)
            {
                SmdEnterOpening(Port);
                return SMD_INPUT_REMOTE_OPEN;
            }
            return SMD_INPUT_NONE;
    }

    return SMD_INPUT_NONE;
}

/* Must be called with the host lock held, returns FALSE if the port no longer exists */
static
BOOLEAN
SmdRunStateMachine(
    _Inout_ PSMD_PORT Port,
    _In_ ULONG Input)
{
    BOOLEAN Retired;
    ULONG Before = Port->LocalState;

    while (Input != SMD_INPUT_NONE)
    {
        Input = SmdStep(Port, Input, &Retired);
        if (Retired)
            return FALSE;
    }

    if (Port->LocalState != Before)
        SmdRaiseStateFlag(Port);

    return TRUE;
}

/* Turns a change in the remote half's state into a state machine input */
static
ULONG
SmdRemoteStateInput(
    _Inout_ PSMD_PORT Port,
    _In_ ULONG New)
{
    ULONG Previous = Port->RemoteState;
    BOOLEAN Reopen;

    Port->RemoteState = New;
    Reopen = (Port->LocalState == SMD_STATE_CLOSING ||
              Port->LocalState == SMD_STATE_RESET ||
              Port->LocalState == SMD_STATE_RESET_OPENING);

    if (New == Previous)
        return SMD_INPUT_NONE;

    DPRINT1("[smd] '%s' edge %lu remote state %lu -> %lu (local %lu)\n",
            Port->Name, Port->Edge, Previous, New, Port->LocalState);

    if (New == SMD_STATE_CLOSED || New == SMD_STATE_CLOSING)
        return SMD_INPUT_REMOTE_CLOSE;

    switch (Previous)
    {
        case SMD_STATE_CLOSED:
            if (New == SMD_STATE_RESET_OPENING)
                return SMD_INPUT_REMOTE_CLOSE;
            if (New == SMD_STATE_OPENING || New == SMD_STATE_OPENED || New == SMD_STATE_FLUSHING)
                return Reopen ? SMD_INPUT_REMOTE_REOPEN : SMD_INPUT_REMOTE_OPEN;
            break;

        case SMD_STATE_OPENING:
            if (New == SMD_STATE_OPENED || New == SMD_STATE_FLUSHING)
                return SMD_INPUT_REMOTE_OPEN;
            break;

        case SMD_STATE_OPENED:
        case SMD_STATE_FLUSHING:
            if (New == SMD_STATE_OPENED || New == SMD_STATE_FLUSHING)
                return Reopen ? SMD_INPUT_REMOTE_REOPEN : SMD_INPUT_REMOTE_OPEN;
            break;

        default:
            if (New == SMD_STATE_OPENING || New == SMD_STATE_OPENED || New == SMD_STATE_FLUSHING)
                return SMD_INPUT_REMOTE_OPEN;
            break;
    }

    return SMD_INPUT_NONE;
}

/* Returns FALSE when the port was retired while handling remote changes */
static
BOOLEAN
SmdProcessPort(
    _Inout_ PSMD_PORT Port)
{
    static const SMD_FIELD SignalFields[4] = { SmdFieldDsr, SmdFieldCts, SmdFieldCd, SmdFieldRi };
    static const ULONG SignalEvents[4] =
    {
        QCOM_SMDL_EVENT_DSR, QCOM_SMDL_EVENT_CTS, QCOM_SMDL_EVENT_CD, QCOM_SMDL_EVENT_RI
    };
    ULONG Input, Value, i;
    BOOLEAN Live;

    if (SmdHalfGet(Port, Port->RxHalf, SmdFieldStateFlag))
    {
        SmdHalfSet(Port, Port->RxHalf, SmdFieldStateFlag, 0);

        Input = SmdRemoteStateInput(Port, SmdHalfGet(Port, Port->RxHalf, SmdFieldState));
        if (Input == SMD_INPUT_REMOTE_CLOSE && Port->LocalState != SMD_STATE_CLOSED)
            SmdNotify(Port, QCOM_SMDL_EVENT_REMOTE_CLOSE);

        if (Input != SMD_INPUT_NONE)
        {
            Live = SmdRunStateMachine(Port, Input);
            if (!Live)
                return FALSE;
        }

        for (i = 0; i < 4; i++)
        {
            Value = SmdHalfGet(Port, Port->RxHalf, SignalFields[i]) != 0;
            if (Value == Port->RemoteSignals[i])
                continue;

            Port->RemoteSignals[i] = Value;
            SmdNotify(Port, SignalEvents[i]);

            /* A writer stalled on flow control may continue now */
            if (i == QCOM_SMDL_SIG_CTS && Value && Port->TxBlocked)
            {
                Port->TxBlocked = FALSE;
                SmdNotify(Port, QCOM_SMDL_EVENT_WRITE);
            }
        }
    }

    if (Port->LocalState != SMD_STATE_OPENED && Port->LocalState != SMD_STATE_FLUSHING)
        return TRUE;

    if (SmdHalfGet(Port, Port->RxHalf, SmdFieldTailFlag))
    {
        SmdHalfSet(Port, Port->RxHalf, SmdFieldTailFlag, 0);
        if (Port->TxBlocked)
        {
            Port->TxBlocked = FALSE;
            SmdNotify(Port, QCOM_SMDL_EVENT_WRITE);
        }
    }

    if (SmdHalfGet(Port, Port->RxHalf, SmdFieldHeadFlag))
    {
        SmdHalfSet(Port, Port->RxHalf, SmdFieldHeadFlag, 0);
        SmdNotify(Port, QCOM_SMDL_EVENT_READ);
    }

    return TRUE;
}

VOID
SmdProcessHost(
    _Inout_ PSMD_DEVICE Device,
    _In_ ULONG Host)
{
    PSMD_HOST Target = &Device->Hosts[Host];
    PLIST_ENTRY Entry, Next;

    SmdRLockAcquire(&Target->Lock);
    for (Entry = Target->Ports.Flink; Entry != &Target->Ports; Entry = Next)
    {
        Next = Entry->Flink;
        SmdProcessPort(CONTAINING_RECORD(Entry, SMD_PORT, HostLink));
    }
    SmdRLockRelease(&Target->Lock);
}

/* The remote processor went down, every channel to it is torn down on our side */
VOID
SmdResetHost(
    _Inout_ PSMD_DEVICE Device,
    _In_ ULONG Host)
{
    PSMD_HOST Target = &Device->Hosts[Host];
    PLIST_ENTRY Entry, Next;
    PSMD_PORT Port;
    BOOLEAN WasOpen;

    DPRINT1("[smd] remote host %lu RESET, tearing down its channels\n", Host);
    SmdRLockAcquire(&Target->Lock);

    SmemMutexReleaseHost(Device, Host);

    for (Entry = Target->Ports.Flink; Entry != &Target->Ports; Entry = Next)
    {
        Next = Entry->Flink;
        Port = CONTAINING_RECORD(Entry, SMD_PORT, HostLink);

        WasOpen = (Port->LocalState != SMD_STATE_CLOSED);
        if (!SmdRunStateMachine(Port, SMD_INPUT_REMOTE_RESET))
            continue;

        if (WasOpen)
            SmdNotify(Port, QCOM_SMDL_EVENT_REMOTE_CLOSE);
        else
            SmdRunStateMachine(Port, SMD_INPUT_REMOTE_CLOSE);
    }

    SmdRLockRelease(&Target->Lock);
}

/* Finds the channel table entry for (Name, Edge), claiming a free slot if the remote has not made one */
static
NTSTATUS
SmdClaimChannel(
    _Inout_ PSMD_DEVICE Device,
    _Inout_ PSMD_PORT Port,
    _In_ ULONG Kind)
{
    PSMD_CHANNEL_ENTRY Table, Entry, Free = NULL;
    ULONG TableSize = SMD_CHANNEL_TABLE_SIZE;
    PVOID Address;
    ULONG i, First;
    NTSTATUS Status = STATUS_SUCCESS;

    if (SmemAllocate(Device, Port->PartitionHost, SMEM_ITEM_CHANNEL_TABLE, &TableSize, 0, &Address) != SMEM_OK)
        return STATUS_INSUFFICIENT_RESOURCES;

    Table = Address;

    /* Slot 0 of the global table is reserved */
    First = (Port->PartitionHost == QCOM_HOST_GLOBAL) ? 1 : 0;

    SmemMutexAcquire(Device, SMEM_MUTEX_HEAP);

    for (i = First; i < SMD_CHANNELS_PER_TABLE; i++)
    {
        Entry = &Table[i];
        if (Entry->Name[0] == '\0')
        {
            if (!Free)
                Free = Entry;
            continue;
        }

        if ((Entry->Type & SMD_TYPE_EDGE_MASK) != Port->Edge ||
            strncmp(Entry->Name, Port->Name, SMD_NAME_LENGTH - 1) != 0)
        {
            continue;
        }

        if ((Entry->Type & SMD_TYPE_RESERVED_MASK) ||
            ((Entry->Type & SMD_TYPE_KIND_MASK) && (Entry->Type & SMD_TYPE_KIND_MASK) != Kind))
        {
            SMD_FATAL(0x40);
        }

        Entry->Type |= Kind;
        Entry->RefCount++;
        Port->ChannelId = Entry->ChannelId;
        goto Exit;
    }

    if (!Free)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    RtlZeroMemory(Free, sizeof(*Free));
    RtlCopyMemory(Free->Name, Port->Name, SMD_NAME_LENGTH - 1);
    Free->ChannelId = (ULONG)(Free - Table);
    Free->RefCount = 1;
    KeMemoryBarrier();
    Free->Type = Port->Edge | Kind;
    Port->ChannelId = Free->ChannelId;

Exit:
    SmemMutexRelease(Device, SMEM_MUTEX_HEAP);
    return Status;
}

static
NTSTATUS
SmdMapChannel(
    _Inout_ PSMD_DEVICE Device,
    _Inout_ PSMD_PORT Port)
{
    ULONG CtrlItem, FifoItem, CtrlSize, FifoSize, HalfSize, Flags = SMEM_FLAG_NO_GLOBAL;
    PULONG Features;
    ULONG FeatureSize;
    PVOID Ctrl, Fifo;
    LONG Result;

    if (Port->ChannelId < SMD_CHANNELS_PER_TABLE)
    {
        CtrlItem = SMEM_ITEM_SMD_CTRL_BASE + Port->ChannelId;
        FifoItem = SMEM_ITEM_SMD_FIFO_BASE + Port->ChannelId;
    }
    else
    {
        CtrlItem = SMEM_ITEM_SMD_CTRL_BASE2 + Port->ChannelId - SMD_CHANNELS_PER_TABLE;
        FifoItem = SMEM_ITEM_SMD_FIFO_BASE2 + Port->ChannelId - SMD_CHANNELS_PER_TABLE;
    }

    HalfSize = Port->WordAccess ? sizeof(SMD_HALF_WORD) : sizeof(SMD_HALF_BYTE);
    CtrlSize = HalfSize * 2;
    Result = SmemAllocate(Device, Port->PartitionHost, CtrlItem, &CtrlSize, SMEM_FLAG_NO_GLOBAL, &Ctrl);
    if (Result != SMEM_OK)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (SmemLookup(Device, QCOM_HOST_GLOBAL, SMEM_ITEM_SMD_FEATURES, &FeatureSize, (PVOID *)&Features) == SMEM_OK &&
        FeatureSize >= sizeof(ULONG) * QCOM_HOST_COUNT &&
        (Features[Port->RemoteHost] & 1))
    {
        Flags |= SMEM_FLAG_CACHED;
    }

    FifoSize = Port->FifoSize * 2;
    Result = SmemAllocate(Device, Port->PartitionHost, FifoItem, &FifoSize, Flags, &Fifo);
    if (Result == SMEM_E_SIZE_MISMATCH)
        SMD_FATAL(0x41);
    if (Result != SMEM_OK)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* The apps processor is always the first host of its edges, so its half comes first */
    Port->TxHalf = Ctrl;
    Port->RxHalf = (PUCHAR)Ctrl + HalfSize;
    Port->TxFifo = Fifo;
    Port->RxFifo = (PUCHAR)Fifo + Port->FifoSize;
    return STATUS_SUCCESS;
}

static
QCOM_SMDL_HANDLE
NTAPI
SmdlOpen(
    _In_z_ PCSTR Name,
    _In_ ULONG Edge,
    _In_ ULONG Flags,
    _In_ ULONG FifoSize,
    _In_opt_ PQCOM_SMDL_NOTIFY Notify,
    _In_opt_ PVOID Context)
{
    PSMD_DEVICE Device = SmdGlobal;
    PSMD_PORT Port;
    PSMD_HOST Host;
    size_t NameLength;
    NTSTATUS Status;

    if (!Device || !Device->SmemReady || !Name || !Name[0] || Edge >= QCOM_SMD_EDGE_COUNT)
    {
        DPRINT1("[smd] open '%s' edge %lu rejected (device %p smem ready %u)\n",
                Name ? Name : "(null)", Edge, Device, Device ? Device->SmemReady : 0);
        return NULL;
    }

    if (SmdEdges[Edge].HostA != QCOM_HOST_APPS && SmdEdges[Edge].HostB != QCOM_HOST_APPS)
    {
        DPRINT1("[smd] open '%s': edge %lu does not involve apps\n", Name, Edge);
        return NULL;
    }

    if (FifoSize == 0)
        FifoSize = SMD_DEFAULT_FIFO_SIZE;
    else if ((FifoSize & 31) || FifoSize < SMD_MIN_FIFO_SIZE || FifoSize > SMD_MAX_FIFO_SIZE)
    {
        DPRINT1("[smd] open '%s': bad fifo size 0x%lx\n", Name, FifoSize);
        return NULL;
    }

    Port = ExAllocatePoolZero(NonPagedPool, sizeof(*Port), SMD_TAG);
    if (!Port)
        return NULL;

    NameLength = strnlen(Name, SMD_NAME_LENGTH - 1);
    RtlCopyMemory(Port->Name, Name, NameLength);
    Port->Device = Device;
    Port->Edge = Edge;
    Port->RemoteHost = SmdEdgeRemoteHost(Edge);
    Port->PartitionHost = Device->Partitions[Port->RemoteHost].Header ? Port->RemoteHost : QCOM_HOST_GLOBAL;
    Port->FifoSize = FifoSize;
    Port->Stream = (Flags & QCOM_SMDL_OPEN_STREAM) != 0;
    Port->WordAccess = (SmdEdges[Edge].HostA == QCOM_HOST_RPM || SmdEdges[Edge].HostB == QCOM_HOST_RPM);
    Port->Notify = Notify;
    Port->NotifyContext = Context;
    Port->LocalState = SMD_STATE_CLOSED;

    Host = &Device->Hosts[Port->RemoteHost];
    SmdRLockAcquire(&Host->Lock);

    Status = SmdClaimChannel(Device, Port, Port->Stream ? SMD_TYPE_KIND_STREAM : SMD_TYPE_KIND_PACKET);
    if (NT_SUCCESS(Status))
        Status = SmdMapChannel(Device, Port);

    if (!NT_SUCCESS(Status))
    {
        SmdRLockRelease(&Host->Lock);
        DPRINT1("[smd] open '%s' edge %lu FAILED 0x%lx\n", Name, Edge, Status);
        ExFreePoolWithTag(Port, SMD_TAG);
        return NULL;
    }

    DPRINT1("[smd] open '%s' edge %lu host %lu cid %lu fifo 0x%lx %s\n",
            Name, Edge, Port->RemoteHost, Port->ChannelId, FifoSize, Port->Stream ? "stream" : "packet");

    /* Our modem signals are up before the remote ever sees the channel opening */
    SmdHalfSet(Port, Port->TxHalf, SmdFieldDsr, 1);
    SmdHalfSet(Port, Port->TxHalf, SmdFieldCts, 1);
    SmdHalfSet(Port, Port->TxHalf, SmdFieldCd, 1);

    SmdRunStateMachine(Port, SMD_INPUT_LOCAL_OPEN);
    SmdRLockRelease(&Host->Lock);

    return Port;
}

static
LONG
NTAPI
SmdlClose(
    _In_ QCOM_SMDL_HANDLE Handle)
{
    PSMD_PORT Port = Handle;
    PSMD_HOST Host;

    if (!Port)
        return -1;

    Host = &Port->Device->Hosts[Port->RemoteHost];
    DPRINT1("[smd] close '%s' edge %lu (local %lu)\n", Port->Name, Port->Edge, Port->LocalState);
    SmdRLockAcquire(&Host->Lock);

    Port->CloseRequested = TRUE;
    if (Port->LocalState == SMD_STATE_CLOSED)
    {
        /* Never got past waiting for a stale remote session */
        SmdRetirePort(Port);
    }
    else
    {
        SmdRunStateMachine(Port, SMD_INPUT_LOCAL_CLOSE);
    }

    SmdRLockRelease(&Host->Lock);
    return 0;
}

static
BOOLEAN
SmdCanWrite(
    _In_ PSMD_PORT Port)
{
    return Port->LocalState == SMD_STATE_OPENED && SmdHalfGet(Port, Port->RxHalf, SmdFieldCts) != 0;
}

static
VOID
SmdPublishWrite(
    _Inout_ PSMD_PORT Port,
    _In_ ULONG NewHead,
    _In_ ULONG Flags)
{
    KeMemoryBarrier();
    SmdHalfSet(Port, Port->TxHalf, SmdFieldHead, NewHead);
    KeMemoryBarrier();
    SmdHalfSet(Port, Port->TxHalf, SmdFieldHeadFlag, 1);

    if (!(Flags & QCOM_SMDL_WRITE_NO_SIGNAL))
        SmdSignalHost(Port->Device, Port->RemoteHost);
}

static
VOID
SmdMarkBlocked(
    _Inout_ PSMD_PORT Port)
{
    /* Ask the reader to interrupt us once it frees space */
    SmdHalfSet(Port, Port->TxHalf, SmdFieldBlockReadIntr, 0);
    Port->TxBlocked = TRUE;
}

/* Writes a gather list as one packet or as stream bytes, Total is the sum of the pieces */
static
LONG
SmdWriteGather(
    _Inout_ PSMD_PORT Port,
    _In_ PQCOM_SMDL_IOVEC Vector,
    _In_ ULONG Total,
    _In_ ULONG Flags)
{
    ULONG Header[SMD_PACKET_HEADER_SIZE / sizeof(ULONG)] = { 0 };
    ULONG Free, Head, Remaining, Piece;
    PQCOM_SMDL_IOVEC Item;

    if (!SmdCanWrite(Port))
        return 0;

    Free = SmdTxFree(Port);
    Head = SmdHalfGet(Port, Port->TxHalf, SmdFieldHead);

    if (!Port->Stream)
    {
        if (Total + SMD_PACKET_HEADER_SIZE > Port->FifoSize - 4)
            return -1;

        if (Total + SMD_PACKET_HEADER_SIZE > Free)
        {
            SmdMarkBlocked(Port);
            return 0;
        }

        Header[0] = Total;
        Head = SmdFifoWrite(Port, Head, Header, sizeof(Header));
        Remaining = Total;
    }
    else
    {
        Remaining = min(Total, Free);
        if (Remaining < Total)
            SmdMarkBlocked(Port);
        if (Remaining == 0)
            return 0;
    }

    Total = Remaining;
    for (Item = Vector; Item && Remaining; Item = Item->Next)
    {
        Piece = min(Item->Length, Remaining);
        Head = SmdFifoWrite(Port, Head, Item->Buffer, Piece);
        Remaining -= Piece;
    }

    if (!Port->TxBlocked)
        SmdHalfSet(Port, Port->TxHalf, SmdFieldBlockReadIntr, 1);

    SmdPublishWrite(Port, Head, Flags);
    return (LONG)Total;
}

static
LONG
NTAPI
SmdlWrite(
    _In_ QCOM_SMDL_HANDLE Handle,
    _In_ ULONG Length,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Flags)
{
    PSMD_PORT Port = Handle;
    QCOM_SMDL_IOVEC Single;
    PSMD_HOST Host;
    LONG Result;

    if (!Port || !Buffer)
        return -1;

    if (Length == 0)
        return 0;

    Single.Next = NULL;
    Single.Length = Length;
    Single.Buffer = (PVOID)Buffer;

    Host = &Port->Device->Hosts[Port->RemoteHost];
    SmdRLockAcquire(&Host->Lock);
    Result = SmdWriteGather(Port, &Single, Length, Flags);
    SmdRLockRelease(&Host->Lock);

    return Result;
}

static
LONG
NTAPI
SmdlWriteV(
    _In_ QCOM_SMDL_HANDLE Handle,
    _In_ PQCOM_SMDL_IOVEC Vector,
    _In_ ULONG Flags)
{
    PSMD_PORT Port = Handle;
    PQCOM_SMDL_IOVEC Item;
    PSMD_HOST Host;
    ULONG Total = 0;
    LONG Result;

    if (!Port || !Vector)
        return -1;

    for (Item = Vector; Item; Item = Item->Next)
    {
        if (!Item->Buffer && Item->Length)
            return -1;
        Total += Item->Length;
    }

    if (Total == 0)
        return 0;

    Host = &Port->Device->Hosts[Port->RemoteHost];
    SmdRLockAcquire(&Host->Lock);
    Result = SmdWriteGather(Port, Vector, Total, Flags & ~QCOM_SMDL_IOV_PARTIAL);
    SmdRLockRelease(&Host->Lock);

    return Result;
}

/* Peeks the next packet's length without consuming it, FALSE if no complete header is there */
static
BOOLEAN
SmdPeekPacket(
    _In_ PSMD_PORT Port,
    _Out_ PULONG PacketLength)
{
    ULONG Header[SMD_PACKET_HEADER_SIZE / sizeof(ULONG)];

    if (SmdRxAvailable(Port) < SMD_PACKET_HEADER_SIZE)
        return FALSE;

    SmdFifoRead(Port, SmdHalfGet(Port, Port->RxHalf, SmdFieldTail), Header, sizeof(Header));
    *PacketLength = Header[0];
    return TRUE;
}

static
VOID
SmdFinishRead(
    _Inout_ PSMD_PORT Port,
    _In_ ULONG NewTail)
{
    KeMemoryBarrier();
    SmdHalfSet(Port, Port->RxHalf, SmdFieldTail, NewTail);

    /* The writer asked to hear about freed space */
    if (SmdHalfGet(Port, Port->RxHalf, SmdFieldBlockReadIntr) == 0)
    {
        KeMemoryBarrier();
        SmdHalfSet(Port, Port->TxHalf, SmdFieldTailFlag, 1);
        SmdSignalHost(Port->Device, Port->RemoteHost);
    }
}

/* Scatters the next packet, or the available stream bytes, into the list */
static
LONG
SmdReadScatter(
    _Inout_ PSMD_PORT Port,
    _In_ PQCOM_SMDL_IOVEC Vector,
    _In_ ULONG Capacity)
{
    ULONG Tail, PacketLength, Copy, Remaining, Piece, Skip;
    PQCOM_SMDL_IOVEC Item;

    if (Port->LocalState != SMD_STATE_OPENED && Port->LocalState != SMD_STATE_FLUSHING)
        return 0;

    Tail = SmdHalfGet(Port, Port->RxHalf, SmdFieldTail);

    if (!Port->Stream)
    {
        if (!SmdPeekPacket(Port, &PacketLength))
            return 0;

        if (SmdRxAvailable(Port) < SMD_PACKET_HEADER_SIZE + PacketLength)
            return 0;

        Tail = SmdFifoRead(Port, Tail, NULL, SMD_PACKET_HEADER_SIZE);
        Copy = min(PacketLength, Capacity);
        Skip = PacketLength - Copy;
    }
    else
    {
        Copy = min(SmdRxAvailable(Port), Capacity);
        Skip = 0;
        if (Copy == 0)
            return 0;
    }

    Remaining = Copy;
    for (Item = Vector; Item && Remaining; Item = Item->Next)
    {
        Piece = min(Item->Length, Remaining);
        Tail = SmdFifoRead(Port, Tail, Item->Buffer, Piece);
        Remaining -= Piece;
    }

    /* The rest of a packet that did not fit is dropped */
    if (Skip)
        Tail = SmdFifoRead(Port, Tail, NULL, Skip);

    SmdFinishRead(Port, Tail);
    return (LONG)Copy;
}

static
LONG
NTAPI
SmdlRead(
    _In_ QCOM_SMDL_HANDLE Handle,
    _In_ ULONG Length,
    _Out_writes_bytes_(Length) PVOID Buffer)
{
    PSMD_PORT Port = Handle;
    QCOM_SMDL_IOVEC Single;
    PSMD_HOST Host;
    LONG Result;

    if (!Port || !Buffer || Length == 0)
        return -1;

    Single.Next = NULL;
    Single.Length = Length;
    Single.Buffer = Buffer;

    Host = &Port->Device->Hosts[Port->RemoteHost];
    SmdRLockAcquire(&Host->Lock);
    Result = SmdReadScatter(Port, &Single, Length);
    SmdRLockRelease(&Host->Lock);

    return Result;
}

static
LONG
NTAPI
SmdlReadV(
    _In_ QCOM_SMDL_HANDLE Handle,
    _In_ PQCOM_SMDL_IOVEC Vector,
    _In_ ULONG Flags)
{
    PSMD_PORT Port = Handle;
    PQCOM_SMDL_IOVEC Item;
    PSMD_HOST Host;
    ULONG Capacity = 0;
    LONG Result;

    UNREFERENCED_PARAMETER(Flags);

    if (!Port || !Vector)
        return -1;

    for (Item = Vector; Item; Item = Item->Next)
        Capacity += Item->Length;

    if (Capacity == 0)
        return -1;

    Host = &Port->Device->Hosts[Port->RemoteHost];
    SmdRLockAcquire(&Host->Lock);
    Result = SmdReadScatter(Port, Vector, Capacity);
    SmdRLockRelease(&Host->Lock);

    return Result;
}

static
LONG
NTAPI
SmdlTxPeek(
    _In_ QCOM_SMDL_HANDLE Handle)
{
    PSMD_PORT Port = Handle;
    PSMD_HOST Host;
    LONG Result = 0;
    ULONG Free;

    if (!Port)
        return -2;

    Host = &Port->Device->Hosts[Port->RemoteHost];
    SmdRLockAcquire(&Host->Lock);

    if (SmdCanWrite(Port))
    {
        Free = SmdTxFree(Port);
        if (!Port->Stream)
            Free = (Free > SMD_PACKET_HEADER_SIZE) ? Free - SMD_PACKET_HEADER_SIZE : 0;
        Result = (LONG)Free;
    }

    SmdRLockRelease(&Host->Lock);
    return Result;
}

static
LONG
NTAPI
SmdlRxPeek(
    _In_ QCOM_SMDL_HANDLE Handle)
{
    PSMD_PORT Port = Handle;
    PSMD_HOST Host;
    ULONG PacketLength;
    LONG Result = 0;

    if (!Port)
        return -2;

    Host = &Port->Device->Hosts[Port->RemoteHost];
    SmdRLockAcquire(&Host->Lock);

    if (Port->LocalState == SMD_STATE_OPENED || Port->LocalState == SMD_STATE_FLUSHING)
    {
        if (Port->Stream)
            Result = (LONG)SmdRxAvailable(Port);
        else if (SmdPeekPacket(Port, &PacketLength))
            Result = (LONG)PacketLength;
    }

    SmdRLockRelease(&Host->Lock);
    return Result;
}

static
LONG
NTAPI
SmdlSigSet(
    _In_ QCOM_SMDL_HANDLE Handle,
    _In_ ULONG Signal,
    _In_ ULONG Value)
{
    static const SMD_FIELD Fields[4] = { SmdFieldDsr, SmdFieldCts, SmdFieldCd, SmdFieldRi };
    PSMD_PORT Port = Handle;
    PSMD_HOST Host;

    if (!Port || Signal > QCOM_SMDL_SIG_RI || Value > 1)
        return -1;

    Host = &Port->Device->Hosts[Port->RemoteHost];
    SmdRLockAcquire(&Host->Lock);

    if (SmdHalfGet(Port, Port->TxHalf, Fields[Signal]) != Value)
    {
        SmdHalfSet(Port, Port->TxHalf, Fields[Signal], Value);
        SmdRaiseStateFlag(Port);
    }

    SmdRLockRelease(&Host->Lock);
    return 0;
}

static
LONG
NTAPI
SmdlSigGet(
    _In_ QCOM_SMDL_HANDLE Handle,
    _In_ ULONG Signal)
{
    static const SMD_FIELD Fields[4] = { SmdFieldDsr, SmdFieldCts, SmdFieldCd, SmdFieldRi };
    PSMD_PORT Port = Handle;

    if (!Port || Signal > QCOM_SMDL_SIG_RI)
        return -1;

    return SmdHalfGet(Port, Port->RxHalf, Fields[Signal]) != 0;
}

const QCOM_SMDL_INTERFACE SmdlInterfaceTemplate =
{
    { sizeof(QCOM_SMDL_INTERFACE), 0, NULL, NULL, NULL },
    SmdlOpen,
    SmdlClose,
    SmdlRead,
    SmdlReadV,
    SmdlWrite,
    SmdlWriteV,
    SmdlTxPeek,
    SmdlRxPeek,
    SmdlSigSet,
    SmdlSigGet
};

NTSTATUS
SmdInitialize(
    _Inout_ PSMD_DEVICE Device)
{
    PULONG Features;
    ULONG Size = sizeof(ULONG) * 8;

    if (!SmemIfVersionSet(SMEM_ITEM_SMD_VERSION, SMD_PROTOCOL_VERSION, 0xFFFFFFFF))
        DPRINT1("SMD protocol version disagrees with a remote processor\n");

    /* Tell the remotes which optional features the apps side understands */
    if (SmemAllocate(Device, QCOM_HOST_GLOBAL, SMEM_ITEM_SMD_FEATURES, &Size, 0, (PVOID *)&Features) == SMEM_OK)
        Features[QCOM_HOST_APPS] = 1;

    return STATUS_SUCCESS;
}
