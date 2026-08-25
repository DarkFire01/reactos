/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     IApplicationAssociationRegistration, the Vista way of asking
 *              and saying which application owns a file type or protocol
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

/*
 * Two registry shapes are involved, and they answer different questions.
 *
 * What is currently the default is a per-user choice, kept per association
 * type:
 *
 *   file extension  HKCU\...\Explorer\FileExts\<.ext>\UserChoice   "Progid"
 *   URL protocol    HKCU\...\Shell\Associations\UrlAssociations\
 *                            <protocol>\UserChoice                 "ProgId"
 *
 * with the machine-wide answer under HKEY_CLASSES_ROOT as the fallback. Note
 * the two spellings of the value name - Windows really does use "Progid" for
 * extensions and "ProgId" for protocols, so read both either way rather than
 * trust one.
 *
 * What an application is *able* to be the default for is separate, and is
 * what lets a caller name an application rather than a ProgID:
 *
 *   HKLM\SOFTWARE\RegisteredApplications      "<AppRegistryName>" = <path>
 *   HKLM\<path>\FileAssociations              ".ext"     = <ProgID>
 *   HKLM\<path>\URLAssociations               "protocol" = <ProgID>
 *
 * where <path> points at the application's Capabilities key. An application
 * that has not registered there cannot be made the default, which is why the
 * Set methods take the registry name rather than a ProgID.
 */

static const WCHAR s_szRegisteredApplications[] =
    L"SOFTWARE\\RegisteredApplications";
static const WCHAR s_szFileExts[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts";
static const WCHAR s_szUrlAssociations[] =
    L"SOFTWARE\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations";
static const WCHAR s_szClients[] = L"SOFTWARE\\Clients";
static const WCHAR s_szMimeDatabase[] = L"MIME\\Database\\Content Type";

static HRESULT
RegQueryStringValue(
    _In_ HKEY hKey,
    _In_opt_ LPCWSTR pszSubKey,
    _In_opt_ LPCWSTR pszValue,
    _Out_ CStringW &strResult)
{
    WCHAR szBuffer[MAX_PATH];
    DWORD cbBuffer = sizeof(szBuffer);
    DWORD dwType = 0;
    LSTATUS Status;

    strResult.Empty();

    Status = SHGetValueW(hKey, pszSubKey, pszValue, &dwType, szBuffer, &cbBuffer);
    if (Status != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(Status);

    if (dwType != REG_SZ && dwType != REG_EXPAND_SZ)
        return E_UNEXPECTED;

    /* SHGetValueW does not promise a terminator on a REG_SZ that was stored
       without one */
    szBuffer[_countof(szBuffer) - 1] = UNICODE_NULL;
    if (szBuffer[0] == UNICODE_NULL)
        return E_FAIL;

    strResult = szBuffer;
    return S_OK;
}

/* The UserChoice key for one association, or an empty string for a type that
   has no per-user choice */
static CStringW
GetUserChoiceKey(_In_ ASSOCIATIONTYPE atType, _In_ LPCWSTR pszQuery)
{
    CStringW strKey;

    switch (atType)
    {
        case AT_FILEEXTENSION:
            strKey.Format(L"%s\\%s\\UserChoice", s_szFileExts, pszQuery);
            break;

        case AT_URLPROTOCOL:
            strKey.Format(L"%s\\%s\\UserChoice", s_szUrlAssociations, pszQuery);
            break;

        default:
            break;
    }

    return strKey;
}

static HRESULT
QueryUserChoice(
    _In_ ASSOCIATIONTYPE atType,
    _In_ LPCWSTR pszQuery,
    _Out_ CStringW &strProgId)
{
    CStringW strKey = GetUserChoiceKey(atType, pszQuery);
    HRESULT hr;

    strProgId.Empty();
    if (strKey.IsEmpty())
        return E_FAIL;

    /* Both spellings, because the two association types disagree on it */
    hr = RegQueryStringValue(HKEY_CURRENT_USER, strKey, L"ProgId", strProgId);
    if (FAILED(hr))
        hr = RegQueryStringValue(HKEY_CURRENT_USER, strKey, L"Progid", strProgId);

    return hr;
}

/* The machine-wide default, which is what applies with no user choice made */
static HRESULT
QueryMachineDefault(
    _In_ ASSOCIATIONTYPE atType,
    _In_ LPCWSTR pszQuery,
    _Out_ CStringW &strProgId)
{
    CStringW strKey;

    strProgId.Empty();

    switch (atType)
    {
        case AT_FILEEXTENSION:
            /* HKCR\.ext defaults to the ProgID that handles it */
            return RegQueryStringValue(HKEY_CLASSES_ROOT, pszQuery, NULL, strProgId);

        case AT_URLPROTOCOL:
        {
            /* A protocol key is its own ProgID - HKCR\http is both - so the
               only question is whether one is registered at all. The empty
               "URL Protocol" value is what marks a key as one. */
            HKEY hKey;

            if (RegOpenKeyExW(HKEY_CLASSES_ROOT, pszQuery, 0, KEY_READ,
                              &hKey) != ERROR_SUCCESS)
            {
                return E_FAIL;
            }

            if (RegQueryValueExW(hKey, L"URL Protocol", NULL, NULL, NULL,
                                 NULL) != ERROR_SUCCESS)
            {
                RegCloseKey(hKey);
                return E_FAIL;
            }

            RegCloseKey(hKey);
            strProgId = pszQuery;
            return S_OK;
        }

        case AT_STARTMENUCLIENT:
            /* HKLM\SOFTWARE\Clients\<type> names the default client */
            strKey.Format(L"%s\\%s", s_szClients, pszQuery);
            return RegQueryStringValue(HKEY_LOCAL_MACHINE, strKey, NULL, strProgId);

        case AT_MIMETYPE:
            /* A MIME type names an extension, which then names the ProgID */
            strKey.Format(L"%s\\%s", s_szMimeDatabase, pszQuery);
            if (FAILED(RegQueryStringValue(HKEY_CLASSES_ROOT, strKey,
                                           L"Extension", strProgId)))
            {
                return E_FAIL;
            }
            return RegQueryStringValue(HKEY_CLASSES_ROOT, strProgId, NULL, strProgId);

        default:
            return E_INVALIDARG;
    }
}

/* What ProgID an application registered for one extension or protocol. An
   application with no Capabilities entry has not claimed the association and
   cannot be made its default. */
static HRESULT
QueryAppAssociation(
    _In_ LPCWSTR pszAppRegistryName,
    _In_ ASSOCIATIONTYPE atType,
    _In_ LPCWSTR pszQuery,
    _Out_ CStringW &strProgId)
{
    CStringW strCapabilities, strKey;
    HRESULT hr;

    strProgId.Empty();

    hr = RegQueryStringValue(HKEY_LOCAL_MACHINE, s_szRegisteredApplications,
                             pszAppRegistryName, strCapabilities);
    if (FAILED(hr))
        return hr;

    switch (atType)
    {
        case AT_FILEEXTENSION:
            strKey.Format(L"%s\\FileAssociations", strCapabilities.GetString());
            break;

        case AT_URLPROTOCOL:
            strKey.Format(L"%s\\URLAssociations", strCapabilities.GetString());
            break;

        case AT_STARTMENUCLIENT:
            strKey.Format(L"%s\\StartMenu", strCapabilities.GetString());
            break;

        default:
            return E_INVALIDARG;
    }

    return RegQueryStringValue(HKEY_LOCAL_MACHINE, strKey, pszQuery, strProgId);
}

/* Not a CComCoClass: Windows exposes this through a coclass as well, but
   nothing here registers CLSID_ApplicationAssociationRegistration, so the
   object is reachable only through SHCreateAssociationRegistration below. */
class CApplicationAssociationRegistration :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IApplicationAssociationRegistration
{
public:
    // *** IApplicationAssociationRegistration methods ***
    STDMETHOD(QueryCurrentDefault)(
        _In_ LPCWSTR pszQuery,
        _In_ ASSOCIATIONTYPE atQueryType,
        _In_ ASSOCIATIONLEVEL alQueryLevel,
        _Outptr_ LPWSTR *ppszAssociation) override;

    STDMETHOD(QueryAppIsDefault)(
        _In_ LPCWSTR pszQuery,
        _In_ ASSOCIATIONTYPE atQueryType,
        _In_ ASSOCIATIONLEVEL alQueryLevel,
        _In_ LPCWSTR pszAppRegistryName,
        _Out_ BOOL *pfDefault) override;

    STDMETHOD(QueryAppIsDefaultAll)(
        _In_ ASSOCIATIONLEVEL alQueryLevel,
        _In_ LPCWSTR pszAppRegistryName,
        _Out_ BOOL *pfDefault) override;

    STDMETHOD(SetAppAsDefault)(
        _In_ LPCWSTR pszAppRegistryName,
        _In_ LPCWSTR pszSet,
        _In_ ASSOCIATIONTYPE atSetType) override;

    STDMETHOD(SetAppAsDefaultAll)(_In_ LPCWSTR pszAppRegistryName) override;

    STDMETHOD(ClearUserAssociations)() override;

private:
    HRESULT QueryDefaultProgId(
        _In_ LPCWSTR pszQuery,
        _In_ ASSOCIATIONTYPE atQueryType,
        _In_ ASSOCIATIONLEVEL alQueryLevel,
        _Out_ CStringW &strProgId);

    HRESULT SetOneAssociation(
        _In_ LPCWSTR pszAppRegistryName,
        _In_ ASSOCIATIONTYPE atSetType,
        _In_ LPCWSTR pszQuery,
        _In_ LPCWSTR pszProgId);

    HRESULT ForEachAppAssociation(
        _In_ LPCWSTR pszAppRegistryName,
        _In_ ASSOCIATIONTYPE atType,
        _In_ BOOL bSet,
        _Out_opt_ BOOL *pfAllDefault);

DECLARE_NOT_AGGREGATABLE(CApplicationAssociationRegistration)
DECLARE_PROTECT_FINAL_CONSTRUCT()

BEGIN_COM_MAP(CApplicationAssociationRegistration)
    COM_INTERFACE_ENTRY_IID(IID_IApplicationAssociationRegistration,
                            IApplicationAssociationRegistration)
END_COM_MAP()
};

HRESULT
CApplicationAssociationRegistration::QueryDefaultProgId(
    _In_ LPCWSTR pszQuery,
    _In_ ASSOCIATIONTYPE atQueryType,
    _In_ ASSOCIATIONLEVEL alQueryLevel,
    _Out_ CStringW &strProgId)
{
    HRESULT hr;

    strProgId.Empty();

    /* AL_EFFECTIVE is what a caller almost always wants: the user's choice if
       there is one, and the machine default if there is not */
    if (alQueryLevel == AL_USER || alQueryLevel == AL_EFFECTIVE)
    {
        hr = QueryUserChoice(atQueryType, pszQuery, strProgId);
        if (SUCCEEDED(hr))
            return hr;

        if (alQueryLevel == AL_USER)
            return hr;
    }

    return QueryMachineDefault(atQueryType, pszQuery, strProgId);
}

STDMETHODIMP
CApplicationAssociationRegistration::QueryCurrentDefault(
    _In_ LPCWSTR pszQuery,
    _In_ ASSOCIATIONTYPE atQueryType,
    _In_ ASSOCIATIONLEVEL alQueryLevel,
    _Outptr_ LPWSTR *ppszAssociation)
{
    CStringW strProgId;
    HRESULT hr;
    SIZE_T cb;

    TRACE("(%p, %s, %d, %d, %p)\n", this, debugstr_w(pszQuery), atQueryType,
          alQueryLevel, ppszAssociation);

    if (!ppszAssociation)
        return E_INVALIDARG;

    *ppszAssociation = NULL;

    if (!pszQuery || !*pszQuery)
        return E_INVALIDARG;

    hr = QueryDefaultProgId(pszQuery, atQueryType, alQueryLevel, strProgId);
    if (FAILED(hr))
        return hr;

    cb = (strProgId.GetLength() + 1) * sizeof(WCHAR);
    *ppszAssociation = static_cast<LPWSTR>(CoTaskMemAlloc(cb));
    if (!*ppszAssociation)
        return E_OUTOFMEMORY;

    CopyMemory(*ppszAssociation, strProgId.GetString(), cb);
    return S_OK;
}

STDMETHODIMP
CApplicationAssociationRegistration::QueryAppIsDefault(
    _In_ LPCWSTR pszQuery,
    _In_ ASSOCIATIONTYPE atQueryType,
    _In_ ASSOCIATIONLEVEL alQueryLevel,
    _In_ LPCWSTR pszAppRegistryName,
    _Out_ BOOL *pfDefault)
{
    CStringW strCurrent, strApp;
    HRESULT hr;

    TRACE("(%p, %s, %d, %d, %s, %p)\n", this, debugstr_w(pszQuery), atQueryType,
          alQueryLevel, debugstr_w(pszAppRegistryName), pfDefault);

    if (!pfDefault)
        return E_INVALIDARG;

    *pfDefault = FALSE;

    if (!pszQuery || !*pszQuery || !pszAppRegistryName || !*pszAppRegistryName)
        return E_INVALIDARG;

    hr = QueryAppAssociation(pszAppRegistryName, atQueryType, pszQuery, strApp);
    if (FAILED(hr))
    {
        /* The application never claimed this association, so it is not the
           default for it - which is an answer, not a failure */
        return S_OK;
    }

    hr = QueryDefaultProgId(pszQuery, atQueryType, alQueryLevel, strCurrent);
    if (FAILED(hr))
        return S_OK;

    *pfDefault = (strCurrent.CompareNoCase(strApp) == 0);
    return S_OK;
}

/*
 * Walk every association an application registered for one type, either
 * testing each against the current default or making the application the
 * default for each.
 */
HRESULT
CApplicationAssociationRegistration::ForEachAppAssociation(
    _In_ LPCWSTR pszAppRegistryName,
    _In_ ASSOCIATIONTYPE atType,
    _In_ BOOL bSet,
    _Out_opt_ BOOL *pfAllDefault)
{
    CStringW strCapabilities, strKey;
    HKEY hKey;
    DWORD dwIndex;
    HRESULT hr;
    LSTATUS Status;

    hr = RegQueryStringValue(HKEY_LOCAL_MACHINE, s_szRegisteredApplications,
                             pszAppRegistryName, strCapabilities);
    if (FAILED(hr))
        return hr;

    switch (atType)
    {
        case AT_FILEEXTENSION:
            strKey.Format(L"%s\\FileAssociations", strCapabilities.GetString());
            break;

        case AT_URLPROTOCOL:
            strKey.Format(L"%s\\URLAssociations", strCapabilities.GetString());
            break;

        default:
            return E_INVALIDARG;
    }

    Status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, strKey, 0, KEY_READ, &hKey);
    if (Status != ERROR_SUCCESS)
    {
        /* Nothing registered for this type. An application that registers no
           file associations is still the default for all of them, vacuously,
           so leave pfAllDefault as the caller set it. */
        return S_FALSE;
    }

    for (dwIndex = 0;; ++dwIndex)
    {
        WCHAR szName[MAX_PATH], szProgId[MAX_PATH];
        DWORD cchName = _countof(szName);
        DWORD cbProgId = sizeof(szProgId);
        DWORD dwType = 0;

        Status = RegEnumValueW(hKey, dwIndex, szName, &cchName, NULL, &dwType,
                               reinterpret_cast<LPBYTE>(szProgId), &cbProgId);
        if (Status != ERROR_SUCCESS)
            break;

        if (dwType != REG_SZ || szName[0] == UNICODE_NULL)
            continue;

        szProgId[_countof(szProgId) - 1] = UNICODE_NULL;

        if (bSet)
        {
            hr = SetOneAssociation(pszAppRegistryName, atType, szName, szProgId);
            if (FAILED(hr))
            {
                RegCloseKey(hKey);
                return hr;
            }
        }
        else
        {
            CStringW strCurrent;

            hr = QueryDefaultProgId(szName, atType, AL_EFFECTIVE, strCurrent);
            if (FAILED(hr) || strCurrent.CompareNoCase(szProgId) != 0)
            {
                if (pfAllDefault)
                    *pfAllDefault = FALSE;
                break;
            }
        }
    }

    RegCloseKey(hKey);
    return S_OK;
}

STDMETHODIMP
CApplicationAssociationRegistration::QueryAppIsDefaultAll(
    _In_ ASSOCIATIONLEVEL alQueryLevel,
    _In_ LPCWSTR pszAppRegistryName,
    _Out_ BOOL *pfDefault)
{
    HRESULT hr;

    TRACE("(%p, %d, %s, %p)\n", this, alQueryLevel,
          debugstr_w(pszAppRegistryName), pfDefault);

    if (!pfDefault)
        return E_INVALIDARG;

    *pfDefault = FALSE;

    if (!pszAppRegistryName || !*pszAppRegistryName)
        return E_INVALIDARG;

    /* Every association the application claims has to be its own for the
       answer to be yes, so start from yes and let either walk say otherwise */
    *pfDefault = TRUE;

    hr = ForEachAppAssociation(pszAppRegistryName, AT_FILEEXTENSION, FALSE,
                               pfDefault);
    if (FAILED(hr))
    {
        *pfDefault = FALSE;
        return hr;
    }

    if (*pfDefault)
    {
        hr = ForEachAppAssociation(pszAppRegistryName, AT_URLPROTOCOL, FALSE,
                                   pfDefault);
        if (FAILED(hr))
        {
            *pfDefault = FALSE;
            return hr;
        }
    }

    return S_OK;
}

HRESULT
CApplicationAssociationRegistration::SetOneAssociation(
    _In_ LPCWSTR pszAppRegistryName,
    _In_ ASSOCIATIONTYPE atSetType,
    _In_ LPCWSTR pszQuery,
    _In_ LPCWSTR pszProgId)
{
    CStringW strKey = GetUserChoiceKey(atSetType, pszQuery);
    LPCWSTR pszValue;
    LSTATUS Status;

    UNREFERENCED_PARAMETER(pszAppRegistryName);

    if (strKey.IsEmpty())
        return E_INVALIDARG;

    /* The two association types spell the value name differently */
    pszValue = (atSetType == AT_FILEEXTENSION) ? L"Progid" : L"ProgId";

    Status = SHSetValueW(HKEY_CURRENT_USER, strKey, pszValue, REG_SZ, pszProgId,
                         (lstrlenW(pszProgId) + 1) * sizeof(WCHAR));
    if (Status != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(Status);

    return S_OK;
}

STDMETHODIMP
CApplicationAssociationRegistration::SetAppAsDefault(
    _In_ LPCWSTR pszAppRegistryName,
    _In_ LPCWSTR pszSet,
    _In_ ASSOCIATIONTYPE atSetType)
{
    CStringW strProgId;
    HRESULT hr;

    TRACE("(%p, %s, %s, %d)\n", this, debugstr_w(pszAppRegistryName),
          debugstr_w(pszSet), atSetType);

    if (!pszAppRegistryName || !*pszAppRegistryName || !pszSet || !*pszSet)
        return E_INVALIDARG;

    /* Only an association the application registered may be handed to it */
    hr = QueryAppAssociation(pszAppRegistryName, atSetType, pszSet, strProgId);
    if (FAILED(hr))
        return hr;

    return SetOneAssociation(pszAppRegistryName, atSetType, pszSet, strProgId);
}

STDMETHODIMP
CApplicationAssociationRegistration::SetAppAsDefaultAll(
    _In_ LPCWSTR pszAppRegistryName)
{
    HRESULT hr;

    TRACE("(%p, %s)\n", this, debugstr_w(pszAppRegistryName));

    if (!pszAppRegistryName || !*pszAppRegistryName)
        return E_INVALIDARG;

    hr = ForEachAppAssociation(pszAppRegistryName, AT_FILEEXTENSION, TRUE, NULL);
    if (FAILED(hr))
        return hr;

    return ForEachAppAssociation(pszAppRegistryName, AT_URLPROTOCOL, TRUE, NULL);
}

STDMETHODIMP
CApplicationAssociationRegistration::ClearUserAssociations()
{
    TRACE("(%p)\n", this);

    /* Dropping every user choice leaves the machine-wide defaults in force,
       which is what this is for */
    SHDeleteKeyW(HKEY_CURRENT_USER, s_szUrlAssociations);
    SHDeleteKeyW(HKEY_CURRENT_USER, s_szFileExts);

    return S_OK;
}

/*************************************************************************
 * SHCreateAssociationRegistration      [SHELL32.@]
 */
EXTERN_C HRESULT WINAPI
SHCreateAssociationRegistration(_In_ REFIID riid, _Outptr_ void **ppv)
{
    TRACE("(%s, %p)\n", debugstr_guid(&riid), ppv);

    if (!ppv)
        return E_INVALIDARG;

    *ppv = NULL;

    return ShellObjectCreator<CApplicationAssociationRegistration>(riid, ppv);
}
