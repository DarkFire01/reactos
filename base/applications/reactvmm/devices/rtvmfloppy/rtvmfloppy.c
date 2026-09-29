/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The removable drive a machine of this kind installs itself from
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The controller takes a command as a run of bytes written one at a time into
 * one register, does the work, and hands back a run of result bytes through the
 * same register. What makes it worth having is not speed but that every
 * installer written in the last thirty years already knows how to drive it.
 *
 * The data itself never goes through that register. It moves through one of the
 * transfer channels, which the driver programs separately and which the
 * controller here only asks the manager to run.
 */

/* INCLUDES *******************************************************************/

#include <windows.h>
#include <strsafe.h>
#include <stdlib.h>
#include <string.h>

#include "rtvm_device.h"

/* DEFINES ********************************************************************/

#define FLOPPY_BASE             0x03F0

/*
 * Six of them, and then one more past a gap. The register in that gap belongs
 * to the fixed disk controller and always has: the two were wired next to each
 * other and the board gave that one address to the other chip.
 */
#define FLOPPY_LOW_COUNT        6
#define FLOPPY_HIGH_PORT        0x03F7
#define FLOPPY_LINE             6
#define FLOPPY_CHANNEL          2

/* Offsets from the base. The two the board leaves out are not answered for */
#define FLOPPY_STATUS_A         0   /* Read */
#define FLOPPY_STATUS_B         1   /* Read */
#define FLOPPY_DIGITAL_OUTPUT   2   /* Write */
#define FLOPPY_TAPE_DRIVE       3
#define FLOPPY_MAIN_STATUS      4   /* Read */
#define FLOPPY_RATE_SELECT      4   /* Write */
#define FLOPPY_DATA             5
#define FLOPPY_DIGITAL_INPUT    7   /* Read */
#define FLOPPY_CONFIGURATION    7   /* Write */

/* What the main status register says */
#define MAIN_DRIVE_BUSY         0x0F
#define MAIN_COMMAND_BUSY       0x10
#define MAIN_NO_CHANNEL         0x20
#define MAIN_TO_PROCESSOR       0x40
#define MAIN_READY              0x80

/* What the digital output register does */
#define OUTPUT_DRIVE            0x03
#define OUTPUT_NOT_RESET        0x04
#define OUTPUT_CHANNEL_ENABLED  0x08
#define OUTPUT_MOTOR            0xF0

/* The digital input register, of which only the top bit is ever looked at */
#define INPUT_MEDIUM_CHANGED    0x80

/* Commands, with the flags the caller puts in the top three bits taken off */
#define COMMAND_MASK            0x1F
#define COMMAND_READ_TRACK      0x02
#define COMMAND_SPECIFY         0x03
#define COMMAND_SENSE_DRIVE     0x04
#define COMMAND_WRITE_DATA      0x05
#define COMMAND_READ_DATA       0x06
#define COMMAND_RECALIBRATE     0x07
#define COMMAND_SENSE_INTERRUPT 0x08
#define COMMAND_WRITE_DELETED   0x09
#define COMMAND_READ_ID         0x0A
#define COMMAND_READ_DELETED    0x0C
#define COMMAND_FORMAT_TRACK    0x0D
#define COMMAND_DUMP_REGISTERS  0x0E
#define COMMAND_SEEK            0x0F
#define COMMAND_VERSION         0x10
#define COMMAND_SCAN_EQUAL      0x11
#define COMMAND_PERPENDICULAR   0x12
#define COMMAND_CONFIGURE       0x13
#define COMMAND_LOCK            0x14
#define COMMAND_RELATIVE_SEEK   0x1F

/* What the first status byte says about how a command went */
#define ST0_DRIVE               0x03
#define ST0_HEAD                0x04
#define ST0_NOT_READY           0x08
#define ST0_EQUIPMENT_FAULT     0x10
#define ST0_SEEK_ENDED          0x20
#define ST0_ABNORMAL            0x40
#define ST0_INVALID             0x80

#define ST1_MISSING_ADDRESS     0x01
#define ST1_WRITE_PROTECTED     0x02
#define ST1_NO_SUCH_SECTOR      0x04
#define ST1_PAST_CYLINDER       0x80

/* What the controller answers when asked which one it is */
#define FLOPPY_VERSION_82077    0x90

/* The shape of the only medium worth defaulting to */
#define FLOPPY_SECTOR_SIZE      512
#define FLOPPY_DEFAULT_HEADS    2
#define FLOPPY_DEFAULT_SECTORS  18
#define FLOPPY_DEFAULT_TRACKS   80

/* As much as one command may ask for, which is a whole track on both heads */
#define FLOPPY_BUFFER_SIZE      (FLOPPY_SECTOR_SIZE * 36)

#define FLOPPY_COMMAND_MAX      9
#define FLOPPY_RESULT_MAX       7

/* TYPES **********************************************************************/

typedef struct _FLOPPY_DEVICE
{
    RTVM_DEVICE Device;

    /* The medium, and the shape the driver is told it has */
    HANDLE Image;
    CHAR ImagePath[MAX_PATH];
    BOOLEAN ReadOnly;
    ULONG Heads;
    ULONG SectorsPerTrack;
    ULONG Tracks;

    /* What the registers hold */
    UCHAR DigitalOutput;
    UCHAR MainStatus;
    UCHAR Rate;
    UCHAR Configuration[4];
    UCHAR Specify[3];
    BOOLEAN Locked;
    BOOLEAN Changed;

    /* Where each drive's head is, which a seek moves and a sense reports */
    UCHAR Cylinder[4];

    /* The command being taken in, and the answer waiting to be read out */
    UCHAR Command[FLOPPY_COMMAND_MAX];
    ULONG CommandLength;
    ULONG CommandWanted;
    UCHAR Result[FLOPPY_RESULT_MAX];
    ULONG ResultLength;
    ULONG ResultRead;

    /* What the last command left behind for a sense interrupt to report */
    UCHAR LastStatus;
    BOOLEAN Pending;
    BOOLEAN LineAsserted;

    UCHAR Buffer[FLOPPY_BUFFER_SIZE];

    CRITICAL_SECTION Lock;
} FLOPPY_DEVICE, *PFLOPPY_DEVICE;

/* GLOBALS ********************************************************************/

static const RTVM_DEVICE_VTABLE FloppyVtable;

/*
 * How many bytes each command takes after the first, indexed by the command
 * itself. Anything not listed here is one the controller does not have, and is
 * answered as such rather than guessed at.
 */
static const UCHAR FloppyCommandLength[32] =
{
    /* 0x00 */ 0, 0, 8, 2, 1, 8, 8, 1,
    /* 0x08 */ 0, 8, 1, 0, 8, 5, 0, 2,
    /* 0x10 */ 0, 8, 0, 3, 1, 0, 0, 0,
    /* 0x18 */ 0, 0, 0, 0, 0, 0, 0, 2
};

/* FUNCTIONS ******************************************************************/

static
VOID
FloppyLog(
    _In_ PFLOPPY_DEVICE Floppy,
    _In_ RTVM_LOG_LEVEL Level,
    _In_ PCSTR Format,
    ...)
{
    const RTVM_HOST_INTERFACE *Host = Floppy->Device.Host;
    CHAR Line[256];
    va_list Arguments;

    if (Host->Log == NULL)
        return;

    va_start(Arguments, Format);
    StringCchVPrintfA(Line, ARRAYSIZE(Line), Format, Arguments);
    va_end(Arguments);

    Host->Log(Host->Context, Level, "%s", Line);
}

static
VOID
FloppySetLine(
    _Inout_ PFLOPPY_DEVICE Floppy,
    _In_ BOOLEAN Asserted)
{
    const RTVM_HOST_INTERFACE *Host = Floppy->Device.Host;

    /* A driver that turned the line off gets nothing, however ready the drive is */
    if (Asserted && ((Floppy->DigitalOutput & OUTPUT_CHANNEL_ENABLED) == 0))
        Asserted = FALSE;

    if (Asserted == Floppy->LineAsserted)
        return;

    Floppy->LineAsserted = Asserted;

    if (Host->SetInterruptLine != NULL)
        Host->SetInterruptLine(Host->Context, FLOPPY_LINE, Asserted);
}

/* Says a command has finished, which is the only thing the line is ever for */
static
VOID
FloppyRaise(
    _Inout_ PFLOPPY_DEVICE Floppy)
{
    Floppy->Pending = TRUE;
    FloppySetLine(Floppy, TRUE);
}

static
VOID
FloppyAnswer(
    _Inout_ PFLOPPY_DEVICE Floppy,
    _In_reads_bytes_(Length) const UCHAR *Bytes,
    _In_ ULONG Length)
{
    memcpy(Floppy->Result, Bytes, Length);
    Floppy->ResultLength = Length;
    Floppy->ResultRead = 0;

    /* Ready, and pointing the other way, which is how a driver knows to read */
    Floppy->MainStatus = MAIN_READY | MAIN_TO_PROCESSOR | MAIN_COMMAND_BUSY;
}

static
VOID
FloppyIdle(
    _Inout_ PFLOPPY_DEVICE Floppy)
{
    Floppy->CommandLength = 0;
    Floppy->CommandWanted = 0;
    Floppy->ResultLength = 0;
    Floppy->ResultRead = 0;
    Floppy->MainStatus = MAIN_READY;
}

/* Where in the image a sector on this track under this head starts */
static
BOOLEAN
FloppyOffset(
    _In_ PFLOPPY_DEVICE Floppy,
    _In_ ULONG Cylinder,
    _In_ ULONG Head,
    _In_ ULONG Sector,
    _Out_ PLARGE_INTEGER Offset)
{
    ULONG Number;

    Offset->QuadPart = 0;

    /* Sectors are numbered from one on the wire and from zero everywhere else */
    if ((Sector == 0) || (Sector > Floppy->SectorsPerTrack) ||
        (Head >= Floppy->Heads) || (Cylinder >= Floppy->Tracks))
    {
        return FALSE;
    }

    Number = ((Cylinder * Floppy->Heads) + Head) * Floppy->SectorsPerTrack;
    Number += Sector - 1;

    Offset->QuadPart = (LONGLONG)Number * FLOPPY_SECTOR_SIZE;
    return TRUE;
}

/**
 * @brief
 * Carries out a read or a write, with the data moving through the channel.
 *
 * @remarks
 * The count the driver programmed into the channel is what decides how much
 * moves, not the sector numbers in the command. That is how the real thing
 * works, and it is why a driver may ask for a whole track and be given only
 * the part it made room for.
 */
static
VOID
FloppyMoveData(
    _Inout_ PFLOPPY_DEVICE Floppy,
    _In_ BOOLEAN Reading)
{
    const RTVM_HOST_INTERFACE *Host = Floppy->Device.Host;
    const UCHAR Drive = (UCHAR)(Floppy->Command[1] & 0x03);
    const ULONG Head = (Floppy->Command[1] >> 2) & 0x01;
    const ULONG Cylinder = Floppy->Command[2];
    const ULONG Sector = Floppy->Command[4];
    const ULONG Last = Floppy->Command[6];
    UCHAR Status[FLOPPY_RESULT_MAX];
    LARGE_INTEGER Offset;
    ULONG Wanted;
    ULONG Moved = 0;
    ULONG Direction = RTVM_CHANNEL_IDLE;
    ULONG64 Where = 0;
    DWORD Carried = 0;

    Status[0] = (UCHAR)(Drive | (Head << 2));
    Status[1] = 0;
    Status[2] = 0;
    Status[3] = (UCHAR)Cylinder;
    Status[4] = (UCHAR)Head;
    Status[5] = (UCHAR)Sector;
    Status[6] = Floppy->Command[5];

    if ((Floppy->Image == INVALID_HANDLE_VALUE) || (Drive != 0))
    {
        Status[0] |= ST0_ABNORMAL | ST0_NOT_READY;
        Status[1] |= ST1_MISSING_ADDRESS;
        FloppyAnswer(Floppy, Status, FLOPPY_RESULT_MAX);
        FloppyRaise(Floppy);
        return;
    }

    if (!Reading && Floppy->ReadOnly)
    {
        Status[0] |= ST0_ABNORMAL;
        Status[1] |= ST1_WRITE_PROTECTED;
        FloppyAnswer(Floppy, Status, FLOPPY_RESULT_MAX);
        FloppyRaise(Floppy);
        return;
    }

    if (!FloppyOffset(Floppy, Cylinder, Head, Sector, &Offset))
    {
        FloppyLog(Floppy, RtvmLogWarning,
                  "floppy: no sector %lu on %lu/%lu\n", Sector, Cylinder, Head);
        Status[0] |= ST0_ABNORMAL;
        Status[1] |= ST1_NO_SUCH_SECTOR;
        FloppyAnswer(Floppy, Status, FLOPPY_RESULT_MAX);
        FloppyRaise(Floppy);
        return;
    }

    /* As far as the end of the track, which is as much as one command covers */
    Wanted = (Last >= Sector) ? ((Last - Sector) + 1) : 1;

    if (Wanted > (FLOPPY_BUFFER_SIZE / FLOPPY_SECTOR_SIZE))
        Wanted = FLOPPY_BUFFER_SIZE / FLOPPY_SECTOR_SIZE;

    Wanted *= FLOPPY_SECTOR_SIZE;

    /*
     * The controller says where in memory the transfer goes and how much of it
     * may go. Carrying it is this device's own work, because the channel has
     * no way of reaching memory and this does.
     */
    if (Host->RequestChannel(Host->Context, FLOPPY_CHANNEL, Wanted,
                             &Direction, &Where, &Moved) != RtvmOk)
    {
        Moved = 0;
    }

    if ((Moved != 0) && Reading && (Direction == RTVM_CHANNEL_TO_MEMORY))
    {
        SetFilePointerEx(Floppy->Image, Offset, NULL, FILE_BEGIN);

        if (!ReadFile(Floppy->Image, Floppy->Buffer, Moved, &Carried, NULL))
            Carried = 0;

        /* Past the end of a short image reads as an empty sector would */
        if (Carried < Moved)
            memset(&Floppy->Buffer[Carried], 0, Moved - Carried);

        if (Host->WriteGuestMemory(Host->Context, Where,
                                   Floppy->Buffer, Moved) != RtvmOk)
        {
            Moved = 0;
        }
    }
    else if ((Moved != 0) && !Reading && (Direction == RTVM_CHANNEL_FROM_MEMORY))
    {
        if (Host->ReadGuestMemory(Host->Context, Where,
                                  Floppy->Buffer, Moved) == RtvmOk)
        {
            SetFilePointerEx(Floppy->Image, Offset, NULL, FILE_BEGIN);

            if (!WriteFile(Floppy->Image, Floppy->Buffer, Moved, &Carried, NULL))
                Carried = 0;
        }
        else
        {
            Moved = 0;
        }
    }
    else
    {
        /* The channel is pointing the other way from what was asked for */
        Moved = 0;
    }

    if (Moved != 0)
        Host->ChannelFinished(Host->Context, FLOPPY_CHANNEL);

    if (Moved == 0)
    {
        FloppyLog(Floppy, RtvmLogWarning,
                  "floppy: the channel would not carry %lu byte(s)\n", Wanted);
        Status[0] |= ST0_ABNORMAL;
        Status[1] |= ST1_MISSING_ADDRESS;
    }
    else
    {
        /*
         * Where the controller stopped, which is one past what it moved and is
         * what a driver reading a run of sectors carries on from.
         */
        const ULONG Done = Moved / FLOPPY_SECTOR_SIZE;

        Status[5] = (UCHAR)(Sector + Done);

        if (Status[5] > Floppy->SectorsPerTrack)
        {
            Status[5] = 1;
            Status[3] = (UCHAR)(Cylinder + 1);
        }
    }

    FloppyAnswer(Floppy, Status, FLOPPY_RESULT_MAX);
    FloppyRaise(Floppy);
}

/* Does whatever has just been written in full */
static
VOID
FloppyRun(
    _Inout_ PFLOPPY_DEVICE Floppy)
{
    const UCHAR What = (UCHAR)(Floppy->Command[0] & COMMAND_MASK);
    UCHAR Answer[FLOPPY_RESULT_MAX];
    UCHAR Drive;

    switch (What)
    {
        case COMMAND_SPECIFY:
            memcpy(Floppy->Specify, &Floppy->Command[1], 2);
            FloppyIdle(Floppy);
            break;

        case COMMAND_SENSE_DRIVE:
            Drive = (UCHAR)(Floppy->Command[1] & 0x03);

            /* Ready, at track zero if it is, and write protected if it is */
            Answer[0] = (UCHAR)(Drive | 0x20);

            if (Floppy->Cylinder[Drive] == 0)
                Answer[0] |= 0x10;

            if (Floppy->ReadOnly)
                Answer[0] |= 0x40;

            if ((Floppy->Command[1] & 0x04) != 0)
                Answer[0] |= 0x04;

            FloppyAnswer(Floppy, Answer, 1);
            break;

        case COMMAND_RECALIBRATE:
            Drive = (UCHAR)(Floppy->Command[1] & 0x03);
            Floppy->Cylinder[Drive] = 0;
            Floppy->LastStatus = (UCHAR)(Drive | ST0_SEEK_ENDED);
            FloppyIdle(Floppy);
            FloppyRaise(Floppy);
            break;

        case COMMAND_SEEK:
            Drive = (UCHAR)(Floppy->Command[1] & 0x03);
            Floppy->Cylinder[Drive] = Floppy->Command[2];
            Floppy->LastStatus = (UCHAR)(Drive | ST0_SEEK_ENDED |
                                         ((Floppy->Command[1] & 0x04) << 0));
            FloppyIdle(Floppy);
            FloppyRaise(Floppy);
            break;

        case COMMAND_SENSE_INTERRUPT:
            if (!Floppy->Pending)
            {
                /* Nothing happened, and saying so is what stops a driver looping */
                Answer[0] = ST0_INVALID;
                FloppyAnswer(Floppy, Answer, 1);
                break;
            }

            Answer[0] = Floppy->LastStatus;
            Answer[1] = Floppy->Cylinder[Floppy->LastStatus & ST0_DRIVE];
            Floppy->Pending = FALSE;
            FloppySetLine(Floppy, FALSE);
            FloppyAnswer(Floppy, Answer, 2);
            break;

        case COMMAND_READ_ID:
            Drive = (UCHAR)(Floppy->Command[1] & 0x03);
            Answer[0] = (UCHAR)(Drive | ((Floppy->Command[1] & 0x04)));
            Answer[1] = 0;
            Answer[2] = 0;
            Answer[3] = Floppy->Cylinder[Drive];
            Answer[4] = (UCHAR)((Floppy->Command[1] >> 2) & 0x01);
            Answer[5] = 1;
            Answer[6] = 2;
            Floppy->LastStatus = Answer[0];
            FloppyAnswer(Floppy, Answer, FLOPPY_RESULT_MAX);
            FloppyRaise(Floppy);
            break;

        case COMMAND_READ_DATA:
        case COMMAND_READ_TRACK:
        case COMMAND_READ_DELETED:
            Floppy->LastStatus = (UCHAR)(Floppy->Command[1] & 0x07);
            FloppyMoveData(Floppy, TRUE);
            break;

        case COMMAND_WRITE_DATA:
        case COMMAND_WRITE_DELETED:
            Floppy->LastStatus = (UCHAR)(Floppy->Command[1] & 0x07);
            FloppyMoveData(Floppy, FALSE);
            break;

        case COMMAND_FORMAT_TRACK:
            /*
             * Taken and nothing written. A medium here is a file that already
             * has every sector in it, so laying the track out again would only
             * be a way of losing what is on it.
             */
            Drive = (UCHAR)(Floppy->Command[1] & 0x03);
            Answer[0] = Drive;
            Answer[1] = Floppy->ReadOnly ? ST1_WRITE_PROTECTED : 0;
            Answer[2] = 0;
            Answer[3] = Floppy->Cylinder[Drive];
            Answer[4] = (UCHAR)((Floppy->Command[1] >> 2) & 0x01);
            Answer[5] = 1;
            Answer[6] = 2;
            Floppy->LastStatus = Answer[0];
            FloppyAnswer(Floppy, Answer, FLOPPY_RESULT_MAX);
            FloppyRaise(Floppy);
            break;

        case COMMAND_VERSION:
            Answer[0] = FLOPPY_VERSION_82077;
            FloppyAnswer(Floppy, Answer, 1);
            break;

        case COMMAND_CONFIGURE:
            memcpy(Floppy->Configuration, &Floppy->Command[1], 3);
            FloppyIdle(Floppy);
            break;

        case COMMAND_PERPENDICULAR:
            FloppyIdle(Floppy);
            break;

        case COMMAND_LOCK:
            Floppy->Locked = ((Floppy->Command[0] & 0x80) != 0);
            Answer[0] = (UCHAR)(Floppy->Locked ? 0x10 : 0x00);
            FloppyAnswer(Floppy, Answer, 1);
            break;

        case COMMAND_DUMP_REGISTERS:
            Answer[0] = Floppy->Cylinder[0];
            Answer[1] = Floppy->Cylinder[1];
            Answer[2] = Floppy->Cylinder[2];
            Answer[3] = Floppy->Cylinder[3];
            Answer[4] = Floppy->Specify[0];
            Answer[5] = Floppy->Specify[1];
            Answer[6] = (UCHAR)(Floppy->SectorsPerTrack);
            FloppyAnswer(Floppy, Answer, FLOPPY_RESULT_MAX);
            break;

        default:
            FloppyLog(Floppy, RtvmLogWarning,
                      "floppy: %02x is not a command this has\n", What);
            Answer[0] = ST0_INVALID;
            FloppyAnswer(Floppy, Answer, 1);
            break;
    }
}

/* THE REGISTERS **************************************************************/

static
RTVM_STATUS
RTVMAPI
FloppyIoRead(
    _In_ PRTVM_DEVICE Device,
    _In_ USHORT Port,
    _In_ ULONG Width,
    _Out_ PULONG Value)
{
    PFLOPPY_DEVICE Floppy = (PFLOPPY_DEVICE)Device->DeviceContext;
    const ULONG Register = (ULONG)(Port - FLOPPY_BASE);

    UNREFERENCED_PARAMETER(Width);

    EnterCriticalSection(&Floppy->Lock);

    switch (Register)
    {
        case FLOPPY_MAIN_STATUS:
            *Value = Floppy->MainStatus;
            break;

        case FLOPPY_DATA:
            if (Floppy->ResultRead < Floppy->ResultLength)
            {
                *Value = Floppy->Result[Floppy->ResultRead];
                Floppy->ResultRead++;

                /*
                 * The last byte taken is what ends the command, and what lets
                 * the line go. A command that answers with bytes has already
                 * said everything it has to say by the time they have been
                 * read, and one that kept holding its line would be asking for
                 * an interrupt nobody has anything left to do about.
                 */
                if (Floppy->ResultRead >= Floppy->ResultLength)
                {
                    FloppyIdle(Floppy);
                    Floppy->Pending = FALSE;
                    FloppySetLine(Floppy, FALSE);
                }
            }
            else
            {
                *Value = 0xFF;
            }

            break;

        case FLOPPY_DIGITAL_INPUT:
            *Value = Floppy->Changed ? INPUT_MEDIUM_CHANGED : 0x00;
            break;

        case FLOPPY_STATUS_A:
        case FLOPPY_STATUS_B:
            /* The two the board added, which nothing that boots looks at */
            *Value = 0xFF;
            break;

        default:
            *Value = 0xFF;
            break;
    }

    LeaveCriticalSection(&Floppy->Lock);
    return RtvmOk;
}

static
RTVM_STATUS
RTVMAPI
FloppyIoWrite(
    _In_ PRTVM_DEVICE Device,
    _In_ USHORT Port,
    _In_ ULONG Width,
    _In_ ULONG Value)
{
    PFLOPPY_DEVICE Floppy = (PFLOPPY_DEVICE)Device->DeviceContext;
    const ULONG Register = (ULONG)(Port - FLOPPY_BASE);
    const UCHAR Byte = (UCHAR)Value;

    UNREFERENCED_PARAMETER(Width);

    EnterCriticalSection(&Floppy->Lock);

    switch (Register)
    {
        case FLOPPY_DIGITAL_OUTPUT:
        {
            const UCHAR Was = Floppy->DigitalOutput;

            Floppy->DigitalOutput = Byte;

            /* Coming out of reset is what a driver waits for an interrupt on */
            if (((Was & OUTPUT_NOT_RESET) == 0) && ((Byte & OUTPUT_NOT_RESET) != 0))
            {
                FloppyIdle(Floppy);
                Floppy->LastStatus = ST0_SEEK_ENDED | 0xC0;
                FloppyRaise(Floppy);
            }
            else if ((Byte & OUTPUT_NOT_RESET) == 0)
            {
                FloppyIdle(Floppy);
                FloppySetLine(Floppy, FALSE);
                Floppy->Pending = FALSE;
            }

            break;
        }

        case FLOPPY_RATE_SELECT:
            Floppy->Rate = (UCHAR)(Byte & 0x03);
            break;

        case FLOPPY_CONFIGURATION:
            Floppy->Rate = (UCHAR)(Byte & 0x03);
            break;

        case FLOPPY_DATA:
            /* A byte written while an answer is waiting is one nothing asked for */
            if (Floppy->ResultLength != 0)
                break;

            if (Floppy->CommandWanted == 0)
            {
                const UCHAR What = (UCHAR)(Byte & COMMAND_MASK);

                Floppy->Command[0] = Byte;
                Floppy->CommandLength = 1;
                Floppy->CommandWanted = FloppyCommandLength[What] + 1u;
                Floppy->MainStatus = MAIN_READY | MAIN_COMMAND_BUSY;
            }
            else if (Floppy->CommandLength < FLOPPY_COMMAND_MAX)
            {
                Floppy->Command[Floppy->CommandLength] = Byte;
                Floppy->CommandLength++;
            }

            if (Floppy->CommandLength >= Floppy->CommandWanted)
            {
                Floppy->CommandWanted = 0;
                FloppyRun(Floppy);
            }

            break;
    }

    LeaveCriticalSection(&Floppy->Lock);
    return RtvmOk;
}

/* THE DEVICE *****************************************************************/

static
VOID
RTVMAPI
FloppyReset(
    _In_ PRTVM_DEVICE Device)
{
    PFLOPPY_DEVICE Floppy = (PFLOPPY_DEVICE)Device->DeviceContext;

    EnterCriticalSection(&Floppy->Lock);

    FloppySetLine(Floppy, FALSE);
    FloppyIdle(Floppy);

    Floppy->DigitalOutput = 0;
    Floppy->Pending = FALSE;
    Floppy->Locked = FALSE;
    Floppy->LastStatus = 0;
    memset(Floppy->Cylinder, 0, sizeof(Floppy->Cylinder));

    /* A drive comes up having noticed that something was put in it */
    Floppy->Changed = TRUE;

    LeaveCriticalSection(&Floppy->Lock);
}

static
RTVM_STATUS
RTVMAPI
FloppyStart(
    _In_ PRTVM_DEVICE Device)
{
    PFLOPPY_DEVICE Floppy = (PFLOPPY_DEVICE)Device->DeviceContext;
    const RTVM_HOST_INTERFACE *Host = Device->Host;
    RTVM_STATUS Status;

    if (!RTVM_CARRIES(Host, RTVM_HOST_INTERFACE, RequestChannel))
    {
        FloppyLog(Floppy, RtvmLogError,
                  "floppy: the manager has no transfer channels to ask for\n");
        return RtvmNotSupported;
    }

    Status = Host->ClaimPortRange(Host->Context, Device,
                                  FLOPPY_BASE, FLOPPY_LOW_COUNT);
    if (Status != RtvmOk)
    {
        FloppyLog(Floppy, RtvmLogError,
                  "floppy: %04x is answered for already\n", FLOPPY_BASE);
        return Status;
    }

    Status = Host->ClaimPortRange(Host->Context, Device, FLOPPY_HIGH_PORT, 1);
    if (Status != RtvmOk)
    {
        FloppyLog(Floppy, RtvmLogError,
                  "floppy: %04x is answered for already\n", FLOPPY_HIGH_PORT);
        return Status;
    }

    FloppyReset(Device);

    FloppyLog(Floppy, RtvmLogInfo,
              "floppy: %lu track(s) of %lu sector(s) on %lu head(s)%s\n",
              Floppy->Tracks, Floppy->SectorsPerTrack, Floppy->Heads,
              Floppy->ReadOnly ? ", read only" : "");

    return RtvmOk;
}

static
VOID
RTVMAPI
FloppyStop(
    _In_ PRTVM_DEVICE Device)
{
    PFLOPPY_DEVICE Floppy = (PFLOPPY_DEVICE)Device->DeviceContext;

    EnterCriticalSection(&Floppy->Lock);
    FloppySetLine(Floppy, FALSE);
    LeaveCriticalSection(&Floppy->Lock);
}

static
VOID
RTVMAPI
FloppyDestroy(
    _In_ PRTVM_DEVICE Device)
{
    PFLOPPY_DEVICE Floppy = (PFLOPPY_DEVICE)Device->DeviceContext;

    if (Floppy->Image != INVALID_HANDLE_VALUE)
        CloseHandle(Floppy->Image);

    DeleteCriticalSection(&Floppy->Lock);
    free(Floppy);
}

static const RTVM_DEVICE_VTABLE FloppyVtable =
{
    sizeof(FloppyVtable),
    FloppyStart,
    FloppyStop,
    FloppyReset,
    FloppyDestroy,
    FloppyIoRead,
    FloppyIoWrite,
    NULL,
    NULL,
    NULL,
    NULL
};

/* MAKING ONE *****************************************************************/

static
PCSTR
FloppySetting(
    _In_opt_ PCSTR Parameters,
    _In_ PCSTR Name)
{
    SIZE_T Length = strlen(Name);
    PCSTR Walk = Parameters;

    while ((Walk != NULL) && (*Walk != '\0'))
    {
        if ((_strnicmp(Walk, Name, Length) == 0) &&
            ((Walk[Length] == '=') || (Walk[Length] == ',') || (Walk[Length] == '\0')))
        {
            return (Walk[Length] == '=') ? &Walk[Length + 1] : &Walk[Length];
        }

        Walk = strchr(Walk, ',');
        if (Walk != NULL)
            Walk++;
    }

    return NULL;
}

static
VOID
FloppyCopySetting(
    _In_ PCSTR Value,
    _Out_writes_z_(Size) PSTR Buffer,
    _In_ SIZE_T Size)
{
    SIZE_T Index = 0;

    while ((Index + 1 < Size) && (Value[Index] != '\0') && (Value[Index] != ','))
    {
        Buffer[Index] = Value[Index];
        Index++;
    }

    Buffer[Index] = '\0';
}

/*
 * How big a medium of each shape is, so that one can be recognised by the size
 * of its file the way a drive recognises it by reading it.
 */
static const struct
{
    ULONG Sectors;
    ULONG Heads;
    ULONG SectorsPerTrack;
    ULONG Tracks;
} FloppyShapes[] =
{
    {  720, 2,  9, 40 },    /* 360 KB */
    { 1440, 2,  9, 80 },    /* 720 KB */
    { 2400, 2, 15, 80 },    /* 1.2 MB */
    { 2880, 2, 18, 80 },    /* 1.44 MB */
    { 5760, 2, 36, 80 }     /* 2.88 MB */
};

static
VOID
FloppyTakeShape(
    _Inout_ PFLOPPY_DEVICE Floppy,
    _In_ ULONGLONG Size)
{
    const ULONG Sectors = (ULONG)(Size / FLOPPY_SECTOR_SIZE);
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(FloppyShapes); Index++)
    {
        if (FloppyShapes[Index].Sectors == Sectors)
        {
            Floppy->Heads = FloppyShapes[Index].Heads;
            Floppy->SectorsPerTrack = FloppyShapes[Index].SectorsPerTrack;
            Floppy->Tracks = FloppyShapes[Index].Tracks;
            return;
        }
    }

    /* Nothing it recognises, so the commonest shape and the size it really is */
    Floppy->Heads = FLOPPY_DEFAULT_HEADS;
    Floppy->SectorsPerTrack = FLOPPY_DEFAULT_SECTORS;
    Floppy->Tracks = (Sectors + (FLOPPY_DEFAULT_HEADS * FLOPPY_DEFAULT_SECTORS) - 1) /
                     (FLOPPY_DEFAULT_HEADS * FLOPPY_DEFAULT_SECTORS);

    if (Floppy->Tracks == 0)
        Floppy->Tracks = FLOPPY_DEFAULT_TRACKS;
}

static
RTVM_STATUS
RTVMAPI
FloppyCreate(
    _In_ const RTVM_HOST_INTERFACE *Host,
    _In_opt_ PCSTR Parameters,
    _Outptr_ PRTVM_DEVICE *Device)
{
    PFLOPPY_DEVICE Floppy;
    LARGE_INTEGER Size;
    PCSTR Value;

    Floppy = (PFLOPPY_DEVICE)calloc(1, sizeof(*Floppy));
    if (Floppy == NULL)
        return RtvmNoMemory;

    Floppy->Device.Size = sizeof(Floppy->Device);
    Floppy->Device.Vtable = &FloppyVtable;
    Floppy->Device.Host = Host;
    Floppy->Device.DeviceContext = Floppy;
    Floppy->Image = INVALID_HANDLE_VALUE;
    Floppy->Heads = FLOPPY_DEFAULT_HEADS;
    Floppy->SectorsPerTrack = FLOPPY_DEFAULT_SECTORS;
    Floppy->Tracks = FLOPPY_DEFAULT_TRACKS;

    StringCchCopyA(Floppy->Device.Name, sizeof(Floppy->Device.Name), "floppy");

    Floppy->ReadOnly = (FloppySetting(Parameters, "readonly") != NULL);

    Value = FloppySetting(Parameters, "image");
    if ((Value == NULL) || (*Value == '\0'))
    {
        Host->Log(Host->Context, RtvmLogError,
                  "a floppy needs image=<path> to have anything in it\n");
        free(Floppy);
        return RtvmBadParameter;
    }

    FloppyCopySetting(Value, Floppy->ImagePath, sizeof(Floppy->ImagePath));

    Floppy->Image = CreateFileA(Floppy->ImagePath,
                                Floppy->ReadOnly
                                    ? GENERIC_READ
                                    : (GENERIC_READ | GENERIC_WRITE),
                                FILE_SHARE_READ,
                                NULL,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                NULL);

    /* One that will not open for writing is still worth having read only */
    if ((Floppy->Image == INVALID_HANDLE_VALUE) && !Floppy->ReadOnly)
    {
        Floppy->Image = CreateFileA(Floppy->ImagePath,
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    NULL,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    NULL);
        if (Floppy->Image != INVALID_HANDLE_VALUE)
            Floppy->ReadOnly = TRUE;
    }

    if (Floppy->Image == INVALID_HANDLE_VALUE)
    {
        Host->Log(Host->Context, RtvmLogError, "%s would not open, error %lu\n",
                  Floppy->ImagePath, GetLastError());
        free(Floppy);
        return RtvmNotFound;
    }

    if (GetFileSizeEx(Floppy->Image, &Size))
        FloppyTakeShape(Floppy, (ULONGLONG)Size.QuadPart);

    /* Whatever was worked out, the command line has the last word */
    Value = FloppySetting(Parameters, "heads");
    if ((Value != NULL) && (*Value != '\0'))
        Floppy->Heads = strtoul(Value, NULL, 0);

    Value = FloppySetting(Parameters, "sectors");
    if ((Value != NULL) && (*Value != '\0'))
        Floppy->SectorsPerTrack = strtoul(Value, NULL, 0);

    Value = FloppySetting(Parameters, "tracks");
    if ((Value != NULL) && (*Value != '\0'))
        Floppy->Tracks = strtoul(Value, NULL, 0);

    if ((Floppy->Heads == 0) || (Floppy->SectorsPerTrack == 0) ||
        (Floppy->Tracks == 0))
    {
        Host->Log(Host->Context, RtvmLogError,
                  "a drive of %lu by %lu by %lu holds nothing\n",
                  Floppy->Tracks, Floppy->Heads, Floppy->SectorsPerTrack);
        CloseHandle(Floppy->Image);
        free(Floppy);
        return RtvmBadParameter;
    }

    InitializeCriticalSection(&Floppy->Lock);

    *Device = &Floppy->Device;
    return RtvmOk;
}

static const RTVM_DEVICE_CLASS FloppyClasses[] =
{
    { "floppy", "A removable drive on the usual controller", FloppyCreate }
};

static const RTVM_DEVICE_MODULE FloppyModule =
{
    sizeof(FloppyModule),
    RTVM_DEVICE_ABI_VERSION,
    "rtvmfloppy",
    "The removable drive and its controller",
    ARRAYSIZE(FloppyClasses),
    FloppyClasses
};

RTVM_DEVICE_EXPORT
const RTVM_DEVICE_MODULE *
RTVMAPI
RtvmDeviceModuleEntry(
    _In_ ULONG HostAbiVersion)
{
    if (HostAbiVersion != RTVM_DEVICE_ABI_VERSION)
        return NULL;

    return &FloppyModule;
}

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE Instance,
    _In_ DWORD Reason,
    _In_ LPVOID Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);

    if (Reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(Instance);

    return TRUE;
}
