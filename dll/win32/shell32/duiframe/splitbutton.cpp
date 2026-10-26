/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Command bar buttons: a command with its icon, label and drop-down part
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

DirectUI::IClassInfo *CSplitButton::Class = NULL;

/* Layout position that takes the element out of the layout */
#define LAYOUTPOS_NONE          (-3)

static
DirectUI::Element *
FindChild(
    _In_ DirectUI::Element *pe,
    _In_ PCWSTR pszID)
{
    return pe->FindDescendent(StrToID(pszID));
}

/* Like FindChild, for ids the markup may not have registered as atoms yet */
static
DirectUI::Element *
FindChildIfAtom(
    _In_ DirectUI::Element *pe,
    _In_ PCWSTR pszID)
{
    if (!FindAtomW(pszID))
        return NULL;
    return FindChild(pe, pszID);
}

CSplitButton *
WINAPI
element_cast_CSplitButton(
    _In_opt_ DirectUI::Element *pe)
{
    if (pe && pe->GetClassInfoW()->IsSubclassOf(CSplitButton::Class))
        return static_cast<CSplitButton *>(pe);
    return NULL;
}

HRESULT
WINAPI
CSplitButton::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement)
{
    return Create(peParent, pdwDeferCookie, ppElement, TRUE);
}

HRESULT
WINAPI
CSplitButton::Create(
    _In_opt_ DirectUI::Element *peParent,
    _Inout_opt_ ULONG *pdwDeferCookie,
    _Outptr_ DirectUI::Element **ppElement,
    _In_ BOOL fSplit)
{
    CSplitButton *pe;
    HRESULT hr;

    *ppElement = NULL;
    pe = DirectUI::HNewAndZero<CSplitButton>();
    if (!pe)
        return E_OUTOFMEMORY;

    hr = pe->Initialize(0, peParent, pdwDeferCookie);
    if (FAILED(hr))
    {
        pe->Destroy(false);
        return hr;
    }

    pe->SetActive(0);
    pe->_fSplit = fSplit;
    pe->SetLayoutPos(ROSUI_LP_LEFT);
    *ppElement = pe;
    return S_OK;
}

HRESULT
WINAPI
CSplitButton::Register()
{
    return DirectUI::ClassInfo<CSplitButton, CElementWithSite, DirectUI::StandardCreator<CSplitButton>>::Register(
        DUIFRAME_HINST, L"AJOSplitButton", NULL, 0, false);
}

/* The action button follows the site so its pressed state can drive the drop-down part */
STDMETHODIMP
CSplitButton::SetSite(
    IUnknown *punkSite)
{
    DirectUI::Element *peAction = FindChild(this, L"ActionButton");

    if (peAction)
    {
        if (punkSite)
            peAction->AddListener(this);
        else
            peAction->RemoveListener(this);
    }
    IUnknown_Set(&_punkSite, punkSite);
    return S_OK;
}

void
CSplitButton::OnDestroy()
{
    IUnknown_SafeReleaseAndNullPtr(&_psf);
    CoTaskMemFree(_pidl);
    _pidl = NULL;
    IUnknown_SafeReleaseAndNullPtr(&_pspCommandSite);
    CElementWithSite::OnDestroy();
}

HRESULT
CSplitButton::SetItem(
    _In_ IShellFolder *psf,
    _In_ PCUITEMID_CHILD pidl,
    _In_ const COMMANDBUTTONDATA *pData,
    _In_opt_ IServiceProvider *pspCommandSite)
{
    HRESULT hr;

    CoTaskMemFree(_pidl);
    _pidl = NULL;
    hr = SHILCloneFirst(pidl, &_pidl);
    if (SUCCEEDED(hr))
    {
        IUnknown_Set((IUnknown **)&_psf, psf);
        IUnknown_Set((IUnknown **)&_pspCommandSite, pspCommandSite);
        _UpdateDisplay(pData);
    }
    return hr;
}

/* Icons only show for the default verb and the right side commands */
void
CSplitButton::_UpdateDisplay(
    _In_ const COMMANDBUTTONDATA *pData)
{
    DirectUI::Element *peIcon;
    WCHAR szID[39];

    SetEnabled((pData->dwState & ECS_DISABLED) != ECS_DISABLED);

    peIcon = FindChildIfAtom(this, L"ActionButtonIcon");
    if (peIcon)
    {
        IImageList *piml;

        if (pData->iImage == -1 ||
            (!IsRightSideCommand(pData->guidCanonical) && !IsEqualGUID(pData->guidCanonical, UICID_DefaultVerb)))
        {
            peIcon->SetWidth(0);
        }
        else if (SUCCEEDED(SHGetImageList(SHIL_SMALL, IID_PPV_ARGS(&piml))))
        {
            HICON hicon;

            if (SUCCEEDED(piml->GetIcon(pData->iImage, ILD_NORMAL, &hicon)))
                DUI_SetElementIcon(peIcon, hicon);
            piml->Release();
        }
    }

    /* The element id is the command's canonical name */
    if (SUCCEEDED(SHStringFromGUIDW(pData->guidCanonical, szID, _countof(szID))))
        SetID(szID);

    if (pData->pszTitle)
    {
        SetAccName(pData->pszTitle);
        if (!(pData->dwFlags & ECF_HIDELABEL))
        {
            DirectUI::Element *peLabel = FindChild(this, L"ActionButtonLabel");

            if (peLabel)
                peLabel->SetContentString(pData->pszTitle);
        }
    }

    if (pData->pszToolTip)
    {
        DirectUI::Element *peAction = FindChild(this, L"ActionButton");

        if (peAction)
            peAction->SetAccDesc(pData->pszToolTip);
        SetAccDesc(pData->pszToolTip);
    }

    /* Plain buttons of commands with sub commands drop their menu and show a glyph for it */
    if (!_fSplit)
    {
        DirectUI::Element *peGlyph = FindChild(this, L"MenuButtonGlyph");

        if (peGlyph)
        {
            _fHasSubCommands = (pData->dwFlags & (ECF_HASSUBCOMMANDS | ECF_ISDROPDOWN)) != 0;
            peGlyph->SetVisible(_fHasSubCommands != FALSE);
            peGlyph->SetLayoutPos(_fHasSubCommands ? ROSUI_LP_LEFT : LAYOUTPOS_NONE);
        }
    }
}

void
CSplitButton::SetEvaluatingAsyncState(
    _In_ bool fEvaluating)
{
    DirectUI::Element *pe;

    pe = FindChild(this, L"ActionButton");
    if (pe && pe->GetClassInfoW()->IsSubclassOf(Button3d::Class))
        static_cast<Button3d *>(pe)->SetEvaluatingAsyncState(fEvaluating);

    pe = FindChild(this, L"SplitMenuButton");
    if (pe && pe->GetClassInfoW()->IsSubclassOf(Button3d::Class))
        static_cast<Button3d *>(pe)->SetEvaluatingAsyncState(fEvaluating);
}

/*
 * Space and enter run the command, down opens the menu. The pressed look shows on key down
 * and the command runs when the key comes up on the same button.
 */
void
CSplitButton::OnInput(
    DirectUI::InputEvent *pie)
{
    if (pie->nStage == GMF_DIRECT && pie->nDevice == GINPUT_KEYBOARD)
    {
        WCHAR ch = static_cast<DirectUI::KeyboardEvent *>(pie)->ch;
        PCWSTR pszTarget = NULL;

        if (ch == VK_SPACE || ch == VK_RETURN)
            pszTarget = L"ActionButton";
        else if (ch == VK_DOWN && _fHasSubCommands)
            pszTarget = L"ActionButton";
        else if (ch == VK_DOWN && _fSplit)
            pszTarget = L"SplitMenuButton";

        DirectUI::Button *pb = pszTarget ? static_cast<DirectUI::Button *>(FindChild(this, pszTarget)) : NULL;

        if (pb)
        {
            if (pie->nCode == GKEY_DOWN)
            {
                _pbKeyPressed = pb;
                pb->SetPressed(true);
                pie->fHandled = true;
            }
            else if (pie->nCode == GKEY_UP && _pbKeyPressed == pb && pb->GetPressed())
            {
                pb->SetPressed(false);
                pb->DefaultAction();
                pie->fHandled = true;
            }
        }
    }

    if (!pie->fHandled)
        DirectUI::Element::OnInput(pie);
}

void
CSplitButton::OnEvent(
    DirectUI::Event *pev)
{
    DirectUI::Element *peMenu = FindChildIfAtom(this, L"SplitMenuButton");

    if (pev->nStage == GMF_BUBBLED && pev->uidType.m_Value == DirectUI::Button::Click().m_Value)
    {
        _DoAction(_fSplit && pev->peTarget == peMenu);
        pev->fHandled = true;
    }

    if (!pev->fHandled)
        DirectUI::Element::OnEvent(pev);
}

/* Hovering either half highlights both; losing focus lets go of a key press */
void
CSplitButton::OnPropertyChanged(
    const DirectUI::PropertyInfo *ppi,
    int iIndex,
    DirectUI::Value *pvOld,
    DirectUI::Value *pvNew)
{
    if (ppi == DirectUI::Element::MouseWithinProp() && (int)(ppi->Flags & 3) == iIndex)
    {
        ULONG dwDeferCookie = 0;
        bool fWithin = pvNew->GetBool();
        DirectUI::Element *pe;

        StartDefer(&dwDeferCookie);
        pe = FindChild(this, L"ActionButton");
        if (pe)
            pe->SetSelected(fWithin);
        if (_fSplit)
        {
            pe = FindChild(this, L"SplitMenuButton");
            if (pe)
                pe->SetSelected(fWithin);
        }
        if (dwDeferCookie)
            EndDefer(dwDeferCookie);
    }
    else if (ppi == DirectUI::Element::KeyFocusedProp() && (int)(ppi->Flags & 3) == iIndex)
    {
        if (_pbKeyPressed)
        {
            _pbKeyPressed->SetPressed(false);
            _pbKeyPressed = NULL;
        }
    }
    DirectUI::Element::OnPropertyChanged(ppi, iIndex, pvOld, pvNew);
}

/* Pressing the action half presses the drop-down half with it */
void
CSplitButton::OnListenedPropertyChanged(
    DirectUI::Element *peFrom,
    const DirectUI::PropertyInfo *ppi,
    int iIndex,
    DirectUI::Value *pvOld,
    DirectUI::Value *pvNew)
{
    if (_punkSite && ppi == DirectUI::Button::PressedProp() && (int)(ppi->Flags & 3) == iIndex &&
        peFrom == FindChild(this, L"ActionButton"))
    {
        DirectUI::Element *peMenu = FindChild(this, L"SplitMenuButton");

        if (peMenu)
            static_cast<DirectUI::Button *>(peMenu)->SetPressed(pvNew->GetBool());
    }
}

HRESULT
CSplitButton::DefaultAction()
{
    _DoAction(FALSE);
    return S_OK;
}

/*
 * Runs the command, or drops its menu. The command module's worker window does the work when
 * the button sits in a command module, so the click returns first.
 */
void
CSplitButton::_DoAction(
    _In_ BOOL fShowMenu)
{
    DirectUI::Element *peParent = GetParent();
    HWND hwndWorker = NULL;
    BOOL fMenuNotUp = TRUE;

    IUnknown_SetSite(_psf, _punkSite);
    if (!_fSplit)
    {
        SFGAOF sfgao = SFGAO_FOLDER;
        PCUITEMID_CHILD pidl = _pidl;

        /* A command with sub commands is a folder of them and drops its menu */
        fShowMenu = SUCCEEDED(_psf->GetAttributesOf(1, &pidl, &sfgao)) && (sfgao & SFGAO_FOLDER);
    }

    if (peParent && peParent->GetClassInfoW()->IsSubclassOf(CCommandModuleInner::Class))
    {
        CCommandModuleInner *pModule = static_cast<CCommandModuleInner *>(peParent);

        hwndWorker = pModule->GetWorkerWindow();
        fMenuNotUp = pModule->GetMenuOwner() != this;
    }

    if (fShowMenu)
    {
        IShellFolder *psfMenu;

        if (fMenuNotUp && SUCCEEDED(_psf->BindToObject(_pidl, NULL, IID_PPV_ARGS(&psfMenu))))
        {
            FOLDERMENUREQUEST *pRequest = (FOLDERMENUREQUEST *)LocalAlloc(LPTR, sizeof(*pRequest));

            if (pRequest)
            {
                DirectUI::Element *peAnchor = FindChild(this, L"ActionButton");
                IUnknown *punkMenuSite = _pspCommandSite ? (IUnknown *)_pspCommandSite : _punkSite;
                BOOL fPosted = FALSE;

                if (SUCCEEDED(DUI_GetElementScreenBounds(peAnchor ? peAnchor : this, &pRequest->rcExclude)))
                {
                    pRequest->psf = psfMenu;
                    psfMenu->AddRef();
                    pRequest->punkSite = punkMenuSite;
                    if (punkMenuSite)
                        punkMenuSite->AddRef();
                    pRequest->peOwner = this;

                    if (hwndWorker)
                        fPosted = PostMessageW(hwndWorker, CMWM_SHOWFOLDERMENU, (WPARAM)pRequest, 0);
                    else
                        ShowFolderMenu(pRequest->psf, pRequest->punkSite, &pRequest->rcExclude, 0, FALSE);
                }

                if (!fPosted)
                {
                    if (pRequest->punkSite)
                        pRequest->punkSite->Release();
                    if (pRequest->psf)
                        pRequest->psf->Release();
                    LocalFree(pRequest);
                }
            }
            psfMenu->Release();
        }
    }
    else
    {
        COMMANDINVOKEREQUEST *pRequest = (COMMANDINVOKEREQUEST *)LocalAlloc(LPTR, sizeof(*pRequest));

        if (pRequest)
        {
            BOOL fPosted = FALSE;
            RECT rc;
            HWND hwndRoot = NULL;

            if (SUCCEEDED(DUI_GetElementScreenBounds(this, &rc)))
            {
                IUnknown *punkButtonSite;

                DUI_GetElementRootHWND(this, &hwndRoot);
                if (SUCCEEDED(CCommandButtonSite::s_CreateInstance(_punkSite, hwndRoot, &rc,
                                                                   IID_PPV_ARGS(&punkButtonSite))))
                {
                    pRequest->psf = _psf;
                    _psf->AddRef();
                    pRequest->punkSite = punkButtonSite;
                    punkButtonSite->AddRef();

                    if (SUCCEEDED(SHILCloneFirst(_pidl, &pRequest->pidl)))
                    {
                        if (hwndWorker)
                        {
                            fPosted = PostMessageW(hwndWorker, CMWM_INVOKECOMMAND, (WPARAM)pRequest, 0);
                        }
                        else
                        {
                            SHInvokeCommandWithFlagsAndSite(NULL, pRequest->punkSite, pRequest->psf, pRequest->pidl,
                                                            0, NULL);
                        }
                    }
                    punkButtonSite->Release();
                }
            }

            if (!fPosted)
            {
                CoTaskMemFree(pRequest->pidl);
                if (pRequest->punkSite)
                    pRequest->punkSite->Release();
                if (pRequest->psf)
                    pRequest->psf->Release();
                LocalFree(pRequest);
            }
        }
    }

    IUnknown_SetSite(_psf, NULL);
}
