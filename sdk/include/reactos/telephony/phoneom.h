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
/* Multimedia message, Text holds its subject and text parts */
#define PHONEOM_MESSAGE_MMS         0x00000010
/* PhoneOmGetAttachments lists media files for this message */
#define PHONEOM_MESSAGE_ATTACHMENTS 0x00000020

#define PHONEOM_MAX_ATTACHMENTS     16
#define PHONEOM_MAX_PATH            260

typedef struct _PHONEOM_MESSAGE
{
    DWORD MessageId;
    DWORD Flags;
    FILETIME Timestamp;
    WCHAR Address[PHONEOM_MAX_NUMBER];
    WCHAR Text[PHONEOM_MAX_TEXT];
} PHONEOM_MESSAGE, *PPHONEOM_MESSAGE;

/* One file of a multimedia message, readable by the caller */
typedef struct _PHONEOM_ATTACHMENT
{
    DWORD Size;
    DWORD Reserved;
    WCHAR Path[PHONEOM_MAX_PATH];
} PHONEOM_ATTACHMENT, *PPHONEOM_ATTACHMENT;

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

#define PHONEOM_INFO_STRING         40
#define PHONEOM_INFO_REVISION       80
#define PHONEOM_INFO_OPERATOR       48
#define PHONEOM_APN_LENGTH          64
#define PHONEOM_CREDENTIAL_LENGTH   48

/* PHONEOM_SUBSCRIBER.SimState */
#define PHONEOM_SIM_ABSENT          0
#define PHONEOM_SIM_READY           1
#define PHONEOM_SIM_PIN             2
#define PHONEOM_SIM_PUK             3
#define PHONEOM_SIM_ERROR           4
#define PHONEOM_SIM_NOT_READY       5

/* PHONEOM_NETWORK.RegState */
#define PHONEOM_REG_NONE            0
#define PHONEOM_REG_HOME            1
#define PHONEOM_REG_SEARCHING       2
#define PHONEOM_REG_DENIED          3
#define PHONEOM_REG_UNKNOWN         4
#define PHONEOM_REG_ROAMING         5

/* PHONEOM_NETWORK.Rat */
#define PHONEOM_RAT_NONE            0
#define PHONEOM_RAT_GSM             1
#define PHONEOM_RAT_UMTS            2
#define PHONEOM_RAT_LTE             3
#define PHONEOM_RAT_CDMA            4
#define PHONEOM_RAT_TDSCDMA         5
#define PHONEOM_RAT_EVDO            6

/* PhoneRequestConnectData operation */
#define PHONEOM_DATA_DISCONNECT     0
#define PHONEOM_DATA_CONNECT        1
#define PHONEOM_DATA_QUERY          2

/* PHONEOM_DATA_CONFIG.AuthType */
#define PHONEOM_AUTH_NONE           0
#define PHONEOM_AUTH_PAP            1
#define PHONEOM_AUTH_CHAP           2

typedef struct _PHONEOM_DEVICE_INFO
{
    WCHAR Imei[PHONEOM_INFO_STRING];
    WCHAR Manufacturer[PHONEOM_INFO_STRING];
    WCHAR Model[PHONEOM_INFO_STRING];
    WCHAR Revision[PHONEOM_INFO_REVISION];
} PHONEOM_DEVICE_INFO, *PPHONEOM_DEVICE_INFO;

typedef struct _PHONEOM_SUBSCRIBER
{
    DWORD SimState;
    WCHAR Imsi[PHONEOM_INFO_STRING];
    WCHAR Number[PHONEOM_INFO_STRING];
} PHONEOM_SUBSCRIBER, *PPHONEOM_SUBSCRIBER;

typedef struct _PHONEOM_NETWORK
{
    DWORD RegState;
    DWORD Rat;
    LONG SignalDbm;
    DWORD SignalBars;
    DWORD Mcc;
    DWORD Mnc;
    DWORD Roaming;
    WCHAR Operator[PHONEOM_INFO_OPERATOR];
} PHONEOM_NETWORK, *PPHONEOM_NETWORK;

typedef struct _PHONEOM_DATA_CONFIG
{
    DWORD Enabled;
    DWORD AuthType;
    WCHAR Apn[PHONEOM_APN_LENGTH];
    WCHAR Username[PHONEOM_CREDENTIAL_LENGTH];
    WCHAR Password[PHONEOM_CREDENTIAL_LENGTH];
} PHONEOM_DATA_CONFIG, *PPHONEOM_DATA_CONFIG;

typedef struct _PHONEOM_DATA_STATUS
{
    DWORD Connected;
    DWORD Reserved;
} PHONEOM_DATA_STATUS, *PPHONEOM_DATA_STATUS;

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

HRESULT
WINAPI
PhoneOmGetAttachments(
    _In_ HPHONEOM Phone,
    _In_ DWORD MessageId,
    _Out_writes_to_(MaxAttachments, *Count) PPHONEOM_ATTACHMENT Attachments,
    _In_ DWORD MaxAttachments,
    _Out_ PDWORD Count);

/* Queues a multimedia message, the status event reports the outcome */
HRESULT
WINAPI
PhoneOmSendMms(
    _In_ HPHONEOM Phone,
    _In_ LPCWSTR Address,
    _In_opt_ LPCWSTR Text,
    _In_opt_ LPCWSTR AttachmentPath,
    _Out_opt_ PDWORD MessageId);

HRESULT
WINAPI
PhoneOmGetDeviceInfo(
    _In_ HPHONEOM Phone,
    _Out_ PPHONEOM_DEVICE_INFO Info);

HRESULT
WINAPI
PhoneOmGetSubscriber(
    _In_ HPHONEOM Phone,
    _Out_ PPHONEOM_SUBSCRIBER Subscriber);

HRESULT
WINAPI
PhoneOmGetNetwork(
    _In_ HPHONEOM Phone,
    _Out_ PPHONEOM_NETWORK Network);

HRESULT
WINAPI
PhoneOmGetDataConfig(
    _In_ HPHONEOM Phone,
    _Out_ PPHONEOM_DATA_CONFIG Config);

HRESULT
WINAPI
PhoneOmSetDataConfig(
    _In_ HPHONEOM Phone,
    _In_ const PHONEOM_DATA_CONFIG *Config);

HRESULT
WINAPI
PhoneOmConnectData(
    _In_ HPHONEOM Phone,
    _In_ BOOL Connect,
    _Out_opt_ PPHONEOM_DATA_STATUS Status);

HRESULT
WINAPI
PhoneOmGetDataStatus(
    _In_ HPHONEOM Phone,
    _Out_ PPHONEOM_DATA_STATUS Status);

#ifdef __cplusplus
}
#endif
