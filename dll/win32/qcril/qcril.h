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
    QcSvcPdc,
    QcSvcImss,
    QcSvcImsa,
    QcSvcCount
} QC_SVC;

/* The services a modem restart is detected by, the rest come and go on their own */
#define QC_SVC_IS_CORE(Which)       ((Which) < QcSvcWds)

/* Present only when the carrier configuration enables them, set up once announced */
#define QC_SVC_IS_OPTIONAL(Which)   ((Which) == QcSvcImss || (Which) == QcSvcImsa)

/* Wire service ids on the router, matching the modem qrtr names */
#define QC_SERVICE_WDS              1
#define QC_SERVICE_DMS              2
#define QC_SERVICE_NAS              3
#define QC_SERVICE_WMS              5
#define QC_SERVICE_VOICE            9
#define QC_SERVICE_UIM              0x0B
#define QC_SERVICE_PDC              0x24
#define QC_SERVICE_IMSS             0x12
#define QC_SERVICE_IMSP             0x1F
#define QC_SERVICE_IMSA             0x21

#define QC_INSTANCE_DEFAULT         0

/* The router name packs the QMI instance and version, version in the low byte,
 * exactly as the modem announces its services. */
#define QC_ROUTER_NAME(Instance, Version)   (((Instance) << 8) | ((Version) & 0xFF))

/* Largest QMI message we send or receive */
#define QC_MESSAGE_MAX              2048

/* How long one request waits for its matching response */
#define QC_REQUEST_TIMEOUT_MS       15000

/* A dial is answered once the network takes the origination, which can run
   well past a normal request. It stays under the 60 s the phone service waits */
#define QC_CALL_SETUP_TIMEOUT_MS    45000

/* How long an equipment state change waits for the modem to report it */
#define QC_EQSTATE_TIMEOUT_MS       15000

/* Attempts, one per setup pass, to put the equipment state back after a restart */
#define QC_RESTORE_RETRY            20

/* Service resolve retries while the router comes up */
#define QC_RESOLVE_RETRY            60
#define QC_RESOLVE_DELAY_MS         500

/* The phone service drives a single executor */
#define QC_EXECUTOR                 0

/* The QMI error codes this library acts on */
#define QC_QMI_ERR_NONE                 0x0000
#define QC_QMI_ERR_NO_EFFECT            0x001A
#define QC_QMI_ERR_DEVICE_NOT_READY     0x0034

typedef struct _QC_MODEM QC_MODEM, *PQC_MODEM;

/* One service endpoint on the modem */
typedef struct _QC_SERVICE
{
    ULONG ServiceId;
    ULONG Instance;
    QC_ADDR Addr;
    BOOL Ready;
    BOOL Configured;
    BOOL Announced;         /* the router reported the server, cleared when it goes */
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
    BOOL Aborted;
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

/* Device management state, guarded by StateLock */
typedef struct _QC_DMS_STATE
{
    DWORD Equipment;        /* last equipment state reported to clients */
    DWORD Requested;        /* last state a client asked for, restored after a modem restart */
    BOOL Shutdown;          /* set by RIL_EQSTATE_SHUTDOWN, the modem is no longer driven */
    HANDLE ChangeEvent;     /* signaled when the event report confirms a change */
    DWORD Awaiting;         /* state SetEquipmentState waits for, zero when nobody waits */
    DWORD Reported;         /* state the event report confirmed for the waiter */
    ULONG RestoreTries;     /* attempts left to put the requested state back after a restart */
} QC_DMS_STATE;

/* Network state cache, served to clients and compared before notifying */
#define QC_NAS_MAX_SIGNAL           2

typedef struct _QC_NAS_STATE
{
    RIL_NETWORK_INFO Net;
    RIL_SIGNAL_QUALITY Signal[QC_NAS_MAX_SIGNAL];
    ULONG SignalCount;
    DWORD SystemTypes;      /* RIL_RAT_* bits of the RATs in service */
} QC_NAS_STATE;

/* UIM card and application state, from the card status */
#define QC_UIM_MAX_CARDS            2
#define QC_UIM_MAX_APPS             8
#define QC_UIM_MAX_AID              32

typedef struct _QC_UIM_APP
{
    UCHAR Type;
    UCHAR State;
    UCHAR PersoState;
    UCHAR Pin1State;
    UCHAR Pin1Retries;
    UCHAR Puk1Retries;
    UCHAR AidLength;
    UCHAR Aid[QC_UIM_MAX_AID];
} QC_UIM_APP, *PQC_UIM_APP;

typedef struct _QC_UIM_CARD
{
    UCHAR CardState;
    UCHAR ErrorCode;
    UCHAR AppCount;
    QC_UIM_APP Apps[QC_UIM_MAX_APPS];
} QC_UIM_CARD, *PQC_UIM_CARD;

typedef struct _QC_UIM_STATE
{
    BOOL Valid;
    USHORT GwPrimary;       /* slot in the high byte, application in the low byte */
    UCHAR CardCount;
    QC_UIM_CARD Cards[QC_UIM_MAX_CARDS];
    DWORD SlotState[QC_UIM_MAX_CARDS];  /* last slot state reported to clients */
    BOOL ConfigRead;
    BOOL AutoSelect;        /* the modem picks the subscription itself */
    BOOL ProvisionSent;
    BOOL Recheck;           /* the setup thread should evaluate provisioning */
} QC_UIM_STATE;

/* Persistent device config requests, answered through token matched indications */
#define QC_PDC_MAX_ID               32
#define QC_PDC_MAX_CONFIGS          16

typedef struct _QC_PDC_STATE
{
    HANDLE Event;           /* set when the waited indication lands in Ind */
    USHORT WaitId;          /* indication message id the caller waits for, zero for none */
    ULONG WaitToken;
    ULONG NextToken;
    BYTE Ind[QC_MESSAGE_MAX];
    ULONG IndLen;
    BOOL Switched;          /* a configuration was activated during this run */
    BOOL Done;              /* the card was read and the configuration handled, until a modem restart */
} QC_PDC_STATE;

/* IMS registration as the IMS application service last reported it */
typedef struct _QC_IMS_STATE
{
    ULONG Registration;     /* 0 not registered, 1 registering, 2 registered, 3 limited */
    ULONG Voice;            /* 0 unavailable, 1 limited, 2 available */
    BOOL PdnPending;        /* the IMS PDN waits for the data service to come up */
} QC_IMS_STATE;

/* RIL call id to modem call id, the modem reuses its ids */
#define QC_VOICE_MAX_CALLS          8

typedef struct _QC_CALL
{
    BOOL Used;
    UCHAR ModemId;
    UCHAR QmiState;
    DWORD RilId;
    DWORD Status;
    DWORD Direction;
} QC_CALL, *PQC_CALL;

struct _QC_MODEM
{
    LONG RefCount;
    HANDLE Ipc;

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

    /* Router event thread, flow control and server arrival or removal */
    HANDLE EventThread;
    HANDLE TxResume;

    /* Set while the modem services are up, cleared on a modem restart */
    volatile LONG ModemUp;
    volatile LONG Restarted;

    /* Command worker and the service bring-up thread */
    HANDLE Worker;
    HANDLE Setup;
    HANDLE SetupSignal;
    HANDLE WorkSignal;
    CRITICAL_SECTION WorkLock;
    LIST_ENTRY WorkQueue;

    /* Open clients */
    CRITICAL_SECTION ClientLock;
    LIST_ENTRY Clients;
    DWORD NextClientId;

    /* Service state caches */
    CRITICAL_SECTION StateLock;
    QC_DMS_STATE Dms;
    QC_NAS_STATE Nas;
    QC_UIM_STATE Uim;
    QC_PDC_STATE Pdc;
    QC_IMS_STATE Ims;
    QC_CALL Calls[QC_VOICE_MAX_CALLS];
    DWORD NextCallId;
};

/* ril.c */
VOID QcTrace(_In_ PCSTR Format, ...);
VOID QcNotifyAll(_In_ PQC_MODEM Modem, _In_ DWORD Code,
                 _In_reads_bytes_opt_(Size) const VOID *Data, _In_ DWORD Size);
VOID QcKickSetup(_In_ PQC_MODEM Modem);

/* resp.c helpers live in codec.c */
VOID QcRespInit(_Out_ PQC_RESP Resp, _Out_writes_bytes_(Cap) PVOID Buffer, _In_ ULONG Cap);
VOID QcRespBytes(_Inout_ PQC_RESP Resp, _In_reads_bytes_(Len) const VOID *Data, _In_ ULONG Len);
VOID QcRespDword(_Inout_ PQC_RESP Resp, _In_ DWORD Value);
VOID QcRespWide(_Inout_ PQC_RESP Resp, _In_z_ PCSTR Text);

/* ipc.c */
BOOL QcIpcOpen(_In_ PQC_MODEM Modem);
BOOL QcIpcStartReceive(_In_ PQC_MODEM Modem);
VOID QcIpcClose(_In_ PQC_MODEM Modem);
BOOL QcIpcOpenPort(_Out_ PHANDLE Port);
VOID QcIpcClosePort(_In_ HANDLE Port);
BOOL QcIpcRegisterServer(_In_ HANDLE Port, _In_ ULONG ServiceId, _In_ ULONG Instance);
BOOL QcIpcSendOn(_In_ HANDLE Port, _In_ const QC_ADDR *Dest,
                 _In_reads_bytes_(Length) const VOID *Message, _In_ ULONG Length);
BOOL QcServiceEnsure(_In_ PQC_MODEM Modem, _In_ QC_SVC Which);
PCSTR QcServiceName(_In_ QC_SVC Which);
HRESULT QcMapQmiError(_In_ USHORT Error);

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

/* QcQmiRequest with its own wait for the response, in milliseconds */
HRESULT QcQmiRequestWait(
    _In_ PQC_MODEM Modem,
    _In_ QC_SVC Which,
    _Inout_updates_bytes_(MessageLength) PVOID Message,
    _In_ ULONG MessageLength,
    _Out_writes_bytes_to_(ResponseCap, *ResponseLen) PVOID Response,
    _In_ ULONG ResponseCap,
    _Out_ PULONG ResponseLen,
    _In_ DWORD TimeoutMs);

/* Sends a request and only checks the QMI result, discarding the payload */
HRESULT QcQmiCommand(
    _In_ PQC_MODEM Modem,
    _In_ QC_SVC Which,
    _Inout_updates_bytes_(MessageLength) PVOID Message,
    _In_ ULONG MessageLength);

/* Raw QMI error of a response, QC_QMI_ERR_NONE when it succeeded or has no result */
USHORT QcQmiError(_In_reads_bytes_(Length) const VOID *Response, _In_ ULONG Length);

/* Indication arrived from a modem service, dispatched by the receive thread */
VOID QcIndication(
    _In_ PQC_MODEM Modem,
    _In_ QC_SVC Which,
    _In_ USHORT MessageId,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length);

/* dms.c */
VOID QcDmsSetup(_In_ PQC_MODEM Modem);
VOID QcDmsModemLost(_In_ PQC_MODEM Modem);
BOOL QcDmsRestore(_In_ PQC_MODEM Modem);
HRESULT QcDmsSetEquipmentState(_In_ PQC_MODEM Modem, _In_ DWORD State);
HRESULT QcDmsGetEquipmentState(_In_ PQC_MODEM Modem, _Inout_ PQC_RESP Resp);
HRESULT QcDmsGetDeviceInfo(_In_ PQC_MODEM Modem, _In_reads_bytes_(InSize) const VOID *In,
                           _In_ ULONG InSize, _Inout_ PQC_RESP Resp);
HRESULT QcDmsReadString(_In_ PQC_MODEM Modem, _In_ USHORT MessageId, _In_ UCHAR Tlv,
                        _Out_writes_z_(OutSize) PSTR Out, _In_ ULONG OutSize);
VOID QcDmsIndication(_In_ PQC_MODEM Modem, _In_ USHORT MessageId,
                     _In_reads_bytes_(Length) const VOID *Message, _In_ ULONG Length);

/* ims.c */
VOID QcImssSetup(_In_ PQC_MODEM Modem);
VOID QcImsaSetup(_In_ PQC_MODEM Modem);
VOID QcImsModemLost(_In_ PQC_MODEM Modem);
VOID QcImsStartPdn(_In_ PQC_MODEM Modem);

/* imsdata.c */
VOID QcImsDataInitialize(VOID);
HRESULT QcImsDataConnect(_In_ PQC_MODEM Modem, _In_ UCHAR Profile, _In_ UCHAR Family,
                         _Out_writes_z_(AddressSize) PSTR Address, _In_ ULONG AddressSize);
VOID QcImsDataDisconnect(VOID);
HRESULT QcImsDataAddress(_In_ UCHAR Family, _Out_writes_z_(Size) PSTR Address, _In_ ULONG Size);
VOID QcImsDataModemLost(VOID);
BOOL QcWdsDataPath(_Out_ PULONG EndpointType, _Out_ PULONG InterfaceId, _Out_ PULONG MuxId);
VOID QcImsaIndication(_In_ PQC_MODEM Modem, _In_ USHORT MessageId,
                      _In_reads_bytes_(Length) const VOID *Message, _In_ ULONG Length);

/* dcm.c */
VOID QcDcmInitialize(VOID);
VOID QcDcmStart(_In_ PQC_MODEM Modem, _In_ UCHAR FallbackProfile);
VOID QcDcmPdnLost(VOID);
VOID QcDcmModemLost(VOID);

/* pdc.c */
VOID QcPdcSetup(_In_ PQC_MODEM Modem);
VOID QcPdcIndication(_In_ PQC_MODEM Modem, _In_ USHORT MessageId,
                     _In_reads_bytes_(Length) const VOID *Message, _In_ ULONG Length);

/* nas.c */
VOID QcNasSetup(_In_ PQC_MODEM Modem);
VOID QcNasModemLost(_In_ PQC_MODEM Modem);
HRESULT QcNasGetRegStatus(_In_ PQC_MODEM Modem, _Inout_ PQC_RESP Resp);
HRESULT QcNasGetSignalQuality(_In_ PQC_MODEM Modem, _Inout_ PQC_RESP Resp);
VOID QcNasIndication(_In_ PQC_MODEM Modem, _In_ USHORT MessageId,
                     _In_reads_bytes_(Length) const VOID *Message, _In_ ULONG Length);

/* uim.c */
VOID QcUimSetup(_In_ PQC_MODEM Modem);
VOID QcUimModemLost(_In_ PQC_MODEM Modem);
VOID QcUimEvaluate(_In_ PQC_MODEM Modem);
HRESULT QcUimGetSubscriber(_In_ PQC_MODEM Modem, _Inout_ PQC_RESP Resp);
HRESULT QcUimReadImsi(_In_ PQC_MODEM Modem, _Out_writes_z_(OutSize) PSTR Out, _In_ ULONG OutSize);
HRESULT QcUimSetExecutorConfig(_In_ PQC_MODEM Modem, _In_reads_bytes_(InSize) const VOID *In,
                               _In_ ULONG InSize);
VOID QcUimIndication(_In_ PQC_MODEM Modem, _In_ USHORT MessageId,
                     _In_reads_bytes_(Length) const VOID *Message, _In_ ULONG Length);

/* wds.c */
VOID QcWdsReset(VOID);
VOID QcWdsModemLost(_In_ PQC_MODEM Modem);
HRESULT QcWdsData(_In_ PQC_MODEM Modem, _In_reads_bytes_(InSize) const VOID *In,
                  _In_ ULONG InSize, _Inout_ PQC_RESP Resp);

/* voice.c */
VOID QcVoiceSetup(_In_ PQC_MODEM Modem);
VOID QcVoiceModemLost(_In_ PQC_MODEM Modem);
HRESULT QcVoiceDial(_In_ PQC_MODEM Modem, _In_reads_bytes_(InSize) const VOID *In,
                    _In_ ULONG InSize, _Inout_ PQC_RESP Resp);
HRESULT QcVoiceManage(_In_ PQC_MODEM Modem, _In_reads_bytes_(InSize) const VOID *In,
                      _In_ ULONG InSize);
HRESULT QcVoiceDtmf(_In_ PQC_MODEM Modem, _In_ DWORD Command,
                    _In_reads_bytes_(InSize) const VOID *In, _In_ ULONG InSize);
VOID QcVoiceIndication(_In_ PQC_MODEM Modem, _In_ USHORT MessageId,
                       _In_reads_bytes_(Length) const VOID *Message, _In_ ULONG Length);

/* wms.c */
VOID QcWmsSetup(_In_ PQC_MODEM Modem);
HRESULT QcWmsSend(_In_ PQC_MODEM Modem, _In_reads_bytes_(InSize) const VOID *In,
                  _In_ ULONG InSize, _Inout_ PQC_RESP Resp);
HRESULT QcWmsAck(_In_ PQC_MODEM Modem, _In_reads_bytes_(InSize) const VOID *In, _In_ ULONG InSize);
VOID QcWmsIndication(_In_ PQC_MODEM Modem, _In_ USHORT MessageId,
                     _In_reads_bytes_(Length) const VOID *Message, _In_ ULONG Length);
VOID QcWdsIndication(_In_ PQC_MODEM Modem, _In_ USHORT MessageId,
                     _In_reads_bytes_(Length) const VOID *Message, _In_ ULONG Length);
