/*
 * PROJECT:     ReactOS Phone Test
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Definitions shared by the pages of the phone test
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <wingdi.h>
#include <stdlib.h>
#include <strsafe.h>

#define WM_PHONE_EVENT              (WM_APP + 1)
#define WM_MODEM_PACKET             (WM_APP + 2)

enum
{
    IDC_TAB_PHONE = 200,
    IDC_TAB_STATUS,
    IDC_TAB_DATA,
    IDC_TAB_MODEM,

    IDC_RADIO_STATUS = 100,
    IDC_RADIO_ON,
    IDC_RADIO_OFF,
    IDC_NUMBER,
    IDC_DIAL,
    IDC_ANSWER,
    IDC_HANGUP,
    IDC_SWAP,
    IDC_DTMF,
    IDC_SPEAKER,
    IDC_MUTE,
    IDC_CALLS,
    IDC_SMS_TO,
    IDC_SMS_TEXT,
    IDC_SMS_SEND,
    IDC_MESSAGES,
    IDC_LOG,

    IDC_STATUS_REFRESH = 210,

    IDC_DATA_APN_LABEL = 220,
    IDC_DATA_APN,
    IDC_DATA_USER_LABEL,
    IDC_DATA_USER,
    IDC_DATA_PASS_LABEL,
    IDC_DATA_PASS,
    IDC_DATA_AUTH_LABEL,
    IDC_DATA_AUTH,
    IDC_DATA_ENABLED,
    IDC_DATA_SAVE,
    IDC_DATA_CONNECT,
    IDC_DATA_DISCONNECT,
    IDC_DATA_STATUS,

    IDC_EXIT = 240,

    IDC_MODEM_STATUS = 300,
    IDC_MODEM_SERVICES,
    IDC_MODEM_MODE,
    IDC_MODEM_ONLINE,
    IDC_MODEM_LOW_POWER,
    IDC_MODEM_OFFLINE,
    IDC_MODEM_RESET,
    IDC_MODEM_SIGNAL,
    IDC_MODEM_SERVING,
    IDC_MODEM_SIM,
    IDC_MODEM_IDENTITY,
    IDC_MODEM_RAW,
    IDC_MODEM_SEND,
    IDC_MODEM_CLEAR,
    IDC_MODEM_LOG
};

/* A packet the router delivered, handed from the receive thread to the window */
typedef struct _PT_MODEM_PACKET
{
    ULONG Node;
    ULONG Port;
    ULONG Length;
    BYTE Data[ANYSIZE_ARRAY];
} PT_MODEM_PACKET, *PPT_MODEM_PACKET;

/* modem.c */
BOOL
PtModemIsCommand(
    _In_ UINT Id);

VOID
PtModemOpen(
    _In_ HWND Window);

VOID
PtModemClose(VOID);

VOID
PtModemCommand(
    _In_ UINT Id);

VOID
PtModemPacket(
    _In_ const PT_MODEM_PACKET *Packet);
