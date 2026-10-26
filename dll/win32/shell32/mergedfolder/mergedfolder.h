/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Shell folder that merges several namespaces into one view
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <shdeprecated.h>

/* Exports these classes use that the headers do not declare */
EXTERN_C BOOL WINAPI DPA_Merge(HDPA hdpaDest, HDPA hdpaSrc, DWORD dwFlags, PFNDPACOMPARE pfnCompare,
                               PFNDPAMERGE pfnMerge, LPARAM lParam);
EXTERN_C HRESULT WINAPI SHGetFolderPathEx(REFKNOWNFOLDERID rfid, DWORD dwFlags, HANDLE hToken,
                                          LPWSTR pszPath, UINT cchPath);
EXTERN_C int WINAPI SHGetIconOverlayIndexW(LPCWSTR pszIconPath, int iIconIndex);
EXTERN_C BOOL WINAPI SHGetPathFromIDListEx(PCIDLIST_ABSOLUTE pidl, PWSTR pszPath, DWORD cchPath,
                                           GPFIDL_FLAGS uOpts);
EXTERN_C HRESULT WINAPI SHCompareIDsFull(LPARAM lParam, PCUIDLIST_RELATIVE pidl1,
                                         PCUIDLIST_RELATIVE pidl2, int *piResult);
EXTERN_C HRESULT WINAPI SHCreateItemWithParent(PCIDLIST_ABSOLUTE pidlParent,
                                               IShellFolder *psfParent, PCUITEMID_CHILD pidl,
                                               REFIID riid, void **ppvItem);

#ifndef GPFIDL_UNCPRINTER
#define GPFIDL_UNCPRINTER   0x2
#endif

EXTERN_C const CLSID CLSID_ProgramsFolderAndFastItems;
EXTERN_C const CLSID CLSID_StartMenuGroupPolicyFilter;
EXTERN_C const CLSID CLSID_ProgramsFolder;

/* Name of the merged child item a parsing bind context carries */
#define STR_MERGEDFOLDER_CHILDITEM  L"MergedFolderChildItem"

/* Namespace flags given to AddNameSpace */
#define MFNS_SORTAFTER          0x0001  /* Items of this namespace sort after the others */
#define MFNS_GROUPED            0x0002  /* Merge only with namespaces of the same group */
#define MFNS_COMMON             0x0004  /* All users location */
#define MFNS_FOLDERSLAST        0x0008  /* Folders sort after the other items */
#define MFNS_DEFAULT            0x0100  /* Takes new items, renames and storage */
#define MFNS_VIEW               0x0400  /* Creates the view and drop target */
#define MFNS_ATTRIBUTES         0x0800  /* Answers attribute queries */
#define MFNS_DISPLAY            0x1000  /* Answers names, columns and comparisons */
#define MFNS_UIOBJECT           0x2000  /* Creates the item UI objects */

/* QueryNameSpace2 mask */
#define MFQNS_FLAGS             0x0001
#define MFQNS_FOLDER            0x0002
#define MFQNS_GUID              0x0004
#define MFQNS_IDLIST            0x0008
#define MFQNS_GROUPID           0x0010

/* GetFlagsForChild result */
#define MFCF_IMPLICIT           0x0001

/* Entry flag of a merged child for a namespace added by name instead of by enumeration */
#define MERGEDENTRY_IMPLICIT    0x0001

typedef struct _MERGEDNAMESPACEINFO
{
    DWORD cbSize;
    DWORD dwMask;
    DWORD dwFlags;
    IShellFolder *psf;
    GUID guidObject;
    PIDLIST_ABSOLUTE pidl;
    UINT uGroupID;
} MERGEDNAMESPACEINFO;

struct DECLSPEC_UUID("2F711B17-773C-41D4-93FA-7F23EDCECB66") DECLSPEC_NOVTABLE
IAugmentedMergedFolder : public IShellFolder
{
    STDMETHOD(AddNameSpace)(
        _In_opt_ const GUID *pguidObject,
        _In_opt_ IShellFolder *psf,
        _In_opt_ PCIDLIST_ABSOLUTE pidl,
        _In_ DWORD dwFlags,
        _In_ UINT uGroupID) PURE;
    STDMETHOD(GetNameSpaceID)(
        _In_ PCUITEMID_CHILD pidl,
        _Out_ GUID *pguidOut) PURE;
    STDMETHOD(QueryNameSpace)(
        _In_ ULONG iNamespace,
        _Out_opt_ GUID *pguidOut,
        _Outptr_opt_ IShellFolder **ppsf) PURE;
    STDMETHOD(EnumNameSpace)(
        _In_ ULONG iNamespace,
        _Out_ ULONG *piNamespace) PURE;
    STDMETHOD(UnWrapIDList)(
        _In_ PCUITEMID_CHILD pidl,
        _In_ LONG cMax,
        _Out_writes_opt_(cMax) IShellFolder **apsf,
        _Out_writes_opt_(cMax) PIDLIST_ABSOLUTE *apidlFolder,
        _Out_writes_opt_(cMax) PITEMID_CHILD *apidlItem,
        _Out_opt_ LONG *pcFetched) PURE;
    STDMETHOD(QueryNameSpace2)(
        _In_ ULONG iNamespace,
        _Inout_ MERGEDNAMESPACEINFO *pqnsi) PURE;
    STDMETHOD(GetNameSpaceGroupID)(
        _In_ PCUITEMID_CHILD pidl,
        _Out_ UINT *puGroupID) PURE;
    STDMETHOD(GetFlagsForChild)(
        _In_ PCUITEMID_CHILD pidl,
        _In_ REFGUID rguidObject,
        _Out_ DWORD *pdwFlags) PURE;
};

struct DECLSPEC_UUID("2778C4AC-05B6-4605-9A3F-89748E0B9164") DECLSPEC_NOVTABLE
ITranslateShellChangeNotify : public IUnknown
{
    STDMETHOD(TranslateIDs)(
        _Inout_ LONG *plEvent,
        _In_opt_ PCIDLIST_ABSOLUTE pidl1,
        _In_opt_ PCIDLIST_ABSOLUTE pidl2,
        _Out_ PIDLIST_ABSOLUTE *ppidlOut1,
        _Out_ PIDLIST_ABSOLUTE *ppidlOut2,
        _Out_ LONG *plEvent2,
        _Out_ PIDLIST_ABSOLUTE *ppidlOutEvent2Pidl1,
        _Out_ PIDLIST_ABSOLUTE *ppidlOutEvent2Pidl2) PURE;
    STDMETHOD(IsChildID)(
        _In_ PCIDLIST_ABSOLUTE pidlKid,
        _In_ BOOL fImmediate) PURE;
    STDMETHOD(IsEqualID)(
        _In_opt_ PCIDLIST_ABSOLUTE pidl,
        _In_opt_ PCIDLIST_ABSOLUTE pidlParent) PURE;
};

struct DECLSPEC_UUID("053B4A86-0DC9-40A3-B7ED-BC6A2E951F48") DECLSPEC_NOVTABLE
IFolderType : public IUnknown
{
    STDMETHOD(GetFolderType)(
        _Out_ FOLDERTYPEID *pftid) PURE;
};

struct DECLSPEC_UUID("321A6A6A-D61F-4BF3-97AE-14BE2986BB36") DECLSPEC_NOVTABLE
IObjectWithBackReferences : public IUnknown
{
    STDMETHOD(RemoveBackReferences)() PURE;
};

struct DECLSPEC_UUID("148BCE7A-0D60-4254-813F-A4E4EA46DF3F") DECLSPEC_NOVTABLE
ILocalizableItemParent : public IUnknown
{
    STDMETHOD(GetResourceDisplayName)(
        _In_ PCUITEMID_CHILD pidl,
        _Out_writes_(cch) LPWSTR pszResource,
        _In_ UINT cch,
        _Out_opt_ int *piIndex) PURE;
    STDMETHOD(SetResourceDisplayName)(
        _In_ PCUITEMID_CHILD pidl,
        _In_ LPCWSTR pszResource,
        _In_ int iIndex) PURE;
    STDMETHOD(RemoveResourceDisplayName)(
        _In_ PCUITEMID_CHILD pidl) PURE;
};

struct DECLSPEC_UUID("FDBEE76E-F12B-408E-93AB-9BE8521000D9") DECLSPEC_NOVTABLE
IAliasedNamespace : public IUnknown
{
    STDMETHOD(GetRealLocations)(
        _Outptr_ IShellItemArray **ppsia) PURE;
};

struct DECLSPEC_UUID("0681C275-472B-4097-97B3-F19E4875FDC9") DECLSPEC_NOVTABLE
ISetWrapperFolder : public IUnknown
{
    STDMETHOD(SetWrapperFolderWeakReference)(
        _In_opt_ IShellFolder2 *psfWrapper) PURE;
};

struct DECLSPEC_UUID("7D903FCA-D6F9-4810-8332-946C0177E247") DECLSPEC_NOVTABLE
IIdentityName : public IRelatedItem
{
};

typedef enum _MERGEDCATINFO_FLAGS
{
    MERGEDCATINFO_NORMAL = 0
} MERGEDCATINFO_FLAGS;

typedef struct _MERGEDCATEGORY_INFO
{
    MERGEDCATINFO_FLAGS cif;
    WCHAR wszName[260];
} MERGEDCATEGORY_INFO;

struct DECLSPEC_UUID("A3B14589-9174-49A8-89A3-06A1AE2B9BA7") DECLSPEC_NOVTABLE
ICategorizer : public IUnknown
{
    STDMETHOD(GetDescription)(
        _Out_writes_(cch) LPWSTR pszDesc,
        _In_ UINT cch) PURE;
    STDMETHOD(GetCategory)(
        _In_ UINT cidl,
        _In_reads_(cidl) PCUITEMID_CHILD_ARRAY apidl,
        _Out_writes_(cidl) DWORD *rgCategoryIds) PURE;
    STDMETHOD(GetCategoryInfo)(
        _In_ DWORD dwCategoryId,
        _Out_ MERGEDCATEGORY_INFO *pci) PURE;
    STDMETHOD(CompareCategory)(
        _In_ DWORD csfFlags,
        _In_ DWORD dwCategoryId1,
        _In_ DWORD dwCategoryId2) PURE;
};

static const IID IID_IAugmentedMergedFolder = __uuidof(IAugmentedMergedFolder);
static const IID IID_ITranslateShellChangeNotify = __uuidof(ITranslateShellChangeNotify);
static const IID IID_IObjectWithBackReferences = __uuidof(IObjectWithBackReferences);
static const IID IID_IAliasedNamespace = __uuidof(IAliasedNamespace);
static const IID IID_ISetWrapperFolder = __uuidof(ISetWrapperFolder);
static const IID IID_IIdentityName = __uuidof(IIdentityName);
static const IID IID_ICategorizer = __uuidof(ICategorizer);
static const IID IID_ICategoryProvider =
    { 0x9af64809, 0x5864, 0x4c26, { 0xa7, 0x20, 0xc1, 0xf7, 0x8c, 0x08, 0x6e, 0xe3 } };
static const IID IID_IMergedExtractImage =
    { 0xbb2e617c, 0x0920, 0x11d1, { 0x9a, 0x0b, 0x00, 0xc0, 0x4f, 0xc2, 0xd6, 0xc1 } };
static const IID IID_IMergedExtractLogo =
    { 0xd4029ec0, 0x0920, 0x11d1, { 0x9a, 0x0b, 0x00, 0xc0, 0x4f, 0xc2, 0xd6, 0xc1 } };
static const IID IID_IMergedThumbnailProvider =
    { 0xe357fccd, 0xa995, 0x4576, { 0xb0, 0x1f, 0x23, 0x46, 0x30, 0x15, 0x4e, 0x96 } };

/* Drop effects the drop target hands to the folders it forwards to */
typedef struct _MERGEDDROPFOLDER
{
    DWORD dwDropEffect;
    WCHAR wszPath[MAX_PATH];
} MERGEDDROPFOLDER;

typedef struct _MERGEDDROPFOLDERLIST
{
    DWORD dwDefaultDropEffect;
    INT cFolders;
    MERGEDDROPFOLDER aFolders[1];
} MERGEDDROPFOLDERLIST;

#define CFSTR_MERGEDDROPFOLDERLIST  L"DropEffectFolderList"

class CMergedShellFolder;

/* One namespace merged into the folder */
class CMergedFldrNamespace
{
public:
    IShellFolder *m_psf;
    GUID m_guid;
    PIDLIST_ABSOLUTE m_pidl;
    DWORD m_dwFlags;
    UINT m_uGroupID;
    BOOL m_fReadPropertyBag;
    WCHAR m_szLocation[MAX_PATH];
    WCHAR m_szRootPath[MAX_PATH];
    DWORD m_dwAttributes;
    DWORD m_dwAttributeMask;
    DWORD m_dwDropEffect;
    int m_iDefaultOverlay;
    int m_iConflictOverlay;

    CMergedFldrNamespace(
        _In_ BOOL fReadPropertyBag);
    ~CMergedFldrNamespace();

    HRESULT SetNamespace(
        _In_ IShellFolder2 *psfWrapper,
        _In_opt_ const GUID *pguidObject,
        _In_opt_ IShellFolder *psf,
        _In_opt_ PCIDLIST_ABSOLUTE pidl,
        _In_ DWORD dwFlags,
        _In_ UINT uGroupID);
    VOID EnsureReadPropertyBag();
    VOID ReleaseNamespace();
};

/* One enumerated child, shared by all namespaces that hold it */
struct MERGEDOBJECT
{
    DWORD dwAttributes;
    LPWSTR pszName;
    PITEMID_CHILD pidlWrap;
    int iNamespace;
};

class CMergedShellFolder :
    public IShellFolder2,
    public IStorage,
    public IAugmentedMergedFolder,
    public IPersistFolder2,
    public IFolderType,
    public IShellService,
    public ITranslateShellChangeNotify,
    public IObjectWithBackReferences,
    public ILocalizableItemParent,
    public IAliasedNamespace
{
public:
    CMergedShellFolder(
        _In_opt_ CMergedShellFolder *pParent,
        _In_ REFCLSID rclsid,
        _In_ DWORD dwDropEffect);

    // IUnknown
    STDMETHOD(QueryInterface)(
        _In_ REFIID riid,
        _Outptr_ void **ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // IShellFolder
    STDMETHOD(ParseDisplayName)(
        _In_opt_ HWND hwnd,
        _In_opt_ IBindCtx *pbc,
        _In_ LPWSTR pszName,
        _Out_opt_ ULONG *pchEaten,
        _Outptr_ PIDLIST_RELATIVE *ppidl,
        _Inout_opt_ ULONG *pdwAttributes) override;
    STDMETHOD(EnumObjects)(
        _In_opt_ HWND hwnd,
        _In_ SHCONTF grfFlags,
        _Outptr_ IEnumIDList **ppenum) override;
    STDMETHOD(BindToObject)(
        _In_ PCUIDLIST_RELATIVE pidl,
        _In_opt_ IBindCtx *pbc,
        _In_ REFIID riid,
        _Outptr_ void **ppv) override;
    STDMETHOD(BindToStorage)(
        _In_ PCUIDLIST_RELATIVE pidl,
        _In_opt_ IBindCtx *pbc,
        _In_ REFIID riid,
        _Outptr_ void **ppv) override;
    STDMETHOD(CompareIDs)(
        _In_ LPARAM lParam,
        _In_ PCUIDLIST_RELATIVE pidl1,
        _In_ PCUIDLIST_RELATIVE pidl2) override;
    STDMETHOD(CreateViewObject)(
        _In_opt_ HWND hwndOwner,
        _In_ REFIID riid,
        _Outptr_ void **ppv) override;
    STDMETHOD(GetAttributesOf)(
        _In_ UINT cidl,
        _In_reads_opt_(cidl) PCUITEMID_CHILD_ARRAY apidl,
        _Inout_ SFGAOF *rgfInOut) override;
    STDMETHOD(GetUIObjectOf)(
        _In_opt_ HWND hwndOwner,
        _In_ UINT cidl,
        _In_reads_opt_(cidl) PCUITEMID_CHILD_ARRAY apidl,
        _In_ REFIID riid,
        _Reserved_ UINT *rgfReserved,
        _Outptr_ void **ppv) override;
    STDMETHOD(GetDisplayNameOf)(
        _In_opt_ PCUITEMID_CHILD pidl,
        _In_ SHGDNF uFlags,
        _Out_ STRRET *pName) override;
    STDMETHOD(SetNameOf)(
        _In_opt_ HWND hwnd,
        _In_ PCUITEMID_CHILD pidl,
        _In_ LPCWSTR pszName,
        _In_ SHGDNF uFlags,
        _Outptr_opt_ PITEMID_CHILD *ppidlOut) override;

    // IShellFolder2
    STDMETHOD(GetDefaultSearchGUID)(
        _Out_ GUID *pguid) override;
    STDMETHOD(EnumSearches)(
        _Outptr_ IEnumExtraSearch **ppenum) override;
    STDMETHOD(GetDefaultColumn)(
        _In_ DWORD dwRes,
        _Out_ ULONG *pSort,
        _Out_ ULONG *pDisplay) override;
    STDMETHOD(GetDefaultColumnState)(
        _In_ UINT iColumn,
        _Out_ SHCOLSTATEF *pcsFlags) override;
    STDMETHOD(GetDetailsEx)(
        _In_ PCUITEMID_CHILD pidl,
        _In_ const SHCOLUMNID *pscid,
        _Out_ VARIANT *pv) override;
    STDMETHOD(GetDetailsOf)(
        _In_opt_ PCUITEMID_CHILD pidl,
        _In_ UINT iColumn,
        _Out_ SHELLDETAILS *psd) override;
    STDMETHOD(MapColumnToSCID)(
        _In_ UINT iColumn,
        _Out_ SHCOLUMNID *pscid) override;

    // IStorage
    STDMETHOD(CreateStream)(
        _In_ LPCOLESTR pwcsName,
        _In_ DWORD grfMode,
        _In_ DWORD reserved1,
        _In_ DWORD reserved2,
        _Outptr_ IStream **ppstm) override;
    STDMETHOD(OpenStream)(
        _In_ LPCOLESTR pwcsName,
        _Reserved_ void *reserved1,
        _In_ DWORD grfMode,
        _In_ DWORD reserved2,
        _Outptr_ IStream **ppstm) override;
    STDMETHOD(CreateStorage)(
        _In_ LPCOLESTR pwcsName,
        _In_ DWORD grfMode,
        _In_ DWORD reserved1,
        _In_ DWORD reserved2,
        _Outptr_ IStorage **ppstg) override;
    STDMETHOD(OpenStorage)(
        _In_opt_ LPCOLESTR pwcsName,
        _In_opt_ IStorage *pstgPriority,
        _In_ DWORD grfMode,
        _In_opt_ SNB snbExclude,
        _In_ DWORD reserved,
        _Outptr_ IStorage **ppstg) override;
    STDMETHOD(CopyTo)(
        _In_ DWORD ciidExclude,
        _In_reads_opt_(ciidExclude) const IID *rgiidExclude,
        _In_opt_ SNB snbExclude,
        _In_ IStorage *pstgDest) override;
    STDMETHOD(MoveElementTo)(
        _In_ LPCOLESTR pwcsName,
        _In_ IStorage *pstgDest,
        _In_ LPCOLESTR pwcsNewName,
        _In_ DWORD grfFlags) override;
    STDMETHOD(Commit)(
        _In_ DWORD grfCommitFlags) override;
    STDMETHOD(Revert)() override;
    STDMETHOD(EnumElements)(
        _Reserved_ DWORD reserved1,
        _Reserved_ void *reserved2,
        _Reserved_ DWORD reserved3,
        _Outptr_ IEnumSTATSTG **ppenum) override;
    STDMETHOD(DestroyElement)(
        _In_ LPCOLESTR pwcsName) override;
    STDMETHOD(RenameElement)(
        _In_ LPCOLESTR pwcsOldName,
        _In_ LPCOLESTR pwcsNewName) override;
    STDMETHOD(SetElementTimes)(
        _In_opt_ LPCOLESTR pwcsName,
        _In_opt_ const FILETIME *pctime,
        _In_opt_ const FILETIME *patime,
        _In_opt_ const FILETIME *pmtime) override;
    STDMETHOD(SetClass)(
        _In_ REFCLSID clsid) override;
    STDMETHOD(SetStateBits)(
        _In_ DWORD grfStateBits,
        _In_ DWORD grfMask) override;
    STDMETHOD(Stat)(
        _Out_ STATSTG *pstatstg,
        _In_ DWORD grfStatFlag) override;

    // IAugmentedMergedFolder
    STDMETHOD(AddNameSpace)(
        _In_opt_ const GUID *pguidObject,
        _In_opt_ IShellFolder *psf,
        _In_opt_ PCIDLIST_ABSOLUTE pidl,
        _In_ DWORD dwFlags,
        _In_ UINT uGroupID) override;
    STDMETHOD(GetNameSpaceID)(
        _In_ PCUITEMID_CHILD pidl,
        _Out_ GUID *pguidOut) override;
    STDMETHOD(QueryNameSpace)(
        _In_ ULONG iNamespace,
        _Out_opt_ GUID *pguidOut,
        _Outptr_opt_ IShellFolder **ppsf) override;
    STDMETHOD(EnumNameSpace)(
        _In_ ULONG iNamespace,
        _Out_ ULONG *piNamespace) override;
    STDMETHOD(UnWrapIDList)(
        _In_ PCUITEMID_CHILD pidl,
        _In_ LONG cMax,
        _Out_writes_opt_(cMax) IShellFolder **apsf,
        _Out_writes_opt_(cMax) PIDLIST_ABSOLUTE *apidlFolder,
        _Out_writes_opt_(cMax) PITEMID_CHILD *apidlItem,
        _Out_opt_ LONG *pcFetched) override;
    STDMETHOD(QueryNameSpace2)(
        _In_ ULONG iNamespace,
        _Inout_ MERGEDNAMESPACEINFO *pqnsi) override;
    STDMETHOD(GetNameSpaceGroupID)(
        _In_ PCUITEMID_CHILD pidl,
        _Out_ UINT *puGroupID) override;
    STDMETHOD(GetFlagsForChild)(
        _In_ PCUITEMID_CHILD pidl,
        _In_ REFGUID rguidObject,
        _Out_ DWORD *pdwFlags) override;

    // IPersist
    STDMETHOD(GetClassID)(
        _Out_ CLSID *pclsid) override;

    // IPersistFolder
    STDMETHOD(Initialize)(
        _In_ PCIDLIST_ABSOLUTE pidl) override;

    // IPersistFolder2
    STDMETHOD(GetCurFolder)(
        _Outptr_ PIDLIST_ABSOLUTE *ppidl) override;

    // IFolderType
    STDMETHOD(GetFolderType)(
        _Out_ FOLDERTYPEID *pftid) override;

    // IShellService
    STDMETHOD(SetOwner)(
        _In_opt_ IUnknown *punkOwner) override;

    // ITranslateShellChangeNotify
    STDMETHOD(TranslateIDs)(
        _Inout_ LONG *plEvent,
        _In_opt_ PCIDLIST_ABSOLUTE pidl1,
        _In_opt_ PCIDLIST_ABSOLUTE pidl2,
        _Out_ PIDLIST_ABSOLUTE *ppidlOut1,
        _Out_ PIDLIST_ABSOLUTE *ppidlOut2,
        _Out_ LONG *plEvent2,
        _Out_ PIDLIST_ABSOLUTE *ppidlOutEvent2Pidl1,
        _Out_ PIDLIST_ABSOLUTE *ppidlOutEvent2Pidl2) override;
    STDMETHOD(IsChildID)(
        _In_ PCIDLIST_ABSOLUTE pidlKid,
        _In_ BOOL fImmediate) override;
    STDMETHOD(IsEqualID)(
        _In_opt_ PCIDLIST_ABSOLUTE pidl,
        _In_opt_ PCIDLIST_ABSOLUTE pidlParent) override;

    // IObjectWithBackReferences
    STDMETHOD(RemoveBackReferences)() override;

    // ILocalizableItemParent
    STDMETHOD(GetResourceDisplayName)(
        _In_ PCUITEMID_CHILD pidl,
        _Out_writes_(cch) LPWSTR pszResource,
        _In_ UINT cch,
        _Out_opt_ int *piIndex) override;
    STDMETHOD(SetResourceDisplayName)(
        _In_ PCUITEMID_CHILD pidl,
        _In_ LPCWSTR pszResource,
        _In_ int iIndex) override;
    STDMETHOD(RemoveResourceDisplayName)(
        _In_ PCUITEMID_CHILD pidl) override;

    // IAliasedNamespace
    STDMETHOD(GetRealLocations)(
        _Outptr_ IShellItemArray **ppsia) override;

    /* Helpers shared with the enumerator, drop target and categorizer */
    HRESULT GetSubPidl(
        _In_ PCUITEMID_CHILD pidl,
        _In_ UINT iEntry,
        _Out_opt_ UINT *piNamespace,
        _Out_opt_ WORD *pwEntryFlags,
        _Outptr_opt_ PITEMID_CHILD *ppidlSub,
        _Outptr_opt_ CMergedFldrNamespace **ppns);
    HRESULT GetNamespace(
        _In_ int iNamespace,
        _Outptr_ CMergedFldrNamespace **ppns);
    HRESULT FindNamespace(
        _In_ DWORD dwMask,
        _In_ DWORD dwValue,
        _Outptr_ CMergedFldrNamespace **ppns);
    int GetNamespaceCount() const;
    UINT AcquireObjects();
    MERGEDOBJECT *GetObject(
        _In_ int iObject);
    BOOL HasObjects() const
    {
        return m_hdpaObjects != NULL;
    }
    int GetObjectCount() const
    {
        return m_hdpaObjects ? DPA_GetPtrCount(m_hdpaObjects) : 0;
    }
    BOOL IsFolder(
        _In_ PCUITEMID_CHILD pidl);
    VOID AddAllOtherNamespacesIfNeeded(
        _Inout_ PITEMID_CHILD *ppidlWrap);
    BOOL IsCDBurn() const
    {
        return m_fCDBurn;
    }
    DWORD GetDefaultDropEffect() const
    {
        return m_dwDropEffect;
    }
    PCIDLIST_ABSOLUTE GetPidl() const
    {
        return m_pidl;
    }

protected:
    virtual ~CMergedShellFolder();

    /* Overridable by folders built on this one */
    virtual HRESULT DeleteItemByIDList(
        _In_ PCUITEMID_CHILD pidl);
    virtual HRESULT StgCreate(
        _In_ PCUITEMID_CHILD pidl,
        _In_ DWORD grfMode,
        _In_ REFIID riid,
        _Outptr_ void **ppv);
    virtual HRESULT CreateChild(
        _Outptr_ CMergedShellFolder **ppChild);
    virtual HRESULT TryParseNameWithNamespace(
        _In_ CMergedFldrNamespace *pns,
        _In_opt_ HWND hwnd,
        _In_opt_ IBindCtx *pbc,
        _In_ LPCWSTR pszName,
        _Outptr_ PITEMID_CHILD *ppidl);

private:
    LONG m_cRef;
    PIDLIST_ABSOLUTE m_pidl;
    HDPA m_hdpaNamespaces;
    UINT m_uWhichFolderColumn;
    IStorage *m_pstgDest;
    CLSID m_clsid;
    HDPA m_hdpaObjects;
    CMergedShellFolder *m_pParent;
    BOOL m_fHasGroupedNamespaces;
    BOOL m_fAcquiring;
    BOOL m_fCDBurn;
    DWORD m_dwDropEffect;

    HRESULT SimpleAddNamespace(
        _In_ CMergedFldrNamespace *pns);
    HRESULT NamespaceForItem(
        _In_ PCUITEMID_CHILD pidl,
        _In_ DWORD dwMask,
        _In_ DWORD dwValue,
        _Outptr_opt_ IShellFolder **ppsf,
        _Outptr_opt_ PITEMID_CHILD *ppidlSub,
        _Outptr_opt_ CMergedFldrNamespace **ppns,
        _In_ BOOL fExactOnly);
    HRESULT GetSubPidlForNamespace(
        _In_ PCUITEMID_CHILD pidl,
        _In_ UINT iNamespace,
        _Outptr_ PITEMID_CHILD *ppidlSub);
    BOOL ContainsSrcID(
        _In_ PCUITEMID_CHILD pidl,
        _In_ UINT iNamespace);
    HRESULT CreateWrap(
        _In_ PCUITEMID_CHILD pidl,
        _In_ UINT iNamespace,
        _In_ WORD wEntryFlags,
        _Outptr_ PITEMID_CHILD *ppidlWrap);
    HRESULT WrapAddIDList(
        _In_ PCUITEMID_CHILD pidl,
        _In_ UINT iNamespace,
        _In_ WORD wEntryFlags,
        _Inout_ PITEMID_CHILD *ppidlWrap);
    HRESULT WrapRemoveIDList(
        _In_ PCUITEMID_CHILD pidlWrap,
        _In_ UINT iNamespace,
        _Outptr_ PITEMID_CHILD *ppidlOut);
    HRESULT WrapRemoveIDListAbs(
        _In_ PCIDLIST_ABSOLUTE pidl,
        _In_ UINT iNamespace,
        _Outptr_ PIDLIST_ABSOLUTE *ppidlOut);
    HRESULT GetFolder2(
        _In_opt_ PCUITEMID_CHILD pidl,
        _Outptr_opt_ PITEMID_CHILD *ppidlSub,
        _Outptr_ IShellFolder2 **ppsf);
    HRESULT GetWhichFolderColumn(
        _In_ PCUITEMID_CHILD pidl,
        _Out_writes_(MAX_PATH) LPWSTR pszLocation);
    HRESULT CompareSingleLevelIDs(
        _In_ LPARAM lParam,
        _In_ PCUITEMID_CHILD pidl1,
        _In_ PCUITEMID_CHILD pidl2);
    BOOL ShouldMergeNamespaces(
        _In_ int iNamespace1,
        _In_ int iNamespace2);
    HRESULT GetContextMenu(
        _In_opt_ HWND hwnd,
        _In_ UINT cidl,
        _In_reads_(cidl) PCUITEMID_CHILD_ARRAY apidl,
        _In_ REFIID riid,
        _Outptr_ void **ppv);
    HRESULT GetTargetUIObjectOf(
        _In_opt_ IShellFolder *psf,
        _In_opt_ HWND hwnd,
        _In_ UINT cidl,
        _In_reads_(cidl) PCUITEMID_CHILD_ARRAY apidl,
        _In_ REFIID riid,
        _Outptr_ void **ppv);
    HRESULT GetDestinationStorage(
        _In_ DWORD grfMode,
        _Outptr_ IStorage **ppstg);
    HRESULT ForceParseDisplayName(
        _In_ PCIDLIST_ABSOLUTE pidlAbs,
        _In_ LPCWSTR pszName,
        _In_ BOOL fForce,
        _In_ BOOL fFolder,
        _Out_ BOOL *pfMerged,
        _Outptr_ PITEMID_CHILD *ppidlWrap);
    HRESULT AbsPidlToAbsWrap(
        _In_ CMergedFldrNamespace *pns,
        _In_ PCIDLIST_ABSOLUTE pidl,
        _In_ BOOL fForce,
        _In_ BOOL fFolder,
        _Out_ BOOL *pfMerged,
        _Outptr_ PIDLIST_ABSOLUTE *ppidlOut);
    HRESULT TransformSingleNotify(
        _In_ CMergedFldrNamespace *pns,
        _In_ UINT iNamespace,
        _In_ BOOL fFolder,
        _In_ BOOL fSecond,
        _In_ PCIDLIST_ABSOLUTE pidl,
        _Inout_ LONG *plEvent,
        _Outptr_ PIDLIST_ABSOLUTE *ppidlOut1,
        _Outptr_ PIDLIST_ABSOLUTE *ppidlOut2);
    BOOL IsChildIDInternal(
        _In_ PCIDLIST_ABSOLUTE pidl,
        _In_ BOOL fImmediate,
        _Out_opt_ int *piNamespace);
    HRESULT ParseAndVerify(
        _In_ LPCWSTR pszName,
        _In_opt_ IBindCtx *pbc,
        _Outptr_ PITEMID_CHILD *ppidl);
    HRESULT BindByName(
        _In_ LPCWSTR pszName,
        _In_ DWORD grfMode,
        _In_ REFIID riid,
        _Outptr_ void **ppv);
    HRESULT CreateHelper(
        _In_ LPCWSTR pszName,
        _In_ DWORD grfMode,
        _In_ REFIID riid,
        _Outptr_ void **ppv);
    VOID DestroyObjects();

    static int CALLBACK CompareObjects(
        _In_ void *p1,
        _In_ void *p2,
        _In_ LPARAM lParam);
    static void * CALLBACK MergeObjects(
        _In_ UINT uMsg,
        _In_ void *pvDest,
        _In_ void *pvSrc,
        _In_ LPARAM lParam);
    static int CALLBACK DestroyObjectCB(
        _In_ void *p,
        _In_opt_ void *pData);
    static int CALLBACK DestroyNamespaceCB(
        _In_ void *p,
        _In_opt_ void *pData);
    static int CALLBACK SetOwnerCB(
        _In_ void *p,
        _In_opt_ void *pData);
};

HRESULT
CMergedShellFolder_CreateInstance(
    _In_ REFCLSID rclsid,
    _In_ REFIID riid,
    _Outptr_ void **ppv);

HRESULT
CProgramsFolderAndFastItems_CreateInstance(
    _In_ REFIID riid,
    _Outptr_ void **ppv);

HRESULT
CPersonalStartMenu_CreateInstance(
    _In_ REFIID riid,
    _Outptr_ void **ppv);

HRESULT
CStartMenuGroupPolicyFilter_CreateInstance(
    _In_ REFIID riid,
    _Outptr_ void **ppv);

/* Enumerator over the merged children */
class CMergedFldrEnum : public IEnumIDList
{
public:
    CMergedFldrEnum(
        _In_ CMergedShellFolder *pmf,
        _In_ SHCONTF grfFlags,
        _In_ int iCur);

    // IUnknown
    STDMETHOD(QueryInterface)(
        _In_ REFIID riid,
        _Outptr_ void **ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // IEnumIDList
    STDMETHOD(Next)(
        _In_ ULONG celt,
        _Out_writes_to_(celt, *pceltFetched) PITEMID_CHILD *rgelt,
        _Out_opt_ ULONG *pceltFetched) override;
    STDMETHOD(Skip)(
        _In_ ULONG celt) override;
    STDMETHOD(Reset)() override;
    STDMETHOD(Clone)(
        _Outptr_ IEnumIDList **ppenum) override;

private:
    ~CMergedFldrEnum();

    LONG m_cRef;
    CMergedShellFolder *m_pmf;
    SHCONTF m_grfFlags;
    int m_iCur;
};

/* Drop target that forwards to the namespace the data belongs in */
class CMergedFldrDropTarget :
    public IDropTarget,
    public IObjectWithSite
{
public:
    CMergedFldrDropTarget(
        _In_ CMergedShellFolder *pmf,
        _In_opt_ HWND hwnd);

    // IUnknown
    STDMETHOD(QueryInterface)(
        _In_ REFIID riid,
        _Outptr_ void **ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // IDropTarget
    STDMETHOD(DragEnter)(
        _In_ IDataObject *pdtobj,
        _In_ DWORD grfKeyState,
        _In_ POINTL pt,
        _Inout_ DWORD *pdwEffect) override;
    STDMETHOD(DragOver)(
        _In_ DWORD grfKeyState,
        _In_ POINTL pt,
        _Inout_ DWORD *pdwEffect) override;
    STDMETHOD(DragLeave)() override;
    STDMETHOD(Drop)(
        _In_ IDataObject *pdtobj,
        _In_ DWORD grfKeyState,
        _In_ POINTL pt,
        _Inout_ DWORD *pdwEffect) override;

    // IObjectWithSite
    STDMETHOD(SetSite)(
        _In_opt_ IUnknown *punkSite) override;
    STDMETHOD(GetSite)(
        _In_ REFIID riid,
        _Outptr_ void **ppvSite) override;

private:
    ~CMergedFldrDropTarget();

    BOOL IsCommonIDList(
        _In_ PCIDLIST_ABSOLUTE pidl);
    BOOL IsCommonWrappedIDList(
        _In_ IDataObject *pdtobj,
        _In_ IAugmentedMergedFolder *pamf);
    HRESULT SetDropEffectFolders();

    LONG m_cRef;
    CMergedShellFolder *m_pmf;
    IDropTarget *m_pdt;
    IUnknown *m_punkSite;
    IDataObject *m_pdtobj;
    HWND m_hwnd;
    BOOL m_fCommon;
    PIDLIST_ABSOLUTE m_pidlSource;
    DWORD m_grfKeyState;
    DWORD m_dwEffect;
};

HRESULT
CMergedFldrContextMenu_CreateInstance(
    _In_opt_ HWND hwnd,
    _In_ IContextMenu *pcmCommon,
    _In_ IContextMenu *pcmUser,
    _In_ REFIID riid,
    _Outptr_ void **ppv);

/* Groups merged items by the location they come from */
class CMergedCategorizer :
    public ICategorizer,
    public IShellExtInit
{
public:
    CMergedCategorizer();

    // IUnknown
    STDMETHOD(QueryInterface)(
        _In_ REFIID riid,
        _Outptr_ void **ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // ICategorizer
    STDMETHOD(GetDescription)(
        _Out_writes_(cch) LPWSTR pszDesc,
        _In_ UINT cch) override;
    STDMETHOD(GetCategory)(
        _In_ UINT cidl,
        _In_reads_(cidl) PCUITEMID_CHILD_ARRAY apidl,
        _Out_writes_(cidl) DWORD *rgCategoryIds) override;
    STDMETHOD(GetCategoryInfo)(
        _In_ DWORD dwCategoryId,
        _Out_ MERGEDCATEGORY_INFO *pci) override;
    STDMETHOD(CompareCategory)(
        _In_ DWORD csfFlags,
        _In_ DWORD dwCategoryId1,
        _In_ DWORD dwCategoryId2) override;

    // IShellExtInit
    STDMETHOD(Initialize)(
        _In_opt_ PCIDLIST_ABSOLUTE pidlFolder,
        _In_opt_ IDataObject *pdtobj,
        _In_opt_ HKEY hkeyProgID) override;

private:
    ~CMergedCategorizer();

    LONG m_cRef;
    CMergedShellFolder *m_pmf;
};

HRESULT
CMergedFolderViewCB_CreateInstance(
    _In_ CMergedShellFolder *pmf,
    _In_ REFIID riid,
    _Outptr_ void **ppv);

/* Shared helpers */
HRESULT
MergedFolder_GetFolderTargetIDList(
    _In_opt_ IUnknown *punk,
    _Outptr_ PIDLIST_ABSOLUTE *ppidl);

HRESULT
MergedFolder_SetDataObjectBlob(
    _In_ IDataObject *pdtobj,
    _In_ LPCWSTR pszFormat,
    _In_reads_bytes_(cb) const void *pv,
    _In_ UINT cb);
