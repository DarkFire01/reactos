/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Class objects of the DirectUI view frame and its layout definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#ifdef SHELL32_DUIFRAME

/* duiframe is built against dui70's ABI, so it is only reached through these C entry points */
EXTERN_C VOID WINAPI DUIFrame_ProcessAttach(VOID);
EXTERN_C VOID WINAPI DUIFrame_ProcessDetach(VOID);

EXTERN_C
HRESULT
WINAPI
DUIFrame_CreateViewFrame(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv);

EXTERN_C
HRESULT
WINAPI
DUIFrame_CreateCommonLayoutDefinition(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv);

EXTERN_C
HRESULT
WINAPI
DUIFrame_CreateCommonLayoutModifier(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv);

EXTERN_C const CLSID CLSID_DUIShellViewFrame;
EXTERN_C const CLSID CLSID_CommonLayoutDefinition;
EXTERN_C const CLSID CLSID_CommonLayoutModifier;

typedef HRESULT (WINAPI *PFNDUIFRAMECREATE)(REFIID riid, void **ppv);

template <class T, const CLSID *pclsid, PFNDUIFRAMECREATE pfnCreate, UINT uRegistryId>
class CDUIFrameClass :
    public CComCoClass<T, pclsid>
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_opt_ PVOID pUnkOuter,
        _In_ REFIID riid,
        _Outptr_ PVOID *ppv)
    {
        *ppv = NULL;
        if (pUnkOuter)
            return CLASS_E_NOAGGREGATION;
        return pfnCreate(riid, ppv);
    }

    typedef CDUIFrameClass _CreatorClass;

    static void WINAPI ObjectMain(bool) {}
    static const struct _ATL_CATMAP_ENTRY *GetCategoryMap() { return NULL; }

    DECLARE_REGISTRY_RESOURCEID(uRegistryId)
};

class CDUIViewFrame :
    public CDUIFrameClass<CDUIViewFrame, &CLSID_DUIShellViewFrame,
                          DUIFrame_CreateViewFrame, IDR_DUISHELLVIEWFRAME>
{
};

class CCommonLayoutDefinition :
    public CDUIFrameClass<CCommonLayoutDefinition, &CLSID_CommonLayoutDefinition,
                          DUIFrame_CreateCommonLayoutDefinition, IDR_COMMONLAYOUTDEFINITION>
{
};

class CCommonLayoutModifier :
    public CDUIFrameClass<CCommonLayoutModifier, &CLSID_CommonLayoutModifier,
                          DUIFrame_CreateCommonLayoutModifier, IDR_COMMONLAYOUTMODIFIER>
{
};

/* Command bar objects: the command folder carries the registration of all of them */
#define IDR_COMMANDBAR  3401

EXTERN_C
HRESULT
WINAPI
DUIFrame_CreateCommandBarObject(
    _In_ REFCLSID rclsid,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv);

EXTERN_C const CLSID CLSID_CommandFolder;
EXTERN_C const CLSID CLSID_CommonCommandState;
EXTERN_C const CLSID CLSID_FolderOptionsCommand;
EXTERN_C const CLSID CLSID_SelectAllExecute;
EXTERN_C const CLSID CLSID_RenameExecute;
EXTERN_C const CLSID CLSID_CloseWindowExecute;
EXTERN_C const CLSID CLSID_MenuBarExecute;
EXTERN_C const CLSID CLSID_UndoExecute;
EXTERN_C const CLSID CLSID_RedoExecute;

template <class T, const CLSID *pclsid>
class CCommandBarClass :
    public CComCoClass<T, pclsid>
{
public:
    static HRESULT WINAPI CreateInstance(
        _In_opt_ PVOID pUnkOuter,
        _In_ REFIID riid,
        _Outptr_ PVOID *ppv)
    {
        *ppv = NULL;
        if (pUnkOuter)
            return CLASS_E_NOAGGREGATION;
        return DUIFrame_CreateCommandBarObject(*pclsid, riid, ppv);
    }

    typedef CCommandBarClass _CreatorClass;

    static void WINAPI ObjectMain(bool) {}
    static const struct _ATL_CATMAP_ENTRY *GetCategoryMap() { return NULL; }

    DECLARE_NO_REGISTRY()
};

class CCommandFolderClass : public CCommandBarClass<CCommandFolderClass, &CLSID_CommandFolder>
{
public:
    DECLARE_REGISTRY_RESOURCEID(IDR_COMMANDBAR)
};

class CCommonCommandStateClass : public CCommandBarClass<CCommonCommandStateClass, &CLSID_CommonCommandState> {};
class CFolderOptionsCommandClass : public CCommandBarClass<CFolderOptionsCommandClass, &CLSID_FolderOptionsCommand> {};
class CSelectAllExecuteClass : public CCommandBarClass<CSelectAllExecuteClass, &CLSID_SelectAllExecute> {};
class CRenameExecuteClass : public CCommandBarClass<CRenameExecuteClass, &CLSID_RenameExecute> {};
class CCloseWindowExecuteClass : public CCommandBarClass<CCloseWindowExecuteClass, &CLSID_CloseWindowExecute> {};
class CMenuBarExecuteClass : public CCommandBarClass<CMenuBarExecuteClass, &CLSID_MenuBarExecute> {};
class CUndoExecuteClass : public CCommandBarClass<CUndoExecuteClass, &CLSID_UndoExecute> {};
class CRedoExecuteClass : public CCommandBarClass<CRedoExecuteClass, &CLSID_RedoExecute> {};

/* SFVM_GET_WEBVIEW_TASKS of file system folders */
EXTERN_C
HRESULT
WINAPI
DUIFrame_GetFolderWebViewTasks(
    _In_opt_ IUnknown *punkSite,
    _Out_ PVOID pvTasks);

#endif /* SHELL32_DUIFRAME */
