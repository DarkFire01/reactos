/*
 * PROJECT:     ReactOS Telephony
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Messages between phoneom.dll and the phone service
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <telephony/phoneom.h>

#define PHONE_PIPE_NAME             L"\\\\.\\pipe\\ReactOS.PhoneService"
#define PHONE_PIPE_MAX_MESSAGE      (64 * 1024)

typedef enum _PHONE_PIPE_KIND
{
    PhonePipeRequest = 1,
    PhonePipeReply,
    PhonePipeEvent
} PHONE_PIPE_KIND;

typedef enum _PHONE_PIPE_REQUEST
{
    PhoneRequestGetRadio = 1,
    PhoneRequestSetRadioPower,
    PhoneRequestDial,
    PhoneRequestAnswer,
    PhoneRequestHangUp,
    PhoneRequestSwap,
    PhoneRequestDtmf,
    PhoneRequestGetCalls,
    PhoneRequestSetAudioRoute,
    PhoneRequestSetMute,
    PhoneRequestSendMessage,
    PhoneRequestGetMessages,
    PhoneRequestMarkRead,
    PhoneRequestDeleteMessage
} PHONE_PIPE_REQUEST;

/* Every pipe message starts with this, the payload follows */
typedef struct _PHONE_PIPE_HEADER
{
    DWORD Kind;
    DWORD Code;
    DWORD Sequence;
    HRESULT Status;
    DWORD DataSize;
} PHONE_PIPE_HEADER, *PPHONE_PIPE_HEADER;

/* Request payloads that are not a single DWORD */
typedef struct _PHONE_REQUEST_DIAL
{
    WCHAR Number[PHONEOM_MAX_NUMBER];
} PHONE_REQUEST_DIAL;

typedef struct _PHONE_REQUEST_MESSAGE
{
    WCHAR Address[PHONEOM_MAX_NUMBER];
    WCHAR Text[PHONEOM_MAX_TEXT];
} PHONE_REQUEST_MESSAGE;

typedef struct _PHONE_REQUEST_RANGE
{
    DWORD FirstId;
    DWORD MaxCount;
} PHONE_REQUEST_RANGE;
