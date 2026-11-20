/*
 * PROJECT:     ReactOS Settings
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Sound page, the default devices, volume and a test tone
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "settings.h"
#include <winsvc.h>
#include <mmsystem.h>
#include <math.h>

#define ST_SND_MAX_DEVICES          16
#define ST_SND_STEP                 10
#define ST_SND_RATE                 44100
#define ST_SND_TONE_MS              300

static const WCHAR StSndMapperKey[] = L"Software\\Microsoft\\Multimedia\\Sound Mapper";

typedef struct _ST_SND_DEVICE
{
    UINT Id;
    WCHAR Name[MAXPNAMELEN];
} ST_SND_DEVICE, *PST_SND_DEVICE;

/* The volume and mute controls of the speaker line behind one output */
typedef struct _ST_SND_MIXER
{
    HMIXER Mixer;
    BOOL HaveVolume;
    BOOL HaveMute;
    MIXERCONTROLW Volume;
    MIXERCONTROLW Mute;
} ST_SND_MIXER, *PST_SND_MIXER;

const UINT StSndControls[] =
{
    IDC_SND_OUTPUT, IDC_SND_INPUT, IDC_SND_QUIETER, IDC_SND_LOUDER, IDC_SND_MUTE, IDC_SND_TEST, IDC_SND_REFRESH
};
const DWORD StSndControlCount = _countof(StSndControls);

static ST_SND_DEVICE StSndOutputs[ST_SND_MAX_DEVICES];
static ST_SND_DEVICE StSndInputs[ST_SND_MAX_DEVICES];
static UINT StSndOutputCount;
static UINT StSndInputCount;
static UINT StSndOutput;
static UINT StSndInput;
static ST_SND_MIXER StSndMixer;
static BOOL StSndHaveVolume;
static UINT StSndVolume;
static BOOL StSndMuted;
static BOOL StSndPlaying;
static DWORD StSndServiceState;

static
VOID
StSndReadDefault(
    _In_ LPCWSTR Value,
    _Out_writes_(Count) LPWSTR Name,
    _In_ DWORD Count)
{
    HKEY Key;
    DWORD Size = (Count - 1) * sizeof(*Name);
    DWORD Type;

    Name[0] = UNICODE_NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, StSndMapperKey, 0, KEY_QUERY_VALUE, &Key) != ERROR_SUCCESS)
        return;

    if (RegQueryValueExW(Key, Value, NULL, &Type, (LPBYTE)Name, &Size) != ERROR_SUCCESS || Type != REG_SZ)
        Size = 0;
    Name[Size / sizeof(*Name)] = UNICODE_NULL;
    RegCloseKey(Key);
}

/* The wave mapper picks the device named here, as the Sound control panel saves it */
static
VOID
StSndWriteDefault(
    _In_ LPCWSTR Value,
    _In_ LPCWSTR Name)
{
    HKEY Key;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, StSndMapperKey, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, NULL, &Key, NULL) != ERROR_SUCCESS)
    {
        return;
    }

    RegSetValueExW(Key, Value, 0, REG_SZ, (const BYTE *)Name, (DWORD)((wcslen(Name) + 1) * sizeof(*Name)));
    RegCloseKey(Key);
}

static
DWORD
StSndQueryService(VOID)
{
    SC_HANDLE Manager, Service;
    SERVICE_STATUS Status;
    DWORD State = 0;

    Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!Manager)
        return 0;

    Service = OpenServiceW(Manager, L"AudioSrv", SERVICE_QUERY_STATUS);
    if (Service)
    {
        if (QueryServiceStatus(Service, &Status))
            State = Status.dwCurrentState;
        CloseServiceHandle(Service);
    }

    CloseServiceHandle(Manager);
    return State;
}

static
VOID
StSndCloseMixer(VOID)
{
    if (StSndMixer.Mixer)
        mixerClose(StSndMixer.Mixer);
    ZeroMemory(&StSndMixer, sizeof(StSndMixer));
}

static
BOOL
StSndFindControl(
    _In_ DWORD LineId,
    _In_ DWORD Type,
    _Out_ PMIXERCONTROLW Control)
{
    MIXERLINECONTROLSW Controls;

    ZeroMemory(&Controls, sizeof(Controls));
    Controls.cbStruct = sizeof(Controls);
    Controls.dwLineID = LineId;
    Controls.dwControlType = Type;
    Controls.cControls = 1;
    Controls.cbmxctrl = sizeof(*Control);
    Controls.pamxctrl = Control;

    return mixerGetLineControlsW((HMIXEROBJ)StSndMixer.Mixer, &Controls,
                                 MIXER_GETLINECONTROLSF_ONEBYTYPE) == MMSYSERR_NOERROR;
}

static
VOID
StSndOpenMixer(VOID)
{
    MIXERLINEW Line;
    UINT MixerId;

    StSndCloseMixer();
    if (StSndOutput >= StSndOutputCount)
        return;

    if (mixerGetID((HMIXEROBJ)(UINT_PTR)StSndOutputs[StSndOutput].Id, &MixerId, MIXER_OBJECTF_WAVEOUT) != MMSYSERR_NOERROR ||
        mixerOpen(&StSndMixer.Mixer, MixerId, 0, 0, MIXER_OBJECTF_MIXER) != MMSYSERR_NOERROR)
    {
        StSndMixer.Mixer = NULL;
        return;
    }

    ZeroMemory(&Line, sizeof(Line));
    Line.cbStruct = sizeof(Line);
    Line.dwComponentType = MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;
    if (mixerGetLineInfoW((HMIXEROBJ)StSndMixer.Mixer, &Line, MIXER_GETLINEINFOF_COMPONENTTYPE) != MMSYSERR_NOERROR)
        return;

    StSndMixer.HaveVolume = StSndFindControl(Line.dwLineID, MIXERCONTROL_CONTROLTYPE_VOLUME, &StSndMixer.Volume);
    StSndMixer.HaveMute = StSndFindControl(Line.dwLineID, MIXERCONTROL_CONTROLTYPE_MUTE, &StSndMixer.Mute);
}

static
BOOL
StSndControlValue(
    _In_ const MIXERCONTROLW *Control,
    _Inout_ PDWORD Value,
    _In_ BOOL Set)
{
    MIXERCONTROLDETAILS Details;
    MIXERCONTROLDETAILS_UNSIGNED Unsigned;

    ZeroMemory(&Details, sizeof(Details));
    Details.cbStruct = sizeof(Details);
    Details.dwControlID = Control->dwControlID;
    Details.cChannels = 1;
    Details.cbDetails = sizeof(Unsigned);
    Details.paDetails = &Unsigned;
    Unsigned.dwValue = *Value;

    if (Set)
    {
        return mixerSetControlDetails((HMIXEROBJ)StSndMixer.Mixer, &Details,
                                      MIXER_SETCONTROLDETAILSF_VALUE) == MMSYSERR_NOERROR;
    }

    if (mixerGetControlDetailsW((HMIXEROBJ)StSndMixer.Mixer, &Details,
                                MIXER_GETCONTROLDETAILSF_VALUE) != MMSYSERR_NOERROR)
    {
        return FALSE;
    }

    *Value = Unsigned.dwValue;
    return TRUE;
}

/* Reads the volume from the speaker line, or from the device when it has no mixer */
static
VOID
StSndReadVolume(VOID)
{
    DWORD Value;

    StSndHaveVolume = FALSE;
    StSndMuted = FALSE;
    if (StSndOutput >= StSndOutputCount)
        return;

    if (StSndMixer.HaveVolume && StSndControlValue(&StSndMixer.Volume, &Value, FALSE))
    {
        DWORD Low = StSndMixer.Volume.Bounds.dwMinimum;
        DWORD High = StSndMixer.Volume.Bounds.dwMaximum;

        StSndHaveVolume = TRUE;
        StSndVolume = (High > Low) ? (UINT)(((ULONGLONG)(Value - Low) * 100 + (High - Low) / 2) / (High - Low)) : 0;
    }
    else if (waveOutGetVolume((HWAVEOUT)(UINT_PTR)StSndOutputs[StSndOutput].Id, &Value) == MMSYSERR_NOERROR)
    {
        StSndHaveVolume = TRUE;
        StSndVolume = (UINT)(((Value & 0xFFFF) * 100 + 0x7FFF) / 0xFFFF);
    }

    if (StSndMixer.HaveMute && StSndControlValue(&StSndMixer.Mute, &Value, FALSE))
        StSndMuted = (Value != 0);
}

static
VOID
StSndWriteVolume(
    _In_ UINT Percent)
{
    DWORD Value;

    if (StSndOutput >= StSndOutputCount)
        return;

    if (StSndMixer.HaveVolume)
    {
        DWORD Low = StSndMixer.Volume.Bounds.dwMinimum;
        DWORD High = StSndMixer.Volume.Bounds.dwMaximum;

        Value = Low + (DWORD)(((ULONGLONG)(High - Low) * Percent) / 100);
        StSndControlValue(&StSndMixer.Volume, &Value, TRUE);
    }
    else
    {
        Value = (DWORD)(0xFFFF * Percent / 100);
        waveOutSetVolume((HWAVEOUT)(UINT_PTR)StSndOutputs[StSndOutput].Id, Value | (Value << 16));
    }
}

static
VOID
StSndUpdateControls(VOID)
{
    WCHAR Text[MAXPNAMELEN + 16];
    BOOL HaveOutput = (StSndOutputCount != 0);

    if (StPage != ST_PAGE_SOUND)
        return;

    StShowGroup(StSndControls, StSndControlCount, TRUE);
    ShowWindow(GetDlgItem(StWindow, IDC_SND_OUTPUT), HaveOutput ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(StWindow, IDC_SND_INPUT), StSndInputCount ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(StWindow, IDC_SND_QUIETER), HaveOutput ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(StWindow, IDC_SND_LOUDER), HaveOutput ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(StWindow, IDC_SND_MUTE), HaveOutput ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(StWindow, IDC_SND_TEST), HaveOutput ? SW_SHOW : SW_HIDE);

    if (HaveOutput)
    {
        StringCchPrintfW(Text, _countof(Text), L"Output: %s", StSndOutputs[StSndOutput].Name);
        SetDlgItemTextW(StWindow, IDC_SND_OUTPUT, Text);
    }

    if (StSndInputCount)
    {
        StringCchPrintfW(Text, _countof(Text), L"Input: %s", StSndInputs[StSndInput].Name);
        SetDlgItemTextW(StWindow, IDC_SND_INPUT, Text);
    }

    EnableWindow(GetDlgItem(StWindow, IDC_SND_QUIETER), StSndHaveVolume && StSndVolume > 0);
    EnableWindow(GetDlgItem(StWindow, IDC_SND_LOUDER), StSndHaveVolume && StSndVolume < 100);
    EnableWindow(GetDlgItem(StWindow, IDC_SND_MUTE), StSndMixer.HaveMute);
    EnableWindow(GetDlgItem(StWindow, IDC_SND_TEST), !StSndPlaying);
    SetDlgItemTextW(StWindow, IDC_SND_MUTE, StSndMuted ? L"Muted" : L"Mute");
    SetDlgItemTextW(StWindow, IDC_SND_TEST, StSndPlaying ? L"Playing" : L"Test sound");
    InvalidateRect(GetDlgItem(StWindow, IDC_SND_MUTE), NULL, FALSE);
}

/* Enumerates both directions and keeps the default devices the mapper uses */
static
VOID
StSndRead(VOID)
{
    WCHAR Default[MAXPNAMELEN];
    WAVEOUTCAPSW OutCaps;
    WAVEINCAPSW InCaps;
    UINT Count, i;

    StSndOutputCount = 0;
    StSndInputCount = 0;
    StSndOutput = 0;
    StSndInput = 0;
    StSndServiceState = StSndQueryService();

    StSndReadDefault(L"Playback", Default, _countof(Default));
    Count = waveOutGetNumDevs();
    for (i = 0; i < Count && StSndOutputCount < _countof(StSndOutputs); i++)
    {
        if (waveOutGetDevCapsW(i, &OutCaps, sizeof(OutCaps)) != MMSYSERR_NOERROR)
            continue;

        if (!_wcsicmp(OutCaps.szPname, Default))
            StSndOutput = StSndOutputCount;
        StSndOutputs[StSndOutputCount].Id = i;
        StringCchCopyW(StSndOutputs[StSndOutputCount].Name, MAXPNAMELEN, OutCaps.szPname);
        StSndOutputCount++;
    }

    StSndReadDefault(L"Record", Default, _countof(Default));
    Count = waveInGetNumDevs();
    for (i = 0; i < Count && StSndInputCount < _countof(StSndInputs); i++)
    {
        if (waveInGetDevCapsW(i, &InCaps, sizeof(InCaps)) != MMSYSERR_NOERROR)
            continue;

        if (!_wcsicmp(InCaps.szPname, Default))
            StSndInput = StSndInputCount;
        StSndInputs[StSndInputCount].Id = i;
        StringCchCopyW(StSndInputs[StSndInputCount].Name, MAXPNAMELEN, InCaps.szPname);
        StSndInputCount++;
    }

    StSndOpenMixer();
    StSndReadVolume();
}

static
VOID
StSndRefresh(VOID)
{
    StSndRead();
    StSndUpdateControls();
    StRelayout();
    InvalidateRect(StWindow, NULL, FALSE);
}

/* Two short rising notes, faded at the edges so the speaker does not click */
static
DWORD
WINAPI
StSndToneWorker(
    _In_ LPVOID Parameter)
{
    UINT Device = (UINT)(UINT_PTR)Parameter;
    UINT Samples = ST_SND_RATE * ST_SND_TONE_MS * 2 / 1000;
    UINT Fade = ST_SND_RATE / 100;
    WAVEFORMATEX Format;
    WAVEHDR Header;
    HWAVEOUT Wave;
    HANDLE Done;
    SHORT *Pcm;
    MMRESULT Result;
    UINT i;

    Done = CreateEventW(NULL, FALSE, FALSE, NULL);
    Pcm = HeapAlloc(GetProcessHeap(), 0, Samples * sizeof(*Pcm));
    if (!Done || !Pcm)
    {
        Result = MMSYSERR_NOMEM;
        goto Exit;
    }

    for (i = 0; i < Samples; i++)
    {
        UINT Note = Samples / 2;
        UINT Offset = i % Note;
        double Frequency = (i < Note) ? 660.0 : 880.0;
        double Level = 0.3;

        if (Offset < Fade)
            Level *= (double)Offset / Fade;
        else if (Note - Offset < Fade)
            Level *= (double)(Note - Offset) / Fade;

        Pcm[i] = (SHORT)(sin(2.0 * 3.14159265358979 * Frequency * i / ST_SND_RATE) * Level * 32767.0);
    }

    ZeroMemory(&Format, sizeof(Format));
    Format.wFormatTag = WAVE_FORMAT_PCM;
    Format.nChannels = 1;
    Format.nSamplesPerSec = ST_SND_RATE;
    Format.wBitsPerSample = 16;
    Format.nBlockAlign = Format.nChannels * Format.wBitsPerSample / 8;
    Format.nAvgBytesPerSec = Format.nSamplesPerSec * Format.nBlockAlign;

    Result = waveOutOpen(&Wave, Device, &Format, (DWORD_PTR)Done, 0, CALLBACK_EVENT);
    if (Result != MMSYSERR_NOERROR)
        goto Exit;

    ZeroMemory(&Header, sizeof(Header));
    Header.lpData = (LPSTR)Pcm;
    Header.dwBufferLength = Samples * sizeof(*Pcm);
    Result = waveOutPrepareHeader(Wave, &Header, sizeof(Header));
    if (Result == MMSYSERR_NOERROR)
    {
        Result = waveOutWrite(Wave, &Header, sizeof(Header));
        if (Result == MMSYSERR_NOERROR)
        {
            /* The open completion signals the event too, so wait on the header itself */
            while (!(Header.dwFlags & WHDR_DONE))
            {
                if (WaitForSingleObject(Done, ST_SND_TONE_MS * 4) == WAIT_TIMEOUT)
                {
                    waveOutReset(Wave);
                    Result = MMSYSERR_ERROR;
                    break;
                }
            }
        }
        waveOutUnprepareHeader(Wave, &Header, sizeof(Header));
    }
    waveOutClose(Wave);

Exit:
    if (Pcm)
        HeapFree(GetProcessHeap(), 0, Pcm);
    if (Done)
        CloseHandle(Done);

    PostMessageW(StWindow, WM_SND_DONE, 0, (LPARAM)Result);
    return 0;
}

VOID
StSndDone(
    _In_ LPARAM Result)
{
    WCHAR Text[MAXERRORLENGTH];

    StSndPlaying = FALSE;

    if ((MMRESULT)Result == MMSYSERR_NOERROR)
    {
        StSetStatus(L"Played the test sound");
    }
    else
    {
        if (waveOutGetErrorTextW((MMRESULT)Result, Text, _countof(Text)) != MMSYSERR_NOERROR)
            StringCchPrintfW(Text, _countof(Text), L"error %lu", (ULONG)Result);
        StSetStatus(L"The test sound failed: %s", Text);
    }

    StSndUpdateControls();
}

VOID
StSndCreateControls(VOID)
{
    StButton(L"Output", WS_TABSTOP, IDC_SND_OUTPUT);
    StButton(L"Input", WS_TABSTOP, IDC_SND_INPUT);
    StButton(L"-", WS_TABSTOP, IDC_SND_QUIETER);
    StButton(L"+", WS_TABSTOP, IDC_SND_LOUDER);
    StButton(L"Mute", WS_TABSTOP, IDC_SND_MUTE);
    StButton(L"Test sound", WS_TABSTOP, IDC_SND_TEST);
    StButton(L"Refresh", WS_TABSTOP, IDC_SND_REFRESH);
}

VOID
StSndShow(VOID)
{
    StSndRefresh();
}

VOID
StSndHeading(
    _Out_writes_(Count) LPWSTR Text,
    _In_ DWORD Count)
{
    StringCchCopyW(Text, Count, L"Sound");
}

/* Rows below the card: output, input, volume, mute and test, refresh */
static
int
StSndRowTop(
    _In_ const RECT *Area,
    _In_ UINT Row)
{
    return Area->top + ST_CARD_H + ST_GAP + (int)Row * (ST_ROW + ST_GAP);
}

VOID
StSndLayout(
    _In_ const RECT *Area)
{
    int Width = Area->right - Area->left;
    int Half = (Width - ST_GAP) / 2;
    int Left = Area->left;
    int Step = ST_ROW + ST_ROW / 2;

    StMove(IDC_SND_OUTPUT, Left, StSndRowTop(Area, 0), Width, ST_ROW);
    StMove(IDC_SND_INPUT, Left, StSndRowTop(Area, 1), Width, ST_ROW);
    StMove(IDC_SND_QUIETER, Left, StSndRowTop(Area, 2), Step, ST_ROW);
    StMove(IDC_SND_LOUDER, Area->right - Step, StSndRowTop(Area, 2), Step, ST_ROW);
    StMove(IDC_SND_MUTE, Left, StSndRowTop(Area, 3), Half, ST_ROW);
    StMove(IDC_SND_TEST, Left + Half + ST_GAP, StSndRowTop(Area, 3), Half, ST_ROW);
    StMove(IDC_SND_REFRESH, Left, StSndOutputCount ? StSndRowTop(Area, 4) : StSndRowTop(Area, 0), Width, ST_ROW);
}

VOID
StSndPaint(
    _In_ HDC Dc,
    _In_ const RECT *Area)
{
    WCHAR Title[MAXPNAMELEN + 32];
    WCHAR Detail[256];
    LPCWSTR Service;
    RECT Card, Track, Fill, Label;
    int Step = ST_ROW + ST_ROW / 2;

    switch (StSndServiceState)
    {
        case SERVICE_RUNNING:
            Service = L"running";
            break;
        case SERVICE_START_PENDING:
            Service = L"starting";
            break;
        case 0:
            Service = L"not installed";
            break;
        default:
            Service = L"stopped";
            break;
    }

    Card = *Area;
    Card.bottom = Card.top + ST_CARD_H;

    if (!StSndOutputCount)
    {
        StringCchCopyW(Title, _countof(Title), L"No sound devices");
        StringCchPrintfW(Detail, _countof(Detail),
                         L"The audio service is %s. Playback devices show up here once the audio drivers load.", Service);
        StDrawCard(Dc, &Card, Title, Detail, FALSE);
        return;
    }

    StringCchCopyW(Title, _countof(Title), StSndOutputs[StSndOutput].Name);
    if (StSndHaveVolume)
    {
        StringCchPrintfW(Detail, _countof(Detail), L"Volume %u%%%s. %u output(s), %u input(s), audio service %s.",
                         StSndVolume, StSndMuted ? L", muted" : L"", StSndOutputCount, StSndInputCount, Service);
    }
    else
    {
        StringCchPrintfW(Detail, _countof(Detail), L"This device has no volume control. Audio service %s.", Service);
    }
    StDrawCard(Dc, &Card, Title, Detail, !StSndMuted);

    if (!StSndHaveVolume)
        return;

    /* Volume bar between the quieter and louder buttons */
    Track.left = Area->left + Step + ST_GAP;
    Track.right = Area->right - Step - ST_GAP;
    Track.top = StSndRowTop(Area, 2) + ST_ROW / 2 - 12;
    Track.bottom = Track.top + 24;
    StFillRound(Dc, &Track, 24, ST_DIM);

    Fill = Track;
    Fill.right = Track.left + (Track.right - Track.left) * (int)StSndVolume / 100;
    if (Fill.right > Fill.left)
        StFillRound(Dc, &Fill, 24, StSndMuted ? ST_SUBTEXT : ST_ACCENT);

    Label = Track;
    Label.top = StSndRowTop(Area, 2);
    Label.bottom = Track.top;
    StringCchPrintfW(Detail, _countof(Detail), L"%u%%", StSndVolume);
    SetBkMode(Dc, TRANSPARENT);
    SelectObject(Dc, StSmallFont);
    SetTextColor(Dc, ST_SUBTEXT);
    DrawTextW(Dc, Detail, -1, &Label, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
}

VOID
StSndCommand(
    _In_ UINT Id,
    _In_ UINT Code)
{
    DWORD Value;

    if (Code != BN_CLICKED)
        return;

    switch (Id)
    {
        case IDC_SND_OUTPUT:
            if (StSndOutputCount < 2)
            {
                StSetStatus(L"This is the only output");
                break;
            }

            StSndOutput = (StSndOutput + 1) % StSndOutputCount;
            StSndWriteDefault(L"Playback", StSndOutputs[StSndOutput].Name);
            StSndOpenMixer();
            StSndReadVolume();
            StSetStatus(L"Sound now plays on %s", StSndOutputs[StSndOutput].Name);
            break;

        case IDC_SND_INPUT:
            if (StSndInputCount < 2)
            {
                StSetStatus(L"This is the only input");
                break;
            }

            StSndInput = (StSndInput + 1) % StSndInputCount;
            StSndWriteDefault(L"Record", StSndInputs[StSndInput].Name);
            StSetStatus(L"Recording now uses %s", StSndInputs[StSndInput].Name);
            break;

        case IDC_SND_QUIETER:
        case IDC_SND_LOUDER:
            if (!StSndHaveVolume)
                break;

            if (Id == IDC_SND_LOUDER)
                StSndVolume = min(StSndVolume + ST_SND_STEP, 100);
            else
                StSndVolume = (StSndVolume > ST_SND_STEP) ? StSndVolume - ST_SND_STEP : 0;

            /* Snap to the step so repeated taps land on round numbers */
            StSndVolume = (StSndVolume + ST_SND_STEP / 2) / ST_SND_STEP * ST_SND_STEP;
            StSndWriteVolume(StSndVolume);
            StSndReadVolume();
            break;

        case IDC_SND_MUTE:
            if (!StSndMixer.HaveMute)
                break;

            Value = !StSndMuted;
            if (!StSndControlValue(&StSndMixer.Mute, &Value, TRUE))
                StSetStatus(L"The device did not take the mute");
            StSndReadVolume();
            break;

        case IDC_SND_TEST:
            if (StSndPlaying || StSndOutput >= StSndOutputCount)
                break;

            if (!StStartWorker(StSndToneWorker, (PVOID)(UINT_PTR)StSndOutputs[StSndOutput].Id))
            {
                StSetStatus(L"Could not start the test sound");
                break;
            }

            StSndPlaying = TRUE;
            StSetStatus(L"Playing a test sound on %s", StSndOutputs[StSndOutput].Name);
            break;

        case IDC_SND_REFRESH:
            StSndRefresh();
            StSetStatus(L"Found %u output(s) and %u input(s)", StSndOutputCount, StSndInputCount);
            return;
    }

    StSndUpdateControls();
    InvalidateRect(StWindow, NULL, FALSE);
}

/* Devices appear once the audio drivers load, so keep looking while the page is up */
VOID
StSndTimer(VOID)
{
    if (waveOutGetNumDevs() != StSndOutputCount || waveInGetNumDevs() != StSndInputCount)
    {
        StSndRefresh();
        return;
    }

    if (StSndOutputCount)
    {
        StSndReadVolume();
        StSndUpdateControls();
        InvalidateRect(StWindow, NULL, FALSE);
    }
}

BOOL
StSndIsActive(
    _In_ UINT Id)
{
    return (Id == IDC_SND_MUTE) && StSndMuted;
}

VOID
StSndCleanup(VOID)
{
    StSndCloseMixer();
}
