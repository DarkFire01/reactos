/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Network access service, signal and registration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcril.h"
#include <string.h>

#define QCRIL_NAS_INDICATION_REGISTER   0x0003
#define QCRIL_NAS_GET_SIGNAL_STRENGTH   0x0020
#define QCRIL_NAS_GET_SERVING_SYSTEM    0x0024

#define QCRIL_NAS_TLV_REG_SERVING       0x13
#define QCRIL_NAS_TLV_REG_SIG_INFO      0x19

#define QCRIL_NAS_TLV_SIG_MASK          0x10
#define QCRIL_NAS_TLV_SIG_STRENGTH      0x01
#define QCRIL_NAS_TLV_SERVING_SYSTEM    0x01
#define QCRIL_NAS_TLV_ROAMING           0x10
#define QCRIL_NAS_TLV_CURRENT_PLMN      0x12

#define QCRIL_NAS_SIG_MASK_ALL          0x00FF

/* nas_radio_if_enum_v01 */
#define QCRIL_NAS_RADIO_CDMA_1X         0x01
#define QCRIL_NAS_RADIO_CDMA_EVDO       0x02
#define QCRIL_NAS_RADIO_GSM             0x04
#define QCRIL_NAS_RADIO_UMTS            0x05
#define QCRIL_NAS_RADIO_LTE             0x08
#define QCRIL_NAS_RADIO_TDSCDMA         0x09

static
DWORD
QcNasMapRat(
    _In_ UCHAR RadioIf)
{
    switch (RadioIf)
    {
        case QCRIL_NAS_RADIO_GSM: return RIL_RAT_GSM;
        case QCRIL_NAS_RADIO_UMTS: return RIL_RAT_UMTS;
        case QCRIL_NAS_RADIO_LTE: return RIL_RAT_LTE;
        case QCRIL_NAS_RADIO_CDMA_1X: return RIL_RAT_CDMA;
        case QCRIL_NAS_RADIO_CDMA_EVDO: return RIL_RAT_EVDO;
        case QCRIL_NAS_RADIO_TDSCDMA: return RIL_RAT_TDSCDMA;
        default: return RIL_RAT_NONE;
    }
}

static
DWORD
QcNasBars(
    _In_ LONG Dbm)
{
    if (Dbm >= -75) return 5;
    if (Dbm >= -85) return 4;
    if (Dbm >= -95) return 3;
    if (Dbm >= -105) return 2;
    if (Dbm >= -112) return 1;
    return 0;
}

static
VOID
QcNasReadSignal(
    _In_ PQC_MODEM Modem,
    _Inout_ PRIL_NETWORK_INFO Net)
{
    QMI_ENCODER Encoder;
    BYTE Message[32];
    BYTE Response[256];
    USHORT Mask = QCRIL_NAS_SIG_MASK_ALL;
    const VOID *Value;
    USHORT ValueLen;
    ULONG Length, ResponseLen;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_NAS_GET_SIGNAL_STRENGTH);
    QmiEncodeTlv(&Encoder, QCRIL_NAS_TLV_SIG_MASK, &Mask, sizeof(Mask));
    Length = QmiEncodeEnd(&Encoder);

    if (FAILED(QcQmiRequest(Modem, QcSvcNas, Message, Length, Response, sizeof(Response), &ResponseLen)))
        return;

    if (QmiFindTlv(Response, ResponseLen, QCRIL_NAS_TLV_SIG_STRENGTH, &Value, &ValueLen) && ValueLen >= 2)
    {
        Net->SignalDbm = (LONG)(CHAR)((const UCHAR *)Value)[0];
        Net->SignalBars = QcNasBars(Net->SignalDbm);
        if (Net->Rat == RIL_RAT_NONE)
            Net->Rat = QcNasMapRat(((const UCHAR *)Value)[1]);
    }
}

HRESULT
QcNasGetNetwork(
    _In_ PQC_MODEM Modem,
    _Inout_ PQC_RESP Resp)
{
    QMI_ENCODER Encoder;
    BYTE Message[16];
    BYTE Response[512];
    RIL_NETWORK_INFO Net;
    const UCHAR *System;
    const VOID *Value;
    USHORT ValueLen;
    ULONG Length, ResponseLen;
    UCHAR Registered = 0, RadioCount;
    HRESULT hr;

    ZeroMemory(&Net, sizeof(Net));
    Net.Size = sizeof(Net);
    Net.RegState = RIL_REG_UNKNOWN;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_NAS_GET_SERVING_SYSTEM);
    Length = QmiEncodeEnd(&Encoder);

    hr = QcQmiRequest(Modem, QcSvcNas, Message, Length, Response, sizeof(Response), &ResponseLen);
    if (FAILED(hr))
        return hr;

    if (QmiFindTlv(Response, ResponseLen, QCRIL_NAS_TLV_SERVING_SYSTEM, &Value, &ValueLen) && ValueLen >= 5)
    {
        System = Value;
        Registered = System[0];
        RadioCount = System[4];

        switch (Registered)
        {
            case 0x00: Net.RegState = RIL_REG_NOT_REGISTERED; break;
            case 0x01: Net.RegState = RIL_REG_HOME; break;
            case 0x02: Net.RegState = RIL_REG_SEARCHING; break;
            case 0x03: Net.RegState = RIL_REG_DENIED; break;
            default: Net.RegState = RIL_REG_UNKNOWN; break;
        }

        if (RadioCount >= 1 && (ULONG)5 + RadioCount <= ValueLen)
            Net.Rat = QcNasMapRat(System[5]);
    }

    /* roaming_indicator is zero when roaming is active */
    if (QmiFindTlv(Response, ResponseLen, QCRIL_NAS_TLV_ROAMING, &Value, &ValueLen) && ValueLen >= 1)
    {
        if (((const UCHAR *)Value)[0] == 0 && Net.RegState == RIL_REG_HOME)
            Net.RegState = RIL_REG_ROAMING;
        Net.Roaming = (((const UCHAR *)Value)[0] == 0) ? 1 : 0;
    }

    if (QmiFindTlv(Response, ResponseLen, QCRIL_NAS_TLV_CURRENT_PLMN, &Value, &ValueLen) && ValueLen >= 4)
    {
        const UCHAR *Plmn = Value;
        ULONG DescOffset = 4, DescLen;

        Net.Mcc = Plmn[0] | (Plmn[1] << 8);
        Net.Mnc = Plmn[2] | (Plmn[3] << 8);

        /* A length byte may precede the description, otherwise it is the remainder */
        if (ValueLen > 4 && Plmn[4] == ValueLen - 5)
        {
            DescLen = Plmn[4];
            DescOffset = 5;
        }
        else
        {
            DescLen = ValueLen - 4;
        }

        DescLen = min(DescLen, sizeof(Net.Operator) - 1);
        memcpy(Net.Operator, Plmn + DescOffset, DescLen);
        Net.Operator[DescLen] = '\0';
    }

    QcNasReadSignal(Modem, &Net);

    QcRespBytes(Resp, &Net, sizeof(Net));
    return Resp->Failed ? E_FAIL : S_OK;
}

VOID
QcNasEnableIndications(
    _In_ PQC_MODEM Modem)
{
    QMI_ENCODER Encoder;
    BYTE Message[32];
    UCHAR Enable = 1;
    ULONG Length;

    if (!QcServiceEnsure(Modem, QcSvcNas))
        return;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_NAS_INDICATION_REGISTER);
    QmiEncodeTlv(&Encoder, QCRIL_NAS_TLV_REG_SERVING, &Enable, sizeof(Enable));
    QmiEncodeTlv(&Encoder, QCRIL_NAS_TLV_REG_SIG_INFO, &Enable, sizeof(Enable));
    Length = QmiEncodeEnd(&Encoder);

    QcQmiCommand(Modem, QcSvcNas, Message, Length);
}
