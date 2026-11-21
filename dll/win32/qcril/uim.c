/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     User identity module service, SIM state and subscriber identity
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcril.h"
#include <string.h>

#define QCRIL_UIM_GET_CARD_STATUS       0x002F
#define QCRIL_UIM_CHANGE_PROVISIONING   0x0038
#define QCRIL_UIM_TLV_CARD_STATUS       0x10
#define QCRIL_UIM_TLV_SESSION_CHANGE    0x01
#define QCRIL_UIM_TLV_APPLICATION       0x10

/* uim_card_state_enum_v01 */
#define QCRIL_UIM_CARD_ABSENT           0x00
#define QCRIL_UIM_CARD_PRESENT          0x01
#define QCRIL_UIM_CARD_ERROR            0x02

/* uim_app_type_enum_v01, the two that carry a GSM/UMTS/LTE subscription */
#define QCRIL_UIM_APP_SIM               0x01
#define QCRIL_UIM_APP_USIM              0x02

/* uim_app_state_enum_v01 */
#define QCRIL_UIM_APP_DETECTED          0x01
#define QCRIL_UIM_APP_PIN1_OR_UPIN      0x02
#define QCRIL_UIM_APP_PUK1_OR_PUK_UPIN  0x03
#define QCRIL_UIM_APP_PIN1_PERM_BLOCKED 0x05
#define QCRIL_UIM_APP_ILLEGAL           0x06

/* No session is provisioned for that slot of the index table */
#define QCRIL_UIM_NO_SESSION            0xFFFF
#define QCRIL_UIM_SESSION_PRIMARY_GW    0x00
#define QCRIL_UIM_SLOT_1                0x01
#define QCRIL_UIM_MAX_AID               32

/* The DMS records that carry subscriber identity */
#define QCRIL_DMS_GET_MSISDN            0x0024
#define QCRIL_DMS_UIM_GET_IMSI          0x0043
#define QCRIL_DMS_TLV_STRING            0x01

/* The first card's subscription application, as the card status reports it */
typedef struct _QC_UIM_APP
{
    UCHAR Type;
    UCHAR State;
    UCHAR AidLength;
    UCHAR Aid[QCRIL_UIM_MAX_AID];
} QC_UIM_APP, *PQC_UIM_APP;

static BOOL QcUimProvisionSent;

/*
 * The modem leaves the subscription to the RIL on builds made for Android, so the
 * application stays detected and nothing registers until a session is opened on it.
 */
static
VOID
QcUimActivatePrimary(
    _In_ PQC_MODEM Modem,
    _In_ const QC_UIM_APP *App)
{
    QMI_ENCODER Encoder;
    BYTE Message[64];
    BYTE Response[64];
    UCHAR Session[2];
    UCHAR Application[2 + QCRIL_UIM_MAX_AID];
    ULONG Length, ResponseLen;
    HRESULT hr;

    Session[0] = QCRIL_UIM_SESSION_PRIMARY_GW;
    Session[1] = 1;
    Application[0] = QCRIL_UIM_SLOT_1;
    Application[1] = App->AidLength;
    memcpy(&Application[2], App->Aid, App->AidLength);

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_UIM_CHANGE_PROVISIONING);
    if (!QmiEncodeTlv(&Encoder, QCRIL_UIM_TLV_SESSION_CHANGE, Session, sizeof(Session)) ||
        !QmiEncodeTlv(&Encoder, QCRIL_UIM_TLV_APPLICATION, Application, (USHORT)(2 + App->AidLength)))
    {
        return;
    }
    Length = QmiEncodeEnd(&Encoder);

    hr = QcQmiRequest(Modem, QcSvcUim, Message, Length, Response, sizeof(Response), &ResponseLen);
    QcTrace("primary subscription on slot 1, app type %u: 0x%lx\n", App->Type, hr);
    QcUimProvisionSent = SUCCEEDED(hr);
}

/* Walks the card status for the first SIM or USIM application on the first slot */
static
BOOL
QcUimFindApp(
    _In_reads_bytes_(Size) const UCHAR *Status,
    _In_ USHORT Size,
    _Out_ PUCHAR CardState,
    _Out_ PQC_UIM_APP App)
{
    ULONG Offset = 9;
    UCHAR Apps, Index, AidLength;

    ZeroMemory(App, sizeof(*App));
    *CardState = QCRIL_UIM_CARD_ABSENT;
    if (Size < 9 || Status[8] == 0 || Size < Offset + 6)
        return FALSE;

    *CardState = Status[Offset];
    Apps = Status[Offset + 5];
    Offset += 6;

    for (Index = 0; Index < Apps && Offset + 7 <= Size; Index++)
    {
        AidLength = Status[Offset + 6];
        if (Offset + 7 + AidLength > Size)
            break;

        if ((Status[Offset] == QCRIL_UIM_APP_SIM || Status[Offset] == QCRIL_UIM_APP_USIM) &&
            AidLength <= QCRIL_UIM_MAX_AID)
        {
            App->Type = Status[Offset];
            App->State = Status[Offset + 1];
            App->AidLength = AidLength;
            memcpy(App->Aid, &Status[Offset + 7], AidLength);
            return TRUE;
        }

        /* Then the universal PIN flag and the PIN1 and PIN2 counters */
        Offset += 7 + AidLength + 7;
    }

    return FALSE;
}

BOOL
QcUimSimState(
    _In_ PQC_MODEM Modem,
    _Out_ PDWORD State)
{
    QMI_ENCODER Encoder;
    BYTE Message[16];
    BYTE Response[512];
    const UCHAR *Status;
    const VOID *Value;
    USHORT ValueLen;
    ULONG Length, ResponseLen;
    QC_UIM_APP App;
    UCHAR CardState;
    BOOL HaveApp;

    *State = RIL_SIM_ABSENT;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_UIM_GET_CARD_STATUS);
    Length = QmiEncodeEnd(&Encoder);

    if (FAILED(QcQmiRequest(Modem, QcSvcUim, Message, Length, Response, sizeof(Response), &ResponseLen)))
        return FALSE;

    if (!QmiFindTlv(Response, ResponseLen, QCRIL_UIM_TLV_CARD_STATUS, &Value, &ValueLen) || ValueLen < 9)
        return FALSE;

    Status = Value;
    HaveApp = QcUimFindApp(Status, ValueLen, &CardState, &App);

    if (CardState == QCRIL_UIM_CARD_ABSENT)
    {
        QcUimProvisionSent = FALSE;
        return TRUE;
    }
    if (CardState != QCRIL_UIM_CARD_PRESENT)
    {
        *State = RIL_SIM_ERROR;
        return TRUE;
    }

    /* The first index is the primary GSM/UMTS/LTE session */
    if (HaveApp &&
        App.State == QCRIL_UIM_APP_DETECTED &&
        (Status[0] | (Status[1] << 8)) == QCRIL_UIM_NO_SESSION &&
        !QcUimProvisionSent)
    {
        QcUimActivatePrimary(Modem, &App);
    }

    switch (HaveApp ? App.State : 0)
    {
        case QCRIL_UIM_APP_PIN1_OR_UPIN: *State = RIL_SIM_PIN; break;
        case QCRIL_UIM_APP_PUK1_OR_PUK_UPIN: *State = RIL_SIM_PUK; break;
        case QCRIL_UIM_APP_PIN1_PERM_BLOCKED:
        case QCRIL_UIM_APP_ILLEGAL: *State = RIL_SIM_ERROR; break;
        default: *State = RIL_SIM_READY; break;
    }

    return TRUE;
}

HRESULT
QcUimGetSubscriber(
    _In_ PQC_MODEM Modem,
    _Inout_ PQC_RESP Resp)
{
    RIL_SUBSCRIBER_INFO Info;
    DWORD State;

    ZeroMemory(&Info, sizeof(Info));
    Info.Size = sizeof(Info);

    if (QcUimSimState(Modem, &State))
        Info.SimState = State;
    else
        Info.SimState = RIL_SIM_ERROR;

    /* Identity comes from the device management service */
    QcDmsReadString(Modem, QCRIL_DMS_UIM_GET_IMSI, QCRIL_DMS_TLV_STRING, Info.Imsi, sizeof(Info.Imsi));
    QcDmsReadString(Modem, QCRIL_DMS_GET_MSISDN, QCRIL_DMS_TLV_STRING, Info.Msisdn, sizeof(Info.Msisdn));

    QcRespBytes(Resp, &Info, sizeof(Info));
    return Resp->Failed ? E_FAIL : S_OK;
}
