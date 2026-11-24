/*
 * PROJECT:     ReactOS WBEM Proxy
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Binary MOF (BMOF) decompression and class extraction
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

/* "FOMB" read as a little endian ULONG */
#define BMOF_SIGNATURE          0x424D4F46
#define BMOF_COMPRESSION_DS     1
#define BMOF_NONE               0xFFFFFFFF
#define BMOF_HEADER_SIZE        (5 * sizeof(ULONG))
#define BMOF_OBJECT_HEADER      (5 * sizeof(ULONG))
#define BMOF_QUALIFIER_HEADER   (4 * sizeof(ULONG))
#define BMOF_PROPERTY_HEADER    (5 * sizeof(ULONG))
#define BMOF_DS_END_MARKER      4415
#define BMOF_MAX_NESTING        8

typedef struct _BMOF_BITS
{
    const BYTE *Data;
    ULONG Size;
    ULONG Position;
    BOOLEAN IsExhausted;
} BMOF_BITS, *PBMOF_BITS;

typedef struct _BMOF_READER
{
    const BYTE *Base;
    ULONG Size;
} BMOF_READER, *PBMOF_READER;

typedef struct _BMOF_OBJECT
{
    PWSTR ClassName;
    PWSTR SuperName;
    PWSTR Namespace;
    BOOLEAN IsInstance;
    ULONG QualifierCount;
    PWMI_QUALIFIER_DEF Qualifiers;
    ULONG PropertyCount;
    PWMI_PROPERTY_DEF Properties;
    ULONG MethodCount;
    PWMI_METHOD_DEF Methods;
} BMOF_OBJECT, *PBMOF_OBJECT;

static
HRESULT
BmofParseObject(
    _In_ PBMOF_READER Reader,
    _In_ ULONG Offset,
    _In_ ULONG Limit,
    _In_ ULONG Depth,
    _Out_ PBMOF_OBJECT Object);

static
VOID
BmofFreeObject(
    _Inout_ PBMOF_OBJECT Object);

PVOID
BmofAlloc(
    _In_ SIZE_T Size)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
}

VOID
BmofFree(
    _In_opt_ PVOID Block)
{
    if (Block)
        HeapFree(GetProcessHeap(), 0, Block);
}

PWSTR
BmofStrDup(
    _In_opt_ PCWSTR Source)
{
    SIZE_T Length;
    PWSTR Copy;

    if (!Source)
        return NULL;

    Length = (wcslen(Source) + 1) * sizeof(WCHAR);
    Copy = BmofAlloc(Length);
    if (Copy)
        memcpy(Copy, Source, Length);
    return Copy;
}

static
ULONG
BmofReadBits(
    _Inout_ PBMOF_BITS Bits,
    _In_ ULONG Count)
{
    ULONG Value = 0;
    ULONG Index;

    for (Index = 0; Index < Count; Index++)
    {
        ULONG Byte = Bits->Position >> 3;

        if (Byte >= Bits->Size)
        {
            Bits->IsExhausted = TRUE;
            return 0;
        }
        Value |= ((Bits->Data[Byte] >> (Bits->Position & 7)) & 1) << Index;
        Bits->Position++;
    }
    return Value;
}

/**
 * @brief Expands the DS (MRCI) stream that follows a compressed BMOF header.
 */
HRESULT
BmofDecompress(
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ ULONG Size,
    _Outptr_result_bytebuffer_(*OutSize) BYTE **Out,
    _Out_ ULONG *OutSize)
{
    const ULONG *Header = (const ULONG *)Data;
    ULONG CompressedSize;
    ULONG ExpandedSize;
    ULONG Written = 0;
    BMOF_BITS Bits;
    BYTE *Buffer;

    *Out = NULL;
    *OutSize = 0;

    if (Size < 4 * sizeof(ULONG) + 4)
        return WBEM_E_INVALID_PARAMETER;
    if (Header[0] != BMOF_SIGNATURE || Header[1] != BMOF_COMPRESSION_DS)
        return WBEM_E_INVALID_PARAMETER;

    CompressedSize = Header[2];
    ExpandedSize = Header[3];
    if (CompressedSize > Size - 4 * sizeof(ULONG) || CompressedSize < 4 || !ExpandedSize)
        return WBEM_E_INVALID_PARAMETER;

    Data += 4 * sizeof(ULONG);
    if (Data[0] != 'D' || Data[1] != 'S' || Data[2] != 0 || Data[3] != 1)
        return WBEM_E_INVALID_PARAMETER;

    Buffer = BmofAlloc(ExpandedSize);
    if (!Buffer)
        return E_OUTOFMEMORY;

    Bits.Data = Data;
    Bits.Size = CompressedSize;
    Bits.Position = 32;
    Bits.IsExhausted = FALSE;

    while (Written < ExpandedSize && !Bits.IsExhausted)
    {
        ULONG Code = BmofReadBits(&Bits, 2);
        ULONG Distance;
        ULONG Length;
        ULONG Zeros = 0;

        if (Code == 2)
        {
            Buffer[Written++] = (BYTE)BmofReadBits(&Bits, 7);
            continue;
        }
        if (Code == 1)
        {
            Buffer[Written++] = (BYTE)(BmofReadBits(&Bits, 7) | 0x80);
            continue;
        }

        if (Code == 0)
        {
            Distance = BmofReadBits(&Bits, 6);
        }
        else if (!BmofReadBits(&Bits, 1))
        {
            Distance = BmofReadBits(&Bits, 8) + 64;
        }
        else
        {
            Distance = BmofReadBits(&Bits, 12) + 320;

            /* Block terminator, the stream simply continues after it */
            if (Distance == BMOF_DS_END_MARKER)
                continue;
        }

        while (!BmofReadBits(&Bits, 1) && !Bits.IsExhausted)
        {
            if (++Zeros > 16)
                break;
        }
        if (Zeros > 16 || Bits.IsExhausted)
            break;

        Length = Zeros ? (1U << Zeros) + BmofReadBits(&Bits, Zeros) + 1 : 2;
        if (!Distance || Distance > Written)
            break;

        while (Length-- && Written < ExpandedSize)
        {
            Buffer[Written] = Buffer[Written - Distance];
            Written++;
        }
    }

    if (Written != ExpandedSize)
    {
        BmofFree(Buffer);
        return WBEM_E_INVALID_SYNTAX;
    }

    *Out = Buffer;
    *OutSize = ExpandedSize;
    return S_OK;
}

static
BOOLEAN
BmofHasRange(
    _In_ ULONG Offset,
    _In_ ULONG Length,
    _In_ ULONG Limit)
{
    return (Offset <= Limit && Length <= Limit - Offset);
}

static
BOOLEAN
BmofReadUlong(
    _In_ PBMOF_READER Reader,
    _In_ ULONG Offset,
    _In_ ULONG Limit,
    _Out_ PULONG Value)
{
    if (!BmofHasRange(Offset, sizeof(ULONG), Limit))
        return FALSE;

    memcpy(Value, Reader->Base + Offset, sizeof(*Value));
    return TRUE;
}

static
PWSTR
BmofReadString(
    _In_ PBMOF_READER Reader,
    _In_ ULONG Offset,
    _In_ ULONG Limit,
    _Out_opt_ PULONG Consumed)
{
    ULONG Cursor = Offset;
    ULONG Length;
    PWSTR String;

    for (;;)
    {
        WCHAR Char;

        if (!BmofHasRange(Cursor, sizeof(WCHAR), Limit))
            return NULL;
        memcpy(&Char, Reader->Base + Cursor, sizeof(Char));
        Cursor += sizeof(WCHAR);
        if (Char == UNICODE_NULL)
            break;
    }

    Length = Cursor - Offset;
    String = BmofAlloc(Length);
    if (!String)
        return NULL;

    memcpy(String, Reader->Base + Offset, Length);
    if (Consumed)
        *Consumed = Length;
    return String;
}

static
VARTYPE
BmofVarType(
    _In_ CIMTYPE Type)
{
    switch (Type & ~CIM_FLAG_ARRAY)
    {
        case CIM_SINT8: return VT_I1;
        case CIM_UINT8: return VT_UI1;
        case CIM_SINT16: return VT_I2;
        case CIM_CHAR16:
        case CIM_UINT16: return VT_I4;
        case CIM_SINT32:
        case CIM_UINT32: return VT_I4;
        case CIM_SINT64: return VT_I8;
        case CIM_UINT64: return VT_UI8;
        case CIM_REAL32: return VT_R4;
        case CIM_REAL64: return VT_R8;
        case CIM_BOOLEAN: return VT_BOOL;
        case CIM_STRING:
        case CIM_DATETIME:
        case CIM_REFERENCE: return VT_BSTR;
        default: return VT_EMPTY;
    }
}

static
ULONG
BmofScalarSize(
    _In_ CIMTYPE Type)
{
    switch (Type & ~CIM_FLAG_ARRAY)
    {
        case CIM_SINT8:
        case CIM_UINT8: return 1;
        case CIM_SINT16:
        case CIM_UINT16:
        case CIM_CHAR16:
        case CIM_BOOLEAN: return 2;
        case CIM_SINT32:
        case CIM_UINT32:
        case CIM_REAL32: return 4;
        case CIM_SINT64:
        case CIM_UINT64:
        case CIM_REAL64: return 8;
        default: return 0;
    }
}

/* Reads one non-array value and reports how many bytes it used */
static
HRESULT
BmofReadScalar(
    _In_ PBMOF_READER Reader,
    _In_ ULONG Offset,
    _In_ ULONG Limit,
    _In_ CIMTYPE Type,
    _Out_ VARIANT *Value,
    _Out_ PULONG Consumed)
{
    const BYTE *Source = Reader->Base + Offset;
    ULONG Size;

    VariantInit(Value);
    *Consumed = 0;

    if (BmofVarType(Type) == VT_BSTR)
    {
        PWSTR String = BmofReadString(Reader, Offset, Limit, Consumed);

        if (!String)
            return WBEM_E_INVALID_SYNTAX;
        V_VT(Value) = VT_BSTR;
        V_BSTR(Value) = SysAllocString(String);
        BmofFree(String);
        return V_BSTR(Value) ? S_OK : E_OUTOFMEMORY;
    }

    Size = BmofScalarSize(Type);
    if (!Size)
        return WBEM_E_NOT_SUPPORTED;
    if (!BmofHasRange(Offset, Size, Limit))
        return WBEM_E_INVALID_SYNTAX;

    switch (Type & ~CIM_FLAG_ARRAY)
    {
        case CIM_SINT8:
            V_I1(Value) = (CHAR)Source[0];
            break;
        case CIM_UINT8:
            V_UI1(Value) = Source[0];
            break;
        case CIM_SINT16:
            memcpy(&V_I2(Value), Source, Size);
            break;
        case CIM_UINT16:
        case CIM_CHAR16:
        {
            USHORT Short;

            memcpy(&Short, Source, Size);
            V_I4(Value) = Short;
            break;
        }
        case CIM_BOOLEAN:
        {
            SHORT Bool;

            memcpy(&Bool, Source, Size);
            V_BOOL(Value) = Bool ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        }
        case CIM_SINT32:
        case CIM_UINT32:
            memcpy(&V_I4(Value), Source, Size);
            break;
        case CIM_SINT64:
            memcpy(&V_I8(Value), Source, Size);
            break;
        case CIM_UINT64:
            memcpy(&V_UI8(Value), Source, Size);
            break;
        case CIM_REAL32:
            memcpy(&V_R4(Value), Source, Size);
            break;
        case CIM_REAL64:
            memcpy(&V_R8(Value), Source, Size);
            break;
    }

    V_VT(Value) = BmofVarType(Type);
    *Consumed = Size;
    return S_OK;
}

/* Arrays are stored as total length, dimensions, count, row length and elements */
static
HRESULT
BmofReadValue(
    _In_ PBMOF_READER Reader,
    _In_ ULONG Offset,
    _In_ ULONG Limit,
    _In_ CIMTYPE Type,
    _Out_ VARIANT *Value)
{
    ULONG Total, Dimensions, Count, Row;
    ULONG Consumed;
    SAFEARRAY *Array;
    VARTYPE ElementType;
    LONG Index;
    HRESULT hr;

    VariantInit(Value);

    if ((Type & ~CIM_FLAG_ARRAY) == CIM_OBJECT)
        return S_OK;

    if (!(Type & CIM_FLAG_ARRAY))
        return BmofReadScalar(Reader, Offset, Limit, Type, Value, &Consumed);

    if (!BmofReadUlong(Reader, Offset, Limit, &Total) ||
        !BmofReadUlong(Reader, Offset + 4, Limit, &Dimensions) ||
        !BmofReadUlong(Reader, Offset + 8, Limit, &Count) ||
        !BmofReadUlong(Reader, Offset + 12, Limit, &Row))
    {
        return WBEM_E_INVALID_SYNTAX;
    }
    if (Dimensions != 1 || Count > WMI_MAX_ARRAY_COUNT || !BmofHasRange(Offset, Total, Limit))
        return WBEM_E_INVALID_SYNTAX;

    ElementType = BmofVarType(Type);
    if (ElementType == VT_EMPTY)
        return WBEM_E_NOT_SUPPORTED;

    Array = SafeArrayCreateVector(ElementType, 0, Count);
    if (!Array)
        return E_OUTOFMEMORY;

    Limit = Offset + Total;
    Offset += 16;
    for (Index = 0; Index < (LONG)Count; Index++)
    {
        VARIANT Element;

        hr = BmofReadScalar(Reader, Offset, Limit, Type, &Element, &Consumed);
        if (FAILED(hr))
        {
            SafeArrayDestroy(Array);
            return hr;
        }

        if (ElementType == VT_BSTR)
            hr = SafeArrayPutElement(Array, &Index, V_BSTR(&Element));
        else
            hr = SafeArrayPutElement(Array, &Index, &V_I1(&Element));
        VariantClear(&Element);
        if (FAILED(hr))
        {
            SafeArrayDestroy(Array);
            return hr;
        }
        Offset += Consumed;
    }

    V_VT(Value) = VT_ARRAY | ElementType;
    V_ARRAY(Value) = Array;
    return S_OK;
}

VOID
BmofFreeQualifiers(
    _In_ ULONG Count,
    _In_opt_ PWMI_QUALIFIER_DEF Qualifiers)
{
    ULONG Index;

    if (!Qualifiers)
        return;

    for (Index = 0; Index < Count; Index++)
    {
        BmofFree(Qualifiers[Index].Name);
        VariantClear(&Qualifiers[Index].Value);
    }
    BmofFree(Qualifiers);
}

const WMI_QUALIFIER_DEF *
BmofFindQualifier(
    _In_ ULONG Count,
    _In_reads_(Count) const WMI_QUALIFIER_DEF *Qualifiers,
    _In_ PCWSTR Name)
{
    ULONG Index;

    for (Index = 0; Index < Count; Index++)
    {
        if (!_wcsicmp(Qualifiers[Index].Name, Name))
            return &Qualifiers[Index];
    }
    return NULL;
}

static
HRESULT
BmofParseQualifiers(
    _In_ PBMOF_READER Reader,
    _In_ ULONG Offset,
    _In_ ULONG Limit,
    _Out_ PULONG Count,
    _Outptr_result_maybenull_ PWMI_QUALIFIER_DEF *Qualifiers)
{
    PWMI_QUALIFIER_DEF List;
    ULONG Length, Total;
    ULONG Index;
    HRESULT hr;

    *Count = 0;
    *Qualifiers = NULL;

    if (!BmofReadUlong(Reader, Offset, Limit, &Length) ||
        !BmofReadUlong(Reader, Offset + 4, Limit, &Total) ||
        !BmofHasRange(Offset, Length, Limit))
    {
        return WBEM_E_INVALID_SYNTAX;
    }
    if (!Total)
        return S_OK;
    if (Total > Length / BMOF_QUALIFIER_HEADER)
        return WBEM_E_INVALID_SYNTAX;

    List = BmofAlloc(Total * sizeof(*List));
    if (!List)
        return E_OUTOFMEMORY;

    Limit = Offset + Length;
    Offset += 8;
    for (Index = 0; Index < Total; Index++)
    {
        ULONG RecordLength, Type, Flavor, ValueOffset;
        ULONG RecordEnd;

        if (!BmofReadUlong(Reader, Offset, Limit, &RecordLength) ||
            !BmofReadUlong(Reader, Offset + 4, Limit, &Type) ||
            !BmofReadUlong(Reader, Offset + 8, Limit, &Flavor) ||
            !BmofReadUlong(Reader, Offset + 12, Limit, &ValueOffset) ||
            RecordLength < BMOF_QUALIFIER_HEADER ||
            !BmofHasRange(Offset, RecordLength, Limit))
        {
            hr = WBEM_E_INVALID_SYNTAX;
            goto Fail;
        }

        RecordEnd = Offset + RecordLength;
        List[Index].Name = BmofReadString(Reader, Offset + BMOF_QUALIFIER_HEADER, RecordEnd, NULL);
        if (!List[Index].Name)
        {
            hr = WBEM_E_INVALID_SYNTAX;
            goto Fail;
        }
        List[Index].Flavor = (LONG)Flavor;

        if (ValueOffset != BMOF_NONE)
        {
            hr = BmofReadValue(Reader,
                               Offset + BMOF_QUALIFIER_HEADER + ValueOffset,
                               RecordEnd,
                               Type,
                               &List[Index].Value);
            if (FAILED(hr) && hr != WBEM_E_NOT_SUPPORTED)
                goto Fail;
        }
        Offset = RecordEnd;
    }

    *Count = Total;
    *Qualifiers = List;
    return S_OK;

Fail:
    BmofFreeQualifiers(Total, List);
    return hr;
}

static
LONG
BmofQualifierLong(
    _In_ ULONG Count,
    _In_reads_(Count) const WMI_QUALIFIER_DEF *Qualifiers,
    _In_ PCWSTR Name,
    _In_ LONG Default)
{
    const WMI_QUALIFIER_DEF *Qualifier = BmofFindQualifier(Count, Qualifiers, Name);
    VARIANT Converted;
    LONG Result = Default;

    if (!Qualifier)
        return Default;

    VariantInit(&Converted);
    if (SUCCEEDED(VariantChangeType(&Converted, (VARIANT *)&Qualifier->Value, 0, VT_I4)))
        Result = V_I4(&Converted);
    VariantClear(&Converted);
    return Result;
}

static
BOOLEAN
BmofQualifierBool(
    _In_ ULONG Count,
    _In_reads_(Count) const WMI_QUALIFIER_DEF *Qualifiers,
    _In_ PCWSTR Name)
{
    const WMI_QUALIFIER_DEF *Qualifier = BmofFindQualifier(Count, Qualifiers, Name);

    if (!Qualifier)
        return FALSE;
    if (V_VT(&Qualifier->Value) == VT_BOOL)
        return V_BOOL(&Qualifier->Value) != VARIANT_FALSE;
    return TRUE;
}

static
PCWSTR
BmofQualifierString(
    _In_ ULONG Count,
    _In_reads_(Count) const WMI_QUALIFIER_DEF *Qualifiers,
    _In_ PCWSTR Name)
{
    const WMI_QUALIFIER_DEF *Qualifier = BmofFindQualifier(Count, Qualifiers, Name);

    if (!Qualifier || V_VT(&Qualifier->Value) != VT_BSTR)
        return NULL;
    return V_BSTR(&Qualifier->Value);
}

static
VOID
BmofFreeProperty(
    _Inout_ PWMI_PROPERTY_DEF Property)
{
    BmofFree(Property->Name);
    BmofFree(Property->ObjectClass);
    BmofFree(Property->SizeIs);
    BmofFreeQualifiers(Property->QualifierCount, Property->Qualifiers);
    memset(Property, 0, sizeof(*Property));
}

/* Derives the layout attributes of a property from its qualifiers */
static
HRESULT
BmofApplyPropertyQualifiers(
    _Inout_ PWMI_PROPERTY_DEF Property)
{
    ULONG Count = Property->QualifierCount;
    PWMI_QUALIFIER_DEF List = Property->Qualifiers;
    PCWSTR CimType;
    PCWSTR SizeIs;

    if (BmofQualifierBool(Count, List, L"key"))
        Property->Flags |= WMI_ITEM_KEY;
    if (BmofQualifierBool(Count, List, L"read"))
        Property->Flags |= WMI_ITEM_READ;
    if (BmofQualifierBool(Count, List, L"write"))
        Property->Flags |= WMI_ITEM_WRITE;
    if (BmofQualifierBool(Count, List, L"in"))
        Property->Flags |= WMI_ITEM_IN;
    if (BmofQualifierBool(Count, List, L"out"))
        Property->Flags |= WMI_ITEM_OUT;

    Property->DataId = (ULONG)BmofQualifierLong(Count, List, L"WmiDataId", 0);
    Property->FixedCount = (ULONG)BmofQualifierLong(Count, List, L"MAX", 0);

    SizeIs = BmofQualifierString(Count, List, L"WmiSizeIs");
    if (SizeIs)
    {
        Property->SizeIs = BmofStrDup(SizeIs);
        if (!Property->SizeIs)
            return E_OUTOFMEMORY;
    }

    CimType = BmofQualifierString(Count, List, L"CIMTYPE");
    if (CimType && !_wcsnicmp(CimType, L"object:", 7) && CimType[7])
    {
        Property->ObjectClass = BmofStrDup(CimType + 7);
        if (!Property->ObjectClass)
            return E_OUTOFMEMORY;
    }
    return S_OK;
}

static
HRESULT
BmofParseMethod(
    _In_ PBMOF_READER Reader,
    _In_ ULONG ValueOffset,
    _In_ ULONG Limit,
    _In_ ULONG Depth,
    _Inout_ PWMI_METHOD_DEF Method);

/* Walks a property or method list; system properties land in the object header fields */
static
HRESULT
BmofParseMembers(
    _In_ PBMOF_READER Reader,
    _In_ ULONG Offset,
    _In_ ULONG Limit,
    _In_ BOOLEAN IsMethodList,
    _In_ ULONG Depth,
    _Inout_ PBMOF_OBJECT Object)
{
    ULONG Length, Total;
    ULONG Index;
    HRESULT hr;

    if (!BmofReadUlong(Reader, Offset, Limit, &Length) ||
        !BmofReadUlong(Reader, Offset + 4, Limit, &Total) ||
        !BmofHasRange(Offset, Length, Limit))
    {
        return WBEM_E_INVALID_SYNTAX;
    }
    if (!Total)
        return S_OK;
    if (Total > Length / BMOF_PROPERTY_HEADER)
        return WBEM_E_INVALID_SYNTAX;

    if (IsMethodList)
    {
        Object->Methods = BmofAlloc(Total * sizeof(*Object->Methods));
        if (!Object->Methods)
            return E_OUTOFMEMORY;
    }
    else
    {
        Object->Properties = BmofAlloc(Total * sizeof(*Object->Properties));
        if (!Object->Properties)
            return E_OUTOFMEMORY;
    }

    Limit = Offset + Length;
    Offset += 8;
    for (Index = 0; Index < Total; Index++)
    {
        ULONG RecordLength, Type, Flavor, ValueOffset, QualifierOffset;
        ULONG RecordEnd, NameOffset;
        ULONG QualifierCount = 0;
        PWMI_QUALIFIER_DEF Qualifiers = NULL;
        PWSTR Name;

        if (!BmofReadUlong(Reader, Offset, Limit, &RecordLength) ||
            !BmofReadUlong(Reader, Offset + 4, Limit, &Type) ||
            !BmofReadUlong(Reader, Offset + 8, Limit, &Flavor) ||
            !BmofReadUlong(Reader, Offset + 12, Limit, &ValueOffset) ||
            !BmofReadUlong(Reader, Offset + 16, Limit, &QualifierOffset) ||
            RecordLength < BMOF_PROPERTY_HEADER ||
            !BmofHasRange(Offset, RecordLength, Limit))
        {
            return WBEM_E_INVALID_SYNTAX;
        }

        RecordEnd = Offset + RecordLength;
        NameOffset = Offset + BMOF_PROPERTY_HEADER;
        Name = BmofReadString(Reader, NameOffset, RecordEnd, NULL);
        if (!Name)
            return WBEM_E_INVALID_SYNTAX;

        if (QualifierOffset != BMOF_NONE)
        {
            hr = BmofParseQualifiers(Reader,
                                     NameOffset + QualifierOffset,
                                     RecordEnd,
                                     &QualifierCount,
                                     &Qualifiers);
            if (FAILED(hr))
            {
                BmofFree(Name);
                return hr;
            }
        }

        if (IsMethodList)
        {
            PWMI_METHOD_DEF Method = &Object->Methods[Object->MethodCount++];

            Method->Name = Name;
            Method->QualifierCount = QualifierCount;
            Method->Qualifiers = Qualifiers;
            Method->MethodId = (ULONG)BmofQualifierLong(QualifierCount, Qualifiers, L"WmiMethodId", 0);

            if (ValueOffset != BMOF_NONE && Type == (CIM_OBJECT | CIM_FLAG_ARRAY))
            {
                hr = BmofParseMethod(Reader, NameOffset + ValueOffset, RecordEnd, Depth, Method);
                if (FAILED(hr))
                    return hr;
            }
        }
        else if (Name[0] == L'_' && Name[1] == L'_')
        {
            VARIANT Value;

            VariantInit(&Value);
            if (ValueOffset != BMOF_NONE && (Type & ~CIM_FLAG_ARRAY) == CIM_STRING)
                BmofReadValue(Reader, NameOffset + ValueOffset, RecordEnd, Type, &Value);

            if (V_VT(&Value) == VT_BSTR)
            {
                PWSTR *Target = NULL;

                if (!_wcsicmp(Name, L"__CLASS"))
                    Target = &Object->ClassName;
                else if (!_wcsicmp(Name, L"__SUPERCLASS"))
                    Target = &Object->SuperName;
                else if (!_wcsicmp(Name, L"__NAMESPACE"))
                    Target = &Object->Namespace;

                if (Target && !*Target)
                    *Target = BmofStrDup(V_BSTR(&Value));
            }
            VariantClear(&Value);
            BmofFree(Name);
            BmofFreeQualifiers(QualifierCount, Qualifiers);
        }
        else
        {
            PWMI_PROPERTY_DEF Property = &Object->Properties[Object->PropertyCount++];

            Property->Name = Name;
            Property->Type = Type;
            Property->QualifierCount = QualifierCount;
            Property->Qualifiers = Qualifiers;
            hr = BmofApplyPropertyQualifiers(Property);
            if (FAILED(hr))
                return hr;
        }

        Offset = RecordEnd;
    }
    return S_OK;
}

static
int
__cdecl
BmofCompareDataId(
    _In_ const void *Left,
    _In_ const void *Right)
{
    const WMI_PROPERTY_DEF *A = Left;
    const WMI_PROPERTY_DEF *B = Right;

    if (A->DataId < B->DataId)
        return -1;
    return (A->DataId > B->DataId) ? 1 : 0;
}

/* Collects parameters with the given direction from the parsed parameter objects */
static
HRESULT
BmofBuildParameters(
    _In_reads_(ObjectCount) PBMOF_OBJECT Objects,
    _In_ ULONG ObjectCount,
    _In_ ULONG Direction,
    _Outptr_result_maybenull_ PWMI_CLASS_DEF *Result)
{
    PWMI_CLASS_DEF Params;
    ULONG Total = 0;
    ULONG ObjectIndex, Index, Check;

    *Result = NULL;

    for (ObjectIndex = 0; ObjectIndex < ObjectCount; ObjectIndex++)
        Total += Objects[ObjectIndex].PropertyCount;

    Params = BmofAlloc(sizeof(*Params));
    if (!Params)
        return E_OUTOFMEMORY;

    Params->Flags = WMI_CLASS_PARAMETERS;
    Params->Name = BmofStrDup(L"__PARAMETERS");
    if (Total)
        Params->Properties = BmofAlloc(Total * sizeof(*Params->Properties));
    if (!Params->Name || (Total && !Params->Properties))
    {
        BmofFreeClass(Params);
        return E_OUTOFMEMORY;
    }

    for (ObjectIndex = 0; ObjectIndex < ObjectCount; ObjectIndex++)
    {
        PBMOF_OBJECT Object = &Objects[ObjectIndex];

        for (Index = 0; Index < Object->PropertyCount; Index++)
        {
            PWMI_PROPERTY_DEF Source = &Object->Properties[Index];
            PWMI_PROPERTY_DEF Target;
            BOOLEAN IsDuplicate = FALSE;
            LONG ParamId;

            if (!(Source->Flags & Direction))
                continue;

            for (Check = 0; Check < Params->PropertyCount; Check++)
            {
                if (!_wcsicmp(Params->Properties[Check].Name, Source->Name))
                    IsDuplicate = TRUE;
            }
            if (IsDuplicate)
                continue;

            /* Move the definition, the source object is freed afterwards */
            Target = &Params->Properties[Params->PropertyCount++];
            *Target = *Source;
            memset(Source, 0, sizeof(*Source));

            ParamId = BmofQualifierLong(Target->QualifierCount, Target->Qualifiers, L"ID", -1);
            if (!Target->DataId && ParamId >= 0)
                Target->DataId = (ULONG)ParamId + 1;
        }
    }

    if (!Params->PropertyCount)
    {
        BmofFreeClass(Params);
        return S_OK;
    }

    qsort(Params->Properties, Params->PropertyCount, sizeof(*Params->Properties), BmofCompareDataId);
    *Result = Params;
    return S_OK;
}

static
HRESULT
BmofParseMethod(
    _In_ PBMOF_READER Reader,
    _In_ ULONG ValueOffset,
    _In_ ULONG Limit,
    _In_ ULONG Depth,
    _Inout_ PWMI_METHOD_DEF Method)
{
    BMOF_OBJECT Objects[2];
    ULONG Total, Dimensions, Count, Row;
    ULONG Offset, End;
    ULONG Index, Parsed = 0;
    HRESULT hr = S_OK;

    memset(Objects, 0, sizeof(Objects));

    if (!BmofReadUlong(Reader, ValueOffset, Limit, &Total) ||
        !BmofReadUlong(Reader, ValueOffset + 4, Limit, &Dimensions) ||
        !BmofReadUlong(Reader, ValueOffset + 8, Limit, &Count) ||
        !BmofReadUlong(Reader, ValueOffset + 12, Limit, &Row) ||
        !BmofHasRange(ValueOffset, Total, Limit) ||
        Dimensions != 1)
    {
        return WBEM_E_INVALID_SYNTAX;
    }

    End = ValueOffset + Total;
    Offset = ValueOffset + 16;
    for (Index = 0; Index < Count && Parsed < ARRAYSIZE(Objects); Index++)
    {
        ULONG ObjectLength;

        if (!BmofReadUlong(Reader, Offset, End, &ObjectLength) || !BmofHasRange(Offset, ObjectLength, End))
        {
            hr = WBEM_E_INVALID_SYNTAX;
            break;
        }

        hr = BmofParseObject(Reader, Offset, Offset + ObjectLength, Depth + 1, &Objects[Parsed]);
        if (FAILED(hr))
            break;
        Parsed++;
        Offset += ObjectLength;
    }

    if (SUCCEEDED(hr))
        hr = BmofBuildParameters(Objects, Parsed, WMI_ITEM_IN, &Method->InParams);
    if (SUCCEEDED(hr))
        hr = BmofBuildParameters(Objects, Parsed, WMI_ITEM_OUT, &Method->OutParams);

    for (Index = 0; Index < ARRAYSIZE(Objects); Index++)
        BmofFreeObject(&Objects[Index]);
    return hr;
}

static
VOID
BmofFreeObject(
    _Inout_ PBMOF_OBJECT Object)
{
    ULONG Index;

    BmofFree(Object->ClassName);
    BmofFree(Object->SuperName);
    BmofFree(Object->Namespace);
    BmofFreeQualifiers(Object->QualifierCount, Object->Qualifiers);

    for (Index = 0; Index < Object->PropertyCount; Index++)
        BmofFreeProperty(&Object->Properties[Index]);
    BmofFree(Object->Properties);

    for (Index = 0; Index < Object->MethodCount; Index++)
    {
        PWMI_METHOD_DEF Method = &Object->Methods[Index];

        BmofFree(Method->Name);
        BmofFreeQualifiers(Method->QualifierCount, Method->Qualifiers);
        BmofFreeClass(Method->InParams);
        BmofFreeClass(Method->OutParams);
    }
    BmofFree(Object->Methods);
    memset(Object, 0, sizeof(*Object));
}

/* Object layout: length, qualifier, property and method offsets, instance flag */
static
HRESULT
BmofParseObject(
    _In_ PBMOF_READER Reader,
    _In_ ULONG Offset,
    _In_ ULONG Limit,
    _In_ ULONG Depth,
    _Out_ PBMOF_OBJECT Object)
{
    ULONG Length, QualifierOffset, PropertyOffset, MethodOffset, IsInstance;
    ULONG Payload;
    HRESULT hr;

    memset(Object, 0, sizeof(*Object));

    if (Depth > BMOF_MAX_NESTING)
        return WBEM_E_INVALID_SYNTAX;

    if (!BmofReadUlong(Reader, Offset, Limit, &Length) ||
        !BmofReadUlong(Reader, Offset + 4, Limit, &QualifierOffset) ||
        !BmofReadUlong(Reader, Offset + 8, Limit, &PropertyOffset) ||
        !BmofReadUlong(Reader, Offset + 12, Limit, &MethodOffset) ||
        !BmofReadUlong(Reader, Offset + 16, Limit, &IsInstance) ||
        Length < BMOF_OBJECT_HEADER ||
        !BmofHasRange(Offset, Length, Limit))
    {
        return WBEM_E_INVALID_SYNTAX;
    }

    Limit = Offset + Length;
    Payload = Offset + BMOF_OBJECT_HEADER;
    Object->IsInstance = (IsInstance != 0);

    if (QualifierOffset != BMOF_NONE)
    {
        hr = BmofParseQualifiers(Reader,
                                 Payload + QualifierOffset,
                                 Limit,
                                 &Object->QualifierCount,
                                 &Object->Qualifiers);
        if (FAILED(hr))
            goto Fail;
    }
    if (PropertyOffset != BMOF_NONE)
    {
        hr = BmofParseMembers(Reader, Payload + PropertyOffset, Limit, FALSE, Depth, Object);
        if (FAILED(hr))
            goto Fail;
    }
    if (MethodOffset != BMOF_NONE)
    {
        hr = BmofParseMembers(Reader, Payload + MethodOffset, Limit, TRUE, Depth, Object);
        if (FAILED(hr))
            goto Fail;
    }
    return S_OK;

Fail:
    BmofFreeObject(Object);
    return hr;
}

VOID
BmofFreeClass(
    _In_opt_ PWMI_CLASS_DEF Class)
{
    ULONG Index;

    if (!Class)
        return;

    BmofFree(Class->Name);
    BmofFree(Class->SuperName);
    BmofFreeQualifiers(Class->QualifierCount, Class->Qualifiers);

    for (Index = 0; Index < Class->PropertyCount; Index++)
        BmofFreeProperty(&Class->Properties[Index]);
    BmofFree(Class->Properties);

    for (Index = 0; Index < Class->MethodCount; Index++)
    {
        PWMI_METHOD_DEF Method = &Class->Methods[Index];

        BmofFree(Method->Name);
        BmofFreeQualifiers(Method->QualifierCount, Method->Qualifiers);
        BmofFreeClass(Method->InParams);
        BmofFreeClass(Method->OutParams);
    }
    BmofFree(Class->Methods);
    BmofFree(Class->All);
    BmofFree(Class->Items);
    BmofFree(Class);
}

static
BOOLEAN
BmofParseHex(
    _In_ PCWSTR Text,
    _In_ ULONG Digits,
    _Out_ PULONG Value)
{
    ULONG Index;

    *Value = 0;
    for (Index = 0; Index < Digits; Index++)
    {
        WCHAR Char = Text[Index];
        ULONG Digit;

        if (Char >= L'0' && Char <= L'9')
            Digit = Char - L'0';
        else if (Char >= L'a' && Char <= L'f')
            Digit = Char - L'a' + 10;
        else if (Char >= L'A' && Char <= L'F')
            Digit = Char - L'A' + 10;
        else
            return FALSE;
        *Value = (*Value << 4) | Digit;
    }
    return TRUE;
}

BOOLEAN
BmofParseGuid(
    _In_ PCWSTR Text,
    _Out_ GUID *Guid)
{
    ULONG Value;
    ULONG Index;

    if (*Text == L'{')
        Text++;
    if (wcslen(Text) < 36 || Text[8] != L'-' || Text[13] != L'-' || Text[18] != L'-' || Text[23] != L'-')
        return FALSE;

    if (!BmofParseHex(Text, 8, &Value))
        return FALSE;
    Guid->Data1 = Value;
    if (!BmofParseHex(Text + 9, 4, &Value))
        return FALSE;
    Guid->Data2 = (USHORT)Value;
    if (!BmofParseHex(Text + 14, 4, &Value))
        return FALSE;
    Guid->Data3 = (USHORT)Value;

    for (Index = 0; Index < 8; Index++)
    {
        PCWSTR Pair = (Index < 2) ? Text + 19 + Index * 2 : Text + 24 + (Index - 2) * 2;

        if (!BmofParseHex(Pair, 2, &Value))
            return FALSE;
        Guid->Data4[Index] = (BYTE)Value;
    }
    return TRUE;
}

/* Turns a parsed class object into a class definition, consuming its members */
static
PWMI_CLASS_DEF
BmofCreateClass(
    _Inout_ PBMOF_OBJECT Object)
{
    PWMI_CLASS_DEF Class;
    PCWSTR GuidText;

    Class = BmofAlloc(sizeof(*Class));
    if (!Class)
        return NULL;

    Class->Name = Object->ClassName;
    Class->SuperName = Object->SuperName;
    Class->QualifierCount = Object->QualifierCount;
    Class->Qualifiers = Object->Qualifiers;
    Class->PropertyCount = Object->PropertyCount;
    Class->Properties = Object->Properties;
    Class->MethodCount = Object->MethodCount;
    Class->Methods = Object->Methods;

    Object->ClassName = NULL;
    Object->SuperName = NULL;
    Object->Qualifiers = NULL;
    Object->QualifierCount = 0;
    Object->Properties = NULL;
    Object->PropertyCount = 0;
    Object->Methods = NULL;
    Object->MethodCount = 0;

    if (BmofQualifierBool(Class->QualifierCount, Class->Qualifiers, L"abstract"))
        Class->Flags |= WMI_CLASS_ABSTRACT;
    if (BmofQualifierBool(Class->QualifierCount, Class->Qualifiers, L"WMI"))
        Class->Flags |= WMI_CLASS_WMI;
    if (BmofQualifierBool(Class->QualifierCount, Class->Qualifiers, L"Dynamic"))
        Class->Flags |= WMI_CLASS_DYNAMIC;

    GuidText = BmofQualifierString(Class->QualifierCount, Class->Qualifiers, L"guid");
    if (GuidText && BmofParseGuid(GuidText, &Class->Guid))
        Class->Flags |= WMI_CLASS_HAS_GUID;

    return Class;
}

/* Driver classes always land in root\wmi, only localized amendment namespaces are skipped */
static
BOOLEAN
BmofIsWmiNamespace(
    _In_opt_ PCWSTR Namespace)
{
    PCWSTR Tail;

    if (!Namespace)
        return TRUE;

    Tail = wcsrchr(Namespace, L'\\');
    Tail = Tail ? Tail + 1 : Namespace;
    return _wcsnicmp(Tail, L"ms_", 3) != 0;
}

/**
 * @brief Parses a BMOF image, compressed or not, and reports each root\wmi class.
 */
HRESULT
BmofParse(
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ ULONG Size,
    _In_ PBMOF_CLASS_ROUTINE Routine,
    _In_opt_ PVOID Context)
{
    BMOF_READER Reader;
    BYTE *Expanded = NULL;
    ULONG ExpandedSize;
    ULONG Total, ObjectCount;
    ULONG Offset, Index;
    HRESULT hr = S_OK;

    if (Size < BMOF_HEADER_SIZE || ((const ULONG *)Data)[0] != BMOF_SIGNATURE)
        return WBEM_E_INVALID_PARAMETER;

    if (((const ULONG *)Data)[1] == BMOF_COMPRESSION_DS)
    {
        hr = BmofDecompress(Data, Size, &Expanded, &ExpandedSize);
        if (FAILED(hr))
            return hr;
        Data = Expanded;
        Size = ExpandedSize;
        if (Size < BMOF_HEADER_SIZE || ((const ULONG *)Data)[0] != BMOF_SIGNATURE)
        {
            BmofFree(Expanded);
            return WBEM_E_INVALID_SYNTAX;
        }
    }

    Reader.Base = Data;
    Reader.Size = Size;

    /* Signature, length of the object area, version, reserved, object count */
    Total = min(((const ULONG *)Data)[1], Size);
    ObjectCount = ((const ULONG *)Data)[4];

    Offset = BMOF_HEADER_SIZE;
    for (Index = 0; Index < ObjectCount; Index++)
    {
        BMOF_OBJECT Object;
        ULONG Length;

        if (!BmofReadUlong(&Reader, Offset, Total, &Length) || !BmofHasRange(Offset, Length, Total))
        {
            hr = WBEM_E_INVALID_SYNTAX;
            break;
        }

        hr = BmofParseObject(&Reader, Offset, Offset + Length, 0, &Object);
        if (FAILED(hr))
            break;

        if (!Object.IsInstance && Object.ClassName && BmofIsWmiNamespace(Object.Namespace))
        {
            PWMI_CLASS_DEF Class = BmofCreateClass(&Object);

            if (!Class)
            {
                BmofFreeObject(&Object);
                hr = E_OUTOFMEMORY;
                break;
            }
            Routine(Context, Class);
        }

        BmofFreeObject(&Object);
        Offset += Length;
    }

    BmofFree(Expanded);
    return hr;
}
