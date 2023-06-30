/*
 * PROJECT:         ReactOS new xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Primary XHCI Class
 * COPYRIGHT:       Copyright 2023 Justin Miller <justinmiller100@gmail.com>
 */

#include "../usbxhci.h"
#include <debug.h>



NTSTATUS
XHCI::NoOp()
{
    /* Do nothing for now */
    return MP_STATUS_SUCCESS;
}

XHCI::XHCI()
{
    /* Do nothing for now */
}

XHCI::~XHCI()
{
    /* Do nothing for now */
}
