/*
 * PROJECT:     ReactOS Bluetooth probe
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     L2CAP basic mode channels and the signaling channel
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "bthprobe.h"

#define L2CAP_RESULT_SUCCESS                0x0000
#define L2CAP_RESULT_PENDING                0x0001
#define L2CAP_RESULT_PSM_NOT_SUPPORTED      0x0002
#define L2CAP_RESULT_SECURITY_BLOCK         0x0003
#define L2CAP_RESULT_NO_RESOURCES           0x0004
#define L2CAP_RESULT_REJECTED               0xFFFF
#define L2CAP_RESULT_TIMEOUT                0xFFFE

#define L2CAP_CONFIG_SUCCESS                0x0000
#define L2CAP_CONFIG_UNACCEPTABLE           0x0001
#define L2CAP_CONFIG_UNKNOWN_OPTIONS        0x0003
#define L2CAP_CONFIG_PENDING                0x0004

#define L2CAP_OPTION_MTU                    0x01
#define L2CAP_OPTION_FLUSH_TIMEOUT          0x02
#define L2CAP_OPTION_QOS                    0x03
#define L2CAP_OPTION_RETRANSMISSION         0x04
#define L2CAP_OPTION_FCS                    0x05
#define L2CAP_OPTION_HINT                   0x80

#define L2CAP_INFO_EXTENDED_FEATURES        0x0002
#define L2CAP_REJECT_NOT_UNDERSTOOD         0x0000
#define L2CAP_REJECT_INVALID_CID            0x0002

static
const CHAR *
L2capResultName(
    _In_ USHORT Result)
{
    switch (Result)
    {
        case L2CAP_RESULT_PSM_NOT_SUPPORTED:
            return "is not offered by the device";
        case L2CAP_RESULT_SECURITY_BLOCK:
            return "was refused for security, pair the device first";
        case L2CAP_RESULT_NO_RESOURCES:
            return "was refused, the device is out of resources";
        case L2CAP_RESULT_REJECTED:
            return "was rejected as a command";
        case L2CAP_RESULT_TIMEOUT:
            return "got no answer";
        default:
            return "failed";
    }
}

static
UCHAR
L2capNextId(
    _Inout_ PBT_RADIO Radio)
{
    UCHAR Id;

    Id = Radio->NextSignalId++;
    if (Radio->NextSignalId == 0)
        Radio->NextSignalId = 1;

    return Id;
}

static
USHORT
L2capNextCid(
    _Inout_ PBT_RADIO Radio)
{
    USHORT Cid;

    Cid = Radio->NextCid++;
    if (Radio->NextCid == 0)
        Radio->NextCid = L2CAP_CID_DYNAMIC_FIRST;

    return Cid;
}

static
PL2CAP_CHANNEL
L2capAllocate(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT Handle,
    _In_ USHORT Psm,
    _In_ PL2CAP_RECEIVE_ROUTINE Receive,
    _In_opt_ PVOID Context)
{
    PL2CAP_CHANNEL Channel;
    ULONG Index;

    for (Index = 0; Index < L2CAP_MAX_CHANNELS; Index++)
    {
        Channel = &Radio->Channels[Index];
        if (Channel->InUse)
            continue;

        ZeroMemory(Channel, sizeof(*Channel));
        Channel->InUse = TRUE;
        Channel->Handle = Handle;
        Channel->Psm = Psm;
        Channel->LocalCid = L2capNextCid(Radio);
        Channel->RemoteMtu = L2CAP_DEFAULT_MTU;
        Channel->Receive = Receive;
        Channel->Context = Context;

        return Channel;
    }

    return NULL;
}

/* The slot stays readable after closing so a waiter can see why */
static
VOID
L2capClose(
    _Inout_ PL2CAP_CHANNEL Channel,
    _In_ USHORT Result)
{
    Channel->Result = Result;
    Channel->State = L2capClosed;
    Channel->InUse = FALSE;
}

static
PL2CAP_CHANNEL
L2capFindLocal(
    _In_ PBT_RADIO Radio,
    _In_ USHORT Handle,
    _In_ USHORT Cid)
{
    ULONG Index;

    for (Index = 0; Index < L2CAP_MAX_CHANNELS; Index++)
    {
        if (Radio->Channels[Index].InUse &&
            Radio->Channels[Index].Handle == Handle &&
            Radio->Channels[Index].LocalCid == Cid)
        {
            return &Radio->Channels[Index];
        }
    }

    return NULL;
}

static
PL2CAP_CHANNEL
L2capFindPending(
    _In_ PBT_RADIO Radio,
    _In_ USHORT Handle,
    _In_ UCHAR Id)
{
    ULONG Index;

    for (Index = 0; Index < L2CAP_MAX_CHANNELS; Index++)
    {
        if (Radio->Channels[Index].InUse &&
            Radio->Channels[Index].Handle == Handle &&
            Radio->Channels[Index].PendingId == Id &&
            (Radio->Channels[Index].State == L2capWaitConnect || Radio->Channels[Index].State == L2capConfig))
        {
            return &Radio->Channels[Index];
        }
    }

    return NULL;
}

PL2CAP_CHANNEL
L2capFindOpen(
    _In_ PBT_RADIO Radio,
    _In_ USHORT Handle,
    _In_ USHORT Psm)
{
    ULONG Index;

    for (Index = 0; Index < L2CAP_MAX_CHANNELS; Index++)
    {
        if (Radio->Channels[Index].InUse &&
            Radio->Channels[Index].State == L2capOpen &&
            Radio->Channels[Index].Handle == Handle &&
            Radio->Channels[Index].Psm == Psm)
        {
            return &Radio->Channels[Index];
        }
    }

    return NULL;
}

static
BOOLEAN
L2capSendPdu(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT Handle,
    _In_ USHORT Cid,
    _In_reads_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    UCHAR Pdu[4 + L2CAP_MAX_PDU];

    if (Length > L2CAP_MAX_PDU)
        return FALSE;

    BtWrite16(&Pdu[0], (USHORT)Length);
    BtWrite16(&Pdu[2], Cid);
    CopyMemory(&Pdu[4], Data, Length);

    return HciSendAcl(Radio, Handle, Pdu, 4 + Length);
}

static
VOID
L2capSendSignal(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT Handle,
    _In_ UCHAR Code,
    _In_ UCHAR Id,
    _In_reads_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    UCHAR Command[4 + L2CAP_MIN_MTU];

    /* Signaling commands must fit the default signaling MTU */
    if (Length > L2CAP_MIN_MTU)
        Length = L2CAP_MIN_MTU;

    Command[0] = Code;
    Command[1] = Id;
    BtWrite16(&Command[2], (USHORT)Length);
    if (Length != 0)
        CopyMemory(&Command[4], Data, Length);

    if (Radio->Verbose)
        printf("  > l2cap signal 0x%02X id %u\n", Code, Id);

    L2capSendPdu(Radio, Handle, L2CAP_CID_SIGNALING, Command, 4 + Length);
}

static
VOID
L2capSendConfigRequest(
    _Inout_ PBT_RADIO Radio,
    _Inout_ PL2CAP_CHANNEL Channel,
    _In_ BOOLEAN WithMtu)
{
    UCHAR Data[8];
    ULONG Length;

    BtWrite16(&Data[0], Channel->RemoteCid);
    BtWrite16(&Data[2], 0x0000);
    Length = 4;

    if (WithMtu)
    {
        Data[4] = L2CAP_OPTION_MTU;
        Data[5] = 2;
        BtWrite16(&Data[6], L2CAP_LOCAL_MTU);
        Length = 8;
    }

    Channel->PendingId = L2capNextId(Radio);
    L2capSendSignal(Radio, Channel->Handle, L2CAP_CONFIGURE_REQUEST, Channel->PendingId, Data, Length);
}

static
VOID
L2capCheckOpen(
    _Inout_ PBT_RADIO Radio,
    _Inout_ PL2CAP_CHANNEL Channel)
{
    if (Channel->State != L2capConfig || !Channel->LocalConfigDone || !Channel->RemoteConfigDone)
        return;

    Channel->State = L2capOpen;

    if (Radio->Verbose)
        printf("  L2CAP PSM 0x%04X open, local CID 0x%04X, remote CID 0x%04X\n",
               Channel->Psm, Channel->LocalCid, Channel->RemoteCid);
}

static
VOID
L2capRejectCid(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT Handle,
    _In_ UCHAR Id,
    _In_ USHORT LocalCid,
    _In_ USHORT RemoteCid)
{
    UCHAR Data[6];

    BtWrite16(&Data[0], L2CAP_REJECT_INVALID_CID);
    BtWrite16(&Data[2], LocalCid);
    BtWrite16(&Data[4], RemoteCid);
    L2capSendSignal(Radio, Handle, L2CAP_COMMAND_REJECT, Id, Data, sizeof(Data));
}

static
PL2CAP_LISTENER
L2capFindListener(
    _In_ PBT_RADIO Radio,
    _In_ USHORT Psm,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address)
{
    ULONG Index;

    for (Index = 0; Index < L2CAP_MAX_LISTENERS; Index++)
    {
        if (Radio->Listeners[Index].InUse &&
            Radio->Listeners[Index].Psm == Psm &&
            memcmp(Radio->Listeners[Index].Address, Address, BT_ADDRESS_LENGTH) == 0)
        {
            return &Radio->Listeners[Index];
        }
    }

    return NULL;
}

static
VOID
L2capConnectionRequest(
    _Inout_ PBT_RADIO Radio,
    _In_ PBT_LINK Link,
    _In_ UCHAR Id,
    _In_reads_(4) const UCHAR *Data)
{
    PL2CAP_LISTENER Listener;
    PL2CAP_CHANNEL Channel;
    UCHAR Response[8];
    USHORT Result;
    USHORT Psm;

    Psm = BT_READ16(&Data[0]);
    Channel = NULL;
    Result = L2CAP_RESULT_PSM_NOT_SUPPORTED;

    Listener = L2capFindListener(Radio, Psm, Link->Address);
    if (Listener != NULL)
    {
        Channel = L2capAllocate(Radio, Link->Handle, Psm, Listener->Receive, Listener->Context);
        Result = (Channel != NULL) ? L2CAP_RESULT_SUCCESS : L2CAP_RESULT_NO_RESOURCES;
    }

    BtWrite16(&Response[0], (Channel != NULL) ? Channel->LocalCid : 0);
    BtWrite16(&Response[2], BT_READ16(&Data[2]));
    BtWrite16(&Response[4], Result);
    BtWrite16(&Response[6], 0x0000);
    L2capSendSignal(Radio, Link->Handle, L2CAP_CONNECTION_RESPONSE, Id, Response, sizeof(Response));

    if (Channel == NULL)
    {
        if (Radio->Verbose)
            printf("  refused incoming L2CAP PSM 0x%04X\n", Psm);
        return;
    }

    Channel->RemoteCid = BT_READ16(&Data[2]);
    Channel->State = L2capConfig;
    L2capSendConfigRequest(Radio, Channel, TRUE);
}

static
VOID
L2capConnectionResponse(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT Handle,
    _In_ UCHAR Id,
    _In_reads_(8) const UCHAR *Data)
{
    PL2CAP_CHANNEL Channel;
    USHORT Result;

    Result = BT_READ16(&Data[4]);

    /* A refusal may echo a zero source CID, so fall back to the identifier */
    Channel = L2capFindLocal(Radio, Handle, BT_READ16(&Data[2]));
    if (Channel == NULL)
        Channel = L2capFindPending(Radio, Handle, Id);

    if (Channel == NULL || Channel->State != L2capWaitConnect)
        return;

    if (Result == L2CAP_RESULT_SUCCESS)
    {
        Channel->RemoteCid = BT_READ16(&Data[0]);
        Channel->State = L2capConfig;
        L2capSendConfigRequest(Radio, Channel, TRUE);
    }
    else if (Result != L2CAP_RESULT_PENDING)
    {
        L2capClose(Channel, Result);
    }
}

static
VOID
L2capConfigureRequest(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT Handle,
    _In_ UCHAR Id,
    _In_reads_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    UCHAR Response[6 + 32];
    UCHAR Unknown[24];
    const UCHAR *Options;
    PL2CAP_CHANNEL Channel;
    ULONG OptionLength;
    ULONG UnknownLength;
    ULONG ResponseLength;
    ULONG Offset;
    USHORT Result;
    USHORT Flags;
    UCHAR Type;
    UCHAR Size;

    Channel = L2capFindLocal(Radio, Handle, BT_READ16(&Data[0]));
    if (Channel == NULL || (Channel->State != L2capConfig && Channel->State != L2capOpen))
    {
        L2capRejectCid(Radio, Handle, Id, BT_READ16(&Data[0]), 0);
        return;
    }

    Flags = BT_READ16(&Data[2]);
    Options = &Data[4];
    OptionLength = Length - 4;
    UnknownLength = 0;
    Result = L2CAP_CONFIG_SUCCESS;

    for (Offset = 0; Offset + 2 <= OptionLength; Offset += 2 + Size)
    {
        Type = Options[Offset];
        Size = Options[Offset + 1];
        if (Offset + 2 + Size > OptionLength)
            break;

        switch (Type & ~L2CAP_OPTION_HINT)
        {
            case L2CAP_OPTION_MTU:
                if (Size >= 2)
                    Channel->RemoteMtu = BT_READ16(&Options[Offset + 2]);
                break;

            case L2CAP_OPTION_FLUSH_TIMEOUT:
            case L2CAP_OPTION_QOS:
            case L2CAP_OPTION_FCS:
                break;

            case L2CAP_OPTION_RETRANSMISSION:
                /* Only basic mode is spoken here */
                if (Size >= 1 && Options[Offset + 2] != 0x00)
                    Result = L2CAP_CONFIG_UNACCEPTABLE;
                break;

            default:
                if (!(Type & L2CAP_OPTION_HINT) && UnknownLength + 2 + Size <= sizeof(Unknown))
                {
                    CopyMemory(&Unknown[UnknownLength], &Options[Offset], 2 + Size);
                    UnknownLength += 2 + Size;
                }
                break;
        }
    }

    if (UnknownLength != 0)
        Result = L2CAP_CONFIG_UNKNOWN_OPTIONS;

    BtWrite16(&Response[0], Channel->RemoteCid);
    BtWrite16(&Response[2], Flags & 0x0001);
    BtWrite16(&Response[4], Result);
    ResponseLength = 6;

    if (Result == L2CAP_CONFIG_UNACCEPTABLE)
    {
        /* Counter with basic mode, every other field zero */
        ZeroMemory(&Response[6], 11);
        Response[6] = L2CAP_OPTION_RETRANSMISSION;
        Response[7] = 9;
        ResponseLength += 11;
    }
    else if (Result == L2CAP_CONFIG_UNKNOWN_OPTIONS)
    {
        CopyMemory(&Response[6], Unknown, UnknownLength);
        ResponseLength += UnknownLength;
    }

    L2capSendSignal(Radio, Handle, L2CAP_CONFIGURE_RESPONSE, Id, Response, ResponseLength);

    if (Result == L2CAP_CONFIG_SUCCESS && !(Flags & 0x0001))
    {
        Channel->RemoteConfigDone = TRUE;
        L2capCheckOpen(Radio, Channel);
    }
}

static
VOID
L2capConfigureResponse(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT Handle,
    _In_reads_(6) const UCHAR *Data)
{
    PL2CAP_CHANNEL Channel;
    UCHAR Disconnect[4];
    USHORT Result;
    USHORT Flags;

    Channel = L2capFindLocal(Radio, Handle, BT_READ16(&Data[0]));
    if (Channel == NULL || Channel->State != L2capConfig)
        return;

    Flags = BT_READ16(&Data[2]);
    Result = BT_READ16(&Data[4]);

    if (Result == L2CAP_CONFIG_SUCCESS)
    {
        if (!(Flags & 0x0001))
        {
            Channel->LocalConfigDone = TRUE;
            L2capCheckOpen(Radio, Channel);
        }

        return;
    }

    if (Result == L2CAP_CONFIG_PENDING)
        return;

    /* Try once more with nothing but defaults before giving up */
    if (!Channel->ConfigRetried)
    {
        Channel->ConfigRetried = TRUE;
        L2capSendConfigRequest(Radio, Channel, FALSE);
        return;
    }

    printf("  L2CAP PSM 0x%04X configuration refused, result %u\n", Channel->Psm, Result);

    BtWrite16(&Disconnect[0], Channel->RemoteCid);
    BtWrite16(&Disconnect[2], Channel->LocalCid);
    L2capSendSignal(Radio, Handle, L2CAP_DISCONNECTION_REQUEST, L2capNextId(Radio), Disconnect, sizeof(Disconnect));
    L2capClose(Channel, Result);
}

static
VOID
L2capInformationRequest(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT Handle,
    _In_ UCHAR Id,
    _In_reads_(2) const UCHAR *Data)
{
    UCHAR Response[8];
    ULONG Length;
    USHORT Type;

    Type = BT_READ16(&Data[0]);
    BtWrite16(&Response[0], Type);

    if (Type == L2CAP_INFO_EXTENDED_FEATURES)
    {
        /* Basic mode only, no fixed channels beyond signaling */
        BtWrite16(&Response[2], 0x0000);
        ZeroMemory(&Response[4], 4);
        Length = 8;
    }
    else
    {
        BtWrite16(&Response[2], 0x0001);
        Length = 4;
    }

    L2capSendSignal(Radio, Handle, L2CAP_INFORMATION_RESPONSE, Id, Response, Length);
}

static
VOID
L2capSignaling(
    _Inout_ PBT_RADIO Radio,
    _In_ PBT_LINK Link,
    _In_reads_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    PL2CAP_CHANNEL Channel;
    const UCHAR *Body;
    UCHAR Reply[4];
    ULONG Offset;
    ULONG Size;
    UCHAR Code;
    UCHAR Id;

    /* One signaling PDU can carry several commands back to back */
    for (Offset = 0; Offset + 4 <= Length; Offset += 4 + Size)
    {
        Code = Data[Offset];
        Id = Data[Offset + 1];
        Size = BT_READ16(&Data[Offset + 2]);
        Body = &Data[Offset + 4];

        if (Offset + 4 + Size > Length)
            break;

        if (Radio->Verbose)
            printf("  < l2cap signal 0x%02X id %u\n", Code, Id);

        switch (Code)
        {
            case L2CAP_CONNECTION_REQUEST:
                if (Size >= 4)
                    L2capConnectionRequest(Radio, Link, Id, Body);
                break;

            case L2CAP_CONNECTION_RESPONSE:
                if (Size >= 8)
                    L2capConnectionResponse(Radio, Link->Handle, Id, Body);
                break;

            case L2CAP_CONFIGURE_REQUEST:
                if (Size >= 4)
                    L2capConfigureRequest(Radio, Link->Handle, Id, Body, Size);
                break;

            case L2CAP_CONFIGURE_RESPONSE:
                if (Size >= 6)
                    L2capConfigureResponse(Radio, Link->Handle, Body);
                break;

            case L2CAP_DISCONNECTION_REQUEST:
                if (Size < 4)
                    break;

                /* The request names our CID first, the response echoes both */
                Channel = L2capFindLocal(Radio, Link->Handle, BT_READ16(&Body[0]));
                CopyMemory(Reply, Body, sizeof(Reply));
                L2capSendSignal(Radio, Link->Handle, L2CAP_DISCONNECTION_RESPONSE, Id, Reply, sizeof(Reply));

                if (Channel != NULL)
                    L2capClose(Channel, L2CAP_RESULT_SUCCESS);
                break;

            case L2CAP_DISCONNECTION_RESPONSE:
                if (Size >= 4 && (Channel = L2capFindLocal(Radio, Link->Handle, BT_READ16(&Body[2]))) != NULL)
                    L2capClose(Channel, L2CAP_RESULT_SUCCESS);
                break;

            case L2CAP_ECHO_REQUEST:
                L2capSendSignal(Radio, Link->Handle, L2CAP_ECHO_RESPONSE, Id, Body, Size);
                break;

            case L2CAP_INFORMATION_REQUEST:
                if (Size >= 2)
                    L2capInformationRequest(Radio, Link->Handle, Id, Body);
                break;

            case L2CAP_COMMAND_REJECT:
                if ((Channel = L2capFindPending(Radio, Link->Handle, Id)) != NULL)
                    L2capClose(Channel, L2CAP_RESULT_REJECTED);
                break;

            case L2CAP_ECHO_RESPONSE:
            case L2CAP_INFORMATION_RESPONSE:
                break;

            default:
                BtWrite16(Reply, L2CAP_REJECT_NOT_UNDERSTOOD);
                L2capSendSignal(Radio, Link->Handle, L2CAP_COMMAND_REJECT, Id, Reply, 2);
                break;
        }
    }
}

static
VOID
L2capDispatch(
    _Inout_ PBT_RADIO Radio,
    _In_ PBT_LINK Link,
    _In_ USHORT Cid,
    _In_reads_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    PL2CAP_CHANNEL Channel;

    if (Cid == L2CAP_CID_SIGNALING)
    {
        L2capSignaling(Radio, Link, Data, Length);
        return;
    }

    if (Cid < L2CAP_CID_DYNAMIC_FIRST)
        return;

    Channel = L2capFindLocal(Radio, Link->Handle, Cid);
    if (Channel != NULL && Channel->State == L2capOpen && Channel->Receive != NULL)
        Channel->Receive(Radio, Channel, Data, Length);
}

VOID
L2capReceiveAcl(
    _Inout_ PBT_RADIO Radio,
    _In_reads_(Length) const UCHAR *Packet,
    _In_ ULONG Length)
{
    PBT_LINK Link;
    ULONG DataLength;
    ULONG Total;
    UCHAR Boundary;

    if (Length < 4)
        return;

    Link = BtFindLinkByHandle(Radio, BT_READ16(&Packet[0]) & 0x0FFF);
    if (Link == NULL)
        return;

    Boundary = (Packet[1] >> 4) & 0x03;
    DataLength = min(BT_READ16(&Packet[2]), Length - 4);

    if (Boundary == HCI_ACL_CONTINUE)
    {
        if (Link->RxLength == 0)
            return;

        if (Link->RxLength + DataLength > sizeof(Link->Rx))
        {
            printf("  L2CAP PDU too large, dropping it\n");
            Link->RxLength = 0;
            return;
        }

        CopyMemory(&Link->Rx[Link->RxLength], &Packet[4], DataLength);
        Link->RxLength += DataLength;
    }
    else
    {
        if (DataLength > sizeof(Link->Rx))
            return;

        CopyMemory(Link->Rx, &Packet[4], DataLength);
        Link->RxLength = DataLength;
    }

    if (Link->RxLength < 4)
        return;

    Total = 4 + BT_READ16(&Link->Rx[0]);
    if (Total > sizeof(Link->Rx))
    {
        printf("  L2CAP PDU too large, dropping it\n");
        Link->RxLength = 0;
        return;
    }

    if (Link->RxLength < Total)
        return;

    Link->RxLength = 0;
    L2capDispatch(Radio, Link, BT_READ16(&Link->Rx[2]), &Link->Rx[4], Total - 4);
}

VOID
L2capLinkClosed(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT Handle)
{
    ULONG Index;

    for (Index = 0; Index < L2CAP_MAX_CHANNELS; Index++)
    {
        if (Radio->Channels[Index].InUse && Radio->Channels[Index].Handle == Handle)
            L2capClose(&Radio->Channels[Index], L2CAP_RESULT_SUCCESS);
    }
}

BOOLEAN
L2capListen(
    _Inout_ PBT_RADIO Radio,
    _In_ USHORT Psm,
    _In_reads_(BT_ADDRESS_LENGTH) const UCHAR *Address,
    _In_ PL2CAP_RECEIVE_ROUTINE Receive,
    _In_opt_ PVOID Context)
{
    ULONG Index;

    for (Index = 0; Index < L2CAP_MAX_LISTENERS; Index++)
    {
        if (Radio->Listeners[Index].InUse)
            continue;

        Radio->Listeners[Index].InUse = TRUE;
        Radio->Listeners[Index].Psm = Psm;
        Radio->Listeners[Index].Receive = Receive;
        Radio->Listeners[Index].Context = Context;
        CopyMemory(Radio->Listeners[Index].Address, Address, BT_ADDRESS_LENGTH);

        return TRUE;
    }

    return FALSE;
}

PL2CAP_CHANNEL
L2capConnect(
    _Inout_ PBT_RADIO Radio,
    _In_ PBT_LINK Link,
    _In_ USHORT Psm,
    _In_ PL2CAP_RECEIVE_ROUTINE Receive,
    _In_opt_ PVOID Context)
{
    PL2CAP_CHANNEL Channel;
    UCHAR Data[4];
    DWORD Deadline;
    USHORT Cid;

    Channel = L2capAllocate(Radio, Link->Handle, Psm, Receive, Context);
    if (Channel == NULL)
    {
        printf("  no free L2CAP channel\n");
        return NULL;
    }

    Cid = Channel->LocalCid;
    Channel->State = L2capWaitConnect;
    Channel->PendingId = L2capNextId(Radio);

    BtWrite16(&Data[0], Psm);
    BtWrite16(&Data[2], Cid);
    L2capSendSignal(Radio, Link->Handle, L2CAP_CONNECTION_REQUEST, Channel->PendingId, Data, sizeof(Data));

    Deadline = GetTickCount() + BT_SHORT_TIMEOUT;

    /* The CID check guards against the slot being handed to a new channel */
    while (Channel->InUse && Channel->LocalCid == Cid && Channel->State != L2capOpen)
    {
        if (HciPump(Radio, BtRemaining(Deadline)) == HciItemError ||
            BtStopRequested ||
            BtExpired(Deadline) ||
            !Link->InUse)
        {
            break;
        }
    }

    if (Channel->InUse && Channel->LocalCid == Cid && Channel->State == L2capOpen)
        return Channel;

    if (Channel->InUse && Channel->LocalCid == Cid)
        L2capClose(Channel, L2CAP_RESULT_TIMEOUT);

    printf("  L2CAP PSM 0x%04X %s\n", Psm, L2capResultName(Channel->Result));

    return NULL;
}

BOOLEAN
L2capSend(
    _Inout_ PBT_RADIO Radio,
    _In_ PL2CAP_CHANNEL Channel,
    _In_reads_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    if (!Channel->InUse || Channel->State != L2capOpen || Length > Channel->RemoteMtu)
        return FALSE;

    return L2capSendPdu(Radio, Channel->Handle, Channel->RemoteCid, Data, Length);
}

VOID
L2capDisconnect(
    _Inout_ PBT_RADIO Radio,
    _In_ PL2CAP_CHANNEL Channel)
{
    UCHAR Data[4];
    DWORD Deadline;
    USHORT Cid;

    if (!Channel->InUse)
        return;

    if (Channel->State == L2capOpen || Channel->State == L2capConfig)
    {
        Cid = Channel->LocalCid;
        Channel->State = L2capWaitDisconnect;

        BtWrite16(&Data[0], Channel->RemoteCid);
        BtWrite16(&Data[2], Cid);
        L2capSendSignal(Radio, Channel->Handle, L2CAP_DISCONNECTION_REQUEST, L2capNextId(Radio), Data, sizeof(Data));

        Deadline = GetTickCount() + 3000;
        while (Channel->InUse && Channel->LocalCid == Cid && !BtExpired(Deadline))
        {
            if (HciPump(Radio, BtRemaining(Deadline)) == HciItemError)
                break;
        }

        if (Channel->LocalCid != Cid)
            return;
    }

    L2capClose(Channel, L2CAP_RESULT_SUCCESS);
}
