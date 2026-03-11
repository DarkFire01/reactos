/*
* PROJECT:     ReactOS Universal Video Class Driver
* LICENSE:     GPL - See COPYING in the top level directory
* FILE:        drivers/usb/usbvideo/uvc.c
* PURPOSE:     USB Video device driver.
* PROGRAMMERS:
*              Johannes Anderwald (johannes.anderwald@reactos.org)
*/
#define NDEBUG
#include "usbvideo.h"

static const JFIF_APP0 g_JfifApp0 = {
    .marker      = { 0xFF, 0xE0 },
    .length      = 0x1000,
    .identifier  = { 'J','F','I','F', 0x00 },
    .versionMajor= 0x01,
    .versionMinor= 0x01,
    .pixelAspect = 0x00,
    .xDensity    = 0x0100,
    .yDensity    = 0x0100,
    .xThumbnail  = 0x00,
    .yThumbnail  = 0x00,
};

NTSTATUS
UvcPatchAvi1ToJfif(
    PUCHAR  pFrame,
    ULONG   frameSize,
    PUCHAR  pOut,
    PULONG  pOutSize)
{
    if (frameSize < 4)
    {
        ASSERT(FALSE);
        return STATUS_INVALID_PARAMETER;
    }

    if (pFrame[0] != 0xFF || pFrame[1] != 0xD8)
    {
        DPRINT("invalid soi\n");
        return STATUS_INVALID_PARAMETER;
    }

    if (pFrame[2] != 0xFF || pFrame[3] != 0xE0)
    {
        DPRINT("no app maker\n");
        return STATUS_INVALID_PARAMETER;
    }

    USHORT app0Len = (pFrame[4] << 8) | pFrame[5];
    if (frameSize < (ULONG)(2 + 2 + app0Len))
    {
        DPRINT("unexpected app0len\n");
        return STATUS_INVALID_PARAMETER;
    }

    BOOLEAN isAvi1 = (pFrame[6]  == 'A' &&
                      pFrame[7]  == 'V' &&
                      pFrame[8]  == 'I' &&
                      pFrame[9]  == '1' &&
                      pFrame[10] == 0x00);

    BOOLEAN isJfif = (pFrame[6]  == 'J' &&
                      pFrame[7]  == 'F' &&
                      pFrame[8]  == 'I' &&
                      pFrame[9]  == 'F' &&
                      pFrame[10] == 0x00);

    if (isJfif) {
        /* already jfif format */
        RtlCopyMemory(pOut, pFrame, frameSize);
        *pOutSize = frameSize;
        return STATUS_SUCCESS;
    }

    if (!isAvi1) {
        /* unknown format */
        ASSERT(FALSE);
        RtlCopyMemory(pOut, pFrame, frameSize);
        *pOutSize = frameSize;
        return STATUS_SUCCESS;
    }

    ASSERT(app0Len);
    ULONG oldApp0Size = 2 + app0Len;
    C_ASSERT(sizeof(JFIF_APP0) == 18);
    ULONG restOffset  = 2 + oldApp0Size;

    ASSERT(frameSize > restOffset);
    ULONG restSize    = frameSize - restOffset;

    ULONG outSize = 2 + sizeof(JFIF_APP0) + restSize + 2;

    // copy SOI
    pOut[0] = 0xFF;
    pOut[1] = 0xD8;

    // copy APP0
    RtlCopyMemory(pOut + 2, &g_JfifApp0, sizeof(JFIF_APP0));
    DPRINT("Marker 1 %x Marker2 %x\n", pFrame[restOffset], pFrame[restOffset+1]);
    ASSERT(pFrame[restOffset] == 0xFF);
    ASSERT(pFrame[restOffset+1] == 0xDB);

    // copy rest of frame
    RtlCopyMemory(pOut + 2 + sizeof(JFIF_APP0),
                  pFrame + restOffset,
                  restSize);
    // mark header as done
    pOut[2 + sizeof(JFIF_APP0) + restSize + 1] = 0xFF;
    pOut[2 + sizeof(JFIF_APP0) + restSize + 2] = 0xD9;

    *pOutSize = outSize;
    return STATUS_SUCCESS;
}

VOID
NTAPI
USBVideoDeliverFrame(
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension,
    PUCHAR FrameBuffer,
    ULONG FrameSize)
{
    PKSSTREAM_POINTER StreamPointer;
    PKSPIN Pin;
    NTSTATUS Status;

    if (!DeviceExtension || !DeviceExtension->Pin)
    {
        DPRINT("USBVideoDeliverFrame: Invalid device extension or pin\n");
        return;
    }

    Pin = DeviceExtension->Pin;

    /* Get leading edge stream pointer - this is the user's buffer from IOCTL_KS_READ_STREAM */
    StreamPointer = KsPinGetLeadingEdgeStreamPointer(Pin, KSSTREAM_POINTER_STATE_LOCKED);
    if (!StreamPointer)
    {
        DPRINT("USBVideoDeliverFrame: No stream pointer available\n");
        return;
    }

    if (!StreamPointer->StreamHeader || !StreamPointer->StreamHeader->Data)
    {
        DPRINT("USBVideoDeliverFrame: No stream header or data pointer\n");
        KsStreamPointerUnlock(StreamPointer, FALSE);
        return;
    }

    /* Copy frame data to user buffer */
    ULONG BytesToCopy = min(StreamPointer->Offset->Remaining, FrameSize);
    if (BytesToCopy > 0)
    {
        DPRINT("USBVideoDeliverFrame: Copying %u bytes to user buffer (frame size %u)\n", BytesToCopy, FrameSize);
        Status = UvcPatchAvi1ToJfif(FrameBuffer, BytesToCopy, StreamPointer->StreamHeader->Data, &BytesToCopy);
        //RtlCopyMemory(StreamPointer->StreamHeader->Data, FrameBuffer, BytesToCopy);
        //Status = STATUS_SUCCESS;
        if (NT_SUCCESS(Status))
        {
            /* Advance the stream pointer to deliver the buffer */
            KsStreamPointerAdvanceOffsetsAndUnlock(StreamPointer, 0, BytesToCopy, FALSE);
            KsStreamPointerUnlock(StreamPointer, FALSE);
        }
        else
        {
            KsStreamPointerUnlock(StreamPointer, FALSE);
        }
    }
    else
    {
        DPRINT("USBVideoDeliverFrame: BytesToCopy is 0\n");
        KsStreamPointerUnlock(StreamPointer, FALSE);
    }
}
