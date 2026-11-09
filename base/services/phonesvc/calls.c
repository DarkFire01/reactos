/*
 * PROJECT:     ReactOS Phone Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Voice call tracking and control
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "phonesvc.h"
#include <telephony/rilapi.h>

#define NDEBUG
#include <debug.h>

static CRITICAL_SECTION PhCallLock;
static PHONEOM_CALL PhCalls[PHONEOM_MAX_CALLS];
static BOOL PhCallInUse[PHONEOM_MAX_CALLS];
static BOOL PhVoiceOpen;

static
DWORD
PhTranslateStatus(
    _In_ DWORD RilStatus)
{
    switch (RilStatus)
    {
        case RIL_CALLSTATUS_OUTGOING:
            return PhoneCallDialing;
        case RIL_CALLSTATUS_INCOMING:
            return PhoneCallIncoming;
        case RIL_CALLSTATUS_CONNECTED:
            return PhoneCallActive;
        case RIL_CALLSTATUS_HELD:
            return PhoneCallHeld;
        case RIL_CALLSTATUS_DISCONNECTED:
            return PhoneCallEnded;
        default:
            return PhoneCallIdle;
    }
}

/* Returns the slot for CallId, claiming a free one when Create is set */
static
LONG
PhFindSlot(
    _In_ DWORD CallId,
    _In_ BOOL Create)
{
    LONG i, Free = -1;

    for (i = 0; i < PHONEOM_MAX_CALLS; i++)
    {
        if (PhCallInUse[i] && PhCalls[i].CallId == CallId)
            return i;
        if (!PhCallInUse[i] && Free < 0)
            Free = i;
    }

    if (!Create || Free < 0)
        return -1;

    ZeroMemory(&PhCalls[Free], sizeof(PhCalls[Free]));
    PhCalls[Free].CallId = CallId;
    PhCallInUse[Free] = TRUE;
    return Free;
}

/* The voice path stays up while any call is past the ringing stage */
static
VOID
PhRefreshVoicePath(
    _In_ BOOL PacketSwitched)
{
    BOOL Needed = FALSE;
    LONG i;

    for (i = 0; i < PHONEOM_MAX_CALLS; i++)
    {
        if (!PhCallInUse[i])
            continue;

        if (PhCalls[i].State == PhoneCallDialing ||
            PhCalls[i].State == PhoneCallActive ||
            PhCalls[i].State == PhoneCallHeld)
        {
            Needed = TRUE;
            break;
        }
    }

    if (Needed == PhVoiceOpen)
        return;

    PhVoiceOpen = Needed;
    PhAudioCallActive(Needed, PacketSwitched);
}

VOID
PhCallStateChanged(
    _In_reads_bytes_(Size) const VOID *Data,
    _In_ DWORD Size)
{
    const RIL_CALL_RECORD *Record = Data;
    PHONEOM_CALL Snapshot;
    PPHONEOM_CALL Call;
    DWORD State;
    LONG Slot;

    if (Size < FIELD_OFFSET(RIL_CALL_RECORD, NumberPresentation))
        return;

    if (Record->Executor != PHONE_EXECUTOR)
        return;

    State = PhTranslateStatus(Record->Status);

    EnterCriticalSection(&PhCallLock);

    Slot = PhFindSlot(Record->CallId, State != PhoneCallEnded && State != PhoneCallIdle);
    if (Slot < 0)
    {
        LeaveCriticalSection(&PhCallLock);
        return;
    }

    Call = &PhCalls[Slot];
    if (Call->State != PhoneCallActive && State == PhoneCallActive)
        GetSystemTimeAsFileTime(&Call->StartTime);

    Call->State = State;
    Call->Flags = 0;
    if (Record->Direction == RIL_CALLDIR_INCOMING)
        Call->Flags |= PHONEOM_CALL_INCOMING;
    if (Record->Multiparty)
        Call->Flags |= PHONEOM_CALL_CONFERENCE;
    if (Record->Domain == RIL_CALL_DOMAIN_IMS)
        Call->Flags |= PHONEOM_CALL_IMS;
    if (Size >= FIELD_OFFSET(RIL_CALL_RECORD, DisconnectInitiator) && (Record->Flags & RIL_CALLFLAG_EMERGENCY))
        Call->Flags |= PHONEOM_CALL_EMERGENCY;

    if (Record->Remote.dwParams & RILADDRESS_PARAM_ADDRESS)
    {
        Call->Number[0] = UNICODE_NULL;
        if ((Record->Remote.dwParams & RILADDRESS_PARAM_TYPE) &&
            Record->Remote.dwType == RILADDRESSTYPE_INTERNATIONAL &&
            Record->Remote.wszAddress[0] != L'+')
        {
            StringCchCopyW(Call->Number, _countof(Call->Number), L"+");
        }

        StringCchCatNW(Call->Number,
                       _countof(Call->Number),
                       Record->Remote.wszAddress,
                       _countof(Record->Remote.wszAddress));
    }

    if (Record->RemoteName[0])
        StringCchCopyNW(Call->Name, _countof(Call->Name), Record->RemoteName, _countof(Record->RemoteName));

    CopyMemory(&Snapshot, Call, sizeof(Snapshot));

    /* An ended call is reported once and then forgotten */
    if (State == PhoneCallEnded || State == PhoneCallIdle)
        PhCallInUse[Slot] = FALSE;

    PhRefreshVoicePath(Record->Domain == RIL_CALL_DOMAIN_IMS);
    LeaveCriticalSection(&PhCallLock);

    PhBroadcast(PhoneEventCall, &Snapshot, sizeof(Snapshot));
}

VOID
PhCallsReset(VOID)
{
    PHONEOM_CALL Ended[PHONEOM_MAX_CALLS];
    DWORD Count = 0;
    LONG i;

    EnterCriticalSection(&PhCallLock);
    for (i = 0; i < PHONEOM_MAX_CALLS; i++)
    {
        if (!PhCallInUse[i])
            continue;

        PhCalls[i].State = PhoneCallEnded;
        CopyMemory(&Ended[Count++], &PhCalls[i], sizeof(PhCalls[i]));
        PhCallInUse[i] = FALSE;
    }
    PhVoiceOpen = FALSE;
    LeaveCriticalSection(&PhCallLock);

    for (i = 0; i < (LONG)Count; i++)
        PhBroadcast(PhoneEventCall, &Ended[i], sizeof(Ended[i]));
}

/* The modem takes 0-9 * # A-D only, a leading '+' becomes the international address type */
BOOL
PhBuildAddress(
    _In_ LPCWSTR Number,
    _Out_ RILADDRESS *Address)
{
    DWORD Length = 0;
    LPCWSTR p = Number;

    ZeroMemory(Address, sizeof(*Address));
    Address->cbSize = sizeof(*Address);
    Address->dwParams = RILADDRESS_PARAM_TYPE | RILADDRESS_PARAM_ADDRESS;
    Address->dwType = RILADDRESSTYPE_UNKNOWN;

    while (*p == L' ')
        p++;

    if (*p == L'+')
    {
        Address->dwType = RILADDRESSTYPE_INTERNATIONAL;
        p++;
    }

    for (; *p; p++)
    {
        if (*p == L' ' || *p == L'-' || *p == L'(' || *p == L')' || *p == L'.')
            continue;

        if (!((*p >= L'0' && *p <= L'9') || *p == L'*' || *p == L'#' || (*p >= L'A' && *p <= L'D')))
            return FALSE;

        if (Length >= PHONE_DIAL_MAX_DIGITS)
            return FALSE;

        Address->wszAddress[Length++] = *p;
    }

    return Length != 0;
}

HRESULT
PhDial(
    _In_ LPCWSTR Number,
    _Out_ PDWORD CallId)
{
    RILADDRESS Address;
    PH_RIL_RESULT Result;
    PPH_REQUEST Request;
    HRESULT hr;

    *CallId = 0;

    if (!PhRil)
        return RIL_E_RADIONOTAVAILABLE;

    if (!PhBuildAddress(Number, &Address))
        return RIL_E_INVALIDDIALSTRING;

    Request = PhBeginRequest();
    if (!Request)
        return E_OUTOFMEMORY;

    hr = RIL_Dial(PhRil, Request->Id, PHONE_EXECUTOR, &Address, 0, RIL_CALL_DOMAIN_AUTO, NULL);
    hr = PhFinishRequest(Request, hr, &Result);
    if (FAILED(hr))
    {
        DPRINT1("Dial failed 0x%lx\n", hr);
        return hr;
    }

    /* The dial answer carries the new call id */
    if (Result.DataSize >= sizeof(DWORD))
        CopyMemory(CallId, Result.Data, sizeof(*CallId));

    return S_OK;
}

HRESULT
PhManageCall(
    _In_ DWORD Action,
    _In_ DWORD CallId)
{
    PPH_REQUEST Request;
    HRESULT hr;

    if (!PhRil)
        return RIL_E_RADIONOTAVAILABLE;

    Request = PhBeginRequest();
    if (!Request)
        return E_OUTOFMEMORY;

    hr = RIL_ManageCalls_V2(PhRil, Request->Id, PHONE_EXECUTOR, Action, CallId, NULL);
    return PhFinishRequest(Request, hr, NULL);
}

HRESULT
PhSendDtmf(
    _In_ WCHAR Digit)
{
    PPH_REQUEST Request;
    CHAR Tone[2];
    HRESULT hr;

    if (!PhRil)
        return RIL_E_RADIONOTAVAILABLE;

    if (!((Digit >= L'0' && Digit <= L'9') || Digit == L'*' || Digit == L'#' ||
          (Digit >= L'A' && Digit <= L'D')))
    {
        return E_INVALIDARG;
    }

    Tone[0] = (CHAR)Digit;
    Tone[1] = '\0';

    Request = PhBeginRequest();
    if (!Request)
        return E_OUTOFMEMORY;

    /* Zero durations let the modem use its network defaults */
    hr = RIL_SendDTMF(PhRil, Request->Id, PHONE_EXECUTOR, Tone, 0, 0);
    return PhFinishRequest(Request, hr, NULL);
}

DWORD
PhGetCalls(
    _Out_writes_to_(MaxCalls, return) PPHONEOM_CALL Calls,
    _In_ DWORD MaxCalls)
{
    DWORD Count = 0;
    LONG i;

    EnterCriticalSection(&PhCallLock);
    for (i = 0; i < PHONEOM_MAX_CALLS && Count < MaxCalls; i++)
    {
        if (PhCallInUse[i])
            CopyMemory(&Calls[Count++], &PhCalls[i], sizeof(PhCalls[i]));
    }
    LeaveCriticalSection(&PhCallLock);

    return Count;
}

VOID
PhCallsInitialize(VOID)
{
    InitializeCriticalSection(&PhCallLock);
}
