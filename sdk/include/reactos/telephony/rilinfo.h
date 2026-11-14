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

/* Radio access technology reported in RIL_NETWORK_INFO.Rat */
#define RIL_RAT_NONE                0
#define RIL_RAT_GSM                 1
#define RIL_RAT_UMTS                2
#define RIL_RAT_LTE                 3
#define RIL_RAT_CDMA                4
#define RIL_RAT_TDSCDMA             5
#define RIL_RAT_EVDO                6

/* Registration state in RIL_NETWORK_INFO.RegState */
#define RIL_REG_NOT_REGISTERED      0
#define RIL_REG_HOME                1
#define RIL_REG_SEARCHING           2
#define RIL_REG_DENIED              3
#define RIL_REG_UNKNOWN             4
#define RIL_REG_ROAMING             5

/* SIM state in RIL_SUBSCRIBER_INFO.SimState */
#define RIL_SIM_ABSENT              0
#define RIL_SIM_READY               1
#define RIL_SIM_PIN                 2
#define RIL_SIM_PUK                 3
#define RIL_SIM_ERROR               4

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
} RIL_NETWORK_INFO, *PRIL_NETWORK_INFO;

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
