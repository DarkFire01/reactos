/*
 *    ShellFolderViewDual
 *
 *    Copyright 2016         Mark Jansen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

class CDefViewDual :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IDispatchImpl<IShellFolderViewDual2, &IID_IShellFolderViewDual2>,
    public IConnectionPointContainerImpl<CDefViewDual>,
    public IConnectionPointImpl<CDefViewDual, &DIID_DShellFolderViewEvents>
{
    public:
        CDefViewDual()
        {
        }

        ~CDefViewDual()
        {
        }

        HRESULT STDMETHODCALLTYPE Initialize()
        {
            // Nothing to do for now..
            return S_OK;
        }

        /* Tells every DShellFolderViewEvents sink about a view change; most events take no arguments */
        void FireViewEvent(DISPID dispid, DISPPARAMS *pParams = NULL)
        {
            CComDynamicUnkArray &vec = IConnectionPointImpl<CDefViewDual, &DIID_DShellFolderViewEvents>::m_vec;
            DISPPARAMS params = { NULL, NULL, 0, 0 };

            for (IUnknown **pp = vec.begin(); pp < vec.end(); pp++)
            {
                CComPtr<IDispatch> pdisp;

                if (*pp && SUCCEEDED((*pp)->QueryInterface(IID_PPV_ARG(IDispatch, &pdisp))))
                    pdisp->Invoke(dispid, GUID_NULL, 0, DISPATCH_METHOD, pParams ? pParams : &params, NULL, NULL, NULL);
            }
        }

        /* Sinks that only expose IDispatch are accepted, as the shell's view events always were */
        STDMETHOD(Advise)(IUnknown *pUnkSink, DWORD *pdwCookie) override
        {
            typedef IConnectionPointImpl<CDefViewDual, &DIID_DShellFolderViewEvents> ViewEventsCP;
            IUnknown *punk;
            DWORD dwCookie;

            if (!pdwCookie)
                return E_POINTER;
            *pdwCookie = 0;
            if (!pUnkSink)
                return E_POINTER;

            if (FAILED(pUnkSink->QueryInterface(DIID_DShellFolderViewEvents, (void **)&punk)) &&
                FAILED(pUnkSink->QueryInterface(IID_IDispatch, (void **)&punk)))
            {
                return CONNECT_E_CANNOTCONNECT;
            }

            dwCookie = ViewEventsCP::m_vec.Add(punk);
            if (!dwCookie)
            {
                punk->Release();
                return CONNECT_E_ADVISELIMIT;
            }
            *pdwCookie = dwCookie;
            return S_OK;
        }

        /* A cookie that names no live connection fails quietly instead of asserting */
        STDMETHOD(Unadvise)(DWORD dwCookie) override
        {
            typedef IConnectionPointImpl<CDefViewDual, &DIID_DShellFolderViewEvents> ViewEventsCP;
            CComDynamicUnkArray &vec = ViewEventsCP::m_vec;
            DWORD cSlots = (DWORD)(vec.end() - vec.begin());

            if (!dwCookie || dwCookie > cSlots || !vec.begin()[dwCookie - 1])
                return CONNECT_E_NOCONNECTION;
            return ViewEventsCP::Unadvise(dwCookie);
        }

        // *** IShellFolderViewDual methods ***
        STDMETHOD(get_Application)(IDispatch **app) override
        {
            if (!app) return E_INVALIDARG;

            return CShellDispatch_Constructor(IID_IDispatch, (LPVOID*)app);
        }

        STDMETHOD(get_Parent)(IDispatch **parent) override
        {
            if (!parent) return E_INVALIDARG;
            *parent = NULL;
            FIXME("CDefViewDual::get_Parent is UNIMPLEMENTED (%p, %p)\n", this, parent);
            return E_NOTIMPL;
        }

        STDMETHOD(get_Folder)(Folder **folder) override
        {
            if (!folder) return E_INVALIDARG;
            *folder = NULL;
            FIXME("CDefViewDual::get_Folder is UNIMPLEMENTED (%p, %p)\n", this, folder);
            return E_NOTIMPL;
        }

        STDMETHOD(SelectedItems)(FolderItems **items) override
        {
            if (!items) return E_INVALIDARG;
            *items = NULL;
            FIXME("CDefViewDual::SelectedItems is UNIMPLEMENTED (%p, %p)\n", this, items);
            return E_NOTIMPL;
        }

        STDMETHOD(get_FocusedItem)(FolderItem **item) override
        {
            if (!item) return E_INVALIDARG;
            *item = NULL;
            FIXME("CDefViewDual::get_FocusedItem is UNIMPLEMENTED (%p, %p)\n", this, item);
            return E_NOTIMPL;
        }

        STDMETHOD(SelectItem)(VARIANT *item, int flags) override
        {
            FIXME("CDefViewDual::SelectItem is UNIMPLEMENTED (%p, %p, %i)\n", this, item, flags);
            return E_NOTIMPL;
        }

        STDMETHOD(PopupItemMenu)(FolderItem *item, VARIANT vx, VARIANT vy, BSTR *command) override
        {
            FIXME("CDefViewDual::PopupItemMenu is UNIMPLEMENTED (%p, %p, %s, %s, %p)\n", this, item, wine_dbgstr_variant(&vx), wine_dbgstr_variant(&vy), command);
            return E_NOTIMPL;
        }

        STDMETHOD(get_Script)(IDispatch **script) override
        {
            FIXME("CDefViewDual::get_Script is UNIMPLEMENTED (%p, %p)\n", this, script);
            return E_NOTIMPL;
        }

        STDMETHOD(get_ViewOptions)(long *options) override
        {
            FIXME("CDefViewDual::get_ViewOptions is UNIMPLEMENTED (%p, %p)\n", this, options);
            return E_NOTIMPL;
        }

        // *** IShellFolderViewDual2 methods ***
        STDMETHOD(get_CurrentViewMode)(UINT *mode) override
        {
            FIXME("CDefViewDual::get_CurrentViewMode is UNIMPLEMENTED (%p, %p)\n", this, mode);
            return E_NOTIMPL;
        }

        STDMETHOD(put_CurrentViewMode)(UINT mode) override
        {
            FIXME("CDefViewDual::put_CurrentViewMode is UNIMPLEMENTED (%p, %u)\n", this, mode);
            return E_NOTIMPL;
        }

        STDMETHOD(SelectItemRelative)(int relative) override
        {
            FIXME("CDefViewDual::SelectItemRelative is UNIMPLEMENTED (%p, %i)\n", this, relative);
            return E_NOTIMPL;
        }

    BEGIN_COM_MAP(CDefViewDual)
        COM_INTERFACE_ENTRY_IID(IID_IDispatch, IDispatch)
        COM_INTERFACE_ENTRY_IID(IID_IShellFolderViewDual, IShellFolderViewDual)
        COM_INTERFACE_ENTRY_IID(IID_IShellFolderViewDual2, IShellFolderViewDual2)
        COM_INTERFACE_ENTRY_IID(IID_IConnectionPointContainer, IConnectionPointContainer)
    END_COM_MAP()

    BEGIN_CONNECTION_POINT_MAP(CDefViewDual)
        CONNECTION_POINT_ENTRY(DIID_DShellFolderViewEvents)
    END_CONNECTION_POINT_MAP()
};

/**********************************************************
 *    CDefViewDual_Constructor
 */

HRESULT WINAPI CDefViewDual_Constructor(REFIID riid, LPVOID * ppvOut)
{
    return ShellObjectCreatorInit<CDefViewDual>(riid, ppvOut);
}

void WINAPI CDefViewDual_FireEvent(IShellFolderViewDual *pDual, DISPID dispid)
{
    if (pDual)
        static_cast<CDefViewDual *>(static_cast<IShellFolderViewDual2 *>(pDual))->FireViewEvent(dispid);
}

void
WINAPI
CDefViewDual_FireEventWithArgs(
    _In_opt_ IShellFolderViewDual *pDual,
    _In_ DISPID dispid,
    _In_ DISPPARAMS *pParams)
{
    if (pDual)
        static_cast<CDefViewDual *>(static_cast<IShellFolderViewDual2 *>(pDual))->FireViewEvent(dispid, pParams);
}
