/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/usage.c
 * PURPOSE:         Bus/Device Usage Reporting
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/*
 * A device usage notification says that something the system cannot lose - the
 * paging file, the hibernation image, the crash dump - now lives behind a
 * device, or has stopped doing so. Every device on the path gets told, so that
 * none of them lets itself be stopped, removed or powered down while it is
 * carrying one.
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

VOID
NTAPI
PciApplyDeviceUsage(IN PPCI_POWER_STATE PowerState,
                    IN PIO_STACK_LOCATION IoStackLocation)
{
    PLONG Counter;

    /* Only the three that would take the system down with them are tracked */
    switch (IoStackLocation->Parameters.UsageNotification.Type)
    {
        case DeviceUsageTypePaging:
            Counter = &PowerState->Paging;
            break;

        case DeviceUsageTypeHibernation:
            Counter = &PowerState->Hibernate;
            break;

        case DeviceUsageTypeDumpFile:
            Counter = &PowerState->CrashDump;
            break;

        default:
            return;
    }

    /*
     * These nest, because several things can be on one device at once, so the
     * device is only free of a usage again once every one of them is gone.
     */
    if (IoStackLocation->Parameters.UsageNotification.InPath)
    {
        InterlockedIncrement(Counter);
    }
    else
    {
        ASSERT(*Counter > 0);
        InterlockedDecrement(Counter);
    }
}

/* EOF */
