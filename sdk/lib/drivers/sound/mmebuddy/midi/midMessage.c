/*
 * PROJECT:     ReactOS Sound System "MME Buddy" Library
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        lib/sound/mmebuddy/midi/midMessage.c
 *
 * PURPOSE:     Provides the midMessage exported function, as required by
 *              the MME API, for MIDI input device support.
 *
 * PROGRAMMERS: Andrew Greenwood (silverblade@reactos.org)
*/

#include "precomp.h"

/*
    Standard MME driver entry-point for messages relating to MIDI input.
*/
DWORD
APIENTRY
midMessage(
    UINT DeviceId,
    UINT Message,
    DWORD_PTR PrivateHandle,
    DWORD_PTR Parameter1,
    DWORD_PTR Parameter2)
{
    MMRESULT Result = MMSYSERR_NOTSUPPORTED;

    AcquireEntrypointMutex(MIDI_IN_DEVICE_TYPE);

    SND_TRACE(L"midMessage - Message type %d\n", Message);

    switch ( Message )
    {
        case MIDM_GETNUMDEVS :
        {
            Result = GetSoundDeviceCount(MIDI_IN_DEVICE_TYPE);
            break;
        }

        case MIDM_GETDEVCAPS :
        {
            Result = MmeGetSoundDeviceCapabilities(MIDI_IN_DEVICE_TYPE,
                                                   DeviceId,
                                                   (PVOID) Parameter1,
                                                   Parameter2);
            break;
        }

        case DRV_QUERYDEVICEINTERFACESIZE :
        {
            Result = MmeGetDeviceInterfaceString(MIDI_IN_DEVICE_TYPE, DeviceId, NULL, 0, (DWORD*)Parameter1); //FIXME DWORD_PTR
            break;
        }

        case DRV_QUERYDEVICEINTERFACE :
        {
            Result = MmeGetDeviceInterfaceString(MIDI_IN_DEVICE_TYPE, DeviceId, (LPWSTR)Parameter1, Parameter2, NULL); //FIXME DWORD_PTR
            break;
        }

        case MIDM_OPEN :
        {
            Result = MmeOpenDevice(MIDI_IN_DEVICE_TYPE,
                                   DeviceId,
                                   (LPWAVEOPENDESC) Parameter1,
                                   Parameter2,
                                   (DWORD_PTR*) PrivateHandle);
            break;
        }

        case MIDM_CLOSE :
        {
            Result = MmeCloseMidiDevice(PrivateHandle);
            break;
        }

       case MIDM_START :
        {
            Result = MmeStartMidiInput(PrivateHandle);
            break;
        }

        case MIDM_STOP :
        {
            Result = MmeStopMidiInput(PrivateHandle, FALSE);
            break;
        }

        case MIDM_RESET :
        {
            Result = MmeStopMidiInput(PrivateHandle, TRUE);
            break;
        }

        case MIDM_PREPARE :
        {
            Result = MmePrepareMidiHeader(PrivateHandle, Parameter1);
            break;
        }

        case MIDM_UNPREPARE :
        {
            Result = MmeUnprepareMidiHeader(PrivateHandle, Parameter1);
            break;
        }

        case MIDM_ADDBUFFER :
        {
            Result = MmeAddMidiInBuffer(PrivateHandle, Parameter1);
            break;
        }
    }

    SND_TRACE(L"midMessage returning MMRESULT %d\n", Result);

    ReleaseEntrypointMutex(MIDI_IN_DEVICE_TYPE);

    return Result;
}
