/*
 * PROJECT:     ReactOS Multimedia Device API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Device enumerator, device collections and audio endpoint devices
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

/* Endpoint ids are "{c.0.f.ssssssss}.{guid}": class 0 to 3, flow 0 or 1, eight hex digits of session */
static
BOOL
MmdIsWellFormedId(
    _In_ PCWSTR Id)
{
    ULONG Index;

    if (wcslen(Id) < 17 || Id[0] != L'{' || Id[1] < L'0' || Id[1] > L'3')
        return FALSE;
    if (Id[2] != L'.' || Id[4] != L'.' || Id[6] != L'.' || Id[15] != L'}' || Id[16] != L'.')
        return FALSE;
    if (Id[5] != L'0' && Id[5] != L'1')
        return FALSE;
    for (Index = 7; Index < 15; Index++)
    {
        if (!iswxdigit(Id[Index]) || (Id[1] == L'0' && Id[Index] != L'0'))
            return FALSE;
    }
    return TRUE;
}

class CDevice
    : public CMmdObject
    , public IMMDevice
    , public IMMEndpoint
{
public:
    CDevice(
        _In_ ULONG Endpoint,
        _In_ const AE_ENDPOINT_STATE *State)
        : m_Endpoint(Endpoint)
        , m_Flow(State->Flow)
    {
        StringCchCopyW(m_Id, ARRAYSIZE(m_Id), State->Id);
    }

    STDMETHODIMP
    QueryInterface(
        _In_ REFIID Riid,
        _Outptr_ VOID **Object) override
    {
        if (!Object)
            return E_POINTER;

        if (IsEqualIID(Riid, IID_IUnknown) || IsEqualIID(Riid, IID_IMMDevice))
            *Object = static_cast<IMMDevice *>(this);
        else if (IsEqualIID(Riid, IID_IMMEndpoint))
            *Object = static_cast<IMMEndpoint *>(this);
        else
        {
            *Object = NULL;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    MMD_IMPLEMENT_REFCOUNT()

    STDMETHODIMP
    Activate(
        _In_ REFIID Riid,
        _In_ DWORD ClsContext,
        _In_opt_ PROPVARIANT *Parameters,
        _Outptr_ VOID **Object) override
    {
        UNREFERENCED_PARAMETER(ClsContext);

        if (!Object)
            return E_POINTER;
        *Object = NULL;
        return MmdActivateOnEndpoint(m_Endpoint, Riid, Parameters, Object);
    }

    STDMETHODIMP
    OpenPropertyStore(
        _In_ DWORD Access,
        _Outptr_ IPropertyStore **Store) override
    {
        if (!Store)
            return E_POINTER;
        *Store = NULL;
        if ((Access & 0xF) > STGM_READWRITE)
            return E_INVALIDARG;
        return MmdCreatePropertyStore(m_Endpoint, Access & 0xF, Store);
    }

    STDMETHODIMP
    GetId(
        _Outptr_ LPWSTR *Id) override
    {
        SIZE_T Bytes = (wcslen(m_Id) + 1) * sizeof(WCHAR);

        if (!Id)
            return E_POINTER;
        *Id = (LPWSTR)CoTaskMemAlloc(Bytes);
        if (!*Id)
            return E_OUTOFMEMORY;
        CopyMemory(*Id, m_Id, Bytes);
        return S_OK;
    }

    STDMETHODIMP
    GetState(
        _Out_ DWORD *State) override
    {
        const AE_STATE *Engine = MmdGetState();

        if (!State)
            return E_POINTER;
        *State = (Engine && m_Endpoint < Engine->EndpointCount) ? Engine->Endpoints[m_Endpoint].DeviceState
                                                                : DEVICE_STATE_NOTPRESENT;
        return S_OK;
    }

    /* IMMEndpoint */
    STDMETHODIMP
    GetDataFlow(
        _Out_ EDataFlow *Flow) override
    {
        if (!Flow)
            return E_POINTER;
        *Flow = (EDataFlow)m_Flow;
        return S_OK;
    }

private:
    ULONG m_Endpoint;
    ULONG m_Flow;
    WCHAR m_Id[AE_ID_LENGTH];
};

class CDeviceCollection
    : public CMmdObject
    , public IMMDeviceCollection
{
public:
    CDeviceCollection() : m_Count(0) {}

    VOID Add(_In_ ULONG Endpoint) { m_Endpoints[m_Count++] = Endpoint; }

    STDMETHODIMP
    QueryInterface(
        _In_ REFIID Riid,
        _Outptr_ VOID **Object) override
    {
        if (!Object)
            return E_POINTER;
        if (!IsEqualIID(Riid, IID_IUnknown) && !IsEqualIID(Riid, IID_IMMDeviceCollection))
        {
            *Object = NULL;
            return E_NOINTERFACE;
        }
        *Object = static_cast<IMMDeviceCollection *>(this);
        AddRef();
        return S_OK;
    }

    MMD_IMPLEMENT_REFCOUNT()

    STDMETHODIMP
    GetCount(
        _Out_ UINT *Count) override
    {
        if (!Count)
            return E_POINTER;
        *Count = m_Count;
        return S_OK;
    }

    STDMETHODIMP
    Item(
        _In_ UINT Index,
        _Outptr_ IMMDevice **Device) override
    {
        if (!Device)
            return E_POINTER;
        *Device = NULL;
        if (Index >= m_Count)
            return E_INVALIDARG;
        return MmdCreateDevice(m_Endpoints[Index], Device);
    }

private:
    ULONG m_Endpoints[AE_MAX_ENDPOINTS];
    ULONG m_Count;
};

class CDeviceEnumerator
    : public CMmdObject
    , public IMMDeviceEnumerator
{
public:
    STDMETHODIMP
    QueryInterface(
        _In_ REFIID Riid,
        _Outptr_ VOID **Object) override
    {
        if (!Object)
            return E_POINTER;
        if (!IsEqualIID(Riid, IID_IUnknown) && !IsEqualIID(Riid, IID_IMMDeviceEnumerator))
        {
            *Object = NULL;
            return E_NOINTERFACE;
        }
        *Object = static_cast<IMMDeviceEnumerator *>(this);
        AddRef();
        return S_OK;
    }

    MMD_IMPLEMENT_REFCOUNT()

    STDMETHODIMP
    EnumAudioEndpoints(
        _In_ EDataFlow Flow,
        _In_ DWORD StateMask,
        _Outptr_ IMMDeviceCollection **Devices) override
    {
        const AE_STATE *State;
        CDeviceCollection *Collection;
        ULONG Index;

        if (!Devices)
            return E_POINTER;
        *Devices = NULL;
        if ((ULONG)Flow > eAll || !StateMask || (StateMask & 0x0FFFFFF0))
            return E_INVALIDARG;

        Collection = new (std::nothrow) CDeviceCollection();
        if (!Collection)
            return E_OUTOFMEMORY;

        State = MmdGetState();
        for (Index = 0; State && Index < State->EndpointCount && Index < AE_MAX_ENDPOINTS; Index++)
        {
            if (!State->Endpoints[Index].Id[0] || !(State->Endpoints[Index].DeviceState & StateMask))
                continue;
            if (Flow != eAll && State->Endpoints[Index].Flow != (ULONG)Flow)
                continue;
            Collection->Add(Index);
        }

        *Devices = Collection;
        return S_OK;
    }

    STDMETHODIMP
    GetDefaultAudioEndpoint(
        _In_ EDataFlow Flow,
        _In_ ERole Role,
        _Outptr_ IMMDevice **Device) override
    {
        const AE_STATE *State;
        ULONG Endpoint;

        if ((ULONG)Role >= AE_ROLE_COUNT)
            return E_INVALIDARG;
        if (!Device)
            return E_POINTER;
        *Device = NULL;
        if ((ULONG)Flow > eCapture)
            return E_INVALIDARG;

        State = MmdGetState();
        if (!State)
            return E_NOTFOUND;
        Endpoint = State->DefaultEndpoint[Flow][Role];
        if (Endpoint >= State->EndpointCount || !MmdIsEndpointActive(Endpoint))
            return E_NOTFOUND;
        return MmdCreateDevice(Endpoint, Device);
    }

    STDMETHODIMP
    GetDevice(
        _In_ LPCWSTR Id,
        _Outptr_ IMMDevice **Device) override
    {
        ULONG Endpoint;

        if (!Id || !Device)
            return E_POINTER;
        *Device = NULL;
        if (!MmdIsWellFormedId(Id))
            return E_INVALIDARG;

        Endpoint = MmdFindEndpoint(Id);
        if (Endpoint == AE_NO_ENDPOINT)
            return E_NOTFOUND;
        return MmdCreateDevice(Endpoint, Device);
    }

    STDMETHODIMP
    RegisterEndpointNotificationCallback(
        _In_ IMMNotificationClient *Client) override
    {
        if (!Client)
            return E_POINTER;
        return MmdAddWatch(MmdWatchEndpoints, 0, 0, Client);
    }

    STDMETHODIMP
    UnregisterEndpointNotificationCallback(
        _In_ IMMNotificationClient *Client) override
    {
        if (!Client)
            return E_POINTER;
        return MmdRemoveWatch(MmdWatchEndpoints, 0, Client);
    }
};

HRESULT
MmdCreateDevice(
    _In_ ULONG Endpoint,
    _Outptr_ IMMDevice **Device)
{
    const AE_STATE *State = MmdGetState();
    CDevice *Object;

    *Device = NULL;
    if (!State || Endpoint >= State->EndpointCount)
        return E_NOTFOUND;

    Object = new (std::nothrow) CDevice(Endpoint, &State->Endpoints[Endpoint]);
    if (!Object)
        return E_OUTOFMEMORY;
    *Device = static_cast<IMMDevice *>(Object);
    return S_OK;
}

/**
 * @brief
 * What IMMDevice::Activate and ActivateAudioInterfaceAsync give out for an endpoint.
 */
HRESULT
MmdActivateOnEndpoint(
    _In_ ULONG Endpoint,
    _In_ REFIID Riid,
    _In_opt_ PROPVARIANT *Parameters,
    _Outptr_ VOID **Object)
{
    const AE_STATE *State = MmdGetState();
    ULONG Kind;

    *Object = NULL;
    if (!State || Endpoint >= State->EndpointCount)
        return AUDCLNT_E_DEVICE_INVALIDATED;

    if (IsEqualIID(Riid, IID_IAudioClient) || IsEqualIID(Riid, IID_IAudioClient2) ||
        IsEqualIID(Riid, IID_IAudioClient3))
    {
        if (!MmdIsEndpointActive(Endpoint))
            return AUDCLNT_E_DEVICE_INVALIDATED;
        Kind = (State->Endpoints[Endpoint].Flow == AE_FLOW_RENDER) ? AE_STREAM_RENDER : AE_STREAM_CAPTURE;
        return MmdCreateAudioClient(Endpoint, Kind, 0, FALSE, Riid, Object);
    }

    if (IsEqualIID(Riid, IID_IAudioEndpointVolume) || IsEqualIID(Riid, IID_IAudioEndpointVolumeEx))
        return MmdCreateEndpointVolume(Endpoint, Riid, Object);

    if (IsEqualIID(Riid, IID_IAudioMeterInformation))
        return MmdCreateEndpointMeter(Endpoint, Riid, Object);

    if (IsEqualIID(Riid, IID_IAudioSessionManager) || IsEqualIID(Riid, IID_IAudioSessionManager2))
    {
        if (!MmdIsEndpointActive(Endpoint))
            return AUDCLNT_E_DEVICE_INVALIDATED;
        return MmdCreateSessionManager(Endpoint, Riid, Object);
    }

    return E_NOINTERFACE;
}

HRESULT
MmdCreateEnumerator(
    _In_ REFIID Riid,
    _Outptr_ VOID **Object)
{
    CDeviceEnumerator *Enumerator;
    HRESULT Hr;

    Enumerator = new (std::nothrow) CDeviceEnumerator();
    if (!Enumerator)
        return E_OUTOFMEMORY;
    Hr = Enumerator->QueryInterface(Riid, Object);
    Enumerator->Release();
    return Hr;
}
