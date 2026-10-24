/*
 * Miscellaneous Marshaling Routines
 *
 * Copyright 2006 Robert Shearman (for CodeWeavers)
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
 */

#include <stdarg.h>
#include <string.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winerror.h"
#include "objbase.h"
#include "servprov.h"
#include "comcat.h"
#include "docobj.h"
#include "shobjidl.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(actxprxy);

HRESULT CALLBACK IServiceProvider_QueryService_Proxy(
    IServiceProvider* This,
    REFGUID guidService,
    REFIID riid,
    void** ppvObject)
{
    TRACE("(%p, %s, %s, %p)\n", This, debugstr_guid(guidService),
        debugstr_guid(riid), ppvObject);

    return IServiceProvider_RemoteQueryService_Proxy(This, guidService, riid,
        (IUnknown **)ppvObject);
}

HRESULT __RPC_STUB IServiceProvider_QueryService_Stub(
    IServiceProvider* This,
    REFGUID guidService,
    REFIID riid,
    IUnknown** ppvObject)
{
    TRACE("(%p, %s, %s, %p)\n", This, debugstr_guid(guidService),
        debugstr_guid(riid), ppvObject);

    return IServiceProvider_QueryService(This, guidService, riid,
        (void **)ppvObject);
}

HRESULT CALLBACK ICatInformation_EnumClassesOfCategories_Proxy(
    ICatInformation *This,
    ULONG cImplemented,
    CATID rgcatidImpl[],
    ULONG cRequired,
    CATID rgcatidReq[],
    IEnumCLSID** ppenumClsid )
{
    TRACE("(%p)\n", This);
    return ICatInformation_RemoteEnumClassesOfCategories_Proxy( This, cImplemented, rgcatidImpl,
                                                                cRequired, rgcatidReq, ppenumClsid );
}

HRESULT __RPC_STUB ICatInformation_EnumClassesOfCategories_Stub(
    ICatInformation *This,
    ULONG cImplemented,
    CATID rgcatidImpl[],
    ULONG cRequired,
    CATID rgcatidReq[],
    IEnumCLSID** ppenumClsid )
{
    TRACE("(%p)\n", This);
    return ICatInformation_EnumClassesOfCategories( This, cImplemented, rgcatidImpl,
                                                    cRequired, rgcatidReq, ppenumClsid );
}

HRESULT CALLBACK ICatInformation_IsClassOfCategories_Proxy(
    ICatInformation *This,
    REFCLSID rclsid,
    ULONG cImplemented,
    CATID rgcatidImpl[],
    ULONG cRequired,
    CATID rgcatidReq[] )
{
    TRACE("(%p)\n", This);
    return ICatInformation_RemoteIsClassOfCategories_Proxy( This, rclsid, cImplemented, rgcatidImpl,
                                                            cRequired, rgcatidReq );
}

HRESULT __RPC_STUB ICatInformation_IsClassOfCategories_Stub(
    ICatInformation *This,
    REFCLSID rclsid,
    ULONG cImplemented,
    CATID rgcatidImpl[],
    ULONG cRequired,
    CATID rgcatidReq[] )
{
    TRACE("(%p)\n", This);
    return ICatInformation_IsClassOfCategories( This, rclsid, cImplemented, rgcatidImpl,
                                                cRequired, rgcatidReq );
}

HRESULT CALLBACK IPrint_Print_Proxy(
    IPrint *This,
    DWORD grfFlags,
    DVTARGETDEVICE **pptd,
    PAGESET **ppPageSet,
    STGMEDIUM *pstgmOptions,
    IContinueCallback *pcallback,
    LONG nFirstPage,
    LONG *pcPagesPrinted,
    LONG *pnLastPage )
{
    TRACE("(%p)\n", This);
    return IPrint_RemotePrint_Proxy( This, grfFlags, pptd, ppPageSet, (RemSTGMEDIUM *)pstgmOptions,
                                     pcallback, nFirstPage, pcPagesPrinted, pnLastPage );
}

HRESULT __RPC_STUB IPrint_Print_Stub(
    IPrint *This,
    DWORD grfFlags,
    DVTARGETDEVICE **pptd,
    PAGESET **ppPageSet,
    RemSTGMEDIUM *pstgmOptions,
    IContinueCallback *pcallback,
    LONG nFirstPage,
    LONG *pcPagesPrinted,
    LONG *pnLastPage )
{
    TRACE("(%p)\n", This);
    return IPrint_Print( This, grfFlags, pptd, ppPageSet, (STGMEDIUM *)pstgmOptions,
                         pcallback, nFirstPage, pcPagesPrinted, pnLastPage );
}

HRESULT CALLBACK IEnumOleDocumentViews_Next_Proxy(
    IEnumOleDocumentViews *This,
    ULONG cViews,
    IOleDocumentView **rgpView,
    ULONG *pcFetched )
{
    TRACE("(%p)\n", This);
    return IEnumOleDocumentViews_RemoteNext_Proxy( This, cViews, rgpView, pcFetched );
}

HRESULT __RPC_STUB IEnumOleDocumentViews_Next_Stub(
    IEnumOleDocumentViews *This,
    ULONG cViews,
    IOleDocumentView **rgpView,
    ULONG *pcFetched )
{
    TRACE("(%p)\n", This);
    return IEnumOleDocumentViews_Next( This, cViews, rgpView, pcFetched );
}

HRESULT CALLBACK IEnumShellItems_Next_Proxy(
    IEnumShellItems *This,
    ULONG celt,
    IShellItem **rgelt,
    ULONG *pceltFetched)
{
    ULONG fetched;
    TRACE("(%p)->(%ld, %p, %p)\n", This, celt, rgelt, pceltFetched);
    if (!pceltFetched) pceltFetched = &fetched;
    return IEnumShellItems_RemoteNext_Proxy(This, celt, rgelt, pceltFetched);
}

HRESULT __RPC_STUB IEnumShellItems_Next_Stub(
    IEnumShellItems *This,
    ULONG celt,
    IShellItem **rgelt,
    ULONG *pceltFetched)
{
    HRESULT hr;
    TRACE("(%p)->(%ld, %p, %p)\n", This, celt, rgelt, pceltFetched);
    *pceltFetched = 0;
    hr = IEnumShellItems_Next(This, celt, rgelt, pceltFetched);
    if (hr == S_OK) *pceltFetched = celt;
    return hr;
}

#ifdef __REACTOS__

HRESULT CALLBACK IEnumExplorerCommand_Next_Proxy(
    IEnumExplorerCommand *This,
    ULONG celt,
    IExplorerCommand **pUICommand,
    ULONG *pceltFetched)
{
    ULONG fetched;
    TRACE("(%p)->(%d, %p, %p)\n", This, celt, pUICommand, pceltFetched);
    if (!pceltFetched) pceltFetched = &fetched;
    return IEnumExplorerCommand_RemoteNext_Proxy(This, celt, pUICommand, pceltFetched);
}

HRESULT __RPC_STUB IEnumExplorerCommand_Next_Stub(
    IEnumExplorerCommand *This,
    ULONG celt,
    IExplorerCommand **pUICommand,
    ULONG *pceltFetched)
{
    HRESULT hr;
    TRACE("(%p)->(%d, %p, %p)\n", This, celt, pUICommand, pceltFetched);
    *pceltFetched = 0;
    hr = IEnumExplorerCommand_Next(This, celt, pUICommand, pceltFetched);
    if (hr == S_OK) *pceltFetched = celt;
    return hr;
}

#endif // __REACTOS__

HRESULT CALLBACK IModalWindow_Show_Proxy(
    IModalWindow *This,
    HWND hwndOwner)
{
    TRACE("(%p)->(%p)\n", This, hwndOwner);
    return IModalWindow_RemoteShow_Proxy(This, hwndOwner);
}

HRESULT __RPC_STUB IModalWindow_Show_Stub(
    IModalWindow *This,
    HWND hwndOwner)
{
    TRACE("(%p)->(%p)\n", This, hwndOwner);
    return IModalWindow_Show(This, hwndOwner);
}

HRESULT __RPC_STUB IFolderView2_GetGroupBy_Stub(
    IFolderView2 *This,
    PROPERTYKEY *pkey,
    BOOL *ascending)
{
    TRACE("(%p)->(%p %p)\n", This, pkey, ascending);
    return IFolderView2_GetGroupBy(This, pkey, ascending);
}

HRESULT __RPC_STUB IFolderView2_GetGroupBy_Proxy(
    IFolderView2 *This,
    PROPERTYKEY *pkey,
    BOOL *ascending)
{
    TRACE("(%p)->(%p %p)\n", This, pkey, ascending);
    return IFolderView2_RemoteGetGroupBy_Proxy(This, pkey, ascending);
}

HRESULT __RPC_STUB IParentAndItem_GetParentAndItem_Stub(
    IParentAndItem *This,
    PIDLIST_ABSOLUTE *parent,
    IShellFolder **folder,
    PITEMID_CHILD *child)
{
    TRACE("(%p)->(%p %p %p)\n", This, parent, folder, child);
    return IParentAndItem_GetParentAndItem(This, parent, folder, child);
}

HRESULT __RPC_STUB IParentAndItem_GetParentAndItem_Proxy(
    IParentAndItem *This,
    PIDLIST_ABSOLUTE *parent,
    IShellFolder **folder,
    PITEMID_CHILD *child)
{
    TRACE("(%p)->(%p %p %p)\n", This, parent, folder, child);
    return IParentAndItem_RemoteGetParentAndItem_Proxy(This, parent, folder, child);
}

#ifdef __REACTOS__

/* PIDLs travel as a ULONG byte count and the bytes, terminator included; NULL is a count of 0 */

static ULONG pidl_wire_size(const ITEMIDLIST *pidl)
{
    ULONG size = 0;

    if (!pidl)
        return 0;

    while (pidl->mkid.cb)
    {
        size += pidl->mkid.cb;
        pidl = (const ITEMIDLIST *)((const BYTE *)pidl + pidl->mkid.cb);
    }
    return size + sizeof(USHORT);
}

static ULONG pidl_user_size(ULONG start, const ITEMIDLIST *pidl)
{
    return start + sizeof(ULONG) + pidl_wire_size(pidl);
}

static unsigned char *pidl_user_marshal(unsigned char *buffer, const ITEMIDLIST *pidl)
{
    ULONG size = pidl_wire_size(pidl);

    memcpy(buffer, &size, sizeof(size));
    buffer += sizeof(size);
    if (size)
    {
        memcpy(buffer, pidl, size);
        buffer += size;
    }
    return buffer;
}

/* The bytes must form a list that ends exactly at the count, or the call is rejected */
static unsigned char *pidl_user_unmarshal(unsigned char *buffer, void **ppidl)
{
    ULONG size, offset = 0;
    const BYTE *bytes;
    void *pidl;

    memcpy(&size, buffer, sizeof(size));
    buffer += sizeof(size);

    if (!size)
    {
        CoTaskMemFree(*ppidl);
        *ppidl = NULL;
        return buffer;
    }

    bytes = buffer;
    if (size < sizeof(USHORT))
        RpcRaiseException(RPC_X_BAD_STUB_DATA);
    while (offset + sizeof(USHORT) <= size)
    {
        USHORT cb;

        memcpy(&cb, bytes + offset, sizeof(cb));
        if (!cb)
            break;
        if (cb < sizeof(USHORT) || cb > size - offset - sizeof(USHORT))
            RpcRaiseException(RPC_X_BAD_STUB_DATA);
        offset += cb;
    }
    if (offset + sizeof(USHORT) != size)
        RpcRaiseException(RPC_X_BAD_STUB_DATA);

    pidl = CoTaskMemRealloc(*ppidl, size);
    if (!pidl)
        RpcRaiseException(E_OUTOFMEMORY);
    memcpy(pidl, bytes, size);
    *ppidl = pidl;
    return buffer + size;
}

static void pidl_user_free(void *pidl)
{
    CoTaskMemFree(pidl);
}

#define PIDL_USER_MARSHAL(type) \
ULONG __RPC_USER type##_UserSize(ULONG *flags, ULONG start, type *pidl) \
{ \
    return pidl_user_size(start, (const ITEMIDLIST *)*pidl); \
} \
unsigned char * __RPC_USER type##_UserMarshal(ULONG *flags, unsigned char *buffer, type *pidl) \
{ \
    return pidl_user_marshal(buffer, (const ITEMIDLIST *)*pidl); \
} \
unsigned char * __RPC_USER type##_UserUnmarshal(ULONG *flags, unsigned char *buffer, type *pidl) \
{ \
    return pidl_user_unmarshal(buffer, (void **)pidl); \
} \
void __RPC_USER type##_UserFree(ULONG *flags, type *pidl) \
{ \
    pidl_user_free((void *)*pidl); \
}

PIDL_USER_MARSHAL(PIDLIST_ABSOLUTE)
PIDL_USER_MARSHAL(PCIDLIST_ABSOLUTE)
PIDL_USER_MARSHAL(PIDLIST_RELATIVE)
PIDL_USER_MARSHAL(PCUIDLIST_RELATIVE)
PIDL_USER_MARSHAL(PITEMID_CHILD)
PIDL_USER_MARSHAL(PCUITEMID_CHILD)

#endif /* __REACTOS__ */
