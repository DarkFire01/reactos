/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Native Wifi client for Settings: radio, scan, networks, join, disconnect, forget
 */

#pragma once

#include <wlanapi.h>
#include <string>
#include <vector>
#include <algorithm>

typedef struct _WIFI_NETWORK
{
    std::wstring Ssid;
    std::wstring Profile;
    DOT11_SSID RawSsid = {};
    DOT11_AUTH_ALGORITHM Auth = DOT11_AUTH_ALGO_80211_OPEN;
    ULONG Quality = 0;
    BOOL Secured = FALSE;
    BOOL Connected = FALSE;
    BOOL Saved = FALSE;
    BOOL Supported = TRUE;
} WIFI_NETWORK;

class CWifi
{
public:
    ~CWifi()
    {
        if (m_Client)
            WlanCloseHandle(m_Client, NULL);
    }

    /* Opens the client and picks the first wireless interface */
    BOOL
    Open(VOID)
    {
        PWLAN_INTERFACE_INFO_LIST Interfaces = NULL;
        DWORD Version = 0;

        if (m_Client && m_HaveInterface)
            return TRUE;

        if (!m_Client && WlanOpenHandle(2, NULL, &Version, &m_Client) != ERROR_SUCCESS)
        {
            m_Client = NULL;
            return FALSE;
        }

        if (WlanEnumInterfaces(m_Client, NULL, &Interfaces) == ERROR_SUCCESS && Interfaces)
        {
            if (Interfaces->dwNumberOfItems > 0)
            {
                m_Interface = Interfaces->InterfaceInfo[0].InterfaceGuid;
                m_HaveInterface = TRUE;
            }
            WlanFreeMemory(Interfaces);
        }
        return m_HaveInterface;
    }

    BOOL
    RadioOn(VOID)
    {
        PWLAN_RADIO_STATE State = NULL;
        DWORD Size = 0;
        BOOL On = TRUE;

        if (!Open())
            return FALSE;

        if (WlanQueryInterface(m_Client, &m_Interface, wlan_intf_opcode_radio_state, NULL, &Size,
                               reinterpret_cast<PVOID *>(&State), NULL) == ERROR_SUCCESS && State)
        {
            if (State->dwNumberOfPhys > 0)
            {
                On = State->PhyRadioState[0].dot11SoftwareRadioState == dot11_radio_state_on &&
                     State->PhyRadioState[0].dot11HardwareRadioState == dot11_radio_state_on;
            }
            WlanFreeMemory(State);
        }
        return On;
    }

    DWORD
    SetRadio(
        _In_ BOOL On)
    {
        WLAN_PHY_RADIO_STATE State = {};

        if (!Open())
            return ERROR_NOT_FOUND;

        State.dwPhyIndex = 0;
        State.dot11SoftwareRadioState = On ? dot11_radio_state_on : dot11_radio_state_off;
        State.dot11HardwareRadioState = dot11_radio_state_on;
        return WlanSetInterface(m_Client, &m_Interface, wlan_intf_opcode_radio_state, sizeof(State), &State, NULL);
    }

    DWORD
    Scan(VOID)
    {
        if (!Open())
            return ERROR_NOT_FOUND;
        return WlanScan(m_Client, &m_Interface, NULL, NULL, NULL);
    }

    /* One entry per network name, the connected or saved one winning, strongest first */
    DWORD
    Networks(
        _Out_ std::vector<WIFI_NETWORK> &List)
    {
        PWLAN_AVAILABLE_NETWORK_LIST Available = NULL;
        DWORD Error;

        List.clear();
        if (!Open())
            return ERROR_NOT_FOUND;

        Error = WlanGetAvailableNetworkList(m_Client, &m_Interface, 0, NULL, &Available);
        if (Error != ERROR_SUCCESS || !Available)
            return Error;

        for (DWORD Index = 0; Index < Available->dwNumberOfItems; Index++)
        {
            const WLAN_AVAILABLE_NETWORK &Entry = Available->Network[Index];
            WIFI_NETWORK Network;

            if (Entry.dot11Ssid.uSSIDLength == 0)
                continue;

            Network.RawSsid = Entry.dot11Ssid;
            Network.Ssid = SsidText(Entry.dot11Ssid);
            Network.Profile = Entry.strProfileName;
            Network.Auth = Entry.dot11DefaultAuthAlgorithm;
            Network.Quality = Entry.wlanSignalQuality;
            Network.Secured = Entry.bSecurityEnabled;
            Network.Connected = (Entry.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;
            Network.Saved = (Entry.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) != 0;
            Network.Supported = Entry.bNetworkConnectable &&
                                (!Network.Secured || Network.Auth == DOT11_AUTH_ALGO_RSNA_PSK ||
                                 Network.Auth == DOT11_AUTH_ALGO_WPA_PSK);

            auto Same = std::find_if(List.begin(), List.end(),
                                     [&](const WIFI_NETWORK &Other) { return Other.Ssid == Network.Ssid; });
            if (Same == List.end())
            {
                List.push_back(Network);
            }
            else
            {
                Same->Quality = max(Same->Quality, Network.Quality);
                if (Network.Connected || (Network.Saved && !Same->Connected))
                {
                    Network.Quality = Same->Quality;
                    *Same = Network;
                }
            }
        }
        WlanFreeMemory(Available);

        std::sort(List.begin(), List.end(), [](const WIFI_NETWORK &A, const WIFI_NETWORK &B)
        {
            if (A.Connected != B.Connected)
                return A.Connected != FALSE;
            return A.Quality > B.Quality;
        });
        return ERROR_SUCCESS;
    }

    /**
     * Joins a network. A saved profile is used as is; otherwise a profile is written for it,
     * saved when the service keeps profiles, and handed to the connect call when it does not.
     */
    DWORD
    Connect(
        _In_ const WIFI_NETWORK &Network,
        _In_ const std::wstring &Password)
    {
        WLAN_CONNECTION_PARAMETERS Parameters = {};
        DOT11_SSID Ssid = Network.RawSsid;
        std::wstring Xml;
        DWORD Reason = 0;

        if (!Open())
            return ERROR_NOT_FOUND;

        Parameters.pDot11Ssid = &Ssid;
        Parameters.dot11BssType = dot11_BSS_type_infrastructure;

        if (Network.Saved && !Network.Profile.empty() && Password.empty())
        {
            Parameters.wlanConnectionMode = wlan_connection_mode_profile;
            Parameters.strProfile = Network.Profile.c_str();
            return WlanConnect(m_Client, &m_Interface, &Parameters, NULL);
        }

        Xml = ProfileXml(Network, Password);
        if (WlanSetProfile(m_Client, &m_Interface, 0, Xml.c_str(), NULL, TRUE, NULL, &Reason) == ERROR_SUCCESS)
        {
            Parameters.wlanConnectionMode = wlan_connection_mode_profile;
            Parameters.strProfile = Network.Ssid.c_str();
        }
        else
        {
            Parameters.wlanConnectionMode = wlan_connection_mode_temporary_profile;
            Parameters.strProfile = Xml.c_str();
        }
        return WlanConnect(m_Client, &m_Interface, &Parameters, NULL);
    }

    DWORD
    Disconnect(VOID)
    {
        if (!Open())
            return ERROR_NOT_FOUND;
        return WlanDisconnect(m_Client, &m_Interface, NULL);
    }

    DWORD
    Forget(
        _In_ const WIFI_NETWORK &Network)
    {
        if (!Open())
            return ERROR_NOT_FOUND;
        return WlanDeleteProfile(m_Client, &m_Interface, Network.Profile.empty() ? Network.Ssid.c_str() : Network.Profile.c_str(), NULL);
    }

private:
    static
    std::wstring
    SsidText(
        _In_ const DOT11_SSID &Ssid)
    {
        WCHAR Text[DOT11_SSID_MAX_LENGTH + 1] = {};
        INT Length;

        Length = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<LPCSTR>(Ssid.ucSSID), Ssid.uSSIDLength, Text, DOT11_SSID_MAX_LENGTH);
        if (Length <= 0)
        {
            for (ULONG Index = 0; Index < Ssid.uSSIDLength; Index++)
                Text[Index] = Ssid.ucSSID[Index];
            Length = Ssid.uSSIDLength;
        }
        return std::wstring(Text, Length);
    }

    static
    std::wstring
    Escape(
        _In_ const std::wstring &Text)
    {
        std::wstring Out;

        for (WCHAR Char : Text)
        {
            switch (Char)
            {
                case L'&': Out += L"&amp;"; break;
                case L'<': Out += L"&lt;"; break;
                case L'>': Out += L"&gt;"; break;
                case L'"': Out += L"&quot;"; break;
                case L'\'': Out += L"&apos;"; break;
                default: Out += Char; break;
            }
        }
        return Out;
    }

    /* A WLAN profile in the v1 schema: open, WPA personal or WPA2 personal */
    static
    std::wstring
    ProfileXml(
        _In_ const WIFI_NETWORK &Network,
        _In_ const std::wstring &Password)
    {
        std::wstring Name = Escape(Network.Ssid);
        std::wstring Xml;

        Xml = L"<?xml version=\"1.0\"?>"
              L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
              L"<name>" + Name + L"</name>"
              L"<SSIDConfig><SSID><name>" + Name + L"</name></SSID></SSIDConfig>"
              L"<connectionType>ESS</connectionType><connectionMode>auto</connectionMode>"
              L"<MSM><security><authEncryption>";

        if (!Network.Secured)
        {
            Xml += L"<authentication>open</authentication><encryption>none</encryption><useOneX>false</useOneX>"
                   L"</authEncryption>";
        }
        else
        {
            BOOL Wpa = (Network.Auth == DOT11_AUTH_ALGO_WPA_PSK);

            Xml += std::wstring(L"<authentication>") + (Wpa ? L"WPAPSK" : L"WPA2PSK") + L"</authentication>"
                   L"<encryption>" + (Wpa ? L"TKIP" : L"AES") + L"</encryption><useOneX>false</useOneX>"
                   L"</authEncryption><sharedKey><keyType>passPhrase</keyType><protected>false</protected>"
                   L"<keyMaterial>" + Escape(Password) + L"</keyMaterial></sharedKey>";
        }
        Xml += L"</security></MSM></WLANProfile>";
        return Xml;
    }

    HANDLE m_Client = NULL;
    GUID m_Interface = {};
    BOOL m_HaveInterface = FALSE;
};
