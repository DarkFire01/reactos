/*
 * PROJECT:     ReactOS Bluetooth API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Remote devices and their services
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

/*
 * Remote devices are known only through a radio. Without one, Windows' support
 * service answers every device query with ERROR_INVALID_HANDLE, so no search
 * handle is ever handed out.
 */

HBLUETOOTH_DEVICE_FIND
WINAPI
BluetoothFindFirstDevice(
    _In_ BLUETOOTH_DEVICE_SEARCH_PARAMS *pbtsp,
    _Inout_ BLUETOOTH_DEVICE_INFO *pbtdi)
{
    if (!pbtsp || !pbtdi)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    if (pbtsp->dwSize != sizeof(*pbtsp) || pbtdi->dwSize != sizeof(*pbtdi))
    {
        SetLastError(ERROR_REVISION_MISMATCH);
        return NULL;
    }

    SetLastError(ERROR_INVALID_HANDLE);
    return NULL;
}

BOOL
WINAPI
BluetoothFindNextDevice(
    _In_ HBLUETOOTH_DEVICE_FIND hFind,
    _Inout_ BLUETOOTH_DEVICE_INFO *pbtdi)
{
    UNREFERENCED_PARAMETER(pbtdi);

    if (!hFind)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    SetLastError(ERROR_NO_MORE_ITEMS);
    return FALSE;
}

BOOL
WINAPI
BluetoothFindDeviceClose(
    _In_ HBLUETOOTH_DEVICE_FIND hFind)
{
    if (!hFind)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    return TRUE;
}

DWORD
WINAPI
BluetoothGetDeviceInfo(
    _In_opt_ HANDLE hRadio,
    _Inout_ BLUETOOTH_DEVICE_INFO *pbtdi)
{
    UNREFERENCED_PARAMETER(hRadio);

    if (!pbtdi)
        return ERROR_INVALID_PARAMETER;

    if (pbtdi->dwSize != sizeof(*pbtdi))
        return ERROR_REVISION_MISMATCH;

    /* Windows passes the service's HRESULT through here */
    return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
}

DWORD
WINAPI
BluetoothUpdateDeviceRecord(
    _In_ BLUETOOTH_DEVICE_INFO *pbtdi)
{
    if (!pbtdi)
        return ERROR_INVALID_PARAMETER;

    if (pbtdi->dwSize != sizeof(*pbtdi))
        return ERROR_REVISION_MISMATCH;

    /* Windows reports success once the service took the request, even when the radio is missing */
    return ERROR_SUCCESS;
}

DWORD
WINAPI
BluetoothRemoveDevice(
    _In_ BLUETOOTH_ADDRESS *pAddress)
{
    UNREFERENCED_PARAMETER(pAddress);

    /* No radio knows the device and there is no device node to remove */
    return ERROR_NOT_FOUND;
}

DWORD
WINAPI
BluetoothEnumerateInstalledServices(
    _In_opt_ HANDLE hRadio,
    _In_ BLUETOOTH_DEVICE_INFO *pbtdi,
    _Inout_ DWORD *pcServiceInout,
    _Out_writes_to_opt_(*pcServiceInout, *pcServiceInout) GUID *pGuidServices)
{
    HANDLE Radio;
    DWORD Error;

    UNREFERENCED_PARAMETER(pbtdi);
    UNREFERENCED_PARAMETER(pcServiceInout);
    UNREFERENCED_PARAMETER(pGuidServices);

    if (hRadio)
        return BthpForwardToRadio(hRadio);

    Radio = BthpOpenAnyRadio();
    if (!Radio)
        return ERROR_NOT_FOUND;

    Error = BthpForwardToRadio(Radio);
    CloseHandle(Radio);
    return Error;
}

DWORD
WINAPI
BluetoothSetServiceState(
    _In_opt_ HANDLE hRadio,
    _In_ BLUETOOTH_DEVICE_INFO *pbtdi,
    _In_ GUID *pGuidService,
    _In_ DWORD dwServiceFlags)
{
    UNREFERENCED_PARAMETER(hRadio);
    UNREFERENCED_PARAMETER(pbtdi);
    UNREFERENCED_PARAMETER(pGuidService);
    UNREFERENCED_PARAMETER(dwServiceFlags);

    /* The service has to be found in the device's cached SDP records, which only a radio provides */
    return ERROR_SERVICE_DOES_NOT_EXIST;
}

DWORD
WINAPI
BluetoothSetLocalServiceInfo(
    _In_opt_ HANDLE hRadioIn,
    _In_ const GUID *pClassGuid,
    _In_ ULONG ulInstance,
    _In_ const BLUETOOTH_LOCAL_SERVICE_INFO *pServiceInfoIn)
{
    HANDLE Radio;
    DWORD Error;

    UNREFERENCED_PARAMETER(pClassGuid);
    UNREFERENCED_PARAMETER(ulInstance);
    UNREFERENCED_PARAMETER(pServiceInfoIn);

    if (hRadioIn)
        return BthpForwardToRadio(hRadioIn);

    Radio = BthpOpenAnyRadio();
    if (!Radio)
        return ERROR_BAD_UNIT;

    Error = BthpForwardToRadio(Radio);
    CloseHandle(Radio);
    return Error;
}
