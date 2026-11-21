/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The IMS packet data session the modem's IMS stack registers over
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The modem's IMS stack asks the host for its PDN through the DCM service (dcm.c),
 * the host brings it up as a call of its own on a second WDS client and hands the
 * address back. The call is not bound to the host data path, the modem keeps its
 * packets.
 */

#include "qcril.h"
#include <stdio.h>
#include <string.h>

#define QCRIL_WDS_START_NETWORK         0x0020
#define QCRIL_WDS_STOP_NETWORK          0x0021
#define QCRIL_WDS_PKT_SRV_STATUS_IND    0x0022
#define QCRIL_WDS_GET_RUNTIME_SETTINGS  0x002D
#define QCRIL_WDS_SET_IP_FAMILY         0x004D
#define QCRIL_WDS_BIND_MUX_DATA_PORT    0x00A2
#define QCRIL_WDS_TLV_IP_FAMILY         0x19
#define QCRIL_WDS_TLV_PROFILE_3GPP      0x31
#define QCRIL_WDS_TLV_HANDLE            0x01
#define QCRIL_WDS_TLV_END_REASON        0x10
#define QCRIL_WDS_TLV_VERBOSE_END       0x11
#define QCRIL_WDS_TLV_REQUESTED         0x10
#define QCRIL_WDS_TLV_IPV4_ADDRESS      0x1E
#define QCRIL_WDS_TLV_IPV6_ADDRESS      0x25
#define QCRIL_WDS_WANT_ADDRESS          (1 << 8)
#define QCRIL_WDS_IP_V4                 4
#define QCRIL_WDS_IP_V6                 6
#define QCRIL_WDS_CONNECTED             2

/* The IMS session sits on the IPA endpoint of the internet session, on a mux of its own */
#define QC_IMS_DATA_MUX                 2

/* Bringing a PDN up can take as long as the network makes it */
#define QC_IMS_DATA_START_TIMEOUT_MS    60000

/* The bind fails until IPA is up and the start until the network has attached */
#define QC_IMS_DATA_POLL_MS             2000
#define QC_IMS_DATA_READY_TIMEOUT_MS    120000

typedef struct _QC_IMS_DATA
{
    PQC_MODEM Modem;
    HANDLE Port;
    HANDLE Reader;
    HANDLE ResponseEvent;
    HANDLE StopEvent;
    CRITICAL_SECTION Lock;
    CRITICAL_SECTION RequestLock;
    QC_ADDR Wds;
    USHORT WaitTransaction;
    USHORT NextTransaction;
    ULONG ResponseLen;
    DWORD Handle;
    BOOL Up;
    BOOL Ready;
    BYTE Response[QC_MESSAGE_MAX];
} QC_IMS_DATA;

static QC_IMS_DATA QcImsData = { NULL, INVALID_HANDLE_VALUE };

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

/* Reads the extra port: answers for the requests, status changes of the IMS call */
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
    BOOL Ok, Lost;

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
                if (*(const UCHAR *)Value != QCRIL_WDS_CONNECTED)
                {
                    QcImsDataLogEnd(Buffer + 8, Length, "session down");

                    EnterCriticalSection(&QcImsData.Lock);
                    Lost = QcImsData.Up;
                    QcImsData.Up = FALSE;
                    QcImsData.Handle = 0;
                    LeaveCriticalSection(&QcImsData.Lock);

                    if (Lost)
                        QcDcmPdnLost();
                }
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
    HRESULT hr;

    /* The DCM reader and worker both ask, one exchange at a time */
    EnterCriticalSection(&QcImsData.RequestLock);

    if (++QcImsData.NextTransaction == 0)
        ++QcImsData.NextTransaction;

    Header->Type = QMI_TYPE_REQUEST;
    Header->TransactionId = QcImsData.NextTransaction;
    QcImsData.WaitTransaction = Header->TransactionId;
    ResetEvent(QcImsData.ResponseEvent);

    if (!QcIpcSendOn(QcImsData.Port, &QcImsData.Wds, Message, Length))
        hr = HRESULT_FROM_WIN32(GetLastError());
    else if (WaitForSingleObject(QcImsData.ResponseEvent, TimeoutMs) != WAIT_OBJECT_0)
        hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    else if (!QmiGetResult(QcImsData.Response, QcImsData.ResponseLen, &Result))
        hr = S_OK;
    else if (Result.Result == QMI_RESULT_SUCCESS)
        hr = S_OK;
    else
    {
        QcTrace("ims data: request 0x%04X failed, error 0x%x\n", Header->MessageId, Result.Error);
        hr = E_FAIL;
    }

    LeaveCriticalSection(&QcImsData.RequestLock);
    return hr;
}

/* The port and its reader live for as long as the modem does */
static
BOOL
QcImsDataOpen(
    _In_ PQC_MODEM Modem)
{
    if (QcImsData.Ready)
        return TRUE;

    QcImsData.Modem = Modem;
    if (!QcImsData.ResponseEvent)
        QcImsData.ResponseEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!QcImsData.StopEvent)
        QcImsData.StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!QcImsData.ResponseEvent || !QcImsData.StopEvent)
        return FALSE;
    ResetEvent(QcImsData.StopEvent);

    if (!QcIpcOpenPort(&QcImsData.Port))
    {
        QcTrace("ims data: no router port for the IMS client (%lu)\n", GetLastError());
        return FALSE;
    }

    QcImsData.Reader = CreateThread(NULL, 0, QcImsDataReader, NULL, 0, NULL);
    if (!QcImsData.Reader)
    {
        QcTrace("ims data: reader thread failed (%lu)\n", GetLastError());
        QcIpcClosePort(QcImsData.Port);
        QcImsData.Port = INVALID_HANDLE_VALUE;
        return FALSE;
    }

    QcImsData.Ready = TRUE;
    return TRUE;
}

/* The bind needs the adapter registered with IPA, the start an attached network */
static
BOOL
QcImsDataReady(
    _Out_writes_(2) PULONG Endpoint)
{
    PQC_MODEM Modem = QcImsData.Modem;
    ULONG HostMux;
    DWORD RegState;

    if (!QcWdsDataPath(&Endpoint[0], &Endpoint[1], &HostMux))
        return FALSE;

    EnterCriticalSection(&Modem->StateLock);
    RegState = Modem->Nas.Net.RegState;
    LeaveCriticalSection(&Modem->StateLock);

    return RegState == RIL_REG_HOME || RegState == RIL_REG_ROAMING || RegState == RIL_REG_ROAMING_DOMESTIC;
}

static
VOID
QcImsDataFormatV6(
    _In_reads_(16) const UCHAR *Address,
    _Out_writes_z_(Size) PSTR Text,
    _In_ ULONG Size)
{
    _snprintf(Text, Size, "%x:%x:%x:%x:%x:%x:%x:%x",
              (Address[0] << 8) | Address[1], (Address[2] << 8) | Address[3],
              (Address[4] << 8) | Address[5], (Address[6] << 8) | Address[7],
              (Address[8] << 8) | Address[9], (Address[10] << 8) | Address[11],
              (Address[12] << 8) | Address[13], (Address[14] << 8) | Address[15]);
    Text[Size - 1] = '\0';
}

/* The address the network gave the IMS call, as text */
static
HRESULT
QcImsDataQueryAddress(
    _In_ UCHAR Family,
    _Out_writes_z_(Size) PSTR Text,
    _In_ ULONG Size)
{
    QMI_ENCODER Encoder;
    BYTE Message[32];
    const VOID *Value;
    ULONG Length, Wanted = QCRIL_WDS_WANT_ADDRESS, V4;
    USHORT ValueLen;
    HRESULT hr;

    Text[0] = '\0';

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WDS_GET_RUNTIME_SETTINGS);
    QmiEncodeTlv(&Encoder, QCRIL_WDS_TLV_REQUESTED, &Wanted, sizeof(Wanted));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcImsDataRequest(Message, Length, QC_REQUEST_TIMEOUT_MS);
    if (FAILED(hr))
        return hr;

    if (Family == QCRIL_WDS_IP_V6 &&
        QmiFindTlv(QcImsData.Response, QcImsData.ResponseLen, QCRIL_WDS_TLV_IPV6_ADDRESS, &Value, &ValueLen) &&
        ValueLen >= 16)
    {
        QcImsDataFormatV6(Value, Text, Size);
        return S_OK;
    }

    if (QmiFindTlv(QcImsData.Response, QcImsData.ResponseLen, QCRIL_WDS_TLV_IPV4_ADDRESS, &Value, &ValueLen) &&
        ValueLen >= sizeof(V4))
    {
        memcpy(&V4, Value, sizeof(V4));
        _snprintf(Text, Size, "%lu.%lu.%lu.%lu", V4 >> 24, (V4 >> 16) & 0xFF, (V4 >> 8) & 0xFF, V4 & 0xFF);
        Text[Size - 1] = '\0';
        return S_OK;
    }

    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

/**
 * @brief Brings the IMS PDN up for the modem, waiting for IPA and the network first.
 *
 * @param[in] Profile
 * The 3GPP profile index of the IMS APN.
 *
 * @param[in] Family
 * 4 or 6.
 *
 * @param[out] Address
 * The address the network gave the call, as text.
 */
HRESULT
QcImsDataConnect(
    _In_ PQC_MODEM Modem,
    _In_ UCHAR Profile,
    _In_ UCHAR Family,
    _Out_writes_z_(AddressSize) PSTR Address,
    _In_ ULONG AddressSize)
{
    QMI_ENCODER Encoder;
    BYTE Message[64];
    const VOID *Value;
    ULONG Length, Endpoint[2], Waited = 0;
    USHORT ValueLen;
    UCHAR Mux = QC_IMS_DATA_MUX;
    DWORD Handle = 0;
    HRESULT hr;

    Address[0] = '\0';

    if (!QcImsDataOpen(Modem))
        return E_FAIL;

    EnterCriticalSection(&QcImsData.Lock);
    if (QcImsData.Up)
    {
        LeaveCriticalSection(&QcImsData.Lock);
        return QcImsDataQueryAddress(Family, Address, AddressSize);
    }
    LeaveCriticalSection(&QcImsData.Lock);

    while (!QcImsDataReady(Endpoint))
    {
        if (Waited == 0)
            QcTrace("ims data: waiting for the network and the IPA data path\n");
        if (Waited >= QC_IMS_DATA_READY_TIMEOUT_MS ||
            WaitForSingleObject(QcImsData.StopEvent, QC_IMS_DATA_POLL_MS) != WAIT_TIMEOUT)
        {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        Waited += QC_IMS_DATA_POLL_MS;
    }

    EnterCriticalSection(&Modem->ServiceLock);
    QcImsData.Wds = Modem->Services[QcSvcWds].Addr;
    LeaveCriticalSection(&Modem->ServiceLock);

    /* Like the internet session the client needs its family and a data port before it starts */
    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WDS_SET_IP_FAMILY);
    QmiEncodeTlv(&Encoder, 0x01, &Family, sizeof(Family));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcImsDataRequest(Message, Length, QC_REQUEST_TIMEOUT_MS);
    QcTrace("ims data: ipv%u family preference 0x%08lx\n", Family, hr);

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WDS_BIND_MUX_DATA_PORT);
    QmiEncodeTlv(&Encoder, 0x10, Endpoint, sizeof(Endpoint));
    QmiEncodeTlv(&Encoder, 0x11, &Mux, sizeof(Mux));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcImsDataRequest(Message, Length, QC_REQUEST_TIMEOUT_MS);
    QcTrace("ims data: bind to endpoint %lu interface %lu mux %u: 0x%08lx\n", Endpoint[0], Endpoint[1], Mux, hr);
    if (FAILED(hr))
        return hr;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WDS_START_NETWORK);
    QmiEncodeTlv(&Encoder, QCRIL_WDS_TLV_PROFILE_3GPP, &Profile, sizeof(Profile));
    QmiEncodeTlv(&Encoder, QCRIL_WDS_TLV_IP_FAMILY, &Family, sizeof(Family));
    Length = QmiEncodeEnd(&Encoder);

    QcTrace("ims data: starting profile %u over ipv%u\n", Profile, Family);
    hr = QcImsDataRequest(Message, Length, QC_IMS_DATA_START_TIMEOUT_MS);
    if (FAILED(hr))
    {
        QcImsDataLogEnd(QcImsData.Response, QcImsData.ResponseLen, "start refused");
        return hr;
    }

    if (QmiFindTlv(QcImsData.Response, QcImsData.ResponseLen, QCRIL_WDS_TLV_HANDLE, &Value, &ValueLen) &&
        ValueLen >= sizeof(Handle))
    {
        memcpy(&Handle, Value, sizeof(Handle));
    }

    EnterCriticalSection(&QcImsData.Lock);
    QcImsData.Handle = Handle;
    QcImsData.Up = TRUE;
    LeaveCriticalSection(&QcImsData.Lock);

    hr = QcImsDataQueryAddress(Family, Address, AddressSize);
    QcTrace("ims data: session up, handle 0x%08lx, address '%s' (0x%08lx)\n", Handle, Address, hr);
    return S_OK;
}

/* Ends the IMS call the modem asked for */
VOID
QcImsDataDisconnect(VOID)
{
    QMI_ENCODER Encoder;
    BYTE Message[32];
    ULONG Length;
    DWORD Handle;
    HRESULT hr;

    if (!QcImsData.Ready)
        return;

    EnterCriticalSection(&QcImsData.Lock);
    Handle = QcImsData.Up ? QcImsData.Handle : 0;
    QcImsData.Up = FALSE;
    QcImsData.Handle = 0;
    LeaveCriticalSection(&QcImsData.Lock);

    if (!Handle)
        return;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WDS_STOP_NETWORK);
    QmiEncodeTlv(&Encoder, QCRIL_WDS_TLV_HANDLE, &Handle, sizeof(Handle));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcImsDataRequest(Message, Length, QC_REQUEST_TIMEOUT_MS);
    QcTrace("ims data: session 0x%08lx stopped 0x%08lx\n", Handle, hr);
}

/* Current address of the IMS call, empty while it is down */
HRESULT
QcImsDataAddress(
    _In_ UCHAR Family,
    _Out_writes_z_(Size) PSTR Address,
    _In_ ULONG Size)
{
    BOOL Up;

    Address[0] = '\0';
    if (!QcImsData.Ready)
        return HRESULT_FROM_WIN32(ERROR_NOT_READY);

    EnterCriticalSection(&QcImsData.Lock);
    Up = QcImsData.Up;
    LeaveCriticalSection(&QcImsData.Lock);
    if (!Up)
        return HRESULT_FROM_WIN32(ERROR_NOT_READY);

    return QcImsDataQueryAddress(Family, Address, Size);
}

VOID
QcImsDataInitialize(VOID)
{
    InitializeCriticalSection(&QcImsData.Lock);
    InitializeCriticalSection(&QcImsData.RequestLock);
}

/* The modem restarted, its calls are gone; the next request brings the PDN up again */
VOID
QcImsDataModemLost(VOID)
{
    EnterCriticalSection(&QcImsData.Lock);
    QcImsData.Up = FALSE;
    QcImsData.Handle = 0;
    LeaveCriticalSection(&QcImsData.Lock);

    if (!QcImsData.Ready)
        return;

    QcImsData.Ready = FALSE;
    SetEvent(QcImsData.StopEvent);
    QcIpcClosePort(QcImsData.Port);
    QcImsData.Port = INVALID_HANDLE_VALUE;
    SetEvent(QcImsData.ResponseEvent);

    if (QcImsData.Reader)
    {
        WaitForSingleObject(QcImsData.Reader, 5000);
        CloseHandle(QcImsData.Reader);
        QcImsData.Reader = NULL;
    }
}
