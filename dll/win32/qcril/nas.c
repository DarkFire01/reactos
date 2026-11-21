/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Network access service, registration and signal quality
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcril.h"
#include <string.h>

#define QCRIL_NAS_INDICATION_REGISTER   0x0003
#define QCRIL_NAS_SERVING_SYSTEM_IND    0x0024
#define QCRIL_NAS_SYS_SEL_PREF          0x0034
#define QCRIL_NAS_OPERATOR_NAME_IND     0x003A
#define QCRIL_NAS_TLV_OPNAME_SPN        0x10
#define QCRIL_NAS_TLV_OPNAME_PLMN       0x13
#define QCRIL_NAS_BIND_SUBSCRIPTION     0x0045
#define QCRIL_NAS_DUAL_STANDBY_PREF_IND 0x0047
#define QCRIL_NAS_NETWORK_TIME_IND      0x004C
#define QCRIL_NAS_GET_SYS_INFO          0x004D
#define QCRIL_NAS_SYS_INFO_IND          0x004E
#define QCRIL_NAS_GET_SIG_INFO          0x004F
#define QCRIL_NAS_CONFIG_SIG_INFO       0x0050
#define QCRIL_NAS_SIG_INFO_IND          0x0051
#define QCRIL_NAS_CURRENT_PLMN_NAME_IND 0x0061
#define QCRIL_NAS_NETWORK_REJECT_IND    0x0068

/* Indication register records, each a one byte switch */
#define QCRIL_NAS_REG_SYS_SEL_PREF      0x10
#define QCRIL_NAS_REG_SERVING_SYSTEM    0x13
#define QCRIL_NAS_REG_DUAL_STANDBY      0x14
#define QCRIL_NAS_REG_NETWORK_TIME      0x17
#define QCRIL_NAS_REG_SYS_INFO          0x18
#define QCRIL_NAS_REG_SIG_INFO          0x19
#define QCRIL_NAS_REG_MANAGED_ROAMING   0x1D
#define QCRIL_NAS_REG_PLMN_NAME         0x1E
#define QCRIL_NAS_REG_NETWORK_REJECT    0x21
#define QCRIL_NAS_REG_SIB16_TIME        0x27

/* System information records, the same in the response and the indication */
#define QCRIL_NAS_TLV_GSM_SRV_STATUS    0x12
#define QCRIL_NAS_TLV_WCDMA_SRV_STATUS  0x13
#define QCRIL_NAS_TLV_LTE_SRV_STATUS    0x14
#define QCRIL_NAS_TLV_GSM_SYS_INFO      0x17
#define QCRIL_NAS_TLV_WCDMA_SYS_INFO    0x18
#define QCRIL_NAS_TLV_LTE_SYS_INFO      0x19
#define QCRIL_NAS_TLV_TDS_SRV_STATUS    0x24
#define QCRIL_NAS_TLV_TDS_SYS_INFO      0x25

/* Offsets inside a 3GPP system information record */
#define QCRIL_SYS_ROAM_VALID            4
#define QCRIL_SYS_ROAM                  5
#define QCRIL_SYS_LAC_VALID             8
#define QCRIL_SYS_LAC                   9
#define QCRIL_SYS_CELL_VALID            11
#define QCRIL_SYS_CELL                  12
#define QCRIL_SYS_REJECT_VALID          16
#define QCRIL_SYS_REJECT_CAUSE          18
#define QCRIL_SYS_NETWORK_VALID         19
#define QCRIL_SYS_MCC                   20
#define QCRIL_SYS_MNC                   23
#define QCRIL_SYS_3GPP_SIZE             26
#define QCRIL_SYS_LTE_TAC_VALID         26
#define QCRIL_SYS_LTE_TAC               27

/* Signal information records */
#define QCRIL_NAS_TLV_SIG_GSM           0x12
#define QCRIL_NAS_TLV_SIG_WCDMA         0x13
#define QCRIL_NAS_TLV_SIG_LTE           0x14

/* Signal report thresholds */
#define QCRIL_NAS_TLV_RSSI_THRESHOLDS   0x10
#define QCRIL_NAS_TLV_RSRP_THRESHOLDS   0x16

/* Current PLMN name records */
#define QCRIL_NAS_TLV_PLMN_ID           0x10
#define QCRIL_NAS_TLV_PLMN_SPN          0x11
#define QCRIL_NAS_TLV_PLMN_SHORT        0x12
#define QCRIL_NAS_TLV_PLMN_LONG         0x13

/* Network reject records */
#define QCRIL_NAS_TLV_REJECT_RADIO      0x01
#define QCRIL_NAS_TLV_REJECT_CAUSE      0x03

/* nas_service_status_enum_type_v01 */
#define QCRIL_SRV_NO_SERVICE            0x00
#define QCRIL_SRV_LIMITED               0x01
#define QCRIL_SRV_SERVICE               0x02
#define QCRIL_SRV_LIMITED_REGIONAL      0x03
#define QCRIL_SRV_POWER_SAVE            0x04

#define QCRIL_THRESHOLD_COUNT           5

/* One 3GPP radio the system information can describe, in preference order */
typedef struct _QC_NAS_RADIO
{
    UCHAR StatusTlv;
    UCHAR InfoTlv;
    DWORD Rat;
    PCSTR Name;
} QC_NAS_RADIO;

static const QC_NAS_RADIO QcNasRadios[] =
{
    { QCRIL_NAS_TLV_LTE_SRV_STATUS, QCRIL_NAS_TLV_LTE_SYS_INFO, RIL_RAT_LTE, "LTE" },
    { QCRIL_NAS_TLV_WCDMA_SRV_STATUS, QCRIL_NAS_TLV_WCDMA_SYS_INFO, RIL_RAT_UMTS, "UMTS" },
    { QCRIL_NAS_TLV_TDS_SRV_STATUS, QCRIL_NAS_TLV_TDS_SYS_INFO, RIL_RAT_TDSCDMA, "TDSCDMA" },
    { QCRIL_NAS_TLV_GSM_SRV_STATUS, QCRIL_NAS_TLV_GSM_SYS_INFO, RIL_RAT_GSM, "GSM" }
};

/* Bars thresholds in dBm, one bar for each threshold the measurement reaches */
static const SHORT QcGsmRssiBars[QCRIL_THRESHOLD_COUNT] = { -110, -104, -98, -92, -86 };
static const SHORT QcUmtsRssiBars[QCRIL_THRESHOLD_COUNT] = { -106, -100, -94, -88, -82 };
static const SHORT QcLteRsrpBars[QCRIL_THRESHOLD_COUNT] = { -128, -118, -108, -98, -88 };

static
DWORD
QcNasBars(
    _In_ LONG Value,
    _In_reads_(QCRIL_THRESHOLD_COUNT) const SHORT *Thresholds)
{
    DWORD Bars = 0;
    ULONG i;

    for (i = 0; i < QCRIL_THRESHOLD_COUNT; i++)
    {
        if (Value >= Thresholds[i])
            Bars++;
    }

    return Bars;
}

/* The WMRil registration state for a QMI service status */
static
DWORD
QcNasRegState(
    _In_ UCHAR SrvStatus,
    _In_ BOOL Roaming,
    _Out_ PBOOL Limited)
{
    *Limited = FALSE;

    switch (SrvStatus)
    {
        case QCRIL_SRV_LIMITED:
        case QCRIL_SRV_LIMITED_REGIONAL:
            *Limited = TRUE;
            return RIL_REG_UNREGISTERED;

        case QCRIL_SRV_SERVICE:
            return Roaming ? RIL_REG_ROAMING : RIL_REG_HOME;

        case QCRIL_SRV_POWER_SAVE:
            return RIL_REG_UNKNOWN;

        default:
            return RIL_REG_UNREGISTERED;
    }
}

/* Three ASCII digits, the third absent when the modem pads it */
static
DWORD
QcNasDigits(
    _In_reads_(3) const UCHAR *Digits)
{
    DWORD Value = 0;
    ULONG i;

    for (i = 0; i < 3; i++)
    {
        if (Digits[i] < '0' || Digits[i] > '9')
            break;
        Value = Value * 10 + (Digits[i] - '0');
    }

    return Value;
}

/* Raises the registration status when anything but the signal changed */
static
VOID
QcNasPublishReg(
    _In_ PQC_MODEM Modem,
    _In_ const RIL_NETWORK_INFO *Net,
    _In_ BOOL Force)
{
    RIL_NETWORK_INFO Copy;
    BOOL Changed;

    EnterCriticalSection(&Modem->StateLock);
    Changed = Force ||
              Modem->Nas.Net.RegState != Net->RegState ||
              Modem->Nas.Net.Rat != Net->Rat ||
              Modem->Nas.Net.Mcc != Net->Mcc ||
              Modem->Nas.Net.Mnc != Net->Mnc ||
              Modem->Nas.Net.Roaming != Net->Roaming ||
              Modem->Nas.Net.Limited != Net->Limited ||
              Modem->Nas.Net.RejectCause != Net->RejectCause ||
              Modem->Nas.Net.Lac != Net->Lac ||
              Modem->Nas.Net.CellId != Net->CellId;

    Copy = *Net;
    Copy.SignalDbm = Modem->Nas.Net.SignalDbm;
    Copy.SignalBars = Modem->Nas.Net.SignalBars;
    if (!Copy.Operator[0] && Copy.Mcc == Modem->Nas.Net.Mcc && Copy.Mnc == Modem->Nas.Net.Mnc)
        memcpy(Copy.Operator, Modem->Nas.Net.Operator, sizeof(Copy.Operator));
    Modem->Nas.Net = Copy;
    LeaveCriticalSection(&Modem->StateLock);

    if (!Changed)
        return;

    QcTrace("reg status %lu rat 0x%lx mcc %03lu mnc %02lu roaming %lu limited %lu lac 0x%lx cell 0x%lx reject %lu '%s'\n",
            Copy.RegState, Copy.Rat, Copy.Mcc, Copy.Mnc, Copy.Roaming, Copy.Limited,
            Copy.Lac, Copy.CellId, Copy.RejectCause, Copy.Operator);
    QcNotifyAll(Modem, RIL_NOTIFY_REGSTATUS, &Copy, sizeof(Copy));
}

/* Builds the registration state from a system information message */
static
VOID
QcNasSysInfo(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    RIL_NETWORK_INFO Net;
    const QC_NAS_RADIO *Best = NULL;
    const UCHAR *Status, *Info;
    const VOID *Value;
    USHORT ValueLen, InfoLen = 0;
    UCHAR BestStatus = QCRIL_SRV_NO_SERVICE;
    DWORD Types = 0;
    BOOL Limited, Roaming = FALSE;
    ULONG i;

    ZeroMemory(&Net, sizeof(Net));
    Net.Size = sizeof(Net);

    for (i = 0; i < RTL_NUMBER_OF(QcNasRadios); i++)
    {
        if (!QmiFindTlv(Message, Length, QcNasRadios[i].StatusTlv, &Value, &ValueLen) || ValueLen < 1)
            continue;

        Status = Value;
        QcTrace("sys info: %s srv status %u\n", QcNasRadios[i].Name, Status[0]);

        if (Status[0] == QCRIL_SRV_SERVICE || Status[0] == QCRIL_SRV_LIMITED ||
            Status[0] == QCRIL_SRV_LIMITED_REGIONAL)
        {
            Types |= QcNasRadios[i].Rat;
        }

        /* Full service wins over limited, the table order breaks ties */
        if (!Best ||
            (Status[0] == QCRIL_SRV_SERVICE && BestStatus != QCRIL_SRV_SERVICE) ||
            (BestStatus == QCRIL_SRV_NO_SERVICE && Status[0] != QCRIL_SRV_NO_SERVICE))
        {
            Best = &QcNasRadios[i];
            BestStatus = Status[0];
        }
    }

    if (Best && QmiFindTlv(Message, Length, Best->InfoTlv, &Value, &InfoLen) && InfoLen >= QCRIL_SYS_3GPP_SIZE)
    {
        Info = Value;

        Roaming = Info[QCRIL_SYS_ROAM_VALID] && Info[QCRIL_SYS_ROAM] != 0;
        if (Info[QCRIL_SYS_NETWORK_VALID])
        {
            Net.Mcc = QcNasDigits(&Info[QCRIL_SYS_MCC]);
            Net.Mnc = QcNasDigits(&Info[QCRIL_SYS_MNC]);
        }
        if (Info[QCRIL_SYS_CELL_VALID])
            memcpy(&Net.CellId, &Info[QCRIL_SYS_CELL], sizeof(DWORD));
        if (Info[QCRIL_SYS_REJECT_VALID])
            Net.RejectCause = Info[QCRIL_SYS_REJECT_CAUSE];

        if (Best->Rat == RIL_RAT_LTE && InfoLen >= QCRIL_SYS_LTE_TAC + 2 && Info[QCRIL_SYS_LTE_TAC_VALID])
            Net.Lac = Info[QCRIL_SYS_LTE_TAC] | (Info[QCRIL_SYS_LTE_TAC + 1] << 8);
        else if (Info[QCRIL_SYS_LAC_VALID])
            Net.Lac = Info[QCRIL_SYS_LAC] | (Info[QCRIL_SYS_LAC + 1] << 8);
    }

    Net.RegState = Best ? QcNasRegState(BestStatus, Roaming, &Limited) : RIL_REG_UNREGISTERED;
    if (!Best)
        Limited = FALSE;
    Net.Limited = Limited;
    Net.Roaming = Roaming;
    Net.Rat = (Best && BestStatus != QCRIL_SRV_NO_SERVICE) ? Best->Rat : RIL_RAT_NONE;

    EnterCriticalSection(&Modem->StateLock);
    Modem->Nas.SystemTypes = Types;
    LeaveCriticalSection(&Modem->StateLock);

    QcNasPublishReg(Modem, &Net, FALSE);
}

/* Builds the signal records from a signal information message */
static
VOID
QcNasSigInfo(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    RIL_SIGNAL_QUALITY Records[QC_NAS_MAX_SIGNAL];
    const UCHAR *Sig;
    const VOID *Value;
    USHORT ValueLen;
    ULONG Count = 0, i;
    DWORD Types;
    BOOL Changed;

    ZeroMemory(Records, sizeof(Records));

    EnterCriticalSection(&Modem->StateLock);
    Types = Modem->Nas.SystemTypes;
    LeaveCriticalSection(&Modem->StateLock);

    if (Types == 0)
    {
        /* Nothing in service, report the executor alone */
        Records[0].Size = sizeof(Records[0]);
        Records[0].Params = RIL_SQPARAM_EXECUTOR | RIL_SQPARAM_SYSTEMTYPE;
        Records[0].Executor = QC_EXECUTOR;
        Count = 1;
    }

    if (Types & RIL_RAT_GSM && Count < QC_NAS_MAX_SIGNAL &&
        QmiFindTlv(Message, Length, QCRIL_NAS_TLV_SIG_GSM, &Value, &ValueLen) && ValueLen >= 1)
    {
        Sig = Value;
        Records[Count].SystemType = RIL_RAT_GSM;
        Records[Count].Strength = (CHAR)Sig[0];
        Records[Count].Bars = QcNasBars(Records[Count].Strength, QcGsmRssiBars);
        Count++;
    }

    if (Types & RIL_RAT_LTE && Count < QC_NAS_MAX_SIGNAL &&
        QmiFindTlv(Message, Length, QCRIL_NAS_TLV_SIG_LTE, &Value, &ValueLen) && ValueLen >= 6)
    {
        Sig = Value;
        Records[Count].SystemType = RIL_RAT_LTE;
        Records[Count].Strength = (SHORT)(Sig[2] | (Sig[3] << 8));
        Records[Count].Bars = QcNasBars(Records[Count].Strength, QcLteRsrpBars);
        QcTrace("sig info: LTE rssi %d rsrq %d rsrp %ld snr %d\n", (CHAR)Sig[0], (CHAR)Sig[1],
                Records[Count].Strength, (SHORT)(Sig[4] | (Sig[5] << 8)));
        Count++;
    }

    if (Types & RIL_RAT_UMTS && Count < QC_NAS_MAX_SIGNAL &&
        QmiFindTlv(Message, Length, QCRIL_NAS_TLV_SIG_WCDMA, &Value, &ValueLen) && ValueLen >= 3)
    {
        Sig = Value;
        Records[Count].SystemType = RIL_RAT_UMTS;
        Records[Count].Strength = (CHAR)Sig[0];
        Records[Count].Bars = QcNasBars(Records[Count].Strength, QcUmtsRssiBars);
        Count++;
    }

    for (i = 0; i < Count; i++)
    {
        if (Records[i].SystemType == 0)
            continue;

        Records[i].Size = sizeof(Records[i]);
        Records[i].Params = RIL_SQPARAM_EXECUTOR | RIL_SQPARAM_SYSTEMTYPE | RIL_SQPARAM_BARS | RIL_SQPARAM_STRENGTH;
        Records[i].Executor = QC_EXECUTOR;
        QcTrace("signal: type 0x%lx %ld dBm, %lu bars\n", Records[i].SystemType, Records[i].Strength, Records[i].Bars);
    }

    if (Count == 0)
        return;

    /* Only a change of the system types or of the bars is reported */
    EnterCriticalSection(&Modem->StateLock);
    Changed = Count != Modem->Nas.SignalCount;
    for (i = 0; i < Count && !Changed; i++)
    {
        if (Records[i].SystemType != Modem->Nas.Signal[i].SystemType || Records[i].Bars != Modem->Nas.Signal[i].Bars)
            Changed = TRUE;
    }
    memcpy(Modem->Nas.Signal, Records, Count * sizeof(Records[0]));
    Modem->Nas.SignalCount = Count;
    Modem->Nas.Net.SignalDbm = Records[0].Strength;
    Modem->Nas.Net.SignalBars = Records[0].Bars;
    LeaveCriticalSection(&Modem->StateLock);

    if (Changed)
        QcNotifyAll(Modem, RIL_NOTIFY_SIGNALQUALITY, Records, Count * sizeof(Records[0]));
}

/* Unpacks a GSM 7 bit name, keeping the printable ASCII part of the alphabet */
static
VOID
QcNasUnpackName(
    _In_reads_bytes_(Length) const UCHAR *Packed,
    _In_ ULONG Length,
    _Out_writes_z_(OutSize) PSTR Out,
    _In_ ULONG OutSize)
{
    ULONG Count = (Length * 8) / 7, i, Written = 0;

    for (i = 0; i < Count && Written + 1 < OutSize; i++)
    {
        ULONG Bit = i * 7;
        ULONG Code = (Packed[Bit / 8] >> (Bit % 8));

        if (Bit % 8 > 1 && Bit / 8 + 1 < Length)
            Code |= Packed[Bit / 8 + 1] << (8 - Bit % 8);
        Code &= 0x7F;

        if (Code == 0 && i + 1 == Count)
            break;
        if (Code == 0x00)
            Code = '@';
        Out[Written++] = (Code >= 0x20 && Code < 0x7F) ? (CHAR)Code : '?';
    }

    Out[Written] = '\0';
}

/* Name record: encoding, then for PLMN names country initials and spare bits, length, bytes */
static
BOOL
QcNasDecodeName(
    _In_reads_bytes_(ValueLen) const UCHAR *Value,
    _In_ USHORT ValueLen,
    _In_ ULONG Header,
    _Out_writes_z_(OutSize) PSTR Out,
    _In_ ULONG OutSize)
{
    ULONG Length, i, Written = 0;

    Out[0] = '\0';
    if (ValueLen < Header + 1)
        return FALSE;

    Length = Value[Header];
    if (Length == 0 || Header + 1 + Length > ValueLen)
        return FALSE;

    /* GSM default alphabet. The modem hands it over one character per byte,
       so only a name with the top bit set somewhere is still packed */
    if (Value[0] == 0)
    {
        for (i = 0; i < Length; i++)
        {
            if (Value[Header + 1 + i] & 0x80)
                break;
        }

        if (i < Length)
        {
            QcNasUnpackName(Value + Header + 1, Length, Out, OutSize);
            return Out[0] != '\0';
        }

        for (i = 0; i < Length && Written + 1 < OutSize; i++)
        {
            UCHAR Code = Value[Header + 1 + i];

            if (Code == 0)
                break;
            Out[Written++] = (Code >= 0x20 && Code < 0x7F) ? (CHAR)Code : '?';
        }
        Out[Written] = '\0';
        return Written != 0;
    }

    /* UCS2 little endian, kept to its ASCII part */
    for (i = 0; i + 1 < Length && Written + 1 < OutSize; i += 2)
    {
        WCHAR Ch = Value[Header + 1 + i] | (Value[Header + 2 + i] << 8);
        if (!Ch)
            break;
        Out[Written++] = (Ch < 0x80) ? (CHAR)Ch : '?';
    }
    Out[Written] = '\0';
    return Written != 0;
}

static
VOID
QcNasPlmnName(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    RIL_NETWORK_INFO Net;
    CHAR Name[RIL_INFO_OPERATOR];
    const VOID *Value;
    USHORT ValueLen;
    BOOL Have = FALSE;

    /* The raw records, until every network's encoding is known to decode right */
    if (QmiFindTlv(Message, Length, QCRIL_NAS_TLV_PLMN_LONG, &Value, &ValueLen) && ValueLen >= 4)
    {
        const UCHAR *Raw = Value;
        QcTrace("plmn long name: encoding %u ci %u spare %u length %u bytes %02x %02x %02x %02x %02x %02x %02x %02x\n",
                Raw[0], Raw[1], Raw[2], Raw[3],
                ValueLen > 4 ? Raw[4] : 0, ValueLen > 5 ? Raw[5] : 0, ValueLen > 6 ? Raw[6] : 0,
                ValueLen > 7 ? Raw[7] : 0, ValueLen > 8 ? Raw[8] : 0, ValueLen > 9 ? Raw[9] : 0,
                ValueLen > 10 ? Raw[10] : 0, ValueLen > 11 ? Raw[11] : 0);
    }
    if (QmiFindTlv(Message, Length, QCRIL_NAS_TLV_PLMN_SHORT, &Value, &ValueLen) && ValueLen >= 4)
    {
        const UCHAR *Raw = Value;
        QcTrace("plmn short name: encoding %u ci %u spare %u length %u bytes %02x %02x %02x %02x %02x %02x %02x %02x\n",
                Raw[0], Raw[1], Raw[2], Raw[3],
                ValueLen > 4 ? Raw[4] : 0, ValueLen > 5 ? Raw[5] : 0, ValueLen > 6 ? Raw[6] : 0,
                ValueLen > 7 ? Raw[7] : 0, ValueLen > 8 ? Raw[8] : 0, ValueLen > 9 ? Raw[9] : 0,
                ValueLen > 10 ? Raw[10] : 0, ValueLen > 11 ? Raw[11] : 0);
    }

    if (QmiFindTlv(Message, Length, QCRIL_NAS_TLV_PLMN_LONG, &Value, &ValueLen))
        Have = QcNasDecodeName(Value, ValueLen, 3, Name, sizeof(Name));
    if (!Have && QmiFindTlv(Message, Length, QCRIL_NAS_TLV_PLMN_SHORT, &Value, &ValueLen))
        Have = QcNasDecodeName(Value, ValueLen, 3, Name, sizeof(Name));
    if (!Have && QmiFindTlv(Message, Length, QCRIL_NAS_TLV_PLMN_SPN, &Value, &ValueLen))
        Have = QcNasDecodeName(Value, ValueLen, 1, Name, sizeof(Name));

    if (!Have)
    {
        QcTrace("current plmn name: no usable name\n");
        return;
    }

    QcTrace("current plmn name: '%s'\n", Name);

    EnterCriticalSection(&Modem->StateLock);
    Net = Modem->Nas.Net;
    LeaveCriticalSection(&Modem->StateLock);

    if (strcmp(Net.Operator, Name) == 0)
        return;

    memcpy(Net.Operator, Name, sizeof(Net.Operator));
    QcNasPublishReg(Modem, &Net, TRUE);
}

/* SIM service provider name and PLMN name; the SPN names the operator until the network does */
static
VOID
QcNasOperatorNameData(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    RIL_NETWORK_INFO Net;
    CHAR Spn[RIL_INFO_OPERATOR];
    CHAR Plmn[RIL_INFO_OPERATOR];
    const UCHAR *Bytes;
    const VOID *Value;
    USHORT ValueLen;
    ULONG Count, i, Written;
    UCHAR Condition = 0;

    Spn[0] = '\0';
    Plmn[0] = '\0';

    /* Display condition, length, then the name unpacked one character per byte */
    if (QmiFindTlv(Message, Length, QCRIL_NAS_TLV_OPNAME_SPN, &Value, &ValueLen) && ValueLen >= 2)
    {
        Bytes = Value;
        Condition = Bytes[0];
        Count = min((ULONG)Bytes[1], (ULONG)ValueLen - 2);
        for (i = 0, Written = 0; i < Count && Written + 1 < sizeof(Spn); i++)
        {
            if (Bytes[2 + i] == 0xFF || Bytes[2 + i] == 0)
                break;
            Spn[Written++] = (Bytes[2 + i] >= 0x20 && Bytes[2 + i] < 0x7F) ? (CHAR)Bytes[2 + i] : '?';
        }
        Spn[Written] = '\0';
    }

    if (QmiFindTlv(Message, Length, QCRIL_NAS_TLV_OPNAME_PLMN, &Value, &ValueLen) && ValueLen >= 1)
    {
        Bytes = Value;
        Count = min((ULONG)Bytes[0], (ULONG)ValueLen - 1);
        Count = min(Count, (ULONG)sizeof(Plmn) - 1);
        memcpy(Plmn, Bytes + 1, Count);
        Plmn[Count] = '\0';
    }

    QcTrace("operator name data: spn '%s' (display condition %u), plmn name '%s'\n", Spn, Condition, Plmn);

    if (!Spn[0])
        return;

    EnterCriticalSection(&Modem->StateLock);
    Net = Modem->Nas.Net;
    LeaveCriticalSection(&Modem->StateLock);

    if (Net.Operator[0])
        return;

    memcpy(Net.Operator, Spn, sizeof(Net.Operator));
    QcNasPublishReg(Modem, &Net, TRUE);
}

static
VOID
QcNasNetworkReject(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    RIL_NETWORK_INFO Net;
    const VOID *Value;
    USHORT ValueLen;
    UCHAR Radio = 0, Cause;

    if (!QmiFindTlv(Message, Length, QCRIL_NAS_TLV_REJECT_CAUSE, &Value, &ValueLen) || ValueLen < 1)
        return;
    Cause = *(const UCHAR *)Value;

    if (QmiFindTlv(Message, Length, QCRIL_NAS_TLV_REJECT_RADIO, &Value, &ValueLen) && ValueLen >= 1)
        Radio = *(const UCHAR *)Value;

    QcTrace("network reject: radio %u cause %u\n", Radio, Cause);
    if (Cause == 0)
        return;

    /* The cached registration goes out again carrying the reject cause */
    EnterCriticalSection(&Modem->StateLock);
    Net = Modem->Nas.Net;
    LeaveCriticalSection(&Modem->StateLock);

    Net.RejectCause = Cause;
    QcNasPublishReg(Modem, &Net, TRUE);
}

/* Queries system and signal information once, the indications keep them current */
static
VOID
QcNasRefresh(
    _In_ PQC_MODEM Modem)
{
    QMI_ENCODER Encoder;
    BYTE Request[16];
    BYTE Response[QC_MESSAGE_MAX];
    ULONG Length, ResponseLen;

    QmiEncodeBegin(&Encoder, Request, sizeof(Request), QMI_TYPE_REQUEST, 0, QCRIL_NAS_GET_SYS_INFO);
    Length = QmiEncodeEnd(&Encoder);
    if (SUCCEEDED(QcQmiRequest(Modem, QcSvcNas, Request, Length, Response, sizeof(Response), &ResponseLen)))
        QcNasSysInfo(Modem, Response, ResponseLen);

    QmiEncodeBegin(&Encoder, Request, sizeof(Request), QMI_TYPE_REQUEST, 0, QCRIL_NAS_GET_SIG_INFO);
    Length = QmiEncodeEnd(&Encoder);
    if (SUCCEEDED(QcQmiRequest(Modem, QcSvcNas, Request, Length, Response, sizeof(Response), &ResponseLen)))
        QcNasSigInfo(Modem, Response, ResponseLen);
}

/* Signal reports fire when a measurement crosses one of the bar thresholds */
static
VOID
QcNasConfigureSignal(
    _In_ PQC_MODEM Modem)
{
    QMI_ENCODER Encoder;
    BYTE Request[96];
    UCHAR Rssi[1 + QCRIL_THRESHOLD_COUNT];
    BYTE Rsrp[1 + QCRIL_THRESHOLD_COUNT * sizeof(SHORT)];
    ULONG Length, i;
    HRESULT hr;

    Rssi[0] = QCRIL_THRESHOLD_COUNT;
    Rsrp[0] = QCRIL_THRESHOLD_COUNT;
    for (i = 0; i < QCRIL_THRESHOLD_COUNT; i++)
    {
        Rssi[1 + i] = (UCHAR)(CHAR)QcGsmRssiBars[i];
        memcpy(&Rsrp[1 + i * sizeof(SHORT)], &QcLteRsrpBars[i], sizeof(SHORT));
    }

    QmiEncodeBegin(&Encoder, Request, sizeof(Request), QMI_TYPE_REQUEST, 0, QCRIL_NAS_CONFIG_SIG_INFO);
    QmiEncodeTlv(&Encoder, QCRIL_NAS_TLV_RSSI_THRESHOLDS, Rssi, sizeof(Rssi));
    QmiEncodeTlv(&Encoder, QCRIL_NAS_TLV_RSRP_THRESHOLDS, Rsrp, sizeof(Rsrp));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiCommand(Modem, QcSvcNas, Request, Length);
    QcTrace("nas: signal thresholds 0x%08lx\n", hr);
}

VOID
QcNasSetup(
    _In_ PQC_MODEM Modem)
{
    static const UCHAR Enabled[] =
    {
        QCRIL_NAS_REG_SYS_SEL_PREF,
        QCRIL_NAS_REG_DUAL_STANDBY,
        QCRIL_NAS_REG_NETWORK_TIME,
        QCRIL_NAS_REG_SYS_INFO,
        QCRIL_NAS_REG_SIG_INFO,
        QCRIL_NAS_REG_MANAGED_ROAMING,
        QCRIL_NAS_REG_PLMN_NAME,
        QCRIL_NAS_REG_SIB16_TIME
    };
    QMI_ENCODER Encoder;
    BYTE Request[96];
    BYTE Response[512];
    const VOID *Value;
    USHORT ValueLen;
    UCHAR One = 1, Zero = 0, Primary = 0;
    UCHAR Reject[2] = { 1, 1 };
    ULONG Length, ResponseLen, i;
    HRESULT hr;

    /* The primary subscription */
    QmiEncodeBegin(&Encoder, Request, sizeof(Request), QMI_TYPE_REQUEST, 0, QCRIL_NAS_BIND_SUBSCRIPTION);
    QmiEncodeTlv(&Encoder, 0x01, &Primary, sizeof(Primary));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiCommand(Modem, QcSvcNas, Request, Length);
    QcTrace("nas: bind subscription 0x%08lx\n", hr);

    QmiEncodeBegin(&Encoder, Request, sizeof(Request), QMI_TYPE_REQUEST, 0, QCRIL_NAS_SYS_SEL_PREF);
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiRequest(Modem, QcSvcNas, Request, Length, Response, sizeof(Response), &ResponseLen);
    if (SUCCEEDED(hr) && QmiFindTlv(Response, ResponseLen, 0x11, &Value, &ValueLen) && ValueLen >= 2)
        QcTrace("nas: mode preference 0x%04x\n", *(const USHORT UNALIGNED *)Value);
    else
        QcTrace("nas: system selection preference 0x%08lx\n", hr);

    /* Serving system reports are replaced by the system information ones */
    QmiEncodeBegin(&Encoder, Request, sizeof(Request), QMI_TYPE_REQUEST, 0, QCRIL_NAS_INDICATION_REGISTER);
    for (i = 0; i < RTL_NUMBER_OF(Enabled); i++)
        QmiEncodeTlv(&Encoder, Enabled[i], &One, sizeof(One));
    QmiEncodeTlv(&Encoder, QCRIL_NAS_REG_SERVING_SYSTEM, &Zero, sizeof(Zero));
    QmiEncodeTlv(&Encoder, QCRIL_NAS_REG_NETWORK_REJECT, Reject, sizeof(Reject));
    Length = QmiEncodeEnd(&Encoder);
    hr = QcQmiCommand(Modem, QcSvcNas, Request, Length);
    QcTrace("nas: indication registration 0x%08lx\n", hr);

    QcNasConfigureSignal(Modem);
    QcNasRefresh(Modem);
}

/* The modem restarted, report it unregistered until it says otherwise */
VOID
QcNasModemLost(
    _In_ PQC_MODEM Modem)
{
    RIL_NETWORK_INFO Net;

    EnterCriticalSection(&Modem->StateLock);
    Modem->Nas.SystemTypes = 0;
    Modem->Nas.SignalCount = 0;
    Modem->Nas.Net.SignalDbm = 0;
    Modem->Nas.Net.SignalBars = 0;
    LeaveCriticalSection(&Modem->StateLock);

    ZeroMemory(&Net, sizeof(Net));
    Net.Size = sizeof(Net);
    Net.RegState = RIL_REG_UNREGISTERED;
    QcNasPublishReg(Modem, &Net, TRUE);
}

HRESULT
QcNasGetRegStatus(
    _In_ PQC_MODEM Modem,
    _Inout_ PQC_RESP Resp)
{
    RIL_NETWORK_INFO Net;

    if (!QcServiceEnsure(Modem, QcSvcNas))
        return RIL_E_RADIONOTPRESENT;

    EnterCriticalSection(&Modem->StateLock);
    Net = Modem->Nas.Net;
    LeaveCriticalSection(&Modem->StateLock);

    QcRespBytes(Resp, &Net, sizeof(Net));
    return S_OK;
}

HRESULT
QcNasGetSignalQuality(
    _In_ PQC_MODEM Modem,
    _Inout_ PQC_RESP Resp)
{
    RIL_SIGNAL_QUALITY Records[QC_NAS_MAX_SIGNAL];
    ULONG Count;

    if (!QcServiceEnsure(Modem, QcSvcNas))
        return RIL_E_RADIONOTPRESENT;

    EnterCriticalSection(&Modem->StateLock);
    Count = Modem->Nas.SignalCount;
    memcpy(Records, Modem->Nas.Signal, sizeof(Records));
    LeaveCriticalSection(&Modem->StateLock);

    if (Count == 0)
    {
        ZeroMemory(Records, sizeof(Records[0]));
        Records[0].Size = sizeof(Records[0]);
        Records[0].Params = RIL_SQPARAM_EXECUTOR | RIL_SQPARAM_SYSTEMTYPE;
        Records[0].Executor = QC_EXECUTOR;
        Count = 1;
    }

    QcRespBytes(Resp, Records, Count * sizeof(Records[0]));
    return S_OK;
}

/* Universal time, the zone in quarter hours and the daylight saving hours become one NITZ notification */
static
VOID
QcNasNetworkTime(
    _In_ PQC_MODEM Modem,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    RIL_NITZ_INFO Nitz;
    const VOID *Value;
    const UCHAR *Time;
    USHORT ValueLen;

    if (!QmiFindTlv(Message, Length, 0x01, &Value, &ValueLen) || ValueLen < 8)
        return;

    Time = Value;
    ZeroMemory(&Nitz, sizeof(Nitz));
    Nitz.Size = sizeof(Nitz);
    Nitz.NotificationCode = RIL_NOTIFY_NITZ;
    Nitz.SysTime.wYear = (WORD)(Time[0] | (Time[1] << 8));
    Nitz.SysTime.wMonth = Time[2];
    Nitz.SysTime.wDay = Time[3];
    Nitz.SysTime.wHour = Time[4];
    Nitz.SysTime.wMinute = Time[5];
    Nitz.SysTime.wSecond = Time[6];

    /* The modem counts weekdays from Monday */
    Nitz.SysTime.wDayOfWeek = (WORD)((Time[7] + 1) % 7);
    Nitz.Params = RIL_NITZPARAM_SYSTEMTIME;

    if (QmiFindTlv(Message, Length, 0x10, &Value, &ValueLen) && ValueLen >= 1)
    {
        Nitz.TimeZoneOffsetMinutes = *(const CHAR *)Value * 15;
        Nitz.Params |= RIL_NITZPARAM_TIMEZONEOFFSET;
    }

    if (QmiFindTlv(Message, Length, 0x11, &Value, &ValueLen) && ValueLen >= 1)
    {
        Nitz.DaylightSavingOffsetMinutes = *(const UCHAR *)Value * 60;
        Nitz.Params |= RIL_NITZPARAM_DAYLIGHTSAVINGOFFSET;
    }

    QcTrace("network time %04u-%02u-%02u %02u:%02u:%02u UTC, zone %ld min, daylight saving %ld min\n",
            Nitz.SysTime.wYear, Nitz.SysTime.wMonth, Nitz.SysTime.wDay, Nitz.SysTime.wHour,
            Nitz.SysTime.wMinute, Nitz.SysTime.wSecond, Nitz.TimeZoneOffsetMinutes,
            Nitz.DaylightSavingOffsetMinutes);

    QcNotifyAll(Modem, RIL_NOTIFY_NITZ, &Nitz, sizeof(Nitz));
}

VOID
QcNasIndication(
    _In_ PQC_MODEM Modem,
    _In_ USHORT MessageId,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    const VOID *Value;
    USHORT ValueLen;

    switch (MessageId)
    {
        case QCRIL_NAS_SYS_INFO_IND:
            QcNasSysInfo(Modem, Message, Length);
            break;

        case QCRIL_NAS_SIG_INFO_IND:
            QcNasSigInfo(Modem, Message, Length);
            break;

        case QCRIL_NAS_NETWORK_REJECT_IND:
            QcNasNetworkReject(Modem, Message, Length);
            break;

        case QCRIL_NAS_CURRENT_PLMN_NAME_IND:
            QcNasPlmnName(Modem, Message, Length);
            break;

        case QCRIL_NAS_OPERATOR_NAME_IND:
            QcNasOperatorNameData(Modem, Message, Length);
            break;

        case QCRIL_NAS_SYS_SEL_PREF:
            if (QmiFindTlv(Message, Length, 0x11, &Value, &ValueLen) && ValueLen >= 2)
                QcTrace("system selection: mode preference 0x%04x\n", *(const USHORT UNALIGNED *)Value);
            else
                QcTrace("system selection preference changed\n");
            break;

        case QCRIL_NAS_DUAL_STANDBY_PREF_IND:
            QcTrace("dual standby preference changed\n");
            break;

        case QCRIL_NAS_NETWORK_TIME_IND:
            QcNasNetworkTime(Modem, Message, Length);
            break;

        case QCRIL_NAS_SERVING_SYSTEM_IND:
            QcTrace("serving system changed, sys info carries the state\n");
            break;

        default:
            QcTrace("ind NAS 0x%04X not handled\n", MessageId);
            break;
    }
}
