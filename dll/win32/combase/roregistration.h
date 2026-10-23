/*
 * PROJECT:     ReactOS COM Base
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Windows Runtime class catalog, server registration and activation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <roapi.h>
#include "rtinterfaces.h"

/* Longest activatable class id or server name the catalog accepts */
#define RT_MAX_NAME_LENGTH 256

/* Package full names are at most 127 characters */
#define RT_PACKAGE_FULL_NAME_LENGTH 128

/* Where a class was found and what activating it needs */
typedef struct _RT_CLASS_INFO
{
    HSTRING ClassId;
    RegistrationScope Scope;
    ActivationType Type;
    HKEY ClassKey;
    WCHAR PackageFullName[RT_PACKAGE_FULL_NAME_LENGTH];
    PWSTR DllPath;
    ThreadingType Threading;
    PWSTR ServerName;
} RT_CLASS_INFO, *PRT_CLASS_INFO;

/* What an activation through an extension entry hands the server launcher */
typedef struct _RT_LAUNCH_CONTEXT
{
    UINT64 HostId;
    UINT64 UserContext;
    UINT64 ActivationId;
} RT_LAUNCH_CONTEXT, *PRT_LAUNCH_CONTEXT;

/* Catalog */

FARPROC
RtGetAppModelExport(
    _In_ PCSTR Name);

HRESULT
RtValidateName(
    _In_opt_ HSTRING Name);

HRESULT
RtGetCurrentPackageFullName(
    _Out_writes_(RT_PACKAGE_FULL_NAME_LENGTH) PWSTR Buffer);

HRESULT
RtGetPackagePath(
    _In_ PCWSTR PackageFullName,
    _Outptr_ PWSTR *Path);

HRESULT
RtFindClass(
    _In_ HSTRING ClassId,
    _In_opt_ PCWSTR PackageFullName,
    _Out_ PRT_CLASS_INFO Info);

VOID
RtFreeClassInfo(
    _Inout_ PRT_CLASS_INFO Info);

HRESULT
RtCreateAttributeMap(
    _In_opt_ HKEY Parent,
    _In_ PCWSTR SubKey,
    _Outptr_ IRtMapView **Map);

HRESULT
RtCreateClassRegistration(
    _In_ PRT_CLASS_INFO Info,
    _Outptr_ IActivatableClassRegistration **Registration);

/* Activation */

HRESULT
RtGetInProcessFactory(
    _In_ PRT_CLASS_INFO Info,
    _In_ REFIID Riid,
    _Outptr_ void **Factory);

HRESULT
RtGetOutOfProcessFactory(
    _In_ PRT_CLASS_INFO Info,
    _In_opt_ const RT_LAUNCH_CONTEXT *Context,
    _In_ REFIID Riid,
    _Outptr_ void **Factory);

HRESULT
RtGetClassFactory(
    _In_ PRT_CLASS_INFO Info,
    _In_opt_ const RT_LAUNCH_CONTEXT *Context,
    _In_ REFIID Riid,
    _Outptr_ void **Factory);

/* Server process lifetime, called by CoAddRefServerProcess and CoReleaseServerProcess */

VOID
RtServerProcessReferenced(VOID);

VOID
RtServerProcessUnreferenced(VOID);

/* Marshaling by value */

extern const CLSID CLSID_RtMarshalByValue;

BOOL
RtUseMarshalByValue(
    _In_ IUnknown *Object,
    _In_ DWORD DestContext,
    _In_ DWORD Flags,
    _Outptr_result_maybenull_ IPropertySetMarshal **Marshal);

HRESULT
RtGetMarshalByValueSize(
    _In_ REFIID Riid,
    _In_ IUnknown *Object,
    _In_ IPropertySetMarshal *Marshal,
    _Out_ ULONG *Size);

HRESULT
RtMarshalByValue(
    _In_ IStream *Stream,
    _In_ REFIID Riid,
    _In_ IUnknown *Object,
    _In_ IPropertySetMarshal *Marshal);

HRESULT
RtUnmarshalByValue(
    _In_ IStream *Stream,
    _In_ REFIID Riid,
    _Outptr_ void **Object);

/* Shared object helpers */

typedef struct _RT_WEAK_REFERENCE *PRT_WEAK_REFERENCE;

HRESULT
RtGetWeakReference(
    _Inout_ PRT_WEAK_REFERENCE *Slot,
    _In_ IInspectable *Target,
    _In_ LONG *StrongCount,
    _Outptr_ IRtWeakReference **Reference);

VOID
RtDetachWeakReference(
    _Inout_ PRT_WEAK_REFERENCE *Slot);

HRESULT
RtCopyIids(
    _In_ ULONG Count,
    _In_reads_(Count) const IID *const *Source,
    _Out_ ULONG *IidCount,
    _Outptr_ IID **Iids);

HRESULT
RtDuplicateRegistryString(
    _In_ HKEY Key,
    _In_ PCWSTR Value,
    _Outptr_result_maybenull_ PWSTR *Text);

HRESULT
RtCreateHString(
    _In_opt_ PCWSTR Text,
    _Out_ HSTRING *String);

/* Exported functions without a public declaration */

void
WINAPI
RoRevokeActivationFactories(
    _In_ RO_REGISTRATION_COOKIE Cookie);
