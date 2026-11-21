/*
 * PROJECT:     ReactOS RIL Adaptation Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Service entry and control
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "riladapt.h"
#include <stdarg.h>
#include <stdio.h>

/* Exit code of a fatal error, the service control manager restarts the service */
#define RILADAPT_FATAL_EXIT_CODE    3

static SERVICE_STATUS_HANDLE StatusHandle;
static SERVICE_STATUS Status;
static HANDLE StopEvent;
static RIL_MODEM Modems[RILADAPT_MAX_MODEMS];
static DWORD ModemCount;

static
VOID
RilWriteLog(
    _In_z_ PCSTR Prefix,
    _In_z_ PCSTR Format,
    _In_ va_list Args)
{
    CHAR Line[512];
    int Used;

    Used = _snprintf(Line, sizeof(Line) - 1, "[riladapt] %s", Prefix);
    if (Used < 0)
        Used = 0;
    _vsnprintf(Line + Used, sizeof(Line) - 1 - Used, Format, Args);
    Line[sizeof(Line) - 1] = '\0';
    OutputDebugStringA(Line);
}

VOID
__cdecl
RilLog(
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...)
{
    va_list Args;

    va_start(Args, Format);
    RilWriteLog("", Format, Args);
    va_end(Args);
}

DECLSPEC_NORETURN
VOID
__cdecl
RilFatal(
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...)
{
    va_list Args;

    va_start(Args, Format);
    RilWriteLog("fatal: ", Format, Args);
    va_end(Args);

    TerminateProcess(GetCurrentProcess(), RILADAPT_FATAL_EXIT_CODE);
    ExitProcess(RILADAPT_FATAL_EXIT_CODE);
}

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
    Status.dwControlsAccepted = (State == SERVICE_START_PENDING) ? 0 : SERVICE_ACCEPT_STOP;

    if (State == SERVICE_RUNNING || State == SERVICE_STOPPED)
        Status.dwCheckPoint = 0;
    else
        Status.dwCheckPoint++;

    RilLog("service state %lu\n", State);
    if (!SetServiceStatus(StatusHandle, &Status))
        RilFatal("SetServiceStatus(%lu) failed %lu\n", State, GetLastError());
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
            RilLog("stop requested\n");
            RilReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 60000);
            if (!SetEvent(StopEvent))
                RilFatal("cannot signal the stop event (%lu)\n", GetLastError());
            return NO_ERROR;

        case SERVICE_CONTROL_INTERROGATE:
            SetServiceStatus(StatusHandle, &Status);
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

VOID
WINAPI
RilServiceMain(
    _In_ DWORD Argc,
    _In_ LPWSTR *Argv)
{
    RIL_MODEM_CONFIG Configs[RILADAPT_MAX_MODEMS];
    DWORD Found, i;

    UNREFERENCED_PARAMETER(Argc);
    UNREFERENCED_PARAMETER(Argv);

    RilLog("service main entry\n");

    StatusHandle = RegisterServiceCtrlHandlerExW(RILADAPT_SERVICE_NAME, RilServiceControl, NULL);
    if (!StatusHandle)
    {
        RilLog("RegisterServiceCtrlHandlerExW failed %lu\n", GetLastError());
        return;
    }

    RilReportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    StopEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!StopEvent)
    {
        RilLog("cannot create the stop event (%lu)\n", GetLastError());
        RilReportStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    /* Running is reported before the modems come up, a failure on the way is fatal */
    RilReportStatus(SERVICE_RUNNING, NO_ERROR, 0);
    RilPublishInstanceCount(0);

    Found = RilFindModems(Configs, _countof(Configs));
    RilLog("found %lu modem config(s)\n", Found);
    for (i = 0; i < Found; i++)
    {
        RilLog("modem %lu: starting %S, watchdog %lu s, message hold %lu s\n",
               i, Configs[i].LibraryPath, Configs[i].WatchdogTimeout, Configs[i].MessageHoldTimeout);
        CopyMemory(&Modems[i].Config, &Configs[i], sizeof(Configs[i]));
        Modems[i].Index = i;
        RilStartModem(&Modems[i]);
        ModemCount++;
    }

    RilLog("running with %lu modem(s)\n", ModemCount);
    RilPublishInstanceCount(ModemCount);

    WaitForSingleObject(StopEvent, INFINITE);

    for (i = 0; i < ModemCount; i++)
        RilStopModem(&Modems[i]);

    RilPublishInstanceCount(0);
    CloseHandle(StopEvent);
    RilReportStatus(SERVICE_STOPPED, NO_ERROR, 0);

    /* The libraries are never unloaded, ending the process is the clean way out */
    TerminateProcess(GetCurrentProcess(), 0);
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
