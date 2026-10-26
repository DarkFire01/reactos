/*
 * PROJECT:     ReactOS Authentication UI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shut down choices offered by the start menu
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define CHOICES_MAX             16

typedef struct SHUTDOWN_CHOICE_TEXT
{
    DWORD dwChoice;
    UINT idsName;
    UINT idsMenuName;
    UINT idsDesc;
} SHUTDOWN_CHOICE_TEXT;

static const SHUTDOWN_CHOICE_TEXT s_rgChoiceText[] =
{
    { SDC_SHUTDOWN, IDS_SHUTDOWN, IDS_SHUTDOWN_MENU, IDS_SHUTDOWN_DESC },
    { SDC_RESTART, IDS_RESTART, IDS_RESTART_MENU, IDS_RESTART_DESC },
    { SDC_SLEEP, IDS_SLEEP, IDS_SLEEP_MENU, IDS_SLEEP_DESC },
    { SDC_HIBERNATE, IDS_HIBERNATE, IDS_HIBERNATE_MENU, IDS_HIBERNATE_DESC },
    {
        SDC_INSTALLUPDATES | SDC_SHUTDOWN,
        IDS_UPDATESHUTDOWN, IDS_UPDATESHUTDOWN_MENU, IDS_UPDATESHUTDOWN_DESC
    },
    {
        SDC_INSTALLUPDATES | SDC_RESTART,
        IDS_UPDATERESTART, IDS_UPDATERESTART_MENU, IDS_UPDATERESTART_DESC
    },
    { SDC_DISCONNECT, IDS_DISCONNECT, IDS_DISCONNECT_MENU, IDS_DISCONNECT_DESC },
    { SDC_SWITCHUSER, IDS_SWITCHUSER, IDS_SWITCHUSER_MENU, IDS_SWITCHUSER_DESC },
    { SDC_LOGOFF, IDS_LOGOFF, IDS_LOGOFF_MENU, IDS_LOGOFF_DESC },
    { SDC_LOCK, IDS_LOCK, IDS_LOCK_MENU, IDS_LOCK_DESC },
    { SDC_UNDOCK, IDS_UNDOCK, IDS_UNDOCK_MENU, IDS_UNDOCK_DESC },
};

/* Session choices first, then a separator and the power choices, like the menu shows them */
static const DWORD s_rgSessionChoices[] =
{
    SDC_SWITCHUSER, SDC_LOGOFF, SDC_LOCK, SDC_DISCONNECT, SDC_UNDOCK
};
static const DWORD s_rgPowerChoices[] = { SDC_RESTART, SDC_SLEEP, SDC_HIBERNATE, SDC_SHUTDOWN };

/* Values Start_PowerButtonAction may hold */
static const DWORD s_rgButtonChoices[] =
{
    SDC_LOGOFF, SDC_SHUTDOWN, SDC_RESTART, SDC_SLEEP, SDC_HIBERNATE, SDC_SWITCHUSER, SDC_LOCK
};

static const WCHAR s_szListenerClass[] = L"ShutdownChoiceListener";

static
BOOL
IsPolicySet(
    _In_ PCWSTR pszArea,
    _In_ PCWSTR pszValue)
{
    WCHAR szKey[MAX_PATH];
    HKEY rghkeyRoots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };

    StringCchPrintfW(szKey,
                     _countof(szKey),
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\%s",
                     pszArea);

    for (UINT i = 0; i < _countof(rghkeyRoots); i++)
    {
        DWORD dwValue = 0;
        DWORD cbValue = sizeof(dwValue);

        if (SHGetValueW(rghkeyRoots[i], szKey, pszValue, NULL, &dwValue, &cbValue) == ERROR_SUCCESS)
            return dwValue != 0;
    }

    return FALSE;
}

static
BOOL
IsServerProduct(VOID)
{
    OSVERSIONINFOEXW osvi;

    ZeroMemory(&osvi, sizeof(osvi));
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    if (!GetVersionExW((OSVERSIONINFOW *)&osvi))
        return FALSE;

    return osvi.wProductType != VER_NT_WORKSTATION;
}

static
const SHUTDOWN_CHOICE_TEXT *
FindChoiceText(
    _In_ DWORD dwChoice)
{
    for (UINT i = 0; i < _countof(s_rgChoiceText); i++)
    {
        if (s_rgChoiceText[i].dwChoice == dwChoice)
            return &s_rgChoiceText[i];
    }

    return NULL;
}

/* Whether the machine or session offers the choice at all */
static
BOOL
IsChoiceAvailable(
    _In_ DWORD dwChoice)
{
    SYSTEM_POWER_CAPABILITIES spc;

    switch (dwChoice)
    {
        case SDC_LOGOFF:
        case SDC_LOCK:
            return TRUE;

        case SDC_SWITCHUSER:
            return !GetSystemMetrics(SM_REMOTESESSION);

        case SDC_DISCONNECT:
            return GetSystemMetrics(SM_REMOTESESSION) != 0;

        case SDC_SHUTDOWN:
        case SDC_RESTART:
            return !IsPolicySet(L"Explorer", L"NoClose");

        case SDC_SLEEP:
            if (IsPolicySet(L"Explorer", L"NoClose") || !GetPwrCapabilities(&spc))
                return FALSE;
            return spc.SystemS1 || spc.SystemS2 || spc.SystemS3;

        case SDC_HIBERNATE:
            if (IsPolicySet(L"Explorer", L"NoClose") || !GetPwrCapabilities(&spc))
                return FALSE;
            return spc.SystemS4 && spc.HiberFilePresent;
    }

    /* No dock detection: undock is never offered */
    return FALSE;
}

/* Whether this user may pick a choice the machine offers */
static
BOOL
IsChoiceAllowed(
    _In_ IShutdownChoices *psdc,
    _In_ DWORD dwChoice)
{
    switch (dwChoice)
    {
        case SDC_LOGOFF:
            return !IsPolicySet(L"Explorer", L"NoLogoff");

        case SDC_SWITCHUSER:
            return !IsPolicySet(L"System", L"HideFastUserSwitching");

        case SDC_LOCK:
            return !IsPolicySet(L"System", L"DisableLockWorkstation");

        case SDC_SHUTDOWN:
        case SDC_RESTART:
        case SDC_SLEEP:
        case SDC_HIBERNATE:
            return psdc->UserHasShutdownRights() == S_OK;
    }

    return TRUE;
}

/* Enumerator */

class CEnumShutdownChoices :
    public CComObjectRootEx<CComSingleThreadModel>,
    public IEnumShutdownChoices
{
private:
    DWORD m_rgChoices[CHOICES_MAX];
    ULONG m_cChoices;
    ULONG m_iNext;

public:
    CEnumShutdownChoices() :
        m_cChoices(0),
        m_iNext(0)
    {
        ZeroMemory(m_rgChoices, sizeof(m_rgChoices));
    }

    void Add(
        _In_ DWORD dwChoice)
    {
        if (m_cChoices < _countof(m_rgChoices))
            m_rgChoices[m_cChoices++] = dwChoice;
    }

    STDMETHODIMP Next(
        _In_ ULONG celt,
        _Out_writes_to_(celt, *pceltFetched) DWORD *rgChoices,
        _Out_opt_ ULONG *pceltFetched) override
    {
        ULONG cFetched = 0;

        while (cFetched < celt && m_iNext < m_cChoices)
            rgChoices[cFetched++] = m_rgChoices[m_iNext++];

        if (pceltFetched)
            *pceltFetched = cFetched;

        return (cFetched == celt) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Skip(
        _In_ ULONG celt) override
    {
        ULONG cLeft = m_cChoices - m_iNext;

        if (celt > cLeft)
        {
            m_iNext = m_cChoices;
            return S_FALSE;
        }

        m_iNext += celt;
        return S_OK;
    }

    STDMETHODIMP Reset() override
    {
        m_iNext = 0;
        return S_OK;
    }

    STDMETHODIMP Clone(
        _COM_Outptr_ IEnumShutdownChoices **ppenum) override
    {
        CComObject<CEnumShutdownChoices> *pClone;
        HRESULT hr;

        *ppenum = NULL;
        hr = CComObject<CEnumShutdownChoices>::CreateInstance(&pClone);
        if (FAILED(hr))
            return hr;

        CopyMemory(pClone->m_rgChoices, m_rgChoices, sizeof(m_rgChoices));
        pClone->m_cChoices = m_cChoices;
        pClone->m_iNext = m_iNext;
        pClone->AddRef();
        *ppenum = pClone;
        return S_OK;
    }

    BEGIN_COM_MAP(CEnumShutdownChoices)
        COM_INTERFACE_ENTRY_IID(IID_IEnumShutdownChoices, IEnumShutdownChoices)
    END_COM_MAP()
};

/* Listener */

class CShutdownChoiceListener :
    public CComObjectRootEx<CComSingleThreadModel>,
    public IShutdownChoiceListener
{
private:
    HWND m_hwndParent;
    HWND m_hwnd;

    static LRESULT CALLBACK s_WndProc(
        _In_ HWND hwnd,
        _In_ UINT uMsg,
        _In_ WPARAM wParam,
        _In_ LPARAM lParam);

public:
    CShutdownChoiceListener() :
        m_hwndParent(NULL),
        m_hwnd(NULL)
    {
    }

    ~CShutdownChoiceListener()
    {
        if (m_hwnd)
        {
            SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
            DestroyWindow(m_hwnd);
        }
    }

    STDMETHODIMP Initialize(
        _In_ HWND hwndParent,
        _In_ DWORD dwFlags) override
    {
        UNREFERENCED_PARAMETER(dwFlags);
        m_hwndParent = hwndParent;
        return S_OK;
    }

    STDMETHODIMP GetWindow(
        _Out_ HWND *phwnd) override
    {
        *phwnd = m_hwnd;
        return m_hwnd ? S_OK : E_FAIL;
    }

    /* The owner reads the choices again when it gets the notification */
    STDMETHODIMP Refresh() override
    {
        NMHDR nmh;

        if (!m_hwnd || !m_hwndParent)
            return E_FAIL;

        nmh.hwndFrom = m_hwnd;
        nmh.idFrom = 0;
        nmh.code = 0;
        SendMessageW(m_hwndParent, WM_NOTIFY, 0, (LPARAM)&nmh);
        return S_OK;
    }

    /* A top level window, so power and setting broadcasts reach it */
    STDMETHODIMP StartListening() override
    {
        WNDCLASSW wc;

        if (m_hwnd)
            return S_OK;

        if (!GetClassInfoW(_AtlBaseModule.GetModuleInstance(), s_szListenerClass, &wc))
        {
            ZeroMemory(&wc, sizeof(wc));
            wc.lpfnWndProc = s_WndProc;
            wc.hInstance = _AtlBaseModule.GetModuleInstance();
            wc.lpszClassName = s_szListenerClass;
            if (!RegisterClassW(&wc))
                return HRESULT_FROM_WIN32(GetLastError());
        }

        m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW,
                                 s_szListenerClass,
                                 NULL,
                                 WS_POPUP,
                                 0,
                                 0,
                                 0,
                                 0,
                                 m_hwndParent,
                                 NULL,
                                 _AtlBaseModule.GetModuleInstance(),
                                 NULL);
        if (!m_hwnd)
            return HRESULT_FROM_WIN32(GetLastError());

        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);
        return S_OK;
    }

    BEGIN_COM_MAP(CShutdownChoiceListener)
        COM_INTERFACE_ENTRY_IID(IID_IUnknown, IShutdownChoiceListener)
    END_COM_MAP()
};

LRESULT
CALLBACK
CShutdownChoiceListener::s_WndProc(
    _In_ HWND hwnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    CShutdownChoiceListener *pListener =
        (CShutdownChoiceListener *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (uMsg)
    {
        case WM_POWERBROADCAST:
            if (pListener && wParam == PBT_APMPOWERSTATUSCHANGE)
                pListener->Refresh();
            return TRUE;

        case WM_SETTINGCHANGE:
        case WM_DEVICECHANGE:
            if (pListener)
                pListener->Refresh();
            break;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

/* Choices */

CShutdownChoices::CShutdownChoices() :
    m_dwChoiceMask(SDC_DEFAULTMASK),
    m_fShowBadChoices(FALSE)
{
}

/* The choice when it can be picked, flagged disabled when only shown, else 0 */
DWORD
CShutdownChoices::TestChoice(
    _In_ DWORD dwChoice)
{
    if ((m_dwChoiceMask & dwChoice) != dwChoice || !IsChoiceAvailable(dwChoice))
        return 0;

    if (IsChoiceAllowed(this, dwChoice))
        return dwChoice;

    return m_fShowBadChoices ? (dwChoice | SDC_DISABLED) : 0;
}

STDMETHODIMP
CShutdownChoices::Refresh()
{
    /* Every query reads the machine state, nothing is cached */
    return S_OK;
}

STDMETHODIMP
CShutdownChoices::CreateListener(
    _COM_Outptr_ IShutdownChoiceListener **ppListener)
{
    CComObject<CShutdownChoiceListener> *pListener;
    HRESULT hr;

    *ppListener = NULL;
    hr = CComObject<CShutdownChoiceListener>::CreateInstance(&pListener);
    if (FAILED(hr))
        return hr;

    pListener->AddRef();
    *ppListener = pListener;
    return S_OK;
}

STDMETHODIMP
CShutdownChoices::SetChoiceMask(
    _In_ DWORD dwChoiceMask)
{
    m_dwChoiceMask = dwChoiceMask;
    return S_OK;
}

STDMETHODIMP
CShutdownChoices::GetChoiceMask(
    _Out_ DWORD *pdwChoiceMask)
{
    *pdwChoiceMask = m_dwChoiceMask;
    return S_OK;
}

STDMETHODIMP
CShutdownChoices::SetShowBadChoicesAsDisabled(
    _In_ BOOL fShow)
{
    m_fShowBadChoices = fShow;
    return S_OK;
}

STDMETHODIMP
CShutdownChoices::GetChoiceEnumerator(
    _COM_Outptr_ IEnumShutdownChoices **ppenum)
{
    CComObject<CEnumShutdownChoices> *pEnum;
    HRESULT hr;

    *ppenum = NULL;
    hr = CComObject<CEnumShutdownChoices>::CreateInstance(&pEnum);
    if (FAILED(hr))
        return hr;

    for (UINT i = 0; i < _countof(s_rgSessionChoices); i++)
    {
        DWORD dwChoice = TestChoice(s_rgSessionChoices[i]);

        if (dwChoice)
            pEnum->Add(dwChoice);
    }

    if (m_dwChoiceMask & SDC_SEPARATOR)
        pEnum->Add(SDC_SEPARATOR);

    for (UINT i = 0; i < _countof(s_rgPowerChoices); i++)
    {
        DWORD dwChoice = TestChoice(s_rgPowerChoices[i]);

        if (dwChoice)
            pEnum->Add(dwChoice);
    }

    pEnum->AddRef();
    *ppenum = pEnum;
    return S_OK;
}

/* The power button choice: policy first, then the user's setting, else shut down */
STDMETHODIMP
CShutdownChoices::GetDefaultChoice(
    _Out_ DWORD *pdwChoice)
{
    DWORD dwChoice = 0;
    DWORD cbChoice = sizeof(dwChoice);
    BOOL fFound = FALSE;

    *pdwChoice = 0;

    /* A remote session ends by logging off */
    if ((m_dwChoiceMask & SDC_LOGOFF) && GetSystemMetrics(SM_REMOTESESSION))
    {
        *pdwChoice = SDC_LOGOFF;
        return S_OK;
    }

    if (SHGetValueW(HKEY_LOCAL_MACHINE,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
                    L"PowerButtonAction",
                    NULL,
                    &dwChoice,
                    &cbChoice) == ERROR_SUCCESS)
    {
        fFound = TRUE;
    }
    else
    {
        HKEY rghkeyRoots[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };

        for (UINT i = 0; i < _countof(rghkeyRoots) && !fFound; i++)
        {
            cbChoice = sizeof(dwChoice);
            fFound = SHGetValueW(rghkeyRoots[i],
                                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                                 L"Start_PowerButtonAction",
                                 NULL,
                                 &dwChoice,
                                 &cbChoice) == ERROR_SUCCESS;
        }
    }

    if (fFound)
    {
        BOOL fValid = FALSE;

        for (UINT i = 0; i < _countof(s_rgButtonChoices); i++)
        {
            if (s_rgButtonChoices[i] == dwChoice)
                fValid = TRUE;
        }

        dwChoice = fValid ? TestChoice(dwChoice) : 0;
    }
    else
    {
        dwChoice = 0;
    }

    if (!dwChoice)
    {
        if (IsServerProduct() && (m_dwChoiceMask & SDC_LOGOFF))
            dwChoice = SDC_LOGOFF;
        else
            dwChoice = SDC_SHUTDOWN;
    }

    /* Without the right to shut down the button falls back to logging off */
    if ((dwChoice & (SDC_SHUTDOWN | SDC_RESTART | SDC_SLEEP | SDC_HIBERNATE)) &&
        UserHasShutdownRights() != S_OK)
    {
        if (m_fShowBadChoices)
            dwChoice |= SDC_DISABLED;
        else
            dwChoice = (m_dwChoiceMask & SDC_LOGOFF) ? SDC_LOGOFF : 0;
    }

    *pdwChoice = dwChoice;
    return S_OK;
}

/* S_OK when the caller's token holds the shut down privilege, enabled or not */
STDMETHODIMP
CShutdownChoices::UserHasShutdownRights()
{
    HANDLE hToken;
    LUID luidShutdown;
    PTOKEN_PRIVILEGES ptp;
    DWORD cbPrivileges = 0;
    HRESULT hr = S_FALSE;

    if (!LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &luidShutdown))
        return S_FALSE;

    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &hToken) &&
        !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    {
        return S_FALSE;
    }

    GetTokenInformation(hToken, TokenPrivileges, NULL, 0, &cbPrivileges);
    ptp = (PTOKEN_PRIVILEGES)LocalAlloc(LPTR, cbPrivileges);
    if (ptp)
    {
        if (GetTokenInformation(hToken, TokenPrivileges, ptp, cbPrivileges, &cbPrivileges))
        {
            for (DWORD i = 0; i < ptp->PrivilegeCount; i++)
            {
                if (ptp->Privileges[i].Luid.LowPart == luidShutdown.LowPart &&
                    ptp->Privileges[i].Luid.HighPart == luidShutdown.HighPart)
                {
                    hr = S_OK;
                    break;
                }
            }
        }

        LocalFree(ptp);
    }

    CloseHandle(hToken);
    return hr;
}

/* dwFlags 0 gives the button label, 1 the menu label with its access key */
STDMETHODIMP
CShutdownChoices::GetChoiceName(
    _In_ DWORD dwChoice,
    _In_ DWORD dwFlags,
    _Out_writes_(cchName) PWSTR pszName,
    _In_ UINT cchName)
{
    const SHUTDOWN_CHOICE_TEXT *pText = FindChoiceText(dwChoice & ~SDC_DISABLEDMASK);
    UINT ids;

    if (!pText)
        return E_INVALIDARG;

    ids = dwFlags ? pText->idsMenuName : pText->idsName;
    if (!LoadStringW(_AtlBaseModule.GetResourceInstance(), ids, pszName, cchName))
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

    return S_OK;
}

STDMETHODIMP
CShutdownChoices::GetChoiceDesc(
    _In_ DWORD dwChoice,
    _Out_writes_(cchDesc) PWSTR pszDesc,
    _In_ UINT cchDesc)
{
    UINT ids;

    if (dwChoice & SDC_DISABLED)
    {
        ids = IDS_DISABLED_DESC;
    }
    else
    {
        const SHUTDOWN_CHOICE_TEXT *pText = FindChoiceText(dwChoice & ~SDC_DISABLEDMASK);

        if (!pText)
            return E_INVALIDARG;

        ids = pText->idsDesc;
    }

    if (!LoadStringW(_AtlBaseModule.GetResourceInstance(), ids, pszDesc, cchDesc))
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

    return S_OK;
}
