/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ACPI fan class device interface
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"

extern const GUID GUID_DEVICE_FAN;   // guid.c

VOID
UacpiFanStart(PUACPI_PDO Pdo)
{
    if (_stricmp(Pdo->Hid, "PNP0C0B") != 0)
    {
        return;   // not a fan
    }
    UacpiDevIfRegister(Pdo, &GUID_DEVICE_FAN, "fan");
    // A fan is also a cooling device.
    UacpiCoolingIfRegister(Pdo);
}
