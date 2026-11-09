/*
 * PROJECT:     ReactOS Phone Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     RIL session, request tracking and radio bring-up
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "phonesvc.h"
#include <telephony/rilapi.h>

#define NDEBUG
#include <debug.h>

HRIL PhRil;

static CRITICAL_SECTION PhRequestLock;
static LIST_ENTRY PhRequests;
static ULONGLONG PhNextRequestId;
static BOOL PhRadioPresent;
static BOOL PhRadioPowered;

/* Notifications the service consumes */
static const DWORD PhNotificationCodes[] =
{
    RIL_NOTIFY_CALLSTATE,
    RIL_NOTIFY_CALLMODIFIED,
    RIL_NOTIFY_MESSAGE,
    RIL_NOTIFY_EMERGENCYNUMBERS,
    RIL_NOTIFY_SUPSERVICEINFO
};

PPH_REQUEST
PhBeginRequest(VOID)
{
    PPH_REQUEST Request;

    Request = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Request));
    if (!Request)
        return NULL;

    Request->Done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Request->Done)
    {
        HeapFree(GetProcessHeap(), 0, Request);
        return NULL;
    }

    EnterCriticalSection(&PhRequestLock);
    Request->Id = ++PhNextRequestId;
    InsertTailList(&PhRequests, &Request->Link);
    LeaveCriticalSection(&PhRequestLock);

    return Request;
}

/* Waits for the RIL answer to a request that SendStatus says was queued */
HRESULT
PhFinishRequest(
    _In_ PPH_REQUEST Request,
    _In_ HRESULT SendStatus,
    _Out_opt_ PPH_RIL_RESULT Result)
{
    HRESULT hr = SendStatus;

    if (SUCCEEDED(hr) && WaitForSingleObject(Request->Done, PHONE_RIL_TIMEOUT_MS) != WAIT_OBJECT_0)
        hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);

    EnterCriticalSection(&PhRequestLock);
    RemoveEntryList(&Request->Link);
    LeaveCriticalSection(&PhRequestLock);

    if (SUCCEEDED(hr))
    {
        if (Request->Result.ResultType != RIL_RESULT_OK)
            hr = FAILED(Request->Result.Error) ? Request->Result.Error : E_FAIL;
        if (Result)
            CopyMemory(Result, &Request->Result, sizeof(*Result));
    }

    CloseHandle(Request->Done);
    HeapFree(GetProcessHeap(), 0, Request);
    return hr;
}

static
VOID
CALLBACK
PhRilResult(
    _In_ HRIL hRil,
    _In_ DWORD ResultType,
    _In_ ULONGLONG Context,
    _In_reads_bytes_opt_(DataSize) const VOID *Data,
    _In_ DWORD DataSize,
    _In_opt_ PVOID Param)
{
    PLIST_ENTRY Entry;
    PPH_REQUEST Request;

    UNREFERENCED_PARAMETER(hRil);
    UNREFERENCED_PARAMETER(Param);

    EnterCriticalSection(&PhRequestLock);
    for (Entry = PhRequests.Flink; Entry != &PhRequests; Entry = Entry->Flink)
    {
        Request = CONTAINING_RECORD(Entry, PH_REQUEST, Link);
        if (Request->Id != Context)
            continue;

        Request->Result.ResultType = ResultType;
        if (ResultType == RIL_RESULT_ERROR && Data && DataSize >= sizeof(RIL_ERROR_RESULT))
            Request->Result.Error = ((const RIL_ERROR_RESULT *)Data)->Error;
        else if (ResultType != RIL_RESULT_OK)
            Request->Result.Error = E_FAIL;

        Request->Result.DataSize = min(DataSize, sizeof(Request->Result.Data));
        if (Data && Request->Result.DataSize)
            CopyMemory(Request->Result.Data, Data, Request->Result.DataSize);

        SetEvent(Request->Done);
        break;
    }
    LeaveCriticalSection(&PhRequestLock);
}

static
VOID
PhPublishRadio(VOID)
{
    PHONEOM_RADIO Radio;

    PhGetRadio(&Radio);
    PhBroadcast(PhoneEventRadio, &Radio, sizeof(Radio));
}

HRESULT
PhSetRadioPower(
    _In_ BOOL On)
{
    PPH_REQUEST Request;
    HRESULT hr;

    Request = PhBeginRequest();
    if (!Request)
        return E_OUTOFMEMORY;

    hr = RIL_SetEquipmentState(PhRil, Request->Id, On ? RIL_EQSTATE_FULL : RIL_EQSTATE_LOWPOWER);
    hr = PhFinishRequest(Request, hr, NULL);
    if (FAILED(hr))
    {
        DPRINT1("Equipment state %u failed 0x%lx\n", On, hr);
        return hr;
    }

    /* Per executor radio switch, not every library implements it */
    Request = PhBeginRequest();
    if (Request)
        PhFinishRequest(Request, RIL_SetExecutorRFState(PhRil, Request->Id, PHONE_EXECUTOR, On ? 1 : 0), NULL);

    PhRadioPowered = On;
    PhPublishRadio();
    return S_OK;
}

/* Runs on a pool thread, RIL callbacks may not block on further RIL traffic */
static
VOID
CALLBACK
PhRadioArrived(
    _Inout_ PTP_CALLBACK_INSTANCE Instance,
    _Inout_opt_ PVOID Context)
{
    PPH_REQUEST Request;
    HRESULT hr;

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(Context);

    Request = PhBeginRequest();
    if (!Request)
        return;

    hr = RIL_EnableNotifications(PhRil, Request->Id, PhNotificationCodes, _countof(PhNotificationCodes));
    hr = PhFinishRequest(Request, hr, NULL);
    if (FAILED(hr))
        DPRINT1("Enabling notifications failed 0x%lx\n", hr);

    PhSetRadioPower(TRUE);
}

typedef struct _PH_NOTIFY_WORK
{
    DWORD Code;
    DWORD Size;
    BYTE Data[ANYSIZE_ARRAY];
} PH_NOTIFY_WORK, *PPH_NOTIFY_WORK;

static
VOID
CALLBACK
PhNotifyWorker(
    _Inout_ PTP_CALLBACK_INSTANCE Instance,
    _Inout_opt_ PVOID Context)
{
    PPH_NOTIFY_WORK Work = Context;

    UNREFERENCED_PARAMETER(Instance);

    switch (Work->Code)
    {
        case RIL_NOTIFY_MESSAGE:
            PhSmsReceived(Work->Data, Work->Size);
            break;
    }

    HeapFree(GetProcessHeap(), 0, Work);
}

static
VOID
CALLBACK
PhRilNotify(
    _In_ HRIL hRil,
    _In_ DWORD Code,
    _In_reads_bytes_opt_(DataSize) const VOID *Data,
    _In_ DWORD DataSize,
    _In_opt_ PVOID Param)
{
    PPH_NOTIFY_WORK Work;

    UNREFERENCED_PARAMETER(hRil);
    UNREFERENCED_PARAMETER(Param);

    switch (Code)
    {
        case RIL_NOTIFY_RADIOPRESENCE:
            if (!Data || DataSize < sizeof(DWORD))
                break;

            if (*(const DWORD *)Data == RIL_RADIO_PRESENT)
            {
                PhRadioPresent = TRUE;
                PhPublishRadio();
                TrySubmitThreadpoolCallback(PhRadioArrived, NULL, NULL);
            }
            else
            {
                PhRadioPresent = FALSE;
                PhRadioPowered = FALSE;
                PhCallsReset();
                PhAudioCallActive(FALSE, FALSE);
                PhPublishRadio();
            }
            break;

        case RIL_NOTIFY_CALLSTATE:
            if (Data)
                PhCallStateChanged(Data, DataSize);
            break;

        case RIL_NOTIFY_MESSAGE:
        {
            /* Decoding and acking issue RIL traffic, hand it to a worker */
            if (!Data || DataSize == 0)
                break;

            Work = HeapAlloc(GetProcessHeap(), 0, FIELD_OFFSET(PH_NOTIFY_WORK, Data) + DataSize);
            if (!Work)
                break;

            Work->Code = Code;
            Work->Size = DataSize;
            CopyMemory(Work->Data, Data, DataSize);
            if (!TrySubmitThreadpoolCallback(PhNotifyWorker, Work, NULL))
                HeapFree(GetProcessHeap(), 0, Work);
            break;
        }

        default:
            break;
    }
}

VOID
PhGetRadio(
    _Out_ PPHONEOM_RADIO Radio)
{
    ZeroMemory(Radio, sizeof(*Radio));
    Radio->Present = PhRadioPresent;
    Radio->PoweredOn = PhRadioPowered;
}

BOOL
PhRadioStart(VOID)
{
    HRESULT hr;

    InitializeCriticalSection(&PhRequestLock);
    InitializeListHead(&PhRequests);

    hr = RIL_Initialize(0,
                        PhRilResult,
                        PhRilNotify,
                        PhNotificationCodes,
                        _countof(PhNotificationCodes),
                        NULL,
                        PHONE_SERVICE_NAME,
                        &PhRil);
    if (FAILED(hr))
    {
        DPRINT1("RIL_Initialize failed 0x%lx\n", hr);
        PhRil = NULL;
        return FALSE;
    }

    return TRUE;
}

VOID
PhRadioStop(VOID)
{
    PLIST_ENTRY Entry;

    if (PhRil)
    {
        RIL_Deinitialize(PhRil);
        PhRil = NULL;
    }

    /* Release anyone still waiting */
    EnterCriticalSection(&PhRequestLock);
    for (Entry = PhRequests.Flink; Entry != &PhRequests; Entry = Entry->Flink)
    {
        PPH_REQUEST Request = CONTAINING_RECORD(Entry, PH_REQUEST, Link);
        Request->Result.ResultType = RIL_RESULT_ERROR;
        Request->Result.Error = RIL_E_RADIONOTAVAILABLE;
        SetEvent(Request->Done);
    }
    LeaveCriticalSection(&PhRequestLock);
}
