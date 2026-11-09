/*
 * PROJECT:     ReactOS RIL Adaptation Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Service entry and control
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "riladapt.h"

#define NDEBUG
#include <debug.h>

static SERVICE_STATUS_HANDLE StatusHandle;
static SERVICE_STATUS Status;
static HANDLE StopEvent;
static RIL_MODEM Modems[RILADAPT_MAX_MODEMS];
static DWORD ModemCount;

static
VOID
RilReportStatus(
    _In_ DWORD State,
    _In_ DWORD ExitCode,
    _In_ DWORD WaitHint)
{
    Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    Status.dwCurrentState = State;
    Status.dwWin32ExitCode = ExitCode;
    Status.dwWaitHint = WaitHint;
    Status.dwControlsAccepted = (State == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;

    if (State == SERVICE_RUNNING || State == SERVICE_STOPPED)
        Status.dwCheckPoint = 0;
    else
        Status.dwCheckPoint++;

    SetServiceStatus(StatusHandle, &Status);
}

/* rilproxy reports this many modems to its callers */
static
VOID
RilPublishInstanceCount(
    _In_ DWORD Count)
{
    HKEY Key;

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, RILADAPT_PARAMETERS_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &Key, NULL) != ERROR_SUCCESS)
    {
        return;
    }

    RegSetValueExW(Key, L"MSRilInstances", 0, REG_DWORD, (PBYTE)&Count, sizeof(Count));
    RegCloseKey(Key);
}

static
DWORD
WINAPI
RilServiceControl(
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
            RilReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
            SetEvent(StopEvent);
            return NO_ERROR;

        case SERVICE_CONTROL_INTERROGATE:
            SetServiceStatus(StatusHandle, &Status);
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static
VOID
WINAPI
RilServiceMain(
    _In_ DWORD Argc,
    _In_ LPWSTR *Argv)
{
    RIL_MODEM_CONFIG Configs[RILADAPT_MAX_MODEMS];
    DWORD Found, i, Error;

    UNREFERENCED_PARAMETER(Argc);
    UNREFERENCED_PARAMETER(Argv);

    StatusHandle = RegisterServiceCtrlHandlerExW(RILADAPT_SERVICE_NAME, RilServiceControl, NULL);
    if (!StatusHandle)
        return;

    RilReportStatus(SERVICE_START_PENDING, NO_ERROR, 30000);

    StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!StopEvent)
    {
        RilReportStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    RilPublishInstanceCount(0);

    Found = RilFindModems(Configs, _countof(Configs));
    for (i = 0; i < Found; i++)
    {
        CopyMemory(&Modems[ModemCount].Config, &Configs[i], sizeof(Configs[i]));
        Error = RilStartModem(&Modems[ModemCount], StopEvent);
        if (Error != ERROR_SUCCESS)
        {
            DPRINT1("Modem %lu failed to start (%lu)\n", i, Error);
            RilStopModem(&Modems[ModemCount]);
            ZeroMemory(&Modems[ModemCount], sizeof(Modems[ModemCount]));
            continue;
        }

        ModemCount++;
        RilReportStatus(SERVICE_START_PENDING, NO_ERROR, 30000);
    }

    /* A device without a cellular radio has nothing to host */
    if (ModemCount == 0)
    {
        CloseHandle(StopEvent);
        RilReportStatus(SERVICE_STOPPED, NO_ERROR, 0);
        return;
    }

    RilPublishInstanceCount(ModemCount);
    RilReportStatus(SERVICE_RUNNING, NO_ERROR, 0);

    WaitForSingleObject(StopEvent, INFINITE);

    for (i = 0; i < ModemCount; i++)
        RilStopModem(&Modems[i]);

    RilPublishInstanceCount(0);
    CloseHandle(StopEvent);
    RilReportStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

int
wmain(
    _In_ int argc,
    _In_ WCHAR *argv[])
{
    SERVICE_TABLE_ENTRYW Table[] =
    {
        { RILADAPT_SERVICE_NAME, RilServiceMain },
        { NULL, NULL }
    };

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    return StartServiceCtrlDispatcherW(Table) ? 0 : (int)GetLastError();
}
