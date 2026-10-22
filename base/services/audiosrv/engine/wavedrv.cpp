/*
 * PROJECT:     ReactOS Audio Engine
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Wave devices through the WDM audio driver's own entry points
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "engine.h"

/*
 * winmm lists the driver's devices once, when it is loaded, and audiosrv loads it before the
 * sound hardware has started. Talking to wdmaud.drv directly lets the engine have the driver
 * list its devices again whenever devices come or go and none of them is open.
 */

/* wdmaud.drv returns a LONG, so only the low half of the result means anything */
typedef LRESULT (APIENTRY *PFN_DRIVER_PROC)(DWORD_PTR, HANDLE, UINT, LPARAM, LPARAM);
typedef DWORD (APIENTRY *PFN_WAVE_MESSAGE)(UINT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);

static HMODULE AeWaveModule;
static PFN_DRIVER_PROC AeDriverProc;
static PFN_WAVE_MESSAGE AeWaveMessage[2];
static BOOL AeWaveListed;
static volatile LONG AeWaveOpenCount;

/* Message numbers of each flow, render first */
static const UINT AeMsgGetNumDevs[2] = { WODM_GETNUMDEVS, WIDM_GETNUMDEVS };
static const UINT AeMsgGetDevCaps[2] = { WODM_GETDEVCAPS, WIDM_GETDEVCAPS };
static const UINT AeMsgOpen[2] = { WODM_OPEN, WIDM_OPEN };
static const UINT AeMsgClose[2] = { WODM_CLOSE, WIDM_CLOSE };
static const UINT AeMsgPrepare[2] = { WODM_PREPARE, WIDM_PREPARE };
static const UINT AeMsgUnprepare[2] = { WODM_UNPREPARE, WIDM_UNPREPARE };
static const UINT AeMsgSubmit[2] = { WODM_WRITE, WIDM_ADDBUFFER };
static const UINT AeMsgReset[2] = { WODM_RESET, WIDM_RESET };

static
BOOL
AeWaveLoadModule(VOID)
{
    if (AeWaveModule)
        return TRUE;

    /* winmm may have the driver loaded, with its devices listed when it could */
    AeWaveListed = GetModuleHandleW(L"wdmaud.drv") != NULL;
    AeWaveModule = LoadLibraryW(L"wdmaud.drv");
    if (!AeWaveModule)
        return FALSE;

    AeDriverProc = (PFN_DRIVER_PROC)GetProcAddress(AeWaveModule, "DriverProc");
    AeWaveMessage[AE_FLOW_RENDER] = (PFN_WAVE_MESSAGE)GetProcAddress(AeWaveModule, "wodMessage");
    AeWaveMessage[AE_FLOW_CAPTURE] = (PFN_WAVE_MESSAGE)GetProcAddress(AeWaveModule, "widMessage");
    if (!AeDriverProc || !AeWaveMessage[AE_FLOW_RENDER] || !AeWaveMessage[AE_FLOW_CAPTURE])
    {
        FreeLibrary(AeWaveModule);
        AeWaveModule = NULL;
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief
 * Has the driver list its devices again. Only while no device is open, as device numbers
 * may change; FALSE if the list was left as it is.
 */
BOOL
AeWaveRefresh(VOID)
{
    if (AeWaveOpenCount || !AeWaveLoadModule())
        return FALSE;

    if (AeWaveListed)
        AeDriverProc(0, NULL, DRV_FREE, 0, 0);

    /* Before the kernel driver is up this fails, and the next rescan tries again */
    AeWaveListed = (LONG)AeDriverProc(0, NULL, DRV_LOAD, 0, 0) != 0;
    return AeWaveListed;
}

UINT
AeWaveGetCount(
    _In_ ULONG Flow)
{
    if (!AeWaveListed)
        return 0;
    return AeWaveMessage[Flow](0, AeMsgGetNumDevs[Flow], 0, 0, 0);
}

MMRESULT
AeWaveGetName(
    _In_ ULONG Flow,
    _In_ UINT Device,
    _Out_writes_(MAXPNAMELEN) PWSTR Name)
{
    WAVEOUTCAPSW OutCaps;
    WAVEINCAPSW InCaps;
    MMRESULT Result;

    if (!AeWaveListed)
        return MMSYSERR_NODRIVER;

    if (Flow == AE_FLOW_RENDER)
    {
        Result = AeWaveMessage[Flow](Device, AeMsgGetDevCaps[Flow], 0, (DWORD_PTR)&OutCaps, sizeof(OutCaps));
        if (Result == MMSYSERR_NOERROR)
            StringCchCopyW(Name, MAXPNAMELEN, OutCaps.szPname);
    }
    else
    {
        Result = AeWaveMessage[Flow](Device, AeMsgGetDevCaps[Flow], 0, (DWORD_PTR)&InCaps, sizeof(InCaps));
        if (Result == MMSYSERR_NOERROR)
            StringCchCopyW(Name, MAXPNAMELEN, InCaps.szPname);
    }
    return Result;
}

/* Opens a device, or with Event NULL only asks whether it takes a format */
static
MMRESULT
AeWaveOpenEx(
    _In_ ULONG Flow,
    _In_ UINT Device,
    _In_ const WAVEFORMATEX *Format,
    _In_opt_ HANDLE Event,
    _Out_opt_ PAE_WAVE Wave)
{
    WAVEOPENDESC Description;
    DWORD_PTR Instance = 0;
    MMRESULT Result;

    if (!AeWaveListed)
        return MMSYSERR_NODRIVER;

    ZeroMemory(&Description, sizeof(Description));
    Description.hWave = (HWAVE)(ULONG_PTR)(Device + 1);
    Description.lpFormat = (LPWAVEFORMATEX)Format;
    Description.dwCallback = (DWORD_PTR)Event;

    Result = AeWaveMessage[Flow](Device, AeMsgOpen[Flow], (DWORD_PTR)&Instance, (DWORD_PTR)&Description,
                                 Event ? CALLBACK_EVENT : WAVE_FORMAT_QUERY);
    if (Result == MMSYSERR_NOERROR && Wave)
    {
        Wave->Flow = Flow;
        Wave->Device = Device;
        Wave->Instance = Instance;
        InterlockedIncrement(&AeWaveOpenCount);
    }
    return Result;
}

MMRESULT
AeWaveQuery(
    _In_ ULONG Flow,
    _In_ UINT Device,
    _In_ const WAVEFORMATEX *Format)
{
    return AeWaveOpenEx(Flow, Device, Format, NULL, NULL);
}

MMRESULT
AeWaveOpen(
    _In_ ULONG Flow,
    _In_ UINT Device,
    _In_ const WAVEFORMATEX *Format,
    _In_ HANDLE Event,
    _Out_ PAE_WAVE Wave)
{
    return AeWaveOpenEx(Flow, Device, Format, Event, Wave);
}

MMRESULT
AeWavePrepare(
    _In_ PAE_WAVE Wave,
    _Inout_ WAVEHDR *Header)
{
    MMRESULT Result = AeWaveMessage[Wave->Flow](Wave->Device, AeMsgPrepare[Wave->Flow], Wave->Instance, (DWORD_PTR)Header, sizeof(*Header));

    /* A driver without its own preparation leaves it to the caller, as winmm does */
    if (Result == MMSYSERR_NOTSUPPORTED)
    {
        Header->dwFlags |= WHDR_PREPARED;
        Result = MMSYSERR_NOERROR;
    }
    return Result;
}

MMRESULT
AeWaveUnprepare(
    _In_ PAE_WAVE Wave,
    _Inout_ WAVEHDR *Header)
{
    MMRESULT Result = AeWaveMessage[Wave->Flow](Wave->Device, AeMsgUnprepare[Wave->Flow], Wave->Instance, (DWORD_PTR)Header, sizeof(*Header));

    if (Result == MMSYSERR_NOTSUPPORTED)
    {
        Header->dwFlags &= ~WHDR_PREPARED;
        Result = MMSYSERR_NOERROR;
    }
    return Result;
}

/* Queues a buffer: played from for render, recorded into for capture */
MMRESULT
AeWaveSubmit(
    _In_ PAE_WAVE Wave,
    _Inout_ WAVEHDR *Header)
{
    return AeWaveMessage[Wave->Flow](Wave->Device, AeMsgSubmit[Wave->Flow], Wave->Instance, (DWORD_PTR)Header, sizeof(*Header));
}

MMRESULT
AeWaveStart(
    _In_ PAE_WAVE Wave)
{
    if (Wave->Flow != AE_FLOW_CAPTURE)
        return MMSYSERR_NOERROR;
    return AeWaveMessage[Wave->Flow](Wave->Device, WIDM_START, Wave->Instance, 0, 0);
}

MMRESULT
AeWaveReset(
    _In_ PAE_WAVE Wave)
{
    return AeWaveMessage[Wave->Flow](Wave->Device, AeMsgReset[Wave->Flow], Wave->Instance, 0, 0);
}

MMRESULT
AeWaveClose(
    _In_ PAE_WAVE Wave)
{
    MMRESULT Result = AeWaveMessage[Wave->Flow](Wave->Device, AeMsgClose[Wave->Flow], Wave->Instance, 0, 0);

    InterlockedDecrement(&AeWaveOpenCount);
    return Result;
}
