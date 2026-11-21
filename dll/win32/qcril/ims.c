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
#include <strsafe.h>

/* IMS settings */
#define QCRIL_IMSS_SET_SERVICES         0x008F
#define QCRIL_IMSS_GET_SERVICES         0x0090
#define QCRIL_IMSS_BIND                 0x0098
#define QCRIL_IMSS_TLV_BINDING          0x01
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
#define QCRIL_IMSA_BIND                 0x0033
#define QCRIL_IMSA_TLV_BINDING          0x10

/* A multi SIM modem answers IMS requests only once the client is bound to a subscription */
#define QCRIL_IMS_PRIMARY_SUBSCRIPTION  0

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

static
HRESULT
QcImsBind(
    _In_ PQC_MODEM Modem,
    _In_ QC_SVC Service,
    _In_ USHORT MessageId,
    _In_ UCHAR Tlv)
{
    QMI_ENCODER Encoder;
    BYTE Message[32];
    BYTE Response[64];
    ULONG Length, ResponseLen;
    ULONG Binding = QCRIL_IMS_PRIMARY_SUBSCRIPTION;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, MessageId);
    if (!QmiEncodeTlv(&Encoder, Tlv, &Binding, sizeof(Binding)))
        return E_FAIL;
    Length = QmiEncodeEnd(&Encoder);

    return QcQmiRequest(Modem, Service, Message, Length, Response, sizeof(Response), &ResponseLen);
}

/* Data profiles, for finding the one IMS registers over */
#define QCRIL_WDS_GET_PROFILE_LIST      0x002A
#define QCRIL_WDS_GET_PROFILE_SETTINGS  0x002B
#define QCRIL_WDS_PROFILE_3GPP          0

static
PCSTR
QcImsPdpName(
    _In_ UCHAR Type)
{
    switch (Type)
    {
        case 0: return "ipv4";
        case 1: return "ppp";
        case 2: return "ipv6";
        case 3: return "ipv4v6";
        default: return "?";
    }
}

/* Logs every 3GPP data profile the carrier configuration carries, returns the IMS one or zero */
static
UCHAR
QcImsListProfiles(
    _In_ PQC_MODEM Modem)
{
    QMI_ENCODER Encoder;
    BYTE Message[32];
    BYTE List[512], Settings[512];
    const VOID *Value;
    const UCHAR *Entry, *End;
    CHAR Name[64], Apn[104];
    ULONG Length, ListLen, SettingsLen;
    USHORT ValueLen;
    UCHAR Type = QCRIL_WDS_PROFILE_3GPP, Key[2], Count, NameLen, Pdp, i, Ims = 0;
    HRESULT hr;

    if (!QcServiceEnsure(Modem, QcSvcWds))
        return 0;

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WDS_GET_PROFILE_LIST);
    QmiEncodeTlv(&Encoder, 0x10, &Type, sizeof(Type));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiRequest(Modem, QcSvcWds, Message, Length, List, sizeof(List), &ListLen);
    if (FAILED(hr) || !QmiFindTlv(List, ListLen, 0x01, &Value, &ValueLen) || ValueLen < 1)
    {
        QcTrace("ims: reading the data profiles failed 0x%08lx\n", hr);
        return 0;
    }

    Entry = Value;
    End = Entry + ValueLen;
    Count = *Entry++;
    QcTrace("ims: %u data profile(s)\n", Count);

    for (i = 0; i < Count && Entry + 3 <= End; i++)
    {
        Key[0] = Entry[0];
        Key[1] = Entry[1];
        NameLen = Entry[2];
        Entry += 3;
        if (Entry + NameLen > End)
            break;
        Entry += NameLen;

        QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, QCRIL_WDS_GET_PROFILE_SETTINGS);
        QmiEncodeTlv(&Encoder, 0x01, Key, sizeof(Key));
        Length = QmiEncodeEnd(&Encoder);
        hr = QcQmiRequest(Modem, QcSvcWds, Message, Length, Settings, sizeof(Settings), &SettingsLen);
        if (FAILED(hr))
        {
            QcTrace("ims: profile %u settings failed 0x%08lx\n", Key[1], hr);
            continue;
        }

        Name[0] = Apn[0] = '\0';
        Pdp = 0xFF;
        if (QmiFindTlv(Settings, SettingsLen, 0x10, &Value, &ValueLen))
            StringCchCopyNA(Name, sizeof(Name), Value, ValueLen);
        if (QmiFindTlv(Settings, SettingsLen, 0x14, &Value, &ValueLen))
            StringCchCopyNA(Apn, sizeof(Apn), Value, ValueLen);
        if (QmiFindTlv(Settings, SettingsLen, 0x11, &Value, &ValueLen) && ValueLen >= 1)
            Pdp = *(const UCHAR *)Value;

        QcTrace("ims: profile %u '%s' apn '%s' %s\n", Key[1], Name, Apn, QcImsPdpName(Pdp));
        if (!Ims && !_stricmp(Apn, "ims"))
            Ims = Key[1];
    }

    return Ims;
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
    ULONG Sms, Voice, VoiceTech, Other;
    UCHAR Tlv;

    QcImsReadDword(Message, Length, 0x10, &Sms);
    QcImsReadDword(Message, Length, 0x11, &Voice);
    QcImsReadDword(Message, Length, 0x14, &VoiceTech);

    EnterCriticalSection(&Modem->StateLock);
    Modem->Ims.Voice = Voice;
    LeaveCriticalSection(&Modem->StateLock);

    QcTrace("ims: %s: voice %s (technology %ld), sms %s\n", Origin,
            QcImsServiceName(Voice), (LONG)VoiceTech, QcImsServiceName(Sms));

    /* The rest are video, UT, video share and similar, named by TLV for the log */
    for (Tlv = 0x12; Tlv <= 0x1F; Tlv++)
    {
        if (Tlv != 0x14 && QcImsReadDword(Message, Length, Tlv, &Other))
            QcTrace("ims: %s: tlv 0x%02x value %lu\n", Origin, Tlv, Other);
    }
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

    hr = QcImsBind(Modem, QcSvcImss, QCRIL_IMSS_BIND, QCRIL_IMSS_TLV_BINDING);
    QcTrace("ims: settings bound to the primary subscription: 0x%08lx\n", hr);

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

    hr = QcImsBind(Modem, QcSvcImsa, QCRIL_IMSA_BIND, QCRIL_IMSA_TLV_BINDING);
    QcTrace("ims: application bound to the primary subscription: 0x%08lx\n", hr);

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

    /* After a modem restart the data service may trail the IMS ones */
    if (!QcServiceEnsure(Modem, QcSvcWds))
    {
        QcTrace("ims: the ims pdn waits for the data service\n");
        Modem->Ims.PdnPending = TRUE;
        return;
    }

    QcImsStartPdn(Modem);
}

/* Registration runs over the IMS PDN, which the modem asks the host for through DCM */
VOID
QcImsStartPdn(
    _In_ PQC_MODEM Modem)
{
    UCHAR Profile;

    Modem->Ims.PdnPending = FALSE;

    Profile = QcImsListProfiles(Modem);
    if (!Profile)
        QcTrace("ims: no data profile with the ims apn\n");

    QcDcmStart(Modem, Profile);
}

/* The registration went with the modem, it is set up again once IMS returns */
VOID
QcImsModemLost(
    _In_ PQC_MODEM Modem)
{
    EnterCriticalSection(&Modem->StateLock);
    Modem->Ims.Registration = 0;
    Modem->Ims.Voice = 0;
    Modem->Ims.PdnPending = FALSE;
    LeaveCriticalSection(&Modem->StateLock);
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
