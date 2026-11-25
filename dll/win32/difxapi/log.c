/*
 * PROJECT:     ReactOS Driver Install Frameworks API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Process wide log callbacks
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES *******************************************************************/

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(difxapi);

/* GLOBALS ********************************************************************/

/* Longest message handed to a callback, in characters */
#define DIFX_LOG_MESSAGE_LENGTH 1024

/* SetDifxLogCallback, called first */
static DIFXLOGCALLBACK_W DifxSinkW;
static DIFXLOGCALLBACK_A DifxSinkA;
static PVOID DifxSinkContext;

/* DIFXAPISetLogCallback */
static DIFXAPILOGCALLBACK_W ApiSinkW;
static DIFXAPILOGCALLBACK_A ApiSinkA;
static PVOID ApiSinkContext;

/* FUNCTIONS ******************************************************************/

VOID
WINAPI
DifxFree(
    _In_opt_ PVOID Buffer)
{
    if (Buffer)
        HeapFree(GetProcessHeap(), 0, Buffer);
}

_Ret_maybenull_
PWSTR
WINAPI
DifxAnsiToWide(
    _In_ PCSTR String)
{
    PWSTR Wide;
    int Length;

    Length = MultiByteToWideChar(CP_ACP, 0, String, -1, NULL, 0);
    if (!Length)
        return NULL;

    Wide = HeapAlloc(GetProcessHeap(), 0, Length * sizeof(*Wide));
    if (!Wide)
        return NULL;

    if (!MultiByteToWideChar(CP_ACP, 0, String, -1, Wide, Length))
    {
        DifxFree(Wide);
        return NULL;
    }

    return Wide;
}

_Ret_maybenull_
PSTR
WINAPI
DifxWideToAnsi(
    _In_ PCWSTR String)
{
    PSTR Ansi;
    int Length;

    Length = WideCharToMultiByte(CP_ACP, 0, String, -1, NULL, 0, NULL, NULL);
    if (!Length)
        return NULL;

    Ansi = HeapAlloc(GetProcessHeap(), 0, Length);
    if (!Ansi)
        return NULL;

    if (!WideCharToMultiByte(CP_ACP, 0, String, -1, Ansi, Length, NULL, NULL))
    {
        DifxFree(Ansi);
        return NULL;
    }

    return Ansi;
}

static
BOOL
WINAPI
DifxIsLogging(VOID)
{
    return (DifxSinkW || DifxSinkA || ApiSinkW || ApiSinkA);
}

/* Hands one formatted message to both callbacks */
static
VOID
WINAPI
DifxDeliver(
    _In_ DIFXAPI_LOG Event,
    _In_ DWORD Error,
    _In_ PCWSTR Message)
{
    PSTR Ansi = NULL;

    if (DifxSinkA || ApiSinkA)
    {
        Ansi = DifxWideToAnsi(Message);
        if (!Ansi)
            WARN("Cannot convert log message\n");
    }

    if (DifxSinkW)
        DifxSinkW(Event, Error, Message, DifxSinkContext);
    else if (DifxSinkA && Ansi)
        DifxSinkA(Event, Error, Ansi, DifxSinkContext);

    if (ApiSinkW)
        ApiSinkW(Event, Error, Message, ApiSinkContext);
    else if (ApiSinkA && Ansi)
        ApiSinkA(Event, Error, Ansi, ApiSinkContext);

    DifxFree(Ansi);
}

static
VOID
WINAPI
DifxLogV(
    _In_ DIFXAPI_LOG Event,
    _In_ DWORD Error,
    _In_ PCWSTR Format,
    _In_ va_list Arguments)
{
    WCHAR Message[DIFX_LOG_MESSAGE_LENGTH];
    HRESULT hr;

    if (!DifxIsLogging())
        return;

    /* A message that does not fit is sent cut short */
    hr = StringCchVPrintfW(Message, ARRAYSIZE(Message), Format, Arguments);
    if (FAILED(hr) && hr != STRSAFE_E_INSUFFICIENT_BUFFER)
        return;

    DifxDeliver(Event, Error, Message);
}

VOID
__cdecl
DifxLog(
    _In_ DIFXAPI_LOG Event,
    _In_ _Printf_format_string_ PCWSTR Format,
    ...)
{
    va_list Arguments;

    va_start(Arguments, Format);
    DifxLogV(Event, ERROR_SUCCESS, Format, Arguments);
    va_end(Arguments);
}

VOID
__cdecl
DifxLogError(
    _In_ DIFXAPI_LOG Event,
    _In_ DWORD Error,
    _In_ _Printf_format_string_ PCWSTR Format,
    ...)
{
    va_list Arguments;

    va_start(Arguments, Format);
    DifxLogV(Event, Error, Format, Arguments);
    va_end(Arguments);
}

VOID
WINAPI
SetDifxLogCallbackW(
    _In_ DIFXLOGCALLBACK_W LogCallback,
    _In_ PVOID CallbackContext)
{
    DifxSinkA = NULL;
    DifxSinkW = LogCallback;
    DifxSinkContext = CallbackContext;
}

VOID
WINAPI
SetDifxLogCallbackA(
    _In_ DIFXLOGCALLBACK_A LogCallback,
    _In_ PVOID CallbackContext)
{
    DifxSinkW = NULL;
    DifxSinkA = LogCallback;
    DifxSinkContext = CallbackContext;
}

VOID
WINAPI
DIFXAPISetLogCallbackW(
    _In_opt_ DIFXAPILOGCALLBACK_W LogCallback,
    _In_opt_ PVOID CallbackContext)
{
    ApiSinkA = NULL;
    ApiSinkW = LogCallback;
    ApiSinkContext = CallbackContext;
}

VOID
WINAPI
DIFXAPISetLogCallbackA(
    _In_opt_ DIFXAPILOGCALLBACK_A LogCallback,
    _In_opt_ PVOID CallbackContext)
{
    ApiSinkW = NULL;
    ApiSinkA = LogCallback;
    ApiSinkContext = CallbackContext;
}
