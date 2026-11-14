/*
 * PROJECT:     ReactOS Wireless Network Connection
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     A small front end that scans for networks and connects to one
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <windows.h>
#include <commctrl.h>
#include <wlanapi.h>
#include <strsafe.h>

#include "resource.h"

static HINSTANCE g_Instance;
static HANDLE g_Client;
static GUID g_Interface;
static BOOL g_HaveInterface;
static PWLAN_AVAILABLE_NETWORK_LIST g_Networks;

/* Turns an SSID's bytes into a display string */
static
VOID
SsidToText(
    _In_ const DOT11_SSID *Ssid,
    _Out_writes_(Count) PWSTR Text,
    _In_ int Count)
{
    if (Ssid->uSSIDLength == 0)
    {
        StringCchCopyW(Text, Count, L"(hidden)");
        return;
    }

    int Written = MultiByteToWideChar(CP_UTF8, 0, (const char *)Ssid->ucSSID,
                                      Ssid->uSSIDLength, Text, Count - 1);
    if (Written <= 0)
        Written = 0;
    Text[Written] = L'\0';
}

static
PCWSTR
SecurityText(
    _In_ const WLAN_AVAILABLE_NETWORK *Network)
{
    if (!Network->bSecurityEnabled)
        return L"Open";

    switch (Network->dot11DefaultAuthAlgorithm)
    {
        case DOT11_AUTH_ALGO_RSNA_PSK: return L"WPA2-Personal";
        case DOT11_AUTH_ALGO_WPA_PSK:  return L"WPA-Personal";
        case DOT11_AUTH_ALGO_RSNA:     return L"WPA2-Enterprise";
        case DOT11_AUTH_ALGO_WPA:      return L"WPA-Enterprise";
        default:                       return L"WEP";
    }
}

static
VOID
SetStatus(
    _In_ HWND Dialog,
    _In_ PCWSTR Text)
{
    SetDlgItemTextW(Dialog, IDC_STATUS, Text);
}

static
VOID
SetupList(
    _In_ HWND List)
{
    LVCOLUMNW Column;

    ListView_SetExtendedListViewStyle(List, LVS_EX_FULLROWSELECT);

    RtlZeroMemory(&Column, sizeof(Column));
    Column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    Column.pszText = L"Network";
    Column.cx = 150;
    Column.iSubItem = 0;
    ListView_InsertColumn(List, 0, &Column);

    Column.pszText = L"Signal";
    Column.cx = 50;
    Column.iSubItem = 1;
    ListView_InsertColumn(List, 1, &Column);

    Column.pszText = L"Security";
    Column.cx = 66;
    Column.iSubItem = 2;
    ListView_InsertColumn(List, 2, &Column);
}

static
VOID
FillList(
    _In_ HWND List)
{
    DWORD i;

    ListView_DeleteAllItems(List);
    if (g_Networks == NULL)
        return;

    for (i = 0; i < g_Networks->dwNumberOfItems; i++)
    {
        WLAN_AVAILABLE_NETWORK *Network = &g_Networks->Network[i];
        WCHAR Text[64];
        LVITEMW Item;
        int Row;

        RtlZeroMemory(&Item, sizeof(Item));
        Item.mask = LVIF_TEXT | LVIF_PARAM;
        Item.iItem = (int)i;
        Item.lParam = (LPARAM)i;
        SsidToText(&Network->dot11Ssid, Text, ARRAYSIZE(Text));
        Item.pszText = Text;
        Row = ListView_InsertItem(List, &Item);
        if (Row < 0)
            continue;

        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%u%%", Network->wlanSignalQuality);
        ListView_SetItemText(List, Row, 1, Text);
        ListView_SetItemText(List, Row, 2, (PWSTR)SecurityText(Network));
    }
}

static
WLAN_AVAILABLE_NETWORK *
SelectedNetwork(
    _In_ HWND List)
{
    int Selected = ListView_GetNextItem(List, -1, LVNI_SELECTED);
    LVITEMW Item;

    if (Selected < 0 || g_Networks == NULL)
        return NULL;

    RtlZeroMemory(&Item, sizeof(Item));
    Item.mask = LVIF_PARAM;
    Item.iItem = Selected;
    if (!ListView_GetItem(List, &Item))
        return NULL;

    if ((DWORD)Item.lParam >= g_Networks->dwNumberOfItems)
        return NULL;

    return &g_Networks->Network[Item.lParam];
}

static
VOID
DoScan(
    _In_ HWND Dialog)
{
    DWORD Result;

    if (!g_HaveInterface)
    {
        SetStatus(Dialog, L"No wireless adapter was found.");
        return;
    }

    SetStatus(Dialog, L"Scanning for networks...");

    Result = WlanScan(g_Client, &g_Interface, NULL, NULL, NULL);
    if (Result != ERROR_SUCCESS)
    {
        SetStatus(Dialog, L"The scan could not be started.");
        return;
    }

    if (g_Networks != NULL)
    {
        WlanFreeMemory(g_Networks);
        g_Networks = NULL;
    }

    Result = WlanGetAvailableNetworkList(g_Client, &g_Interface, 0, NULL, &g_Networks);
    if (Result != ERROR_SUCCESS || g_Networks == NULL)
    {
        SetStatus(Dialog, L"The network list could not be read.");
        return;
    }

    FillList(GetDlgItem(Dialog, IDC_NETWORKS));

    {
        WCHAR Text[64];
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"Found %u network(s).",
                         g_Networks->dwNumberOfItems);
        SetStatus(Dialog, Text);
    }
}

/* Builds the WLAN profile XML a secured connect is driven from */
static
VOID
BuildProfileXml(
    _In_ PCWSTR Ssid,
    _In_ const WLAN_AVAILABLE_NETWORK *Network,
    _In_ PCWSTR Key,
    _Out_writes_(Count) PWSTR Xml,
    _In_ int Count)
{
    PCWSTR Authentication = L"WPA2PSK";
    PCWSTR Encryption = L"AES";

    if (Network->dot11DefaultAuthAlgorithm == DOT11_AUTH_ALGO_WPA_PSK)
        Authentication = L"WPAPSK";
    if (Network->dot11DefaultCipherAlgorithm == DOT11_CIPHER_ALGO_TKIP)
        Encryption = L"TKIP";

    StringCchPrintfW(Xml, Count,
        L"<?xml version=\"1.0\"?>"
        L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
        L"<name>%s</name>"
        L"<SSIDConfig><SSID><name>%s</name></SSID></SSIDConfig>"
        L"<connectionType>ESS</connectionType>"
        L"<MSM><security>"
        L"<authEncryption>"
        L"<authentication>%s</authentication>"
        L"<encryption>%s</encryption>"
        L"<useOneX>false</useOneX>"
        L"</authEncryption>"
        L"<sharedKey>"
        L"<keyType>passPhrase</keyType>"
        L"<protected>false</protected>"
        L"<keyMaterial>%s</keyMaterial>"
        L"</sharedKey>"
        L"</security></MSM>"
        L"</WLANProfile>",
        Ssid, Ssid, Authentication, Encryption, Key);
}

static
VOID
DoConnect(
    _In_ HWND Dialog)
{
    HWND List = GetDlgItem(Dialog, IDC_NETWORKS);
    WLAN_AVAILABLE_NETWORK *Network = SelectedNetwork(List);
    WLAN_CONNECTION_PARAMETERS Parameters;
    DOT11_SSID Ssid;
    WCHAR SsidText[64];
    WCHAR Xml[1024];
    WCHAR Key[128];
    DWORD Result;

    if (Network == NULL)
    {
        SetStatus(Dialog, L"Select a network first.");
        return;
    }

    Ssid = Network->dot11Ssid;
    SsidToText(&Ssid, SsidText, ARRAYSIZE(SsidText));

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.pDot11Ssid = &Ssid;
    Parameters.dot11BssType = dot11_BSS_type_infrastructure;

    if (Network->bSecurityEnabled)
    {
        GetDlgItemTextW(Dialog, IDC_KEY, Key, ARRAYSIZE(Key));
        if (Key[0] == L'\0')
        {
            SetStatus(Dialog, L"This network needs a passphrase.");
            return;
        }

        BuildProfileXml(SsidText, Network, Key, Xml, ARRAYSIZE(Xml));
        Parameters.wlanConnectionMode = wlan_connection_mode_temporary_profile;
        Parameters.strProfile = Xml;
    }
    else
    {
        Parameters.wlanConnectionMode = wlan_connection_mode_discovery_unsecure;
    }

    SetStatus(Dialog, L"Connecting...");
    Result = WlanConnect(g_Client, &g_Interface, &Parameters, NULL);

    if (Result == ERROR_SUCCESS)
        SetStatus(Dialog, L"Connected.");
    else
        SetStatus(Dialog, L"The connection attempt failed.");
}

static
VOID
OpenWlanService(
    _In_ HWND Dialog)
{
    DWORD Negotiated = 0;
    PWLAN_INTERFACE_INFO_LIST Interfaces = NULL;

    if (WlanOpenHandle(2, NULL, &Negotiated, &g_Client) != ERROR_SUCCESS)
    {
        g_Client = NULL;
        SetStatus(Dialog, L"The WLAN service is not available.");
        return;
    }

    if (WlanEnumInterfaces(g_Client, NULL, &Interfaces) == ERROR_SUCCESS &&
        Interfaces != NULL && Interfaces->dwNumberOfItems != 0)
    {
        g_Interface = Interfaces->InterfaceInfo[0].InterfaceGuid;
        g_HaveInterface = TRUE;
        SetStatus(Dialog, L"Ready. Press Scan to look for networks.");
    }
    else
    {
        SetStatus(Dialog, L"No wireless adapter was found.");
    }

    if (Interfaces != NULL)
        WlanFreeMemory(Interfaces);
}

static
INT_PTR
CALLBACK
MainDlgProc(
    _In_ HWND Dialog,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    switch (Message)
    {
        case WM_INITDIALOG:
            SetupList(GetDlgItem(Dialog, IDC_NETWORKS));
            OpenWlanService(Dialog);
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDC_SCAN:
                    DoScan(Dialog);
                    return TRUE;

                case IDC_CONNECT:
                    DoConnect(Dialog);
                    return TRUE;

                case IDCANCEL:
                    EndDialog(Dialog, 0);
                    return TRUE;
            }
            break;

        case WM_DESTROY:
            if (g_Networks != NULL)
            {
                WlanFreeMemory(g_Networks);
                g_Networks = NULL;
            }
            if (g_Client != NULL)
            {
                WlanCloseHandle(g_Client, NULL);
                g_Client = NULL;
            }
            return TRUE;
    }

    return FALSE;
}

int
WINAPI
wWinMain(
    _In_ HINSTANCE Instance,
    _In_opt_ HINSTANCE Previous,
    _In_ PWSTR CommandLine,
    _In_ int Show)
{
    INITCOMMONCONTROLSEX Controls;

    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CommandLine);
    UNREFERENCED_PARAMETER(Show);

    g_Instance = Instance;

    Controls.dwSize = sizeof(Controls);
    Controls.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&Controls);

    DialogBoxParamW(Instance, MAKEINTRESOURCEW(IDD_MAIN), NULL, MainDlgProc, 0);
    return 0;
}
