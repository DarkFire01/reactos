/*
 * PROJECT:     ReactOS WBEM Proxy
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     root\wmi namespace backed by kernel WMI data blocks
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#define COBJMACROS

#include <stdarg.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "wbemcli.h"

#include "wine/debug.h"
#include "wbemprox_private.h"
#include "wmiprov.h"

WINE_DEFAULT_DEBUG_CHANNEL(wbemprox);

#define WMIPROV_QUERY_ATTEMPTS  8
#define WMIPROV_INITIAL_BUFFER  0x1000
#define WMIPROV_METHOD_BUFFER   0x400

typedef struct _WMIPROV_TABLE
{
    struct table Table;
    PWMI_CLASS_DEF Class;
} WMIPROV_TABLE, *PWMIPROV_TABLE;

typedef struct _WMIPROV_SOURCE
{
    struct list Entry;
    PWSTR ImagePath;
    PWSTR ResourceName;
} WMIPROV_SOURCE, *PWMIPROV_SOURCE;

typedef struct _WMIPROV_ROW
{
    const VARIANT *Values;
    PCWSTR Name;
    ULONG NameLength;
    ULONG Index;
    BOOLEAN IsInstance;
} WMIPROV_ROW, *PWMIPROV_ROW;

typedef struct _WMIPROV_LOAD
{
    struct table *Table;
    PWMI_CLASS_DEF Class;
    UINT Row;
} WMIPROV_LOAD, *PWMIPROV_LOAD;

typedef struct _WMIPROV_EVENT
{
    LONG Refs;
    BOOLEAN IsActive;
    PWMI_CLASS_DEF Class;
    IWbemObjectSink *Sink;
} WMIPROV_EVENT, *PWMIPROV_EVENT;

typedef struct _WMIPROV_QUALIFIER_SET
{
    IWbemQualifierSet IWbemQualifierSet_iface;
    LONG Refs;
    ULONG Count;
    const WMI_QUALIFIER_DEF *Items;
    ULONG Cursor;
} WMIPROV_QUALIFIER_SET, *PWMIPROV_QUALIFIER_SET;

static CRITICAL_SECTION WmiProvLock;
static CRITICAL_SECTION_DEBUG WmiProvLockDebug =
{
    0, 0, &WmiProvLock,
    { &WmiProvLockDebug.ProcessLocksList, &WmiProvLockDebug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": WmiProvLock") }
};
static CRITICAL_SECTION WmiProvLock = { &WmiProvLockDebug, -1, 0, 0, 0, 0 };

static PWMI_CLASS_DEF *WmiProvClasses;
static ULONG WmiProvClassCount;
static ULONG WmiProvClassCapacity;
static BOOLEAN WmiProvIsInitialized;
static struct list WmiProvSources = LIST_INIT(WmiProvSources);

static const WCHAR WmiProvInstanceName[] = {'I','n','s','t','a','n','c','e','N','a','m','e',0};
static const WCHAR WmiProvActive[] = {'A','c','t','i','v','e',0};
static const WCHAR WmiProvClassProperty[] = {'_','_','C','L','A','S','S',0};
static const WCHAR WmiProvParameters[] = {'_','_','P','A','R','A','M','E','T','E','R','S',0};

static const struct column WmiProvParameterColumns[] =
{
    { L"Class",        CIM_STRING },
    { L"Method",       CIM_STRING },
    { L"Direction",    CIM_SINT32 },
    { L"Parameter",    CIM_STRING },
    { L"Type",         CIM_UINT32 },
    { L"DefaultValue", CIM_UINT32 },
};

#include <pshpack1.h>
typedef struct _WMIPROV_PARAMETER_ROW
{
    const WCHAR *Class;
    const WCHAR *Method;
    INT32 Direction;
    const WCHAR *Parameter;
    UINT32 Type;
    UINT32 DefaultValue;
} WMIPROV_PARAMETER_ROW, *PWMIPROV_PARAMETER_ROW;
#include <poppack.h>

static
HRESULT
WmiProvCreateObjects(
    _In_ PWMI_CLASS_DEF Class,
    _In_ ULONG Count,
    _In_reads_(Count) const WMIPROV_ROW *Rows,
    _Out_writes_(Count) IWbemClassObject **Objects);

static
HRESULT
WmiProvMapError(
    _In_ ULONG Error)
{
    switch (Error)
    {
        case ERROR_SUCCESS:
            return S_OK;
        case ERROR_WMI_GUID_NOT_FOUND:
        case ERROR_WMI_INSTANCE_NOT_FOUND:
        case ERROR_FILE_NOT_FOUND:
            return WBEM_E_NOT_FOUND;
        case ERROR_WMI_ITEMID_NOT_FOUND:
            return WBEM_E_INVALID_METHOD;
        case ERROR_ACCESS_DENIED:
            return WBEM_E_ACCESS_DENIED;
        case ERROR_INVALID_PARAMETER:
        case ERROR_INVALID_DATA:
            return WBEM_E_INVALID_PARAMETER;
        case ERROR_NOT_SUPPORTED:
        case ERROR_INVALID_FUNCTION:
        case ERROR_CALL_NOT_IMPLEMENTED:
            return WBEM_E_NOT_SUPPORTED;
        case ERROR_WMI_READ_ONLY:
            return WBEM_E_READ_ONLY;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return E_OUTOFMEMORY;
        case ERROR_WMI_SET_FAILURE:
            return WBEM_E_PROVIDER_FAILURE;
        default:
            return WBEM_E_FAILED;
    }
}

static
PWMI_CLASS_DEF
WmiProvFindClassLocked(
    _In_ PCWSTR Name)
{
    ULONG Index;

    for (Index = 0; Index < WmiProvClassCount; Index++)
    {
        if (!_wcsicmp(WmiProvClasses[Index]->Name, Name))
            return WmiProvClasses[Index];
    }
    return NULL;
}

static
PWMI_CLASS_DEF
WmiProvLookupRoutine(
    _In_opt_ PVOID Context,
    _In_ PCWSTR Name)
{
    UNREFERENCED_PARAMETER(Context);
    return WmiProvFindClassLocked(Name);
}

/* Class definitions are never freed, so the result stays valid after the lock is dropped */
static
PWMI_CLASS_DEF
WmiProvFindClass(
    _In_opt_ PCWSTR Name)
{
    PWMI_CLASS_DEF Class;

    if (!Name)
        return NULL;

    EnterCriticalSection(&WmiProvLock);
    Class = WmiProvFindClassLocked(Name);
    LeaveCriticalSection(&WmiProvLock);
    return Class;
}

static
BOOLEAN
WmiProvIsDataClass(
    _In_ const WMI_CLASS_DEF *Class)
{
    if (!(Class->Flags & WMI_CLASS_HAS_GUID))
        return FALSE;
    return !(Class->Flags & (WMI_CLASS_ABSTRACT | WMI_CLASS_EVENT | WMI_CLASS_TRACE));
}

static
VOID
WmiProvAddClassRoutine(
    _In_opt_ PVOID Context,
    _In_ PWMI_CLASS_DEF Class)
{
    PULONG Added = Context;

    if (WmiProvFindClassLocked(Class->Name))
    {
        BmofFreeClass(Class);
        return;
    }

    if (WmiProvClassCount == WmiProvClassCapacity)
    {
        ULONG Capacity = WmiProvClassCapacity ? WmiProvClassCapacity * 2 : 64;
        PWMI_CLASS_DEF *Grown = heap_realloc(WmiProvClasses, Capacity * sizeof(*Grown));

        if (!Grown)
        {
            BmofFreeClass(Class);
            return;
        }
        WmiProvClasses = Grown;
        WmiProvClassCapacity = Capacity;
    }

    WmiProvClasses[WmiProvClassCount++] = Class;
    if (Added)
        (*Added)++;
}

static
CIMTYPE
WmiProvColumnType(
    _In_ const WMI_PROPERTY_DEF *Property)
{
    CIMTYPE Base = Property->Type & ~CIM_FLAG_ARRAY;

    switch (Base)
    {
        case CIM_SINT8:
        case CIM_UINT8:
        case CIM_SINT16:
        case CIM_UINT16:
        case CIM_SINT32:
        case CIM_UINT32:
        case CIM_SINT64:
        case CIM_UINT64:
        case CIM_REAL32:
        case CIM_REAL64:
        case CIM_BOOLEAN:
        case CIM_STRING:
        case CIM_DATETIME:
        case CIM_REFERENCE:
        case CIM_CHAR16:
        case CIM_OBJECT:
            break;
        default:
            Base = CIM_STRING;
            break;
    }
    return Base | (Property->Type & CIM_FLAG_ARRAY);
}

/* Builds the schema table of a class, the template that queries copy */
static
PWMIPROV_TABLE
WmiProvCreateTemplate(
    _In_ PWMI_CLASS_DEF Class,
    _In_opt_ enum fill_status (*Fill)(struct table *, const struct expr *))
{
    PWMIPROV_TABLE Template;
    struct column *Columns;
    ULONG Index, Count = 0;

    Template = heap_alloc_zero(sizeof(*Template));
    Columns = heap_alloc_zero((Class->AllCount + Class->MethodCount + 1) * sizeof(*Columns));
    if (!Template || !Columns)
    {
        heap_free(Template);
        heap_free(Columns);
        return NULL;
    }

    for (Index = 0; Index < Class->AllCount; Index++)
    {
        const WMI_PROPERTY_DEF *Property = Class->All[Index];

        Columns[Count].name = Property->Name;
        Columns[Count].type = WmiProvColumnType(Property) | COL_FLAG_DYNAMIC;
        if (Property->Flags & WMI_ITEM_KEY)
            Columns[Count].type |= COL_FLAG_KEY;
        Count++;
    }
    for (Index = 0; Index < Class->MethodCount; Index++)
    {
        Columns[Count].name = Class->Methods[Index].Name;
        Columns[Count].type = CIM_FLAG_ARRAY | COL_FLAG_METHOD;
        Count++;
    }

    Template->Class = Class;
    Template->Table.name = Class->Name;
    Template->Table.num_cols = Count;
    Template->Table.columns = Columns;
    Template->Table.fill = Fill;
    Template->Table.flags = TABLE_FLAG_WMIPROV;
    list_init(&Template->Table.entry);
    return Template;
}

/**
 * @brief Copies a provider table so a query can own its rows.
 */
struct table *
WmiProvCloneTable(
    _In_ const struct table *Template)
{
    const WMIPROV_TABLE *Source = CONTAINING_RECORD(Template, WMIPROV_TABLE, Table);
    PWMIPROV_TABLE Copy;
    struct column *Columns;
    UINT Index;

    Copy = heap_alloc_zero(sizeof(*Copy));
    Columns = heap_alloc_zero(max(Template->num_cols, 1) * sizeof(*Columns));
    if (!Copy || !Columns)
    {
        heap_free(Copy);
        heap_free(Columns);
        return NULL;
    }

    for (Index = 0; Index < Template->num_cols; Index++)
    {
        Columns[Index].type = Template->columns[Index].type;
        Columns[Index].name = heap_strdupW(Template->columns[Index].name);
        if (!Columns[Index].name)
        {
            free_columns(Columns, Index);
            heap_free(Copy);
            return NULL;
        }
    }

    Copy->Class = Source->Class;
    Copy->Table.name = heap_strdupW(Template->name);
    if (!Copy->Table.name)
    {
        free_columns(Columns, Template->num_cols);
        heap_free(Copy);
        return NULL;
    }
    Copy->Table.num_cols = Template->num_cols;
    Copy->Table.columns = Columns;
    Copy->Table.fill = Template->fill;
    Copy->Table.flags = TABLE_FLAG_DYNAMIC | TABLE_FLAG_WMIPROV;
    Copy->Table.refs = 1;
    list_init(&Copy->Table.entry);
    return &Copy->Table;
}

static
BOOLEAN
WmiProvAllocateRows(
    _Inout_ struct table *Table,
    _In_ UINT Count)
{
    Table->data = NULL;
    Table->num_rows = 0;
    Table->num_rows_allocated = 0;

    /* Classes without properties have no row layout */
    if (!Count || !Table->num_cols)
        return !Count;

    Table->data = heap_alloc_zero(Count * get_row_size(Table));
    if (!Table->data)
        return FALSE;
    Table->num_rows_allocated = Count;
    return TRUE;
}

/* Turns decoded embedded values (nested variant arrays) into class objects */
static
HRESULT
WmiProvEmbeddedToValue(
    _In_ const WMI_PROPERTY_DEF *Property,
    _In_ const VARIANT *Value,
    _Out_ LONGLONG *Result)
{
    PWMI_CLASS_DEF Embedded = Property->Embedded;
    IWbemClassObject **Objects = NULL;
    PWMIPROV_ROW Rows = NULL;
    VARIANT *Elements = NULL;
    struct array *Array;
    ULONG Count, Index;
    HRESULT hr;

    *Result = 0;
    if (!Embedded || V_VT(Value) != (VT_ARRAY | VT_VARIANT) || !V_ARRAY(Value))
        return S_OK;

    if (!(Property->Type & CIM_FLAG_ARRAY))
    {
        WMIPROV_ROW Row;
        IWbemClassObject *Object;

        if (V_ARRAY(Value)->rgsabound[0].cElements < Embedded->ItemCount)
            return S_OK;

        SafeArrayAccessData(V_ARRAY(Value), (void **)&Elements);
        memset(&Row, 0, sizeof(Row));
        Row.Values = Elements;
        hr = WmiProvCreateObjects(Embedded, 1, &Row, &Object);
        SafeArrayUnaccessData(V_ARRAY(Value));
        if (SUCCEEDED(hr))
            *Result = (INT_PTR)Object;
        return hr;
    }

    Count = V_ARRAY(Value)->rgsabound[0].cElements;
    Array = heap_alloc_zero(sizeof(*Array));
    if (!Array)
        return E_OUTOFMEMORY;
    Array->elem_size = sizeof(IWbemClassObject *);
    Array->ptr = heap_alloc_zero(max(Count, 1) * sizeof(IWbemClassObject *));
    Rows = heap_alloc_zero(max(Count, 1) * sizeof(*Rows));
    if (!Array->ptr || !Rows)
    {
        heap_free(Rows);
        destroy_array(Array, CIM_OBJECT);
        return E_OUTOFMEMORY;
    }

    SafeArrayAccessData(V_ARRAY(Value), (void **)&Elements);
    hr = S_OK;
    for (Index = 0; Index < Count; Index++)
    {
        const VARIANT *Element = &Elements[Index];

        if (V_VT(Element) != (VT_ARRAY | VT_VARIANT) || !V_ARRAY(Element) ||
            V_ARRAY(Element)->rgsabound[0].cElements < Embedded->ItemCount)
        {
            hr = WBEM_E_INVALID_PARAMETER;
            break;
        }
        Rows[Index].Values = V_ARRAY(Element)->pvData;
    }

    Objects = Array->ptr;
    if (SUCCEEDED(hr) && Count)
        hr = WmiProvCreateObjects(Embedded, Count, Rows, Objects);
    SafeArrayUnaccessData(V_ARRAY(Value));
    heap_free(Rows);

    if (FAILED(hr))
    {
        destroy_array(Array, CIM_OBJECT);
        return hr;
    }

    Array->count = Count;
    *Result = (INT_PTR)Array;
    return S_OK;
}

static
HRESULT
WmiProvStoreValue(
    _Inout_ struct table *Table,
    _In_ UINT Row,
    _In_ UINT Column,
    _In_ const WMI_PROPERTY_DEF *Property,
    _In_ VARIANT *Value)
{
    CIMTYPE Type = Table->columns[Column].type & COL_TYPE_MASK;
    LONGLONG Result = 0;
    HRESULT hr;

    if (V_VT(Value) == VT_EMPTY || V_VT(Value) == VT_NULL)
        return S_OK;

    if ((Type & ~CIM_FLAG_ARRAY) == CIM_OBJECT)
        hr = WmiProvEmbeddedToValue(Property, Value, &Result);
    else
        hr = variant_to_value(Value, Type, &Result);
    if (FAILED(hr))
        return hr;

    return set_value(Table, Row, Column, Result, Type);
}

static
VOID
WmiProvStoreRow(
    _Inout_ struct table *Table,
    _In_ UINT Row,
    _In_ const WMI_CLASS_DEF *Class,
    _In_ const WMIPROV_ROW *Source)
{
    UINT Column;

    for (Column = 0; Column < Table->num_cols; Column++)
    {
        const struct column *Info = &Table->columns[Column];
        LONG Item;

        if (Info->type & COL_FLAG_METHOD)
            continue;

        if (Source->IsInstance && !_wcsicmp(Info->name, WmiProvInstanceName) &&
            (Info->type & COL_TYPE_MASK) == CIM_STRING)
        {
            WCHAR *Name;

            if (Source->Name)
            {
                Name = heap_alloc((Source->NameLength + 1) * sizeof(WCHAR));
                if (Name)
                {
                    memcpy(Name, Source->Name, Source->NameLength * sizeof(WCHAR));
                    Name[Source->NameLength] = UNICODE_NULL;
                }
            }
            else
            {
                /* Providers with static names that user mode did not resolve */
                Name = heap_alloc((lstrlenW(Class->Name) + 16) * sizeof(WCHAR));
                if (Name)
                    swprintf(Name, L"%s_%u", Class->Name, Source->Index);
            }
            if (Name)
                set_value(Table, Row, Column, (INT_PTR)Name, CIM_STRING);
            continue;
        }

        if (Source->IsInstance && !_wcsicmp(Info->name, WmiProvActive) && (Info->type & COL_TYPE_MASK) == CIM_BOOLEAN)
        {
            set_value(Table, Row, Column, -1, CIM_BOOLEAN);
            continue;
        }

        Item = WmiBlockFindItem(Class, Info->name);
        if (Item >= 0)
            WmiProvStoreValue(Table, Row, Column, Class->Items[Item], (VARIANT *)&Source->Values[Item]);
    }
}

/* Builds standalone instances, as used for embedded objects, events and method output */
static
HRESULT
WmiProvCreateObjects(
    _In_ PWMI_CLASS_DEF Class,
    _In_ ULONG Count,
    _In_reads_(Count) const WMIPROV_ROW *Rows,
    _Out_writes_(Count) IWbemClassObject **Objects)
{
    IEnumWbemClassObject *Iterator = NULL;
    struct table *Template, *Table;
    struct query *Query;
    struct view *View;
    ULONG Index;
    HRESULT hr;

    for (Index = 0; Index < Count; Index++)
        Objects[Index] = NULL;
    if (!Count)
        return S_OK;

    Template = grab_table(WBEMPROX_NAMESPACE_WMI, Class->Name);
    if (!Template)
        return WBEM_E_INVALID_CLASS;
    if (!(Template->flags & TABLE_FLAG_WMIPROV))
    {
        release_table(Template);
        return WBEM_E_INVALID_CLASS;
    }
    Table = WmiProvCloneTable(Template);
    release_table(Template);
    if (!Table)
        return E_OUTOFMEMORY;

    /* The rows are fixed, the copy never refills itself */
    Table->fill = NULL;
    if (!WmiProvAllocateRows(Table, Count))
    {
        release_table(Table);
        return E_OUTOFMEMORY;
    }
    for (Index = 0; Index < Count; Index++)
        WmiProvStoreRow(Table, Index, Class, &Rows[Index]);
    Table->num_rows = Count;

    Query = create_query(WBEMPROX_NAMESPACE_WMI);
    View = heap_alloc_zero(sizeof(*View));
    if (!Query || !View)
    {
        heap_free(View);
        if (Query)
            release_query(Query);
        release_table(Table);
        return E_OUTOFMEMORY;
    }

    View->ns = WBEMPROX_NAMESPACE_WMI;
    View->type = VIEW_TYPE_SELECT;
    View->table = heap_alloc(sizeof(*View->table));
    View->result = heap_alloc(Count * sizeof(*View->result));
    Query->view = View;
    if (!View->table || !View->result)
    {
        release_table(Table);
        release_query(Query);
        return E_OUTOFMEMORY;
    }
    View->table[0] = Table;
    View->table_count = 1;
    for (Index = 0; Index < Count; Index++)
        View->result[Index] = Index;
    View->result_count = Count;

    hr = EnumWbemClassObject_create(Query, (void **)&Iterator);
    for (Index = 0; SUCCEEDED(hr) && Index < Count; Index++)
        hr = create_class_object(WBEMPROX_NAMESPACE_WMI, Class->Name, Iterator, Index, NULL, &Objects[Index]);

    if (FAILED(hr))
    {
        for (Index = 0; Index < Count; Index++)
        {
            if (Objects[Index])
                IWbemClassObject_Release(Objects[Index]);
            Objects[Index] = NULL;
        }
    }
    if (Iterator)
        IEnumWbemClassObject_Release(Iterator);
    release_query(Query);
    return hr;
}

static
ULONG
WmiProvQueryAllData(
    _In_ const GUID *Guid,
    _Outptr_result_bytebuffer_(*Size) BYTE **Buffer,
    _Out_ PULONG Size)
{
    WMIHANDLE Handle;
    ULONG Length = WMIPROV_INITIAL_BUFFER;
    ULONG Attempt;
    ULONG Error;

    *Buffer = NULL;
    *Size = 0;

    Error = WmiOpenBlock((GUID *)Guid, WMIGUID_QUERY, &Handle);
    if (Error != ERROR_SUCCESS)
        return Error;

    for (Attempt = 0; Attempt < WMIPROV_QUERY_ATTEMPTS; Attempt++)
    {
        ULONG Returned = Length;
        BYTE *Data = heap_alloc_zero(Length);

        if (!Data)
        {
            Error = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }

        Error = WmiQueryAllDataW(Handle, &Returned, Data);
        if (Error == ERROR_SUCCESS && Returned >= sizeof(WNODE_TOO_SMALL) &&
            (((PWNODE_HEADER)Data)->Flags & WNODE_FLAG_TOO_SMALL))
        {
            Returned = ((PWNODE_TOO_SMALL)Data)->SizeNeeded;
            Error = ERROR_INSUFFICIENT_BUFFER;
        }

        if (Error == ERROR_INSUFFICIENT_BUFFER)
        {
            heap_free(Data);
            Length = max(Returned, Length * 2);
            continue;
        }
        if (Error != ERROR_SUCCESS)
        {
            heap_free(Data);
            break;
        }

        *Buffer = Data;
        *Size = min(Returned, Length);
        break;
    }

    WmiCloseBlock(Handle);
    return Error;
}

static
BOOLEAN
WmiProvCountRoutine(
    _In_opt_ PVOID Context,
    _In_reads_opt_(NameLength) PCWSTR Name,
    _In_ ULONG NameLength,
    _In_ ULONG Index,
    _In_reads_bytes_(DataSize) const BYTE *Data,
    _In_ ULONG DataSize)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Name);
    UNREFERENCED_PARAMETER(NameLength);
    UNREFERENCED_PARAMETER(Index);
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(DataSize);
    return TRUE;
}

static
BOOLEAN
WmiProvLoadRoutine(
    _In_opt_ PVOID Context,
    _In_reads_opt_(NameLength) PCWSTR Name,
    _In_ ULONG NameLength,
    _In_ ULONG Index,
    _In_reads_bytes_(DataSize) const BYTE *Data,
    _In_ ULONG DataSize)
{
    PWMIPROV_LOAD Load = Context;
    WMIPROV_ROW Row;
    VARIANT *Values;
    HRESULT hr;

    if (Load->Row >= Load->Table->num_rows_allocated)
        return FALSE;

    Values = heap_alloc_zero(max(Load->Class->ItemCount, 1) * sizeof(*Values));
    if (!Values)
        return FALSE;

    hr = WmiBlockDecode(Load->Class, Data, DataSize, Values);
    if (FAILED(hr))
    {
        WARN("failed to decode %s instance %u, %08x\n", debugstr_w(Load->Class->Name), Index, hr);
        heap_free(Values);
        return TRUE;
    }

    Row.Values = Values;
    Row.Name = Name;
    Row.NameLength = NameLength;
    Row.Index = Index;
    Row.IsInstance = TRUE;
    WmiProvStoreRow(Load->Table, Load->Row, Load->Class, &Row);
    Load->Row++;
    Load->Table->num_rows = Load->Row;

    WmiBlockClearValues(Load->Class->ItemCount, Values);
    heap_free(Values);
    return TRUE;
}

static
enum fill_status
WmiProvFillClass(
    _Inout_ struct table *Table,
    _In_opt_ const struct expr *Condition)
{
    PWMI_CLASS_DEF Class = CONTAINING_RECORD(Table, WMIPROV_TABLE, Table)->Class;
    WMIPROV_LOAD Load;
    BYTE *Buffer;
    ULONG Size, Count;
    ULONG Error;

    UNREFERENCED_PARAMETER(Condition);

    if (!WmiProvAllocateRows(Table, 0) || !Class || !WmiProvIsDataClass(Class))
        return FILL_STATUS_UNFILTERED;

    Error = WmiProvQueryAllData(&Class->Guid, &Buffer, &Size);
    if (Error != ERROR_SUCCESS)
    {
        TRACE("no data for %s, error %u\n", debugstr_w(Class->Name), Error);
        return FILL_STATUS_UNFILTERED;
    }

    Count = WmiBlockWalk(Buffer, Size, WmiProvCountRoutine, NULL);
    if (WmiProvAllocateRows(Table, Count))
    {
        Load.Table = Table;
        Load.Class = Class;
        Load.Row = 0;
        WmiBlockWalk(Buffer, Size, WmiProvLoadRoutine, &Load);
    }

    heap_free(Buffer);
    return FILL_STATUS_UNFILTERED;
}

/* Lists every method parameter, create_signature reads this table */
static
enum fill_status
WmiProvFillParameters(
    _Inout_ struct table *Table,
    _In_opt_ const struct expr *Condition)
{
    PWMIPROV_PARAMETER_ROW Rows;
    ULONG Total = 0;
    ULONG Index, MethodIndex, Item, Row = 0;

    UNREFERENCED_PARAMETER(Condition);

    WmiProvAllocateRows(Table, 0);
    EnterCriticalSection(&WmiProvLock);

    for (Index = 0; Index < WmiProvClassCount; Index++)
    {
        PWMI_CLASS_DEF Class = WmiProvClasses[Index];

        for (MethodIndex = 0; MethodIndex < Class->MethodCount; MethodIndex++)
        {
            PWMI_METHOD_DEF Method = &Class->Methods[MethodIndex];

            if (Method->InParams)
                Total += Method->InParams->ItemCount;
            if (Method->OutParams)
                Total += Method->OutParams->ItemCount;
        }
    }

    if (!Total || !WmiProvAllocateRows(Table, Total))
    {
        LeaveCriticalSection(&WmiProvLock);
        return FILL_STATUS_UNFILTERED;
    }

    Rows = (PWMIPROV_PARAMETER_ROW)Table->data;
    for (Index = 0; Index < WmiProvClassCount; Index++)
    {
        PWMI_CLASS_DEF Class = WmiProvClasses[Index];

        for (MethodIndex = 0; MethodIndex < Class->MethodCount; MethodIndex++)
        {
            PWMI_METHOD_DEF Method = &Class->Methods[MethodIndex];
            PWMI_CLASS_DEF Lists[2];
            ULONG List;

            Lists[0] = Method->InParams;
            Lists[1] = Method->OutParams;
            for (List = 0; List < ARRAYSIZE(Lists); List++)
            {
                if (!Lists[List])
                    continue;
                for (Item = 0; Item < Lists[List]->ItemCount && Row < Total; Item++)
                {
                    Rows[Row].Class = Class->Name;
                    Rows[Row].Method = Method->Name;
                    Rows[Row].Direction = List ? PARAM_OUT : PARAM_IN;
                    Rows[Row].Parameter = Lists[List]->Items[Item]->Name;
                    Rows[Row].Type = WmiProvColumnType(Lists[List]->Items[Item]);
                    Rows[Row].DefaultValue = 0;
                    Row++;
                }
            }
        }
    }

    Table->num_rows = Row;
    LeaveCriticalSection(&WmiProvLock);
    return FILL_STATUS_UNFILTERED;
}

static
PWSTR
WmiProvExpandImagePath(
    _In_ PCWSTR ImagePath)
{
    static const WCHAR SystemRootPrefix[] = {'\\','S','y','s','t','e','m','R','o','o','t','\\',0};
    static const WCHAR DosPrefix[] = {'\\','?','?','\\',0};
    WCHAR Windows[MAX_PATH];
    PWSTR Result;
    UINT Length;

    if (!_wcsnicmp(ImagePath, DosPrefix, ARRAYSIZE(DosPrefix) - 1))
        return heap_strdupW(ImagePath + ARRAYSIZE(DosPrefix) - 1);

    Length = GetWindowsDirectoryW(Windows, ARRAYSIZE(Windows));
    if (!Length || Length >= ARRAYSIZE(Windows))
        return heap_strdupW(ImagePath);

    if (!_wcsnicmp(ImagePath, SystemRootPrefix, ARRAYSIZE(SystemRootPrefix) - 1))
        ImagePath += ARRAYSIZE(SystemRootPrefix) - 1;
    else if (ImagePath[0] == L'\\' || wcschr(ImagePath, L':') || !wcschr(ImagePath, L'\\'))
        return heap_strdupW(ImagePath);

    /* Relative to the Windows directory, like "System32\drivers\acpi.sys" */
    Result = heap_alloc((Length + lstrlenW(ImagePath) + 2) * sizeof(WCHAR));
    if (Result)
    {
        lstrcpyW(Result, Windows);
        lstrcatW(Result, L"\\");
        lstrcatW(Result, ImagePath);
    }
    return Result;
}

/* Kernel paths such as \Device\HarddiskVolume1\... fall back to the driver and system directories */
static
HMODULE
WmiProvLoadByName(
    _In_ PCWSTR ImagePath)
{
    static const WCHAR DriversDirectory[] = {'\\','d','r','i','v','e','r','s','\\',0};
    WCHAR Path[MAX_PATH];
    PCWSTR Name = wcsrchr(ImagePath, L'\\');
    HMODULE Module;
    UINT Length;

    Name = Name ? Name + 1 : ImagePath;
    Length = GetSystemDirectoryW(Path, ARRAYSIZE(Path));
    if (!Length || Length + ARRAYSIZE(DriversDirectory) + lstrlenW(Name) >= ARRAYSIZE(Path))
        return NULL;

    lstrcatW(Path, DriversDirectory);
    lstrcatW(Path, Name);
    Module = LoadLibraryExW(Path, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (Module)
        return Module;

    Path[Length] = UNICODE_NULL;
    lstrcatW(Path, L"\\");
    lstrcatW(Path, Name);
    return LoadLibraryExW(Path, NULL, LOAD_LIBRARY_AS_DATAFILE);
}

static
VOID
WmiProvLoadResource(
    _In_ const MOFRESOURCEINFOW *Info,
    _Inout_ PULONG Added)
{
    const BYTE *Data = NULL;
    ULONG Size = 0;
    HMODULE Module = NULL;
    HRESULT hr;

    if (Info->ResourceBuffer && Info->ResourceSize)
    {
        Data = Info->ResourceBuffer;
        Size = Info->ResourceSize;
    }
    else if (Info->ImagePath && Info->ResourceName)
    {
        PWSTR Path = WmiProvExpandImagePath(Info->ImagePath);
        HRSRC Resource;
        HGLOBAL Handle;

        if (!Path)
            return;
        Module = LoadLibraryExW(Path, NULL, LOAD_LIBRARY_AS_DATAFILE);
        heap_free(Path);
        if (!Module)
            Module = WmiProvLoadByName(Info->ImagePath);
        if (!Module)
        {
            WARN("cannot load %s\n", debugstr_w(Info->ImagePath));
            return;
        }

        Resource = FindResourceW(Module, Info->ResourceName, L"MOFDATA");
        Handle = Resource ? LoadResource(Module, Resource) : NULL;
        if (Handle)
        {
            Data = LockResource(Handle);
            Size = SizeofResource(Module, Resource);
        }
    }

    if (Data && Size)
    {
        hr = BmofParse(Data, Size, WmiProvAddClassRoutine, Added);
        if (FAILED(hr))
            WARN("bad MOF %s in %s, %08x\n", debugstr_w(Info->ResourceName), debugstr_w(Info->ImagePath), hr);
    }

    if (Module)
        FreeLibrary(Module);
}

static
BOOLEAN
WmiProvIsNewSource(
    _In_ const MOFRESOURCEINFOW *Info)
{
    PWMIPROV_SOURCE Source;

    if (!Info->ImagePath || !Info->ResourceName)
        return TRUE;

    LIST_FOR_EACH_ENTRY(Source, &WmiProvSources, WMIPROV_SOURCE, Entry)
    {
        if (!_wcsicmp(Source->ImagePath, Info->ImagePath) && !_wcsicmp(Source->ResourceName, Info->ResourceName))
            return FALSE;
    }

    Source = heap_alloc_zero(sizeof(*Source));
    if (!Source)
        return FALSE;
    Source->ImagePath = heap_strdupW(Info->ImagePath);
    Source->ResourceName = heap_strdupW(Info->ResourceName);
    if (!Source->ImagePath || !Source->ResourceName)
    {
        heap_free(Source->ImagePath);
        heap_free(Source->ResourceName);
        heap_free(Source);
        return FALSE;
    }
    list_add_tail(&WmiProvSources, &Source->Entry);
    return TRUE;
}

/**
 * @brief Picks up classes from newly registered MOF resources and builds their tables.
 */
HRESULT
WmiProvRefresh(VOID)
{
    PMOFRESOURCEINFOW Info = NULL;
    ULONG Count = 0;
    ULONG Added = 0;
    ULONG Index, First;
    ULONG Error;

    EnterCriticalSection(&WmiProvLock);

    if (!WmiProvIsInitialized)
    {
        PWMIPROV_TABLE Parameters;

        /* The __PARAMETERS table lists method signatures of every class */
        Parameters = heap_alloc_zero(sizeof(*Parameters));
        if (Parameters)
        {
            Parameters->Table.name = WmiProvParameters;
            Parameters->Table.num_cols = ARRAYSIZE(WmiProvParameterColumns);
            Parameters->Table.columns = WmiProvParameterColumns;
            Parameters->Table.fill = WmiProvFillParameters;
            Parameters->Table.flags = TABLE_FLAG_WMIPROV;
            list_init(&Parameters->Table.entry);
            if (!add_table(WBEMPROX_NAMESPACE_WMI, &Parameters->Table))
                heap_free(Parameters);
        }
    }

    First = WmiProvClassCount;

    Error = WmiMofEnumerateResourcesW(NULL, &Count, &Info);
    if (Error == ERROR_SUCCESS && Info)
    {
        for (Index = 0; Index < Count; Index++)
        {
            if (WmiProvIsNewSource(&Info[Index]))
                WmiProvLoadResource(&Info[Index], &Added);
        }
        WmiFreeBuffer(Info);
    }
    else
    {
        TRACE("WmiMofEnumerateResourcesW failed, error %u\n", Error);
    }

    /* Standard classes fill in whatever the registered resources did not define */
    if (!WmiProvIsInitialized)
    {
        WmiStdEnumerateClasses(WmiProvAddClassRoutine, &Added);
        WmiProvIsInitialized = TRUE;
    }

    for (Index = First; Index < WmiProvClassCount; Index++)
    {
        PWMI_CLASS_DEF Class = WmiProvClasses[Index];
        HRESULT hr = WmiBlockResolveClass(Class, WmiProvLookupRoutine, NULL);

        if (FAILED(hr))
            WARN("cannot resolve %s, %08x\n", debugstr_w(Class->Name), hr);
    }

    for (Index = First; Index < WmiProvClassCount; Index++)
    {
        PWMI_CLASS_DEF Class = WmiProvClasses[Index];
        PWMIPROV_TABLE Template;

        if (!(Class->Flags & WMI_CLASS_RESOLVED))
            continue;

        Template = WmiProvCreateTemplate(Class, WmiProvFillClass);
        if (!Template)
            continue;
        if (!add_table(WBEMPROX_NAMESPACE_WMI, &Template->Table))
        {
            heap_free((void *)Template->Table.columns);
            heap_free(Template);
        }
    }

    LeaveCriticalSection(&WmiProvLock);

    TRACE("%u new classes from %u resources\n", Added, Count);
    return S_OK;
}

const WCHAR *
WmiProvGetSuperclass(
    _In_ const WCHAR *Name)
{
    PWMI_CLASS_DEF Class = WmiProvFindClass(Name);

    return Class ? Class->SuperName : NULL;
}

static
PWMI_METHOD_DEF
WmiProvFindMethod(
    _In_ PWMI_CLASS_DEF Class,
    _In_ PCWSTR Name)
{
    ULONG Index;

    for (; Class; Class = Class->Super)
    {
        for (Index = 0; Index < Class->MethodCount; Index++)
        {
            if (!_wcsicmp(Class->Methods[Index].Name, Name))
                return &Class->Methods[Index];
        }
    }
    return NULL;
}

static
PWMI_CLASS_DEF
WmiProvFindMethodClass(
    _In_ PWMI_CLASS_DEF Class,
    _In_ const WMI_METHOD_DEF *Method)
{
    for (; Class; Class = Class->Super)
    {
        if (Method >= Class->Methods && Method < Class->Methods + Class->MethodCount)
            return Class;
    }
    return NULL;
}

/* Reads a class object into data item order, embedded objects become nested arrays */
static
HRESULT
WmiProvObjectToValues(
    _In_ const WMI_CLASS_DEF *Class,
    _In_opt_ IWbemClassObject *Object,
    _In_ ULONG Depth,
    _Out_writes_(Class->ItemCount) VARIANT *Values);

static
HRESULT
WmiProvUnknownToNested(
    _In_ const WMI_CLASS_DEF *Embedded,
    _In_opt_ IUnknown *Unknown,
    _In_ ULONG Depth,
    _Out_ VARIANT *Result)
{
    IWbemClassObject *Object = NULL;
    SAFEARRAY *Array;
    VARIANT *Members;
    HRESULT hr;

    VariantInit(Result);
    if (!Unknown)
        return S_OK;

    hr = IUnknown_QueryInterface(Unknown, &IID_IWbemClassObject, (void **)&Object);
    if (FAILED(hr))
        return WBEM_E_TYPE_MISMATCH;

    Array = SafeArrayCreateVector(VT_VARIANT, 0, Embedded->ItemCount);
    if (!Array)
    {
        IWbemClassObject_Release(Object);
        return E_OUTOFMEMORY;
    }

    SafeArrayAccessData(Array, (void **)&Members);
    hr = WmiProvObjectToValues(Embedded, Object, Depth + 1, Members);
    SafeArrayUnaccessData(Array);
    IWbemClassObject_Release(Object);
    if (FAILED(hr))
    {
        SafeArrayDestroy(Array);
        return hr;
    }

    V_VT(Result) = VT_ARRAY | VT_VARIANT;
    V_ARRAY(Result) = Array;
    return S_OK;
}

static
HRESULT
WmiProvConvertEmbedded(
    _In_ const WMI_PROPERTY_DEF *Item,
    _In_ VARIANT *Value,
    _In_ ULONG Depth,
    _Out_ VARIANT *Result)
{
    SAFEARRAY *Array;
    VARTYPE Type;
    LONG Index, Upper;
    HRESULT hr;

    VariantInit(Result);
    if (!Item->Embedded || Depth > 8)
        return S_OK;

    if (V_VT(Value) == VT_UNKNOWN || V_VT(Value) == VT_DISPATCH)
        return WmiProvUnknownToNested(Item->Embedded, V_UNKNOWN(Value), Depth, Result);

    if (!(V_VT(Value) & VT_ARRAY) || !V_ARRAY(Value) || !(Item->Type & CIM_FLAG_ARRAY))
        return S_OK;
    if (FAILED(SafeArrayGetVartype(V_ARRAY(Value), &Type)) || FAILED(SafeArrayGetUBound(V_ARRAY(Value), 1, &Upper)))
        return WBEM_E_TYPE_MISMATCH;

    Array = SafeArrayCreateVector(VT_VARIANT, 0, Upper + 1);
    if (!Array)
        return E_OUTOFMEMORY;

    for (Index = 0; Index <= Upper; Index++)
    {
        VARIANT Element, Nested;
        IUnknown *Unknown = NULL;

        VariantInit(&Element);
        if (Type == VT_VARIANT)
        {
            hr = SafeArrayGetElement(V_ARRAY(Value), &Index, &Element);
            if (SUCCEEDED(hr) && (V_VT(&Element) == VT_UNKNOWN || V_VT(&Element) == VT_DISPATCH))
                Unknown = V_UNKNOWN(&Element);
        }
        else if (Type == VT_UNKNOWN || Type == VT_DISPATCH)
        {
            hr = SafeArrayGetElement(V_ARRAY(Value), &Index, &Unknown);
            V_VT(&Element) = VT_UNKNOWN;
            V_UNKNOWN(&Element) = Unknown;
        }
        else
        {
            hr = WBEM_E_TYPE_MISMATCH;
        }

        if (SUCCEEDED(hr))
            hr = WmiProvUnknownToNested(Item->Embedded, Unknown, Depth, &Nested);
        VariantClear(&Element);
        if (SUCCEEDED(hr))
        {
            hr = SafeArrayPutElement(Array, &Index, &Nested);
            VariantClear(&Nested);
        }
        if (FAILED(hr))
        {
            SafeArrayDestroy(Array);
            return hr;
        }
    }

    V_VT(Result) = VT_ARRAY | VT_VARIANT;
    V_ARRAY(Result) = Array;
    return S_OK;
}

static
HRESULT
WmiProvObjectToValues(
    _In_ const WMI_CLASS_DEF *Class,
    _In_opt_ IWbemClassObject *Object,
    _In_ ULONG Depth,
    _Out_writes_(Class->ItemCount) VARIANT *Values)
{
    ULONG Index;
    HRESULT hr;

    for (Index = 0; Index < Class->ItemCount; Index++)
        VariantInit(&Values[Index]);
    if (!Object)
        return S_OK;

    for (Index = 0; Index < Class->ItemCount; Index++)
    {
        const WMI_PROPERTY_DEF *Item = Class->Items[Index];
        VARIANT Value;

        VariantInit(&Value);
        if (FAILED(IWbemClassObject_Get(Object, Item->Name, 0, &Value, NULL, NULL)))
            continue;

        if ((Item->Type & ~CIM_FLAG_ARRAY) == CIM_OBJECT)
        {
            hr = WmiProvConvertEmbedded(Item, &Value, Depth, &Values[Index]);
            VariantClear(&Value);
            if (FAILED(hr))
            {
                WmiBlockClearValues(Class->ItemCount, Values);
                return hr;
            }
            continue;
        }
        Values[Index] = Value;
    }
    return S_OK;
}

/* Extracts the InstanceName key from an object path filter */
static
PWSTR
WmiProvParseKey(
    _In_opt_ PCWSTR Filter)
{
    PCWSTR Value;
    PWSTR Result;
    WCHAR Quote;
    ULONG Length = 0;

    if (!Filter)
        return NULL;

    Value = wcschr(Filter, L'=');
    if (!Value)
        return NULL;
    if ((ULONG)(Value - Filter) != ARRAYSIZE(WmiProvInstanceName) - 1 ||
        _wcsnicmp(Filter, WmiProvInstanceName, Value - Filter))
    {
        return NULL;
    }

    Value++;
    Quote = *Value;
    if (Quote != L'"' && Quote != L'\'')
        return NULL;
    Value++;

    Result = heap_alloc((lstrlenW(Value) + 1) * sizeof(WCHAR));
    if (!Result)
        return NULL;

    while (*Value && *Value != Quote)
    {
        if (Value[0] == L'\\' && Value[1])
            Value++;
        Result[Length++] = *Value++;
    }
    Result[Length] = UNICODE_NULL;
    return Result;
}

/* Wraps decoded output values in variants suitable for IWbemClassObject::Put */
static
HRESULT
WmiProvValueForPut(
    _In_ const WMI_PROPERTY_DEF *Item,
    _In_ VARIANT *Value,
    _Out_ VARIANT *Result)
{
    LONGLONG Stored = 0;
    HRESULT hr;

    VariantInit(Result);
    if ((Item->Type & ~CIM_FLAG_ARRAY) != CIM_OBJECT)
        return VariantCopy(Result, Value);

    hr = WmiProvEmbeddedToValue(Item, Value, &Stored);
    if (FAILED(hr) || !Stored)
        return hr;

    if (Item->Type & CIM_FLAG_ARRAY)
    {
        hr = value_to_variant(CIM_OBJECT | CIM_FLAG_ARRAY, Stored, Result);
        destroy_array((struct array *)(INT_PTR)Stored, CIM_OBJECT);
        return hr;
    }

    V_VT(Result) = VT_UNKNOWN;
    V_UNKNOWN(Result) = (IUnknown *)(INT_PTR)Stored;
    return S_OK;
}

static
HRESULT
WmiProvBuildOutput(
    _In_ PWMI_CLASS_DEF Class,
    _In_ PWMI_METHOD_DEF Method,
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ ULONG Size,
    _Outptr_result_maybenull_ IWbemClassObject **Out)
{
    PWMI_CLASS_DEF Params = Method->OutParams;
    IWbemClassObject *Signature = NULL, *Instance = NULL;
    VARIANT *Values;
    ULONG Index;
    HRESULT hr;

    *Out = NULL;
    if (!Params || !Params->ItemCount)
        return S_OK;

    hr = create_signature(WBEMPROX_NAMESPACE_WMI, Class->Name, Method->Name, PARAM_OUT, &Signature);
    if (FAILED(hr) || !Signature)
        return hr;
    hr = IWbemClassObject_SpawnInstance(Signature, 0, &Instance);
    IWbemClassObject_Release(Signature);
    if (FAILED(hr))
        return hr;

    Values = heap_alloc_zero(Params->ItemCount * sizeof(*Values));
    if (!Values)
    {
        IWbemClassObject_Release(Instance);
        return E_OUTOFMEMORY;
    }

    hr = WmiBlockDecode(Params, Data, Size, Values);
    for (Index = 0; SUCCEEDED(hr) && Index < Params->ItemCount; Index++)
    {
        VARIANT Value;

        if (V_VT(&Values[Index]) == VT_EMPTY)
            continue;
        if (SUCCEEDED(WmiProvValueForPut(Params->Items[Index], &Values[Index], &Value)))
        {
            IWbemClassObject_Put(Instance, Params->Items[Index]->Name, 0, &Value, 0);
            VariantClear(&Value);
        }
    }
    if (hr == S_FALSE)
        hr = S_OK;

    WmiBlockClearValues(Params->ItemCount, Values);
    heap_free(Values);

    if (FAILED(hr))
    {
        IWbemClassObject_Release(Instance);
        return hr;
    }
    *Out = Instance;
    return S_OK;
}

/**
 * @brief Runs a WDM method through WmiExecuteMethodW.
 */
HRESULT
WmiProvExecMethod(
    _In_ const WCHAR *Path,
    _In_ const WCHAR *MethodName,
    _In_opt_ IWbemClassObject *InParams,
    _Outptr_opt_result_maybenull_ IWbemClassObject **OutParams)
{
    PWMI_CLASS_DEF Class, Owner;
    PWMI_METHOD_DEF Method;
    struct path *ObjectPath;
    PWSTR InstanceName = NULL;
    BYTE *Input = NULL, *Output = NULL;
    ULONG InputSize = 0, OutputSize = WMIPROV_METHOD_BUFFER;
    WMIHANDLE Handle;
    VARIANT *Values = NULL;
    ULONG Attempt, Error;
    HRESULT hr;

    if (OutParams)
        *OutParams = NULL;

    hr = parse_path(WBEMPROX_NAMESPACE_WMI, Path, &ObjectPath);
    if (hr != S_OK)
        return hr;

    Class = WmiProvFindClass(ObjectPath->class);
    if (!Class || !(Class->Flags & WMI_CLASS_HAS_GUID))
    {
        hr = WBEM_E_INVALID_CLASS;
        goto Exit;
    }

    Method = WmiProvFindMethod(Class, MethodName);
    if (!Method)
    {
        hr = WBEM_E_INVALID_METHOD;
        goto Exit;
    }
    Owner = WmiProvFindMethodClass(Class, Method);

    InstanceName = WmiProvParseKey(ObjectPath->filter);
    if (!InstanceName)
    {
        hr = WBEM_E_INVALID_OBJECT_PATH;
        goto Exit;
    }

    if (Method->InParams && Method->InParams->ItemCount)
    {
        Values = heap_alloc_zero(Method->InParams->ItemCount * sizeof(*Values));
        if (!Values)
        {
            hr = E_OUTOFMEMORY;
            goto Exit;
        }
        hr = WmiProvObjectToValues(Method->InParams, InParams, 0, Values);
        if (SUCCEEDED(hr))
            hr = WmiBlockEncode(Method->InParams, Values, &Input, &InputSize);
        WmiBlockClearValues(Method->InParams->ItemCount, Values);
        heap_free(Values);
        if (FAILED(hr))
            goto Exit;
    }

    Error = WmiOpenBlock(&Class->Guid, WMIGUID_EXECUTE, &Handle);
    if (Error != ERROR_SUCCESS)
    {
        hr = WmiProvMapError(Error);
        goto Exit;
    }

    for (Attempt = 0; Attempt < WMIPROV_QUERY_ATTEMPTS; Attempt++)
    {
        ULONG Returned = OutputSize;

        Output = heap_alloc_zero(OutputSize);
        if (!Output)
        {
            Error = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }

        Error = WmiExecuteMethodW(Handle, InstanceName, Method->MethodId, InputSize, Input, &Returned, Output);
        if (Error == ERROR_INSUFFICIENT_BUFFER)
        {
            heap_free(Output);
            Output = NULL;
            OutputSize = max(Returned, OutputSize * 2);
            continue;
        }
        OutputSize = min(Returned, OutputSize);
        break;
    }
    WmiCloseBlock(Handle);

    if (Error != ERROR_SUCCESS)
    {
        hr = WmiProvMapError(Error);
        goto Exit;
    }

    if (OutParams)
        hr = WmiProvBuildOutput(Owner ? Owner : Class, Method, Output, OutputSize, OutParams);

Exit:
    BmofFree(Input);
    heap_free(Output);
    heap_free(InstanceName);
    free_path(ObjectPath);
    return hr;
}

/**
 * @brief Writes a full instance back through WmiSetSingleInstanceW.
 */
HRESULT
WmiProvPutInstance(
    _In_ IWbemClassObject *Instance)
{
    PWMI_CLASS_DEF Class;
    VARIANT ClassName, InstanceName;
    VARIANT *Values = NULL;
    BYTE *Data = NULL;
    ULONG Size = 0;
    WMIHANDLE Handle;
    ULONG Error;
    HRESULT hr;

    VariantInit(&ClassName);
    VariantInit(&InstanceName);

    hr = IWbemClassObject_Get(Instance, WmiProvClassProperty, 0, &ClassName, NULL, NULL);
    if (FAILED(hr) || V_VT(&ClassName) != VT_BSTR)
    {
        hr = WBEM_E_INVALID_OBJECT;
        goto Exit;
    }

    Class = WmiProvFindClass(V_BSTR(&ClassName));
    if (!Class || !WmiProvIsDataClass(Class))
    {
        hr = WBEM_E_INVALID_CLASS;
        goto Exit;
    }

    hr = IWbemClassObject_Get(Instance, WmiProvInstanceName, 0, &InstanceName, NULL, NULL);
    if (FAILED(hr) || V_VT(&InstanceName) != VT_BSTR || !V_BSTR(&InstanceName))
    {
        hr = WBEM_E_INVALID_OBJECT;
        goto Exit;
    }

    Values = heap_alloc_zero(max(Class->ItemCount, 1) * sizeof(*Values));
    if (!Values)
    {
        hr = E_OUTOFMEMORY;
        goto Exit;
    }
    hr = WmiProvObjectToValues(Class, Instance, 0, Values);
    if (SUCCEEDED(hr))
        hr = WmiBlockEncode(Class, Values, &Data, &Size);
    WmiBlockClearValues(Class->ItemCount, Values);
    heap_free(Values);
    if (FAILED(hr))
        goto Exit;

    Error = WmiOpenBlock(&Class->Guid, WMIGUID_SET, &Handle);
    if (Error == ERROR_SUCCESS)
    {
        /* The reserved argument carries the WNODE version, which is 1 */
        Error = WmiSetSingleInstanceW(Handle, V_BSTR(&InstanceName), 1, Size, Data);
        WmiCloseBlock(Handle);
    }
    hr = WmiProvMapError(Error);

Exit:
    BmofFree(Data);
    VariantClear(&ClassName);
    VariantClear(&InstanceName);
    return hr;
}

static
VOID
WmiProvReleaseEvent(
    _In_ PWMIPROV_EVENT Event)
{
    if (InterlockedDecrement(&Event->Refs))
        return;

    IWbemObjectSink_Release(Event->Sink);
    heap_free(Event);
}

static
BOOLEAN
WmiProvEventRoutine(
    _In_opt_ PVOID Context,
    _In_reads_opt_(NameLength) PCWSTR Name,
    _In_ ULONG NameLength,
    _In_ ULONG Index,
    _In_reads_bytes_(DataSize) const BYTE *Data,
    _In_ ULONG DataSize)
{
    PWMIPROV_EVENT Event = Context;
    IWbemClassObject *Object;
    WMIPROV_ROW Row;
    VARIANT *Values;
    HRESULT hr;

    Values = heap_alloc_zero(max(Event->Class->ItemCount, 1) * sizeof(*Values));
    if (!Values)
        return FALSE;

    hr = WmiBlockDecode(Event->Class, Data, DataSize, Values);
    if (SUCCEEDED(hr))
    {
        Row.Values = Values;
        Row.Name = Name;
        Row.NameLength = NameLength;
        Row.Index = Index;
        Row.IsInstance = TRUE;
        hr = WmiProvCreateObjects(Event->Class, 1, &Row, &Object);
        if (SUCCEEDED(hr))
        {
            IWbemObjectSink_Indicate(Event->Sink, 1, &Object);
            IWbemClassObject_Release(Object);
        }
    }

    WmiBlockClearValues(Event->Class->ItemCount, Values);
    heap_free(Values);
    return TRUE;
}

static
VOID
WINAPI
WmiProvEventCallback(
    _In_ PWNODE_HEADER Wnode,
    _In_ UINT_PTR NotificationContext)
{
    PWMIPROV_EVENT Event = (PWMIPROV_EVENT)NotificationContext;
    BOOLEAN IsActive;

    if (!Wnode || !Event)
        return;

    EnterCriticalSection(&WmiProvLock);
    IsActive = Event->IsActive;
    if (IsActive)
        InterlockedIncrement(&Event->Refs);
    LeaveCriticalSection(&WmiProvLock);

    if (!IsActive)
        return;

    WmiBlockWalk((const BYTE *)Wnode, Wnode->BufferSize, WmiProvEventRoutine, Event);
    WmiProvReleaseEvent(Event);
}

/* Returns the class named after FROM in an event query */
static
PWMI_CLASS_DEF
WmiProvParseEventQuery(
    _In_ PCWSTR Query)
{
    WCHAR Name[256];
    PCWSTR Cursor = Query;
    ULONG Length = 0;

    while (*Cursor)
    {
        if (!_wcsnicmp(Cursor, L"from", 4) && (Cursor == Query || iswspace(Cursor[-1])) && iswspace(Cursor[4]))
            break;
        Cursor++;
    }
    if (!*Cursor)
        return NULL;

    Cursor += 4;
    while (iswspace(*Cursor))
        Cursor++;
    while (Cursor[Length] && !iswspace(Cursor[Length]) && Length < ARRAYSIZE(Name) - 1)
    {
        Name[Length] = Cursor[Length];
        Length++;
    }
    Name[Length] = UNICODE_NULL;
    return Length ? WmiProvFindClass(Name) : NULL;
}

/**
 * @brief Delivers kernel WMI events of the queried class to a sink.
 */
HRESULT
WmiProvSubscribe(
    _In_ const WCHAR *Query,
    _In_ IWbemObjectSink *Sink,
    _Outptr_ void **Cookie)
{
    PWMI_CLASS_DEF Class;
    PWMIPROV_EVENT Event;
    ULONG Error;

    *Cookie = NULL;

    Class = WmiProvParseEventQuery(Query);
    if (!Class)
        return WBEM_E_INVALID_CLASS;
    if (!(Class->Flags & WMI_CLASS_EVENT) || !(Class->Flags & WMI_CLASS_HAS_GUID))
        return WBEM_E_NOT_EVENT_CLASS;

    Event = heap_alloc_zero(sizeof(*Event));
    if (!Event)
        return E_OUTOFMEMORY;

    Event->Refs = 1;
    Event->IsActive = TRUE;
    Event->Class = Class;
    Event->Sink = Sink;
    IWbemObjectSink_AddRef(Sink);

    Error = WmiNotificationRegistrationW(&Class->Guid,
                                         TRUE,
                                         (PVOID)WmiProvEventCallback,
                                         (ULONG_PTR)Event,
                                         NOTIFICATION_CALLBACK_DIRECT);
    if (Error != ERROR_SUCCESS)
    {
        WmiProvReleaseEvent(Event);
        return WmiProvMapError(Error);
    }

    *Cookie = Event;
    return S_OK;
}

VOID
WmiProvUnsubscribe(
    _In_opt_ void *Cookie)
{
    PWMIPROV_EVENT Event = Cookie;

    if (!Event)
        return;

    EnterCriticalSection(&WmiProvLock);
    Event->IsActive = FALSE;
    LeaveCriticalSection(&WmiProvLock);

    WmiNotificationRegistrationW(&Event->Class->Guid,
                                 FALSE,
                                 (PVOID)WmiProvEventCallback,
                                 (ULONG_PTR)Event,
                                 NOTIFICATION_CALLBACK_DIRECT);
    WmiProvReleaseEvent(Event);
}

typedef struct _WMIPROV_EVENT_ENUM
{
    IEnumWbemClassObject IEnumWbemClassObject_iface;
    IWbemObjectSink IWbemObjectSink_iface;
    LONG Refs;
    LONG EnumRefs;
    CRITICAL_SECTION Lock;
    HANDLE Ready;
    struct list Queue;
    void *Cookie;
} WMIPROV_EVENT_ENUM, *PWMIPROV_EVENT_ENUM;

typedef struct _WMIPROV_QUEUED_EVENT
{
    struct list Entry;
    IWbemClassObject *Object;
} WMIPROV_QUEUED_EVENT, *PWMIPROV_QUEUED_EVENT;

static
inline
PWMIPROV_EVENT_ENUM
WmiProvEventEnumFromEnum(
    _In_ IEnumWbemClassObject *Iface)
{
    return CONTAINING_RECORD(Iface, WMIPROV_EVENT_ENUM, IEnumWbemClassObject_iface);
}

static
inline
PWMIPROV_EVENT_ENUM
WmiProvEventEnumFromSink(
    _In_ IWbemObjectSink *Iface)
{
    return CONTAINING_RECORD(Iface, WMIPROV_EVENT_ENUM, IWbemObjectSink_iface);
}

static
VOID
WmiProvReleaseEventEnum(
    _In_ PWMIPROV_EVENT_ENUM Enum)
{
    PWMIPROV_QUEUED_EVENT Queued, Next;

    if (InterlockedDecrement(&Enum->Refs))
        return;

    LIST_FOR_EACH_ENTRY_SAFE(Queued, Next, &Enum->Queue, WMIPROV_QUEUED_EVENT, Entry)
    {
        list_remove(&Queued->Entry);
        IWbemClassObject_Release(Queued->Object);
        heap_free(Queued);
    }
    CloseHandle(Enum->Ready);
    DeleteCriticalSection(&Enum->Lock);
    heap_free(Enum);
}

/* Takes the oldest queued event, or NULL when the queue is empty */
static
IWbemClassObject *
WmiProvPopEvent(
    _In_ PWMIPROV_EVENT_ENUM Enum)
{
    PWMIPROV_QUEUED_EVENT Queued = NULL;
    IWbemClassObject *Object = NULL;
    struct list *Head;

    EnterCriticalSection(&Enum->Lock);
    Head = list_head(&Enum->Queue);
    if (Head)
    {
        Queued = LIST_ENTRY(Head, WMIPROV_QUEUED_EVENT, Entry);
        list_remove(&Queued->Entry);
    }
    if (list_empty(&Enum->Queue))
        ResetEvent(Enum->Ready);
    LeaveCriticalSection(&Enum->Lock);

    if (Queued)
    {
        Object = Queued->Object;
        heap_free(Queued);
    }
    return Object;
}

static
HRESULT
WINAPI
WmiProvEventEnum_QueryInterface(
    _In_ IEnumWbemClassObject *Iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    if (IsEqualGUID(Riid, &IID_IEnumWbemClassObject) || IsEqualGUID(Riid, &IID_IUnknown))
    {
        *Object = Iface;
        IEnumWbemClassObject_AddRef(Iface);
        return S_OK;
    }
    if (IsEqualGUID(Riid, &IID_IClientSecurity))
    {
        *Object = &client_security;
        return S_OK;
    }
    *Object = NULL;
    return E_NOINTERFACE;
}

static
ULONG
WINAPI
WmiProvEventEnum_AddRef(
    _In_ IEnumWbemClassObject *Iface)
{
    return InterlockedIncrement(&WmiProvEventEnumFromEnum(Iface)->EnumRefs);
}

static
ULONG
WINAPI
WmiProvEventEnum_Release(
    _In_ IEnumWbemClassObject *Iface)
{
    PWMIPROV_EVENT_ENUM Enum = WmiProvEventEnumFromEnum(Iface);
    LONG Refs = InterlockedDecrement(&Enum->EnumRefs);

    if (!Refs)
    {
        /* Stops delivery, which drops the reference held through the sink */
        WmiProvUnsubscribe(Enum->Cookie);
        WmiProvReleaseEventEnum(Enum);
    }
    return Refs;
}

static
HRESULT
WINAPI
WmiProvEventEnum_Reset(
    _In_ IEnumWbemClassObject *Iface)
{
    UNREFERENCED_PARAMETER(Iface);
    return WBEM_S_NO_ERROR;
}

static
HRESULT
WINAPI
WmiProvEventEnum_Next(
    _In_ IEnumWbemClassObject *Iface,
    _In_ LONG Timeout,
    _In_ ULONG Count,
    _Out_writes_to_(Count, *Returned) IWbemClassObject **Objects,
    _Out_ ULONG *Returned)
{
    PWMIPROV_EVENT_ENUM Enum = WmiProvEventEnumFromEnum(Iface);

    if (!Objects || !Returned)
        return WBEM_E_INVALID_PARAMETER;

    *Returned = 0;
    while (*Returned < Count)
    {
        IWbemClassObject *Object = WmiProvPopEvent(Enum);

        if (Object)
        {
            Objects[(*Returned)++] = Object;
            continue;
        }
        if (*Returned)
            break;
        if (WaitForSingleObject(Enum->Ready, (Timeout == WBEM_INFINITE) ? INFINITE : (DWORD)Timeout) != WAIT_OBJECT_0)
            return WBEM_S_TIMEDOUT;
    }
    return (*Returned == Count) ? WBEM_S_NO_ERROR : WBEM_S_TIMEDOUT;
}

static
HRESULT
WINAPI
WmiProvEventEnum_NextAsync(
    _In_ IEnumWbemClassObject *Iface,
    _In_ ULONG Count,
    _In_ IWbemObjectSink *Sink)
{
    UNREFERENCED_PARAMETER(Iface);
    UNREFERENCED_PARAMETER(Count);
    UNREFERENCED_PARAMETER(Sink);
    return WBEM_E_NOT_SUPPORTED;
}

static
HRESULT
WINAPI
WmiProvEventEnum_Clone(
    _In_ IEnumWbemClassObject *Iface,
    _Outptr_ IEnumWbemClassObject **Enum)
{
    UNREFERENCED_PARAMETER(Iface);
    *Enum = NULL;
    return WBEM_E_NOT_SUPPORTED;
}

static
HRESULT
WINAPI
WmiProvEventEnum_Skip(
    _In_ IEnumWbemClassObject *Iface,
    _In_ LONG Timeout,
    _In_ ULONG Count)
{
    IWbemClassObject *Object;
    ULONG Returned;
    HRESULT hr = WBEM_S_NO_ERROR;

    while (Count-- && hr == WBEM_S_NO_ERROR)
    {
        hr = WmiProvEventEnum_Next(Iface, Timeout, 1, &Object, &Returned);
        if (Returned)
            IWbemClassObject_Release(Object);
    }
    return hr;
}

static const IEnumWbemClassObjectVtbl WmiProvEventEnumVtbl =
{
    WmiProvEventEnum_QueryInterface,
    WmiProvEventEnum_AddRef,
    WmiProvEventEnum_Release,
    WmiProvEventEnum_Reset,
    WmiProvEventEnum_Next,
    WmiProvEventEnum_NextAsync,
    WmiProvEventEnum_Clone,
    WmiProvEventEnum_Skip
};

static
HRESULT
WINAPI
WmiProvEventSink_QueryInterface(
    _In_ IWbemObjectSink *Iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    if (IsEqualGUID(Riid, &IID_IWbemObjectSink) || IsEqualGUID(Riid, &IID_IUnknown))
    {
        *Object = Iface;
        IWbemObjectSink_AddRef(Iface);
        return S_OK;
    }
    *Object = NULL;
    return E_NOINTERFACE;
}

static
ULONG
WINAPI
WmiProvEventSink_AddRef(
    _In_ IWbemObjectSink *Iface)
{
    return InterlockedIncrement(&WmiProvEventEnumFromSink(Iface)->Refs);
}

static
ULONG
WINAPI
WmiProvEventSink_Release(
    _In_ IWbemObjectSink *Iface)
{
    PWMIPROV_EVENT_ENUM Enum = WmiProvEventEnumFromSink(Iface);
    LONG Refs = Enum->Refs - 1;

    WmiProvReleaseEventEnum(Enum);
    return Refs;
}

static
HRESULT
WINAPI
WmiProvEventSink_Indicate(
    _In_ IWbemObjectSink *Iface,
    _In_ LONG Count,
    _In_reads_(Count) IWbemClassObject **Objects)
{
    PWMIPROV_EVENT_ENUM Enum = WmiProvEventEnumFromSink(Iface);
    LONG Index;

    EnterCriticalSection(&Enum->Lock);
    for (Index = 0; Index < Count; Index++)
    {
        PWMIPROV_QUEUED_EVENT Queued = heap_alloc(sizeof(*Queued));

        if (!Queued)
            break;
        Queued->Object = Objects[Index];
        IWbemClassObject_AddRef(Queued->Object);
        list_add_tail(&Enum->Queue, &Queued->Entry);
    }
    if (!list_empty(&Enum->Queue))
        SetEvent(Enum->Ready);
    LeaveCriticalSection(&Enum->Lock);
    return WBEM_S_NO_ERROR;
}

static
HRESULT
WINAPI
WmiProvEventSink_SetStatus(
    _In_ IWbemObjectSink *Iface,
    _In_ LONG Flags,
    _In_ HRESULT Result,
    _In_opt_ BSTR Parameter,
    _In_opt_ IWbemClassObject *Object)
{
    UNREFERENCED_PARAMETER(Iface);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(Result);
    UNREFERENCED_PARAMETER(Parameter);
    UNREFERENCED_PARAMETER(Object);
    return WBEM_S_NO_ERROR;
}

static const IWbemObjectSinkVtbl WmiProvEventSinkVtbl =
{
    WmiProvEventSink_QueryInterface,
    WmiProvEventSink_AddRef,
    WmiProvEventSink_Release,
    WmiProvEventSink_Indicate,
    WmiProvEventSink_SetStatus
};

/**
 * @brief Semisynchronous event query, Next blocks until an event arrives or times out.
 */
HRESULT
WmiProvCreateEventEnum(
    _In_ const WCHAR *Query,
    _Outptr_ IEnumWbemClassObject **Result)
{
    PWMIPROV_EVENT_ENUM Enum;
    HRESULT hr;

    *Result = NULL;

    Enum = heap_alloc_zero(sizeof(*Enum));
    if (!Enum)
        return E_OUTOFMEMORY;

    Enum->Ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Enum->Ready)
    {
        heap_free(Enum);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    Enum->IEnumWbemClassObject_iface.lpVtbl = &WmiProvEventEnumVtbl;
    Enum->IWbemObjectSink_iface.lpVtbl = &WmiProvEventSinkVtbl;
    Enum->Refs = 1;
    Enum->EnumRefs = 1;
    InitializeCriticalSection(&Enum->Lock);
    list_init(&Enum->Queue);

    hr = WmiProvSubscribe(Query, &Enum->IWbemObjectSink_iface, &Enum->Cookie);
    if (FAILED(hr))
    {
        WmiProvReleaseEventEnum(Enum);
        return hr;
    }

    *Result = &Enum->IEnumWbemClassObject_iface;
    return S_OK;
}

static
inline
PWMIPROV_QUALIFIER_SET
WmiProvQualifierSetFromIface(
    _In_ IWbemQualifierSet *Iface)
{
    return CONTAINING_RECORD(Iface, WMIPROV_QUALIFIER_SET, IWbemQualifierSet_iface);
}

static
HRESULT
WINAPI
WmiProvQualifierSet_QueryInterface(
    _In_ IWbemQualifierSet *Iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    if (IsEqualGUID(Riid, &IID_IWbemQualifierSet) || IsEqualGUID(Riid, &IID_IUnknown))
    {
        *Object = Iface;
        IWbemQualifierSet_AddRef(Iface);
        return S_OK;
    }
    *Object = NULL;
    return E_NOINTERFACE;
}

static
ULONG
WINAPI
WmiProvQualifierSet_AddRef(
    _In_ IWbemQualifierSet *Iface)
{
    return InterlockedIncrement(&WmiProvQualifierSetFromIface(Iface)->Refs);
}

static
ULONG
WINAPI
WmiProvQualifierSet_Release(
    _In_ IWbemQualifierSet *Iface)
{
    PWMIPROV_QUALIFIER_SET Set = WmiProvQualifierSetFromIface(Iface);
    LONG Refs = InterlockedDecrement(&Set->Refs);

    if (!Refs)
        heap_free(Set);
    return Refs;
}

static
HRESULT
WINAPI
WmiProvQualifierSet_Get(
    _In_ IWbemQualifierSet *Iface,
    _In_ LPCWSTR Name,
    _In_ LONG Flags,
    _Out_opt_ VARIANT *Value,
    _Out_opt_ LONG *Flavor)
{
    PWMIPROV_QUALIFIER_SET Set = WmiProvQualifierSetFromIface(Iface);
    const WMI_QUALIFIER_DEF *Qualifier;

    UNREFERENCED_PARAMETER(Flags);

    if (!Name)
        return WBEM_E_INVALID_PARAMETER;

    Qualifier = BmofFindQualifier(Set->Count, Set->Items, Name);
    if (!Qualifier)
        return WBEM_E_NOT_FOUND;

    if (Flavor)
        *Flavor = Qualifier->Flavor;
    if (Value)
    {
        VariantInit(Value);
        return VariantCopy(Value, (VARIANT *)&Qualifier->Value);
    }
    return S_OK;
}

static
HRESULT
WINAPI
WmiProvQualifierSet_Put(
    _In_ IWbemQualifierSet *Iface,
    _In_ LPCWSTR Name,
    _In_ VARIANT *Value,
    _In_ LONG Flavor)
{
    UNREFERENCED_PARAMETER(Iface);
    UNREFERENCED_PARAMETER(Name);
    UNREFERENCED_PARAMETER(Value);
    UNREFERENCED_PARAMETER(Flavor);
    return WBEM_E_READ_ONLY;
}

static
HRESULT
WINAPI
WmiProvQualifierSet_Delete(
    _In_ IWbemQualifierSet *Iface,
    _In_ LPCWSTR Name)
{
    UNREFERENCED_PARAMETER(Iface);
    UNREFERENCED_PARAMETER(Name);
    return WBEM_E_READ_ONLY;
}

static
HRESULT
WINAPI
WmiProvQualifierSet_GetNames(
    _In_ IWbemQualifierSet *Iface,
    _In_ LONG Flags,
    _Out_ SAFEARRAY **Names)
{
    PWMIPROV_QUALIFIER_SET Set = WmiProvQualifierSetFromIface(Iface);
    SAFEARRAY *Array;
    LONG Index;

    UNREFERENCED_PARAMETER(Flags);

    Array = SafeArrayCreateVector(VT_BSTR, 0, Set->Count);
    if (!Array)
        return E_OUTOFMEMORY;

    for (Index = 0; Index < (LONG)Set->Count; Index++)
    {
        BSTR Name = SysAllocString(Set->Items[Index].Name);
        HRESULT hr = Name ? SafeArrayPutElement(Array, &Index, Name) : E_OUTOFMEMORY;

        SysFreeString(Name);
        if (FAILED(hr))
        {
            SafeArrayDestroy(Array);
            return hr;
        }
    }

    *Names = Array;
    return S_OK;
}

static
HRESULT
WINAPI
WmiProvQualifierSet_BeginEnumeration(
    _In_ IWbemQualifierSet *Iface,
    _In_ LONG Flags)
{
    UNREFERENCED_PARAMETER(Flags);
    WmiProvQualifierSetFromIface(Iface)->Cursor = 0;
    return S_OK;
}

static
HRESULT
WINAPI
WmiProvQualifierSet_Next(
    _In_ IWbemQualifierSet *Iface,
    _In_ LONG Flags,
    _Out_opt_ BSTR *Name,
    _Out_opt_ VARIANT *Value,
    _Out_opt_ LONG *Flavor)
{
    PWMIPROV_QUALIFIER_SET Set = WmiProvQualifierSetFromIface(Iface);
    const WMI_QUALIFIER_DEF *Qualifier;

    UNREFERENCED_PARAMETER(Flags);

    if (Set->Cursor >= Set->Count)
        return WBEM_S_NO_MORE_DATA;

    Qualifier = &Set->Items[Set->Cursor++];
    if (Name && !(*Name = SysAllocString(Qualifier->Name)))
        return E_OUTOFMEMORY;
    if (Flavor)
        *Flavor = Qualifier->Flavor;
    if (Value)
    {
        VariantInit(Value);
        return VariantCopy(Value, (VARIANT *)&Qualifier->Value);
    }
    return S_OK;
}

static
HRESULT
WINAPI
WmiProvQualifierSet_EndEnumeration(
    _In_ IWbemQualifierSet *Iface)
{
    WmiProvQualifierSetFromIface(Iface)->Cursor = 0;
    return S_OK;
}

static const IWbemQualifierSetVtbl WmiProvQualifierSetVtbl =
{
    WmiProvQualifierSet_QueryInterface,
    WmiProvQualifierSet_AddRef,
    WmiProvQualifierSet_Release,
    WmiProvQualifierSet_Get,
    WmiProvQualifierSet_Put,
    WmiProvQualifierSet_Delete,
    WmiProvQualifierSet_GetNames,
    WmiProvQualifierSet_BeginEnumeration,
    WmiProvQualifierSet_Next,
    WmiProvQualifierSet_EndEnumeration
};

/**
 * @brief Exposes class, property, parameter or method qualifiers of a root\wmi class.
 */
HRESULT
WmiProvCreateQualifierSet(
    _In_opt_ const WCHAR *ClassName,
    _In_opt_ const WCHAR *Member,
    _Outptr_ IWbemQualifierSet **QualifierSet)
{
    PWMIPROV_QUALIFIER_SET Set;
    PWMI_CLASS_DEF Class = WmiProvFindClass(ClassName);
    const WMI_QUALIFIER_DEF *Items = NULL;
    ULONG Count = 0;
    ULONG Index;

    *QualifierSet = NULL;

    if (Class && !Member)
    {
        Items = Class->Qualifiers;
        Count = Class->QualifierCount;
    }
    else if (Class)
    {
        PWMI_METHOD_DEF Method;

        for (Index = 0; Index < Class->AllCount; Index++)
        {
            if (!_wcsicmp(Class->All[Index]->Name, Member))
            {
                Items = Class->All[Index]->Qualifiers;
                Count = Class->All[Index]->QualifierCount;
                break;
            }
        }
        if (Index == Class->AllCount)
        {
            Method = WmiProvFindMethod(Class, Member);
            if (!Method)
                return WBEM_E_NOT_FOUND;
            Items = Method->Qualifiers;
            Count = Method->QualifierCount;
        }
    }

    Set = heap_alloc_zero(sizeof(*Set));
    if (!Set)
        return E_OUTOFMEMORY;

    Set->IWbemQualifierSet_iface.lpVtbl = &WmiProvQualifierSetVtbl;
    Set->Refs = 1;
    Set->Items = Items;
    Set->Count = Count;
    *QualifierSet = &Set->IWbemQualifierSet_iface;
    return S_OK;
}
