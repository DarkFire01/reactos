/*
 * PROJECT:     ReactOS COM Base
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Windows Runtime class catalog and class registration objects
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <stdio.h>
#include <stdlib.h>

#define COBJMACROS
#include "objbase.h"
#include "sddl.h"
#include "winstring.h"
#include "roerrorapi.h"

#include "initguid.h"
#include "roregistration.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(combase);

#define RT_RUNTIME_ROOT L"Software\\Microsoft\\WindowsRuntime"
#define RT_PACKAGED_ROOT L"Software\\Classes\\ActivatableClasses\\Package"
#define RT_REGISTRY_READ (KEY_READ | KEY_WOW64_64KEY)

#ifndef APPMODEL_ERROR_NO_PACKAGE
#define APPMODEL_ERROR_NO_PACKAGE 15700L
#endif

typedef LONG (WINAPI *PFN_GET_CURRENT_PACKAGE_FULL_NAME)(UINT32 *, PWSTR);
typedef LONG (WINAPI *PFN_GET_PACKAGE_PATH_BY_FULL_NAME)(PCWSTR, UINT32 *, PWSTR);

/* Registry roots of packaged registrations, user first */
static const struct
{
    HKEY Root;
    RegistrationScope Scope;
} PackagedRoots[] =
{
    { HKEY_CURRENT_USER, RegistrationScope_PerUser },
    { HKEY_LOCAL_MACHINE, RegistrationScope_InboxApp },
};

static CRITICAL_SECTION WeakLock;
static CRITICAL_SECTION_DEBUG WeakLockDebug =
{
    0, 0, &WeakLock,
    { &WeakLockDebug.ProcessLocksList, &WeakLockDebug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": WeakLock") }
};
static CRITICAL_SECTION WeakLock = { &WeakLockDebug, -1, 0, 0, 0, 0 };

static IRtPropertyValueStatics *PropertyValueStatics;

/* HELPERS *******************************************************************/

HRESULT
RtCreateHString(
    _In_opt_ PCWSTR Text,
    _Out_ HSTRING *String)
{
    *String = NULL;
    if (!Text)
        return S_OK;
    return WindowsCreateString(Text, (UINT32)wcslen(Text), String);
}

HRESULT
RtValidateName(
    _In_opt_ HSTRING Name)
{
    BOOL HasNull = FALSE;
    UINT32 Length;

    if (!Name)
        return E_INVALIDARG;
    if (FAILED(WindowsStringHasEmbeddedNull(Name, &HasNull)) || HasNull)
        return E_INVALIDARG;

    Length = WindowsGetStringLen(Name);
    if (!Length || Length > RT_MAX_NAME_LENGTH)
        return E_INVALIDARG;
    return S_OK;
}

/**
 * @brief
 * Finds a package API: kernel32 forwards them to kernel.appcore, which is asked
 * directly when the forwarder is not visible to the caller.
 */
FARPROC
RtGetAppModelExport(
    _In_ PCSTR Name)
{
    static HMODULE AppCore;
    HMODULE Module = GetModuleHandleW(L"kernel32.dll");
    FARPROC Found = Module ? GetProcAddress(Module, Name) : NULL;

    if (Found)
        return Found;
    if (!AppCore)
    {
        Module = LoadLibraryW(L"kernel.appcore.dll");
        if (!Module)
            return NULL;
        if (InterlockedCompareExchangePointer((void **)&AppCore, Module, NULL))
            FreeLibrary(Module);
    }
    return GetProcAddress(AppCore, Name);
}

/**
 * @brief
 * Gets the package full name of the calling process.
 *
 * @return
 * S_OK for a packaged process, S_FALSE with an empty buffer otherwise.
 */
HRESULT
RtGetCurrentPackageFullName(
    _Out_writes_(RT_PACKAGE_FULL_NAME_LENGTH) PWSTR Buffer)
{
    PFN_GET_CURRENT_PACKAGE_FULL_NAME GetFullName;
    UINT32 Length = RT_PACKAGE_FULL_NAME_LENGTH;
    LONG Error;

    Buffer[0] = UNICODE_NULL;
    GetFullName = (PFN_GET_CURRENT_PACKAGE_FULL_NAME)RtGetAppModelExport("GetCurrentPackageFullName");
    if (!GetFullName)
        return S_FALSE;

    Error = GetFullName(&Length, Buffer);
    if (Error == ERROR_SUCCESS)
        return S_OK;

    Buffer[0] = UNICODE_NULL;
    if (Error == APPMODEL_ERROR_NO_PACKAGE)
        return S_FALSE;
    return HRESULT_FROM_WIN32(Error);
}

HRESULT
RtGetPackagePath(
    _In_ PCWSTR PackageFullName,
    _Outptr_ PWSTR *Path)
{
    PFN_GET_PACKAGE_PATH_BY_FULL_NAME GetPath;
    UINT32 Length = 0;
    LONG Error;

    *Path = NULL;
    GetPath = (PFN_GET_PACKAGE_PATH_BY_FULL_NAME)RtGetAppModelExport("GetPackagePathByFullName");
    if (!GetPath)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    Error = GetPath(PackageFullName, &Length, NULL);
    if (Error != ERROR_INSUFFICIENT_BUFFER)
        return HRESULT_FROM_WIN32(Error == ERROR_SUCCESS ? ERROR_NOT_FOUND : Error);

    *Path = malloc(Length * sizeof(WCHAR));
    if (!*Path)
        return E_OUTOFMEMORY;

    Error = GetPath(PackageFullName, &Length, *Path);
    if (Error != ERROR_SUCCESS)
    {
        free(*Path);
        *Path = NULL;
        return HRESULT_FROM_WIN32(Error);
    }
    return S_OK;
}

/**
 * @brief
 * Reads a REG_SZ or REG_EXPAND_SZ value, expanding the latter.
 *
 * @return
 * HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) when the value is missing,
 * REGDB_E_INVALIDVALUE when it has another type.
 */
HRESULT
RtDuplicateRegistryString(
    _In_ HKEY Key,
    _In_ PCWSTR Value,
    _Outptr_result_maybenull_ PWSTR *Text)
{
    DWORD Type, Size = 0;
    PWSTR Buffer, Expanded;
    DWORD Needed;
    LONG Error;

    *Text = NULL;
    Error = RegQueryValueExW(Key, Value, NULL, &Type, NULL, &Size);
    if (Error != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(Error);
    if (Type != REG_SZ && Type != REG_EXPAND_SZ)
        return REGDB_E_INVALIDVALUE;

    Buffer = calloc(1, Size + sizeof(WCHAR));
    if (!Buffer)
        return E_OUTOFMEMORY;

    Error = RegQueryValueExW(Key, Value, NULL, NULL, (BYTE *)Buffer, &Size);
    if (Error != ERROR_SUCCESS)
    {
        free(Buffer);
        return HRESULT_FROM_WIN32(Error);
    }

    if (Type == REG_EXPAND_SZ)
    {
        Needed = ExpandEnvironmentStringsW(Buffer, NULL, 0);
        Expanded = Needed ? malloc(Needed * sizeof(WCHAR)) : NULL;
        if (!Expanded)
        {
            free(Buffer);
            return E_OUTOFMEMORY;
        }
        ExpandEnvironmentStringsW(Buffer, Expanded, Needed);
        free(Buffer);
        Buffer = Expanded;
    }

    *Text = Buffer;
    return S_OK;
}

static
DWORD
RtReadDword(
    _In_ HKEY Key,
    _In_ PCWSTR Value,
    _In_ DWORD Default)
{
    DWORD Data, Size = sizeof(Data), Type;

    if (RegQueryValueExW(Key, Value, NULL, &Type, (BYTE *)&Data, &Size) != ERROR_SUCCESS ||
        Type != REG_DWORD || Size != sizeof(Data))
    {
        return Default;
    }
    return Data;
}

HRESULT
RtCopyIids(
    _In_ ULONG Count,
    _In_reads_(Count) const IID *const *Source,
    _Out_ ULONG *IidCount,
    _Outptr_ IID **Iids)
{
    ULONG Index;

    *IidCount = 0;
    *Iids = CoTaskMemAlloc(Count * sizeof(**Iids));
    if (!*Iids)
        return E_OUTOFMEMORY;

    for (Index = 0; Index < Count; Index++)
        (*Iids)[Index] = *Source[Index];
    *IidCount = Count;
    return S_OK;
}

static
HRESULT
RtGetPropertyValueStatics(
    _Outptr_ IRtPropertyValueStatics **Statics)
{
    IRtPropertyValueStatics *Found;
    HSTRING_HEADER Header;
    HSTRING ClassName;
    HRESULT Status;

    *Statics = NULL;
    if (!PropertyValueStatics)
    {
        Status = WindowsCreateStringReference(L"Windows.Foundation.PropertyValue",
                                              ARRAYSIZE(L"Windows.Foundation.PropertyValue") - 1,
                                              &Header,
                                              &ClassName);
        if (FAILED(Status))
            return Status;

        Status = RoGetActivationFactory(ClassName, &IID_IRtPropertyValueStatics, (void **)&Found);
        if (FAILED(Status))
            return Status;

        /* The factory is agile, so one reference serves every apartment */
        if (InterlockedCompareExchangePointer((void **)&PropertyValueStatics, Found, NULL))
            IRtPropertyValueStatics_Release(Found);
    }

    *Statics = PropertyValueStatics;
    IRtPropertyValueStatics_AddRef(*Statics);
    return S_OK;
}

/* WEAK REFERENCES ***********************************************************/

typedef struct _RT_WEAK_REFERENCE
{
    IRtWeakReference IRtWeakReference_iface;
    LONG RefCount;
    IInspectable *Target;
    LONG *StrongCount;
} RT_WEAK_REFERENCE;

static inline PRT_WEAK_REFERENCE impl_from_IRtWeakReference(IRtWeakReference *iface)
{
    return CONTAINING_RECORD(iface, RT_WEAK_REFERENCE, IRtWeakReference_iface);
}

static
HRESULT
STDMETHODCALLTYPE
WeakReference_QueryInterface(
    _In_ IRtWeakReference *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    if (IsEqualIID(Riid, &IID_IUnknown) || IsEqualIID(Riid, &IID_IRtWeakReference))
    {
        *Object = iface;
        IRtWeakReference_AddRef(iface);
        return S_OK;
    }

    *Object = NULL;
    return E_NOINTERFACE;
}

static
ULONG
STDMETHODCALLTYPE
WeakReference_AddRef(
    _In_ IRtWeakReference *iface)
{
    return InterlockedIncrement(&impl_from_IRtWeakReference(iface)->RefCount);
}

static
ULONG
STDMETHODCALLTYPE
WeakReference_Release(
    _In_ IRtWeakReference *iface)
{
    PRT_WEAK_REFERENCE Reference = impl_from_IRtWeakReference(iface);
    ULONG Count = InterlockedDecrement(&Reference->RefCount);

    if (!Count)
        free(Reference);
    return Count;
}

static
HRESULT
STDMETHODCALLTYPE
WeakReference_Resolve(
    _In_ IRtWeakReference *iface,
    _In_ REFIID Riid,
    _Outptr_ IInspectable **Object)
{
    PRT_WEAK_REFERENCE Reference = impl_from_IRtWeakReference(iface);
    IInspectable *Target = NULL;
    LONG Count;
    HRESULT Status;

    *Object = NULL;

    /* Take a strong reference only while the target still has one */
    EnterCriticalSection(&WeakLock);
    if (Reference->Target)
    {
        do
        {
            Count = *Reference->StrongCount;
            if (!Count)
                break;
        } while (InterlockedCompareExchange(Reference->StrongCount, Count + 1, Count) != Count);

        if (Count)
            Target = Reference->Target;
    }
    LeaveCriticalSection(&WeakLock);

    if (!Target)
        return S_OK;

    Status = IInspectable_QueryInterface(Target, Riid, (void **)Object);
    IInspectable_Release(Target);
    return Status;
}

static const IRtWeakReferenceVtbl WeakReferenceVtbl =
{
    WeakReference_QueryInterface,
    WeakReference_AddRef,
    WeakReference_Release,
    WeakReference_Resolve,
};

HRESULT
RtGetWeakReference(
    _Inout_ PRT_WEAK_REFERENCE *Slot,
    _In_ IInspectable *Target,
    _In_ LONG *StrongCount,
    _Outptr_ IRtWeakReference **Reference)
{
    PRT_WEAK_REFERENCE Created;

    *Reference = NULL;
    EnterCriticalSection(&WeakLock);
    if (!*Slot)
    {
        Created = calloc(1, sizeof(*Created));
        if (!Created)
        {
            LeaveCriticalSection(&WeakLock);
            return E_OUTOFMEMORY;
        }

        Created->IRtWeakReference_iface.lpVtbl = &WeakReferenceVtbl;
        Created->RefCount = 1;
        Created->Target = Target;
        Created->StrongCount = StrongCount;
        *Slot = Created;
    }

    *Reference = &(*Slot)->IRtWeakReference_iface;
    IRtWeakReference_AddRef(*Reference);
    LeaveCriticalSection(&WeakLock);
    return S_OK;
}

/**
 * @brief
 * Cuts the link between an object that is going away and its weak reference.
 */
VOID
RtDetachWeakReference(
    _Inout_ PRT_WEAK_REFERENCE *Slot)
{
    PRT_WEAK_REFERENCE Reference;

    EnterCriticalSection(&WeakLock);
    Reference = *Slot;
    *Slot = NULL;
    if (Reference)
        Reference->Target = NULL;
    LeaveCriticalSection(&WeakLock);

    if (Reference)
        IRtWeakReference_Release(&Reference->IRtWeakReference_iface);
}

/* ATTRIBUTE MAP *************************************************************/

/*
 * A live view over the values of one registry key. Only REG_SZ and REG_DWORD values
 * are entries; they become String and UInt32 property values when looked up.
 */
typedef struct _RT_ATTRIBUTE_MAP
{
    IRtMapView IRtMapView_iface;
    IRtKeyValueIterable IRtKeyValueIterable_iface;
    LONG RefCount;
    HKEY Key;
} RT_ATTRIBUTE_MAP, *PRT_ATTRIBUTE_MAP;

typedef struct _RT_ATTRIBUTE_ITERATOR
{
    IRtKeyValueIterator IRtKeyValueIterator_iface;
    LONG RefCount;
    PRT_ATTRIBUTE_MAP Map;
    DWORD NextIndex;
    IRtKeyValuePair *Current;
} RT_ATTRIBUTE_ITERATOR, *PRT_ATTRIBUTE_ITERATOR;

typedef struct _RT_KEY_VALUE_PAIR
{
    IRtKeyValuePair IRtKeyValuePair_iface;
    LONG RefCount;
    HSTRING Key;
    IInspectable *Value;
} RT_KEY_VALUE_PAIR, *PRT_KEY_VALUE_PAIR;

static inline PRT_ATTRIBUTE_MAP impl_from_IRtMapView(IRtMapView *iface)
{
    return CONTAINING_RECORD(iface, RT_ATTRIBUTE_MAP, IRtMapView_iface);
}

static inline PRT_ATTRIBUTE_MAP impl_from_IRtKeyValueIterable(IRtKeyValueIterable *iface)
{
    return CONTAINING_RECORD(iface, RT_ATTRIBUTE_MAP, IRtKeyValueIterable_iface);
}

static inline PRT_ATTRIBUTE_ITERATOR impl_from_IRtKeyValueIterator(IRtKeyValueIterator *iface)
{
    return CONTAINING_RECORD(iface, RT_ATTRIBUTE_ITERATOR, IRtKeyValueIterator_iface);
}

static inline PRT_KEY_VALUE_PAIR impl_from_IRtKeyValuePair(IRtKeyValuePair *iface)
{
    return CONTAINING_RECORD(iface, RT_KEY_VALUE_PAIR, IRtKeyValuePair_iface);
}

static
HRESULT
RtGetEntryType(
    _In_opt_ HKEY Key,
    _In_ PCWSTR Name,
    _Out_ DWORD *Type)
{
    LONG Error;

    *Type = REG_NONE;
    if (!Key)
        return E_BOUNDS;

    Error = RegQueryValueExW(Key, Name, NULL, Type, NULL, NULL);
    if (Error == ERROR_FILE_NOT_FOUND)
        return E_BOUNDS;
    if (Error != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(Error);
    if (*Type != REG_SZ && *Type != REG_DWORD)
        return REGDB_E_INVALIDVALUE;
    return S_OK;
}

static
HRESULT
RtReadEntry(
    _In_opt_ HKEY Key,
    _In_ PCWSTR Name,
    _Outptr_ IInspectable **Value)
{
    IRtPropertyValueStatics *Statics;
    HSTRING Text = NULL;
    PWSTR Buffer = NULL;
    DWORD Type, Data, Size;
    HRESULT Status;
    LONG Error;

    *Value = NULL;
    Status = RtGetEntryType(Key, Name, &Type);
    if (FAILED(Status))
        return Status;

    Status = RtGetPropertyValueStatics(&Statics);
    if (FAILED(Status))
        return Status;

    if (Type == REG_DWORD)
    {
        Size = sizeof(Data);
        Error = RegGetValueW(Key, NULL, Name, RRF_RT_REG_DWORD, NULL, &Data, &Size);
        Status = HRESULT_FROM_WIN32(Error);
        if (SUCCEEDED(Status))
            Status = IRtPropertyValueStatics_CreateUInt32(Statics, Data, Value);
    }
    else
    {
        Size = 0;
        Error = RegGetValueW(Key, NULL, Name, RRF_RT_REG_SZ, NULL, NULL, &Size);
        Status = HRESULT_FROM_WIN32(Error);
        if (SUCCEEDED(Status))
        {
            Buffer = malloc(Size + sizeof(WCHAR));
            Status = Buffer ? S_OK : E_OUTOFMEMORY;
        }
        if (SUCCEEDED(Status))
        {
            Error = RegGetValueW(Key, NULL, Name, RRF_RT_REG_SZ, NULL, Buffer, &Size);
            Status = HRESULT_FROM_WIN32(Error);
        }
        if (SUCCEEDED(Status))
            Status = RtCreateHString(Buffer, &Text);
        if (SUCCEEDED(Status))
            Status = IRtPropertyValueStatics_CreateString(Statics, Text, Value);
        WindowsDeleteString(Text);
        free(Buffer);
    }

    IRtPropertyValueStatics_Release(Statics);
    return Status;
}

static
HRESULT
RtCheckKeyName(
    _In_opt_ HSTRING Key)
{
    BOOL HasNull = FALSE;

    if (WindowsIsStringEmpty(Key))
        return S_OK;
    if (SUCCEEDED(WindowsStringHasEmbeddedNull(Key, &HasNull)) && HasNull)
    {
        RoOriginateErrorW(E_INVALIDARG, 3, L"key");
        return E_INVALIDARG;
    }
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
KeyValuePair_QueryInterface(
    _In_ IRtKeyValuePair *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    if (IsEqualIID(Riid, &IID_IUnknown) ||
        IsEqualIID(Riid, &IID_IInspectable) ||
        IsEqualIID(Riid, &IID_IRtKeyValuePair) ||
        IsEqualIID(Riid, &IID_IAgileObject))
    {
        *Object = iface;
        IRtKeyValuePair_AddRef(iface);
        return S_OK;
    }

    *Object = NULL;
    return E_NOINTERFACE;
}

static
ULONG
STDMETHODCALLTYPE
KeyValuePair_AddRef(
    _In_ IRtKeyValuePair *iface)
{
    return InterlockedIncrement(&impl_from_IRtKeyValuePair(iface)->RefCount);
}

static
ULONG
STDMETHODCALLTYPE
KeyValuePair_Release(
    _In_ IRtKeyValuePair *iface)
{
    PRT_KEY_VALUE_PAIR Pair = impl_from_IRtKeyValuePair(iface);
    ULONG Count = InterlockedDecrement(&Pair->RefCount);

    if (!Count)
    {
        WindowsDeleteString(Pair->Key);
        if (Pair->Value)
            IInspectable_Release(Pair->Value);
        free(Pair);
    }
    return Count;
}

static
HRESULT
STDMETHODCALLTYPE
KeyValuePair_GetIids(
    _In_ IRtKeyValuePair *iface,
    _Out_ ULONG *Count,
    _Outptr_ IID **Iids)
{
    static const IID *const Implemented[] = { &IID_IRtKeyValuePair };

    return RtCopyIids(ARRAYSIZE(Implemented), Implemented, Count, Iids);
}

static
HRESULT
STDMETHODCALLTYPE
KeyValuePair_GetRuntimeClassName(
    _In_ IRtKeyValuePair *iface,
    _Out_ HSTRING *Name)
{
    *Name = NULL;
    return E_NOTIMPL;
}

static
HRESULT
STDMETHODCALLTYPE
KeyValuePair_GetTrustLevel(
    _In_ IRtKeyValuePair *iface,
    _Out_ TrustLevel *Level)
{
    *Level = BaseTrust;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
KeyValuePair_get_Key(
    _In_ IRtKeyValuePair *iface,
    _Out_ HSTRING *Key)
{
    return WindowsDuplicateString(impl_from_IRtKeyValuePair(iface)->Key, Key);
}

static
HRESULT
STDMETHODCALLTYPE
KeyValuePair_get_Value(
    _In_ IRtKeyValuePair *iface,
    _Outptr_result_maybenull_ IInspectable **Value)
{
    PRT_KEY_VALUE_PAIR Pair = impl_from_IRtKeyValuePair(iface);

    *Value = Pair->Value;
    if (*Value)
        IInspectable_AddRef(*Value);
    return S_OK;
}

static const IRtKeyValuePairVtbl KeyValuePairVtbl =
{
    KeyValuePair_QueryInterface,
    KeyValuePair_AddRef,
    KeyValuePair_Release,
    KeyValuePair_GetIids,
    KeyValuePair_GetRuntimeClassName,
    KeyValuePair_GetTrustLevel,
    KeyValuePair_get_Key,
    KeyValuePair_get_Value,
};

/* Moves to the next REG_SZ or REG_DWORD value, or clears Current at the end */
static
HRESULT
AttributeIterator_Advance(
    _Inout_ PRT_ATTRIBUTE_ITERATOR Iterator)
{
    WCHAR Name[MAX_PATH];
    PWSTR LongName = NULL;
    DWORD Length, Type;
    PRT_KEY_VALUE_PAIR Pair;
    HRESULT Status;
    LONG Error;

    if (Iterator->Current)
    {
        IRtKeyValuePair_Release(Iterator->Current);
        Iterator->Current = NULL;
    }
    if (!Iterator->Map->Key)
        return S_OK;

    for (;;)
    {
        Length = ARRAYSIZE(Name);
        Error = RegEnumValueW(Iterator->Map->Key, Iterator->NextIndex, Name, &Length, NULL, &Type, NULL, NULL);
        if (Error == ERROR_MORE_DATA)
        {
            Error = RegQueryInfoKeyW(Iterator->Map->Key, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                     &Length, NULL, NULL, NULL);
            if (Error != ERROR_SUCCESS)
                return HRESULT_FROM_WIN32(Error);
            Length++;
            free(LongName);
            LongName = malloc(Length * sizeof(WCHAR));
            if (!LongName)
                return E_OUTOFMEMORY;
            Error = RegEnumValueW(Iterator->Map->Key, Iterator->NextIndex, LongName, &Length, NULL, &Type, NULL, NULL);
        }
        if (Error == ERROR_NO_MORE_ITEMS)
            break;
        if (Error != ERROR_SUCCESS)
        {
            free(LongName);
            return HRESULT_FROM_WIN32(Error);
        }

        Iterator->NextIndex++;
        if (Type != REG_SZ && Type != REG_DWORD)
            continue;

        Pair = calloc(1, sizeof(*Pair));
        if (!Pair)
        {
            free(LongName);
            return E_OUTOFMEMORY;
        }
        Pair->IRtKeyValuePair_iface.lpVtbl = &KeyValuePairVtbl;
        Pair->RefCount = 1;

        Status = RtCreateHString(LongName ? LongName : Name, &Pair->Key);
        if (SUCCEEDED(Status))
            Status = RtReadEntry(Iterator->Map->Key, LongName ? LongName : Name, &Pair->Value);
        free(LongName);
        if (FAILED(Status))
        {
            IRtKeyValuePair_Release(&Pair->IRtKeyValuePair_iface);
            return Status;
        }

        Iterator->Current = &Pair->IRtKeyValuePair_iface;
        return S_OK;
    }

    free(LongName);
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
AttributeIterator_QueryInterface(
    _In_ IRtKeyValueIterator *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    if (IsEqualIID(Riid, &IID_IUnknown) ||
        IsEqualIID(Riid, &IID_IInspectable) ||
        IsEqualIID(Riid, &IID_IRtKeyValueIterator) ||
        IsEqualIID(Riid, &IID_IAgileObject))
    {
        *Object = iface;
        IRtKeyValueIterator_AddRef(iface);
        return S_OK;
    }

    *Object = NULL;
    return E_NOINTERFACE;
}

static
ULONG
STDMETHODCALLTYPE
AttributeIterator_AddRef(
    _In_ IRtKeyValueIterator *iface)
{
    return InterlockedIncrement(&impl_from_IRtKeyValueIterator(iface)->RefCount);
}

static
ULONG
STDMETHODCALLTYPE
AttributeIterator_Release(
    _In_ IRtKeyValueIterator *iface)
{
    PRT_ATTRIBUTE_ITERATOR Iterator = impl_from_IRtKeyValueIterator(iface);
    ULONG Count = InterlockedDecrement(&Iterator->RefCount);

    if (!Count)
    {
        if (Iterator->Current)
            IRtKeyValuePair_Release(Iterator->Current);
        IRtMapView_Release(&Iterator->Map->IRtMapView_iface);
        free(Iterator);
    }
    return Count;
}

static
HRESULT
STDMETHODCALLTYPE
AttributeIterator_GetIids(
    _In_ IRtKeyValueIterator *iface,
    _Out_ ULONG *Count,
    _Outptr_ IID **Iids)
{
    static const IID *const Implemented[] = { &IID_IRtKeyValueIterator };

    return RtCopyIids(ARRAYSIZE(Implemented), Implemented, Count, Iids);
}

static
HRESULT
STDMETHODCALLTYPE
AttributeIterator_GetRuntimeClassName(
    _In_ IRtKeyValueIterator *iface,
    _Out_ HSTRING *Name)
{
    *Name = NULL;
    return E_NOTIMPL;
}

static
HRESULT
STDMETHODCALLTYPE
AttributeIterator_GetTrustLevel(
    _In_ IRtKeyValueIterator *iface,
    _Out_ TrustLevel *Level)
{
    *Level = BaseTrust;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
AttributeIterator_get_Current(
    _In_ IRtKeyValueIterator *iface,
    _Outptr_ IRtKeyValuePair **Current)
{
    PRT_ATTRIBUTE_ITERATOR Iterator = impl_from_IRtKeyValueIterator(iface);

    *Current = Iterator->Current;
    if (!*Current)
        return E_BOUNDS;
    IRtKeyValuePair_AddRef(*Current);
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
AttributeIterator_get_HasCurrent(
    _In_ IRtKeyValueIterator *iface,
    _Out_ boolean *HasCurrent)
{
    *HasCurrent = impl_from_IRtKeyValueIterator(iface)->Current != NULL;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
AttributeIterator_MoveNext(
    _In_ IRtKeyValueIterator *iface,
    _Out_ boolean *HasCurrent)
{
    PRT_ATTRIBUTE_ITERATOR Iterator = impl_from_IRtKeyValueIterator(iface);
    HRESULT Status;

    *HasCurrent = FALSE;
    if (!Iterator->Current)
        return E_BOUNDS;

    Status = AttributeIterator_Advance(Iterator);
    *HasCurrent = Iterator->Current != NULL;
    return Status;
}

static
HRESULT
STDMETHODCALLTYPE
AttributeIterator_GetMany(
    _In_ IRtKeyValueIterator *iface,
    _In_ UINT32 Capacity,
    _Out_writes_to_(Capacity, *Actual) IRtKeyValuePair **Items,
    _Out_ UINT32 *Actual)
{
    PRT_ATTRIBUTE_ITERATOR Iterator = impl_from_IRtKeyValueIterator(iface);
    HRESULT Status = S_OK;

    *Actual = 0;
    while (*Actual < Capacity && Iterator->Current)
    {
        Items[*Actual] = Iterator->Current;
        IRtKeyValuePair_AddRef(Items[*Actual]);
        (*Actual)++;

        Status = AttributeIterator_Advance(Iterator);
        if (FAILED(Status))
            break;
    }
    return Status;
}

static const IRtKeyValueIteratorVtbl AttributeIteratorVtbl =
{
    AttributeIterator_QueryInterface,
    AttributeIterator_AddRef,
    AttributeIterator_Release,
    AttributeIterator_GetIids,
    AttributeIterator_GetRuntimeClassName,
    AttributeIterator_GetTrustLevel,
    AttributeIterator_get_Current,
    AttributeIterator_get_HasCurrent,
    AttributeIterator_MoveNext,
    AttributeIterator_GetMany,
};

static
HRESULT
STDMETHODCALLTYPE
AttributeMap_QueryInterface(
    _In_ IRtMapView *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    PRT_ATTRIBUTE_MAP Map = impl_from_IRtMapView(iface);

    if (IsEqualIID(Riid, &IID_IUnknown) ||
        IsEqualIID(Riid, &IID_IInspectable) ||
        IsEqualIID(Riid, &IID_IRtMapView) ||
        IsEqualIID(Riid, &IID_IAgileObject))
    {
        *Object = &Map->IRtMapView_iface;
    }
    else if (IsEqualIID(Riid, &IID_IRtKeyValueIterable))
    {
        *Object = &Map->IRtKeyValueIterable_iface;
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
AttributeMap_AddRef(
    _In_ IRtMapView *iface)
{
    return InterlockedIncrement(&impl_from_IRtMapView(iface)->RefCount);
}

static
ULONG
STDMETHODCALLTYPE
AttributeMap_Release(
    _In_ IRtMapView *iface)
{
    PRT_ATTRIBUTE_MAP Map = impl_from_IRtMapView(iface);
    ULONG Count = InterlockedDecrement(&Map->RefCount);

    if (!Count)
    {
        if (Map->Key)
            RegCloseKey(Map->Key);
        free(Map);
    }
    return Count;
}

static
HRESULT
STDMETHODCALLTYPE
AttributeMap_GetIids(
    _In_ IRtMapView *iface,
    _Out_ ULONG *Count,
    _Outptr_ IID **Iids)
{
    static const IID *const Implemented[] = { &IID_IRtMapView, &IID_IRtKeyValueIterable };

    return RtCopyIids(ARRAYSIZE(Implemented), Implemented, Count, Iids);
}

static
HRESULT
STDMETHODCALLTYPE
AttributeMap_GetRuntimeClassName(
    _In_ IRtMapView *iface,
    _Out_ HSTRING *Name)
{
    *Name = NULL;
    return E_NOTIMPL;
}

static
HRESULT
STDMETHODCALLTYPE
AttributeMap_GetTrustLevel(
    _In_ IRtMapView *iface,
    _Out_ TrustLevel *Level)
{
    *Level = BaseTrust;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
AttributeMap_Lookup(
    _In_ IRtMapView *iface,
    _In_opt_ HSTRING Key,
    _Outptr_ IInspectable **Value)
{
    HRESULT Status;

    *Value = NULL;
    Status = RtCheckKeyName(Key);
    if (FAILED(Status))
        return Status;
    return RtReadEntry(impl_from_IRtMapView(iface)->Key, WindowsGetStringRawBuffer(Key, NULL), Value);
}

static
HRESULT
STDMETHODCALLTYPE
AttributeMap_get_Size(
    _In_ IRtMapView *iface,
    _Out_ UINT32 *Size)
{
    PRT_ATTRIBUTE_MAP Map = impl_from_IRtMapView(iface);
    DWORD Values = 0;
    LONG Error;

    *Size = 0;
    if (!Map->Key)
        return S_OK;

    /* The raw value count, values of other types included */
    Error = RegQueryInfoKeyW(Map->Key, NULL, NULL, NULL, NULL, NULL, NULL, &Values, NULL, NULL, NULL, NULL);
    if (Error != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(Error);
    *Size = Values;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
AttributeMap_HasKey(
    _In_ IRtMapView *iface,
    _In_opt_ HSTRING Key,
    _Out_ boolean *Found)
{
    DWORD Type;
    HRESULT Status;

    *Found = FALSE;
    Status = RtCheckKeyName(Key);
    if (FAILED(Status))
        return Status;

    Status = RtGetEntryType(impl_from_IRtMapView(iface)->Key, WindowsGetStringRawBuffer(Key, NULL), &Type);
    if (Status == E_BOUNDS)
        return S_OK;
    if (FAILED(Status))
        return Status;
    *Found = TRUE;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
AttributeMap_Split(
    _In_ IRtMapView *iface,
    _Outptr_result_maybenull_ IRtMapView **First,
    _Outptr_result_maybenull_ IRtMapView **Second)
{
    *First = NULL;
    *Second = NULL;
    return S_OK;
}

static const IRtMapViewVtbl AttributeMapVtbl =
{
    AttributeMap_QueryInterface,
    AttributeMap_AddRef,
    AttributeMap_Release,
    AttributeMap_GetIids,
    AttributeMap_GetRuntimeClassName,
    AttributeMap_GetTrustLevel,
    AttributeMap_Lookup,
    AttributeMap_get_Size,
    AttributeMap_HasKey,
    AttributeMap_Split,
};

static
HRESULT
STDMETHODCALLTYPE
AttributeIterable_QueryInterface(
    _In_ IRtKeyValueIterable *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    return AttributeMap_QueryInterface(&impl_from_IRtKeyValueIterable(iface)->IRtMapView_iface, Riid, Object);
}

static
ULONG
STDMETHODCALLTYPE
AttributeIterable_AddRef(
    _In_ IRtKeyValueIterable *iface)
{
    return AttributeMap_AddRef(&impl_from_IRtKeyValueIterable(iface)->IRtMapView_iface);
}

static
ULONG
STDMETHODCALLTYPE
AttributeIterable_Release(
    _In_ IRtKeyValueIterable *iface)
{
    return AttributeMap_Release(&impl_from_IRtKeyValueIterable(iface)->IRtMapView_iface);
}

static
HRESULT
STDMETHODCALLTYPE
AttributeIterable_GetIids(
    _In_ IRtKeyValueIterable *iface,
    _Out_ ULONG *Count,
    _Outptr_ IID **Iids)
{
    return AttributeMap_GetIids(&impl_from_IRtKeyValueIterable(iface)->IRtMapView_iface, Count, Iids);
}

static
HRESULT
STDMETHODCALLTYPE
AttributeIterable_GetRuntimeClassName(
    _In_ IRtKeyValueIterable *iface,
    _Out_ HSTRING *Name)
{
    *Name = NULL;
    return E_NOTIMPL;
}

static
HRESULT
STDMETHODCALLTYPE
AttributeIterable_GetTrustLevel(
    _In_ IRtKeyValueIterable *iface,
    _Out_ TrustLevel *Level)
{
    *Level = BaseTrust;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
AttributeIterable_First(
    _In_ IRtKeyValueIterable *iface,
    _Outptr_ IRtKeyValueIterator **First)
{
    PRT_ATTRIBUTE_MAP Map = impl_from_IRtKeyValueIterable(iface);
    PRT_ATTRIBUTE_ITERATOR Iterator;
    HRESULT Status;

    *First = NULL;
    Iterator = calloc(1, sizeof(*Iterator));
    if (!Iterator)
        return E_OUTOFMEMORY;

    Iterator->IRtKeyValueIterator_iface.lpVtbl = &AttributeIteratorVtbl;
    Iterator->RefCount = 1;
    Iterator->Map = Map;
    IRtMapView_AddRef(&Map->IRtMapView_iface);

    Status = AttributeIterator_Advance(Iterator);
    if (FAILED(Status))
    {
        IRtKeyValueIterator_Release(&Iterator->IRtKeyValueIterator_iface);
        return Status;
    }

    *First = &Iterator->IRtKeyValueIterator_iface;
    return S_OK;
}

static const IRtKeyValueIterableVtbl AttributeIterableVtbl =
{
    AttributeIterable_QueryInterface,
    AttributeIterable_AddRef,
    AttributeIterable_Release,
    AttributeIterable_GetIids,
    AttributeIterable_GetRuntimeClassName,
    AttributeIterable_GetTrustLevel,
    AttributeIterable_First,
};

/**
 * @brief
 * Makes the attribute view of a registration from the given subkey of its key.
 * A missing subkey gives an empty view.
 */
HRESULT
RtCreateAttributeMap(
    _In_opt_ HKEY Parent,
    _In_ PCWSTR SubKey,
    _Outptr_ IRtMapView **Map)
{
    PRT_ATTRIBUTE_MAP Created;
    HKEY Key = NULL;
    LONG Error;

    *Map = NULL;
    if (Parent)
    {
        Error = RegOpenKeyExW(Parent, SubKey, 0, RT_REGISTRY_READ, &Key);
        if (Error == ERROR_FILE_NOT_FOUND)
            Key = NULL;
        else if (Error != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(Error);
    }

    Created = calloc(1, sizeof(*Created));
    if (!Created)
    {
        if (Key)
            RegCloseKey(Key);
        return E_OUTOFMEMORY;
    }

    Created->IRtMapView_iface.lpVtbl = &AttributeMapVtbl;
    Created->IRtKeyValueIterable_iface.lpVtbl = &AttributeIterableVtbl;
    Created->RefCount = 1;
    Created->Key = Key;
    *Map = &Created->IRtMapView_iface;
    return S_OK;
}

/* CATALOG *******************************************************************/

static
HRESULT
RtOpenPackagedEntry(
    _In_ HKEY Root,
    _In_ PCWSTR PackageFullName,
    _In_ PCWSTR Collection,
    _In_ PCWSTR Name,
    _Out_ HKEY *Key)
{
    WCHAR Path[MAX_PATH * 2];
    LONG Error;

    *Key = NULL;
    if (_snwprintf(Path, ARRAYSIZE(Path), L"%s\\%s\\%s\\%s", RT_PACKAGED_ROOT,
                   PackageFullName, Collection, Name) < 0)
    {
        return E_INVALIDARG;
    }
    Path[ARRAYSIZE(Path) - 1] = UNICODE_NULL;

    Error = RegOpenKeyExW(Root, Path, 0, RT_REGISTRY_READ, Key);
    return HRESULT_FROM_WIN32(Error);
}

/**
 * @brief
 * Finds a class or server entry in one package, user registrations first.
 */
static
HRESULT
RtOpenPackageEntry(
    _In_ PCWSTR PackageFullName,
    _In_ PCWSTR Collection,
    _In_ PCWSTR Name,
    _Out_ HKEY *Key,
    _Out_ RegistrationScope *Scope)
{
    HRESULT Status = REGDB_E_CLASSNOTREG;
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(PackagedRoots); Index++)
    {
        Status = RtOpenPackagedEntry(PackagedRoots[Index].Root, PackageFullName, Collection, Name, Key);
        if (SUCCEEDED(Status))
        {
            *Scope = PackagedRoots[Index].Scope;
            return S_OK;
        }
    }
    return Status;
}

static
HRESULT
RtOpenMachineEntry(
    _In_ PCWSTR Collection,
    _In_ PCWSTR Name,
    _Out_ HKEY *Key)
{
    WCHAR Path[MAX_PATH * 2];
    LONG Error;

    *Key = NULL;
    if (_snwprintf(Path, ARRAYSIZE(Path), L"%s\\%s\\%s", RT_RUNTIME_ROOT, Collection, Name) < 0)
        return E_INVALIDARG;
    Path[ARRAYSIZE(Path) - 1] = UNICODE_NULL;

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, RT_REGISTRY_READ, Key);
    return HRESULT_FROM_WIN32(Error);
}

/**
 * @brief
 * Looks up a registry entry the way the catalog does: the machine wide runtime
 * registrations, then the package of the caller. Classes of other packages are only
 * found with an explicit package, which restricts the search to that package.
 */
static
HRESULT
RtLocateEntry(
    _In_ PCWSTR Collection,
    _In_ PCWSTR Name,
    _In_opt_ PCWSTR PackageFullName,
    _Out_writes_(RT_PACKAGE_FULL_NAME_LENGTH) PWSTR FoundPackage,
    _Out_ HKEY *Key,
    _Out_ RegistrationScope *Scope)
{
    WCHAR CallerPackage[RT_PACKAGE_FULL_NAME_LENGTH];

    FoundPackage[0] = UNICODE_NULL;
    *Key = NULL;

    if (PackageFullName && PackageFullName[0])
    {
        if (FAILED(RtOpenPackageEntry(PackageFullName, Collection, Name, Key, Scope)))
            return REGDB_E_CLASSNOTREG;
        wcsncpy(FoundPackage, PackageFullName, RT_PACKAGE_FULL_NAME_LENGTH - 1);
        FoundPackage[RT_PACKAGE_FULL_NAME_LENGTH - 1] = UNICODE_NULL;
        return S_OK;
    }

    if (SUCCEEDED(RtOpenMachineEntry(Collection, Name, Key)))
    {
        *Scope = RegistrationScope_PerMachine;
        return S_OK;
    }

    if (RtGetCurrentPackageFullName(CallerPackage) == S_OK &&
        SUCCEEDED(RtOpenPackageEntry(CallerPackage, Collection, Name, Key, Scope)))
    {
        wcscpy(FoundPackage, CallerPackage);
        return S_OK;
    }
    return REGDB_E_CLASSNOTREG;
}

/**
 * @brief
 * Finds the registration of an activatable class.
 *
 * @param[in] PackageFullName
 * Restricts the search to this package when given.
 *
 * @return
 * REGDB_E_CLASSNOTREG when no registration exists.
 */
HRESULT
RtFindClass(
    _In_ HSTRING ClassId,
    _In_opt_ PCWSTR PackageFullName,
    _Out_ PRT_CLASS_INFO Info)
{
    PWSTR PackagePath = NULL, RelativePath;
    SIZE_T Length;
    HRESULT Status;

    RtlZeroMemory(Info, sizeof(*Info));

    Status = RtValidateName(ClassId);
    if (FAILED(Status))
        return Status;

    Status = RtLocateEntry(L"ActivatableClassId",
                           WindowsGetStringRawBuffer(ClassId, NULL),
                           PackageFullName,
                           Info->PackageFullName,
                           &Info->ClassKey,
                           &Info->Scope);
    if (FAILED(Status))
        return REGDB_E_CLASSNOTREG;

    Status = WindowsDuplicateString(ClassId, &Info->ClassId);
    if (FAILED(Status))
        goto Fail;

    Info->Type = RtReadDword(Info->ClassKey, L"ActivationType", ActivationType_InProcess);
    if (Info->Type == ActivationType_InProcess)
    {
        Info->Threading = RtReadDword(Info->ClassKey, L"Threading", ThreadingType_BOTH);
        Status = RtDuplicateRegistryString(Info->ClassKey, L"DllPath", &Info->DllPath);
        if (Status == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            Status = REGDB_E_INVALIDVALUE;
        if (FAILED(Status))
            goto Fail;

        /* Package relative DLLs live in the package's install folder */
        if (Info->PackageFullName[0] && RtReadDword(Info->ClassKey, L"IsPackageRelativePath", 0))
        {
            Status = RtGetPackagePath(Info->PackageFullName, &PackagePath);
            if (FAILED(Status))
                goto Fail;

            Length = wcslen(PackagePath) + wcslen(Info->DllPath) + 2;
            RelativePath = Info->DllPath;
            Info->DllPath = malloc(Length * sizeof(WCHAR));
            if (!Info->DllPath)
            {
                free(RelativePath);
                Status = E_OUTOFMEMORY;
                goto Fail;
            }
            _snwprintf(Info->DllPath, Length, L"%s\\%s", PackagePath, RelativePath);
            Info->DllPath[Length - 1] = UNICODE_NULL;
            free(RelativePath);
        }
    }
    else if (Info->Type == ActivationType_OutOfProcess)
    {
        Status = RtDuplicateRegistryString(Info->ClassKey, L"Server", &Info->ServerName);
        if (Status == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            Status = REGDB_E_INVALIDVALUE;
        if (FAILED(Status))
            goto Fail;
    }

    free(PackagePath);
    return S_OK;

Fail:
    free(PackagePath);
    RtFreeClassInfo(Info);
    return Status;
}

VOID
RtFreeClassInfo(
    _Inout_ PRT_CLASS_INFO Info)
{
    WindowsDeleteString(Info->ClassId);
    if (Info->ClassKey)
        RegCloseKey(Info->ClassKey);
    free(Info->DllPath);
    free(Info->ServerName);
    RtlZeroMemory(Info, sizeof(*Info));
}

/* SERVER REGISTRATION *******************************************************/

typedef struct _RT_SERVER_REGISTRATION
{
    IExeServerRegistration IExeServerRegistration_iface;
    IServerAttributeInfo IServerAttributeInfo_iface;
    IRtWeakReferenceSource IRtWeakReferenceSource_iface;
    LONG RefCount;
    PRT_WEAK_REFERENCE Weak;
    HSTRING ServerName;
    HSTRING ExePath;
    HSTRING CommandLine;
    HSTRING AppUserModelId;
    HSTRING Identity;
    HSTRING Permissions;
    IdentityType Identities;
    InstancingType Instancing;
    IRtMapView *Attributes;
} RT_SERVER_REGISTRATION, *PRT_SERVER_REGISTRATION;

static inline PRT_SERVER_REGISTRATION impl_from_IExeServerRegistration(IExeServerRegistration *iface)
{
    return CONTAINING_RECORD(iface, RT_SERVER_REGISTRATION, IExeServerRegistration_iface);
}

static inline PRT_SERVER_REGISTRATION impl_from_IServerAttributeInfo(IServerAttributeInfo *iface)
{
    return CONTAINING_RECORD(iface, RT_SERVER_REGISTRATION, IServerAttributeInfo_iface);
}

static inline PRT_SERVER_REGISTRATION impl_server_from_IRtWeakReferenceSource(IRtWeakReferenceSource *iface)
{
    return CONTAINING_RECORD(iface, RT_SERVER_REGISTRATION, IRtWeakReferenceSource_iface);
}

static
HRESULT
STDMETHODCALLTYPE
ServerRegistration_QueryInterface(
    _In_ IExeServerRegistration *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    PRT_SERVER_REGISTRATION Server = impl_from_IExeServerRegistration(iface);

    if (IsEqualIID(Riid, &IID_IUnknown) ||
        IsEqualIID(Riid, &IID_IInspectable) ||
        IsEqualIID(Riid, &IID_IExeServerRegistration))
    {
        *Object = &Server->IExeServerRegistration_iface;
    }
    else if (IsEqualIID(Riid, &IID_IServerAttributeInfo))
    {
        *Object = &Server->IServerAttributeInfo_iface;
    }
    else if (IsEqualIID(Riid, &IID_IRtWeakReferenceSource))
    {
        *Object = &Server->IRtWeakReferenceSource_iface;
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
ServerRegistration_AddRef(
    _In_ IExeServerRegistration *iface)
{
    return InterlockedIncrement(&impl_from_IExeServerRegistration(iface)->RefCount);
}

static
ULONG
STDMETHODCALLTYPE
ServerRegistration_Release(
    _In_ IExeServerRegistration *iface)
{
    PRT_SERVER_REGISTRATION Server = impl_from_IExeServerRegistration(iface);
    ULONG Count = InterlockedDecrement(&Server->RefCount);

    if (!Count)
    {
        RtDetachWeakReference(&Server->Weak);
        WindowsDeleteString(Server->ServerName);
        WindowsDeleteString(Server->ExePath);
        WindowsDeleteString(Server->CommandLine);
        WindowsDeleteString(Server->AppUserModelId);
        WindowsDeleteString(Server->Identity);
        WindowsDeleteString(Server->Permissions);
        if (Server->Attributes)
            IRtMapView_Release(Server->Attributes);
        free(Server);
    }
    return Count;
}

static
HRESULT
STDMETHODCALLTYPE
ServerRegistration_GetIids(
    _In_ IExeServerRegistration *iface,
    _Out_ ULONG *Count,
    _Outptr_ IID **Iids)
{
    static const IID *const Implemented[] =
    {
        &IID_IExeServerRegistration,
        &IID_IRtWeakReferenceSource,
        &IID_IServerAttributeInfo,
    };

    return RtCopyIids(ARRAYSIZE(Implemented), Implemented, Count, Iids);
}

static
HRESULT
STDMETHODCALLTYPE
ServerRegistration_GetRuntimeClassName(
    _In_ IExeServerRegistration *iface,
    _Out_ HSTRING *Name)
{
    *Name = NULL;
    RoOriginateError(E_NOTIMPL, NULL);
    return E_NOTIMPL;
}

static
HRESULT
STDMETHODCALLTYPE
ServerRegistration_GetTrustLevel(
    _In_ IExeServerRegistration *iface,
    _Out_ TrustLevel *Level)
{
    RoOriginateError(E_NOTIMPL, NULL);
    return E_NOTIMPL;
}

static
HRESULT
STDMETHODCALLTYPE
ServerRegistration_get_ServerName(
    _In_ IExeServerRegistration *iface,
    _Out_ HSTRING *Value)
{
    *Value = NULL;
    WindowsDuplicateString(impl_from_IExeServerRegistration(iface)->ServerName, Value);
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ServerRegistration_get_ExePath(
    _In_ IExeServerRegistration *iface,
    _Out_ HSTRING *Value)
{
    *Value = NULL;
    WindowsDuplicateString(impl_from_IExeServerRegistration(iface)->ExePath, Value);
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ServerRegistration_get_CommandLine(
    _In_ IExeServerRegistration *iface,
    _Out_ HSTRING *Value)
{
    *Value = NULL;
    WindowsDuplicateString(impl_from_IExeServerRegistration(iface)->CommandLine, Value);
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ServerRegistration_get_AppUserModelId(
    _In_ IExeServerRegistration *iface,
    _Out_ HSTRING *Value)
{
    *Value = NULL;
    WindowsDuplicateString(impl_from_IExeServerRegistration(iface)->AppUserModelId, Value);
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ServerRegistration_get_IdentityType(
    _In_ IExeServerRegistration *iface,
    _Out_ IdentityType *Value)
{
    *Value = impl_from_IExeServerRegistration(iface)->Identities;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ServerRegistration_get_Identity(
    _In_ IExeServerRegistration *iface,
    _Out_ HSTRING *Value)
{
    *Value = NULL;
    WindowsDuplicateString(impl_from_IExeServerRegistration(iface)->Identity, Value);
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ServerRegistration_get_Instancing(
    _In_ IExeServerRegistration *iface,
    _Out_ InstancingType *Value)
{
    *Value = impl_from_IExeServerRegistration(iface)->Instancing;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ServerRegistration_get_Permissions(
    _In_ IExeServerRegistration *iface,
    _Out_ HSTRING *Value)
{
    *Value = NULL;
    WindowsDuplicateString(impl_from_IExeServerRegistration(iface)->Permissions, Value);
    return S_OK;
}

static const IExeServerRegistrationVtbl ServerRegistrationVtbl =
{
    ServerRegistration_QueryInterface,
    ServerRegistration_AddRef,
    ServerRegistration_Release,
    ServerRegistration_GetIids,
    ServerRegistration_GetRuntimeClassName,
    ServerRegistration_GetTrustLevel,
    ServerRegistration_get_ServerName,
    ServerRegistration_get_ExePath,
    ServerRegistration_get_CommandLine,
    ServerRegistration_get_AppUserModelId,
    ServerRegistration_get_IdentityType,
    ServerRegistration_get_Identity,
    ServerRegistration_get_Instancing,
    ServerRegistration_get_Permissions,
};

static
HRESULT
STDMETHODCALLTYPE
ServerAttributes_QueryInterface(
    _In_ IServerAttributeInfo *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    return ServerRegistration_QueryInterface(&impl_from_IServerAttributeInfo(iface)->IExeServerRegistration_iface, Riid, Object);
}

static
ULONG
STDMETHODCALLTYPE
ServerAttributes_AddRef(
    _In_ IServerAttributeInfo *iface)
{
    return ServerRegistration_AddRef(&impl_from_IServerAttributeInfo(iface)->IExeServerRegistration_iface);
}

static
ULONG
STDMETHODCALLTYPE
ServerAttributes_Release(
    _In_ IServerAttributeInfo *iface)
{
    return ServerRegistration_Release(&impl_from_IServerAttributeInfo(iface)->IExeServerRegistration_iface);
}

static
HRESULT
STDMETHODCALLTYPE
ServerAttributes_GetIids(
    _In_ IServerAttributeInfo *iface,
    _Out_ ULONG *Count,
    _Outptr_ IID **Iids)
{
    return ServerRegistration_GetIids(&impl_from_IServerAttributeInfo(iface)->IExeServerRegistration_iface, Count, Iids);
}

static
HRESULT
STDMETHODCALLTYPE
ServerAttributes_GetRuntimeClassName(
    _In_ IServerAttributeInfo *iface,
    _Out_ HSTRING *Name)
{
    return ServerRegistration_GetRuntimeClassName(&impl_from_IServerAttributeInfo(iface)->IExeServerRegistration_iface, Name);
}

static
HRESULT
STDMETHODCALLTYPE
ServerAttributes_GetTrustLevel(
    _In_ IServerAttributeInfo *iface,
    _Out_ TrustLevel *Level)
{
    return ServerRegistration_GetTrustLevel(&impl_from_IServerAttributeInfo(iface)->IExeServerRegistration_iface, Level);
}

static
HRESULT
STDMETHODCALLTYPE
ServerAttributes_get_Attributes(
    _In_ IServerAttributeInfo *iface,
    _Outptr_ IRtMapView **Attributes)
{
    *Attributes = impl_from_IServerAttributeInfo(iface)->Attributes;
    IRtMapView_AddRef(*Attributes);
    return S_OK;
}

static const IServerAttributeInfoVtbl ServerAttributesVtbl =
{
    ServerAttributes_QueryInterface,
    ServerAttributes_AddRef,
    ServerAttributes_Release,
    ServerAttributes_GetIids,
    ServerAttributes_GetRuntimeClassName,
    ServerAttributes_GetTrustLevel,
    ServerAttributes_get_Attributes,
};

static
HRESULT
STDMETHODCALLTYPE
ServerWeakSource_QueryInterface(
    _In_ IRtWeakReferenceSource *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    return ServerRegistration_QueryInterface(&impl_server_from_IRtWeakReferenceSource(iface)->IExeServerRegistration_iface, Riid, Object);
}

static
ULONG
STDMETHODCALLTYPE
ServerWeakSource_AddRef(
    _In_ IRtWeakReferenceSource *iface)
{
    return ServerRegistration_AddRef(&impl_server_from_IRtWeakReferenceSource(iface)->IExeServerRegistration_iface);
}

static
ULONG
STDMETHODCALLTYPE
ServerWeakSource_Release(
    _In_ IRtWeakReferenceSource *iface)
{
    return ServerRegistration_Release(&impl_server_from_IRtWeakReferenceSource(iface)->IExeServerRegistration_iface);
}

static
HRESULT
STDMETHODCALLTYPE
ServerWeakSource_GetWeakReference(
    _In_ IRtWeakReferenceSource *iface,
    _Outptr_ IRtWeakReference **Reference)
{
    PRT_SERVER_REGISTRATION Server = impl_server_from_IRtWeakReferenceSource(iface);

    return RtGetWeakReference(&Server->Weak,
                              (IInspectable *)&Server->IExeServerRegistration_iface,
                              &Server->RefCount,
                              Reference);
}

static const IRtWeakReferenceSourceVtbl ServerWeakSourceVtbl =
{
    ServerWeakSource_QueryInterface,
    ServerWeakSource_AddRef,
    ServerWeakSource_Release,
    ServerWeakSource_GetWeakReference,
};

/* The launch and access permissions of a server, as SDDL like the catalog hands them out */
static
HRESULT
RtReadPermissions(
    _In_ HKEY Key,
    _Out_ HSTRING *Permissions)
{
    PWSTR Sddl = NULL;
    BYTE *Descriptor;
    DWORD Type, Size = 0;
    HRESULT Status;
    LONG Error;

    *Permissions = NULL;
    Error = RegQueryValueExW(Key, L"Permissions", NULL, &Type, NULL, &Size);
    if (Error == ERROR_FILE_NOT_FOUND)
        return S_OK;
    if (Error != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(Error);

    if (Type == REG_SZ)
    {
        Status = RtDuplicateRegistryString(Key, L"Permissions", &Sddl);
        if (SUCCEEDED(Status))
            Status = RtCreateHString(Sddl, Permissions);
        free(Sddl);
        return Status;
    }
    if (Type != REG_BINARY || !Size)
        return REGDB_E_INVALIDVALUE;

    Descriptor = malloc(Size);
    if (!Descriptor)
        return E_OUTOFMEMORY;

    Error = RegQueryValueExW(Key, L"Permissions", NULL, NULL, Descriptor, &Size);
    Status = HRESULT_FROM_WIN32(Error);
    if (SUCCEEDED(Status) &&
        !ConvertSecurityDescriptorToStringSecurityDescriptorW(Descriptor, SDDL_REVISION_1,
                                                              OWNER_SECURITY_INFORMATION |
                                                              GROUP_SECURITY_INFORMATION |
                                                              DACL_SECURITY_INFORMATION |
                                                              SACL_SECURITY_INFORMATION |
                                                              LABEL_SECURITY_INFORMATION,
                                                              &Sddl, NULL))
    {
        Status = HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(Status))
        Status = RtCreateHString(Sddl, Permissions);

    if (Sddl)
        LocalFree(Sddl);
    free(Descriptor);
    return Status;
}

static
HRESULT
RtReadServerString(
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

/* The executable of a server, in its package's install folder when registered relative to it */
static
HRESULT
RtReadServerExePath(
    _In_ HKEY Key,
    _In_opt_ PCWSTR PackageFullName,
    _Out_ HSTRING *String)
{
    PWSTR Text, PackagePath = NULL, Full;
    SIZE_T Length;
    HRESULT Status;

    *String = NULL;
    Status = RtDuplicateRegistryString(Key, L"ExePath", &Text);
    if (Status == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        return S_OK;
    if (FAILED(Status))
        return Status;

    if (PackageFullName && PackageFullName[0] && RtReadDword(Key, L"IsPackageRelativePath", 0))
    {
        Status = RtGetPackagePath(PackageFullName, &PackagePath);
        if (SUCCEEDED(Status))
        {
            Length = wcslen(PackagePath) + wcslen(Text) + 2;
            Full = malloc(Length * sizeof(WCHAR));
            if (Full)
            {
                _snwprintf(Full, Length, L"%s\\%s", PackagePath, Text);
                Full[Length - 1] = UNICODE_NULL;
                free(Text);
                Text = Full;
            }
            else
            {
                Status = E_OUTOFMEMORY;
            }
            free(PackagePath);
        }
    }

    if (SUCCEEDED(Status))
        Status = RtCreateHString(Text, String);
    free(Text);
    return Status;
}

static
HRESULT
RtCreateServerRegistration(
    _In_ PCWSTR ServerName,
    _In_ HKEY Key,
    _In_ RegistrationScope Scope,
    _In_opt_ PCWSTR PackageFullName,
    _Outptr_ IExeServerRegistration **Registration)
{
    PRT_SERVER_REGISTRATION Server;
    HRESULT Status;

    *Registration = NULL;
    Server = calloc(1, sizeof(*Server));
    if (!Server)
        return E_OUTOFMEMORY;

    Server->IExeServerRegistration_iface.lpVtbl = &ServerRegistrationVtbl;
    Server->IServerAttributeInfo_iface.lpVtbl = &ServerAttributesVtbl;
    Server->IRtWeakReferenceSource_iface.lpVtbl = &ServerWeakSourceVtbl;
    Server->RefCount = 1;

    Status = RtCreateHString(ServerName, &Server->ServerName);
    if (SUCCEEDED(Status))
        Status = RtReadServerExePath(Key, PackageFullName, &Server->ExePath);

    /* Without a command line of its own the server is started from its executable */
    if (SUCCEEDED(Status))
        Status = RtReadServerString(Key, L"CommandLine", &Server->CommandLine);
    if (SUCCEEDED(Status) && !Server->CommandLine)
        Status = WindowsDuplicateString(Server->ExePath, &Server->CommandLine);

    if (SUCCEEDED(Status))
        Status = RtReadServerString(Key, L"AppUserModelId", &Server->AppUserModelId);
    if (SUCCEEDED(Status))
        Status = RtReadPermissions(Key, &Server->Permissions);
    if (SUCCEEDED(Status))
    {
        Server->Identities = RtReadDword(Key, L"IdentityType", IdentityType_ActivateAsActivator);
        Server->Instancing = RtReadDword(Key, L"Instancing", InstancingType_SingleInstance);
        if (Server->Identities == IdentityType_RunAs && Scope == RegistrationScope_PerMachine)
            Status = RtReadServerString(Key, L"Identity", &Server->Identity);
    }
    if (SUCCEEDED(Status))
        Status = RtCreateAttributeMap(Key, L"CustomAttributes", &Server->Attributes);

    if (FAILED(Status))
    {
        ServerRegistration_Release(&Server->IExeServerRegistration_iface);
        return Status == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ? REGDB_E_INVALIDVALUE : Status;
    }

    *Registration = &Server->IExeServerRegistration_iface;
    return S_OK;
}

/**
 * @brief
 * Opens the server entry a class names, in the scope the class came from.
 */
static
HRESULT
RtOpenServerOfClass(
    _In_ PRT_CLASS_INFO Info,
    _Out_ HKEY *Key)
{
    RegistrationScope Scope;

    if (Info->Scope == RegistrationScope_PerMachine)
        return RtOpenMachineEntry(L"Server", Info->ServerName, Key);
    return RtOpenPackageEntry(Info->PackageFullName, L"Server", Info->ServerName, Key, &Scope);
}

/* CLASS REGISTRATION ********************************************************/

typedef struct _RT_CLASS_REGISTRATION
{
    IActivatableClassRegistration IActivatableClassRegistration_iface;
    IDllServerActivatableClassRegistration IDllServerActivatableClassRegistration_iface;
    IExeServerActivatableClassRegistration IExeServerActivatableClassRegistration_iface;
    IRtWeakReferenceSource IRtWeakReferenceSource_iface;
    LONG RefCount;
    PRT_WEAK_REFERENCE Weak;
    HSTRING ClassId;
    ActivationType Type;
    RegistrationScope Scope;
    TrustLevel RegisteredTrust;
    IRtMapView *Attributes;
    HSTRING DllPath;
    ThreadingType Threading;
    IExeServerRegistration *Server;
} RT_CLASS_REGISTRATION, *PRT_CLASS_REGISTRATION;

static inline PRT_CLASS_REGISTRATION impl_from_IActivatableClassRegistration(IActivatableClassRegistration *iface)
{
    return CONTAINING_RECORD(iface, RT_CLASS_REGISTRATION, IActivatableClassRegistration_iface);
}

static inline PRT_CLASS_REGISTRATION impl_from_IDllServerActivatableClassRegistration(IDllServerActivatableClassRegistration *iface)
{
    return CONTAINING_RECORD(iface, RT_CLASS_REGISTRATION, IDllServerActivatableClassRegistration_iface);
}

static inline PRT_CLASS_REGISTRATION impl_from_IExeServerActivatableClassRegistration(IExeServerActivatableClassRegistration *iface)
{
    return CONTAINING_RECORD(iface, RT_CLASS_REGISTRATION, IExeServerActivatableClassRegistration_iface);
}

static inline PRT_CLASS_REGISTRATION impl_class_from_IRtWeakReferenceSource(IRtWeakReferenceSource *iface)
{
    return CONTAINING_RECORD(iface, RT_CLASS_REGISTRATION, IRtWeakReferenceSource_iface);
}

static
HRESULT
STDMETHODCALLTYPE
ClassRegistration_QueryInterface(
    _In_ IActivatableClassRegistration *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    PRT_CLASS_REGISTRATION Class = impl_from_IActivatableClassRegistration(iface);

    if (IsEqualIID(Riid, &IID_IUnknown) ||
        IsEqualIID(Riid, &IID_IInspectable) ||
        IsEqualIID(Riid, &IID_IActivatableClassRegistration))
    {
        *Object = &Class->IActivatableClassRegistration_iface;
    }
    else if (IsEqualIID(Riid, &IID_IDllServerActivatableClassRegistration) &&
             Class->Type == ActivationType_InProcess)
    {
        *Object = &Class->IDllServerActivatableClassRegistration_iface;
    }
    else if (IsEqualIID(Riid, &IID_IExeServerActivatableClassRegistration) &&
             Class->Type == ActivationType_OutOfProcess)
    {
        *Object = &Class->IExeServerActivatableClassRegistration_iface;
    }
    else if (IsEqualIID(Riid, &IID_IRtWeakReferenceSource))
    {
        *Object = &Class->IRtWeakReferenceSource_iface;
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
ClassRegistration_AddRef(
    _In_ IActivatableClassRegistration *iface)
{
    return InterlockedIncrement(&impl_from_IActivatableClassRegistration(iface)->RefCount);
}

static
ULONG
STDMETHODCALLTYPE
ClassRegistration_Release(
    _In_ IActivatableClassRegistration *iface)
{
    PRT_CLASS_REGISTRATION Class = impl_from_IActivatableClassRegistration(iface);
    ULONG Count = InterlockedDecrement(&Class->RefCount);

    if (!Count)
    {
        RtDetachWeakReference(&Class->Weak);
        WindowsDeleteString(Class->ClassId);
        WindowsDeleteString(Class->DllPath);
        if (Class->Attributes)
            IRtMapView_Release(Class->Attributes);
        if (Class->Server)
            IExeServerRegistration_Release(Class->Server);
        free(Class);
    }
    return Count;
}

static
HRESULT
STDMETHODCALLTYPE
ClassRegistration_GetIids(
    _In_ IActivatableClassRegistration *iface,
    _Out_ ULONG *Count,
    _Outptr_ IID **Iids)
{
    static const IID *const InProcess[] =
    {
        &IID_IActivatableClassRegistration,
        &IID_IRtWeakReferenceSource,
        &IID_IDllServerActivatableClassRegistration,
    };
    static const IID *const OutOfProcess[] =
    {
        &IID_IActivatableClassRegistration,
        &IID_IRtWeakReferenceSource,
        &IID_IExeServerActivatableClassRegistration,
    };

    if (impl_from_IActivatableClassRegistration(iface)->Type == ActivationType_InProcess)
        return RtCopyIids(ARRAYSIZE(InProcess), InProcess, Count, Iids);
    return RtCopyIids(ARRAYSIZE(OutOfProcess), OutOfProcess, Count, Iids);
}

static
HRESULT
STDMETHODCALLTYPE
ClassRegistration_GetRuntimeClassName(
    _In_ IActivatableClassRegistration *iface,
    _Out_ HSTRING *Name)
{
    *Name = NULL;
    RoOriginateError(E_NOTIMPL, NULL);
    return E_NOTIMPL;
}

static
HRESULT
STDMETHODCALLTYPE
ClassRegistration_GetTrustLevel(
    _In_ IActivatableClassRegistration *iface,
    _Out_ TrustLevel *Level)
{
    RoOriginateError(E_NOTIMPL, NULL);
    return E_NOTIMPL;
}

static
HRESULT
STDMETHODCALLTYPE
ClassRegistration_get_ActivatableClassId(
    _In_ IActivatableClassRegistration *iface,
    _Out_ HSTRING *ClassId)
{
    *ClassId = NULL;
    WindowsDuplicateString(impl_from_IActivatableClassRegistration(iface)->ClassId, ClassId);
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ClassRegistration_get_ActivationType(
    _In_ IActivatableClassRegistration *iface,
    _Out_ ActivationType *Type)
{
    *Type = impl_from_IActivatableClassRegistration(iface)->Type;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ClassRegistration_get_RegistrationScope(
    _In_ IActivatableClassRegistration *iface,
    _Out_ RegistrationScope *Scope)
{
    *Scope = impl_from_IActivatableClassRegistration(iface)->Scope;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ClassRegistration_get_RegisteredTrustLevel(
    _In_ IActivatableClassRegistration *iface,
    _Out_ TrustLevel *Level)
{
    /* Windows never fills this in, so every registration reports base trust */
    *Level = impl_from_IActivatableClassRegistration(iface)->RegisteredTrust;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ClassRegistration_get_Attributes(
    _In_ IActivatableClassRegistration *iface,
    _Outptr_ IRtMapView **Attributes)
{
    *Attributes = impl_from_IActivatableClassRegistration(iface)->Attributes;
    IRtMapView_AddRef(*Attributes);
    return S_OK;
}

static const IActivatableClassRegistrationVtbl ClassRegistrationVtbl =
{
    ClassRegistration_QueryInterface,
    ClassRegistration_AddRef,
    ClassRegistration_Release,
    ClassRegistration_GetIids,
    ClassRegistration_GetRuntimeClassName,
    ClassRegistration_GetTrustLevel,
    ClassRegistration_get_ActivatableClassId,
    ClassRegistration_get_ActivationType,
    ClassRegistration_get_RegistrationScope,
    ClassRegistration_get_RegisteredTrustLevel,
    ClassRegistration_get_Attributes,
};

static
HRESULT
STDMETHODCALLTYPE
DllRegistration_QueryInterface(
    _In_ IDllServerActivatableClassRegistration *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    return ClassRegistration_QueryInterface(&impl_from_IDllServerActivatableClassRegistration(iface)->IActivatableClassRegistration_iface, Riid, Object);
}

static
ULONG
STDMETHODCALLTYPE
DllRegistration_AddRef(
    _In_ IDllServerActivatableClassRegistration *iface)
{
    return ClassRegistration_AddRef(&impl_from_IDllServerActivatableClassRegistration(iface)->IActivatableClassRegistration_iface);
}

static
ULONG
STDMETHODCALLTYPE
DllRegistration_Release(
    _In_ IDllServerActivatableClassRegistration *iface)
{
    return ClassRegistration_Release(&impl_from_IDllServerActivatableClassRegistration(iface)->IActivatableClassRegistration_iface);
}

static
HRESULT
STDMETHODCALLTYPE
DllRegistration_GetIids(
    _In_ IDllServerActivatableClassRegistration *iface,
    _Out_ ULONG *Count,
    _Outptr_ IID **Iids)
{
    return ClassRegistration_GetIids(&impl_from_IDllServerActivatableClassRegistration(iface)->IActivatableClassRegistration_iface, Count, Iids);
}

static
HRESULT
STDMETHODCALLTYPE
DllRegistration_GetRuntimeClassName(
    _In_ IDllServerActivatableClassRegistration *iface,
    _Out_ HSTRING *Name)
{
    return ClassRegistration_GetRuntimeClassName(&impl_from_IDllServerActivatableClassRegistration(iface)->IActivatableClassRegistration_iface, Name);
}

static
HRESULT
STDMETHODCALLTYPE
DllRegistration_GetTrustLevel(
    _In_ IDllServerActivatableClassRegistration *iface,
    _Out_ TrustLevel *Level)
{
    return ClassRegistration_GetTrustLevel(&impl_from_IDllServerActivatableClassRegistration(iface)->IActivatableClassRegistration_iface, Level);
}

static
HRESULT
STDMETHODCALLTYPE
DllRegistration_get_DllPath(
    _In_ IDllServerActivatableClassRegistration *iface,
    _Out_ HSTRING *DllPath)
{
    *DllPath = NULL;
    WindowsDuplicateString(impl_from_IDllServerActivatableClassRegistration(iface)->DllPath, DllPath);
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
DllRegistration_get_ThreadingType(
    _In_ IDllServerActivatableClassRegistration *iface,
    _Out_ ThreadingType *Threading)
{
    *Threading = impl_from_IDllServerActivatableClassRegistration(iface)->Threading;
    return S_OK;
}

static const IDllServerActivatableClassRegistrationVtbl DllRegistrationVtbl =
{
    DllRegistration_QueryInterface,
    DllRegistration_AddRef,
    DllRegistration_Release,
    DllRegistration_GetIids,
    DllRegistration_GetRuntimeClassName,
    DllRegistration_GetTrustLevel,
    DllRegistration_get_DllPath,
    DllRegistration_get_ThreadingType,
};

static
HRESULT
STDMETHODCALLTYPE
ExeRegistration_QueryInterface(
    _In_ IExeServerActivatableClassRegistration *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    return ClassRegistration_QueryInterface(&impl_from_IExeServerActivatableClassRegistration(iface)->IActivatableClassRegistration_iface, Riid, Object);
}

static
ULONG
STDMETHODCALLTYPE
ExeRegistration_AddRef(
    _In_ IExeServerActivatableClassRegistration *iface)
{
    return ClassRegistration_AddRef(&impl_from_IExeServerActivatableClassRegistration(iface)->IActivatableClassRegistration_iface);
}

static
ULONG
STDMETHODCALLTYPE
ExeRegistration_Release(
    _In_ IExeServerActivatableClassRegistration *iface)
{
    return ClassRegistration_Release(&impl_from_IExeServerActivatableClassRegistration(iface)->IActivatableClassRegistration_iface);
}

static
HRESULT
STDMETHODCALLTYPE
ExeRegistration_GetIids(
    _In_ IExeServerActivatableClassRegistration *iface,
    _Out_ ULONG *Count,
    _Outptr_ IID **Iids)
{
    return ClassRegistration_GetIids(&impl_from_IExeServerActivatableClassRegistration(iface)->IActivatableClassRegistration_iface, Count, Iids);
}

static
HRESULT
STDMETHODCALLTYPE
ExeRegistration_GetRuntimeClassName(
    _In_ IExeServerActivatableClassRegistration *iface,
    _Out_ HSTRING *Name)
{
    return ClassRegistration_GetRuntimeClassName(&impl_from_IExeServerActivatableClassRegistration(iface)->IActivatableClassRegistration_iface, Name);
}

static
HRESULT
STDMETHODCALLTYPE
ExeRegistration_GetTrustLevel(
    _In_ IExeServerActivatableClassRegistration *iface,
    _Out_ TrustLevel *Level)
{
    return ClassRegistration_GetTrustLevel(&impl_from_IExeServerActivatableClassRegistration(iface)->IActivatableClassRegistration_iface, Level);
}

static
HRESULT
STDMETHODCALLTYPE
ExeRegistration_get_ServerRegistration(
    _In_ IExeServerActivatableClassRegistration *iface,
    _Outptr_ IExeServerRegistration **Server)
{
    PRT_CLASS_REGISTRATION Class = impl_from_IExeServerActivatableClassRegistration(iface);

    *Server = NULL;
    return IExeServerRegistration_QueryInterface(Class->Server, &IID_IExeServerRegistration, (void **)Server);
}

static const IExeServerActivatableClassRegistrationVtbl ExeRegistrationVtbl =
{
    ExeRegistration_QueryInterface,
    ExeRegistration_AddRef,
    ExeRegistration_Release,
    ExeRegistration_GetIids,
    ExeRegistration_GetRuntimeClassName,
    ExeRegistration_GetTrustLevel,
    ExeRegistration_get_ServerRegistration,
};

static
HRESULT
STDMETHODCALLTYPE
ClassWeakSource_QueryInterface(
    _In_ IRtWeakReferenceSource *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    return ClassRegistration_QueryInterface(&impl_class_from_IRtWeakReferenceSource(iface)->IActivatableClassRegistration_iface, Riid, Object);
}

static
ULONG
STDMETHODCALLTYPE
ClassWeakSource_AddRef(
    _In_ IRtWeakReferenceSource *iface)
{
    return ClassRegistration_AddRef(&impl_class_from_IRtWeakReferenceSource(iface)->IActivatableClassRegistration_iface);
}

static
ULONG
STDMETHODCALLTYPE
ClassWeakSource_Release(
    _In_ IRtWeakReferenceSource *iface)
{
    return ClassRegistration_Release(&impl_class_from_IRtWeakReferenceSource(iface)->IActivatableClassRegistration_iface);
}

static
HRESULT
STDMETHODCALLTYPE
ClassWeakSource_GetWeakReference(
    _In_ IRtWeakReferenceSource *iface,
    _Outptr_ IRtWeakReference **Reference)
{
    PRT_CLASS_REGISTRATION Class = impl_class_from_IRtWeakReferenceSource(iface);

    return RtGetWeakReference(&Class->Weak,
                              (IInspectable *)&Class->IActivatableClassRegistration_iface,
                              &Class->RefCount,
                              Reference);
}

static const IRtWeakReferenceSourceVtbl ClassWeakSourceVtbl =
{
    ClassWeakSource_QueryInterface,
    ClassWeakSource_AddRef,
    ClassWeakSource_Release,
    ClassWeakSource_GetWeakReference,
};

/**
 * @brief
 * Builds the registration object of a class found by RtFindClass.
 *
 * @return
 * REGDB_E_INVALIDVALUE for remote classes or incomplete registrations.
 */
HRESULT
RtCreateClassRegistration(
    _In_ PRT_CLASS_INFO Info,
    _Outptr_ IActivatableClassRegistration **Registration)
{
    PRT_CLASS_REGISTRATION Class;
    HKEY ServerKey = NULL;
    HRESULT Status;

    *Registration = NULL;
    if (Info->Type != ActivationType_InProcess && Info->Type != ActivationType_OutOfProcess)
        return REGDB_E_INVALIDVALUE;

    Class = calloc(1, sizeof(*Class));
    if (!Class)
        return E_OUTOFMEMORY;

    Class->IActivatableClassRegistration_iface.lpVtbl = &ClassRegistrationVtbl;
    Class->IDllServerActivatableClassRegistration_iface.lpVtbl = &DllRegistrationVtbl;
    Class->IExeServerActivatableClassRegistration_iface.lpVtbl = &ExeRegistrationVtbl;
    Class->IRtWeakReferenceSource_iface.lpVtbl = &ClassWeakSourceVtbl;
    Class->RefCount = 1;
    Class->Type = Info->Type;
    Class->Scope = Info->Scope;
    Class->RegisteredTrust = BaseTrust;
    Class->Threading = Info->Threading;

    Status = WindowsDuplicateString(Info->ClassId, &Class->ClassId);
    if (SUCCEEDED(Status))
        Status = RtCreateAttributeMap(Info->ClassKey, L"CustomAttributes", &Class->Attributes);

    if (SUCCEEDED(Status) && Info->Type == ActivationType_InProcess)
        Status = RtCreateHString(Info->DllPath, &Class->DllPath);

    if (SUCCEEDED(Status) && Info->Type == ActivationType_OutOfProcess)
    {
        Status = RtOpenServerOfClass(Info, &ServerKey);
        if (SUCCEEDED(Status))
            Status = RtCreateServerRegistration(Info->ServerName, ServerKey, Info->Scope,
                                                Info->PackageFullName, &Class->Server);
        else
            Status = REGDB_E_INVALIDVALUE;
        if (ServerKey)
            RegCloseKey(ServerKey);
    }

    if (FAILED(Status))
    {
        ClassRegistration_Release(&Class->IActivatableClassRegistration_iface);
        return Status;
    }

    *Registration = &Class->IActivatableClassRegistration_iface;
    return S_OK;
}

/* EXPORTS *******************************************************************/

/***********************************************************************
 *      RoGetActivatableClassRegistration (combase.@)
 */
HRESULT
WINAPI
RoGetActivatableClassRegistration(
    _In_ HSTRING ClassId,
    _Outptr_ IActivatableClassRegistration **Registration)
{
    RT_CLASS_INFO Info;
    HRESULT Status;

    TRACE("(%s, %p)\n", debugstr_hstring(ClassId), Registration);

    *Registration = NULL;
    Status = RtValidateName(ClassId);
    if (FAILED(Status))
        return Status;

    Status = RtFindClass(ClassId, NULL, &Info);
    if (FAILED(Status))
        return Status;

    Status = RtCreateClassRegistration(&Info, Registration);
    RtFreeClassInfo(&Info);
    return Status;
}

/***********************************************************************
 *      RoGetServerActivatableClasses (combase.@)
 */
HRESULT
WINAPI
RoGetServerActivatableClasses(
    _In_ HSTRING ServerName,
    _Outptr_result_buffer_maybenull_(*Count) HSTRING **ClassIds,
    _Out_ DWORD *Count)
{
    WCHAR Package[RT_PACKAGE_FULL_NAME_LENGTH];
    RegistrationScope Scope;
    HSTRING *Array;
    PWSTR List = NULL, Cursor;
    DWORD Type, Size = 0, Found = 0, Index;
    HKEY Key;
    HRESULT Status;
    LONG Error;

    TRACE("(%s, %p, %p)\n", debugstr_hstring(ServerName), ClassIds, Count);

    *ClassIds = NULL;
    *Count = 0;
    if (FAILED(RtValidateName(ServerName)))
    {
        RoOriginateError(E_INVALIDARG, NULL);
        return E_INVALIDARG;
    }

    Status = RtLocateEntry(L"Server", WindowsGetStringRawBuffer(ServerName, NULL), NULL, Package, &Key, &Scope);
    if (FAILED(Status))
        return REGDB_E_CLASSNOTREG;

    Error = RegQueryValueExW(Key, L"ActivatableClasses", NULL, &Type, NULL, &Size);
    if (Error == ERROR_SUCCESS && Type == REG_MULTI_SZ && Size >= 2 * sizeof(WCHAR))
    {
        List = calloc(1, Size + 2 * sizeof(WCHAR));
        if (!List)
        {
            RegCloseKey(Key);
            return E_OUTOFMEMORY;
        }
        Error = RegQueryValueExW(Key, L"ActivatableClasses", NULL, NULL, (BYTE *)List, &Size);
    }
    RegCloseKey(Key);

    if (!List || Error != ERROR_SUCCESS)
    {
        free(List);
        return S_OK;
    }

    for (Cursor = List; *Cursor; Cursor += wcslen(Cursor) + 1)
        Found++;
    if (!Found)
    {
        free(List);
        return S_OK;
    }

    Array = CoTaskMemAlloc(Found * sizeof(*Array));
    if (!Array)
    {
        free(List);
        RoOriginateError(E_OUTOFMEMORY, NULL);
        return E_OUTOFMEMORY;
    }

    for (Index = 0, Cursor = List; Index < Found; Index++, Cursor += wcslen(Cursor) + 1)
    {
        Status = WindowsCreateString(Cursor, (UINT32)wcslen(Cursor), &Array[Index]);
        if (FAILED(Status))
        {
            while (Index--)
                WindowsDeleteString(Array[Index]);
            CoTaskMemFree(Array);
            free(List);
            return Status;
        }
    }

    free(List);
    *ClassIds = Array;
    *Count = Found;
    return S_OK;
}
