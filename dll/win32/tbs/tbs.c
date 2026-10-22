/*
 * PROJECT:     ReactOS TPM Base Services
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     TBS client interface for systems without a TPM
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <windef.h>
#include <winbase.h>
#include <winerror.h>
#include <tbs.h>

/*
 * ReactOS ships no TPM driver. Every entry point reports what Windows reports
 * when the TPM device cannot be opened, after the same argument checks.
 */

/* Smallest TPM response: tag, size and return code */
#define TBSP_RESPONSE_HEADER_SIZE 10

/**
 * @brief Queries the TPM version and interface type.
 */
TBS_RESULT
WINAPI
Tbsi_GetDeviceInfo(
    _In_ UINT32 Size,
    _Out_writes_bytes_(Size) PVOID Info)
{
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(Info);

    return TBS_E_TPM_NOT_FOUND;
}

/**
 * @brief Opens a TBS context for submitting TPM commands.
 */
TBS_RESULT
WINAPI
Tbsi_Context_Create(
    _In_ PCTBS_CONTEXT_PARAMS pContextParams,
    _Out_ PTBS_HCONTEXT phContext)
{
    TPM_DEVICE_INFO DeviceInfo;

    if (!pContextParams)
        return TBS_E_BAD_PARAMETER;

    if (!phContext)
        return TBS_E_INVALID_OUTPUT_POINTER;

    /* The context parameters are only looked at once a device answers */
    return Tbsi_GetDeviceInfo(sizeof(DeviceInfo), &DeviceInfo);
}

/**
 * @brief Closes a context returned by Tbsi_Context_Create.
 */
TBS_RESULT
WINAPI
Tbsip_Context_Close(
    _In_ TBS_HCONTEXT hContext)
{
    UNREFERENCED_PARAMETER(hContext);

    /* No context can have been created */
    return TBS_E_INVALID_CONTEXT;
}

/**
 * @brief Sends a command to the TPM and waits for its response.
 */
TBS_RESULT
WINAPI
Tbsip_Submit_Command(
    _In_ TBS_HCONTEXT hContext,
    _In_ TBS_COMMAND_LOCALITY Locality,
    _In_ TBS_COMMAND_PRIORITY Priority,
    _In_reads_bytes_(cbCommand) const BYTE *pabCommand,
    _In_ UINT32 cbCommand,
    _Out_writes_bytes_(*pcbResult) PBYTE pabResult,
    _Inout_ PUINT32 pcbResult)
{
    UNREFERENCED_PARAMETER(hContext);

    if (Locality != TBS_COMMAND_LOCALITY_ZERO || Priority > TBS_COMMAND_PRIORITY_MAX)
        return TBS_E_BAD_PARAMETER;

    if (!pabResult || !pcbResult)
        return TBS_E_INVALID_OUTPUT_POINTER;

    if (*pcbResult < TBSP_RESPONSE_HEADER_SIZE)
        return TBS_E_INSUFFICIENT_BUFFER;

    if (!pabCommand || !cbCommand)
        return TBS_E_BAD_PARAMETER;

    return TBS_E_INVALID_CONTEXT;
}

/**
 * @brief Cancels the commands pending on a context.
 */
TBS_RESULT
WINAPI
Tbsip_Cancel_Commands(
    _In_ TBS_HCONTEXT hContext)
{
    UNREFERENCED_PARAMETER(hContext);

    return TBS_E_INVALID_CONTEXT;
}
