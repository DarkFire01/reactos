/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared definitions for the SCI host (uacpi_isr.c)
 */
#ifndef _UACPI_SCI_H_
#define _UACPI_SCI_H_

#include <ntddk.h>

#include <uacpi/kernel_api.h>
#include <uacpi/status.h>

#include "uacpi_host.h"

#define UACPI_HOST_TAG   'HpcA'   // 'AcpH'

// SCI handler installed by uACPI, connected by UacpiHostConnectSci.
typedef struct _ACPI_HOST_INTERRUPT
{
    PKINTERRUPT             KInterrupt;
    KDPC                    Dpc;
    uacpi_interrupt_handler Handler;
    uacpi_handle            Ctx;
    ULONG                   Vector;
    KIRQL                   Irql;
    BOOLEAN                 LevelTriggered;
    ULONG                   Unhandled;      ///< consecutive unhandled SCIs
} UACPI_HOST_INTERRUPT, *PUACPI_HOST_INTERRUPT;

// GSIV to vector, IRQL, affinity, polarity and mode. Implemented in sys/irqlib.c.
extern NTSTATUS
UacpiIrqLibResolveVector(
    _In_ ULONG Gsiv,
    _Out_ PULONG Vector,
    _Out_ PKIRQL Irql,
    _Out_ PKAFFINITY Affinity,
    _Out_ PULONG Polarity,
    _Out_ PULONG Mode);

#endif // _UACPI_SCI_H_
