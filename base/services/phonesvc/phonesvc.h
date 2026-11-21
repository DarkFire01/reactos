/*
 * PROJECT:     ReactOS Phone Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#include <winreg.h>
#include <winuser.h>
#include <winsvc.h>
#include <winioctl.h>
#include <setupapi.h>
#include <stdlib.h>
#include <ndk/rtlfuncs.h>
#include <strsafe.h>

#include <telephony/rilcmd.h>
#include <telephony/phoneom.h>
#include <telephony/phonepipe.h>

#define PHONE_SERVICE_NAME          L"PhoneSvc"
#define PHONE_EXECUTOR              0
#define PHONE_RIL_TIMEOUT_MS        60000
#define PHONE_RADIO_ON_ATTEMPTS     60
#define PHONE_RADIO_ON_RETRY_MS     2000
#define PHONE_DIAL_MAX_DIGITS       81

/* main.c */
extern HANDLE PhStopEvent;

/* radio.c */
typedef struct _PH_RIL_RESULT
{
    DWORD ResultType;
    HRESULT Error;
    DWORD DataSize;
    BYTE Data[256];
} PH_RIL_RESULT, *PPH_RIL_RESULT;

typedef struct _PH_REQUEST
{
    LIST_ENTRY Link;
    ULONGLONG Id;
    HANDLE Done;
    PH_RIL_RESULT Result;
} PH_REQUEST, *PPH_REQUEST;

extern HRIL PhRil;

BOOL
PhRadioStart(VOID);

VOID
PhRadioStop(VOID);

PPH_REQUEST
PhBeginRequest(VOID);

HRESULT
PhFinishRequest(
    _In_ PPH_REQUEST Request,
    _In_ HRESULT SendStatus,
    _Out_opt_ PPH_RIL_RESULT Result);

VOID
PhGetRadio(
    _Out_ PPHONEOM_RADIO Radio);

HRESULT
PhSetRadioPower(
    _In_ BOOL On);

/* wake.c */
VOID
PhWakeDevice(
    _In_z_ PCSTR Reason);

/* calls.c */
VOID
PhCallsInitialize(VOID);

BOOL
PhBuildAddress(
    _In_ LPCWSTR Number,
    _Out_ RILADDRESS *Address);

VOID
PhCallsReset(VOID);

VOID
PhCallStateChanged(
    _In_reads_bytes_(Size) const VOID *Data,
    _In_ DWORD Size);

HRESULT
PhDial(
    _In_ LPCWSTR Number,
    _Out_ PDWORD CallId);

HRESULT
PhManageCall(
    _In_ DWORD Action,
    _In_ DWORD CallId);

HRESULT
PhSendDtmf(
    _In_ WCHAR Digit);

DWORD
PhGetCalls(
    _Out_writes_to_(MaxCalls, return) PPHONEOM_CALL Calls,
    _In_ DWORD MaxCalls);

/* sms.c */
VOID
PhSmsInitialize(VOID);

VOID
PhSmsReceived(
    _In_reads_bytes_(Size) const VOID *Data,
    _In_ DWORD Size);

HRESULT
PhSmsSend(
    _In_ LPCWSTR Address,
    _In_ LPCWSTR Text,
    _Out_ PDWORD MessageId);

/* store.c */
BOOL
PhStoreInitialize(VOID);

DWORD
PhStoreAdd(
    _Inout_ PPHONEOM_MESSAGE Message);

BOOL
PhStoreUpdateFlags(
    _In_ DWORD MessageId,
    _In_ DWORD SetFlags,
    _In_ DWORD ClearFlags,
    _Out_opt_ PPHONEOM_MESSAGE Message);

BOOL
PhStoreDelete(
    _In_ DWORD MessageId);

BOOL
PhStoreGetAttachmentFolder(
    _In_ DWORD MessageId,
    _In_ BOOL Create,
    _Out_writes_z_(Count) PWSTR Path,
    _In_ DWORD Count);

VOID
PhStoreRemoveAttachments(
    _In_ DWORD MessageId);

DWORD
PhStoreGetAttachments(
    _In_ DWORD MessageId,
    _Out_writes_to_(MaxAttachments, return) PPHONEOM_ATTACHMENT Attachments,
    _In_ DWORD MaxAttachments);

DWORD
PhStoreEnumerate(
    _In_ DWORD FirstId,
    _Out_writes_to_(MaxMessages, return) PPHONEOM_MESSAGE Messages,
    _In_ DWORD MaxMessages);

/* audio.c */
VOID
PhAudioInitialize(VOID);

VOID
PhAudioUninitialize(VOID);

VOID
PhAudioCallActive(
    _In_ BOOL Active,
    _In_ BOOL PacketSwitched);

HRESULT
PhAudioSetRoute(
    _In_ DWORD Route);

HRESULT
PhAudioSetMute(
    _In_ BOOL Mute);

/* info.c */
#define PH_MMS_URL_CHARS            512
#define PH_MMS_FIELD_CHARS          256

/* MMSC access, read from the data key */
typedef struct _PH_MMS_CONFIG
{
    WCHAR Url[PH_MMS_URL_CHARS];
    WCHAR Proxy[PH_MMS_FIELD_CHARS];
    WCHAR UserAgent[PH_MMS_FIELD_CHARS];
    WCHAR Profile[PH_MMS_FIELD_CHARS];
} PH_MMS_CONFIG, *PPH_MMS_CONFIG;

VOID
PhGetMmsConfig(
    _Out_ PPH_MMS_CONFIG Config);

HRESULT
PhGetDeviceInfo(
    _Out_ PPHONEOM_DEVICE_INFO Info);

HRESULT
PhGetSubscriber(
    _Out_ PPHONEOM_SUBSCRIBER Subscriber);

HRESULT
PhGetNetwork(
    _Out_ PPHONEOM_NETWORK Network);

VOID
PhGetDataConfig(
    _Out_ PPHONEOM_DATA_CONFIG Config);

HRESULT
PhSetDataConfig(
    _In_ const PHONEOM_DATA_CONFIG *Config);

HRESULT
PhConnectData(
    _In_ DWORD Op,
    _Out_ PPHONEOM_DATA_STATUS Status);

/* Bumped when the modem starts a restart, set until the data session is dealt with */
extern volatile LONG PhModemResets;
extern volatile LONG PhModemResetting;

VOID
PhDataModemLost(VOID);

VOID
CALLBACK
PhDataModemBack(
    _Inout_ PTP_CALLBACK_INSTANCE Instance,
    _Inout_opt_ PVOID Context);

/* mms.c */
VOID
PhMmsInitialize(VOID);

VOID
PhMmsStop(VOID);

VOID
PhMmsResume(VOID);

BOOL
PhMmsPushReceived(
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ DWORD Size,
    _In_z_ LPCWSTR Origin,
    _In_ const FILETIME *Received);

HRESULT
PhMmsSend(
    _In_z_ LPCWSTR Address,
    _In_z_ LPCWSTR Text,
    _In_z_ LPCWSTR Path,
    _Out_ PDWORD MessageId);

/* mmsnet.c */
HRESULT
PhMmsHttpRequest(
    _In_ const PH_MMS_CONFIG *Config,
    _In_z_ LPCWSTR Url,
    _In_reads_bytes_opt_(BodySize) const BYTE *Body,
    _In_ DWORD BodySize,
    _Outptr_result_bytebuffer_(*ResponseSize) PBYTE *Response,
    _Out_ PDWORD ResponseSize,
    _Out_ PDWORD HttpStatus);

/* pipe.c */
BOOL
PhPipeStart(VOID);

VOID
PhPipeStop(VOID);

VOID
PhBroadcast(
    _In_ DWORD Event,
    _In_reads_bytes_opt_(Size) const VOID *Data,
    _In_ DWORD Size);
