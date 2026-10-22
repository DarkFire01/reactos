/*
 * PROJECT:     ReactOS Multimedia Device API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Property store of an endpoint, kept in the MMDevices registry key
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

/* Values that are not strings or DWORDs are binary: their VARTYPE, an element count, then the data */
#define MMD_BINARY_HEADER   8

/* Characters of "{0.0.0.00000000}." in front of the endpoint's GUID */
#define MMD_ID_PREFIX       17

#define MMD_MAX_PROPERTIES  128

typedef struct _MMD_PROPERTY
{
    PROPERTYKEY Key;
    PROPVARIANT Value;
    BOOL Dirty;
} MMD_PROPERTY;

/* The name of the adapter an endpoint belongs to, as the endpoint builder stores it */
static const PROPERTYKEY MmdAdapterNameKey =
{
    { 0xb3f8fa53, 0x0004, 0x438e, { 0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc } }, 6
};

static
ULONG
MmdFixedSize(
    _In_ VARTYPE Type)
{
    switch (Type)
    {
        case VT_I1:
        case VT_UI1:
            return 1;
        case VT_I2:
        case VT_UI2:
            return 2;
        case VT_I4:
        case VT_UI4:
        case VT_R4:
        case VT_INT:
        case VT_UINT:
        case VT_ERROR:
            return 4;
        case VT_I8:
        case VT_UI8:
        case VT_R8:
        case VT_CY:
        case VT_DATE:
        case VT_FILETIME:
            return 8;
        default:
            return 0;
    }
}

static
HRESULT
MmdAllocString(
    _In_reads_(Length) const WCHAR *Source,
    _In_ SIZE_T Length,
    _Outptr_ LPWSTR *String)
{
    *String = (LPWSTR)CoTaskMemAlloc((Length + 1) * sizeof(WCHAR));
    if (!*String)
        return E_OUTOFMEMORY;
    CopyMemory(*String, Source, Length * sizeof(WCHAR));
    (*String)[Length] = UNICODE_NULL;
    return S_OK;
}

static
HRESULT
MmdMultiStringToVector(
    _In_reads_(Length) const WCHAR *Source,
    _In_ SIZE_T Length,
    _Out_ PROPVARIANT *Value)
{
    const WCHAR *Current;
    ULONG Count = 0, Index;
    SIZE_T Run;
    HRESULT Hr;

    for (Current = Source; Current < Source + Length && *Current; Current += wcslen(Current) + 1)
        Count++;

    Value->calpwstr.pElems = (LPWSTR *)CoTaskMemAlloc(max(Count, 1UL) * sizeof(LPWSTR));
    if (!Value->calpwstr.pElems)
        return E_OUTOFMEMORY;
    Value->vt = VT_VECTOR | VT_LPWSTR;
    Value->calpwstr.cElems = 0;

    for (Current = Source, Index = 0; Index < Count; Index++, Current += Run + 1)
    {
        Run = wcslen(Current);
        Hr = MmdAllocString(Current, Run, &Value->calpwstr.pElems[Index]);
        if (FAILED(Hr))
            return Hr;
        Value->calpwstr.cElems++;
    }
    return S_OK;
}

static
HRESULT
MmdRegistryToVariant(
    _In_ DWORD Type,
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ DWORD Size,
    _Out_ PROPVARIANT *Value)
{
    const BYTE *Payload = Data + MMD_BINARY_HEADER;
    ULONG PayloadSize, Fixed, Count;
    VARTYPE VarType;

    PropVariantInit(Value);
    switch (Type)
    {
        case REG_SZ:
        case REG_EXPAND_SZ:
            Value->vt = VT_LPWSTR;
            return MmdAllocString((const WCHAR *)Data, wcsnlen((const WCHAR *)Data, Size / sizeof(WCHAR)), &Value->pwszVal);

        case REG_DWORD:
            if (Size != sizeof(DWORD))
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            Value->vt = VT_UI4;
            Value->ulVal = *(const DWORD *)Data;
            return S_OK;

        case REG_MULTI_SZ:
            return MmdMultiStringToVector((const WCHAR *)Data, Size / sizeof(WCHAR), Value);

        case REG_BINARY:
            break;

        default:
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATATYPE);
    }

    if (Size < MMD_BINARY_HEADER)
        return E_UNEXPECTED;
    VarType = (VARTYPE)((const DWORD *)Data)[0];
    Count = ((const DWORD *)Data)[1];
    PayloadSize = Size - MMD_BINARY_HEADER;

    switch (VarType)
    {
        case VT_BLOB:
            Value->blob.pBlobData = (BYTE *)CoTaskMemAlloc(max(PayloadSize, 1UL));
            if (!Value->blob.pBlobData)
                return E_OUTOFMEMORY;
            CopyMemory(Value->blob.pBlobData, Payload, PayloadSize);
            Value->blob.cbSize = PayloadSize;
            Value->vt = VT_BLOB;
            return S_OK;

        case VT_CLSID:
            if (PayloadSize < sizeof(CLSID))
                return E_UNEXPECTED;
            Value->puuid = (CLSID *)CoTaskMemAlloc(sizeof(CLSID));
            if (!Value->puuid)
                return E_OUTOFMEMORY;
            CopyMemory(Value->puuid, Payload, sizeof(CLSID));
            Value->vt = VT_CLSID;
            return S_OK;

        case VT_BOOL:
            if (PayloadSize < sizeof(VARIANT_BOOL))
                return E_UNEXPECTED;
            Value->boolVal = *(const VARIANT_BOOL *)Payload ? VARIANT_TRUE : VARIANT_FALSE;
            Value->vt = VT_BOOL;
            return S_OK;

        case VT_LPWSTR:
            Value->vt = VT_LPWSTR;
            return MmdAllocString((const WCHAR *)Payload, wcsnlen((const WCHAR *)Payload, PayloadSize / sizeof(WCHAR)),
                                  &Value->pwszVal);

        case VT_VECTOR | VT_UI1:
            Count = min(Count, PayloadSize);
            Value->caub.pElems = (UCHAR *)CoTaskMemAlloc(max(Count, 1UL));
            if (!Value->caub.pElems)
                return E_OUTOFMEMORY;
            CopyMemory(Value->caub.pElems, Payload, Count);
            Value->caub.cElems = Count;
            Value->vt = VT_VECTOR | VT_UI1;
            return S_OK;
    }

    Fixed = MmdFixedSize(VarType);
    if (!Fixed || PayloadSize < Fixed)
        return E_UNEXPECTED;
    CopyMemory(&Value->bVal, Payload, Fixed);
    Value->vt = VarType;
    return S_OK;
}

static
LONG
MmdWriteBinary(
    _In_ HKEY Key,
    _In_ PCWSTR Name,
    _In_ VARTYPE Type,
    _In_ ULONG Count,
    _In_reads_bytes_(Size) const VOID *Data,
    _In_ ULONG Size)
{
    BYTE *Buffer;
    LONG Error;

    Buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, MMD_BINARY_HEADER + Size);
    if (!Buffer)
        return ERROR_OUTOFMEMORY;
    ((DWORD *)Buffer)[0] = Type;
    ((DWORD *)Buffer)[1] = Count;
    CopyMemory(Buffer + MMD_BINARY_HEADER, Data, Size);
    Error = RegSetValueExW(Key, Name, 0, REG_BINARY, Buffer, MMD_BINARY_HEADER + Size);
    HeapFree(GetProcessHeap(), 0, Buffer);
    return Error;
}

static
LONG
MmdWriteVariant(
    _In_ HKEY Key,
    _In_ PCWSTR Name,
    _In_ const PROPVARIANT *Value)
{
    WCHAR *Multi, *Current;
    SIZE_T Chars = 1;
    ULONG Index, Fixed;
    VARIANT_BOOL Bool;
    LONG Error;

    switch (Value->vt)
    {
        case VT_EMPTY:
            Error = RegDeleteValueW(Key, Name);
            return (Error == ERROR_FILE_NOT_FOUND) ? ERROR_SUCCESS : Error;

        case VT_LPWSTR:
            return RegSetValueExW(Key, Name, 0, REG_SZ, (const BYTE *)Value->pwszVal,
                                  (DWORD)((wcslen(Value->pwszVal) + 1) * sizeof(WCHAR)));

        case VT_UI4:
            return RegSetValueExW(Key, Name, 0, REG_DWORD, (const BYTE *)&Value->ulVal, sizeof(DWORD));

        case VT_VECTOR | VT_LPWSTR:
            for (Index = 0; Index < Value->calpwstr.cElems; Index++)
                Chars += wcslen(Value->calpwstr.pElems[Index]) + 1;
            Multi = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, Chars * sizeof(WCHAR));
            if (!Multi)
                return ERROR_OUTOFMEMORY;
            Current = Multi;
            for (Index = 0; Index < Value->calpwstr.cElems; Index++)
            {
                wcscpy(Current, Value->calpwstr.pElems[Index]);
                Current += wcslen(Current) + 1;
            }
            *Current = UNICODE_NULL;
            Error = RegSetValueExW(Key, Name, 0, REG_MULTI_SZ, (const BYTE *)Multi, (DWORD)(Chars * sizeof(WCHAR)));
            HeapFree(GetProcessHeap(), 0, Multi);
            return Error;

        case VT_BLOB:
            return MmdWriteBinary(Key, Name, VT_BLOB, 1, Value->blob.pBlobData, Value->blob.cbSize);

        case VT_CLSID:
            return MmdWriteBinary(Key, Name, VT_CLSID, 1, Value->puuid, sizeof(CLSID));

        case VT_BOOL:
            Bool = Value->boolVal;
            return MmdWriteBinary(Key, Name, VT_BOOL, 1, &Bool, sizeof(Bool));

        case VT_VECTOR | VT_UI1:
            return MmdWriteBinary(Key, Name, VT_VECTOR | VT_UI1, Value->caub.cElems,
                                  Value->caub.pElems, Value->caub.cElems);
    }

    Fixed = MmdFixedSize(Value->vt);
    if (!Fixed)
        return ERROR_NOT_SUPPORTED;
    return MmdWriteBinary(Key, Name, Value->vt, 1, &Value->bVal, Fixed);
}

/* Value names are "{fmtid},pid" */
static
BOOL
MmdParsePropertyName(
    _In_ PCWSTR Name,
    _Out_ PROPERTYKEY *Key)
{
    WCHAR Guid[39];
    PCWSTR Comma = wcschr(Name, L',');

    if (!Comma || Comma - Name != 38)
        return FALSE;
    CopyMemory(Guid, Name, 38 * sizeof(WCHAR));
    Guid[38] = UNICODE_NULL;
    if (FAILED(CLSIDFromString(Guid, &Key->fmtid)))
        return FALSE;
    Key->pid = wcstoul(Comma + 1, NULL, 10);
    return TRUE;
}

static
VOID
MmdFormatPropertyName(
    _In_ REFPROPERTYKEY Key,
    _Out_writes_(Length) PWSTR Name,
    _In_ SIZE_T Length)
{
    const GUID *Id = &Key.fmtid;

    StringCchPrintfW(Name, Length, L"{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x},%lu",
                     Id->Data1, Id->Data2, Id->Data3, Id->Data4[0], Id->Data4[1], Id->Data4[2],
                     Id->Data4[3], Id->Data4[4], Id->Data4[5], Id->Data4[6], Id->Data4[7], Key.pid);
}

class CPropertyStore
    : public CMmdObject
    , public IPropertyStore
{
public:
    CPropertyStore(
        _In_ DWORD Access)
        : m_Access(Access)
        , m_Count(0)
    {
        m_Path[0] = UNICODE_NULL;
    }

    ~CPropertyStore()
    {
        ULONG Index;

        for (Index = 0; Index < m_Count; Index++)
            PropVariantClear(&m_Properties[Index].Value);
    }

    HRESULT
    Load(
        _In_ const AE_ENDPOINT_STATE *Endpoint)
    {
        WCHAR Name[64];
        DWORD NameLength, Type, Size, Index;
        BYTE *Data;
        PROPERTYKEY Key;
        HKEY Properties;
        PROPVARIANT Value;
        MMD_PROPERTY *Adapter;
        LONG Error;

        if (wcslen(Endpoint->Id) <= MMD_ID_PREFIX)
            return E_UNEXPECTED;
        StringCchPrintfW(m_Path, ARRAYSIZE(m_Path), L"%s\\%s\\%s\\Properties", MMD_MMDEVICES_KEY,
                         Endpoint->Flow == AE_FLOW_RENDER ? L"Render" : L"Capture", Endpoint->Id + MMD_ID_PREFIX);

        /* Writers need the right to write the key up front, as Windows asks for it at open */
        if (m_Access != STGM_READ)
        {
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, m_Path, 0, NULL, 0, KEY_SET_VALUE, NULL, &Properties, NULL) != ERROR_SUCCESS)
                return E_ACCESSDENIED;
            RegCloseKey(Properties);
        }

        Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, m_Path, 0, KEY_READ, &Properties);
        if (Error != ERROR_SUCCESS && m_Access == STGM_READ)
            return HRESULT_FROM_WIN32(Error);
        if (Error == ERROR_SUCCESS)
        {
            Data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 64 * 1024);
            for (Index = 0; Data && m_Count < MMD_MAX_PROPERTIES; Index++)
            {
                NameLength = ARRAYSIZE(Name);
                Size = 64 * 1024 - sizeof(WCHAR);
                Error = RegEnumValueW(Properties, Index, Name, &NameLength, NULL, &Type, Data, &Size);
                if (Error == ERROR_NO_MORE_ITEMS)
                    break;
                if (Error != ERROR_SUCCESS || !MmdParsePropertyName(Name, &Key) || IsSynthesized(Key))
                    continue;

                /* Strings from the registry may lack their terminator */
                Data[Size] = 0;
                Data[Size + 1] = 0;
                if (SUCCEEDED(MmdRegistryToVariant(Type, Data, Size, &Value)))
                    Append(Key, &Value);
            }
            if (Data)
                HeapFree(GetProcessHeap(), 0, Data);
            RegCloseKey(Properties);
        }

        /* The friendly name joins the description and the adapter name; the GUID comes from the id */
        PropVariantInit(&Value);
        Value.vt = VT_LPWSTR;
        if (SUCCEEDED(MmdAllocString(Endpoint->FriendlyName, wcslen(Endpoint->FriendlyName), &Value.pwszVal)))
            Append(PKEY_Device_FriendlyName, &Value);

        PropVariantInit(&Value);
        Value.vt = VT_LPWSTR;
        if (SUCCEEDED(MmdAllocString(Endpoint->Id + MMD_ID_PREFIX, wcslen(Endpoint->Id + MMD_ID_PREFIX), &Value.pwszVal)))
        {
            _wcsupr(Value.pwszVal);
            Append(PKEY_AudioEndpoint_GUID, &Value);
        }

        /* The interface name is the adapter's, which the endpoint keeps under a key of its own */
        Adapter = Find(MmdAdapterNameKey);
        PropVariantInit(&Value);
        Value.vt = VT_LPWSTR;
        if (Adapter && Adapter->Value.vt == VT_LPWSTR && Adapter->Value.pwszVal &&
            SUCCEEDED(MmdAllocString(Adapter->Value.pwszVal, wcslen(Adapter->Value.pwszVal), &Value.pwszVal)))
        {
            Append(PKEY_DeviceInterface_FriendlyName, &Value);
        }
        return S_OK;
    }

    STDMETHODIMP
    QueryInterface(
        _In_ REFIID Riid,
        _Outptr_ VOID **Object) override
    {
        if (!Object)
            return E_POINTER;
        if (!IsEqualIID(Riid, IID_IUnknown) && !IsEqualIID(Riid, IID_IPropertyStore))
        {
            *Object = NULL;
            return E_NOINTERFACE;
        }
        *Object = static_cast<IPropertyStore *>(this);
        AddRef();
        return S_OK;
    }

    MMD_IMPLEMENT_REFCOUNT()

    STDMETHODIMP
    GetCount(
        _Out_ DWORD *Count) override
    {
        if (!Count)
            return E_POINTER;
        *Count = m_Count;
        return S_OK;
    }

    STDMETHODIMP
    GetAt(
        _In_ DWORD Index,
        _Out_ PROPERTYKEY *Key) override
    {
        if (!Key)
            return E_POINTER;
        if (Index >= m_Count)
            return E_INVALIDARG;
        *Key = m_Properties[Index].Key;
        return S_OK;
    }

    STDMETHODIMP
    GetValue(
        _In_ REFPROPERTYKEY Key,
        _Out_ PROPVARIANT *Value) override
    {
        MMD_PROPERTY *Property;

        if (!Value)
            return E_POINTER;
        PropVariantInit(Value);
        if (m_Access == STGM_WRITE)
            return STG_E_ACCESSDENIED;

        Property = Find(Key);
        return Property ? PropVariantCopy(Value, &Property->Value) : S_OK;
    }

    STDMETHODIMP
    SetValue(
        _In_ REFPROPERTYKEY Key,
        _In_ REFPROPVARIANT Value) override
    {
        MMD_PROPERTY *Property;
        PROPVARIANT Copy;
        HRESULT Hr;

        if (m_Access == STGM_READ)
            return E_ACCESSDENIED;
        if (IsSynthesized(Key))
            return E_ACCESSDENIED;

        Hr = PropVariantCopy(&Copy, &Value);
        if (FAILED(Hr))
            return Hr;

        Property = Find(Key);
        if (Property)
        {
            PropVariantClear(&Property->Value);
            Property->Value = Copy;
        }
        else if (!(Property = Append(Key, &Copy)))
        {
            PropVariantClear(&Copy);
            return E_OUTOFMEMORY;
        }
        Property->Dirty = TRUE;
        return S_OK;
    }

    STDMETHODIMP
    Commit() override
    {
        WCHAR Name[64];
        HKEY Properties;
        ULONG Index;
        LONG Error = ERROR_SUCCESS;

        if (m_Access == STGM_READ)
            return E_ACCESSDENIED;

        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, m_Path, 0, NULL, 0, KEY_SET_VALUE, NULL, &Properties, NULL) != ERROR_SUCCESS)
            return E_ACCESSDENIED;

        for (Index = 0; Index < m_Count && Error == ERROR_SUCCESS; Index++)
        {
            if (!m_Properties[Index].Dirty)
                continue;
            MmdFormatPropertyName(m_Properties[Index].Key, Name, ARRAYSIZE(Name));
            Error = MmdWriteVariant(Properties, Name, &m_Properties[Index].Value);
            if (Error == ERROR_SUCCESS)
                m_Properties[Index].Dirty = FALSE;
        }
        RegCloseKey(Properties);
        return HRESULT_FROM_WIN32(Error);
    }

private:
    static
    BOOL
    IsSynthesized(
        _In_ REFPROPERTYKEY Key)
    {
        return (IsEqualGUID(Key.fmtid, PKEY_Device_FriendlyName.fmtid) && Key.pid == PKEY_Device_FriendlyName.pid) ||
               (IsEqualGUID(Key.fmtid, PKEY_AudioEndpoint_GUID.fmtid) && Key.pid == PKEY_AudioEndpoint_GUID.pid) ||
               (IsEqualGUID(Key.fmtid, PKEY_DeviceInterface_FriendlyName.fmtid) &&
                Key.pid == PKEY_DeviceInterface_FriendlyName.pid);
    }

    MMD_PROPERTY *
    Find(
        _In_ REFPROPERTYKEY Key)
    {
        ULONG Index;

        for (Index = 0; Index < m_Count; Index++)
        {
            if (IsEqualGUID(m_Properties[Index].Key.fmtid, Key.fmtid) && m_Properties[Index].Key.pid == Key.pid)
                return &m_Properties[Index];
        }
        return NULL;
    }

    /* Takes over Value */
    MMD_PROPERTY *
    Append(
        _In_ REFPROPERTYKEY Key,
        _In_ PROPVARIANT *Value)
    {
        MMD_PROPERTY *Property;

        if (m_Count == MMD_MAX_PROPERTIES)
        {
            PropVariantClear(Value);
            return NULL;
        }
        Property = &m_Properties[m_Count++];
        Property->Key = Key;
        Property->Value = *Value;
        Property->Dirty = FALSE;
        return Property;
    }

    DWORD m_Access;
    WCHAR m_Path[MAX_PATH];
    MMD_PROPERTY m_Properties[MMD_MAX_PROPERTIES];
    ULONG m_Count;
};

HRESULT
MmdCreatePropertyStore(
    _In_ ULONG Endpoint,
    _In_ DWORD Access,
    _Outptr_ IPropertyStore **Store)
{
    const AE_STATE *State = MmdGetState();
    CPropertyStore *Object;
    HRESULT Hr;

    *Store = NULL;
    if (!State || Endpoint >= State->EndpointCount)
        return E_NOTFOUND;

    Object = new (std::nothrow) CPropertyStore(Access);
    if (!Object)
        return E_OUTOFMEMORY;
    Hr = Object->Load(&State->Endpoints[Endpoint]);
    if (FAILED(Hr))
    {
        Object->Release();
        return Hr;
    }
    *Store = Object;
    return S_OK;
}
