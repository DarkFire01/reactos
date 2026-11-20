/*
 * PROJECT:     ReactOS Power Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Service entry and control
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "powersvc.h"

#define NDEBUG
#include <debug.h>

HANDLE PwStopEvent;

static SERVICE_STATUS_HANDLE PwStatusHandle;
static SERVICE_STATUS PwStatus;

static
VOID
PwReportStatus(
    _In_ DWORD State,
    _In_ DWORD ExitCode,
    _In_ DWORD WaitHint)
{
    PwStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    PwStatus.dwCurrentState = State;
    PwStatus.dwWin32ExitCode = ExitCode;
    PwStatus.dwWaitHint = WaitHint;
    PwStatus.dwControlsAccepted = (State == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;

    if (State == SERVICE_RUNNING || State == SERVICE_STOPPED)
        PwStatus.dwCheckPoint = 0;
    else
        PwStatus.dwCheckPoint++;

    SetServiceStatus(PwStatusHandle, &PwStatus);
}

static
DWORD
WINAPI
PwServiceControl(
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
            PwReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
            SetEvent(PwStopEvent);
            return NO_ERROR;

        case SERVICE_CONTROL_INTERROGATE:
            SetServiceStatus(PwStatusHandle, &PwStatus);
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static
VOID
WINAPI
PwServiceMain(
    _In_ DWORD Argc,
    _In_ LPWSTR *Argv)
{
    HANDLE Waits[2];

    UNREFERENCED_PARAMETER(Argc);
    UNREFERENCED_PARAMETER(Argv);

    PwStatusHandle = RegisterServiceCtrlHandlerExW(POWER_SERVICE_NAME, PwServiceControl, NULL);
    if (!PwStatusHandle)
        return;

    PwReportStatus(SERVICE_START_PENDING, NO_ERROR, 10000);

    PwStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!PwStopEvent || !PwSchemeInitialize())
    {
        PwReportStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    /* Drivers that registered during boot get their first value from this push */
    PwSchemeWatch();
    PwSchemePushAll();

    PwReportStatus(SERVICE_RUNNING, NO_ERROR, 0);

    Waits[0] = PwStopEvent;
    Waits[1] = PwSchemeChangeEvent();
    while (WaitForMultipleObjects(ARRAYSIZE(Waits), Waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
    {
        /* Watch again first, so a change made during the push is not missed */
        PwSchemeWatch();
        PwSchemePushAll();
    }

    PwSchemeUninitialize();
    PwReportStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

int
wmain(
    _In_ int argc,
    _In_ WCHAR *argv[])
{
    SERVICE_TABLE_ENTRYW Table[] =
    {
        { POWER_SERVICE_NAME, PwServiceMain },
        { NULL, NULL }
    };

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    if (!StartServiceCtrlDispatcherW(Table))
        return (int)GetLastError();

    return 0;
}
