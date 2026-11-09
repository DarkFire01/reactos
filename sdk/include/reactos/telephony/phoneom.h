/*
 * PROJECT:     ReactOS Telephony
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Phone object model client interface
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _PHONEOM_SESSION *HPHONEOM;

#define PHONEOM_MAX_NUMBER          64
#define PHONEOM_MAX_NAME            64
#define PHONEOM_MAX_TEXT            1600
#define PHONEOM_MAX_CALLS           8

typedef enum _PHONEOM_CALL_STATE
{
    PhoneCallIdle = 0,
    PhoneCallDialing,
    PhoneCallIncoming,
    PhoneCallActive,
    PhoneCallHeld,
    PhoneCallEnded
} PHONEOM_CALL_STATE;

#define PHONEOM_CALL_INCOMING       0x00000001
#define PHONEOM_CALL_EMERGENCY      0x00000002
#define PHONEOM_CALL_CONFERENCE     0x00000004
#define PHONEOM_CALL_IMS            0x00000008

typedef struct _PHONEOM_CALL
{
    DWORD CallId;
    DWORD State;
    DWORD Flags;
    DWORD Reserved;
    FILETIME StartTime;
    WCHAR Number[PHONEOM_MAX_NUMBER];
    WCHAR Name[PHONEOM_MAX_NAME];
} PHONEOM_CALL, *PPHONEOM_CALL;

#define PHONEOM_MESSAGE_INCOMING    0x00000001
#define PHONEOM_MESSAGE_READ        0x00000002
#define PHONEOM_MESSAGE_FAILED      0x00000004
#define PHONEOM_MESSAGE_SENT        0x00000008

typedef struct _PHONEOM_MESSAGE
{
    DWORD MessageId;
    DWORD Flags;
    FILETIME Timestamp;
    WCHAR Address[PHONEOM_MAX_NUMBER];
    WCHAR Text[PHONEOM_MAX_TEXT];
} PHONEOM_MESSAGE, *PPHONEOM_MESSAGE;

typedef struct _PHONEOM_RADIO
{
    BOOL Present;
    BOOL PoweredOn;
    DWORD Reserved[6];
} PHONEOM_RADIO, *PPHONEOM_RADIO;

typedef enum _PHONEOM_AUDIO_ROUTE
{
    PhoneAudioEarpiece = 0,
    PhoneAudioSpeaker,
    PhoneAudioHeadset
} PHONEOM_AUDIO_ROUTE;

typedef enum _PHONEOM_EVENT
{
    /* Data is PHONEOM_RADIO */
    PhoneEventRadio = 1,
    /* Data is PHONEOM_CALL */
    PhoneEventCall,
    /* Data is PHONEOM_MESSAGE */
    PhoneEventMessage,
    /* Data is PHONEOM_MESSAGE with PHONEOM_MESSAGE_SENT or PHONEOM_MESSAGE_FAILED */
    PhoneEventMessageStatus,
    /* The service went away, the session must be reopened */
    PhoneEventDisconnected
} PHONEOM_EVENT;

typedef VOID
(CALLBACK *PPHONEOM_EVENT_CALLBACK)(
    _In_ HPHONEOM Phone,
    _In_ DWORD Event,
    _In_reads_bytes_opt_(DataSize) const VOID *Data,
    _In_ DWORD DataSize,
    _In_opt_ PVOID Context);

HRESULT
WINAPI
PhoneOmOpen(
    _In_opt_ PPHONEOM_EVENT_CALLBACK Callback,
    _In_opt_ PVOID Context,
    _Out_ HPHONEOM *Phone);

/* Must not be called from the event callback of the same session */
VOID
WINAPI
PhoneOmClose(
    _In_ HPHONEOM Phone);

HRESULT
WINAPI
PhoneOmGetRadio(
    _In_ HPHONEOM Phone,
    _Out_ PPHONEOM_RADIO Radio);

HRESULT
WINAPI
PhoneOmSetRadioPower(
    _In_ HPHONEOM Phone,
    _In_ BOOL On);

HRESULT
WINAPI
PhoneOmDial(
    _In_ HPHONEOM Phone,
    _In_ LPCWSTR Number,
    _Out_opt_ PDWORD CallId);

HRESULT
WINAPI
PhoneOmAnswer(
    _In_ HPHONEOM Phone,
    _In_ DWORD CallId);

/* CallId 0 releases every call */
HRESULT
WINAPI
PhoneOmHangUp(
    _In_ HPHONEOM Phone,
    _In_ DWORD CallId);

HRESULT
WINAPI
PhoneOmSwap(
    _In_ HPHONEOM Phone);

HRESULT
WINAPI
PhoneOmSendDtmf(
    _In_ HPHONEOM Phone,
    _In_ WCHAR Digit);

HRESULT
WINAPI
PhoneOmGetCalls(
    _In_ HPHONEOM Phone,
    _Out_writes_to_(MaxCalls, *Count) PPHONEOM_CALL Calls,
    _In_ DWORD MaxCalls,
    _Out_ PDWORD Count);

HRESULT
WINAPI
PhoneOmSetAudioRoute(
    _In_ HPHONEOM Phone,
    _In_ DWORD Route);

HRESULT
WINAPI
PhoneOmSetMute(
    _In_ HPHONEOM Phone,
    _In_ BOOL Mute);

HRESULT
WINAPI
PhoneOmSendMessage(
    _In_ HPHONEOM Phone,
    _In_ LPCWSTR Address,
    _In_ LPCWSTR Text,
    _Out_opt_ PDWORD MessageId);

HRESULT
WINAPI
PhoneOmGetMessages(
    _In_ HPHONEOM Phone,
    _In_ DWORD FirstId,
    _Out_writes_to_(MaxMessages, *Count) PPHONEOM_MESSAGE Messages,
    _In_ DWORD MaxMessages,
    _Out_ PDWORD Count);

HRESULT
WINAPI
PhoneOmMarkMessageRead(
    _In_ HPHONEOM Phone,
    _In_ DWORD MessageId);

HRESULT
WINAPI
PhoneOmDeleteMessage(
    _In_ HPHONEOM Phone,
    _In_ DWORD MessageId);

#ifdef __cplusplus
}
#endif
