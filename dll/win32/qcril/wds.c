/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Wireless data service, packet data connect and status
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcril.h"
#include <string.h>

#define QCRIL_WDS_START_NETWORK         0x0020
#define QCRIL_WDS_STOP_NETWORK          0x0021
#define QCRIL_WDS_GET_PKT_STATUS        0x0022

/* START request records */
#define QCRIL_WDS_TLV_APN               0x14
#define QCRIL_WDS_TLV_AUTH              0x16
#define QCRIL_WDS_TLV_USERNAME          0x17
#define QCRIL_WDS_TLV_PASSWORD          0x18
#define QCRIL_WDS_TLV_IP_FAMILY         0x19

/* START response and STOP request carry the packet data handle in record 0x01 */
#define QCRIL_WDS_TLV_HANDLE            0x01
#define QCRIL_WDS_TLV_CONN_STATUS       0x01

/* wds_connection_status_enum_v01 */
#define QCRIL_WDS_CONNECTED             0x02

#define QCRIL_WDS_IP_V4                 0x04

/* A single packet data session, serialised through the command worker */
static DWORD gWdsHandle;

static
HRESULT
QcWdsConnect(
    _In_ PQC_MODEM Modem,
    _In_ const RIL_DATA_REQUEST *Request,
    _Out_ PDWORD Handle)
{
    QMI_ENCODER Encoder;
    BYTE Message[256];
    BYTE Response[128];
    const VOID *Value;
    USHORT ValueLen, ApnLen;
    ULONG Length, ResponseLen;
    UCHAR IpFamily = QCRIL_WDS_IP_V4;
    HRESULT hr;

    *Handle = 0;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WDS_START_NETWORK);

    ApnLen = (USHORT)strnlen(Request->Apn, sizeof(Request->Apn));
    if (ApnLen)
        QmiEncodeTlv(&Encoder, QCRIL_WDS_TLV_APN, Request->Apn, ApnLen);

    if (Request->AuthType != RIL_DATA_AUTH_NONE)
    {
        UCHAR Auth = (UCHAR)Request->AuthType;
        USHORT UserLen = (USHORT)strnlen(Request->Username, sizeof(Request->Username));
        USHORT PassLen = (USHORT)strnlen(Request->Password, sizeof(Request->Password));

        QmiEncodeTlv(&Encoder, QCRIL_WDS_TLV_AUTH, &Auth, sizeof(Auth));
        if (UserLen)
            QmiEncodeTlv(&Encoder, QCRIL_WDS_TLV_USERNAME, Request->Username, UserLen);
        if (PassLen)
            QmiEncodeTlv(&Encoder, QCRIL_WDS_TLV_PASSWORD, Request->Password, PassLen);
    }

    QmiEncodeTlv(&Encoder, QCRIL_WDS_TLV_IP_FAMILY, &IpFamily, sizeof(IpFamily));
    Length = QmiEncodeEnd(&Encoder);

    hr = QcQmiRequest(Modem, QcSvcWds, Message, Length, Response, sizeof(Response), &ResponseLen);
    if (FAILED(hr))
        return hr;

    if (QmiFindTlv(Response, ResponseLen, QCRIL_WDS_TLV_HANDLE, &Value, &ValueLen) && ValueLen >= sizeof(DWORD))
        memcpy(Handle, Value, sizeof(DWORD));

    return S_OK;
}

static
HRESULT
QcWdsDisconnect(
    _In_ PQC_MODEM Modem,
    _In_ DWORD Handle)
{
    QMI_ENCODER Encoder;
    BYTE Message[32];
    ULONG Length;

    if (Handle == 0)
        return S_OK;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WDS_STOP_NETWORK);
    QmiEncodeTlv(&Encoder, QCRIL_WDS_TLV_HANDLE, &Handle, sizeof(Handle));
    Length = QmiEncodeEnd(&Encoder);

    return QcQmiCommand(Modem, QcSvcWds, Message, Length);
}

static
BOOL
QcWdsConnected(
    _In_ PQC_MODEM Modem)
{
    QMI_ENCODER Encoder;
    BYTE Message[16];
    BYTE Response[128];
    const VOID *Value;
    USHORT ValueLen;
    ULONG Length, ResponseLen;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WDS_GET_PKT_STATUS);
    Length = QmiEncodeEnd(&Encoder);

    if (FAILED(QcQmiRequest(Modem, QcSvcWds, Message, Length, Response, sizeof(Response), &ResponseLen)))
        return FALSE;

    if (QmiFindTlv(Response, ResponseLen, QCRIL_WDS_TLV_CONN_STATUS, &Value, &ValueLen) && ValueLen >= 1)
        return ((const UCHAR *)Value)[0] == QCRIL_WDS_CONNECTED;

    return FALSE;
}

HRESULT
QcWdsData(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(InSize) const VOID *In,
    _In_ ULONG InSize,
    _Inout_ PQC_RESP Resp)
{
    const RIL_DATA_REQUEST *Request = In;
    RIL_DATA_STATUS Status;
    HRESULT hr = S_OK;

    if (InSize < sizeof(RIL_DATA_REQUEST))
        return E_INVALIDARG;

    if (!QcServiceEnsure(Modem, QcSvcWds))
        return RIL_E_RADIONOTAVAILABLE;

    ZeroMemory(&Status, sizeof(Status));
    Status.Size = sizeof(Status);

    switch (Request->Op)
    {
        case RIL_DATA_OP_CONNECT:
            hr = QcWdsConnect(Modem, Request, &gWdsHandle);
            break;

        case RIL_DATA_OP_DISCONNECT:
            hr = QcWdsDisconnect(Modem, gWdsHandle);
            gWdsHandle = 0;
            break;

        case RIL_DATA_OP_STATUS:
        default:
            break;
    }

    if (FAILED(hr))
        return hr;

    Status.Handle = gWdsHandle;
    Status.Connected = QcWdsConnected(Modem) ? 1 : 0;

    QcRespBytes(Resp, &Status, sizeof(Status));
    return Resp->Failed ? E_FAIL : S_OK;
}
