/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Device management service, equipment state and identity
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcril.h"
#include <stdio.h>
#include <string.h>

#define QCRIL_DMS_SET_EVENT_REPORT      0x0001
#define QCRIL_DMS_EVENT_REPORT_IND      0x0001
#define QCRIL_DMS_GET_DEVICE_MFR        0x0021
#define QCRIL_DMS_GET_DEVICE_MODEL      0x0022
#define QCRIL_DMS_GET_DEVICE_REV        0x0023
#define QCRIL_DMS_GET_DEVICE_SERIAL     0x0025
#define QCRIL_DMS_GET_OPERATING_MODE    0x002D
#define QCRIL_DMS_SET_OPERATING_MODE    0x002E
#define QCRIL_DMS_SET_AP_SW_VERSION     0x0056

/* Event report request records */
#define QCRIL_DMS_TLV_REPORT_OPRT_MODE  0x14
#define QCRIL_DMS_TLV_REPORT_PRL_INIT   0x17
#define QCRIL_DMS_TLV_REPORT_MULTISIM   0x19

/* Event report indication record carrying the operating mode */
#define QCRIL_DMS_TLV_IND_OPRT_MODE     0x14

/* Serial numbers response records, simple getters use 0x01 */
#define QCRIL_DMS_TLV_STRING            0x01
#define QCRIL_DMS_TLV_ESN               0x10
#define QCRIL_DMS_TLV_IMEI              0x11
#define QCRIL_DMS_TLV_MEID              0x12
#define QCRIL_DMS_TLV_OPERATING_MODE    0x01

/* dms_operating_mode_enum_v01 values on the wire */
#define QCRIL_DMS_MODE_ONLINE           0x00
#define QCRIL_DMS_MODE_LOW_POWER        0x01
#define QCRIL_DMS_MODE_OFFLINE          0x03
#define QCRIL_DMS_MODE_SHUTTING_DOWN    0x05
#define QCRIL_DMS_MODE_NET_TEST_GW      0x08

/* The IMEI digits the check digit is computed over */
#define QCRIL_IMEI_BODY                 14

static
PCSTR
QcEquipmentName(
    _In_ DWORD State)
{
    switch (State)
    {
        case RIL_EQSTATE_MINIMUM: return "minimum";
        case RIL_EQSTATE_FULL: return "full";
        case RIL_EQSTATE_SHUTDOWN: return "shutdown";
        default: return "unknown";
    }
}

/* Raises the equipment state when it changed, or always when Force is set */
static
VOID
QcDmsPublish(
    _In_ PQC_MODEM Modem,
    _In_ DWORD State,
    _In_ BOOL Force)
{
    BOOL Changed;

    EnterCriticalSection(&Modem->StateLock);
    Changed = Force || Modem->Dms.Equipment != State;
    Modem->Dms.Equipment = State;
    LeaveCriticalSection(&Modem->StateLock);

    if (!Changed)
        return;

    QcTrace("equipment state now %lu (%s)\n", State, QcEquipmentName(State));
    QcNotifyAll(Modem, RIL_NOTIFY_EQUIPMENTSTATE, &State, sizeof(State));
}

/* Reads the current operating mode, returning the raw QMI mode */
static
HRESULT
QcDmsReadMode(
    _In_ PQC_MODEM Modem,
    _Out_ PUCHAR Mode)
{
    QMI_ENCODER Encoder;
    BYTE Request[16];
    BYTE Response[128];
    const VOID *Value;
    USHORT ValueLen;
    ULONG Length, ResponseLen;
    HRESULT hr;

    *Mode = 0xFF;

    QmiEncodeBegin(&Encoder, Request, sizeof(Request), QMI_TYPE_REQUEST, 0, QCRIL_DMS_GET_OPERATING_MODE);
    Length = QmiEncodeEnd(&Encoder);

    hr = QcQmiRequest(Modem, QcSvcDms, Request, Length, Response, sizeof(Response), &ResponseLen);
    if (FAILED(hr))
        return hr;

    if (!QmiFindTlv(Response, ResponseLen, QCRIL_DMS_TLV_OPERATING_MODE, &Value, &ValueLen) || ValueLen < 1)
        return E_FAIL;

    *Mode = *(const UCHAR *)Value;

    if (QmiFindTlv(Response, ResponseLen, 0x10, &Value, &ValueLen) && ValueLen >= 2)
        QcTrace("operating mode %u, offline reason 0x%04x\n", *Mode, *(const USHORT UNALIGNED *)Value);
    else
        QcTrace("operating mode %u\n", *Mode);

    return S_OK;
}

static
HRESULT
QcDmsWriteMode(
    _In_ PQC_MODEM Modem,
    _In_ UCHAR Mode)
{
    QMI_ENCODER Encoder;
    BYTE Request[32];
    BYTE Response[128];
    ULONG Length, ResponseLen;
    HRESULT hr;

    QmiEncodeBegin(&Encoder, Request, sizeof(Request), QMI_TYPE_REQUEST, 0, QCRIL_DMS_SET_OPERATING_MODE);
    if (!QmiEncodeTlv(&Encoder, QCRIL_DMS_TLV_OPERATING_MODE, &Mode, sizeof(Mode)))
        return E_FAIL;
    Length = QmiEncodeEnd(&Encoder);

    hr = QcQmiRequest(Modem, QcSvcDms, Request, Length, Response, sizeof(Response), &ResponseLen);
    QcTrace("set operating mode %u: 0x%08lx, QMI error 0x%x\n", Mode, hr, QcQmiError(Response, ResponseLen));

    /* WMRil reports any refusal of the mode change as a generic failure */
    if (FAILED(hr) && ResponseLen != 0)
        return E_FAIL;

    return hr;
}

VOID
QcDmsSetup(
    _In_ PQC_MODEM Modem)
{
    RTL_OSVERSIONINFOW Os;
    QMI_ENCODER Encoder;
    BYTE Request[96];
    CHAR Version[33];
    UCHAR One = 1;
    ULONG Length;
    HRESULT hr;

    /* Operating mode, PRL init and multisim changes come back as event reports */
    QmiEncodeBegin(&Encoder, Request, sizeof(Request), QMI_TYPE_REQUEST, 0, QCRIL_DMS_SET_EVENT_REPORT);
    QmiEncodeTlv(&Encoder, QCRIL_DMS_TLV_REPORT_OPRT_MODE, &One, sizeof(One));
    QmiEncodeTlv(&Encoder, QCRIL_DMS_TLV_REPORT_PRL_INIT, &One, sizeof(One));
    QmiEncodeTlv(&Encoder, QCRIL_DMS_TLV_REPORT_MULTISIM, &One, sizeof(One));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiCommand(Modem, QcSvcDms, Request, Length);
    QcTrace("dms: event report registration 0x%08lx\n", hr);

    /* Tell the modem which application processor build drives it */
    ZeroMemory(&Os, sizeof(Os));
    Os.dwOSVersionInfoSize = sizeof(Os);
    if (!NT_SUCCESS(RtlGetVersion(&Os)))
        return;

    if (_snprintf(Version, sizeof(Version), "%lu.%lu:%lu", Os.dwMajorVersion, Os.dwMinorVersion, Os.dwBuildNumber) < 0)
        return;
    Version[sizeof(Version) - 1] = '\0';

    QmiEncodeBegin(&Encoder, Request, sizeof(Request), QMI_TYPE_REQUEST, 0, QCRIL_DMS_SET_AP_SW_VERSION);
    if (!QmiEncodeTlv(&Encoder, QCRIL_DMS_TLV_STRING, Version, (USHORT)strlen(Version)))
        return;
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiCommand(Modem, QcSvcDms, Request, Length);
    QcTrace("dms: AP software version '%s' 0x%08lx\n", Version, hr);
}

/* The modem restarted, the radio is down until it is set up again */
VOID
QcDmsModemLost(
    _In_ PQC_MODEM Modem)
{
    EnterCriticalSection(&Modem->StateLock);
    if (!Modem->Dms.Shutdown)
        Modem->Dms.Equipment = RIL_EQSTATE_SHUTDOWN;
    if (Modem->Dms.Awaiting)
        SetEvent(Modem->Dms.ChangeEvent);
    LeaveCriticalSection(&Modem->StateLock);

    QcTrace("ssr: equipment state cached as shutdown\n");
}

/**
 * @brief
 * Puts the modem back in the state a client last asked for, after a restart.
 *
 * @return
 * FALSE when the modem was not ready for it and it should be tried again.
 */
BOOL
QcDmsRestore(
    _In_ PQC_MODEM Modem)
{
    DWORD State;
    BOOL Shutdown;
    UCHAR Mode, Wanted;

    EnterCriticalSection(&Modem->StateLock);
    State = Modem->Dms.Requested;
    Shutdown = Modem->Dms.Shutdown;
    LeaveCriticalSection(&Modem->StateLock);

    if (Shutdown || State == RIL_EQSTATE_SHUTDOWN)
        return TRUE;

    Wanted = (State == RIL_EQSTATE_FULL) ? QCRIL_DMS_MODE_ONLINE : QCRIL_DMS_MODE_LOW_POWER;
    if (SUCCEEDED(QcDmsReadMode(Modem, &Mode)) && Mode == Wanted)
    {
        QcTrace("ssr: equipment state %lu (%s) already in place\n", State, QcEquipmentName(State));
        return TRUE;
    }

    QcTrace("ssr: restoring equipment state %lu (%s)\n", State, QcEquipmentName(State));
    return SUCCEEDED(QcDmsWriteMode(Modem, Wanted));
}

HRESULT
QcDmsSetEquipmentState(
    _In_ PQC_MODEM Modem,
    _In_ DWORD State)
{
    DWORD Current, Reported;
    UCHAR Mode;
    HRESULT hr;

    QcTrace("set equipment state %lu (%s)\n", State, QcEquipmentName(State));

    /* Shutdown is local, the modem is left alone from here on */
    if (State == RIL_EQSTATE_SHUTDOWN)
    {
        EnterCriticalSection(&Modem->StateLock);
        Modem->Dms.Shutdown = TRUE;
        Modem->Dms.Requested = State;
        LeaveCriticalSection(&Modem->StateLock);

        QcDmsPublish(Modem, State, TRUE);
        return S_OK;
    }

    if (State != RIL_EQSTATE_MINIMUM && State != RIL_EQSTATE_FULL)
        return E_INVALIDARG;

    hr = QcDmsReadMode(Modem, &Mode);
    if (FAILED(hr))
        return hr;

    switch (Mode)
    {
        case QCRIL_DMS_MODE_ONLINE: Current = RIL_EQSTATE_FULL; break;
        case QCRIL_DMS_MODE_LOW_POWER: Current = RIL_EQSTATE_MINIMUM; break;
        case QCRIL_DMS_MODE_OFFLINE:
        case QCRIL_DMS_MODE_SHUTTING_DOWN: Current = RIL_EQSTATE_SHUTDOWN; break;
        default:
            QcTrace("operating mode %u has no equipment state\n", Mode);
            return E_FAIL;
    }

    if (Current == State)
    {
        EnterCriticalSection(&Modem->StateLock);
        Modem->Dms.Requested = State;
        LeaveCriticalSection(&Modem->StateLock);

        QcTrace("equipment state already %lu\n", State);
        QcDmsPublish(Modem, State, TRUE);
        return S_OK;
    }

    /* Arm the waiter before asking, the report may beat the response */
    EnterCriticalSection(&Modem->StateLock);
    ResetEvent(Modem->Dms.ChangeEvent);
    Modem->Dms.Awaiting = State;
    Modem->Dms.Reported = 0;
    LeaveCriticalSection(&Modem->StateLock);

    hr = QcDmsWriteMode(Modem, (State == RIL_EQSTATE_FULL) ? QCRIL_DMS_MODE_ONLINE : QCRIL_DMS_MODE_LOW_POWER);
    if (SUCCEEDED(hr) && WaitForSingleObject(Modem->Dms.ChangeEvent, QC_EQSTATE_TIMEOUT_MS) != WAIT_OBJECT_0)
    {
        QcTrace("equipment state %lu not confirmed in %u ms\n", State, QC_EQSTATE_TIMEOUT_MS);
        hr = RIL_ERROR(0x48);
    }

    EnterCriticalSection(&Modem->StateLock);
    Reported = Modem->Dms.Reported;
    Modem->Dms.Awaiting = 0;
    if (SUCCEEDED(hr) && Reported == State)
        Modem->Dms.Requested = State;
    LeaveCriticalSection(&Modem->StateLock);

    if (FAILED(hr))
        return hr;

    if (Reported != State)
    {
        QcTrace("modem reported equipment state %lu instead of %lu\n", Reported, State);
        return RIL_ERROR(0x01);
    }

    return S_OK;
}

HRESULT
QcDmsGetEquipmentState(
    _In_ PQC_MODEM Modem,
    _Inout_ PQC_RESP Resp)
{
    UCHAR Mode;
    DWORD State;
    HRESULT hr;

    hr = QcDmsReadMode(Modem, &Mode);
    if (FAILED(hr))
        return hr;

    switch (Mode)
    {
        case QCRIL_DMS_MODE_ONLINE:
        case QCRIL_DMS_MODE_NET_TEST_GW:
            State = RIL_EQSTATE_FULL;
            break;

        case QCRIL_DMS_MODE_LOW_POWER:
        case QCRIL_DMS_MODE_SHUTTING_DOWN:
            State = RIL_EQSTATE_MINIMUM;
            break;

        default:
            QcTrace("operating mode %u has no equipment state\n", Mode);
            return E_FAIL;
    }

    QcRespDword(Resp, State);
    return S_OK;
}

static
VOID
QcDmsEventReport(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    const VOID *Value;
    USHORT ValueLen;
    DWORD State;
    UCHAR Mode;
    BOOL Shutdown;

    EnterCriticalSection(&Modem->StateLock);
    Shutdown = Modem->Dms.Shutdown;
    LeaveCriticalSection(&Modem->StateLock);

    if (Shutdown)
    {
        QcTrace("dms event report ignored, equipment is shut down\n");
        return;
    }

    if (!QmiFindTlv(Message, Length, QCRIL_DMS_TLV_IND_OPRT_MODE, &Value, &ValueLen) || ValueLen < 1)
    {
        QcTrace("dms event report without an operating mode\n");
        return;
    }

    Mode = *(const UCHAR *)Value;
    switch (Mode)
    {
        case QCRIL_DMS_MODE_ONLINE:
        case QCRIL_DMS_MODE_NET_TEST_GW:
            State = RIL_EQSTATE_FULL;
            break;

        case QCRIL_DMS_MODE_LOW_POWER:
            State = RIL_EQSTATE_MINIMUM;
            break;

        case QCRIL_DMS_MODE_OFFLINE:
        case QCRIL_DMS_MODE_SHUTTING_DOWN:
            State = RIL_EQSTATE_SHUTDOWN;
            break;

        default:
            QcTrace("dms event report: operating mode %u ignored\n", Mode);
            return;
    }

    QcTrace("dms event report: operating mode %u -> equipment state %lu\n", Mode, State);

    EnterCriticalSection(&Modem->StateLock);
    if (Modem->Dms.Awaiting)
    {
        Modem->Dms.Reported = State;
        SetEvent(Modem->Dms.ChangeEvent);
    }
    LeaveCriticalSection(&Modem->StateLock);

    QcDmsPublish(Modem, State, FALSE);
}

VOID
QcDmsIndication(
    _In_ PQC_MODEM Modem,
    _In_ USHORT MessageId,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    if (MessageId == QCRIL_DMS_EVENT_REPORT_IND)
        QcDmsEventReport(Modem, Message, Length);
    else
        QcTrace("ind DMS 0x%04X not handled\n", MessageId);
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

    if (!QmiFindTlv(Response, ResponseLen, Tlv, &Value, &ValueLen) || ValueLen == 0)
        return RIL_E_NOTFOUND;

    Copy = min(ValueLen, OutSize - 1);
    memcpy(Out, Value, Copy);
    Out[Copy] = '\0';
    return S_OK;
}

/* The Luhn check digit over the first 14 IMEI digits, FALSE when they are not all digits */
static
BOOL
QcImeiCheckDigit(
    _In_reads_(QCRIL_IMEI_BODY) PCSTR Digits,
    _Out_ PULONG Check)
{
    ULONG i, Sum = 0, Value;

    for (i = 0; i < QCRIL_IMEI_BODY; i++)
    {
        if (Digits[i] < '0' || Digits[i] > '9')
            return FALSE;

        Value = Digits[i] - '0';
        if (i & 1)
        {
            Value *= 2;
            Value = Value / 10 + Value % 10;
        }
        Sum += Value;
    }

    *Check = (10 - Sum % 10) % 10;
    return TRUE;
}

static
HRESULT
QcDmsGetImei(
    _In_ PQC_MODEM Modem,
    _Out_writes_z_(OutSize) PSTR Out,
    _In_ ULONG OutSize)
{
    CHAR Imei[40];
    ULONG Check;
    HRESULT hr;

    hr = QcDmsReadString(Modem, QCRIL_DMS_GET_DEVICE_SERIAL, QCRIL_DMS_TLV_IMEI, Imei, sizeof(Imei));
    if (FAILED(hr))
        return hr;

    /* The modem may report the check digit or not, rebuild it from the body */
    if (strlen(Imei) >= QCRIL_IMEI_BODY)
    {
        Imei[QCRIL_IMEI_BODY] = '\0';
        if (QcImeiCheckDigit(Imei, &Check))
        {
            Imei[QCRIL_IMEI_BODY] = (CHAR)('0' + Check);
            Imei[QCRIL_IMEI_BODY + 1] = '\0';
        }
    }

    strncpy(Out, Imei, OutSize - 1);
    Out[OutSize - 1] = '\0';
    return S_OK;
}

/* In: executor, device information class. Out: the value as a UTF-16 string. */
HRESULT
QcDmsGetDeviceInfo(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(InSize) const VOID *In,
    _In_ ULONG InSize,
    _Inout_ PQC_RESP Resp)
{
    const DWORD *Args = In;
    CHAR Text[RIL_INFO_REVISION];
    DWORD Class;
    HRESULT hr;

    if (InSize < 2 * sizeof(DWORD))
        return E_INVALIDARG;

    Class = Args[1];

    switch (Class)
    {
        case RIL_DEVICEINFO_MANUFACTURER:
            hr = QcDmsReadString(Modem, QCRIL_DMS_GET_DEVICE_MFR, QCRIL_DMS_TLV_STRING, Text, sizeof(Text));
            break;

        case RIL_DEVICEINFO_MODEL:
            hr = QcDmsReadString(Modem, QCRIL_DMS_GET_DEVICE_MODEL, QCRIL_DMS_TLV_STRING, Text, sizeof(Text));
            break;

        case RIL_DEVICEINFO_REVISION:
            hr = QcDmsReadString(Modem, QCRIL_DMS_GET_DEVICE_REV, QCRIL_DMS_TLV_STRING, Text, sizeof(Text));
            break;

        case RIL_DEVICEINFO_SERIALNUMBER:
            hr = QcDmsGetImei(Modem, Text, sizeof(Text));
            break;

        case RIL_DEVICEINFO_SERIALNUMBER_CDMA:
            hr = QcDmsReadString(Modem, QCRIL_DMS_GET_DEVICE_SERIAL, QCRIL_DMS_TLV_MEID, Text, sizeof(Text));
            if (FAILED(hr))
                hr = QcDmsReadString(Modem, QCRIL_DMS_GET_DEVICE_SERIAL, QCRIL_DMS_TLV_ESN, Text, sizeof(Text));
            if (hr == RIL_E_NOTFOUND)
                hr = RIL_ERROR(0x0D);
            break;

        default:
            return E_INVALIDARG;
    }

    if (FAILED(hr))
        return hr;

    QcTrace("device info class %lu = '%s'\n", Class, Text);
    QcRespWide(Resp, Text);
    return Resp->Failed ? E_FAIL : S_OK;
}
