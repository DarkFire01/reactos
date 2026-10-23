/*
 * PROJECT:     ReactOS COM Base
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Extension catalog entries of package contracts
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <stdio.h>
#include <stdlib.h>

#define COBJMACROS
#include "objbase.h"
#include "winstring.h"

#include "roregistration.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(combase);

#define RT_EXTENSION_ROOT L"Software\\Classes\\Extensions\\ContractId"
#define RT_REGISTRY_READ (KEY_READ | KEY_WOW64_64KEY)

typedef LONG (WINAPI *PFN_PACKAGE_FAMILY_NAME_FROM_FULL_NAME)(PCWSTR, UINT32 *, PWSTR);

/* User registrations first, then the ones made for every user */
static const struct
{
    HKEY Root;
    RegistrationScope Scope;
} ExtensionRoots[] =
{
    { HKEY_CURRENT_USER, RegistrationScope_PerUser },
    { HKEY_LOCAL_MACHINE, RegistrationScope_InboxApp },
};

typedef struct _RT_EXTENSION_ENTRY
{
    IExtensionEntry IExtensionEntry_iface;
    IExtensionLaunchContext IExtensionLaunchContext_iface;
    IRtWeakReferenceSource IRtWeakReferenceSource_iface;
    IRtMapView IRtMapView_iface;
    LONG RefCount;
    PRT_WEAK_REFERENCE Weak;
    IUnknown *Marshaler;
    CRITICAL_SECTION Lock;
    HSTRING ContractId;
    HSTRING PackageId;
    HSTRING ClassId;
    HSTRING Vendor;
    HSTRING Icon;
    HSTRING DisplayName;
    HSTRING Description;
    RegistrationScope Scope;
    IRtMapView *Properties;
    IActivatableClassRegistration *Registration;
    UINT32 OutOfProcFlags;
    RT_LAUNCH_CONTEXT Launch;
    GUID ComponentProcessId;
    UINT64 RacTokenId;
    UINT64 ConsoleHandlesId;
    BLOB *LpacAttributes;
} RT_EXTENSION_ENTRY, *PRT_EXTENSION_ENTRY;

static inline PRT_EXTENSION_ENTRY impl_from_IExtensionEntry(IExtensionEntry *iface)
{
    return CONTAINING_RECORD(iface, RT_EXTENSION_ENTRY, IExtensionEntry_iface);
}

static inline PRT_EXTENSION_ENTRY impl_from_IExtensionLaunchContext(IExtensionLaunchContext *iface)
{
    return CONTAINING_RECORD(iface, RT_EXTENSION_ENTRY, IExtensionLaunchContext_iface);
}

static inline PRT_EXTENSION_ENTRY impl_entry_from_IRtWeakReferenceSource(IRtWeakReferenceSource *iface)
{
    return CONTAINING_RECORD(iface, RT_EXTENSION_ENTRY, IRtWeakReferenceSource_iface);
}

static inline PRT_EXTENSION_ENTRY impl_entry_from_IRtMapView(IRtMapView *iface)
{
    return CONTAINING_RECORD(iface, RT_EXTENSION_ENTRY, IRtMapView_iface);
}

static
HRESULT
RtCheckArgument(
    _In_opt_ HSTRING Text)
{
    BOOL HasNull = FALSE;

    if (WindowsIsStringEmpty(Text))
        return E_INVALIDARG;
    if (FAILED(WindowsStringHasEmbeddedNull(Text, &HasNull)) || HasNull)
        return E_INVALIDARG;
    return S_OK;
}

static
HRESULT
RtFamilyNameFromFullName(
    _In_ PCWSTR PackageFullName,
    _Out_writes_(RT_PACKAGE_FULL_NAME_LENGTH) PWSTR FamilyName)
{
    PFN_PACKAGE_FAMILY_NAME_FROM_FULL_NAME FromFullName;
    UINT32 Length = RT_PACKAGE_FULL_NAME_LENGTH;
    LONG Error;

    FamilyName[0] = UNICODE_NULL;
    FromFullName = (PFN_PACKAGE_FAMILY_NAME_FROM_FULL_NAME)RtGetAppModelExport("PackageFamilyNameFromFullName");
    if (!FromFullName)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    Error = FromFullName(PackageFullName, &Length, FamilyName);
    return HRESULT_FROM_WIN32(Error);
}

/* IExtensionEntry */

static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_QueryInterface(
    _In_ IExtensionEntry *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    PRT_EXTENSION_ENTRY Entry = impl_from_IExtensionEntry(iface);

    if (IsEqualIID(Riid, &IID_IUnknown) ||
        IsEqualIID(Riid, &IID_IInspectable) ||
        IsEqualIID(Riid, &IID_IExtensionEntry) ||
        IsEqualIID(Riid, &IID_IAgileObject))
    {
        *Object = &Entry->IExtensionEntry_iface;
    }
    else if (IsEqualIID(Riid, &IID_IExtensionLaunchContext))
    {
        *Object = &Entry->IExtensionLaunchContext_iface;
    }
    else if (IsEqualIID(Riid, &IID_IRtWeakReferenceSource))
    {
        *Object = &Entry->IRtWeakReferenceSource_iface;
    }
    else if (IsEqualIID(Riid, &IID_IRtMapView))
    {
        *Object = &Entry->IRtMapView_iface;
    }
    else if (IsEqualIID(Riid, &IID_IMarshal))
    {
        return IUnknown_QueryInterface(Entry->Marshaler, Riid, Object);
    }
    else
    {
        *Object = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*Object);
    return S_OK;
}

static
ULONG
STDMETHODCALLTYPE
ExtensionEntry_AddRef(
    _In_ IExtensionEntry *iface)
{
    return InterlockedIncrement(&impl_from_IExtensionEntry(iface)->RefCount);
}

static
ULONG
STDMETHODCALLTYPE
ExtensionEntry_Release(
    _In_ IExtensionEntry *iface)
{
    PRT_EXTENSION_ENTRY Entry = impl_from_IExtensionEntry(iface);
    ULONG Count = InterlockedDecrement(&Entry->RefCount);

    if (!Count)
    {
        RtDetachWeakReference(&Entry->Weak);
        if (Entry->Marshaler)
            IUnknown_Release(Entry->Marshaler);
        WindowsDeleteString(Entry->ContractId);
        WindowsDeleteString(Entry->PackageId);
        WindowsDeleteString(Entry->ClassId);
        WindowsDeleteString(Entry->Vendor);
        WindowsDeleteString(Entry->Icon);
        WindowsDeleteString(Entry->DisplayName);
        WindowsDeleteString(Entry->Description);
        if (Entry->Properties)
            IRtMapView_Release(Entry->Properties);
        if (Entry->Registration)
            IActivatableClassRegistration_Release(Entry->Registration);
        free(Entry->LpacAttributes);
        DeleteCriticalSection(&Entry->Lock);
        free(Entry);
    }
    return Count;
}

static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_GetIids(
    _In_ IExtensionEntry *iface,
    _Out_ ULONG *Count,
    _Outptr_ IID **Iids)
{
    static const IID *const Implemented[] =
    {
        &IID_IExtensionEntry,
        &IID_IRtWeakReferenceSource,
        &IID_IRtMapView,
    };

    return RtCopyIids(ARRAYSIZE(Implemented), Implemented, Count, Iids);
}

static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_GetRuntimeClassName(
    _In_ IExtensionEntry *iface,
    _Out_ HSTRING *Name)
{
    *Name = NULL;
    return E_NOTIMPL;
}

static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_GetTrustLevel(
    _In_ IExtensionEntry *iface,
    _Out_ TrustLevel *Level)
{
    *Level = BaseTrust;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_get_ActivatableClassId(
    _In_ IExtensionEntry *iface,
    _Out_ HSTRING *Value)
{
    return WindowsDuplicateString(impl_from_IExtensionEntry(iface)->ClassId, Value);
}

static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_get_ContractId(
    _In_ IExtensionEntry *iface,
    _Out_ HSTRING *Value)
{
    return WindowsDuplicateString(impl_from_IExtensionEntry(iface)->ContractId, Value);
}

static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_get_PackageId(
    _In_ IExtensionEntry *iface,
    _Out_ HSTRING *Value)
{
    return WindowsDuplicateString(impl_from_IExtensionEntry(iface)->PackageId, Value);
}

/* "<package family name>:<activatable class id>" */
static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_get_ExtensionId(
    _In_ IExtensionEntry *iface,
    _Out_ HSTRING *Value)
{
    PRT_EXTENSION_ENTRY Entry = impl_from_IExtensionEntry(iface);
    WCHAR Family[RT_PACKAGE_FULL_NAME_LENGTH];
    WCHAR Text[520];
    HRESULT Status;
    int Length;

    *Value = NULL;
    Status = RtFamilyNameFromFullName(WindowsGetStringRawBuffer(Entry->PackageId, NULL), Family);
    if (FAILED(Status))
        return Status;

    Length = _snwprintf(Text, ARRAYSIZE(Text), L"%s:%s", Family, WindowsGetStringRawBuffer(Entry->ClassId, NULL));
    if (Length < 0)
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    return WindowsCreateString(Text, Length, Value);
}

static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_get_ActivatableClassRegistration(
    _In_ IExtensionEntry *iface,
    _Outptr_ IActivatableClassRegistration **Registration)
{
    PRT_EXTENSION_ENTRY Entry = impl_from_IExtensionEntry(iface);
    RT_CLASS_INFO Info;
    HRESULT Status = S_OK;

    *Registration = NULL;
    EnterCriticalSection(&Entry->Lock);
    if (!Entry->Registration)
    {
        Status = RtFindClass(Entry->ClassId, WindowsGetStringRawBuffer(Entry->PackageId, NULL), &Info);
        if (SUCCEEDED(Status))
        {
            Status = RtCreateClassRegistration(&Info, &Entry->Registration);
            RtFreeClassInfo(&Info);
        }
    }
    if (SUCCEEDED(Status))
    {
        *Registration = Entry->Registration;
        IActivatableClassRegistration_AddRef(*Registration);
    }
    LeaveCriticalSection(&Entry->Lock);
    return Status;
}

static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_get_Vendor(
    _In_ IExtensionEntry *iface,
    _Out_ HSTRING *Value)
{
    return WindowsDuplicateString(impl_from_IExtensionEntry(iface)->Vendor, Value);
}

static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_get_Icon(
    _In_ IExtensionEntry *iface,
    _Out_ HSTRING *Value)
{
    return WindowsDuplicateString(impl_from_IExtensionEntry(iface)->Icon, Value);
}

static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_get_DisplayName(
    _In_ IExtensionEntry *iface,
    _Out_ HSTRING *Value)
{
    return WindowsDuplicateString(impl_from_IExtensionEntry(iface)->DisplayName, Value);
}

static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_get_Description(
    _In_ IExtensionEntry *iface,
    _Out_ HSTRING *Value)
{
    return WindowsDuplicateString(impl_from_IExtensionEntry(iface)->Description, Value);
}

static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_get_RegistrationScope(
    _In_ IExtensionEntry *iface,
    _Out_ RegistrationScope *Scope)
{
    *Scope = impl_from_IExtensionEntry(iface)->Scope;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_get_Attributes(
    _In_ IExtensionEntry *iface,
    _Outptr_ IRtMapView **Attributes)
{
    *Attributes = impl_from_IExtensionEntry(iface)->Properties;
    IRtMapView_AddRef(*Attributes);
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_get_OutOfProcActivationFlags(
    _In_ IExtensionEntry *iface,
    _Out_ UINT32 *Flags)
{
    *Flags = impl_from_IExtensionEntry(iface)->OutOfProcFlags;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_put_OutOfProcActivationFlags(
    _In_ IExtensionEntry *iface,
    _In_ UINT32 Flags)
{
    impl_from_IExtensionEntry(iface)->OutOfProcFlags = Flags;
    return S_OK;
}

/**
 * @brief
 * Activates the extension's class from the package that registered it, carrying
 * the launch context set on this entry to the server launcher.
 */
static
HRESULT
STDMETHODCALLTYPE
ExtensionEntry_Activate(
    _In_ IExtensionEntry *iface,
    _Outptr_ IInspectable **Instance)
{
    PRT_EXTENSION_ENTRY Entry = impl_from_IExtensionEntry(iface);
    IActivationFactory *Factory;
    RT_LAUNCH_CONTEXT Launch;
    RT_CLASS_INFO Info;
    HRESULT Status;

    TRACE("(%p) %s from %s\n", Entry, debugstr_hstring(Entry->ClassId), debugstr_hstring(Entry->PackageId));

    *Instance = NULL;
    Status = RtFindClass(Entry->ClassId, WindowsGetStringRawBuffer(Entry->PackageId, NULL), &Info);
    if (FAILED(Status))
        return Status;

    EnterCriticalSection(&Entry->Lock);
    Launch = Entry->Launch;
    LeaveCriticalSection(&Entry->Lock);

    Status = RtGetClassFactory(&Info, &Launch, &IID_IActivationFactory, (void **)&Factory);
    RtFreeClassInfo(&Info);
    if (FAILED(Status))
        return Status;

    Status = IActivationFactory_ActivateInstance(Factory, Instance);
    IActivationFactory_Release(Factory);
    return Status;
}

static const IExtensionEntryVtbl ExtensionEntryVtbl =
{
    ExtensionEntry_QueryInterface,
    ExtensionEntry_AddRef,
    ExtensionEntry_Release,
    ExtensionEntry_GetIids,
    ExtensionEntry_GetRuntimeClassName,
    ExtensionEntry_GetTrustLevel,
    ExtensionEntry_get_ActivatableClassId,
    ExtensionEntry_get_ContractId,
    ExtensionEntry_get_PackageId,
    ExtensionEntry_get_ExtensionId,
    ExtensionEntry_get_ActivatableClassRegistration,
    ExtensionEntry_get_Vendor,
    ExtensionEntry_get_Icon,
    ExtensionEntry_get_DisplayName,
    ExtensionEntry_get_Description,
    ExtensionEntry_get_RegistrationScope,
    ExtensionEntry_get_Attributes,
    ExtensionEntry_get_OutOfProcActivationFlags,
    ExtensionEntry_put_OutOfProcActivationFlags,
    ExtensionEntry_Activate,
};

/* IExtensionLaunchContext */

static
HRESULT
STDMETHODCALLTYPE
LaunchContext_QueryInterface(
    _In_ IExtensionLaunchContext *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    return ExtensionEntry_QueryInterface(&impl_from_IExtensionLaunchContext(iface)->IExtensionEntry_iface, Riid, Object);
}

static
ULONG
STDMETHODCALLTYPE
LaunchContext_AddRef(
    _In_ IExtensionLaunchContext *iface)
{
    return ExtensionEntry_AddRef(&impl_from_IExtensionLaunchContext(iface)->IExtensionEntry_iface);
}

static
ULONG
STDMETHODCALLTYPE
LaunchContext_Release(
    _In_ IExtensionLaunchContext *iface)
{
    return ExtensionEntry_Release(&impl_from_IExtensionLaunchContext(iface)->IExtensionEntry_iface);
}

static
HRESULT
STDMETHODCALLTYPE
LaunchContext_get_HostId(
    _In_ IExtensionLaunchContext *iface,
    _Out_ UINT64 *HostId)
{
    *HostId = impl_from_IExtensionLaunchContext(iface)->Launch.HostId;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
LaunchContext_put_HostId(
    _In_ IExtensionLaunchContext *iface,
    _In_ UINT64 HostId)
{
    PRT_EXTENSION_ENTRY Entry = impl_from_IExtensionLaunchContext(iface);

    EnterCriticalSection(&Entry->Lock);
    Entry->Launch.HostId = HostId;
    LeaveCriticalSection(&Entry->Lock);
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
LaunchContext_get_UserContext(
    _In_ IExtensionLaunchContext *iface,
    _Out_ UINT64 *UserContext)
{
    *UserContext = impl_from_IExtensionLaunchContext(iface)->Launch.UserContext;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
LaunchContext_put_UserContext(
    _In_ IExtensionLaunchContext *iface,
    _In_ UINT64 UserContext)
{
    PRT_EXTENSION_ENTRY Entry = impl_from_IExtensionLaunchContext(iface);

    EnterCriticalSection(&Entry->Lock);
    Entry->Launch.UserContext = UserContext;
    LeaveCriticalSection(&Entry->Lock);
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
LaunchContext_get_ComponentProcessId(
    _In_ IExtensionLaunchContext *iface,
    _Out_ GUID *ProcessId)
{
    *ProcessId = impl_from_IExtensionLaunchContext(iface)->ComponentProcessId;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
LaunchContext_put_ComponentProcessId(
    _In_ IExtensionLaunchContext *iface,
    _In_ GUID *ProcessId)
{
    impl_from_IExtensionLaunchContext(iface)->ComponentProcessId = *ProcessId;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
LaunchContext_get_RacActivationTokenId(
    _In_ IExtensionLaunchContext *iface,
    _Out_ UINT64 *TokenId)
{
    *TokenId = impl_from_IExtensionLaunchContext(iface)->RacTokenId;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
LaunchContext_put_RacActivationTokenId(
    _In_ IExtensionLaunchContext *iface,
    _In_ UINT64 TokenId)
{
    impl_from_IExtensionLaunchContext(iface)->RacTokenId = TokenId;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
LaunchContext_get_LpacAttributes(
    _In_ IExtensionLaunchContext *iface,
    _Outptr_result_maybenull_ BLOB **Attributes)
{
    PRT_EXTENSION_ENTRY Entry = impl_from_IExtensionLaunchContext(iface);

    *Attributes = Entry->LpacAttributes;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
LaunchContext_put_LpacAttributes(
    _In_ IExtensionLaunchContext *iface,
    _In_opt_ BLOB *Attributes)
{
    PRT_EXTENSION_ENTRY Entry = impl_from_IExtensionLaunchContext(iface);
    BLOB *Copy = NULL;

    if (Attributes)
    {
        Copy = malloc(sizeof(*Copy) + Attributes->cbSize);
        if (!Copy)
            return E_OUTOFMEMORY;
        Copy->cbSize = Attributes->cbSize;
        Copy->pBlobData = (BYTE *)(Copy + 1);
        if (Attributes->cbSize)
            memcpy(Copy->pBlobData, Attributes->pBlobData, Attributes->cbSize);
    }

    EnterCriticalSection(&Entry->Lock);
    free(Entry->LpacAttributes);
    Entry->LpacAttributes = Copy;
    LeaveCriticalSection(&Entry->Lock);
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
LaunchContext_get_ConsoleHandlesId(
    _In_ IExtensionLaunchContext *iface,
    _Out_ UINT64 *HandlesId)
{
    *HandlesId = impl_from_IExtensionLaunchContext(iface)->ConsoleHandlesId;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
LaunchContext_put_ConsoleHandlesId(
    _In_ IExtensionLaunchContext *iface,
    _In_ UINT64 HandlesId)
{
    impl_from_IExtensionLaunchContext(iface)->ConsoleHandlesId = HandlesId;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
LaunchContext_get_ActivationId(
    _In_ IExtensionLaunchContext *iface,
    _Out_ UINT64 *ActivationId)
{
    *ActivationId = impl_from_IExtensionLaunchContext(iface)->Launch.ActivationId;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
LaunchContext_put_ActivationId(
    _In_ IExtensionLaunchContext *iface,
    _In_ UINT64 ActivationId)
{
    PRT_EXTENSION_ENTRY Entry = impl_from_IExtensionLaunchContext(iface);

    EnterCriticalSection(&Entry->Lock);
    Entry->Launch.ActivationId = ActivationId;
    LeaveCriticalSection(&Entry->Lock);
    return S_OK;
}

static const IExtensionLaunchContextVtbl LaunchContextVtbl =
{
    LaunchContext_QueryInterface,
    LaunchContext_AddRef,
    LaunchContext_Release,
    LaunchContext_get_HostId,
    LaunchContext_put_HostId,
    LaunchContext_get_UserContext,
    LaunchContext_put_UserContext,
    LaunchContext_get_ComponentProcessId,
    LaunchContext_put_ComponentProcessId,
    LaunchContext_get_RacActivationTokenId,
    LaunchContext_put_RacActivationTokenId,
    LaunchContext_get_LpacAttributes,
    LaunchContext_put_LpacAttributes,
    LaunchContext_get_ConsoleHandlesId,
    LaunchContext_put_ConsoleHandlesId,
    LaunchContext_get_ActivationId,
    LaunchContext_put_ActivationId,
};

/* IRtWeakReferenceSource */

static
HRESULT
STDMETHODCALLTYPE
EntryWeakSource_QueryInterface(
    _In_ IRtWeakReferenceSource *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    return ExtensionEntry_QueryInterface(&impl_entry_from_IRtWeakReferenceSource(iface)->IExtensionEntry_iface, Riid, Object);
}

static
ULONG
STDMETHODCALLTYPE
EntryWeakSource_AddRef(
    _In_ IRtWeakReferenceSource *iface)
{
    return ExtensionEntry_AddRef(&impl_entry_from_IRtWeakReferenceSource(iface)->IExtensionEntry_iface);
}

static
ULONG
STDMETHODCALLTYPE
EntryWeakSource_Release(
    _In_ IRtWeakReferenceSource *iface)
{
    return ExtensionEntry_Release(&impl_entry_from_IRtWeakReferenceSource(iface)->IExtensionEntry_iface);
}

static
HRESULT
STDMETHODCALLTYPE
EntryWeakSource_GetWeakReference(
    _In_ IRtWeakReferenceSource *iface,
    _Outptr_ IRtWeakReference **Reference)
{
    PRT_EXTENSION_ENTRY Entry = impl_entry_from_IRtWeakReferenceSource(iface);

    return RtGetWeakReference(&Entry->Weak, (IInspectable *)&Entry->IExtensionEntry_iface, &Entry->RefCount, Reference);
}

static const IRtWeakReferenceSourceVtbl EntryWeakSourceVtbl =
{
    EntryWeakSource_QueryInterface,
    EntryWeakSource_AddRef,
    EntryWeakSource_Release,
    EntryWeakSource_GetWeakReference,
};

/* IRtMapView over the custom properties of the entry */

static
HRESULT
STDMETHODCALLTYPE
EntryMap_QueryInterface(
    _In_ IRtMapView *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    return ExtensionEntry_QueryInterface(&impl_entry_from_IRtMapView(iface)->IExtensionEntry_iface, Riid, Object);
}

static
ULONG
STDMETHODCALLTYPE
EntryMap_AddRef(
    _In_ IRtMapView *iface)
{
    return ExtensionEntry_AddRef(&impl_entry_from_IRtMapView(iface)->IExtensionEntry_iface);
}

static
ULONG
STDMETHODCALLTYPE
EntryMap_Release(
    _In_ IRtMapView *iface)
{
    return ExtensionEntry_Release(&impl_entry_from_IRtMapView(iface)->IExtensionEntry_iface);
}

static
HRESULT
STDMETHODCALLTYPE
EntryMap_GetIids(
    _In_ IRtMapView *iface,
    _Out_ ULONG *Count,
    _Outptr_ IID **Iids)
{
    return ExtensionEntry_GetIids(&impl_entry_from_IRtMapView(iface)->IExtensionEntry_iface, Count, Iids);
}

static
HRESULT
STDMETHODCALLTYPE
EntryMap_GetRuntimeClassName(
    _In_ IRtMapView *iface,
    _Out_ HSTRING *Name)
{
    *Name = NULL;
    return E_NOTIMPL;
}

static
HRESULT
STDMETHODCALLTYPE
EntryMap_GetTrustLevel(
    _In_ IRtMapView *iface,
    _Out_ TrustLevel *Level)
{
    *Level = BaseTrust;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
EntryMap_Lookup(
    _In_ IRtMapView *iface,
    _In_opt_ HSTRING Key,
    _Outptr_ IInspectable **Value)
{
    return IRtMapView_Lookup(impl_entry_from_IRtMapView(iface)->Properties, Key, Value);
}

static
HRESULT
STDMETHODCALLTYPE
EntryMap_get_Size(
    _In_ IRtMapView *iface,
    _Out_ UINT32 *Size)
{
    return IRtMapView_get_Size(impl_entry_from_IRtMapView(iface)->Properties, Size);
}

static
HRESULT
STDMETHODCALLTYPE
EntryMap_HasKey(
    _In_ IRtMapView *iface,
    _In_opt_ HSTRING Key,
    _Out_ boolean *Found)
{
    return IRtMapView_HasKey(impl_entry_from_IRtMapView(iface)->Properties, Key, Found);
}

static
HRESULT
STDMETHODCALLTYPE
EntryMap_Split(
    _In_ IRtMapView *iface,
    _Outptr_result_maybenull_ IRtMapView **First,
    _Outptr_result_maybenull_ IRtMapView **Second)
{
    return IRtMapView_Split(impl_entry_from_IRtMapView(iface)->Properties, First, Second);
}

static const IRtMapViewVtbl EntryMapVtbl =
{
    EntryMap_QueryInterface,
    EntryMap_AddRef,
    EntryMap_Release,
    EntryMap_GetIids,
    EntryMap_GetRuntimeClassName,
    EntryMap_GetTrustLevel,
    EntryMap_Lookup,
    EntryMap_get_Size,
    EntryMap_HasKey,
    EntryMap_Split,
};

/* CATALOG *******************************************************************/

static
HRESULT
RtReadEntryString(
    _In_ HKEY Key,
    _In_ PCWSTR Value,
    _Out_ HSTRING *String)
{
    PWSTR Text;
    HRESULT Status;

    *String = NULL;
    Status = RtDuplicateRegistryString(Key, Value, &Text);
    if (Status == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        return S_OK;
    if (FAILED(Status))
        return Status;

    Status = RtCreateHString(Text, String);
    free(Text);
    return Status;
}

/**
 * @brief
 * Opens Extensions\ContractId\<contract>\PackageId\<package>\ActivatableClassId\<class>,
 * user registrations first.
 */
static
HRESULT
RtOpenExtension(
    _In_ PCWSTR ContractId,
    _In_ PCWSTR PackageFullName,
    _In_ PCWSTR ClassId,
    _Out_ HKEY *Key,
    _Out_ RegistrationScope *Scope)
{
    WCHAR Path[MAX_PATH * 2];
    ULONG Index;

    *Key = NULL;
    if (_snwprintf(Path, ARRAYSIZE(Path), L"%s\\%s\\PackageId\\%s\\ActivatableClassId\\%s",
                   RT_EXTENSION_ROOT, ContractId, PackageFullName, ClassId) < 0)
    {
        return E_INVALIDARG;
    }
    Path[ARRAYSIZE(Path) - 1] = UNICODE_NULL;

    for (Index = 0; Index < ARRAYSIZE(ExtensionRoots); Index++)
    {
        if (RegOpenKeyExW(ExtensionRoots[Index].Root, Path, 0, RT_REGISTRY_READ, Key) == ERROR_SUCCESS)
        {
            *Scope = ExtensionRoots[Index].Scope;
            return S_OK;
        }
    }
    return REGDB_E_CLASSNOTREG;
}

static
HRESULT
RtCreateExtensionEntry(
    _In_ HSTRING ContractId,
    _In_ HSTRING PackageId,
    _In_ HSTRING ClassId,
    _In_ HKEY Key,
    _In_ RegistrationScope Scope,
    _Outptr_ IExtensionEntry **Result)
{
    PRT_EXTENSION_ENTRY Entry;
    HRESULT Status;

    *Result = NULL;
    Entry = calloc(1, sizeof(*Entry));
    if (!Entry)
        return E_OUTOFMEMORY;

    Entry->IExtensionEntry_iface.lpVtbl = &ExtensionEntryVtbl;
    Entry->IExtensionLaunchContext_iface.lpVtbl = &LaunchContextVtbl;
    Entry->IRtWeakReferenceSource_iface.lpVtbl = &EntryWeakSourceVtbl;
    Entry->IRtMapView_iface.lpVtbl = &EntryMapVtbl;
    Entry->RefCount = 1;
    Entry->Scope = Scope;
    InitializeCriticalSection(&Entry->Lock);

    Status = CoCreateFreeThreadedMarshaler((IUnknown *)&Entry->IExtensionEntry_iface, &Entry->Marshaler);
    if (SUCCEEDED(Status))
        Status = WindowsDuplicateString(ContractId, &Entry->ContractId);
    if (SUCCEEDED(Status))
        Status = WindowsDuplicateString(PackageId, &Entry->PackageId);
    if (SUCCEEDED(Status))
        Status = WindowsDuplicateString(ClassId, &Entry->ClassId);
    if (SUCCEEDED(Status))
        Status = RtReadEntryString(Key, L"Vendor", &Entry->Vendor);
    if (SUCCEEDED(Status))
        Status = RtReadEntryString(Key, L"Icon", &Entry->Icon);
    if (SUCCEEDED(Status))
        Status = RtReadEntryString(Key, L"DisplayName", &Entry->DisplayName);
    if (SUCCEEDED(Status))
        Status = RtReadEntryString(Key, L"Description", &Entry->Description);
    if (SUCCEEDED(Status))
        Status = RtCreateAttributeMap(Key, L"CustomProperties", &Entry->Properties);

    if (FAILED(Status))
    {
        ExtensionEntry_Release(&Entry->IExtensionEntry_iface);
        return Status;
    }

    *Result = &Entry->IExtensionEntry_iface;
    return S_OK;
}

/**
 * @brief
 * Finds the first of the given classes registered for a contract by a package.
 */
static
HRESULT
RtFindExtension(
    _In_ HSTRING ContractId,
    _In_ HSTRING PackageId,
    _In_ UINT32 ClassCount,
    _In_reads_(ClassCount) HSTRING *ClassIds,
    _Outptr_ IExtensionEntry **Result)
{
    WCHAR Family[RT_PACKAGE_FULL_NAME_LENGTH];
    RegistrationScope Scope;
    HRESULT Status;
    UINT32 Index;
    HKEY Key;

    *Result = NULL;
    Status = RtFamilyNameFromFullName(WindowsGetStringRawBuffer(PackageId, NULL), Family);
    if (FAILED(Status))
        return Status;

    for (Index = 0; Index < ClassCount; Index++)
    {
        if (FAILED(RtCheckArgument(ClassIds[Index])))
            return E_INVALIDARG;

        Status = RtOpenExtension(WindowsGetStringRawBuffer(ContractId, NULL),
                                 WindowsGetStringRawBuffer(PackageId, NULL),
                                 WindowsGetStringRawBuffer(ClassIds[Index], NULL),
                                 &Key,
                                 &Scope);
        if (FAILED(Status))
            continue;

        Status = RtCreateExtensionEntry(ContractId, PackageId, ClassIds[Index], Key, Scope, Result);
        RegCloseKey(Key);
        return Status;
    }
    return REGDB_E_CLASSNOTREG;
}

/***********************************************************************
 *      RoGetExtensionRegistration (combase.65)
 */
HRESULT
WINAPI
RoGetExtensionRegistration(
    _In_ HSTRING ContractId,
    _In_ HSTRING PackageId,
    _In_ HSTRING ClassId,
    _Outptr_ IExtensionEntry **Registration)
{
    TRACE("(%s, %s, %s, %p)\n", debugstr_hstring(ContractId), debugstr_hstring(PackageId),
          debugstr_hstring(ClassId), Registration);

    *Registration = NULL;
    if (FAILED(RtCheckArgument(ContractId)) ||
        FAILED(RtCheckArgument(PackageId)) ||
        FAILED(RtCheckArgument(ClassId)))
    {
        return E_INVALIDARG;
    }
    return RtFindExtension(ContractId, PackageId, 1, &ClassId, Registration);
}

/***********************************************************************
 *      RoGetExtensionRegistrationForLaunchContracts (combase.159)
 */
HRESULT
WINAPI
RoGetExtensionRegistrationForLaunchContracts(
    _In_ HSTRING ContractId,
    _In_ HSTRING PackageId,
    _In_ UINT32 ClassCount,
    _In_reads_(ClassCount) HSTRING *ClassIds,
    _Outptr_ IExtensionEntry **Registration)
{
    TRACE("(%s, %s, %u, %p, %p)\n", debugstr_hstring(ContractId), debugstr_hstring(PackageId),
          ClassCount, ClassIds, Registration);

    *Registration = NULL;
    if (FAILED(RtCheckArgument(ContractId)) || FAILED(RtCheckArgument(PackageId)) || !ClassCount || !ClassIds)
        return E_INVALIDARG;
    return RtFindExtension(ContractId, PackageId, ClassCount, ClassIds, Registration);
}

/***********************************************************************
 *      RoGetExtensionRegistrationByExtensionId (combase.79)
 *
 * The extension id is "<package family name>:<activatable class id>".
 */
HRESULT
WINAPI
RoGetExtensionRegistrationByExtensionId(
    _In_ HSTRING ContractId,
    _In_ HSTRING ExtensionId,
    _Outptr_ IExtensionEntry **Registration)
{
    WCHAR Path[MAX_PATH], Package[RT_PACKAGE_FULL_NAME_LENGTH], Family[RT_PACKAGE_FULL_NAME_LENGTH];
    HSTRING PackageId = NULL, ClassId = NULL;
    PCWSTR Text, Separator;
    HRESULT Status = REGDB_E_CLASSNOTREG;
    ULONG Root;
    DWORD Index, Length;
    HKEY Packages;

    TRACE("(%s, %s, %p)\n", debugstr_hstring(ContractId), debugstr_hstring(ExtensionId), Registration);

    *Registration = NULL;
    if (FAILED(RtCheckArgument(ContractId)) || FAILED(RtCheckArgument(ExtensionId)))
        return E_INVALIDARG;

    Text = WindowsGetStringRawBuffer(ExtensionId, NULL);
    Separator = wcschr(Text, L':');
    if (!Separator || Separator == Text || !Separator[1] || Separator - Text >= RT_PACKAGE_FULL_NAME_LENGTH)
        return E_INVALIDARG;

    if (FAILED(RtCreateHString(Separator + 1, &ClassId)))
        return E_OUTOFMEMORY;

    if (_snwprintf(Path, ARRAYSIZE(Path), L"%s\\%s\\PackageId", RT_EXTENSION_ROOT,
                   WindowsGetStringRawBuffer(ContractId, NULL)) < 0)
    {
        WindowsDeleteString(ClassId);
        return E_INVALIDARG;
    }
    Path[ARRAYSIZE(Path) - 1] = UNICODE_NULL;

    for (Root = 0; Root < ARRAYSIZE(ExtensionRoots) && Status == REGDB_E_CLASSNOTREG; Root++)
    {
        if (RegOpenKeyExW(ExtensionRoots[Root].Root, Path, 0, RT_REGISTRY_READ, &Packages) != ERROR_SUCCESS)
            continue;

        for (Index = 0; Status == REGDB_E_CLASSNOTREG; Index++)
        {
            Length = ARRAYSIZE(Package);
            if (RegEnumKeyExW(Packages, Index, Package, &Length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            if (FAILED(RtFamilyNameFromFullName(Package, Family)) ||
                wcslen(Family) != (SIZE_T)(Separator - Text) ||
                _wcsnicmp(Family, Text, Separator - Text))
            {
                continue;
            }

            if (FAILED(RtCreateHString(Package, &PackageId)))
            {
                Status = E_OUTOFMEMORY;
                break;
            }
            Status = RtFindExtension(ContractId, PackageId, 1, &ClassId, Registration);
            WindowsDeleteString(PackageId);
            PackageId = NULL;
            if (Status != REGDB_E_CLASSNOTREG)
                break;
        }
        RegCloseKey(Packages);
    }

    WindowsDeleteString(ClassId);
    return Status;
}
