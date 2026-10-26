/*
 * PROJECT:     ReactOS shlwapi
 * LICENSE:     LGPL-2.0-or-later (https://spdx.org/licenses/LGPL-2.0-or-later)
 * PURPOSE:     Implement SHWindowsPolicyGetValue
 * COPYRIGHT:   Copyright 2026 Katayama Hirofumi MZ (katayama.hirofumi.mz@gmail.com)
 */

#define _ATL_NO_EXCEPTIONS
#include <windef.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shlwapi_undoc.h>
#include <shlguid_undoc.h>
#include <atlstr.h>
#include <strsafe.h>
#include <new>
#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(policy);

typedef enum tagPOLICY_STATE
{
    POLICY_STATE_UNCACHED  = 0, // Uncached
    POLICY_STATE_NOT_FOUND = 1, // Not found
    POLICY_STATE_CACHED    = 2, // Cached
} POLICY_STATE;

// Result
typedef struct tagSHPOLICY_RESULT
{
    POLICY_STATE state;
    DWORD dwValue;
} SHPOLICY_RESULT, *PSHPOLICY_RESULT;

// Constraints
typedef struct tagSHPOLICY_CONSTRAINT
{
    WORD wFlags;
    WORD wMinSize;
    DWORD dwMin;
    DWORD dwMax;
} SHPOLICY_CONSTRAINT, *PSHPOLICY_CONSTRAINT;

static const SHPOLICY_CONSTRAINT c_Bool     = { SRRF_RT_DWORD,  sizeof(DWORD), 0, 1 };
static const SHPOLICY_CONSTRAINT c_String   = { SRRF_RT_REG_SZ, sizeof(WCHAR), 0, 0 };
static const SHPOLICY_CONSTRAINT c_TriValue = { SRRF_RT_DWORD,  sizeof(DWORD), 1, 3 };
static const SHPOLICY_CONSTRAINT c_Special  = { SRRF_RT_DWORD,  sizeof(DWORD), 0x1806, 0x1808 };

// Start menu policies
static const GUID POLID_NoRun =
    { 0xa9ea41a7, 0x4ea4, 0x4776, { 0x8c, 0x1e, 0xaa, 0x06, 0xa2, 0x51, 0x60, 0xf9 } };
static const GUID POLID_NoClose =
    { 0x29b0cc43, 0x2f2b, 0x4d0c, { 0xa0, 0x81, 0xa5, 0x28, 0xdd, 0x34, 0x96, 0x31 } };
static const GUID POLID_StartMenuLogoff =
    { 0xef2a45cf, 0xd2c9, 0x4ef8, { 0xa8, 0x4b, 0x12, 0xd7, 0xf1, 0x15, 0x0c, 0xa8 } };
static const GUID POLID_NoSMConfigurePrograms =
    { 0xffd2f653, 0x7f76, 0x4eba, { 0x9b, 0x49, 0xb7, 0x39, 0x24, 0xa7, 0xef, 0xae } };
static const GUID POLID_NoWindowsUpdate =
    { 0x68f894e4, 0x3c9d, 0x4c68, { 0x82, 0x95, 0xc8, 0x38, 0x0e, 0x22, 0x76, 0x55 } };
static const GUID POLID_NoStartMenuSubFolders =
    { 0x78627e11, 0xec80, 0x49a3, { 0xb3, 0xb7, 0x6a, 0xbe, 0x73, 0x96, 0x93, 0xfc } };
static const GUID POLID_TurnOffSidebar =
    { 0x13a1260e, 0x41c7, 0x462f, { 0xa3, 0x97, 0xe7, 0xfb, 0x03, 0xbe, 0x94, 0x7f } };
static const GUID POLID_NoMobilityCenter =
    { 0xa55f91dc, 0xaadc, 0x4038, { 0x82, 0xfc, 0x84, 0x80, 0x6d, 0x92, 0x89, 0x78 } };

// Items
typedef struct tagSHPOLICY_ITEM
{
    REFGUID rpolid; // POLID (policy descriptor)
    PCWSTR pszKeyName;
    PCWSTR pszValueName;
    const SHPOLICY_CONSTRAINT *pConstraint;
} SHPOLICY_ITEM, *PSHPOLICY_ITEM;

static const SHPOLICY_ITEM g_PolicyItems[] =
{
    { POLID_UsePathEnvVarForCommandTemplates, L"Explorer", L"UsePathEnvVarForCommandTemplates",
      &c_Bool },
    { POLID_ScanWithAntiVirus, L"Attachments", L"ScanWithAntiVirus", &c_TriValue },
    { POLID_SaveZoneInformation, L"Attachments", L"SaveZoneInformation", &c_TriValue },
    { POLID_UseTrustedHandlers, L"Attachments", L"UseTrustedHandlers", &c_TriValue },
    { POLID_HideZoneInfoOnProperties, L"Attachments", L"HideZoneInfoOnProperties", &c_Bool },
    { POLID_DefaultFileTypeRisk, L"Associations", L"DefaultFileTypeRisk", &c_Special },
    { POLID_HighRiskFileTypes, L"Associations", L"HighRiskFileTypes", &c_String },
    { POLID_ModRiskFileTypes, L"Associations", L"ModRiskFileTypes", &c_String },
    { POLID_LowRiskFileTypes, L"Associations", L"LowRiskFileTypes", &c_String },
    { POLID_PreXPSP2ShellProtocolBehavior, L"Explorer", L"PreXPSP2ShellProtocolBehavior", &c_Bool },
    { POLID_CompareJunctionness, L"Explorer", L"CompareJunctionness", &c_Bool },
    { POLID_NoRun, L"Explorer", L"NoRun", &c_Bool },
    { POLID_NoClose, L"Explorer", L"NoClose", &c_Bool },
    { POLID_StartMenuLogoff, L"Explorer", L"StartMenuLogoff", &c_Bool },
    { POLID_NoSMConfigurePrograms, L"Explorer", L"NoSMConfigurePrograms", &c_Bool },
    { POLID_NoWindowsUpdate, L"Explorer", L"NoWindowsUpdate", &c_Bool },
    { POLID_NoStartMenuSubFolders, L"Explorer", L"NoStartMenuSubFolders", &c_Bool },
    { POLID_TurnOffSidebar, L"Windows", L"TurnOffSidebar", &c_Bool },
    { POLID_NoMobilityCenter, L"MobilityCenter", L"NoMobilityCenter", &c_Bool },
};

/**************************************************************************
 * CPolicyCache
 */
class CPolicyCache
{
public:
    ~CPolicyCache()
    {
        LocalFree(m_pResults);

        if (m_hGlobalCounter)
            CloseHandle(m_hGlobalCounter);
    }

    BOOL Initialize(const SHPOLICY_ITEM *pItems, UINT cItems)
    {
        m_nCounterValue = MINLONG;
        m_pszRootKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies";
        m_pItems = pItems;
        m_cItems = cItems;
        m_pResults = (PSHPOLICY_RESULT)LocalAlloc(LPTR, cItems * sizeof(SHPOLICY_RESULT));
        if (!m_pResults)
            return FALSE;
        m_hGlobalCounter = SHGlobalCounterCreate(GUID_Restrictions);
        if (!m_hGlobalCounter)
        {
            LocalFree(m_pResults);
            m_pResults = NULL;
            return FALSE;
        }
        return TRUE;
    }

    HRESULT GetValue(_In_ REFGUID rpolid, _Out_opt_ PVOID pvValue, _Out_opt_ PDWORD pcbValue);

protected:
    LPCWSTR m_pszRootKey;
    HANDLE m_hGlobalCounter;
    LONG m_nCounterValue;
    DWORD m_cItems;
    const SHPOLICY_ITEM *m_pItems;
    PSHPOLICY_RESULT m_pResults;

    void _ValidateCachedResults()
    {
        LONG Value = SHGlobalCounterGetValue(m_hGlobalCounter);
        if (m_nCounterValue != Value)
        {
            m_nCounterValue = Value;
            ZeroMemory(m_pResults, m_cItems * sizeof(SHPOLICY_RESULT));
        }
    }

    HRESULT _GetValue(
        _In_ LPCWSTR pszSubKey,
        _In_ LPCWSTR pszValueName,
        _In_ const SHPOLICY_CONSTRAINT* pConstraint,
        _Out_opt_ PDWORD pdwType,
        _Out_opt_ PVOID pvData,
        _Inout_opt_ PDWORD pcbData);

    static void _CacheResult(
        const SHPOLICY_CONSTRAINT *pConstraint,
        PVOID pvValue,
        PDWORD pcbValue,
        PSHPOLICY_RESULT pResult);
};

HRESULT
CPolicyCache::GetValue(_In_ REFGUID rpolid, _Out_opt_ PVOID pvValue, _Out_opt_ PDWORD pcbValue)
{
    _ValidateCachedResults();

    if (m_cItems == 0)
        return E_UNEXPECTED;

    UINT iItem;
    for (iItem = 0; iItem < m_cItems; ++iItem)
    {
        if (IsEqualGUID(m_pItems[iItem].rpolid, rpolid))
            break;
    }

    if (iItem >= m_cItems)
        return E_UNEXPECTED;

    const SHPOLICY_ITEM *pItem = &m_pItems[iItem];
    PSHPOLICY_RESULT pResult = &m_pResults[iItem];

    if (pResult->state == POLICY_STATE_NOT_FOUND)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    // Use caching only when dealing with DWORD value
    if (pvValue && pcbValue && *pcbValue == sizeof(DWORD) && pResult->state == POLICY_STATE_CACHED)
    {
        *(PDWORD)pvValue = pResult->dwValue;
        return S_OK;
    }

    HRESULT hr = _GetValue(pItem->pszKeyName, pItem->pszValueName, pItem->pConstraint, NULL,
                           pvValue, pcbValue);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        pResult->state = POLICY_STATE_NOT_FOUND;
    else if (SUCCEEDED(hr) && pvValue)
        _CacheResult(pItem->pConstraint, pvValue, pcbValue, pResult);

    return hr;
}

HRESULT CPolicyCache::_GetValue(
    _In_ LPCWSTR pszSubKey,
    _In_ LPCWSTR pszValueName,
    _In_ const SHPOLICY_CONSTRAINT* pConstraint,
    _Out_opt_ PDWORD pdwType,
    _Out_opt_ PVOID pvData,
    _Inout_opt_ PDWORD pcbData)
{
    CStringW szFullKey = CStringW(m_pszRootKey) + L"\\" + pszSubKey;

    DWORD cbDataSaved = pcbData ? *pcbData : 0;
    WORD wFlags = pConstraint->wFlags;

    LSTATUS error = RegGetValueW(HKEY_LOCAL_MACHINE, szFullKey, pszValueName, wFlags, pdwType,
                                 pvData, pcbData);
    if (error == ERROR_FILE_NOT_FOUND)
    {
        if (pcbData)
            *pcbData = cbDataSaved;
        error = RegGetValueW(HKEY_CURRENT_USER, szFullKey, pszValueName, wFlags, pdwType,
                             pvData, pcbData);
    }

    if (error)
        return HRESULT_FROM_WIN32(error);

    if (!pvData)
        return S_OK;

    if (pConstraint->wFlags == SRRF_RT_DWORD && pConstraint->wMinSize == sizeof(DWORD))
    {
        DWORD dwValue = *(PDWORD)pvData;
        if (dwValue < pConstraint->dwMin || pConstraint->dwMax < dwValue)
            return E_DATATYPE_MISMATCH;
    }

    return S_OK;
}

void
CPolicyCache::_CacheResult(
    const SHPOLICY_CONSTRAINT *pConstraint,
    PVOID pvValue,
    PDWORD pcbValue,
    PSHPOLICY_RESULT pResult)
{
    UNREFERENCED_PARAMETER(pcbValue); // Reserved for future use

    // Use caching only when dealing with DWORD value
    if (pConstraint->wFlags == SRRF_RT_DWORD)
    {
        pResult->dwValue = *(PDWORD)pvValue;
        pResult->state = POLICY_STATE_CACHED;
    }
}

/***************************************************************************/

CPolicyCache* g_pPolicyCache = NULL;
CRITICAL_SECTION g_csPolicyLock;

// This function must be called under g_csPolicyLock
static BOOL SHPolicyCache_Create(VOID)
{
    if (g_pPolicyCache)
        return TRUE;

    CPolicyCache* pCache = new(std::nothrow) CPolicyCache;
    if (!pCache)
        return FALSE;

    if (!pCache->Initialize(g_PolicyItems, _countof(g_PolicyItems)))
    {
        delete pCache;
        return FALSE;
    }

    g_pPolicyCache = pCache;
    return TRUE;
}

EXTERN_C VOID SHPolicyCache_DllProcessAttach(VOID)
{
    InitializeCriticalSection(&g_csPolicyLock);
}

EXTERN_C VOID SHPolicyCache_DllProcessDetach(VOID)
{
    CPolicyCache* pCache;

    EnterCriticalSection(&g_csPolicyLock);
    pCache = g_pPolicyCache;
    g_pPolicyCache = NULL;
    LeaveCriticalSection(&g_csPolicyLock);

    delete pCache;
    DeleteCriticalSection(&g_csPolicyLock);
}

/**************************************************************************
 *  SHWindowsPolicyGetValue (SHLWAPI.560)
 *
 * https://www.geoffchappell.com/studies/windows/shell/shlwapi/api/winpolicy/getvalue.htm
 */
EXTERN_C
HRESULT WINAPI
SHWindowsPolicyGetValue(
    _In_ REFGUID rpolid,
    _Out_opt_ PVOID pvValue,
    _Out_opt_ PDWORD pcbValue)
{
    HRESULT hr = E_FAIL;

    EnterCriticalSection(&g_csPolicyLock);
    if (SHPolicyCache_Create())
        hr = g_pPolicyCache->GetValue(rpolid, pvValue, pcbValue);
    LeaveCriticalSection(&g_csPolicyLock);
    return hr;
}
