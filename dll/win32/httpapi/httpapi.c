/*
 * PROJECT:     ReactOS HTTP Server API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Server session, URL group and request queue entry points
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * These are the version 2 objects of the HTTP Server API. They live in http.sys,
 * which ReactOS does not have, so nothing can be created and no ID or handle given
 * to the rest names one of ours.
 */

#include <windef.h>
#include <winbase.h>
#include <http.h>

ULONG
WINAPI
HttpCreateServerSession(
    _In_ HTTPAPI_VERSION Version,
    _Out_ PHTTP_SERVER_SESSION_ID ServerSessionId,
    _Reserved_ ULONG Reserved)
{
    if (!ServerSessionId || Reserved)
        return ERROR_INVALID_PARAMETER;

    *ServerSessionId = HTTP_NULL_ID;

    if (Version.HttpApiMajorVersion != 2 || Version.HttpApiMinorVersion != 0)
        return ERROR_REVISION_MISMATCH;

    return ERROR_NOT_SUPPORTED;
}

ULONG
WINAPI
HttpCloseServerSession(
    _In_ HTTP_SERVER_SESSION_ID ServerSessionId)
{
    UNREFERENCED_PARAMETER(ServerSessionId);

    return ERROR_INVALID_PARAMETER;
}

ULONG
WINAPI
HttpCreateUrlGroup(
    _In_ HTTP_SERVER_SESSION_ID ServerSessionId,
    _Out_ PHTTP_URL_GROUP_ID pUrlGroupId,
    _Reserved_ ULONG Reserved)
{
    UNREFERENCED_PARAMETER(ServerSessionId);

    if (!pUrlGroupId || Reserved)
        return ERROR_INVALID_PARAMETER;

    *pUrlGroupId = HTTP_NULL_ID;
    return ERROR_INVALID_PARAMETER;
}

ULONG
WINAPI
HttpCloseUrlGroup(
    _In_ HTTP_URL_GROUP_ID UrlGroupId)
{
    UNREFERENCED_PARAMETER(UrlGroupId);

    return ERROR_INVALID_PARAMETER;
}

ULONG
WINAPI
HttpSetUrlGroupProperty(
    _In_ HTTP_URL_GROUP_ID UrlGroupId,
    _In_ HTTP_SERVER_PROPERTY Property,
    _In_reads_bytes_(PropertyInformationLength) PVOID PropertyInformation,
    _In_ ULONG PropertyInformationLength)
{
    UNREFERENCED_PARAMETER(UrlGroupId);
    UNREFERENCED_PARAMETER(Property);
    UNREFERENCED_PARAMETER(PropertyInformation);
    UNREFERENCED_PARAMETER(PropertyInformationLength);

    return ERROR_INVALID_PARAMETER;
}

ULONG
WINAPI
HttpCreateRequestQueue(
    _In_ HTTPAPI_VERSION Version,
    _In_opt_ PCWSTR Name,
    _In_opt_ PSECURITY_ATTRIBUTES SecurityAttributes,
    _In_ ULONG Flags,
    _Out_ PHANDLE RequestQueueHandle)
{
    UNREFERENCED_PARAMETER(Name);
    UNREFERENCED_PARAMETER(SecurityAttributes);
    UNREFERENCED_PARAMETER(Flags);

    if (!RequestQueueHandle)
        return ERROR_INVALID_PARAMETER;

    *RequestQueueHandle = NULL;

    if (Version.HttpApiMajorVersion != 2 || Version.HttpApiMinorVersion != 0)
        return ERROR_REVISION_MISMATCH;

    return ERROR_NOT_SUPPORTED;
}

ULONG
WINAPI
HttpCloseRequestQueue(
    _In_ HANDLE RequestQueueHandle)
{
    UNREFERENCED_PARAMETER(RequestQueueHandle);

    return ERROR_INVALID_HANDLE;
}

ULONG
WINAPI
HttpSetRequestQueueProperty(
    _In_ HANDLE RequestQueueHandle,
    _In_ HTTP_SERVER_PROPERTY Property,
    _In_reads_bytes_(PropertyInformationLength) PVOID PropertyInformation,
    _In_ ULONG PropertyInformationLength,
    _Reserved_ ULONG Reserved1,
    _Reserved_ PVOID Reserved2)
{
    UNREFERENCED_PARAMETER(RequestQueueHandle);
    UNREFERENCED_PARAMETER(Property);
    UNREFERENCED_PARAMETER(PropertyInformation);
    UNREFERENCED_PARAMETER(PropertyInformationLength);
    UNREFERENCED_PARAMETER(Reserved1);
    UNREFERENCED_PARAMETER(Reserved2);

    return ERROR_INVALID_HANDLE;
}

ULONG
WINAPI
HttpShutdownRequestQueue(
    _In_ HANDLE RequestQueueHandle)
{
    UNREFERENCED_PARAMETER(RequestQueueHandle);

    return ERROR_INVALID_HANDLE;
}
