/*
* PROJECT:     ReactOS Universal Video Class Driver
* LICENSE:     GPL - See COPYING in the top level directory
* FILE:        drivers/usb/usbvideo/uvc.c
* PURPOSE:     USB Video device driver.
* PROGRAMMERS:
*              Johannes Anderwald (johannes.anderwald@reactos.org)
*/
#define NDEBUG
#include <math.h>
#include "usbvideo.h"

BOOLEAN
UvcDiagnoseYuy2Frame(
    PUCHAR pFrame,
    ULONG  length)
{
    // Ersten Pixel lesen
    UCHAR Y0 = pFrame[0];
    UCHAR Cb = pFrame[1];
    UCHAR Y1 = pFrame[2];
    UCHAR Cr = pFrame[3];

    DbgPrint("YUY2 Pixel[0]: Y0=%d Cb=%d Y1=%d Cr=%d\n",
        Y0, Cb, Y1, Cr);

    // YCbCr → RGB konvertieren
    int R = (int)Y0 + 1.402   * (Cr - 128);
    int G = (int)Y0 - 0.344   * (Cb - 128)
                    - 0.714   * (Cr - 128);
    int B = (int)Y0 + 1.772   * (Cb - 128);

    DbgPrint("  → RGB: R=%d G=%d B=%d\n",
        max(0,min(255,R)),
        max(0,min(255,G)),
        max(0,min(255,B)));

    // Prüfe ob Cb/Cr neutral sind
    if (abs(Cb - 128) < 10 && abs(Cr - 128) < 10)
        DbgPrint("  Chroma neutral → Graustufen korrekt\n");
    else
        DbgPrint("  Chroma verschoben → Farbstich moeglich\n");

    // Prüfe ob Buffer leer
    ULONG zeros = 0;
    for (ULONG i = 0; i < length; i++)
        if (pFrame[i] == 0) zeros++;
    DbgPrint("  Null-Bytes: %d von %d (%.1f%%)\n",
        zeros, length,
        100.0*zeros/(length));
     return zeros < length;
}

VOID
NTAPI
USBVideoDeliverFrame(
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension,
    PUCHAR FrameBuffer,
    ULONG FrameSize,
    UCHAR CompleteFrame)
{
    PKSSTREAM_POINTER StreamPointer;
    PKSPIN Pin;

    //if (!UvcDiagnoseYuy2Frame(FrameBuffer, FrameSize))
    //    return;

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
        //ASSERT(BytesToCopy == FrameSize);
        //ASSERT(FrameSize < StreamPointer->Offset->Remaining);
        ULONG Offset = StreamPointer->Offset->Count - StreamPointer->Offset->Remaining;
        DPRINT("USBVideoDeliverFrame: Copying %u bytes at offset %u user buffer (frame size %u)\n",
            BytesToCopy,
            Offset,
            FrameSize);

        RtlCopyMemory(&((PUCHAR)StreamPointer->StreamHeader->Data)[Offset], FrameBuffer, BytesToCopy);

        /* Advance the stream pointer to deliver the buffer */
        KsStreamPointerAdvanceOffsetsAndUnlock(StreamPointer, 0, BytesToCopy, CompleteFrame);

    }
    else
    {
        DPRINT("USBVideoDeliverFrame: BytesToCopy is 0\n");
        KsStreamPointerUnlock(StreamPointer, FALSE);
    }
}
