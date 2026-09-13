/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ACPI processor class device interface
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

// Legacy Processor() objects (IsProcessor) and ACPI0007 processor devices.

#include "acpipriv.h"

extern const GUID GUID_DEVICE_PROCESSOR;   // guid.c

VOID
UacpiProcessorStart(PUACPI_PDO Pdo)
{
    if (!Pdo->IsProcessor && _stricmp(Pdo->Hid, "ACPI0007") != 0)
    {
        return;   // not a processor
    }
    UacpiDevIfRegister(Pdo, &GUID_DEVICE_PROCESSOR, "processor");
    // Throttling (_PTC/_TSS) or P-states (_PSS): also a cooling device.
    if (UacpiNodeHasChild(Pdo->Node, "_PTC") || UacpiNodeHasChild(Pdo->Node, "_TSS") ||
        UacpiNodeHasChild(Pdo->Node, "_PSS"))
        {
        UacpiCoolingIfRegister(Pdo);
    }
}
