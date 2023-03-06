/*
 * PROJECT:     ReactOS GenericFramebuffer graphics card driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     GenericFramebuffer primary header
 * COPYRIGHT:   Copyright 2022 Hervé Poussineau <hpoussin@reactos.org>
 */

#ifndef GenericFramebuffer_H
#define GenericFramebuffer_H

#include <ntdef.h>
#include <dderror.h>
#include <miniport.h>
#include <video.h>
#include <devioctl.h>
#include <section_attribs.h>

typedef struct
{
    USHORT XResolution;
    USHORT YResolution;
} GENERICFRAMEBUFFER_SIZE, *PGENERICFRAMEBUFFER_SIZE;

typedef struct
{
    PUCHAR Mapped;
    PHYSICAL_ADDRESS RangeStart;
    ULONG RangeLength;
    UCHAR RangeInIoSpace;
} GENERICFRAMEBUFFER_ADDRESS_RANGE;

typedef struct
{
    GENERICFRAMEBUFFER_SIZE AvailableModeInfo;
    ULONG AvailableModeCount;
    USHORT CurrentMode;

    GENERICFRAMEBUFFER_ADDRESS_RANGE FrameBuffer;
    GENERICFRAMEBUFFER_ADDRESS_RANGE IoPorts;

    ULONG MaxXResolution;
    ULONG MaxYResolution;
} GENERICFRAMEBUFFER_DEVICE_EXTENSION, *PGENERICFRAMEBUFFER_DEVICE_EXTENSION;


#endif //GenericFramebuffer_H