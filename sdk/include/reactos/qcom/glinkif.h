/*
 * PROJECT:     ReactOS Qualcomm SoC support
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Client table of the platform GLINK driver (qcglink850.sys)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* {DC424AEC-4D0F-4A41-8EA6-B33F4CCADF28} */
DEFINE_GUID(GUID_QCOM_GLINK_INTERFACE,
            0xdc424aec, 0x4d0f, 0x4a41, 0x8e, 0xa6, 0xb3, 0x3f, 0x4c, 0xca, 0xdf, 0x28);

#define QCOM_GLINK_DEVICE_NAME          L"\\Device\\GLINK"
#define QCOM_GLINK_INTERFACE_VERSION    1

/* Remote processor names the driver knows */
#define QCOM_GLINK_REMOTE_MODEM         "mpss"
#define QCOM_GLINK_REMOTE_ADSP          "lpass"
#define QCOM_GLINK_REMOTE_CDSP          "cdsp"

/* Channel state events */
#define QCOM_GLINK_CONNECTED            0
#define QCOM_GLINK_LOCAL_DISCONNECTED   1
#define QCOM_GLINK_REMOTE_DISCONNECTED  2

/* Tx options: ask the remote for an intent when none is queued */
#define QCOM_GLINK_TX_REQUEST_INTENT    0x00000001

typedef PVOID QCOM_GLINK_HANDLE, *PQCOM_GLINK_HANDLE;

typedef VOID
(*PQCOM_GLINK_NOTIFY_RX)(
    _In_ QCOM_GLINK_HANDLE Handle,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID PacketContext,
    _In_reads_bytes_(Size) const VOID *Data,
    _In_ SIZE_T Size,
    _In_ SIZE_T IntentSize);

typedef VOID
(*PQCOM_GLINK_NOTIFY_TX_DONE)(
    _In_ QCOM_GLINK_HANDLE Handle,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID PacketContext,
    _In_ const VOID *Data,
    _In_ SIZE_T Size);

typedef VOID
(*PQCOM_GLINK_NOTIFY_STATE)(
    _In_ QCOM_GLINK_HANDLE Handle,
    _In_opt_ PVOID Context,
    _In_ ULONG Event);

/* Returns TRUE when the client will queue an intent of Size bytes */
typedef BOOLEAN
(*PQCOM_GLINK_NOTIFY_RX_INTENT_REQ)(
    _In_ QCOM_GLINK_HANDLE Handle,
    _In_opt_ PVOID Context,
    _In_ SIZE_T Size);

/* Remote and Name are required, as are NotifyRx and NotifyState */
typedef struct _QCOM_GLINK_OPEN_CONFIG
{
    PCSTR Transport;
    PCSTR Remote;
    PCSTR Name;
    ULONG Options;
    PVOID Context;
    PQCOM_GLINK_NOTIFY_RX NotifyRx;
    PVOID NotifyRxTracer;
    PVOID NotifyRxVector;
    PQCOM_GLINK_NOTIFY_TX_DONE NotifyTxDone;
    PQCOM_GLINK_NOTIFY_STATE NotifyState;
    PQCOM_GLINK_NOTIFY_RX_INTENT_REQ NotifyRxIntentRequest;
    PVOID NotifyRxIntent;
    PVOID NotifyRxSignals;
    PVOID NotifyRxAbort;
    PVOID NotifyTxAbort;
    ULONG Reserved;
    PVOID NotifyAllocate;
    PVOID NotifyDeallocate;
} QCOM_GLINK_OPEN_CONFIG, *PQCOM_GLINK_OPEN_CONFIG;

#ifdef _WIN64
C_ASSERT(sizeof(QCOM_GLINK_OPEN_CONFIG) == 0x90);
#endif

typedef LONG
(*PQCOM_GLINK_OPEN)(
    _In_ PQCOM_GLINK_OPEN_CONFIG Config,
    _Out_ PQCOM_GLINK_HANDLE Handle);

typedef LONG
(*PQCOM_GLINK_CLOSE)(
    _In_ QCOM_GLINK_HANDLE Handle);

/* Data must stay valid until NotifyTxDone reports it */
typedef LONG
(*PQCOM_GLINK_TX)(
    _In_ QCOM_GLINK_HANDLE Handle,
    _In_opt_ PVOID PacketContext,
    _In_reads_bytes_(Size) const VOID *Data,
    _In_ SIZE_T Size,
    _In_ ULONG Options);

typedef LONG
(*PQCOM_GLINK_QUEUE_RX_INTENT)(
    _In_ QCOM_GLINK_HANDLE Handle,
    _In_opt_ PVOID PacketContext,
    _In_ SIZE_T Size);

typedef LONG
(*PQCOM_GLINK_RX_DONE)(
    _In_ QCOM_GLINK_HANDLE Handle,
    _In_ const VOID *Data,
    _In_ BOOLEAN Reuse);

typedef struct _QCOM_GLINK_INTERFACE
{
    INTERFACE Header;
    PVOID RegisterLinkState;
    PVOID DeregisterLinkState;
    PQCOM_GLINK_OPEN Open;
    PQCOM_GLINK_CLOSE Close;
    PQCOM_GLINK_TX Tx;
    PVOID TxVector;
    PQCOM_GLINK_QUEUE_RX_INTENT QueueRxIntent;
    PQCOM_GLINK_RX_DONE RxDone;
    PVOID SignalsSet;
    PVOID SignalsLocalGet;
    PVOID SignalsRemoteGet;
    PVOID Reserved[9];
} QCOM_GLINK_INTERFACE, *PQCOM_GLINK_INTERFACE;

#ifdef _WIN64
C_ASSERT(sizeof(QCOM_GLINK_INTERFACE) == 0xC0);
#endif
