/*
 * PROJECT:     ReactOS Broadcom NetXtreme Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Interrupt handlers
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmai.com>
 */

#include "nic.h"

#include "debug.h"

VOID
NTAPI
MiniportISR(OUT PBOOLEAN InterruptRecognized,
            OUT PBOOLEAN QueueMiniportHandleInterrupt,
            IN NDIS_HANDLE MiniportAdapterContext)
{
    //ULONG Value;
    //PI21X_ADAPTER Adapter = (PI21X_ADAPTER)MiniportAdapterContext;
    
    NDIS_MinDbgPrint("I21X ISR\n");

    /* Reading the interrupt acknowledges them */
    /*I21XReadUlong(Adapter, I21X_REG_ICR, &Value);

    Value &= Adapter->InterruptMask;
    _InterlockedOr(&Adapter->InterruptPending, Value);

    if (Value)
    {
        *InterruptRecognized = TRUE;
        // Mark the events pending service
        *QueueMiniportHandleInterrupt = TRUE;
    }
    else
    {
        // This is not ours.
        *InterruptRecognized = FALSE;
        *QueueMiniportHandleInterrupt = FALSE;
    }*/
}

VOID
NTAPI
MiniportHandleInterrupt(IN NDIS_HANDLE MiniportAdapterContext)
{
    //ULONG InterruptPending;
    //PI21X_ADAPTER Adapter = (PI21X_ADAPTER)MiniportAdapterContext;

    NDIS_MinDbgPrint("I21X HandleInterrupt\n");
}
