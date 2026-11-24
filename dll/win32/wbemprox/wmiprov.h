/*
 * PROJECT:     ReactOS WBEM Proxy
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Kernel WMI data block provider for the root\wmi namespace
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <wmistr.h>
#include <wmium.h>

/* Class flags */
#define WMI_CLASS_ABSTRACT      0x00000001
#define WMI_CLASS_WMI           0x00000002
#define WMI_CLASS_DYNAMIC       0x00000004
#define WMI_CLASS_HAS_GUID      0x00000008
#define WMI_CLASS_EVENT         0x00000010
#define WMI_CLASS_TRACE         0x00000020
#define WMI_CLASS_RESOLVED      0x00000040
#define WMI_CLASS_RESOLVING     0x00000080
#define WMI_CLASS_PARAMETERS    0x00000100
#define WMI_CLASS_BUILTIN       0x00000200

/* Property and parameter flags */
#define WMI_ITEM_KEY            0x00000001
#define WMI_ITEM_READ           0x00000002
#define WMI_ITEM_WRITE          0x00000004
#define WMI_ITEM_IN             0x00000008
#define WMI_ITEM_OUT            0x00000010

/* Upper bound for WmiSizeIs counts, guards against corrupt blocks */
#define WMI_MAX_ARRAY_COUNT     0x00100000

typedef struct _WMI_QUALIFIER_DEF
{
    PWSTR Name;
    LONG Flavor;
    VARIANT Value;
} WMI_QUALIFIER_DEF, *PWMI_QUALIFIER_DEF;

struct _WMI_CLASS_DEF;

typedef struct _WMI_PROPERTY_DEF
{
    PWSTR Name;
    CIMTYPE Type;
    PWSTR ObjectClass;
    struct _WMI_CLASS_DEF *Embedded;
    ULONG DataId;
    ULONG FixedCount;
    PWSTR SizeIs;
    ULONG Flags;
    ULONG QualifierCount;
    PWMI_QUALIFIER_DEF Qualifiers;
} WMI_PROPERTY_DEF, *PWMI_PROPERTY_DEF;

typedef struct _WMI_METHOD_DEF
{
    PWSTR Name;
    ULONG MethodId;
    ULONG QualifierCount;
    PWMI_QUALIFIER_DEF Qualifiers;
    struct _WMI_CLASS_DEF *InParams;
    struct _WMI_CLASS_DEF *OutParams;
} WMI_METHOD_DEF, *PWMI_METHOD_DEF;

typedef struct _WMI_CLASS_DEF
{
    PWSTR Name;
    PWSTR SuperName;
    struct _WMI_CLASS_DEF *Super;
    GUID Guid;
    ULONG Flags;
    ULONG QualifierCount;
    PWMI_QUALIFIER_DEF Qualifiers;
    ULONG PropertyCount;
    PWMI_PROPERTY_DEF Properties;
    ULONG MethodCount;
    PWMI_METHOD_DEF Methods;

    /* Filled by WmiBlockResolveClass */
    ULONG AllCount;
    PWMI_PROPERTY_DEF *All;
    ULONG ItemCount;
    PWMI_PROPERTY_DEF *Items;
    ULONG Alignment;
} WMI_CLASS_DEF, *PWMI_CLASS_DEF;

typedef
VOID
(*PBMOF_CLASS_ROUTINE)(
    _In_opt_ PVOID Context,
    _In_ PWMI_CLASS_DEF Class);

typedef
PWMI_CLASS_DEF
(*PWMI_CLASS_LOOKUP)(
    _In_opt_ PVOID Context,
    _In_ PCWSTR Name);

typedef
BOOLEAN
(*PWMI_INSTANCE_ROUTINE)(
    _In_opt_ PVOID Context,
    _In_reads_opt_(NameLength) PCWSTR Name,
    _In_ ULONG NameLength,
    _In_ ULONG Index,
    _In_reads_bytes_(DataSize) const BYTE *Data,
    _In_ ULONG DataSize);

/* bmof.c */
HRESULT
BmofDecompress(
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ ULONG Size,
    _Outptr_result_bytebuffer_(*OutSize) BYTE **Out,
    _Out_ ULONG *OutSize);

HRESULT
BmofParse(
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ ULONG Size,
    _In_ PBMOF_CLASS_ROUTINE Routine,
    _In_opt_ PVOID Context);

VOID
BmofFreeClass(
    _In_opt_ PWMI_CLASS_DEF Class);

VOID
BmofFreeQualifiers(
    _In_ ULONG Count,
    _In_opt_ PWMI_QUALIFIER_DEF Qualifiers);

PWSTR
BmofStrDup(
    _In_opt_ PCWSTR Source);

PVOID
BmofAlloc(
    _In_ SIZE_T Size);

VOID
BmofFree(
    _In_opt_ PVOID Block);

BOOLEAN
BmofParseGuid(
    _In_ PCWSTR Text,
    _Out_ GUID *Guid);

const WMI_QUALIFIER_DEF *
BmofFindQualifier(
    _In_ ULONG Count,
    _In_reads_(Count) const WMI_QUALIFIER_DEF *Qualifiers,
    _In_ PCWSTR Name);

/* wmiblock.c */
HRESULT
WmiBlockResolveClass(
    _Inout_ PWMI_CLASS_DEF Class,
    _In_ PWMI_CLASS_LOOKUP Lookup,
    _In_opt_ PVOID Context);

HRESULT
WmiBlockDecode(
    _In_ const WMI_CLASS_DEF *Class,
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ ULONG Size,
    _Out_writes_(Class->ItemCount) VARIANT *Values);

HRESULT
WmiBlockEncode(
    _In_ const WMI_CLASS_DEF *Class,
    _In_reads_(Class->ItemCount) const VARIANT *Values,
    _Outptr_result_bytebuffer_(*Size) BYTE **Data,
    _Out_ ULONG *Size);

ULONG
WmiBlockWalk(
    _In_reads_bytes_(Size) const BYTE *Buffer,
    _In_ ULONG Size,
    _In_ PWMI_INSTANCE_ROUTINE Routine,
    _In_opt_ PVOID Context);

VARTYPE
WmiBlockVarType(
    _In_ CIMTYPE Type);

VOID
WmiBlockClearValues(
    _In_ ULONG Count,
    _Inout_updates_(Count) VARIANT *Values);

LONG
WmiBlockFindItem(
    _In_ const WMI_CLASS_DEF *Class,
    _In_ PCWSTR Name);

/* wmistd.c */
VOID
WmiStdEnumerateClasses(
    _In_ PBMOF_CLASS_ROUTINE Routine,
    _In_opt_ PVOID Context);
