/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared class device-interface registration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"

extern const GUID GUID_DEVINTERFACE_THERMAL_COOLING;   // guid.c

// Register and enable a class interface; kept in ClassIf* for teardown.
VOID
UacpiDevIfRegister(PUACPI_PDO Pdo, const GUID *Guid, const char *Label)
{
    NTSTATUS status;

    if (Pdo->ClassIfOn)
    {
        return;
    }
    status = IoRegisterDeviceInterface(Pdo->Common.Self, Guid, NULL,
                                       &Pdo->ClassIfLink);
    if (!NT_SUCCESS(status))
    {
        UacpiTrace("[acpi] %s: %s IoRegisterDeviceInterface failed 0x%X\n",
                  Label, Pdo->Name, status);
        return;
    }
    Pdo->ClassIfOn = TRUE;
    (void)IoSetDeviceInterfaceState(&Pdo->ClassIfLink, TRUE);
    UacpiTrace("[acpi] %s: %s class interface up\n", Label, Pdo->Name);
}

VOID
UacpiDevIfRemove(PUACPI_PDO Pdo)
{
    if (Pdo->ClassIfOn)
    {
        (void)IoSetDeviceInterfaceState(&Pdo->ClassIfLink, FALSE);
        RtlFreeUnicodeString(&Pdo->ClassIfLink);
        Pdo->ClassIfOn = FALSE;
    }
    if (Pdo->CoolingIfOn)
    {
        (void)IoSetDeviceInterfaceState(&Pdo->CoolingIfLink, FALSE);
        RtlFreeUnicodeString(&Pdo->CoolingIfLink);
        Pdo->CoolingIfOn = FALSE;
    }
}

// Thermal-cooling interface; separate from ClassIfLink so a PDO can hold both.
VOID
UacpiCoolingIfRegister(PUACPI_PDO Pdo)
{
    NTSTATUS status;

    if (Pdo->CoolingIfOn)
    {
        return;
    }
    status = IoRegisterDeviceInterface(Pdo->Common.Self,
                                       &GUID_DEVINTERFACE_THERMAL_COOLING, NULL,
                                       &Pdo->CoolingIfLink);
    if (!NT_SUCCESS(status))
    {
        UacpiTrace("[acpi] cooling: %s IoRegisterDeviceInterface failed 0x%X\n",
                  Pdo->Name, status);
        return;
    }
    Pdo->CoolingIfOn = TRUE;
    (void)IoSetDeviceInterfaceState(&Pdo->CoolingIfLink, TRUE);
    UacpiTrace("[acpi] cooling: %s class interface up\n", Pdo->Name);
}

// Called from pdo.c. Each start ignores devices it does not own.
VOID
UacpiDrvsStartDevice(PUACPI_PDO Pdo)
{
    UacpiButtonStart(Pdo);      // power/sleep/lid (SYS_BUTTON, its own state)
    UacpiProcessorStart(Pdo);   // Processor() / ACPI0007 -> GUID_DEVICE_PROCESSOR
    UacpiThermalStart(Pdo);     // ThermalZone -> GUID_DEVICE_THERMAL_ZONE
    UacpiFanStart(Pdo);         // PNP0C0B -> GUID_DEVICE_FAN
    UacpiApplaunchStart(Pdo);   // PNP0C32 -> GUID_DEVICE_APPLICATIONLAUNCH_BUTTON
    UacpiEcStart(Pdo);          // PNP0C09 -> adopt or install the controller
}

VOID
UacpiDrvsRemoveDevice(PUACPI_PDO Pdo)
{
    UacpiButtonRemove(Pdo);     // SYS_BUTTON has its own event-queue teardown
    UacpiThermalRemove(Pdo);    // deregister the thermal WMI provider
    UacpiDevIfRemove(Pdo);      // processor / thermal / fan share the generic one
    UacpiEcRemove(Pdo);         // drop the device link; the EC stays up for AML
}
