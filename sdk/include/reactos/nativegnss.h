/*
 * PROJECT:     ReactOS GNSS Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Messages between the GNSS service and its clients
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define NATIVEGNSS_PIPE_NAME            L"\\\\.\\pipe\\ReactOS.Gnss"

/* Shortest and default report interval a client can ask for */
#define NATIVEGNSS_MIN_INTERVAL_MS      1000
#define NATIVEGNSS_DEFAULT_INTERVAL_MS  1000

#define NATIVEGNSS_MAX_USED_SV          32

/* NATIVEGNSS_FIX.Flags, which fields hold a value */
#define NATIVEGNSS_FIX_POSITION         0x00000001  /* Latitude, Longitude */
#define NATIVEGNSS_FIX_ALTITUDE         0x00000002  /* Altitude above mean sea level */
#define NATIVEGNSS_FIX_ALTITUDE_WGS84   0x00000004  /* Altitude is above the WGS84 ellipsoid instead */
#define NATIVEGNSS_FIX_ACCURACY         0x00000008  /* HorizontalAccuracy */
#define NATIVEGNSS_FIX_ALT_ACCURACY     0x00000010  /* VerticalAccuracy */
#define NATIVEGNSS_FIX_SPEED            0x00000020
#define NATIVEGNSS_FIX_HEADING          0x00000040
#define NATIVEGNSS_FIX_TIME             0x00000080  /* UtcTime */
#define NATIVEGNSS_FIX_SATELLITES       0x00000100  /* SatellitesUsed and UsedSv */
#define NATIVEGNSS_FIX_DOP              0x00000200
#define NATIVEGNSS_FIX_INTERMEDIATE     0x80000000  /* the engine is still refining this one */

typedef struct _NATIVEGNSS_FIX
{
    DWORD Flags;
    DWORD FixId;                    /* grows with every report the service takes */
    double Latitude;                /* degrees, north positive */
    double Longitude;               /* degrees, east positive */
    FLOAT Altitude;                 /* meters */
    FLOAT HorizontalAccuracy;       /* meters, circular */
    FLOAT VerticalAccuracy;         /* meters */
    FLOAT Speed;                    /* meters per second over ground */
    FLOAT Heading;                  /* degrees from true north */
    FLOAT Pdop;
    FLOAT Hdop;
    FLOAT Vdop;
    FILETIME UtcTime;               /* time of the fix */
    FILETIME Received;              /* system time the service got it */
    DWORD SatellitesUsed;
    WORD UsedSv[NATIVEGNSS_MAX_USED_SV];
} NATIVEGNSS_FIX, *PNATIVEGNSS_FIX;

typedef enum _NATIVEGNSS_STATE
{
    NativeGnssNoService = 0,        /* the modem location service is not there */
    NativeGnssIdle,                 /* nobody asked for location, the engine is off */
    NativeGnssSearching,            /* a session runs, no fix yet */
    NativeGnssFixed                 /* a session runs and the last report had a position */
} NATIVEGNSS_STATE;

typedef struct _NATIVEGNSS_STATUS
{
    DWORD State;
    DWORD Clients;                  /* connections that want location */
    DWORD IntervalMs;               /* of the running session */
    DWORD EngineOn;
    DWORD ServiceRevision;
    DWORD LastSessionStatus;        /* raw status of the last position report */
    DWORD SatellitesInView;
    DWORD SatellitesTracked;
    FLOAT BestSignal;               /* dB-Hz of the strongest satellite heard */
    FILETIME LastFix;               /* system time of the last good fix, zero if none */
} NATIVEGNSS_STATUS, *PNATIVEGNSS_STATUS;

typedef enum _NATIVEGNSS_COMMAND
{
    NativeGnssGetFix = 1,           /* reply carries the last fix, does not start the engine */
    NativeGnssStart,                /* this connection wants location every IntervalMs */
    NativeGnssStop,                 /* this connection no longer wants location */
    NativeGnssSubscribe,            /* like Start, then the connection only carries NATIVEGNSS_EVENT messages */
    NativeGnssGetStatus
} NATIVEGNSS_COMMAND;

typedef struct _NATIVEGNSS_REQUEST
{
    DWORD Command;
    DWORD IntervalMs;               /* zero takes the default */
} NATIVEGNSS_REQUEST, *PNATIVEGNSS_REQUEST;

typedef struct _NATIVEGNSS_REPLY
{
    DWORD Error;                    /* Win32 error code */
    NATIVEGNSS_STATUS Status;
    NATIVEGNSS_FIX Fix;
} NATIVEGNSS_REPLY, *PNATIVEGNSS_REPLY;

typedef enum _NATIVEGNSS_EVENT_CODE
{
    NativeGnssEventFix = 1,         /* Fix is a new report */
    NativeGnssEventState            /* Status.State changed */
} NATIVEGNSS_EVENT_CODE;

typedef struct _NATIVEGNSS_EVENT
{
    DWORD Code;
    NATIVEGNSS_STATUS Status;
    NATIVEGNSS_FIX Fix;
} NATIVEGNSS_EVENT, *PNATIVEGNSS_EVENT;
