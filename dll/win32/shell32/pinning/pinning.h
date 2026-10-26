/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Start menu and taskbar pinning, application IDs and the application resolver
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Win7 shell objects start zeroed; shell32's operator new does not zero */
class CPinZeroAlloc
{
public:
    static void *operator new(size_t cb)
    {
        return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cb);
    }

    static void operator delete(void *pv)
    {
        if (pv)
            HeapFree(GetProcessHeap(), 0, pv);
    }
};

EXTERN_C const CLSID CLSID_StartMenuPin;
EXTERN_C const CLSID CLSID_TaskbandPin;
EXTERN_C const CLSID CLSID_StartMenuCacheAndAppResolver;
EXTERN_C const CLSID CLSID_AutomaticDestinationList;

/* Bind context parameter that keeps parsing on the real file system path */
#define PIN_BIND_NO_ALIAS   L"Don't Translate Aliases"

/* Flags for IPinnedList::GetPinnableInfo and IsPinnable */
#define PINNABLE_EXEONLY                0x01
#define PINNABLE_REJECTSLOWMEDIA        0x02
#define PINNABLE_DETERMINE_APPLICATION  0x04
#define PINNABLE_APPLICATION_ONLY       0x08
#define PINNABLE_ALLOW_ALL_APPSTATES    0x10

/* Errors the resolver and the pinned lists hand out */
#define E_PIN_NOT_FOLDER_APP            ((HRESULT)0x80040F03)
#define E_PIN_HAS_APPID                 ((HRESULT)0x80040F04)
#define E_APP_NOT_INTERESTING           ((HRESULT)0x80040F07)
#define E_APP_NO_EXTENSION              ((HRESULT)0x80040F08)

struct DECLSPEC_UUID("D0191542-7954-4908-BC06-B2360BBE45BA") DECLSPEC_NOVTABLE
IEnumFullIDList : public IUnknown
{
    STDMETHOD(Next)(
        _In_ ULONG celt,
        _Out_writes_to_(celt, *pceltFetched) PIDLIST_ABSOLUTE *rgelt,
        _Out_opt_ ULONG *pceltFetched) PURE;
    STDMETHOD(Skip)(
        _In_ ULONG celt) PURE;
    STDMETHOD(Reset)() PURE;
    STDMETHOD(Clone)(
        _Outptr_ IEnumFullIDList **ppenum) PURE;
};

/* Windows 7 slot order */
struct DECLSPEC_UUID("BBD20037-BC0E-42F1-913F-E2936BB0EA0C") DECLSPEC_NOVTABLE
IPinnedList : public IUnknown
{
    STDMETHOD(EnumObjects)(
        _Outptr_ IEnumFullIDList **ppenum) PURE;
    STDMETHOD(Modify)(
        _In_opt_ PCIDLIST_ABSOLUTE pidlFrom,
        _In_opt_ PCIDLIST_ABSOLUTE pidlTo) PURE;
    STDMETHOD(GetChangeCount)(
        _Out_ UINT *pcChanges) PURE;
    STDMETHOD(GetPinnableInfo)(
        _In_ IDataObject *pdo,
        _In_ DWORD dwFlags,
        _Outptr_opt_ IShellItem **ppsiApplication,
        _Outptr_opt_ IShellItem **ppsiDestination,
        _Outptr_opt_ PWSTR *ppszAppID,
        _Out_opt_ BOOL *pfPinnable) PURE;
    STDMETHOD(IsPinnable)(
        _In_ IDataObject *pdo,
        _In_ DWORD dwFlags) PURE;
    STDMETHOD(Resolve)(
        _In_opt_ HWND hwnd,
        _In_ DWORD dwFlags,
        _In_ PCIDLIST_ABSOLUTE pidl,
        _Outptr_ PIDLIST_ABSOLUTE *ppidlResolved) PURE;
    STDMETHOD(IsPinned)(
        _In_ PCIDLIST_ABSOLUTE pidl) PURE;
    STDMETHOD(GetPinnedItem)(
        _In_ PCIDLIST_ABSOLUTE pidl,
        _Outptr_ PIDLIST_ABSOLUTE *ppidlPinned) PURE;
    STDMETHOD(GetAppIDForPinnedItem)(
        _In_ PCIDLIST_ABSOLUTE pidl,
        _Outptr_ PWSTR *ppszAppID) PURE;
    STDMETHOD(ItemChangeNotify)(
        _In_ PCIDLIST_ABSOLUTE pidlFrom,
        _In_ PCIDLIST_ABSOLUTE pidlTo) PURE;
    STDMETHOD(UpdateForRemovedItemsAsNecessary)() PURE;
};

struct DECLSPEC_UUID("4CD19ADA-25A5-4A32-B3B7-347BEE5BE36B") DECLSPEC_NOVTABLE
IStartMenuPinnedList : public IUnknown
{
    STDMETHOD(RemoveFromList)(
        _In_ IShellItem *psi) PURE;
};

/* Windows 7 slot order; Windows 8 added GetAppIDForShortcutObject and CanPinAppShortcut */
struct DECLSPEC_UUID("46A6EEFF-908E-4DC6-92A6-64BE9177B41C") DECLSPEC_NOVTABLE
IApplicationResolver : public IUnknown
{
    STDMETHOD(GetAppIDForShortcut)(
        _In_ IShellItem *psi,
        _Outptr_ PWSTR *ppszAppID) PURE;
    STDMETHOD(GetAppIDForWindow)(
        _In_ HWND hwnd,
        _Outptr_ PWSTR *ppszAppID,
        _Out_opt_ BOOL *pfPinningPrevented,
        _Out_opt_ BOOL *pfExplicitAppID,
        _Out_opt_ BOOL *pfEmbeddedShortcutValid) PURE;
    STDMETHOD(GetAppIDForProcess)(
        _In_ DWORD dwProcessId,
        _Outptr_ PWSTR *ppszAppID,
        _Out_opt_ BOOL *pfPinningPrevented,
        _Out_opt_ BOOL *pfExplicitAppID,
        _Out_opt_ BOOL *pfEmbeddedShortcutValid) PURE;
    STDMETHOD(GetShortcutForProcess)(
        _In_ DWORD dwProcessId,
        _Outptr_ IShellItem **ppsi) PURE;
    STDMETHOD(GetBestShortcutForAppID)(
        _In_ PCWSTR pszAppID,
        _Outptr_ IShellItem **ppsi) PURE;
    STDMETHOD(GetBestShortcutAndAppIDForAppPath)(
        _In_ PCWSTR pszAppPath,
        _Outptr_opt_ IShellItem **ppsi,
        _Outptr_opt_ PWSTR *ppszAppID) PURE;
    STDMETHOD(CanPinApp)(
        _In_ IShellItem *psi) PURE;
    STDMETHOD(GetRelaunchProperties)(
        _In_ HWND hwnd,
        _Outptr_opt_ PWSTR *ppszAppID,
        _Outptr_opt_ PWSTR *ppszCmdLine,
        _Outptr_opt_ PWSTR *ppszIconResource,
        _Outptr_opt_ PWSTR *ppszDisplayNameResource,
        _Out_opt_ BOOL *pfPinnable) PURE;
    STDMETHOD(GenerateShortcutFromWindowProperties)(
        _In_ HWND hwnd,
        _Outptr_ IShellItem **ppsi) PURE;
    STDMETHOD(GenerateShortcutFromItemProperties)(
        _In_ IShellItem2 *psi,
        _Outptr_ IShellItem **ppsi) PURE;
};

/* Explorer owns the change notification registrations the resolver asks for */
struct DECLSPEC_NOVTABLE IAppResolverNotify : public IUnknown
{
    STDMETHOD(RegisterNotification)(
        _In_ UINT uId,
        _In_ LONG lEvents,
        _In_ PCIDLIST_ABSOLUTE pidl,
        _In_ BOOL fRecursive) PURE;
    STDMETHOD(UnregisterNotification)(
        _In_ UINT uId) PURE;
    STDMETHOD(AddHotkey)(
        _In_ WORD wHotkey,
        _In_ PCIDLIST_ABSOLUTE pidlFolder,
        _In_ PCUITEMID_CHILD pidlItem,
        _In_ BOOL fAdd) PURE;
};

struct DECLSPEC_UUID("BE1AE761-D62A-49FB-A8A1-B871A5890F71") DECLSPEC_NOVTABLE
IStartMenuItemsCacheNotify : public IUnknown
{
    STDMETHOD(ChangeNotify)(
        _In_ int iEvent) PURE;
};

/* Usage data kept by the user assist store */
typedef struct _UA_USAGE
{
    UINT cbSize;
    DWORD dwMask;
    FLOAT flScore;
    UINT cLaunches;
    UINT cSwitches;
    DWORD dwFocusTime;
    FILETIME ftLastUsed;
    BOOL fExcluded;
} UA_USAGE, *PUA_USAGE;

/* UA_USAGE::dwMask bits the MFU list asks for: last use, relevance and exclusion */
#define UA_USAGE_LASTUSED       0x01
#define UA_USAGE_SCORE          0x10
#define UA_USAGE_EXCLUDED       0x20

/* One pinned or frequently used program as the start menu receives it */
typedef struct _SM_PROGRAM_ITEM
{
    PIDLIST_ABSOLUTE pidlFolder;
    PIDLIST_RELATIVE pidlItem;
    PWSTR pszAppID;
    UA_USAGE Usage;
    int iPinIndex;
    BOOL fNew;
} SM_PROGRAM_ITEM, *PSM_PROGRAM_ITEM;

/* iPinIndex of the entry that separates pinned programs from the rest */
#define SM_PROGRAM_SEPARATOR    (-2)

struct DECLSPEC_UUID("767B826A-2AA5-4AB9-BE6F-AC0642FEAA06") DECLSPEC_NOVTABLE
IEnumStartMenuItems : public IUnknown
{
    STDMETHOD(Next)(
        _In_ ULONG celt,
        _Out_writes_to_(celt, *pceltFetched) SM_PROGRAM_ITEM *rgelt,
        _Out_opt_ ULONG *pceltFetched) PURE;
    STDMETHOD(Skip)(
        _In_ ULONG celt) PURE;
    STDMETHOD(Reset)() PURE;
    STDMETHOD(Clone)(
        _Outptr_ IEnumStartMenuItems **ppenum) PURE;
};

struct DECLSPEC_UUID("05A232FD-2BFB-4349-9D48-4787F317F50A") DECLSPEC_NOVTABLE
IStartMenuItemsCache : public IUnknown
{
    STDMETHOD(OnChangeNotify)(
        _In_ UINT uId,
        _In_ LONG lEvent,
        _In_opt_ PCIDLIST_ABSOLUTE pidl1,
        _In_opt_ PCIDLIST_ABSOLUTE pidl2) PURE;
    STDMETHOD(PinListChanged)() PURE;
    STDMETHOD(GetPinnedItemsCount)(
        _Out_ int *pcPinned) PURE;
    STDMETHOD(GetStartMenuMFUList)(
        _In_ UINT cMax,
        _Outptr_ IEnumStartMenuItems **ppenumItems,
        _Outptr_ IEnumString **ppenumNewApps,
        _Out_ FILETIME *pftNewest) PURE;
    STDMETHOD(RegisterSMNotify)(
        _In_opt_ IStartMenuItemsCacheNotify *pNotify) PURE;
    STDMETHOD(RegisterARNotify)(
        _In_opt_ IAppResolverNotify *pNotify) PURE;
    STDMETHOD(SetAltName)(
        _In_ PCUIDLIST_RELATIVE pidl,
        _In_ PCWSTR pszName,
        _Outptr_ PIDLIST_RELATIVE *ppidlNew) PURE;
    STDMETHOD(GetAltName)(
        _In_ PCUIDLIST_RELATIVE pidl,
        _Outptr_ PWSTR *ppszName) PURE;
};

static const IID IID_IEnumFullIDList = __uuidof(IEnumFullIDList);
static const IID IID_IStartMenuPinnedList = __uuidof(IStartMenuPinnedList);
static const IID IID_IApplicationResolver = __uuidof(IApplicationResolver);
static const IID IID_IStartMenuItemsCacheNotify = __uuidof(IStartMenuItemsCacheNotify);
static const IID IID_IEnumStartMenuItems = __uuidof(IEnumStartMenuItems);
static const IID IID_IStartMenuItemsCache = __uuidof(IStartMenuItemsCache);

/* Application model properties of shortcuts and windows */
#define PIN_DEFINE_APPMODEL_KEY(name, pid)     static const PROPERTYKEY name =         { { 0x9f4c2855, 0x9f79, 0x4b39, { 0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3 } }, pid }

PIN_DEFINE_APPMODEL_KEY(PKEY_AppUserModel_RelaunchCommand, 2);
PIN_DEFINE_APPMODEL_KEY(PKEY_AppUserModel_RelaunchIconResource, 3);
PIN_DEFINE_APPMODEL_KEY(PKEY_AppUserModel_RelaunchDisplayNameResource, 4);
PIN_DEFINE_APPMODEL_KEY(PKEY_AppUserModel_ID, 5);
PIN_DEFINE_APPMODEL_KEY(PKEY_AppUserModel_ExcludeFromShowInNewInstall, 8);
PIN_DEFINE_APPMODEL_KEY(PKEY_AppUserModel_PreventPinning, 9);

static const PROPERTYKEY PKEY_Link_Arguments =
    { { 0x436f2667, 0x14e2, 0x4feb, { 0xb3, 0x0a, 0x14, 0x6c, 0x53, 0xb5, 0xb6, 0x74 } }, 100 };

/* shell32 exports used here without a header */
EXTERN_C
LPITEMIDLIST
WINAPI
SHLogILFromFSIL(
    _In_ LPITEMIDLIST pidl);

EXTERN_C
HRESULT
WINAPI
SHCreateItemInKnownFolder(
    _In_ REFKNOWNFOLDERID kfid,
    _In_ DWORD dwKFFlags,
    _In_opt_ PCWSTR pszItem,
    _In_ REFIID riid,
    _Outptr_ void **ppv);

EXTERN_C
HRESULT
WINAPI
SHGetPropertyStoreForWindow(
    _In_ HWND hwnd,
    _In_ REFIID riid,
    _Outptr_ void **ppv);

EXTERN_C
BOOL
WINAPI
SHGetPathFromIDListEx(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _Out_writes_(cchPath) PWSTR pszPath,
    _In_ DWORD cchPath,
    _In_ GPFIDL_FLAGS uOpts);

/* Path and ID list helpers */
EXTERN_C
HRESULT
WINAPI
SHCompareIDsFull(
    _In_ LPARAM lParam,
    _In_ PCUIDLIST_RELATIVE pidl1,
    _In_ PCUIDLIST_RELATIVE pidl2,
    _Out_opt_ int *piResult);

/* Results of PathComparePaths */
#define PCP_UNRELATED   0x1
#define PCP_EQUAL       0x2
#define PCP_IMMEDIATE   0x4
#define PCP_CHILD       0x8

EXTERN_C
int
WINAPI
PathComparePaths(
    _In_ PCWSTR pszFolder,
    _In_ PCWSTR pszPath);

BOOL
ILIsEqualByPath(
    _In_ PCIDLIST_ABSOLUTE pidl1,
    _In_ PCIDLIST_ABSOLUTE pidl2);

BOOL
ILIsParentByPath(
    _In_ PCIDLIST_ABSOLUTE pidlParent,
    _In_ PCIDLIST_ABSOLUTE pidlChild,
    _In_ BOOL fImmediate);

VOID
ILExpungeRemovedHiddenIDs(
    _Inout_ LPITEMIDLIST pidl);

DWORD
SetLinkFlags(
    _In_ IShellLinkW *psl,
    _In_ DWORD dwFlags,
    _In_ DWORD dwMask);

HRESULT
GetPathOrDarwinID(
    _In_ IShellLinkW *psl,
    _Out_writes_(MAX_PATH) PWSTR pszPath,
    _Outptr_opt_ PWSTR *ppszArgs);

HRESULT
SimpleToRealIDList(
    _In_ PCIDLIST_ABSOLUTE pidl,
    _Outptr_ PIDLIST_ABSOLUTE *ppidlReal);

HRESULT
PinGetIDListFromDataObject(
    _In_ IDataObject *pdo,
    _Outptr_ PIDLIST_ABSOLUTE *ppidl);

HRESULT
CreateBindCtxWithParam(
    _In_ PCWSTR pszParam,
    _Outptr_ IBindCtx **ppbc);

HRESULT
PathGetLongPath(
    _In_ PCWSTR pszPath,
    _Out_writes_(cchLong) PWSTR pszLong,
    _In_ DWORD cchLong);

BOOL
IsExplorerProcess(VOID);

VOID
PinSendChangeNotify(
    _In_ LONG lEvent,
    _In_ PCIDLIST_ABSOLUTE pidl);

BOOL
PinPolicyEnabled(
    _In_ UINT uPolicy);

#define PIN_POLICY_NOPINNINGTOTASKBAR       0
#define PIN_POLICY_TASKBARNOPINNEDLIST      1
#define PIN_POLICY_NOPINNINGTODESTINATIONS  2
#define PIN_POLICY_NOSTARTMENUSUBFOLDERS    3

UINT
GetExeOrMscExtension(
    _In_ PCWSTR pszPath);

/* Application ID helpers */
VOID
EncodeCommonRelativePath(
    _Out_writes_(cchOut) PWSTR pszOut,
    _In_ SIZE_T cchOut,
    _In_ PCWSTR pszPath);

HRESULT
GenerateAppIDWorker(
    _In_ PCWSTR pszEncodedPath,
    _In_opt_ PCWSTR pszArgs,
    _In_opt_ PCWSTR pszName,
    _Outptr_ PWSTR *ppszAppID);

HRESULT
GenerateAppID(
    _In_ PCWSTR pszPath,
    _In_opt_ PCWSTR pszArgs,
    _In_opt_ PCWSTR pszName,
    _Outptr_ PWSTR *ppszAppID);

HRESULT
GetAppPathFromShortcut(
    _In_ IShellLinkW *psl,
    _In_opt_ PCIDLIST_ABSOLUTE pidlLink,
    _Out_writes_(cchPath) PWSTR pszPath,
    _In_ DWORD cchPath);

HRESULT
GetAppPathFromLink(
    _In_ IUnknown *punkItem,
    _Out_writes_(cchPath) PWSTR pszPath,
    _In_ DWORD cchPath);

EXTERN_C
HRESULT
WINAPI
GetAppIDRoot(
    _In_ IShellItem *psi,
    _In_ REFIID riid,
    _Outptr_ void **ppv,
    _Out_ PROPVARIANT *ppropvarAppID);

HRESULT
GetAppIDFromIDList(
    _In_ IApplicationResolver *par,
    _In_ PCIDLIST_ABSOLUTE pidl,
    _Outptr_ PWSTR *ppszAppID,
    _Out_opt_ BOOL *pfCached);

HRESULT
CacheAppIDInIDList(
    _In_ PIDLIST_ABSOLUTE pidl,
    _Outptr_ PIDLIST_ABSOLUTE *ppidlOut);

HRESULT
DetermineFolderDestinationParentAppID(
    _In_ IApplicationResolver *par,
    _In_ IShellItem *psiFolder,
    _Outptr_opt_ IShellItem **ppsiApplication,
    _Outptr_ PWSTR *ppszAppID,
    _Out_opt_ BOOL *pfExplorer);

HRESULT
MakeDestinationItem(
    _In_ IShellItem *psiDestination,
    _In_opt_ IApplicationResolver *par,
    _In_ PCWSTR pszAppID,
    _In_opt_ PCWSTR pszAppPath);

HRESULT
GetBoolProperty(
    _In_ IPropertyStore *pps,
    _In_ REFPROPERTYKEY key,
    _Out_ BOOL *pfValue);

HRESULT
GetStringProperty(
    _In_ IPropertyStore *pps,
    _In_ REFPROPERTYKEY key,
    _Outptr_ PWSTR *ppszValue);

VOID
GetShortcutAppProperties(
    _In_ IShellLinkW *psl,
    _Outptr_result_maybenull_ PWSTR *ppszAppID,
    _Out_ BOOL *pfPreventPinning,
    _Out_opt_ BOOL *pfExcludeFromNew);

BOOL
IsInterestingAppID(
    _In_ PCWSTR pszAppID);
