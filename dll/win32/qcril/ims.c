/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     IMS settings and IMS application services, the modem's VoLTE stack
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The modem only runs these services when its carrier configuration enables IMS.
 * The settings service turns VoLTE on, the application service reports whether
 * the modem registered with the carrier's IMS core, which voice calls need on
 * networks without a circuit switched fallback.
 */

#include "qcril.h"
#include <string.h>

/* IMS settings */
#define QCRIL_IMSS_SET_SERVICES         0x008F
#define QCRIL_IMSS_GET_SERVICES         0x0090
#define QCRIL_IMSS_TLV_SET_VOLTE        0x10
#define QCRIL_IMSS_TLV_SET_IMS          0x18
#define QCRIL_IMSS_TLV_GET_VOICE        0x11
#define QCRIL_IMSS_TLV_GET_REGISTRATION 0x19

/* IMS application */
#define QCRIL_IMSA_GET_REGISTRATION     0x0020
#define QCRIL_IMSA_GET_SERVICES         0x0021
#define QCRIL_IMSA_REGISTER_IND         0x0022
#define QCRIL_IMSA_REGISTRATION_IND     0x0023
#define QCRIL_IMSA_SERVICES_IND         0x0024

static
PCSTR
QcImsRegistrationName(
    _In_ ULONG Status)
{
    switch (Status)
    {
        case 0: return "not registered";
        case 1: return "registering";
        case 2: return "registered";
        case 3: return "limited";
        default: return "unknown";
    }
}

static
PCSTR
QcImsServiceName(
    _In_ ULONG Status)
{
    switch (Status)
    {
        case 0: return "unavailable";
        case 1: return "limited";
        case 2: return "available";
        default: return "unknown";
    }
}

static
BOOL
QcImsReadDword(
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length,
    _In_ UCHAR Tlv,
    _Out_ PULONG Value)
{
    const VOID *Data;
    USHORT DataLen;

    *Value = MAXULONG;
    if (!QmiFindTlv(Message, Length, Tlv, &Data, &DataLen) || DataLen < sizeof(*Value))
        return FALSE;
    memcpy(Value, Data, sizeof(*Value));
    return TRUE;
}

static
BOOL
QcImsReadByte(
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length,
    _In_ UCHAR Tlv,
    _Out_ PUCHAR Value)
{
    const VOID *Data;
    USHORT DataLen;

    *Value = 0xFF;
    if (!QmiFindTlv(Message, Length, Tlv, &Data, &DataLen) || DataLen < 1)
        return FALSE;
    *Value = *(const UCHAR *)Data;
    return TRUE;
}

/* Registration status, technology and error, from a response or an indication */
static
VOID
QcImsLogRegistration(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length,
    _In_ UCHAR StatusTlv,
    _In_ UCHAR ErrorTlv,
    _In_ UCHAR TechTlv,
    _In_ PCSTR Origin)
{
    ULONG Status, Tech;
    const VOID *Data;
    USHORT DataLen, Error = 0;

    QcImsReadDword(Message, Length, StatusTlv, &Status);
    QcImsReadDword(Message, Length, TechTlv, &Tech);
    if (QmiFindTlv(Message, Length, ErrorTlv, &Data, &DataLen) && DataLen >= sizeof(Error))
        memcpy(&Error, Data, sizeof(Error));

    EnterCriticalSection(&Modem->StateLock);
    Modem->Ims.Registration = Status;
    LeaveCriticalSection(&Modem->StateLock);

    QcTrace("ims: %s: %s, technology %ld, error %u\n", Origin, QcImsRegistrationName(Status), (LONG)Tech, Error);
}

static
VOID
QcImsLogServices(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length,
    _In_ PCSTR Origin)
{
    ULONG Sms, Voice, VoiceTech;

    QcImsReadDword(Message, Length, 0x10, &Sms);
    QcImsReadDword(Message, Length, 0x11, &Voice);
    QcImsReadDword(Message, Length, 0x14, &VoiceTech);

    EnterCriticalSection(&Modem->StateLock);
    Modem->Ims.Voice = Voice;
    LeaveCriticalSection(&Modem->StateLock);

    QcTrace("ims: %s: voice %s (technology %ld), sms %s\n", Origin,
            QcImsServiceName(Voice), (LONG)VoiceTech, QcImsServiceName(Sms));
}

VOID
QcImssSetup(
    _In_ PQC_MODEM Modem)
{
    QMI_ENCODER Encoder;
    BYTE Message[64];
    BYTE Response[256];
    ULONG Length, ResponseLen;
    UCHAR Voice, Registration, On = 1;
    HRESULT hr;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_IMSS_GET_SERVICES);
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiRequest(Modem, QcSvcImss, Message, Length, Response, sizeof(Response), &ResponseLen);
    if (FAILED(hr))
    {
        QcTrace("ims: settings query failed 0x%08lx\n", hr);
        return;
    }

    QcImsReadByte(Response, ResponseLen, QCRIL_IMSS_TLV_GET_VOICE, &Voice);
    QcImsReadByte(Response, ResponseLen, QCRIL_IMSS_TLV_GET_REGISTRATION, &Registration);
    QcTrace("ims: settings say voice over lte %u, ims registration %u\n", Voice, Registration);

    /* The carrier configuration may leave either off; voice calls need both */
    if (Voice != 1 || Registration != 1)
    {
        QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_IMSS_SET_SERVICES);
        if (!QmiEncodeTlv(&Encoder, QCRIL_IMSS_TLV_SET_VOLTE, &On, sizeof(On)) ||
            !QmiEncodeTlv(&Encoder, QCRIL_IMSS_TLV_SET_IMS, &On, sizeof(On)))
        {
            return;
        }
        Length = QmiEncodeEnd(&Encoder);
        hr = QcQmiRequest(Modem, QcSvcImss, Message, Length, Response, sizeof(Response), &ResponseLen);
        QcTrace("ims: turning voice over lte and ims on: 0x%08lx\n", hr);
    }
}

VOID
QcImsaSetup(
    _In_ PQC_MODEM Modem)
{
    QMI_ENCODER Encoder;
    BYTE Message[64];
    BYTE Response[512];
    ULONG Length, ResponseLen;
    UCHAR On = 1;
    HRESULT hr;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_IMSA_REGISTER_IND);
    if (!QmiEncodeTlv(&Encoder, 0x10, &On, sizeof(On)) ||
        !QmiEncodeTlv(&Encoder, 0x11, &On, sizeof(On)))
    {
        return;
    }
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiRequest(Modem, QcSvcImsa, Message, Length, Response, sizeof(Response), &ResponseLen);
    QcTrace("ims: indication registration 0x%08lx\n", hr);

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_IMSA_GET_REGISTRATION);
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiRequest(Modem, QcSvcImsa, Message, Length, Response, sizeof(Response), &ResponseLen);
    if (SUCCEEDED(hr))
        QcImsLogRegistration(Modem, Response, ResponseLen, 0x12, 0x11, 0x14, "registration");

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_IMSA_GET_SERVICES);
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiRequest(Modem, QcSvcImsa, Message, Length, Response, sizeof(Response), &ResponseLen);
    if (SUCCEEDED(hr))
        QcImsLogServices(Modem, Response, ResponseLen, "services");
}

VOID
QcImsaIndication(
    _In_ PQC_MODEM Modem,
    _In_ USHORT MessageId,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    if (MessageId == QCRIL_IMSA_REGISTRATION_IND)
        QcImsLogRegistration(Modem, Message, Length, 0x11, 0x10, 0x13, "registration changed");
    else if (MessageId == QCRIL_IMSA_SERVICES_IND)
        QcImsLogServices(Modem, Message, Length, "services changed");
    else
        QcTrace("ind IMSA 0x%04X not handled\n", MessageId);
}
