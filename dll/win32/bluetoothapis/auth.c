/*
 * PROJECT:     ReactOS Bluetooth API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Authentication callbacks and responses
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

typedef struct _BTH_AUTH_REGISTRATION
{
    BTH_ADDR Address;
    PVOID Callback;
    BOOL Extended;
    PVOID Context;
} BTH_AUTH_REGISTRATION, *PBTH_AUTH_REGISTRATION;

/* Registering works without a radio, the callback just never runs */
static
DWORD
BthpRegisterForAuthentication(
    _In_opt_ const BLUETOOTH_DEVICE_INFO *DeviceInfo,
    _Out_ HBLUETOOTH_AUTHENTICATION_REGISTRATION *Registration,
    _In_ PVOID Callback,
    _In_ BOOL Extended,
    _In_opt_ PVOID Context)
{
    PBTH_AUTH_REGISTRATION Entry;

    if (!Registration || !Callback)
        return ERROR_INVALID_PARAMETER;

    *Registration = NULL;
    Entry = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Entry));
    if (!Entry)
        return ERROR_OUTOFMEMORY;

    Entry->Address = DeviceInfo ? DeviceInfo->Address.ullLong : BLUETOOTH_NULL_ADDRESS;
    Entry->Callback = Callback;
    Entry->Extended = Extended;
    Entry->Context = Context;

    *Registration = Entry;
    return ERROR_SUCCESS;
}

DWORD
WINAPI
BluetoothRegisterForAuthentication(
    _In_opt_ const BLUETOOTH_DEVICE_INFO *pbtdi,
    _Out_ HBLUETOOTH_AUTHENTICATION_REGISTRATION *phRegHandle,
    _In_opt_ PFN_AUTHENTICATION_CALLBACK pfnCallback,
    _In_opt_ PVOID pvParam)
{
    return BthpRegisterForAuthentication(pbtdi, phRegHandle, (PVOID)pfnCallback, FALSE, pvParam);
}

DWORD
WINAPI
BluetoothRegisterForAuthenticationEx(
    _In_opt_ const BLUETOOTH_DEVICE_INFO *pbtdiIn,
    _Out_ HBLUETOOTH_AUTHENTICATION_REGISTRATION *phRegHandleOut,
    _In_opt_ PFN_AUTHENTICATION_CALLBACK_EX pfnCallbackIn,
    _In_opt_ PVOID pvParam)
{
    return BthpRegisterForAuthentication(pbtdiIn, phRegHandleOut, (PVOID)pfnCallbackIn, TRUE, pvParam);
}

BOOL
WINAPI
BluetoothUnregisterAuthentication(
    _In_ HBLUETOOTH_AUTHENTICATION_REGISTRATION hRegHandle)
{
    if (!hRegHandle)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    HeapFree(GetProcessHeap(), 0, hRegHandle);
    return TRUE;
}

DWORD
WINAPI
BluetoothSendAuthenticationResponseEx(
    _In_opt_ HANDLE hRadioIn,
    _In_ PBLUETOOTH_AUTHENTICATE_RESPONSE pauthResponse)
{
    HANDLE Radio;
    DWORD Error;

    if (!pauthResponse)
        return ERROR_INVALID_PARAMETER;

    if (hRadioIn)
        return BthpForwardToRadio(hRadioIn);

    /* Without a radio Windows hands back the error of the radio search */
    Radio = BthpOpenAnyRadio();
    if (!Radio)
        return GetLastError();

    Error = BthpForwardToRadio(Radio);
    CloseHandle(Radio);
    return Error;
}

DWORD
WINAPI
BluetoothSendAuthenticationResponse(
    _In_opt_ HANDLE hRadio,
    _In_ BLUETOOTH_DEVICE_INFO *pbtdi,
    _In_ WCHAR *pszPasskey)
{
    BLUETOOTH_AUTHENTICATE_RESPONSE Response;
    SIZE_T Length;

    Length = pszPasskey ? wcslen(pszPasskey) : 0;
    if (Length == 0 || Length > BLUETOOTH_MAX_PASSKEY_SIZE)
        return ERROR_INVALID_PARAMETER;

    /* A legacy PIN goes out as the narrow form of the passkey */
    ZeroMemory(&Response, sizeof(Response));
    Response.bthAddressRemote.ullLong = pbtdi ? pbtdi->Address.ullLong : BLUETOOTH_NULL_ADDRESS;
    Response.authMethod = BLUETOOTH_AUTHENTICATION_METHOD_LEGACY;
    Response.pinInfo.pinLength = (UCHAR)Length;
    WideCharToMultiByte(CP_ACP,
                        0,
                        pszPasskey,
                        (int)Length,
                        (LPSTR)Response.pinInfo.pin,
                        sizeof(Response.pinInfo.pin),
                        NULL,
                        NULL);

    return BluetoothSendAuthenticationResponseEx(hRadio, &Response);
}
