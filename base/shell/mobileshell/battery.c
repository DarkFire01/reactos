/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Battery level for the status row
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <windows.h>
#include "mobileshell.h"
#include <setupapi.h>
#include <initguid.h>
#include <batclass.h>

/* Gauges that register no battery interface are still reachable by their device name */
#define MS_GAUGE_DEVICE         L"\\\\.\\GLOBALROOT\\Device\\QcomBattery"

/* Looking for a battery again only every so many reads while none answers */
#define MS_BATTERY_RETRY_READS  10

static HANDLE MsBatteryDevice = INVALID_HANDLE_VALUE;
static ULONG MsBatteryRetry;

static
HANDLE
NTAPI
MsBatteryOpenInterface(VOID)
{
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W Detail;
    SP_DEVICE_INTERFACE_DATA Interface;
    HANDLE Device = INVALID_HANDLE_VALUE;
    DWORD Size = 0;
    HDEVINFO Set;

    Set = SetupDiGetClassDevsW(&GUID_DEVICE_BATTERY, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (Set == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;

    Interface.cbSize = sizeof(Interface);
    if (SetupDiEnumDeviceInterfaces(Set, NULL, &GUID_DEVICE_BATTERY, 0, &Interface))
    {
        SetupDiGetDeviceInterfaceDetailW(Set, &Interface, NULL, 0, &Size, NULL);
        Detail = (Size >= sizeof(*Detail)) ? HeapAlloc(GetProcessHeap(), 0, Size) : NULL;
        if (Detail)
        {
            Detail->cbSize = sizeof(*Detail);
            if (SetupDiGetDeviceInterfaceDetailW(Set, &Interface, Detail, Size, NULL, NULL))
            {
                Device = CreateFileW(Detail->DevicePath,
                                     GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL,
                                     OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL,
                                     NULL);
            }

            HeapFree(GetProcessHeap(), 0, Detail);
        }
    }

    SetupDiDestroyDeviceInfoList(Set);
    return Device;
}

static
BOOL
NTAPI
MsBatteryQuery(
    _In_ HANDLE Device,
    _Out_ PULONG Percent,
    _Out_ PBOOL Charging)
{
    BATTERY_QUERY_INFORMATION Query;
    BATTERY_INFORMATION Information;
    BATTERY_WAIT_STATUS Wait;
    BATTERY_STATUS Status;
    ULONG Timeout = 0, Tag = BATTERY_TAG_INVALID, Full;
    DWORD Returned;

    if (!DeviceIoControl(Device, IOCTL_BATTERY_QUERY_TAG, &Timeout, sizeof(Timeout), &Tag, sizeof(Tag), &Returned, NULL) ||
        Tag == BATTERY_TAG_INVALID)
    {
        return FALSE;
    }

    ZeroMemory(&Query, sizeof(Query));
    Query.BatteryTag = Tag;
    Query.InformationLevel = BatteryInformation;
    if (!DeviceIoControl(Device,
                         IOCTL_BATTERY_QUERY_INFORMATION,
                         &Query,
                         sizeof(Query),
                         &Information,
                         sizeof(Information),
                         &Returned,
                         NULL))
    {
        return FALSE;
    }

    ZeroMemory(&Wait, sizeof(Wait));
    Wait.BatteryTag = Tag;
    if (!DeviceIoControl(Device, IOCTL_BATTERY_QUERY_STATUS, &Wait, sizeof(Wait), &Status, sizeof(Status), &Returned, NULL) ||
        Status.Capacity == BATTERY_UNKNOWN_CAPACITY)
    {
        return FALSE;
    }

    /* A relative gauge reports in percent of a full charge of 100 */
    Full = Information.FullChargedCapacity;
    if (Full == 0 || Full == BATTERY_UNKNOWN_CAPACITY)
        Full = 100;

    *Percent = min(100, (ULONG)((ULONGLONG)Status.Capacity * 100 / Full));
    *Charging = (Status.PowerState & BATTERY_CHARGING) != 0;
    return TRUE;
}

/**
 * @brief
 * Reads the battery level from the first battery that answers.
 *
 * @return
 * FALSE when the device reports no battery at all.
 */
BOOL
NTAPI
MsBatteryRead(
    _Out_ PULONG Percent,
    _Out_ PBOOL Charging)
{
    SYSTEM_POWER_STATUS Power;

    *Percent = 0;
    *Charging = FALSE;

    if (MsBatteryDevice == INVALID_HANDLE_VALUE && MsBatteryRetry == 0)
    {
        MsBatteryDevice = MsBatteryOpenInterface();
        if (MsBatteryDevice == INVALID_HANDLE_VALUE)
        {
            MsBatteryDevice = CreateFileW(MS_GAUGE_DEVICE,
                                          GENERIC_READ | GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          NULL,
                                          OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL,
                                          NULL);
        }

        if (MsBatteryDevice == INVALID_HANDLE_VALUE)
            MsBatteryRetry = MS_BATTERY_RETRY_READS;
    }
    else if (MsBatteryRetry)
    {
        MsBatteryRetry--;
    }

    if (MsBatteryDevice != INVALID_HANDLE_VALUE)
    {
        if (MsBatteryQuery(MsBatteryDevice, Percent, Charging))
            return TRUE;

        CloseHandle(MsBatteryDevice);
        MsBatteryDevice = INVALID_HANDLE_VALUE;
        MsBatteryRetry = MS_BATTERY_RETRY_READS;
    }

    /* Batteries the power manager already knows about */
    if (GetSystemPowerStatus(&Power) && Power.BatteryLifePercent <= 100 && !(Power.BatteryFlag & BATTERY_FLAG_NO_BATTERY))
    {
        *Percent = Power.BatteryLifePercent;
        *Charging = (Power.BatteryFlag & BATTERY_FLAG_CHARGING) != 0;
        return TRUE;
    }

    return FALSE;
}
