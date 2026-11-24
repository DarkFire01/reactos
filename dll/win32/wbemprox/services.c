/*
 * Copyright 2012 Hans Leidekker for CodeWeavers
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

#define COBJMACROS

#include <stdarg.h>
#ifdef __REACTOS__
#include <wchar.h>
#endif

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "wbemcli.h"

#include "wine/debug.h"
#include "wbemprox_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wbemprox);

struct client_security
{
    IClientSecurity IClientSecurity_iface;
};

static inline struct client_security *impl_from_IClientSecurity( IClientSecurity *iface )
{
    return CONTAINING_RECORD( iface, struct client_security, IClientSecurity_iface );
}

static HRESULT WINAPI client_security_QueryInterface(
    IClientSecurity *iface,
    REFIID riid,
    void **ppvObject )
{
    struct client_security *cs = impl_from_IClientSecurity( iface );

    TRACE("%p %s %p\n", cs, debugstr_guid( riid ), ppvObject );

    if ( IsEqualGUID( riid, &IID_IClientSecurity ) ||
         IsEqualGUID( riid, &IID_IUnknown ) )
    {
        *ppvObject = cs;
    }
    else
    {
        FIXME("interface %s not implemented\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }
    IClientSecurity_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI client_security_AddRef(
    IClientSecurity *iface )
{
    FIXME("%p\n", iface);
    return 2;
}

static ULONG WINAPI client_security_Release(
    IClientSecurity *iface )
{
    FIXME("%p\n", iface);
    return 1;
}

static HRESULT WINAPI client_security_QueryBlanket(
    IClientSecurity *iface,
    IUnknown *pProxy,
    DWORD *pAuthnSvc,
    DWORD *pAuthzSvc,
    OLECHAR **pServerPrincName,
    DWORD *pAuthnLevel,
    DWORD *pImpLevel,
    void **pAuthInfo,
    DWORD *pCapabilities )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI client_security_SetBlanket(
    IClientSecurity *iface,
    IUnknown *pProxy,
    DWORD AuthnSvc,
    DWORD AuthzSvc,
    OLECHAR *pServerPrincName,
    DWORD AuthnLevel,
    DWORD ImpLevel,
    void *pAuthInfo,
    DWORD Capabilities )
{
    static const OLECHAR defaultW[] =
        {'<','C','O','L','E','_','D','E','F','A','U','L','T','_','P','R','I','N','C','I','P','A','L','>',0};
    const OLECHAR *princname = (pServerPrincName == COLE_DEFAULT_PRINCIPAL) ? defaultW : pServerPrincName;

    FIXME("%p, %p, %u, %u, %s, %u, %u, %p, 0x%08x\n", iface, pProxy, AuthnSvc, AuthzSvc,
          debugstr_w(princname), AuthnLevel, ImpLevel, pAuthInfo, Capabilities);
    return WBEM_NO_ERROR;
}

static HRESULT WINAPI client_security_CopyProxy(
    IClientSecurity *iface,
    IUnknown *pProxy,
    IUnknown **ppCopy )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static const IClientSecurityVtbl client_security_vtbl =
{
    client_security_QueryInterface,
    client_security_AddRef,
    client_security_Release,
    client_security_QueryBlanket,
    client_security_SetBlanket,
    client_security_CopyProxy
};

IClientSecurity client_security = { &client_security_vtbl };

struct async_header
{
    IWbemObjectSink *sink;
    void (*proc)( struct async_header * );
    HANDLE cancel;
    HANDLE wait;
};

struct async_query
{
    struct async_header hdr;
    enum wbm_namespace ns;
    WCHAR *str;
};

struct event_subscription
{
    struct list entry;
    IWbemObjectSink *sink;
    void *cookie;
};

static void free_async( struct async_header *async )
{
    if (async->sink) IWbemObjectSink_Release( async->sink );
    CloseHandle( async->cancel );
    CloseHandle( async->wait );
    heap_free( async );
}

static BOOL init_async( struct async_header *async, IWbemObjectSink *sink,
                        void (*proc)(struct async_header *) )
{
    if (!(async->wait = CreateEventW( NULL, FALSE, FALSE, NULL ))) return FALSE;
    if (!(async->cancel = CreateEventW( NULL, FALSE, FALSE, NULL )))
    {
        CloseHandle( async->wait );
        return FALSE;
    }
    async->proc = proc;
    async->sink = sink;
    IWbemObjectSink_AddRef( sink );
    return TRUE;
}

static DWORD CALLBACK async_proc( LPVOID param )
{
    struct async_header *async = param;
    HANDLE wait = async->wait;

    async->proc( async );

    WaitForSingleObject( async->cancel, INFINITE );
    SetEvent( wait );
    return ERROR_SUCCESS;
}

static HRESULT queue_async( struct async_header *async )
{
    if (QueueUserWorkItem( async_proc, async, WT_EXECUTELONGFUNCTION )) return S_OK;
    return HRESULT_FROM_WIN32( GetLastError() );
}

struct wbem_services
{
    IWbemServices IWbemServices_iface;
    LONG refs;
    CRITICAL_SECTION cs;
    WCHAR *namespace;
    enum wbm_namespace ns;
    struct async_header *async;
    struct list subscriptions;
};

static void free_subscription( struct event_subscription *sub, HRESULT status )
{
    WmiProvUnsubscribe( sub->cookie );
    IWbemObjectSink_SetStatus( sub->sink, WBEM_STATUS_COMPLETE, status, NULL, NULL );
    IWbemObjectSink_Release( sub->sink );
    heap_free( sub );
}

static inline struct wbem_services *impl_from_IWbemServices( IWbemServices *iface )
{
    return CONTAINING_RECORD( iface, struct wbem_services, IWbemServices_iface );
}

static ULONG WINAPI wbem_services_AddRef(
    IWbemServices *iface )
{
    struct wbem_services *ws = impl_from_IWbemServices( iface );
    return InterlockedIncrement( &ws->refs );
}

static ULONG WINAPI wbem_services_Release(
    IWbemServices *iface )
{
    struct wbem_services *ws = impl_from_IWbemServices( iface );
    LONG refs = InterlockedDecrement( &ws->refs );
    if (!refs)
    {
        struct event_subscription *sub, *next;

        TRACE("destroying %p\n", ws);

        LIST_FOR_EACH_ENTRY_SAFE( sub, next, &ws->subscriptions, struct event_subscription, entry )
        {
            list_remove( &sub->entry );
            free_subscription( sub, WBEM_E_CALL_CANCELLED );
        }

        EnterCriticalSection( &ws->cs );
        if (ws->async) SetEvent( ws->async->cancel );
        LeaveCriticalSection( &ws->cs );
        if (ws->async)
        {
            WaitForSingleObject( ws->async->wait, INFINITE );
            free_async( ws->async );
        }
        ws->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &ws->cs );
        heap_free( ws->namespace );
        heap_free( ws );
    }
    return refs;
}

static HRESULT WINAPI wbem_services_QueryInterface(
    IWbemServices *iface,
    REFIID riid,
    void **ppvObject )
{
    struct wbem_services *ws = impl_from_IWbemServices( iface );

    TRACE("%p %s %p\n", ws, debugstr_guid( riid ), ppvObject );

    if ( IsEqualGUID( riid, &IID_IWbemServices ) ||
         IsEqualGUID( riid, &IID_IUnknown ) )
    {
        *ppvObject = ws;
    }
    else if ( IsEqualGUID( riid, &IID_IClientSecurity ) )
    {
        *ppvObject = &client_security;
        return S_OK;
    }
    else
    {
        FIXME("interface %s not implemented\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }
    IWbemServices_AddRef( iface );
    return S_OK;
}

static HRESULT WINAPI wbem_services_OpenNamespace(
    IWbemServices *iface,
    const BSTR strNamespace,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemServices **ppWorkingNamespace,
    IWbemCallResult **ppResult )
{
    struct wbem_services *ws = impl_from_IWbemServices( iface );

    TRACE("%p, %s, 0x%08x, %p, %p, %p\n", iface, debugstr_w(strNamespace), lFlags,
          pCtx, ppWorkingNamespace, ppResult);

    if (!strNamespace || ws->namespace || get_namespace_from_string( strNamespace ) == WBEMPROX_NAMESPACE_LAST)
        return WBEM_E_INVALID_NAMESPACE;

    return WbemServices_create( strNamespace, (void **)ppWorkingNamespace );
}

static HRESULT WINAPI wbem_services_CancelAsyncCall(
    IWbemServices *iface,
    IWbemObjectSink *pSink )
{
    struct wbem_services *services = impl_from_IWbemServices( iface );
    struct event_subscription *sub;
    struct async_header *async;

    TRACE("%p, %p\n", iface, pSink);

    if (!pSink) return WBEM_E_INVALID_PARAMETER;

    EnterCriticalSection( &services->cs );

    LIST_FOR_EACH_ENTRY( sub, &services->subscriptions, struct event_subscription, entry )
    {
        if (sub->sink != pSink) continue;
        list_remove( &sub->entry );
        LeaveCriticalSection( &services->cs );
        free_subscription( sub, WBEM_E_CALL_CANCELLED );
        return S_OK;
    }

    if (!(async = services->async))
    {
        LeaveCriticalSection( &services->cs );
        return WBEM_E_INVALID_PARAMETER;
    }
    services->async = NULL;
    SetEvent( async->cancel );

    LeaveCriticalSection( &services->cs );

    WaitForSingleObject( async->wait, INFINITE );
    free_async( async );
    return S_OK;
}

static HRESULT WINAPI wbem_services_QueryObjectSink(
    IWbemServices *iface,
    LONG lFlags,
    IWbemObjectSink **ppResponseHandler )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

enum wbm_namespace get_namespace_from_string( const WCHAR *namespace )
{
    static const WCHAR cimv2W[] = {'c','i','m','v','2',0};
    static const WCHAR defaultW[] = {'d','e','f','a','u','l','t',0};
    static const WCHAR wmiW[] = {'w','m','i',0};

    if (!wcsicmp( namespace, cimv2W ) || !wcsicmp( namespace, defaultW )) return WBEMPROX_NAMESPACE_CIMV2;
    if (!wcsicmp( namespace, wmiW )) return WBEMPROX_NAMESPACE_WMI;
    return WBEMPROX_NAMESPACE_LAST;
}

HRESULT parse_path( enum wbm_namespace ns, const WCHAR *str, struct path **ret )
{
    struct path *path;
    const WCHAR *p = str, *q;
    UINT len;

    if (!(path = heap_alloc_zero( sizeof(*path) ))) return E_OUTOFMEMORY;

    if (*p == '\\')
    {
        static const WCHAR cimv2W[] = {'R','O','O','T','\\','C','I','M','V','2',0};
        static const WCHAR wmiW[] = {'R','O','O','T','\\','W','M','I',0};
        const WCHAR *expected = (ns == WBEMPROX_NAMESPACE_WMI) ? wmiW : cimv2W;
        WCHAR server[MAX_COMPUTERNAME_LENGTH+1];
        DWORD server_len = ARRAY_SIZE(server);

        p++;
        if (*p != '\\')
        {
            heap_free( path );
            return WBEM_E_INVALID_OBJECT_PATH;
        }
        p++;

        q = p;
        while (*p && *p != '\\') p++;
        if (!*p)
        {
            heap_free( path );
            return WBEM_E_INVALID_OBJECT_PATH;
        }

        len = p - q;
        if (!GetComputerNameW( server, &server_len ) || server_len != len || _wcsnicmp( q, server, server_len ))
        {
            heap_free( path );
            return WBEM_E_NOT_SUPPORTED;
        }

        q = ++p;
        while (*p && *p != ':') p++;
        if (!*p)
        {
            heap_free( path );
            return WBEM_E_INVALID_OBJECT_PATH;
        }

        len = p - q;
        if (len != lstrlenW( expected ) || _wcsnicmp( q, expected, len ))
        {
            heap_free( path );
            return WBEM_E_INVALID_NAMESPACE;
        }
        p++;
    }

    q = p;
    while (*p && *p != '.') p++;

    len = p - q;
    if (!(path->class = heap_alloc( (len + 1) * sizeof(WCHAR) )))
    {
        heap_free( path );
        return E_OUTOFMEMORY;
    }
    memcpy( path->class, q, len * sizeof(WCHAR) );
    path->class[len] = 0;
    path->class_len = len;

    if (p[0] == '.' && p[1])
    {
        q = ++p;
        while (*q) q++;

        len = q - p;
        if (!(path->filter = heap_alloc( (len + 1) * sizeof(WCHAR) )))
        {
            heap_free( path->class );
            heap_free( path );
            return E_OUTOFMEMORY;
        }
        memcpy( path->filter, p, len * sizeof(WCHAR) );
        path->filter[len] = 0;
        path->filter_len = len;
    }
    *ret = path;
    return S_OK;
}

void free_path( struct path *path )
{
    if (!path) return;
    heap_free( path->class );
    heap_free( path->filter );
    heap_free( path );
}

WCHAR *query_from_path( const struct path *path )
{
    static const WCHAR selectW[] =
        {'S','E','L','E','C','T',' ','*',' ','F','R','O','M',' ','%','s',' ',
         'W','H','E','R','E',' ','%','s',0};
    static const WCHAR select_allW[] =
        {'S','E','L','E','C','T',' ','*',' ','F','R','O','M',' ',0};
    WCHAR *query;
    UINT len;

    if (path->filter)
    {
        len = path->class_len + path->filter_len + ARRAY_SIZE(selectW);
        if (!(query = heap_alloc( len * sizeof(WCHAR) ))) return NULL;
        swprintf( query, selectW, path->class, path->filter );
    }
    else
    {
        len = path->class_len + ARRAY_SIZE(select_allW);
        if (!(query = heap_alloc( len * sizeof(WCHAR) ))) return NULL;
        lstrcpyW( query, select_allW );
        lstrcatW( query, path->class );
    }
    return query;
}

static HRESULT create_instance_enum( enum wbm_namespace ns, const struct path *path, IEnumWbemClassObject **iter )
{
    WCHAR *query;
    HRESULT hr;

    if (!(query = query_from_path( path ))) return E_OUTOFMEMORY;
    hr = exec_query( ns, query, iter );
    heap_free( query );
    return hr;
}

HRESULT get_object( enum wbm_namespace ns, const WCHAR *object_path, IWbemClassObject **obj )
{
    IEnumWbemClassObject *iter;
    struct path *path;
    HRESULT hr;

    hr = parse_path( ns, object_path, &path );
    if (hr != S_OK) return hr;

    /* root\wmi hands out real class definitions and reports missing instances */
    if (ns == WBEMPROX_NAMESPACE_WMI && !path->filter)
        hr = exec_class_enum( ns, path->class, NULL, 0, &iter );
    else
        hr = create_instance_enum( ns, path, &iter );
    if (hr != S_OK)
    {
        free_path( path );
        return hr;
    }
    if (ns == WBEMPROX_NAMESPACE_WMI && IEnumWbemClassObject_Skip( iter, WBEM_INFINITE, 1 ) != WBEM_S_NO_ERROR)
        hr = WBEM_E_NOT_FOUND;
    if (hr == S_OK)
    {
        IEnumWbemClassObject_Reset( iter );
        hr = create_class_object( ns, path->class, iter, 0, NULL, obj );
    }
    IEnumWbemClassObject_Release( iter );
    free_path( path );
    return hr;
}

static HRESULT WINAPI wbem_services_GetObject(
    IWbemServices *iface,
    const BSTR strObjectPath,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemClassObject **ppObject,
    IWbemCallResult **ppCallResult )
{
    struct wbem_services *ws = impl_from_IWbemServices( iface );

    TRACE("%p, %s, 0x%08x, %p, %p, %p\n", iface, debugstr_w(strObjectPath), lFlags,
          pCtx, ppObject, ppCallResult);

    if (lFlags) FIXME("unsupported flags 0x%08x\n", lFlags);

    if (!strObjectPath || !strObjectPath[0])
        return create_class_object( ws->ns, NULL, NULL, 0, NULL, ppObject );

    return get_object( ws->ns, strObjectPath, ppObject );
}

/* Completes a call on the sink of an async request that was served synchronously */
static HRESULT complete_async( IWbemObjectSink *sink, IWbemClassObject *obj, IEnumWbemClassObject *iter, HRESULT hr )
{
    if (!sink) return WBEM_E_INVALID_PARAMETER;

    if (obj) IWbemObjectSink_Indicate( sink, 1, &obj );
    if (iter)
    {
        for (;;)
        {
            IWbemClassObject *next;
            ULONG count;

            IEnumWbemClassObject_Next( iter, WBEM_INFINITE, 1, &next, &count );
            if (!count) break;
            IWbemObjectSink_Indicate( sink, 1, &next );
            IWbemClassObject_Release( next );
        }
    }
    IWbemObjectSink_SetStatus( sink, WBEM_STATUS_COMPLETE, hr, NULL, NULL );
    return S_OK;
}

static HRESULT WINAPI wbem_services_GetObjectAsync(
    IWbemServices *iface,
    const BSTR strObjectPath,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    IWbemClassObject *obj = NULL;
    HRESULT hr;

    TRACE("%p, %s, 0x%08x, %p, %p\n", iface, debugstr_w(strObjectPath), lFlags, pCtx, pResponseHandler);

    if (!pResponseHandler) return WBEM_E_INVALID_PARAMETER;

    hr = IWbemServices_GetObject( iface, strObjectPath, lFlags, pCtx, &obj, NULL );
    complete_async( pResponseHandler, obj, NULL, hr );
    if (obj) IWbemClassObject_Release( obj );
    return S_OK;
}

static HRESULT WINAPI wbem_services_PutClass(
    IWbemServices *iface,
    IWbemClassObject *pObject,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemCallResult **ppCallResult )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_PutClassAsync(
    IWbemServices *iface,
    IWbemClassObject *pObject,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_DeleteClass(
    IWbemServices *iface,
    const BSTR strClass,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemCallResult **ppCallResult )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_DeleteClassAsync(
    IWbemServices *iface,
    const BSTR strClass,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_CreateClassEnum(
    IWbemServices *iface,
    const BSTR strSuperclass,
    LONG lFlags,
    IWbemContext *pCtx,
    IEnumWbemClassObject **ppEnum )
{
    struct wbem_services *ws = impl_from_IWbemServices( iface );

    TRACE("%p, %s, 0x%08x, %p, %p\n", iface, debugstr_w(strSuperclass), lFlags, pCtx, ppEnum);

    if (!ppEnum) return WBEM_E_INVALID_PARAMETER;
    return exec_class_enum( ws->ns, NULL, strSuperclass, lFlags, ppEnum );
}

static HRESULT WINAPI wbem_services_CreateClassEnumAsync(
    IWbemServices *iface,
    const BSTR strSuperclass,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    IEnumWbemClassObject *iter = NULL;
    HRESULT hr;

    TRACE("%p, %s, 0x%08x, %p, %p\n", iface, debugstr_w(strSuperclass), lFlags, pCtx, pResponseHandler);

    if (!pResponseHandler) return WBEM_E_INVALID_PARAMETER;

    hr = IWbemServices_CreateClassEnum( iface, strSuperclass, lFlags, pCtx, &iter );
    complete_async( pResponseHandler, NULL, iter, hr );
    if (iter) IEnumWbemClassObject_Release( iter );
    return S_OK;
}

static HRESULT WINAPI wbem_services_PutInstance(
    IWbemServices *iface,
    IWbemClassObject *pInst,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemCallResult **ppCallResult )
{
    struct wbem_services *ws = impl_from_IWbemServices( iface );

    TRACE("%p, %p, 0x%08x, %p, %p\n", iface, pInst, lFlags, pCtx, ppCallResult);

    if (!pInst) return WBEM_E_INVALID_PARAMETER;
    if (ws->ns == WBEMPROX_NAMESPACE_WMI) return WmiProvPutInstance( pInst );

    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_PutInstanceAsync(
    IWbemServices *iface,
    IWbemClassObject *pInst,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    HRESULT hr;

    TRACE("%p, %p, 0x%08x, %p, %p\n", iface, pInst, lFlags, pCtx, pResponseHandler);

    if (!pResponseHandler) return WBEM_E_INVALID_PARAMETER;

    hr = IWbemServices_PutInstance( iface, pInst, lFlags, pCtx, NULL );
    complete_async( pResponseHandler, NULL, NULL, hr );
    return S_OK;
}

static HRESULT WINAPI wbem_services_DeleteInstance(
    IWbemServices *iface,
    const BSTR strObjectPath,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemCallResult **ppCallResult )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_DeleteInstanceAsync(
    IWbemServices *iface,
    const BSTR strObjectPath,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_CreateInstanceEnum(
    IWbemServices *iface,
    const BSTR strClass,
    LONG lFlags,
    IWbemContext *pCtx,
    IEnumWbemClassObject **ppEnum )
{
    struct wbem_services *ws = impl_from_IWbemServices( iface );
    struct path *path;
    HRESULT hr;

    TRACE("%p, %s, 0%08x, %p, %p\n", iface, debugstr_w(strClass), lFlags, pCtx, ppEnum);

    if (lFlags) FIXME("unsupported flags 0x%08x\n", lFlags);

    hr = parse_path( ws->ns, strClass, &path );
    if (hr != S_OK) return hr;

    hr = create_instance_enum( ws->ns, path, ppEnum );
    free_path( path );
    return hr;
}

static HRESULT WINAPI wbem_services_CreateInstanceEnumAsync(
    IWbemServices *iface,
    const BSTR strFilter,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    IEnumWbemClassObject *iter = NULL;
    HRESULT hr;

    TRACE("%p, %s, 0x%08x, %p, %p\n", iface, debugstr_w(strFilter), lFlags, pCtx, pResponseHandler);

    if (!pResponseHandler) return WBEM_E_INVALID_PARAMETER;

    hr = IWbemServices_CreateInstanceEnum( iface, strFilter, lFlags, pCtx, &iter );
    complete_async( pResponseHandler, NULL, iter, hr );
    if (iter) IEnumWbemClassObject_Release( iter );
    return S_OK;
}

static HRESULT WINAPI wbem_services_ExecQuery(
    IWbemServices *iface,
    const BSTR strQueryLanguage,
    const BSTR strQuery,
    LONG lFlags,
    IWbemContext *pCtx,
    IEnumWbemClassObject **ppEnum )
{
    static const WCHAR wqlW[] = {'W','Q','L',0};
    struct wbem_services *ws = impl_from_IWbemServices( iface );

    TRACE("%p, %s, %s, 0x%08x, %p, %p\n", iface, debugstr_w(strQueryLanguage),
          debugstr_w(strQuery), lFlags, pCtx, ppEnum);

    if (!strQueryLanguage || !strQuery || !strQuery[0]) return WBEM_E_INVALID_PARAMETER;
    if (wcsicmp( strQueryLanguage, wqlW )) return WBEM_E_INVALID_QUERY_TYPE;
    return exec_query( ws->ns, strQuery, ppEnum );
}

static void async_exec_query( struct async_header *hdr )
{
    struct async_query *query = (struct async_query *)hdr;
    IEnumWbemClassObject *result;
    IWbemClassObject *obj;
    ULONG count;
    HRESULT hr;

    hr = exec_query( query->ns, query->str, &result );
    if (hr == S_OK)
    {
        for (;;)
        {
            IEnumWbemClassObject_Next( result, WBEM_INFINITE, 1, &obj, &count );
            if (!count) break;
            IWbemObjectSink_Indicate( query->hdr.sink, 1, &obj );
            IWbemClassObject_Release( obj );
        }
        IEnumWbemClassObject_Release( result );
    }
    IWbemObjectSink_SetStatus( query->hdr.sink, WBEM_STATUS_COMPLETE, hr, NULL, NULL );
    heap_free( query->str );
}

static HRESULT WINAPI wbem_services_ExecQueryAsync(
    IWbemServices *iface,
    const BSTR strQueryLanguage,
    const BSTR strQuery,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    struct wbem_services *services = impl_from_IWbemServices( iface );
    IWbemObjectSink *sink;
    HRESULT hr = E_OUTOFMEMORY;
    struct async_header *async;
    struct async_query *query;

    TRACE("%p, %s, %s, 0x%08x, %p, %p\n", iface, debugstr_w(strQueryLanguage), debugstr_w(strQuery),
          lFlags, pCtx, pResponseHandler);

    if (!pResponseHandler) return WBEM_E_INVALID_PARAMETER;

    hr = IWbemObjectSink_QueryInterface( pResponseHandler, &IID_IWbemObjectSink, (void **)&sink );
    if (FAILED(hr)) return hr;

    EnterCriticalSection( &services->cs );

    if (services->async)
    {
        FIXME("handle more than one pending async\n");
        hr = WBEM_E_FAILED;
        goto done;
    }
    if (!(query = heap_alloc_zero( sizeof(*query) ))) goto done;
    async = (struct async_header *)query;

    if (!(init_async( async, sink, async_exec_query )))
    {
        free_async( async );
        goto done;
    }
    query->ns = services->ns;
    if (!(query->str = heap_strdupW( strQuery )))
    {
        free_async( async );
        goto done;
    }
    hr = queue_async( async );
    if (hr == S_OK) services->async = async;
    else
    {
        heap_free( query->str );
        free_async( async );
    }

done:
    LeaveCriticalSection( &services->cs );
    IWbemObjectSink_Release( sink );
    return hr;
}

static HRESULT WINAPI wbem_services_ExecNotificationQuery(
    IWbemServices *iface,
    const BSTR strQueryLanguage,
    const BSTR strQuery,
    LONG lFlags,
    IWbemContext *pCtx,
    IEnumWbemClassObject **ppEnum )
{
    struct wbem_services *ws = impl_from_IWbemServices( iface );

    TRACE("%p, %s, %s, 0x%08x, %p, %p\n", iface, debugstr_w(strQueryLanguage),
          debugstr_w(strQuery), lFlags, pCtx, ppEnum);

    if (!strQuery || !ppEnum) return WBEM_E_INVALID_PARAMETER;
    if (ws->ns == WBEMPROX_NAMESPACE_WMI) return WmiProvCreateEventEnum( strQuery, ppEnum );

    FIXME("event queries are only supported in root\\wmi\n");
    return WBEM_E_NOT_SUPPORTED;
}

static HRESULT WINAPI wbem_services_ExecNotificationQueryAsync(
    IWbemServices *iface,
    const BSTR strQueryLanguage,
    const BSTR strQuery,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    struct wbem_services *services = impl_from_IWbemServices( iface );
    IWbemObjectSink *sink;
    HRESULT hr = E_OUTOFMEMORY;
    struct async_header *async;
    struct async_query *query;

    TRACE("%p, %s, %s, 0x%08x, %p, %p\n", iface, debugstr_w(strQueryLanguage), debugstr_w(strQuery),
          lFlags, pCtx, pResponseHandler);

    if (!pResponseHandler) return WBEM_E_INVALID_PARAMETER;

    hr = IWbemObjectSink_QueryInterface( pResponseHandler, &IID_IWbemObjectSink, (void **)&sink );
    if (FAILED(hr)) return hr;

    /* Events of root\wmi come from kernel WMI notifications */
    if (services->ns == WBEMPROX_NAMESPACE_WMI)
    {
        struct event_subscription *sub;

        if (!strQuery) hr = WBEM_E_INVALID_PARAMETER;
        else if (!(sub = heap_alloc_zero( sizeof(*sub) ))) hr = E_OUTOFMEMORY;
        else if ((hr = WmiProvSubscribe( strQuery, sink, &sub->cookie )) != S_OK) heap_free( sub );
        else
        {
            sub->sink = sink;
            IWbemObjectSink_AddRef( sink );
            EnterCriticalSection( &services->cs );
            list_add_tail( &services->subscriptions, &sub->entry );
            LeaveCriticalSection( &services->cs );
        }
        IWbemObjectSink_Release( sink );
        return hr;
    }

    EnterCriticalSection( &services->cs );

    if (services->async)
    {
        FIXME("handle more than one pending async\n");
        hr = WBEM_E_FAILED;
        goto done;
    }
    if (!(query = heap_alloc_zero( sizeof(*query) ))) goto done;
    async = (struct async_header *)query;

    if (!(init_async( async, sink, async_exec_query )))
    {
        free_async( async );
        goto done;
    }
    query->ns = services->ns;
    if (!(query->str = heap_strdupW( strQuery )))
    {
        free_async( async );
        goto done;
    }
    hr = queue_async( async );
    if (hr == S_OK) services->async = async;
    else
    {
        heap_free( query->str );
        free_async( async );
    }

done:
    LeaveCriticalSection( &services->cs );
    IWbemObjectSink_Release( sink );
    return hr;
}

static HRESULT WINAPI wbem_services_ExecMethod(
    IWbemServices *iface,
    const BSTR strObjectPath,
    const BSTR strMethodName,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemClassObject *pInParams,
    IWbemClassObject **ppOutParams,
    IWbemCallResult **ppCallResult )
{
    struct wbem_services *ws = impl_from_IWbemServices( iface );
    IEnumWbemClassObject *result = NULL;
    IWbemClassObject *obj = NULL;
    struct query *query = NULL;
    struct path *path;
    WCHAR *str = NULL;
    class_method *func;
    struct table *table;
    HRESULT hr;

    TRACE("%p, %s, %s, %08x, %p, %p, %p, %p\n", iface, debugstr_w(strObjectPath),
          debugstr_w(strMethodName), lFlags, pCtx, pInParams, ppOutParams, ppCallResult);

    if (lFlags) FIXME("flags %08x not supported\n", lFlags);

    if (ws->ns == WBEMPROX_NAMESPACE_WMI)
    {
        if (!strObjectPath || !strMethodName) return WBEM_E_INVALID_PARAMETER;
        return WmiProvExecMethod( strObjectPath, strMethodName, pInParams, ppOutParams );
    }

    if ((hr = parse_path( ws->ns, strObjectPath, &path )) != S_OK) return hr;
    if (!(str = query_from_path( path )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (!(query = create_query( ws->ns )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    hr = parse_query( ws->ns, str, &query->view, &query->mem );
    if (hr != S_OK) goto done;

    hr = execute_view( query->view );
    if (hr != S_OK) goto done;

    hr = EnumWbemClassObject_create( query, (void **)&result );
    if (hr != S_OK) goto done;

    table = get_view_table( query->view, 0 );
    hr = create_class_object( ws->ns, table->name, result, 0, NULL, &obj );
    if (hr != S_OK) goto done;

    hr = get_method( table, strMethodName, &func );
    if (hr != S_OK) goto done;

    hr = func( obj, pInParams, ppOutParams );

done:
    if (result) IEnumWbemClassObject_Release( result );
    if (obj) IWbemClassObject_Release( obj );
    free_query( query );
    free_path( path );
    heap_free( str );
    return hr;
}

static HRESULT WINAPI wbem_services_ExecMethodAsync(
    IWbemServices *iface,
    const BSTR strObjectPath,
    const BSTR strMethodName,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemClassObject *pInParams,
    IWbemObjectSink *pResponseHandler )
{
    IWbemClassObject *out = NULL;
    HRESULT hr;

    TRACE("%p, %s, %s, %08x, %p, %p, %p\n", iface, debugstr_w(strObjectPath),
          debugstr_w(strMethodName), lFlags, pCtx, pInParams, pResponseHandler);

    if (!pResponseHandler) return WBEM_E_INVALID_PARAMETER;

    hr = IWbemServices_ExecMethod( iface, strObjectPath, strMethodName, lFlags, pCtx, pInParams, &out, NULL );
    complete_async( pResponseHandler, out, NULL, hr );
    if (out) IWbemClassObject_Release( out );
    return S_OK;
}

static const IWbemServicesVtbl wbem_services_vtbl =
{
    wbem_services_QueryInterface,
    wbem_services_AddRef,
    wbem_services_Release,
    wbem_services_OpenNamespace,
    wbem_services_CancelAsyncCall,
    wbem_services_QueryObjectSink,
    wbem_services_GetObject,
    wbem_services_GetObjectAsync,
    wbem_services_PutClass,
    wbem_services_PutClassAsync,
    wbem_services_DeleteClass,
    wbem_services_DeleteClassAsync,
    wbem_services_CreateClassEnum,
    wbem_services_CreateClassEnumAsync,
    wbem_services_PutInstance,
    wbem_services_PutInstanceAsync,
    wbem_services_DeleteInstance,
    wbem_services_DeleteInstanceAsync,
    wbem_services_CreateInstanceEnum,
    wbem_services_CreateInstanceEnumAsync,
    wbem_services_ExecQuery,
    wbem_services_ExecQueryAsync,
    wbem_services_ExecNotificationQuery,
    wbem_services_ExecNotificationQueryAsync,
    wbem_services_ExecMethod,
    wbem_services_ExecMethodAsync
};

HRESULT WbemServices_create( const WCHAR *namespace, LPVOID *ppObj )
{
    struct wbem_services *ws;

    TRACE("(%p)\n", ppObj);

    ws = heap_alloc( sizeof(*ws) );
    if (!ws) return E_OUTOFMEMORY;

    ws->IWbemServices_iface.lpVtbl = &wbem_services_vtbl;
    ws->refs      = 1;
    ws->namespace = heap_strdupW( namespace );
    ws->ns        = namespace ? get_namespace_from_string( namespace ) : WBEMPROX_NAMESPACE_CIMV2;
    ws->async     = NULL;
    list_init( &ws->subscriptions );

    if (ws->ns == WBEMPROX_NAMESPACE_LAST)
    {
        heap_free( ws->namespace );
        heap_free( ws );
        return WBEM_E_INVALID_NAMESPACE;
    }
    if (ws->ns == WBEMPROX_NAMESPACE_WMI) WmiProvRefresh();
    InitializeCriticalSection( &ws->cs );
    ws->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": wbemprox_services.cs");

    *ppObj = &ws->IWbemServices_iface;

    TRACE("returning iface %p\n", *ppObj);
    return S_OK;
}
