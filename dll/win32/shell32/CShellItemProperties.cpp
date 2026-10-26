/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Property stores and property lists of shell items
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include "prop.h"
#include <propvarutil.h>
#include "CShellItemProperties.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

/* Keys the item stores fill in that shell32's headers do not carry */
#define FMTID_SHELLITEM_STORAGE \
    { 0xb725f130, 0x47ef, 0x101a, { 0xa5, 0xf1, 0x02, 0x60, 0x8c, 0x9e, 0xeb, 0xac } }
#define FMTID_SHELLITEM_QUERY \
    { 0x28636aa6, 0x953d, 0x11d2, { 0xb5, 0xd6, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0xd0 } }
#define FMTID_SHELLITEM_PATH \
    { 0xe3e0584c, 0xb788, 0x4a5a, { 0xbb, 0x20, 0x7f, 0x5a, 0x44, 0xc9, 0xac, 0xdd } }
#define FMTID_SHELLITEM_VOLUME \
    { 0x9b174b35, 0x40ff, 0x11d2, { 0xa2, 0x7e, 0x00, 0xc0, 0x4f, 0xc3, 0x08, 0x71 } }
#define FMTID_SHELLITEM_PROPLIST \
    { 0xc9944a21, 0xa406, 0x48fe, { 0x82, 0x25, 0xae, 0xc7, 0xe2, 0x4c, 0x21, 0x1b } }

static const PROPERTYKEY c_pkItemNameDisplay = { FMTID_SHELLITEM_STORAGE, 10 };
static const PROPERTYKEY c_pkItemFolderNameDisplay = { FMTID_SHELLITEM_STORAGE, 2 };
static const PROPERTYKEY c_pkNameWithoutExtension = { FMTID_SHELLITEM_STORAGE, 24 };
static const PROPERTYKEY c_pkDateCreated = { FMTID_SHELLITEM_STORAGE, 15 };
static const PROPERTYKEY c_pkDateAccessed = { FMTID_SHELLITEM_STORAGE, 16 };
static const PROPERTYKEY c_pkItemType = { FMTID_SHELLITEM_QUERY, 11 };
static const PROPERTYKEY c_pkPerceivedType = { FMTID_SHELLITEM_QUERY, 9 };
static const PROPERTYKEY c_pkParsingName = { FMTID_SHELLITEM_QUERY, 24 };
static const PROPERTYKEY c_pkParsingPath = { FMTID_SHELLITEM_QUERY, 30 };
static const PROPERTYKEY c_pkItemFolderPathDisplay = { FMTID_SHELLITEM_PATH, 6 };
static const PROPERTYKEY c_pkItemPathDisplay = { FMTID_SHELLITEM_PATH, 7 };
static const PROPERTYKEY c_pkFileName =
    { { 0x41cf5ae0, 0xf75a, 0x4806, { 0xbd, 0x87, 0x59, 0xc7, 0xd9, 0x24, 0x8e, 0xb9 } }, 100 };
static const PROPERTYKEY c_pkFileExtension =
    { { 0xe4f10a3c, 0x49e6, 0x405d, { 0x82, 0x88, 0xa2, 0x3b, 0xd4, 0xee, 0xaa, 0x6c } }, 100 };
static const PROPERTYKEY c_pkKind =
    { { 0x1e3ee840, 0xbc2b, 0x476c, { 0x82, 0x37, 0x2a, 0xcd, 0x1a, 0x83, 0x9b, 0x22 } }, 3 };
static const PROPERTYKEY c_pkIsFolder =
    { { 0x09329b74, 0x40a3, 0x4c68, { 0xbf, 0x07, 0xaf, 0x9a, 0x57, 0x2f, 0x60, 0x7c } }, 100 };
static const PROPERTYKEY c_pkFreeSpace = { FMTID_SHELLITEM_VOLUME, 2 };
static const PROPERTYKEY c_pkCapacity = { FMTID_SHELLITEM_VOLUME, 3 };
static const PROPERTYKEY c_pkFileSystem = { FMTID_SHELLITEM_VOLUME, 4 };
static const PROPERTYKEY c_pkPercentFull = { FMTID_SHELLITEM_VOLUME, 5 };
static const PROPERTYKEY c_pkDecoratedFreeSpace = { FMTID_SHELLITEM_VOLUME, 7 };
static const PROPERTYKEY c_pkVolumeIsRoot = { FMTID_SHELLITEM_VOLUME, 10 };

/* Property lists an item reads from its file association, by registry value name */
static const struct
{
    PROPERTYKEY key;
    PCWSTR pszValue;
} c_rgPropLists[] =
{
    { { FMTID_SHELLITEM_PROPLIST, 2 }, L"FullDetails" },
    { { FMTID_SHELLITEM_PROPLIST, 3 }, L"TileInfo" },
    { { FMTID_SHELLITEM_PROPLIST, 4 }, L"InfoTip" },
    { { FMTID_SHELLITEM_PROPLIST, 5 }, L"QuickTip" },
    { { FMTID_SHELLITEM_PROPLIST, 6 }, L"PreviewTitle" },
    { { FMTID_SHELLITEM_PROPLIST, 8 }, L"PreviewDetails" },
    { { FMTID_SHELLITEM_PROPLIST, 9 }, L"ExtendedTileInfo" },
    { { FMTID_SHELLITEM_PROPLIST, 11 }, L"ConflictPrompt" },
    { { FMTID_SHELLITEM_PROPLIST, 13 }, L"ContentViewModeForBrowse" },
    { { FMTID_SHELLITEM_PROPLIST, 14 }, L"ContentViewModeForSearch" },
};

/* The kind a perceived type maps to when the type has no KindMap entry */
static const struct
{
    PERCEIVED ptype;
    PCWSTR pszKind;
} c_rgPerceivedKinds[] =
{
    { PERCEIVED_TYPE_IMAGE, L"picture" },
    { PERCEIVED_TYPE_AUDIO, L"music" },
    { PERCEIVED_TYPE_VIDEO, L"video" },
    { PERCEIVED_TYPE_TEXT, L"document" },
    { PERCEIVED_TYPE_DOCUMENT, L"document" },
    { PERCEIVED_TYPE_APPLICATION, L"program" },
};

static inline BOOL
SHELL_IsSameKey(
    _In_ REFPROPERTYKEY key1,
    _In_ REFPROPERTYKEY key2)
{
    return key1.pid == key2.pid && IsEqualGUID(key1.fmtid, key2.fmtid);
}

static inline BOOL
IsEmptyPropVariant(
    _In_ REFPROPVARIANT propvar)
{
    return propvar.vt == VT_EMPTY || propvar.vt == VT_NULL;
}

/* Read-only store of one item: file system data, names, type and association lists */
class CItemPropertyStore :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IPropertyStore,
    public IPropertyStoreCapabilities
{
    struct ENTRY
    {
        PROPERTYKEY key;
        PROPVARIANT value;
    };

    CAtlArray<ENTRY> m_entries;
    CComAutoCriticalSection m_cs;
    CComPtr<IShellFolder2> m_psf2;
    LPITEMIDLIST m_pidlChild;
    BOOL m_fFolderFallback;

    int _Find(_In_ REFPROPERTYKEY key);
    VOID _Take(_In_ REFPROPERTYKEY key, _Inout_ PROPVARIANT *ppropvar);
    VOID _AddString(_In_ REFPROPERTYKEY key, _In_opt_ PCWSTR psz);
    VOID _AddFileTime(_In_ REFPROPERTYKEY key, _In_ const FILETIME *pft);
    VOID _AddUInt64(_In_ REFPROPERTYKEY key, _In_ ULONGLONG ull);
    VOID _AddUInt32(_In_ REFPROPERTYKEY key, _In_ ULONG ul);
    VOID _AddInt32(_In_ REFPROPERTYKEY key, _In_ LONG l);
    VOID _AddBool(_In_ REFPROPERTYKEY key, _In_ BOOL f);
    VOID _AddFromFolder(_In_ REFPROPERTYKEY key);
    VOID _AddNames(_In_ IShellFolder *psf, _In_ PCUITEMID_CHILD pidlChild, _In_ PCIDLIST_ABSOLUTE pidl);
    VOID _AddFileSystemData(_In_ PCIDLIST_ABSOLUTE pidl, _In_ const WIN32_FIND_DATAW *pfd, _In_ PCWSTR pszPath);
    VOID _AddDriveData(_In_ PCWSTR pszRoot);
    VOID _AddPropLists(_In_ PCWSTR pszExtension, _In_ BOOL fDirectory, _In_ BOOL fDrive, _In_ BOOL fFolder);

public:
    CItemPropertyStore();
    ~CItemPropertyStore();

    HRESULT Initialize(
        _In_ PCIDLIST_ABSOLUTE pidl,
        _In_ GETPROPERTYSTOREFLAGS flags,
        _In_reads_opt_(cKeys) const PROPERTYKEY *rgKeys,
        _In_ UINT cKeys);

    // IPropertyStore
    STDMETHODIMP GetCount(_Out_ DWORD *cProps) override;
    STDMETHODIMP GetAt(_In_ DWORD iProp, _Out_ PROPERTYKEY *pkey) override;
    STDMETHODIMP GetValue(_In_ REFPROPERTYKEY key, _Out_ PROPVARIANT *pv) override;
    STDMETHODIMP SetValue(_In_ REFPROPERTYKEY key, _In_ REFPROPVARIANT propvar) override;
    STDMETHODIMP Commit() override;

    // IPropertyStoreCapabilities
    STDMETHODIMP IsPropertyWritable(_In_ REFPROPERTYKEY key) override;

    DECLARE_NOT_AGGREGATABLE(CItemPropertyStore)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CItemPropertyStore)
        COM_INTERFACE_ENTRY_IID(IID_IPropertyStore, IPropertyStore)
        COM_INTERFACE_ENTRY_IID(IID_IPropertyStoreCapabilities, IPropertyStoreCapabilities)
    END_COM_MAP()
};

CItemPropertyStore::CItemPropertyStore() :
    m_pidlChild(NULL),
    m_fFolderFallback(FALSE)
{
}

CItemPropertyStore::~CItemPropertyStore()
{
    for (SIZE_T i = 0; i < m_entries.GetCount(); i++)
        PropVariantClear(&m_entries[i].value);
    ILFree(m_pidlChild);
}

int
CItemPropertyStore::_Find(
    _In_ REFPROPERTYKEY key)
{
    for (SIZE_T i = 0; i < m_entries.GetCount(); i++)
    {
        if (SHELL_IsSameKey(m_entries[i].key, key))
            return (int)i;
    }
    return -1;
}

/* Takes ownership of the value; a key already present keeps its first value */
VOID
CItemPropertyStore::_Take(
    _In_ REFPROPERTYKEY key,
    _Inout_ PROPVARIANT *ppropvar)
{
    ENTRY entry;

    if (_Find(key) >= 0)
    {
        PropVariantClear(ppropvar);
        return;
    }

    entry.key = key;
    entry.value = *ppropvar;
    if (m_entries.Add(entry) == (SIZE_T)-1)
        PropVariantClear(ppropvar);
    PropVariantInit(ppropvar);
}

VOID
CItemPropertyStore::_AddString(
    _In_ REFPROPERTYKEY key,
    _In_opt_ PCWSTR psz)
{
    PROPVARIANT propvar;

    if (!psz || !*psz)
        return;
    if (SUCCEEDED(InitPropVariantFromString(psz, &propvar)))
        _Take(key, &propvar);
}

VOID
CItemPropertyStore::_AddFileTime(
    _In_ REFPROPERTYKEY key,
    _In_ const FILETIME *pft)
{
    PROPVARIANT propvar;

    if (!pft->dwLowDateTime && !pft->dwHighDateTime)
        return;
    PropVariantInit(&propvar);
    propvar.vt = VT_FILETIME;
    propvar.filetime = *pft;
    _Take(key, &propvar);
}

VOID
CItemPropertyStore::_AddUInt64(
    _In_ REFPROPERTYKEY key,
    _In_ ULONGLONG ull)
{
    PROPVARIANT propvar;

    PropVariantInit(&propvar);
    propvar.vt = VT_UI8;
    propvar.uhVal.QuadPart = ull;
    _Take(key, &propvar);
}

VOID
CItemPropertyStore::_AddUInt32(
    _In_ REFPROPERTYKEY key,
    _In_ ULONG ul)
{
    PROPVARIANT propvar;

    PropVariantInit(&propvar);
    propvar.vt = VT_UI4;
    propvar.ulVal = ul;
    _Take(key, &propvar);
}

VOID
CItemPropertyStore::_AddInt32(
    _In_ REFPROPERTYKEY key,
    _In_ LONG l)
{
    PROPVARIANT propvar;

    PropVariantInit(&propvar);
    propvar.vt = VT_I4;
    propvar.lVal = l;
    _Take(key, &propvar);
}

VOID
CItemPropertyStore::_AddBool(
    _In_ REFPROPERTYKEY key,
    _In_ BOOL f)
{
    PROPVARIANT propvar;

    PropVariantInit(&propvar);
    propvar.vt = VT_BOOL;
    propvar.boolVal = f ? VARIANT_TRUE : VARIANT_FALSE;
    _Take(key, &propvar);
}

/* Anything the store does not compute itself comes from the parent folder's details */
VOID
CItemPropertyStore::_AddFromFolder(
    _In_ REFPROPERTYKEY key)
{
    PROPVARIANT propvar;
    VARIANT var;

    PropVariantInit(&propvar);
    if (m_psf2 && m_fFolderFallback)
    {
        VariantInit(&var);
        if (SUCCEEDED(m_psf2->GetDetailsEx(m_pidlChild, (const SHCOLUMNID *)&key, &var)))
        {
            if (FAILED(SHELL_VariantToPropVariant(&var, &propvar)))
                PropVariantInit(&propvar);
            VariantClear(&var);
        }
    }

    /* Remember misses too, so the folder is asked once per key */
    {
        ENTRY entry;

        entry.key = key;
        entry.value = propvar;
        if (m_entries.Add(entry) == (SIZE_T)-1)
            PropVariantClear(&propvar);
    }
}

VOID
CItemPropertyStore::_AddNames(
    _In_ IShellFolder *psf,
    _In_ PCUITEMID_CHILD pidlChild,
    _In_ PCIDLIST_ABSOLUTE pidl)
{
    WCHAR szName[MAX_PATH];
    SHFILEINFOW sfi;
    STRRET str;
    PWSTR psz;

    if (SUCCEEDED(psf->GetDisplayNameOf(pidlChild, SHGDN_INFOLDER, &str)) &&
        SUCCEEDED(StrRetToBufW(&str, pidlChild, szName, _countof(szName))))
    {
        _AddString(c_pkItemNameDisplay, szName);
    }

    if (SUCCEEDED(psf->GetDisplayNameOf(pidlChild, SHGDN_INFOLDER | SHGDN_FORPARSING, &str)) &&
        SUCCEEDED(StrRetToBufW(&str, pidlChild, szName, _countof(szName))))
    {
        _AddString(c_pkParsingName, szName);
    }

    if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &psz)))
    {
        _AddString(c_pkParsingPath, psz);
        CoTaskMemFree(psz);
    }

    ZeroMemory(&sfi, sizeof(sfi));
    if (SHGetFileInfoW((PCWSTR)pidl, 0, &sfi, sizeof(sfi), SHGFI_PIDL | SHGFI_TYPENAME))
        _AddString(PKEY_ItemTypeText, sfi.szTypeName);
}

VOID
CItemPropertyStore::_AddFileSystemData(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_ const WIN32_FIND_DATAW *pfd,
    _In_ PCWSTR pszPath)
{
    BOOL fDirectory = (pfd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    PCWSTR pszExtension = PathFindExtensionW(pfd->cFileName);
    WCHAR szBuffer[MAX_PATH];
    PERCEIVED ptype = PERCEIVED_TYPE_UNSPECIFIED;
    PCWSTR pszKind = NULL;

    _AddFileTime(PKEY_DateModified, &pfd->ftLastWriteTime);
    _AddFileTime(c_pkDateCreated, &pfd->ftCreationTime);
    _AddFileTime(c_pkDateAccessed, &pfd->ftLastAccessTime);
    _AddUInt32(PKEY_FileAttributes, pfd->dwFileAttributes);
    if (!fDirectory)
        _AddUInt64(PKEY_Size, ((ULONGLONG)pfd->nFileSizeHigh << 32) | pfd->nFileSizeLow);

    _AddString(c_pkFileName, pfd->cFileName);
    StringCchCopyW(szBuffer, _countof(szBuffer), pfd->cFileName);
    if (!fDirectory)
        PathRemoveExtensionW(szBuffer);
    _AddString(c_pkNameWithoutExtension, szBuffer);

    if (fDirectory)
    {
        _AddString(c_pkItemType, L"Directory");
        ptype = PERCEIVED_TYPE_FOLDER;
        pszKind = L"folder";
    }
    else if (*pszExtension)
    {
        _AddString(c_pkFileExtension, pszExtension);
        _AddString(c_pkItemType, pszExtension);
        AssocGetPerceivedType(pszExtension, &ptype, NULL, NULL);
        for (UINT i = 0; i < _countof(c_rgPerceivedKinds); i++)
        {
            if (c_rgPerceivedKinds[i].ptype == ptype)
                pszKind = c_rgPerceivedKinds[i].pszKind;
        }
    }
    _AddInt32(c_pkPerceivedType, ptype);
    _AddBool(c_pkIsFolder, fDirectory);

    if (pszKind)
    {
        PROPVARIANT propvar;

        if (SUCCEEDED(InitPropVariantFromStringVector(&pszKind, 1, &propvar)))
            _Take(c_pkKind, &propvar);
    }

    if (*pszPath)
    {
        _AddString(c_pkItemPathDisplay, pszPath);
        StringCchCopyW(szBuffer, _countof(szBuffer), pszPath);
        if (PathRemoveFileSpecW(szBuffer))
            _AddString(c_pkItemFolderPathDisplay, szBuffer);
    }

    {
        LPITEMIDLIST pidlParent = ILClone(pidl);
        PWSTR pszParent;

        if (pidlParent && ILRemoveLastID(pidlParent) &&
            SUCCEEDED(SHGetNameFromIDList(pidlParent, SIGDN_NORMALDISPLAY, &pszParent)))
        {
            _AddString(c_pkItemFolderNameDisplay, pszParent);
            CoTaskMemFree(pszParent);
        }
        ILFree(pidlParent);
    }

    _AddPropLists(pszExtension, fDirectory, FALSE, fDirectory);
}

VOID
CItemPropertyStore::_AddDriveData(
    _In_ PCWSTR pszRoot)
{
    ULARGE_INTEGER uliFree;
    ULARGE_INTEGER uliTotal;
    WCHAR szFileSystem[MAX_PATH];

    if (GetDiskFreeSpaceExW(pszRoot, NULL, &uliTotal, &uliFree) && uliTotal.QuadPart)
    {
        PROPVARIANT propvar;
        ULARGE_INTEGER *pElems;

        _AddUInt64(c_pkFreeSpace, uliFree.QuadPart);
        _AddUInt64(c_pkCapacity, uliTotal.QuadPart);
        _AddUInt32(c_pkPercentFull,
                   (ULONG)(((uliTotal.QuadPart - uliFree.QuadPart) * 100) / uliTotal.QuadPart));

        pElems = (ULARGE_INTEGER *)CoTaskMemAlloc(2 * sizeof(*pElems));
        if (pElems)
        {
            pElems[0] = uliFree;
            pElems[1] = uliTotal;
            PropVariantInit(&propvar);
            propvar.vt = VT_VECTOR | VT_UI8;
            propvar.cauh.cElems = 2;
            propvar.cauh.pElems = pElems;
            _Take(c_pkDecoratedFreeSpace, &propvar);
        }
    }

    if (GetVolumeInformationW(pszRoot, NULL, 0, NULL, NULL, NULL, szFileSystem, _countof(szFileSystem)))
        _AddString(c_pkFileSystem, szFileSystem);

    _AddBool(c_pkVolumeIsRoot, TRUE);
    _AddBool(c_pkIsFolder, TRUE);
    _AddPropLists(NULL, FALSE, TRUE, TRUE);
}

/* Lists come from the first association class that has them, most specific class first */
VOID
CItemPropertyStore::_AddPropLists(
    _In_opt_ PCWSTR pszExtension,
    _In_ BOOL fDirectory,
    _In_ BOOL fDrive,
    _In_ BOOL fFolder)
{
    WCHAR rgszClasses[6][MAX_PATH];
    UINT cClasses = 0;

    if (fDrive)
    {
        StringCchCopyW(rgszClasses[cClasses++], MAX_PATH, L"Drive");
        StringCchCopyW(rgszClasses[cClasses++], MAX_PATH, L"Folder");
    }
    else if (fDirectory)
    {
        StringCchCopyW(rgszClasses[cClasses++], MAX_PATH, L"Directory");
        StringCchCopyW(rgszClasses[cClasses++], MAX_PATH, L"Folder");
        StringCchCopyW(rgszClasses[cClasses++], MAX_PATH, L"AllFilesystemObjects");
    }
    else if (fFolder)
    {
        StringCchCopyW(rgszClasses[cClasses++], MAX_PATH, L"Folder");
    }
    else
    {
        if (pszExtension && *pszExtension)
        {
            WCHAR szValue[MAX_PATH];
            DWORD cb = sizeof(szValue);

            if (RegGetValueW(HKEY_CLASSES_ROOT, pszExtension, NULL, RRF_RT_REG_SZ, NULL, szValue, &cb) ==
                    ERROR_SUCCESS && szValue[0])
            {
                StringCchCopyW(rgszClasses[cClasses++], MAX_PATH, szValue);
            }
            StringCchCopyW(rgszClasses[cClasses++], MAX_PATH, pszExtension);
            StringCchPrintfW(rgszClasses[cClasses++], MAX_PATH, L"SystemFileAssociations\\%s", pszExtension);

            cb = sizeof(szValue);
            if (RegGetValueW(HKEY_CLASSES_ROOT, pszExtension, L"PerceivedType", RRF_RT_REG_SZ, NULL, szValue,
                             &cb) == ERROR_SUCCESS && szValue[0])
            {
                StringCchPrintfW(rgszClasses[cClasses++], MAX_PATH, L"SystemFileAssociations\\%s", szValue);
            }
        }
        StringCchCopyW(rgszClasses[cClasses++], MAX_PATH, L"*");
        StringCchCopyW(rgszClasses[cClasses++], MAX_PATH, L"AllFilesystemObjects");
    }

    for (UINT iList = 0; iList < _countof(c_rgPropLists); iList++)
    {
        for (UINT iClass = 0; iClass < cClasses; iClass++)
        {
            WCHAR szList[1024];
            DWORD cb = sizeof(szList);

            if (RegGetValueW(HKEY_CLASSES_ROOT, rgszClasses[iClass], c_rgPropLists[iList].pszValue,
                             RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, szList, &cb) == ERROR_SUCCESS)
            {
                _AddString(c_rgPropLists[iList].key, szList);
                break;
            }
        }
    }
}

HRESULT
CItemPropertyStore::Initialize(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_ GETPROPERTYSTOREFLAGS flags,
    _In_reads_opt_(cKeys) const PROPERTYKEY *rgKeys,
    _In_ UINT cKeys)
{
    CComPtr<IShellFolder> psf;
    PCUITEMID_CHILD pidlChild;
    WIN32_FIND_DATAW fd;
    WCHAR szPath[MAX_PATH];
    SFGAOF sfgao = SFGAO_FOLDER | SFGAO_FILESYSTEM;
    HRESULT hr;

    hr = SHBindToParent(pidl, IID_PPV_ARG(IShellFolder, &psf), &pidlChild);
    if (FAILED(hr))
        return hr;

    m_pidlChild = ILClone(pidlChild);
    if (!m_pidlChild)
        return E_OUTOFMEMORY;
    psf->QueryInterface(IID_PPV_ARG(IShellFolder2, &m_psf2));

    /* No property handlers exist yet, so a handler only store is empty */
    if (flags & GPS_HANDLERPROPERTIESONLY)
        return S_OK;
    m_fFolderFallback = TRUE;

    if (FAILED(psf->GetAttributesOf(1, &pidlChild, &sfgao)))
        sfgao = 0;

    _AddNames(psf, pidlChild, pidl);

    if (!SHGetPathFromIDListW(pidl, szPath))
        szPath[0] = UNICODE_NULL;

    ZeroMemory(&fd, sizeof(fd));
    if (!ILIsEmpty(pidlChild) &&
        SUCCEEDED(SHGetDataFromIDListW(psf, pidlChild, SHGDFIL_FINDDATA, &fd, sizeof(fd))) && fd.cFileName[0])
    {
        _AddFileSystemData(pidl, &fd, szPath);
    }
    else if (szPath[0] && PathIsRootW(szPath))
    {
        _AddDriveData(szPath);
    }
    else if (sfgao & SFGAO_FOLDER)
    {
        _AddBool(c_pkIsFolder, TRUE);
        _AddPropLists(NULL, FALSE, FALSE, TRUE);
    }

    /* A store for given keys answers exactly those keys */
    if (rgKeys && cKeys)
    {
        CAtlArray<ENTRY> kept;

        for (UINT i = 0; i < cKeys; i++)
        {
            int iEntry = _Find(rgKeys[i]);

            if (iEntry < 0)
            {
                _AddFromFolder(rgKeys[i]);
                iEntry = (int)m_entries.GetCount() - 1;
            }
            kept.Add(m_entries[iEntry]);
            PropVariantInit(&m_entries[iEntry].value);
        }

        for (SIZE_T i = 0; i < m_entries.GetCount(); i++)
            PropVariantClear(&m_entries[i].value);
        m_entries.SetCount(0);
        for (SIZE_T i = 0; i < kept.GetCount(); i++)
            m_entries.Add(kept[i]);
        m_fFolderFallback = FALSE;
    }
    return S_OK;
}

STDMETHODIMP
CItemPropertyStore::GetCount(
    _Out_ DWORD *cProps)
{
    DWORD cValues = 0;

    if (!cProps)
        return E_POINTER;

    m_cs.Lock();
    for (SIZE_T i = 0; i < m_entries.GetCount(); i++)
    {
        if (!IsEmptyPropVariant(m_entries[i].value))
            cValues++;
    }
    m_cs.Unlock();
    *cProps = cValues;
    return S_OK;
}

STDMETHODIMP
CItemPropertyStore::GetAt(
    _In_ DWORD iProp,
    _Out_ PROPERTYKEY *pkey)
{
    HRESULT hr = E_INVALIDARG;

    if (!pkey)
        return E_POINTER;

    m_cs.Lock();
    for (SIZE_T i = 0; i < m_entries.GetCount(); i++)
    {
        if (IsEmptyPropVariant(m_entries[i].value))
            continue;
        if (!iProp)
        {
            *pkey = m_entries[i].key;
            hr = S_OK;
            break;
        }
        iProp -= 1;
    }
    m_cs.Unlock();
    return hr;
}

STDMETHODIMP
CItemPropertyStore::GetValue(
    _In_ REFPROPERTYKEY key,
    _Out_ PROPVARIANT *pv)
{
    HRESULT hr;
    int iEntry;

    if (!pv)
        return E_POINTER;
    PropVariantInit(pv);

    m_cs.Lock();
    iEntry = _Find(key);
    if (iEntry < 0 && m_fFolderFallback)
    {
        _AddFromFolder(key);
        iEntry = _Find(key);
    }
    hr = (iEntry >= 0) ? PropVariantCopy(pv, &m_entries[iEntry].value) : S_OK;
    m_cs.Unlock();
    return hr;
}

STDMETHODIMP
CItemPropertyStore::SetValue(
    _In_ REFPROPERTYKEY key,
    _In_ REFPROPVARIANT propvar)
{
    return STG_E_ACCESSDENIED;
}

STDMETHODIMP
CItemPropertyStore::Commit()
{
    return STG_E_ACCESSDENIED;
}

STDMETHODIMP
CItemPropertyStore::IsPropertyWritable(
    _In_ REFPROPERTYKEY key)
{
    return S_FALSE;
}

HRESULT
SHELL_CreateItemPropertyStore(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_ GETPROPERTYSTOREFLAGS flags,
    _In_reads_opt_(cKeys) const PROPERTYKEY *rgKeys,
    _In_ UINT cKeys,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    *ppv = NULL;
    if (!pidl)
        return E_INVALIDARG;
    if (flags & GPS_READWRITE)
        return STG_E_ACCESSDENIED;
    return ShellObjectCreatorInit<CItemPropertyStore>(pidl, flags, rgKeys, cKeys, riid, ppv);
}

/* Store of several items: each key is combined the way its description says */
class CItemArrayPropertyStore :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IPropertyStore,
    public IPropertyStoreCapabilities
{
    CAtlArray<CComPtr<IPropertyStore> > m_stores;

    HRESULT _CombineDefault(_In_ PROPVARIANT *rgValues, _In_ UINT cValues, _Out_ PROPVARIANT *pv);
    HRESULT _CombineSum(_In_ PROPVARIANT *rgValues, _In_ UINT cValues, _In_ BOOL fAverage, _Out_ PROPVARIANT *pv);
    HRESULT _CombineDateRange(_In_ PROPVARIANT *rgValues, _In_ UINT cValues, _Out_ PROPVARIANT *pv);
    HRESULT _CombineUnion(_In_ PROPVARIANT *rgValues, _In_ UINT cValues, _Out_ PROPVARIANT *pv);
    HRESULT _CombineExtreme(_In_ PROPVARIANT *rgValues, _In_ UINT cValues, _In_ BOOL fMax, _Out_ PROPVARIANT *pv);

public:
    HRESULT Initialize(
        _In_ IShellItemArray *psia,
        _In_ GETPROPERTYSTOREFLAGS flags);

    // IPropertyStore
    STDMETHODIMP GetCount(_Out_ DWORD *cProps) override;
    STDMETHODIMP GetAt(_In_ DWORD iProp, _Out_ PROPERTYKEY *pkey) override;
    STDMETHODIMP GetValue(_In_ REFPROPERTYKEY key, _Out_ PROPVARIANT *pv) override;
    STDMETHODIMP SetValue(_In_ REFPROPERTYKEY key, _In_ REFPROPVARIANT propvar) override;
    STDMETHODIMP Commit() override;

    // IPropertyStoreCapabilities
    STDMETHODIMP IsPropertyWritable(_In_ REFPROPERTYKEY key) override;

    DECLARE_NOT_AGGREGATABLE(CItemArrayPropertyStore)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CItemArrayPropertyStore)
        COM_INTERFACE_ENTRY_IID(IID_IPropertyStore, IPropertyStore)
        COM_INTERFACE_ENTRY_IID(IID_IPropertyStoreCapabilities, IPropertyStoreCapabilities)
    END_COM_MAP()
};

static HRESULT
SHELL_GetItemStore(
    _In_ IShellItem *psi,
    _In_ GETPROPERTYSTOREFLAGS flags,
    _COM_Outptr_ IPropertyStore **ppps)
{
    CComPtr<IShellItem2> psi2;
    PIDLIST_ABSOLUTE pidl;
    HRESULT hr;

    *ppps = NULL;
    if (SUCCEEDED(psi->QueryInterface(IID_PPV_ARG(IShellItem2, &psi2))))
        return psi2->GetPropertyStore(flags, IID_PPV_ARG(IPropertyStore, ppps));

    hr = SHGetIDListFromObject(psi, &pidl);
    if (SUCCEEDED(hr))
    {
        hr = SHELL_CreateItemPropertyStore(pidl, flags, NULL, 0, IID_PPV_ARG(IPropertyStore, ppps));
        ILFree(pidl);
    }
    return hr;
}

HRESULT
CItemArrayPropertyStore::Initialize(
    _In_ IShellItemArray *psia,
    _In_ GETPROPERTYSTOREFLAGS flags)
{
    DWORD cItems = 0;
    HRESULT hr;

    hr = psia->GetCount(&cItems);
    if (FAILED(hr))
        return hr;

    for (DWORD i = 0; i < cItems; i++)
    {
        CComPtr<IShellItem> psi;
        CComPtr<IPropertyStore> pps;

        hr = psia->GetItemAt(i, &psi);
        if (SUCCEEDED(hr))
            hr = SHELL_GetItemStore(psi, flags, &pps);
        if (FAILED(hr))
        {
            if (flags & GPS_BESTEFFORT)
                continue;
            return hr;
        }
        m_stores.Add(pps);
    }
    return m_stores.GetCount() ? S_OK : E_FAIL;
}

/* Equal values stay, differing values leave the property empty */
HRESULT
CItemArrayPropertyStore::_CombineDefault(
    _In_ PROPVARIANT *rgValues,
    _In_ UINT cValues,
    _Out_ PROPVARIANT *pv)
{
    for (UINT i = 1; i < cValues; i++)
    {
        if (PropVariantCompareEx(rgValues[0], rgValues[i], PVCU_DEFAULT, PVCF_DEFAULT) != 0)
            return S_OK;
    }
    return PropVariantCopy(pv, &rgValues[0]);
}

HRESULT
CItemArrayPropertyStore::_CombineSum(
    _In_ PROPVARIANT *rgValues,
    _In_ UINT cValues,
    _In_ BOOL fAverage,
    _Out_ PROPVARIANT *pv)
{
    PROPVARIANT pvTotal;
    VARTYPE vt = VT_EMPTY;
    double dblTotal = 0;
    UINT cNumbers = 0;
    HRESULT hr;

    for (UINT i = 0; i < cValues; i++)
    {
        double dbl;

        if (IsEmptyPropVariant(rgValues[i]) || FAILED(PropVariantToDouble(rgValues[i], &dbl)))
            continue;
        if (vt == VT_EMPTY)
            vt = rgValues[i].vt;
        dblTotal += dbl;
        cNumbers++;
    }
    if (!cNumbers)
        return S_OK;
    if (fAverage)
        dblTotal /= cNumbers;

    PropVariantInit(&pvTotal);
    pvTotal.vt = VT_R8;
    pvTotal.dblVal = dblTotal;
    hr = PropVariantChangeType(pv, pvTotal, PVCHF_DEFAULT, vt);
    if (FAILED(hr))
        *pv = pvTotal;
    return S_OK;
}

/* A date range is a two element vector holding the oldest and newest date */
HRESULT
CItemArrayPropertyStore::_CombineDateRange(
    _In_ PROPVARIANT *rgValues,
    _In_ UINT cValues,
    _Out_ PROPVARIANT *pv)
{
    FILETIME ftMin = {0};
    FILETIME ftMax = {0};
    BOOL fAny = FALSE;
    FILETIME *pElems;

    for (UINT i = 0; i < cValues; i++)
    {
        FILETIME ft;

        if (rgValues[i].vt != VT_FILETIME)
            continue;
        ft = rgValues[i].filetime;
        if (!fAny || CompareFileTime(&ft, &ftMin) < 0)
            ftMin = ft;
        if (!fAny || CompareFileTime(&ft, &ftMax) > 0)
            ftMax = ft;
        fAny = TRUE;
    }
    if (!fAny)
        return S_OK;

    if (!CompareFileTime(&ftMin, &ftMax))
    {
        pv->vt = VT_FILETIME;
        pv->filetime = ftMin;
        return S_OK;
    }

    pElems = (FILETIME *)CoTaskMemAlloc(2 * sizeof(*pElems));
    if (!pElems)
        return E_OUTOFMEMORY;
    pElems[0] = ftMin;
    pElems[1] = ftMax;
    pv->vt = VT_VECTOR | VT_FILETIME;
    pv->cafiletime.cElems = 2;
    pv->cafiletime.pElems = pElems;
    return S_OK;
}

HRESULT
CItemArrayPropertyStore::_CombineUnion(
    _In_ PROPVARIANT *rgValues,
    _In_ UINT cValues,
    _Out_ PROPVARIANT *pv)
{
    CAtlArray<PWSTR> strings;
    BOOL fVector = FALSE;
    HRESULT hr = S_OK;

    for (UINT i = 0; i < cValues && SUCCEEDED(hr); i++)
    {
        const PROPVARIANT &value = rgValues[i];
        ULONG cStrings;

        if (value.vt == VT_LPWSTR || value.vt == VT_BSTR)
            cStrings = 1;
        else if (value.vt == (VT_VECTOR | VT_LPWSTR))
            cStrings = value.calpwstr.cElems;
        else
            continue;
        if (value.vt & VT_VECTOR)
            fVector = TRUE;

        for (ULONG j = 0; j < cStrings; j++)
        {
            PCWSTR psz = (value.vt == VT_LPWSTR) ? value.pwszVal :
                         (value.vt == VT_BSTR) ? value.bstrVal : value.calpwstr.pElems[j];
            BOOL fFound = FALSE;
            PWSTR pszCopy;

            if (!psz || !*psz)
                continue;
            for (SIZE_T k = 0; k < strings.GetCount() && !fFound; k++)
                fFound = !lstrcmpiW(strings[k], psz);
            if (fFound)
                continue;

            hr = SHStrDupW(psz, &pszCopy);
            if (FAILED(hr))
                break;
            if (strings.Add(pszCopy) == (SIZE_T)-1)
                CoTaskMemFree(pszCopy);
        }
    }

    if (strings.GetCount() == 1 && !fVector)
        hr = InitPropVariantFromString(strings[0], pv);
    else if (strings.GetCount())
        hr = InitPropVariantFromStringVector((PCWSTR *)strings.GetData(), (ULONG)strings.GetCount(), pv);

    for (SIZE_T k = 0; k < strings.GetCount(); k++)
        CoTaskMemFree(strings[k]);
    return hr;
}

HRESULT
CItemArrayPropertyStore::_CombineExtreme(
    _In_ PROPVARIANT *rgValues,
    _In_ UINT cValues,
    _In_ BOOL fMax,
    _Out_ PROPVARIANT *pv)
{
    int iBest = -1;

    for (UINT i = 0; i < cValues; i++)
    {
        int iCompare;

        if (IsEmptyPropVariant(rgValues[i]))
            continue;
        if (iBest < 0)
        {
            iBest = i;
            continue;
        }
        iCompare = PropVariantCompareEx(rgValues[i], rgValues[iBest], PVCU_DEFAULT, PVCF_DEFAULT);
        if (fMax ? iCompare > 0 : iCompare < 0)
            iBest = i;
    }
    return (iBest >= 0) ? PropVariantCopy(pv, &rgValues[iBest]) : S_OK;
}

STDMETHODIMP
CItemArrayPropertyStore::GetCount(
    _Out_ DWORD *cProps)
{
    return m_stores[0]->GetCount(cProps);
}

STDMETHODIMP
CItemArrayPropertyStore::GetAt(
    _In_ DWORD iProp,
    _Out_ PROPERTYKEY *pkey)
{
    return m_stores[0]->GetAt(iProp, pkey);
}

STDMETHODIMP
CItemArrayPropertyStore::GetValue(
    _In_ REFPROPERTYKEY key,
    _Out_ PROPVARIANT *pv)
{
    CComPtr<IPropertyDescription> ppd;
    PROPDESC_AGGREGATION_TYPE aggregation = PDAT_DEFAULT;
    PROPVARIANT *rgValues;
    UINT cValues = (UINT)m_stores.GetCount();
    HRESULT hr = S_OK;

    if (!pv)
        return E_POINTER;
    PropVariantInit(pv);

    rgValues = (PROPVARIANT *)CoTaskMemAlloc(cValues * sizeof(*rgValues));
    if (!rgValues)
        return E_OUTOFMEMORY;
    for (UINT i = 0; i < cValues; i++)
    {
        if (FAILED(m_stores[i]->GetValue(key, &rgValues[i])))
            PropVariantInit(&rgValues[i]);
    }

    if (SUCCEEDED(PSGetPropertyDescription(key, IID_PPV_ARG(IPropertyDescription, &ppd))))
        ppd->GetAggregationType(&aggregation);

    switch (aggregation)
    {
        case PDAT_FIRST:
            for (UINT i = 0; i < cValues; i++)
            {
                if (!IsEmptyPropVariant(rgValues[i]))
                {
                    hr = PropVariantCopy(pv, &rgValues[i]);
                    break;
                }
            }
            break;
        case PDAT_SUM:
            hr = _CombineSum(rgValues, cValues, FALSE, pv);
            break;
        case PDAT_AVERAGE:
            hr = _CombineSum(rgValues, cValues, TRUE, pv);
            break;
        case PDAT_DATERANGE:
            hr = _CombineDateRange(rgValues, cValues, pv);
            break;
        case PDAT_UNION:
            hr = _CombineUnion(rgValues, cValues, pv);
            break;
        case PDAT_MAX:
            hr = _CombineExtreme(rgValues, cValues, TRUE, pv);
            break;
        case PDAT_MIN:
            hr = _CombineExtreme(rgValues, cValues, FALSE, pv);
            break;
        default:
            hr = _CombineDefault(rgValues, cValues, pv);
            break;
    }

    for (UINT i = 0; i < cValues; i++)
        PropVariantClear(&rgValues[i]);
    CoTaskMemFree(rgValues);
    return hr;
}

STDMETHODIMP
CItemArrayPropertyStore::SetValue(
    _In_ REFPROPERTYKEY key,
    _In_ REFPROPVARIANT propvar)
{
    return STG_E_ACCESSDENIED;
}

STDMETHODIMP
CItemArrayPropertyStore::Commit()
{
    return STG_E_ACCESSDENIED;
}

STDMETHODIMP
CItemArrayPropertyStore::IsPropertyWritable(
    _In_ REFPROPERTYKEY key)
{
    return S_FALSE;
}

/* One item answers with its own store; several are combined */
HRESULT
SHELL_CreateItemArrayPropertyStore(
    _In_ IShellItemArray *psia,
    _In_ GETPROPERTYSTOREFLAGS flags,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    DWORD cItems = 0;
    HRESULT hr;

    *ppv = NULL;
    if (flags & GPS_TEMPORARY)
        return E_INVALIDARG;

    hr = psia->GetCount(&cItems);
    if (FAILED(hr))
        return hr;
    if (!cItems)
        return E_UNEXPECTED;

    if (cItems == 1)
    {
        CComPtr<IShellItem> psi;
        CComPtr<IPropertyStore> pps;

        hr = psia->GetItemAt(0, &psi);
        if (SUCCEEDED(hr))
            hr = SHELL_GetItemStore(psi, flags, &pps);
        if (SUCCEEDED(hr))
            hr = pps->QueryInterface(riid, ppv);
        return hr;
    }
    return ShellObjectCreatorInit<CItemArrayPropertyStore>(psia, flags, riid, ppv);
}

/* Descriptions shared by every item's list, in the first item's order */
class CItemArrayPropertyDescriptionList :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IPropertyDescriptionList
{
    CAtlArray<CComPtr<IPropertyDescription> > m_descriptions;

public:
    HRESULT Initialize(
        _In_ IShellItemArray *psia,
        _In_ REFPROPERTYKEY keyType);

    // IPropertyDescriptionList
    STDMETHODIMP GetCount(_Out_ UINT *pcElem) override;
    STDMETHODIMP GetAt(_In_ UINT iElem, _In_ REFIID riid, _Outptr_ void **ppv) override;

    DECLARE_NOT_AGGREGATABLE(CItemArrayPropertyDescriptionList)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CItemArrayPropertyDescriptionList)
        COM_INTERFACE_ENTRY_IID(IID_IPropertyDescriptionList, IPropertyDescriptionList)
    END_COM_MAP()
};

static HRESULT
SHELL_GetItemPropertyList(
    _In_ IShellItem *psi,
    _In_ REFPROPERTYKEY keyType,
    _COM_Outptr_ IPropertyDescriptionList **pppdl)
{
    CComPtr<IPropertyStore> pps;
    PROPVARIANT propvar;
    HRESULT hr;

    *pppdl = NULL;
    hr = SHELL_GetItemStore(psi, GPS_DEFAULT, &pps);
    if (FAILED(hr))
        return hr;

    hr = pps->GetValue(keyType, &propvar);
    if (FAILED(hr))
        return hr;

    if (propvar.vt == VT_LPWSTR && propvar.pwszVal && propvar.pwszVal[0])
        hr = PSGetPropertyDescriptionListFromString(propvar.pwszVal, IID_PPV_ARG(IPropertyDescriptionList, pppdl));
    else
        hr = HResultFromWin32(ERROR_NOT_FOUND);
    PropVariantClear(&propvar);
    return hr;
}

static BOOL
SHELL_ListHasKey(
    _In_ IPropertyDescriptionList *ppdl,
    _In_ REFPROPERTYKEY key)
{
    UINT cElems = 0;

    ppdl->GetCount(&cElems);
    for (UINT i = 0; i < cElems; i++)
    {
        CComPtr<IPropertyDescription> ppd;
        PROPERTYKEY keyElem;

        if (SUCCEEDED(ppdl->GetAt(i, IID_PPV_ARG(IPropertyDescription, &ppd))) &&
            SUCCEEDED(ppd->GetPropertyKey(&keyElem)) && SHELL_IsSameKey(keyElem, key))
        {
            return TRUE;
        }
    }
    return FALSE;
}

HRESULT
CItemArrayPropertyDescriptionList::Initialize(
    _In_ IShellItemArray *psia,
    _In_ REFPROPERTYKEY keyType)
{
    CAtlArray<CComPtr<IPropertyDescriptionList> > lists;
    DWORD cItems = 0;
    HRESULT hr;

    hr = psia->GetCount(&cItems);
    if (FAILED(hr))
        return hr;

    for (DWORD i = 0; i < cItems; i++)
    {
        CComPtr<IShellItem> psi;
        CComPtr<IPropertyDescriptionList> ppdl;

        if (FAILED(psia->GetItemAt(i, &psi)))
            return S_OK;
        if (FAILED(SHELL_GetItemPropertyList(psi, keyType, &ppdl)))
            return S_OK;
        lists.Add(ppdl);
    }
    if (!lists.GetCount())
        return S_OK;

    UINT cElems = 0;
    lists[0]->GetCount(&cElems);
    for (UINT i = 0; i < cElems; i++)
    {
        CComPtr<IPropertyDescription> ppd;
        PROPERTYKEY key;
        BOOL fShared = TRUE;

        if (FAILED(lists[0]->GetAt(i, IID_PPV_ARG(IPropertyDescription, &ppd))) ||
            FAILED(ppd->GetPropertyKey(&key)))
        {
            continue;
        }
        for (SIZE_T j = 1; j < lists.GetCount() && fShared; j++)
            fShared = SHELL_ListHasKey(lists[j], key);
        if (fShared)
            m_descriptions.Add(ppd);
    }
    return S_OK;
}

STDMETHODIMP
CItemArrayPropertyDescriptionList::GetCount(
    _Out_ UINT *pcElem)
{
    if (!pcElem)
        return E_POINTER;
    *pcElem = (UINT)m_descriptions.GetCount();
    return S_OK;
}

STDMETHODIMP
CItemArrayPropertyDescriptionList::GetAt(
    _In_ UINT iElem,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = NULL;
    if (iElem >= m_descriptions.GetCount())
        return E_INVALIDARG;
    return m_descriptions[iElem]->QueryInterface(riid, ppv);
}

HRESULT
SHELL_CreateItemArrayPropertyDescriptionList(
    _In_ IShellItemArray *psia,
    _In_ REFPROPERTYKEY keyType,
    _In_ REFIID riid,
    _Outptr_ void **ppv)
{
    DWORD cItems = 0;

    *ppv = NULL;
    if (SUCCEEDED(psia->GetCount(&cItems)) && cItems == 1)
    {
        CComPtr<IShellItem> psi;
        CComPtr<IPropertyDescriptionList> ppdl;

        if (SUCCEEDED(psia->GetItemAt(0, &psi)) && SUCCEEDED(SHELL_GetItemPropertyList(psi, keyType, &ppdl)))
            return ppdl->QueryInterface(riid, ppv);
    }
    return ShellObjectCreatorInit<CItemArrayPropertyDescriptionList>(psia, keyType, riid, ppv);
}
