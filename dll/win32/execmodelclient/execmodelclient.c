/*
 * PROJECT:     ReactOS Execution Model Client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The runtime class that says how an application was started
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A packaged application is started by being activated, and asks this class
 * what the activation was. An application started from a command line was not
 * activated at all, which is an answer rather than a failure: the class has to
 * be there to give it, and a caller that cannot reach the class takes that for
 * the runtime being broken and gives up.
 */

/* INCLUDES *******************************************************************/

#include <stdarg.h>
#include <wchar.h>

#define WIN32_NO_STATUS
#define COBJMACROS

#include <windef.h>
#include <winbase.h>
#include <objbase.h>

/* Ahead of the headers that carry them, so the interface ids land in this module */
#include <initguid.h>

#include <winstring.h>
#include <activation.h>
#include <roapi.h>

#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(execmodelclient);

/* TYPES **********************************************************************/

typedef struct IAppInstanceStatics IAppInstanceStatics;

/*
 * The statics of Windows.ApplicationModel.AppInstance, in the order the runtime
 * class declares them. Every one of them is about the other instances of a
 * packaged application, of which there are none, so the activation is the only
 * one with an answer worth giving.
 */
typedef struct IAppInstanceStaticsVtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IAppInstanceStatics *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IAppInstanceStatics *);
    ULONG (STDMETHODCALLTYPE *Release)(IAppInstanceStatics *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(IAppInstanceStatics *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(IAppInstanceStatics *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(IAppInstanceStatics *, TrustLevel *);
    HRESULT (STDMETHODCALLTYPE *get_RecommendedInstance)(IAppInstanceStatics *, IInspectable **);
    HRESULT (STDMETHODCALLTYPE *GetActivatedEventArgs)(IAppInstanceStatics *, IInspectable **);
    HRESULT (STDMETHODCALLTYPE *FindOrRegisterInstanceForKey)(IAppInstanceStatics *, HSTRING, IInspectable **);
    HRESULT (STDMETHODCALLTYPE *Unregister)(IAppInstanceStatics *);
    HRESULT (STDMETHODCALLTYPE *GetInstances)(IAppInstanceStatics *, IInspectable **);
} IAppInstanceStaticsVtbl;

struct IAppInstanceStatics
{
    const IAppInstanceStaticsVtbl *lpVtbl;
};

typedef struct _APP_INSTANCE_FACTORY
{
    IActivationFactory IActivationFactory_iface;
    IAppInstanceStatics IAppInstanceStatics_iface;
} APP_INSTANCE_FACTORY, *PAPP_INSTANCE_FACTORY;

/* GLOBALS ********************************************************************/

DEFINE_GUID(IID_IAppInstanceStatics,
            0x9d11e77f, 0x9ea6, 0x47af, 0xa6, 0xec, 0x46, 0x78, 0x4c, 0x5b, 0xa2, 0x54);

static const WCHAR AppInstanceClass[] = L"Windows.ApplicationModel.AppInstance";

static APP_INSTANCE_FACTORY AppInstanceFactory;

/* FUNCTIONS ******************************************************************/

/*
 * The object lives in the module rather than on a heap, so it is never freed and
 * its count is only there for callers that check what they are handed back.
 */
static
HRESULT
AppInstanceResolve(
    _In_ REFIID riid,
    _Outptr_ void **object)
{
    if (object == NULL)
        return E_POINTER;

    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IInspectable) ||
        IsEqualIID(riid, &IID_IAgileObject) ||
        IsEqualIID(riid, &IID_IActivationFactory))
    {
        *object = &AppInstanceFactory.IActivationFactory_iface;
        return S_OK;
    }

    if (IsEqualIID(riid, &IID_IAppInstanceStatics))
    {
        *object = &AppInstanceFactory.IAppInstanceStatics_iface;
        return S_OK;
    }

    WARN("%s is not one of ours\n", wine_dbgstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static
HRESULT
AppInstanceName(
    _Out_ HSTRING *name)
{
    if (name == NULL)
        return E_POINTER;

    return WindowsCreateString(AppInstanceClass,
                               (UINT32)wcslen(AppInstanceClass),
                               name);
}

/* The activation factory */

static
HRESULT
STDMETHODCALLTYPE
FactoryQueryInterface(
    IActivationFactory *iface,
    REFIID riid,
    void **object)
{
    UNREFERENCED_PARAMETER(iface);

    return AppInstanceResolve(riid, object);
}

static
ULONG
STDMETHODCALLTYPE
FactoryAddRef(
    IActivationFactory *iface)
{
    UNREFERENCED_PARAMETER(iface);

    return 2;
}

static
ULONG
STDMETHODCALLTYPE
FactoryRelease(
    IActivationFactory *iface)
{
    UNREFERENCED_PARAMETER(iface);

    return 1;
}

static
HRESULT
STDMETHODCALLTYPE
FactoryGetIids(
    IActivationFactory *iface,
    ULONG *count,
    IID **iids)
{
    UNREFERENCED_PARAMETER(iface);

    if ((count == NULL) || (iids == NULL))
        return E_POINTER;

    *count = 0;
    *iids = NULL;

    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
FactoryGetRuntimeClassName(
    IActivationFactory *iface,
    HSTRING *name)
{
    UNREFERENCED_PARAMETER(iface);

    return AppInstanceName(name);
}

static
HRESULT
STDMETHODCALLTYPE
FactoryGetTrustLevel(
    IActivationFactory *iface,
    TrustLevel *level)
{
    UNREFERENCED_PARAMETER(iface);

    if (level == NULL)
        return E_POINTER;

    *level = BaseTrust;

    return S_OK;
}

/* The class carries statics only, so there is no instance of it to make */
static
HRESULT
STDMETHODCALLTYPE
FactoryActivateInstance(
    IActivationFactory *iface,
    IInspectable **instance)
{
    UNREFERENCED_PARAMETER(iface);

    if (instance == NULL)
        return E_POINTER;

    *instance = NULL;

    return E_NOTIMPL;
}

static IActivationFactoryVtbl FactoryVtbl =
{
    FactoryQueryInterface,
    FactoryAddRef,
    FactoryRelease,
    FactoryGetIids,
    FactoryGetRuntimeClassName,
    FactoryGetTrustLevel,
    FactoryActivateInstance
};

/* The statics */

static
HRESULT
STDMETHODCALLTYPE
StaticsQueryInterface(
    IAppInstanceStatics *iface,
    REFIID riid,
    void **object)
{
    UNREFERENCED_PARAMETER(iface);

    return AppInstanceResolve(riid, object);
}

static
ULONG
STDMETHODCALLTYPE
StaticsAddRef(
    IAppInstanceStatics *iface)
{
    UNREFERENCED_PARAMETER(iface);

    return 2;
}

static
ULONG
STDMETHODCALLTYPE
StaticsRelease(
    IAppInstanceStatics *iface)
{
    UNREFERENCED_PARAMETER(iface);

    return 1;
}

static
HRESULT
STDMETHODCALLTYPE
StaticsGetIids(
    IAppInstanceStatics *iface,
    ULONG *count,
    IID **iids)
{
    UNREFERENCED_PARAMETER(iface);

    if ((count == NULL) || (iids == NULL))
        return E_POINTER;

    *count = 0;
    *iids = NULL;

    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
StaticsGetRuntimeClassName(
    IAppInstanceStatics *iface,
    HSTRING *name)
{
    UNREFERENCED_PARAMETER(iface);

    return AppInstanceName(name);
}

static
HRESULT
STDMETHODCALLTYPE
StaticsGetTrustLevel(
    IAppInstanceStatics *iface,
    TrustLevel *level)
{
    UNREFERENCED_PARAMETER(iface);

    if (level == NULL)
        return E_POINTER;

    *level = BaseTrust;

    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
StaticsGetRecommendedInstance(
    IAppInstanceStatics *iface,
    IInspectable **value)
{
    UNREFERENCED_PARAMETER(iface);

    if (value == NULL)
        return E_POINTER;

    /* Nothing is recommended, because nothing else of this is running */
    *value = NULL;

    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
StaticsGetActivatedEventArgs(
    IAppInstanceStatics *iface,
    IInspectable **result)
{
    UNREFERENCED_PARAMETER(iface);

    if (result == NULL)
        return E_POINTER;

    /* Started from a command line, so there was no activation to report */
    *result = NULL;

    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
StaticsFindOrRegisterInstanceForKey(
    IAppInstanceStatics *iface,
    HSTRING key,
    IInspectable **result)
{
    UNREFERENCED_PARAMETER(iface);
    UNREFERENCED_PARAMETER(key);

    if (result == NULL)
        return E_POINTER;

    *result = NULL;

    return E_NOTIMPL;
}

static
HRESULT
STDMETHODCALLTYPE
StaticsUnregister(
    IAppInstanceStatics *iface)
{
    UNREFERENCED_PARAMETER(iface);

    /* Nothing was ever registered, so this is already how it asks for things */
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
StaticsGetInstances(
    IAppInstanceStatics *iface,
    IInspectable **result)
{
    UNREFERENCED_PARAMETER(iface);

    if (result == NULL)
        return E_POINTER;

    *result = NULL;

    return E_NOTIMPL;
}

static IAppInstanceStaticsVtbl StaticsVtbl =
{
    StaticsQueryInterface,
    StaticsAddRef,
    StaticsRelease,
    StaticsGetIids,
    StaticsGetRuntimeClassName,
    StaticsGetTrustLevel,
    StaticsGetRecommendedInstance,
    StaticsGetActivatedEventArgs,
    StaticsFindOrRegisterInstanceForKey,
    StaticsUnregister,
    StaticsGetInstances
};

static APP_INSTANCE_FACTORY AppInstanceFactory =
{
    { &FactoryVtbl },
    { &StaticsVtbl }
};

/**
 * @brief
 * Hands out the factory of a runtime class this module carries.
 *
 * @param[in] classid
 * The name of the class being asked for.
 *
 * @param[out] factory
 * Receives the factory.
 *
 * @return
 * S_OK, or REGDB_E_CLASSNOTREG for a class that is not ours.
 */
HRESULT
WINAPI
DllGetActivationFactory(
    _In_ HSTRING classid,
    _Out_ IActivationFactory **factory)
{
    const WCHAR *Name;

    if (factory == NULL)
        return E_POINTER;

    *factory = NULL;

    Name = WindowsGetStringRawBuffer(classid, NULL);
    if (Name == NULL)
        return E_INVALIDARG;

    TRACE("%s\n", wine_dbgstr_w(Name));

    if (wcscmp(Name, AppInstanceClass) != 0)
        return REGDB_E_CLASSNOTREG;

    *factory = &AppInstanceFactory.IActivationFactory_iface;

    return S_OK;
}

/**
 * @brief
 * Whether this module is holding nothing and can go.
 *
 * @return
 * S_FALSE. What it hands out is part of the module rather than allocated, so
 * unloading would take the object with it.
 */
HRESULT
WINAPI
DllCanUnloadNow(VOID)
{
    return S_FALSE;
}

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE Instance,
    _In_ ULONG Reason,
    _In_opt_ PVOID Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);

    if (Reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(Instance);

    return TRUE;
}

/* EOF */
