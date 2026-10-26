/*
 * PROJECT:     ReactOS Bluetooth API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Radio enumeration, radio state and discovery
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <initguid.h>
#include "precomp.h"

C_ASSERT(sizeof(BTH_LOCAL_RADIO_INFO) == 0x124);

typedef struct _BTH_RADIO_FIND
{
    HDEVINFO DevInfo;
    DWORD NextIndex;
} BTH_RADIO_FIND, *PBTH_RADIO_FIND;

/* Discovery state this process asked for, kept the way the support service keeps it per client */
static LONG BthpDiscoveryEnabled;
static LONG BthpIncomingEnabled;

DWORD
BthpQueryLocalRadio(
    _In_ HANDLE Radio,
    _Out_ PBTH_LOCAL_RADIO_INFO Info)
{
    DWORD Returned = 0;

    ZeroMemory(Info, sizeof(*Info));
    if (!DeviceIoControl(Radio,
                         IOCTL_BTH_GET_LOCAL_INFO,
                         NULL,
                         0,
                         Info,
                         sizeof(*Info),
                         &Returned,
                         NULL))
    {
        return GetLastError();
    }

    return ERROR_SUCCESS;
}

/* There is no Bluetooth stack to pass requests to, so only a handle that is not a radio gets a real answer */
DWORD
BthpForwardToRadio(
    _In_ HANDLE Radio)
{
    BTH_LOCAL_RADIO_INFO Info;
    DWORD Error;

    Error = BthpQueryLocalRadio(Radio, &Info);
    if (Error != ERROR_SUCCESS)
        return Error;

    return ERROR_NOT_SUPPORTED;
}

/* Opens the next present radio interface of the search */
static
BOOL
BthpOpenNextRadio(
    _Inout_ PBTH_RADIO_FIND Find,
    _Out_ HANDLE *Radio)
{
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W Detail;
    SP_DEVICE_INTERFACE_DATA Interface;
    DWORD Size;
    DWORD Error;
    HANDLE Handle;

    for (;;)
    {
        Interface.cbSize = sizeof(Interface);
        if (!SetupDiEnumDeviceInterfaces(Find->DevInfo,
                                         NULL,
                                         &GUID_BTHPORT_DEVICE_INTERFACE,
                                         Find->NextIndex,
                                         &Interface))
        {
            SetLastError(ERROR_NO_MORE_ITEMS);
            return FALSE;
        }
        Find->NextIndex++;

        Size = 0;
        SetupDiGetDeviceInterfaceDetailW(Find->DevInfo, &Interface, NULL, 0, &Size, NULL);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            continue;

        Detail = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
        if (!Detail)
        {
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }

        Detail->cbSize = sizeof(*Detail);
        Handle = INVALID_HANDLE_VALUE;
        if (SetupDiGetDeviceInterfaceDetailW(Find->DevInfo, &Interface, Detail, Size, &Size, NULL))
        {
            Handle = CreateFileW(Detail->DevicePath,
                                 GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL,
                                 OPEN_EXISTING,
                                 0,
                                 NULL);
        }

        Error = GetLastError();
        HeapFree(GetProcessHeap(), 0, Detail);
        SetLastError(Error);

        if (Handle != INVALID_HANDLE_VALUE)
        {
            *Radio = Handle;
            return TRUE;
        }
    }
}

static
VOID
BthpFreeRadioFind(
    _In_ PBTH_RADIO_FIND Find)
{
    if (Find->DevInfo != INVALID_HANDLE_VALUE)
        SetupDiDestroyDeviceInfoList(Find->DevInfo);
    HeapFree(GetProcessHeap(), 0, Find);
}

HBLUETOOTH_RADIO_FIND
WINAPI
BluetoothFindFirstRadio(
    _In_ BLUETOOTH_FIND_RADIO_PARAMS *pbtfrp,
    _Out_ HANDLE *phRadio)
{
    PBTH_RADIO_FIND Find;
    DWORD Error;

    if (!pbtfrp)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    if (pbtfrp->dwSize != sizeof(*pbtfrp))
    {
        SetLastError(ERROR_REVISION_MISMATCH);
        return NULL;
    }

    Find = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Find));
    if (!Find)
        return NULL;

    Find->DevInfo = SetupDiGetClassDevsW(&GUID_BTHPORT_DEVICE_INTERFACE,
                                         NULL,
                                         NULL,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (Find->DevInfo == INVALID_HANDLE_VALUE)
    {
        /* No radio interface was ever registered */
        BthpFreeRadioFind(Find);
        SetLastError(ERROR_NO_MORE_ITEMS);
        return NULL;
    }

    if (BthpOpenNextRadio(Find, phRadio))
        return Find;

    Error = GetLastError();
    BthpFreeRadioFind(Find);
    SetLastError(Error);
    return NULL;
}

BOOL
WINAPI
BluetoothFindNextRadio(
    _In_ HBLUETOOTH_RADIO_FIND hFind,
    _Out_ HANDLE *phRadio)
{
    if (!hFind)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    return BthpOpenNextRadio(hFind, phRadio);
}

BOOL
WINAPI
BluetoothFindRadioClose(
    _In_ HBLUETOOTH_RADIO_FIND hFind)
{
    if (!hFind)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    BthpFreeRadioFind(hFind);
    return TRUE;
}

HANDLE
BthpOpenAnyRadio(VOID)
{
    BLUETOOTH_FIND_RADIO_PARAMS Params;
    HBLUETOOTH_RADIO_FIND Find;
    HANDLE Radio;
    DWORD Error;

    Params.dwSize = sizeof(Params);
    Find = BluetoothFindFirstRadio(&Params, &Radio);
    if (!Find)
        return NULL;

    Error = GetLastError();
    BluetoothFindRadioClose(Find);
    SetLastError(Error);
    return Radio;
}

DWORD
WINAPI
BluetoothGetRadioInfo(
    _In_ HANDLE hRadio,
    _Inout_ PBLUETOOTH_RADIO_INFO pRadioInfo)
{
    BTH_LOCAL_RADIO_INFO Local;
    DWORD Error;

    if (!hRadio || !pRadioInfo)
        return ERROR_INVALID_PARAMETER;

    if (pRadioInfo->dwSize != sizeof(*pRadioInfo))
        return ERROR_REVISION_MISMATCH;

    Error = BthpQueryLocalRadio(hRadio, &Local);
    if (Error != ERROR_SUCCESS)
        return Error;

    pRadioInfo->address.ullLong = Local.localInfo.address;
    pRadioInfo->ulClassofDevice = Local.localInfo.classOfDevice;
    pRadioInfo->lmpSubversion = Local.radioInfo.lmpSubversion;
    pRadioInfo->manufacturer = Local.radioInfo.mfg;
    pRadioInfo->szName[0] = UNICODE_NULL;

    /* The radio name is UTF-8 and only valid when the radio flags it */
    if ((Local.localInfo.flags & BDIF_NAME) &&
        !MultiByteToWideChar(CP_UTF8,
                             0,
                             Local.localInfo.name,
                             BTH_MAX_NAME_SIZE,
                             pRadioInfo->szName,
                             BLUETOOTH_MAX_NAME_SIZE))
    {
        pRadioInfo->szName[0] = UNICODE_NULL;
        SetLastError(ERROR_SUCCESS);
    }

    return ERROR_SUCCESS;
}

/* Tells whether a radio, or any radio when none is given, has one of the state flags set */
static
BOOL
BthpRadioHasFlag(
    _In_opt_ HANDLE Radio,
    _In_ ULONG Flag)
{
    BLUETOOTH_FIND_RADIO_PARAMS Params;
    BTH_LOCAL_RADIO_INFO Local;
    HBLUETOOTH_RADIO_FIND Find;
    HANDLE Next;
    BOOL Result = FALSE;

    if (Radio)
    {
        if (BthpQueryLocalRadio(Radio, &Local) != ERROR_SUCCESS)
            return FALSE;
        return (Local.flags & Flag) != 0;
    }

    Params.dwSize = sizeof(Params);
    Find = BluetoothFindFirstRadio(&Params, &Next);
    if (!Find)
        return FALSE;

    do
    {
        if (BthpRadioHasFlag(Next, Flag))
            Result = TRUE;
        CloseHandle(Next);
    } while (BluetoothFindNextRadio(Find, &Next));

    BluetoothFindRadioClose(Find);
    return Result;
}

BOOL
WINAPI
BluetoothIsConnectable(
    _In_opt_ HANDLE hRadio)
{
    return BthpRadioHasFlag(hRadio, LOCAL_RADIO_CONNECTABLE);
}

BOOL
WINAPI
BluetoothIsDiscoverable(
    _In_opt_ HANDLE hRadio)
{
    return BthpRadioHasFlag(hRadio, LOCAL_RADIO_DISCOVERABLE);
}

/*
 * The support service counts one request per client and fails a repeated
 * enable or an unmatched disable. With no radio the request itself succeeds.
 */
static
BOOL
BthpSetScanState(
    _Inout_ LONG *State,
    _In_ BOOL Enable)
{
    LONG Wanted = Enable ? 1 : 0;

    if (InterlockedCompareExchange(State, Wanted, !Wanted) != !Wanted)
    {
        SetLastError(HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER));
        return FALSE;
    }

    return TRUE;
}

BOOL
WINAPI
BluetoothEnableDiscovery(
    _In_opt_ HANDLE hRadio,
    _In_ BOOL fEnabled)
{
    UNREFERENCED_PARAMETER(hRadio);
    return BthpSetScanState(&BthpDiscoveryEnabled, fEnabled);
}

BOOL
WINAPI
BluetoothEnableIncomingConnections(
    _In_opt_ HANDLE hRadio,
    _In_ BOOL fEnabled)
{
    UNREFERENCED_PARAMETER(hRadio);
    return BthpSetScanState(&BthpIncomingEnabled, fEnabled);
}

BOOL
WINAPI
BluetoothIsVersionAvailable(
    _In_ UCHAR MajorVersion,
    _In_ UCHAR MinorVersion)
{
    /* The binaries implement version 2.1 of the API */
    if (MajorVersion != 2)
        return MajorVersion < 2;

    return MinorVersion <= 1;
}
