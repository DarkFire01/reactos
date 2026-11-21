/*
 * PROJECT:     ReactOS Native Time Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Messages between the native time service and its clients
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define NATIVETIME_PIPE_NAME        L"\\\\.\\pipe\\ReactOS.NativeTime"
#define NATIVETIME_MAX_LABEL        64
#define NATIVETIME_MAX_TRIGGERS     32
#define NATIVETIME_MAX_ZONE         128

/* Command line the service starts the clock app with when an alarm rings and no client listens */
#define NATIVETIME_RING_SWITCH      L"/ring"

typedef enum _NATIVETIME_KIND
{
    NativeTimeAlarm = 1,            /* local Hour:Minute, repeating on Days or once */
    NativeTimeTimer                 /* one shot at Due */
} NATIVETIME_KIND;

/* NATIVETIME_TRIGGER.Flags */
#define NATIVETIME_FLAG_ENABLED     0x00000001
#define NATIVETIME_FLAG_RINGING     0x00000002  /* set by the service while it rings */

/* NATIVETIME_TRIGGER.Days, a repeating alarm; zero rings once */
#define NATIVETIME_DAY_SUNDAY       0x01
#define NATIVETIME_DAY_MONDAY       0x02
#define NATIVETIME_DAY_TUESDAY      0x04
#define NATIVETIME_DAY_WEDNESDAY    0x08
#define NATIVETIME_DAY_THURSDAY     0x10
#define NATIVETIME_DAY_FRIDAY       0x20
#define NATIVETIME_DAY_SATURDAY     0x40
#define NATIVETIME_DAY_ALL          0x7F

typedef struct _NATIVETIME_TRIGGER
{
    DWORD Id;                       /* zero asks the service for a new one */
    DWORD Kind;
    DWORD Flags;
    WORD Hour;                      /* alarms, local time */
    WORD Minute;
    DWORD Days;
    FILETIME Due;                   /* UTC; timers set it, the service fills the next ring for alarms */
    FILETIME SnoozeUntil;           /* UTC, zero when not snoozed */
    WCHAR Label[NATIVETIME_MAX_LABEL];
} NATIVETIME_TRIGGER, *PNATIVETIME_TRIGGER;

typedef enum _NATIVETIME_COMMAND
{
    NativeTimeList = 1,             /* reply carries every trigger */
    NativeTimeSet,                  /* add or replace Trigger, reply Id is its id */
    NativeTimeDelete,               /* Id */
    NativeTimeSnooze,               /* Id, SnoozeMinutes */
    NativeTimeDismiss,              /* Id */
    NativeTimeSubscribe,            /* the connection then only carries NATIVETIME_EVENT messages */
    NativeTimeStatus                /* reply Status block */
} NATIVETIME_COMMAND;

typedef struct _NATIVETIME_REQUEST
{
    DWORD Command;
    DWORD Id;
    DWORD SnoozeMinutes;
    NATIVETIME_TRIGGER Trigger;
} NATIVETIME_REQUEST, *PNATIVETIME_REQUEST;

typedef enum _NATIVETIME_SOURCE
{
    NativeTimeSourceNone = 0,
    NativeTimeSourceClock,          /* the platform clock at boot */
    NativeTimeSourceNetwork         /* cellular network time */
} NATIVETIME_SOURCE;

typedef struct _NATIVETIME_STATUS
{
    DWORD Source;
    FILETIME LastSync;              /* UTC */
    LONG OffsetMinutes;             /* network offset from UTC, daylight saving included */
    WCHAR Zone[NATIVETIME_MAX_ZONE];
} NATIVETIME_STATUS;

typedef struct _NATIVETIME_REPLY
{
    DWORD Error;                    /* Win32 error code */
    DWORD Id;
    DWORD Count;
    NATIVETIME_STATUS Status;
    NATIVETIME_TRIGGER Triggers[NATIVETIME_MAX_TRIGGERS];
} NATIVETIME_REPLY, *PNATIVETIME_REPLY;

typedef enum _NATIVETIME_EVENT_CODE
{
    NativeTimeEventRing = 1,        /* Trigger is ringing now */
    NativeTimeEventStopped,         /* Trigger stopped ringing: dismissed, snoozed or timed out */
    NativeTimeEventChanged,         /* the trigger list changed */
    NativeTimeEventTimeSet          /* the system time or zone was set */
} NATIVETIME_EVENT_CODE;

typedef struct _NATIVETIME_EVENT
{
    DWORD Code;
    NATIVETIME_TRIGGER Trigger;
} NATIVETIME_EVENT, *PNATIVETIME_EVENT;
