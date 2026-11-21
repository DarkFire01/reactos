/*
 * PROJECT:     ReactOS Telephony
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Structured device, subscriber, network and data records
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/*
 * These records are the payloads the OEM RIL library returns for the device
 * information, card information and registration queries, and the data request
 * it accepts through the device specific command. They are shared so the phone
 * service decodes exactly what the library encodes.
 */

#define RIL_INFO_STRING             40
#define RIL_INFO_REVISION           80
#define RIL_INFO_OPERATOR           48

/* System type of RIL_NETWORK_INFO.Rat, the Windows RIL system type bits */
#define RIL_RAT_NONE                0x00
#define RIL_RAT_CDMA                0x01
#define RIL_RAT_EVDO                0x02
#define RIL_RAT_GSM                 0x04
#define RIL_RAT_UMTS                0x08
#define RIL_RAT_LTE                 0x10
#define RIL_RAT_TDSCDMA             0x20

/* Registration state in RIL_NETWORK_INFO.RegState, the Windows RIL values */
#define RIL_REG_UNKNOWN             0
#define RIL_REG_UNREGISTERED        1
#define RIL_REG_HOME                2
#define RIL_REG_ATTEMPTING          3
#define RIL_REG_DENIED              4
#define RIL_REG_ROAMING             5
#define RIL_REG_ROAMING_DOMESTIC    6

/* Older names for the same states */
#define RIL_REG_NOT_REGISTERED      RIL_REG_UNREGISTERED
#define RIL_REG_SEARCHING           RIL_REG_ATTEMPTING

/* SIM state in RIL_SUBSCRIBER_INFO.SimState */
#define RIL_SIM_ABSENT              0
#define RIL_SIM_READY               1
#define RIL_SIM_PIN                 2
#define RIL_SIM_PUK                 3
#define RIL_SIM_ERROR               4
#define RIL_SIM_NOT_READY           5

#include <pshpack4.h>

typedef struct _RIL_DEVICE_INFO
{
    DWORD Size;
    CHAR Imei[RIL_INFO_STRING];
    CHAR Manufacturer[RIL_INFO_STRING];
    CHAR Model[RIL_INFO_STRING];
    CHAR Revision[RIL_INFO_REVISION];
} RIL_DEVICE_INFO, *PRIL_DEVICE_INFO;

typedef struct _RIL_SUBSCRIBER_INFO
{
    DWORD Size;
    DWORD SimState;
    CHAR Imsi[RIL_INFO_STRING];
    CHAR Msisdn[RIL_INFO_STRING];
} RIL_SUBSCRIBER_INFO, *PRIL_SUBSCRIBER_INFO;

typedef struct _RIL_NETWORK_INFO
{
    DWORD Size;
    DWORD RegState;
    DWORD Rat;
    LONG SignalDbm;
    DWORD SignalBars;
    DWORD Mcc;
    DWORD Mnc;
    DWORD Roaming;
    CHAR Operator[RIL_INFO_OPERATOR];
    DWORD Limited;
    DWORD RejectCause;
    DWORD Lac;
    DWORD CellId;
} RIL_NETWORK_INFO, *PRIL_NETWORK_INFO;

/* RIL_SIGNAL_QUALITY.Params */
#define RIL_SQPARAM_EXECUTOR        0x00000001
#define RIL_SQPARAM_SYSTEMTYPE      0x00000002
#define RIL_SQPARAM_BARS            0x00000004
#define RIL_SQPARAM_STRENGTH        0x00000008
#define RIL_SQPARAM_SECONDARY       0x00000010

/* Signal quality of one system type, the Windows RIL layout. Strength and Secondary are dBm or dB. */
typedef struct _RIL_SIGNAL_QUALITY
{
    DWORD Size;
    DWORD Params;
    DWORD Executor;
    DWORD SystemType;
    DWORD Bars;
    LONG Strength;
    LONG Secondary;
} RIL_SIGNAL_QUALITY, *PRIL_SIGNAL_QUALITY;

C_ASSERT(sizeof(RIL_SIGNAL_QUALITY) == 28);

/* RIL_NITZ_INFO.Params */
#define RIL_NITZPARAM_SYSTEMTIME            0x00000001
#define RIL_NITZPARAM_TIMEZONEOFFSET        0x00000002
#define RIL_NITZPARAM_DAYLIGHTSAVINGOFFSET  0x00000004

/* Network time, the Windows RIL layout. The zone offset is local minus UTC with daylight
   saving included, the daylight saving part is reported on its own as well. */
typedef struct _RIL_NITZ_INFO
{
    DWORD Size;
    DWORD Params;
    DWORD NotificationCode;
    LONG TimeZoneOffsetMinutes;
    LONG DaylightSavingOffsetMinutes;
    SYSTEMTIME SysTime;
} RIL_NITZ_INFO, *PRIL_NITZ_INFO;

C_ASSERT(sizeof(RIL_NITZ_INFO) == 36);

/* RIL_GetDeviceInfo classes, each answered as one UTF-16 string */
#define RIL_DEVICEINFO_MANUFACTURER 1
#define RIL_DEVICEINFO_MODEL        2
#define RIL_DEVICEINFO_REVISION     3
#define RIL_DEVICEINFO_SERIALNUMBER 4
#define RIL_DEVICEINFO_SERIALNUMBER_CDMA 5

/* Data request operations carried over the device specific command */
#define RIL_DATA_OP_STATUS          0
#define RIL_DATA_OP_CONNECT         1
#define RIL_DATA_OP_DISCONNECT      2

/* Authentication in RIL_DATA_REQUEST.AuthType */
#define RIL_DATA_AUTH_NONE          0
#define RIL_DATA_AUTH_PAP           1
#define RIL_DATA_AUTH_CHAP          2

#define RIL_DATA_APN                64
#define RIL_DATA_CREDENTIAL         48

typedef struct _RIL_DATA_REQUEST
{
    DWORD Size;
    DWORD Op;
    DWORD AuthType;
    CHAR Apn[RIL_DATA_APN];
    CHAR Username[RIL_DATA_CREDENTIAL];
    CHAR Password[RIL_DATA_CREDENTIAL];
} RIL_DATA_REQUEST, *PRIL_DATA_REQUEST;

typedef struct _RIL_DATA_STATUS
{
    DWORD Size;
    DWORD Connected;
    DWORD Handle;
    DWORD Reserved;
} RIL_DATA_STATUS, *PRIL_DATA_STATUS;

#include <poppack.h>
