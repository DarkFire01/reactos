/*
 * PROJECT:     ReactOS GNSS Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Service entry and control
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "gnsssvc.h"

#define NDEBUG
#include <debug.h>

HANDLE GsStopEvent;

static SERVICE_STATUS_HANDLE GsStatusHandle;
static SERVICE_STATUS GsStatus;

static
VOID
GsReportStatus(
    _In_ DWORD State,
    _In_ DWORD ExitCode,
    _In_ DWORD WaitHint)
{
    GsStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    GsStatus.dwCurrentState = State;
    GsStatus.dwWin32ExitCode = ExitCode;
    GsStatus.dwWaitHint = WaitHint;
    GsStatus.dwControlsAccepted = (State == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;

    if (State == SERVICE_RUNNING || State == SERVICE_STOPPED)
        GsStatus.dwCheckPoint = 0;
    else
        GsStatus.dwCheckPoint++;

    SetServiceStatus(GsStatusHandle, &GsStatus);
}

static
DWORD
WINAPI
GsServiceControl(
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
            GsReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 15000);
            SetEvent(GsStopEvent);
            return NO_ERROR;

        case SERVICE_CONTROL_INTERROGATE:
            SetServiceStatus(GsStatusHandle, &GsStatus);
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static
VOID
WINAPI
GsServiceMain(
    _In_ DWORD Argc,
    _In_ LPWSTR *Argv)
{
    UNREFERENCED_PARAMETER(Argc);
    UNREFERENCED_PARAMETER(Argv);

    GsStatusHandle = RegisterServiceCtrlHandlerExW(GS_SERVICE_NAME, GsServiceControl, NULL);
    if (!GsStatusHandle)
        return;

    GsReportStatus(SERVICE_START_PENDING, NO_ERROR, 10000);

    GsStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!GsStopEvent)
    {
        GsReportStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    /* The modem LOC service may come much later, the worker keeps looking for it */
    if (!GsLocInit() || !GsPipeStart() || !GsLocStart())
    {
        DPRINT1("GNSS: service setup failed %lu\n", GetLastError());
        SetEvent(GsStopEvent);
        GsLocStop();
        GsPipeStop();
        GsReportStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    GsReportStatus(SERVICE_RUNNING, NO_ERROR, 0);
    DPRINT1("GNSS: service running, idle until a client asks for location\n");

    WaitForSingleObject(GsStopEvent, INFINITE);

    GsLocStop();
    GsPipeStop();

    GsReportStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

int
wmain(
    _In_ int argc,
    _In_ WCHAR *argv[])
{
    SERVICE_TABLE_ENTRYW Table[] =
    {
        { GS_SERVICE_NAME, GsServiceMain },
        { NULL, NULL }
    };

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    if (!StartServiceCtrlDispatcherW(Table))
        return (int)GetLastError();

    return 0;
}
