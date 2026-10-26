/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Command bar: command folder, registry driven commands and their buttons
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* The SDK declares these as enums; commands combine their bits */
DEFINE_ENUM_FLAG_OPERATORS(EXPCMDSTATE)
DEFINE_ENUM_FLAG_OPERATORS(EXPCMDFLAGS)

#ifndef GIL_SHIELD
#define GIL_SHIELD              0x0200
#endif
#ifndef GIL_FORCENOSHIELD
#define GIL_FORCENOSHIELD       0x0400
#endif

/* Private interfaces, named from use */

typedef enum _COMMANDPOSITION
{
    CMDPOSITION_DEFAULT = 0,
    CMDPOSITION_BEFORE = 1,
    CMDPOSITION_AFTER = 2,
    CMDPOSITION_LAST = 3
} COMMANDPOSITION;

MIDL_INTERFACE("CE394D81-3702-4294-9CF0-2F541563D5C0")
IRegDataDrivenCommand : public IUnknown
{
    STDMETHOD(SetKeyAndName)(
        _In_ HKEY hkey,
        _In_ PCWSTR pszName) PURE;
    STDMETHOD(GetKeyAndName)(
        _Out_ HKEY *phkey,
        _Outptr_ PWSTR *ppszName) PURE;
    STDMETHOD(GetPosition)(
        _Out_ COMMANDPOSITION *pPosition,
        _Out_ GUID *pguidRelative) PURE;
    STDMETHOD(InvokeFromContextMenu)(
        _In_ const CMINVOKECOMMANDINFOEX *pici,
        _In_opt_ IShellItemArray *psia) PURE;
};

MIDL_INTERFACE("D960050C-F4E1-4294-AC4B-598913605923")
IObjectWithRegistryKey : public IUnknown
{
    STDMETHOD(SetKey)(
        _In_ HKEY hkey) PURE;
    STDMETHOD(GetKey)(
        _Out_ HKEY *phkey) PURE;
};

/* Window and screen rectangle of the button a command was started from */
MIDL_INTERFACE("8A49A5EA-5E80-46D0-8506-285496E4592E")
ICommandButtonInfo : public IOleWindow
{
    STDMETHOD(GetRect)(
        _Out_ RECT *prc) PURE;
};

MIDL_INTERFACE("48F8A5B1-82B4-47A3-8A7A-A097BC5E349D")
IUICommandEx : public IUICommand
{
    STDMETHOD(GetFlags)(
        _Out_ EXPCMDFLAGS *pdwFlags) PURE;
    STDMETHOD(EnumSubCommands)(
        _COM_Outptr_ IEnumUICommand **ppenum) PURE;
};

EXTERN_C const IID IID_IRegDataDrivenCommand;
EXTERN_C const IID IID_IObjectWithRegistryKey;
EXTERN_C const IID IID_ICommandButtonInfo;
EXTERN_C const IID IID_IUICommand;
EXTERN_C const IID IID_IUICommandEx;
EXTERN_C const IID IID_IEnumUICommand;
EXTERN_C const GUID SID_CommandsPropertyBag;
EXTERN_C const GUID SID_CommandModuleSite;
EXTERN_C const GUID TOID_DVGetCommandState;
EXTERN_C const GUID UICID_ViewControl;
EXTERN_C const GUID UICID_DefaultVerb;
EXTERN_C const GUID UICID_ExplorerOptions;
EXTERN_C const GUID POLID_NoReadingPane;
EXTERN_C const CLSID CLSID_CommandFolder;
EXTERN_C const CLSID CLSID_CommonCommandState;
EXTERN_C const CLSID CLSID_FolderOptionsCommand;
EXTERN_C const CLSID CLSID_SelectAllExecute;
EXTERN_C const CLSID CLSID_RenameExecute;
EXTERN_C const CLSID CLSID_CloseWindowExecute;
EXTERN_C const CLSID CLSID_MenuBarExecute;
EXTERN_C const CLSID CLSID_UndoExecute;
EXTERN_C const CLSID CLSID_RedoExecute;

/* Namespace of the commands that apply to the folder in view */
#define COMMANDFOLDER_PARSENAME L"shell:::{437ff9c0-a07f-4fa0-af80-84b6c6440a16}"

/* Command folder item ids carry the command's canonical name and its slot in the folder */
#define COMMANDITEM_SIGNATURE   0x07062229

/* ImpliedSelectionModel values of a command store entry */
#define SELMODEL_NONE                   0x00
#define SELMODEL_FOLDER                 0x01
#define SELMODEL_ALLITEMS               0x02
#define SELMODEL_BACKGROUND             0x04
#define SELMODEL_TYPEMASK               0x0F
#define SELMODEL_PREFERSELECTION        0x10
#define SELMODEL_FOLDERIFSELECTION      0x20

/* GetSelectionStateFromItemArray results */
#define SELSTATE_NONE                   2
#define SELSTATE_FILE                   3
#define SELSTATE_FOLDER                 4
#define SELSTATE_MULTIPLE               5

#include <pshpack1.h>
typedef struct _COMMANDITEMID
{
    USHORT cb;
    USHORT wReserved;
    DWORD dwSignature;
    GUID guidCanonical;
    UINT iCommand;
} COMMANDITEMID;
#include <poppack.h>

/* What a command shows on its button, read once per refresh */
typedef struct _COMMANDBUTTONDATA
{
    GUID guidCanonical;
    EXPCMDFLAGS dwFlags;
    EXPCMDSTATE dwState;
    PWSTR pszTitle;
    PWSTR pszToolTip;
    int iImage;
} COMMANDBUTTONDATA;

HRESULT
WINAPI
CommandButtonData_Init(
    _Out_ COMMANDBUTTONDATA *pData,
    _In_ IExplorerCommand *pec,
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ BOOL fOkToBeSlow);

VOID
WINAPI
CommandButtonData_Free(
    _Inout_ COMMANDBUTTONDATA *pData);

/* Section data of SFVM_GET_WEBVIEW_TASKS */
typedef struct _WEBVIEWTASKS
{
    IEnumUICommand *penumSpecialTasks;
    IEnumUICommand *penumFolderTasks;
    DWORD dwUpdateFlags;
} WEBVIEWTASKS;

/* Shared helpers (cmdutil.cpp) */

HRESULT
WINAPI
GetSelectionFromSite(
    _In_opt_ IUnknown *punkSite,
    _COM_Outptr_result_maybenull_ IShellItemArray **ppsia);

HRESULT
WINAPI
SHGetItemArrayOfSelection(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ DWORD dwModel,
    _COM_Outptr_ IShellItemArray **ppsia);

HRESULT
WINAPI
SHGetContextMenuOfSelection(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ DWORD dwModel,
    _COM_Outptr_ IContextMenu **ppcm);

HRESULT
WINAPI
SHInvokeCommandOnSelection(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ DWORD dwModel,
    _In_ PCSTR pszVerb);

HRESULT
WINAPI
SHIsVerbAvailableOnSelection(
    _In_opt_ IUnknown *punkSite,
    _In_opt_ IShellItemArray *psia,
    _In_ DWORD dwModel,
    _In_ UINT uFlags,
    _In_ BOOL fAnyState,
    _In_ PCSTR pszVerb,
    _Out_ BOOL *pfAvailable);

HRESULT
WINAPI
IUnknown_QueryCurrentView(
    _In_opt_ IUnknown *punkSite,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv);

HRESULT
WINAPI
IUnknown_SetSelection(
    _In_opt_ IUnknown *punk,
    _In_opt_ IShellItemArray *psia);

HRESULT
WINAPI
IInitializeCommand_InitializeWithHKEY(
    _In_ IUnknown *punk,
    _In_ PCWSTR pszCommandName,
    _In_ HKEY hkey);

HRESULT
WINAPI
CanShowPaneAux(
    _In_ PCWSTR pszVisibleProp,
    _In_ REFGUID rguidPolicy,
    _In_ REFEXPLORERPANE ep,
    _In_ REFGUID rguidBagService,
    _In_opt_ IUnknown *punkSite,
    _Out_ DWORD *pdwState);

UINT
WINAPI
GetSelectionStateFromItemArray(
    _In_opt_ IShellItemArray *psia);

HRESULT
WINAPI
IShellItemArray_GetEstimatedCount(
    _In_ IShellItemArray *psia,
    _Out_ DWORD *pcItems);

HWND
WINAPI
SHOwnerWindowFromSite(
    _In_opt_ IUnknown *punkSite);

HRESULT
WINAPI
SHRegGetGUID(
    _In_ HKEY hkey,
    _In_opt_ PCWSTR pszSubKey,
    _In_ PCWSTR pszValue,
    _Out_ GUID *pguid);

HRESULT
WINAPI
SHRegAllocString(
    _In_ HKEY hkey,
    _In_opt_ PCWSTR pszSubKey,
    _In_opt_ PCWSTR pszValue,
    _Outptr_ PWSTR *ppsz);

BOOL
WINAPI
SHRegValueExists(
    _In_ HKEY hkey,
    _In_opt_ PCWSTR pszSubKey,
    _In_ PCWSTR pszValue);

HRESULT
WINAPI
DUI_GetElementScreenBounds(
    _In_ DirectUI::Element *pe,
    _Out_ RECT *prc);

HRESULT
WINAPI
DUI_SetElementIcon(
    _In_ DirectUI::Element *pe,
    _In_ HICON hicon);

/* Registry data driven commands (regcommand.cpp) */

HRESULT
WINAPI
GetRegDataDrivenCommand(
    _In_ HKEY hkeyStore,
    _In_ PCWSTR pszName,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv);

HRESULT
WINAPI
GetCanonicalCommand(
    _In_ PCWSTR pszName,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv);

HRESULT
WINAPI
GetExplorerCommandEnumeratorForRegKey(
    _In_ HKEY hkey,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv);

class CRegDataDrivenCommand :
    public CObjectWithSite,
    public IObjectWithSelection,
    public IExplorerCommand,
    public IRegDataDrivenCommand,
    public CDUIFrameZeroAlloc
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_ REFIID riid,
        _COM_Outptr_ void **ppv);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP SetSite(IUnknown *punkSite) override;

    STDMETHODIMP SetSelection(IShellItemArray *psia) override;
    STDMETHODIMP GetSelection(REFIID riid, void **ppv) override;

    STDMETHODIMP GetTitle(IShellItemArray *psiItemArray, LPWSTR *ppszName) override;
    STDMETHODIMP GetIcon(IShellItemArray *psiItemArray, LPWSTR *ppszIcon) override;
    STDMETHODIMP GetToolTip(IShellItemArray *psiItemArray, LPWSTR *ppszInfotip) override;
    STDMETHODIMP GetCanonicalName(GUID *pguidCommandName) override;
    STDMETHODIMP GetState(IShellItemArray *psiItemArray, BOOL fOkToBeSlow, EXPCMDSTATE *pCmdState) override;
    STDMETHODIMP Invoke(IShellItemArray *psiItemArray, IBindCtx *pbc) override;
    STDMETHODIMP GetFlags(EXPCMDFLAGS *pFlags) override;
    STDMETHODIMP EnumSubCommands(IEnumExplorerCommand **ppEnum) override;

    STDMETHODIMP SetKeyAndName(HKEY hkey, PCWSTR pszName) override;
    STDMETHODIMP GetKeyAndName(HKEY *phkey, PWSTR *ppszName) override;
    STDMETHODIMP GetPosition(COMMANDPOSITION *pPosition, GUID *pguidRelative) override;
    STDMETHODIMP InvokeFromContextMenu(const CMINVOKECOMMANDINFOEX *pici, IShellItemArray *psia) override;

    HRESULT Initialize(
        _In_ HKEY hkeyStore,
        _In_ PCWSTR pszName);

    CRegDataDrivenCommand();

private:
    virtual ~CRegDataDrivenCommand();

    IShellItemArray *_GetImpliedSelection(
        _In_opt_ IShellItemArray *psia);
    BOOL _HasExtendedSubCommands(
        _Out_opt_ HKEY *phkey);
    HRESULT _Invoke(
        _In_opt_ const CMINVOKECOMMANDINFOEX *pici,
        _In_opt_ IShellItemArray *psia,
        _In_opt_ IBindCtx *pbc);
    HRESULT _CheckAttributes(
        _In_opt_ IShellItemArray *psia,
        _Out_ EXPCMDSTATE *pState);
    HRESULT _CheckIsVerbOnContextMenu(
        _In_opt_ IShellItemArray *psia,
        _In_ BOOL fOkToBeSlow,
        _Out_ EXPCMDSTATE *pState);
    HRESULT _CheckTogglePane(
        _Out_ EXPCMDSTATE *pState);
    HRESULT _CheckEnabledStateFromAQSQuery(
        _In_opt_ IShellItemArray *psia,
        _Out_ EXPCMDSTATE *pState);
    HRESULT _CheckCommandStateHandler(
        _In_opt_ IShellItemArray *psia,
        _In_ BOOL fOkToBeSlow,
        _Out_ EXPCMDSTATE *pState);
    HRESULT _CheckActivationFilters(
        _In_opt_ IShellItemArray *psia,
        _Out_ EXPCMDSTATE *pState);
    HRESULT _TryInvokeOnSelection(
        _In_opt_ IShellItemArray *psia,
        _Out_ BOOL *pfHandled);
    HRESULT _TryOpenControlPanel(
        _Out_ BOOL *pfHandled);
    HRESULT _TryPane(
        _Out_ BOOL *pfHandled);

    LONG _cRef;
    PWSTR _pszName;
    IShellItemArray *_psiaSelection;
    IShellItemArray *_psiaImplied;
    IExplorerCommand *_pecHandler;
    WCHAR _szDelegate[MAX_PATH];
    BOOL _fDelegateIsCommandKey;
    BOOL _fAcceptsUrls;
    HKEY _hkeyCommand;
    HKEY _hkeyStore;
};

/* Lists the commands named by a registry key: a "SubCommands" style list or numbered subkeys */
class CExplorerCommandEnumerator :
    public IEnumExplorerCommand,
    public IObjectWithRegistryKey,
    public CDUIFrameZeroAlloc
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_ REFIID riid,
        _COM_Outptr_ void **ppv);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP Next(ULONG celt, IExplorerCommand **pUICommand, ULONG *pceltFetched) override;
    STDMETHODIMP Skip(ULONG celt) override;
    STDMETHODIMP Reset() override;
    STDMETHODIMP Clone(IEnumExplorerCommand **ppenum) override;

    STDMETHODIMP SetKey(HKEY hkey) override;
    STDMETHODIMP GetKey(HKEY *phkey) override;

private:
    CExplorerCommandEnumerator();
    ~CExplorerCommandEnumerator();

    HRESULT _Initialize();

    LONG _cRef;
    UINT _iNext;
    UINT _iUnordered;
    HKEY _hkey;
    HKEY _hkeyShell;
    HDPA _hdpaNames;
};

/* Chains several command enumerators */
class CEnumExplorerCommandAggregator : public IEnumExplorerCommand, public CDUIFrameZeroAlloc
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_ IEnumExplorerCommand *penumFirst,
        _In_ IEnumExplorerCommand *penumSecond,
        _In_ REFIID riid,
        _COM_Outptr_ void **ppv);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP Next(ULONG celt, IExplorerCommand **pUICommand, ULONG *pceltFetched) override;
    STDMETHODIMP Skip(ULONG celt) override;
    STDMETHODIMP Reset() override;
    STDMETHODIMP Clone(IEnumExplorerCommand **ppenum) override;

private:
    CEnumExplorerCommandAggregator();
    ~CEnumExplorerCommandAggregator();

    LONG _cRef;
    UINT _iCurrent;
    HDPA _hdpaEnums;
};

/* Folder type commands: FolderTypes\{type}\TasksItemsSelected and TasksNoItemsSelected */
class CFolderTypeCommandProvider : public IExplorerCommandProvider, public CDUIFrameZeroAlloc
{
public:
    CFolderTypeCommandProvider(
        _In_ REFGUID rguidFolderType);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetCommands(IUnknown *punkSite, REFIID riid, void **ppv) override;
    STDMETHODIMP GetCommand(REFGUID rguidCommandId, REFIID riid, void **ppv) override;

private:
    LONG _cRef;
    GUID _guidFolderType;
};

/* Legacy web view tasks seen as explorer commands (legacycmd.cpp) */

HRESULT
WINAPI
CreateExplorerCommandFromUICommand(
    _In_ IUICommand *puic,
    _COM_Outptr_ IExplorerCommand **ppec);

HRESULT
WINAPI
CEnumExplorerCommandOnEnumUICommand_CreateInstance(
    _In_ IEnumUICommand *penum,
    _COM_Outptr_ IEnumExplorerCommand **ppenum);

HRESULT
WINAPI
Create_IEnumUICommandWithArray(
    _In_reads_(cCommands) IUnknown **ppunkCommands,
    _In_ UINT cCommands,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv);

HRESULT
WINAPI
EnumCommonTasks(
    _In_opt_ IUnknown *punkSite,
    _COM_Outptr_ IEnumUICommand **ppenum);

HRESULT
WINAPI
LegacyEnumSpecialTasksByType(
    _In_ UINT uType,
    _COM_Outptr_ IEnumUICommand **ppenum);

/* Command folder (commandfolder.cpp) */

HRESULT
WINAPI
_GetCommandFolder(
    _COM_Outptr_ IShellFolder **ppsf);

BOOL
WINAPI
IsRightSideCommand(
    _In_ REFGUID rguidCanonical);

typedef struct _COMMANDFOLDERENTRY
{
    IExplorerCommand *pec;
    DWORD dwEntryFlags;
    BOOL fPositioned;
} COMMANDFOLDERENTRY;

/* Entries kept out of the bar's enumeration */
#define CFEF_HIDDEN     0x80000

class CCommandFolder :
    public IShellFolder2,
    public IPersistFolder2,
    public IContextMenuCB,
    public CObjectWithSite,
    public CDUIFrameZeroAlloc
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_ REFIID riid,
        _COM_Outptr_ void **ppv);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP ParseDisplayName(HWND hwnd, IBindCtx *pbc, LPWSTR pszDisplayName, ULONG *pchEaten,
                                  PIDLIST_RELATIVE *ppidl, ULONG *pdwAttributes) override;
    STDMETHODIMP EnumObjects(HWND hwnd, SHCONTF grfFlags, IEnumIDList **ppenumIDList) override;
    STDMETHODIMP BindToObject(PCUIDLIST_RELATIVE pidl, IBindCtx *pbc, REFIID riid, void **ppv) override;
    STDMETHODIMP BindToStorage(PCUIDLIST_RELATIVE pidl, IBindCtx *pbc, REFIID riid, void **ppv) override;
    STDMETHODIMP CompareIDs(LPARAM lParam, PCUIDLIST_RELATIVE pidl1, PCUIDLIST_RELATIVE pidl2) override;
    STDMETHODIMP CreateViewObject(HWND hwndOwner, REFIID riid, void **ppv) override;
    STDMETHODIMP GetAttributesOf(UINT cidl, PCUITEMID_CHILD_ARRAY apidl, SFGAOF *rgfInOut) override;
    STDMETHODIMP GetUIObjectOf(HWND hwndOwner, UINT cidl, PCUITEMID_CHILD_ARRAY apidl, REFIID riid,
                               UINT *rgfReserved, void **ppv) override;
    STDMETHODIMP GetDisplayNameOf(PCUITEMID_CHILD pidl, SHGDNF uFlags, STRRET *pName) override;
    STDMETHODIMP SetNameOf(HWND hwnd, PCUITEMID_CHILD pidl, LPCWSTR pszName, SHGDNF uFlags,
                           PITEMID_CHILD *ppidlOut) override;

    STDMETHODIMP GetDefaultSearchGUID(GUID *pguid) override;
    STDMETHODIMP EnumSearches(IEnumExtraSearch **ppenum) override;
    STDMETHODIMP GetDefaultColumn(DWORD dwRes, ULONG *pSort, ULONG *pDisplay) override;
    STDMETHODIMP GetDefaultColumnState(UINT iColumn, SHCOLSTATEF *pcsFlags) override;
    STDMETHODIMP GetDetailsEx(PCUITEMID_CHILD pidl, const SHCOLUMNID *pscid, VARIANT *pv) override;
    STDMETHODIMP GetDetailsOf(PCUITEMID_CHILD pidl, UINT iColumn, SHELLDETAILS *psd) override;
    STDMETHODIMP MapColumnToSCID(UINT iColumn, SHCOLUMNID *pscid) override;

    STDMETHODIMP GetClassID(CLSID *pClassID) override;
    STDMETHODIMP Initialize(PCIDLIST_ABSOLUTE pidl) override;
    STDMETHODIMP GetCurFolder(PIDLIST_ABSOLUTE *ppidl) override;

    STDMETHODIMP CallBack(IShellFolder *psf, HWND hwndOwner, IDataObject *pdtobj, UINT uMsg,
                          WPARAM wParam, LPARAM lParam) override;

    STDMETHODIMP SetSite(IUnknown *punkSite) override { return CObjectWithSite::SetSite(punkSite); }
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CObjectWithSite::GetSite(riid, ppvSite); }

    HRESULT GetCommandsFromSite(
        _In_opt_ IUnknown *punkSite);
    HRESULT GetEntry(
        _In_ UINT iEntry,
        _Out_ COMMANDFOLDERENTRY *pEntry);
    UINT GetEntryCount() { return _cEntries; }
    HRESULT CreateItemID(
        _In_ REFGUID rguidCanonical,
        _In_ UINT iCommand,
        _Outptr_ PITEMID_CHILD *ppidl);

private:
    CCommandFolder();
    virtual ~CCommandFolder();

    HRESULT _EnumFolderCommandsForExplorer(
        _In_opt_ IUnknown *punkSite);
    HRESULT _GetView(
        _In_opt_ IUnknown *punkSite,
        _In_ REFIID riid,
        _COM_Outptr_ void **ppv);
    HRESULT _AppendOrganizeCommand(
        _In_opt_ IUnknown *punkSite);
    HRESULT _AppendRightSideCommands(
        _In_opt_ IUnknown *punkSite);
    HRESULT _EnumLegacyCommands(
        _In_ IShellFolderViewCB *psfvcb);
    HRESULT _AppendLegacyCommands(
        _In_ IEnumUICommand *penum);
    HRESULT _AppendCommands(
        _In_ IEnumExplorerCommand *penum);
    HRESULT _AddEntry(
        _In_ IExplorerCommand *pec,
        _In_ DWORD dwEntryFlags);
    HRESULT _InsertEntry(
        _In_ UINT iEntry,
        _In_ const COMMANDFOLDERENTRY *pEntry);
    VOID _RemoveEntry(
        _In_ UINT iEntry);
    VOID _AdjustPosition();
    HRESULT _FindByGUID(
        _In_ REFGUID rguidCanonical,
        _COM_Outptr_opt_ IExplorerCommand **ppec,
        _Out_opt_ UINT *piEntry);
    HRESULT _GetCommandFromItemID(
        _In_ PCUITEMID_CHILD pidl,
        _In_ REFIID riid,
        _COM_Outptr_ void **ppv);
    HRESULT _CreateFolder(
        _In_ PCUITEMID_CHILD pidl,
        _In_ REFIID riid,
        _COM_Outptr_ void **ppv);
    HRESULT _InvokeItem(
        _In_ IDataObject *pdtobj);

    LONG _cRef;
    PIDLIST_ABSOLUTE _pidl;
    COMMANDFOLDERENTRY *_pEntries;
    UINT _cEntries;
    UINT _cAlloc;
};

class CCommandFolderEnum : public IEnumIDList, public CObjectWithSite, public CDUIFrameZeroAlloc
{
public:
    CCommandFolderEnum(
        _In_ CCommandFolder *pFolder,
        _In_ SHCONTF grfFlags);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP Next(ULONG celt, PITEMID_CHILD *rgelt, ULONG *pceltFetched) override;
    STDMETHODIMP Skip(ULONG celt) override;
    STDMETHODIMP Reset() override;
    STDMETHODIMP Clone(IEnumIDList **ppenum) override;

    STDMETHODIMP SetSite(IUnknown *punkSite) override { return CObjectWithSite::SetSite(punkSite); }
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CObjectWithSite::GetSite(riid, ppvSite); }

private:
    virtual ~CCommandFolderEnum();

    BOOL _Next(
        _Outptr_ PITEMID_CHILD *ppidl);

    LONG _cRef;
    CCommandFolder *_pFolder;
    UINT _iNext;
    SHCONTF _grfFlags;
};

/* Drop-down menu built from a command folder (foldermenu.cpp) */

HRESULT
WINAPI
ShowFolderMenu(
    _In_ IShellFolder *psf,
    _In_opt_ IUnknown *punkSite,
    _In_ const RECT *prcExclude,
    _In_ UINT cSkip,
    _In_ BOOL fOverflow);

#define FOLDERMENUITEM_SIGNATURE    0xAA0DF00D

typedef struct _FOLDERMENUITEM
{
    DWORD dwSignature;
    int cchMnemonicText;
    PCWSTR pszMnemonicText;
    IExplorerCommand *pec;
    HBITMAP hbmp;
    WCHAR szMnemonicPrefix[4];
    PWSTR pszTitle;
    PWSTR pszToolTip;
    GUID guidCanonical;
    EXPCMDFLAGS dwFlags;
} FOLDERMENUITEM;

class CFolderMenu :
    public CObjectWithSite,
    public IContextMenu3,
    public IShellExtInit,
    public IServiceProvider,
    public CDUIFrameZeroAlloc
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_opt_ IShellFolder *psf,
        _In_ UINT cSkip,
        _In_ BOOL fOverflow,
        _In_opt_ HWND hwnd,
        _In_ REFIID riid,
        _COM_Outptr_ void **ppv);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP QueryContextMenu(HMENU hmenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast,
                                  UINT uFlags) override;
    STDMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO *pici) override;
    STDMETHODIMP GetCommandString(UINT_PTR idCmd, UINT uType, UINT *pReserved, CHAR *pszName,
                                  UINT cchMax) override;
    STDMETHODIMP HandleMenuMsg(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
    STDMETHODIMP HandleMenuMsg2(UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *plResult) override;

    STDMETHODIMP Initialize(PCIDLIST_ABSOLUTE pidlFolder, IDataObject *pdtobj, HKEY hkeyProgID) override;

    STDMETHODIMP QueryService(REFGUID guidService, REFIID riid, void **ppv) override;

    STDMETHODIMP SetSite(IUnknown *punkSite) override { return CObjectWithSite::SetSite(punkSite); }
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CObjectWithSite::GetSite(riid, ppvSite); }

    HRESULT SetEnumExplorerCommand(
        _In_opt_ IEnumExplorerCommand *penum);

private:
    CFolderMenu();
    virtual ~CFolderMenu();

    static int CALLBACK s_FreeMenuItemData(
        _In_ void *p,
        _In_opt_ void *pData);
    static LRESULT _MatchMenuItem(
        _In_ HMENU hmenu,
        _In_ WCHAR chKey);

    HRESULT _AllocFolderMenuItemData(
        _In_ IExplorerCommand *pec,
        _In_opt_ IShellItemArray *psia,
        _In_ BOOL fWantIcon,
        _Outptr_ FOLDERMENUITEM **ppItem);
    HRESULT _GetRightSideAndTotalCommand(
        _In_ IEnumIDList *penum,
        _In_ IShellFolder *psf,
        _In_opt_ IShellItemArray *psia,
        _Out_ UINT *pcRightSide,
        _Out_ UINT *pcTotal);
    HRESULT _InsertCommandToMenu(
        _In_ IExplorerCommand *pec,
        _In_opt_ IShellItemArray *psia,
        _In_ HMENU hmenu,
        _In_ UINT indexMenu,
        _Inout_ UINT *pidCmdBase,
        _In_ UINT idCmdLast,
        _Inout_ UINT *pidCmdMax,
        _Inout_ UINT *pcInserted,
        _Inout_ BOOL *pfSeparatorPending,
        _Inout_ BOOL *pfRightSideSeparated,
        _Inout_ BOOL *pfHasBitmaps,
        _In_ BOOL fWantIcon);
    HRESULT _ComposeMenuFromCommandFolder(
        _In_ IShellFolder *psf,
        _In_opt_ IShellItemArray *psia,
        _In_ HMENU hmenu,
        _In_ UINT indexMenu,
        _In_ UINT idCmdFirst,
        _In_ UINT idCmdLast,
        _Inout_ UINT *pidCmdMax,
        _In_ UINT cSkip);
    HRESULT _ComposeMenuFromEnumCommand(
        _In_ IEnumExplorerCommand *penum,
        _In_opt_ IShellItemArray *psia,
        _In_ HMENU hmenu,
        _In_ UINT indexMenu,
        _In_ UINT idCmdFirst,
        _In_ UINT idCmdLast,
        _Inout_ UINT *pidCmdMax);
    HRESULT _GetSelection(
        _COM_Outptr_result_maybenull_ IShellItemArray **ppsia);
    FOLDERMENUITEM *_GetItemData(
        _In_ HMENU hmenu,
        _In_ UINT idCmd);

    LONG _cRef;
    IDataObject *_pdtobj;
    IShellFolder *_psf;
    IShellItemArray *_psiaSelection;
    HMENU _hmenu;
    HWND _hwnd;
    UINT _idCmdFirst;
    UINT _cSkip;
    BOOL _fOverflow;
    HBITMAP _hbmShield;
    IEnumExplorerCommand *_penumCommands;
    HDPA _hdpaItems;
};

/* Split and drop-down buttons of the bar (splitbutton.cpp) */
class CSplitButton : public CElementWithSite, public DirectUI::IElementListener
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement,
        _In_ BOOL fSplit);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    virtual void OnInput(DirectUI::InputEvent *pie);
    virtual void OnEvent(DirectUI::Event *pev);
    virtual void OnPropertyChanged(const DirectUI::PropertyInfo *ppi, int iIndex, DirectUI::Value *pvOld,
                                   DirectUI::Value *pvNew);
    virtual void OnDestroy();

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override { return CElementWithSite::QueryInterface(riid, ppv); }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SetSite(IUnknown *punkSite) override;
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CElementWithSite::GetSite(riid, ppvSite); }

    void OnListenerAttach(DirectUI::Element *peFrom) override {}
    void OnListenerDetach(DirectUI::Element *peFrom) override {}
    bool OnListenedPropertyChanging(DirectUI::Element *peFrom, const DirectUI::PropertyInfo *ppi, int iIndex,
                                    DirectUI::Value *pvOld, DirectUI::Value *pvNew) override { return true; }
    void OnListenedPropertyChanged(DirectUI::Element *peFrom, const DirectUI::PropertyInfo *ppi, int iIndex,
                                   DirectUI::Value *pvOld, DirectUI::Value *pvNew) override;
    void OnListenedInput(DirectUI::Element *peFrom, DirectUI::InputEvent *pie) override {}
    void OnListenedEvent(DirectUI::Element *peFrom, DirectUI::Event *pev) override {}

    HRESULT SetItem(
        _In_ IShellFolder *psf,
        _In_ PCUITEMID_CHILD pidl,
        _In_ const COMMANDBUTTONDATA *pData,
        _In_opt_ IServiceProvider *pspCommandSite);
    void SetEvaluatingAsyncState(
        _In_ bool fEvaluating);
    BOOL IsSplit() { return _fSplit; }
    HRESULT DefaultAction();

    static DirectUI::IClassInfo *Class;

private:
    void _UpdateDisplay(
        _In_ const COMMANDBUTTONDATA *pData);
    void _DoAction(
        _In_ BOOL fShowMenu);

    BOOL _fSplit;
    BOOL _fHasSubCommands;
    DirectUI::Button *_pbKeyPressed;
    IShellFolder *_psf;
    PITEMID_CHILD _pidl;
    IServiceProvider *_pspCommandSite;
};

CSplitButton *
WINAPI
element_cast_CSplitButton(
    _In_opt_ DirectUI::Element *pe);

/* Views button of the bar: cycles the view mode, its arrow opens the view slider (viewcontrol.cpp) */
class CViewSlider;

class ViewControl : public CElementWithSite, public IDispatch, public DirectUI::IElementListener
{
public:
    static HRESULT WINAPI Create(
        _In_opt_ DirectUI::Element *peParent,
        _Inout_opt_ ULONG *pdwDeferCookie,
        _Outptr_ DirectUI::Element **ppElement);
    static HRESULT WINAPI Register();
    static DirectUI::IClassInfo *WINAPI GetClassInfoPtr() { return Class; }
    virtual DirectUI::IClassInfo *GetClassInfoW() { return Class; }

    virtual void OnInput(DirectUI::InputEvent *pie);
    virtual void OnEvent(DirectUI::Event *pev);
    virtual bool OnPropertyChanging(const DirectUI::PropertyInfo *ppi, int iIndex, DirectUI::Value *pvOld,
                                    DirectUI::Value *pvNew);
    virtual void OnPropertyChanged(const DirectUI::PropertyInfo *ppi, int iIndex, DirectUI::Value *pvOld,
                                   DirectUI::Value *pvNew);
    virtual void OnDestroy();
    HRESULT DefaultAction();

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SetSite(IUnknown *punkSite) override;
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CElementWithSite::GetSite(riid, ppvSite); }

    STDMETHODIMP GetTypeInfoCount(UINT *pctinfo) override;
    STDMETHODIMP GetTypeInfo(UINT iTInfo, LCID lcid, ITypeInfo **ppTInfo) override;
    STDMETHODIMP GetIDsOfNames(REFIID riid, LPOLESTR *rgszNames, UINT cNames, LCID lcid, DISPID *rgDispId) override;
    STDMETHODIMP Invoke(DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
                        VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr) override;

    void OnListenerAttach(DirectUI::Element *peFrom) override {}
    void OnListenerDetach(DirectUI::Element *peFrom) override {}
    bool OnListenedPropertyChanging(DirectUI::Element *peFrom, const DirectUI::PropertyInfo *ppi, int iIndex,
                                    DirectUI::Value *pvOld, DirectUI::Value *pvNew) override;
    void OnListenedPropertyChanged(DirectUI::Element *peFrom, const DirectUI::PropertyInfo *ppi, int iIndex,
                                   DirectUI::Value *pvOld, DirectUI::Value *pvNew) override;
    void OnListenedInput(DirectUI::Element *peFrom, DirectUI::InputEvent *pie) override {}
    void OnListenedEvent(DirectUI::Element *peFrom, DirectUI::Event *pev) override {}

    void UnFreeze();

    static DirectUI::IClassInfo *Class;

private:
    void _DoAction(
        _In_ BOOL fShowSlider);
    HRESULT _ShowViewSlider(
        _In_ DirectUI::Element *peAnchor);
    HRESULT _UpdateIcon();
    HRESULT _MouseWheel(
        _In_ BOOL fUp);
    HRESULT _ConnectToView(
        _In_ BOOL fConnect);

    DWORD _dwViewEventsCookie;
    IDispatch *_pdispView;
    DirectUI::Button *_pbKeyPressed;
    BOOL _fFrozen;
    BOOL _fMouseEnteredWhileFrozen;
    UINT _idIcon;
    CViewSlider *_pSlider;
};

/* Command module services (commandmodule.cpp) */

/* Site the bar's commands run under: adds a property bag shared by the commands */
class CCommandModuleSite :
    public CObjectWithSite,
    public IServiceProvider,
    public IOleWindow,
    public CDUIFrameZeroAlloc
{
public:
    CCommandModuleSite();

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP QueryService(REFGUID guidService, REFIID riid, void **ppv) override;

    STDMETHODIMP GetWindow(HWND *phwnd) override;
    STDMETHODIMP ContextSensitiveHelp(BOOL fEnterMode) override { return E_NOTIMPL; }

    STDMETHODIMP SetSite(IUnknown *punkSite) override { return CObjectWithSite::SetSite(punkSite); }
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CObjectWithSite::GetSite(riid, ppvSite); }

    VOID ResetCommandsPropertyBag();

private:
    virtual ~CCommandModuleSite();

    LONG _cRef;
    IPropertyBag *_ppbCommands;
};

/* Site handed to a command started from a button: knows the button's window and rectangle */
class CCommandButtonSite :
    public CObjectWithSite,
    public IServiceProvider,
    public ICommandButtonInfo,
    public CDUIFrameZeroAlloc
{
public:
    static HRESULT WINAPI s_CreateInstance(
        _In_ IUnknown *punkSite,
        _In_opt_ HWND hwnd,
        _In_ const RECT *prc,
        _In_ REFIID riid,
        _COM_Outptr_ void **ppv);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP QueryService(REFGUID guidService, REFIID riid, void **ppv) override;

    STDMETHODIMP GetWindow(HWND *phwnd) override;
    STDMETHODIMP ContextSensitiveHelp(BOOL fEnterMode) override { return E_NOTIMPL; }
    STDMETHODIMP GetRect(RECT *prc) override;

    STDMETHODIMP SetSite(IUnknown *punkSite) override { return CObjectWithSite::SetSite(punkSite); }
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CObjectWithSite::GetSite(riid, ppvSite); }

private:
    CCommandButtonSite();
    virtual ~CCommandButtonSite();

    LONG _cRef;
    HWND _hwnd;
    RECT _rc;
};

/* Worker window messages of the command module */
#define CMWM_SHOWFOLDERMENU     (WM_USER + 1)
#define CMWM_INVOKECOMMAND      (WM_USER + 2)
#define CMWM_FOLDERMENUDONE     (WM_USER + 4)
#define CMWM_COMMANDSTATE       (WM_USER + 5)

/* Posted with CMWM_SHOWFOLDERMENU */
typedef struct _FOLDERMENUREQUEST
{
    IShellFolder *psf;
    IUnknown *punkSite;
    RECT rcExclude;
    UINT cSkip;
    BOOL fOverflow;
    DirectUI::Element *peOwner;
} FOLDERMENUREQUEST;

/* Posted with CMWM_INVOKECOMMAND */
typedef struct _COMMANDINVOKEREQUEST
{
    IShellFolder *psf;
    PITEMID_CHILD pidl;
    IUnknown *punkSite;
} COMMANDINVOKEREQUEST;

/* Works out a command's state off the frame thread when it says doing so is slow */
class CGetCommandStateTask : public CFrameWorkItem
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_ PCUITEMID_CHILD pidl,
        _In_ IExplorerCommand *pec,
        _In_opt_ IShellItemArray *psia,
        _In_opt_ IUnknown *punkSite,
        _In_ REFGUID rguidCanonical,
        _In_ HWND hwndNotify,
        _In_ UINT uRefreshID,
        _COM_Outptr_ IFrameWorkItem **ppfwi);

    STDMETHODIMP DoWork() override;
    STDMETHODIMP Dispatch() override { return S_OK; }

private:
    CGetCommandStateTask();
    virtual ~CGetCommandStateTask();

    HRESULT _SetData(
        _In_ PCUITEMID_CHILD pidl,
        _In_ IExplorerCommand *pec,
        _In_opt_ IShellItemArray *psia,
        _In_opt_ IUnknown *punkSite,
        _In_ REFGUID rguidCanonical,
        _In_ HWND hwndNotify,
        _In_ UINT uRefreshID);

    PITEMID_CHILD _pidl;
    PIDLIST_ABSOLUTE _pidlFolder;
    PIDLIST_ABSOLUTE _pidlView;
    IStream *_pstmSelection;
    IStream *_pstmSite;
    GUID _guidCanonical;
    HWND _hwndNotify;
    HKEY _hkeyStore;
    PWSTR _pszName;
    UINT _uRefreshID;
};

/* Built-in command handlers (cmdhandlers.cpp) */

typedef HRESULT (WINAPI *PFNCOMMANDSTATE)(IUnknown *punkSite, IShellItemArray *psia, BOOL fOkToBeSlow,
                                          EXPCMDSTATE *pState);
typedef HRESULT (WINAPI *PFNCOMMANDEXECUTE)(IUnknown *punkSite, IShellItemArray *psia, IBindCtx *pbc);

class CCommonCommandState :
    public CObjectWithSite,
    public IExplorerCommandState,
    public IInitializeCommand,
    public CDUIFrameZeroAlloc
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_ REFIID riid,
        _COM_Outptr_ void **ppv);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetState(IShellItemArray *psiItemArray, BOOL fOkToBeSlow, EXPCMDSTATE *pCmdState) override;
    STDMETHODIMP Initialize(LPCWSTR pszCommandName, IPropertyBag *ppb) override;

    STDMETHODIMP SetSite(IUnknown *punkSite) override { return CObjectWithSite::SetSite(punkSite); }
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CObjectWithSite::GetSite(riid, ppvSite); }

private:
    CCommonCommandState();
    virtual ~CCommonCommandState();

    LONG _cRef;
    PWSTR _pszCommandName;
    IPropertyBag *_ppb;
};

/* DelegateExecute handler that runs one of the built-in command functions */
class CExecuteCommon :
    public CObjectWithSite,
    public IExecuteCommand,
    public IObjectWithAssociationElement,
    public IObjectWithSelection,
    public CDUIFrameZeroAlloc
{
public:
    static HRESULT WINAPI s_CreateInstance(
        _In_ PFNCOMMANDEXECUTE pfnExecute,
        _In_ REFIID riid,
        _COM_Outptr_ void **ppv);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP SetKeyState(DWORD grfKeyState) override;
    STDMETHODIMP SetParameters(LPCWSTR pszParameters) override;
    STDMETHODIMP SetPosition(POINT pt) override;
    STDMETHODIMP SetShowWindow(int nShow) override;
    STDMETHODIMP SetNoShowUI(BOOL fNoShowUI) override;
    STDMETHODIMP SetDirectory(LPCWSTR pszDirectory) override;
    STDMETHODIMP Execute() override;

    STDMETHODIMP SetAssocElement(IAssociationElement *pae) override;
    STDMETHODIMP GetAssocElement(REFIID riid, void **ppv) override;

    STDMETHODIMP SetSelection(IShellItemArray *psia) override;
    STDMETHODIMP GetSelection(REFIID riid, void **ppv) override;

    STDMETHODIMP SetSite(IUnknown *punkSite) override { return CObjectWithSite::SetSite(punkSite); }
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CObjectWithSite::GetSite(riid, ppvSite); }

private:
    CExecuteCommon(
        _In_ PFNCOMMANDEXECUTE pfnExecute);
    virtual ~CExecuteCommon();

    LONG _cRef;
    DWORD _grfKeyState;
    POINT _pt;
    int _nShow;
    BOOL _fNoShowUI;
    PWSTR _pszParameters;
    PWSTR _pszDirectory;
    IAssociationElement *_pae;
    IShellItemArray *_psia;
    PFNCOMMANDEXECUTE _pfnExecute;
};

class CFolderOptionsCommand : public CObjectWithSite, public IExplorerCommand, public CDUIFrameZeroAlloc
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_ REFIID riid,
        _COM_Outptr_ void **ppv);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetTitle(IShellItemArray *psiItemArray, LPWSTR *ppszName) override;
    STDMETHODIMP GetIcon(IShellItemArray *psiItemArray, LPWSTR *ppszIcon) override;
    STDMETHODIMP GetToolTip(IShellItemArray *psiItemArray, LPWSTR *ppszInfotip) override;
    STDMETHODIMP GetCanonicalName(GUID *pguidCommandName) override;
    STDMETHODIMP GetState(IShellItemArray *psiItemArray, BOOL fOkToBeSlow, EXPCMDSTATE *pCmdState) override;
    STDMETHODIMP Invoke(IShellItemArray *psiItemArray, IBindCtx *pbc) override;
    STDMETHODIMP GetFlags(EXPCMDFLAGS *pFlags) override;
    STDMETHODIMP EnumSubCommands(IEnumExplorerCommand **ppEnum) override;

    STDMETHODIMP SetSite(IUnknown *punkSite) override { return CObjectWithSite::SetSite(punkSite); }
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override { return CObjectWithSite::GetSite(riid, ppvSite); }

private:
    CFolderOptionsCommand();
    virtual ~CFolderOptionsCommand() {}

    LONG _cRef;
};

/* Property list of the outer command module element */
extern const DirectUI::PropertyInfo *const c_rgpiCommandModule[];
extern const UINT c_cpiCommandModule;

/* Class objects shell32 registers for the command bar */
HRESULT
WINAPI
CommandBar_CreateInstance(
    _In_ REFCLSID rclsid,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv);
