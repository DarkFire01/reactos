/*
 * PROJECT:     ReactOS Native Time Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Service entry and control
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "nativetimesvc.h"

#define NDEBUG
#include <debug.h>

HANDLE NtStopEvent;

static SERVICE_STATUS_HANDLE NtStatusHandle;
static SERVICE_STATUS NtStatus;

static
VOID
NtReportStatus(
    _In_ DWORD State,
    _In_ DWORD ExitCode,
    _In_ DWORD WaitHint)
{
    NtStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    NtStatus.dwCurrentState = State;
    NtStatus.dwWin32ExitCode = ExitCode;
    NtStatus.dwWaitHint = WaitHint;
    NtStatus.dwControlsAccepted = (State == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;

    if (State == SERVICE_RUNNING || State == SERVICE_STOPPED)
        NtStatus.dwCheckPoint = 0;
    else
        NtStatus.dwCheckPoint++;

    SetServiceStatus(NtStatusHandle, &NtStatus);
}

static
DWORD
WINAPI
NtServiceControl(
    _In_ DWORD Control,
    _In_ DWORD EventType,
    _In_ LPVOID EventData,
    _In_ LPVOID Context)
{
    UNREFERENCED_PARAMETER(EventType);
    UNREFERENCED_PARAMETER(EventData);
    UNREFERENCED_PARAMETER(Context);

    switch (Control)
    {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            NtReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 10000);
            SetEvent(NtStopEvent);
            return NO_ERROR;

        case SERVICE_CONTROL_INTERROGATE:
            SetServiceStatus(NtStatusHandle, &NtStatus);
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static
VOID
WINAPI
NtServiceMain(
    _In_ DWORD Argc,
    _In_ LPWSTR *Argv)
{
    UNREFERENCED_PARAMETER(Argc);
    UNREFERENCED_PARAMETER(Argv);

    NtStatusHandle = RegisterServiceCtrlHandlerExW(NT_SERVICE_NAME, NtServiceControl, NULL);
    if (!NtStatusHandle)
        return;

    NtReportStatus(SERVICE_START_PENDING, NO_ERROR, 10000);

    NtStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!NtStopEvent)
    {
        NtReportStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    if (!NtTriggersStart() || !NtPipeStart())
    {
        NtReportStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    /* The radio may come later, rilproxy keeps looking for it */
    if (!NtClockStart())
        DPRINT1("No RIL session, network time is unavailable\n");

    NtReportStatus(SERVICE_RUNNING, NO_ERROR, 0);
    DPRINT1("Native time service running\n");

    WaitForSingleObject(NtStopEvent, INFINITE);

    NtClockStop();
    NtPipeStop();
    NtTriggersStop();

    NtReportStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

int
wmain(
    _In_ int argc,
    _In_ WCHAR *argv[])
{
    SERVICE_TABLE_ENTRYW Table[] =
    {
        { NT_SERVICE_NAME, NtServiceMain },
        { NULL, NULL }
    };

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    if (!StartServiceCtrlDispatcherW(Table))
        return (int)GetLastError();

    return 0;
}
