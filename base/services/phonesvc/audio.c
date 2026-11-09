/*
 * PROJECT:     ReactOS Phone Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Cellular call audio path control through kernel streaming
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "phonesvc.h"
#include <mmsystem.h>
#include <ks.h>
#include <ksmedia.h>

#define NDEBUG
#include <debug.h>

#define PH_NO_PIN                   ((ULONG)-1)

static const GUID PhCategoryAudio = { STATIC_KSCATEGORY_AUDIO };
static const GUID PhPropSetPin = { STATIC_KSPROPSETID_Pin };
static const GUID PhPropSetTelephonyControl = { STATIC_KSPROPSETID_TelephonyControl };
static const GUID PhPropSetTelephonyTopology = { STATIC_KSPROPSETID_TelephonyTopology };
static const GUID PhNodeHandset = { STATIC_KSNODETYPE_HANDSET };
static const GUID PhNodeHeadset = { STATIC_KSNODETYPE_HEADSET };
static const GUID PhNodeSpeaker = { STATIC_KSNODETYPE_SPEAKER };
static const GUID PhNodeHeadphones = { STATIC_KSNODETYPE_HEADPHONES };
static const GUID PhNodeMicrophone = { STATIC_KSNODETYPE_MICROPHONE };

typedef struct _PH_KS_FILTER
{
    HANDLE Handle;
    WCHAR Path[MAX_PATH];
} PH_KS_FILTER, *PPH_KS_FILTER;

static CRITICAL_SECTION PhAudioLock;
static PH_KS_FILTER PhWaveFilter = { INVALID_HANDLE_VALUE };
static PH_KS_FILTER PhTopologyFilter = { INVALID_HANDLE_VALUE };
static DWORD PhRoute = PhoneAudioEarpiece;
static BOOL PhCallOn;
static TELEPHONY_CALLTYPE PhCallType;

static
BOOL
PhKsProperty(
    _In_ HANDLE Filter,
    _In_ const GUID *Set,
    _In_ ULONG Id,
    _In_ ULONG Flags,
    _In_opt_ PVOID Instance,
    _In_ ULONG InstanceSize,
    _Inout_updates_bytes_opt_(DataSize) PVOID Data,
    _In_ ULONG DataSize,
    _Out_opt_ PULONG Returned)
{
    BYTE Buffer[sizeof(KSP_PIN) + 16];
    PKSPROPERTY Property = (PKSPROPERTY)Buffer;
    ULONG Size = sizeof(KSPROPERTY);
    DWORD Bytes = 0;
    BOOL Ok;

    ZeroMemory(Buffer, sizeof(Buffer));
    if (Instance && InstanceSize)
    {
        if (InstanceSize > sizeof(Buffer))
            return FALSE;
        CopyMemory(Buffer, Instance, InstanceSize);
        Size = InstanceSize;
    }

    Property->Set = *Set;
    Property->Id = Id;
    Property->Flags = Flags;

    Ok = DeviceIoControl(Filter, IOCTL_KS_PROPERTY, Buffer, Size, Data, DataSize, &Bytes, NULL);
    if (Returned)
        *Returned = Bytes;
    return Ok;
}

static
BOOL
PhKsPinProperty(
    _In_ HANDLE Filter,
    _In_ ULONG PinId,
    _In_ ULONG Id,
    _Out_writes_bytes_(DataSize) PVOID Data,
    _In_ ULONG DataSize)
{
    KSP_PIN Pin;

    ZeroMemory(&Pin, sizeof(Pin));
    Pin.PinId = PinId;

    return PhKsProperty(Filter, &PhPropSetPin, Id, KSPROPERTY_TYPE_GET, &Pin, sizeof(Pin), Data, DataSize, NULL);
}

static
VOID
PhCloseFilter(
    _Inout_ PPH_KS_FILTER Filter)
{
    if (Filter->Handle != INVALID_HANDLE_VALUE)
        CloseHandle(Filter->Handle);
    Filter->Handle = INVALID_HANDLE_VALUE;
    Filter->Path[0] = UNICODE_NULL;
}

/* Walks the audio interfaces once and keeps the filters that answer the telephony property sets */
static
BOOL
PhFindFilters(VOID)
{
    BYTE DetailBuffer[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + MAX_PATH * sizeof(WCHAR)];
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W Detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)DetailBuffer;
    SP_DEVICE_INTERFACE_DATA Interface;
    KSTELEPHONY_CALLINFO CallInfo;
    KSTOPOLOGY_ENDPOINTIDPAIR *Pair;
    HDEVINFO Set;
    HANDLE Handle;
    DWORD Index;
    BOOL Used;

    if (PhWaveFilter.Handle != INVALID_HANDLE_VALUE && PhTopologyFilter.Handle != INVALID_HANDLE_VALUE)
        return TRUE;

    Set = SetupDiGetClassDevsW(&PhCategoryAudio, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (Set == INVALID_HANDLE_VALUE)
        return FALSE;

    Pair = HeapAlloc(GetProcessHeap(), 0, sizeof(*Pair));
    if (!Pair)
    {
        SetupDiDestroyDeviceInfoList(Set);
        return FALSE;
    }

    for (Index = 0; ; Index++)
    {
        Interface.cbSize = sizeof(Interface);
        if (!SetupDiEnumDeviceInterfaces(Set, NULL, &PhCategoryAudio, Index, &Interface))
            break;

        Detail->cbSize = sizeof(*Detail);
        if (!SetupDiGetDeviceInterfaceDetailW(Set, &Interface, Detail, sizeof(DetailBuffer), NULL, NULL))
            continue;

        Handle = CreateFileW(Detail->DevicePath,
                             GENERIC_READ | GENERIC_WRITE,
                             0,
                             NULL,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             NULL);
        if (Handle == INVALID_HANDLE_VALUE)
            continue;

        Used = FALSE;

        if (PhWaveFilter.Handle == INVALID_HANDLE_VALUE &&
            PhKsProperty(Handle, &PhPropSetTelephonyControl, KSPROPERTY_TELEPHONY_CALLINFO,
                         KSPROPERTY_TYPE_GET, NULL, 0, &CallInfo, sizeof(CallInfo), NULL))
        {
            PhWaveFilter.Handle = Handle;
            StringCchCopyW(PhWaveFilter.Path, _countof(PhWaveFilter.Path), Detail->DevicePath);
            Used = TRUE;
        }
        else if (PhTopologyFilter.Handle == INVALID_HANDLE_VALUE &&
                 PhKsProperty(Handle, &PhPropSetTelephonyTopology, KSPROPERTY_TELEPHONY_ENDPOINTIDPAIR,
                              KSPROPERTY_TYPE_GET, NULL, 0, Pair, sizeof(*Pair), NULL))
        {
            PhTopologyFilter.Handle = Handle;
            StringCchCopyW(PhTopologyFilter.Path, _countof(PhTopologyFilter.Path), Detail->DevicePath);
            Used = TRUE;
        }

        if (!Used)
            CloseHandle(Handle);

        if (PhWaveFilter.Handle != INVALID_HANDLE_VALUE && PhTopologyFilter.Handle != INVALID_HANDLE_VALUE)
            break;
    }

    HeapFree(GetProcessHeap(), 0, Pair);
    SetupDiDestroyDeviceInfoList(Set);

    if (PhWaveFilter.Handle == INVALID_HANDLE_VALUE)
        DPRINT1("No telephony wave filter found\n");

    return PhWaveFilter.Handle != INVALID_HANDLE_VALUE;
}

/* Finds the first bridge pin with the given flow whose category is one of Categories */
static
ULONG
PhFindEndpointPin(
    _In_ KSPIN_DATAFLOW Flow,
    _In_reads_(Count) const GUID *const *Categories,
    _In_ ULONG Count)
{
    ULONG Pins = 0, Pin, i;
    KSPIN_DATAFLOW PinFlow;
    GUID Category;

    if (!PhKsPinProperty(PhTopologyFilter.Handle, 0, KSPROPERTY_PIN_CTYPES, &Pins, sizeof(Pins)))
        return PH_NO_PIN;

    /* Categories are in preference order */
    for (i = 0; i < Count; i++)
    {
        for (Pin = 0; Pin < Pins; Pin++)
        {
            if (!PhKsPinProperty(PhTopologyFilter.Handle, Pin, KSPROPERTY_PIN_DATAFLOW, &PinFlow, sizeof(PinFlow)) ||
                PinFlow != Flow)
            {
                continue;
            }

            if (PhKsPinProperty(PhTopologyFilter.Handle, Pin, KSPROPERTY_PIN_CATEGORY, &Category, sizeof(Category)) &&
                IsEqualGUID(&Category, Categories[i]))
            {
                return Pin;
            }
        }
    }

    return PH_NO_PIN;
}

static
BOOL
PhApplyRoute(
    _In_ DWORD Route)
{
    static const GUID *const EarpieceOut[] = { &PhNodeHandset, &PhNodeSpeaker };
    static const GUID *const SpeakerOut[] = { &PhNodeSpeaker, &PhNodeHandset };
    static const GUID *const HeadsetOut[] = { &PhNodeHeadset, &PhNodeHeadphones };
    static const GUID *const HandsetIn[] = { &PhNodeHandset, &PhNodeMicrophone };
    static const GUID *const SpeakerIn[] = { &PhNodeMicrophone, &PhNodeHandset };
    static const GUID *const HeadsetIn[] = { &PhNodeHeadset, &PhNodeMicrophone };
    KSTOPOLOGY_ENDPOINTIDPAIR *Pair;
    ULONG RenderPin, CapturePin;
    BOOL Ok;

    if (PhTopologyFilter.Handle == INVALID_HANDLE_VALUE)
        return FALSE;

    /* Bridge pins that leave the topology filter render, those that enter it capture */
    switch (Route)
    {
        case PhoneAudioSpeaker:
            RenderPin = PhFindEndpointPin(KSPIN_DATAFLOW_OUT, SpeakerOut, _countof(SpeakerOut));
            CapturePin = PhFindEndpointPin(KSPIN_DATAFLOW_IN, SpeakerIn, _countof(SpeakerIn));
            break;

        case PhoneAudioHeadset:
            RenderPin = PhFindEndpointPin(KSPIN_DATAFLOW_OUT, HeadsetOut, _countof(HeadsetOut));
            CapturePin = PhFindEndpointPin(KSPIN_DATAFLOW_IN, HeadsetIn, _countof(HeadsetIn));
            break;

        default:
            RenderPin = PhFindEndpointPin(KSPIN_DATAFLOW_OUT, EarpieceOut, _countof(EarpieceOut));
            CapturePin = PhFindEndpointPin(KSPIN_DATAFLOW_IN, HandsetIn, _countof(HandsetIn));
            break;
    }

    if (RenderPin == PH_NO_PIN || CapturePin == PH_NO_PIN)
    {
        DPRINT1("Route %lu has no endpoint pins\n", Route);
        return FALSE;
    }

    Pair = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Pair));
    if (!Pair)
        return FALSE;

    StringCchCopyW(Pair->RenderEndpoint.TopologyName, _countof(Pair->RenderEndpoint.TopologyName), PhTopologyFilter.Path);
    Pair->RenderEndpoint.PinId = RenderPin;
    StringCchCopyW(Pair->CaptureEndpoint.TopologyName, _countof(Pair->CaptureEndpoint.TopologyName), PhTopologyFilter.Path);
    Pair->CaptureEndpoint.PinId = CapturePin;

    Ok = PhKsProperty(PhTopologyFilter.Handle,
                      &PhPropSetTelephonyTopology,
                      KSPROPERTY_TELEPHONY_ENDPOINTIDPAIR,
                      KSPROPERTY_TYPE_SET,
                      NULL,
                      0,
                      Pair,
                      sizeof(*Pair),
                      NULL);

    HeapFree(GetProcessHeap(), 0, Pair);
    return Ok;
}

static
BOOL
PhSetCallControl(
    _In_ BOOL Enable)
{
    KSTELEPHONY_CALLCONTROL Control;

    Control.CallType = PhCallType;
    Control.CallControlOp = Enable ? TELEPHONY_CALLCONTROLOP_ENABLE : TELEPHONY_CALLCONTROLOP_DISABLE;

    return PhKsProperty(PhWaveFilter.Handle,
                        &PhPropSetTelephonyControl,
                        KSPROPERTY_TELEPHONY_CALLCONTROL,
                        KSPROPERTY_TYPE_SET,
                        NULL,
                        0,
                        &Control,
                        sizeof(Control),
                        NULL);
}

VOID
PhAudioCallActive(
    _In_ BOOL Active,
    _In_ BOOL PacketSwitched)
{
    EnterCriticalSection(&PhAudioLock);

    if (Active == PhCallOn)
        goto Exit;

    if (Active)
    {
        if (!PhFindFilters())
            goto Exit;

        PhCallType = PacketSwitched ? TELEPHONY_CALLTYPE_PACKETSWITCHED_LTE : TELEPHONY_CALLTYPE_CIRCUITSWITCHED;

        /* The endpoints have to be chosen before the path is enabled */
        PhApplyRoute(PhRoute);
        if (!PhSetCallControl(TRUE))
        {
            DPRINT1("Enabling call audio failed (%lu)\n", GetLastError());
            goto Exit;
        }

        PhCallOn = TRUE;
    }
    else
    {
        if (PhWaveFilter.Handle != INVALID_HANDLE_VALUE)
            PhSetCallControl(FALSE);
        PhCallOn = FALSE;
    }

Exit:
    LeaveCriticalSection(&PhAudioLock);
}

HRESULT
PhAudioSetRoute(
    _In_ DWORD Route)
{
    HRESULT hr = S_OK;

    if (Route > PhoneAudioHeadset)
        return E_INVALIDARG;

    EnterCriticalSection(&PhAudioLock);

    PhRoute = Route;
    if (PhCallOn && !PhApplyRoute(Route))
        hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

    LeaveCriticalSection(&PhAudioLock);
    return hr;
}

HRESULT
PhAudioSetMute(
    _In_ BOOL Mute)
{
    HRESULT hr = S_OK;

    EnterCriticalSection(&PhAudioLock);

    if (!PhFindFilters())
    {
        hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    else if (!PhKsProperty(PhWaveFilter.Handle,
                           &PhPropSetTelephonyControl,
                           KSPROPERTY_TELEPHONY_MUTE_TX,
                           KSPROPERTY_TYPE_SET,
                           NULL,
                           0,
                           &Mute,
                           sizeof(Mute),
                           NULL))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
    }

    LeaveCriticalSection(&PhAudioLock);
    return hr;
}

VOID
PhAudioInitialize(VOID)
{
    InitializeCriticalSection(&PhAudioLock);
}

VOID
PhAudioUninitialize(VOID)
{
    EnterCriticalSection(&PhAudioLock);

    if (PhCallOn && PhWaveFilter.Handle != INVALID_HANDLE_VALUE)
        PhSetCallControl(FALSE);
    PhCallOn = FALSE;

    PhCloseFilter(&PhWaveFilter);
    PhCloseFilter(&PhTopologyFilter);

    LeaveCriticalSection(&PhAudioLock);
}
