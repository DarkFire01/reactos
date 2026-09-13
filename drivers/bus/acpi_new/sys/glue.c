/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     uACPI interpreter bring-up and teardown
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"
#include <uacpi/notify.h>
#include <uacpi/internal/event.h>   // uacpi_clear_all_events
#include <uacpi/context.h>           // uacpi_context_set_log_level

static const char *
UacpiStatusName(uacpi_status st)
{
    return uacpi_status_to_string(st);
}

// Event handlers run at DISPATCH (SCI DPC) or PASSIVE (work item).

static uacpi_status
UacpiGlobalNotifyHandler(uacpi_handle ctx, uacpi_namespace_node *node,
                        uacpi_u64 value)
{
    uacpi_object_name name = uacpi_namespace_node_name(node);
    UNREFERENCED_PARAMETER(ctx);
    UacpiTrace("[acpi] Notify(%c%c%c%c, 0x%I64X)\n",
              name.text[0], name.text[1], name.text[2], name.text[3], value);
    // Forward to the target device's ACPI_INTERFACE_STANDARD Notify.
    UacpiRouteNotify(node, (ULONG)value);
    return UACPI_STATUS_OK;
}

static VOID
UacpiInstallEventHandlers(PUACPI_FDO Fdo)
{
    uacpi_status st;

    // A Notify handler on the root catches notifies to every node.
    UacpiTrace("[acpi] installing global Notify handler...\n");
    st = uacpi_install_notify_handler(uacpi_namespace_root(),
                                      UacpiGlobalNotifyHandler, NULL);
    if (uacpi_unlikely_error(st))
    {
        UacpiTrace("[acpi] install_notify_handler failed: %s\n", UacpiStatusName(st));
    }
    else
    {
        UacpiTrace("[acpi] global Notify handler installed\n");
    }

    // Bind the resource hub before enumeration; failure is not fatal.
    (void)UacpiConnectResourceHub();

    // ACPI\FixedButton PDO and PM1 fixed-event handlers (drvs/button.c).
    UacpiFixedButtonInit(Fdo);
}

NTSTATUS
UacpiBringUpInterpreter(PUACPI_FDO Fdo)
{
    uacpi_status st;

    PAGED_CODE();

    // Phase 1: subsystem init. flags = 0 (enter ACPI mode, honor _OSI).
    st = uacpi_initialize(0);
    if (uacpi_unlikely_error(st))
    {
        UacpiTrace("[acpi] uacpi_initialize failed: %s\n", UacpiStatusName(st));
        return STATUS_UNSUCCESSFUL;
    }


    UacpiTrace("[acpi] uacpi_initialize OK\n");

    // Phase 2: load DSDT/SSDTs; uACPI records the SCI here.
    st = uacpi_namespace_load();
    if (uacpi_unlikely_error(st))
    {
        UacpiTrace("[acpi] uacpi_namespace_load failed: %s\n", UacpiStatusName(st));
        return STATUS_UNSUCCESSFUL;
    }
    UacpiTrace("[acpi] uacpi_namespace_load OK\n");

    // HAL handshake (\_Sx, interrupt model, \_PIC), then the vector subsystem.
    (void)UacpiHalPmHandshake();
    (void)UacpiIrqLibInitialize();

    // Disable all GPEs and clear pending events so the level SCI cannot storm.
    {
        uacpi_status quiesce;

        quiesce = uacpi_disable_all_gpes();
        if (uacpi_unlikely_error(quiesce))
        {
            UacpiTrace("[acpi] SCI: disable_all_gpes failed: %s\n",
                       UacpiStatusName(quiesce));
        }

        quiesce = uacpi_clear_all_events();
        if (uacpi_unlikely_error(quiesce))
        {
            UacpiTrace("[acpi] SCI: clear_all_events failed: %s\n",
                       UacpiStatusName(quiesce));
        }
    }

    // Connect the SCI after IrqLib and before any GPE is enabled.
    {
        NTSTATUS sci = UacpiHostConnectSci();
        if (!NT_SUCCESS(sci))
        {
            UacpiTrace("[acpi] FATAL - SCI connect failed 0x%X\n", sci);
            return sci;
        }
    }

    // EC handler before namespace init; _INI methods often read the EC.
    UacpiEcInitialize();

    // Phase 3: _STA/_INI walk and _REG for the default opregions.
    st = uacpi_namespace_initialize();
    if (uacpi_unlikely_error(st))
    {
        UacpiTrace("[acpi] uacpi_namespace_initialize failed: %s\n", UacpiStatusName(st));
        return STATUS_UNSUCCESSFUL;
    }
    UacpiTrace("[acpi] uacpi_namespace_initialize OK\n");

    // \_SB._OSC platform negotiation before devices come up.
    UacpiPlatformOscNegotiate();

    // Enumerate power resources for D-state reference counting (powerres.c).
    UacpiPowerResInit();

    // Phase 4: install handlers first; a GPE during install can deadlock.
    UacpiInstallEventHandlers(Fdo);

    // Mark every _PRW GPE for wake and start each device disarmed, keeping the
    // fixed-function buttons/lid/RTC live in S0. Must precede finalize so those
    // GPEs are treated as wake sources, not runtime-auto-enabled.
    UacpiWakeBootInit();

    // Phase 5: enable the runtime GPEs last.
    st = uacpi_finalize_gpe_initialization();
    if (uacpi_unlikely_error(st))
    {
        UacpiTrace("[acpi] uacpi_finalize_gpe_initialization failed: %s\n",
                  UacpiStatusName(st));
        // Non-fatal for bring-up; the namespace is still usable.
    }
    else
    {
        UacpiTrace("[acpi] GPE initialization finalized\n");
    }

    Fdo->UacpiUp = TRUE;
    UacpiTrace("[acpi] interpreter up\n");
    return STATUS_SUCCESS;
}

VOID
UacpiTearDownInterpreter(PUACPI_FDO Fdo)
{
    // uACPI has no shutdown that reverses initialize; only mark it down.
    UacpiMsiDiagDisarm();
    Fdo->UacpiUp = FALSE;
}
