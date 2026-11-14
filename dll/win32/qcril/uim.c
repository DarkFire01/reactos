/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     User identity module service, SIM state and subscriber identity
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcril.h"
#include <string.h>

#define QCRIL_UIM_GET_CARD_STATUS       0x002F
#define QCRIL_UIM_TLV_CARD_STATUS       0x10

/* uim_card_state_enum_v01 */
#define QCRIL_UIM_CARD_ABSENT           0x00
#define QCRIL_UIM_CARD_PRESENT          0x01
#define QCRIL_UIM_CARD_ERROR            0x02

/* The DMS records that carry subscriber identity */
#define QCRIL_DMS_GET_MSISDN            0x0024
#define QCRIL_DMS_UIM_GET_IMSI          0x0043
#define QCRIL_DMS_TLV_STRING            0x01

BOOL
QcUimSimState(
    _In_ PQC_MODEM Modem,
    _Out_ PDWORD State)
{
    QMI_ENCODER Encoder;
    BYTE Message[16];
    BYTE Response[512];
    const UCHAR *Card;
    const VOID *Value;
    USHORT ValueLen;
    ULONG Length, ResponseLen;
    UCHAR Cards, CardState;

    *State = RIL_SIM_ABSENT;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_UIM_GET_CARD_STATUS);
    Length = QmiEncodeEnd(&Encoder);

    if (FAILED(QcQmiRequest(Modem, QcSvcUim, Message, Length, Response, sizeof(Response), &ResponseLen)))
        return FALSE;

    if (!QmiFindTlv(Response, ResponseLen, QCRIL_UIM_TLV_CARD_STATUS, &Value, &ValueLen) || ValueLen < 9)
        return FALSE;

    /* Four u16 index fields, the card count, then the first card state */
    Card = Value;
    Cards = Card[8];
    if (Cards == 0 || ValueLen < 10)
    {
        *State = RIL_SIM_ABSENT;
        return TRUE;
    }

    CardState = Card[9];
    switch (CardState)
    {
        case QCRIL_UIM_CARD_PRESENT: *State = RIL_SIM_READY; break;
        case QCRIL_UIM_CARD_ERROR: *State = RIL_SIM_ERROR; break;
        default: *State = RIL_SIM_ABSENT; break;
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
