/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Device management service, radio power and identity
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcril.h"
#include <string.h>

#define QCRIL_DMS_SET_OPERATING_MODE    0x002E
#define QCRIL_DMS_GET_DEVICE_MFR        0x0021
#define QCRIL_DMS_GET_DEVICE_MODEL      0x0022
#define QCRIL_DMS_GET_DEVICE_REV        0x0023
#define QCRIL_DMS_GET_DEVICE_SERIAL     0x0025

/* dms_operating_mode_enum_v01 values on the wire */
#define QCRIL_DMS_MODE_ONLINE           0x00
#define QCRIL_DMS_MODE_LOW_POWER        0x01

/* The radio stack answers this until it has finished coming up after boot */
#define QCRIL_DMS_ERR_DEVICE_NOT_READY  0x0034
#define QCRIL_RADIO_ON_TRIES            20
#define QCRIL_RADIO_ON_DELAY_MS         2000

/* Serial number response: IMEI is record 0x11, simple getters use 0x01 */
#define QCRIL_DMS_TLV_IMEI              0x11
#define QCRIL_DMS_TLV_STRING            0x01
#define QCRIL_DMS_TLV_OPERATING_MODE    0x01

HRESULT
QcDmsSetRadioPower(
    _In_ PQC_MODEM Modem,
    _In_ BOOL On)
{
    QMI_ENCODER Encoder;
    BYTE Message[32];
    BYTE Response[256];
    UCHAR Mode;
    ULONG Length, ResponseLen;
    QMI_RESULT Result;
    HRESULT hr;
    ULONG Attempt;

    Mode = On ? QCRIL_DMS_MODE_ONLINE : QCRIL_DMS_MODE_LOW_POWER;

    for (Attempt = 0; Attempt < QCRIL_RADIO_ON_TRIES; Attempt++)
    {
        QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_DMS_SET_OPERATING_MODE);
        if (!QmiEncodeTlv(&Encoder, QCRIL_DMS_TLV_OPERATING_MODE, &Mode, sizeof(Mode)))
            return E_FAIL;
        Length = QmiEncodeEnd(&Encoder);

        hr = QcQmiRequest(Modem, QcSvcDms, Message, Length, Response, sizeof(Response), &ResponseLen);
        if (SUCCEEDED(hr))
        {
            QcTrace("operating mode %u accepted", Mode);
            return S_OK;
        }
        QcTrace("operating mode %u attempt %lu: 0x%08lx, QMI error 0x%x", Mode, Attempt, hr,
                QmiGetResult(Response, ResponseLen, &Result) ? Result.Error : 0xFFFF);

        /* The modem rejects an operating mode change while the radio stack is
         * still coming up, so wait for it before giving up */
        if (!On ||
            !QmiGetResult(Response, ResponseLen, &Result) ||
            Result.Error != QCRIL_DMS_ERR_DEVICE_NOT_READY)
        {
            return hr;
        }

        Sleep(QCRIL_RADIO_ON_DELAY_MS);
    }

    return RIL_E_RADIONOTAVAILABLE;
}

/* Reads one ASCII string record from a device management response into Out */
HRESULT
QcDmsReadString(
    _In_ PQC_MODEM Modem,
    _In_ USHORT MessageId,
    _In_ UCHAR Tlv,
    _Out_writes_z_(OutSize) PSTR Out,
    _In_ ULONG OutSize)
{
    QMI_ENCODER Encoder;
    BYTE Message[16];
    BYTE Response[256];
    const VOID *Value;
    USHORT ValueLen;
    ULONG Length, ResponseLen, Copy;
    HRESULT hr;

    Out[0] = '\0';

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, MessageId);
    Length = QmiEncodeEnd(&Encoder);

    hr = QcQmiRequest(Modem, QcSvcDms, Message, Length, Response, sizeof(Response), &ResponseLen);
    if (FAILED(hr))
        return hr;

    if (!QmiFindTlv(Response, ResponseLen, Tlv, &Value, &ValueLen))
        return RIL_E_NOTFOUND;

    Copy = min(ValueLen, OutSize - 1);
    memcpy(Out, Value, Copy);
    Out[Copy] = '\0';
    return S_OK;
}

HRESULT
QcDmsGetDeviceInfo(
    _In_ PQC_MODEM Modem,
    _Inout_ PQC_RESP Resp)
{
    RIL_DEVICE_INFO Info;

    ZeroMemory(&Info, sizeof(Info));
    Info.Size = sizeof(Info);

    /* The IMEI is the identity that matters, the rest is best effort */
    if (FAILED(QcDmsReadString(Modem, QCRIL_DMS_GET_DEVICE_SERIAL, QCRIL_DMS_TLV_IMEI,
                               Info.Imei, sizeof(Info.Imei))))
    {
        return RIL_E_RADIONOTAVAILABLE;
    }

    QcDmsReadString(Modem, QCRIL_DMS_GET_DEVICE_MFR, QCRIL_DMS_TLV_STRING,
                    Info.Manufacturer, sizeof(Info.Manufacturer));
    QcDmsReadString(Modem, QCRIL_DMS_GET_DEVICE_MODEL, QCRIL_DMS_TLV_STRING,
                    Info.Model, sizeof(Info.Model));
    QcDmsReadString(Modem, QCRIL_DMS_GET_DEVICE_REV, QCRIL_DMS_TLV_STRING,
                    Info.Revision, sizeof(Info.Revision));

    QcRespBytes(Resp, &Info, sizeof(Info));
    return Resp->Failed ? E_FAIL : S_OK;
}
