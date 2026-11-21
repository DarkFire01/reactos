/*
* PROJECT:         ReactOS Kernel
* LICENSE:         GPL - See COPYING in the top level directory
* FILE:            drivers/battery/battc/battc.h
* PURPOSE:         Battery Class Driver
* PROGRAMMERS:     Cameron Gutman (cameron.gutman@reactos.org)
*/

#pragma once

#include <ntddk.h>
#include <wmilib.h>
#include <initguid.h>
#include <batclass.h>

typedef struct _BATTERY_CLASS_DATA
{
    BATTERY_MINIPORT_INFO MiniportInfo;
    UNICODE_STRING InterfaceName;

    /* Tag and status requests that wait, pended on IRP Tail.Overlay.ListEntry */
    KSPIN_LOCK WaitLock;
    LIST_ENTRY WaitList;

    /* Orders SetStatusNotify against DisableStatusNotify, both are called at passive level */
    KEVENT NotifyLock;
    BOOLEAN NotifySet;
    BOOLEAN Unloading;

    /* Waits are rechecked at passive level, on a status notify or when the timer runs out */
    KTIMER Timer;
    KDPC TimerDpc;
    WORK_QUEUE_ITEM WorkItem;
    volatile LONG WorkQueued;
    volatile LONG WorkBusy;
} BATTERY_CLASS_DATA, *PBATTERY_CLASS_DATA;

typedef struct _BATTC_WAIT
{
    PBATTERY_CLASS_DATA Class;
    PIRP Irp;
    UCHAR Kind;
    BOOLEAN Polled;
    ULONGLONG Deadline;
    ULONGLONG NextPoll;
    BATTERY_WAIT_STATUS Criteria;
} BATTC_WAIT, *PBATTC_WAIT;

/* Memory tags */
#define BATTERY_CLASS_DATA_TAG 'CtaB'
#define BATTERY_CLASS_WAIT_TAG 'WtaB'

/* Wait kinds */
#define BATTC_WAIT_TAG       0x01
#define BATTC_WAIT_STATUS    0x02

/* Status waits whose thresholds the miniport cannot watch are read again this often */
#define BATTC_POLL_INTERVAL_MS  5000
