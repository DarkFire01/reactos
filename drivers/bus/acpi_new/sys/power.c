/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Device D-states, S->D capability map, and wake arming
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"
#include <uacpi/event.h>
#include <uacpi/namespace.h>      // uacpi_namespace_node_find
#include <uacpi/utilities.h>      // uacpi_eval_simple_integer

// TRUE if 'node' has a direct child object named 'name'.
BOOLEAN
UacpiNodeHasChild(uacpi_namespace_node *node, const char *name)
{
    uacpi_namespace_node *child = NULL;
    return node != NULL &&
           uacpi_likely_success(uacpi_namespace_node_find(node, name, &child)) &&
           child != NULL;
}

// Evaluate a no-argument method; TRUE if it existed and ran.
static BOOLEAN
UacpiPowerEvalMethod(uacpi_namespace_node *node, const char *name)
{
    uacpi_status st = uacpi_eval(node, name, NULL, NULL);
    if (uacpi_unlikely_error(st))
    {
        if (st != UACPI_STATUS_NOT_FOUND)
        {
            UacpiTrace("[acpi] power: %s eval failed: %s\n",
                      name, uacpi_status_to_string(st));
        }
        return FALSE;
    }
    return TRUE;
}

// Set a device D-state: acquire _PRx rails, run _PSx, release unused rails.
NTSTATUS
UacpiPowerSetDeviceState(PUACPI_PDO Pdo, DEVICE_POWER_STATE DState)
{
    static const char *psx[4] = { "_PS0", "_PS1", "_PS2", "_PS3" };
    PUACPI_POWER_RESOURCE newList[UACPI_MAX_PR_PER_DEV];
    ULONG newCount = 0;
    ULONG d;

    if (Pdo->Node == NULL)
    {
        return STATUS_SUCCESS;
    }
    switch (DState)
    {
    case PowerDeviceD0: d = 0; break;
    case PowerDeviceD1: d = 1; break;
    case PowerDeviceD2: d = 2; break;
    case PowerDeviceD3: d = 3; break;
    default:            return STATUS_SUCCESS;
    }

    // Rails for the new state go on before _PSx; unneeded rails go off after.
    UacpiPowerResAcquireForState(Pdo, DState, newList, &newCount);
    (void)UacpiPowerEvalMethod(Pdo->Node, psx[d]);
    UacpiPowerResReleaseDelta(Pdo, newList, newCount);

    Pdo->CurrentDState = DState;
    Pdo->DStateKnown = TRUE;
    UacpiTrace("[acpi] power: %s -> D%u\n", Pdo->Name, d);
    return STATUS_SUCCESS;
}

// Fill DEVICE_CAPABILITIES.DeviceState[] from _SxD and the wake fields from _PRW.
VOID
UacpiPowerBuildStateMap(PUACPI_PDO Pdo, PDEVICE_CAPABILITIES Caps)
{
    ULONG s;

    // Cached after the first walk; see UACPI_PDO::CapsMapDone.
    if (Pdo->CapsMapDone)
    {
        for (s = 0; s < PowerSystemMaximum; s++)
        {
            Caps->DeviceState[s] = Pdo->CapsMap.DeviceState[s];
        }
        Caps->DeviceWake = Pdo->CapsMap.DeviceWake;
        Caps->SystemWake = Pdo->CapsMap.SystemWake;
        Caps->DeviceD1   = Pdo->CapsMap.DeviceD1;
        Caps->DeviceD2   = Pdo->CapsMap.DeviceD2;
        Caps->WakeFromD0 = Pdo->CapsMap.WakeFromD0;
        Caps->WakeFromD1 = Pdo->CapsMap.WakeFromD1;
        Caps->WakeFromD2 = Pdo->CapsMap.WakeFromD2;
        Caps->WakeFromD3 = Pdo->CapsMap.WakeFromD3;
        return;
    }

    // Default mapping.
    for (s = 0; s < PowerSystemMaximum; s++)
    {
        Caps->DeviceState[s] =
            (s == PowerSystemWorking) ? PowerDeviceD0 : PowerDeviceD3;
    }
    Caps->DeviceWake = PowerDeviceUnspecified;
    Caps->SystemWake = PowerSystemUnspecified;

    if (Pdo->Node == NULL)
    {
        return;
    }

    // D0 and D3 are always supported; D1/D2 only with a _PSx or _PRx for them.
    {
        BOOLEAN d1 = UacpiNodeHasChild(Pdo->Node, "_PS1") ||
                     UacpiNodeHasChild(Pdo->Node, "_PR1");
        BOOLEAN d2 = UacpiNodeHasChild(Pdo->Node, "_PS2") ||
                     UacpiNodeHasChild(Pdo->Node, "_PR2");
        Caps->DeviceD1 = d1;
        Caps->DeviceD2 = d2;

        // _S1D.._S4D, clamped to a supported D-state. S0 stays D0.
        for (s = PowerSystemSleeping1; s <= PowerSystemHibernate; s++)
        {
            char name[5] = { '_', 'S', (char)('0' + (s - PowerSystemWorking)), 'D', 0 };
            uacpi_u64 v = 0;
            if (uacpi_likely_success(uacpi_eval_simple_integer(Pdo->Node, name, &v)) &&
                v <= 3)
                {
                DEVICE_POWER_STATE d = (DEVICE_POWER_STATE)(PowerDeviceD0 + (ULONG)v);
                if (d == PowerDeviceD1 && !d1)
                {
                    d = d2 ? PowerDeviceD2 : PowerDeviceD3;
                }
                if (d == PowerDeviceD2 && !d2)
                {
                    d = PowerDeviceD3;
                }
                Caps->DeviceState[s] = d;
            }
        }

        // _PRW: [0] GPE, [1] deepest wake S-state, [2..] wake power resources.
        // SystemWake from _PRW[1] (5 = Shutdown); DeviceWake from _S<wake>D,
        // else DeviceState[wake], else D3. _SxW is not consulted.
        {
            uacpi_object *ret = NULL;
            if (uacpi_likely_success(uacpi_eval(Pdo->Node, "_PRW", NULL, &ret)) &&
                ret != NULL)
                {
                uacpi_object_array pkg;
                if (uacpi_object_get_type(ret) == UACPI_OBJECT_PACKAGE &&
                    uacpi_likely_success(uacpi_object_get_package(ret, &pkg)) &&
                    pkg.count >= 2)
                    {
                    uacpi_u64 deepest = 0;
                    if (uacpi_likely_success(
                            uacpi_object_get_integer(pkg.objects[1], &deepest)) &&
                        deepest >= 1 && deepest <= 5)
                            {
                        // _PRW[1] N -> S-state N; SYSTEM_POWER_STATE index N+1.
                        SYSTEM_POWER_STATE sw =
                            (SYSTEM_POWER_STATE)(PowerSystemWorking + (ULONG)deepest);
                        BOOLEAN supported;
                        DEVICE_POWER_STATE dw;
                        uacpi_u64 sxd = 0;
                        char sxdName[5] = { '_', 'S', (char)('0' + (ULONG)deepest),
                                            'D', 0 };

                        // S4 (Hibernate) and S5 (Shutdown) always exist; S1-S3
                        // only if the matching \_Sx package is present.
                        if (deepest >= 4)
                        {
                            supported = TRUE;
                        }
                        else
                        {
                            char sx[5] = { '_', 'S', (char)('0' + (ULONG)deepest), '_', 0 };
                            supported = UacpiNodeHasChild(uacpi_namespace_root(), sx);
                        }

                        if (supported)
                        {
                            Caps->SystemWake = sw;

                            // DeviceWake: _S<wake>D, else the mapped DeviceState
                            // for that S-state, else D3.
                            if (uacpi_likely_success(uacpi_eval_simple_integer(
                                    Pdo->Node, sxdName, &sxd)) && sxd <= 3)
                            {
                                dw = (DEVICE_POWER_STATE)(PowerDeviceD0 + (ULONG)sxd);
                            }
                            else if (Caps->DeviceState[sw] >= PowerDeviceD0 &&
                                     Caps->DeviceState[sw] <= PowerDeviceD3)
                            {
                                dw = Caps->DeviceState[sw];
                            }
                            else
                            {
                                dw = PowerDeviceD3;
                            }

                            Caps->DeviceWake = dw;
                            // acpi.sys sets exactly the DeviceWake bit.
                            Caps->WakeFromD0 = (BOOLEAN)(dw == PowerDeviceD0);
                            Caps->WakeFromD1 = (BOOLEAN)(dw == PowerDeviceD1);
                            Caps->WakeFromD2 = (BOOLEAN)(dw == PowerDeviceD2);
                            Caps->WakeFromD3 = (BOOLEAN)(dw == PowerDeviceD3);
                            UacpiTrace("[acpi] power: %s wake-capable (sys S%u dev D%u)\n",
                                      Pdo->Name, (ULONG)deepest,
                                      (ULONG)(dw - PowerDeviceD0));
                        }
                        else
                        {
                            UacpiTrace("[acpi] power: %s _PRW wake S%u unsupported\n",
                                      Pdo->Name, (ULONG)deepest);
                        }
                    }
                }
                uacpi_object_unref(ret);
            }
        }
    }

    // Cache the result; AML answers do not change for the life of the PDO.
    for (s = 0; s < PowerSystemMaximum; s++)
    {
        Pdo->CapsMap.DeviceState[s] = Caps->DeviceState[s];
    }
    Pdo->CapsMap.DeviceWake = Caps->DeviceWake;
    Pdo->CapsMap.SystemWake = Caps->SystemWake;
    Pdo->CapsMap.DeviceD1   = Caps->DeviceD1;
    Pdo->CapsMap.DeviceD2   = Caps->DeviceD2;
    Pdo->CapsMap.WakeFromD0 = Caps->WakeFromD0;
    Pdo->CapsMap.WakeFromD1 = Caps->WakeFromD1;
    Pdo->CapsMap.WakeFromD2 = Caps->WakeFromD2;
    Pdo->CapsMap.WakeFromD3 = Caps->WakeFromD3;
    Pdo->CapsMapDone = TRUE;
}

// Merge _PRW/_SxD wake capability into caps a bus driver already filled. Used
// for filtered foreign PDOs (the on-board NIC/USB), where the bus reports its
// own D-states but knows nothing of the ACPI wake plumbing. Only the wake
// fields are touched; DeviceState[] from the bus is left as-is.
VOID
UacpiPowerMergeWakeCaps(uacpi_namespace_node *Node, PDEVICE_CAPABILITIES Caps)
{
    uacpi_object *ret = NULL;
    uacpi_object_array pkg;
    uacpi_u64 deepest = 0;

    if (Node == NULL || Caps == NULL)
    {
        return;
    }
    if (uacpi_unlikely_error(uacpi_eval(Node, "_PRW", NULL, &ret)) || ret == NULL)
    {
        return;
    }

    if (uacpi_object_get_type(ret) == UACPI_OBJECT_PACKAGE &&
        uacpi_likely_success(uacpi_object_get_package(ret, &pkg)) &&
        pkg.count >= 2 &&
        uacpi_likely_success(uacpi_object_get_integer(pkg.objects[1], &deepest)) &&
        deepest >= 1 && deepest <= 5)
        {
        SYSTEM_POWER_STATE sw =
            (SYSTEM_POWER_STATE)(PowerSystemWorking + (ULONG)deepest);
        BOOLEAN supported;
        DEVICE_POWER_STATE dw;
        uacpi_u64 sxd = 0;
        char sxdName[5] = { '_', 'S', (char)('0' + (ULONG)deepest), 'D', 0 };

        // S4/S5 always exist; S1-S3 only if the matching \_Sx package is present.
        if (deepest >= 4)
        {
            supported = TRUE;
        }
        else
        {
            char sx[5] = { '_', 'S', (char)('0' + (ULONG)deepest), '_', 0 };
            supported = UacpiNodeHasChild(uacpi_namespace_root(), sx);
        }

        if (supported)
        {
            // SystemWake: the shallower of the bus report and what the GPE can
            // actually deliver (both constraints have to hold).
            if (Caps->SystemWake == PowerSystemUnspecified ||
                Caps->SystemWake > sw)
            {
                Caps->SystemWake = sw;
            }

            // DeviceWake: _S<wake>D, else the mapped DeviceState, else D3.
            if (uacpi_likely_success(
                    uacpi_eval_simple_integer(Node, sxdName, &sxd)) && sxd <= 3)
            {
                dw = (DEVICE_POWER_STATE)(PowerDeviceD0 + (ULONG)sxd);
            }
            else if (Caps->DeviceState[sw] >= PowerDeviceD0 &&
                     Caps->DeviceState[sw] <= PowerDeviceD3)
            {
                dw = Caps->DeviceState[sw];
            }
            else
            {
                dw = PowerDeviceD3;
            }

            // Push the wake D-state at least as deep as ACPI allows and light
            // the matching WakeFromDx bit so the PM will arm from that state.
            if (Caps->DeviceWake < dw)
            {
                Caps->DeviceWake = dw;
            }
            switch (dw)
            {
            case PowerDeviceD0: Caps->WakeFromD0 = TRUE; break;
            case PowerDeviceD1: Caps->WakeFromD1 = TRUE; break;
            case PowerDeviceD2: Caps->WakeFromD2 = TRUE; break;
            default:            Caps->WakeFromD3 = TRUE; break;
            }
            UacpiTrace("[acpi] filter wake caps: sys S%u dev D%u\n",
                      (ULONG)deepest, (ULONG)(dw - PowerDeviceD0));
        }
    }

    uacpi_object_unref(ret);
}

// Wake: IRP_MN_WAIT_WAKE arms the _PRW GPE and pends until the GPE fires. The
// state lives in a UACPI_WAKE embedded in both a PDO and a filter DO, so the
// exact same arming path serves ACPI's own devnodes and the foreign PDOs we
// filter (on-board NIC/USB, the usual S3 wake sources).

// Bind a wake block to an AML node: init the lock, cache the node name.
VOID
UacpiWakeInit(PUACPI_WAKE Wake, uacpi_namespace_node *Node)
{
    Wake->Node = Node;
    Wake->SysWake = PowerSystemUnspecified;
    Wake->DevWake = PowerDeviceUnspecified;
    Wake->ReqSysState = PowerSystemUnspecified;
    KeInitializeSpinLock(&Wake->Lock);

    Wake->Name[0] = '?'; Wake->Name[1] = '\0';
    if (Node != NULL)
    {
        uacpi_object_name n = uacpi_namespace_node_name(Node);
        Wake->Name[0] = n.text[0]; Wake->Name[1] = n.text[1];
        Wake->Name[2] = n.text[2]; Wake->Name[3] = n.text[3];
        Wake->Name[4] = '\0';
    }
}

// Parse \_PRW[0] into (GpeDevice, GpeIdx). Cached after the first attempt.
static BOOLEAN
UacpiWakeParsePrw(PUACPI_WAKE Wake)
{
    uacpi_object *ret = NULL;

    if (Wake->GpeParsed)
    {
        return Wake->GpeValid;
    }
    Wake->GpeParsed = TRUE;

    if (Wake->Node == NULL ||
        uacpi_unlikely_error(uacpi_eval(Wake->Node, "_PRW", NULL, &ret)) ||
        ret == NULL)
        {
        return FALSE;
    }

    {
        uacpi_object_array pkg;
        if (uacpi_object_get_type(ret) == UACPI_OBJECT_PACKAGE &&
            uacpi_likely_success(uacpi_object_get_package(ret, &pkg)) &&
            pkg.count >= 2)
            {
            // Only the Integer _PRW[0] form (a \_GPE index) is supported.
            uacpi_u64 gpe = 0;
            uacpi_u64 deepest = 0;

            if (uacpi_object_get_type(pkg.objects[0]) == UACPI_OBJECT_INTEGER &&
                uacpi_likely_success(
                    uacpi_object_get_integer(pkg.objects[0], &gpe)) &&
                gpe <= 0xFF)
                {
                Wake->GpeDevice = NULL;
                Wake->GpeIdx    = (UCHAR)gpe;
                Wake->GpeValid  = TRUE;
                UacpiTrace("[acpi] wake: %s _PRW GPE = 0x%02X (\\_GPE)\n",
                          Wake->Name, (ULONG)gpe);
            }
            else
            {
                UacpiTrace("[acpi] wake: %s _PRW uses a GPE-block device form "
                          "(unsupported), device will not be armed\n", Wake->Name);
            }

            // _PRW[1] is the deepest wake S-state; cache it (and the matching
            // wake D-state) for validation and the _DSW target arguments.
            if (uacpi_likely_success(
                    uacpi_object_get_integer(pkg.objects[1], &deepest)) &&
                deepest >= 1 && deepest <= 5)
                {
                BOOLEAN supported;
                if (deepest >= 4)
                {
                    supported = TRUE;
                }
                else
                {
                    char sx[5] = { '_', 'S', (char)('0' + (ULONG)deepest), '_', 0 };
                    supported = UacpiNodeHasChild(uacpi_namespace_root(), sx);
                }
                if (supported)
                {
                    uacpi_u64 sxd = 0;
                    char sxdName[5] = { '_', 'S', (char)('0' + (ULONG)deepest),
                                        'D', 0 };
                    Wake->SysWake = (SYSTEM_POWER_STATE)
                        (PowerSystemWorking + (ULONG)deepest);
                    if (uacpi_likely_success(uacpi_eval_simple_integer(
                            Wake->Node, sxdName, &sxd)) && sxd <= 3)
                    {
                        Wake->DevWake =
                            (DEVICE_POWER_STATE)(PowerDeviceD0 + (ULONG)sxd);
                    }
                    else
                    {
                        Wake->DevWake = PowerDeviceD3;
                    }
                }
            }
        }
    }

    uacpi_object_unref(ret);
    return Wake->GpeValid;
}

BOOLEAN
UacpiWakeHasPrw(PUACPI_WAKE Wake)
{
    return UacpiWakeParsePrw(Wake);
}

// Switch the device's own wake circuit. PASSIVE_LEVEL.
//
// Vista and Win7 drive it with _PSW(Enable) only, matching the reference power
// service. Win8 and later prefer _DSW(Enable, TargetSystemState, TargetDevice)
// when the device declares it, so the firmware can tune the wake circuit for the
// exact sleep depth; _PSW is the fallback.
static VOID
UacpiWakeSetPsw(PUACPI_WAKE Wake, uacpi_u64 Enable)
{
    uacpi_object_array args;

    if (Wake->Node == NULL)
    {
        return;
    }

    /* Which of the two exists never changes, so resolve it once per device. */
    if (!Wake->PswParsed)
    {
        Wake->HasDsw = UacpiNodeHasChild(Wake->Node, "_DSW");
        Wake->HasPsw = Wake->HasDsw ? FALSE
                                    : UacpiNodeHasChild(Wake->Node, "_PSW");
        Wake->PswParsed = TRUE;
    }

    if (!Wake->HasDsw && !Wake->HasPsw)
    {
        return;
    }

#if (NTDDI_VERSION >= NTDDI_WIN8)
    if (Wake->HasDsw)
    {
        // Arg1 = target system state (sleep number), Arg2 = target device state.
        uacpi_u64 targetS = 0;
        uacpi_u64 targetD = 0;
        uacpi_object *argv[3];

        if (Enable && Wake->ReqSysState > PowerSystemWorking)
        {
            targetS = (uacpi_u64)(Wake->ReqSysState - PowerSystemWorking);
        }
        if (Enable && Wake->DevWake >= PowerDeviceD0 &&
            Wake->DevWake <= PowerDeviceD3)
        {
            targetD = (uacpi_u64)(Wake->DevWake - PowerDeviceD0);
        }

        argv[0] = uacpi_object_create_integer(Enable);
        argv[1] = uacpi_object_create_integer(targetS);
        argv[2] = uacpi_object_create_integer(targetD);
        if (argv[0] != NULL && argv[1] != NULL && argv[2] != NULL)
        {
            args.objects = argv;
            args.count   = 3;
            (void)uacpi_eval(Wake->Node, "_DSW", &args, NULL);
        }
        if (argv[0] != NULL) uacpi_object_unref(argv[0]);
        if (argv[1] != NULL) uacpi_object_unref(argv[1]);
        if (argv[2] != NULL) uacpi_object_unref(argv[2]);
        return;
    }
#endif
    {
        uacpi_object *arg = uacpi_object_create_integer(Enable);
        if (arg != NULL)
        {
            args.objects = &arg;
            args.count   = 1;
            (void)uacpi_eval(Wake->Node, "_PSW", &args, NULL);
            uacpi_object_unref(arg);
        }
    }
}

/*
 * The boot-time _PRW sweep that used to live here is gone. acpi.sys resolves
 * _PRW and _DSW/_PSW per device inside its build state machine, and marks a
 * GPE for wake only when the device actually gets a WAIT_WAKE. UacpiWakeArm
 * below is that path. Sweeping the whole namespace at bring-up instead ran AML
 * on every node with the SCI already live, which is what hung boot.
 */

// Disarm the wake GPE if armed. PASSIVE_LEVEL, Lock not held.
static VOID
UacpiWakeDisarm(PUACPI_WAKE Wake)
{
    if (Wake->Armed)
    {
        Wake->Armed = FALSE;
        // Drop the runtime enable first, then the wake mask, mirroring the arm
        // order in reverse so the refcount balances.
        (void)uacpi_disable_gpe(Wake->GpeDevice, Wake->GpeIdx);
        (void)uacpi_disable_gpe_for_wake(Wake->GpeDevice, Wake->GpeIdx);
        UacpiWakeSetPsw(Wake, 0);   // physically disable the device wake circuit
        UacpiPowerResReleaseWake(Wake->WakeRes, Wake->WakeResCount);
        Wake->WakeResCount = 0;
    }
}

// Deferred disarm for the cancel routine: uacpi_disable_gpe_for_wake takes a
// uACPI mutex and _PSW is AML, neither legal at the DISPATCH_LEVEL a cancel
// routine may run at. PASSIVE_LEVEL.
static VOID
NTAPI
UacpiWakeDisarmWorker(PVOID Context)
{
    PUACPI_WAKE Wake = (PUACPI_WAKE)Context;
    KIRQL   irql;
    BOOLEAN doIt = FALSE;

    InterlockedExchange(&Wake->DisarmQueued, 0);

    // Skip if a new WAIT_WAKE re-armed the device after the cancel.
    KeAcquireSpinLock(&Wake->Lock, &irql);
    if (Wake->WaitWakeIrp == NULL && Wake->Armed)
    {
        Wake->Armed = FALSE;
        doIt = TRUE;
    }
    KeReleaseSpinLock(&Wake->Lock, irql);

    if (doIt)
    {
        (void)uacpi_disable_gpe(Wake->GpeDevice, Wake->GpeIdx);
        (void)uacpi_disable_gpe_for_wake(Wake->GpeDevice, Wake->GpeIdx);
        UacpiWakeSetPsw(Wake, 0);
        UacpiPowerResReleaseWake(Wake->WakeRes, Wake->WakeResCount);
        Wake->WakeResCount = 0;
        UacpiTrace("[acpi] wake: %s deferred disarm done\n", Wake->Name);
    }
}

// Snapshot every wake block currently armed or depth-suspended, so the sleep and
// resume passes can run AML at PASSIVE without holding the child lock. Returns the
// count placed in 'snap'. The lifetime window matches the rest of the driver's
// list use: removal at PASSIVE during a power transition does not race here.
#define UACPI_WAKE_SNAP_MAX 64
static ULONG
UacpiWakeSnapshotActive(PUACPI_WAKE *snap, ULONG max)
{
    ULONG       n = 0;
    PLIST_ENTRY e;
    PUACPI_FDO  fdo = g_AcpiFdo;

    if (fdo == NULL)
    {
        return 0;
    }

    ExAcquireFastMutex(&fdo->ChildLock);
    for (e = fdo->Children.Flink; e != &fdo->Children && n < max; e = e->Flink)
    {
        PUACPI_PDO pdo = CONTAINING_RECORD(e, UACPI_PDO, Link);
        if ((pdo->Wake.Armed || pdo->Wake.DepthSuspended) && pdo->Wake.Node != NULL)
        {
            snap[n++] = &pdo->Wake;
        }
    }
    for (e = fdo->Filters.Flink; e != &fdo->Filters && n < max; e = e->Flink)
    {
        PUACPI_FILTER f = CONTAINING_RECORD(e, UACPI_FILTER, Link);
        if ((f->Wake.Armed || f->Wake.DepthSuspended) && f->Wake.Node != NULL)
        {
            snap[n++] = &f->Wake;
        }
    }
    ExReleaseFastMutex(&fdo->ChildLock);
    return n;
}

// Sleep: drop the wake mask on armed devices whose WAIT_WAKE asked for a state
// shallower than the one we are entering, so they do not wake from a depth they
// cannot service. Restore mirrors it on resume. Call between clear_all_events and
// enable_all_wake_gpes. PASSIVE_LEVEL.
VOID
UacpiWakeSuspendShallow(SYSTEM_POWER_STATE Target)
{
    PUACPI_WAKE snap[UACPI_WAKE_SNAP_MAX];
    ULONG       n, i;

    n = UacpiWakeSnapshotActive(snap, UACPI_WAKE_SNAP_MAX);
    for (i = 0; i < n; i++)
    {
        PUACPI_WAKE w = snap[i];
        if (w->Armed && !w->DepthSuspended &&
            w->ReqSysState != PowerSystemUnspecified && w->ReqSysState < Target)
        {
            (void)uacpi_disable_gpe_for_wake(w->GpeDevice, w->GpeIdx);
            w->DepthSuspended = TRUE;
            UacpiTrace("[acpi] wake: %s masked (wants S%u, entering S%u)\n",
                      w->Name, (ULONG)(w->ReqSysState - PowerSystemWorking),
                      (ULONG)(Target - PowerSystemWorking));
        }
    }
}

// Resume: restore the wake mask on devices masked by UacpiWakeSuspendShallow.
VOID
UacpiWakeRestoreSuspended(void)
{
    PUACPI_WAKE snap[UACPI_WAKE_SNAP_MAX];
    ULONG       n, i;

    n = UacpiWakeSnapshotActive(snap, UACPI_WAKE_SNAP_MAX);
    for (i = 0; i < n; i++)
    {
        PUACPI_WAKE w = snap[i];
        if (w->DepthSuspended)
        {
            (void)uacpi_enable_gpe_for_wake(w->GpeDevice, w->GpeIdx);
            w->DepthSuspended = FALSE;
        }
    }
}

// S4 resume: a hibernate cut all power, so every device still armed for wake
// needs its _PSW/_DSW circuit re-asserted (the wake rails come back via
// UacpiPowerResResume). Run this after UacpiWakeRestoreSuspended. PASSIVE_LEVEL.
VOID
UacpiWakeReArmAfterHibernate(void)
{
    PUACPI_WAKE snap[UACPI_WAKE_SNAP_MAX];
    ULONG       n, i;

    n = UacpiWakeSnapshotActive(snap, UACPI_WAKE_SNAP_MAX);
    for (i = 0; i < n; i++)
    {
        if (snap[i]->Armed)
        {
            UacpiWakeSetPsw(snap[i], 1);
            UacpiTrace("[acpi] wake: %s _PSW re-armed after hibernate\n",
                      snap[i]->Name);
        }
    }
}

// WAIT_WAKE cancel routine. May run at DISPATCH_LEVEL, so it completes the IRP
// inline and hands the AML/mutex disarm to a work item.
static VOID
NTAPI
UacpiWaitWakeCancel(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PUACPI_WAKE Wake = (PUACPI_WAKE)Irp->Tail.Overlay.DriverContext[0];
    KIRQL     irql;
    BOOLEAN   claimed = FALSE;

    UNREFERENCED_PARAMETER(DeviceObject);

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    KeAcquireSpinLock(&Wake->Lock, &irql);
    if (Wake->WaitWakeIrp == Irp)
    {
        Wake->WaitWakeIrp = NULL;   // Armed stays set; the worker disarms
        claimed = TRUE;
    }
    KeReleaseSpinLock(&Wake->Lock, irql);

    if (claimed)
    {
        // PoStartNextPowerIrp already ran on the way in. Completing at DISPATCH
        // is fine; the disarm is what must defer.
        Irp->IoStatus.Status = STATUS_CANCELLED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        if (InterlockedExchange(&Wake->DisarmQueued, 1) == 0)
        {
            ExInitializeWorkItem(&Wake->DisarmWork, UacpiWakeDisarmWorker, Wake);
#pragma warning(suppress: 4996)
            ExQueueWorkItem(&Wake->DisarmWork, DelayedWorkQueue);
        }
        UacpiTrace("[acpi] wake: %s WAIT_WAKE cancelled (disarm deferred)\n", Wake->Name);
    }
}

// Wake GPE fired (implicit Notify(node, 2)): complete the WAIT_WAKE. PASSIVE.
VOID
UacpiWakeComplete(PUACPI_WAKE Wake)
{
    KIRQL irql;
    PIRP  irp;

    KeAcquireSpinLock(&Wake->Lock, &irql);
    irp = Wake->WaitWakeIrp;
    if (irp != NULL && IoSetCancelRoutine(irp, NULL) != NULL)
    {
        // Cancel routine cleared first, so the IRP is ours. NULL: cancel owns it.
        Wake->WaitWakeIrp = NULL;
    }
    else
    {
        irp = NULL;
    }
    KeReleaseSpinLock(&Wake->Lock, irql);

    if (irp != NULL)
    {
        UacpiWakeDisarm(Wake);   // outside the lock, at PASSIVE
        UacpiTrace("[acpi] wake: %s completed WAIT_WAKE (device signalled wake)\n",
                  Wake->Name);
        // Flag the IRP as the system wake source only for a real resume, and
        // only for the first device to complete in the resume window; an S0
        // runtime device wake is not a system wake at all.
        if (UacpiSystemResuming != 0 &&
            InterlockedCompareExchange(&UacpiWakeSourceClaimed, 1, 0) == 0)
        {
#if !defined(__REACTOS__)
            // The ReactOS kernel does not track the wake source
            PoSetSystemWake(irp);
#endif
        }
        irp->IoStatus.Status = STATUS_SUCCESS;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

// Device removal: complete any pended WAIT_WAKE and drop the GPE arm so a
// vanished device does not leave its wake GPE enabled forever. PASSIVE_LEVEL.
VOID
UacpiWakeTeardown(PUACPI_WAKE Wake)
{
    KIRQL irql;
    PIRP  irp;

    KeAcquireSpinLock(&Wake->Lock, &irql);
    irp = Wake->WaitWakeIrp;
    if (irp != NULL && IoSetCancelRoutine(irp, NULL) != NULL)
    {
        Wake->WaitWakeIrp = NULL;
    }
    else
    {
        irp = NULL;   // cancel routine already owns it
    }
    KeReleaseSpinLock(&Wake->Lock, irql);

    if (irp != NULL)
    {
        irp->IoStatus.Status = STATUS_NO_SUCH_DEVICE;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }

    UacpiWakeDisarm(Wake);
}

// IRP_MN_WAIT_WAKE. PASSIVE_LEVEL; uACPI GPE calls run without Lock held.
NTSTATUS
UacpiWakeArm(PUACPI_WAKE Wake, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    SYSTEM_POWER_STATE reqS = sp->Parameters.WaitWake.PowerState;
    KIRQL        irql;
    uacpi_status st;
    BOOLEAN      busy;

    // No \_PRW => this device cannot wake the system.
    if (!UacpiWakeParsePrw(Wake))
    {
        return UacpiCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0);
    }

    // Reject a request to wake from a state deeper than _PRW[1] allows. Only
    // when we actually resolved the deepest state, so a parse miss never blocks.
    if (Wake->SysWake != PowerSystemUnspecified && reqS > Wake->SysWake)
    {
        UacpiTrace("[acpi] wake: %s WAIT_WAKE S%u exceeds _PRW wake S%u\n",
                  Wake->Name, (ULONG)(reqS - PowerSystemWorking),
                  (ULONG)(Wake->SysWake - PowerSystemWorking));
        return UacpiCompleteIrp(Irp, STATUS_INVALID_DEVICE_STATE, 0);
    }
    Wake->ReqSysState = reqS;

    // Reject a duplicate outstanding WAIT_WAKE before touching the GPE.
    KeAcquireSpinLock(&Wake->Lock, &irql);
    busy = (Wake->WaitWakeIrp != NULL);
    KeReleaseSpinLock(&Wake->Lock, irql);
    if (busy)
    {
        return UacpiCompleteIrp(Irp, STATUS_DEVICE_BUSY, 0);
    }

    // Set up the wake GPE once, then arm it; hardware enable happens at sleep.
    if (!Wake->SetupDone)
    {
        st = uacpi_setup_gpe_for_wake(Wake->GpeDevice, Wake->GpeIdx, Wake->Node);
        if (uacpi_unlikely_error(st))
        {
            UacpiTrace("[acpi] wake: %s setup_gpe_for_wake failed: %s\n",
                      Wake->Name, uacpi_status_to_string(st));
            return UacpiCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0);
        }
        Wake->SetupDone = TRUE;
    }

    // Drop any latched status so a stale event does not complete the wait the
    // instant it is armed.
    (void)uacpi_clear_gpe(Wake->GpeDevice, Wake->GpeIdx);

    st = uacpi_enable_gpe_for_wake(Wake->GpeDevice, Wake->GpeIdx);
    if (uacpi_unlikely_error(st))
    {
        UacpiTrace("[acpi] wake: %s enable_gpe_for_wake failed: %s\n",
                  Wake->Name, uacpi_status_to_string(st));
        return UacpiCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0);
    }

    // enable_gpe_for_wake only sets the wake mask consulted at sleep time; the
    // GPE also has to be enabled in the runtime register so it fires while the
    // system is still in S0 and so a latched status survives the next sleep.
    st = uacpi_enable_gpe(Wake->GpeDevice, Wake->GpeIdx);
    if (uacpi_unlikely_error(st))
    {
        (void)uacpi_disable_gpe_for_wake(Wake->GpeDevice, Wake->GpeIdx);
        UacpiTrace("[acpi] wake: %s enable_gpe failed: %s\n",
                  Wake->Name, uacpi_status_to_string(st));
        return UacpiCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0);
    }
    Wake->Armed = TRUE;
    // Power the _PRW[2..] wake rails before switching the device's own circuit on.
    Wake->WakeResCount = UacpiPowerResAcquireWake(Wake->Node, Wake->WakeRes,
                                                  UACPI_MAX_PR_PER_DEV);
    UacpiWakeSetPsw(Wake, 1);   // physically enable the device wake circuit

    // Pend the IRP with a cancel routine (handle an already-cancelled IRP).
    KeAcquireSpinLock(&Wake->Lock, &irql);
    IoSetCancelRoutine(Irp, UacpiWaitWakeCancel);
    if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL) != NULL)
    {
        KeReleaseSpinLock(&Wake->Lock, irql);
        UacpiWakeDisarm(Wake);   // outside the lock, at PASSIVE
        return UacpiCompleteIrp(Irp, STATUS_CANCELLED, 0);
    }
    Irp->Tail.Overlay.DriverContext[0] = Wake;
    IoMarkIrpPending(Irp);
    Wake->WaitWakeIrp = Irp;
    KeReleaseSpinLock(&Wake->Lock, irql);

    UacpiTrace("[acpi] wake: %s armed WAIT_WAKE on GPE 0x%02X\n",
              Wake->Name, Wake->GpeIdx);
    return STATUS_PENDING;
}

// IRP_MJ_POWER on a PDO (bottom of stack; always completes).
NTSTATUS
UacpiPdoSetPower(PUACPI_PDO Pdo, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_SUCCESS;

    PoStartNextPowerIrp(Irp);

    switch (sp->MinorFunction)
    {
    case IRP_MN_WAIT_WAKE:
        // Completes or pends the IRP itself.
        return UacpiWakeArm(&Pdo->Wake, Irp);

    case IRP_MN_SET_POWER:
        if (sp->Parameters.Power.Type == DevicePowerState)
        {
            DEVICE_POWER_STATE d = sp->Parameters.Power.State.DeviceState;
            status = UacpiPowerSetDeviceState(Pdo, d);
            PoSetPowerState(Pdo->Common.Self, DevicePowerState,
                            sp->Parameters.Power.State);
        }
        else
        {
            // System power IRP: acknowledge only.
            SYSTEM_POWER_STATE s = sp->Parameters.Power.State.SystemState;
            if (s < PowerSystemMaximum)
            {
                // No-op; the matching device IRP follows.
            }
            status = STATUS_SUCCESS;
        }
        break;

    case IRP_MN_QUERY_POWER:
        status = STATUS_SUCCESS;   // ACPI devices can always reach the queried state
        break;

    default:
        status = STATUS_SUCCESS;
        break;
    }

    return UacpiCompleteIrp(Irp, status, 0);
}
