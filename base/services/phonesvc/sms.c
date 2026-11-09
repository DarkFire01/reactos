/*
 * PROJECT:     ReactOS Phone Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SMS encoding, sending and reception
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "phonesvc.h"
#include <telephony/rilapi.h>

#define NDEBUG
#include <debug.h>

#define PH_GSM7_ESCAPE              0x1B
#define PH_GSM7_SINGLE_MAX          160
#define PH_UCS2_SINGLE_MAX          70
#define PH_PART_TEXT_MAX            160
#define PH_MAX_PARTS                10
#define PH_MAX_ASSEMBLIES           8

/* 3GPP TS 23.038 default alphabet */
static const WCHAR PhGsm7Basic[128] =
{
    0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC,
    0x00F2, 0x00C7, 0x000A, 0x00D8, 0x00F8, 0x000D, 0x00C5, 0x00E5,
    0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8,
    0x03A3, 0x0398, 0x039E, 0x00A0, 0x00C6, 0x00E6, 0x00DF, 0x00C9,
    0x0020, 0x0021, 0x0022, 0x0023, 0x00A4, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x00A1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7,
    0x00BF, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0
};

typedef struct _PH_GSM7_EXTENSION
{
    BYTE Code;
    WCHAR Char;
} PH_GSM7_EXTENSION;

static const PH_GSM7_EXTENSION PhGsm7Extension[] =
{
    { 0x0A, 0x000C },
    { 0x14, 0x005E },
    { 0x28, 0x007B },
    { 0x29, 0x007D },
    { 0x2F, 0x005C },
    { 0x3C, 0x005B },
    { 0x3D, 0x007E },
    { 0x3E, 0x005D },
    { 0x40, 0x007C },
    { 0x65, 0x20AC }
};

/* Parts of a concatenated message waiting for the rest */
typedef struct _PH_ASSEMBLY
{
    BOOL InUse;
    ULONGLONG LastSeen;
    WCHAR Address[PHONEOM_MAX_NUMBER];
    USHORT Reference;
    BYTE Total;
    BYTE Received;
    FILETIME Timestamp;
    BOOL Have[PH_MAX_PARTS];
    WCHAR Text[PH_MAX_PARTS][PH_PART_TEXT_MAX + 1];
} PH_ASSEMBLY, *PPH_ASSEMBLY;

static CRITICAL_SECTION PhSmsLock;
static PH_ASSEMBLY PhAssemblies[PH_MAX_ASSEMBLIES];

/* Returns the number of septets needed, or 0 if Text has a character outside the GSM alphabet */
static
DWORD
PhGsm7Encode(
    _In_ LPCWSTR Text,
    _Out_writes_opt_(MaxCodes) PBYTE Codes,
    _In_ DWORD MaxCodes)
{
    DWORD Count = 0, i;
    LPCWSTR p;
    BOOL Found;

    for (p = Text; *p; p++)
    {
        Found = FALSE;

        for (i = 0; i < _countof(PhGsm7Basic); i++)
        {
            if (i == PH_GSM7_ESCAPE || PhGsm7Basic[i] != *p)
                continue;

            if (Codes && Count < MaxCodes)
                Codes[Count] = (BYTE)i;
            Count++;
            Found = TRUE;
            break;
        }

        if (Found)
            continue;

        for (i = 0; i < _countof(PhGsm7Extension); i++)
        {
            if (PhGsm7Extension[i].Char != *p)
                continue;

            if (Codes && Count + 1 < MaxCodes)
            {
                Codes[Count] = PH_GSM7_ESCAPE;
                Codes[Count + 1] = PhGsm7Extension[i].Code;
            }
            Count += 2;
            Found = TRUE;
            break;
        }

        if (!Found)
            return 0;
    }

    return Count;
}

static
VOID
PhGsm7Decode(
    _In_reads_(Count) const BYTE *Codes,
    _In_ DWORD Count,
    _Out_writes_z_(MaxChars) PWCHAR Text,
    _In_ DWORD MaxChars)
{
    DWORD i, j, Out = 0;
    WCHAR Char;

    for (i = 0; i < Count && Out + 1 < MaxChars; i++)
    {
        if (Codes[i] == PH_GSM7_ESCAPE && i + 1 < Count)
        {
            i++;

            /* An unknown extension code falls back to the basic table */
            Char = PhGsm7Basic[Codes[i] & 0x7F];
            for (j = 0; j < _countof(PhGsm7Extension); j++)
            {
                if (PhGsm7Extension[j].Code == Codes[i])
                {
                    Char = PhGsm7Extension[j].Char;
                    break;
                }
            }
        }
        else
        {
            Char = PhGsm7Basic[Codes[i] & 0x7F];
        }

        Text[Out++] = Char;
    }

    Text[Out] = UNICODE_NULL;
}

static
VOID
PhDecodeBody(
    _In_ const RIL_MESSAGE *Message,
    _Out_writes_z_(MaxChars) PWCHAR Text,
    _In_ DWORD MaxChars)
{
    DWORD Alphabet = RIL_ALPHABET_GSM7;
    DWORD Length, i;

    Text[0] = UNICODE_NULL;

    if (!(Message->Params & RIL_MSGPARAM_BODY))
        return;

    if ((Message->Params & RIL_MSGPARAM_DCS) && (Message->Dcs.Params & RIL_DCSPARAM_ALPHABET))
        Alphabet = Message->Dcs.Alphabet;

    Length = Message->u.Deliver.BodyLength;

    switch (Alphabet)
    {
        case RIL_ALPHABET_UCS2:
            Length = min(Length, RIL_MESSAGE_BODY_MAX / sizeof(WCHAR));
            Length = min(Length, MaxChars - 1);
            CopyMemory(Text, Message->u.Deliver.Body, Length * sizeof(WCHAR));
            Text[Length] = UNICODE_NULL;
            break;

        case RIL_ALPHABET_8BIT:
            /* Binary payloads have no text form, show the bytes as Latin-1 */
            Length = min(Length, RIL_MESSAGE_BODY_MAX);
            Length = min(Length, MaxChars - 1);
            for (i = 0; i < Length; i++)
                Text[i] = Message->u.Deliver.Body[i];
            Text[Length] = UNICODE_NULL;
            break;

        default:
            PhGsm7Decode(Message->u.Deliver.Body, min(Length, RIL_MESSAGE_BODY_MAX), Text, MaxChars);
            break;
    }
}

/* Finds the concatenation element in a user data header */
static
BOOL
PhParseConcat(
    _In_ const RIL_MESSAGE *Message,
    _Out_ PUSHORT Reference,
    _Out_ PBYTE Total,
    _Out_ PBYTE Sequence)
{
    const BYTE *Header = Message->u.Deliver.Header;
    DWORD Length = min(Message->u.Deliver.HeaderLength, RIL_MESSAGE_HEADER_MAX);
    DWORD Offset = 0;
    BYTE Id, Size;

    if (Length == 0)
        return FALSE;

    /* Some stacks keep the UDHL byte in front of the elements */
    if (Header[0] == Length - 1)
        Offset = 1;

    while (Offset + 2 <= Length)
    {
        Id = Header[Offset];
        Size = Header[Offset + 1];
        Offset += 2;

        if (Offset + Size > Length)
            break;

        if (Id == 0x00 && Size == 3)
        {
            *Reference = Header[Offset];
            *Total = Header[Offset + 1];
            *Sequence = Header[Offset + 2];
            return *Total != 0 && *Sequence != 0 && *Sequence <= *Total;
        }

        if (Id == 0x08 && Size == 4)
        {
            *Reference = (USHORT)((Header[Offset] << 8) | Header[Offset + 1]);
            *Total = Header[Offset + 2];
            *Sequence = Header[Offset + 3];
            return *Total != 0 && *Sequence != 0 && *Sequence <= *Total;
        }

        Offset += Size;
    }

    return FALSE;
}

/* Collects one part, fills Message with the full text once the last part is in */
static
BOOL
PhAssemblePart(
    _In_ LPCWSTR Text,
    _In_ USHORT Reference,
    _In_ BYTE Total,
    _In_ BYTE Sequence,
    _Inout_ PPHONEOM_MESSAGE Message)
{
    PPH_ASSEMBLY Assembly = NULL, Oldest = NULL;
    ULONGLONG Now = GetTickCount64();
    BOOL Complete = FALSE;
    DWORD i;

    if (Total > PH_MAX_PARTS)
        return FALSE;

    EnterCriticalSection(&PhSmsLock);

    for (i = 0; i < PH_MAX_ASSEMBLIES; i++)
    {
        PPH_ASSEMBLY Entry = &PhAssemblies[i];

        if (Entry->InUse &&
            Entry->Reference == Reference &&
            Entry->Total == Total &&
            !_wcsicmp(Entry->Address, Message->Address))
        {
            Assembly = Entry;
            break;
        }

        if (!Oldest || !Entry->InUse || (Oldest->InUse && Entry->LastSeen < Oldest->LastSeen))
            Oldest = Entry;
    }

    if (!Assembly)
    {
        /* Out of room, the stalest partial message is dropped */
        Assembly = Oldest;
        ZeroMemory(Assembly, sizeof(*Assembly));
        Assembly->InUse = TRUE;
        Assembly->Reference = Reference;
        Assembly->Total = Total;
        Assembly->Timestamp = Message->Timestamp;
        StringCchCopyW(Assembly->Address, _countof(Assembly->Address), Message->Address);
    }

    Assembly->LastSeen = Now;
    if (!Assembly->Have[Sequence - 1])
    {
        Assembly->Have[Sequence - 1] = TRUE;
        Assembly->Received++;
        StringCchCopyW(Assembly->Text[Sequence - 1], _countof(Assembly->Text[0]), Text);
    }

    if (Assembly->Received == Assembly->Total)
    {
        Message->Text[0] = UNICODE_NULL;
        for (i = 0; i < Assembly->Total; i++)
            StringCchCatW(Message->Text, _countof(Message->Text), Assembly->Text[i]);

        Message->Timestamp = Assembly->Timestamp;
        Assembly->InUse = FALSE;
        Complete = TRUE;
    }

    LeaveCriticalSection(&PhSmsLock);
    return Complete;
}

static
VOID
PhAckMessage(
    _In_ const RIL_MESSAGE_NOTIFICATION *Notification,
    _In_ DWORD Status)
{
    PPH_REQUEST Request;
    HRESULT hr;

    Request = PhBeginRequest();
    if (!Request)
        return;

    hr = RIL_SendMsgAck(PhRil,
                        Request->Id,
                        Notification->Executor,
                        0,
                        Notification->AckId,
                        Status,
                        RIL_MSGACK_PROTOCOL_3GPP,
                        Notification->OverIms);
    hr = PhFinishRequest(Request, hr, NULL);
    if (FAILED(hr))
        DPRINT1("Message ack %lu failed 0x%lx\n", Notification->AckId, hr);
}

VOID
PhSmsReceived(
    _In_reads_bytes_(Size) const VOID *Data,
    _In_ DWORD Size)
{
    const RIL_MESSAGE_NOTIFICATION *Notification = Data;
    const RIL_MESSAGE *Message;
    PPHONEOM_MESSAGE Stored;
    WCHAR Part[PH_PART_TEXT_MAX * 2 + 1];
    USHORT Reference;
    BYTE Total, Sequence;
    DWORD AckStatus = RIL_MSGACK_OK;
    SYSTEMTIME Utc;

    if (Size < sizeof(*Notification))
        return;

    Message = &Notification->Message;

    /* Status reports and anything else we do not store are still acknowledged */
    if (Message->Type != RIL_MSGTYPE_GW_DELIVER)
    {
        PhAckMessage(Notification, RIL_MSGACK_OK);
        return;
    }

    Stored = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Stored));
    if (!Stored)
    {
        PhAckMessage(Notification, RIL_MSGACK_MEMORYFULL);
        return;
    }

    Stored->Flags = PHONEOM_MESSAGE_INCOMING;

    if (Message->Params & RIL_MSGPARAM_ADDRESS)
    {
        if (Message->Address.dwType == RILADDRESSTYPE_INTERNATIONAL && Message->Address.wszAddress[0] != L'+')
            StringCchCopyW(Stored->Address, _countof(Stored->Address), L"+");

        StringCchCatNW(Stored->Address,
                       _countof(Stored->Address),
                       Message->Address.wszAddress,
                       _countof(Message->Address.wszAddress));
    }

    Utc = Message->u.Deliver.Timestamp;
    if (!(Message->Params & RIL_MSGPARAM_TIMESTAMP) || !SystemTimeToFileTime(&Utc, &Stored->Timestamp))
        GetSystemTimeAsFileTime(&Stored->Timestamp);

    PhDecodeBody(Message, Part, _countof(Part));

    if ((Message->Params & RIL_MSGPARAM_HEADER) &&
        PhParseConcat(Message, &Reference, &Total, &Sequence) &&
        Total > 1)
    {
        if (!PhAssemblePart(Part, Reference, Total, Sequence, Stored))
        {
            /* The network may resend if we fail here, so a held part is acked as received */
            PhAckMessage(Notification, RIL_MSGACK_OK);
            HeapFree(GetProcessHeap(), 0, Stored);
            return;
        }
    }
    else
    {
        StringCchCopyW(Stored->Text, _countof(Stored->Text), Part);
    }

    if (PhStoreAdd(Stored) == 0)
        AckStatus = RIL_MSGACK_MEMORYFULL;

    PhAckMessage(Notification, AckStatus);

    if (AckStatus == RIL_MSGACK_OK)
        PhBroadcast(PhoneEventMessage, Stored, sizeof(*Stored));

    HeapFree(GetProcessHeap(), 0, Stored);
}

/* Builds a single segment submit record, picking GSM 7 bit when the text allows it */
static
HRESULT
PhBuildSubmit(
    _In_ LPCWSTR Address,
    _In_ LPCWSTR Text,
    _Out_ PRIL_MESSAGE Message)
{
    DWORD Septets, Length;

    ZeroMemory(Message, sizeof(*Message));
    Message->Size = sizeof(*Message);
    Message->Params = RIL_MSGPARAM_TYPE | RIL_MSGPARAM_FLAGS | RIL_MSGPARAM_ADDRESS |
                      RIL_MSGPARAM_PROTOCOLID | RIL_MSGPARAM_DCS |
                      RIL_MSGPARAM_HEADER | RIL_MSGPARAM_BODY;
    Message->Type = RIL_MSGTYPE_GW_SUBMIT;
    Message->Flags = 0;

    if (!PhBuildAddress(Address, &Message->Address))
        return RIL_E_INVALIDDIALSTRING;

    Message->Dcs.Size = sizeof(Message->Dcs);
    Message->Dcs.Params = RIL_DCSPARAM_ALPHABET;

    Septets = PhGsm7Encode(Text, NULL, 0);
    if (Septets != 0 || Text[0] == UNICODE_NULL)
    {
        if (Septets > PH_GSM7_SINGLE_MAX)
            return HRESULT_FROM_WIN32(ERROR_MESSAGE_EXCEEDS_MAX_SIZE);

        Message->Dcs.Alphabet = RIL_ALPHABET_GSM7;
        Message->u.Submit.BodyLength = PhGsm7Encode(Text, Message->u.Submit.Body, RIL_MESSAGE_BODY_MAX);
        return S_OK;
    }

    Length = (DWORD)wcslen(Text);
    if (Length > PH_UCS2_SINGLE_MAX)
        return HRESULT_FROM_WIN32(ERROR_MESSAGE_EXCEEDS_MAX_SIZE);

    Message->Dcs.Alphabet = RIL_ALPHABET_UCS2;
    Message->u.Submit.BodyLength = Length;
    CopyMemory(Message->u.Submit.Body, Text, Length * sizeof(WCHAR));
    return S_OK;
}

HRESULT
PhSmsSend(
    _In_ LPCWSTR Address,
    _In_ LPCWSTR Text,
    _Out_ PDWORD MessageId)
{
    PPHONEOM_MESSAGE Stored;
    PRIL_MESSAGE Message;
    PPH_REQUEST Request;
    DWORD Flags;
    HRESULT hr;

    *MessageId = 0;

    if (!PhRil)
        return RIL_E_RADIONOTAVAILABLE;

    Message = HeapAlloc(GetProcessHeap(), 0, sizeof(*Message));
    Stored = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Stored));
    if (!Message || !Stored)
    {
        hr = E_OUTOFMEMORY;
        goto Exit;
    }

    hr = PhBuildSubmit(Address, Text, Message);
    if (FAILED(hr))
        goto Exit;

    /* Keep the outgoing copy first so the conversation shows it while it is in flight */
    StringCchCopyW(Stored->Address, _countof(Stored->Address), Address);
    StringCchCopyW(Stored->Text, _countof(Stored->Text), Text);
    Stored->Flags = PHONEOM_MESSAGE_READ;
    GetSystemTimeAsFileTime(&Stored->Timestamp);

    *MessageId = PhStoreAdd(Stored);
    if (*MessageId == 0)
    {
        hr = HRESULT_FROM_WIN32(ERROR_DISK_FULL);
        goto Exit;
    }

    Request = PhBeginRequest();
    if (!Request)
    {
        hr = E_OUTOFMEMORY;
    }
    else
    {
        hr = RIL_SendMsg(PhRil, Request->Id, PHONE_EXECUTOR, 0, Message, 0);
        hr = PhFinishRequest(Request, hr, NULL);
    }

    if (FAILED(hr))
        DPRINT1("Send to %S failed 0x%lx\n", Address, hr);

    Flags = SUCCEEDED(hr) ? PHONEOM_MESSAGE_SENT : PHONEOM_MESSAGE_FAILED;
    if (PhStoreUpdateFlags(*MessageId, Flags, 0, Stored))
        PhBroadcast(PhoneEventMessageStatus, Stored, sizeof(*Stored));

Exit:
    if (Message)
        HeapFree(GetProcessHeap(), 0, Message);
    if (Stored)
        HeapFree(GetProcessHeap(), 0, Stored);
    return hr;
}

VOID
PhSmsInitialize(VOID)
{
    InitializeCriticalSection(&PhSmsLock);
}
