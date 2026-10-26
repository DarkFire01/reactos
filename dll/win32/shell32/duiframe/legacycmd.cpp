/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Web view tasks of folder views and their explorer command wrappers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

/* IExplorerCommand over a web view IUICommand */
class CExplorerCommandOnUICommand : public IExplorerCommand, public IObjectWithSite, public CDUIFrameZeroAlloc
{
public:
    CExplorerCommandOnUICommand(
        _In_ IUICommand *puic);

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

    STDMETHODIMP SetSite(IUnknown *punkSite) override;
    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override;

private:
    virtual ~CExplorerCommandOnUICommand();

    LONG _cRef;
    IUICommand *_puic;
};

/* IEnumExplorerCommand over an IEnumUICommand */
class CEnumExplorerCommandOnEnumUICommand : public IEnumExplorerCommand, public CDUIFrameZeroAlloc
{
public:
    CEnumExplorerCommandOnEnumUICommand(
        _In_ IEnumUICommand *penum);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP Next(ULONG celt, IExplorerCommand **pUICommand, ULONG *pceltFetched) override;
    STDMETHODIMP Skip(ULONG celt) override;
    STDMETHODIMP Reset() override;
    STDMETHODIMP Clone(IEnumExplorerCommand **ppenum) override;

private:
    virtual ~CEnumExplorerCommandOnEnumUICommand();

    BOOL _Next(
        _COM_Outptr_ IExplorerCommand **ppec);

    LONG _cRef;
    IEnumUICommand *_penum;
};

/* Web view task list over command objects; hands them out as IUICommand or IExplorerCommand */
class CEnumUICommand : public IEnumUICommand, public IEnumExplorerCommand, public CDUIFrameZeroAlloc
{
public:
    CEnumUICommand();

    HRESULT Initialize(
        _In_reads_(cCommands) IUnknown **ppunkCommands,
        _In_ UINT cCommands);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    /* IEnumUICommand */
    STDMETHODIMP Next(UINT celt, IUICommand **ppuic, UINT *pceltFetched) override;
    STDMETHODIMP Skip(UINT celt) override;
    STDMETHODIMP Reset() override;
    STDMETHODIMP Clone(IEnumUICommand **ppenum) override;

    /* IEnumExplorerCommand */
    STDMETHODIMP Next(ULONG celt, IExplorerCommand **pUICommand, ULONG *pceltFetched) override;
    STDMETHODIMP Skip(ULONG celt) override;
    STDMETHODIMP Clone(IEnumExplorerCommand **ppenum) override;

private:
    virtual ~CEnumUICommand();

    HRESULT _Next(
        _In_ REFIID riid,
        _COM_Outptr_ void **ppv);

    LONG _cRef;
    IUnknown **_ppunkCommands;
    UINT _cCommands;
    UINT _iNext;
};

/* CExplorerCommandOnUICommand */

CExplorerCommandOnUICommand::CExplorerCommandOnUICommand(
    _In_ IUICommand *puic) :
    _cRef(1),
    _puic(puic)
{
    _puic->AddRef();
}

CExplorerCommandOnUICommand::~CExplorerCommandOnUICommand()
{
    _puic->Release();
}

HRESULT
WINAPI
CreateExplorerCommandFromUICommand(
    _In_ IUICommand *puic,
    _COM_Outptr_ IExplorerCommand **ppec)
{
    CExplorerCommandOnUICommand *pCommand;
    HRESULT hr;

    *ppec = NULL;
    pCommand = new CExplorerCommandOnUICommand(puic);
    if (!pCommand)
        return E_OUTOFMEMORY;

    hr = pCommand->QueryInterface(IID_PPV_ARGS(ppec));
    pCommand->Release();
    return hr;
}

STDMETHODIMP
CExplorerCommandOnUICommand::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CExplorerCommandOnUICommand, IExplorerCommand),
        QITABENT(CExplorerCommandOnUICommand, IObjectWithSite),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CExplorerCommandOnUICommand::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CExplorerCommandOnUICommand::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

STDMETHODIMP
CExplorerCommandOnUICommand::GetTitle(
    IShellItemArray *psiItemArray,
    LPWSTR *ppszName)
{
    return _puic->get_Name(psiItemArray, ppszName);
}

STDMETHODIMP
CExplorerCommandOnUICommand::GetIcon(
    IShellItemArray *psiItemArray,
    LPWSTR *ppszIcon)
{
    return _puic->get_Icon(psiItemArray, ppszIcon);
}

STDMETHODIMP
CExplorerCommandOnUICommand::GetToolTip(
    IShellItemArray *psiItemArray,
    LPWSTR *ppszInfotip)
{
    return _puic->get_Tooltip(psiItemArray, ppszInfotip);
}

STDMETHODIMP
CExplorerCommandOnUICommand::GetCanonicalName(
    GUID *pguidCommandName)
{
    return _puic->get_CanonicalName(pguidCommandName);
}

STDMETHODIMP
CExplorerCommandOnUICommand::GetState(
    IShellItemArray *psiItemArray,
    BOOL fOkToBeSlow,
    EXPCMDSTATE *pCmdState)
{
    return _puic->get_State(psiItemArray, fOkToBeSlow, (int *)pCmdState);
}

STDMETHODIMP
CExplorerCommandOnUICommand::Invoke(
    IShellItemArray *psiItemArray,
    IBindCtx *pbc)
{
    return _puic->Invoke(psiItemArray, pbc);
}

/* Flags and sub commands exist only on the extended web view command */
STDMETHODIMP
CExplorerCommandOnUICommand::GetFlags(
    EXPCMDFLAGS *pFlags)
{
    IUICommandEx *puicx;
    HRESULT hr;

    *pFlags = ECF_DEFAULT;
    hr = _puic->QueryInterface(IID_IUICommandEx, (void **)&puicx);
    if (SUCCEEDED(hr))
    {
        hr = puicx->GetFlags(pFlags);
        puicx->Release();
    }
    return hr;
}

STDMETHODIMP
CExplorerCommandOnUICommand::EnumSubCommands(
    IEnumExplorerCommand **ppEnum)
{
    IUICommandEx *puicx;
    HRESULT hr;

    *ppEnum = NULL;
    hr = _puic->QueryInterface(IID_IUICommandEx, (void **)&puicx);
    if (SUCCEEDED(hr))
    {
        IEnumUICommand *penum = NULL;

        hr = puicx->EnumSubCommands(&penum);
        if (SUCCEEDED(hr))
            hr = CEnumExplorerCommandOnEnumUICommand_CreateInstance(penum, ppEnum);
        if (penum)
            penum->Release();
        puicx->Release();
    }
    return hr;
}

STDMETHODIMP
CExplorerCommandOnUICommand::SetSite(
    IUnknown *punkSite)
{
    return IUnknown_SetSite(_puic, punkSite);
}

STDMETHODIMP
CExplorerCommandOnUICommand::GetSite(
    REFIID riid,
    void **ppvSite)
{
    return IUnknown_GetSite(_puic, riid, ppvSite);
}

/* CEnumExplorerCommandOnEnumUICommand */

CEnumExplorerCommandOnEnumUICommand::CEnumExplorerCommandOnEnumUICommand(
    _In_ IEnumUICommand *penum) :
    _cRef(1),
    _penum(penum)
{
    _penum->AddRef();
}

CEnumExplorerCommandOnEnumUICommand::~CEnumExplorerCommandOnEnumUICommand()
{
    _penum->Release();
}

HRESULT
WINAPI
CEnumExplorerCommandOnEnumUICommand_CreateInstance(
    _In_ IEnumUICommand *penum,
    _COM_Outptr_ IEnumExplorerCommand **ppenum)
{
    CEnumExplorerCommandOnEnumUICommand *pEnum;
    HRESULT hr;

    *ppenum = NULL;
    pEnum = new CEnumExplorerCommandOnEnumUICommand(penum);
    if (!pEnum)
        return E_OUTOFMEMORY;

    hr = pEnum->QueryInterface(IID_PPV_ARGS(ppenum));
    pEnum->Release();
    return hr;
}

STDMETHODIMP
CEnumExplorerCommandOnEnumUICommand::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CEnumExplorerCommandOnEnumUICommand, IEnumExplorerCommand),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CEnumExplorerCommandOnEnumUICommand::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CEnumExplorerCommandOnEnumUICommand::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

/* A web view command that is already an explorer command is handed out as it is */
BOOL
CEnumExplorerCommandOnEnumUICommand::_Next(
    _COM_Outptr_ IExplorerCommand **ppec)
{
    IUICommand *puic = NULL;
    HRESULT hr;

    *ppec = NULL;
    if (_penum->Next(1, &puic, NULL) != S_OK)
    {
        if (puic)
            puic->Release();
        return FALSE;
    }

    hr = puic->QueryInterface(IID_PPV_ARGS(ppec));
    if (FAILED(hr))
        hr = CreateExplorerCommandFromUICommand(puic, ppec);
    puic->Release();
    return SUCCEEDED(hr);
}

STDMETHODIMP
CEnumExplorerCommandOnEnumUICommand::Next(
    ULONG celt,
    IExplorerCommand **pUICommand,
    ULONG *pceltFetched)
{
    ULONG cFetched = 0;

    while (cFetched < celt && _Next(&pUICommand[cFetched]))
        cFetched++;

    if (pceltFetched)
        *pceltFetched = cFetched;
    return (cFetched == celt) ? S_OK : S_FALSE;
}

STDMETHODIMP
CEnumExplorerCommandOnEnumUICommand::Skip(
    ULONG celt)
{
    return _penum->Skip(celt);
}

STDMETHODIMP
CEnumExplorerCommandOnEnumUICommand::Reset()
{
    return _penum->Reset();
}

STDMETHODIMP
CEnumExplorerCommandOnEnumUICommand::Clone(
    IEnumExplorerCommand **ppenum)
{
    *ppenum = NULL;
    return E_NOTIMPL;
}

/* CEnumUICommand */

CEnumUICommand::CEnumUICommand() :
    _cRef(1)
{
}

CEnumUICommand::~CEnumUICommand()
{
    for (UINT i = 0; i < _cCommands; i++)
        _ppunkCommands[i]->Release();
    LocalFree(_ppunkCommands);
}

HRESULT
CEnumUICommand::Initialize(
    _In_reads_(cCommands) IUnknown **ppunkCommands,
    _In_ UINT cCommands)
{
    if (!cCommands)
        return S_OK;

    _ppunkCommands = (IUnknown **)LocalAlloc(LPTR, cCommands * sizeof(*_ppunkCommands));
    if (!_ppunkCommands)
        return E_OUTOFMEMORY;

    /* The list ends at the first empty slot */
    for (UINT i = 0; i < cCommands && ppunkCommands[i]; i++)
    {
        _ppunkCommands[i] = ppunkCommands[i];
        _ppunkCommands[i]->AddRef();
        _cCommands++;
    }
    return S_OK;
}

HRESULT
WINAPI
Create_IEnumUICommandWithArray(
    _In_reads_(cCommands) IUnknown **ppunkCommands,
    _In_ UINT cCommands,
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    CEnumUICommand *penum;
    HRESULT hr;

    *ppv = NULL;
    penum = new CEnumUICommand();
    if (!penum)
        return E_OUTOFMEMORY;

    hr = penum->Initialize(ppunkCommands, cCommands);
    if (SUCCEEDED(hr))
        hr = penum->QueryInterface(riid, ppv);
    penum->Release();
    return hr;
}

STDMETHODIMP
CEnumUICommand::QueryInterface(
    REFIID riid,
    void **ppv)
{
    static const QITAB qit[] =
    {
        { &IID_IEnumUICommand, OFFSETOFCLASS(IEnumUICommand, CEnumUICommand) },
        QITABENT(CEnumUICommand, IEnumExplorerCommand),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG)
CEnumUICommand::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG)
CEnumUICommand::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);

    if (!cRef)
        delete this;
    return cRef;
}

/* A command that is only a web view command is wrapped when an explorer command is wanted */
HRESULT
CEnumUICommand::_Next(
    _In_ REFIID riid,
    _COM_Outptr_ void **ppv)
{
    IUnknown *punk;
    HRESULT hr;

    *ppv = NULL;
    if (_iNext >= _cCommands)
        return S_FALSE;

    punk = _ppunkCommands[_iNext++];
    hr = punk->QueryInterface(riid, ppv);
    if (FAILED(hr) && IsEqualIID(riid, IID_IExplorerCommand))
    {
        IUICommand *puic;

        hr = punk->QueryInterface(IID_IUICommand, (void **)&puic);
        if (SUCCEEDED(hr))
        {
            IExplorerCommand *pec;

            hr = CreateExplorerCommandFromUICommand(puic, &pec);
            if (SUCCEEDED(hr))
            {
                hr = pec->QueryInterface(riid, ppv);
                pec->Release();
            }
            puic->Release();
        }
    }
    return hr;
}

STDMETHODIMP
CEnumUICommand::Next(
    UINT celt,
    IUICommand **ppuic,
    UINT *pceltFetched)
{
    HRESULT hr = _Next(IID_IUICommand, (void **)ppuic);

    if (pceltFetched)
        *pceltFetched = (hr == S_OK) ? 1 : 0;
    return hr;
}

STDMETHODIMP
CEnumUICommand::Next(
    ULONG celt,
    IExplorerCommand **pUICommand,
    ULONG *pceltFetched)
{
    HRESULT hr = _Next(IID_PPV_ARGS(pUICommand));

    if (pceltFetched)
        *pceltFetched = (hr == S_OK) ? 1 : 0;
    return hr;
}

STDMETHODIMP
CEnumUICommand::Skip(
    UINT celt)
{
    _iNext = min(_iNext + celt, _cCommands);
    return S_OK;
}

STDMETHODIMP
CEnumUICommand::Skip(
    ULONG celt)
{
    return Skip((UINT)celt);
}

STDMETHODIMP
CEnumUICommand::Reset()
{
    _iNext = 0;
    return S_OK;
}

STDMETHODIMP
CEnumUICommand::Clone(
    IEnumUICommand **ppenum)
{
    *ppenum = NULL;
    return E_NOTIMPL;
}

STDMETHODIMP
CEnumUICommand::Clone(
    IEnumExplorerCommand **ppenum)
{
    *ppenum = NULL;
    return E_NOTIMPL;
}

/*
 * Folder tasks every file system view offers. Windows puts the open task first when items
 * are selected; that task needs the association handler commands shell32 does not have yet.
 */
HRESULT
WINAPI
EnumCommonTasks(
    _In_opt_ IUnknown *punkSite,
    _COM_Outptr_ IEnumUICommand **ppenum)
{
    static const PCWSTR c_rgpszTasks[] =
    {
        L"Windows.IncludeInLibrary",
        L"Windows.Share",
        L"Windows.Sync",
        L"Windows.NewFolder",
    };
    IUnknown *rgpunk[_countof(c_rgpszTasks)];
    UINT cTasks = 0;
    HRESULT hr;

    *ppenum = NULL;
    for (UINT i = 0; i < _countof(c_rgpszTasks); i++)
    {
        IExplorerCommand *pec;

        if (SUCCEEDED(GetCanonicalCommand(c_rgpszTasks[i], IID_PPV_ARGS(&pec))))
            rgpunk[cTasks++] = pec;
    }

    hr = Create_IEnumUICommandWithArray(rgpunk, cTasks, IID_IEnumUICommand, (void **)ppenum);
    for (UINT i = 0; i < cTasks; i++)
        rgpunk[i]->Release();
    return hr;
}

/*
 * Special tasks by folder kind (print, e-mail, burn, slide show, play and the view control).
 * They are web view task definitions with their own state and invoke code; shell32 does not
 * have that table yet.
 */
HRESULT
WINAPI
LegacyEnumSpecialTasksByType(
    _In_ UINT uType,
    _COM_Outptr_ IEnumUICommand **ppenum)
{
    *ppenum = NULL;
    return E_NOTIMPL;
}

/* SFVM_GET_WEBVIEW_TASKS of file system folders, called from shell32's folder view callback */
EXTERN_C
HRESULT
WINAPI
DUIFrame_GetFolderWebViewTasks(
    _In_opt_ IUnknown *punkSite,
    _Out_ PVOID pvTasks)
{
    WEBVIEWTASKS *pTasks = (WEBVIEWTASKS *)pvTasks;

    ZeroMemory(pTasks, sizeof(*pTasks));
    return EnumCommonTasks(punkSite, &pTasks->penumFolderTasks);
}
