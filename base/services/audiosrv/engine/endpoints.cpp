/*
 * PROJECT:     ReactOS Audio Engine
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Audio endpoints built from the wave devices, kept in the MMDevices registry
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "engine.h"

#include <math.h>

#define AE_MMDEVICES_KEY        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio"
#define AE_WAVE_NAME_VALUE      L"ReactOS.WaveName"
#define AE_WAVE_ORDINAL_VALUE   L"ReactOS.WaveOrdinal"
#define AE_VOLUME_VALUE         L"ReactOS.ChannelVolume"

/* Property value names, as "{fmtid},pid" */
#define AE_PROP_DEVICE_DESC         L"{a45c254e-df1c-4efd-8020-67d146a850e0},2"
#define AE_PROP_INTERFACE_NAME      L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6"
#define AE_PROP_ENUMERATOR          L"{a45c254e-df1c-4efd-8020-67d146a850e0},24"
#define AE_PROP_FORM_FACTOR         L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},0"
#define AE_PROP_ASSOCIATION         L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},2"
#define AE_PROP_PHYSICAL_SPEAKERS   L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},3"
#define AE_PROP_ENDPOINT_GUID       L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},4"
#define AE_PROP_EVENT_DRIVEN        L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},7"
#define AE_PROP_JACK_SUBTYPE        L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},8"
#define AE_PROP_DEVICE_FORMAT       L"{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0"
#define AE_PROP_OEM_FORMAT          L"{e4870e26-3cc5-4cd2-ba46-ca0a9a70ed04},3"

/* Form factors and jack types of EndpointFormFactor and KSNODETYPE */
#define AE_FORM_SPEAKERS            1
#define AE_FORM_MICROPHONE          4
#define AE_JACK_SPEAKER             L"{DFF21CE1-F70F-11D0-B917-00A0C9223196}"
#define AE_JACK_MICROPHONE          L"{DFF21BE1-F70F-11D0-B917-00A0C9223196}"

/* Serialized VARTYPEs of REG_BINARY property values */
#define AE_VT_BLOB                  0x41

typedef struct _AE_SAVED_VOLUME
{
    ULONG Mute;
    ULONG Channels;
    float ChannelDb[AE_MAX_CHANNELS];
} AE_SAVED_VOLUME;

AE_ENDPOINT AeEndpoints[AE_MAX_ENDPOINTS];
ULONG AeEndpointCount;
BOOL AeWaveRefreshPending;

static const PCWSTR AeFlowKeys[2] = { L"Render", L"Capture" };

/*
 * The scalar a volume slider shows maps onto the dB range through a taper, a power curve of
 * the linear amplitude below unity and its mirror image above it.
 */
static
double
AeTaperFromAmplitude(
    _In_ double Amplitude)
{
    return (Amplitude <= 1.0) ? pow(Amplitude, 1.0 / AE_VOLUME_TAPER) : 2.0 - pow(Amplitude, -1.0 / AE_VOLUME_TAPER);
}

static
double
AeAmplitudeFromTaper(
    _In_ double Taper)
{
    return (Taper <= 1.0) ? pow(Taper, (double)AE_VOLUME_TAPER) : pow(2.0 - Taper, -(double)AE_VOLUME_TAPER);
}

float
AeScalarToDb(
    _In_ float Scalar)
{
    double Low = AeTaperFromAmplitude(pow(10.0, AE_VOLUME_MIN_DB / 20.0));
    double High = AeTaperFromAmplitude(pow(10.0, AE_VOLUME_MAX_DB / 20.0));
    double Amplitude = AeAmplitudeFromTaper(Low + Scalar * (High - Low));

    if (Amplitude < pow(10.0, -9.6))
        return AE_VOLUME_MIN_DB;
    return (float)min(max(20.0 * log10(Amplitude), (double)AE_VOLUME_MIN_DB), (double)AE_VOLUME_MAX_DB);
}

float
AeDbToScalar(
    _In_ float Db)
{
    double Low = AeTaperFromAmplitude(pow(10.0, AE_VOLUME_MIN_DB / 20.0));
    double High = AeTaperFromAmplitude(pow(10.0, AE_VOLUME_MAX_DB / 20.0));
    double Scalar;

    if (High == Low)
        return 1.0f;
    Scalar = (AeTaperFromAmplitude(pow(10.0, Db / 20.0)) - Low) / (High - Low);
    return (float)min(max(Scalar, 0.0), 1.0);
}

/*
 * There is no master level of its own: the master is the loudest channel, the first of them
 * on a tie. Refreshes it and the gain the mixer applies after a channel changed.
 */
VOID
AeUpdateEndpointGain(
    _In_ ULONG Endpoint)
{
    PAE_ENDPOINT_STATE State = &AeState->Endpoints[Endpoint];
    PAE_ENDPOINT Device = &AeEndpoints[Endpoint];
    ULONG Channel, Channels = max(min(State->Channels, (ULONG)AE_MAX_CHANNELS), 1UL);
    float Master = State->ChannelLevelDb[0];

    for (Channel = 0; Channel < AE_MAX_CHANNELS; Channel++)
    {
        State->ChannelScalar[Channel] = AeDbToScalar(State->ChannelLevelDb[Channel]);
        Device->ChannelGain[Channel] = State->Mute ? 0.0f : powf(10.0f, State->ChannelLevelDb[Channel] / 20.0f);
        if (Channel < Channels && State->ChannelLevelDb[Channel] > Master)
            Master = State->ChannelLevelDb[Channel];
    }
    State->MasterLevelDb = Master;
    State->MasterScalar = AeDbToScalar(Master);
}

static
VOID
AeFormatEndpointId(
    _In_ ULONG Flow,
    _In_ const GUID *Guid,
    _Out_writes_(AE_ID_LENGTH) PWSTR Id)
{
    StringCchPrintfW(Id, AE_ID_LENGTH,
                     L"{0.0.%lu.00000000}.{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                     Flow, Guid->Data1, Guid->Data2, Guid->Data3,
                     Guid->Data4[0], Guid->Data4[1], Guid->Data4[2], Guid->Data4[3],
                     Guid->Data4[4], Guid->Data4[5], Guid->Data4[6], Guid->Data4[7]);
}

static
VOID
AeGuidKeyName(
    _In_ const GUID *Guid,
    _Out_writes_(40) PWSTR Name)
{
    StringCchPrintfW(Name, 40, L"{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                     Guid->Data1, Guid->Data2, Guid->Data3,
                     Guid->Data4[0], Guid->Data4[1], Guid->Data4[2], Guid->Data4[3],
                     Guid->Data4[4], Guid->Data4[5], Guid->Data4[6], Guid->Data4[7]);
}

static
HKEY
AeOpenEndpointKey(
    _In_ ULONG Flow,
    _In_ const GUID *Guid,
    _In_ BOOL Create)
{
    WCHAR Path[160];
    WCHAR Name[40];
    HKEY Key = NULL;

    AeGuidKeyName(Guid, Name);
    StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\%s\\%s", AE_MMDEVICES_KEY, AeFlowKeys[Flow], Name);
    if (Create)
        RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Key, NULL);
    else
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_ALL_ACCESS, &Key);
    return Key;
}

static
VOID
AeWriteString(
    _In_ HKEY Key,
    _In_ PCWSTR Name,
    _In_ PCWSTR Value)
{
    RegSetValueExW(Key, Name, 0, REG_SZ, (const BYTE *)Value, (DWORD)((wcslen(Value) + 1) * sizeof(WCHAR)));
}

static
VOID
AeWriteDword(
    _In_ HKEY Key,
    _In_ PCWSTR Name,
    _In_ DWORD Value)
{
    RegSetValueExW(Key, Name, 0, REG_DWORD, (const BYTE *)&Value, sizeof(Value));
}

/* A blob is stored behind its VARTYPE and a version word, as the property store reads it back */
static
VOID
AeWriteBlob(
    _In_ HKEY Key,
    _In_ PCWSTR Name,
    _In_reads_bytes_(Size) const VOID *Data,
    _In_ ULONG Size)
{
    BYTE Buffer[8 + AE_FORMAT_BYTES];

    if (Size > AE_FORMAT_BYTES)
        return;
    ((DWORD *)Buffer)[0] = AE_VT_BLOB;
    ((DWORD *)Buffer)[1] = 1;
    CopyMemory(Buffer + 8, Data, Size);
    RegSetValueExW(Key, Name, 0, REG_BINARY, Buffer, 8 + Size);
}

/*
 * The best format a wave device takes: 48 kHz before 44.1 kHz, stereo before mono, and the
 * smallest sample that works, as some USB devices offer only 24 or 32 bit.
 */
static
VOID
AeProbeDeviceFormat(
    _In_ ULONG Flow,
    _In_ UINT Device,
    _Out_ WAVEFORMATEXTENSIBLE *Format)
{
    static const DWORD Rates[] = { 48000, 44100 };
    static const WORD Channels[] = { 2, 1 };
    static const WORD Bits[] = { 16, 24, 32 };
    WAVEFORMATEX Query;
    MMRESULT Result;
    ULONG RateIndex, ChannelIndex, BitsIndex;

    for (ChannelIndex = 0; ChannelIndex < ARRAYSIZE(Channels); ChannelIndex++)
    {
        for (RateIndex = 0; RateIndex < ARRAYSIZE(Rates); RateIndex++)
        {
            for (BitsIndex = 0; BitsIndex < ARRAYSIZE(Bits); BitsIndex++)
            {
                Query.wFormatTag = WAVE_FORMAT_PCM;
                Query.nChannels = Channels[ChannelIndex];
                Query.nSamplesPerSec = Rates[RateIndex];
                Query.wBitsPerSample = Bits[BitsIndex];
                Query.nBlockAlign = Query.nChannels * Query.wBitsPerSample / 8;
                Query.nAvgBytesPerSec = Query.nSamplesPerSec * Query.nBlockAlign;
                Query.cbSize = 0;

                Result = AeWaveQuery(Flow, Device, &Query);
                if (Result == MMSYSERR_NOERROR)
                    goto Found;
            }
        }
    }

    /* The driver answers nothing; the device thread falls back to silence if opening fails too */
    Query.nChannels = 2;
    Query.nSamplesPerSec = 48000;
    Query.wBitsPerSample = 16;
    Query.nBlockAlign = 4;
    Query.nAvgBytesPerSec = 48000 * 4;

Found:
    ZeroMemory(Format, sizeof(*Format));
    Format->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    Format->Format.nChannels = Query.nChannels;
    Format->Format.nSamplesPerSec = Query.nSamplesPerSec;
    Format->Format.wBitsPerSample = Query.wBitsPerSample;
    Format->Format.nBlockAlign = Query.nBlockAlign;
    Format->Format.nAvgBytesPerSec = Query.nAvgBytesPerSec;
    Format->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    Format->Samples.wValidBitsPerSample = Query.wBitsPerSample;
    Format->dwChannelMask = AeDefaultChannelMask(Query.nChannels);
    Format->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
}

/* The endpoint a wave device had before, found by its name and its place among same named devices */
static
BOOL
AeFindSavedGuid(
    _In_ ULONG Flow,
    _In_ PCWSTR WaveName,
    _In_ ULONG Ordinal,
    _Out_ GUID *Guid)
{
    WCHAR Path[128];
    WCHAR SubKey[40];
    WCHAR Name[MAXPNAMELEN];
    DWORD Index, Size, SavedOrdinal;
    HKEY FlowKey, Endpoint;
    BOOL Found = FALSE;

    StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\%s", AE_MMDEVICES_KEY, AeFlowKeys[Flow]);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_READ, &FlowKey) != ERROR_SUCCESS)
        return FALSE;

    for (Index = 0; !Found; Index++)
    {
        Size = ARRAYSIZE(SubKey);
        if (RegEnumKeyExW(FlowKey, Index, SubKey, &Size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (RegOpenKeyExW(FlowKey, SubKey, 0, KEY_READ, &Endpoint) != ERROR_SUCCESS)
            continue;

        Size = sizeof(Name);
        if (RegQueryValueExW(Endpoint, AE_WAVE_NAME_VALUE, NULL, NULL, (BYTE *)Name, &Size) == ERROR_SUCCESS &&
            !_wcsicmp(Name, WaveName))
        {
            Size = sizeof(SavedOrdinal);
            if (RegQueryValueExW(Endpoint, AE_WAVE_ORDINAL_VALUE, NULL, NULL, (BYTE *)&SavedOrdinal, &Size) == ERROR_SUCCESS &&
                SavedOrdinal == Ordinal)
            {
                Found = SUCCEEDED(CLSIDFromString(SubKey, Guid));
            }
        }
        RegCloseKey(Endpoint);
    }

    RegCloseKey(FlowKey);
    return Found;
}

static
VOID
AeLoadEndpointVolume(
    _In_ ULONG Endpoint)
{
    PAE_ENDPOINT Device = &AeEndpoints[Endpoint];
    PAE_ENDPOINT_STATE State = &AeState->Endpoints[Endpoint];
    AE_SAVED_VOLUME Saved;
    DWORD Size = sizeof(Saved);
    ULONG Channel;
    HKEY Key;

    State->Mute = FALSE;
    for (Channel = 0; Channel < AE_MAX_CHANNELS; Channel++)
        State->ChannelLevelDb[Channel] = AE_VOLUME_MAX_DB;

    Key = AeOpenEndpointKey(Device->Flow, &Device->Guid, FALSE);
    if (Key)
    {
        if (RegQueryValueExW(Key, AE_VOLUME_VALUE, NULL, NULL, (BYTE *)&Saved, &Size) == ERROR_SUCCESS &&
            Size == sizeof(Saved))
        {
            State->Mute = Saved.Mute != 0;
            for (Channel = 0; Channel < AE_MAX_CHANNELS && Channel < Saved.Channels; Channel++)
                State->ChannelLevelDb[Channel] = min(max(Saved.ChannelDb[Channel], AE_VOLUME_MIN_DB), AE_VOLUME_MAX_DB);
        }
        RegCloseKey(Key);
    }

    AeUpdateEndpointGain(Endpoint);
}

VOID
AeSaveEndpointVolume(
    _In_ ULONG Endpoint)
{
    PAE_ENDPOINT Device = &AeEndpoints[Endpoint];
    PAE_ENDPOINT_STATE State = &AeState->Endpoints[Endpoint];
    AE_SAVED_VOLUME Saved;
    ULONG Channel;
    HKEY Key;

    Saved.Mute = State->Mute;
    Saved.Channels = AE_MAX_CHANNELS;
    for (Channel = 0; Channel < AE_MAX_CHANNELS; Channel++)
        Saved.ChannelDb[Channel] = State->ChannelLevelDb[Channel];

    Key = AeOpenEndpointKey(Device->Flow, &Device->Guid, FALSE);
    if (!Key)
        return;
    RegSetValueExW(Key, AE_VOLUME_VALUE, 0, REG_BINARY, (const BYTE *)&Saved, sizeof(Saved));
    RegCloseKey(Key);
}

static
VOID
AeWriteEndpointProperties(
    _In_ PAE_ENDPOINT Device)
{
    WCHAR GuidString[40];
    HKEY Key, Properties;

    Key = AeOpenEndpointKey(Device->Flow, &Device->Guid, TRUE);
    if (!Key)
        return;

    AeWriteDword(Key, L"DeviceState", Device->Present ? DEVICE_STATE_ACTIVE : DEVICE_STATE_NOTPRESENT);
    AeWriteString(Key, AE_WAVE_NAME_VALUE, Device->WaveName);
    AeWriteDword(Key, AE_WAVE_ORDINAL_VALUE, Device->WaveOrdinal);

    if (RegCreateKeyExW(Key, L"Properties", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Properties, NULL) == ERROR_SUCCESS)
    {
        AeGuidKeyName(&Device->Guid, GuidString);
        AeWriteString(Properties, AE_PROP_DEVICE_DESC, Device->Flow == AE_FLOW_RENDER ? L"Speakers" : L"Microphone");
        AeWriteString(Properties, AE_PROP_INTERFACE_NAME, Device->WaveName);
        AeWriteString(Properties, AE_PROP_ENUMERATOR, L"ROOT");
        AeWriteDword(Properties, AE_PROP_FORM_FACTOR, Device->Flow == AE_FLOW_RENDER ? AE_FORM_SPEAKERS : AE_FORM_MICROPHONE);
        AeWriteString(Properties, AE_PROP_ASSOCIATION, L"{00000000-0000-0000-0000-000000000000}");
        AeWriteString(Properties, AE_PROP_ENDPOINT_GUID, GuidString);
        AeWriteDword(Properties, AE_PROP_EVENT_DRIVEN, 1);
        AeWriteString(Properties, AE_PROP_JACK_SUBTYPE, Device->Flow == AE_FLOW_RENDER ? AE_JACK_SPEAKER : AE_JACK_MICROPHONE);
        if (Device->Flow == AE_FLOW_RENDER)
            AeWriteDword(Properties, AE_PROP_PHYSICAL_SPEAKERS, Device->DeviceFormatEx.dwChannelMask);
        AeWriteBlob(Properties, AE_PROP_DEVICE_FORMAT, &Device->DeviceFormatEx, sizeof(Device->DeviceFormatEx));
        AeWriteBlob(Properties, AE_PROP_OEM_FORMAT, &Device->DeviceFormatEx, sizeof(Device->DeviceFormatEx));
        RegCloseKey(Properties);
    }

    RegCloseKey(Key);
}

PAE_ENDPOINT
AeFindEndpoint(
    _In_ PCWSTR Id)
{
    ULONG Index;

    for (Index = 0; Index < AeEndpointCount; Index++)
    {
        if (!_wcsicmp(AeEndpoints[Index].Id, Id))
            return &AeEndpoints[Index];
    }
    return NULL;
}

/* Puts a wave device in the slot its endpoint had, or the next free one */
static
VOID
AeAddWaveDevice(
    _In_ ULONG Flow,
    _In_ UINT WaveDevice,
    _In_ PCWSTR WaveName,
    _In_ ULONG Ordinal)
{
    PAE_ENDPOINT Device = NULL;
    PAE_ENDPOINT_STATE State;
    GUID Guid;
    ULONG Index;

    if (!AeFindSavedGuid(Flow, WaveName, Ordinal, &Guid) && FAILED(CoCreateGuid(&Guid)))
        return;

    for (Index = 0; Index < AeEndpointCount; Index++)
    {
        if (AeEndpoints[Index].Flow == Flow && IsEqualGUID(AeEndpoints[Index].Guid, Guid))
        {
            Device = &AeEndpoints[Index];
            break;
        }
    }

    if (!Device)
    {
        if (AeEndpointCount == AE_MAX_ENDPOINTS)
            return;
        Index = AeEndpointCount++;
        Device = &AeEndpoints[Index];
        ZeroMemory(Device, sizeof(*Device));
        Device->Index = Index;
        Device->Flow = Flow;
        Device->Guid = Guid;
        AeFormatEndpointId(Flow, &Guid, Device->Id);
    }

    Device->Present = TRUE;
    Device->WaveDevice = WaveDevice;
    Device->WaveOrdinal = Ordinal;
    StringCchCopyW(Device->WaveName, ARRAYSIZE(Device->WaveName), WaveName);

    /* A device thread and its streams were sized for the format it has, so a busy one keeps it */
    if (!Device->Thread && !Device->StreamCount)
    {
        AeProbeDeviceFormat(Flow, WaveDevice, &Device->DeviceFormatEx);
        AeDecodeFormat(&Device->DeviceFormatEx.Format, &Device->DeviceFormat);
        Device->MixFormat = Device->DeviceFormat;
        Device->MixFormat.SampleType = AeSampleFloat;
        Device->MixFormat.ContainerBits = Device->MixFormat.ValidBits = 32;
        Device->MixFormat.BlockAlign = Device->MixFormat.Channels * 4;
        Device->PeriodFrames = (ULONG)((ULONGLONG)Device->DeviceFormat.SamplesPerSec * AE_PERIOD_100NS / 10000000);
    }

    State = &AeState->Endpoints[Index];
    StringCchCopyW(State->Id, ARRAYSIZE(State->Id), Device->Id);
    StringCchPrintfW(State->FriendlyName, ARRAYSIZE(State->FriendlyName), L"%s (%s)",
                     Flow == AE_FLOW_RENDER ? L"Speakers" : L"Microphone", WaveName);
    State->Flow = Flow;
    State->DeviceState = DEVICE_STATE_ACTIVE;
    State->Channels = Device->MixFormat.Channels;
    State->ChannelMask = Device->MixFormat.ChannelMask;
    AeMakeFloatFormat(Device->MixFormat.SamplesPerSec, Device->MixFormat.Channels, Device->MixFormat.ChannelMask,
                      (WAVEFORMATEXTENSIBLE *)State->MixFormat);

    AeWriteEndpointProperties(Device);
    AeLoadEndpointVolume(Index);
}

/* Endpoints whose saved devices are not here are left not present */
static
VOID
AeMarkMissingEndpoints(
    _In_ ULONG Flow)
{
    WCHAR Path[128];
    WCHAR SubKey[40];
    DWORD Index, Size, NotPresent = DEVICE_STATE_NOTPRESENT;
    HKEY FlowKey, Endpoint;
    GUID Guid;
    ULONG Slot;
    BOOL Known;

    StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\%s", AE_MMDEVICES_KEY, AeFlowKeys[Flow]);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_READ, &FlowKey) != ERROR_SUCCESS)
        return;

    for (Index = 0; ; Index++)
    {
        Size = ARRAYSIZE(SubKey);
        if (RegEnumKeyExW(FlowKey, Index, SubKey, &Size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (FAILED(CLSIDFromString(SubKey, &Guid)))
            continue;

        Known = FALSE;
        for (Slot = 0; Slot < AeEndpointCount; Slot++)
        {
            if (AeEndpoints[Slot].Flow == Flow && AeEndpoints[Slot].Present &&
                IsEqualGUID(AeEndpoints[Slot].Guid, Guid))
            {
                Known = TRUE;
            }
        }
        if (Known || RegOpenKeyExW(FlowKey, SubKey, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &Endpoint) != ERROR_SUCCESS)
            continue;

        /* Only endpoints this engine made, not ones another system left behind */
        if (RegQueryValueExW(Endpoint, AE_WAVE_NAME_VALUE, NULL, NULL, NULL, NULL) == ERROR_SUCCESS)
            RegSetValueExW(Endpoint, L"DeviceState", 0, REG_DWORD, (const BYTE *)&NotPresent, sizeof(NotPresent));
        RegCloseKey(Endpoint);
    }

    RegCloseKey(FlowKey);
}

static
VOID
AeAddFlow(
    _In_ ULONG Flow)
{
    WCHAR Names[AE_MAX_ENDPOINTS][MAXPNAMELEN];
    UINT Count, Device, Earlier;
    ULONG Ordinal;
    PCWSTR Name;

    Count = AeWaveGetCount(Flow);
    for (Device = 0; Device < Count && Device < AE_MAX_ENDPOINTS; Device++)
    {
        Names[Device][0] = UNICODE_NULL;
        if (AeWaveGetName(Flow, Device, Names[Device]) != MMSYSERR_NOERROR)
            continue;
        Name = Names[Device];

        Ordinal = 0;
        for (Earlier = 0; Earlier < Device; Earlier++)
        {
            if (!_wcsicmp(Names[Earlier], Name))
                Ordinal++;
        }
        AeAddWaveDevice(Flow, Device, Name, Ordinal);
    }

    AeMarkMissingEndpoints(Flow);
}

/**
 * @brief
 * Makes an endpoint for every wave device, keeping each one's id across boots, and marks the
 * ones whose device went away. The engine lock must be held.
 */
VOID
AeBuildEndpoints(VOID)
{
    ULONG Index;

    /* Device numbers may change with a new list, so it is only made while no endpoint is open */
    for (Index = 0; Index < AeEndpointCount && !AeEndpoints[Index].Thread; Index++);
    if (Index == AeEndpointCount)
        AeWaveRefreshPending = !AeWaveRefresh();
    else
        AeWaveRefreshPending = TRUE;

    for (Index = 0; Index < AeEndpointCount; Index++)
        AeEndpoints[Index].Present = FALSE;

    AeAddFlow(AE_FLOW_RENDER);
    AeAddFlow(AE_FLOW_CAPTURE);

    for (Index = 0; Index < AeEndpointCount; Index++)
    {
        if (!AeEndpoints[Index].Present)
            AeState->Endpoints[Index].DeviceState = DEVICE_STATE_NOTPRESENT;
    }
    AeState->EndpointCount = AeEndpointCount;
    AeLoadDefaults();
}

static
BOOL
AeReadRoleTime(
    _In_ PAE_ENDPOINT Device,
    _In_ ULONG Role,
    _Out_ ULONGLONG *Time)
{
    WCHAR Name[16];
    SYSTEMTIME Saved;
    FILETIME File;
    DWORD Size = sizeof(Saved);
    BOOL Found = FALSE;
    HKEY Key;

    Key = AeOpenEndpointKey(Device->Flow, &Device->Guid, FALSE);
    if (!Key)
        return FALSE;

    StringCchPrintfW(Name, ARRAYSIZE(Name), L"Role:%lu", Role);
    if (RegQueryValueExW(Key, Name, NULL, NULL, (BYTE *)&Saved, &Size) == ERROR_SUCCESS &&
        Size == sizeof(Saved) && SystemTimeToFileTime(&Saved, &File))
    {
        *Time = ((ULONGLONG)File.dwHighDateTime << 32) | File.dwLowDateTime;
        Found = TRUE;
    }
    RegCloseKey(Key);
    return Found;
}

/**
 * @brief
 * Picks the default endpoint of each flow and role: the one most recently made default, or else
 * the first present one. The engine lock must be held.
 */
VOID
AeLoadDefaults(VOID)
{
    ULONG Flow, Role, Index, Best;
    ULONGLONG Time, BestTime;

    for (Flow = 0; Flow < 2; Flow++)
    {
        for (Role = 0; Role < AE_ROLE_COUNT; Role++)
        {
            Best = AE_NO_ENDPOINT;
            BestTime = 0;
            for (Index = 0; Index < AeEndpointCount; Index++)
            {
                if (AeEndpoints[Index].Flow != Flow || !AeEndpoints[Index].Present)
                    continue;
                if (Best == AE_NO_ENDPOINT)
                    Best = Index;
                if (AeReadRoleTime(&AeEndpoints[Index], Role, &Time) && Time > BestTime)
                {
                    Best = Index;
                    BestTime = Time;
                }
            }
            AeState->DefaultEndpoint[Flow][Role] = Best;
        }
    }
}

VOID
AeSaveDefault(
    _In_ ULONG Endpoint,
    _In_ ULONG Role)
{
    PAE_ENDPOINT Device = &AeEndpoints[Endpoint];
    WCHAR Name[16];
    SYSTEMTIME Now;
    HKEY Key;

    Key = AeOpenEndpointKey(Device->Flow, &Device->Guid, FALSE);
    if (!Key)
        return;

    GetSystemTime(&Now);
    StringCchPrintfW(Name, ARRAYSIZE(Name), L"Role:%lu", Role);
    RegSetValueExW(Key, Name, 0, REG_BINARY, (const BYTE *)&Now, sizeof(Now));
    RegCloseKey(Key);
}
