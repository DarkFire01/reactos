/*
 * PROJECT:     ReactOS Phone Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     HTTP transport to the MMS relay
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "phonesvc.h"
#include <winhttp.h>

#define NDEBUG
#include <debug.h>

#define PH_HTTP_RESOLVE_TIMEOUT_MS  20000
#define PH_HTTP_CONNECT_TIMEOUT_MS  20000
#define PH_HTTP_TRANSFER_TIMEOUT_MS 60000
#define PH_HTTP_MAX_RESPONSE        (4 * 1024 * 1024)
#define PH_HTTP_READ_CHUNK          (16 * 1024)

static BOOL PhHttpLoaded;

/* WinHTTP imports user32, so it is delay loaded and only touched by the MMS worker */
static
BOOL
PhHttpLoad(VOID)
{
    if (PhHttpLoaded)
        return TRUE;

    if (!LoadLibraryW(L"winhttp.dll"))
    {
        DPRINT1("MMS http: winhttp.dll did not load (%lu)\n", GetLastError());
        return FALSE;
    }

    PhHttpLoaded = TRUE;
    return TRUE;
}

static
HRESULT
PhHttpFail(
    _In_z_ PCSTR Step)
{
    DWORD Error = GetLastError();

    DPRINT1("MMS http: %s failed (%lu)\n", Step, Error);
    return HRESULT_FROM_WIN32(Error ? Error : ERROR_WINHTTP_INTERNAL_ERROR);
}

static
HRESULT
PhHttpAddHeader(
    _In_ HINTERNET Request,
    _In_z_ LPCWSTR Name,
    _In_z_ LPCWSTR Value)
{
    WCHAR Line[PH_MMS_FIELD_CHARS + 64];

    if (FAILED(StringCchPrintfW(Line, _countof(Line), L"%s: %s", Name, Value)))
        return E_INVALIDARG;

    if (!WinHttpAddRequestHeaders(Request, Line, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
        return PhHttpFail("adding a request header");

    return S_OK;
}

/* Reads the whole response body, bounded so a broken relay cannot exhaust memory */
static
HRESULT
PhHttpReadBody(
    _In_ HINTERNET Request,
    _Outptr_result_bytebuffer_(*Size) PBYTE *Body,
    _Out_ PDWORD Size)
{
    PBYTE Buffer = NULL, Grown;
    DWORD Capacity = 0, Used = 0, Read;

    *Body = NULL;
    *Size = 0;

    for (;;)
    {
        if (Capacity - Used < PH_HTTP_READ_CHUNK)
        {
            if (Capacity >= PH_HTTP_MAX_RESPONSE)
            {
                DPRINT1("MMS http: response larger than %lu bytes\n", PH_HTTP_MAX_RESPONSE);
                HeapFree(GetProcessHeap(), 0, Buffer);
                return HRESULT_FROM_WIN32(ERROR_MESSAGE_EXCEEDS_MAX_SIZE);
            }

            Capacity += 4 * PH_HTTP_READ_CHUNK;
            Grown = Buffer ? HeapReAlloc(GetProcessHeap(), 0, Buffer, Capacity)
                           : HeapAlloc(GetProcessHeap(), 0, Capacity);
            if (!Grown)
            {
                HeapFree(GetProcessHeap(), 0, Buffer);
                return E_OUTOFMEMORY;
            }
            Buffer = Grown;
        }

        Read = 0;
        if (!WinHttpReadData(Request, Buffer + Used, Capacity - Used, &Read))
        {
            HeapFree(GetProcessHeap(), 0, Buffer);
            return PhHttpFail("reading the response");
        }

        if (Read == 0)
            break;
        Used += Read;
    }

    *Body = Buffer;
    *Size = Used;
    return S_OK;
}

HRESULT
PhMmsHttpRequest(
    _In_ const PH_MMS_CONFIG *Config,
    _In_z_ LPCWSTR Url,
    _In_reads_bytes_opt_(BodySize) const BYTE *Body,
    _In_ DWORD BodySize,
    _Outptr_result_bytebuffer_(*ResponseSize) PBYTE *Response,
    _Out_ PDWORD ResponseSize,
    _Out_ PDWORD HttpStatus)
{
    LPCWSTR Accept[] = { L"*/*", NULL };
    WCHAR Host[256], Path[PH_MMS_URL_CHARS], Extra[PH_MMS_URL_CHARS], Object[PH_MMS_URL_CHARS];
    HINTERNET Session = NULL, Connect = NULL, Request = NULL;
    URL_COMPONENTS Parts;
    DWORD Status = 0, Size;
    HRESULT hr;

    *Response = NULL;
    *ResponseSize = 0;
    *HttpStatus = 0;

    if (!PhHttpLoad())
        return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);

    ZeroMemory(&Parts, sizeof(Parts));
    Parts.dwStructSize = sizeof(Parts);
    Parts.lpszHostName = Host;
    Parts.dwHostNameLength = _countof(Host);
    Parts.lpszUrlPath = Path;
    Parts.dwUrlPathLength = _countof(Path);
    Parts.lpszExtraInfo = Extra;
    Parts.dwExtraInfoLength = _countof(Extra);
    if (!WinHttpCrackUrl(Url, 0, 0, &Parts))
        return PhHttpFail("parsing the URL");

    /* The query carries the message handle on most relays */
    if (FAILED(StringCchPrintfW(Object, _countof(Object), L"%s%s", Path[0] ? Path : L"/", Extra)))
        return E_INVALIDARG;

    Session = WinHttpOpen(Config->UserAgent,
                          Config->Proxy[0] ? WINHTTP_ACCESS_TYPE_NAMED_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY,
                          Config->Proxy[0] ? Config->Proxy : WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS,
                          0);
    if (!Session)
    {
        hr = PhHttpFail("opening a session");
        goto Exit;
    }

    WinHttpSetTimeouts(Session,
                       PH_HTTP_RESOLVE_TIMEOUT_MS,
                       PH_HTTP_CONNECT_TIMEOUT_MS,
                       PH_HTTP_TRANSFER_TIMEOUT_MS,
                       PH_HTTP_TRANSFER_TIMEOUT_MS);

    Connect = WinHttpConnect(Session, Host, Parts.nPort, 0);
    if (!Connect)
    {
        hr = PhHttpFail("connecting");
        goto Exit;
    }

    Request = WinHttpOpenRequest(Connect,
                                 Body ? L"POST" : L"GET",
                                 Object,
                                 NULL,
                                 WINHTTP_NO_REFERER,
                                 Accept,
                                 (Parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    if (!Request)
    {
        hr = PhHttpFail("opening the request");
        goto Exit;
    }

    if (Config->Profile[0])
    {
        hr = PhHttpAddHeader(Request, L"x-wap-profile", Config->Profile);
        if (FAILED(hr))
            goto Exit;
    }

    if (Body)
    {
        hr = PhHttpAddHeader(Request, L"Content-Type", L"application/vnd.wap.mms-message");
        if (FAILED(hr))
            goto Exit;
    }

    if (!WinHttpSendRequest(Request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (PVOID)Body, BodySize, BodySize, 0))
    {
        hr = PhHttpFail("sending the request");
        goto Exit;
    }

    if (!WinHttpReceiveResponse(Request, NULL))
    {
        hr = PhHttpFail("receiving the response");
        goto Exit;
    }

    Size = sizeof(Status);
    if (!WinHttpQueryHeaders(Request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &Status,
                             &Size,
                             WINHTTP_NO_HEADER_INDEX))
    {
        hr = PhHttpFail("reading the status code");
        goto Exit;
    }

    *HttpStatus = Status;
    hr = PhHttpReadBody(Request, Response, ResponseSize);

Exit:
    if (Request)
        WinHttpCloseHandle(Request);
    if (Connect)
        WinHttpCloseHandle(Connect);
    if (Session)
        WinHttpCloseHandle(Session);
    return hr;
}
