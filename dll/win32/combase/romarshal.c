/*
 * PROJECT:     ReactOS COM Base
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Marshaling Windows Runtime objects by value through a property set
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * An object that implements IPropertySetMarshal crosses standard marshaling as a
 * custom OBJREF with CLSID_RtMarshalByValue. After the OBJREF header come the
 * UINT32 length and characters of its runtime class name, then the UINT32 length
 * and bytes of its property set as RoCreatePropertySetSerializer writes it. The
 * receiver gives a new property set to the IPropertySetUnmarshal factory of the class.
 */

#include <stdlib.h>

#define COBJMACROS
#include "objbase.h"
#include "winstring.h"

#include "combase_private.h"
#include "roregistration.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ole);

/* MSHLFLAGS bits that are never marshaled by value */
#define RT_MSHLFLAGS_NO_VALUE 0x30

/* Bounds on what a by value OBJREF may claim */
#define RT_MAX_VALUE_CLASS_NAME 1024
#define RT_MAX_VALUE_DATA (16 * 1024 * 1024)

const CLSID CLSID_RtMarshalByValue = { 0x0000035a, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

typedef HRESULT (WINAPI *PFN_CREATE_PROPERTY_SET_SERIALIZER)(IRtPropertySetSerializer **);

/* BUFFER ********************************************************************/

/* A read only buffer over bytes taken from a marshal stream */
typedef struct _RT_VALUE_BUFFER
{
    IRtBuffer IRtBuffer_iface;
    IRtBufferByteAccess IRtBufferByteAccess_iface;
    LONG RefCount;
    UINT32 Length;
    BYTE Data[ANYSIZE_ARRAY];
} RT_VALUE_BUFFER, *PRT_VALUE_BUFFER;

static inline PRT_VALUE_BUFFER impl_from_IRtBuffer(IRtBuffer *iface)
{
    return CONTAINING_RECORD(iface, RT_VALUE_BUFFER, IRtBuffer_iface);
}

static inline PRT_VALUE_BUFFER impl_from_IRtBufferByteAccess(IRtBufferByteAccess *iface)
{
    return CONTAINING_RECORD(iface, RT_VALUE_BUFFER, IRtBufferByteAccess_iface);
}

static
HRESULT
STDMETHODCALLTYPE
ValueBuffer_QueryInterface(
    _In_ IRtBuffer *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    PRT_VALUE_BUFFER Buffer = impl_from_IRtBuffer(iface);

    if (IsEqualIID(Riid, &IID_IUnknown) ||
        IsEqualIID(Riid, &IID_IInspectable) ||
        IsEqualIID(Riid, &IID_IRtBuffer) ||
        IsEqualIID(Riid, &IID_IAgileObject))
    {
        *Object = &Buffer->IRtBuffer_iface;
    }
    else if (IsEqualIID(Riid, &IID_IRtBufferByteAccess))
    {
        *Object = &Buffer->IRtBufferByteAccess_iface;
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
ValueBuffer_AddRef(
    _In_ IRtBuffer *iface)
{
    return InterlockedIncrement(&impl_from_IRtBuffer(iface)->RefCount);
}

static
ULONG
STDMETHODCALLTYPE
ValueBuffer_Release(
    _In_ IRtBuffer *iface)
{
    PRT_VALUE_BUFFER Buffer = impl_from_IRtBuffer(iface);
    ULONG Count = InterlockedDecrement(&Buffer->RefCount);

    if (!Count)
        free(Buffer);
    return Count;
}

static
HRESULT
STDMETHODCALLTYPE
ValueBuffer_GetIids(
    _In_ IRtBuffer *iface,
    _Out_ ULONG *Count,
    _Outptr_ IID **Iids)
{
    static const IID *const Implemented[] = { &IID_IRtBuffer };

    return RtCopyIids(ARRAYSIZE(Implemented), Implemented, Count, Iids);
}

static
HRESULT
STDMETHODCALLTYPE
ValueBuffer_GetRuntimeClassName(
    _In_ IRtBuffer *iface,
    _Out_ HSTRING *Name)
{
    return WindowsCreateString(L"Windows.Storage.Streams.Buffer",
                               ARRAYSIZE(L"Windows.Storage.Streams.Buffer") - 1,
                               Name);
}

static
HRESULT
STDMETHODCALLTYPE
ValueBuffer_GetTrustLevel(
    _In_ IRtBuffer *iface,
    _Out_ TrustLevel *Level)
{
    *Level = BaseTrust;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ValueBuffer_get_Capacity(
    _In_ IRtBuffer *iface,
    _Out_ UINT32 *Capacity)
{
    *Capacity = impl_from_IRtBuffer(iface)->Length;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ValueBuffer_get_Length(
    _In_ IRtBuffer *iface,
    _Out_ UINT32 *Length)
{
    *Length = impl_from_IRtBuffer(iface)->Length;
    return S_OK;
}

static
HRESULT
STDMETHODCALLTYPE
ValueBuffer_put_Length(
    _In_ IRtBuffer *iface,
    _In_ UINT32 Length)
{
    PRT_VALUE_BUFFER Buffer = impl_from_IRtBuffer(iface);

    if (Length > Buffer->Length)
        return E_INVALIDARG;
    Buffer->Length = Length;
    return S_OK;
}

static const IRtBufferVtbl ValueBufferVtbl =
{
    ValueBuffer_QueryInterface,
    ValueBuffer_AddRef,
    ValueBuffer_Release,
    ValueBuffer_GetIids,
    ValueBuffer_GetRuntimeClassName,
    ValueBuffer_GetTrustLevel,
    ValueBuffer_get_Capacity,
    ValueBuffer_get_Length,
    ValueBuffer_put_Length,
};

static
HRESULT
STDMETHODCALLTYPE
ValueBufferBytes_QueryInterface(
    _In_ IRtBufferByteAccess *iface,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    return ValueBuffer_QueryInterface(&impl_from_IRtBufferByteAccess(iface)->IRtBuffer_iface, Riid, Object);
}

static
ULONG
STDMETHODCALLTYPE
ValueBufferBytes_AddRef(
    _In_ IRtBufferByteAccess *iface)
{
    return ValueBuffer_AddRef(&impl_from_IRtBufferByteAccess(iface)->IRtBuffer_iface);
}

static
ULONG
STDMETHODCALLTYPE
ValueBufferBytes_Release(
    _In_ IRtBufferByteAccess *iface)
{
    return ValueBuffer_Release(&impl_from_IRtBufferByteAccess(iface)->IRtBuffer_iface);
}

static
HRESULT
STDMETHODCALLTYPE
ValueBufferBytes_Buffer(
    _In_ IRtBufferByteAccess *iface,
    _Outptr_ BYTE **Data)
{
    *Data = impl_from_IRtBufferByteAccess(iface)->Data;
    return S_OK;
}

static const IRtBufferByteAccessVtbl ValueBufferBytesVtbl =
{
    ValueBufferBytes_QueryInterface,
    ValueBufferBytes_AddRef,
    ValueBufferBytes_Release,
    ValueBufferBytes_Buffer,
};

/* PROPERTY SETS *************************************************************/

static
HRESULT
RtCreatePropertySet(
    _Outptr_ IRtPropertySet **Set)
{
    static const WCHAR ClassName[] = L"Windows.Foundation.Collections.PropertySet";
    IInspectable *Instance;
    HSTRING_HEADER Header;
    HSTRING Name;
    HRESULT Status;

    *Set = NULL;
    Status = WindowsCreateStringReference(ClassName, ARRAYSIZE(ClassName) - 1, &Header, &Name);
    if (FAILED(Status))
        return Status;

    Status = RoActivateInstance(Name, &Instance);
    if (FAILED(Status))
        return Status;

    Status = IInspectable_QueryInterface(Instance, &IID_IRtPropertySet, (void **)Set);
    IInspectable_Release(Instance);
    return Status;
}

static
HRESULT
RtCreateSerializer(
    _Outptr_ IRtPropertySetSerializer **Serializer)
{
    static PFN_CREATE_PROPERTY_SET_SERIALIZER CreateSerializer;
    HMODULE Module;

    *Serializer = NULL;
    if (!CreateSerializer)
    {
        Module = LoadLibraryW(L"wintypes.dll");
        if (!Module)
            return HRESULT_FROM_WIN32(GetLastError());
        CreateSerializer = (PFN_CREATE_PROPERTY_SET_SERIALIZER)GetProcAddress(Module, "RoCreatePropertySetSerializer");
        if (!CreateSerializer)
            return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }
    return CreateSerializer(Serializer);
}

/**
 * @brief
 * Tells whether the serializer can write a value: property values and nested
 * property sets. Other objects would need a stream that carries objects.
 */
static
BOOL
RtIsSerializableValue(
    _In_opt_ IInspectable *Value)
{
    IRtPropertyValue *Property;
    IRtPropertySet *Set;
    RtPropertyType Type;

    if (!Value)
        return TRUE;

    if (SUCCEEDED(IInspectable_QueryInterface(Value, &IID_IRtPropertyValue, (void **)&Property)))
    {
        if (FAILED(IRtPropertyValue_get_Type(Property, &Type)))
            Type = RtPropertyType_Inspectable;
        IRtPropertyValue_Release(Property);
        return Type != RtPropertyType_Inspectable;
    }

    if (SUCCEEDED(IInspectable_QueryInterface(Value, &IID_IRtPropertySet, (void **)&Set)))
    {
        IRtPropertySet_Release(Set);
        return TRUE;
    }
    return FALSE;
}

/* Copies the entries the serializer can carry into a new set */
static
HRESULT
RtCopySerializableEntries(
    _In_ IRtPropertySet *Source,
    _Outptr_ IRtPropertySet **Copy)
{
    IRtKeyValueIterable *Entries = NULL;
    IRtKeyValueIterator *Cursor = NULL;
    IRtKeyValuePair *Pair;
    IRtMap *Target = NULL;
    IInspectable *Value;
    boolean More = FALSE, Replaced;
    HSTRING Key;
    HRESULT Status;

    *Copy = NULL;
    Status = RtCreatePropertySet(Copy);
    if (SUCCEEDED(Status))
        Status = IRtPropertySet_QueryInterface(*Copy, &IID_IRtMap, (void **)&Target);
    if (SUCCEEDED(Status))
        Status = IRtPropertySet_QueryInterface(Source, &IID_IRtKeyValueIterable, (void **)&Entries);
    if (SUCCEEDED(Status))
        Status = IRtKeyValueIterable_First(Entries, &Cursor);
    if (SUCCEEDED(Status))
        Status = IRtKeyValueIterator_get_HasCurrent(Cursor, &More);

    while (SUCCEEDED(Status) && More)
    {
        Status = IRtKeyValueIterator_get_Current(Cursor, &Pair);
        if (FAILED(Status))
            break;

        Key = NULL;
        Value = NULL;
        Status = IRtKeyValuePair_get_Key(Pair, &Key);
        if (SUCCEEDED(Status))
            Status = IRtKeyValuePair_get_Value(Pair, &Value);
        if (SUCCEEDED(Status))
        {
            if (RtIsSerializableValue(Value))
                Status = IRtMap_Insert(Target, Key, Value, &Replaced);
            else
                FIXME("Value %s is an object and does not cross by value\n", debugstr_hstring(Key));
        }

        if (Value)
            IInspectable_Release(Value);
        WindowsDeleteString(Key);
        IRtKeyValuePair_Release(Pair);

        if (SUCCEEDED(Status))
            Status = IRtKeyValueIterator_MoveNext(Cursor, &More);
    }

    if (Cursor)
        IRtKeyValueIterator_Release(Cursor);
    if (Entries)
        IRtKeyValueIterable_Release(Entries);
    if (Target)
        IRtMap_Release(Target);
    if (FAILED(Status) && *Copy)
    {
        IRtPropertySet_Release(*Copy);
        *Copy = NULL;
    }
    return Status;
}

/**
 * @brief
 * Asks the object for its property set and serializes it.
 */
static
HRESULT
RtSerializeValue(
    _In_ IUnknown *Object,
    _In_ IPropertySetMarshal *Marshal,
    _Out_ HSTRING *ClassName,
    _Outptr_ IRtBuffer **Data)
{
    IRtPropertySetSerializer *Serializer = NULL;
    IRtPropertySet *Set = NULL, *Carried = NULL;
    IInspectable *Inspectable;
    HRESULT Status;

    *ClassName = NULL;
    *Data = NULL;

    Status = IUnknown_QueryInterface(Object, &IID_IInspectable, (void **)&Inspectable);
    if (FAILED(Status))
        return Status;
    Status = IInspectable_GetRuntimeClassName(Inspectable, ClassName);
    IInspectable_Release(Inspectable);
    if (FAILED(Status))
        return Status;

    Status = RtCreatePropertySet(&Set);
    if (SUCCEEDED(Status))
        Status = IPropertySetMarshal_MarshalObjectToPropertySet(Marshal, Set);
    if (SUCCEEDED(Status))
        Status = RtCopySerializableEntries(Set, &Carried);
    if (SUCCEEDED(Status))
        Status = RtCreateSerializer(&Serializer);
    if (SUCCEEDED(Status))
        Status = IRtPropertySetSerializer_Serialize(Serializer, Carried, Data);

    if (Serializer)
        IRtPropertySetSerializer_Release(Serializer);
    if (Carried)
        IRtPropertySet_Release(Carried);
    if (Set)
        IRtPropertySet_Release(Set);
    if (FAILED(Status))
    {
        WindowsDeleteString(*ClassName);
        *ClassName = NULL;
    }
    return Status;
}

static
HRESULT
RtGetBufferBytes(
    _In_ IRtBuffer *Buffer,
    _Out_ BYTE **Bytes,
    _Out_ UINT32 *Length)
{
    IRtBufferByteAccess *Access;
    HRESULT Status;

    *Bytes = NULL;
    Status = IRtBuffer_get_Length(Buffer, Length);
    if (FAILED(Status))
        return Status;

    Status = IRtBuffer_QueryInterface(Buffer, &IID_IRtBufferByteAccess, (void **)&Access);
    if (FAILED(Status))
        return Status;
    Status = IRtBufferByteAccess_Buffer(Access, Bytes);
    IRtBufferByteAccess_Release(Access);
    return Status;
}

/* EXPORTED TO MARSHAL.C *****************************************************/

/**
 * @brief
 * Decides whether standard marshaling of an object goes by value: the object
 * offers IPropertySetMarshal and is not agile object passed in process.
 */
BOOL
RtUseMarshalByValue(
    _In_ IUnknown *Object,
    _In_ DWORD DestContext,
    _In_ DWORD Flags,
    _Outptr_result_maybenull_ IPropertySetMarshal **Marshal)
{
    IUnknown *Agile;

    *Marshal = NULL;
    if ((Flags & RT_MSHLFLAGS_NO_VALUE) || DestContext == MSHCTX_DIFFERENTMACHINE)
        return FALSE;

    if (DestContext == MSHCTX_INPROC &&
        SUCCEEDED(IUnknown_QueryInterface(Object, &IID_IAgileObject, (void **)&Agile)))
    {
        IUnknown_Release(Agile);
        return FALSE;
    }

    return SUCCEEDED(IUnknown_QueryInterface(Object, &IID_IPropertySetMarshal, (void **)Marshal));
}

HRESULT
RtGetMarshalByValueSize(
    _In_ REFIID Riid,
    _In_ IUnknown *Object,
    _In_ IPropertySetMarshal *Marshal,
    _Out_ ULONG *Size)
{
    IRtBuffer *Data;
    HSTRING ClassName;
    UINT32 Length;
    BYTE *Bytes;
    HRESULT Status;

    *Size = 0;
    Status = RtSerializeValue(Object, Marshal, &ClassName, &Data);
    if (FAILED(Status))
        return Status;

    Status = RtGetBufferBytes(Data, &Bytes, &Length);
    if (SUCCEEDED(Status))
    {
        *Size = FIELD_OFFSET(OBJREF, u_objref.u_custom.pData) +
                sizeof(UINT32) + WindowsGetStringLen(ClassName) * sizeof(WCHAR) +
                sizeof(UINT32) + Length;
    }

    IRtBuffer_Release(Data);
    WindowsDeleteString(ClassName);
    return Status;
}

/**
 * @brief
 * Writes the complete by value OBJREF of an object.
 */
HRESULT
RtMarshalByValue(
    _In_ IStream *Stream,
    _In_ REFIID Riid,
    _In_ IUnknown *Object,
    _In_ IPropertySetMarshal *Marshal)
{
    IRtBuffer *Data;
    HSTRING ClassName;
    OBJREF Header;
    UINT32 NameLength, Length;
    BYTE *Bytes;
    HRESULT Status;

    Status = RtSerializeValue(Object, Marshal, &ClassName, &Data);
    if (FAILED(Status))
    {
        WARN("Could not serialize %p by value, %#lx\n", Object, Status);
        return Status;
    }

    Status = RtGetBufferBytes(Data, &Bytes, &Length);
    if (SUCCEEDED(Status))
    {
        NameLength = WindowsGetStringLen(ClassName);

        RtlZeroMemory(&Header, sizeof(Header));
        Header.signature = OBJREF_SIGNATURE;
        Header.flags = OBJREF_CUSTOM;
        Header.iid = *Riid;
        Header.u_objref.u_custom.clsid = CLSID_RtMarshalByValue;
        Header.u_objref.u_custom.cbExtension = 0;
        Header.u_objref.u_custom.size = sizeof(UINT32) + NameLength * sizeof(WCHAR) + sizeof(UINT32) + Length;

        Status = IStream_Write(Stream, &Header, FIELD_OFFSET(OBJREF, u_objref.u_custom.pData), NULL);
        if (SUCCEEDED(Status))
            Status = IStream_Write(Stream, &NameLength, sizeof(NameLength), NULL);
        if (SUCCEEDED(Status))
            Status = IStream_Write(Stream, WindowsGetStringRawBuffer(ClassName, NULL), NameLength * sizeof(WCHAR), NULL);
        if (SUCCEEDED(Status))
            Status = IStream_Write(Stream, &Length, sizeof(Length), NULL);
        if (SUCCEEDED(Status) && Length)
            Status = IStream_Write(Stream, Bytes, Length, NULL);
    }

    TRACE("%s by value, %#lx\n", debugstr_hstring(ClassName), Status);
    IRtBuffer_Release(Data);
    WindowsDeleteString(ClassName);
    return Status;
}

static
HRESULT
RtReadExact(
    _In_ IStream *Stream,
    _Out_writes_bytes_(Length) void *Buffer,
    _In_ ULONG Length)
{
    ULONG Read = 0;
    HRESULT Status;

    Status = IStream_Read(Stream, Buffer, Length, &Read);
    if (FAILED(Status))
        return Status;
    return Read == Length ? S_OK : STG_E_READFAULT;
}

/**
 * @brief
 * Rebuilds an object from the payload of a by value OBJREF, whose header the
 * caller already read.
 */
HRESULT
RtUnmarshalByValue(
    _In_ IStream *Stream,
    _In_ REFIID Riid,
    _Outptr_ void **Object)
{
    IRtPropertySetSerializer *Serializer = NULL;
    IPropertySetUnmarshal *Factory = NULL;
    PRT_VALUE_BUFFER Buffer = NULL;
    IRtPropertySet *Set = NULL;
    IInspectable *Instance = NULL;
    HSTRING ClassName = NULL;
    PWSTR Name = NULL;
    UINT32 NameLength, Length;
    HRESULT Status;

    *Object = NULL;

    Status = RtReadExact(Stream, &NameLength, sizeof(NameLength));
    if (SUCCEEDED(Status) && (!NameLength || NameLength > RT_MAX_VALUE_CLASS_NAME))
        Status = RPC_E_INVALID_OBJREF;
    if (SUCCEEDED(Status))
    {
        Name = malloc((NameLength + 1) * sizeof(WCHAR));
        Status = Name ? S_OK : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(Status))
        Status = RtReadExact(Stream, Name, NameLength * sizeof(WCHAR));
    if (SUCCEEDED(Status))
        Status = WindowsCreateString(Name, NameLength, &ClassName);

    if (SUCCEEDED(Status))
        Status = RtReadExact(Stream, &Length, sizeof(Length));
    if (SUCCEEDED(Status) && Length > RT_MAX_VALUE_DATA)
        Status = RPC_E_INVALID_OBJREF;
    if (SUCCEEDED(Status))
    {
        Buffer = malloc(FIELD_OFFSET(RT_VALUE_BUFFER, Data[Length ? Length : 1]));
        Status = Buffer ? S_OK : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(Status))
    {
        Buffer->IRtBuffer_iface.lpVtbl = &ValueBufferVtbl;
        Buffer->IRtBufferByteAccess_iface.lpVtbl = &ValueBufferBytesVtbl;
        Buffer->RefCount = 1;
        Buffer->Length = Length;
        Status = Length ? RtReadExact(Stream, Buffer->Data, Length) : S_OK;
    }

    if (SUCCEEDED(Status))
    {
        Status = RoGetActivationFactory(ClassName, &IID_IPropertySetUnmarshal, (void **)&Factory);
        if (FAILED(Status))
            WARN("No factory rebuilds %s by value, %#lx\n", debugstr_hstring(ClassName), Status);
    }
    if (SUCCEEDED(Status))
        Status = RtCreatePropertySet(&Set);
    if (SUCCEEDED(Status))
        Status = RtCreateSerializer(&Serializer);
    if (SUCCEEDED(Status) && Length)
        Status = IRtPropertySetSerializer_Deserialize(Serializer, Set, &Buffer->IRtBuffer_iface);
    if (SUCCEEDED(Status))
        Status = IPropertySetUnmarshal_UnmarshalObjectFromPropertySet(Factory, Set, &Instance);
    if (SUCCEEDED(Status))
        Status = IInspectable_QueryInterface(Instance, Riid, Object);

    TRACE("%s from its value, %#lx\n", debugstr_hstring(ClassName), Status);

    if (Instance)
        IInspectable_Release(Instance);
    if (Serializer)
        IRtPropertySetSerializer_Release(Serializer);
    if (Set)
        IRtPropertySet_Release(Set);
    if (Factory)
        IPropertySetUnmarshal_Release(Factory);
    if (Buffer)
        IRtBuffer_Release(&Buffer->IRtBuffer_iface);
    WindowsDeleteString(ClassName);
    free(Name);
    return Status;
}
