/*
 * PROJECT:     ReactOS Multimedia Device API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Endpoint volume and endpoint peak meter
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#include <math.h>

static
const AE_ENDPOINT_STATE *
MmdGetEndpoint(
    _In_ ULONG Endpoint)
{
    const AE_STATE *State = MmdGetState();

    return (State && Endpoint < State->EndpointCount) ? &State->Endpoints[Endpoint] : NULL;
}

class CEndpointVolume
    : public CMmdObject
    , public IAudioEndpointVolumeEx
{
public:
    CEndpointVolume(
        _In_ ULONG Endpoint)
        : m_Endpoint(Endpoint)
    {
    }

    STDMETHODIMP
    QueryInterface(
        _In_ REFIID Riid,
        _Outptr_ VOID **Object) override
    {
        if (!Object)
            return E_POINTER;
        if (!IsEqualIID(Riid, IID_IUnknown) && !IsEqualIID(Riid, IID_IAudioEndpointVolume) &&
            !IsEqualIID(Riid, IID_IAudioEndpointVolumeEx))
        {
            *Object = NULL;
            return E_NOINTERFACE;
        }
        *Object = static_cast<IAudioEndpointVolumeEx *>(this);
        AddRef();
        return S_OK;
    }

    MMD_IMPLEMENT_REFCOUNT()

    STDMETHODIMP
    RegisterControlChangeNotify(
        _In_ IAudioEndpointVolumeCallback *Callback) override
    {
        if (!Callback)
            return E_INVALIDARG;
        return MmdAddWatch(MmdWatchEndpointVolume, m_Endpoint, 0, Callback);
    }

    STDMETHODIMP
    UnregisterControlChangeNotify(
        _In_ IAudioEndpointVolumeCallback *Callback) override
    {
        if (!Callback)
            return E_INVALIDARG;
        MmdRemoveWatch(MmdWatchEndpointVolume, m_Endpoint, Callback);
        return S_OK;
    }

    STDMETHODIMP
    GetChannelCount(
        _Out_ UINT *Count) override
    {
        const AE_ENDPOINT_STATE *Endpoint;

        if (!Count)
            return E_POINTER;
        Endpoint = MmdGetEndpoint(m_Endpoint);
        if (!Endpoint)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        *Count = Endpoint->Channels;
        return S_OK;
    }

    STDMETHODIMP
    SetMasterVolumeLevel(
        _In_ float LevelDb,
        _In_opt_ LPCGUID Context) override
    {
        return Set(AE_SET_MASTER_DB, 0, LevelDb, FALSE, Context);
    }

    STDMETHODIMP
    SetMasterVolumeLevelScalar(
        _In_ float Level,
        _In_opt_ LPCGUID Context) override
    {
        return Set(AE_SET_MASTER_SCALAR, 0, Level, FALSE, Context);
    }

    STDMETHODIMP
    GetMasterVolumeLevel(
        _Out_ float *LevelDb) override
    {
        const AE_ENDPOINT_STATE *Endpoint;

        if (!LevelDb)
            return E_POINTER;
        Endpoint = MmdGetEndpoint(m_Endpoint);
        if (!Endpoint)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        *LevelDb = Endpoint->MasterLevelDb;
        return S_OK;
    }

    STDMETHODIMP
    GetMasterVolumeLevelScalar(
        _Out_ float *Level) override
    {
        const AE_ENDPOINT_STATE *Endpoint;

        if (!Level)
            return E_POINTER;
        Endpoint = MmdGetEndpoint(m_Endpoint);
        if (!Endpoint)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        *Level = Endpoint->MasterScalar;
        return S_OK;
    }

    STDMETHODIMP
    SetChannelVolumeLevel(
        _In_ UINT Channel,
        _In_ float LevelDb,
        _In_opt_ LPCGUID Context) override
    {
        return Set(AE_SET_CHANNEL_DB, Channel, LevelDb, FALSE, Context);
    }

    STDMETHODIMP
    SetChannelVolumeLevelScalar(
        _In_ UINT Channel,
        _In_ float Level,
        _In_opt_ LPCGUID Context) override
    {
        return Set(AE_SET_CHANNEL_SCALAR, Channel, Level, FALSE, Context);
    }

    STDMETHODIMP
    GetChannelVolumeLevel(
        _In_ UINT Channel,
        _Out_ float *LevelDb) override
    {
        const AE_ENDPOINT_STATE *Endpoint;

        if (!LevelDb)
            return E_POINTER;
        Endpoint = MmdGetEndpoint(m_Endpoint);
        if (!Endpoint)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        if (Channel >= Endpoint->Channels)
            return E_INVALIDARG;
        *LevelDb = Endpoint->ChannelLevelDb[Channel];
        return S_OK;
    }

    STDMETHODIMP
    GetChannelVolumeLevelScalar(
        _In_ UINT Channel,
        _Out_ float *Level) override
    {
        const AE_ENDPOINT_STATE *Endpoint;

        if (!Level)
            return E_POINTER;
        Endpoint = MmdGetEndpoint(m_Endpoint);
        if (!Endpoint)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        if (Channel >= Endpoint->Channels)
            return E_INVALIDARG;
        *Level = Endpoint->ChannelScalar[Channel];
        return S_OK;
    }

    STDMETHODIMP
    SetMute(
        _In_ BOOL Mute,
        _In_opt_ LPCGUID Context) override
    {
        return Set(AE_SET_MUTE, 0, 0.0f, Mute, Context);
    }

    STDMETHODIMP
    GetMute(
        _Out_ BOOL *Mute) override
    {
        const AE_ENDPOINT_STATE *Endpoint;

        if (!Mute)
            return E_POINTER;
        Endpoint = MmdGetEndpoint(m_Endpoint);
        if (!Endpoint)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        *Mute = Endpoint->Mute ? TRUE : FALSE;
        return S_OK;
    }

    STDMETHODIMP
    GetVolumeStepInfo(
        _Out_ UINT *Step,
        _Out_ UINT *StepCount) override
    {
        const AE_ENDPOINT_STATE *Endpoint;

        if (!Step || !StepCount)
            return E_POINTER;
        Endpoint = MmdGetEndpoint(m_Endpoint);
        if (!Endpoint)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        *StepCount = MMD_VOLUME_STEPS;
        *Step = (UINT)min(floorf(Endpoint->MasterScalar * (MMD_VOLUME_STEPS - 1) + 0.5f), (float)(MMD_VOLUME_STEPS - 1));
        return S_OK;
    }

    STDMETHODIMP
    VolumeStepUp(
        _In_opt_ LPCGUID Context) override
    {
        return StepBy(1, Context);
    }

    STDMETHODIMP
    VolumeStepDown(
        _In_opt_ LPCGUID Context) override
    {
        return StepBy(-1, Context);
    }

    STDMETHODIMP
    QueryHardwareSupport(
        _Out_ DWORD *Mask) override
    {
        if (!Mask)
            return E_POINTER;
        *Mask = 0;
        return S_OK;
    }

    STDMETHODIMP
    GetVolumeRange(
        _Out_ float *MinimumDb,
        _Out_ float *MaximumDb,
        _Out_ float *IncrementDb) override
    {
        if (!MinimumDb || !MaximumDb || !IncrementDb)
            return E_POINTER;
        *MinimumDb = MMD_VOLUME_MIN_DB;
        *MaximumDb = MMD_VOLUME_MAX_DB;
        *IncrementDb = MMD_VOLUME_STEP_DB;
        return S_OK;
    }

    /* IAudioEndpointVolumeEx */
    STDMETHODIMP
    GetVolumeRangeChannel(
        _In_ UINT Channel,
        _Out_ float *MinimumDb,
        _Out_ float *MaximumDb,
        _Out_ float *IncrementDb) override
    {
        const AE_ENDPOINT_STATE *Endpoint = MmdGetEndpoint(m_Endpoint);

        if (!Endpoint)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        if (Channel >= Endpoint->Channels)
            return E_INVALIDARG;
        return GetVolumeRange(MinimumDb, MaximumDb, IncrementDb);
    }

private:
    HRESULT
    Set(
        _In_ ULONG What,
        _In_ ULONG Channel,
        _In_ float Value,
        _In_ BOOL Mute,
        _In_opt_ LPCGUID Context)
    {
        AE_REQUEST Request;

        ZeroMemory(&Request, sizeof(Request.SetEndpointVolume));
        Request.SetEndpointVolume.Endpoint = m_Endpoint;
        Request.SetEndpointVolume.What = What;
        Request.SetEndpointVolume.Channel = Channel;
        Request.SetEndpointVolume.Value = Value;
        Request.SetEndpointVolume.Mute = Mute ? TRUE : FALSE;
        Request.SetEndpointVolume.Context = Context ? *Context : GUID_NULL;
        return MmdCall(&Request, AeCommandSetEndpointVolume, sizeof(Request.SetEndpointVolume), NULL, 0);
    }

    /* Steps are even on the scalar, so a step moves the slider by the same distance anywhere */
    HRESULT
    StepBy(
        _In_ LONG Direction,
        _In_opt_ LPCGUID Context)
    {
        const AE_ENDPOINT_STATE *Endpoint = MmdGetEndpoint(m_Endpoint);
        float Last = (float)(MMD_VOLUME_STEPS - 1);
        float Step;

        if (!Endpoint)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        Step = floorf(Endpoint->MasterScalar * Last + ((Direction > 0) ? 1.5f : -0.5f));
        Step = min(max(Step, 0.0f), Last);
        return Set(AE_SET_MASTER_SCALAR, 0, Step / Last, FALSE, Context);
    }

    ULONG m_Endpoint;
};

class CEndpointMeter
    : public CMmdObject
    , public IAudioMeterInformation
{
public:
    CEndpointMeter(
        _In_ ULONG Endpoint)
        : m_Endpoint(Endpoint)
    {
    }

    STDMETHODIMP
    QueryInterface(
        _In_ REFIID Riid,
        _Outptr_ VOID **Object) override
    {
        if (!Object)
            return E_POINTER;
        if (!IsEqualIID(Riid, IID_IUnknown) && !IsEqualIID(Riid, IID_IAudioMeterInformation))
        {
            *Object = NULL;
            return E_NOINTERFACE;
        }
        *Object = static_cast<IAudioMeterInformation *>(this);
        AddRef();
        return S_OK;
    }

    MMD_IMPLEMENT_REFCOUNT()

    STDMETHODIMP
    GetPeakValue(
        _Out_ float *Peak) override
    {
        const AE_ENDPOINT_STATE *Endpoint;

        if (!Peak)
            return E_POINTER;
        Endpoint = MmdGetEndpoint(m_Endpoint);
        if (!Endpoint)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        *Peak = Endpoint->PeakMaster;
        return S_OK;
    }

    STDMETHODIMP
    GetMeteringChannelCount(
        _Out_ UINT *Count) override
    {
        const AE_ENDPOINT_STATE *Endpoint;

        if (!Count)
            return E_POINTER;
        Endpoint = MmdGetEndpoint(m_Endpoint);
        if (!Endpoint)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        *Count = Endpoint->Channels;
        return S_OK;
    }

    STDMETHODIMP
    GetChannelsPeakValues(
        _In_ UINT32 Count,
        _Out_writes_(Count) float *Peaks) override
    {
        const AE_ENDPOINT_STATE *Endpoint;
        UINT32 Index;

        if (!Peaks)
            return E_POINTER;
        Endpoint = MmdGetEndpoint(m_Endpoint);
        if (!Endpoint)
            return AUDCLNT_E_DEVICE_INVALIDATED;
        if (Count != Endpoint->Channels)
            return E_INVALIDARG;
        for (Index = 0; Index < Count; Index++)
            Peaks[Index] = Endpoint->PeakChannel[Index];
        return S_OK;
    }

    STDMETHODIMP
    QueryHardwareSupport(
        _Out_ DWORD *Mask) override
    {
        if (!Mask)
            return E_POINTER;
        *Mask = 0;
        return S_OK;
    }

private:
    ULONG m_Endpoint;
};

HRESULT
MmdCreateEndpointVolume(
    _In_ ULONG Endpoint,
    _In_ REFIID Riid,
    _Outptr_ VOID **Object)
{
    CEndpointVolume *Volume;
    HRESULT Hr;

    *Object = NULL;
    Volume = new (std::nothrow) CEndpointVolume(Endpoint);
    if (!Volume)
        return E_OUTOFMEMORY;
    Hr = Volume->QueryInterface(Riid, Object);
    Volume->Release();
    return Hr;
}

HRESULT
MmdCreateEndpointMeter(
    _In_ ULONG Endpoint,
    _In_ REFIID Riid,
    _Outptr_ VOID **Object)
{
    CEndpointMeter *Meter;
    HRESULT Hr;

    *Object = NULL;
    Meter = new (std::nothrow) CEndpointMeter(Endpoint);
    if (!Meter)
        return E_OUTOFMEMORY;
    Hr = Meter->QueryInterface(Riid, Object);
    Meter->Release();
    return Hr;
}
