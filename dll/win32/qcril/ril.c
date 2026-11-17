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

static PQC_MODEM gModem;
static CRITICAL_SECTION gInitLock;
static LONG gInitLockReady;

VOID
QcTrace(
    _In_ PCSTR Format,
    ...)
{
    CHAR Line[256];
    va_list Args;
    int Length;

    va_start(Args, Format);
    Length = _vsnprintf(Line, sizeof(Line) - 8, Format, Args);
    va_end(Args);

    if (Length < 0)
        Length = sizeof(Line) - 8;

    memmove(Line + 7, Line, Length + 1);
    memcpy(Line, "[qcril] ", 7);
    Line[7 + Length] = '\0';
    OutputDebugStringA(Line);
}

static
VOID
QcEnsureInitLock(VOID)
{
    if (InterlockedCompareExchange(&gInitLockReady, 1, 0) == 0)
        InitializeCriticalSection(&gInitLock);
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

    EnterCriticalSection(&Modem->ClientLock);
    for (Entry = Modem->Clients.Flink; Entry != &Modem->Clients; Entry = Entry->Flink)
    {
        PQC_CLIENT Client = CONTAINING_RECORD(Entry, QC_CLIENT, Link);
        if (Client->Open)
            QcQueueItem(Client, Code, 0, Data, Size);
    }
    LeaveCriticalSection(&Modem->ClientLock);
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
            return QcDmsSetRadioPower(Modem, *(const DWORD *)In == RIL_EQSTATE_FULL);

        case RilCmdGetDeviceInfo:
            return QcDmsGetDeviceInfo(Modem, Resp);

        case RilCmdGetSignalQuality:
        case RilCmdGetCurrentRegStatus:
            return QcNasGetNetwork(Modem, Resp);

        case RilCmdGetCardInfo:
        case RilCmdEnumerateSlots:
            return QcUimGetSubscriber(Modem, Resp);

        case RilCmdDevSpecific:
            return QcWdsData(Modem, In, InSize, Resp);

        case RilCmdDial:
            return QcVoiceDial(Modem, In, InSize, Resp);

        case RilCmdManageCalls:
            return QcVoiceManage(Modem, In, InSize);

        case RilCmdSendMsg:
            return QcWmsSend(Modem, In, InSize, Resp);

        /* Accepted without modem traffic during bring-up */
        case RilCmdSetExecutorRfState:
        case RilCmdSetRfState:
        case RilCmdSendMsgAck:
        case RilCmdSendDtmf:
        case RilCmdStartDtmf:
        case RilCmdStopDtmf:
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
        hr = QcDispatch(Modem, Work->Command, Work->In, Work->InSize, &Resp);
        QcComplete(Work->Client, Work->CommandId, hr, &Resp);

        QcClientRelease(Work->Client);
        HeapFree(GetProcessHeap(), 0, Work);
    }

    UNREFERENCED_PARAMETER(Waits);
    return 0;
}

/*
 * Asks the modem for what only it can report, identity and signal, and logs
 * it. None of this needs a SIM, so it proves the radio is reachable from user
 * mode and shows what it is doing, independent of the host RIL plumbing.
 */
static
VOID
QcModemSelfTest(
    _In_ PQC_MODEM Modem)
{
    CHAR Text[96];
    QMI_ENCODER Encoder;
    BYTE Request[16];
    BYTE Response[512];
    QC_RESP Resp;
    const VOID *Value;
    USHORT ValueLen;
    ULONG ReqLen, RespLen;

    QcTrace("selftest: modem reachable, reading what it reports");

    if (SUCCEEDED(QcDmsReadString(Modem, 0x0021, 0x01, Text, sizeof(Text))))
        QcTrace("selftest: manufacturer = '%s'", Text);
    if (SUCCEEDED(QcDmsReadString(Modem, 0x0022, 0x01, Text, sizeof(Text))))
        QcTrace("selftest: model = '%s'", Text);
    if (SUCCEEDED(QcDmsReadString(Modem, 0x0023, 0x01, Text, sizeof(Text))))
        QcTrace("selftest: revision = '%s'", Text);
    if (SUCCEEDED(QcDmsReadString(Modem, 0x0025, 0x11, Text, sizeof(Text))))
        QcTrace("selftest: imei = '%s'", Text);

    /* Current operating mode, reported with or without a SIM */
    QmiEncodeBegin(&Encoder, Request, sizeof(Request), QMI_TYPE_REQUEST, 0, 0x002D);
    ReqLen = QmiEncodeEnd(&Encoder);
    if (SUCCEEDED(QcQmiRequest(Modem, QcSvcDms, Request, ReqLen, Response, sizeof(Response), &RespLen)) &&
        QmiFindTlv(Response, RespLen, 0x01, &Value, &ValueLen) && ValueLen >= 1)
    {
        QcTrace("selftest: operating mode %u", *(const UCHAR *)Value);

        /* Why the modem stays out of online, when it says */
        if (QmiFindTlv(Response, RespLen, 0x10, &Value, &ValueLen) && ValueLen >= 2)
            QcTrace("selftest: offline reason 0x%04x", *(const USHORT UNALIGNED *)Value);
        if (QmiFindTlv(Response, RespLen, 0x11, &Value, &ValueLen) && ValueLen >= 1)
            QcTrace("selftest: hardware restricted mode %u", *(const UCHAR *)Value);
    }

    /* Signal and registration, the radio searches and reports these with no SIM */
    if (QcServiceEnsure(Modem, QcSvcNas))
    {
        QcRespInit(&Resp, Response, sizeof(Response));
        if (SUCCEEDED(QcNasGetNetwork(Modem, &Resp)) && Resp.Length >= sizeof(RIL_NETWORK_INFO))
        {
            const RIL_NETWORK_INFO *Net = (const RIL_NETWORK_INFO *)Resp.Data;
            QcTrace("selftest: signal %ld dBm, %lu bars, reg state %lu, rat %lu, mcc %lu mnc %lu",
                    Net->SignalDbm, Net->SignalBars, Net->RegState, Net->Rat, Net->Mcc, Net->Mnc);
        }
    }

    QcTrace("selftest: done");
}

/* Waits for each modem service to appear, then runs its one time setup */
static
DWORD
WINAPI
QcSetupThread(
    _In_ LPVOID Parameter)
{
    PQC_MODEM Modem = Parameter;
    BOOL Dms = FALSE, Wms = FALSE, Nas = FALSE, Voice = FALSE;
    ULONG Attempt;

    for (Attempt = 0; Attempt < QC_SETUP_RETRY && !Modem->Stopping; Attempt++)
    {
        if (!Dms && QcServiceEnsure(Modem, QcSvcDms))
        {
            QcModemSelfTest(Modem);
            Dms = TRUE;
        }
        if (!Wms && QcServiceEnsure(Modem, QcSvcWms))
        {
            QcWmsConfigureRoutes(Modem);
            Wms = TRUE;
        }
        if (!Nas && QcServiceEnsure(Modem, QcSvcNas))
        {
            QcNasEnableIndications(Modem);
            Nas = TRUE;
        }
        if (!Voice && QcServiceEnsure(Modem, QcSvcVoice))
        {
            QcVoiceSeed(Modem);
            Voice = TRUE;
        }

        if (Dms && Wms && Nas && Voice)
            break;

        Sleep(QC_RESOLVE_DELAY_MS);
    }

    QcTrace("service setup done: dms %u wms %u nas %u voice %u\n", Dms, Wms, Nas, Voice);
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
    InitializeListHead(&Modem->Pending);
    InitializeListHead(&Modem->WorkQueue);
    InitializeListHead(&Modem->Clients);

    Modem->WorkSignal = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!Modem->WorkSignal)
    {
        HeapFree(GetProcessHeap(), 0, Modem);
        return NULL;
    }

    if (!QcIpcOpen(Modem))
    {
        CloseHandle(Modem->WorkSignal);
        HeapFree(GetProcessHeap(), 0, Modem);
        return NULL;
    }

    QcIpcStartReceive(Modem);
    Modem->Worker = CreateThread(NULL, 0, QcWorkerThread, Modem, 0, NULL);
    Modem->Setup = CreateThread(NULL, 0, QcSetupThread, Modem, 0, NULL);

    QcTrace("modem bring-up started\n");
    return Modem;
}

/* ---- Exported host interface ---- */

DWORD
WINAPI
RIL_Version(
    _In_ DWORD MinVersion,
    _In_ DWORD MaxVersion)
{
    if (MaxVersion >= 2 && MinVersion <= 2)
        return 2;
    if (MinVersion <= 1 && MaxVersion >= 1)
        return 1;
    return 0;
}

DWORD
WINAPI
RIL_Init(
    _In_ DWORD Index)
{
    UNREFERENCED_PARAMETER(Index);

    QcEnsureInitLock();

    EnterCriticalSection(&gInitLock);
    if (!gModem)
        gModem = QcModemCreate();
    LeaveCriticalSection(&gInitLock);

    return gModem ? QC_DRIVER_HANDLE : 0;
}

DWORD
WINAPI
RIL_Open(
    _In_ DWORD Driver)
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
    RIL_HOST_QUEUED_V2 Header;
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

    if (!Actual)
        Actual = &Scratch;
    *Actual = 0;

    if (!Modem)
        return FALSE;

    Client = QcClientAcquire(Modem, ClientId);
    if (!Client)
        return FALSE;

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
    if (OutSize < sizeof(DWORD))
    {
        QcClientRelease(Client);
        return FALSE;
    }

    Token = *(const DWORD *)Out;

    Work = HeapAlloc(GetProcessHeap(), 0, FIELD_OFFSET(QC_WORK, In) + InSize);
    if (!Work)
    {
        QcClientRelease(Client);
        return FALSE;
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

    QcClientRelease(Client);
    *Actual = sizeof(DWORD);
    return TRUE;
}
