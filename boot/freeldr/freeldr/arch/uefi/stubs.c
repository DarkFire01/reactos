/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI stubs
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#include <debug.h>

/* TODO: Handle this with custom Disk / partition setup */
UCHAR
DriveMapGetBiosDriveNumber(PCSTR DeviceName)
{
    return 0;
}

VOID
StallExecutionProcessor(ULONG Microseconds)
{

}

VOID
NTAPI
KeStallExecutionProcessor(ULONG Microseconds)
{
    StallExecutionProcessor(Microseconds);
}

USHORT
__cdecl PxeCallApi(USHORT Segment, USHORT Offset, USHORT Service, VOID* Parameter)
{
    return 0;
}
