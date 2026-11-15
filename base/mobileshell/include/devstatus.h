/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Device status: power, network, Wi-Fi signal and master volume
 */

#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <wlanapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <strsafe.h>
#include "uixptr.h"

#define DEV_BATTERY_UNKNOWN 255

typedef enum _DEV_NETWORK
{
    DevNetworkNone = 0,
    DevNetworkWired,
    DevNetworkWireless
} DEV_NETWORK;

typedef struct _DEV_STATUS
{
    /* Power */
    ULONG BatteryPercent;
    BOOL HasBattery;
    BOOL OnAc;
    BOOL Charging;
    ULONG SecondsLeft;

    /* Network */
    DEV_NETWORK Network;
    ULONG Quality;
    WCHAR Ssid[33];
    WCHAR Adapter[64];

    /* Audio, Volume is 0..100 or -1 when there is no endpoint */
    LONG Volume;
    BOOL Muted;
} DEV_STATUS, *PDEV_STATUS;

inline
VOID
DevQueryPower(
    _Inout_ PDEV_STATUS Status)
{
    SYSTEM_POWER_STATUS Power;

    Status->BatteryPercent = DEV_BATTERY_UNKNOWN;
    Status->HasBattery = FALSE;
    Status->OnAc = TRUE;
    Status->Charging = FALSE;
    Status->SecondsLeft = (ULONG)-1;

    if (!GetSystemPowerStatus(&Power))
        return;

    /* 128 is "no system battery", 255 is "unknown status" */
    Status->HasBattery = !(Power.BatteryFlag & 128) && Power.BatteryFlag != 255;
    Status->OnAc = (Power.ACLineStatus != 0);
    Status->Charging = Status->HasBattery && (Power.BatteryFlag & 8);
    if (Power.BatteryLifePercent <= 100)
        Status->BatteryPercent = Power.BatteryLifePercent;
    Status->SecondsLeft = Power.BatteryLifeTime;
}

#ifdef __REACTOS__
/* ReactOS wlanapi reports success from WlanQueryInterface without filling it in, so ask the adapters only */
inline
VOID
DevQueryWireless(
    _Inout_ PDEV_STATUS Status)
{
    UNREFERENCED_PARAMETER(Status);
}
#else
inline
VOID
DevQueryWireless(
    _Inout_ PDEV_STATUS Status)
{
    PWLAN_INTERFACE_INFO_LIST List = NULL;
    PWLAN_CONNECTION_ATTRIBUTES Connection;
    HANDLE Client = NULL;
    DWORD Version;
    DWORD Size;

    /* wlanapi is delay loaded, a missing service or DLL just means no Wi-Fi data */
    __try
    {
        if (WlanOpenHandle(2, NULL, &Version, &Client) != ERROR_SUCCESS)
            return;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }

    if (WlanEnumInterfaces(Client, NULL, &List) == ERROR_SUCCESS)
    {
        for (DWORD Index = 0; Index < List->dwNumberOfItems; Index++)
        {
            if (List->InterfaceInfo[Index].isState != wlan_interface_state_connected)
                continue;

            Connection = NULL;
            if (WlanQueryInterface(Client, &List->InterfaceInfo[Index].InterfaceGuid, wlan_intf_opcode_current_connection,
                                   NULL, &Size, reinterpret_cast<PVOID *>(&Connection), NULL) == ERROR_SUCCESS)
            {
                const DOT11_SSID &Ssid = Connection->wlanAssociationAttributes.dot11Ssid;

                Status->Network = DevNetworkWireless;
                Status->Quality = Connection->wlanAssociationAttributes.wlanSignalQuality;
                MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<LPCSTR>(Ssid.ucSSID), static_cast<int>(min(Ssid.uSSIDLength, 32UL)),
                                    Status->Ssid, ARRAYSIZE(Status->Ssid) - 1);
                WlanFreeMemory(Connection);
                break;
            }
        }
        WlanFreeMemory(List);
    }
    WlanCloseHandle(Client, NULL);
}
#endif

inline
VOID
DevQueryNetwork(
    _Inout_ PDEV_STATUS Status)
{
    PIP_ADAPTER_ADDRESSES Addresses;
    PIP_ADAPTER_ADDRESSES Walk;
    ULONG Size = 16 * 1024;

    Status->Network = DevNetworkNone;
    Status->Quality = 0;
    Status->Ssid[0] = UNICODE_NULL;
    Status->Adapter[0] = UNICODE_NULL;

    Addresses = static_cast<PIP_ADAPTER_ADDRESSES>(HeapAlloc(GetProcessHeap(), 0, Size));
    if (!Addresses)
        return;

    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST,
                             NULL, Addresses, &Size) == ERROR_SUCCESS)
    {
        /* Connected means up, not loopback or tunnel, and holding a default gateway */
        for (Walk = Addresses; Walk; Walk = Walk->Next)
        {
            if (Walk->OperStatus != IfOperStatusUp || !Walk->FirstGatewayAddress ||
                Walk->IfType == IF_TYPE_SOFTWARE_LOOPBACK || Walk->IfType == IF_TYPE_TUNNEL)
            {
                continue;
            }

            StringCchCopyW(Status->Adapter, ARRAYSIZE(Status->Adapter), Walk->FriendlyName);
            if (Walk->IfType == IF_TYPE_IEEE80211)
            {
                Status->Network = DevNetworkWireless;
                break;
            }
            Status->Network = DevNetworkWired;
            Status->Quality = 100;
        }
    }
    HeapFree(GetProcessHeap(), 0, Addresses);

    if (Status->Network == DevNetworkWireless || Status->Network == DevNetworkNone)
        DevQueryWireless(Status);
}

inline
HRESULT
DevOpenVolume(
    _Outptr_ IAudioEndpointVolume **Volume)
{
    UixPtr<IMMDeviceEnumerator> Enumerator;
    UixPtr<IMMDevice> Device;
    HRESULT Status;

    *Volume = NULL;
    Status = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER,
                              __uuidof(IMMDeviceEnumerator), Enumerator.PutVoid());
    if (SUCCEEDED(Status))
        Status = Enumerator->GetDefaultAudioEndpoint(eRender, eConsole, Device.Put());
    if (SUCCEEDED(Status))
        Status = Device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, NULL, reinterpret_cast<PVOID *>(Volume));
    return Status;
}

inline
VOID
DevQueryVolume(
    _Inout_ PDEV_STATUS Status)
{
    UixPtr<IAudioEndpointVolume> Volume;
    FLOAT Level = 0.0f;
    BOOL Muted = FALSE;

    Status->Volume = -1;
    Status->Muted = FALSE;
    if (FAILED(DevOpenVolume(Volume.Put())))
        return;

    if (SUCCEEDED(Volume->GetMasterVolumeLevelScalar(&Level)))
        Status->Volume = static_cast<LONG>(Level * 100.0f + 0.5f);
    if (SUCCEEDED(Volume->GetMute(&Muted)))
        Status->Muted = Muted;
}

/* Delta in percent; Mute 0 or 1 sets it, -1 toggles, any other value leaves it */
inline
VOID
DevChangeVolume(
    _In_ LONG Delta,
    _In_ LONG Mute)
{
    UixPtr<IAudioEndpointVolume> Volume;
    FLOAT Level = 0.0f;
    BOOL Muted = FALSE;

    if (FAILED(DevOpenVolume(Volume.Put())))
        return;

    if (Delta && SUCCEEDED(Volume->GetMasterVolumeLevelScalar(&Level)))
    {
        Level = max(0.0f, min(1.0f, Level + Delta / 100.0f));
        Volume->SetMasterVolumeLevelScalar(Level, NULL);
        if (Delta > 0)
            Volume->SetMute(FALSE, NULL);
    }

    if (Mute == -1 && SUCCEEDED(Volume->GetMute(&Muted)))
        Volume->SetMute(!Muted, NULL);
    else if (Mute == 0 || Mute == 1)
        Volume->SetMute(Mute, NULL);
}

inline
VOID
DevQueryAll(
    _Out_ PDEV_STATUS Status)
{
    ZeroMemory(Status, sizeof(*Status));
    DevQueryPower(Status);
    DevQueryNetwork(Status);
    DevQueryVolume(Status);
}
