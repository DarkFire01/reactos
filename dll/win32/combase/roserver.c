/*
 * PROJECT:     ReactOS COM Base
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Windows Runtime server registration and out of process activation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <stdlib.h>

#define COBJMACROS
#include "objbase.h"
#include "servprov.h"
#include "winstring.h"
#include "roerrorapi.h"
#include <pseh/pseh2.h>

#include "combase_private.h"
#include "roregistration.h"
#include "rtscm_c.h"

#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(combase);

/* rpcss serves the runtime activation interface on its Irpcss endpoint */
#define RT_SCM_PROTSEQ L"ncalrpc"
#define RT_SCM_ENDPOINT L"irpcss"

/* RoRegisterActivationFactories takes at most this many classes at once */
#define RT_MAX_SERVER_CLASSES 0x10000

/* Activations handled in this process for one RoRegisterActivationFactories call */
typedef struct _RT_SERVER_CLASS_ENTRY
{
    HSTRING ClassId;
    PFNGETACTIVATIONFACTORY Callback;
    GUID Key;
} RT_SERVER_CLASS_ENTRY, *PRT_SERVER_CLASS_ENTRY;

/*
 * The object rpcss hands to clients for a registration. Clients ask it for the
 * factory of a class by the key rpcss gave them.
 */
typedef struct _RT_SERVER_REGISTRATION
{
    IServiceProvider IServiceProvider_iface;
    LONG RefCount;
    struct list Entry;
    UINT32 Count;
    PRT_SERVER_CLASS_ENTRY Classes;
    DWORD RpcssCookie;
    IStream *MarshalStream;
    BOOL Revoked;
} RT_SERVER_REGISTRATION, *PRT_SERVER_REGISTRATION;

static struct list ServerRegistrations = LIST_INIT(ServerRegistrations);

static CRITICAL_SECTION ServerLock;
static CRITICAL_SECTION_DEBUG ServerLockDebug =
{
    0, 0, &ServerLock,
    { &ServerLockDebug.ProcessLocksList, &ServerLockDebug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": ServerLock") }
};
static CRITICAL_SECTION ServerLock = { &ServerLockDebug, -1, 0, 0, 0, 0 };

/* State of CoRegisterServerShutdownDelay, under ServerLock */
static HANDLE ShutdownTimer;
static HANDLE ShutdownEvent;
static DWORD ShutdownDelay;
static BOOL ShutdownStarted;

static RPC_BINDING_HANDLE ScmBinding;

/* RPC PLUMBING **************************************************************/

static
RPC_BINDING_HANDLE
RtGetScmBinding(VOID)
{
    RPC_BINDING_HANDLE Binding = NULL;
    RPC_WSTR StringBinding;

    if (ScmBinding)
        return ScmBinding;

    if (RpcStringBindingComposeW(NULL, (RPC_WSTR)RT_SCM_PROTSEQ, NULL, (RPC_WSTR)RT_SCM_ENDPOINT,
                                 NULL, &StringBinding) != RPC_S_OK)
    {
        return NULL;
    }
    RpcBindingFromStringBindingW(StringBinding, &Binding);
    RpcStringFreeW(&StringBinding);

    if (Binding && InterlockedCompareExchangePointer(&ScmBinding, Binding, NULL))
        RpcBindingFree(&Binding);
    return ScmBinding;
}


static
HRESULT
RtScmRegister(
    _In_ DWORD Count,
    _In_reads_(Count) RT_SERVER_CLASS *Classes,
    _In_ DWORD Length,
    _In_reads_bytes_(Length) BYTE *Provider,
    _Out_ DWORD *Cookie)
{
    RPC_BINDING_HANDLE Binding = RtGetScmBinding();
    HRESULT Status;

    if (!Binding)
        return HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE);

    RpcTryExcept
    {
        Status = RtScmRegisterServer(Binding, Count, Classes, Length, Provider, Cookie);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = HRESULT_FROM_WIN32(RpcExceptionCode());
    }
    RpcEndExcept;

    return Status;
}

static
HRESULT
RtScmRevoke(
    _In_ DWORD Cookie)
{
    RPC_BINDING_HANDLE Binding = RtGetScmBinding();
    HRESULT Status;

    if (!Binding)
        return HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE);

    RpcTryExcept
    {
        Status = RtScmRevokeServer(Binding, Cookie);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = HRESULT_FROM_WIN32(RpcExceptionCode());
    }
    RpcEndExcept;

    return Status;
}

static
HRESULT
RtScmActivate(
    _In_ RT_ACTIVATION_REQUEST *Request,
    _Out_ RT_ACTIVATION_RESULT *Result)
{
    RPC_BINDING_HANDLE Binding = RtGetScmBinding();
    HRESULT Status;

    RtlZeroMemory(Result, sizeof(*Result));
    if (!Binding)
        return HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE);

    RpcTryExcept
    {
        Status = RtScmGetActivationFactory(Binding, Request, Result);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = HRESULT_FROM_WIN32(RpcExceptionCode());
    }
    RpcEndExcept;

    return Status;
}

/* SERVER PROVIDER ***********************************************************/

static inline PRT_SERVER_REGISTRATION impl_from_IServiceProvider(IServiceProvider *iface)
{
    return CONTAINING_RECORD(iface, RT_SERVER_REGISTRATION, IServiceProvider_iface);
}

static
HRESULT
STDMETHODCALLTYPE
ServerProvider_QueryInterface(
    _In_ IServiceProvider *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    if (IsEqualIID(Riid, &IID_IUnknown) || IsEqualIID(Riid, &IID_IServiceProvider))
    {
        *Object = iface;
        IServiceProvider_AddRef(iface);
        return S_OK;
    }

    *Object = NULL;
    return E_NOINTERFACE;
}

static
ULONG
STDMETHODCALLTYPE
ServerProvider_AddRef(
    _In_ IServiceProvider *iface)
{
    return InterlockedIncrement(&impl_from_IServiceProvider(iface)->RefCount);
}

static
ULONG
STDMETHODCALLTYPE
ServerProvider_Release(
    _In_ IServiceProvider *iface)
{
    PRT_SERVER_REGISTRATION Registration = impl_from_IServiceProvider(iface);
    ULONG Count = InterlockedDecrement(&Registration->RefCount);
    UINT32 Index;

    if (!Count)
    {
        for (Index = 0; Index < Registration->Count; Index++)
            WindowsDeleteString(Registration->Classes[Index].ClassId);
        free(Registration->Classes);
        free(Registration);
    }
    return Count;
}

/**
 * @brief
 * Hands a client the factory of the class rpcss matched to Key, asking the
 * callback the server registered for it.
 */
static
HRESULT
STDMETHODCALLTYPE
ServerProvider_QueryService(
    _In_ IServiceProvider *iface,
    _In_ REFGUID Key,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    PRT_SERVER_REGISTRATION Registration = impl_from_IServiceProvider(iface);
    PFNGETACTIVATIONFACTORY Callback = NULL;
    IActivationFactory *Factory = NULL;
    HSTRING ClassId = NULL;
    HRESULT Status = S_OK;
    UINT32 Index;

    *Object = NULL;

    EnterCriticalSection(&ServerLock);
    if (Registration->Revoked || ShutdownStarted)
    {
        LeaveCriticalSection(&ServerLock);
        return CO_E_SERVER_STOPPING;
    }
    for (Index = 0; Index < Registration->Count; Index++)
    {
        if (IsEqualGUID(Key, &Registration->Classes[Index].Key))
        {
            Callback = Registration->Classes[Index].Callback;
            Status = WindowsDuplicateString(Registration->Classes[Index].ClassId, &ClassId);
            break;
        }
    }
    LeaveCriticalSection(&ServerLock);

    if (!Callback)
        return REGDB_E_CLASSNOTREG;
    if (FAILED(Status))
        return Status;

    TRACE("activation of %s\n", debugstr_hstring(ClassId));

    Status = Callback(ClassId, &Factory);
    WindowsDeleteString(ClassId);
    if (FAILED(Status))
        return Status;
    if (!Factory)
        return E_POINTER;

    Status = IActivationFactory_QueryInterface(Factory, Riid, Object);
    IActivationFactory_Release(Factory);
    return Status;
}

static const IServiceProviderVtbl ServerProviderVtbl =
{
    ServerProvider_QueryInterface,
    ServerProvider_AddRef,
    ServerProvider_Release,
    ServerProvider_QueryService,
};

/* Drops the table marshal that kept the provider reachable */
static
VOID
RtReleaseServerMarshal(
    _Inout_ PRT_SERVER_REGISTRATION Registration)
{
    LARGE_INTEGER Zero = { { 0 } };

    if (!Registration->MarshalStream)
        return;

    IStream_Seek(Registration->MarshalStream, Zero, STREAM_SEEK_SET, NULL);
    CoReleaseMarshalData(Registration->MarshalStream);
    IStream_Release(Registration->MarshalStream);
    Registration->MarshalStream = NULL;
}

static
BOOL
RtIsClassRegistered(
    _In_ HSTRING ClassId)
{
    PRT_SERVER_REGISTRATION Registration;
    UINT32 Index;
    INT32 Order;

    LIST_FOR_EACH_ENTRY(Registration, &ServerRegistrations, RT_SERVER_REGISTRATION, Entry)
    {
        for (Index = 0; Index < Registration->Count; Index++)
        {
            if (SUCCEEDED(WindowsCompareStringOrdinal(ClassId, Registration->Classes[Index].ClassId, &Order)) &&
                !Order)
            {
                return TRUE;
            }
        }
    }
    return FALSE;
}

/***********************************************************************
 *      RoRegisterActivationFactories (combase.@)
 */
HRESULT
WINAPI
RoRegisterActivationFactories(
    _In_reads_(Count) HSTRING *ClassIds,
    _In_reads_(Count) PFNGETACTIVATIONFACTORY *Callbacks,
    _In_ UINT32 Count,
    _Out_ RO_REGISTRATION_COOKIE *Cookie)
{
    PRT_SERVER_REGISTRATION Registration;
    RT_SERVER_CLASS *Wire = NULL;
    struct apartment *Apartment;
    RT_CLASS_INFO Info;
    HGLOBAL Memory;
    HRESULT Status;
    UINT32 Index;

    TRACE("(%p, %p, %u, %p)\n", ClassIds, Callbacks, Count, Cookie);

    *Cookie = NULL;
    if (!(Apartment = apartment_get_current_or_mta()))
        return CO_E_NOTINITIALIZED;
    apartment_release(Apartment);

    /* Every class must be an out of process class this process can see */
    for (Index = 0; Index < Count; Index++)
    {
        Status = RtFindClass(ClassIds[Index], NULL, &Info);
        if (FAILED(Status))
            return Status;
        if (Info.Type != ActivationType_OutOfProcess)
        {
            WARN("Attempted to register in process class %s\n", debugstr_hstring(ClassIds[Index]));
            RtFreeClassInfo(&Info);
            return REGDB_E_CLASSNOTREG;
        }
        RtFreeClassInfo(&Info);
    }

    if (!Count || Count > RT_MAX_SERVER_CLASSES)
        return E_INVALIDARG;

    Registration = calloc(1, sizeof(*Registration));
    if (!Registration)
        return E_OUTOFMEMORY;
    Registration->IServiceProvider_iface.lpVtbl = &ServerProviderVtbl;
    Registration->RefCount = 1;
    list_init(&Registration->Entry);

    Registration->Classes = calloc(Count, sizeof(*Registration->Classes));
    Wire = calloc(Count, sizeof(*Wire));
    if (!Registration->Classes || !Wire)
    {
        Status = E_OUTOFMEMORY;
        goto Fail;
    }

    EnterCriticalSection(&ServerLock);
    for (Index = 0; Index < Count; Index++)
    {
        if (RtIsClassRegistered(ClassIds[Index]))
        {
            LeaveCriticalSection(&ServerLock);
            Status = CO_E_OBJISREG;
            goto Fail;
        }
    }
    LeaveCriticalSection(&ServerLock);

    for (Index = 0; Index < Count; Index++)
    {
        Status = WindowsDuplicateString(ClassIds[Index], &Registration->Classes[Index].ClassId);
        if (SUCCEEDED(Status))
            Status = CoCreateGuid(&Registration->Classes[Index].Key);
        if (FAILED(Status))
            goto Fail;

        Registration->Classes[Index].Callback = Callbacks[Index];
        Registration->Count = Index + 1;
        Wire[Index].ClassId = (WCHAR *)WindowsGetStringRawBuffer(Registration->Classes[Index].ClassId, NULL);
        Wire[Index].Key = Registration->Classes[Index].Key;
    }

    /* The provider stays reachable for every client until it is revoked */
    Status = CreateStreamOnHGlobal(NULL, TRUE, &Registration->MarshalStream);
    if (SUCCEEDED(Status))
    {
        Status = CoMarshalInterface(Registration->MarshalStream, &IID_IServiceProvider,
                                    (IUnknown *)&Registration->IServiceProvider_iface,
                                    MSHCTX_LOCAL, NULL, MSHLFLAGS_TABLESTRONG);
        if (FAILED(Status))
        {
            IStream_Release(Registration->MarshalStream);
            Registration->MarshalStream = NULL;
        }
    }
    if (SUCCEEDED(Status))
        Status = GetHGlobalFromStream(Registration->MarshalStream, &Memory);
    if (SUCCEEDED(Status))
    {
        Status = RtScmRegister(Count, Wire, (DWORD)GlobalSize(Memory), GlobalLock(Memory), &Registration->RpcssCookie);
        GlobalUnlock(Memory);
    }
    if (FAILED(Status))
    {
        ERR("Registering %u classes with rpcss failed, %#lx\n", Count, Status);
        goto Fail;
    }

    EnterCriticalSection(&ServerLock);
    list_add_tail(&ServerRegistrations, &Registration->Entry);
    LeaveCriticalSection(&ServerLock);

    free(Wire);
    *Cookie = (RO_REGISTRATION_COOKIE)Registration;
    return S_OK;

Fail:
    free(Wire);
    RtReleaseServerMarshal(Registration);
    IServiceProvider_Release(&Registration->IServiceProvider_iface);
    return Status;
}

/***********************************************************************
 *      RoRevokeActivationFactories (combase.@)
 */
void
WINAPI
RoRevokeActivationFactories(
    _In_ RO_REGISTRATION_COOKIE Cookie)
{
    PRT_SERVER_REGISTRATION Registration, Found = NULL;

    TRACE("(%p)\n", Cookie);

    EnterCriticalSection(&ServerLock);
    LIST_FOR_EACH_ENTRY(Registration, &ServerRegistrations, RT_SERVER_REGISTRATION, Entry)
    {
        if (Registration == (PRT_SERVER_REGISTRATION)Cookie)
        {
            Found = Registration;
            list_remove(&Found->Entry);
            Found->Revoked = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&ServerLock);

    if (!Found)
        return;

    RtScmRevoke(Found->RpcssCookie);
    RtReleaseServerMarshal(Found);
    CoDisconnectObject((IUnknown *)&Found->IServiceProvider_iface, 0);
    IServiceProvider_Release(&Found->IServiceProvider_iface);
}

/* SERVER SHUTDOWN ***********************************************************/

/* Stops handing out factories; called under ServerLock */
static
VOID
RtSuspendServerClasses(VOID)
{
    TRACE("suspending the runtime classes of this server\n");
    ShutdownStarted = TRUE;
}

static
VOID
CALLBACK
RtShutdownTimerCallback(
    _In_ PVOID Context,
    _In_ BOOLEAN TimerFired)
{
    EnterCriticalSection(&ServerLock);
    if (ShutdownEvent && !ShutdownStarted)
    {
        RtSuspendServerClasses();
        SetEvent(ShutdownEvent);
    }
    LeaveCriticalSection(&ServerLock);
}

/* Cancels a pending delay; returns with ServerLock held */
static
VOID
RtCancelShutdownTimer(VOID)
{
    HANDLE Timer;

    EnterCriticalSection(&ServerLock);
    while ((Timer = ShutdownTimer))
    {
        ShutdownTimer = NULL;
        LeaveCriticalSection(&ServerLock);
        DeleteTimerQueueTimer(NULL, Timer, INVALID_HANDLE_VALUE);
        EnterCriticalSection(&ServerLock);
    }
}

VOID
RtServerProcessReferenced(VOID)
{
    RtCancelShutdownTimer();
    LeaveCriticalSection(&ServerLock);
}

/**
 * @brief
 * The last server reference went away: shut down after the registered delay, or
 * right away when there is none.
 */
VOID
RtServerProcessUnreferenced(VOID)
{
    RtCancelShutdownTimer();
    if (ShutdownEvent && ShutdownDelay)
    {
        if (!CreateTimerQueueTimer(&ShutdownTimer, NULL, RtShutdownTimerCallback, NULL,
                                   ShutdownDelay, 0, WT_EXECUTEONLYONCE))
        {
            ShutdownTimer = NULL;
        }
    }
    else
    {
        RtSuspendServerClasses();
    }
    LeaveCriticalSection(&ServerLock);
}

/***********************************************************************
 *      CoRegisterServerShutdownDelay (combase.69)
 */
HRESULT
WINAPI
CoRegisterServerShutdownDelay(
    _In_opt_ HANDLE StopEvent,
    _In_ DWORD Milliseconds)
{
    HRESULT Status = S_OK;

    TRACE("(%p, %lu)\n", StopEvent, Milliseconds);

    if (!StopEvent != !Milliseconds)
        return E_INVALIDARG;

    RtCancelShutdownTimer();
    if (!StopEvent)
    {
        ShutdownEvent = NULL;
        ShutdownDelay = 0;
    }
    else if (ShutdownStarted)
    {
        Status = CO_E_SERVER_STOPPING;
    }
    else
    {
        ShutdownEvent = StopEvent;
        ShutdownDelay = Milliseconds;
    }
    LeaveCriticalSection(&ServerLock);
    return Status;
}

/* ACTIVATION ****************************************************************/

/**
 * @brief
 * Loads the DLL of an in process class and gets its factory.
 */
HRESULT
RtGetInProcessFactory(
    _In_ PRT_CLASS_INFO Info,
    _In_ REFIID Riid,
    _Outptr_ void **Factory)
{
    PFNGETACTIVATIONFACTORY GetFactory;
    IActivationFactory *Created;
    HMODULE Module;
    HRESULT Status;

    *Factory = NULL;
    Module = LoadLibraryExW(Info->DllPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!Module)
    {
        ERR("Failed to load %s\n", debugstr_w(Info->DllPath));
        return HRESULT_FROM_WIN32(GetLastError());
    }

    GetFactory = (PFNGETACTIVATIONFACTORY)GetProcAddress(Module, "DllGetActivationFactory");
    if (!GetFactory)
    {
        ERR("%s does not export DllGetActivationFactory\n", debugstr_w(Info->DllPath));
        FreeLibrary(Module);
        return E_FAIL;
    }

    Status = GetFactory(Info->ClassId, &Created);
    if (SUCCEEDED(Status))
    {
        Status = IActivationFactory_QueryInterface(Created, Riid, Factory);
        IActivationFactory_Release(Created);
    }

    /* A DLL that handed out a factory stays loaded */
    if (FAILED(Status))
        FreeLibrary(Module);
    return Status;
}

/**
 * @brief
 * Gets the factory of an out of process class from its server, which rpcss
 * finds or starts.
 */
HRESULT
RtGetOutOfProcessFactory(
    _In_ PRT_CLASS_INFO Info,
    _In_opt_ const RT_LAUNCH_CONTEXT *Context,
    _In_ REFIID Riid,
    _Outptr_ void **Factory)
{
    RT_ACTIVATION_REQUEST Request;
    RT_ACTIVATION_RESULT Result;
    IServiceProvider *Provider;
    LARGE_INTEGER Zero = { { 0 } };
    IStream *Stream = NULL;
    HRESULT Status;

    TRACE("%s of %s\n", debugstr_hstring(Info->ClassId), debugstr_w(Info->PackageFullName));

    *Factory = NULL;
    RtlZeroMemory(&Request, sizeof(Request));
    Request.PackageFullName = Info->PackageFullName[0] ? Info->PackageFullName : NULL;
    Request.ClassId = (WCHAR *)WindowsGetStringRawBuffer(Info->ClassId, NULL);
    if (Context)
    {
        Request.HostId = Context->HostId;
        Request.UserContext = Context->UserContext;
        Request.ActivationId = Context->ActivationId;
    }

    Status = RtScmActivate(&Request, &Result);
    if (FAILED(Status))
    {
        WARN("rpcss could not provide %s, %#lx\n", debugstr_hstring(Info->ClassId), Status);
        return Status;
    }

    Status = CreateStreamOnHGlobal(NULL, TRUE, &Stream);
    if (SUCCEEDED(Status))
        Status = IStream_Write(Stream, Result.Provider, Result.ProviderLength, NULL);
    if (SUCCEEDED(Status))
        Status = IStream_Seek(Stream, Zero, STREAM_SEEK_SET, NULL);
    MIDL_user_free(Result.Provider);

    if (SUCCEEDED(Status))
        Status = CoUnmarshalInterface(Stream, &IID_IServiceProvider, (void **)&Provider);
    if (Stream)
        IStream_Release(Stream);
    if (FAILED(Status))
        return Status;

    Status = IServiceProvider_QueryService(Provider, &Result.Key, Riid, Factory);
    IServiceProvider_Release(Provider);
    return Status;
}

HRESULT
RtGetClassFactory(
    _In_ PRT_CLASS_INFO Info,
    _In_opt_ const RT_LAUNCH_CONTEXT *Context,
    _In_ REFIID Riid,
    _Outptr_ void **Factory)
{
    *Factory = NULL;
    if (Info->Type == ActivationType_InProcess)
        return RtGetInProcessFactory(Info, Riid, Factory);
    if (Info->Type == ActivationType_OutOfProcess)
        return RtGetOutOfProcessFactory(Info, Context, Riid, Factory);
    return REGDB_E_INVALIDVALUE;
}

/***********************************************************************
 *      CoAllowSetForegroundWindow (combase.140)
 */
HRESULT
WINAPI
CoAllowSetForegroundWindow(
    _In_ IUnknown *Object,
    _In_opt_ void *Reserved)
{
    IServerProcessIdentity *Identity;
    DWORD ProcessId = 0;
    HRESULT Status;

    TRACE("(%p, %p)\n", Object, Reserved);

    if (!Object || Reserved)
        return E_INVALIDARG;

    Status = IUnknown_QueryInterface(Object, &IID_IServerProcessIdentity, (void **)&Identity);
    if (FAILED(Status))
        return E_INVALIDARG;

    Status = IServerProcessIdentity_GetServerProcessId(Identity, &ProcessId);
    IServerProcessIdentity_Release(Identity);
    if (FAILED(Status))
        return Status;

    if (!AllowSetForegroundWindow(ProcessId))
        return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}
