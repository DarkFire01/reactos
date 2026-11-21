/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Wireless data service, packet data connect and status
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcril.h"
#include <string.h>
#include <winioctl.h>
#include <telephony/rmnetioctl.h>

#define QCRIL_WDS_START_NETWORK         0x0020
#define QCRIL_WDS_STOP_NETWORK          0x0021
#define QCRIL_WDS_GET_PKT_STATUS        0x0022
#define QCRIL_WDS_GET_RUNTIME_SETTINGS  0x002D
#define QCRIL_WDS_SET_IP_FAMILY         0x004D
#define QCRIL_WDS_BIND_MUX_DATA_PORT    0x00A2

/* BIND_MUX_DATA_PORT records: the IPA endpoint and the mux the adapter registered as */
#define QCRIL_WDS_TLV_ENDPOINT          0x10
#define QCRIL_WDS_TLV_MUX_ID            0x11

/* GET_RUNTIME_SETTINGS: what to ask for, and the IPv4 records of the answer */
#define QCRIL_WDS_TLV_REQUESTED         0x10
#define QCRIL_WDS_TLV_DNS1              0x15
#define QCRIL_WDS_TLV_DNS2              0x16
#define QCRIL_WDS_TLV_ADDRESS           0x1E
#define QCRIL_WDS_TLV_GATEWAY           0x20
#define QCRIL_WDS_TLV_SUBNET            0x21
#define QCRIL_WDS_TLV_MTU               0x29
#define QCRIL_WDS_WANT_DNS              (1 << 4)
#define QCRIL_WDS_WANT_ADDRESS          (1 << 8)
#define QCRIL_WDS_WANT_GATEWAY          (1 << 9)
#define QCRIL_WDS_WANT_MTU              (1 << 13)

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
#define QCRIL_WDS_DISCONNECTED          0x01
#define QCRIL_WDS_CONNECTED             0x02
#define QCRIL_WDS_PKT_SRV_STATUS_IND    0x0022

#define QCRIL_WDS_IP_V4                 0x04

/* A single packet data session, serialised through the command worker */
static DWORD gWdsHandle;
static BOOL gWdsLinkUp;

#include <pshpack1.h>
typedef struct _QCRIL_WDS_ENDPOINT
{
    ULONG Type;
    ULONG InterfaceId;
} QCRIL_WDS_ENDPOINT;
#include <poppack.h>

/* Talks to the cellular data adapter, which carries the session's packets through IPA */
static
BOOL
QcRmNetIoctl(
    _In_ DWORD Code,
    _In_reads_bytes_opt_(InSize) PVOID In,
    _In_ DWORD InSize,
    _Out_writes_bytes_opt_(OutSize) PVOID Out,
    _In_ DWORD OutSize)
{
    HANDLE Device;
    DWORD Returned;
    BOOL Ok;

    /* Shared, the IMS session polls the adapter while the internet session may be using it */
    Device = CreateFileW(RMNET_CONTROL_DEVICE,
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL,
                         OPEN_EXISTING,
                         0,
                         NULL);
    if (Device == INVALID_HANDLE_VALUE)
        return FALSE;

    Ok = DeviceIoControl(Device, Code, In, InSize, Out, OutSize, &Returned, NULL);
    CloseHandle(Device);
    return Ok;
}

/**
 * @brief Reports the IPA endpoint a session can bind to once the adapter has registered with IPA.
 *
 * @return FALSE while IPA is not ready or the adapter is missing.
 */
BOOL
QcWdsDataPath(
    _Out_ PULONG EndpointType,
    _Out_ PULONG InterfaceId,
    _Out_ PULONG MuxId)
{
    RMNET_QUERY Adapter;

    ZeroMemory(&Adapter, sizeof(Adapter));
    if (!QcRmNetIoctl(IOCTL_RMNET_QUERY, NULL, 0, &Adapter, sizeof(Adapter)) ||
        Adapter.State == RMNET_STATE_NO_IPA)
    {
        return FALSE;
    }

    *EndpointType = Adapter.EndpointType;
    *InterfaceId = Adapter.InterfaceId;
    *MuxId = Adapter.MuxId;
    return TRUE;
}

static
ULONG
QcNetworkOrder(
    _In_ ULONG Value)
{
    return ((Value & 0xFF) << 24) | ((Value & 0xFF00) << 8) | ((Value >> 8) & 0xFF00) | (Value >> 24);
}

/* The session must be bound to the adapter's mux before it starts, or its packets go nowhere */
static
VOID
QcWdsBindDataPath(
    _In_ PQC_MODEM Modem,
    _In_ const RMNET_QUERY *Adapter)
{
    QCRIL_WDS_ENDPOINT Endpoint;
    QMI_ENCODER Encoder;
    BYTE Message[64];
    ULONG Length;
    UCHAR Family = QCRIL_WDS_IP_V4;
    UCHAR Mux = (UCHAR)Adapter->MuxId;
    HRESULT hr;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WDS_SET_IP_FAMILY);
    QmiEncodeTlv(&Encoder, 0x01, &Family, sizeof(Family));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiCommand(Modem, QcSvcWds, Message, Length);
    if (FAILED(hr))
        QcTrace("data: IPv4 family preference 0x%08lx\n", hr);

    Endpoint.Type = Adapter->EndpointType;
    Endpoint.InterfaceId = Adapter->InterfaceId;
    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WDS_BIND_MUX_DATA_PORT);
    QmiEncodeTlv(&Encoder, QCRIL_WDS_TLV_ENDPOINT, &Endpoint, sizeof(Endpoint));
    QmiEncodeTlv(&Encoder, QCRIL_WDS_TLV_MUX_ID, &Mux, sizeof(Mux));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiCommand(Modem, QcSvcWds, Message, Length);
    QcTrace("data: bind to endpoint %lu interface %lu mux %u: 0x%08lx\n",
            Endpoint.Type, Endpoint.InterfaceId, Mux, hr);
}

static
ULONG
QcWdsAddress(
    _In_reads_bytes_(ResponseLen) const VOID *Response,
    _In_ ULONG ResponseLen,
    _In_ UCHAR Type)
{
    const VOID *Value;
    USHORT ValueLen;
    ULONG Address;

    if (!QmiFindTlv(Response, ResponseLen, Type, &Value, &ValueLen) || ValueLen < sizeof(ULONG))
        return 0;

    memcpy(&Address, Value, sizeof(Address));
    return QcNetworkOrder(Address);
}

/* Hands the address the network gave the session to the adapter, which serves it to tcpip */
static
HRESULT
QcWdsLinkUp(
    _In_ PQC_MODEM Modem)
{
    QMI_ENCODER Encoder;
    BYTE Message[32];
    BYTE Response[512];
    ULONG Length, ResponseLen, Wanted;
    const VOID *Value;
    USHORT ValueLen;
    RMNET_LINK Link;
    HRESULT hr;

    Wanted = QCRIL_WDS_WANT_DNS | QCRIL_WDS_WANT_ADDRESS | QCRIL_WDS_WANT_GATEWAY | QCRIL_WDS_WANT_MTU;
    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WDS_GET_RUNTIME_SETTINGS);
    QmiEncodeTlv(&Encoder, QCRIL_WDS_TLV_REQUESTED, &Wanted, sizeof(Wanted));
    Length = QmiEncodeEnd(&Encoder);

    hr = QcQmiRequest(Modem, QcSvcWds, Message, Length, Response, sizeof(Response), &ResponseLen);
    if (FAILED(hr))
    {
        QcTrace("data: runtime settings 0x%08lx\n", hr);
        return hr;
    }

    ZeroMemory(&Link, sizeof(Link));
    Link.Version = RMNET_INTERFACE_VERSION;
    Link.Up = TRUE;
    Link.Address = QcWdsAddress(Response, ResponseLen, QCRIL_WDS_TLV_ADDRESS);
    Link.SubnetMask = QcWdsAddress(Response, ResponseLen, QCRIL_WDS_TLV_SUBNET);
    Link.Gateway = QcWdsAddress(Response, ResponseLen, QCRIL_WDS_TLV_GATEWAY);
    Link.PrimaryDns = QcWdsAddress(Response, ResponseLen, QCRIL_WDS_TLV_DNS1);
    Link.SecondaryDns = QcWdsAddress(Response, ResponseLen, QCRIL_WDS_TLV_DNS2);
    if (QmiFindTlv(Response, ResponseLen, QCRIL_WDS_TLV_MTU, &Value, &ValueLen) && ValueLen >= sizeof(ULONG))
        memcpy(&Link.Mtu, Value, sizeof(Link.Mtu));

    QcTrace("data: address %08lx mask %08lx gateway %08lx dns %08lx %08lx mtu %lu\n",
            QcNetworkOrder(Link.Address), QcNetworkOrder(Link.SubnetMask), QcNetworkOrder(Link.Gateway),
            QcNetworkOrder(Link.PrimaryDns), QcNetworkOrder(Link.SecondaryDns), Link.Mtu);

    if (!Link.Address)
        return E_FAIL;

    if (!QcRmNetIoctl(IOCTL_RMNET_SET_LINK, &Link, sizeof(Link), NULL, 0))
    {
        QcTrace("data: the data adapter refused the link (%lu)\n", GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }

    gWdsLinkUp = TRUE;
    return S_OK;
}

static
VOID
QcWdsLinkDown(VOID)
{
    RMNET_LINK Link;

    ZeroMemory(&Link, sizeof(Link));
    Link.Version = RMNET_INTERFACE_VERSION;
    QcRmNetIoctl(IOCTL_RMNET_SET_LINK, &Link, sizeof(Link), NULL, 0);
    gWdsLinkUp = FALSE;
}

/* A link the adapter still holds from an earlier host process has no session behind it */
VOID
QcWdsReset(VOID)
{
    RMNET_QUERY Adapter;

    ZeroMemory(&Adapter, sizeof(Adapter));
    if (!QcRmNetIoctl(IOCTL_RMNET_QUERY, NULL, 0, &Adapter, sizeof(Adapter)) ||
        Adapter.State != RMNET_STATE_CONNECTED)
    {
        return;
    }

    QcTrace("data: the data adapter still has a link from before, taking it down\n");
    QcWdsLinkDown();
}

/**
 * @brief
 * The modem or its data service went away and the packet data call with it.
 * The adapter reports the media disconnected until a client connects again.
 */
VOID
QcWdsModemLost(
    _In_ PQC_MODEM Modem)
{
    UNREFERENCED_PARAMETER(Modem);

    if (!gWdsHandle && !gWdsLinkUp)
        return;

    QcTrace("ssr: data session 0x%08lx lost, the data link goes down\n", gWdsHandle);
    gWdsHandle = 0;
    QcWdsLinkDown();
}

/* Packet service status: the modem ended the call, so the adapter loses its address */
VOID
QcWdsIndication(
    _In_ PQC_MODEM Modem,
    _In_ USHORT MessageId,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    const UCHAR *Status;
    const VOID *Value;
    USHORT ValueLen;
    USHORT Reason = 0;
    USHORT VerboseType = 0, VerboseReason = 0;

    UNREFERENCED_PARAMETER(Modem);

    if (MessageId != QCRIL_WDS_PKT_SRV_STATUS_IND)
    {
        QcTrace("ind WDS 0x%04X not handled\n", MessageId);
        return;
    }

    if (!QmiFindTlv(Message, Length, 0x01, &Value, &ValueLen) || ValueLen < 2)
        return;
    Status = Value;

    if (QmiFindTlv(Message, Length, 0x10, &Value, &ValueLen) && ValueLen >= 2)
        memcpy(&Reason, Value, sizeof(Reason));
    if (QmiFindTlv(Message, Length, 0x11, &Value, &ValueLen) && ValueLen >= 4)
    {
        memcpy(&VerboseType, Value, sizeof(VerboseType));
        memcpy(&VerboseReason, (const UCHAR *)Value + 2, sizeof(VerboseReason));
    }

    QcTrace("data: packet service status %u reconfigure %u, end reason %u (type %u reason %u)\n",
            Status[0], Status[1], Reason, VerboseType, VerboseReason);

    if (Status[0] == QCRIL_WDS_DISCONNECTED)
        QcWdsLinkDown();
}

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
        {
            RMNET_QUERY Adapter;

            /* Without the data adapter the session still comes up, it just carries nothing */
            ZeroMemory(&Adapter, sizeof(Adapter));
            if (QcRmNetIoctl(IOCTL_RMNET_QUERY, NULL, 0, &Adapter, sizeof(Adapter)) &&
                Adapter.State != RMNET_STATE_NO_IPA)
            {
                QcWdsBindDataPath(Modem, &Adapter);
            }
            else
            {
                QcTrace("data: no cellular data adapter (%lu), the session will carry no packets\n",
                        GetLastError());
            }

            hr = QcWdsConnect(Modem, Request, &gWdsHandle);
            if (SUCCEEDED(hr))
                QcWdsLinkUp(Modem);
            break;
        }

        case RIL_DATA_OP_DISCONNECT:
            QcWdsLinkDown();
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
