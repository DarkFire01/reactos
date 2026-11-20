/*
 * PROJECT:     ReactOS Settings
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Touch settings app for the phone
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "settings.h"
#include <winver.h>
#include <wlanapi.h>

/* The longest WPA passphrase, a 64 digit hex key is not taken by the supplicant */
#define ST_PASSPHRASE_MAX           63
#define ST_PASSPHRASE_MIN           8

typedef enum _ST_WIFI_OPERATION
{
    StWifiRefresh,
    StWifiScan,
    StWifiConnect,
    StWifiDisconnect
} ST_WIFI_OPERATION;

typedef enum _ST_WIFI_STATE
{
    StWifiChecking,
    StWifiNoService,
    StWifiNoAdapter,
    StWifiReady
} ST_WIFI_STATE;

/* One request for the Wi-Fi worker, it fills in the results and posts it back */
typedef struct _ST_WIFI_JOB
{
    ST_WIFI_OPERATION Operation;
    BOOL Quiet;
    BOOL HaveInterface;
    GUID InterfaceGuid;
    WLAN_AVAILABLE_NETWORK Network;
    WCHAR Passphrase[ST_PASSPHRASE_MAX + 1];

    DWORD OpenError;
    DWORD Error;
    PWLAN_INTERFACE_INFO_LIST Interfaces;
    PWLAN_AVAILABLE_NETWORK_LIST Networks;
    BOOL Connected;
    WLAN_CONNECTION_ATTRIBUTES Connection;
} ST_WIFI_JOB, *PST_WIFI_JOB;

HINSTANCE StInstance;
HWND StWindow;
HFONT StFont;
HFONT StSmallFont;
HFONT StBigFont;
HFONT StButtonFont;
HBRUSH StBgBrush;
DWORD StPage;
static HFONT StHeadFont;
static HBRUSH StFieldBrush;

/* Wi-Fi page state, owned by the UI thread */
static ST_WIFI_STATE StWifiState = StWifiChecking;
static DWORD StWifiServiceError;
static PWLAN_INTERFACE_INFO_LIST StInterfaces;
static GUID StInterfaceGuid;
static BOOL StHaveInterface;
static PWLAN_AVAILABLE_NETWORK_LIST StNetworks;
static DWORD *StOrder;
static DWORD StOrderCount;
static BOOL StConnected;
static WLAN_CONNECTION_ATTRIBUTES StConnection;
static BOOL StBusy;
static BOOL StAutoScanned;
static BOOL StJoining;
static BOOL StShowPassword;
static WLAN_AVAILABLE_NETWORK StJoinNetwork;

static ST_INFO_ROW StAboutRows[10];
static DWORD StAboutCount;

static const UINT StWifiListControls[] =
{
    IDC_WIFI_ADAPTER, IDC_WIFI_SCAN, IDC_WIFI_REFRESH, IDC_WIFI_DISCONNECT, IDC_WIFI_LIST
};

static const UINT StWifiJoinControls[] =
{
    IDC_WIFI_PASSWORD_LABEL, IDC_WIFI_PASSWORD, IDC_WIFI_SHOW_PASSWORD, IDC_WIFI_CONNECT, IDC_WIFI_BACK
};

static const UINT StWifiControls[] =
{
    IDC_WIFI_ADAPTER, IDC_WIFI_SCAN, IDC_WIFI_REFRESH, IDC_WIFI_DISCONNECT, IDC_WIFI_LIST,
    IDC_WIFI_PASSWORD_LABEL, IDC_WIFI_PASSWORD, IDC_WIFI_SHOW_PASSWORD, IDC_WIFI_CONNECT, IDC_WIFI_BACK
};
static const DWORD StWifiControlCount = _countof(StWifiControls);

VOID
StFillRound(
    _In_ HDC Dc,
    _In_ const RECT *Rect,
    _In_ int Radius,
    _In_ COLORREF Color)
{
    HBRUSH Brush = CreateSolidBrush(Color);
    HGDIOBJ OldBrush = SelectObject(Dc, Brush);
    HGDIOBJ OldPen = SelectObject(Dc, GetStockObject(NULL_PEN));

    RoundRect(Dc, Rect->left, Rect->top, Rect->right + 1, Rect->bottom + 1, Radius, Radius);

    SelectObject(Dc, OldPen);
    SelectObject(Dc, OldBrush);
    DeleteObject(Brush);
}

/* The summary card at the top of a page, a title over a wrapped detail line */
VOID
StDrawCard(
    _In_ HDC Dc,
    _In_ const RECT *Card,
    _In_ LPCWSTR Title,
    _In_ LPCWSTR Detail,
    _In_ BOOL Highlight)
{
    RECT Text;

    StFillRound(Dc, Card, ST_RADIUS, ST_TILE);
    SetBkMode(Dc, TRANSPARENT);

    Text = *Card;
    InflateRect(&Text, -ST_CARD_PAD, -ST_CARD_PAD / 2);
    Text.bottom = Text.top + 56;
    SelectObject(Dc, StBigFont);
    SetTextColor(Dc, Highlight ? ST_ACCENT : ST_TEXT);
    DrawTextW(Dc, Title, -1, &Text, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    Text.top = Text.bottom;
    Text.bottom = Card->bottom - ST_CARD_PAD / 2;
    SelectObject(Dc, StSmallFont);
    SetTextColor(Dc, ST_SUBTEXT);
    DrawTextW(Dc, Detail, -1, &Text, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
}

VOID
StSetInfoRow(
    _Out_ PST_INFO_ROW Row,
    _In_ LPCWSTR Label,
    _In_ LPCWSTR Format,
    ...)
{
    va_list Args;

    StringCchCopyW(Row->Label, _countof(Row->Label), Label);

    va_start(Args, Format);
    StringCchVPrintfW(Row->Value, _countof(Row->Value), Format, Args);
    va_end(Args);

    if (!Row->Value[0])
        StringCchCopyW(Row->Value, _countof(Row->Value), L"Unknown");
}

VOID
StPaintInfoRows(
    _In_ HDC Dc,
    _In_ const RECT *Area,
    _In_reads_(Count) const ST_INFO_ROW *Rows,
    _In_ DWORD Count)
{
    RECT Row = *Area;
    DWORD i;

    SetBkMode(Dc, TRANSPARENT);

    for (i = 0; i < Count && Row.top + ST_INFO_ROW_H <= Area->bottom; i++)
    {
        RECT Text;

        Row.bottom = Row.top + ST_INFO_ROW_H;

        Text = Row;
        Text.bottom = Text.top + 34;
        SelectObject(Dc, StSmallFont);
        SetTextColor(Dc, ST_SUBTEXT);
        DrawTextW(Dc, Rows[i].Label, -1, &Text, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX);

        Text.top = Text.bottom;
        Text.bottom = Row.bottom - ST_GAP;
        SelectObject(Dc, StFont);
        SetTextColor(Dc, ST_TEXT);
        DrawTextW(Dc, Rows[i].Value, -1, &Text, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

        Row.top = Row.bottom;
    }
}

BOOL
StStartWorker(
    _In_ LPTHREAD_START_ROUTINE Routine,
    _In_ PVOID Parameter)
{
    HANDLE Thread = CreateThread(NULL, 0, Routine, Parameter, 0, NULL);

    if (!Thread)
        return FALSE;

    CloseHandle(Thread);
    return TRUE;
}

VOID
StSetStatus(
    _In_ LPCWSTR Format,
    ...)
{
    WCHAR Text[256];
    va_list Args;

    va_start(Args, Format);
    StringCchVPrintfW(Text, _countof(Text), Format, Args);
    va_end(Args);

    SetDlgItemTextW(StWindow, IDC_STATUS, Text);
}

VOID
StShowGroup(
    _In_reads_(Count) const UINT *Ids,
    _In_ DWORD Count,
    _In_ BOOL Show)
{
    DWORD i;

    for (i = 0; i < Count; i++)
        ShowWindow(GetDlgItem(StWindow, Ids[i]), Show ? SW_SHOW : SW_HIDE);
}

VOID
StMove(
    _In_ UINT Id,
    _In_ int X,
    _In_ int Y,
    _In_ int Width,
    _In_ int Height)
{
    MoveWindow(GetDlgItem(StWindow, Id), X, Y, Width, Height, TRUE);
}

/* Wi-Fi helpers */

static
VOID
StSsidText(
    _In_ const DOT11_SSID *Ssid,
    _Out_writes_(Count) LPWSTR Text,
    _In_ DWORD Count)
{
    int Length;
    ULONG SsidLength = min(Ssid->uSSIDLength, (ULONG)sizeof(Ssid->ucSSID));

    Length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCSTR)Ssid->ucSSID,
                                 (int)SsidLength, Text, (int)Count - 1);
    if (Length <= 0)
    {
        Length = MultiByteToWideChar(CP_ACP, 0, (LPCSTR)Ssid->ucSSID,
                                     (int)SsidLength, Text, (int)Count - 1);
    }

    if (Length < 0)
        Length = 0;
    Text[Length] = UNICODE_NULL;
}

static
BOOL
StSameSsid(
    _In_ const DOT11_SSID *A,
    _In_ const DOT11_SSID *B)
{
    if (A->uSSIDLength != B->uSSIDLength || A->uSSIDLength > sizeof(A->ucSSID))
        return FALSE;

    return RtlEqualMemory(A->ucSSID, B->ucSSID, A->uSSIDLength);
}

static
LPCWSTR
StSecurityText(
    _In_ const WLAN_AVAILABLE_NETWORK *Network)
{
    if (!Network->bSecurityEnabled)
        return L"Open";

    switch (Network->dot11DefaultAuthAlgorithm)
    {
        case DOT11_AUTH_ALGO_RSNA_PSK:
            return L"WPA2-Personal";
        case DOT11_AUTH_ALGO_WPA_PSK:
            return L"WPA-Personal";
        case DOT11_AUTH_ALGO_RSNA:
            return L"WPA2-Enterprise";
        case DOT11_AUTH_ALGO_WPA:
            return L"WPA-Enterprise";
        case DOT11_AUTH_ALGO_80211_OPEN:
        case DOT11_AUTH_ALGO_80211_SHARED_KEY:
            return L"WEP";
        default:
            return L"Secured";
    }
}

/* The supplicant does open networks and the pre-shared key handshake only */
static
BOOL
StCanJoin(
    _In_ const WLAN_AVAILABLE_NETWORK *Network)
{
    if (!Network->bSecurityEnabled)
        return TRUE;

    return (Network->dot11DefaultAuthAlgorithm == DOT11_AUTH_ALGO_RSNA_PSK ||
            Network->dot11DefaultAuthAlgorithm == DOT11_AUTH_ALGO_WPA_PSK);
}

static
BOOL
StIsConnectedNetwork(
    _In_ const WLAN_AVAILABLE_NETWORK *Network)
{
    if (!StConnected)
        return FALSE;

    if (StConnection.wlanAssociationAttributes.dot11Ssid.uSSIDLength == 0)
        return (Network->dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;

    return StSameSsid(&Network->dot11Ssid, &StConnection.wlanAssociationAttributes.dot11Ssid);
}

static
const WLAN_INTERFACE_INFO *
StCurrentInterface(VOID)
{
    DWORD i;

    if (!StInterfaces || !StHaveInterface)
        return NULL;

    for (i = 0; i < StInterfaces->dwNumberOfItems; i++)
    {
        if (IsEqualGUID(&StInterfaces->InterfaceInfo[i].InterfaceGuid, &StInterfaceGuid))
            return &StInterfaces->InterfaceInfo[i];
    }

    return NULL;
}

VOID
StErrorText(
    _In_ DWORD Error,
    _Out_writes_(Count) LPWSTR Text,
    _In_ DWORD Count)
{
    DWORD Length;

    switch (Error)
    {
        case ERROR_TIMEOUT:
            StringCchCopyW(Text, Count, L"the network did not answer in time");
            return;
        case ERROR_BAD_UNIT:
            StringCchCopyW(Text, Count, L"the adapter is busy or gone");
            return;
        case ERROR_NOT_SUPPORTED:
        case ERROR_CALL_NOT_IMPLEMENTED:
            StringCchCopyW(Text, Count, L"the adapter does not support this");
            return;
        case RPC_S_SERVER_UNAVAILABLE:
            StringCchCopyW(Text, Count, L"the WLAN service is not running");
            return;
    }

    Length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                            NULL, Error, 0, Text, Count, NULL);
    while (Length && (Text[Length - 1] == L'\r' || Text[Length - 1] == L'\n' || Text[Length - 1] == L'.'))
        Text[--Length] = UNICODE_NULL;

    if (!Length)
        StringCchPrintfW(Text, Count, L"error %lu", Error);
}

/* Profile values are XML text, so the markup characters are written as entities */
static
VOID
StXmlEscape(
    _In_ LPCWSTR Text,
    _Out_writes_(Count) LPWSTR Escaped,
    _In_ DWORD Count)
{
    Escaped[0] = UNICODE_NULL;

    for (; *Text; Text++)
    {
        WCHAR One[2] = { *Text, UNICODE_NULL };
        LPCWSTR Part = One;

        switch (*Text)
        {
            case L'&': Part = L"&amp;"; break;
            case L'<': Part = L"&lt;"; break;
            case L'>': Part = L"&gt;"; break;
            case L'"': Part = L"&quot;"; break;
            case L'\'': Part = L"&apos;"; break;
        }

        if (FAILED(StringCchCatW(Escaped, Count, Part)))
            break;
    }
}

/* Wi-Fi worker, every wlanapi call runs here so the UI never waits on the service */

static
VOID
StFreeJob(
    _In_ PST_WIFI_JOB Job)
{
    if (Job->Interfaces)
        WlanFreeMemory(Job->Interfaces);
    if (Job->Networks)
        WlanFreeMemory(Job->Networks);
    SecureZeroMemory(Job->Passphrase, sizeof(Job->Passphrase));
    HeapFree(GetProcessHeap(), 0, Job);
}

static
DWORD
StWifiJoin(
    _In_ HANDLE Client,
    _In_ const GUID *Interface,
    _In_ const WLAN_AVAILABLE_NETWORK *Network,
    _In_ LPCWSTR Passphrase)
{
    static const WCHAR Format[] =
        L"<?xml version=\"1.0\"?>"
        L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
        L"<name>%s</name>"
        L"<SSIDConfig><SSID><name>%s</name></SSID></SSIDConfig>"
        L"<connectionType>ESS</connectionType>"
        L"<connectionMode>manual</connectionMode>"
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
        L"</WLANProfile>";
    WLAN_CONNECTION_PARAMETERS Parameters;
    DOT11_SSID Ssid = Network->dot11Ssid;
    WCHAR Name[DOT11_SSID_MAX_LENGTH + 1];
    WCHAR SafeName[DOT11_SSID_MAX_LENGTH * 6 + 1];
    WCHAR SafeKey[ST_PASSPHRASE_MAX * 6 + 1];
    LPWSTR Xml;
    DWORD XmlCount = 2048;
    DWORD Error;

    ZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.pDot11Ssid = &Ssid;
    Parameters.dot11BssType = dot11_BSS_type_infrastructure;

    if (!Network->bSecurityEnabled)
    {
        Parameters.wlanConnectionMode = wlan_connection_mode_discovery_unsecure;
        return WlanConnect(Client, Interface, &Parameters, NULL);
    }

    Xml = HeapAlloc(GetProcessHeap(), 0, XmlCount * sizeof(*Xml));
    if (!Xml)
        return ERROR_NOT_ENOUGH_MEMORY;

    StSsidText(&Network->dot11Ssid, Name, _countof(Name));
    StXmlEscape(Name, SafeName, _countof(SafeName));
    StXmlEscape(Passphrase, SafeKey, _countof(SafeKey));

    StringCchPrintfW(Xml, XmlCount, Format, SafeName, SafeName,
                     (Network->dot11DefaultAuthAlgorithm == DOT11_AUTH_ALGO_WPA_PSK) ? L"WPAPSK" : L"WPA2PSK",
                     (Network->dot11DefaultCipherAlgorithm == DOT11_CIPHER_ALGO_TKIP) ? L"TKIP" : L"AES",
                     SafeKey);

    Parameters.wlanConnectionMode = wlan_connection_mode_temporary_profile;
    Parameters.strProfile = Xml;
    Error = WlanConnect(Client, Interface, &Parameters, NULL);

    SecureZeroMemory(SafeKey, sizeof(SafeKey));
    SecureZeroMemory(Xml, XmlCount * sizeof(*Xml));
    HeapFree(GetProcessHeap(), 0, Xml);
    return Error;
}

static
VOID
StWifiReadState(
    _In_ HANDLE Client,
    _Inout_ PST_WIFI_JOB Job)
{
    PWLAN_CONNECTION_ATTRIBUTES Attributes = NULL;
    DWORD Size = 0;

    if (WlanGetAvailableNetworkList(Client, &Job->InterfaceGuid, 0, NULL, &Job->Networks) != ERROR_SUCCESS)
        Job->Networks = NULL;

    if (WlanQueryInterface(Client, &Job->InterfaceGuid, wlan_intf_opcode_current_connection,
                           NULL, &Size, (PVOID *)&Attributes, NULL) == ERROR_SUCCESS &&
        Attributes && Size >= sizeof(*Attributes))
    {
        Job->Connection = *Attributes;
        Job->Connected = (Attributes->isState == wlan_interface_state_connected);
    }

    if (Attributes)
        WlanFreeMemory(Attributes);
}

static
DWORD
WINAPI
StWifiWorker(
    _In_ LPVOID Parameter)
{
    PST_WIFI_JOB Job = Parameter;
    HANDLE Client = NULL;
    DWORD Version = 0;
    DWORD Index = 0;
    DWORD i;

    Job->OpenError = WlanOpenHandle(WLAN_API_VERSION_2_0, NULL, &Version, &Client);
    if (Job->OpenError != ERROR_SUCCESS)
    {
        Client = NULL;
        goto Done;
    }

    Job->Error = WlanEnumInterfaces(Client, NULL, &Job->Interfaces);
    if (Job->Error != ERROR_SUCCESS || !Job->Interfaces || !Job->Interfaces->dwNumberOfItems)
        goto Done;

    /* Stay on the adapter the page shows, or take the first one */
    if (Job->HaveInterface)
    {
        for (i = 0; i < Job->Interfaces->dwNumberOfItems; i++)
        {
            if (IsEqualGUID(&Job->Interfaces->InterfaceInfo[i].InterfaceGuid, &Job->InterfaceGuid))
            {
                Index = i;
                break;
            }
        }
    }
    Job->InterfaceGuid = Job->Interfaces->InterfaceInfo[Index].InterfaceGuid;
    Job->HaveInterface = TRUE;

    switch (Job->Operation)
    {
        case StWifiScan:
            Job->Error = WlanScan(Client, &Job->InterfaceGuid, NULL, NULL, NULL);
            break;

        case StWifiConnect:
            Job->Error = StWifiJoin(Client, &Job->InterfaceGuid, &Job->Network, Job->Passphrase);
            break;

        case StWifiDisconnect:
            Job->Error = WlanDisconnect(Client, &Job->InterfaceGuid, NULL);
            break;

        default:
            break;
    }
    SecureZeroMemory(Job->Passphrase, sizeof(Job->Passphrase));

    StWifiReadState(Client, Job);

Done:
    if (Client)
        WlanCloseHandle(Client, NULL);

    if (!PostMessageW(StWindow, WM_WIFI_DONE, 0, (LPARAM)Job))
        StFreeJob(Job);
    return 0;
}

static
BOOL
StWifiStart(
    _In_ ST_WIFI_OPERATION Operation,
    _In_ BOOL Quiet,
    _In_opt_ const WLAN_AVAILABLE_NETWORK *Network,
    _In_opt_ LPCWSTR Passphrase)
{
    PST_WIFI_JOB Job;

    if (StBusy)
    {
        if (!Quiet)
            StSetStatus(L"Wait for the current Wi-Fi request to finish");
        return FALSE;
    }

    Job = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Job));
    if (!Job)
        return FALSE;

    Job->Operation = Operation;
    Job->Quiet = Quiet;
    Job->HaveInterface = StHaveInterface;
    Job->InterfaceGuid = StInterfaceGuid;
    if (Network)
        Job->Network = *Network;
    if (Passphrase)
        StringCchCopyW(Job->Passphrase, _countof(Job->Passphrase), Passphrase);

    if (!StStartWorker(StWifiWorker, Job))
    {
        StFreeJob(Job);
        return FALSE;
    }

    StBusy = TRUE;
    return TRUE;
}

/* Wi-Fi page */

/* Connected first, then the strongest signal, hidden networks are left out */
static
VOID
StBuildOrder(VOID)
{
    DWORD i, j;

    HeapFree(GetProcessHeap(), 0, StOrder);
    StOrder = NULL;
    StOrderCount = 0;

    if (!StNetworks || !StNetworks->dwNumberOfItems)
        return;

    StOrder = HeapAlloc(GetProcessHeap(), 0, StNetworks->dwNumberOfItems * sizeof(*StOrder));
    if (!StOrder)
        return;

    for (i = 0; i < StNetworks->dwNumberOfItems; i++)
    {
        const WLAN_AVAILABLE_NETWORK *Network = &StNetworks->Network[i];
        DWORD Key = i;
        ULONG Rank;

        if (Network->dot11Ssid.uSSIDLength == 0)
            continue;

        Rank = Network->wlanSignalQuality + (StIsConnectedNetwork(Network) ? 1000 : 0);
        j = StOrderCount++;
        while (j > 0)
        {
            const WLAN_AVAILABLE_NETWORK *Prev = &StNetworks->Network[StOrder[j - 1]];
            ULONG PrevRank = Prev->wlanSignalQuality + (StIsConnectedNetwork(Prev) ? 1000 : 0);

            if (PrevRank >= Rank)
                break;
            StOrder[j] = StOrder[j - 1];
            j--;
        }
        StOrder[j] = Key;
    }
}

static
VOID
StFillList(VOID)
{
    HWND List = GetDlgItem(StWindow, IDC_WIFI_LIST);
    LRESULT Top = SendMessageW(List, LB_GETTOPINDEX, 0, 0);
    DWORD i;

    SendMessageW(List, WM_SETREDRAW, FALSE, 0);
    SendMessageW(List, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < StOrderCount; i++)
        SendMessageW(List, LB_ADDSTRING, 0, (LPARAM)StOrder[i]);
    if (Top != LB_ERR && (DWORD)Top < StOrderCount)
        SendMessageW(List, LB_SETTOPINDEX, Top, 0);
    SendMessageW(List, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(List, NULL, TRUE);
}

/* The join view asks for a password only for a secured network it can join */
static
BOOL
StJoinNeedsPassword(VOID)
{
    return StJoinNetwork.bSecurityEnabled && StCanJoin(&StJoinNetwork) &&
           !StIsConnectedNetwork(&StJoinNetwork);
}

/* Shows the controls that fit the current Wi-Fi state and view */
static
VOID
StWifiUpdateControls(VOID)
{
    BOOL Ready = (StWifiState == StWifiReady);
    BOOL Here = StJoining && StIsConnectedNetwork(&StJoinNetwork);
    UINT i;

    if (StPage != ST_PAGE_WIFI)
        return;

    if (StJoining)
    {
        StShowGroup(StWifiListControls, _countof(StWifiListControls), FALSE);
        for (i = 0; i < _countof(StWifiJoinControls); i++)
        {
            UINT Id = StWifiJoinControls[i];
            BOOL Show = TRUE;

            if (Id == IDC_WIFI_PASSWORD_LABEL || Id == IDC_WIFI_PASSWORD || Id == IDC_WIFI_SHOW_PASSWORD)
                Show = StJoinNeedsPassword();
            ShowWindow(GetDlgItem(StWindow, Id), Show ? SW_SHOW : SW_HIDE);
        }

        SetDlgItemTextW(StWindow, IDC_WIFI_CONNECT, Here ? L"Disconnect" : L"Connect");
        EnableWindow(GetDlgItem(StWindow, IDC_WIFI_CONNECT), Ready && (Here || StCanJoin(&StJoinNetwork)));
    }
    else
    {
        StShowGroup(StWifiJoinControls, _countof(StWifiJoinControls), FALSE);
        ShowWindow(GetDlgItem(StWindow, IDC_WIFI_REFRESH), SW_SHOW);
        ShowWindow(GetDlgItem(StWindow, IDC_WIFI_SCAN), Ready ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(StWindow, IDC_WIFI_DISCONNECT), Ready ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(StWindow, IDC_WIFI_LIST), Ready ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(StWindow, IDC_WIFI_ADAPTER),
                   (Ready && StInterfaces && StInterfaces->dwNumberOfItems > 1) ? SW_SHOW : SW_HIDE);
        EnableWindow(GetDlgItem(StWindow, IDC_WIFI_DISCONNECT), Ready && StConnected);
    }

    if (StInterfaces && StInterfaces->dwNumberOfItems > 1)
    {
        const WLAN_INTERFACE_INFO *Info = StCurrentInterface();
        WCHAR Text[300];

        StringCchPrintfW(Text, _countof(Text), L"Adapter: %s",
                         Info ? Info->strInterfaceDescription : L"none");
        SetDlgItemTextW(StWindow, IDC_WIFI_ADAPTER, Text);
    }
}

static
VOID
StWifiShowJoin(
    _In_ BOOL Join)
{
    StJoining = Join;
    StShowPassword = FALSE;
    SetDlgItemTextW(StWindow, IDC_WIFI_PASSWORD, L"");
    SendDlgItemMessageW(StWindow, IDC_WIFI_PASSWORD, EM_SETPASSWORDCHAR, L'*', 0);

    StWifiUpdateControls();
    StRelayout();
    InvalidateRect(StWindow, NULL, FALSE);

    if (Join && StJoinNeedsPassword())
    {
        SetFocus(GetDlgItem(StWindow, IDC_WIFI_PASSWORD));
    }
    else if (!Join)
    {
        SendDlgItemMessageW(StWindow, IDC_WIFI_LIST, LB_SETCURSEL, (WPARAM)-1, 0);
        SetFocus(GetDlgItem(StWindow, IDC_WIFI_LIST));
    }
}

static
VOID
StWifiDone(
    _In_ PST_WIFI_JOB Job)
{
    WCHAR Name[DOT11_SSID_MAX_LENGTH + 1];
    WCHAR Error[128];
    DWORD i;

    StBusy = FALSE;

    if (StInterfaces)
        WlanFreeMemory(StInterfaces);
    if (StNetworks)
        WlanFreeMemory(StNetworks);
    StInterfaces = Job->Interfaces;
    StNetworks = Job->Networks;
    Job->Interfaces = NULL;
    Job->Networks = NULL;

    StConnected = Job->Connected;
    StConnection = Job->Connection;

    if (Job->OpenError != ERROR_SUCCESS)
    {
        StWifiState = StWifiNoService;
        StWifiServiceError = Job->OpenError;
        StHaveInterface = FALSE;
    }
    else if (!StInterfaces || !StInterfaces->dwNumberOfItems)
    {
        StWifiState = StWifiNoAdapter;
        StHaveInterface = FALSE;
    }
    else
    {
        StWifiState = StWifiReady;
        StInterfaceGuid = Job->InterfaceGuid;
        StHaveInterface = Job->HaveInterface;
    }

    /* A service without the connection query still flags the network it is on */
    if (!StConnected && StNetworks)
    {
        for (i = 0; i < StNetworks->dwNumberOfItems; i++)
        {
            if (StNetworks->Network[i].dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED)
            {
                StConnected = TRUE;
                StConnection.isState = wlan_interface_state_connected;
                StConnection.wlanAssociationAttributes.dot11Ssid = StNetworks->Network[i].dot11Ssid;
                StConnection.wlanAssociationAttributes.wlanSignalQuality = StNetworks->Network[i].wlanSignalQuality;
                break;
            }
        }
    }

    if (StWifiState != StWifiReady)
    {
        StAutoScanned = FALSE;
        StJoining = FALSE;
    }

    StBuildOrder();
    StFillList();

    StSsidText(&Job->Network.dot11Ssid, Name, _countof(Name));
    StErrorText(Job->Error, Error, _countof(Error));

    if (StWifiState == StWifiNoService)
    {
        if (!Job->Quiet)
            StSetStatus(L"The WLAN service could not be reached");
    }
    else if (StWifiState == StWifiNoAdapter)
    {
        if (!Job->Quiet)
            StSetStatus(L"No Wi-Fi adapter found");
    }
    else
    {
        switch (Job->Operation)
        {
            case StWifiScan:
                if (Job->Error != ERROR_SUCCESS)
                    StSetStatus(L"Scan failed: %s", Error);
                else
                    StSetStatus(L"Found %lu network(s)", StOrderCount);
                break;

            case StWifiConnect:
                if (Job->Error == ERROR_SUCCESS)
                {
                    StSetStatus(L"Connected to %s", Name);
                    StWifiShowJoin(FALSE);
                }
                else if (Job->Network.bSecurityEnabled)
                {
                    StSetStatus(L"Could not join %s: %s. Check the password.", Name, Error);
                }
                else
                {
                    StSetStatus(L"Could not join %s: %s", Name, Error);
                }
                break;

            case StWifiDisconnect:
                if (Job->Error != ERROR_SUCCESS)
                {
                    StSetStatus(L"Disconnect failed: %s", Error);
                }
                else
                {
                    StSetStatus(L"Disconnected");
                    if (StJoining)
                        StWifiShowJoin(FALSE);
                }
                break;

            default:
                if (!Job->Quiet)
                    StSetStatus(L"%lu network(s) in range", StOrderCount);
                break;
        }
    }

    StFreeJob(Job);

    StWifiUpdateControls();
    StRelayout();
    InvalidateRect(StWindow, NULL, FALSE);

    /* The first time an adapter shows up, look for networks right away */
    if (StWifiState == StWifiReady && !StAutoScanned)
    {
        StAutoScanned = TRUE;
        if (StWifiStart(StWifiScan, TRUE, NULL, NULL))
            StSetStatus(L"Scanning for networks");
    }
}

static
VOID
StWifiCommand(
    _In_ UINT Id)
{
    WCHAR Passphrase[ST_PASSPHRASE_MAX + 2];
    WCHAR Name[DOT11_SSID_MAX_LENGTH + 1];
    size_t Length = 0;
    DWORD i;

    switch (Id)
    {
        case IDC_WIFI_SCAN:
            if (StWifiStart(StWifiScan, FALSE, NULL, NULL))
                StSetStatus(L"Scanning for networks");
            break;

        case IDC_WIFI_REFRESH:
            if (StWifiStart(StWifiRefresh, FALSE, NULL, NULL))
                StSetStatus(L"Checking Wi-Fi");
            break;

        case IDC_WIFI_DISCONNECT:
            if (StWifiStart(StWifiDisconnect, FALSE, NULL, NULL))
                StSetStatus(L"Disconnecting");
            break;

        case IDC_WIFI_ADAPTER:
            if (!StInterfaces || StInterfaces->dwNumberOfItems < 2)
                break;

            if (StBusy)
            {
                StSetStatus(L"Wait for the current Wi-Fi request to finish");
                break;
            }

            /* Step to the next adapter and read it */
            for (i = 0; i < StInterfaces->dwNumberOfItems; i++)
            {
                if (IsEqualGUID(&StInterfaces->InterfaceInfo[i].InterfaceGuid, &StInterfaceGuid))
                    break;
            }
            i = (i + 1) % StInterfaces->dwNumberOfItems;
            StInterfaceGuid = StInterfaces->InterfaceInfo[i].InterfaceGuid;
            StHaveInterface = TRUE;
            StWifiUpdateControls();
            if (StWifiStart(StWifiScan, FALSE, NULL, NULL))
                StSetStatus(L"Scanning for networks");
            break;

        case IDC_WIFI_SHOW_PASSWORD:
            StShowPassword = !StShowPassword;
            SendDlgItemMessageW(StWindow, IDC_WIFI_PASSWORD, EM_SETPASSWORDCHAR, StShowPassword ? 0 : L'*', 0);
            InvalidateRect(GetDlgItem(StWindow, IDC_WIFI_PASSWORD), NULL, TRUE);
            InvalidateRect(GetDlgItem(StWindow, IDC_WIFI_SHOW_PASSWORD), NULL, FALSE);
            break;

        case IDC_WIFI_BACK:
            StWifiShowJoin(FALSE);
            break;

        case IDOK:
        case IDC_WIFI_CONNECT:
            if (!StJoining)
                break;

            StSsidText(&StJoinNetwork.dot11Ssid, Name, _countof(Name));

            if (StIsConnectedNetwork(&StJoinNetwork))
            {
                if (StWifiStart(StWifiDisconnect, FALSE, NULL, NULL))
                    StSetStatus(L"Disconnecting from %s", Name);
                break;
            }

            if (!StCanJoin(&StJoinNetwork))
            {
                StSetStatus(L"%s networks are not supported yet", StSecurityText(&StJoinNetwork));
                break;
            }

            Passphrase[0] = UNICODE_NULL;
            if (StJoinNetwork.bSecurityEnabled)
            {
                GetDlgItemTextW(StWindow, IDC_WIFI_PASSWORD, Passphrase, _countof(Passphrase));
                StringCchLengthW(Passphrase, _countof(Passphrase), &Length);
                if (Length < ST_PASSPHRASE_MIN || Length > ST_PASSPHRASE_MAX)
                {
                    SecureZeroMemory(Passphrase, sizeof(Passphrase));
                    StSetStatus(L"The password must be %u to %u characters", ST_PASSPHRASE_MIN, ST_PASSPHRASE_MAX);
                    SetFocus(GetDlgItem(StWindow, IDC_WIFI_PASSWORD));
                    break;
                }
            }

            if (StWifiStart(StWifiConnect, FALSE, &StJoinNetwork, Passphrase))
                StSetStatus(L"Connecting to %s", Name);
            SecureZeroMemory(Passphrase, sizeof(Passphrase));
            break;
    }
}

static
VOID
StWifiPick(VOID)
{
    LRESULT Index = SendDlgItemMessageW(StWindow, IDC_WIFI_LIST, LB_GETCURSEL, 0, 0);

    if (Index == LB_ERR || (DWORD)Index >= StOrderCount || !StNetworks)
        return;

    StJoinNetwork = StNetworks->Network[StOrder[Index]];
    StWifiShowJoin(TRUE);

    if (!StCanJoin(&StJoinNetwork))
        StSetStatus(L"%s networks are not supported yet", StSecurityText(&StJoinNetwork));
    else
        StSetStatus(L"");
}

static
VOID
StWifiShow(VOID)
{
    StWifiUpdateControls();
    StWifiStart(StWifiRefresh, TRUE, NULL, NULL);
}

static
VOID
StWifiHeading(
    _Out_writes_(Count) LPWSTR Text,
    _In_ DWORD Count)
{
    if (StJoining)
        StSsidText(&StJoinNetwork.dot11Ssid, Text, Count);
    else
        StringCchCopyW(Text, Count, L"Wi-Fi");
}

static
VOID
StWifiLayout(
    _In_ const RECT *Area)
{
    int Width = Area->right - Area->left;
    int Third = (Width - 2 * ST_GAP) / 3;
    int Left = Area->left;
    int Y = Area->top + ST_CARD_H + ST_GAP;

    if (StJoining)
    {
        StMove(IDC_WIFI_PASSWORD_LABEL, Left, Y, Width, ST_PILL_H);
        StMove(IDC_WIFI_PASSWORD, Left, Y + ST_PILL_H, Width, ST_ROW);
        StMove(IDC_WIFI_SHOW_PASSWORD, Left, Y + ST_PILL_H + ST_ROW + ST_GAP, Width, ST_ROW);
        if (StJoinNeedsPassword())
            Y += ST_PILL_H + 2 * ST_ROW + 2 * ST_GAP;

        StMove(IDC_WIFI_CONNECT, Left, Y, Width - Third - ST_GAP, ST_ROW);
        StMove(IDC_WIFI_BACK, Left + Width - Third, Y, Third, ST_ROW);
        return;
    }

    if (StWifiState == StWifiReady && StInterfaces && StInterfaces->dwNumberOfItems > 1)
    {
        StMove(IDC_WIFI_ADAPTER, Left, Y, Width, ST_ROW);
        Y += ST_ROW + ST_GAP;
    }

    if (StWifiState == StWifiReady)
    {
        StMove(IDC_WIFI_SCAN, Left, Y, Third, ST_ROW);
        StMove(IDC_WIFI_REFRESH, Left + Third + ST_GAP, Y, Third, ST_ROW);
        StMove(IDC_WIFI_DISCONNECT, Left + 2 * (Third + ST_GAP), Y, Third, ST_ROW);
    }
    else
    {
        StMove(IDC_WIFI_REFRESH, Left, Y, Width, ST_ROW);
    }
    Y += ST_ROW + ST_GAP;

    StMove(IDC_WIFI_LIST, Left, Y, Width, max(Area->bottom - Y, ST_ROW));
}

/* The card at the top of the page tells what Wi-Fi is doing */
static
VOID
StWifiPaint(
    _In_ HDC Dc,
    _In_ const RECT *Area)
{
    const WLAN_INTERFACE_INFO *Info = StCurrentInterface();
    WCHAR Title[128];
    WCHAR Detail[400];
    WCHAR Name[DOT11_SSID_MAX_LENGTH + 1];
    RECT Card;

    Card = *Area;
    Card.bottom = Card.top + ST_CARD_H;

    if (StJoining)
    {
        BOOL Here = StIsConnectedNetwork(&StJoinNetwork);

        StringCchCopyW(Title, _countof(Title), Here ? L"Connected" : StSecurityText(&StJoinNetwork));
        if (!StCanJoin(&StJoinNetwork) && !Here)
            StringCchPrintfW(Detail, _countof(Detail), L"Signal %lu%%. This kind of network is not supported yet.",
                             StJoinNetwork.wlanSignalQuality);
        else if (Here)
            StringCchPrintfW(Detail, _countof(Detail), L"%s, signal %lu%%",
                             StSecurityText(&StJoinNetwork), StJoinNetwork.wlanSignalQuality);
        else if (StJoinNetwork.bSecurityEnabled)
            StringCchPrintfW(Detail, _countof(Detail), L"Signal %lu%%. Enter the network password to join.",
                             StJoinNetwork.wlanSignalQuality);
        else
            StringCchPrintfW(Detail, _countof(Detail), L"Signal %lu%%. Anyone nearby can see traffic on an open network.",
                             StJoinNetwork.wlanSignalQuality);
    }
    else
    {
        switch (StWifiState)
        {
            case StWifiChecking:
                StringCchCopyW(Title, _countof(Title), L"Checking Wi-Fi");
                StringCchCopyW(Detail, _countof(Detail), L"Looking for a wireless adapter");
                break;

            case StWifiNoService:
                StringCchCopyW(Title, _countof(Title), L"Wi-Fi is unavailable");
                StringCchPrintfW(Detail, _countof(Detail), L"The WLAN service is not running (error %lu). Tap Refresh to try again.",
                                 StWifiServiceError);
                break;

            case StWifiNoAdapter:
                StringCchCopyW(Title, _countof(Title), L"No Wi-Fi adapter");
                StringCchCopyW(Detail, _countof(Detail),
                               L"No wireless adapter was found. It shows up here once its driver has loaded.");
                break;

            default:
                if (StConnected)
                {
                    const WLAN_ASSOCIATION_ATTRIBUTES *Association = &StConnection.wlanAssociationAttributes;

                    StSsidText(&Association->dot11Ssid, Name, _countof(Name));
                    StringCchPrintfW(Title, _countof(Title), L"Connected to %s", Name[0] ? Name : L"a network");
                    StringCchPrintfW(Detail, _countof(Detail), L"Signal %lu%%, %s",
                                     Association->wlanSignalQuality,
                                     Info ? Info->strInterfaceDescription : L"Wi-Fi");
                }
                else
                {
                    StringCchCopyW(Title, _countof(Title), L"Not connected");
                    StringCchPrintfW(Detail, _countof(Detail), L"%s. Pick a network to join.",
                                     Info ? Info->strInterfaceDescription : L"Wi-Fi");
                }
                break;
        }
    }

    StDrawCard(Dc, &Card, Title, Detail,
               StJoining ? StIsConnectedNetwork(&StJoinNetwork) : (StConnected && StWifiState == StWifiReady));
}

/* Four bars, lit by signal quality */
static
VOID
StDrawSignal(
    _In_ HDC Dc,
    _In_ const RECT *Box,
    _In_ ULONG Quality,
    _In_ COLORREF On)
{
    int Bars = (Quality > 75) ? 4 : (Quality > 50) ? 3 : (Quality > 25) ? 2 : (Quality > 0) ? 1 : 0;
    int BarWidth = (Box->right - Box->left - 3 * 6) / 4;
    int Height = Box->bottom - Box->top;
    int i;

    for (i = 0; i < 4; i++)
    {
        RECT Bar;

        Bar.left = Box->left + i * (BarWidth + 6);
        Bar.right = Bar.left + BarWidth;
        Bar.bottom = Box->bottom;
        Bar.top = Box->bottom - Height * (i + 1) / 4;
        StFillRound(Dc, &Bar, 6, (i < Bars) ? On : ST_DIM);
    }
}

static
VOID
StDrawNetwork(
    _In_ const DRAWITEMSTRUCT *Item)
{
    const WLAN_AVAILABLE_NETWORK *Network;
    WCHAR Name[DOT11_SSID_MAX_LENGTH + 1];
    WCHAR Line[128];
    RECT Card = Item->rcItem, Text, Bars;
    BOOL Selected = (Item->itemState & ODS_SELECTED) != 0;
    BOOL Here;

    FillRect(Item->hDC, &Item->rcItem, StBgBrush);
    if (Item->itemID == (UINT)-1 || !StNetworks || (DWORD)Item->itemData >= StNetworks->dwNumberOfItems)
        return;

    Network = &StNetworks->Network[Item->itemData];
    Here = StIsConnectedNetwork(Network);

    InflateRect(&Card, 0, -ST_GAP / 2);
    StFillRound(Item->hDC, &Card, ST_RADIUS, Selected ? ST_ACCENT : ST_TILE);
    SetBkMode(Item->hDC, TRANSPARENT);

    Bars.right = Card.right - ST_CARD_PAD;
    Bars.left = Bars.right - 72;
    Bars.top = Card.top + (Card.bottom - Card.top - 52) / 2;
    Bars.bottom = Bars.top + 52;
    StDrawSignal(Item->hDC, &Bars, Network->wlanSignalQuality, Selected ? ST_TEXT : ST_ACCENT);

    Text = Card;
    Text.left += ST_CARD_PAD;
    Text.right = Bars.left - ST_GAP;
    Text.top += ST_CARD_PAD / 2;
    Text.bottom = Text.top + (Card.bottom - Card.top - ST_CARD_PAD) / 2 + 8;

    StSsidText(&Network->dot11Ssid, Name, _countof(Name));
    SelectObject(Item->hDC, StBigFont);
    SetTextColor(Item->hDC, ST_TEXT);
    DrawTextW(Item->hDC, Name, -1, &Text, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (Here)
        StringCchPrintfW(Line, _countof(Line), L"Connected, %s", StSecurityText(Network));
    else
        StringCchPrintfW(Line, _countof(Line), L"%s, %lu%%", StSecurityText(Network), Network->wlanSignalQuality);

    Text.top = Text.bottom;
    Text.bottom = Card.bottom - ST_CARD_PAD / 2;
    SelectObject(Item->hDC, StSmallFont);
    SetTextColor(Item->hDC, Selected ? ST_TEXT : (Here ? ST_ACCENT : ST_SUBTEXT));
    DrawTextW(Item->hDC, Line, -1, &Text, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

/* About page */

static
VOID
StReadString(
    _In_ HKEY Root,
    _In_ LPCWSTR Path,
    _In_ LPCWSTR Name,
    _Out_writes_(Count) LPWSTR Value,
    _In_ DWORD Count)
{
    HKEY Key;
    DWORD Type;
    DWORD Size = (Count - 1) * sizeof(*Value);

    Value[0] = UNICODE_NULL;
    if (RegOpenKeyExW(Root, Path, 0, KEY_QUERY_VALUE, &Key) != ERROR_SUCCESS)
        return;

    if (RegQueryValueExW(Key, Name, NULL, &Type, (LPBYTE)Value, &Size) != ERROR_SUCCESS ||
        (Type != REG_SZ && Type != REG_EXPAND_SZ))
    {
        Size = 0;
    }
    Value[Size / sizeof(*Value)] = UNICODE_NULL;

    /* Firmware strings often come padded */
    for (Size = (DWORD)wcslen(Value); Size && Value[Size - 1] == L' '; Size--)
        Value[Size - 1] = UNICODE_NULL;

    RegCloseKey(Key);
}

/* The kernel image carries the ReactOS release in its version resource */
static
VOID
StReadOsRelease(
    _Out_writes_(Count) LPWSTR Value,
    _In_ DWORD Count)
{
    WCHAR Path[MAX_PATH];
    LPWSTR Release = NULL;
    PVOID Data;
    DWORD Size;
    UINT Length = 0;

    Value[0] = UNICODE_NULL;
    if (!GetSystemDirectoryW(Path, _countof(Path)) ||
        FAILED(StringCchCatW(Path, _countof(Path), L"\\ntoskrnl.exe")))
    {
        return;
    }

    Size = GetFileVersionInfoSizeW(Path, NULL);
    if (!Size)
        return;

    Data = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Data)
        return;

    if (GetFileVersionInfoW(Path, 0, Size, Data) &&
        VerQueryValueW(Data, L"\\StringFileInfo\\040904b0\\ProductVersion", (PVOID *)&Release, &Length) &&
        Release && Length)
    {
        StringCchCopyW(Value, Count, Release);
    }

    HeapFree(GetProcessHeap(), 0, Data);
}

static
VOID
StAboutAdd(
    _In_ LPCWSTR Label,
    _In_ LPCWSTR Format,
    ...)
{
    WCHAR Value[160];
    va_list Args;

    if (StAboutCount >= _countof(StAboutRows))
        return;

    va_start(Args, Format);
    StringCchVPrintfW(Value, _countof(Value), Format, Args);
    va_end(Args);

    StSetInfoRow(&StAboutRows[StAboutCount++], Label, L"%s", Value);
}

static
VOID
StAboutShow(VOID)
{
    static const WCHAR NtKey[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    static const WCHAR BiosKey[] = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    static const WCHAR CpuKey[] = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    WCHAR First[128];
    WCHAR Second[128];
    OSVERSIONINFOW Version;
    SYSTEM_INFO System;
    MEMORYSTATUSEX Memory;
    DWORD Size;
    DWORD Minutes;

    StAboutCount = 0;

    StReadString(HKEY_LOCAL_MACHINE, NtKey, L"ProductName", First, _countof(First));
    StReadOsRelease(Second, _countof(Second));
    StAboutAdd(L"Software", L"%s %s", First[0] ? First : L"ReactOS", Second);

    ZeroMemory(&Version, sizeof(Version));
    Version.dwOSVersionInfoSize = sizeof(Version);
    if (GetVersionExW(&Version))
    {
        StAboutAdd(L"Reported version", L"NT %lu.%lu build %lu%s%s",
                   Version.dwMajorVersion, Version.dwMinorVersion, Version.dwBuildNumber,
                   Version.szCSDVersion[0] ? L", " : L"", Version.szCSDVersion);
    }

    StReadString(HKEY_LOCAL_MACHINE, BiosKey, L"SystemManufacturer", First, _countof(First));
    StReadString(HKEY_LOCAL_MACHINE, BiosKey, L"SystemProductName", Second, _countof(Second));
    StAboutAdd(L"Device", L"%s%s%s", First, (First[0] && Second[0]) ? L" " : L"", Second);

    StReadString(HKEY_LOCAL_MACHINE, BiosKey, L"BIOSVendor", First, _countof(First));
    StReadString(HKEY_LOCAL_MACHINE, BiosKey, L"BIOSVersion", Second, _countof(Second));
    StAboutAdd(L"Firmware", L"%s%s%s", First, (First[0] && Second[0]) ? L" " : L"", Second);

    GetNativeSystemInfo(&System);
    StReadString(HKEY_LOCAL_MACHINE, CpuKey, L"ProcessorNameString", First, _countof(First));
    if (!First[0])
        StReadString(HKEY_LOCAL_MACHINE, CpuKey, L"Identifier", First, _countof(First));
    if (!First[0] && System.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64)
        StringCchCopyW(First, _countof(First), L"ARM64");
    StAboutAdd(L"Processor", L"%s, %lu core(s)", First[0] ? First : L"Unknown", System.dwNumberOfProcessors);

    ZeroMemory(&Memory, sizeof(Memory));
    Memory.dwLength = sizeof(Memory);
    if (GlobalMemoryStatusEx(&Memory))
    {
        StAboutAdd(L"Memory", L"%I64u MB, %I64u MB free",
                   Memory.ullTotalPhys / (1024 * 1024), Memory.ullAvailPhys / (1024 * 1024));
    }

    StAboutAdd(L"Display", L"%d x %d", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));

    Size = _countof(First);
    if (!GetComputerNameW(First, &Size))
        First[0] = UNICODE_NULL;
    StAboutAdd(L"Device name", L"%s", First);

    Minutes = GetTickCount() / 60000;
    StAboutAdd(L"Up time", L"%lu h %02lu min", Minutes / 60, Minutes % 60);

    InvalidateRect(StWindow, NULL, FALSE);
}

static
VOID
StAboutHeading(
    _Out_writes_(Count) LPWSTR Text,
    _In_ DWORD Count)
{
    StringCchCopyW(Text, Count, L"About this phone");
}

static
VOID
StAboutLayout(
    _In_ const RECT *Area)
{
    UNREFERENCED_PARAMETER(Area);
}

static
VOID
StAboutPaint(
    _In_ HDC Dc,
    _In_ const RECT *Area)
{
    StPaintInfoRows(Dc, Area, StAboutRows, StAboutCount);
}

static
VOID
StWifiPageCommand(
    _In_ UINT Id,
    _In_ UINT Code)
{
    if (Id == IDC_WIFI_LIST)
    {
        if (Code == LBN_SELCHANGE)
            StWifiPick();
    }
    else if (Code == BN_CLICKED || Id == IDOK)
    {
        StWifiCommand(Id);
    }
}

/* Keeps the list and status current, and notices an adapter that appears */
static
VOID
StWifiTimer(VOID)
{
    if (!StJoining && !StBusy)
        StWifiStart(StWifiRefresh, TRUE, NULL, NULL);
}

static
BOOL
StWifiIsActive(
    _In_ UINT Id)
{
    if (Id == IDC_WIFI_SHOW_PASSWORD)
        return StShowPassword;
    if (Id == IDC_WIFI_CONNECT)
        return IsWindowEnabled(GetDlgItem(StWindow, Id));
    return FALSE;
}

/* Pages in tab order, a new page needs only an entry here */
static const ST_PAGE StPages[] =
{
    { L"Wi-Fi", StWifiControls, &StWifiControlCount, StWifiShow, StWifiLayout, StWifiPaint, StWifiHeading,
      StWifiPageCommand, StWifiTimer, StWifiIsActive },
    { L"Network", StNetControls, &StNetControlCount, StNetShow, StNetLayout, StNetPaint, StNetHeading,
      StNetCommand, StNetTimer, StNetIsActive },
    { L"Sound", StSndControls, &StSndControlCount, StSndShow, StSndLayout, StSndPaint, StSndHeading,
      StSndCommand, StSndTimer, StSndIsActive },
    { L"Power", StPwrControls, &StPwrControlCount, StPwrShow, StPwrLayout, StPwrPaint, StPwrHeading,
      StPwrCommand, StPwrTimer, StPwrIsActive },
    { L"About", NULL, 0, StAboutShow, StAboutLayout, StAboutPaint, StAboutHeading,
      NULL, NULL, NULL },
};

/* Window */

static
VOID
StContentRect(
    _Out_ RECT *Area)
{
    RECT Client;

    GetClientRect(StWindow, &Client);
    Area->left = ST_MARGIN;
    Area->right = Client.right - ST_MARGIN;
    Area->top = ST_SAFE_TOP + ST_TAB_H + ST_GAP + ST_HEAD_H + ST_GAP;
    /* The status line and the exit button own the bottom of every page */
    Area->bottom = Client.bottom - ST_MARGIN - ST_ROW - ST_GAP - ST_STATUS_H - ST_GAP;
}

VOID
StRelayout(VOID)
{
    RECT Client, Area;
    int Width, Segment;
    DWORD i;

    GetClientRect(StWindow, &Client);
    Width = Client.right - 2 * ST_MARGIN;
    if (Width < 200)
        return;

    Segment = (Width - ((int)_countof(StPages) - 1) * ST_GAP) / (int)_countof(StPages);
    for (i = 0; i < _countof(StPages); i++)
        StMove(IDC_TAB_FIRST + i, ST_MARGIN + i * (Segment + ST_GAP), ST_SAFE_TOP, Segment, ST_TAB_H);

    StMove(IDC_EXIT, ST_MARGIN, Client.bottom - ST_MARGIN - ST_ROW, Width, ST_ROW);
    StMove(IDC_STATUS, ST_MARGIN, Client.bottom - ST_MARGIN - ST_ROW - ST_GAP - ST_STATUS_H, Width, ST_STATUS_H);

    StContentRect(&Area);
    if (Area.bottom - Area.top < ST_ROW)
        return;

    StPages[StPage].Layout(&Area);
}

static
VOID
StSelectPage(
    _In_ DWORD Page)
{
    DWORD i;

    StPage = Page;

    for (i = 0; i < _countof(StPages); i++)
    {
        if (i != Page && StPages[i].Controls)
            StShowGroup(StPages[i].Controls, *StPages[i].ControlCount, FALSE);
    }

    /* The tabs are child windows, the parent repaint does not reach them */
    for (i = 0; i < _countof(StPages); i++)
        InvalidateRect(GetDlgItem(StWindow, IDC_TAB_FIRST + i), NULL, FALSE);

    StSetStatus(L"");
    StPages[Page].Show();
    StRelayout();
    InvalidateRect(StWindow, NULL, FALSE);
}

static
VOID
StPaint(VOID)
{
    PAINTSTRUCT Paint;
    RECT Client, Head, Area;
    WCHAR Heading[128];
    HDC Dc = BeginPaint(StWindow, &Paint);

    GetClientRect(StWindow, &Client);
    FillRect(Dc, &Client, StBgBrush);

    Head.left = ST_MARGIN;
    Head.right = Client.right - ST_MARGIN;
    Head.top = ST_SAFE_TOP + ST_TAB_H + ST_GAP;
    Head.bottom = Head.top + ST_HEAD_H;

    StPages[StPage].Heading(Heading, _countof(Heading));
    SetBkMode(Dc, TRANSPARENT);
    SetTextColor(Dc, ST_TEXT);
    SelectObject(Dc, StHeadFont);
    DrawTextW(Dc, Heading, -1, &Head, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    StContentRect(&Area);
    if (Area.bottom > Area.top)
        StPages[StPage].Paint(Dc, &Area);

    EndPaint(StWindow, &Paint);
}

static
VOID
StDrawButton(
    _In_ const DRAWITEMSTRUCT *Item)
{
    WCHAR Text[300];
    RECT Rect = Item->rcItem;
    UINT Id = Item->CtlID;
    BOOL Tab = (Id >= IDC_TAB_FIRST && Id < IDC_TAB_FIRST + _countof(StPages));
    BOOL Pressed = (Item->itemState & ODS_SELECTED) != 0;
    BOOL Disabled = (Item->itemState & ODS_DISABLED) != 0;
    BOOL Active = FALSE;
    COLORREF Fill, Ink;

    if (Tab)
        Active = (StPage == Id - IDC_TAB_FIRST);
    else if (StPages[StPage].IsActive)
        Active = StPages[StPage].IsActive(Id);

    if (Disabled)
    {
        Fill = ST_FIELD;
        Ink = ST_DIM;
    }
    else if (Active || Pressed)
    {
        Fill = ST_ACCENT;
        Ink = ST_TEXT;
    }
    else
    {
        Fill = ST_TILE;
        Ink = Tab ? ST_SUBTEXT : ST_TEXT;
    }

    FillRect(Item->hDC, &Rect, StBgBrush);
    StFillRound(Item->hDC, &Rect, (Rect.bottom - Rect.top) / 3, Fill);

    GetWindowTextW(Item->hwndItem, Text, _countof(Text));
    InflateRect(&Rect, -ST_GAP, 0);
    SetBkMode(Item->hDC, TRANSPARENT);
    SetTextColor(Item->hDC, Ink);
    SelectObject(Item->hDC, StButtonFont);
    DrawTextW(Item->hDC, Text, -1, &Rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static
HFONT
StMakeFont(
    _In_ int Height,
    _In_ int Weight)
{
    return CreateFontW(Height, 0, 0, 0, Weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
}

HWND
StControl(
    _In_ LPCWSTR Class,
    _In_opt_ LPCWSTR Text,
    _In_ DWORD Style,
    _In_ UINT Id,
    _In_ HFONT Font)
{
    HWND Control;

    Control = CreateWindowExW(0, Class, Text, WS_CHILD | Style, 0, 0, 10, 10,
                              StWindow, (HMENU)(ULONG_PTR)Id, StInstance, NULL);
    if (Control)
        SendMessageW(Control, WM_SETFONT, (WPARAM)Font, FALSE);
    return Control;
}

HWND
StButton(
    _In_ LPCWSTR Text,
    _In_ DWORD Style,
    _In_ UINT Id)
{
    return StControl(L"BUTTON", Text, Style | BS_OWNERDRAW, Id, StButtonFont);
}

static
VOID
StCreateControls(VOID)
{
    DWORD i;

    for (i = 0; i < _countof(StPages); i++)
        StButton(StPages[i].Tab, WS_VISIBLE, IDC_TAB_FIRST + i);

    /* Wi-Fi network list */
    StButton(L"Adapter", WS_TABSTOP, IDC_WIFI_ADAPTER);
    StButton(L"Scan", WS_TABSTOP, IDC_WIFI_SCAN);
    StButton(L"Refresh", WS_TABSTOP, IDC_WIFI_REFRESH);
    StButton(L"Disconnect", WS_TABSTOP, IDC_WIFI_DISCONNECT);
    StControl(L"LISTBOX", NULL, WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | LBS_OWNERDRAWFIXED,
              IDC_WIFI_LIST, StFont);

    /* Wi-Fi join view */
    StControl(L"STATIC", L"Password", SS_CENTERIMAGE, IDC_WIFI_PASSWORD_LABEL, StFont);
    StControl(L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, IDC_WIFI_PASSWORD, StFont);
    StButton(L"Show password", WS_TABSTOP, IDC_WIFI_SHOW_PASSWORD);
    StButton(L"Connect", WS_TABSTOP, IDC_WIFI_CONNECT);
    StButton(L"Back", WS_TABSTOP, IDC_WIFI_BACK);

    StNetCreateControls();
    StSndCreateControls();
    StPwrCreateControls();

    StControl(L"STATIC", L"", WS_VISIBLE | SS_CENTERIMAGE | SS_ENDELLIPSIS, IDC_STATUS, StSmallFont);
    StButton(L"Exit", WS_VISIBLE | WS_TABSTOP, IDC_EXIT);

    SendDlgItemMessageW(StWindow, IDC_WIFI_LIST, LB_SETITEMHEIGHT, 0, ST_ITEM_H);
    SendDlgItemMessageW(StWindow, IDC_WIFI_PASSWORD, EM_LIMITTEXT, ST_PASSPHRASE_MAX, 0);
}

static
LRESULT
CALLBACK
StWndProc(
    _In_ HWND Window,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    switch (Message)
    {
        case WM_CREATE:
            StWindow = Window;
            StCreateControls();
            StSelectPage(ST_PAGE_WIFI);
            SetTimer(Window, ST_REFRESH_TIMER, ST_REFRESH_MS, NULL);
            return 0;

        case WM_SIZE:
            StRelayout();
            InvalidateRect(Window, NULL, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            StPaint();
            return 0;

        case WM_MEASUREITEM:
            ((MEASUREITEMSTRUCT *)lParam)->itemHeight = ST_ITEM_H;
            return TRUE;

        case WM_DRAWITEM:
        {
            const DRAWITEMSTRUCT *Item = (const DRAWITEMSTRUCT *)lParam;

            if (Item->CtlID == IDC_WIFI_LIST)
                StDrawNetwork(Item);
            else if (Item->CtlID == IDC_NET_LIST)
                StNetDrawItem(Item);
            else
                StDrawButton(Item);
            return TRUE;
        }

        case WM_CTLCOLOREDIT:
            SetTextColor((HDC)wParam, ST_TEXT);
            SetBkColor((HDC)wParam, ST_FIELD);
            return (LRESULT)StFieldBrush;

        case WM_CTLCOLORLISTBOX:
            SetTextColor((HDC)wParam, ST_TEXT);
            SetBkColor((HDC)wParam, ST_BG);
            return (LRESULT)StBgBrush;

        case WM_CTLCOLORSTATIC:
            SetTextColor((HDC)wParam, ST_SUBTEXT);
            SetBkColor((HDC)wParam, ST_BG);
            return (LRESULT)StBgBrush;

        case WM_TIMER:
            if (wParam == ST_REFRESH_TIMER && StPages[StPage].Timer)
                StPages[StPage].Timer();
            return 0;

        case WM_COMMAND:
        {
            UINT Id = LOWORD(wParam);

            if (Id == IDC_EXIT)
                DestroyWindow(Window);
            else if (Id >= IDC_TAB_FIRST && Id < IDC_TAB_FIRST + _countof(StPages))
                StSelectPage(Id - IDC_TAB_FIRST);
            else if (StPages[StPage].Command)
                StPages[StPage].Command(Id, HIWORD(wParam));
            return 0;
        }

        case WM_WIFI_DONE:
            StWifiDone((PST_WIFI_JOB)lParam);
            return 0;

        case WM_NET_DONE:
            StNetDone(lParam);
            return 0;

        case WM_SND_DONE:
            StSndDone(lParam);
            return 0;

        case WM_DESTROY:
            KillTimer(Window, ST_REFRESH_TIMER);
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(Window, Message, wParam, lParam);
}

int
WINAPI
wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nShowCmd)
{
    WNDCLASSEXW Class;
    HWND Window;
    MSG Msg;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nShowCmd);

    StInstance = hInstance;
    StFont = StMakeFont(34, FW_NORMAL);
    StSmallFont = StMakeFont(28, FW_NORMAL);
    StBigFont = StMakeFont(40, FW_SEMIBOLD);
    StHeadFont = StMakeFont(48, FW_SEMIBOLD);
    StButtonFont = StMakeFont(32, FW_SEMIBOLD);
    if (!StFont)
        StFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (!StSmallFont)
        StSmallFont = StFont;
    if (!StBigFont)
        StBigFont = StFont;
    if (!StHeadFont)
        StHeadFont = StFont;
    if (!StButtonFont)
        StButtonFont = StFont;

    StBgBrush = CreateSolidBrush(ST_BG);
    StFieldBrush = CreateSolidBrush(ST_FIELD);

    ZeroMemory(&Class, sizeof(Class));
    Class.cbSize = sizeof(Class);
    Class.lpfnWndProc = StWndProc;
    Class.hInstance = hInstance;
    Class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    Class.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    Class.hbrBackground = StBgBrush;
    Class.lpszClassName = L"ReactOSSettings";
    if (!RegisterClassExW(&Class))
        return 1;

    Window = CreateWindowExW(0, Class.lpszClassName, L"Settings", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                             NULL, NULL, hInstance, NULL);
    if (!Window)
        return 1;

    ShowWindow(Window, SW_SHOWMAXIMIZED);
    UpdateWindow(Window);

    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        if (!IsDialogMessageW(Window, &Msg))
        {
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
    }

    if (StInterfaces)
        WlanFreeMemory(StInterfaces);
    if (StNetworks)
        WlanFreeMemory(StNetworks);
    HeapFree(GetProcessHeap(), 0, StOrder);
    StNetCleanup();
    StSndCleanup();
    DeleteObject(StBgBrush);
    DeleteObject(StFieldBrush);
    return (int)Msg.wParam;
}
