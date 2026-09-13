/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Application-launch button interface (PNP0C32)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"

extern const GUID GUID_DEVICE_APPLICATIONLAUNCH_BUTTON;   // guid.c

VOID
UacpiApplaunchStart(PUACPI_PDO Pdo)
{
    if (_stricmp(Pdo->Hid, "PNP0C32") != 0)
    {
        return;   // not an experience / app-launch button
    }
    UacpiDevIfRegister(Pdo, &GUID_DEVICE_APPLICATIONLAUNCH_BUTTON, "applaunch");
}
