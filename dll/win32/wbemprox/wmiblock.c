/*
 * PROJECT:     ReactOS WBEM Proxy
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     WMI data block layout, decoding and encoding
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#define COBJMACROS

#include <stdarg.h>
#include <wchar.h>

#include <windef.h>
#include <winbase.h>
#include <objbase.h>
#include <oleauto.h>
#include <wbemcli.h>

#include "wmiprov.h"

#define WMI_DATETIME_CHARS      25
#define WMI_MAX_EMBED_DEPTH     8

#define WMI_ALIGN_UP(Value, Alignment) (((Value) + (Alignment) - 1) & ~((Alignment) - 1))

typedef struct _WMI_OUTPUT
{
    BYTE *Data;
    ULONG Size;
    ULONG Capacity;
} WMI_OUTPUT, *PWMI_OUTPUT;

static
HRESULT
WmiBlockDecodeClass(
    _In_ const WMI_CLASS_DEF *Class,
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ ULONG Size,
    _Inout_ PULONG Offset,
    _In_ ULONG Depth,
    _Out_writes_(Class->ItemCount) VARIANT *Values);

static
HRESULT
WmiBlockEncodeClass(
    _In_ const WMI_CLASS_DEF *Class,
    _In_reads_opt_(Class->ItemCount) const VARIANT *Values,
    _In_ ULONG Depth,
    _Inout_ PWMI_OUTPUT Output);

VARTYPE
WmiBlockVarType(
    _In_ CIMTYPE Type)
{
    switch (Type & ~CIM_FLAG_ARRAY)
    {
        case CIM_BOOLEAN: return VT_BOOL;
        case CIM_SINT8: return VT_I1;
        case CIM_UINT8: return VT_UI1;
        case CIM_SINT16: return VT_I2;
        case CIM_UINT16:
        case CIM_CHAR16: return VT_UI2;
        case CIM_SINT32: return VT_I4;
        case CIM_UINT32: return VT_UI4;
        case CIM_SINT64: return VT_I8;
        case CIM_UINT64: return VT_UI8;
        case CIM_REAL32: return VT_R4;
        case CIM_REAL64: return VT_R8;
        case CIM_STRING:
        case CIM_DATETIME:
        case CIM_REFERENCE: return VT_BSTR;
        case CIM_OBJECT: return VT_VARIANT;
        default: return VT_EMPTY;
    }
}

/* Size of one element as it appears in a data block, zero for variable sized types */
static
ULONG
WmiBlockScalarSize(
    _In_ CIMTYPE Type)
{
    switch (Type & ~CIM_FLAG_ARRAY)
    {
        case CIM_BOOLEAN:
        case CIM_SINT8:
        case CIM_UINT8: return 1;
        case CIM_SINT16:
        case CIM_UINT16:
        case CIM_CHAR16: return 2;
        case CIM_SINT32:
        case CIM_UINT32:
        case CIM_REAL32: return 4;
        case CIM_SINT64:
        case CIM_UINT64:
        case CIM_REAL64: return 8;
        default: return 0;
    }
}

static
ULONG
WmiBlockItemAlignment(
    _In_ const WMI_PROPERTY_DEF *Item)
{
    ULONG Size;

    switch (Item->Type & ~CIM_FLAG_ARRAY)
    {
        case CIM_STRING:
        case CIM_DATETIME:
        case CIM_REFERENCE:
            return sizeof(USHORT);
        case CIM_OBJECT:
            return (Item->Embedded && Item->Embedded->Alignment) ? Item->Embedded->Alignment : 1;
    }

    Size = WmiBlockScalarSize(Item->Type);
    return Size ? Size : 1;
}

LONG
WmiBlockFindItem(
    _In_ const WMI_CLASS_DEF *Class,
    _In_ PCWSTR Name)
{
    ULONG Index;

    for (Index = 0; Index < Class->ItemCount; Index++)
    {
        if (!_wcsicmp(Class->Items[Index]->Name, Name))
            return (LONG)Index;
    }
    return -1;
}

static
int
__cdecl
WmiBlockCompareItems(
    _In_ const void *Left,
    _In_ const void *Right)
{
    const WMI_PROPERTY_DEF *A = *(const WMI_PROPERTY_DEF * const *)Left;
    const WMI_PROPERTY_DEF *B = *(const WMI_PROPERTY_DEF * const *)Right;

    if (A->DataId < B->DataId)
        return -1;
    return (A->DataId > B->DataId) ? 1 : 0;
}

/* Builds the data item list of a class or parameter set and resolves embedded classes */
static
HRESULT
WmiBlockBuildItems(
    _Inout_ PWMI_CLASS_DEF Class,
    _In_ PWMI_CLASS_LOOKUP Lookup,
    _In_opt_ PVOID Context)
{
    ULONG Index;
    HRESULT hr;

    BmofFree(Class->Items);
    Class->Items = NULL;
    Class->ItemCount = 0;
    Class->Alignment = 1;

    if (!Class->AllCount)
        return S_OK;

    Class->Items = BmofAlloc(Class->AllCount * sizeof(*Class->Items));
    if (!Class->Items)
        return E_OUTOFMEMORY;

    for (Index = 0; Index < Class->AllCount; Index++)
    {
        PWMI_PROPERTY_DEF Property = Class->All[Index];

        if (!Property->DataId)
            continue;

        if ((Property->Type & ~CIM_FLAG_ARRAY) == CIM_OBJECT && Property->ObjectClass && !Property->Embedded)
        {
            PWMI_CLASS_DEF Embedded = Lookup(Context, Property->ObjectClass);

            if (Embedded && !(Embedded->Flags & WMI_CLASS_RESOLVING))
            {
                hr = WmiBlockResolveClass(Embedded, Lookup, Context);
                if (SUCCEEDED(hr))
                    Property->Embedded = Embedded;
            }
        }
        Class->Items[Class->ItemCount++] = Property;
    }

    qsort(Class->Items, Class->ItemCount, sizeof(*Class->Items), WmiBlockCompareItems);

    for (Index = 0; Index < Class->ItemCount; Index++)
        Class->Alignment = max(Class->Alignment, WmiBlockItemAlignment(Class->Items[Index]));
    return S_OK;
}

static
HRESULT
WmiBlockResolveParameters(
    _Inout_opt_ PWMI_CLASS_DEF Params,
    _In_ PWMI_CLASS_LOOKUP Lookup,
    _In_opt_ PVOID Context)
{
    ULONG Index;

    if (!Params || (Params->Flags & WMI_CLASS_RESOLVED))
        return S_OK;

    BmofFree(Params->All);
    Params->All = NULL;
    Params->AllCount = 0;
    if (Params->PropertyCount)
    {
        Params->All = BmofAlloc(Params->PropertyCount * sizeof(*Params->All));
        if (!Params->All)
            return E_OUTOFMEMORY;
    }

    for (Index = 0; Index < Params->PropertyCount; Index++)
    {
        PWMI_PROPERTY_DEF Param = &Params->Properties[Index];

        /* A return value is not part of a WDM method buffer */
        if (!_wcsicmp(Param->Name, L"ReturnValue") &&
            !BmofFindQualifier(Param->QualifierCount, Param->Qualifiers, L"WmiDataId"))
        {
            Param->DataId = 0;
        }
        Params->All[Params->AllCount++] = Param;
    }

    Params->Flags |= WMI_CLASS_RESOLVED;
    return WmiBlockBuildItems(Params, Lookup, Context);
}

/**
 * @brief Links a class to its ancestors and computes its data item layout.
 */
HRESULT
WmiBlockResolveClass(
    _Inout_ PWMI_CLASS_DEF Class,
    _In_ PWMI_CLASS_LOOKUP Lookup,
    _In_opt_ PVOID Context)
{
    PWMI_CLASS_DEF Super = NULL;
    ULONG Index, Check;
    HRESULT hr;

    if (Class->Flags & WMI_CLASS_RESOLVED)
        return S_OK;
    if (Class->Flags & WMI_CLASS_RESOLVING)
        return WBEM_E_CIRCULAR_REFERENCE;

    Class->Flags |= WMI_CLASS_RESOLVING;

    if (!_wcsicmp(Class->Name, L"WMIEvent"))
        Class->Flags |= WMI_CLASS_EVENT;
    if (!_wcsicmp(Class->Name, L"EventTrace"))
        Class->Flags |= WMI_CLASS_TRACE;

    if (Class->SuperName)
    {
        if (!_wcsicmp(Class->SuperName, L"WMIEvent"))
            Class->Flags |= WMI_CLASS_EVENT;
        else if (!_wcsicmp(Class->SuperName, L"EventTrace"))
            Class->Flags |= WMI_CLASS_TRACE;

        Super = Lookup(Context, Class->SuperName);
        if (Super == Class)
            Super = NULL;
        if (Super && FAILED(WmiBlockResolveClass(Super, Lookup, Context)))
            Super = NULL;
    }

    Class->Super = Super;
    if (Super)
        Class->Flags |= (Super->Flags & (WMI_CLASS_EVENT | WMI_CLASS_TRACE));

    Class->AllCount = 0;
    Class->All = BmofAlloc(((Super ? Super->AllCount : 0) + Class->PropertyCount + 1) * sizeof(*Class->All));
    if (!Class->All)
    {
        Class->Flags &= ~WMI_CLASS_RESOLVING;
        return E_OUTOFMEMORY;
    }

    if (Super)
    {
        memcpy(Class->All, Super->All, Super->AllCount * sizeof(*Class->All));
        Class->AllCount = Super->AllCount;
    }

    /* Own definitions override inherited ones of the same name */
    for (Index = 0; Index < Class->PropertyCount; Index++)
    {
        PWMI_PROPERTY_DEF Property = &Class->Properties[Index];
        BOOLEAN IsOverride = FALSE;

        for (Check = 0; Check < Class->AllCount; Check++)
        {
            if (!_wcsicmp(Class->All[Check]->Name, Property->Name))
            {
                Class->All[Check] = Property;
                IsOverride = TRUE;
                break;
            }
        }
        if (!IsOverride)
            Class->All[Class->AllCount++] = Property;
    }

    hr = WmiBlockBuildItems(Class, Lookup, Context);

    for (Index = 0; SUCCEEDED(hr) && Index < Class->MethodCount; Index++)
    {
        hr = WmiBlockResolveParameters(Class->Methods[Index].InParams, Lookup, Context);
        if (SUCCEEDED(hr))
            hr = WmiBlockResolveParameters(Class->Methods[Index].OutParams, Lookup, Context);
    }

    Class->Flags &= ~WMI_CLASS_RESOLVING;
    if (SUCCEEDED(hr))
        Class->Flags |= WMI_CLASS_RESOLVED;
    return hr;
}

VOID
WmiBlockClearValues(
    _In_ ULONG Count,
    _Inout_updates_(Count) VARIANT *Values)
{
    ULONG Index;

    for (Index = 0; Index < Count; Index++)
        VariantClear(&Values[Index]);
}

static
HRESULT
WmiBlockDecodeScalar(
    _In_ const WMI_PROPERTY_DEF *Item,
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ ULONG Size,
    _Inout_ PULONG Offset,
    _In_ ULONG Depth,
    _Out_ VARIANT *Value)
{
    CIMTYPE Type = Item->Type & ~CIM_FLAG_ARRAY;
    ULONG Cursor = WMI_ALIGN_UP(*Offset, WmiBlockItemAlignment(Item));
    ULONG Length;

    VariantInit(Value);

    if (Cursor > Size)
        return S_FALSE;

    switch (Type)
    {
        case CIM_STRING:
        case CIM_REFERENCE:
        {
            USHORT ByteCount;

            if (Size - Cursor < sizeof(USHORT))
                return S_FALSE;
            memcpy(&ByteCount, Data + Cursor, sizeof(ByteCount));
            Cursor += sizeof(USHORT);
            if (Size - Cursor < ByteCount)
                return S_FALSE;

            V_VT(Value) = VT_BSTR;
            V_BSTR(Value) = SysAllocStringLen((const OLECHAR *)(Data + Cursor), ByteCount / sizeof(WCHAR));
            if (!V_BSTR(Value))
                return E_OUTOFMEMORY;
            *Offset = Cursor + ByteCount;
            return S_OK;
        }

        case CIM_DATETIME:
        {
            WCHAR Text[WMI_DATETIME_CHARS + 1];

            if (Size - Cursor < WMI_DATETIME_CHARS * sizeof(WCHAR))
                return S_FALSE;
            memcpy(Text, Data + Cursor, WMI_DATETIME_CHARS * sizeof(WCHAR));
            Text[WMI_DATETIME_CHARS] = UNICODE_NULL;

            V_VT(Value) = VT_BSTR;
            V_BSTR(Value) = SysAllocString(Text);
            if (!V_BSTR(Value))
                return E_OUTOFMEMORY;
            *Offset = Cursor + WMI_DATETIME_CHARS * sizeof(WCHAR);
            return S_OK;
        }

        case CIM_OBJECT:
        {
            const WMI_CLASS_DEF *Embedded = Item->Embedded;
            SAFEARRAY *Array;
            VARIANT *Members;
            HRESULT hr;

            if (!Embedded || Depth >= WMI_MAX_EMBED_DEPTH)
                return S_FALSE;

            Array = SafeArrayCreateVector(VT_VARIANT, 0, Embedded->ItemCount);
            if (!Array)
                return E_OUTOFMEMORY;

            SafeArrayAccessData(Array, (void **)&Members);
            hr = WmiBlockDecodeClass(Embedded, Data, Size, &Cursor, Depth + 1, Members);
            SafeArrayUnaccessData(Array);
            if (FAILED(hr))
            {
                SafeArrayDestroy(Array);
                return hr;
            }

            V_VT(Value) = VT_ARRAY | VT_VARIANT;
            V_ARRAY(Value) = Array;

            /* Embedded classes are padded like the C structure they describe */
            *Offset = min(WMI_ALIGN_UP(Cursor, Embedded->Alignment), Size);
            return hr;
        }
    }

    Length = WmiBlockScalarSize(Type);
    if (!Length)
        return S_FALSE;
    if (Size - Cursor < Length)
        return S_FALSE;

    switch (Type)
    {
        case CIM_BOOLEAN:
            V_BOOL(Value) = Data[Cursor] ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        case CIM_SINT8:
            V_I1(Value) = (CHAR)Data[Cursor];
            break;
        case CIM_UINT8:
            V_UI1(Value) = Data[Cursor];
            break;
        default:
            memcpy(&V_UI1(Value), Data + Cursor, Length);
            break;
    }

    V_VT(Value) = WmiBlockVarType(Type);
    *Offset = Cursor + Length;
    return S_OK;
}

static
ULONG
WmiBlockArrayCount(
    _In_ const WMI_CLASS_DEF *Class,
    _In_ const WMI_PROPERTY_DEF *Item,
    _In_reads_(Class->ItemCount) const VARIANT *Values)
{
    VARIANT Converted;
    ULONG Count = 0;
    LONG SizeIndex;

    if (Item->FixedCount)
        return Item->FixedCount;
    if (!Item->SizeIs)
        return 0;

    SizeIndex = WmiBlockFindItem(Class, Item->SizeIs);
    if (SizeIndex < 0)
        return 0;

    VariantInit(&Converted);
    if (SUCCEEDED(VariantChangeType(&Converted, (VARIANT *)&Values[SizeIndex], 0, VT_UI4)))
        Count = V_UI4(&Converted);
    return min(Count, WMI_MAX_ARRAY_COUNT);
}

static
HRESULT
WmiBlockDecodeArray(
    _In_ const WMI_PROPERTY_DEF *Item,
    _In_ ULONG Count,
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ ULONG Size,
    _Inout_ PULONG Offset,
    _In_ ULONG Depth,
    _Out_ VARIANT *Value)
{
    VARTYPE ElementType = WmiBlockVarType(Item->Type);
    SAFEARRAY *Array;
    HRESULT hr = S_OK;
    LONG Index;

    VariantInit(Value);

    /* Do not trust a count that cannot fit in the remaining data */
    if (WmiBlockScalarSize(Item->Type) && Count > (Size - min(*Offset, Size)) / WmiBlockScalarSize(Item->Type))
        return S_FALSE;

    Array = SafeArrayCreateVector(ElementType, 0, Count);
    if (!Array)
        return E_OUTOFMEMORY;

    for (Index = 0; Index < (LONG)Count; Index++)
    {
        VARIANT Element;

        hr = WmiBlockDecodeScalar(Item, Data, Size, Offset, Depth, &Element);
        if (hr != S_OK)
        {
            VariantClear(&Element);
            break;
        }

        if (ElementType == VT_VARIANT)
            hr = SafeArrayPutElement(Array, &Index, &Element);
        else if (ElementType == VT_BSTR)
            hr = SafeArrayPutElement(Array, &Index, V_BSTR(&Element));
        else
            hr = SafeArrayPutElement(Array, &Index, &V_UI1(&Element));
        VariantClear(&Element);
        if (FAILED(hr))
            break;
    }

    if (FAILED(hr))
    {
        SafeArrayDestroy(Array);
        return hr;
    }

    V_VT(Value) = VT_ARRAY | ElementType;
    V_ARRAY(Value) = Array;
    return hr;
}

static
HRESULT
WmiBlockDecodeClass(
    _In_ const WMI_CLASS_DEF *Class,
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ ULONG Size,
    _Inout_ PULONG Offset,
    _In_ ULONG Depth,
    _Out_writes_(Class->ItemCount) VARIANT *Values)
{
    ULONG Index;
    HRESULT hr = S_OK;

    for (Index = 0; Index < Class->ItemCount; Index++)
        VariantInit(&Values[Index]);

    for (Index = 0; Index < Class->ItemCount; Index++)
    {
        const WMI_PROPERTY_DEF *Item = Class->Items[Index];

        if (Item->Type & CIM_FLAG_ARRAY)
        {
            ULONG Count = WmiBlockArrayCount(Class, Item, Values);

            hr = WmiBlockDecodeArray(Item, Count, Data, Size, Offset, Depth, &Values[Index]);
        }
        else
        {
            hr = WmiBlockDecodeScalar(Item, Data, Size, Offset, Depth, &Values[Index]);
        }

        /* A short block leaves the remaining items empty */
        if (hr != S_OK)
            break;
    }

    if (FAILED(hr))
        WmiBlockClearValues(Class->ItemCount, Values);
    return hr;
}

/**
 * @brief Decodes one instance data block into variants in WmiDataId order.
 */
HRESULT
WmiBlockDecode(
    _In_ const WMI_CLASS_DEF *Class,
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ ULONG Size,
    _Out_writes_(Class->ItemCount) VARIANT *Values)
{
    ULONG Offset = 0;

    return WmiBlockDecodeClass(Class, Data, Size, &Offset, 0, Values);
}

static
HRESULT
WmiBlockReserve(
    _Inout_ PWMI_OUTPUT Output,
    _In_ ULONG Alignment,
    _In_ ULONG Length,
    _Outptr_ BYTE **Target)
{
    ULONG Start = WMI_ALIGN_UP(Output->Size, Alignment);

    if (Start < Output->Size || Start + Length < Start)
        return E_OUTOFMEMORY;

    if (Start + Length > Output->Capacity)
    {
        ULONG Capacity = max(Output->Capacity * 2, Start + Length + 64);
        BYTE *Grown = BmofAlloc(Capacity);

        if (!Grown)
            return E_OUTOFMEMORY;
        if (Output->Data)
        {
            memcpy(Grown, Output->Data, Output->Size);
            BmofFree(Output->Data);
        }
        Output->Data = Grown;
        Output->Capacity = Capacity;
    }

    memset(Output->Data + Output->Size, 0, Start + Length - Output->Size);
    *Target = Output->Data + Start;
    Output->Size = Start + Length;
    return S_OK;
}

static
HRESULT
WmiBlockEncodeScalar(
    _In_ const WMI_PROPERTY_DEF *Item,
    _In_opt_ const VARIANT *Value,
    _In_ ULONG Depth,
    _Inout_ PWMI_OUTPUT Output)
{
    CIMTYPE Type = Item->Type & ~CIM_FLAG_ARRAY;
    ULONG Alignment = WmiBlockItemAlignment(Item);
    VARIANT Converted;
    BYTE *Target;
    ULONG Length;
    HRESULT hr;

    VariantInit(&Converted);

    if (Type == CIM_OBJECT)
    {
        const WMI_CLASS_DEF *Embedded = Item->Embedded;
        VARIANT *Members = NULL;
        ULONG Count = 0;

        if (!Embedded || Depth >= WMI_MAX_EMBED_DEPTH)
            return WBEM_E_NOT_SUPPORTED;

        hr = WmiBlockReserve(Output, Embedded->Alignment, 0, &Target);
        if (FAILED(hr))
            return hr;

        if (Value && V_VT(Value) == (VT_ARRAY | VT_VARIANT) && V_ARRAY(Value))
        {
            SafeArrayAccessData(V_ARRAY(Value), (void **)&Members);
            Count = V_ARRAY(Value)->rgsabound[0].cElements;
        }
        if (Count < Embedded->ItemCount)
            Members = NULL;

        hr = WmiBlockEncodeClass(Embedded, Members, Depth + 1, Output);
        if (Value && V_VT(Value) == (VT_ARRAY | VT_VARIANT) && V_ARRAY(Value))
            SafeArrayUnaccessData(V_ARRAY(Value));
        if (FAILED(hr))
            return hr;

        return WmiBlockReserve(Output, Embedded->Alignment, 0, &Target);
    }

    if (Type == CIM_STRING || Type == CIM_REFERENCE || Type == CIM_DATETIME)
    {
        ULONG Chars = 0;

        if (Value && V_VT(Value) != VT_EMPTY && V_VT(Value) != VT_NULL)
        {
            hr = VariantChangeType(&Converted, (VARIANT *)Value, 0, VT_BSTR);
            if (FAILED(hr))
                return WBEM_E_TYPE_MISMATCH;
            Chars = SysStringLen(V_BSTR(&Converted));
        }

        if (Type == CIM_DATETIME)
        {
            hr = WmiBlockReserve(Output, sizeof(USHORT), WMI_DATETIME_CHARS * sizeof(WCHAR), &Target);
            if (SUCCEEDED(hr) && Chars)
                memcpy(Target, V_BSTR(&Converted), min(Chars, WMI_DATETIME_CHARS) * sizeof(WCHAR));
        }
        else
        {
            USHORT ByteCount = (USHORT)min(Chars * sizeof(WCHAR), 0xFFFE);

            hr = WmiBlockReserve(Output, sizeof(USHORT), sizeof(USHORT) + ByteCount, &Target);
            if (SUCCEEDED(hr))
            {
                memcpy(Target, &ByteCount, sizeof(ByteCount));
                if (ByteCount)
                    memcpy(Target + sizeof(USHORT), V_BSTR(&Converted), ByteCount);
            }
        }
        VariantClear(&Converted);
        return hr;
    }

    Length = WmiBlockScalarSize(Type);
    if (!Length)
        return WBEM_E_NOT_SUPPORTED;

    hr = WmiBlockReserve(Output, Alignment, Length, &Target);
    if (FAILED(hr))
        return hr;
    if (!Value || V_VT(Value) == VT_EMPTY || V_VT(Value) == VT_NULL)
        return S_OK;

    if (Type == CIM_BOOLEAN)
    {
        hr = VariantChangeType(&Converted, (VARIANT *)Value, 0, VT_BOOL);
        if (FAILED(hr))
            return WBEM_E_TYPE_MISMATCH;
        *Target = (V_BOOL(&Converted) != VARIANT_FALSE) ? 1 : 0;
        return S_OK;
    }

    /* Unsigned targets also accept negative signed input, as WMI clients send VT_I4 */
    hr = VariantChangeType(&Converted, (VARIANT *)Value, 0, WmiBlockVarType(Type));
    if (FAILED(hr) && Type != CIM_REAL32 && Type != CIM_REAL64)
    {
        hr = VariantChangeType(&Converted, (VARIANT *)Value, 0, VT_I8);
        if (SUCCEEDED(hr))
        {
            memcpy(Target, &V_I8(&Converted), Length);
            return S_OK;
        }
    }
    if (FAILED(hr))
        return WBEM_E_TYPE_MISMATCH;

    memcpy(Target, &V_UI1(&Converted), Length);
    return S_OK;
}

static
ULONG
WmiBlockValueCount(
    _In_opt_ const VARIANT *Value)
{
    if (!Value || !(V_VT(Value) & VT_ARRAY) || !V_ARRAY(Value))
        return 0;
    return V_ARRAY(Value)->rgsabound[0].cElements;
}

static
HRESULT
WmiBlockEncodeArray(
    _In_ const WMI_PROPERTY_DEF *Item,
    _In_ ULONG Count,
    _In_opt_ const VARIANT *Value,
    _In_ ULONG Depth,
    _Inout_ PWMI_OUTPUT Output)
{
    ULONG Available = WmiBlockValueCount(Value);
    VARTYPE ArrayType = VT_EMPTY;
    LONG Index;
    HRESULT hr = S_OK;

    if (Available)
        SafeArrayGetVartype(V_ARRAY(Value), &ArrayType);

    for (Index = 0; Index < (LONG)Count; Index++)
    {
        VARIANT Element;

        VariantInit(&Element);
        if ((ULONG)Index < Available)
        {
            if (ArrayType == VT_VARIANT)
            {
                hr = SafeArrayGetElement(V_ARRAY(Value), &Index, &Element);
            }
            else if (ArrayType == VT_BSTR)
            {
                BSTR String = NULL;

                hr = SafeArrayGetElement(V_ARRAY(Value), &Index, &String);
                V_VT(&Element) = VT_BSTR;
                V_BSTR(&Element) = String;
            }
            else if (ArrayType == VT_UNKNOWN || ArrayType == VT_DISPATCH)
            {
                hr = WBEM_E_TYPE_MISMATCH;
            }
            else
            {
                hr = SafeArrayGetElement(V_ARRAY(Value), &Index, &V_UI1(&Element));
                V_VT(&Element) = ArrayType;
            }
            if (FAILED(hr))
                return hr;
        }

        hr = WmiBlockEncodeScalar(Item, ((ULONG)Index < Available) ? &Element : NULL, Depth, Output);
        VariantClear(&Element);
        if (FAILED(hr))
            return hr;
    }

    /* An empty array still aligns the following item correctly */
    if (!Count)
    {
        BYTE *Target;

        if ((Item->Type & ~CIM_FLAG_ARRAY) != CIM_OBJECT || Item->Embedded)
            hr = WmiBlockReserve(Output, WmiBlockItemAlignment(Item), 0, &Target);
    }
    return hr;
}

static
HRESULT
WmiBlockEncodeClass(
    _In_ const WMI_CLASS_DEF *Class,
    _In_reads_opt_(Class->ItemCount) const VARIANT *Values,
    _In_ ULONG Depth,
    _Inout_ PWMI_OUTPUT Output)
{
    VARIANT *Counts = NULL;
    ULONG Index;
    HRESULT hr = S_OK;

    /* WmiSizeIs counters always follow the actual array length */
    if (Class->ItemCount)
    {
        Counts = BmofAlloc(Class->ItemCount * sizeof(*Counts));
        if (!Counts)
            return E_OUTOFMEMORY;
    }

    for (Index = 0; Index < Class->ItemCount; Index++)
    {
        const WMI_PROPERTY_DEF *Item = Class->Items[Index];
        LONG SizeIndex;

        if (!(Item->Type & CIM_FLAG_ARRAY) || Item->FixedCount || !Item->SizeIs)
            continue;

        SizeIndex = WmiBlockFindItem(Class, Item->SizeIs);
        if (SizeIndex >= 0 && (ULONG)SizeIndex < Index)
        {
            V_VT(&Counts[SizeIndex]) = VT_UI4;
            V_UI4(&Counts[SizeIndex]) = Values ? WmiBlockValueCount(&Values[Index]) : 0;
        }
    }

    for (Index = 0; Index < Class->ItemCount; Index++)
    {
        const WMI_PROPERTY_DEF *Item = Class->Items[Index];
        const VARIANT *Value = Values ? &Values[Index] : NULL;

        if (V_VT(&Counts[Index]) != VT_EMPTY)
            Value = &Counts[Index];

        if (Item->Type & CIM_FLAG_ARRAY)
        {
            ULONG Count = Item->FixedCount ? Item->FixedCount : WmiBlockValueCount(Value);

            hr = WmiBlockEncodeArray(Item, Count, Value, Depth, Output);
        }
        else
        {
            hr = WmiBlockEncodeScalar(Item, Value, Depth, Output);
        }
        if (FAILED(hr))
            break;
    }

    BmofFree(Counts);
    return hr;
}

/**
 * @brief Encodes variants in WmiDataId order into a data block buffer.
 */
HRESULT
WmiBlockEncode(
    _In_ const WMI_CLASS_DEF *Class,
    _In_reads_(Class->ItemCount) const VARIANT *Values,
    _Outptr_result_bytebuffer_(*Size) BYTE **Data,
    _Out_ ULONG *Size)
{
    WMI_OUTPUT Output;
    HRESULT hr;

    memset(&Output, 0, sizeof(Output));
    *Data = NULL;
    *Size = 0;

    hr = WmiBlockEncodeClass(Class, Values, 0, &Output);
    if (FAILED(hr))
    {
        BmofFree(Output.Data);
        return hr;
    }

    /* Keep a valid pointer for blocks without data items */
    if (!Output.Data)
    {
        Output.Data = BmofAlloc(sizeof(ULONGLONG));
        if (!Output.Data)
            return E_OUTOFMEMORY;
    }

    *Data = Output.Data;
    *Size = Output.Size;
    return S_OK;
}

static
BOOLEAN
WmiBlockReadName(
    _In_reads_bytes_(NodeSize) const BYTE *Node,
    _In_ ULONG NodeSize,
    _In_ ULONG Offset,
    _Outptr_result_maybenull_ PCWSTR *Name,
    _Out_ PULONG NameLength)
{
    USHORT ByteCount;

    *Name = NULL;
    *NameLength = 0;

    if (!Offset || Offset > NodeSize || NodeSize - Offset < sizeof(USHORT))
        return FALSE;

    memcpy(&ByteCount, Node + Offset, sizeof(ByteCount));
    if (NodeSize - Offset - sizeof(USHORT) < ByteCount)
        return FALSE;

    *Name = (PCWSTR)(Node + Offset + sizeof(USHORT));
    *NameLength = ByteCount / sizeof(WCHAR);
    return TRUE;
}

/**
 * @brief Reports every instance in a chain of WNODE_ALL_DATA or WNODE_SINGLE_INSTANCE blocks.
 */
ULONG
WmiBlockWalk(
    _In_reads_bytes_(Size) const BYTE *Buffer,
    _In_ ULONG Size,
    _In_ PWMI_INSTANCE_ROUTINE Routine,
    _In_opt_ PVOID Context)
{
    ULONG Offset = 0;
    ULONG Reported = 0;

    while (Offset < Size && Size - Offset >= sizeof(WNODE_HEADER))
    {
        const BYTE *Node = Buffer + Offset;
        WNODE_HEADER Header;
        ULONG NodeSize;

        memcpy(&Header, Node, sizeof(Header));
        NodeSize = min(Header.BufferSize, Size - Offset);
        if (NodeSize < sizeof(WNODE_HEADER) || (Header.Flags & WNODE_FLAG_TOO_SMALL))
            break;

        if ((Header.Flags & WNODE_FLAG_ALL_DATA) && NodeSize >= (ULONG)FIELD_OFFSET(WNODE_ALL_DATA, FixedInstanceSize))
        {
            const WNODE_ALL_DATA *AllData = (const WNODE_ALL_DATA *)Node;
            ULONG Index;

            for (Index = 0; Index < AllData->InstanceCount; Index++)
            {
                ULONG DataOffset, DataSize;
                PCWSTR Name = NULL;
                ULONG NameLength = 0;

                if (Header.Flags & WNODE_FLAG_FIXED_INSTANCE_SIZE)
                {
                    if (NodeSize < sizeof(WNODE_ALL_DATA))
                        break;
                    DataSize = AllData->FixedInstanceSize;
                    DataOffset = AllData->DataBlockOffset + Index * WMI_ALIGN_UP(DataSize, 8);
                }
                else
                {
                    ULONG Entry = FIELD_OFFSET(WNODE_ALL_DATA, OffsetInstanceDataAndLength) +
                                  Index * sizeof(OFFSETINSTANCEDATAANDLENGTH);
                    OFFSETINSTANCEDATAANDLENGTH Location;

                    if (Entry > NodeSize || NodeSize - Entry < sizeof(Location))
                        break;
                    memcpy(&Location, Node + Entry, sizeof(Location));
                    DataOffset = Location.OffsetInstanceData;
                    DataSize = Location.LengthInstanceData;
                }

                if (DataOffset > NodeSize || NodeSize - DataOffset < DataSize)
                    break;

                /* User mode fills in names even when the provider registered static ones */
                if (AllData->OffsetInstanceNameOffsets)
                {
                    ULONG Entry = AllData->OffsetInstanceNameOffsets + Index * sizeof(ULONG);
                    ULONG NameOffset;

                    if (Entry <= NodeSize && NodeSize - Entry >= sizeof(ULONG))
                    {
                        memcpy(&NameOffset, Node + Entry, sizeof(NameOffset));
                        WmiBlockReadName(Node, NodeSize, NameOffset, &Name, &NameLength);
                    }
                }

                if (!Routine(Context, Name, NameLength, Reported, Node + DataOffset, DataSize))
                    return Reported + 1;
                Reported++;
            }
        }
        else if ((Header.Flags & WNODE_FLAG_SINGLE_INSTANCE) && NodeSize >= sizeof(WNODE_SINGLE_INSTANCE))
        {
            const WNODE_SINGLE_INSTANCE *Single = (const WNODE_SINGLE_INSTANCE *)Node;
            PCWSTR Name = NULL;
            ULONG NameLength = 0;

            if (Single->DataBlockOffset <= NodeSize && NodeSize - Single->DataBlockOffset >= Single->SizeDataBlock)
            {
                WmiBlockReadName(Node, NodeSize, Single->OffsetInstanceName, &Name, &NameLength);

                if (!Routine(Context,
                             Name,
                             NameLength,
                             (Header.Flags & WNODE_FLAG_STATIC_INSTANCE_NAMES) ? Single->InstanceIndex : Reported,
                             Node + Single->DataBlockOffset,
                             Single->SizeDataBlock))
                {
                    return Reported + 1;
                }
                Reported++;
            }
        }

        if (!Header.Linkage || Header.Linkage > Size - Offset)
            break;
        Offset += Header.Linkage;
    }
    return Reported;
}
