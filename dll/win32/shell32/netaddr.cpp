/*
 * PROJECT:     ReactOS Shell
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Network address edit control (msctls_netaddress)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

typedef DWORD (WINAPI *PFN_PARSE_NETWORK_STRING)(PCWSTR, DWORD, PVOID, PUSHORT, PBYTE);

typedef struct _NETADDR_CONTROL
{
    DWORD LastError;
    DWORD AllowType;
    HWND hWnd;
} NETADDR_CONTROL, *PNETADDR_CONTROL;

static WNDPROC g_pfnNetAddrEditProc;
static INT g_NetAddrStateIndex;
static PFN_PARSE_NETWORK_STRING g_pfnParseNetworkString;

/* NET_STRING_ANY_ADDRESS, named hosts plus IPv4 and IPv6 addresses */
static const DWORD g_NetAddrDefaultAllowType = 0x109;

static
HRESULT
NetAddrValidate(
    _In_ PNETADDR_CONTROL Control,
    _Inout_opt_ PNC_ADDRESS Address)
{
    HMODULE hIpHlp;
    PWSTR Text;
    INT Length;
    DWORD Error;

    if (!Address || !Address->pAddrInfo)
        return E_INVALIDARG;

    Length = GetWindowTextLengthW(Control->hWnd);
    if (!Length)
        return S_FALSE;

    Text = (PWSTR)LocalAlloc(LMEM_FIXED, (Length + 1) * sizeof(*Text));
    if (!Text)
        return E_OUTOFMEMORY;

    if (!GetWindowTextW(Control->hWnd, Text, Length + 1))
    {
        Error = GetLastError();
        LocalFree(Text);
        return HRESULT_FROM_WIN32(Error);
    }

    /* Format is the first field, unspecified until parsed */
    *(PDWORD)Address->pAddrInfo = 0;

    if (!*Text)
    {
        LocalFree(Text);
        return S_FALSE;
    }

    if (!g_pfnParseNetworkString)
    {
        hIpHlp = LoadLibraryW(L"iphlpapi.dll");
        if (hIpHlp)
            g_pfnParseNetworkString = (PFN_PARSE_NETWORK_STRING)GetProcAddress(hIpHlp, "ParseNetworkString");
    }

    if (g_pfnParseNetworkString)
    {
        Error = g_pfnParseNetworkString(Text,
                                        Control->AllowType,
                                        Address->pAddrInfo,
                                        &Address->PortNumber,
                                        &Address->PrefixLength);
    }
    else
    {
        ERR("iphlpapi has no ParseNetworkString\n");
        Error = ERROR_PROC_NOT_FOUND;
    }

    LocalFree(Text);
    Control->LastError = Error;
    return HRESULT_FROM_WIN32(Error);
}

static
HRESULT
NetAddrShowErrorTip(
    _In_ PNETADDR_CONTROL Control)
{
    EDITBALLOONTIP Tip;
    WCHAR Message[256];

    if (!LoadStringW(shell32_hInstance, IDS_NETADDRESS_INVALID, Message, _countof(Message)))
        return HRESULT_FROM_WIN32(GetLastError());

    Tip.cbStruct = sizeof(Tip);
    Tip.pszTitle = NULL;
    Tip.pszText = Message;
    Tip.ttiIcon = TTI_NONE;
    if (!SendMessageW(Control->hWnd, EM_SHOWBALLOONTIP, 0, (LPARAM)&Tip))
        return E_FAIL;

    return S_OK;
}

static
LRESULT
NetAddrHandleMessage(
    _In_ PNETADDR_CONTROL Control,
    _In_ HWND hWnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    switch (uMsg)
    {
        case NCM_GETADDRESS:
            return NetAddrValidate(Control, (PNC_ADDRESS)lParam);

        case NCM_SETALLOWTYPE:
            Control->AllowType = (DWORD)wParam;
            return TRUE;

        case NCM_GETALLOWTYPE:
            return Control->AllowType;

        case NCM_DISPLAYERRORTIP:
            return NetAddrShowErrorTip(Control);
    }

    return CallWindowProcW(g_pfnNetAddrEditProc, hWnd, uMsg, wParam, lParam);
}

static
LRESULT
CALLBACK
NetAddrWndProc(
    _In_ HWND hWnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    PNETADDR_CONTROL Control;
    LRESULT Result;

    Control = (PNETADDR_CONTROL)GetWindowLongPtrW(hWnd, g_NetAddrStateIndex);
    if (Control)
    {
        Result = NetAddrHandleMessage(Control, hWnd, uMsg, wParam, lParam);
        if (uMsg == WM_NCDESTROY)
        {
            SetWindowLongPtrW(hWnd, g_NetAddrStateIndex, 0);
            HeapFree(GetProcessHeap(), 0, Control);
        }
        return Result;
    }

    if (uMsg != WM_NCCREATE)
        return CallWindowProcW(g_pfnNetAddrEditProc, hWnd, uMsg, wParam, lParam);

    Control = (PNETADDR_CONTROL)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Control));
    if (!Control)
        return FALSE;

    Control->AllowType = g_NetAddrDefaultAllowType;
    Control->hWnd = hWnd;
    SetWindowLongPtrW(hWnd, g_NetAddrStateIndex, (LONG_PTR)Control);
    return NetAddrHandleMessage(Control, hWnd, uMsg, wParam, lParam);
}

/**
 * @brief
 * Registers the msctls_netaddress window class, an edit control that
 * parses its text as a network address.
 */
EXTERN_C
BOOL
WINAPI
InitNetworkAddressControl(VOID)
{
    WNDCLASSW WndClass;

    if (g_pfnNetAddrEditProc)
        return TRUE;

    if (!GetClassInfoW(NULL, WC_EDITW, &WndClass))
        return FALSE;

    g_pfnNetAddrEditProc = WndClass.lpfnWndProc;
    g_NetAddrStateIndex = WndClass.cbWndExtra;

    WndClass.style |= CS_GLOBALCLASS;
    WndClass.cbWndExtra += sizeof(PNETADDR_CONTROL);
    WndClass.hInstance = shell32_hInstance;
    WndClass.lpfnWndProc = NetAddrWndProc;
    WndClass.lpszClassName = WC_NETADDRESS;
    WndClass.lpszMenuName = NULL;

    if (RegisterClassW(&WndClass))
        return TRUE;

    return (GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
}
