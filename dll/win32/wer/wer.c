/*
 * PROJECT:     ReactOS Windows Error Reporting
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Gathering a report on a program that went wrong
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * A program builds a report here, hangs named values on it and submits it to
 * be sent on. Nothing here sends anything, so no report is ever opened and a
 * caller that is told so carries on without filing one.
 */

#include <windef.h>
#include <winbase.h>
#include <winerror.h>
#include <werapi.h>

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE Instance,
    _In_ ULONG Reason,
    _In_opt_ PVOID Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);

    if (Reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(Instance);

    return TRUE;
}

/**
 * @brief
 * Opens a report for a program that went wrong.
 *
 * @return
 * E_NOTIMPL, because no report is kept and none is ever sent.
 */
HRESULT
WINAPI
WerReportCreate(
    _In_ PCWSTR pwzEventType,
    _In_ WER_REPORT_TYPE repType,
    _In_opt_ PWER_REPORT_INFORMATION pReportInformation,
    _Out_ HREPORT *phReportHandle)
{
    UNREFERENCED_PARAMETER(pwzEventType);
    UNREFERENCED_PARAMETER(repType);
    UNREFERENCED_PARAMETER(pReportInformation);

    if (phReportHandle != NULL)
        *phReportHandle = NULL;

    return E_NOTIMPL;
}

/**
 * @brief
 * Hangs one named value on a report.
 *
 * @return
 * E_HANDLE, since no report was ever opened to hang it on.
 */
HRESULT
WINAPI
WerReportSetParameter(
    _In_ HREPORT hReportHandle,
    _In_ DWORD dwparamID,
    _In_opt_ PCWSTR pwzName,
    _In_ PCWSTR pwzValue)
{
    UNREFERENCED_PARAMETER(hReportHandle);
    UNREFERENCED_PARAMETER(dwparamID);
    UNREFERENCED_PARAMETER(pwzName);
    UNREFERENCED_PARAMETER(pwzValue);

    return E_HANDLE;
}

/**
 * @brief
 * Hands a report over to be sent on.
 *
 * @return
 * E_HANDLE. The result says reporting is off, which is what a caller
 * checks before telling anyone the report was filed.
 */
HRESULT
WINAPI
WerReportSubmit(
    _In_ HREPORT hReportHandle,
    _In_ WER_CONSENT consent,
    _In_ DWORD dwFlags,
    _Out_opt_ PWER_SUBMIT_RESULT pSubmitResult)
{
    UNREFERENCED_PARAMETER(hReportHandle);
    UNREFERENCED_PARAMETER(consent);
    UNREFERENCED_PARAMETER(dwFlags);

    if (pSubmitResult != NULL)
        *pSubmitResult = WerDisabled;

    return E_HANDLE;
}

/**
 * @brief
 * Gives back a report opened by WerReportCreate.
 *
 * @return
 * E_HANDLE, since none was ever handed out.
 */
HRESULT
WINAPI
WerReportCloseHandle(
    _In_ HREPORT hReportHandle)
{
    UNREFERENCED_PARAMETER(hReportHandle);

    return E_HANDLE;
}

/* EOF */
