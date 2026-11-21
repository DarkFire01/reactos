/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The IMS packet data session the modem's IMS stack registers over
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The modem's IMS stack registers over its own PDN on the carrier's IMS APN, and
 * waits for the host to bring that PDN up. It is a call of its own on a second
 * WDS client, so it gets a router port of its own; it is not bound to the host
 * data path, the modem keeps its packets.
 */

#include "qcril.h"
#include <string.h>

#define QCRIL_WDS_START_NETWORK         0x0020
#define QCRIL_WDS_PKT_SRV_STATUS_IND    0x0022
#define QCRIL_WDS_TLV_IP_FAMILY         0x19
#define QCRIL_WDS_TLV_PROFILE_3GPP      0x31
#define QCRIL_WDS_TLV_HANDLE            0x01
#define QCRIL_WDS_TLV_END_REASON        0x10
#define QCRIL_WDS_TLV_VERBOSE_END       0x11
#define QCRIL_WDS_IP_V6                 6
#define QCRIL_WDS_SET_IP_FAMILY         0x004D
#define QCRIL_WDS_BIND_MUX_DATA_PORT    0x00A2

/* The embedded IPA endpoint the internet session uses, on a mux of its own; the
   internet session takes the IPA client id, the first mux */
#define QC_IMS_DATA_ENDPOINT            4
#define QC_IMS_DATA_INTERFACE           0
#define QC_IMS_DATA_MUX                 2

/* Bringing a PDN up can take as long as the network makes it */
#define QC_IMS_DATA_START_TIMEOUT_MS    60000

typedef struct _QC_IMS_DATA
{
    HANDLE Port;
    HANDLE Reader;
    HANDLE Worker;
    HANDLE ResponseEvent;
    QC_ADDR Wds;
    USHORT WaitTransaction;
    USHORT NextTransaction;
    ULONG ResponseLen;
    UCHAR Profile;
    volatile LONG Active;
    BYTE Response[QC_MESSAGE_MAX];
} QC_IMS_DATA;

static QC_IMS_DATA QcImsData = { INVALID_HANDLE_VALUE };

static
VOID
QcImsDataLogEnd(
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length,
    _In_z_ PCSTR Origin)
{
    const VOID *Value;
    USHORT ValueLen, Reason = 0, Type = 0, Verbose = 0;

    if (QmiFindTlv(Message, Length, QCRIL_WDS_TLV_END_REASON, &Value, &ValueLen) && ValueLen >= 2)
        memcpy(&Reason, Value, sizeof(Reason));
    if (QmiFindTlv(Message, Length, QCRIL_WDS_TLV_VERBOSE_END, &Value, &ValueLen) && ValueLen >= 4)
    {
        memcpy(&Type, Value, sizeof(Type));
        memcpy(&Verbose, (const UCHAR *)Value + 2, sizeof(Verbose));
    }

    QcTrace("ims data: %s, end reason %u (type %u reason %u)\n", Origin, Reason, Type, Verbose);
}

/* Reads the extra port: answers for the worker, status changes of the IMS call */
static
DWORD
WINAPI
QcImsDataReader(
    _In_ LPVOID Parameter)
{
    OVERLAPPED Overlapped;
    const QMI_HEADER *Header;
    const VOID *Value;
    PUCHAR Buffer;
    HANDLE Event;
    DWORD Read;
    ULONG Length;
    USHORT ValueLen;
    BOOL Ok;

    UNREFERENCED_PARAMETER(Parameter);

    Buffer = HeapAlloc(GetProcessHeap(), 0, 8 + QC_MESSAGE_MAX);
    Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Buffer || !Event)
        goto Exit;

    for (;;)
    {
        ZeroMemory(&Overlapped, sizeof(Overlapped));
        Overlapped.hEvent = Event;

        Ok = ReadFile(QcImsData.Port, Buffer, 8 + QC_MESSAGE_MAX, NULL, &Overlapped);
        if (!Ok && GetLastError() != ERROR_IO_PENDING)
            break;
        if (!GetOverlappedResult(QcImsData.Port, &Overlapped, &Read, TRUE))
            break;

        if (Read < 8 + sizeof(QMI_HEADER))
            continue;

        Length = Read - 8;
        if (!QmiMessageValid(Buffer + 8, Length))
            continue;

        Header = (const QMI_HEADER *)(Buffer + 8);
        if (Header->Type == QMI_TYPE_RESPONSE && Header->TransactionId == QcImsData.WaitTransaction)
        {
            QcImsData.ResponseLen = min(Length, sizeof(QcImsData.Response));
            memcpy(QcImsData.Response, Buffer + 8, QcImsData.ResponseLen);
            SetEvent(QcImsData.ResponseEvent);
            continue;
        }

        if (Header->Type == QMI_TYPE_INDICATION && Header->MessageId == QCRIL_WDS_PKT_SRV_STATUS_IND)
        {
            if (QmiFindTlv(Buffer + 8, Length, 0x01, &Value, &ValueLen) && ValueLen >= 1)
            {
                QcTrace("ims data: session status %u\n", *(const UCHAR *)Value);
                if (*(const UCHAR *)Value != 2)
                    QcImsDataLogEnd(Buffer + 8, Length, "session down");
            }
            continue;
        }

        QcTrace("ims data: message type %u id 0x%04X ignored\n", Header->Type, Header->MessageId);
    }

Exit:
    if (Event)
        CloseHandle(Event);
    if (Buffer)
        HeapFree(GetProcessHeap(), 0, Buffer);
    return 0;
}

/* One request on the extra port and its answer */
static
HRESULT
QcImsDataRequest(
    _Inout_updates_bytes_(Length) PVOID Message,
    _In_ ULONG Length,
    _In_ DWORD TimeoutMs)
{
    PQMI_HEADER Header = Message;
    QMI_RESULT Result;

    if (++QcImsData.NextTransaction == 0)
        ++QcImsData.NextTransaction;

    Header->Type = QMI_TYPE_REQUEST;
    Header->TransactionId = QcImsData.NextTransaction;
    QcImsData.WaitTransaction = Header->TransactionId;
    ResetEvent(QcImsData.ResponseEvent);

    if (!QcIpcSendOn(QcImsData.Port, &QcImsData.Wds, Message, Length))
        return HRESULT_FROM_WIN32(GetLastError());

    if (WaitForSingleObject(QcImsData.ResponseEvent, TimeoutMs) != WAIT_OBJECT_0)
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);

    if (!QmiGetResult(QcImsData.Response, QcImsData.ResponseLen, &Result))
        return S_OK;

    if (Result.Result != QMI_RESULT_SUCCESS)
    {
        QcTrace("ims data: request 0x%04X failed, error 0x%x\n", Header->MessageId, Result.Error);
        return E_FAIL;
    }

    return S_OK;
}

static
DWORD
WINAPI
QcImsDataWorker(
    _In_ LPVOID Parameter)
{
    QMI_ENCODER Encoder;
    BYTE Message[64];
    const VOID *Value;
    ULONG Length;
    USHORT ValueLen;
    UCHAR Family = QCRIL_WDS_IP_V6, Mux = QC_IMS_DATA_MUX;
    ULONG Endpoint[2];
    DWORD Handle = 0;
    HRESULT hr;

    UNREFERENCED_PARAMETER(Parameter);

    /* Like the internet session the client needs its family and a data port before it starts */
    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WDS_SET_IP_FAMILY);
    QmiEncodeTlv(&Encoder, 0x01, &Family, sizeof(Family));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcImsDataRequest(Message, Length, QC_REQUEST_TIMEOUT_MS);
    QcTrace("ims data: ipv6 family preference 0x%08lx\n", hr);

    Endpoint[0] = QC_IMS_DATA_ENDPOINT;
    Endpoint[1] = QC_IMS_DATA_INTERFACE;
    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WDS_BIND_MUX_DATA_PORT);
    QmiEncodeTlv(&Encoder, 0x10, Endpoint, sizeof(Endpoint));
    QmiEncodeTlv(&Encoder, 0x11, &Mux, sizeof(Mux));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcImsDataRequest(Message, Length, QC_REQUEST_TIMEOUT_MS);
    QcTrace("ims data: bind to endpoint %lu interface %lu mux %u: 0x%08lx\n", Endpoint[0], Endpoint[1], Mux, hr);

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WDS_START_NETWORK);
    QmiEncodeTlv(&Encoder, QCRIL_WDS_TLV_PROFILE_3GPP, &QcImsData.Profile, sizeof(QcImsData.Profile));
    QmiEncodeTlv(&Encoder, QCRIL_WDS_TLV_IP_FAMILY, &Family, sizeof(Family));
    Length = QmiEncodeEnd(&Encoder);

    QcTrace("ims data: starting profile %u over ipv6\n", QcImsData.Profile);
    hr = QcImsDataRequest(Message, Length, QC_IMS_DATA_START_TIMEOUT_MS);
    if (SUCCEEDED(hr))
    {
        if (QmiFindTlv(QcImsData.Response, QcImsData.ResponseLen, QCRIL_WDS_TLV_HANDLE, &Value, &ValueLen) &&
            ValueLen >= sizeof(Handle))
        {
            memcpy(&Handle, Value, sizeof(Handle));
        }
        QcTrace("ims data: session up, handle 0x%08lx\n", Handle);
    }
    else
    {
        QcImsDataLogEnd(QcImsData.Response, QcImsData.ResponseLen, "start refused");
        QcTrace("ims data: start failed 0x%08lx\n", hr);
    }

    return 0;
}

/**
 * @brief
 * Brings up the IMS PDN once per modem start, on the carrier's IMS profile.
 *
 * @param[in] Profile
 * The 3GPP profile index whose APN is the IMS one.
 */
VOID
QcImsDataStart(
    _In_ PQC_MODEM Modem,
    _In_ UCHAR Profile)
{
    if (InterlockedExchange(&QcImsData.Active, 1))
        return;

    EnterCriticalSection(&Modem->ServiceLock);
    QcImsData.Wds = Modem->Services[QcSvcWds].Addr;
    LeaveCriticalSection(&Modem->ServiceLock);

    QcImsData.Profile = Profile;
    if (!QcImsData.ResponseEvent)
        QcImsData.ResponseEvent = CreateEventW(NULL, FALSE, FALSE, NULL);

    if (!QcImsData.ResponseEvent || !QcIpcOpenPort(&QcImsData.Port))
    {
        QcTrace("ims data: no router port for the IMS client (%lu)\n", GetLastError());
        InterlockedExchange(&QcImsData.Active, 0);
        return;
    }

    QcImsData.Reader = CreateThread(NULL, 0, QcImsDataReader, NULL, 0, NULL);
    QcImsData.Worker = CreateThread(NULL, 0, QcImsDataWorker, NULL, 0, NULL);
    if (!QcImsData.Reader || !QcImsData.Worker)
        QcTrace("ims data: worker threads failed (%lu)\n", GetLastError());
}

/* The modem restarted, its calls are gone; the next start brings the PDN up again */
VOID
QcImsDataModemLost(VOID)
{
    if (!InterlockedExchange(&QcImsData.Active, 0))
        return;

    QcIpcClosePort(QcImsData.Port);
    QcImsData.Port = INVALID_HANDLE_VALUE;
    SetEvent(QcImsData.ResponseEvent);

    if (QcImsData.Worker)
    {
        WaitForSingleObject(QcImsData.Worker, 5000);
        CloseHandle(QcImsData.Worker);
        QcImsData.Worker = NULL;
    }
    if (QcImsData.Reader)
    {
        WaitForSingleObject(QcImsData.Reader, 5000);
        CloseHandle(QcImsData.Reader);
        QcImsData.Reader = NULL;
    }
}
