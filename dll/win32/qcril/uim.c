/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     User identity module service, card state, provisioning and identity
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcril.h"
#include <string.h>

#define QCRIL_UIM_READ_TRANSPARENT      0x0020
#define QCRIL_UIM_READ_RECORD           0x0021
#define QCRIL_UIM_GET_FILE_ATTRIBUTES   0x0024
#define QCRIL_UIM_EVENT_REG             0x002E
#define QCRIL_UIM_GET_CARD_STATUS       0x002F
#define QCRIL_UIM_STATUS_CHANGE_IND     0x0032
#define QCRIL_UIM_CHANGE_PROVISIONING   0x0038
#define QCRIL_UIM_GET_CONFIGURATION     0x003A

#define QCRIL_UIM_TLV_CARD_STATUS       0x10
#define QCRIL_UIM_TLV_SESSION           0x01
#define QCRIL_UIM_TLV_FILE              0x02
#define QCRIL_UIM_TLV_READ              0x03
#define QCRIL_UIM_TLV_SESSION_CHANGE    0x01
#define QCRIL_UIM_TLV_APPLICATION       0x10
#define QCRIL_UIM_TLV_CONFIG_MASK       0x10
#define QCRIL_UIM_TLV_AUTO_SELECTION    0x10
#define QCRIL_UIM_TLV_EVENT_MASK        0x01
#define QCRIL_UIM_TLV_EVENT_ECHO        0x10
#define QCRIL_UIM_TLV_READ_RESULT       0x11
#define QCRIL_UIM_TLV_FILE_ATTRIBUTES   0x11

/* Automatic selection and halt subscription */
#define QCRIL_UIM_CONFIG_MASK           0x00000005
#define QCRIL_UIM_EVENT_CARD_STATUS     0x00000001

/* uim_card_state_enum_v01 */
#define QCRIL_UIM_CARD_ABSENT           0x00
#define QCRIL_UIM_CARD_PRESENT          0x01
#define QCRIL_UIM_CARD_ERROR            0x02

/* uim_card_error_code_enum_v01 values WMRil treats specially */
#define QCRIL_UIM_ERROR_POWER_DOWN      0x01
#define QCRIL_UIM_ERROR_NO_ATR          0x03
#define QCRIL_UIM_ERROR_REMOVED         0x06

/* uim_app_type_enum_v01, the two that carry a GSM/UMTS/LTE subscription */
#define QCRIL_UIM_APP_SIM               0x01
#define QCRIL_UIM_APP_USIM              0x02

/* uim_app_state_enum_v01 */
#define QCRIL_UIM_APP_UNKNOWN           0x00
#define QCRIL_UIM_APP_DETECTED          0x01
#define QCRIL_UIM_APP_PIN1_OR_UPIN      0x02
#define QCRIL_UIM_APP_PUK1_OR_PUK_UPIN  0x03
#define QCRIL_UIM_APP_PERSO             0x04
#define QCRIL_UIM_APP_PIN1_PERM_BLOCKED 0x05
#define QCRIL_UIM_APP_ILLEGAL           0x06
#define QCRIL_UIM_APP_READY             0x07

/* Session type of the primary GSM/UMTS/LTE subscription */
#define QCRIL_UIM_SESSION_PRIMARY_GW    0x00
#define QCRIL_UIM_NO_SESSION            0xFFFF

/* Slot states reported through RIL_NOTIFY_SLOTINFO */
#define QCRIL_SLOT_OFF                  2
#define QCRIL_SLOT_EMPTY                3
#define QCRIL_SLOT_NOT_READY            4
#define QCRIL_SLOT_ACTIVE               5
#define QCRIL_SLOT_ERROR                6

/* Elementary files and the directories holding them */
#define QCRIL_EF_IMSI                   0x6F07
#define QCRIL_EF_MSISDN                 0x6F40

/* Dedicated files of the SIM and USIM, little endian path words */
static const UCHAR QcPathUsim[] = { 0x00, 0x3F, 0xFF, 0x7F };
static const UCHAR QcPathSim[] = { 0x00, 0x3F, 0x20, 0x7F };

/* The DMS records that carry subscriber identity, the fallback when the card reads fail */
#define QCRIL_DMS_GET_MSISDN            0x0024
#define QCRIL_DMS_UIM_GET_IMSI          0x0043
#define QCRIL_DMS_TLV_STRING            0x01

/* Walks the card status record into the cache layout */
static
BOOL
QcUimParseStatus(
    _In_reads_bytes_(Size) const UCHAR *Status,
    _In_ USHORT Size,
    _Out_ QC_UIM_STATE *State)
{
    ULONG Offset = 9;
    UCHAR Cards, Card, App, Apps, AidLength;

    ZeroMemory(State, sizeof(*State));
    if (Size < 9)
        return FALSE;

    State->Valid = TRUE;
    State->GwPrimary = Status[0] | (Status[1] << 8);
    Cards = Status[8];

    for (Card = 0; Card < Cards; Card++)
    {
        PQC_UIM_CARD Slot = (Card < QC_UIM_MAX_CARDS) ? &State->Cards[Card] : NULL;

        /* card state, upin state, upin and upuk retries, error code, app count */
        if (Offset + 6 > Size)
            return FALSE;

        Apps = Status[Offset + 5];
        if (Slot)
        {
            Slot->CardState = Status[Offset];
            Slot->ErrorCode = Status[Offset + 4];
            Slot->AppCount = min(Apps, QC_UIM_MAX_APPS);
        }
        Offset += 6;

        for (App = 0; App < Apps; App++)
        {
            /* type, state, perso state, perso feature, perso retries, unblock retries, aid length */
            if (Offset + 7 > Size)
                return FALSE;

            AidLength = Status[Offset + 6];
            if (Offset + 7 + AidLength + 7 > Size)
                return FALSE;

            if (Slot && App < QC_UIM_MAX_APPS)
            {
                PQC_UIM_APP Entry = &Slot->Apps[App];

                Entry->Type = Status[Offset];
                Entry->State = Status[Offset + 1];
                Entry->PersoState = Status[Offset + 2];
                Entry->AidLength = min(AidLength, QC_UIM_MAX_AID);
                memcpy(Entry->Aid, &Status[Offset + 7], Entry->AidLength);

                /* universal pin, pin1 state and retries, puk1 retries, then pin2 */
                Entry->Pin1State = Status[Offset + 7 + AidLength + 1];
                Entry->Pin1Retries = Status[Offset + 7 + AidLength + 2];
                Entry->Puk1Retries = Status[Offset + 7 + AidLength + 3];
            }

            Offset += 7 + AidLength + 7;
        }
    }

    State->CardCount = min(Cards, QC_UIM_MAX_CARDS);
    return TRUE;
}

static
DWORD
QcUimSlotState(
    _In_ const QC_UIM_CARD *Card)
{
    switch (Card->CardState)
    {
        case QCRIL_UIM_CARD_ABSENT:
            return QCRIL_SLOT_EMPTY;

        case QCRIL_UIM_CARD_PRESENT:
            return QCRIL_SLOT_ACTIVE;

        case QCRIL_UIM_CARD_ERROR:
            if (Card->ErrorCode == QCRIL_UIM_ERROR_NO_ATR || Card->ErrorCode == QCRIL_UIM_ERROR_REMOVED)
                return QCRIL_SLOT_EMPTY;
            if (Card->ErrorCode == QCRIL_UIM_ERROR_POWER_DOWN)
                return QCRIL_SLOT_OFF;
            return QCRIL_SLOT_ERROR;

        default:
            return QCRIL_SLOT_ERROR;
    }
}

/* The SIM or USIM application of the first card, NULL when there is none */
static
const QC_UIM_APP *
QcUimFindGwApp(
    _In_ const QC_UIM_STATE *State,
    _Out_ PUCHAR Slot)
{
    UCHAR Card, App;

    *Slot = 0;

    for (Card = 0; Card < State->CardCount; Card++)
    {
        if (State->Cards[Card].CardState != QCRIL_UIM_CARD_PRESENT)
            continue;

        for (App = 0; App < State->Cards[Card].AppCount; App++)
        {
            const QC_UIM_APP *Entry = &State->Cards[Card].Apps[App];

            if (Entry->Type == QCRIL_UIM_APP_USIM || Entry->Type == QCRIL_UIM_APP_SIM)
            {
                *Slot = Card;
                return Entry;
            }
        }
    }

    return NULL;
}

/* Raises the slot states when any of them changed */
static
VOID
QcUimPublishSlots(
    _In_ PQC_MODEM Modem,
    _In_ const QC_UIM_STATE *State,
    _In_ BOOL Force)
{
    DWORD Info[3 + QC_UIM_MAX_CARDS];
    DWORD Slots, i;
    BOOL Changed = Force;

    Slots = max(State->CardCount, 1);
    Info[0] = (3 + Slots) * sizeof(DWORD);
    Info[1] = 0x00000003;
    Info[2] = Slots;

    EnterCriticalSection(&Modem->StateLock);
    for (i = 0; i < Slots; i++)
    {
        Info[3 + i] = State->Valid ? QcUimSlotState(&State->Cards[i]) : QCRIL_SLOT_NOT_READY;
        if (Modem->Uim.SlotState[i] != Info[3 + i])
            Changed = TRUE;
        Modem->Uim.SlotState[i] = Info[3 + i];
    }
    LeaveCriticalSection(&Modem->StateLock);

    if (!Changed)
        return;

    for (i = 0; i < Slots; i++)
        QcTrace("slot %lu state %lu\n", i, Info[3 + i]);
    QcNotifyAll(Modem, RIL_NOTIFY_SLOTINFO, Info, Info[0]);
}

/* Takes a new card status into the cache, logging what moved */
static
VOID
QcUimUpdate(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(Size) const UCHAR *Status,
    _In_ USHORT Size)
{
    QC_UIM_STATE New;
    UCHAR Card, App;
    BOOL Reset = FALSE;

    if (!QcUimParseStatus(Status, Size, &New))
    {
        QcTrace("card status: malformed, %u bytes\n", Size);
        return;
    }

    for (Card = 0; Card < New.CardCount; Card++)
    {
        QcTrace("card %u: state %u error %u apps %u\n", Card, New.Cards[Card].CardState,
                New.Cards[Card].ErrorCode, New.Cards[Card].AppCount);
        for (App = 0; App < New.Cards[Card].AppCount; App++)
        {
            const QC_UIM_APP *Entry = &New.Cards[Card].Apps[App];
            QcTrace("card %u app %u: type %u state %u perso %u pin1 %u (%u/%u left)\n", Card, App, Entry->Type,
                    Entry->State, Entry->PersoState, Entry->Pin1State, Entry->Pin1Retries, Entry->Puk1Retries);
        }
    }
    QcTrace("card status: gw primary index 0x%04x\n", New.GwPrimary);

    EnterCriticalSection(&Modem->StateLock);

    /* A card that went away or a valid session ends the provisioning attempt */
    for (Card = 0; Card < QC_UIM_MAX_CARDS; Card++)
    {
        if (Modem->Uim.Cards[Card].CardState == QCRIL_UIM_CARD_PRESENT &&
            New.Cards[Card].CardState != QCRIL_UIM_CARD_PRESENT)
        {
            Reset = TRUE;
        }
    }

    New.ConfigRead = Modem->Uim.ConfigRead;
    New.AutoSelect = Modem->Uim.AutoSelect;
    New.ProvisionSent = Modem->Uim.ProvisionSent && !Reset && New.GwPrimary == QCRIL_UIM_NO_SESSION;
    New.Recheck = TRUE;
    memcpy(New.SlotState, Modem->Uim.SlotState, sizeof(New.SlotState));
    Modem->Uim = New;

    LeaveCriticalSection(&Modem->StateLock);

    QcUimPublishSlots(Modem, &New, FALSE);
}

static
HRESULT
QcUimReadCardStatus(
    _In_ PQC_MODEM Modem)
{
    QMI_ENCODER Encoder;
    BYTE Request[16];
    BYTE Response[QC_MESSAGE_MAX];
    const VOID *Value;
    USHORT ValueLen;
    ULONG Length, ResponseLen;
    HRESULT hr;

    QmiEncodeBegin(&Encoder, Request, sizeof(Request), QMI_TYPE_REQUEST, 0, QCRIL_UIM_GET_CARD_STATUS);
    Length = QmiEncodeEnd(&Encoder);

    hr = QcQmiRequest(Modem, QcSvcUim, Request, Length, Response, sizeof(Response), &ResponseLen);
    if (FAILED(hr))
        return hr;

    if (!QmiFindTlv(Response, ResponseLen, QCRIL_UIM_TLV_CARD_STATUS, &Value, &ValueLen))
        return E_FAIL;

    QcUimUpdate(Modem, Value, ValueLen);
    return S_OK;
}

/* Opens or closes the primary GSM/UMTS/LTE session on one application */
static
HRESULT
QcUimProvision(
    _In_ PQC_MODEM Modem,
    _In_ BOOL Activate,
    _In_ UCHAR Slot,
    _In_opt_ const QC_UIM_APP *App)
{
    QMI_ENCODER Encoder;
    BYTE Message[96];
    BYTE Response[64];
    UCHAR Session[2];
    UCHAR Application[2 + QC_UIM_MAX_AID];
    ULONG Length, ResponseLen;
    HRESULT hr;

    Session[0] = QCRIL_UIM_SESSION_PRIMARY_GW;
    Session[1] = Activate ? 1 : 0;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_UIM_CHANGE_PROVISIONING);
    if (!QmiEncodeTlv(&Encoder, QCRIL_UIM_TLV_SESSION_CHANGE, Session, sizeof(Session)))
        return E_FAIL;

    if (Activate && App)
    {
        /* Slots are numbered from one on the wire */
        Application[0] = Slot + 1;
        Application[1] = App->AidLength;
        memcpy(&Application[2], App->Aid, App->AidLength);
        if (!QmiEncodeTlv(&Encoder, QCRIL_UIM_TLV_APPLICATION, Application, (USHORT)(2 + App->AidLength)))
            return E_FAIL;
    }
    Length = QmiEncodeEnd(&Encoder);

    hr = QcQmiRequest(Modem, QcSvcUim, Message, Length, Response, sizeof(Response), &ResponseLen);
    QcTrace("provisioning %s on slot %u: 0x%08lx, QMI error 0x%x\n", Activate ? "activate" : "deactivate",
            Slot + 1, hr, QcQmiError(Response, ResponseLen));

    /* Closing a session that is not open has no effect, which is fine */
    if (!Activate && FAILED(hr) && QcQmiError(Response, ResponseLen) == QC_QMI_ERR_NO_EFFECT)
        hr = S_OK;

    return hr;
}

/*
 * The modem leaves the subscription to the RIL on builds made for Android, so the
 * application stays detected and nothing registers until a session is opened on it.
 */
VOID
QcUimEvaluate(
    _In_ PQC_MODEM Modem)
{
    QC_UIM_STATE State;
    const QC_UIM_APP *App;
    QC_UIM_APP Copy;
    UCHAR Slot;
    HRESULT hr;

    EnterCriticalSection(&Modem->StateLock);
    State = Modem->Uim;
    LeaveCriticalSection(&Modem->StateLock);

    if (!State.Valid || !State.ConfigRead || State.AutoSelect || State.ProvisionSent)
        return;
    if (State.GwPrimary != QCRIL_UIM_NO_SESSION)
        return;

    App = QcUimFindGwApp(&State, &Slot);
    if (!App)
        return;

    Copy = *App;
    QcTrace("no primary session and automatic selection off, provisioning app type %u on slot %u\n",
            Copy.Type, Slot + 1);

    hr = QcUimProvision(Modem, TRUE, Slot, &Copy);

    EnterCriticalSection(&Modem->StateLock);
    Modem->Uim.ProvisionSent = SUCCEEDED(hr);
    LeaveCriticalSection(&Modem->StateLock);
}

VOID
QcUimSetup(
    _In_ PQC_MODEM Modem)
{
    QMI_ENCODER Encoder;
    BYTE Request[32];
    BYTE Response[256];
    const VOID *Value;
    USHORT ValueLen;
    ULONG Length, ResponseLen, Mask;
    BOOL AutoSelect = TRUE;
    HRESULT hr;

    Mask = QCRIL_UIM_CONFIG_MASK;
    QmiEncodeBegin(&Encoder, Request, sizeof(Request), QMI_TYPE_REQUEST, 0, QCRIL_UIM_GET_CONFIGURATION);
    QmiEncodeTlv(&Encoder, QCRIL_UIM_TLV_CONFIG_MASK, &Mask, sizeof(Mask));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiRequest(Modem, QcSvcUim, Request, Length, Response, sizeof(Response), &ResponseLen);
    if (SUCCEEDED(hr) && QmiFindTlv(Response, ResponseLen, QCRIL_UIM_TLV_AUTO_SELECTION, &Value, &ValueLen) &&
        ValueLen >= 1)
    {
        AutoSelect = *(const UCHAR *)Value != 0;
    }
    QcTrace("uim: configuration 0x%08lx, automatic selection %u\n", hr, AutoSelect);

    EnterCriticalSection(&Modem->StateLock);
    Modem->Uim.AutoSelect = AutoSelect;
    Modem->Uim.ConfigRead = TRUE;
    LeaveCriticalSection(&Modem->StateLock);

    Mask = QCRIL_UIM_EVENT_CARD_STATUS;
    QmiEncodeBegin(&Encoder, Request, sizeof(Request), QMI_TYPE_REQUEST, 0, QCRIL_UIM_EVENT_REG);
    QmiEncodeTlv(&Encoder, QCRIL_UIM_TLV_EVENT_MASK, &Mask, sizeof(Mask));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiRequest(Modem, QcSvcUim, Request, Length, Response, sizeof(Response), &ResponseLen);
    if (SUCCEEDED(hr) && QmiFindTlv(Response, ResponseLen, QCRIL_UIM_TLV_EVENT_ECHO, &Value, &ValueLen) &&
        ValueLen >= sizeof(ULONG))
    {
        memcpy(&Mask, Value, sizeof(Mask));
        if (!(Mask & QCRIL_UIM_EVENT_CARD_STATUS))
            QcTrace("uim: card status events not granted, mask 0x%lx\n", Mask);
    }
    QcTrace("uim: event registration 0x%08lx\n", hr);

    hr = QcUimReadCardStatus(Modem);
    QcTrace("uim: card status 0x%08lx\n", hr);
    QcUimEvaluate(Modem);
}

VOID
QcUimModemLost(
    _In_ PQC_MODEM Modem)
{
    EnterCriticalSection(&Modem->StateLock);
    ZeroMemory(Modem->Uim.Cards, sizeof(Modem->Uim.Cards));
    Modem->Uim.Valid = FALSE;
    Modem->Uim.CardCount = 0;
    Modem->Uim.GwPrimary = QCRIL_UIM_NO_SESSION;
    Modem->Uim.ConfigRead = FALSE;
    Modem->Uim.ProvisionSent = FALSE;
    Modem->Uim.Recheck = FALSE;
    LeaveCriticalSection(&Modem->StateLock);
}

/* Reads a whole transparent file or one record from the primary session */
static
HRESULT
QcUimReadFile(
    _In_ PQC_MODEM Modem,
    _In_ USHORT FileId,
    _In_ BOOL Usim,
    _In_ USHORT Record,
    _Out_writes_bytes_to_(OutSize, *OutLen) PUCHAR Out,
    _In_ ULONG OutSize,
    _Out_ PULONG OutLen)
{
    QMI_ENCODER Encoder;
    BYTE Request[64];
    BYTE Response[512];
    UCHAR Session[2] = { QCRIL_UIM_SESSION_PRIMARY_GW, 0 };
    UCHAR File[3 + sizeof(QcPathUsim)];
    USHORT Read[2];
    const UCHAR *Data;
    const VOID *Value;
    USHORT ValueLen, Content;
    ULONG Length, ResponseLen;
    HRESULT hr;

    *OutLen = 0;

    memcpy(File, &FileId, sizeof(FileId));
    File[2] = sizeof(QcPathUsim);
    memcpy(&File[3], Usim ? QcPathUsim : QcPathSim, sizeof(QcPathUsim));

    /* Offset or record, then length, zero reads all of it */
    Read[0] = Record;
    Read[1] = 0;

    QmiEncodeBegin(&Encoder, Request, sizeof(Request), QMI_TYPE_REQUEST, 0,
                   Record ? QCRIL_UIM_READ_RECORD : QCRIL_UIM_READ_TRANSPARENT);
    QmiEncodeTlv(&Encoder, QCRIL_UIM_TLV_SESSION, Session, sizeof(Session));
    QmiEncodeTlv(&Encoder, QCRIL_UIM_TLV_FILE, File, sizeof(File));
    QmiEncodeTlv(&Encoder, QCRIL_UIM_TLV_READ, Read, sizeof(Read));
    Length = QmiEncodeEnd(&Encoder);

    hr = QcQmiRequest(Modem, QcSvcUim, Request, Length, Response, sizeof(Response), &ResponseLen);
    if (FAILED(hr))
        return hr;

    if (!QmiFindTlv(Response, ResponseLen, QCRIL_UIM_TLV_READ_RESULT, &Value, &ValueLen) || ValueLen < 2)
        return E_FAIL;

    Data = Value;
    Content = Data[0] | (Data[1] << 8);
    if ((ULONG)Content + 2 > ValueLen)
        return E_FAIL;

    *OutLen = min(Content, OutSize);
    memcpy(Out, Data + 2, *OutLen);
    return S_OK;
}

static
CHAR
QcBcdDigit(
    _In_ UCHAR Nibble)
{
    if (Nibble <= 9)
        return (CHAR)('0' + Nibble);
    if (Nibble == 0x0A)
        return '*';
    if (Nibble == 0x0B)
        return '#';
    return '\0';
}

/* EF IMSI: length byte, then BCD digits with the parity in the first low nibble */
static
BOOL
QcUimDecodeImsi(
    _In_reads_bytes_(Length) const UCHAR *Ef,
    _In_ ULONG Length,
    _Out_writes_z_(OutSize) PSTR Out,
    _In_ ULONG OutSize)
{
    ULONG i, Written = 0, Bytes;
    CHAR Ch;

    Out[0] = '\0';
    if (Length < 2 || Ef[0] == 0 || Ef[0] == 0xFF)
        return FALSE;

    Bytes = min((ULONG)Ef[0], Length - 1);
    for (i = 0; i < Bytes && Written + 1 < OutSize; i++)
    {
        if (i != 0)
        {
            Ch = QcBcdDigit(Ef[1 + i] & 0x0F);
            if (!Ch)
                break;
            Out[Written++] = Ch;
        }

        if (Written + 1 >= OutSize)
            break;
        Ch = QcBcdDigit(Ef[1 + i] >> 4);
        if (!Ch)
            break;
        Out[Written++] = Ch;
    }

    Out[Written] = '\0';
    return Written != 0;
}

/* EF MSISDN record: alpha identifier, BCD length, type of number, digits */
static
BOOL
QcUimDecodeMsisdn(
    _In_reads_bytes_(Length) const UCHAR *Ef,
    _In_ ULONG Length,
    _Out_writes_z_(OutSize) PSTR Out,
    _In_ ULONG OutSize)
{
    ULONG Base, Bytes, i, Written = 0;
    CHAR Ch;

    Out[0] = '\0';
    if (Length < 14)
        return FALSE;

    Base = Length - 14;
    Bytes = Ef[Base];
    if (Bytes < 2 || Bytes == 0xFF || Bytes > 11)
        return FALSE;

    if ((Ef[Base + 1] & 0x70) == 0x10 && Written + 1 < OutSize)
        Out[Written++] = '+';

    for (i = 0; i < Bytes - 1 && Written + 2 < OutSize; i++)
    {
        Ch = QcBcdDigit(Ef[Base + 2 + i] & 0x0F);
        if (!Ch)
            break;
        Out[Written++] = Ch;

        Ch = QcBcdDigit(Ef[Base + 2 + i] >> 4);
        if (!Ch)
            break;
        Out[Written++] = Ch;
    }

    Out[Written] = '\0';
    return Written != 0;
}

HRESULT
QcUimGetSubscriber(
    _In_ PQC_MODEM Modem,
    _Inout_ PQC_RESP Resp)
{
    RIL_SUBSCRIBER_INFO Info;
    QC_UIM_STATE State;
    const QC_UIM_APP *App;
    UCHAR Ef[64];
    ULONG EfLen;
    UCHAR Slot;
    BOOL Usim = TRUE;

    ZeroMemory(&Info, sizeof(Info));
    Info.Size = sizeof(Info);

    if (!QcServiceEnsure(Modem, QcSvcUim))
        return RIL_E_RADIONOTPRESENT;

    EnterCriticalSection(&Modem->StateLock);
    State = Modem->Uim;
    LeaveCriticalSection(&Modem->StateLock);

    if (!State.Valid && SUCCEEDED(QcUimReadCardStatus(Modem)))
    {
        EnterCriticalSection(&Modem->StateLock);
        State = Modem->Uim;
        LeaveCriticalSection(&Modem->StateLock);
    }

    App = QcUimFindGwApp(&State, &Slot);
    if (!State.Valid)
    {
        Info.SimState = RIL_SIM_ERROR;
    }
    else if (State.CardCount == 0 || State.Cards[0].CardState == QCRIL_UIM_CARD_ABSENT)
    {
        Info.SimState = RIL_SIM_ABSENT;
    }
    else if (State.Cards[0].CardState != QCRIL_UIM_CARD_PRESENT || !App)
    {
        Info.SimState = RIL_SIM_ERROR;
    }
    else
    {
        /* Only a ready application is usable, everything short of it is not yet */
        switch (App->State)
        {
            case QCRIL_UIM_APP_READY: Info.SimState = RIL_SIM_READY; break;
            case QCRIL_UIM_APP_PIN1_OR_UPIN: Info.SimState = RIL_SIM_PIN; break;
            case QCRIL_UIM_APP_PUK1_OR_PUK_UPIN: Info.SimState = RIL_SIM_PUK; break;
            case QCRIL_UIM_APP_PERSO: Info.SimState = RIL_SIM_PIN; break;
            case QCRIL_UIM_APP_PIN1_PERM_BLOCKED:
            case QCRIL_UIM_APP_ILLEGAL: Info.SimState = RIL_SIM_ERROR; break;
            default: Info.SimState = RIL_SIM_NOT_READY; break;
        }
        Usim = App->Type == QCRIL_UIM_APP_USIM;
    }

    if (Info.SimState == RIL_SIM_READY)
    {
        if (SUCCEEDED(QcUimReadFile(Modem, QCRIL_EF_IMSI, Usim, 0, Ef, sizeof(Ef), &EfLen)))
            QcUimDecodeImsi(Ef, EfLen, Info.Imsi, sizeof(Info.Imsi));
        if (SUCCEEDED(QcUimReadFile(Modem, QCRIL_EF_MSISDN, Usim, 1, Ef, sizeof(Ef), &EfLen)))
            QcUimDecodeMsisdn(Ef, EfLen, Info.Msisdn, sizeof(Info.Msisdn));
    }

    /* The device management copies serve when the card reads come back empty */
    if (!Info.Imsi[0])
        QcDmsReadString(Modem, QCRIL_DMS_UIM_GET_IMSI, QCRIL_DMS_TLV_STRING, Info.Imsi, sizeof(Info.Imsi));
    if (!Info.Msisdn[0])
        QcDmsReadString(Modem, QCRIL_DMS_GET_MSISDN, QCRIL_DMS_TLV_STRING, Info.Msisdn, sizeof(Info.Msisdn));

    QcTrace("subscriber: sim state %lu imsi '%s' msisdn '%s'\n", Info.SimState, Info.Imsi, Info.Msisdn);
    QcRespBytes(Resp, &Info, sizeof(Info));
    return Resp->Failed ? E_FAIL : S_OK;
}

/* In: executor, then the config: size, params, application count and handles. No application unbinds. */
HRESULT
QcUimSetExecutorConfig(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(InSize) const VOID *In,
    _In_ ULONG InSize)
{
    const DWORD *Args = In;
    QC_UIM_STATE State;
    const QC_UIM_APP *App;
    QC_UIM_APP Copy;
    UCHAR Slot;
    BOOL Bind;

    if (InSize < 4 * sizeof(DWORD))
        return E_INVALIDARG;

    Bind = Args[3] != 0;
    QcTrace("executor %lu %s, %lu application(s)\n", Args[0], Bind ? "bind" : "unbind", Args[3]);

    if (!Bind)
    {
        EnterCriticalSection(&Modem->StateLock);
        Modem->Uim.ProvisionSent = TRUE;
        LeaveCriticalSection(&Modem->StateLock);
        return QcUimProvision(Modem, FALSE, 0, NULL);
    }

    EnterCriticalSection(&Modem->StateLock);
    State = Modem->Uim;
    LeaveCriticalSection(&Modem->StateLock);

    App = QcUimFindGwApp(&State, &Slot);
    if (!App)
        return RIL_E_UICCNOTINSERTED;
    if (State.GwPrimary != QCRIL_UIM_NO_SESSION)
        return S_OK;

    Copy = *App;
    return QcUimProvision(Modem, TRUE, Slot, &Copy);
}

VOID
QcUimIndication(
    _In_ PQC_MODEM Modem,
    _In_ USHORT MessageId,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    const VOID *Value;
    USHORT ValueLen;

    if (MessageId != QCRIL_UIM_STATUS_CHANGE_IND)
    {
        QcTrace("ind UIM 0x%04X not handled\n", MessageId);
        return;
    }

    if (!QmiFindTlv(Message, Length, QCRIL_UIM_TLV_CARD_STATUS, &Value, &ValueLen))
    {
        QcTrace("card status change without a status\n");
        return;
    }

    /* Provisioning sends requests, it runs on the setup thread */
    QcUimUpdate(Modem, Value, ValueLen);
    QcKickSetup(Modem);
}

/* The IMSI of the ready GW application, from its EF IMSI; E_PENDING until the card is ready */
HRESULT
QcUimReadImsi(
    _In_ PQC_MODEM Modem,
    _Out_writes_z_(OutSize) PSTR Out,
    _In_ ULONG OutSize)
{
    QC_UIM_STATE State;
    const QC_UIM_APP *App;
    UCHAR Ef[64];
    ULONG EfLen;
    UCHAR Slot;
    HRESULT hr;

    Out[0] = '\0';
    if (!QcServiceEnsure(Modem, QcSvcUim))
        return RIL_E_RADIONOTPRESENT;

    EnterCriticalSection(&Modem->StateLock);
    State = Modem->Uim;
    LeaveCriticalSection(&Modem->StateLock);

    App = State.Valid ? QcUimFindGwApp(&State, &Slot) : NULL;
    if (!App || App->State != QCRIL_UIM_APP_READY)
        return E_PENDING;

    hr = QcUimReadFile(Modem, QCRIL_EF_IMSI, App->Type == QCRIL_UIM_APP_USIM, 0, Ef, sizeof(Ef), &EfLen);
    if (FAILED(hr))
        return hr;

    QcUimDecodeImsi(Ef, EfLen, Out, OutSize);
    return Out[0] ? S_OK : E_FAIL;
}
