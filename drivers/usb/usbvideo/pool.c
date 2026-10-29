/*
* PROJECT:     ReactOS Universal Video Class Driver
* LICENSE:     GPL - See COPYING in the top level directory
* FILE:        drivers/usb/usbvideo/pool.c
* PURPOSE:     USB Video device driver.
* PROGRAMMERS:
*              Johannes Anderwald (johannes.anderwald@reactos.org)
*/
#include "usbvideo.h"

PVOID
NTAPI
AllocFunction(
    IN ULONG ItemSize)
{
    PVOID Item = ExAllocatePoolZero(NonPagedPool, ItemSize, USBVIDEO_TAG);

    // done
    return Item;
}

VOID
NTAPI
FreeFunction(
    IN PVOID Item)
{
    /* free item */
    ExFreePoolWithTag(Item, USBVIDEO_TAG);
}

