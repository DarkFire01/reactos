/*
 * PROJECT:     ReactOS Phone Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Service entry and control
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "phonesvc.h"

#define NDEBUG
#include <debug.h>

HANDLE PhStopEvent;

static SERVICE_STATUS_HANDLE PhStatusHandle;
static SERVICE_STATUS PhStatus;

static
VOID
PhReportStatus(
    _In_ DWORD State,
    _In_ DWORD ExitCode,
    _In_ DWORD WaitHint)
{
    PhStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    PhStatus.dwCurrentState = State;
    PhStatus.dwWin32ExitCode = ExitCode;
    PhStatus.dwWaitHint = WaitHint;
    PhStatus.dwControlsAccepted = (State == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;

    if (State == SERVICE_RUNNING || State == SERVICE_STOPPED)
        PhStatus.dwCheckPoint = 0;
    else
        PhStatus.dwCheckPoint++;

    SetServiceStatus(PhStatusHandle, &PhStatus);
}

static
DWORD
WINAPI
PhServiceControl(
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
            PhReportStatus(SERVICE_STOP_PENDING, NO_ERROR, PHONE_RIL_TIMEOUT_MS + 5000);
            SetEvent(PhStopEvent);
            return NO_ERROR;

        case SERVICE_CONTROL_INTERROGATE:
            SetServiceStatus(PhStatusHandle, &PhStatus);
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static
VOID
WINAPI
PhServiceMain(
    _In_ DWORD Argc,
    _In_ LPWSTR *Argv)
{
    UNREFERENCED_PARAMETER(Argc);
    UNREFERENCED_PARAMETER(Argv);

    PhStatusHandle = RegisterServiceCtrlHandlerExW(PHONE_SERVICE_NAME, PhServiceControl, NULL);
    if (!PhStatusHandle)
        return;

    PhReportStatus(SERVICE_START_PENDING, NO_ERROR, 10000);

    PhStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!PhStopEvent)
    {
        PhReportStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    PhCallsInitialize();
    PhSmsInitialize();
    PhAudioInitialize();
    PhStoreInitialize();

    if (!PhPipeStart())
    {
        PhReportStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    /* rilproxy keeps retrying in the background, a radio that shows up later is picked up then */
    if (!PhRadioStart())
        DPRINT1("No RIL session, phone functions are unavailable\n");

    PhReportStatus(SERVICE_RUNNING, NO_ERROR, 0);

    WaitForSingleObject(PhStopEvent, INFINITE);

    PhRadioStop();
    PhPipeStop();
    PhAudioUninitialize();

    PhReportStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

int
wmain(
    _In_ int argc,
    _In_ WCHAR *argv[])
{
    SERVICE_TABLE_ENTRYW Table[] =
    {
        { PHONE_SERVICE_NAME, PhServiceMain },
        { NULL, NULL }
    };

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    return StartServiceCtrlDispatcherW(Table) ? 0 : (int)GetLastError();
}
