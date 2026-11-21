/*
 * PROJECT:     ReactOS RIL Proxy
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Connection to the MSRil broker, request and record dispatch
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "rilproxy.h"
#include <stdio.h>

/* Used while no device publishes the client interface yet */
#define RIL_BROKER_FALLBACK_NAME    L"\\\\.\\MSRil1"

#define RIL_TRACE_PREFIX            "[rilproxy] "

static const PCSTR RilpCommandNames[] =
{
    [RilCmdInitNotifications] = "InitNotifications",
    [RilCmdGetNextNotification] = "GetNextNotification",
    [RilCmdEnableNotifications] = "EnableNotifications",
    [RilCmdDisableNotifications] = "DisableNotifications",
    [RilCmdGetDriverVersion] = "GetDriverVersion",
    [RilCmdGetDevCaps] = "GetDevCaps",
    [RilCmdGetDeviceInfo] = "GetDeviceInfo",
    [RilCmdGetEquipmentState] = "GetEquipmentState",
    [RilCmdSetEquipmentState] = "SetEquipmentState",
    [RilCmdSetNotificationFilterState] = "SetNotificationFilterState",
    [RilCmdGetNotificationFilterState] = "GetNotificationFilterState",
    [RilCmdEnumerateSlots] = "EnumerateSlots",
    [RilCmdGetCardInfo] = "GetCardInfo",
    [RilCmdSetSlotPower] = "SetSlotPower",
    [RilCmdGetUiccRecordStatus] = "GetUiccRecordStatus",
    [RilCmdSendRestrictedUiccCmd] = "SendRestrictedUiccCmd",
    [RilCmdWatchUiccFileChange] = "WatchUiccFileChange",
    [RilCmdGetUiccPrlId] = "GetUiccPrlId",
    [RilCmdGetImsi] = "GetImsi",
    [RilCmdGetSubscriberNumbers] = "GetSubscriberNumbers",
    [RilCmdGetUiccLockState] = "GetUiccLockState",
    [RilCmdGetUiccServiceLock] = "GetUiccServiceLock",
    [RilCmdVerifyUiccLock] = "VerifyUiccLock",
    [RilCmdSetUiccLockEnabled] = "SetUiccLockEnabled",
    [RilCmdUnblockUiccLock] = "UnblockUiccLock",
    [RilCmdChangeUiccLockPassword] = "ChangeUiccLockPassword",
    [RilCmdGetUiccAppPersoCheckState] = "GetUiccAppPersoCheckState",
    [RilCmdGetPersoDeactivationState] = "GetPersoDeactivationState",
    [RilCmdDeactivatePerso] = "DeactivatePerso",
    [RilCmdReadPhonebookEntries] = "ReadPhonebookEntries",
    [RilCmdWritePhonebookEntry] = "WritePhonebookEntry",
    [RilCmdDeletePhonebookEntry] = "DeletePhonebookEntry",
    [RilCmdGetPhonebookOptions] = "GetPhonebookOptions",
    [RilCmdGetAllAdditionalNumberStrings] = "GetAllAdditionalNumberStrings",
    [RilCmdGetAllEmergencyNumbers] = "GetAllEmergencyNumbers",
    [RilCmdSetRadioConfiguration] = "SetRadioConfiguration",
    [RilCmdGetRadioConfiguration] = "GetRadioConfiguration",
    [RilCmdSetExecutorConfig] = "SetExecutorConfig",
    [RilCmdGetExecutorConfig] = "GetExecutorConfig",
    [RilCmdSetSystemSelectionPrefs] = "SetSystemSelectionPrefs",
    [RilCmdGetSystemSelectionPrefs] = "GetSystemSelectionPrefs",
    [RilCmdGetOperatorList] = "GetOperatorList",
    [RilCmdGetPreferredOperatorList] = "GetPreferredOperatorList",
    [RilCmdGetCurrentRegStatus] = "GetCurrentRegStatus",
    [RilCmdGetSignalQuality] = "GetSignalQuality",
    [RilCmdSendUiccToolkitCmdResponse] = "SendUiccToolkitCmdResponse",
    [RilCmdSendUiccToolkitEnvelope] = "SendUiccToolkitEnvelope",
    [RilCmdDial] = "Dial",
    [RilCmdManageCalls] = "ManageCalls",
    [RilCmdEmergencyModeControl] = "EmergencyModeControl",
    [RilCmdGetCallForwardingSettings] = "GetCallForwardingSettings",
    [RilCmdSetCallForwardingStatus] = "SetCallForwardingStatus",
    [RilCmdAddCallForwarding] = "AddCallForwarding",
    [RilCmdRemoveCallForwarding] = "RemoveCallForwarding",
    [RilCmdGetCallBarringStatus] = "GetCallBarringStatus",
    [RilCmdSetCallBarringStatus] = "SetCallBarringStatus",
    [RilCmdChangeCallBarringPassword] = "ChangeCallBarringPassword",
    [RilCmdGetCallWaitingSettings] = "GetCallWaitingSettings",
    [RilCmdSetCallWaitingStatus] = "SetCallWaitingStatus",
    [RilCmdGetCallerIdSettings] = "GetCallerIdSettings",
    [RilCmdGetDialedIdSettings] = "GetDialedIdSettings",
    [RilCmdGetHideConnectedIdSettings] = "GetHideConnectedIdSettings",
    [RilCmdGetHideIdSettings] = "GetHideIdSettings",
    [RilCmdSendFlash] = "SendFlash",
    [RilCmdSendSupServiceData] = "SendSupServiceData",
    [RilCmdSendDtmf] = "SendDtmf",
    [RilCmdStartDtmf] = "StartDtmf",
    [RilCmdStopDtmf] = "StopDtmf",
    [RilCmdGetMsgServiceOptions] = "GetMsgServiceOptions",
    [RilCmdReadMsg] = "ReadMsg",
    [RilCmdWriteMsg] = "WriteMsg",
    [RilCmdDeleteMsg] = "DeleteMsg",
    [RilCmdGetCellBroadcastMsgConfig] = "GetCellBroadcastMsgConfig",
    [RilCmdSetCellBroadcastMsgConfig] = "SetCellBroadcastMsgConfig",
    [RilCmdGetMsgInUiccStatus] = "GetMsgInUiccStatus",
    [RilCmdSetMsgInUiccStatus] = "SetMsgInUiccStatus",
    [RilCmdSetMsgMemoryStatus] = "SetMsgMemoryStatus",
    [RilCmdSendMsg] = "SendMsg",
    [RilCmdGetSmsc] = "GetSmsc",
    [RilCmdSetSmsc] = "SetSmsc",
    [RilCmdGetImsStatus] = "GetImsStatus",
    [RilCmdGetPositionInfo] = "GetPositionInfo",
    [RilCmdGetRadioStateGroups] = "GetRadioStateGroups",
    [RilCmdGetRadioStateDetails] = "GetRadioStateDetails",
    [RilCmdSetRadioStateDetails] = "SetRadioStateDetails",
    [RilCmdRadioStatePasswordCompare] = "RadioStatePasswordCompare",
    [RilCmdRadioStateGetPasswordRetryCount] = "RadioStateGetPasswordRetryCount",
    [RilCmdDevSpecific] = "DevSpecific",
    [RilCmdSetRfState] = "SetRfState",
    [RilCmdGetRfState] = "GetRfState",
    [RilCmdGetDmProfileConfigInfo] = "GetDmProfileConfigInfo",
    [RilCmdSetDmProfileConfigInfo] = "SetDmProfileConfigInfo",
    [RilCmdWriteAdditionalNumberString] = "WriteAdditionalNumberString",
    [RilCmdDeleteAdditionalNumberString] = "DeleteAdditionalNumberString",
    [RilCmdGetUiccAtr] = "GetUiccAtr",
    [RilCmdOpenUiccLogicalChannel] = "OpenUiccLogicalChannel",
    [RilCmdCloseUiccLogicalChannel] = "CloseUiccLogicalChannel",
    [RilCmdExchangeUiccApdu] = "ExchangeUiccApdu",
    [RilCmdSendSupServiceDataResponse] = "SendSupServiceDataResponse",
    [RilCmdCancelSupServiceDataSession] = "CancelSupServiceDataSession",
    [RilCmdSetUiccToolkitProfile] = "SetUiccToolkitProfile",
    [RilCmdGetUiccToolkitProfile] = "GetUiccToolkitProfile",
    [RilCmdRegisterUiccToolkitService] = "RegisterUiccToolkitService",
    [RilCmdSendMsgAck] = "SendMsgAck",
    [RilCmdCloseUiccLogicalChannelGroup] = "CloseUiccLogicalChannelGroup",
    [RilCmdSetPreferredOperatorList] = "SetPreferredOperatorList",
    [RilCmdGetUiccServiceState] = "GetUiccServiceState",
    [RilCmdSetUiccServiceState] = "SetUiccServiceState",
    [RilCmdGetCallList] = "GetCallList",
    [RilCmdGetExecutorFocus] = "GetExecutorFocus",
    [RilCmdSetExecutorFocus] = "SetExecutorFocus",
    [RilCmdGetEmergencyMode] = "GetEmergencyMode",
    [RilCmdGetExecutorRfState] = "GetExecutorRfState",
    [RilCmdSetExecutorRfState] = "SetExecutorRfState",
    [RilCmdResetModem] = "ResetModem",
    [RilCmdCancelGetOperatorList] = "CancelGetOperatorList",
    [RilCmdAvoidCdmaSystem] = "AvoidCdmaSystem",
    [RilCmdSetPsMediaConfiguration] = "SetPsMediaConfiguration",
    [RilCmdGetPsMediaConfiguration] = "GetPsMediaConfiguration",
    [RilCmdSetGeolocationData] = "SetGeolocationData",
    [RilCmdSetTerminalCapability] = "SetTerminalCapability",
    [RilCmdGetTerminalCapability] = "GetTerminalCapability",
    [RilCmdSendRtt] = "SendRtt",
};

static const PCSTR RilpModemLogCommandNames[] =
{
    [RilCmdEnableModemFilters - RilCmdEnableModemFilters] = "EnableModemFilters",
    [RilCmdDisableModemFilters - RilCmdEnableModemFilters] = "DisableModemFilters",
    [RilCmdStartModemLogs - RilCmdEnableModemFilters] = "StartModemLogs",
    [RilCmdStopModemLogs - RilCmdEnableModemFilters] = "StopModemLogs",
    [RilCmdDrainModemLogs - RilCmdEnableModemFilters] = "DrainModemLogs",
};

VOID
__cdecl
RilpTrace(
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...)
{
    CHAR Buffer[512];
    const SIZE_T PrefixLength = sizeof(RIL_TRACE_PREFIX) - 1;
    va_list Args;

    CopyMemory(Buffer, RIL_TRACE_PREFIX, PrefixLength);

    va_start(Args, Format);
    _vsnprintf(Buffer + PrefixLength, sizeof(Buffer) - PrefixLength - 1, Format, Args);
    va_end(Args);

    Buffer[sizeof(Buffer) - 1] = ANSI_NULL;
    OutputDebugStringA(Buffer);
}

PCSTR
RilpCommandName(
    _In_ DWORD Command)
{
    PCSTR Name = NULL;

    if (Command < ARRAYSIZE(RilpCommandNames))
        Name = RilpCommandNames[Command];
    else if (Command - RilCmdEnableModemFilters < ARRAYSIZE(RilpModemLogCommandNames))
        Name = RilpModemLogCommandNames[Command - RilCmdEnableModemFilters];

    return Name ? Name : "?";
}

static
PCSTR
RilpResultTypeName(
    _In_ DWORD ResultType)
{
    switch (ResultType)
    {
        case RIL_RESULT_OK:
            return "ok";
        case RIL_RESULT_ERROR:
            return "error";
        default:
            return "?";
    }
}

static
PCSTR
RilpPresenceName(
    _In_ DWORD Presence)
{
    return (Presence == RIL_RADIO_PRESENT) ? "present" : "absent";
}

/* Caller holds DeviceLock, it is dropped around the callback */
static
VOID
RilpNotifyPresenceLocked(
    _In_ PRIL_CLIENT Client,
    _In_ DWORD Presence)
{
    RilpTrace("client %lu: radio presence %lu (%s)\n",
              Client->Index, Presence, RilpPresenceName(Presence));

    LeaveCriticalSection(&Client->DeviceLock);
    if (Client->NotifyCallback)
    {
        Client->NotifyCallback(Client,
                               RIL_NOTIFY_RADIOPRESENCE,
                               &Presence,
                               sizeof(Presence),
                               Client->Param);
    }
    EnterCriticalSection(&Client->DeviceLock);
}

/* Caller holds DeviceLock */
static
BOOL
RilpTakePendingLocked(
    _In_ PRIL_CLIENT Client,
    _In_ ULONGLONG Context,
    _Out_opt_ PDWORD Command)
{
    PLIST_ENTRY Entry;
    PRIL_PENDING Pending;

    for (Entry = Client->Pending.Flink; Entry != &Client->Pending; Entry = Entry->Flink)
    {
        Pending = CONTAINING_RECORD(Entry, RIL_PENDING, Link);
        if (Pending->Context != Context)
            continue;

        RemoveEntryList(&Pending->Link);
        if (Command)
            *Command = Pending->Command;
        HeapFree(GetProcessHeap(), 0, Pending);
        return TRUE;
    }

    return FALSE;
}

/* Caller holds DeviceLock, it is dropped around each result callback */
static
VOID
RilpFailAllPendingLocked(
    _In_ PRIL_CLIENT Client)
{
    RIL_ERROR_RESULT ErrorResult;
    PRIL_PENDING Pending;
    ULONGLONG Context;
    DWORD Command;

    ErrorResult.Size = sizeof(ErrorResult);
    ErrorResult.Params = 0;
    ErrorResult.Error = RIL_E_RADIONOTPRESENT;
    ErrorResult.Reserved = 0;

    while (!IsListEmpty(&Client->Pending))
    {
        Pending = CONTAINING_RECORD(RemoveHeadList(&Client->Pending), RIL_PENDING, Link);
        Context = Pending->Context;
        Command = Pending->Command;
        HeapFree(GetProcessHeap(), 0, Pending);

        RilpTrace("client %lu: failing %s (0x%lx) context 0x%I64x with 0x%08lx\n",
                  Client->Index, RilpCommandName(Command), Command, Context, ErrorResult.Error);

        LeaveCriticalSection(&Client->DeviceLock);
        if (Client->ResultCallback)
        {
            Client->ResultCallback(Client,
                                   RIL_RESULT_ERROR,
                                   Context,
                                   &ErrorResult,
                                   sizeof(ErrorResult),
                                   Client->Param);
        }
        EnterCriticalSection(&Client->DeviceLock);
    }
}

/* Caller holds DeviceLock */
static
VOID
RilpCloseDeviceLocked(
    _In_ PRIL_CLIENT Client)
{
    if (Client->Device == INVALID_HANDLE_VALUE)
        return;

    RilpTrace("client %lu: closing the broker handle %p\n", Client->Index, Client->Device);

    CancelIoEx(Client->Device, NULL);
    if (!CloseHandle(Client->Device))
        RilpTrace("client %lu: CloseHandle failed with %lu\n", Client->Index, GetLastError());

    Client->Device = INVALID_HANDLE_VALUE;
}

/* Caller holds DeviceLock */
static
VOID
RilpDropConnectionLocked(
    _In_ PRIL_CLIENT Client)
{
    RilpCloseDeviceLocked(Client);
    RilpFailAllPendingLocked(Client);
    RilpNotifyPresenceLocked(Client, RIL_RADIO_ABSENT);
}

/* Caller holds DeviceLock */
static
BOOL
RilpIssueReadLocked(
    _In_ PRIL_CLIENT Client)
{
    DWORD Error;

    ZeroMemory(&Client->ReadOverlapped, sizeof(Client->ReadOverlapped));
    Client->ReadOverlapped.hEvent = Client->ReadEvent;

    Client->ReadOutstanding = TRUE;
    if (ReadFile(Client->Device,
                 Client->ReadBuffer,
                 Client->ReadBufferSize,
                 NULL,
                 &Client->ReadOverlapped))
    {
        return TRUE;
    }

    Error = GetLastError();
    if (Error == ERROR_IO_PENDING)
        return TRUE;

    Client->ReadOutstanding = FALSE;
    RilpTrace("client %lu: ReadFile failed with %lu\n", Client->Index, Error);
    return FALSE;
}

/* The interface notifications are not delivered on every system, so keep retrying */
static
VOID
RilpScheduleRetry(
    _In_ PRIL_CLIENT Client)
{
    ULARGE_INTEGER Due;
    FILETIME DueTime;

    Due.QuadPart = (ULONGLONG)(-(LONGLONG)RIL_CONNECT_RETRY_MS * 10000);
    DueTime.dwLowDateTime = Due.LowPart;
    DueTime.dwHighDateTime = Due.HighPart;

    SetThreadpoolTimer(Client->RetryTimer, &DueTime, 0, 0);
}

static
PWSTR
RilpQueryInterfacePath(
    _In_ PRIL_CLIENT Client)
{
    ULONG Length = 0;
    PWSTR List;
    CONFIGRET Cr;

    Cr = CM_Get_Device_Interface_List_SizeW(&Length,
                                            (LPGUID)&GUID_DEVINTERFACE_RIL_CLIENT,
                                            NULL,
                                            CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES);
    if (Cr != CR_SUCCESS)
    {
        RilpTrace("client %lu: interface list size query failed with 0x%lx\n", Client->Index, Cr);
        return NULL;
    }

    /* An empty list is a single terminator */
    if (Length <= 5)
        return NULL;

    List = HeapAlloc(GetProcessHeap(), 0, Length * sizeof(WCHAR));
    if (!List)
        return NULL;

    Cr = CM_Get_Device_Interface_ListW((LPGUID)&GUID_DEVINTERFACE_RIL_CLIENT,
                                       NULL,
                                       List,
                                       Length,
                                       CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES);
    if (Cr != CR_SUCCESS)
    {
        RilpTrace("client %lu: interface list query failed with 0x%lx\n", Client->Index, Cr);
        HeapFree(GetProcessHeap(), 0, List);
        return NULL;
    }

    return List;
}

/* Caller holds DeviceLock */
static
VOID
RilpConnectLocked(
    _In_ PRIL_CLIENT Client)
{
    PWSTR List;
    PCWSTR Path;
    HANDLE Device;
    DWORD Error;

    List = RilpQueryInterfacePath(Client);
    Path = List ? List : RIL_BROKER_FALLBACK_NAME;

    Device = CreateFileW(Path,
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL,
                         OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED,
                         NULL);
    if (Device == INVALID_HANDLE_VALUE)
    {
        Error = GetLastError();
        if (Error != Client->LastConnectError)
        {
            RilpTrace("client %lu: opening %S failed with %lu\n", Client->Index, Path, Error);
            Client->LastConnectError = Error;
        }

        if (List)
            HeapFree(GetProcessHeap(), 0, List);
        return;
    }

    RilpTrace("client %lu: connected to %S%s, handle %p\n",
              Client->Index, Path, List ? "" : " (fallback name)", Device);
    Client->LastConnectError = ERROR_SUCCESS;
    if (List)
        HeapFree(GetProcessHeap(), 0, List);

    SetFileCompletionNotificationModes(Device, FILE_SKIP_SET_EVENT_ON_HANDLE);
    Client->Device = Device;

    ResetEvent(Client->ReadEvent);
    if (!RilpIssueReadLocked(Client))
        RilpCloseDeviceLocked(Client);
}

static
VOID
CALLBACK
RilpConnectSignaled(
    _Inout_ PTP_CALLBACK_INSTANCE Instance,
    _Inout_opt_ PVOID Context,
    _Inout_ PTP_WAIT Wait,
    _In_ TP_WAIT_RESULT WaitResult)
{
    PRIL_CLIENT Client = Context;

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(WaitResult);

    AcquireSRWLockExclusive(&Client->CallbackLock);
    EnterCriticalSection(&Client->DeviceLock);

    if (Client->ShuttingDown)
    {
        RilpTrace("client %lu: shutting down\n", Client->Index);
        RilpCloseDeviceLocked(Client);
        RilpFailAllPendingLocked(Client);
        SetEvent(Client->ShutdownDoneEvent);
        goto Exit;
    }

    SetThreadpoolWait(Wait, Client->ConnectEvent, NULL);

    if (Client->Device != INVALID_HANDLE_VALUE)
        goto Exit;

    RilpConnectLocked(Client);

    if (!Client->Initialized)
    {
        /* Initialize returns once the first attempt is done, presence follows from here */
        SetEvent(Client->InitDoneEvent);
        RilpNotifyPresenceLocked(Client,
                                 (Client->Device != INVALID_HANDLE_VALUE) ?
                                 RIL_RADIO_PRESENT : RIL_RADIO_ABSENT);
    }
    else if (Client->Device != INVALID_HANDLE_VALUE)
    {
        RilpNotifyPresenceLocked(Client, RIL_RADIO_PRESENT);
    }

    if (Client->Device == INVALID_HANDLE_VALUE && !Client->ShuttingDown)
        RilpScheduleRetry(Client);

Exit:
    LeaveCriticalSection(&Client->DeviceLock);
    ReleaseSRWLockExclusive(&Client->CallbackLock);
}

static
VOID
CALLBACK
RilpDisconnectSignaled(
    _Inout_ PTP_CALLBACK_INSTANCE Instance,
    _Inout_opt_ PVOID Context,
    _Inout_ PTP_WAIT Wait,
    _In_ TP_WAIT_RESULT WaitResult)
{
    PRIL_CLIENT Client = Context;

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(WaitResult);

    AcquireSRWLockExclusive(&Client->CallbackLock);
    EnterCriticalSection(&Client->DeviceLock);

    if (!Client->ShuttingDown)
    {
        SetThreadpoolWait(Wait, Client->DisconnectEvent, NULL);

        RilpTrace("client %lu: disconnecting after a failed write\n", Client->Index);
        RilpDropConnectionLocked(Client);
        if (!Client->ShuttingDown)
            RilpScheduleRetry(Client);
    }

    LeaveCriticalSection(&Client->DeviceLock);
    ReleaseSRWLockExclusive(&Client->CallbackLock);
}

/* Caller holds DeviceLock. Returns FALSE when the record can not be handled. */
static
BOOL
RilpDispatchRecordLocked(
    _In_ PRIL_CLIENT Client,
    _In_reads_bytes_(Length) PBYTE Record,
    _In_ DWORD Length)
{
    PRIL_RESPONSE_RECORD Response;
    PRIL_NOTIFICATION_RECORD Notification;
    PRIL_ERROR_RESULT ErrorResult;
    DWORD PayloadLength;
    PBYTE Payload;
    DWORD Command;
    DWORD Type = (Length >= sizeof(DWORD)) ? *(PDWORD)Record : 0;

    if (Type == RIL_RECORD_RESPONSE && Length >= sizeof(*Response))
    {
        Response = (PRIL_RESPONSE_RECORD)Record;
        PayloadLength = Length - sizeof(*Response);
        Payload = PayloadLength ? Record + sizeof(*Response) : NULL;

        if (!RilpTakePendingLocked(Client, Response->Context, &Command))
        {
            RilpTrace("client %lu: response to command 0x%lx for unknown context 0x%I64x dropped\n",
                      Client->Index, Response->Command, Response->Context);
            return TRUE;
        }

        if (Response->ResultType == RIL_RESULT_ERROR && PayloadLength >= sizeof(*ErrorResult))
        {
            ErrorResult = (PRIL_ERROR_RESULT)Payload;
            RilpTrace("client %lu: response %s (0x%lx) context 0x%I64x result %lu (%s) hr 0x%08lx\n",
                      Client->Index, RilpCommandName(Command), Command, Response->Context,
                      Response->ResultType, RilpResultTypeName(Response->ResultType),
                      ErrorResult->Error);
        }
        else
        {
            RilpTrace("client %lu: response %s (0x%lx) context 0x%I64x result %lu (%s) %lu bytes\n",
                      Client->Index, RilpCommandName(Command), Command, Response->Context,
                      Response->ResultType, RilpResultTypeName(Response->ResultType),
                      PayloadLength);
        }

        LeaveCriticalSection(&Client->DeviceLock);
        if (Client->ResultCallback)
        {
            Client->ResultCallback(Client,
                                   Response->ResultType,
                                   Response->Context,
                                   Payload,
                                   PayloadLength,
                                   Client->Param);
        }
        EnterCriticalSection(&Client->DeviceLock);
        return TRUE;
    }

    if (Type == RIL_RECORD_NOTIFICATION && Length >= sizeof(*Notification))
    {
        Notification = (PRIL_NOTIFICATION_RECORD)Record;
        PayloadLength = Length - sizeof(*Notification);
        Payload = PayloadLength ? Record + sizeof(*Notification) : NULL;

        RilpTrace("client %lu: notification 0x%08lx, %lu bytes\n",
                  Client->Index, Notification->Code, PayloadLength);

        LeaveCriticalSection(&Client->DeviceLock);
        if (Client->NotifyCallback)
        {
            Client->NotifyCallback(Client,
                                   Notification->Code,
                                   Payload,
                                   PayloadLength,
                                   Client->Param);
        }
        EnterCriticalSection(&Client->DeviceLock);
        return TRUE;
    }

    /* Windows fail fasts the process here, dropping the connection is enough */
    RilpTrace("client %lu: ERROR: malformed record type %lu length %lu from the broker\n",
              Client->Index, Type, Length);
    return FALSE;
}

/* Caller holds DeviceLock. Returns FALSE when the connection has to be dropped. */
static
BOOL
RilpGrowReadBufferLocked(
    _In_ PRIL_CLIENT Client)
{
    DWORD NewSize = Client->ReadBufferSize * 2;
    PBYTE NewBuffer;

    RilpTrace("client %lu: record larger than %lu bytes, growing the read buffer\n",
              Client->Index, Client->ReadBufferSize);

    /* The broker keeps the record queued, so nothing has to be carried over */
    NewBuffer = HeapAlloc(GetProcessHeap(), 0, NewSize);
    if (!NewBuffer)
    {
        RilpTrace("client %lu: no memory for a %lu byte read buffer\n", Client->Index, NewSize);
        return FALSE;
    }

    HeapFree(GetProcessHeap(), 0, Client->ReadBuffer);
    Client->ReadBuffer = NewBuffer;
    Client->ReadBufferSize = NewSize;

    return RilpIssueReadLocked(Client);
}

static
VOID
CALLBACK
RilpReadSignaled(
    _Inout_ PTP_CALLBACK_INSTANCE Instance,
    _Inout_opt_ PVOID Context,
    _Inout_ PTP_WAIT Wait,
    _In_ TP_WAIT_RESULT WaitResult)
{
    PRIL_CLIENT Client = Context;
    DWORD Transferred = 0;
    DWORD Error;
    BOOL Keep = FALSE;

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(WaitResult);

    AcquireSRWLockExclusive(&Client->CallbackLock);
    EnterCriticalSection(&Client->DeviceLock);

    if (Client->ShuttingDown)
    {
        if (HasOverlappedIoCompleted(&Client->ReadOverlapped))
            Client->ReadOutstanding = FALSE;
        goto Exit;
    }

    SetThreadpoolWait(Wait, Client->ReadEvent, NULL);

    if (Client->Device == INVALID_HANDLE_VALUE)
    {
        /* The handle went away under the read, through removal or a failed write */
        Client->ReadOutstanding = FALSE;
        ResetEvent(Client->ReadEvent);
        RilpTrace("client %lu: read ended on a closed handle\n", Client->Index);
        goto Drop;
    }

    if (GetOverlappedResult(Client->Device, &Client->ReadOverlapped, &Transferred, FALSE))
    {
        Client->ReadOutstanding = FALSE;
        if (!RilpDispatchRecordLocked(Client, Client->ReadBuffer, Transferred))
            goto Drop;

        if (Client->Device == INVALID_HANDLE_VALUE)
        {
            RilpTrace("client %lu: handle closed while dispatching\n", Client->Index);
            goto Drop;
        }

        Keep = RilpIssueReadLocked(Client);
        goto Done;
    }

    Error = GetLastError();
    switch (Error)
    {
        case ERROR_IO_INCOMPLETE:
            RilpTrace("client %lu: read signaled before it completed\n", Client->Index);
            goto Exit;

        case ERROR_INSUFFICIENT_BUFFER:
            Client->ReadOutstanding = FALSE;
            Keep = RilpGrowReadBufferLocked(Client);
            goto Done;

        case ERROR_NOT_READY:
        case ERROR_OPERATION_ABORTED:
        case ERROR_CANCELLED:
            Client->ReadOutstanding = FALSE;
            RilpTrace("client %lu: read from the broker ended with %lu\n", Client->Index, Error);
            goto Drop;

        default:
            /* Windows fail fasts the process on anything else */
            Client->ReadOutstanding = FALSE;
            RilpTrace("client %lu: ERROR: read from the broker failed with %lu\n", Client->Index, Error);
            goto Drop;
    }

Done:
    if (Keep)
        goto Exit;

Drop:
    RilpDropConnectionLocked(Client);
    if (!Client->ShuttingDown)
        RilpScheduleRetry(Client);

Exit:
    LeaveCriticalSection(&Client->DeviceLock);
    ReleaseSRWLockExclusive(&Client->CallbackLock);
}

static
VOID
CALLBACK
RilpRetryTimer(
    _Inout_ PTP_CALLBACK_INSTANCE Instance,
    _Inout_opt_ PVOID Context,
    _Inout_ PTP_TIMER Timer)
{
    PRIL_CLIENT Client = Context;

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(Timer);

    SetEvent(Client->ConnectEvent);
}

static
DWORD
CALLBACK
RilpInterfaceChanged(
    _In_ HCMNOTIFICATION Notify,
    _In_opt_ PVOID Context,
    _In_ CM_NOTIFY_ACTION Action,
    _In_reads_bytes_(EventDataSize) PCM_NOTIFY_EVENT_DATA EventData,
    _In_ DWORD EventDataSize)
{
    PRIL_CLIENT Client = Context;

    UNREFERENCED_PARAMETER(Notify);
    UNREFERENCED_PARAMETER(EventDataSize);

    if (!Client)
        return ERROR_INVALID_PARAMETER;

    switch (Action)
    {
        case CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL:
            RilpTrace("client %lu: broker interface arrived: %S\n",
                      Client->Index, EventData->u.DeviceInterface.SymbolicLink);
            SetEvent(Client->ConnectEvent);
            break;

        case CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL:
            RilpTrace("client %lu: broker interface removed: %S\n",
                      Client->Index, EventData->u.DeviceInterface.SymbolicLink);

            /* The read completes aborted and its callback reports the loss */
            EnterCriticalSection(&Client->DeviceLock);
            RilpCloseDeviceLocked(Client);
            LeaveCriticalSection(&Client->DeviceLock);
            break;

        default:
            RilpTrace("client %lu: interface notification action %d ignored\n", Client->Index, Action);
            break;
    }

    return ERROR_SUCCESS;
}

HRESULT
RilpSend(
    _In_ HRIL hRil,
    _In_ DWORD Command,
    _In_ ULONGLONG Context,
    _Inout_updates_bytes_(Size) PVOID Block,
    _In_ DWORD Size)
{
    PRIL_CLIENT Client = hRil;
    PRIL_COMMAND_RECORD Header = Block;
    PRIL_PENDING Pending;
    DWORD Written = 0;
    DWORD Error;
    BOOL Ok;
    HRESULT hr = RIL_E_RADIONOTPRESENT;

    if (!Client)
        return RIL_E_RADIONOTPRESENT;

    Header->Type = RIL_RECORD_COMMAND;
    Header->Command = Command;
    Header->Context = Context;

    Pending = HeapAlloc(GetProcessHeap(), 0, sizeof(*Pending));
    if (!Pending)
        return E_OUTOFMEMORY;

    Pending->Context = Context;
    Pending->Command = Command;

    EnterCriticalSection(&Client->SendLock);
    EnterCriticalSection(&Client->DeviceLock);

    if (Client->Device == INVALID_HANDLE_VALUE || Client->ShuttingDown)
    {
        RilpTrace("client %lu: send %s (0x%lx) context 0x%I64x refused, no broker connection\n",
                  Client->Index, RilpCommandName(Command), Command, Context);
        LeaveCriticalSection(&Client->DeviceLock);
        LeaveCriticalSection(&Client->SendLock);
        HeapFree(GetProcessHeap(), 0, Pending);
        return hr;
    }

    /* Track it before writing so a fast response always finds its entry */
    InsertTailList(&Client->Pending, &Pending->Link);

    ResetEvent(Client->WriteEvent);
    ZeroMemory(&Client->WriteOverlapped, sizeof(Client->WriteOverlapped));
    Client->WriteOverlapped.hEvent = Client->WriteEvent;

    RilpTrace("client %lu: send %s (0x%lx) context 0x%I64x, %lu bytes\n",
              Client->Index, RilpCommandName(Command), Command, Context, Size);

    Ok = WriteFile(Client->Device, Block, Size, NULL, &Client->WriteOverlapped);
    Error = GetLastError();
    LeaveCriticalSection(&Client->DeviceLock);

    if (!Ok && Error != ERROR_IO_PENDING)
    {
        RilpTrace("client %lu: WriteFile failed with %lu\n", Client->Index, Error);
        goto Failed;
    }

    if (WaitForSingleObject(Client->WriteEvent, INFINITE) != WAIT_OBJECT_0)
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
    }
    else
    {
        EnterCriticalSection(&Client->DeviceLock);
        if (Client->Device == INVALID_HANDLE_VALUE)
        {
            hr = HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_AVAILABLE);
        }
        else if (!GetOverlappedResult(Client->Device, &Client->WriteOverlapped, &Written, FALSE))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
        }
        else if (Written != Size)
        {
            /* Windows fail fasts on a short write */
            RilpTrace("client %lu: ERROR: short write, %lu of %lu bytes\n", Client->Index, Written, Size);
            hr = HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        }
        else
        {
            hr = S_OK;
        }
        LeaveCriticalSection(&Client->DeviceLock);
    }

    if (SUCCEEDED(hr))
        goto Exit;

Failed:
    EnterCriticalSection(&Client->DeviceLock);
    RilpTrace("client %lu: send %s (0x%lx) context 0x%I64x failed with 0x%08lx\n",
              Client->Index, RilpCommandName(Command), Command, Context, hr);

    if (hr != E_ACCESSDENIED && hr != E_INVALIDARG)
    {
        /* The handle is dropped from the threadpool, outside of this call */
        if (Client->Device != INVALID_HANDLE_VALUE)
            SetEvent(Client->DisconnectEvent);
        hr = RIL_E_RADIONOTPRESENT;
    }

    RilpTakePendingLocked(Client, Context, NULL);
    LeaveCriticalSection(&Client->DeviceLock);

Exit:
    LeaveCriticalSection(&Client->SendLock);
    return hr;
}

HRESULT
RilpSendHeaderOnly(
    _In_ HRIL hRil,
    _In_ DWORD Command,
    _In_ ULONGLONG Context)
{
    RIL_COMMAND_RECORD Header;

    return RilpSend(hRil, Command, Context, &Header, sizeof(Header));
}

HRESULT
RilpSendVariable(
    _In_ HRIL hRil,
    _In_ DWORD Command,
    _In_ ULONGLONG Context,
    _In_reads_bytes_opt_(PrefixSize) const VOID *Prefix,
    _In_ DWORD PrefixSize,
    _In_reads_bytes_opt_(DataSize) const VOID *Data,
    _In_ DWORD DataSize)
{
    DWORD Size;
    PBYTE Block;
    HRESULT hr;

    Size = sizeof(RIL_COMMAND_RECORD) + PrefixSize + DataSize;
    if (Size < PrefixSize || Size > RIL_RECORD_MAX_LENGTH)
        return E_INVALIDARG;

    Block = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (!Block)
        return E_OUTOFMEMORY;

    if (PrefixSize)
        CopyMemory(Block + sizeof(RIL_COMMAND_RECORD), Prefix, PrefixSize);
    if (DataSize)
        CopyMemory(Block + sizeof(RIL_COMMAND_RECORD) + PrefixSize, Data, DataSize);

    hr = RilpSend(hRil, Command, Context, Block, Size);
    HeapFree(GetProcessHeap(), 0, Block);
    return hr;
}

HRESULT
RilpCreateClient(
    _In_ DWORD Index,
    _In_opt_ PRIL_RESULT_CALLBACK ResultCallback,
    _In_opt_ PRIL_NOTIFY_CALLBACK NotifyCallback,
    _In_opt_ PVOID Param,
    _Out_ PRIL_CLIENT *Client)
{
    PRIL_CLIENT NewClient;
    CM_NOTIFY_FILTER Filter;
    CONFIGRET Cr;

    *Client = NULL;

    NewClient = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*NewClient));
    if (!NewClient)
        return E_FAIL;

    NewClient->Index = Index;
    NewClient->ResultCallback = ResultCallback;
    NewClient->NotifyCallback = NotifyCallback;
    NewClient->Param = Param;
    NewClient->Device = INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&NewClient->SendLock);
    InitializeCriticalSection(&NewClient->DeviceLock);
    InitializeSRWLock(&NewClient->CallbackLock);
    InitializeListHead(&NewClient->Pending);

    NewClient->ReadBufferSize = RIL_INITIAL_READ_SIZE;
    NewClient->ReadBuffer = HeapAlloc(GetProcessHeap(), 0, NewClient->ReadBufferSize);

    NewClient->InitDoneEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    NewClient->ShutdownDoneEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    NewClient->ConnectEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    NewClient->DisconnectEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    NewClient->ReadEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    NewClient->WriteEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    NewClient->ConnectWait = CreateThreadpoolWait(RilpConnectSignaled, NewClient, NULL);
    NewClient->DisconnectWait = CreateThreadpoolWait(RilpDisconnectSignaled, NewClient, NULL);
    NewClient->ReadWait = CreateThreadpoolWait(RilpReadSignaled, NewClient, NULL);
    NewClient->RetryTimer = CreateThreadpoolTimer(RilpRetryTimer, NewClient, NULL);

    if (!NewClient->ReadBuffer ||
        !NewClient->InitDoneEvent || !NewClient->ShutdownDoneEvent ||
        !NewClient->ConnectEvent || !NewClient->DisconnectEvent ||
        !NewClient->ReadEvent || !NewClient->WriteEvent ||
        !NewClient->ConnectWait || !NewClient->DisconnectWait ||
        !NewClient->ReadWait || !NewClient->RetryTimer)
    {
        RilpTrace("client %lu: out of resources while initializing\n", Index);
        RilpDestroyClient(NewClient);
        return E_FAIL;
    }

    /* The caller has the handle before any callback can run */
    *Client = NewClient;

    ZeroMemory(&Filter, sizeof(Filter));
    Filter.cbSize = sizeof(Filter);
    Filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    Filter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_RIL_CLIENT;

    Cr = CM_Register_Notification(&Filter, NewClient, RilpInterfaceChanged, &NewClient->InterfaceNotify);
    if (Cr != CR_SUCCESS)
    {
        RilpTrace("client %lu: interface notification registration failed with 0x%lx\n", Index, Cr);
        NewClient->InterfaceNotify = NULL;
    }

    SetThreadpoolWait(NewClient->ConnectWait, NewClient->ConnectEvent, NULL);
    SetThreadpoolWait(NewClient->DisconnectWait, NewClient->DisconnectEvent, NULL);
    SetThreadpoolWait(NewClient->ReadWait, NewClient->ReadEvent, NULL);
    NewClient->Started = TRUE;

    RilpTrace("client %lu: initializing\n", Index);
    SetEvent(NewClient->ConnectEvent);
    WaitForSingleObject(NewClient->InitDoneEvent, INFINITE);

    EnterCriticalSection(&NewClient->DeviceLock);
    NewClient->Initialized = TRUE;
    LeaveCriticalSection(&NewClient->DeviceLock);

    return S_OK;
}

VOID
RilpDestroyClient(
    _In_ PRIL_CLIENT Client)
{
    PRIL_PENDING Pending;

    RilpTrace("client %lu: deinitializing\n", Client->Index);

    if (Client->InterfaceNotify)
        CM_Unregister_Notification(Client->InterfaceNotify);

    EnterCriticalSection(&Client->DeviceLock);
    Client->ShuttingDown = TRUE;
    if (Client->Started)
        SetEvent(Client->ConnectEvent);
    LeaveCriticalSection(&Client->DeviceLock);

    /* The connect callback closes the handle and fails what is still in flight */
    if (Client->Started)
        WaitForSingleObject(Client->ShutdownDoneEvent, INFINITE);

    if (Client->RetryTimer)
    {
        SetThreadpoolTimer(Client->RetryTimer, NULL, 0, 0);
        WaitForThreadpoolTimerCallbacks(Client->RetryTimer, TRUE);
        CloseThreadpoolTimer(Client->RetryTimer);
    }

    if (Client->ConnectWait)
    {
        SetThreadpoolWait(Client->ConnectWait, NULL, NULL);
        WaitForThreadpoolWaitCallbacks(Client->ConnectWait, TRUE);
        CloseThreadpoolWait(Client->ConnectWait);
    }

    if (Client->DisconnectWait)
    {
        SetThreadpoolWait(Client->DisconnectWait, NULL, NULL);
        WaitForThreadpoolWaitCallbacks(Client->DisconnectWait, TRUE);
        CloseThreadpoolWait(Client->DisconnectWait);
    }

    if (Client->ReadWait)
    {
        /* Let a callback that already took the event run, it clears ReadOutstanding */
        SetThreadpoolWait(Client->ReadWait, NULL, NULL);
        WaitForThreadpoolWaitCallbacks(Client->ReadWait, FALSE);
        CloseThreadpoolWait(Client->ReadWait);
    }

    EnterCriticalSection(&Client->DeviceLock);
    RilpCloseDeviceLocked(Client);
    LeaveCriticalSection(&Client->DeviceLock);

    /* The cancelled read owns the buffer until its event fires */
    if (Client->ReadOutstanding && Client->ReadEvent)
        WaitForSingleObject(Client->ReadEvent, INFINITE);

    while (!IsListEmpty(&Client->Pending))
    {
        Pending = CONTAINING_RECORD(RemoveHeadList(&Client->Pending), RIL_PENDING, Link);
        HeapFree(GetProcessHeap(), 0, Pending);
    }

    if (Client->InitDoneEvent)
        CloseHandle(Client->InitDoneEvent);
    if (Client->ShutdownDoneEvent)
        CloseHandle(Client->ShutdownDoneEvent);
    if (Client->ConnectEvent)
        CloseHandle(Client->ConnectEvent);
    if (Client->DisconnectEvent)
        CloseHandle(Client->DisconnectEvent);
    if (Client->ReadEvent)
        CloseHandle(Client->ReadEvent);
    if (Client->WriteEvent)
        CloseHandle(Client->WriteEvent);
    if (Client->ReadBuffer)
        HeapFree(GetProcessHeap(), 0, Client->ReadBuffer);

    DeleteCriticalSection(&Client->DeviceLock);
    DeleteCriticalSection(&Client->SendLock);
    HeapFree(GetProcessHeap(), 0, Client);
}
