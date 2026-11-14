/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     OEM RIL library private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winnt.h>
#include <stdlib.h>
#include <ndk/rtlfuncs.h>

#include <qcom/qmi.h>
#include <telephony/rilcmd.h>
#include <telephony/rilwire.h>
#include <telephony/rilinfo.h>

/* Port address on the IPC router, the pair every message is addressed by */
typedef struct _QC_ADDR
{
    ULONG Node;
    ULONG Port;
} QC_ADDR, *PQC_ADDR;

/* The QMI services this library talks to, indexes into the modem service table */
typedef enum _QC_SVC
{
    QcSvcDms = 0,
    QcSvcNas,
    QcSvcUim,
    QcSvcVoice,
    QcSvcWms,
    QcSvcWds,
    QcSvcCount
} QC_SVC;

/* Wire service ids on the router, matching the modem qrtr names */
#define QC_SERVICE_WDS              1
#define QC_SERVICE_DMS              2
#define QC_SERVICE_NAS              3
#define QC_SERVICE_WMS              5
#define QC_SERVICE_VOICE            9
#define QC_SERVICE_UIM              0x0B

#define QC_INSTANCE_DEFAULT         0
#define QC_VERSION_DEFAULT          1

/* The router name packs the QMI instance and version, version in the low byte,
 * exactly as the modem announces its services. */
#define QC_ROUTER_NAME(Instance, Version)   (((Instance) << 8) | ((Version) & 0xFF))

/* Largest QMI message we send or receive */
#define QC_MESSAGE_MAX              2048

/* How long one request waits for its matching response */
#define QC_REQUEST_TIMEOUT_MS       8000

/* Service resolve retries while the modem comes up */
#define QC_RESOLVE_RETRY            60
#define QC_RESOLVE_DELAY_MS         500

/* The modem takes much longer to finish init and register telephony */
#define QC_SETUP_RETRY              240

typedef struct _QC_MODEM QC_MODEM, *PQC_MODEM;

/* One service endpoint on the modem */
typedef struct _QC_SERVICE
{
    ULONG ServiceId;
    ULONG Instance;
    QC_ADDR Addr;
    BOOL Ready;
} QC_SERVICE, *PQC_SERVICE;

/* A request waiting for its response, matched by server address and transaction */
typedef struct _QC_PENDING
{
    LIST_ENTRY Link;
    QC_ADDR Server;
    USHORT Transaction;
    USHORT MessageId;
    HANDLE Done;
    PUCHAR Response;
    ULONG ResponseCap;
    ULONG ResponseLen;
    BOOL Got;
} QC_PENDING, *PQC_PENDING;

/* Growable response builder handed to each command handler */
typedef struct _QC_RESP
{
    PUCHAR Data;
    ULONG Length;
    ULONG Capacity;
    BOOL Failed;
} QC_RESP, *PQC_RESP;

/* One queued notification or command answer for a client */
typedef struct _QC_QITEM
{
    LIST_ENTRY Link;
    DWORD Code;        /* RIL_RESULT_OK/ERROR for answers, RIL_NOTIFY_* for events */
    DWORD CommandId;   /* host token for answers, zero for events */
    DWORD DataSize;
    BYTE Data[1];
} QC_QITEM, *PQC_QITEM;

/* A client opened through RIL_Open */
typedef struct _QC_CLIENT
{
    LIST_ENTRY Link;
    PQC_MODEM Modem;
    DWORD Id;
    HANDLE NotifyEvent;
    HANDLE CancelEvent;
    CRITICAL_SECTION Lock;
    LIST_ENTRY Queue;
    BOOL Open;
    volatile LONG RefCount;
} QC_CLIENT, *PQC_CLIENT;

/* One command handed to the worker thread */
typedef struct _QC_WORK
{
    LIST_ENTRY Link;
    PQC_CLIENT Client;
    DWORD CommandId;
    DWORD Command;
    DWORD InSize;
    BYTE In[1];
} QC_WORK, *PQC_WORK;

struct _QC_MODEM
{
    LONG RefCount;
    HANDLE Ipc;
    QC_ADDR Self;

    QC_SERVICE Services[QcSvcCount];
    CRITICAL_SECTION ServiceLock;

    /* Request and response matching over the single router port */
    CRITICAL_SECTION SendLock;
    USHORT NextTransaction;
    CRITICAL_SECTION PendingLock;
    LIST_ENTRY Pending;

    /* Receive thread, demultiplexes responses and indications */
    HANDLE RxThread;
    volatile LONG Stopping;

    /* Command worker and the service bring-up thread */
    HANDLE Worker;
    HANDLE Setup;
    HANDLE WorkSignal;
    CRITICAL_SECTION WorkLock;
    LIST_ENTRY WorkQueue;

    /* Open clients */
    CRITICAL_SECTION ClientLock;
    LIST_ENTRY Clients;
    DWORD NextClientId;

    /* Voice call snapshot for edge detection, guarded by ServiceLock */
    BYTE CallSeen[64];
};

/* ril.c */
VOID QcTrace(_In_ PCSTR Format, ...);
VOID QcNotifyAll(_In_ PQC_MODEM Modem, _In_ DWORD Code,
                 _In_reads_bytes_opt_(Size) const VOID *Data, _In_ DWORD Size);

/* resp.c helpers live in codec.c */
VOID QcRespInit(_Out_ PQC_RESP Resp, _Out_writes_bytes_(Cap) PVOID Buffer, _In_ ULONG Cap);
VOID QcRespBytes(_Inout_ PQC_RESP Resp, _In_reads_bytes_(Len) const VOID *Data, _In_ ULONG Len);
VOID QcRespDword(_Inout_ PQC_RESP Resp, _In_ DWORD Value);

/* ipc.c */
BOOL QcIpcOpen(_In_ PQC_MODEM Modem);
BOOL QcIpcStartReceive(_In_ PQC_MODEM Modem);
VOID QcIpcClose(_In_ PQC_MODEM Modem);
BOOL QcServiceEnsure(_In_ PQC_MODEM Modem, _In_ QC_SVC Which);

/*
 * Sends one request to the service and copies the full response message,
 * header included, into Response. Message is a complete QMI message built with
 * QmiEncodeBegin(QMI_TYPE_REQUEST, 0, id) and QmiEncodeEnd; the transaction is
 * filled in here. Returns the QMI result as an HRESULT.
 */
HRESULT QcQmiRequest(
    _In_ PQC_MODEM Modem,
    _In_ QC_SVC Which,
    _Inout_updates_bytes_(MessageLength) PVOID Message,
    _In_ ULONG MessageLength,
    _Out_writes_bytes_to_(ResponseCap, *ResponseLen) PVOID Response,
    _In_ ULONG ResponseCap,
    _Out_ PULONG ResponseLen);

/* Sends a request and only checks the QMI result, discarding the payload */
HRESULT QcQmiCommand(
    _In_ PQC_MODEM Modem,
    _In_ QC_SVC Which,
    _Inout_updates_bytes_(MessageLength) PVOID Message,
    _In_ ULONG MessageLength);

/* Indication arrived from a modem service, dispatched by the receive thread */
VOID QcIndication(
    _In_ PQC_MODEM Modem,
    _In_ QC_SVC Which,
    _In_ USHORT MessageId,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length);

/* dms.c */
HRESULT QcDmsSetRadioPower(_In_ PQC_MODEM Modem, _In_ BOOL On);
HRESULT QcDmsGetDeviceInfo(_In_ PQC_MODEM Modem, _Inout_ PQC_RESP Resp);
HRESULT QcDmsReadString(_In_ PQC_MODEM Modem, _In_ USHORT MessageId, _In_ UCHAR Tlv,
                        _Out_writes_z_(OutSize) PSTR Out, _In_ ULONG OutSize);

/* nas.c */
HRESULT QcNasGetNetwork(_In_ PQC_MODEM Modem, _Inout_ PQC_RESP Resp);
VOID QcNasEnableIndications(_In_ PQC_MODEM Modem);

/* uim.c */
HRESULT QcUimGetSubscriber(_In_ PQC_MODEM Modem, _Inout_ PQC_RESP Resp);
BOOL QcUimSimState(_In_ PQC_MODEM Modem, _Out_ PDWORD State);

/* wds.c */
HRESULT QcWdsData(_In_ PQC_MODEM Modem, _In_reads_bytes_(InSize) const VOID *In,
                  _In_ ULONG InSize, _Inout_ PQC_RESP Resp);

/* voice.c */
HRESULT QcVoiceDial(_In_ PQC_MODEM Modem, _In_reads_bytes_(InSize) const VOID *In,
                    _In_ ULONG InSize, _Inout_ PQC_RESP Resp);
HRESULT QcVoiceManage(_In_ PQC_MODEM Modem, _In_reads_bytes_(InSize) const VOID *In,
                      _In_ ULONG InSize);
VOID QcVoiceIndication(_In_ PQC_MODEM Modem, _In_ USHORT MessageId,
                       _In_reads_bytes_(Length) const VOID *Message, _In_ ULONG Length);
VOID QcVoiceSeed(_In_ PQC_MODEM Modem);

/* wms.c */
HRESULT QcWmsSend(_In_ PQC_MODEM Modem, _In_reads_bytes_(InSize) const VOID *In,
                  _In_ ULONG InSize, _Inout_ PQC_RESP Resp);
VOID QcWmsConfigureRoutes(_In_ PQC_MODEM Modem);
VOID QcWmsIndication(_In_ PQC_MODEM Modem, _In_ USHORT MessageId,
                     _In_reads_bytes_(Length) const VOID *Message, _In_ ULONG Length);
