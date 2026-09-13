/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PnP and power handling for ACPI-enumerated child PDOs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"

extern const GUID GUID_BUS_TYPE_ACPI;   // storage in guid.c (wdmguid.h)

// Driverless leaf devices need RawDeviceOK so PnP starts them.
static BOOLEAN
UacpiPdoRawDeviceOK(PUACPI_PDO Pdo)
{
    // A missing HID fails with Code 28; an extra one never gets its driver.
    static const char *const rawHids[] = {
        "ACPI0004",                                    // module container
        "PNP0A05", "PNP0A06",                          // generic ACPI containers
        "PNP0B00",                                     // RTC
        "PNP0C09",                                     // embedded controller
        "PNP0C0B",                                     // fan
        "PNP0C0C", "PNP0C0D", "PNP0C0E",               // power / lid / sleep buttons
        "PNP0C32",                                     // experience / app-launch button
        "PNP0D80", "TOS6200",                          // vendor leaves
    };
    ULONG i;

    // Button PDOs, node or node-less, start raw so SYS_BUTTON comes up.
    if (Pdo->ButtonCaps != 0 || Pdo->IsThermalZone)
    {
        return TRUE;
    }
    for (i = 0; i < RTL_NUMBER_OF(rawHids); i++)
    {
        if (_stricmp(Pdo->Hid, rawHids[i]) == 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}

// IRP_MN_QUERY_CAPABILITIES
static NTSTATUS
UacpiPdoQueryCaps(PUACPI_PDO Pdo, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_CAPABILITIES caps = sp->Parameters.DeviceCapabilities.Capabilities;
    ULONG i;

    if (caps == NULL || caps->Version < 1)
    {
        return STATUS_UNSUCCESSFUL;
    }

    // Motherboard devices: silent install, not removable or surprise-removable.
    caps->SilentInstall  = TRUE;
    caps->RawDeviceOK    = UacpiPdoRawDeviceOK(Pdo);
    caps->SurpriseRemovalOK = FALSE;
    caps->Removable      = FALSE;
    caps->UniqueID       = FALSE;

    UNREFERENCED_PARAMETER(i);

    // Device power mapping from _SxD / _PRW (power.c).
    UacpiPowerBuildStateMap(Pdo, caps);

    // Address from _ADR, UINumber from _SUN; _STA bit 2 clear hides the device.
    if (Pdo->HasAdr)
    {
        caps->Address = (ULONG)Pdo->Adr;
    }
    if (Pdo->Node != NULL)
    {
        uacpi_u64 sun = 0;
        uacpi_u32 sta = UACPI_STA_PRESENT | UACPI_STA_FUNCTIONING | 0x4;
        if (uacpi_likely_success(uacpi_eval_simple_integer(Pdo->Node, "_SUN", &sun)))
        {
            caps->UINumber = (ULONG)sun;
        }
        (void)uacpi_eval_sta(Pdo->Node, &sta);
        if (!(sta & 0x4))
        {
            caps->NoDisplayInUI = TRUE;
        }
        // Instance IDs are full namespace paths, so _UID makes them unique.
        caps->UniqueID = UacpiNodeHasChild(Pdo->Node, "_UID");
    }
    return STATUS_SUCCESS;
}

// IRP_MN_QUERY_DEVICE_RELATIONS (TargetDeviceRelation)
static NTSTATUS
UacpiPdoTargetRelations(PUACPI_PDO Pdo, PIRP Irp)
{
    PDEVICE_RELATIONS rel = (PDEVICE_RELATIONS)ExAllocatePoolWithTag(
        PagedPool, sizeof(DEVICE_RELATIONS), UACPI_POOL_TAG);
    if (rel == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    rel->Count = 1;
    ObReferenceObject(Pdo->Common.Self);
    rel->Objects[0] = Pdo->Common.Self;
    Irp->IoStatus.Information = (ULONG_PTR)rel;
    return STATUS_SUCCESS;
}

// PnP dispatch (PDO = bottom of stack; always completes)
NTSTATUS
UacpiPdoPnp(PUACPI_PDO Pdo, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = Irp->IoStatus.Status;   // default: leave unchanged

    switch (sp->MinorFunction)
    {
    case IRP_MN_START_DEVICE:
        Pdo->Started = TRUE;
        // Register the class interface this PDO owns (drvs/).
        UacpiDrvsStartDevice(Pdo);
        // Register the path with the resource hub for Connection() reparse.
        UacpiRegisterBiosNameForPdo(Pdo);
        status = STATUS_SUCCESS;
        break;

    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
        // Refuse while on the paging, hibernation, or crash-dump path.
        status = (Pdo->UsageCount > 0) ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
        break;

    case IRP_MN_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        status = STATUS_SUCCESS;
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        UacpiWakeTeardown(&Pdo->Wake);   // fail any pended WAIT_WAKE, disarm GPE
        UacpiDrvsRemoveDevice(Pdo);   // drop class interfaces + drain button IRPs
        status = STATUS_SUCCESS;
        break;

    case IRP_MN_REMOVE_DEVICE:
        // If the node is gone (not present) and unreported, tear the PDO down.
        UacpiWakeTeardown(&Pdo->Wake);
        UacpiDrvsRemoveDevice(Pdo);
        UacpiResArbiterTeardown(Pdo);   // free the PCI-root Memory/IO/Bus arbiters
        status = STATUS_SUCCESS;
        break;

    case IRP_MN_QUERY_ID:
        status = UacpiPdoQueryId(Pdo, Irp);
        break;

    case IRP_MN_QUERY_CAPABILITIES:
        status = UacpiPdoQueryCaps(Pdo, Irp);
        break;

    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (sp->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation)
        {
            status = UacpiPdoTargetRelations(Pdo, Irp);
        }
        else if (sp->Parameters.QueryDeviceRelations.Type == BusRelations)
        {
            // Merge ACPI child PDOs, then filter the _ADR matches (filter.c).
            UacpiBuildChildPdosForNode(Pdo->Parent, Pdo->Node);
            status = UacpiMergeChildRelations(Pdo->Parent, Pdo->Node, NULL, Irp);
            if (NT_SUCCESS(status))
            {
                (void)UacpiDetectFilterDevices(
                    Pdo->Parent, Pdo->Node,
                    (PDEVICE_RELATIONS)Irp->IoStatus.Information);
            }
        }
        break;

    case IRP_MN_QUERY_RESOURCES:
    {
        // Report _CRS as the device's boot and current resources.
        PCM_RESOURCE_LIST cm = NULL;
        NTSTATUS s = UacpiCrsToCmList(Pdo->Node, Pdo->Common.Self, &cm);
        if (NT_SUCCESS(s))
        {
            Irp->IoStatus.Information = (ULONG_PTR)cm;
            status = STATUS_SUCCESS;
        }
        else if (s == STATUS_NOT_FOUND)
        {
            Irp->IoStatus.Information = 0;   // device needs no resources
            status = STATUS_SUCCESS;
        }
        else
        {
            status = s;
        }
        break;
    }

    case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
    {
        PIO_RESOURCE_REQUIREMENTS_LIST req = NULL;
        // Requirements are _CRS only; PCI link _PRS/_SRS is handled in irqarb.c.
        NTSTATUS s = UacpiPrsToRequirements(Pdo->Node, FALSE, Pdo->Common.Self, &req);

        if (NT_SUCCESS(s))
        {
            Irp->IoStatus.Information = (ULONG_PTR)req;
            status = STATUS_SUCCESS;
        }
        else if (s == STATUS_NOT_FOUND)
        {
            Irp->IoStatus.Information = 0;
            status = STATUS_SUCCESS;
        }
        else
        {
            status = s;
        }
        break;
    }

    case IRP_MN_QUERY_PNP_DEVICE_STATE:
    {
        // PnP state bits from _STA. Node-less PDOs are always present.
        ULONG pnp = 0;
        if (Pdo->Node != NULL)
        {
            uacpi_u32 sta = UACPI_STA_PRESENT | UACPI_STA_FUNCTIONING;
            (void)uacpi_eval_sta(Pdo->Node, &sta);
            if (!(sta & UACPI_STA_PRESENT))
            {
                pnp |= PNP_DEVICE_DISABLED;             // 0x1
            }
            else if (!(sta & UACPI_STA_FUNCTIONING))
            {
                pnp |= PNP_DEVICE_FAILED;               // 0x8 (present but broken)
            }
            if (!(sta & 0x4))   // _STA bit 2 = "show in UI"
            {
                pnp |= PNP_DEVICE_DONT_DISPLAY_IN_UI;   // 0x2
            }
        }
        // Boot-essential paths (paging, hibernation, dump) cannot be disabled.
        if (Pdo->UsageCount > 0)
        {
            pnp |= PNP_DEVICE_NOT_DISABLEABLE;          // 0x20
        }
        Irp->IoStatus.Information = pnp;
        status = STATUS_SUCCESS;
        break;
    }

    case IRP_MN_QUERY_DEVICE_TEXT:
        // _STR (Unicode buffer) as DeviceTextDescription.
        if (sp->Parameters.QueryDeviceText.DeviceTextType == DeviceTextDescription &&
            Irp->IoStatus.Information == 0 && Pdo->Node != NULL)
            {
            uacpi_object *ret = NULL;
            if (uacpi_likely_success(uacpi_eval(Pdo->Node, "_STR", NULL, &ret)) &&
                ret != NULL)
                {
                uacpi_data_view buf;
                if (uacpi_object_get_type(ret) == UACPI_OBJECT_BUFFER &&
                    uacpi_likely_success(uacpi_object_get_buffer(ret, &buf)) &&
                    buf.length >= sizeof(WCHAR))
                    {
                    // Extra zeroed WCHAR: AML _STR may be unterminated.
                    SIZE_T bytes = buf.length + sizeof(WCHAR);
                    PWSTR  text  = (PWSTR)ExAllocatePoolWithTag(PagedPool, bytes,
                                                               UACPI_POOL_TAG);
                    if (text != NULL)
                    {
                        RtlZeroMemory(text, bytes);
                        RtlCopyMemory(text, buf.data, buf.length);
                        Irp->IoStatus.Information = (ULONG_PTR)text;
                        status = STATUS_SUCCESS;
                    }
                }
                uacpi_object_unref(ret);
            }
        }
        break;

    case IRP_MN_QUERY_INTERFACE:
        // ACPI_INTERFACE_STANDARD; other GUIDs leave the status unchanged.
        status = UacpiPdoQueryInterface(Pdo, Irp);
        break;

    case IRP_MN_DEVICE_USAGE_NOTIFICATION:
        // Count boot-critical path usage for PNP_DEVICE_NOT_DISABLEABLE.
        if (sp->Parameters.UsageNotification.InPath)
        {
            InterlockedIncrement(&Pdo->UsageCount);
        }
        else if (Pdo->UsageCount > 0)
        {
            InterlockedDecrement(&Pdo->UsageCount);
        }
        status = STATUS_SUCCESS;
        break;

    case IRP_MN_QUERY_BUS_INFORMATION:
    {
        // GUID_BUS_TYPE_ACPI, legacy bus type PNPBus.
        PPNP_BUS_INFORMATION bi = (PPNP_BUS_INFORMATION)ExAllocatePoolWithTag(
            PagedPool, sizeof(PNP_BUS_INFORMATION), UACPI_POOL_TAG);
        if (bi == NULL)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        bi->BusTypeGuid   = GUID_BUS_TYPE_ACPI;
        bi->LegacyBusType = PNPBus;
        bi->BusNumber     = 0;
        Irp->IoStatus.Information = (ULONG_PTR)bi;
        status = STATUS_SUCCESS;
        break;
    }

    case IRP_MN_EJECT:
        // Run _EJ0(1) if present.
        if (Pdo->Node != NULL)
        {
            uacpi_object *arg = uacpi_object_create_integer(1);
            if (arg != NULL)
            {
                uacpi_object_array a = { &arg, 1 };
                (void)uacpi_eval(Pdo->Node, "_EJ0", &a, NULL);
                uacpi_object_unref(arg);
            }
        }
        status = STATUS_SUCCESS;
        break;

    case IRP_MN_SET_LOCK:
        // _LCK(1/0): lock or unlock the device in its slot.
        if (Pdo->Node != NULL)
        {
            uacpi_object *arg =
                uacpi_object_create_integer(sp->Parameters.SetLock.Lock ? 1 : 0);
            if (arg != NULL)
            {
                uacpi_object_array a = { &arg, 1 };
                (void)uacpi_eval(Pdo->Node, "_LCK", &a, NULL);
                uacpi_object_unref(arg);
            }
        }
        status = STATUS_SUCCESS;
        break;

    default:
        break;   // leave status unchanged
    }

    return UacpiCompleteIrp(Irp, status, Irp->IoStatus.Information);
}

// Power (PDO = bottom of stack; always completes); handled in power.c.
NTSTATUS
UacpiPdoPower(PUACPI_PDO Pdo, PIRP Irp)
{
    return UacpiPdoSetPower(Pdo, Irp);
}
