/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     OEM RIL library host interface, clients and command dispatch
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcril.h"
#include <stdio.h>
#include <stdarg.h>

#define QC_DRIVER_HANDLE            1
#define QC_RESPONSE_MAX             4096

/* Version this library reports through RIL_GetDriverVersion */
#define QC_DRIVER_INTERFACE         2
#define QC_DRIVER_MAJOR             1
#define QC_DRIVER_MINOR             0

static PQC_MODEM gModem;
static CRITICAL_SECTION gInitLock;
static LONG gInitLockReady;

VOID
QcTrace(
    _In_ PCSTR Format,
    ...)
{
    static const CHAR Prefix[] = "[qcril] ";
    CHAR Line[512];
    va_list Args;
    int Length;

    memcpy(Line, Prefix, sizeof(Prefix) - 1);

    va_start(Args, Format);
    Length = _vsnprintf(Line + sizeof(Prefix) - 1, sizeof(Line) - sizeof(Prefix) - 1, Format, Args);
    va_end(Args);

    if (Length < 0)
        Length = sizeof(Line) - sizeof(Prefix) - 1;
    Length += sizeof(Prefix) - 1;

    /* Every line ends in a newline, even one that was cut short */
    if (Length == 0 || Line[Length - 1] != '\n')
        Line[Length++] = '\n';
    Line[Length] = '\0';

    OutputDebugStringA(Line);
}

static
PCSTR
QcCommandName(
    _In_ DWORD Command)
{
    switch (Command)
    {
        case RilCmdEnableNotifications: return "EnableNotifications";
        case RilCmdDisableNotifications: return "DisableNotifications";
        case RilCmdGetDriverVersion: return "GetDriverVersion";
        case RilCmdGetDevCaps: return "GetDevCaps";
        case RilCmdGetDeviceInfo: return "GetDeviceInfo";
        case RilCmdGetEquipmentState: return "GetEquipmentState";
        case RilCmdSetEquipmentState: return "SetEquipmentState";
        case RilCmdEnumerateSlots: return "EnumerateSlots";
        case RilCmdGetCardInfo: return "GetCardInfo";
        case RilCmdSetExecutorConfig: return "SetExecutorConfig";
        case RilCmdGetCurrentRegStatus: return "GetCurrentRegStatus";
        case RilCmdGetSignalQuality: return "GetSignalQuality";
        case RilCmdDial: return "Dial";
        case RilCmdManageCalls: return "ManageCalls";
        case RilCmdSendDtmf: return "SendDtmf";
        case RilCmdStartDtmf: return "StartDtmf";
        case RilCmdStopDtmf: return "StopDtmf";
        case RilCmdSendMsg: return "SendMsg";
        case RilCmdSendMsgAck: return "SendMsgAck";
        case RilCmdDevSpecific: return "DevSpecific";
        case RilCmdSetRfState: return "SetRfState";
        case RilCmdSetExecutorRfState: return "SetExecutorRfState";
        default: return "other";
    }
}

static
VOID
QcEnsureInitLock(VOID)
{
    if (InterlockedCompareExchange(&gInitLockReady, 1, 0) == 0)
    {
        InitializeCriticalSection(&gInitLock);
        QcImsDataInitialize();
        QcDcmInitialize();
    }
}

static
PQC_CLIENT
QcClientFromId(
    _In_ PQC_MODEM Modem,
    _In_ DWORD Id)
{
    PLIST_ENTRY Entry;

    for (Entry = Modem->Clients.Flink; Entry != &Modem->Clients; Entry = Entry->Flink)
    {
        PQC_CLIENT Client = CONTAINING_RECORD(Entry, QC_CLIENT, Link);
        if (Client->Id == Id)
            return Client;
    }

    return NULL;
}

/* Frees a client once its last user drops it, clients outlive RIL_Close */
static
VOID
QcClientRelease(
    _In_ PQC_CLIENT Client)
{
    if (InterlockedDecrement(&Client->RefCount) != 0)
        return;

    while (!IsListEmpty(&Client->Queue))
    {
        PLIST_ENTRY Entry = RemoveHeadList(&Client->Queue);
        HeapFree(GetProcessHeap(), 0, CONTAINING_RECORD(Entry, QC_QITEM, Link));
    }

    if (Client->NotifyEvent)
        CloseHandle(Client->NotifyEvent);
    DeleteCriticalSection(&Client->Lock);
    HeapFree(GetProcessHeap(), 0, Client);
}

/* Looks a client up and takes a reference so it cannot be freed under us */
static
PQC_CLIENT
QcClientAcquire(
    _In_ PQC_MODEM Modem,
    _In_ DWORD Id)
{
    PQC_CLIENT Client;

    EnterCriticalSection(&Modem->ClientLock);
    Client = QcClientFromId(Modem, Id);
    if (Client)
        InterlockedIncrement(&Client->RefCount);
    LeaveCriticalSection(&Modem->ClientLock);

    return Client;
}

static
VOID
QcQueueItem(
    _In_ PQC_CLIENT Client,
    _In_ DWORD Code,
    _In_ DWORD CommandId,
    _In_reads_bytes_opt_(Size) const VOID *Data,
    _In_ DWORD Size)
{
    PQC_QITEM Item;

    Item = HeapAlloc(GetProcessHeap(), 0, FIELD_OFFSET(QC_QITEM, Data) + Size);
    if (!Item)
        return;

    Item->Code = Code;
    Item->CommandId = CommandId;
    Item->DataSize = Size;
    if (Data && Size)
        memcpy(Item->Data, Data, Size);

    EnterCriticalSection(&Client->Lock);
    InsertTailList(&Client->Queue, &Item->Link);
    if (Client->NotifyEvent)
        SetEvent(Client->NotifyEvent);
    LeaveCriticalSection(&Client->Lock);
}

VOID
QcNotifyAll(
    _In_ PQC_MODEM Modem,
    _In_ DWORD Code,
    _In_reads_bytes_opt_(Size) const VOID *Data,
    _In_ DWORD Size)
{
    PLIST_ENTRY Entry;
    ULONG Count = 0;

    EnterCriticalSection(&Modem->ClientLock);
    for (Entry = Modem->Clients.Flink; Entry != &Modem->Clients; Entry = Entry->Flink)
    {
        PQC_CLIENT Client = CONTAINING_RECORD(Entry, QC_CLIENT, Link);
        if (Client->Open)
        {
            QcQueueItem(Client, Code, 0, Data, Size);
            Count++;
        }
    }
    LeaveCriticalSection(&Modem->ClientLock);

    QcTrace("notify 0x%08lx size %lu to %lu client(s)\n", Code, Size, Count);
}

/* Wakes the setup thread to bring services up or reevaluate the card */
VOID
QcKickSetup(
    _In_ PQC_MODEM Modem)
{
    if (Modem->SetupSignal)
        SetEvent(Modem->SetupSignal);
}

/* In: minimum and maximum interface version, the interface in the high word */
static
HRESULT
QcGetDriverVersion(
    _In_reads_bytes_(InSize) const VOID *In,
    _In_ ULONG InSize,
    _Inout_ PQC_RESP Resp)
{
    const DWORD *Range = In;
    DWORD Version;

    if (InSize < 2 * sizeof(DWORD))
        return E_INVALIDARG;

    if (QC_DRIVER_INTERFACE < HIWORD(Range[0]) || QC_DRIVER_INTERFACE > HIWORD(Range[1]))
    {
        QcTrace("driver version: interface %u outside 0x%08lx..0x%08lx\n", QC_DRIVER_INTERFACE, Range[0], Range[1]);
        return RIL_ERROR(0x65);
    }

    Version = ((((QC_DRIVER_INTERFACE << 4) | (QC_DRIVER_MAJOR & 0xF)) << 12) | 0x50 | (QC_DRIVER_MINOR & 0xF));
    QcRespDword(Resp, Version);
    return S_OK;
}

/* Maps one RIL command to the QMI services, filling Resp on success */
static
HRESULT
QcDispatch(
    _In_ PQC_MODEM Modem,
    _In_ DWORD Command,
    _In_reads_bytes_(InSize) const VOID *In,
    _In_ ULONG InSize,
    _Inout_ PQC_RESP Resp)
{
    switch (Command)
    {
        case RilCmdSetEquipmentState:
            if (InSize < sizeof(DWORD))
                return E_INVALIDARG;
            return QcDmsSetEquipmentState(Modem, *(const DWORD *)In);

        case RilCmdGetEquipmentState:
            return QcDmsGetEquipmentState(Modem, Resp);

        case RilCmdGetDeviceInfo:
            return QcDmsGetDeviceInfo(Modem, In, InSize, Resp);

        case RilCmdGetDriverVersion:
            return QcGetDriverVersion(In, InSize, Resp);

        case RilCmdGetSignalQuality:
            return QcNasGetSignalQuality(Modem, Resp);

        case RilCmdGetCurrentRegStatus:
            return QcNasGetRegStatus(Modem, Resp);

        case RilCmdGetCardInfo:
        case RilCmdEnumerateSlots:
            return QcUimGetSubscriber(Modem, Resp);

        case RilCmdSetExecutorConfig:
            return QcUimSetExecutorConfig(Modem, In, InSize);

        case RilCmdDevSpecific:
            return QcWdsData(Modem, In, InSize, Resp);

        case RilCmdDial:
            return QcVoiceDial(Modem, In, InSize, Resp);

        case RilCmdManageCalls:
            return QcVoiceManage(Modem, In, InSize);

        case RilCmdSendDtmf:
        case RilCmdStartDtmf:
        case RilCmdStopDtmf:
            return QcVoiceDtmf(Modem, Command, In, InSize);

        case RilCmdSendMsg:
            return QcWmsSend(Modem, In, InSize, Resp);

        case RilCmdSendMsgAck:
            return QcWmsAck(Modem, In, InSize);

        /* Accepted without modem traffic during bring-up */
        case RilCmdSetExecutorRfState:
        case RilCmdSetRfState:
        case RilCmdEnableModemFilters:
        case RilCmdDisableModemFilters:
        case RilCmdStartModemLogs:
        case RilCmdStopModemLogs:
        case RilCmdDrainModemLogs:
            return S_OK;

        default:
            return RIL_E_OPNOTSUPPORTED;
    }
}

static
VOID
QcComplete(
    _In_ PQC_CLIENT Client,
    _In_ DWORD CommandId,
    _In_ HRESULT hr,
    _In_ PQC_RESP Resp)
{
    if (SUCCEEDED(hr) && !Resp->Failed)
    {
        QcQueueItem(Client, RIL_RESULT_OK, CommandId, Resp->Length ? Resp->Data : NULL, Resp->Length);
    }
    else
    {
        RIL_ERROR_RESULT Error;

        Error.Size = sizeof(Error);
        Error.Params = 0;
        Error.Error = FAILED(hr) ? hr : E_FAIL;
        Error.Reserved = 0;
        QcQueueItem(Client, RIL_RESULT_ERROR, CommandId, &Error, sizeof(Error));
    }
}

static
DWORD
WINAPI
QcWorkerThread(
    _In_ LPVOID Parameter)
{
    PQC_MODEM Modem = Parameter;
    HANDLE Waits[2];

    Waits[0] = Modem->WorkSignal;

    for (;;)
    {
        PLIST_ENTRY Entry = NULL;
        PQC_WORK Work;
        QC_RESP Resp;
        BYTE Buffer[QC_RESPONSE_MAX];
        HRESULT hr;

        WaitForSingleObject(Modem->WorkSignal, INFINITE);
        if (Modem->Stopping)
            break;

        EnterCriticalSection(&Modem->WorkLock);
        if (!IsListEmpty(&Modem->WorkQueue))
            Entry = RemoveHeadList(&Modem->WorkQueue);
        LeaveCriticalSection(&Modem->WorkLock);

        if (!Entry)
            continue;

        /* More may be queued, keep the signal set until the queue drains */
        EnterCriticalSection(&Modem->WorkLock);
        if (!IsListEmpty(&Modem->WorkQueue))
            SetEvent(Modem->WorkSignal);
        LeaveCriticalSection(&Modem->WorkLock);

        Work = CONTAINING_RECORD(Entry, QC_WORK, Link);

        QcRespInit(&Resp, Buffer, sizeof(Buffer));
        QcTrace("cmd %lu (%s) id 0x%lx client %lu in %lu\n", Work->Command, QcCommandName(Work->Command),
                Work->CommandId, Work->Client->Id, Work->InSize);
        hr = QcDispatch(Modem, Work->Command, Work->In, Work->InSize, &Resp);
        if (SUCCEEDED(hr) && Resp.Failed)
            hr = E_FAIL;
        QcTrace("cmd %lu (%s) id 0x%lx done 0x%08lx out %lu\n", Work->Command, QcCommandName(Work->Command),
                Work->CommandId, hr, SUCCEEDED(hr) ? Resp.Length : 0);
        QcComplete(Work->Client, Work->CommandId, hr, &Resp);

        QcClientRelease(Work->Client);
        HeapFree(GetProcessHeap(), 0, Work);
    }

    UNREFERENCED_PARAMETER(Waits);
    return 0;
}

/* Runs the one time setup of one service, in the order WMRil initializes them */
static
VOID
QcSetupService(
    _In_ PQC_MODEM Modem,
    _In_ QC_SVC Which)
{
    QcTrace("setup: %s\n", QcServiceName(Which));

    switch (Which)
    {
        case QcSvcDms:
            QcDmsSetup(Modem);
            break;

        case QcSvcNas:
            QcNasSetup(Modem);
            break;

        case QcSvcWms:
            QcWmsSetup(Modem);
            break;

        case QcSvcVoice:
            QcVoiceSetup(Modem);
            break;

        case QcSvcUim:
            QcUimSetup(Modem);
            break;

        case QcSvcPdc:
            QcPdcSetup(Modem);
            break;

        case QcSvcImss:
            QcImssSetup(Modem);
            break;

        case QcSvcImsa:
            QcImsaSetup(Modem);
            break;

        default:
            break;
    }
}

/* Brings the modem services up, again after every modem restart, and runs card work */
static
DWORD
WINAPI
QcSetupThread(
    _In_ LPVOID Parameter)
{
    static const QC_SVC Order[] = { QcSvcDms, QcSvcNas, QcSvcWms, QcSvcVoice, QcSvcUim, QcSvcPdc,
                                    QcSvcImss, QcSvcImsa };
    PQC_MODEM Modem = Parameter;
    BOOL Missing, Configured, Recheck, Announced;
    ULONG i;

    while (!Modem->Stopping)
    {
        Missing = FALSE;

        for (i = 0; i < RTL_NUMBER_OF(Order) && !Modem->Stopping; i++)
        {
            EnterCriticalSection(&Modem->ServiceLock);
            Configured = Modem->Services[Order[i]].Configured;
            Announced = Modem->Services[Order[i]].Announced;
            LeaveCriticalSection(&Modem->ServiceLock);
            if (Configured)
                continue;

            /* Optional services are not polled for, the router announcing them kicks setup */
            if (QC_SVC_IS_OPTIONAL(Order[i]) && !Announced)
                continue;

            if (!QcServiceEnsure(Modem, Order[i]))
            {
                Missing = TRUE;
                continue;
            }

            QcSetupService(Modem, Order[i]);

            /* The carrier configuration waits on the card, so it is tried again shortly */
            EnterCriticalSection(&Modem->ServiceLock);
            Configured = Modem->Services[Order[i]].Ready;
            if (Order[i] == QcSvcPdc && !Modem->Pdc.Done)
            {
                Configured = FALSE;
                Missing = TRUE;
            }
            Modem->Services[Order[i]].Configured = Configured;
            LeaveCriticalSection(&Modem->ServiceLock);
        }

        /* The modem counts as up once its core services are, the rest may trail */
        EnterCriticalSection(&Modem->ServiceLock);
        Configured = Modem->Services[QcSvcDms].Configured && Modem->Services[QcSvcNas].Configured;
        LeaveCriticalSection(&Modem->ServiceLock);

        if (Configured && InterlockedCompareExchange(&Modem->ModemUp, 1, 0) == 0)
        {
            QcTrace("setup: core modem services configured%s\n", Missing ? ", others still missing" : "");
            if (InterlockedExchange(&Modem->Restarted, 0) != 0)
            {
                DWORD Reset = RIL_MODEMRESET_COMPLETED;

                QcTrace("ssr: services back, restoring the requested equipment state\n");
                Modem->Dms.RestoreTries = QC_RESTORE_RETRY;
                QcNotifyAll(Modem, RIL_NOTIFY_MODEMRESETSTATE, &Reset, sizeof(Reset));
            }
        }

        /* A modem that just restarted may refuse the mode change for a moment */
        if (Modem->Dms.RestoreTries && Modem->ModemUp)
        {
            if (QcDmsRestore(Modem))
            {
                Modem->Dms.RestoreTries = 0;
            }
            else if (Modem->Dms.RestoreTries > 1)
            {
                Modem->Dms.RestoreTries -= 1;
                Missing = TRUE;
            }
            else
            {
                QcTrace("ssr: the modem never took the requested equipment state back\n");
                Modem->Dms.RestoreTries = 0;
            }
        }

        /* The IMS PDN is started once the data service shows up */
        if (Modem->Ims.PdnPending && Modem->ModemUp)
        {
            if (QcServiceEnsure(Modem, QcSvcWds))
                QcImsStartPdn(Modem);
            else
                Missing = TRUE;
        }

        EnterCriticalSection(&Modem->StateLock);
        Recheck = Modem->Uim.Recheck;
        Modem->Uim.Recheck = FALSE;
        LeaveCriticalSection(&Modem->StateLock);
        if (Recheck)
            QcUimEvaluate(Modem);

        WaitForSingleObject(Modem->SetupSignal, Missing ? QC_RESOLVE_DELAY_MS : INFINITE);
    }

    return 0;
}

static
PQC_MODEM
QcModemCreate(VOID)
{
    PQC_MODEM Modem;

    Modem = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Modem));
    if (!Modem)
        return NULL;

    Modem->Ipc = INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&Modem->ServiceLock);
    InitializeCriticalSection(&Modem->SendLock);
    InitializeCriticalSection(&Modem->PendingLock);
    InitializeCriticalSection(&Modem->WorkLock);
    InitializeCriticalSection(&Modem->ClientLock);
    InitializeCriticalSection(&Modem->StateLock);
    InitializeListHead(&Modem->Pending);
    InitializeListHead(&Modem->WorkQueue);
    InitializeListHead(&Modem->Clients);

    Modem->Dms.Equipment = RIL_EQSTATE_MINIMUM;
    Modem->Dms.Requested = RIL_EQSTATE_MINIMUM;
    Modem->Nas.Net.Size = sizeof(Modem->Nas.Net);
    Modem->Nas.Net.RegState = RIL_REG_UNKNOWN;
    Modem->Uim.GwPrimary = 0xFFFF;
    Modem->NextCallId = 1;

    Modem->WorkSignal = CreateEventW(NULL, FALSE, FALSE, NULL);
    Modem->SetupSignal = CreateEventW(NULL, FALSE, FALSE, NULL);
    Modem->TxResume = CreateEventW(NULL, TRUE, FALSE, NULL);
    Modem->Dms.ChangeEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Modem->WorkSignal || !Modem->SetupSignal || !Modem->TxResume || !Modem->Dms.ChangeEvent)
    {
        QcTrace("cannot create the modem events\n");
        return NULL;
    }

    /* The modem object lives for the process, a failed open keeps it around unused */
    if (!QcIpcOpen(Modem))
        return NULL;

    QcWdsReset();

    QcIpcStartReceive(Modem);
    Modem->Worker = CreateThread(NULL, 0, QcWorkerThread, Modem, 0, NULL);
    Modem->Setup = CreateThread(NULL, 0, QcSetupThread, Modem, 0, NULL);

    QcTrace("modem bring-up started\n");
    return Modem;
}

/* Exported host interface */

DWORD
WINAPI
RIL_Version(
    _In_ DWORD MinVersion,
    _In_ DWORD MaxVersion)
{
    QcTrace("RIL_Version(%lu, %lu)\n", MinVersion, MaxVersion);

    /* The host interface is pinned to version 1 */
    if (MinVersion <= RIL_HOST_VERSION && MaxVersion >= RIL_HOST_VERSION)
        return RIL_HOST_VERSION;
    return 0;
}

DWORD
WINAPI
RIL_Init(
    _In_ DWORD Index)
{
    QcEnsureInitLock();

    EnterCriticalSection(&gInitLock);
    if (!gModem)
        gModem = QcModemCreate();
    LeaveCriticalSection(&gInitLock);

    QcTrace("RIL_Init(%lu): %s\n", Index, gModem ? "ready" : "no modem");

    return gModem ? QC_DRIVER_HANDLE : 0;
}

DWORD
WINAPI
RIL_Open(
    _In_ DWORD Driver,
    _In_ DWORD Access,
    _In_ DWORD Share)
{
    PQC_MODEM Modem = gModem;
    PQC_CLIENT Client;
    DWORD Id;

    if (Driver != QC_DRIVER_HANDLE || !Modem)
        return 0;

    Client = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Client));
    if (!Client)
        return 0;

    Client->Modem = Modem;
    InitializeCriticalSection(&Client->Lock);
    InitializeListHead(&Client->Queue);
    Client->Open = TRUE;
    Client->RefCount = 1;

    EnterCriticalSection(&Modem->ClientLock);
    Id = ++Modem->NextClientId;
    if (Id == 0)
        Id = ++Modem->NextClientId;
    Client->Id = Id;
    InsertTailList(&Modem->Clients, &Client->Link);
    LeaveCriticalSection(&Modem->ClientLock);

    QcTrace("RIL_Open: client %lu access 0x%lx share 0x%lx\n", Id, Access, Share);
    return Id;
}

BOOL
WINAPI
RIL_Close(
    _In_ DWORD ClientId)
{
    PQC_MODEM Modem = gModem;
    PQC_CLIENT Client;

    if (!Modem)
        return FALSE;

    EnterCriticalSection(&Modem->ClientLock);
    Client = QcClientFromId(Modem, ClientId);
    if (Client)
    {
        Client->Open = FALSE;
        RemoveEntryList(&Client->Link);
    }
    LeaveCriticalSection(&Modem->ClientLock);

    if (!Client)
        return FALSE;

    QcTrace("RIL_Close: client %lu\n", ClientId);

    /* Drop the reference the open held, a worker in flight keeps it alive */
    QcClientRelease(Client);
    return TRUE;
}

static
BOOL
QcInitNotifications(
    _In_ PQC_CLIENT Client,
    _In_reads_bytes_(InSize) const VOID *In,
    _In_ ULONG InSize,
    _Out_writes_bytes_(OutSize) PVOID Out,
    _In_ ULONG OutSize,
    _Out_ PDWORD Actual)
{
    ULONGLONG Handle;

    *Actual = 0;
    if (OutSize < sizeof(ULONGLONG))
        return FALSE;

    if (InSize >= sizeof(ULONGLONG))
        Client->CancelEvent = (HANDLE)(ULONG_PTR)(*(const ULONGLONG *)In);

    if (!Client->NotifyEvent)
    {
        Client->NotifyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!Client->NotifyEvent)
            return FALSE;
    }

    Handle = (ULONGLONG)(ULONG_PTR)Client->NotifyEvent;
    memcpy(Out, &Handle, sizeof(Handle));
    *Actual = sizeof(Handle);
    return TRUE;
}

static
BOOL
QcGetNextNotification(
    _In_ PQC_CLIENT Client,
    _Out_writes_bytes_(OutSize) PVOID Out,
    _In_ ULONG OutSize,
    _Out_ PDWORD Actual)
{
    RIL_HOST_QUEUED_V1 Header;
    PQC_QITEM Item = NULL;
    PLIST_ENTRY Entry;
    DWORD Provided, Required;

    *Actual = 0;
    if (OutSize < sizeof(Header))
        return FALSE;

    /* The host presets the first dword to the buffer size it is offering */
    Provided = *(const DWORD *)Out;
    if (Provided < sizeof(Header) || Provided > OutSize)
        Provided = OutSize;

    EnterCriticalSection(&Client->Lock);

    if (IsListEmpty(&Client->Queue))
    {
        if (Client->NotifyEvent)
            ResetEvent(Client->NotifyEvent);
        LeaveCriticalSection(&Client->Lock);
        return FALSE;
    }

    Entry = Client->Queue.Flink;
    Item = CONTAINING_RECORD(Entry, QC_QITEM, Link);
    Required = sizeof(Header) + Item->DataSize;

    ZeroMemory(&Header, sizeof(Header));
    Header.Size = sizeof(Header);
    Header.Required = Required;
    Header.Code = Item->Code;
    Header.CommandId = Item->CommandId;
    Header.DataSize = Item->DataSize;

    if (Required > Provided)
    {
        /* Report the needed size and keep the entry for a larger buffer */
        memcpy(Out, &Header, sizeof(Header));
        *Actual = sizeof(Header);
        LeaveCriticalSection(&Client->Lock);
        return TRUE;
    }

    RemoveEntryList(&Item->Link);
    if (IsListEmpty(&Client->Queue) && Client->NotifyEvent)
        ResetEvent(Client->NotifyEvent);
    LeaveCriticalSection(&Client->Lock);

    memcpy(Out, &Header, sizeof(Header));
    if (Item->DataSize)
        memcpy((PUCHAR)Out + sizeof(Header), Item->Data, Item->DataSize);
    *Actual = Required;

    HeapFree(GetProcessHeap(), 0, Item);
    return TRUE;
}

BOOL
WINAPI
RIL_IOControl(
    _In_ DWORD ClientId,
    _In_ DWORD Code,
    _In_reads_bytes_opt_(InSize) PBYTE In,
    _In_ DWORD InSize,
    _Out_writes_bytes_opt_(OutSize) PBYTE Out,
    _In_ DWORD OutSize,
    _Out_opt_ PDWORD Actual)
{
    PQC_MODEM Modem = gModem;
    PQC_CLIENT Client;
    PQC_WORK Work;
    DWORD Token, Scratch = 0;
    HRESULT Failure;

    if (!Actual)
        Actual = &Scratch;
    *Actual = 0;

    Client = Modem ? QcClientAcquire(Modem, ClientId) : NULL;
    if (!Client)
    {
        QcTrace("ioctl %lu for unknown client %lu\n", Code, ClientId);
        Failure = RIL_E_RADIONOTPRESENT;
        goto Fail;
    }

    if (Code == RilCmdInitNotifications)
    {
        BOOL Ok = QcInitNotifications(Client, In, InSize, Out, OutSize, Actual);
        QcClientRelease(Client);
        return Ok;
    }

    if (Code == RilCmdGetNextNotification)
    {
        BOOL Ok = QcGetNextNotification(Client, Out, OutSize, Actual);
        QcClientRelease(Client);
        return Ok;
    }

    /* Every other command is answered through the notification queue. The host
     * preset Out to the command id it tracks, keep it to mark the call accepted. */
    if (!Out || OutSize < sizeof(DWORD))
    {
        QcTrace("cmd %lu without a result word\n", Code);
        QcClientRelease(Client);
        return FALSE;
    }

    Token = *(const DWORD *)Out;

    Work = HeapAlloc(GetProcessHeap(), 0, FIELD_OFFSET(QC_WORK, In) + InSize);
    if (!Work)
    {
        QcClientRelease(Client);
        Failure = E_OUTOFMEMORY;
        goto Fail;
    }

    /* The work item holds its own reference until the worker answers it */
    InterlockedIncrement(&Client->RefCount);
    Work->Client = Client;
    Work->CommandId = Token;
    Work->Command = Code;
    Work->InSize = InSize;
    if (In && InSize)
        memcpy(Work->In, In, InSize);

    EnterCriticalSection(&Modem->WorkLock);
    InsertTailList(&Modem->WorkQueue, &Work->Link);
    SetEvent(Modem->WorkSignal);
    LeaveCriticalSection(&Modem->WorkLock);

    /* The preset command id stays in Out, the answer comes through the queue */
    QcClientRelease(Client);
    *Actual = sizeof(DWORD);
    return TRUE;

Fail:
    /* A synchronous failure is reported as a negative HRESULT in the result word */
    if (!Out || OutSize < sizeof(DWORD))
        return FALSE;

    memcpy(Out, &Failure, sizeof(Failure));
    *Actual = sizeof(DWORD);
    return TRUE;
}
