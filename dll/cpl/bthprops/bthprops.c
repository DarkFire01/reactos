/*
 * PROJECT:     ReactOS Bluetooth Control Panel
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Control panel applet, pairing and device dialogs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <stdarg.h>
#include <stdlib.h>
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <winnls.h>
#include <winsvc.h>
#include <winioctl.h>
#include <commctrl.h>
#include <prsht.h>
#include <cpl.h>
#include <strsafe.h>
#include <bthsdpdef.h>
#include <bluetoothapis.h>
#include <bthioctl.h>

#include "resource.h"

static HINSTANCE BthpInstance;

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE hinstDLL,
    _In_ DWORD fdwReason,
    _In_ LPVOID lpvReserved)
{
    UNREFERENCED_PARAMETER(lpvReserved);

    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        BthpInstance = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }

    return TRUE;
}

/* The applet only loads while the Bluetooth support service runs */
static
BOOL
BthpIsSupportServiceRunning(VOID)
{
    SERVICE_STATUS Status;
    SC_HANDLE Manager;
    SC_HANDLE Service;
    BOOL Running = FALSE;

    Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (!Manager)
        return FALSE;

    Service = OpenServiceW(Manager, L"BTHSERV", SERVICE_QUERY_STATUS);
    if (Service)
    {
        if (QueryServiceStatus(Service, &Status))
        {
            Running = Status.dwCurrentState != SERVICE_STOPPED &&
                      Status.dwCurrentState != SERVICE_STOP_PENDING;
        }
        CloseServiceHandle(Service);
    }

    CloseServiceHandle(Manager);
    return Running;
}

LONG
CALLBACK
CPlApplet(
    _In_ HWND hwndCPl,
    _In_ UINT uMsg,
    _In_ LPARAM lParam1,
    _In_ LPARAM lParam2)
{
    INITCOMMONCONTROLSEX Controls;
    LPCPLINFO Info;

    UNREFERENCED_PARAMETER(hwndCPl);
    UNREFERENCED_PARAMETER(lParam1);

    switch (uMsg)
    {
        case CPL_INIT:
            if (!BthpIsSupportServiceRunning())
                return FALSE;
            Controls.dwSize = sizeof(Controls);
            Controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES;
            return InitCommonControlsEx(&Controls);

        case CPL_GETCOUNT:
            return 1;

        case CPL_INQUIRE:
            Info = (LPCPLINFO)lParam2;
            Info->idIcon = IDI_BLUETOOTH;
            Info->idName = IDS_CPLNAME;
            Info->idInfo = IDS_CPLDESCRIPTION;
            Info->lData = 0;
            return 0;

        case CPL_DBLCLK:
        case CPL_STARTWPARMSA:
        case CPL_STARTWPARMSW:
            /* Windows opens its Bluetooth settings here, which are reached only through the support service */
            return TRUE;
    }

    return 0;
}

/* Pairs through one radio, or through the first radio found when none is given */
static
DWORD
BthpPairThroughRadio(
    _In_opt_ HANDLE Radio)
{
    BLUETOOTH_FIND_RADIO_PARAMS Params;
    BTH_LOCAL_RADIO_INFO Info;
    HBLUETOOTH_RADIO_FIND Find;
    DWORD Returned;
    HANDLE Found;

    if (!Radio)
    {
        Params.dwSize = sizeof(Params);
        Find = BluetoothFindFirstRadio(&Params, &Found);
        if (!Find)
            return ERROR_NO_MORE_ITEMS;

        BluetoothFindRadioClose(Find);
        CloseHandle(Found);
        return ERROR_NOT_SUPPORTED;
    }

    /* A handle that is not a radio fails the way the pairing request would */
    if (!DeviceIoControl(Radio,
                         IOCTL_BTH_GET_LOCAL_INFO,
                         NULL,
                         0,
                         &Info,
                         sizeof(Info),
                         &Returned,
                         NULL))
    {
        return GetLastError();
    }

    return ERROR_NOT_SUPPORTED;
}

static
DWORD
BthpAuthenticateDevices(
    _In_opt_ HANDLE Radio,
    _Inout_updates_opt_(Count) BLUETOOTH_DEVICE_INFO *Devices,
    _In_ DWORD Count,
    _In_opt_ PCWSTR Passkey,
    _In_ ULONG PasskeyLength)
{
    BOOL AllAuthenticated = TRUE;
    DWORD Result = ERROR_SUCCESS;
    DWORD Error;
    DWORD Index;

    if ((Passkey && PasskeyLength > BLUETOOTH_MAX_PASSKEY_SIZE) || (Count && !Devices))
        return ERROR_INVALID_PARAMETER;

    for (Index = 0; Index < Count; Index++)
    {
        if (Devices[Index].dwSize != sizeof(Devices[Index]))
            return ERROR_INVALID_PARAMETER;
        if (!Devices[Index].fAuthenticated)
            AllAuthenticated = FALSE;
    }

    /* Nothing is left to pair */
    if (AllAuthenticated)
        return ERROR_NO_MORE_ITEMS;

    for (Index = 0; Index < Count; Index++)
    {
        /* The device stays unpaired unless pairing succeeds */
        Devices[Index].fRemembered = FALSE;
        Devices[Index].fAuthenticated = FALSE;

        Error = BthpPairThroughRadio(Radio);
        if (Result == ERROR_SUCCESS)
            Result = Error;
    }

    return Result;
}

DWORD
WINAPI
BluetoothAuthenticateDevice(
    _In_opt_ HWND hwndParent,
    _In_opt_ HANDLE hRadio,
    _Inout_ BLUETOOTH_DEVICE_INFO *pbtbi,
    _In_reads_opt_(ulPasskeyLength) PWSTR pszPasskey,
    _In_ ULONG ulPasskeyLength)
{
    UNREFERENCED_PARAMETER(hwndParent);
    return BthpAuthenticateDevices(hRadio, pbtbi, 1, pszPasskey, ulPasskeyLength);
}

DWORD
WINAPI
BluetoothAuthenticateDeviceEx(
    _In_opt_ HWND hwndParentIn,
    _In_opt_ HANDLE hRadioIn,
    _Inout_ BLUETOOTH_DEVICE_INFO *pbtdiInout,
    _In_opt_ PBLUETOOTH_OOB_DATA_INFO pbtOobData,
    _In_ AUTHENTICATION_REQUIREMENTS authenticationRequirement)
{
    UNREFERENCED_PARAMETER(hwndParentIn);
    UNREFERENCED_PARAMETER(pbtOobData);
    UNREFERENCED_PARAMETER(authenticationRequirement);
    return BthpAuthenticateDevices(hRadioIn, pbtdiInout, 1, NULL, 0);
}

DWORD
WINAPI
BluetoothAuthenticateMultipleDevices(
    _In_opt_ HWND hwndParent,
    _In_opt_ HANDLE hRadio,
    _In_ DWORD cDevices,
    _Inout_updates_(cDevices) BLUETOOTH_DEVICE_INFO *rgbtdi)
{
    UNREFERENCED_PARAMETER(hwndParent);
    return BthpAuthenticateDevices(hRadio, rgbtdi, cDevices, NULL, 0);
}

static
INT_PTR
CALLBACK
BthpSelectDialogProc(
    _In_ HWND hwndDlg,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    BLUETOOTH_SELECT_DEVICE_PARAMS *Params;

    switch (uMsg)
    {
        case WM_INITDIALOG:
            Params = (BLUETOOTH_SELECT_DEVICE_PARAMS *)lParam;
            if (Params->pszInfo)
                SetDlgItemTextW(hwndDlg, IDC_SELECT_INFO, Params->pszInfo);

            /* No radio means no device shows up, so there is nothing to confirm */
            EnableWindow(GetDlgItem(hwndDlg, IDOK), FALSE);
            return TRUE;

        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(hwndDlg, IDCANCEL);
                return TRUE;
            }
            break;
    }

    return FALSE;
}

BOOL
WINAPI
BluetoothSelectDevicesFree(
    _Inout_ BLUETOOTH_SELECT_DEVICE_PARAMS *pbtsdp)
{
    BOOL Freed = FALSE;

    if (pbtsdp->dwSize != sizeof(*pbtsdp))
    {
        SetLastError(ERROR_REVISION_MISMATCH);
        return FALSE;
    }

    if (pbtsdp->pDevices)
    {
        HeapFree(GetProcessHeap(), 0, pbtsdp->pDevices);
        Freed = TRUE;
    }

    pbtsdp->cNumDevices = 0;
    pbtsdp->pDevices = NULL;
    return Freed;
}

BOOL
WINAPI
BluetoothSelectDevices(
    _Inout_ BLUETOOTH_SELECT_DEVICE_PARAMS *pbtsdp)
{
    if (!pbtsdp)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    pbtsdp->pDevices = NULL;
    if (pbtsdp->dwSize != sizeof(*pbtsdp))
    {
        SetLastError(ERROR_REVISION_MISMATCH);
        return FALSE;
    }

    /* The picker and the add device wizard can only be dismissed when no radio is present */
    DialogBoxParamW(BthpInstance,
                    MAKEINTRESOURCEW(IDD_SELECT_DEVICE),
                    pbtsdp->hwndParent,
                    BthpSelectDialogProc,
                    (LPARAM)pbtsdp);

    BluetoothSelectDevicesFree(pbtsdp);
    SetLastError(ERROR_CANCELLED);
    return FALSE;
}

static
VOID
BthpSetDateText(
    _In_ HWND hwndDlg,
    _In_ INT Control,
    _In_ const SYSTEMTIME *Time)
{
    WCHAR Text[128];
    INT Length;

    if (Time->wYear == 0)
    {
        LoadStringW(BthpInstance, IDS_NEVER, Text, _countof(Text));
        SetDlgItemTextW(hwndDlg, Control, Text);
        return;
    }

    Length = GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, Time, NULL, Text, _countof(Text));
    if (Length > 0 && Length < _countof(Text))
    {
        Text[Length - 1] = L' ';
        GetTimeFormatW(LOCALE_USER_DEFAULT, 0, Time, NULL, Text + Length, _countof(Text) - Length);
    }
    SetDlgItemTextW(hwndDlg, Control, Text);
}

static
VOID
BthpSetYesNoText(
    _In_ HWND hwndDlg,
    _In_ INT Control,
    _In_ BOOL Value)
{
    WCHAR Text[32];

    LoadStringW(BthpInstance, Value ? IDS_YES : IDS_NO, Text, _countof(Text));
    SetDlgItemTextW(hwndDlg, Control, Text);
}

static
INT_PTR
CALLBACK
BthpGeneralPageProc(
    _In_ HWND hwndDlg,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    const BLUETOOTH_DEVICE_INFO *Device;
    const BYTE *Bytes;
    WCHAR Text[64];

    UNREFERENCED_PARAMETER(wParam);

    if (uMsg != WM_INITDIALOG)
        return FALSE;

    Device = (const BLUETOOTH_DEVICE_INFO *)((LPPROPSHEETPAGEW)lParam)->lParam;
    Bytes = Device->Address.rgBytes;

    SetDlgItemTextW(hwndDlg, IDC_DEVICE_NAME, Device->szName);

    /* The address prints most significant byte first */
    StringCchPrintfW(Text,
                     _countof(Text),
                     L"%02x:%02x:%02x:%02x:%02x:%02x",
                     Bytes[5], Bytes[4], Bytes[3], Bytes[2], Bytes[1], Bytes[0]);
    SetDlgItemTextW(hwndDlg, IDC_DEVICE_ADDRESS, Text);

    StringCchPrintfW(Text, _countof(Text), L"0x%06lX", Device->ulClassofDevice);
    SetDlgItemTextW(hwndDlg, IDC_DEVICE_CLASS, Text);

    BthpSetYesNoText(hwndDlg, IDC_DEVICE_CONNECTED, Device->fConnected);
    BthpSetYesNoText(hwndDlg, IDC_DEVICE_REMEMBERED, Device->fRemembered);
    BthpSetYesNoText(hwndDlg, IDC_DEVICE_AUTHENTICATED, Device->fAuthenticated);
    BthpSetDateText(hwndDlg, IDC_DEVICE_LASTSEEN, &Device->stLastSeen);
    BthpSetDateText(hwndDlg, IDC_DEVICE_LASTUSED, &Device->stLastUsed);
    return TRUE;
}

BOOL
WINAPI
BluetoothDisplayDeviceProperties(
    _In_opt_ HWND hwndParent,
    _Inout_ BLUETOOTH_DEVICE_INFO *pbtdi)
{
    PROPSHEETHEADERW Header;
    PROPSHEETPAGEW Page;
    WCHAR Format[64];
    WCHAR Title[BLUETOOTH_MAX_NAME_SIZE + 64];

    if (!pbtdi)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    LoadStringW(BthpInstance, IDS_PROPERTIES_TITLE, Format, _countof(Format));
    StringCchPrintfW(Title, _countof(Title), Format, pbtdi->szName);

    ZeroMemory(&Page, sizeof(Page));
    Page.dwSize = sizeof(Page);
    Page.hInstance = BthpInstance;
    Page.pszTemplate = MAKEINTRESOURCEW(IDD_DEVICE_GENERAL);
    Page.pfnDlgProc = BthpGeneralPageProc;
    Page.lParam = (LPARAM)pbtdi;

    ZeroMemory(&Header, sizeof(Header));
    Header.dwSize = sizeof(Header);
    Header.dwFlags = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW;
    Header.hwndParent = hwndParent;
    Header.hInstance = BthpInstance;
    Header.pszCaption = Title;
    Header.nPages = 1;
    Header.ppsp = &Page;

    return PropertySheetW(&Header) >= 0;
}
