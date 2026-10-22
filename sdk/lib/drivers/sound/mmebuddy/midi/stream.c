/*
 * PROJECT:     ReactOS Sound System "MME Buddy" Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     MIDI data to and from the device as KS music records
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"
#include <ks.h>
#include <ksmedia.h>

/* Reads kept outstanding on an input pin, and the bytes each one asks for */
#define MIDI_IN_READS           4
#define MIDI_IN_READ_BYTES      512

/* How long closing or stopping waits for the device to hand its requests back */
#define MIDI_DRAIN_MS           2000

/* Records are padded so the next one starts on a ULONG */
#define MIDI_RECORD_PAD(Bytes)  (((Bytes) + sizeof(ULONG) - 1) & ~(sizeof(ULONG) - 1))

/* One write or read in flight; the KS records follow the overlapped part */
typedef struct _MIDI_PACKET
{
    SOUND_OVERLAPPED Overlap;
    LPMIDIHDR Header;
    DWORD Length;
    BYTE Data[ANYSIZE_ARRAY];
} MIDI_PACKET, *PMIDI_PACKET;

static
VOID
MidiNotify(
    _In_ PSOUND_DEVICE_INSTANCE Instance,
    _In_ UINT Message,
    _In_ DWORD_PTR Parameter1,
    _In_ DWORD_PTR Parameter2)
{
    if (Instance->WinMM.ClientCallback)
    {
        DriverCallback(Instance->WinMM.ClientCallback,
                       HIWORD(Instance->WinMM.Flags),
                       Instance->WinMM.Handle,
                       Message,
                       Instance->WinMM.ClientCallbackInstanceData,
                       Parameter1,
                       Parameter2);
    }
}

/* Bytes in a message that starts with Status, the status byte included */
static
ULONG
MidiMessageLength(
    _In_ BYTE Status)
{
    if (Status < 0xF0)
        return ((Status & 0xE0) == 0xC0) ? 2 : 3;

    switch (Status)
    {
        case 0xF1:
        case 0xF3:
            return 2;
        case 0xF2:
            return 3;
        default:
            return 1;
    }
}

static
MMRESULT
MidiGetFunctionTable(
    _In_ PSOUND_DEVICE_INSTANCE Instance,
    _Out_ PMMFUNCTION_TABLE *FunctionTable)
{
    PSOUND_DEVICE SoundDevice;
    MMRESULT Result;

    Result = GetSoundDeviceFromInstance(Instance, &SoundDevice);
    if (!MMSUCCESS(Result))
        return TranslateInternalMmResult(Result);

    Result = GetSoundDeviceFunctionTable(SoundDevice, FunctionTable);
    if (!MMSUCCESS(Result))
        return TranslateInternalMmResult(Result);

    return (*FunctionTable)->CommitWaveBuffer ? MMSYSERR_NOERROR : MMSYSERR_NOTSUPPORTED;
}

/* A packet holding one record of Bytes bytes, ready to be written */
static
PMIDI_PACKET
MidiAllocateWrite(
    _In_ PSOUND_DEVICE_INSTANCE Instance,
    _In_reads_bytes_(Bytes) const BYTE *Data,
    _In_ DWORD Bytes)
{
    PKSMUSICFORMAT Record;
    PMIDI_PACKET Packet;
    DWORD Length = sizeof(*Record) + MIDI_RECORD_PAD(Bytes);

    Packet = AllocateMemory(FIELD_OFFSET(MIDI_PACKET, Data) + Length);
    if (!Packet)
        return NULL;

    Packet->Overlap.SoundDeviceInstance = Instance;
    Packet->Length = Length;
    Record = (PKSMUSICFORMAT)Packet->Data;
    Record->TimeDeltaMs = 0;
    Record->ByteCount = Bytes;
    CopyMemory(Record + 1, Data, Bytes);
    return Packet;
}

static
VOID
CALLBACK
MidiWriteDone(
    _In_ DWORD ErrorCode,
    _In_ DWORD Bytes,
    _In_ LPOVERLAPPED Overlapped)
{
    PMIDI_PACKET Packet = CONTAINING_RECORD(Overlapped, MIDI_PACKET, Overlap.Standard);
    PSOUND_DEVICE_INSTANCE Instance = Packet->Overlap.SoundDeviceInstance;

    Instance->MidiPending--;
    if (Packet->Header)
    {
        Packet->Header->dwFlags &= ~MHDR_INQUEUE;
        Packet->Header->dwFlags |= MHDR_DONE;
        MidiNotify(Instance, MOM_DONE, (DWORD_PTR)Packet->Header, 0);
    }
    FreeMemory(Packet);
}

/* Runs on the sound thread, so the completion comes back to it */
static
MMRESULT
MidiSubmitWrite(
    _In_ PSOUND_DEVICE_INSTANCE Instance,
    _In_ PVOID Parameter)
{
    PMIDI_PACKET Packet = (PMIDI_PACKET)Parameter;
    PMMFUNCTION_TABLE FunctionTable;
    MMRESULT Result;

    Result = MidiGetFunctionTable(Instance, &FunctionTable);
    if (!MMSUCCESS(Result))
        return Result;

    Result = FunctionTable->CommitWaveBuffer(Instance, Packet->Data, Packet->Length, &Packet->Overlap, MidiWriteDone);
    if (MMSUCCESS(Result))
        Instance->MidiPending++;
    return Result;
}

static
MMRESULT
MidiWrite(
    _In_ PSOUND_DEVICE_INSTANCE Instance,
    _In_reads_bytes_(Bytes) const BYTE *Data,
    _In_ DWORD Bytes,
    _In_opt_ LPMIDIHDR Header)
{
    PMIDI_PACKET Packet;
    MMRESULT Result;

    Packet = MidiAllocateWrite(Instance, Data, Bytes);
    if (!Packet)
        return MMSYSERR_NOMEM;

    Packet->Header = Header;
    Result = CallSoundThread(Instance, MidiSubmitWrite, Packet);
    if (!MMSUCCESS(Result))
        FreeMemory(Packet);
    return Result;
}

/* Lets completions run until the device has handed back every request */
static
MMRESULT
MidiDrain(
    _In_ PSOUND_DEVICE_INSTANCE Instance,
    _In_ PVOID Parameter)
{
    DWORD Start = GetTickCount();

    UNREFERENCED_PARAMETER(Parameter);
    while (Instance->MidiPending && GetTickCount() - Start < MIDI_DRAIN_MS)
        SleepEx(10, TRUE);
    return Instance->MidiPending ? MMSYSERR_ERROR : MMSYSERR_NOERROR;
}

MMRESULT
MmePrepareMidiHeader(
    _In_ DWORD_PTR PrivateHandle,
    _Inout_ DWORD_PTR Parameter)
{
    LPMIDIHDR Header = (LPMIDIHDR)Parameter;

    VALIDATE_MMSYS_PARAMETER(PrivateHandle);
    VALIDATE_MMSYS_PARAMETER(Header);

    Header->dwFlags |= MHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

MMRESULT
MmeUnprepareMidiHeader(
    _In_ DWORD_PTR PrivateHandle,
    _Inout_ DWORD_PTR Parameter)
{
    LPMIDIHDR Header = (LPMIDIHDR)Parameter;

    VALIDATE_MMSYS_PARAMETER(PrivateHandle);
    VALIDATE_MMSYS_PARAMETER(Header);

    if (Header->dwFlags & MHDR_INQUEUE)
        return MIDIERR_STILLPLAYING;
    Header->dwFlags &= ~MHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

/**
 * @brief
 * Sends one short message. A message without a status byte goes out with the
 * last channel status, as running status allows.
 */
MMRESULT
MmeWriteMidiShort(
    _In_ DWORD_PTR PrivateHandle,
    _In_ DWORD Message)
{
    PSOUND_DEVICE_INSTANCE Instance = (PSOUND_DEVICE_INSTANCE)PrivateHandle;
    BYTE Bytes[3];
    ULONG Length, Index, First = 1;

    VALIDATE_MMSYS_PARAMETER(IsValidSoundDeviceInstance(Instance));

    Bytes[0] = (BYTE)Message;
    if (Bytes[0] < 0x80)
    {
        /* the message is data only and follows the status sent before */
        if (!Instance->MidiRunningStatus)
            return MMSYSERR_NOERROR;
        Bytes[0] = Instance->MidiRunningStatus;
        First = 0;
    }
    if (Bytes[0] == 0xF0 || Bytes[0] == 0xF7)
        return MMSYSERR_INVALPARAM;

    Length = MidiMessageLength(Bytes[0]);
    for (Index = 1; Index < Length; Index++)
        Bytes[Index] = (BYTE)(Message >> (8 * (Index - 1 + First)));

    /* Channel messages set the running status, system common ones clear it, real time ones leave it */
    if (Bytes[0] < 0xF0)
        Instance->MidiRunningStatus = Bytes[0];
    else if (Bytes[0] < 0xF8)
        Instance->MidiRunningStatus = 0;

    return MidiWrite(Instance, Bytes, Length, NULL);
}

MMRESULT
MmeWriteMidiLong(
    _In_ DWORD_PTR PrivateHandle,
    _Inout_ DWORD_PTR Parameter)
{
    PSOUND_DEVICE_INSTANCE Instance = (PSOUND_DEVICE_INSTANCE)PrivateHandle;
    LPMIDIHDR Header = (LPMIDIHDR)Parameter;
    MMRESULT Result;

    VALIDATE_MMSYS_PARAMETER(IsValidSoundDeviceInstance(Instance));
    VALIDATE_MMSYS_PARAMETER(Header && Header->lpData);

    if (!(Header->dwFlags & MHDR_PREPARED))
        return MIDIERR_UNPREPARED;
    if (Header->dwFlags & MHDR_INQUEUE)
        return MIDIERR_STILLPLAYING;

    Header->dwFlags &= ~MHDR_DONE;
    Header->dwFlags |= MHDR_INQUEUE;
    Instance->MidiRunningStatus = 0;

    Result = MidiWrite(Instance, (const BYTE *)Header->lpData, Header->dwBufferLength, Header);
    if (!MMSUCCESS(Result))
        Header->dwFlags &= ~MHDR_INQUEUE;
    return Result;
}

/* Releases the sustain pedal and ends every note on all sixteen channels */
MMRESULT
MmeResetMidiOutput(
    _In_ DWORD_PTR PrivateHandle)
{
    PSOUND_DEVICE_INSTANCE Instance = (PSOUND_DEVICE_INSTANCE)PrivateHandle;
    BYTE Bytes[16 * 6];
    ULONG Channel;

    VALIDATE_MMSYS_PARAMETER(IsValidSoundDeviceInstance(Instance));

    for (Channel = 0; Channel < 16; Channel++)
    {
        Bytes[Channel * 6 + 0] = (BYTE)(0xB0 | Channel);
        Bytes[Channel * 6 + 1] = 64;
        Bytes[Channel * 6 + 2] = 0;
        Bytes[Channel * 6 + 3] = (BYTE)(0xB0 | Channel);
        Bytes[Channel * 6 + 4] = 123;
        Bytes[Channel * 6 + 5] = 0;
    }
    Instance->MidiRunningStatus = 0;
    return MidiWrite(Instance, Bytes, sizeof(Bytes), NULL);
}

/* Hands back the buffer system exclusive data is collected in */
static
VOID
MidiInReturnHeader(
    _In_ PSOUND_DEVICE_INSTANCE Instance,
    _In_ DWORD Time)
{
    LPMIDIHDR Header = Instance->MidiInHead;

    if (!Header)
        return;

    Instance->MidiInHead = Header->lpNext;
    Header->lpNext = NULL;
    Header->dwFlags &= ~MHDR_INQUEUE;
    Header->dwFlags |= MHDR_DONE;
    MidiNotify(Instance, MIM_LONGDATA, (DWORD_PTR)Header, Time);
}

static
VOID
MidiInSysExByte(
    _In_ PSOUND_DEVICE_INSTANCE Instance,
    _In_ BYTE Byte,
    _In_ DWORD Time)
{
    LPMIDIHDR Header = Instance->MidiInHead;

    /* Without a buffer the data has nowhere to go */
    if (!Header)
        return;

    Header->lpData[Header->dwBytesRecorded++] = Byte;
    if (Byte == 0xF7 || Header->dwBytesRecorded >= Header->dwBufferLength)
        MidiInReturnHeader(Instance, Time);
}

/* Puts input bytes back together into messages for the client */
static
VOID
MidiInByte(
    _In_ PSOUND_DEVICE_INSTANCE Instance,
    _In_ BYTE Byte,
    _In_ DWORD Time)
{
    ULONG Length;

    if (Byte >= 0xF8)
    {
        MidiNotify(Instance, MIM_DATA, Byte, Time);
        return;
    }

    if (Instance->MidiInSysEx)
    {
        if (Byte < 0x80 || Byte == 0xF7)
        {
            MidiInSysExByte(Instance, Byte, Time);
            Instance->MidiInSysEx = (Byte != 0xF7);
            return;
        }

        /* Another status ends the system exclusive message early */
        Instance->MidiInSysEx = FALSE;
        if (Instance->MidiInHead && Instance->MidiInHead->dwBytesRecorded)
            MidiInReturnHeader(Instance, Time);
    }

    if (Byte == 0xF0)
    {
        Instance->MidiInSysEx = TRUE;
        Instance->MidiInStatus = 0;
        MidiInSysExByte(Instance, Byte, Time);
        return;
    }

    if (Byte >= 0x80)
    {
        Instance->MidiInStatus = Byte;
        Instance->MidiInCount = 0;
        if (MidiMessageLength(Byte) == 1)
        {
            MidiNotify(Instance, MIM_DATA, Byte, Time);
            Instance->MidiInStatus = 0;
        }
        return;
    }

    /* Data without a status to belong to is dropped */
    if (!Instance->MidiInStatus)
        return;

    Instance->MidiInData[Instance->MidiInCount++] = Byte;
    Length = MidiMessageLength(Instance->MidiInStatus);
    if (Instance->MidiInCount < Length - 1)
        return;

    MidiNotify(Instance,
               MIM_DATA,
               Instance->MidiInStatus | (Instance->MidiInData[0] << 8) | ((Length > 2) ? (Instance->MidiInData[1] << 16) : 0),
               Time);
    Instance->MidiInCount = 0;
    if (Instance->MidiInStatus >= 0xF0)
        Instance->MidiInStatus = 0;
}

static
VOID
CALLBACK
MidiReadDone(
    _In_ DWORD ErrorCode,
    _In_ DWORD Bytes,
    _In_ LPOVERLAPPED Overlapped);

static
MMRESULT
MidiSubmitRead(
    _In_ PSOUND_DEVICE_INSTANCE Instance,
    _In_ PMIDI_PACKET Packet)
{
    PMMFUNCTION_TABLE FunctionTable;
    MMRESULT Result;

    Result = MidiGetFunctionTable(Instance, &FunctionTable);
    if (!MMSUCCESS(Result))
        return Result;

    ZeroMemory(&Packet->Overlap.Standard, sizeof(Packet->Overlap.Standard));
    Result = FunctionTable->CommitWaveBuffer(Instance, Packet->Data, Packet->Length, &Packet->Overlap, MidiReadDone);
    if (MMSUCCESS(Result))
        Instance->MidiPending++;
    return Result;
}

static
VOID
CALLBACK
MidiReadDone(
    _In_ DWORD ErrorCode,
    _In_ DWORD Bytes,
    _In_ LPOVERLAPPED Overlapped)
{
    PMIDI_PACKET Packet = CONTAINING_RECORD(Overlapped, MIDI_PACKET, Overlap.Standard);
    PSOUND_DEVICE_INSTANCE Instance = Packet->Overlap.SoundDeviceInstance;
    PKSMUSICFORMAT Record;
    DWORD Offset = 0, Count, Index, Time;

    Instance->MidiPending--;
    Time = GetTickCount() - Instance->MidiInStartTime;

    if (ErrorCode == ERROR_SUCCESS)
    {
        Bytes = min(Bytes, Packet->Length);
        while (Offset + sizeof(*Record) <= Bytes)
        {
            Record = (PKSMUSICFORMAT)(Packet->Data + Offset);
            Count = min(Record->ByteCount, Bytes - Offset - sizeof(*Record));
            for (Index = 0; Index < Count; Index++)
                MidiInByte(Instance, ((PBYTE)(Record + 1))[Index], Time);
            Offset += sizeof(*Record) + MIDI_RECORD_PAD(Record->ByteCount);
        }
    }

    /* While input runs the same packet goes straight back to the device */
    if (Instance->MidiInStarted && ErrorCode == ERROR_SUCCESS &&
        MMSUCCESS(MidiSubmitRead(Instance, Packet)))
    {
        return;
    }
    FreeMemory(Packet);
}

static
MMRESULT
MidiQueueBuffer(
    _In_ PSOUND_DEVICE_INSTANCE Instance,
    _In_ PVOID Parameter)
{
    LPMIDIHDR Header = (LPMIDIHDR)Parameter;
    LPMIDIHDR *Tail = &Instance->MidiInHead;

    while (*Tail)
        Tail = &(*Tail)->lpNext;
    *Tail = Header;
    return MMSYSERR_NOERROR;
}

MMRESULT
MmeAddMidiInBuffer(
    _In_ DWORD_PTR PrivateHandle,
    _Inout_ DWORD_PTR Parameter)
{
    PSOUND_DEVICE_INSTANCE Instance = (PSOUND_DEVICE_INSTANCE)PrivateHandle;
    LPMIDIHDR Header = (LPMIDIHDR)Parameter;

    VALIDATE_MMSYS_PARAMETER(IsValidSoundDeviceInstance(Instance));
    VALIDATE_MMSYS_PARAMETER(Header && Header->lpData && Header->dwBufferLength);

    if (!(Header->dwFlags & MHDR_PREPARED))
        return MIDIERR_UNPREPARED;
    if (Header->dwFlags & MHDR_INQUEUE)
        return MIDIERR_STILLPLAYING;

    Header->dwFlags &= ~MHDR_DONE;
    Header->dwFlags |= MHDR_INQUEUE;
    Header->dwBytesRecorded = 0;
    Header->lpNext = NULL;
    return CallSoundThread(Instance, MidiQueueBuffer, Header);
}

static
MMRESULT
MidiStartReads(
    _In_ PSOUND_DEVICE_INSTANCE Instance,
    _In_ PVOID Parameter)
{
    PMIDI_PACKET Packet;
    MMRESULT Result = MMSYSERR_NOERROR;

    UNREFERENCED_PARAMETER(Parameter);

    if (Instance->MidiInStarted)
        return MMSYSERR_NOERROR;

    Instance->MidiInStarted = TRUE;
    Instance->MidiInStartTime = GetTickCount();
    Instance->MidiInStatus = 0;
    Instance->MidiInSysEx = FALSE;

    while (MMSUCCESS(Result) && Instance->MidiPending < MIDI_IN_READS)
    {
        Packet = AllocateMemory(FIELD_OFFSET(MIDI_PACKET, Data) + MIDI_IN_READ_BYTES);
        if (!Packet)
            return MMSYSERR_NOMEM;

        Packet->Overlap.SoundDeviceInstance = Instance;
        Packet->Length = MIDI_IN_READ_BYTES;
        Result = MidiSubmitRead(Instance, Packet);
        if (!MMSUCCESS(Result))
            FreeMemory(Packet);
    }
    return Result;
}

MMRESULT
MmeStartMidiInput(
    _In_ DWORD_PTR PrivateHandle)
{
    PSOUND_DEVICE_INSTANCE Instance = (PSOUND_DEVICE_INSTANCE)PrivateHandle;
    MMRESULT Result;

    VALIDATE_MMSYS_PARAMETER(IsValidSoundDeviceInstance(Instance));

    Result = MmeSetState(PrivateHandle, TRUE);
    if (!MMSUCCESS(Result))
        return Result;
    return CallSoundThread(Instance, MidiStartReads, NULL);
}

static
MMRESULT
MidiStopReads(
    _In_ PSOUND_DEVICE_INSTANCE Instance,
    _In_ PVOID Parameter)
{
    BOOL Reset = (BOOL)(ULONG_PTR)Parameter;
    DWORD Time = GetTickCount() - Instance->MidiInStartTime;

    Instance->MidiInStarted = FALSE;
    MidiDrain(Instance, NULL);

    /* Stopping returns a partly filled buffer, resetting returns all of them */
    if (Instance->MidiInHead && Instance->MidiInHead->dwBytesRecorded)
        MidiInReturnHeader(Instance, Time);
    while (Reset && Instance->MidiInHead)
        MidiInReturnHeader(Instance, Time);

    Instance->MidiInSysEx = FALSE;
    Instance->MidiInStatus = 0;
    return MMSYSERR_NOERROR;
}

MMRESULT
MmeStopMidiInput(
    _In_ DWORD_PTR PrivateHandle,
    _In_ BOOL Reset)
{
    PSOUND_DEVICE_INSTANCE Instance = (PSOUND_DEVICE_INSTANCE)PrivateHandle;

    VALIDATE_MMSYS_PARAMETER(IsValidSoundDeviceInstance(Instance));

    /* Stopping the pin gives back the reads it holds */
    MmeSetState(PrivateHandle, FALSE);
    return CallSoundThread(Instance, MidiStopReads, (PVOID)(ULONG_PTR)Reset);
}

/**
 * @brief
 * Closes a MIDI device once its requests are back; input still holding the
 * client's buffers stays open, as the client must reset it first.
 */
MMRESULT
MmeCloseMidiDevice(
    _In_ DWORD_PTR PrivateHandle)
{
    PSOUND_DEVICE_INSTANCE Instance = (PSOUND_DEVICE_INSTANCE)PrivateHandle;

    VALIDATE_MMSYS_PARAMETER(IsValidSoundDeviceInstance(Instance));

    if (Instance->MidiInHead)
        return MIDIERR_STILLPLAYING;

    if (Instance->MidiInStarted)
        MmeStopMidiInput(PrivateHandle, FALSE);
    CallSoundThread(Instance, MidiDrain, NULL);
    return MmeCloseDevice(PrivateHandle);
}
