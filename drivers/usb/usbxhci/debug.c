/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         debug functions
 * COPYRIGHT:       Rama Teja Gampa <ramateja.g@gmail.com>
 */

#include "usbxhcip.h"
//NDEBUG
#include <debug.h>

VOID
NTAPI
XHCI_DumpHwTD(IN PXHCI_HCD_TD TD)
{
    if (!TD)
    {
        return;
    }

    DPRINT1(": TD                       - %p\n", TD);
    DPRINT1(": TD->PhysicalAddress      - %p\n", TD->PhysicalAddress);
    DPRINT1(": TD->HwTD.NextTD          - %p\n", TD->HwTD.NextTD);
    DPRINT1(": TD->HwTD.AlternateNextTD - %p\n", TD->HwTD.AlternateNextTD);
    DPRINT1(": TD->HwTD.Token.AsULONG   - %p\n", TD->HwTD.Token.AsULONG);
}

VOID
NTAPI
XHCI_DumpHwQH(IN PXHCI_HCD_QH QH)
{
    if (!QH)
    {
        return;
    }

    DPRINT1(": QH->sqh.HwQH.CurrentTD       - %p\n", QH->sqh.HwQH.CurrentTD);
    DPRINT1(": QH->sqh.HwQH.NextTD          - %p\n", QH->sqh.HwQH.NextTD);
    DPRINT1(": QH->sqh.HwQH.AlternateNextTD - %p\n", QH->sqh.HwQH.AlternateNextTD);
    DPRINT1(": QH->sqh.HwQH.Token.AsULONG   - %p\n", QH->sqh.HwQH.Token.AsULONG);
}
