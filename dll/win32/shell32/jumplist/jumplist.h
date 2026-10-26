/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Jump lists: destination lists, object collections and recent item tracking
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "../pinning/pinning.h"

EXTERN_C const CLSID CLSID_ApplicationDestinations;
EXTERN_C const CLSID CLSID_ApplicationDocumentLists;

/* Public in the Windows 7 SDK, missing from the ReactOS one */
struct DECLSPEC_UUID("2C1C7E2E-2D0E-4059-831E-1E6F82335C2E") DECLSPEC_NOVTABLE
IEnumObjects : public IUnknown
{
    STDMETHOD(Next)(
        _In_ ULONG celt,
        _In_ REFIID riid,
        _Out_writes_to_(celt, *pceltFetched) void **rgelt,
        _Out_opt_ ULONG *pceltFetched) PURE;
    STDMETHOD(Skip)(
        _In_ ULONG celt) PURE;
    STDMETHOD(Reset)() PURE;
    STDMETHOD(Clone)(
        _Outptr_ IEnumObjects **ppenum) PURE;
};

static const IID IID_IEnumObjects = __uuidof(IEnumObjects);

#ifndef SHARD_APPIDINFO
#define SHARD_APPIDINFO         0x00000004L
#define SHARD_APPIDINFOIDLIST   0x00000005L
#define SHARD_LINK              0x00000006L
#define SHARD_APPIDINFOLINK     0x00000007L
#define SHARD_SHELLITEM         0x00000008L

typedef struct SHARDAPPIDINFO
{
    IShellItem *psi;
    PCWSTR pszAppID;
} SHARDAPPIDINFO;

typedef struct SHARDAPPIDINFOIDLIST
{
    PCIDLIST_ABSOLUTE pidl;
    PCWSTR pszAppID;
} SHARDAPPIDINFOIDLIST;

typedef struct SHARDAPPIDINFOLINK
{
    IShellLinkW *psl;
    PCWSTR pszAppID;
} SHARDAPPIDINFOLINK;
#endif

#ifndef GPFIDL_UNCPRINTER
#define GPFIDL_UNCPRINTER       0x2
#endif

/* Which automatic list IAutomaticDestinationList::GetList returns */
typedef enum DESTLISTTYPE
{
    DLT_PINNED = 0,
    DLT_RECENT = 1,
    DLT_FREQUENT = 2
} DESTLISTTYPE;

typedef enum tagDESTCATTYPE
{
    DCT_CUSTOM = 0,
    DCT_KNOWN = 1,
    DCT_TASKS = 2
} DESTCATTYPE;

typedef struct tagAPPDESTCATEGORY
{
    DESTCATTYPE cattype;
    union
    {
        PWSTR pszCategoryName;
        KNOWNDESTCATEGORY category;
        BYTE reserved;
    };
    UINT cItems;
    UINT cSeparators;
} APPDESTCATEGORY;

typedef enum GETCATFLAG
{
    GETCATFLAG_NONE = 0,
    GETCATFLAG_GETNAME = 1
} GETCATFLAG;

/* Flags for CDestinationList::s_DeleteList */
typedef enum CDOPT
{
    CDOPT_DELETECUSTOM = 0x100,
    CDOPT_DELETETASKS = 0x200,
    CDOPT_REMOVEDESTINATIONS = 0x400
} CDOPT;

/* Windows 7 slot order */
struct DECLSPEC_UUID("BC10DCE3-62F2-4BC6-AF37-DB46ED7873C4") DECLSPEC_NOVTABLE
IAutomaticDestinationList : public IUnknown
{
    STDMETHOD(Initialize)(
        _In_ PCWSTR pszAppID,
        _In_opt_ PCWSTR pszAppPath,
        _In_opt_ PCWSTR pszListPath) PURE;
    STDMETHOD(HasList)(
        _Out_ BOOL *pfHasList) PURE;
    STDMETHOD(GetList)(
        _In_ DESTLISTTYPE type,
        _In_ int cMaxCount,
        _In_ REFIID riid,
        _Outptr_ void **ppv) PURE;
    STDMETHOD(AddUsagePoint)(
        _In_ IUnknown *punk) PURE;
    STDMETHOD(PinItem)(
        _In_ IUnknown *punk,
        _In_ int iPinPos) PURE;
    STDMETHOD(IsPinned)(
        _In_ IUnknown *punk,
        _Out_ int *piPinPos) PURE;
    STDMETHOD(RemoveDestination)(
        _In_ IUnknown *punk) PURE;
    STDMETHOD(SetUsageData)(
        _In_ IUnknown *punk,
        _In_opt_ FLOAT *pflPoints,
        _In_opt_ FILETIME *pftLastAccess) PURE;
    STDMETHOD(GetUsageData)(
        _In_ IUnknown *punk,
        _Out_ FLOAT *pflPoints,
        _Out_ FILETIME *pftLastAccess) PURE;
    STDMETHOD(ResolveDestination)(
        _In_opt_ HWND hwnd,
        _In_ DWORD dwFlags,
        _In_ IShellItem *psi,
        _In_ REFIID riid,
        _Outptr_ void **ppv) PURE;
    STDMETHOD(ClearList)(
        _In_ BOOL fClearPinned) PURE;
};

/* Windows 7 slot order; Windows 8 added HasListEx and ClearRemovedDestinations */
struct DECLSPEC_UUID("03F1EED2-8676-430B-ABE1-765C1D8FE147") DECLSPEC_NOVTABLE
IInternalCustomDestinationList : public IUnknown
{
    STDMETHOD(SetMinItems)(
        _In_ UINT cMinItems) PURE;
    STDMETHOD(SetApplicationID)(
        _In_ PCWSTR pszAppID) PURE;
    STDMETHOD(GetSlotCount)(
        _Out_ UINT *pcSlots) PURE;
    STDMETHOD(GetCategoryCount)(
        _Out_ UINT *pcCategories) PURE;
    STDMETHOD(GetCategory)(
        _In_ UINT iCategory,
        _In_ GETCATFLAG flags,
        _Out_ APPDESTCATEGORY *pCategory) PURE;
    STDMETHOD(DeleteCategory)(
        _In_ UINT iCategory,
        _In_ BOOL fDeleteDestinations) PURE;
    STDMETHOD(EnumerateCategoryDestinations)(
        _In_ UINT iCategory,
        _In_ REFIID riid,
        _Outptr_ void **ppv) PURE;
    STDMETHOD(RemoveDestination)(
        _In_ IUnknown *punk) PURE;
    STDMETHOD(ResolveDestination)(
        _In_opt_ HWND hwnd,
        _In_ DWORD dwFlags,
        _In_ IShellItem *psi,
        _In_ REFIID riid,
        _Outptr_ void **ppv) PURE;
};

static const IID IID_IAutomaticDestinationList = __uuidof(IAutomaticDestinationList);
static const IID IID_IInternalCustomDestinationList = __uuidof(IInternalCustomDestinationList);

PIN_DEFINE_APPMODEL_KEY(PKEY_AppUserModel_IsDestListSeparator, 6);
PIN_DEFINE_APPMODEL_KEY(PKEY_AppUserModel_IsDestListLink, 11);

static const PROPERTYKEY PKEY_Title =
    { { 0xf29f85e0, 0x4ff9, 0x1068, { 0xab, 0x91, 0x08, 0x00, 0x2b, 0x27, 0xb3, 0xd9 } }, 2 };
static const PROPERTYKEY PKEY_Comment =
    { { 0xf29f85e0, 0x4ff9, 0x1068, { 0xab, 0x91, 0x08, 0x00, 0x2b, 0x27, 0xb3, 0xd9 } }, 6 };
static const PROPERTYKEY PKEY_Kind =
    { { 0x1e3ee840, 0xbc2b, 0x476c, { 0x82, 0x37, 0x2a, 0xcd, 0x1a, 0x83, 0x9b, 0x22 } }, 3 };
static const PROPERTYKEY PKEY_FileExtension =
    { { 0xe4f10a3c, 0x49e6, 0x405d, { 0x82, 0x88, 0xa2, 0x3b, 0xd4, 0xee, 0xaa, 0x6c } }, 100 };

/* Change counter explorer polls to notice destination list changes */
#define JUMPLIST_GLOBALCOUNTER  12

/* Where the lists live under the Recent folder */
#define DESTINATIONS_AUTOMATIC  0
#define DESTINATIONS_CUSTOM     1

/* Limits Windows 7 puts on the lists */
#define JUMPLIST_MAX_CATEGORIES         50
#define JUMPLIST_MAX_ITEMS              1000
#define JUMPLIST_MAX_PINNED             100
#define JUMPLIST_DEFAULT_ITEMS          10
#define JUMPLIST_MAX_SETTING_ITEMS      60

/* Reflected 64 bit CRC used for list file names and entry checksums */
class CJumpListCrc64
{
public:
    CJumpListCrc64();

    CJumpListCrc64(
        _In_reads_bytes_(cb) const void *pv,
        _In_ ULONG cb);

    ULONGLONG
    Update(
        _In_reads_bytes_(cb) const void *pv,
        _In_ ULONG cb);

    HRESULT
    ToString(
        _Out_writes_(cch) PWSTR psz,
        _In_ UINT cch) const;

    ULONGLONG m_ullValue;
};

HRESULT
HashAppID(
    _In_ PCWSTR pszAppID,
    _Out_writes_(cchHash) PWSTR pszHash,
    _In_ UINT cchHash);

HRESULT
GetDestinationsPath(
    _In_ UINT uType,
    _Out_writes_(MAX_PATH) PWSTR pszPath);

BOOL
IsRecentDocsEnabled(VOID);

UINT
ReadStartPageSettingBounded(
    _In_ PCWSTR pszValue,
    _In_ UINT uDefault,
    _In_ UINT uMin,
    _In_ UINT uMax);

VOID
JumpListCounterIncrement(VOID);

HRESULT
JumpListGetString(
    _In_ IPropertyStore *pps,
    _In_ REFPROPERTYKEY key,
    _Outptr_ PWSTR *ppsz);

BOOL
IsSeparator(
    _In_ IUnknown *punk);

BOOL
IsDestListLink(
    _In_ IUnknown *punk);

HRESULT
MarkDestListLink(
    _In_ IUnknown *punk);

HRESULT
CreateLinkForItem(
    _In_ IUnknown *punkItem,
    _Outptr_ IShellLinkW **ppsl,
    _In_ BOOL fKeepTracking);

HRESULT
CreateItemFromLink(
    _In_ IShellLinkW *psl,
    _In_ REFIID riid,
    _Outptr_ void **ppv);

HRESULT
DisplayNameFromLink(
    _In_ IShellLinkW *psl,
    _Outptr_ PWSTR *ppszName,
    _Inout_updates_z_(MAX_PATH) PWSTR pszPath);

int
CALLBACK
DPA_CompareLinksCB(
    _In_ void *p1,
    _In_ void *p2,
    _In_ LPARAM lParam);

int
CALLBACK
DPA_ReleaseUnknownCB(
    _In_opt_ void *p,
    _In_opt_ void *pData);

HRESULT
IUnknown_LoadKnownImplFromStream(
    _In_ IStream *pstm,
    _In_reads_(cclsid) const CLSID * const *rgpclsid,
    _In_ UINT cclsid,
    _In_ REFIID riid,
    _Outptr_ void **ppv);

HRESULT
IUnknown_SaveKnownImplToStream(
    _In_ IStream *pstm,
    _In_reads_(cclsid) const CLSID * const *rgpclsid,
    _In_ UINT cclsid,
    _In_ IUnknown *punk);

HRESULT
JumpListStreamReadString(
    _In_ IStream *pstm,
    _Outptr_ PWSTR *ppsz);

HRESULT
JumpListStreamWriteString(
    _In_ IStream *pstm,
    _In_opt_ PCWSTR psz);

HRESULT
MakeDestinationItemForApp(
    _In_ IShellItem *psi,
    _In_opt_ IApplicationResolver *par,
    _In_opt_ PCWSTR pszAppID,
    _In_ DWORD dwProcessId,
    _In_opt_ PCWSTR pszAppPath,
    _Outptr_opt_ PWSTR *ppszAppID);

HRESULT
CreateEnumerableObjectCollection(
    _In_ UINT cGrow,
    _In_ REFIID riid,
    _Outptr_ void **ppv);

HRESULT
AddPointToItem(
    _In_ IUnknown *punk,
    _In_ PCWSTR pszAppID,
    _In_opt_ PCWSTR pszAppPath);

HRESULT
AddToAutoDestList(
    _In_ DWORD dwProcessId,
    _In_ PCIDLIST_ABSOLUTE pidl,
    _In_opt_ PCWSTR pszAppID,
    _In_opt_ PCWSTR pszAppPath,
    _Out_ BOOL *pfExplorer);

VOID
AddLinkToAutoDestList(
    _In_ DWORD dwProcessId,
    _In_ PCWSTR pszPath,
    _In_opt_ PCWSTR pszArgs,
    _In_opt_ PCWSTR pszTitle,
    _In_opt_ PCWSTR pszAppID,
    _In_opt_ PCWSTR pszDescription,
    _In_opt_ PCWSTR pszIconPath,
    _In_ int iIcon);

EXTERN_C
VOID
SHELL_AddToRecentDocs(
    _In_ UINT uFlags,
    _In_opt_ LPCVOID pv,
    _In_opt_ PCWSTR pszAppPath);
