/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     IMS data connection manager, the host service the modem's IMS stack asks for its PDN
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The modem's IMS stack does not bring its PDN up itself. It looks up the DCM
 * service on the host, asks it to activate a PDP on the IMS APN, and registers
 * once the host reports the PDP up with its address. The host serves DCM as a
 * QMI server on the IPC router.
 */

#include "qcril.h"
#include <stdio.h>
#include <string.h>

#define QC_DCM_SERVICE              0x0302
#define QC_DCM_INSTANCE             0x01

#define QC_DCM_PDP_ACTIVATE         0x0020
#define QC_DCM_PDP_DEACTIVATE       0x0021
#define QC_DCM_GET_IP_ADDRESS       0x0022
#define QC_DCM_LINK_ADDR            0x0023
#define QC_DCM_ADDRESS_CHANGE_IND   0x0024

/* Records */
#define QC_DCM_TLV_APN_INFO         0x01
#define QC_DCM_TLV_PDP_ID           0x01
#define QC_DCM_TLV_COOKIE           0x10
#define QC_DCM_TLV_RSP_PDP_ID       0x10
#define QC_DCM_TLV_RSP_COOKIE       0x11
#define QC_DCM_TLV_IND_COOKIE       0x10
#define QC_DCM_TLV_IND_ADDRESS      0x11
#define QC_DCM_TLV_ADDRESS          0x10

/* Address families as DCM numbers them */
#define QC_DCM_FAMILY_V4            0
#define QC_DCM_FAMILY_V6            1

#define QC_DCM_ADDRESS_MAX          40

/* What the host reports when the PDP did not come up or went down */
#define QC_DCM_ERROR_NO_NETWORK     13
#define QC_QMI_ERR_NOT_SUPPORTED    0x5E

/* The one PDP the host hands out */
#define QC_DCM_PDP                  1

typedef struct _QC_DCM
{
    PQC_MODEM Modem;
    HANDLE Port;
    HANDLE Reader;
    HANDLE Worker;
    HANDLE WorkEvent;
    CRITICAL_SECTION Lock;
    volatile LONG Started;

    /* Who asked for the PDP and what for */
    QC_ADDR Client;
    BOOL HaveClient;
    BOOL CookieValid;
    ULONG Cookie;
    UCHAR Family;
    UCHAR Profile;
    UCHAR FallbackProfile;
    BOOL Want;
    BOOL Up;
    CHAR Address[QC_DCM_ADDRESS_MAX + 1];
} QC_DCM;

static QC_DCM QcDcm = { NULL, INVALID_HANDLE_VALUE };

static
VOID
QcDcmTraceBytes(
    _In_z_ PCSTR Label,
    _In_reads_bytes_(Length) const VOID *Data,
    _In_ ULONG Length)
{
    CHAR Line[3 * 48 + 1];
    ULONG Index, Count = min(Length, 48);

    for (Index = 0; Index < Count; Index++)
        _snprintf(Line + 3 * Index, 4, "%02x ", ((const UCHAR *)Data)[Index]);
    Line[3 * Count] = '\0';

    QcTrace("dcm: %s (%lu bytes) %s\n", Label, Length, Line);
}

static
VOID
QcDcmSend(
    _In_ const QC_ADDR *To,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    if (!QcIpcSendOn(QcDcm.Port, To, Message, Length))
        QcTrace("dcm: send to %lu:%lu failed (%lu)\n", To->Node, To->Port, GetLastError());
}

/* The address record: family, then the address as text with a one byte length */
static
BOOLEAN
QcDcmEncodeAddress(
    _Inout_ PQMI_ENCODER Encoder,
    _In_ UCHAR Type,
    _In_ UCHAR Family,
    _In_z_ PCSTR Address)
{
    UCHAR Record[4 + 1 + QC_DCM_ADDRESS_MAX];
    ULONG DcmFamily = Family;
    UCHAR Length = (UCHAR)min(strlen(Address), QC_DCM_ADDRESS_MAX);

    memcpy(Record, &DcmFamily, sizeof(DcmFamily));
    Record[4] = Length;
    memcpy(Record + 5, Address, Length);
    return QmiEncodeTlv(Encoder, Type, Record, (USHORT)(5 + Length));
}

/* PDP_ACTIVATE_IND, with the address when the PDP is up and the error when it is not */
static
VOID
QcDcmSendActivateInd(
    _In_ BOOL Up)
{
    QMI_ENCODER Encoder;
    BYTE Message[128];
    QC_ADDR Client;
    UCHAR Pdp = QC_DCM_PDP;
    CHAR Address[QC_DCM_ADDRESS_MAX + 1];
    BOOL CookieValid;
    ULONG Cookie, Length;
    UCHAR Family;

    EnterCriticalSection(&QcDcm.Lock);
    if (!QcDcm.HaveClient)
    {
        LeaveCriticalSection(&QcDcm.Lock);
        return;
    }
    Client = QcDcm.Client;
    CookieValid = QcDcm.CookieValid;
    Cookie = QcDcm.Cookie;
    Family = QcDcm.Family;
    memcpy(Address, QcDcm.Address, sizeof(Address));
    LeaveCriticalSection(&QcDcm.Lock);

    /* The result record keeps a success result, only the error says the PDP is gone */
    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_INDICATION, 0, QC_DCM_PDP_ACTIVATE);
    QmiEncodeResult(&Encoder, QMI_RESULT_SUCCESS, Up ? 0 : QC_DCM_ERROR_NO_NETWORK);
    QmiEncodeTlv(&Encoder, QC_DCM_TLV_PDP_ID, &Pdp, sizeof(Pdp));
    if (CookieValid)
        QmiEncodeTlv(&Encoder, QC_DCM_TLV_IND_COOKIE, &Cookie, sizeof(Cookie));
    if (Up && Address[0])
        QcDcmEncodeAddress(&Encoder, QC_DCM_TLV_IND_ADDRESS, Family, Address);
    Length = QmiEncodeEnd(&Encoder);

    QcTrace("dcm: pdp %u %s, address '%s'\n", Pdp, Up ? "up" : "down", Up ? Address : "");
    QcDcmSend(&Client, Message, Length);
}

static
VOID
QcDcmRespond(
    _In_ const QC_ADDR *From,
    _In_ USHORT Transaction,
    _In_ USHORT MessageId,
    _In_ USHORT Result,
    _In_ USHORT Error)
{
    QMI_ENCODER Encoder;
    BYTE Message[32];
    ULONG Length;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_RESPONSE, Transaction, MessageId);
    QmiEncodeResult(&Encoder, Result, Error);
    Length = QmiEncodeEnd(&Encoder);
    QcDcmSend(From, Message, Length);
}

/* The APN record: the name, then APN type, RAT, family and profile as four words */
static
VOID
QcDcmActivate(
    _In_ const QC_ADDR *From,
    _In_ USHORT Transaction,
    _In_reads_bytes_(Length) const VOID *Request,
    _In_ ULONG Length)
{
    QMI_ENCODER Encoder;
    BYTE Message[64];
    const UCHAR *Value;
    const VOID *Cookie;
    CHAR Apn[101];
    ULONG Words[4], NameLength, CookieWord = 0, MessageLength;
    USHORT ValueLen, CookieLen;
    UCHAR Pdp = QC_DCM_PDP;
    BOOL CookieValid, Start;

    if (!QmiFindTlv(Request, Length, QC_DCM_TLV_APN_INFO, (const VOID **)&Value, &ValueLen) ||
        ValueLen < sizeof(Words))
    {
        QcTrace("dcm: pdp activate without an APN record\n");
        return;
    }
    QcDcmTraceBytes("pdp activate apn record", Value, ValueLen);

    /* The words close the record, a length byte may lead the name */
    memcpy(Words, Value + ValueLen - sizeof(Words), sizeof(Words));
    NameLength = ValueLen - sizeof(Words);
    if (NameLength && Value[0] == NameLength - 1)
    {
        Value++;
        NameLength--;
    }
    NameLength = min(NameLength, sizeof(Apn) - 1);
    memcpy(Apn, Value, NameLength);
    Apn[NameLength] = '\0';

    CookieValid = QmiFindTlv(Request, Length, QC_DCM_TLV_COOKIE, &Cookie, &CookieLen) &&
                  CookieLen >= sizeof(CookieWord);
    if (CookieValid)
        memcpy(&CookieWord, Cookie, sizeof(CookieWord));

    QcTrace("dcm: pdp activate apn '%s' type %lu rat %lu family %lu profile %lu, cookie %s %lu\n",
            Apn, Words[0], Words[1], Words[2], Words[3], CookieValid ? "valid" : "none", CookieWord);

    if (Words[2] != QC_DCM_FAMILY_V4 && Words[2] != QC_DCM_FAMILY_V6)
    {
        QcTrace("dcm: family %lu is neither IPv4 nor IPv6, no answer\n", Words[2]);
        return;
    }

    EnterCriticalSection(&QcDcm.Lock);
    QcDcm.Client = *From;
    QcDcm.HaveClient = TRUE;
    QcDcm.CookieValid = CookieValid;
    QcDcm.Cookie = CookieWord;
    QcDcm.Family = (UCHAR)Words[2];
    QcDcm.Profile = (Words[3] && Words[3] <= 0xFF) ? (UCHAR)Words[3] : QcDcm.FallbackProfile;
    Start = !QcDcm.Up;
    QcDcm.Want = TRUE;
    LeaveCriticalSection(&QcDcm.Lock);

    /* The PDP id is handed out right away, the PDP itself follows in an indication */
    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_RESPONSE, Transaction, QC_DCM_PDP_ACTIVATE);
    QmiEncodeResult(&Encoder, QMI_RESULT_SUCCESS, 0);
    QmiEncodeTlv(&Encoder, QC_DCM_TLV_RSP_PDP_ID, &Pdp, sizeof(Pdp));
    QmiEncodeTlv(&Encoder, QC_DCM_TLV_RSP_COOKIE, &CookieWord, sizeof(CookieWord));
    MessageLength = QmiEncodeEnd(&Encoder);
    QcDcmSend(From, Message, MessageLength);

    if (Start)
        SetEvent(QcDcm.WorkEvent);
    else
        QcDcmSendActivateInd(TRUE);
}

static
VOID
QcDcmDeactivate(
    _In_ const QC_ADDR *From,
    _In_ USHORT Transaction,
    _In_reads_bytes_(Length) const VOID *Request,
    _In_ ULONG Length)
{
    QMI_ENCODER Encoder;
    BYTE Message[32];
    const VOID *Value;
    USHORT ValueLen;
    UCHAR Pdp = 0;
    ULONG MessageLength;

    if (QmiFindTlv(Request, Length, QC_DCM_TLV_PDP_ID, &Value, &ValueLen) && ValueLen >= 1)
        Pdp = *(const UCHAR *)Value;
    QcTrace("dcm: pdp %u deactivate\n", Pdp);

    EnterCriticalSection(&QcDcm.Lock);
    QcDcm.Want = FALSE;
    LeaveCriticalSection(&QcDcm.Lock);

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_RESPONSE, Transaction, QC_DCM_PDP_DEACTIVATE);
    QmiEncodeResult(&Encoder, QMI_RESULT_SUCCESS, 0);
    QmiEncodeTlv(&Encoder, QC_DCM_TLV_RSP_PDP_ID, &Pdp, sizeof(Pdp));
    MessageLength = QmiEncodeEnd(&Encoder);
    QcDcmSend(From, Message, MessageLength);

    SetEvent(QcDcm.WorkEvent);
}

/* Answered at once, the address follows in an indication of the same id */
static
VOID
QcDcmGetIpAddress(
    _In_ const QC_ADDR *From,
    _In_ USHORT Transaction,
    _In_reads_bytes_(Length) const VOID *Request,
    _In_ ULONG Length)
{
    QMI_ENCODER Encoder;
    BYTE Message[96];
    const VOID *Value;
    CHAR Address[QC_DCM_ADDRESS_MAX + 1];
    USHORT ValueLen;
    UCHAR Pdp = QC_DCM_PDP, Family;
    ULONG MessageLength;
    BOOL Up;

    if (QmiFindTlv(Request, Length, QC_DCM_TLV_PDP_ID, &Value, &ValueLen) && ValueLen >= 1)
        Pdp = *(const UCHAR *)Value;

    QcDcmRespond(From, Transaction, QC_DCM_GET_IP_ADDRESS, QMI_RESULT_SUCCESS, 0);

    EnterCriticalSection(&QcDcm.Lock);
    Up = QcDcm.Up;
    Family = QcDcm.Family;
    memcpy(Address, QcDcm.Address, sizeof(Address));
    LeaveCriticalSection(&QcDcm.Lock);

    QcTrace("dcm: pdp %u address asked, '%s'\n", Pdp, Up ? Address : "");

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_INDICATION, 0, QC_DCM_GET_IP_ADDRESS);
    QmiEncodeTlv(&Encoder, QC_DCM_TLV_PDP_ID, &Pdp, sizeof(Pdp));
    if (Up && Address[0])
        QcDcmEncodeAddress(&Encoder, QC_DCM_TLV_ADDRESS, Family, Address);
    MessageLength = QmiEncodeEnd(&Encoder);
    QcDcmSend(From, Message, MessageLength);
}

/* The modem's own link address for the PDP, the host only acknowledges it */
static
VOID
QcDcmLinkAddr(
    _In_ const QC_ADDR *From,
    _In_ USHORT Transaction,
    _In_reads_bytes_(Length) const VOID *Request,
    _In_ ULONG Length)
{
    const UCHAR *Value;
    CHAR Address[QC_DCM_ADDRESS_MAX + 1];
    USHORT ValueLen, Port = 0;
    ULONG Family = 0, AddressLength = 0;

    if (QmiFindTlv(Request, Length, 0x01, (const VOID **)&Value, &ValueLen) && ValueLen >= 7)
    {
        memcpy(&Port, Value, sizeof(Port));
        memcpy(&Family, Value + 2, sizeof(Family));
        AddressLength = min(min(Value[6], (ULONG)ValueLen - 7), QC_DCM_ADDRESS_MAX);
        memcpy(Address, Value + 7, AddressLength);
        QcDcmTraceBytes("link address record", Value, ValueLen);
    }
    Address[AddressLength] = '\0';

    QcTrace("dcm: modem link address '%s' family %lu port %u\n", Address, Family, Port);
    QcDcmRespond(From, Transaction, QC_DCM_LINK_ADDR, QMI_RESULT_SUCCESS, 0);
}

static
DWORD
WINAPI
QcDcmReader(
    _In_ LPVOID Parameter)
{
    OVERLAPPED Overlapped;
    const QMI_HEADER *Header;
    PUCHAR Buffer;
    HANDLE Event;
    QC_ADDR From;
    DWORD Read;
    ULONG Length;
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

        Ok = ReadFile(QcDcm.Port, Buffer, 8 + QC_MESSAGE_MAX, NULL, &Overlapped);
        if (!Ok && GetLastError() != ERROR_IO_PENDING)
            break;
        if (!GetOverlappedResult(QcDcm.Port, &Overlapped, &Read, TRUE))
            break;

        if (Read < 8 + sizeof(QMI_HEADER))
            continue;

        From.Node = ((PULONG)Buffer)[0];
        From.Port = ((PULONG)Buffer)[1];
        Length = Read - 8;
        if (!QmiMessageValid(Buffer + 8, Length))
            continue;

        Header = (const QMI_HEADER *)(Buffer + 8);
        if (Header->Type != QMI_TYPE_REQUEST)
            continue;

        QcTrace("dcm: request 0x%04X txn %u from %lu:%lu, %lu bytes\n",
                Header->MessageId, Header->TransactionId, From.Node, From.Port, Length);

        switch (Header->MessageId)
        {
            case QC_DCM_PDP_ACTIVATE:
                QcDcmActivate(&From, Header->TransactionId, Buffer + 8, Length);
                break;

            case QC_DCM_PDP_DEACTIVATE:
                QcDcmDeactivate(&From, Header->TransactionId, Buffer + 8, Length);
                break;

            case QC_DCM_GET_IP_ADDRESS:
                QcDcmGetIpAddress(&From, Header->TransactionId, Buffer + 8, Length);
                break;

            case QC_DCM_LINK_ADDR:
                QcDcmLinkAddr(&From, Header->TransactionId, Buffer + 8, Length);
                break;

            /* Wi-Fi quality and handover measurement, for IMS over Wi-Fi */
            default:
                QcDcmTraceBytes("unhandled request", Buffer + 8, Length);
                QcDcmRespond(&From, Header->TransactionId, Header->MessageId,
                             QMI_RESULT_FAILURE, QC_QMI_ERR_NOT_SUPPORTED);
                break;
        }
    }

Exit:
    QcTrace("dcm: reader exits (%lu)\n", GetLastError());
    if (Event)
        CloseHandle(Event);
    if (Buffer)
        HeapFree(GetProcessHeap(), 0, Buffer);
    return 0;
}

/* Brings the PDP up or down to what the modem last asked for */
static
DWORD
WINAPI
QcDcmWorker(
    _In_ LPVOID Parameter)
{
    CHAR Address[QC_DCM_ADDRESS_MAX + 1];
    BOOL Want, Up;
    UCHAR Profile, Family;
    HRESULT hr;

    UNREFERENCED_PARAMETER(Parameter);

    for (;;)
    {
        WaitForSingleObject(QcDcm.WorkEvent, INFINITE);

        EnterCriticalSection(&QcDcm.Lock);
        Want = QcDcm.Want;
        Up = QcDcm.Up;
        Profile = QcDcm.Profile;
        Family = QcDcm.Family;
        LeaveCriticalSection(&QcDcm.Lock);

        if (Want && !Up)
        {
            if (!Profile)
            {
                QcTrace("dcm: no profile for the IMS APN\n");
                hr = E_FAIL;
            }
            else
            {
                hr = QcImsDataConnect(QcDcm.Modem,
                                      Profile,
                                      Family == QC_DCM_FAMILY_V6 ? 6 : 4,
                                      Address,
                                      sizeof(Address));
            }

            EnterCriticalSection(&QcDcm.Lock);
            QcDcm.Up = SUCCEEDED(hr);
            if (QcDcm.Up)
                memcpy(QcDcm.Address, Address, sizeof(QcDcm.Address));
            else
                QcDcm.Want = FALSE;
            LeaveCriticalSection(&QcDcm.Lock);

            QcTrace("dcm: pdp bring-up 0x%08lx\n", hr);
            QcDcmSendActivateInd(SUCCEEDED(hr));
        }
        else if (!Want && Up)
        {
            QcImsDataDisconnect();

            EnterCriticalSection(&QcDcm.Lock);
            QcDcm.Up = FALSE;
            QcDcm.Address[0] = '\0';
            LeaveCriticalSection(&QcDcm.Lock);
        }
    }
}

VOID
QcDcmInitialize(VOID)
{
    InitializeCriticalSection(&QcDcm.Lock);
}

/**
 * @brief Serves DCM on the IPC router, once per process.
 *
 * @param[in] FallbackProfile
 * The IMS profile to use when the modem names none.
 */
VOID
QcDcmStart(
    _In_ PQC_MODEM Modem,
    _In_ UCHAR FallbackProfile)
{
    EnterCriticalSection(&QcDcm.Lock);
    if (FallbackProfile)
        QcDcm.FallbackProfile = FallbackProfile;
    LeaveCriticalSection(&QcDcm.Lock);

    if (InterlockedExchange(&QcDcm.Started, 1))
        return;

    QcDcm.Modem = Modem;
    QcDcm.WorkEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!QcDcm.WorkEvent || !QcIpcOpenPort(&QcDcm.Port))
    {
        QcTrace("dcm: no router port (%lu)\n", GetLastError());
        InterlockedExchange(&QcDcm.Started, 0);
        return;
    }

    if (!QcIpcRegisterServer(QcDcm.Port, QC_DCM_SERVICE, QC_DCM_INSTANCE))
    {
        QcTrace("dcm: the router refused the service (%lu)\n", GetLastError());
        QcIpcClosePort(QcDcm.Port);
        QcDcm.Port = INVALID_HANDLE_VALUE;
        InterlockedExchange(&QcDcm.Started, 0);
        return;
    }

    QcDcm.Reader = CreateThread(NULL, 0, QcDcmReader, NULL, 0, NULL);
    QcDcm.Worker = CreateThread(NULL, 0, QcDcmWorker, NULL, 0, NULL);
    QcTrace("dcm: serving service 0x%x version %u, fallback profile %u\n",
            QC_DCM_SERVICE, QC_DCM_INSTANCE, FallbackProfile);
}

/* The network ended the PDP, the modem hears it as a PDP that has no network */
VOID
QcDcmPdnLost(VOID)
{
    BOOL Up;

    EnterCriticalSection(&QcDcm.Lock);
    Up = QcDcm.Up;
    QcDcm.Up = FALSE;
    QcDcm.Want = FALSE;
    QcDcm.Address[0] = '\0';
    LeaveCriticalSection(&QcDcm.Lock);

    if (Up)
        QcDcmSendActivateInd(FALSE);
}

/* The modem restarted, its IMS stack asks again once it is back */
VOID
QcDcmModemLost(VOID)
{
    EnterCriticalSection(&QcDcm.Lock);
    QcDcm.HaveClient = FALSE;
    QcDcm.Up = FALSE;
    QcDcm.Want = FALSE;
    QcDcm.Address[0] = '\0';
    LeaveCriticalSection(&QcDcm.Lock);
}
