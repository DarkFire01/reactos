/*
 * PROJECT:     ReactOS Shell Account Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Property store over a local SAM user account
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define USER_PICTURE_DATA_EXT   L".dat"

typedef struct SAM_USER_PROPERTY
{
    const PROPERTYKEY *pkey;
    VARTYPE vt;
} SAM_USER_PROPERTY;

static const SAM_USER_PROPERTY s_rgUserProperties[] =
{
    { &PKEY_SAM_Name, VT_LPWSTR },
    { &PKEY_SAM_FullName, VT_LPWSTR },
    { &PKEY_SAM_AdminComment, VT_LPWSTR },
    { &PKEY_SAM_UserComment, VT_LPWSTR },
    { &PKEY_SAM_UserAccountControl, VT_UI4 },
    { &PKEY_SAM_UserPicture, VT_BLOB },
    { &PKEY_SAM_Type, VT_UI4 },
    { &PKEY_SAM_SecurityID, VT_BLOB },
    { &PKEY_SAM_ResidualID, VT_UI4 },
};

/*
 * ReactOS SAM keeps no user tile, so the picture blob lives in the per account
 * <ProgramData>\Microsoft\User Account Pictures\<user>.dat file instead.
 */
class CSamUserPropertyStore :
    public CComObjectRootEx<CComMultiThreadModel>,
    public IPropertyStore
{
public:
    CSamUserPropertyStore();
    ~CSamUserPropertyStore();

    HRESULT Initialize(
        _In_ PSID psidDomain,
        _In_ ULONG ulRid);

    /* IPropertyStore */
    STDMETHODIMP GetCount(
        _Out_ DWORD *pcProps) override;
    STDMETHODIMP GetAt(
        _In_ DWORD iProp,
        _Out_ PROPERTYKEY *pkey) override;
    STDMETHODIMP GetValue(
        _In_ REFPROPERTYKEY key,
        _Out_ PROPVARIANT *pv) override;
    STDMETHODIMP SetValue(
        _In_ REFPROPERTYKEY key,
        _In_ REFPROPVARIANT propvar) override;
    STDMETHODIMP Commit() override;

    DECLARE_NOT_AGGREGATABLE(CSamUserPropertyStore)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CSamUserPropertyStore)
        COM_INTERFACE_ENTRY_IID(IID_IPropertyStore, IPropertyStore)
    END_COM_MAP()

private:
    HRESULT _QueryUser(
        _In_ SAM_HANDLE hDomain);
    HRESULT _GetPictureDataPath(
        _Out_writes_(cchPath) PWSTR pszPath,
        _In_ UINT cchPath);
    HRESULT _GetPicture(
        _Out_ PROPVARIANT *pv);
    HRESULT _GetSecurityId(
        _Out_ PROPVARIANT *pv);

    PSID _psidDomain;
    ULONG _ulRid;
    PWSTR _pszName;
    PWSTR _pszFullName;
    PWSTR _pszAdminComment;
    PWSTR _pszUserComment;
    ULONG _ulUserAccountControl;
    BOOL _fPictureChanged;
    BYTE *_pbNewPicture;
    ULONG _cbNewPicture;
};

static
const SAM_USER_PROPERTY *
WINAPI
FindUserProperty(
    _In_ REFPROPERTYKEY key)
{
    UINT i;

    for (i = 0; i < ARRAYSIZE(s_rgUserProperties); i++)
    {
        if (IsEqualPropertyKey(key, *s_rgUserProperties[i].pkey))
            return &s_rgUserProperties[i];
    }

    return NULL;
}

/* An absent SAM string is reported as an empty string */
static
HRESULT
WINAPI
DupUnicodeString(
    _In_ const UNICODE_STRING *pString,
    _Outptr_ PWSTR *ppsz)
{
    UINT cch = pString->Buffer ? pString->Length / sizeof(WCHAR) : 0;
    PWSTR psz;

    *ppsz = NULL;

    psz = (PWSTR)CoTaskMemAlloc((cch + 1) * sizeof(WCHAR));
    if (!psz)
        return E_OUTOFMEMORY;

    if (cch)
        CopyMemory(psz, pString->Buffer, cch * sizeof(WCHAR));
    psz[cch] = UNICODE_NULL;

    *ppsz = psz;
    return S_OK;
}

static
HRESULT
WINAPI
InitBlobPropVariant(
    _In_reads_bytes_(cb) const void *pv,
    _In_ ULONG cb,
    _Out_ PROPVARIANT *ppropvar)
{
    PropVariantInit(ppropvar);

    ppropvar->blob.pBlobData = (BYTE *)CoTaskMemAlloc(cb);
    if (!ppropvar->blob.pBlobData)
        return E_OUTOFMEMORY;

    CopyMemory(ppropvar->blob.pBlobData, pv, cb);
    ppropvar->blob.cbSize = cb;
    ppropvar->vt = VT_BLOB;
    return S_OK;
}

static
HRESULT
WINAPI
InitStringPropVariant(
    _In_ PCWSTR psz,
    _Out_ PROPVARIANT *ppropvar)
{
    HRESULT hr;

    PropVariantInit(ppropvar);

    hr = SHStrDupW(psz ? psz : L"", &ppropvar->pwszVal);
    if (SUCCEEDED(hr))
        ppropvar->vt = VT_LPWSTR;

    return hr;
}

CSamUserPropertyStore::CSamUserPropertyStore()
    : _psidDomain(NULL),
      _ulRid(0),
      _pszName(NULL),
      _pszFullName(NULL),
      _pszAdminComment(NULL),
      _pszUserComment(NULL),
      _ulUserAccountControl(0),
      _fPictureChanged(FALSE),
      _pbNewPicture(NULL),
      _cbNewPicture(0)
{
}

CSamUserPropertyStore::~CSamUserPropertyStore()
{
    CoTaskMemFree(_psidDomain);
    CoTaskMemFree(_pszName);
    CoTaskMemFree(_pszFullName);
    CoTaskMemFree(_pszAdminComment);
    CoTaskMemFree(_pszUserComment);
    CoTaskMemFree(_pbNewPicture);
}

HRESULT
CSamUserPropertyStore::_QueryUser(
    _In_ SAM_HANDLE hDomain)
{
    PUSER_ALL_INFORMATION pUserAll = NULL;
    SAM_HANDLE hUser = NULL;
    NTSTATUS Status;
    HRESULT hr;

    Status = SamOpenUser(hDomain,
                         READ_CONTROL | USER_READ_GENERAL | USER_READ_PREFERENCES |
                         USER_READ_LOGON | USER_READ_ACCOUNT,
                         _ulRid,
                         &hUser);
    if (!NT_SUCCESS(Status))
        return SamHResultFromStatus(Status);

    Status = SamQueryInformationUser(hUser, UserAllInformation, (PVOID *)&pUserAll);
    hr = SamHResultFromStatus(Status);
    if (NT_SUCCESS(Status))
    {
        hr = DupUnicodeString(&pUserAll->UserName, &_pszName);
        if (SUCCEEDED(hr))
            hr = DupUnicodeString(&pUserAll->FullName, &_pszFullName);
        if (SUCCEEDED(hr))
            hr = DupUnicodeString(&pUserAll->AdminComment, &_pszAdminComment);
        if (SUCCEEDED(hr))
            hr = DupUnicodeString(&pUserAll->UserComment, &_pszUserComment);
        _ulUserAccountControl = pUserAll->UserAccountControl;

        SamFreeMemory(pUserAll);
    }

    SamCloseHandle(hUser);
    return hr;
}

HRESULT
CSamUserPropertyStore::Initialize(
    _In_ PSID psidDomain,
    _In_ ULONG ulRid)
{
    SAM_HANDLE hServer;
    SAM_HANDLE hDomain;
    HRESULT hr;

    hr = CopySidToTaskMem(psidDomain, &_psidDomain);
    if (FAILED(hr))
        return hr;

    _ulRid = ulRid;

    hr = OpenSamDomain(_psidDomain, &hServer, &hDomain);
    if (SUCCEEDED(hr))
    {
        hr = _QueryUser(hDomain);
        CloseSamDomain(hServer, hDomain);
    }

    return hr;
}

HRESULT
CSamUserPropertyStore::_GetPictureDataPath(
    _Out_writes_(cchPath) PWSTR pszPath,
    _In_ UINT cchPath)
{
    HRESULT hr;

    hr = GetUserPicturesDirectory(pszPath, cchPath);
    if (SUCCEEDED(hr))
        hr = StringCchCatW(pszPath, cchPath, L"\\");
    if (SUCCEEDED(hr))
        hr = StringCchCatW(pszPath, cchPath, _pszName);
    if (SUCCEEDED(hr))
        hr = StringCchCatW(pszPath, cchPath, USER_PICTURE_DATA_EXT);

    return hr;
}

/* No picture is not an error: the caller gets VT_EMPTY and uses the default tile */
HRESULT
CSamUserPropertyStore::_GetPicture(
    _Out_ PROPVARIANT *pv)
{
    WCHAR szPath[MAX_PATH];
    LARGE_INTEGER cbFile;
    DWORD cbRead;
    HANDLE hFile;
    BYTE *pbData;
    HRESULT hr;

    if (_fPictureChanged)
    {
        if (!_cbNewPicture)
            return S_OK;
        return InitBlobPropVariant(_pbNewPicture, _cbNewPicture, pv);
    }

    hr = _GetPictureDataPath(szPath, ARRAYSIZE(szPath));
    if (FAILED(hr))
        return S_OK;

    hFile = CreateFileW(szPath,
                        GENERIC_READ,
                        FILE_SHARE_READ,
                        NULL,
                        OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL,
                        NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return S_OK;

    if (GetFileSizeEx(hFile, &cbFile) && cbFile.HighPart == 0 && cbFile.LowPart != 0)
    {
        pbData = (BYTE *)CoTaskMemAlloc(cbFile.LowPart);
        if (pbData)
        {
            if (ReadFile(hFile, pbData, cbFile.LowPart, &cbRead, NULL) &&
                cbRead == cbFile.LowPart)
            {
                pv->blob.cbSize = cbRead;
                pv->blob.pBlobData = pbData;
                pv->vt = VT_BLOB;
                pbData = NULL;
            }
            CoTaskMemFree(pbData);
        }
        else
        {
            hr = E_OUTOFMEMORY;
        }
    }

    CloseHandle(hFile);
    return hr;
}

HRESULT
CSamUserPropertyStore::_GetSecurityId(
    _Out_ PROPVARIANT *pv)
{
    UCHAR cSubAuthorities = *GetSidSubAuthorityCount(_psidDomain);
    DWORD cbSid = GetSidLengthRequired(cSubAuthorities + 1);
    PSID psidUser;
    UCHAR i;

    psidUser = CoTaskMemAlloc(cbSid);
    if (!psidUser)
        return E_OUTOFMEMORY;

    InitializeSid(psidUser, GetSidIdentifierAuthority(_psidDomain), cSubAuthorities + 1);
    for (i = 0; i < cSubAuthorities; i++)
        *GetSidSubAuthority(psidUser, i) = *GetSidSubAuthority(_psidDomain, i);
    *GetSidSubAuthority(psidUser, cSubAuthorities) = _ulRid;

    pv->blob.cbSize = cbSid;
    pv->blob.pBlobData = (BYTE *)psidUser;
    pv->vt = VT_BLOB;
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CSamUserPropertyStore::GetCount(
    _Out_ DWORD *pcProps)
{
    if (!pcProps)
        return E_POINTER;

    *pcProps = ARRAYSIZE(s_rgUserProperties);
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CSamUserPropertyStore::GetAt(
    _In_ DWORD iProp,
    _Out_ PROPERTYKEY *pkey)
{
    if (!pkey)
        return E_POINTER;

    if (iProp >= ARRAYSIZE(s_rgUserProperties))
        return E_INVALIDARG;

    *pkey = *s_rgUserProperties[iProp].pkey;
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CSamUserPropertyStore::GetValue(
    _In_ REFPROPERTYKEY key,
    _Out_ PROPVARIANT *pv)
{
    if (!pv)
        return E_POINTER;

    PropVariantInit(pv);

    if (!FindUserProperty(key))
        return E_INVALIDARG;

    if (IsEqualPropertyKey(key, PKEY_SAM_Name))
        return InitStringPropVariant(_pszName, pv);

    if (IsEqualPropertyKey(key, PKEY_SAM_FullName))
        return InitStringPropVariant(_pszFullName, pv);

    if (IsEqualPropertyKey(key, PKEY_SAM_AdminComment))
        return InitStringPropVariant(_pszAdminComment, pv);

    if (IsEqualPropertyKey(key, PKEY_SAM_UserComment))
        return InitStringPropVariant(_pszUserComment, pv);

    if (IsEqualPropertyKey(key, PKEY_SAM_UserPicture))
        return _GetPicture(pv);

    if (IsEqualPropertyKey(key, PKEY_SAM_SecurityID))
        return _GetSecurityId(pv);

    pv->vt = VT_UI4;
    if (IsEqualPropertyKey(key, PKEY_SAM_UserAccountControl))
        pv->ulVal = _ulUserAccountControl;
    else if (IsEqualPropertyKey(key, PKEY_SAM_Type))
        pv->ulVal = SAM_ACCOUNT_TYPE_USER;
    else
        pv->ulVal = _ulRid;

    return S_OK;
}

/* Only the picture can be changed; it is written out by Commit */
HRESULT
STDMETHODCALLTYPE
CSamUserPropertyStore::SetValue(
    _In_ REFPROPERTYKEY key,
    _In_ REFPROPVARIANT propvar)
{
    BYTE *pbPicture = NULL;
    ULONG cbPicture = 0;

    if (!FindUserProperty(key))
        return E_INVALIDARG;

    if (!IsEqualPropertyKey(key, PKEY_SAM_UserPicture))
        return STG_E_ACCESSDENIED;

    if (propvar.vt == VT_BLOB && propvar.blob.cbSize)
    {
        if (!propvar.blob.pBlobData)
            return E_INVALIDARG;

        pbPicture = (BYTE *)CoTaskMemAlloc(propvar.blob.cbSize);
        if (!pbPicture)
            return E_OUTOFMEMORY;

        CopyMemory(pbPicture, propvar.blob.pBlobData, propvar.blob.cbSize);
        cbPicture = propvar.blob.cbSize;
    }
    else if (propvar.vt != VT_EMPTY && propvar.vt != VT_BLOB)
    {
        return E_INVALIDARG;
    }

    CoTaskMemFree(_pbNewPicture);
    _pbNewPicture = pbPicture;
    _cbNewPicture = cbPicture;
    _fPictureChanged = TRUE;
    return S_OK;
}

HRESULT
STDMETHODCALLTYPE
CSamUserPropertyStore::Commit()
{
    WCHAR szDir[MAX_PATH];
    WCHAR szPath[MAX_PATH];
    DWORD cbWritten;
    HANDLE hFile;
    HRESULT hr;
    int iError;

    if (!_fPictureChanged)
        return S_OK;

    hr = _GetPictureDataPath(szPath, ARRAYSIZE(szPath));
    if (FAILED(hr))
        return hr;

    if (!_cbNewPicture)
    {
        if (!DeleteFileW(szPath) && GetLastError() != ERROR_FILE_NOT_FOUND)
            return HRESULT_FROM_WIN32(GetLastError());

        _fPictureChanged = FALSE;
        return S_OK;
    }

    hr = GetUserPicturesDirectory(szDir, ARRAYSIZE(szDir));
    if (FAILED(hr))
        return hr;

    iError = SHCreateDirectoryExW(NULL, szDir, NULL);
    if (iError != ERROR_SUCCESS && iError != ERROR_ALREADY_EXISTS && iError != ERROR_FILE_EXISTS)
        return HRESULT_FROM_WIN32(iError);

    hFile = CreateFileW(szPath,
                        GENERIC_WRITE,
                        0,
                        NULL,
                        CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL,
                        NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());

    if (!WriteFile(hFile, _pbNewPicture, _cbNewPicture, &cbWritten, NULL))
        hr = HRESULT_FROM_WIN32(GetLastError());
    else if (cbWritten != _cbNewPicture)
        hr = E_FAIL;
    else
        _fPictureChanged = FALSE;

    CloseHandle(hFile);
    return hr;
}

HRESULT
WINAPI
CreateSamUserPropertyStore(
    _In_ PSID psidDomain,
    _In_ ULONG ulRid,
    _COM_Outptr_ IPropertyStore **ppps)
{
    CComObject<CSamUserPropertyStore> *pStore;
    HRESULT hr;

    *ppps = NULL;

    hr = CComObject<CSamUserPropertyStore>::CreateInstance(&pStore);
    if (FAILED(hr))
        return hr;

    pStore->AddRef();
    hr = pStore->Initialize(psidDomain, ulRid);
    if (SUCCEEDED(hr))
        hr = pStore->QueryInterface(IID_IPropertyStore, (void **)ppps);
    pStore->Release();

    return hr;
}
