/*
 * PROJECT:     ReactOS GNSS Fix Viewer
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Prints what the GNSS service knows, or follows its fixes
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <windef.h>
#include <winbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <nativegnss.h>

static const PCWSTR StateNames[] =
{
    L"no service", L"idle", L"searching", L"fixed"
};

static
VOID
PrintStatus(
    _In_ const NATIVEGNSS_STATUS *Status)
{
    wprintf(L"state %ls, clients %lu, engine %ls, revision %lu, last status %lu\n",
            Status->State < _countof(StateNames) ? StateNames[Status->State] : L"?",
            Status->Clients,
            Status->EngineOn ? L"on" : L"off",
            Status->ServiceRevision,
            Status->LastSessionStatus);
    wprintf(L"satellites in view %lu, tracked %lu, best %.1f dB-Hz\n",
            Status->SatellitesInView, Status->SatellitesTracked, Status->BestSignal);
}

static
VOID
PrintFix(
    _In_ const NATIVEGNSS_FIX *Fix)
{
    SYSTEMTIME Time;

    if (!(Fix->Flags & NATIVEGNSS_FIX_POSITION))
    {
        wprintf(L"no fix\n");
        return;
    }

    wprintf(L"fix %lu%ls: %.6f, %.6f", Fix->FixId,
            (Fix->Flags & NATIVEGNSS_FIX_INTERMEDIATE) ? L" (intermediate)" : L"",
            Fix->Latitude, Fix->Longitude);
    if (Fix->Flags & NATIVEGNSS_FIX_ACCURACY)
        wprintf(L" +-%.0f m", Fix->HorizontalAccuracy);
    if (Fix->Flags & NATIVEGNSS_FIX_ALTITUDE)
        wprintf(L" alt %.1f m%ls", Fix->Altitude, (Fix->Flags & NATIVEGNSS_FIX_ALTITUDE_WGS84) ? L" (WGS84)" : L"");
    if (Fix->Flags & NATIVEGNSS_FIX_SPEED)
        wprintf(L" speed %.1f m/s", Fix->Speed);
    if (Fix->Flags & NATIVEGNSS_FIX_HEADING)
        wprintf(L" heading %.0f", Fix->Heading);
    if (Fix->Flags & NATIVEGNSS_FIX_SATELLITES)
        wprintf(L" sats %lu", Fix->SatellitesUsed);
    if ((Fix->Flags & NATIVEGNSS_FIX_TIME) && FileTimeToSystemTime(&Fix->UtcTime, &Time))
    {
        wprintf(L" at %04u-%02u-%02u %02u:%02u:%02u UTC",
                Time.wYear, Time.wMonth, Time.wDay, Time.wHour, Time.wMinute, Time.wSecond);
    }
    wprintf(L"\n");
}

int
wmain(
    _In_ int argc,
    _In_ WCHAR *argv[])
{
    NATIVEGNSS_REQUEST Request;
    NATIVEGNSS_REPLY Reply;
    NATIVEGNSS_EVENT Event;
    HANDLE Pipe;
    DWORD Mode = PIPE_READMODE_MESSAGE, Bytes;
    BOOL Watch = (argc > 1 && (_wcsicmp(argv[1], L"/watch") == 0 || _wcsicmp(argv[1], L"-watch") == 0));

    Pipe = CreateFileW(NATIVEGNSS_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (Pipe == INVALID_HANDLE_VALUE)
    {
        wprintf(L"GNSS service not reachable (%lu)\n", GetLastError());
        return 1;
    }
    SetNamedPipeHandleState(Pipe, &Mode, NULL, NULL);

    ZeroMemory(&Request, sizeof(Request));
    Request.Command = Watch ? NativeGnssSubscribe : NativeGnssGetStatus;

    if (!WriteFile(Pipe, &Request, sizeof(Request), &Bytes, NULL))
    {
        wprintf(L"request failed (%lu)\n", GetLastError());
        CloseHandle(Pipe);
        return 1;
    }

    if (!Watch)
    {
        if (!ReadFile(Pipe, &Reply, sizeof(Reply), &Bytes, NULL) || Bytes != sizeof(Reply))
        {
            wprintf(L"no reply (%lu)\n", GetLastError());
            CloseHandle(Pipe);
            return 1;
        }

        PrintStatus(&Reply.Status);
        PrintFix(&Reply.Fix);
        CloseHandle(Pipe);
        return 0;
    }

    wprintf(L"following fixes, Ctrl+C ends\n");
    while (ReadFile(Pipe, &Event, sizeof(Event), &Bytes, NULL) && Bytes == sizeof(Event))
    {
        if (Event.Code == NativeGnssEventState)
            PrintStatus(&Event.Status);
        else
            PrintFix(&Event.Fix);
    }

    CloseHandle(Pipe);
    return 0;
}
