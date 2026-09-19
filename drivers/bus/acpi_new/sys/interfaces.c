/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     QUERY_INTERFACE handlers for PDOs and filters
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"
#include <wdmguid.h>       // GUID_ACPI_INTERFACE_STANDARD (extern decl)
#include <uacpi/event.h>
#include <uacpi/tables.h>  // uacpi_table_fadt (ExpressWakeControl gate)
#include <uacpi/acpi.h>    // struct acpi_fadt

// Connected GPE vector; the opaque handle returned by GpeConnectVector.
typedef struct _ACPI_GPE_CONNECTION
{
    ULONG                Index;
    PGPE_SERVICE_ROUTINE Service;
    PVOID                ServiceContext;
    KDPC                 Dpc;          ///< runs Service at DISPATCH_LEVEL
    WORK_QUEUE_ITEM      FinishItem;   ///< re-enables the GPE at PASSIVE_LEVEL
    volatile LONG        Busy;         ///< fired and not yet re-enabled
} UACPI_GPE_CONNECTION, *PUACPI_GPE_CONNECTION;

// node -> PDO + Notify routing (used by glue.c's global Notify handler).
PUACPI_PDO
UacpiFindPdoByNode(PUACPI_FDO Fdo, uacpi_namespace_node *node)
{
    PLIST_ENTRY e;
    PUACPI_PDO found = NULL;

    if (Fdo == NULL)
    {
        return NULL;
    }
    ExAcquireFastMutex(&Fdo->ChildLock);
    for (e = Fdo->Children.Flink; e != &Fdo->Children; e = e->Flink)
    {
        PUACPI_PDO pdo = CONTAINING_RECORD(e, UACPI_PDO, Link);
        if (pdo->Node == node)
        {
            found = pdo;
            break;
        }
    }
    ExReleaseFastMutex(&Fdo->ChildLock);
    return found;
}

VOID
UacpiRouteNotify(uacpi_namespace_node *node, ULONG value)
{
    PUACPI_PDO pdo = UacpiFindPdoByNode(g_AcpiFdo, node);

    if (pdo == NULL)
    {
        // No PDO owns this node; it may be a foreign PDO we filter.
        PLIST_ENTRY e;
        PUACPI_FILTER filt = NULL;

        if (g_AcpiFdo == NULL)
        {
            return;
        }
        ExAcquireFastMutex(&g_AcpiFdo->ChildLock);
        for (e = g_AcpiFdo->Filters.Flink; e != &g_AcpiFdo->Filters; e = e->Flink)
        {
            PUACPI_FILTER f = CONTAINING_RECORD(e, UACPI_FILTER, Link);

            if (f->Node == node)
            {
                filt = f;
                break;
            }
        }
        ExReleaseFastMutex(&g_AcpiFdo->ChildLock);

        if (filt == NULL)
        {
            return;
        }

        // Notify 2 completes a filtered device's pended WAIT_WAKE, same as a PDO.
        if (value == 2 && filt->Wake.WaitWakeIrp != NULL)
        {
            UacpiWakeComplete(&filt->Wake);
        }
        if (filt->NotifyRoutine != NULL)
        {
            filt->NotifyRoutine(filt->NotifyContext, value);
        }
        return;
    }

    // Notify 2 (Device Wake) from an armed _PRW GPE completes the WAIT_WAKE.
    if (value == 2 && pdo->Wake.WaitWakeIrp != NULL)
    {
        UacpiWakeComplete(&pdo->Wake);
    }

    // Button Notify: 0x80 press or lid change, 0x02 wake. See drvs/button.c.
    if (pdo->ButtonCaps != 0 && (value == 0x80 || value == 0x02))
    {
        UacpiButtonNotify(pdo, value);
    }

    // Thermal zone Notify: 0x80 temperature, 0x81 trip points. See drvs/thermal.c.
    if (pdo->IsThermalZone && (value == 0x80 || value == 0x81))
    {
        UacpiThermalNotify(pdo, value);
    }

    if (pdo->NotifyRoutine != NULL)
    {
        pdo->NotifyRoutine(pdo->NotifyContext, value);
    }
}

// GPE bridge. The SCI ISR leaves the GPE disabled; the consumer's routine runs
// from a DPC as acpi.sys calls it, and the GPE is re-enabled at PASSIVE_LEVEL
// because uacpi_finish_handling_gpe takes uACPI's event mutex.
static VOID
NTAPI
UacpiGpeBridgeFinish(PVOID Context)
{
    PUACPI_GPE_CONNECTION conn = (PUACPI_GPE_CONNECTION)Context;

    (void)uacpi_finish_handling_gpe(uacpi_namespace_root(), (uacpi_u16)conn->Index);
    InterlockedExchange(&conn->Busy, 0);
}

_Function_class_(KDEFERRED_ROUTINE)
static VOID
NTAPI
UacpiGpeBridgeDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    PUACPI_GPE_CONNECTION conn = (PUACPI_GPE_CONNECTION)Context;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    if (conn->Service != NULL)
    {
        // (GpeVectorObject, ServiceContext), as acpi.sys's ACPIVectorConnect callers expect.
        conn->Service(conn, conn->ServiceContext);
    }
#pragma warning(suppress: 4996)
    ExQueueWorkItem(&conn->FinishItem, DelayedWorkQueue);
}

static uacpi_interrupt_ret
UacpiGpeBridge(uacpi_handle ctx, uacpi_namespace_node *gpe_device, uacpi_u16 idx)
{
    PUACPI_GPE_CONNECTION conn = (PUACPI_GPE_CONNECTION)ctx;

    UNREFERENCED_PARAMETER(gpe_device);
    UNREFERENCED_PARAMETER(idx);

    InterlockedExchange(&conn->Busy, 1);
    KeInsertQueueDpc(&conn->Dpc, NULL, NULL);
    return UACPI_INTERRUPT_HANDLED;   // no UACPI_GPE_REENABLE: the work item does it
}

static NTSTATUS __stdcall
UacpiIfGpeConnectVector(PDEVICE_OBJECT Device, ULONG GpeVector,
                       KINTERRUPT_MODE Mode, BOOLEAN Shareable,
                       PGPE_SERVICE_ROUTINE Service, PVOID ServiceCtx,
                       PVOID ObjectContext)
{
    PUACPI_GPE_CONNECTION conn;
    uacpi_status st;

    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Shareable);

    conn = (PUACPI_GPE_CONNECTION)ExAllocatePoolWithTag(NonPagedPool, sizeof(*conn),
                                                       UACPI_POOL_TAG);
    if (conn == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    conn->Index = GpeVector;
    conn->Service = Service;
    conn->ServiceContext = ServiceCtx;
    conn->Busy = 0;
    KeInitializeDpc(&conn->Dpc, UacpiGpeBridgeDpc, conn);
#pragma warning(suppress: 4996)
    ExInitializeWorkItem(&conn->FinishItem, UacpiGpeBridgeFinish, conn);

    st = uacpi_install_gpe_handler(
             uacpi_namespace_root(), (uacpi_u16)GpeVector,
             (Mode == Latched) ? UACPI_GPE_TRIGGERING_EDGE
                               : UACPI_GPE_TRIGGERING_LEVEL,
             UacpiGpeBridge, conn);
    if (uacpi_unlikely_error(st))
    {
        ExFreePoolWithTag(conn, UACPI_POOL_TAG);
        return STATUS_UNSUCCESSFUL;
    }
    // ObjectContext is the OUT slot for the connection handle.
    if (ObjectContext != NULL)
    {
        *(PVOID *)ObjectContext = conn;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS __stdcall
UacpiIfGpeDisconnectVector(PVOID Connection)
{
    PUACPI_GPE_CONNECTION conn = (PUACPI_GPE_CONNECTION)Connection;
    if (conn == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    (void)uacpi_uninstall_gpe_handler(uacpi_namespace_root(),
                                      (uacpi_u16)conn->Index, UacpiGpeBridge);

    // Drain a GPE already fired: its DPC, then its PASSIVE re-enable.
    KeFlushQueuedDpcs();
    while (InterlockedCompareExchange(&conn->Busy, 0, 0) != 0)
    {
        LARGE_INTEGER delay;

        delay.QuadPart = -10000;   // 1 ms
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    ExFreePoolWithTag(conn, UACPI_POOL_TAG);
    return STATUS_SUCCESS;
}

static NTSTATUS __stdcall
UacpiIfGpeEnableEvent(PDEVICE_OBJECT Device, PVOID Connection)
{
    PUACPI_GPE_CONNECTION conn = (PUACPI_GPE_CONNECTION)Connection;
    UNREFERENCED_PARAMETER(Device);
    if (conn == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    return uacpi_likely_success(
               uacpi_enable_gpe(uacpi_namespace_root(), (uacpi_u16)conn->Index))
           ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static NTSTATUS __stdcall
UacpiIfGpeDisableEvent(PDEVICE_OBJECT Device, PVOID Connection)
{
    PUACPI_GPE_CONNECTION conn = (PUACPI_GPE_CONNECTION)Connection;
    UNREFERENCED_PARAMETER(Device);
    if (conn == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    return uacpi_likely_success(
               uacpi_disable_gpe(uacpi_namespace_root(), (uacpi_u16)conn->Index))
           ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static NTSTATUS __stdcall
UacpiIfGpeClearStatus(PDEVICE_OBJECT Device, PVOID Connection)
{
    PUACPI_GPE_CONNECTION conn = (PUACPI_GPE_CONNECTION)Connection;
    UNREFERENCED_PARAMETER(Device);
    if (conn == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    (void)uacpi_clear_gpe(uacpi_namespace_root(), (uacpi_u16)conn->Index);
    return STATUS_SUCCESS;
}

// Notify registration
static NTSTATUS __stdcall
UacpiIfRegisterNotify(PDEVICE_OBJECT Device, PDEVICE_NOTIFY_CALLBACK Cb, PVOID Ctx)
{
    PUACPI_COMMON common = (PUACPI_COMMON)Device->DeviceExtension;

    // Filter devnodes can register for Notify too.
    if (common->Type == UacpiExtFilter)
    {
        ((PUACPI_FILTER)common)->NotifyRoutine = Cb;
        ((PUACPI_FILTER)common)->NotifyContext = Ctx;
        return STATUS_SUCCESS;
    }
    if (common->Type != UacpiExtPdo)
    {
        return STATUS_INVALID_PARAMETER;
    }
    ((PUACPI_PDO)common)->NotifyRoutine = Cb;
    ((PUACPI_PDO)common)->NotifyContext = Ctx;
    return STATUS_SUCCESS;
}

static VOID __stdcall
UacpiIfUnregisterNotify(PDEVICE_OBJECT Device, PDEVICE_NOTIFY_CALLBACK Cb)
{
    PUACPI_COMMON common = (PUACPI_COMMON)Device->DeviceExtension;
    UNREFERENCED_PARAMETER(Cb);
    if (common->Type == UacpiExtPdo)
    {
        ((PUACPI_PDO)common)->NotifyRoutine = NULL;
        ((PUACPI_PDO)common)->NotifyContext = NULL;
    }
    else if (common->Type == UacpiExtFilter)
    {
        ((PUACPI_FILTER)common)->NotifyRoutine = NULL;
        ((PUACPI_FILTER)common)->NotifyContext = NULL;
    }
}

// v2 variants whose argument count differs from v1.
static NTSTATUS __stdcall
UacpiIfGpeDisconnectVector2(PVOID Context, PVOID ObjectContext)
{
    UNREFERENCED_PARAMETER(Context);
    return UacpiIfGpeDisconnectVector(ObjectContext);
}

static VOID __stdcall
UacpiIfUnregisterNotify2(PVOID Context)
{
    PUACPI_COMMON common = (PUACPI_COMMON)((PDEVICE_OBJECT)Context)->DeviceExtension;
    if (common->Type == UacpiExtPdo)
    {
        ((PUACPI_PDO)common)->NotifyRoutine = NULL;
        ((PUACPI_PDO)common)->NotifyContext = NULL;
    }
    else if (common->Type == UacpiExtFilter)
    {
        ((PUACPI_FILTER)common)->NotifyRoutine = NULL;
        ((PUACPI_FILTER)common)->NotifyContext = NULL;
    }
}

static VOID __stdcall UacpiIfRef(PVOID c)   { UNREFERENCED_PARAMETER(c); }
static VOID __stdcall UacpiIfDeref(PVOID c) { UNREFERENCED_PARAMETER(c); }

// Interrupt (_PRT) translator: GSIV to vector, IRQL and affinity.
#ifndef STATUS_TRANSLATION_COMPLETE
#define STATUS_TRANSLATION_COMPLETE ((NTSTATUS)0x00000120L)
#endif

extern const GUID GUID_TRANSLATOR_INTERFACE_STANDARD;   // wdmguid.h

static NTSTATUS __stdcall
UacpiIrqTranslateResources(PVOID Context, PCM_PARTIAL_RESOURCE_DESCRIPTOR Source,
                          RESOURCE_TRANSLATION_DIRECTION Direction,
                          ULONG AlternativesCount,
                          IO_RESOURCE_DESCRIPTOR Alternatives[],
                          PDEVICE_OBJECT PhysicalDeviceObject,
                          PCM_PARTIAL_RESOURCE_DESCRIPTOR Target)
{
    ULONG gsiv, vector = 0, polarity = 0, mode = 0;
    KIRQL irql = 0;
    KAFFINITY affinity = 0;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(AlternativesCount);
    UNREFERENCED_PARAMETER(Alternatives);

    *Target = *Source;   // start from an identity copy

    /*
     * acpi.sys answers the reverse direction with STATUS_NOT_SUPPORTED, and
     * IopTranslateAssignmentToDevice fails the device on anything but success.
     * Whether that path is reachable at all depends on how the requirement
     * levels are chained, so the identity copy stands until that is known.
     */
    if (Direction != TranslateChildToParent)
    {
        return STATUS_SUCCESS;
    }

    if (Source->Type != CmResourceTypeInterrupt)
    {
        return STATUS_SUCCESS;
    }

#ifndef CM_RESOURCE_INTERRUPT_MESSAGE
#define CM_RESOURCE_INTERRUPT_MESSAGE 0x0002
#endif

    // Trace every interrupt translation.
    UacpiTrace("[acpi] irqTrans: PDO %p class 0x%04X translate INTERRUPT gsiv/vec %u "
              "flags 0x%X share %u%s\n",
              PhysicalDeviceObject, UacpiReadPciClassCode(PhysicalDeviceObject),
              Source->u.Interrupt.Vector, Source->Flags, Source->ShareDisposition,
              (Source->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) ? " [MSG/MSI]" : " [LINE]");

    // Vector holds the GSIV; irqlib.c maps it and writes the connection data.
    gsiv = Source->u.Interrupt.Vector;

    // MSI/MSI-X: count is Level's high USHORT; emit the translated form.
#ifndef CM_RESOURCE_INTERRUPT_MESSAGE
#define CM_RESOURCE_INTERRUPT_MESSAGE 0x0002
#endif
    if ((Source->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) || gsiv >= 0xFFF00000)
    {
        ULONG count = (ULONG)(Source->u.Interrupt.Level >> 16);
        ULONG base = 0;
        KIRQL mirql = 0;
        KAFFINITY maff = 0;

        if (count == 0 || count > 32)
        {
            count = 1;
        }
        if (!NT_SUCCESS(UacpiIrqLibResolveMessageVector(PhysicalDeviceObject, gsiv,
                                                       count, &base, &mirql, &maff))
            || mirql == PASSIVE_LEVEL)
            {
            /*
             * No vector has been handed to this device yet. acpi.sys leaves the
             * descriptor alone and reports success here, because failing the
             * translation fails the whole resource list and the device never
             * starts. The message slot stays in Vector for whoever assigns it.
             */
            UacpiTrace("[acpi] irqTrans: msg-gsiv 0x%X x%u has no vector yet\n",
                      gsiv, count);
            return STATUS_SUCCESS;
        }
        Target->u.Interrupt.Level    = mirql;      // Translated.Level  = IRQL
        Target->u.Interrupt.Vector   = base;       // Translated.Vector = base IDT entry
        Target->u.Interrupt.Affinity = maff;       // Translated.Affinity
        if (PhysicalDeviceObject != NULL)
        {
            (VOID)UacpiIrqLibWriteConnectionData(PhysicalDeviceObject, gsiv, count);
        }
        return STATUS_TRANSLATION_COMPLETE;
    }

    // The descriptor's LATCHED bit sets the GSIV's IOAPIC trigger and polarity.
    if (!(Source->Flags & CM_RESOURCE_INTERRUPT_LATCHED))
    {
        UacpiIrqLibNoteLevelGsiv(gsiv);   // level-sensitive -> level/active-low
    }

    if (!NT_SUCCESS(UacpiIrqLibResolveVector(gsiv, &vector, &irql, &affinity,
                                            &polarity, &mode))
        || irql == PASSIVE_LEVEL)
        {
        // As above: leave the descriptor alone rather than fail the device.
        UacpiTrace("[acpi] irqTrans: gsiv %u has no vector yet\n", gsiv);
        return STATUS_SUCCESS;
    }

    Target->u.Interrupt.Level    = irql;
    Target->u.Interrupt.Vector   = vector;
    Target->u.Interrupt.Affinity = affinity;
    if (PhysicalDeviceObject != NULL)
    {
        (VOID)UacpiIrqLibWriteConnectionData(PhysicalDeviceObject, gsiv, 1);
    }
    return STATUS_TRANSLATION_COMPLETE;   // fully translated at the ACPI root
}

static NTSTATUS __stdcall
UacpiIrqTranslateRequirements(PVOID Context, PIO_RESOURCE_DESCRIPTOR Source,
                             PDEVICE_OBJECT PhysicalDeviceObject,
                             PULONG TargetCount, PIO_RESOURCE_DESCRIPTOR *Target)
{
    PIO_RESOURCE_DESCRIPTOR out;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    // Pass through; the arbiter works in GSIV space.
    out = (PIO_RESOURCE_DESCRIPTOR)ExAllocatePoolWithTag(PagedPool, sizeof(*out),
                                                         UACPI_POOL_TAG);
    if (out == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *out = *Source;
    *TargetCount = 1;
    *Target = out;
    return STATUS_TRANSLATION_COMPLETE;
}

// Translator callbacks ignore Context, so PDOs and filters share this.
NTSTATUS
UacpiBuildIrqTranslator(const char *TraceName, PIO_STACK_LOCATION sp)
{
    PTRANSLATOR_INTERFACE ti = (PTRANSLATOR_INTERFACE)sp->Parameters.QueryInterface.Interface;

    if (sp->Parameters.QueryInterface.Size < sizeof(TRANSLATOR_INTERFACE))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    ti->Size    = sizeof(TRANSLATOR_INTERFACE);
    ti->Version = 1;
    ti->Context = NULL;   // callbacks ignore Context (the resource carries the PDO)
    ti->InterfaceReference   = (PINTERFACE_REFERENCE)UacpiIfRef;
    ti->InterfaceDereference = (PINTERFACE_DEREFERENCE)UacpiIfDeref;
    ti->TranslateResources             = UacpiIrqTranslateResources;
    ti->TranslateResourceRequirements  = UacpiIrqTranslateRequirements;
    UacpiTrace("[acpi] provided interrupt TRANSLATOR to %s\n", TraceName);
    return STATUS_SUCCESS;
}

// PCI_BUS_INTERFACE_STANDARD(2) for PCI roots; pci.sys needs it at AddDevice.

// Log PCI slots that stop answering vendor-ID reads. DISPATCH-safe.
static UCHAR UacpiPciSlotSeenPresent[8][256 / 8];   // [bus][packed slot bit]

static ULONG __stdcall
UacpiIfPciReadConfigLogPresence(ULONG Bus, ULONG Slot, PVOID Buffer,
                               ULONG Offset, ULONG Length, ULONG Returned)
{
    USHORT vendor;
    UCHAR bit;

    if (Returned < Length && Length > 2)
    {
        DbgPrint("[acpi] pcicfg SHORT READ bus %u slot %u.%u off 0x%x len %u -> %u\n",
                 Bus, Slot & 0x1F, (Slot >> 5) & 0x7, Offset, Length, Returned);
    }

    // Presence tracking keys off vendor-ID reads only.
    if (Offset != 0 || Length < 2 || Bus >= 8 || Slot >= 256)
    {
        return Returned;
    }

    vendor = *(USHORT UNALIGNED *)Buffer;
    bit = (UCHAR)(1 << (Slot & 7));

    if (Returned >= 2 && vendor != 0xFFFF && vendor != 0)
    {
        UacpiPciSlotSeenPresent[Bus][Slot >> 3] |= bit;
    }
    else if (UacpiPciSlotSeenPresent[Bus][Slot >> 3] & bit)
    {
        DbgPrint("[acpi] pcicfg DEVICE VANISHED bus %u slot %u.%u: "
                 "vendor read -> ret %u data 0x%04x (was present)\n",
                 Bus, Slot & 0x1F, (Slot >> 5) & 0x7, Returned, vendor);
        UacpiPciSlotSeenPresent[Bus][Slot >> 3] &= ~bit;
    }

    return Returned;
}

// Raw config cycles of Length bytes; pci.sys bugchecks on a short access.
static ULONG __stdcall
UacpiIfPciReadConfig(PVOID Context, ULONG Bus, ULONG Slot, PVOID Buffer,
                    ULONG Offset, ULONG Length)
{
    ULONG ret = UacpiHalPciReadConfig(Context, Bus, Slot, Buffer, Offset, Length);
    return UacpiIfPciReadConfigLogPresence(Bus, Slot, Buffer, Offset, Length, ret);
}

static ULONG __stdcall
UacpiIfPciWriteConfig(PVOID Context, ULONG Bus, ULONG Slot, PVOID Buffer,
                     ULONG Offset, ULONG Length)
{
    return UacpiHalPciWriteConfig(Context, Bus, Slot, Buffer, Offset, Length);
}

// Interrupt Line fixups are no-ops; _PRT owns interrupt routing.
static VOID __stdcall
UacpiIfPciPinToLine(PVOID Context, PPCI_COMMON_CONFIG PciData)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PciData);
}

static VOID __stdcall
UacpiIfPciLineToPin(PVOID Context, PPCI_COMMON_CONFIG PciNewData,
                   PPCI_COMMON_CONFIG PciOldData)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PciNewData);
    UNREFERENCED_PARAMETER(PciOldData);
}

// PCI root _OSC: a query pass, then a commit pass with the query result.
#define UACPI_PCI_OSC_SUPPORT      0x0000001Fu
#define UACPI_PCI_OSC_CONTROL_REQ  0x0000001Du

extern const GUID PCI_ROOT_BUS_OSC_UUID;        // guid.c
extern const GUID PCI_ROOT_BUS_DSM_UUID;        // guid.c

extern const GUID SB_OSC_UUID;                  // guid.c

// Platform-wide \_SB._OSC feature declaration. No-op if absent.
VOID
UacpiPlatformOscNegotiate(void)
{
    uacpi_namespace_node *sb =
        uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_SB);
    ULONG caps[2];
    uacpi_object *argv[4] = { 0 };
    uacpi_object *ret = NULL;
    uacpi_object_array args;
    uacpi_data_view view;
    ULONG i;

    if (sb == NULL)
    {
        return;
    }
    caps[0] = 0;         // status DWORD in
    caps[1] = 0xB75u; // OS feature support bits

    view.bytes  = (uacpi_u8 *)&SB_OSC_UUID;
    view.length = sizeof(GUID);
    argv[0] = uacpi_object_create_buffer(view);
    argv[1] = uacpi_object_create_integer(1);   // revision 1 (platform-wide _OSC)
    argv[2] = uacpi_object_create_integer(2);   // 2 capability DWORDs
    view.bytes  = (uacpi_u8 *)caps;
    view.length = sizeof(caps);
    argv[3] = uacpi_object_create_buffer(view);
    for (i = 0; i < 4; i++)
    {
        if (argv[i] == NULL)
        {
            goto out;
        }
    }
    args.objects = argv;
    args.count   = 4;
    // Log a firmware rejection (DWORD0 error bits 1..3).
    ret = NULL;
    if (uacpi_likely_success(uacpi_eval(sb, "_OSC", &args, &ret)) && ret != NULL &&
        uacpi_object_get_type(ret) == UACPI_OBJECT_BUFFER)
        {
        uacpi_data_view out;
        if (uacpi_likely_success(uacpi_object_get_buffer(ret, &out)) &&
            out.length >= sizeof(caps))
            {
            ULONG st = ((const ULONG *)out.bytes)[0];
            UacpiTrace("[acpi] platform \\_SB._OSC: support 0x%X, status 0x%X%s\n",
                      caps[1], st, (st & 0x0Eu) ? " (firmware rejected)" : " OK");
        }
    }
out:
    for (i = 0; i < 4; i++)
    {
        if (argv[i] != NULL)
        {
            uacpi_object_unref(argv[i]);
        }
    }
    if (ret != NULL)
    {
        uacpi_object_unref(ret);
    }
}

static NTSTATUS
UacpipPciRootOscRun(PUACPI_PDO Pdo, BOOLEAN Query, PULONG ControlInOut)
{
    ULONG caps[3];
    uacpi_object *argv[4] = { 0 };
    uacpi_object_array args;
    uacpi_object *ret = NULL;
    uacpi_data_view view;
    uacpi_status ust;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    ULONG i;

    caps[0] = Query ? 1u : 0u;                  // bit0 of DWORD0 = query flag
    caps[1] = UACPI_PCI_OSC_SUPPORT;
    caps[2] = *ControlInOut;

    view.bytes  = (uacpi_u8 *)&PCI_ROOT_BUS_OSC_UUID;
    view.length = sizeof(GUID);
    argv[0] = uacpi_object_create_buffer(view);
    argv[1] = uacpi_object_create_integer(1);   // revision
    argv[2] = uacpi_object_create_integer(3);   // DWORD count
    view.bytes  = (uacpi_u8 *)caps;
    view.length = sizeof(caps);
    argv[3] = uacpi_object_create_buffer(view);
    for (i = 0; i < 4; i++)
    {
        if (argv[i] == NULL)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto out;
        }
    }

    args.objects = argv;
    args.count   = 4;
    ust = uacpi_eval(Pdo->Node, "_OSC", &args, &ret);
    if (uacpi_unlikely_error(ust))
    {
        status = (ust == UACPI_STATUS_NOT_FOUND) ? STATUS_NOT_IMPLEMENTED
                                                 : STATUS_UNSUCCESSFUL;
        goto out;
    }
    if (ret != NULL &&
        uacpi_object_get_type(ret) == UACPI_OBJECT_BUFFER &&
        uacpi_likely_success(uacpi_object_get_buffer(ret, &view)) &&
        view.length >= sizeof(caps))
        {
        RtlCopyMemory(caps, view.bytes, sizeof(caps));
        // DWORD0 error bits (1..3) mean the platform rejected the request.
        if ((caps[0] & 0x0Eu) == 0)
        {
            *ControlInOut = caps[2];
            status = STATUS_SUCCESS;
        }
    }

out:
    for (i = 0; i < 4; i++)
    {
        if (argv[i] != NULL)
        {
            uacpi_object_unref(argv[i]);
        }
    }
    if (ret != NULL)
    {
        uacpi_object_unref(ret);
    }
    return status;
}

static VOID
UacpipPciRootOscNegotiate(PUACPI_PDO Pdo)
{
    ULONG control;
    NTSTATUS status;

    if (Pdo->PciRoot.OscEvaluated)
    {
        return;
    }
    Pdo->PciRoot.OscEvaluated = TRUE;
    Pdo->PciRoot.OscControlGranted = 0;

    // Query pass: ask for the full control set.
    control = UACPI_PCI_OSC_CONTROL_REQ;
    status = UacpipPciRootOscRun(Pdo, TRUE, &control);
    if (!NT_SUCCESS(status))
    {
        if (status != STATUS_NOT_IMPLEMENTED)
        {
            UacpiTrace("[acpi] %s: _OSC query failed 0x%X\n", Pdo->Name, status);
        }
        return;                                 // no _OSC: nothing granted
    }

    // Commit pass; a query result beyond the request is dropped to 0.
    if ((control | UACPI_PCI_OSC_CONTROL_REQ) != UACPI_PCI_OSC_CONTROL_REQ)
    {
        control = 0;
    }
    status = UacpipPciRootOscRun(Pdo, FALSE, &control);
    if (NT_SUCCESS(status))
    {
        Pdo->PciRoot.OscControlGranted = control;
    }
    UacpiTrace("[acpi] %s: _OSC granted 0x%02X\n", Pdo->Name,
              Pdo->PciRoot.OscControlGranted);
}

// PCI root _DSM, revision 1: function 4 returns the bus capability record.
static NTSTATUS
UacpipPciRootDsmEval(PUACPI_PDO Pdo, ULONG Function, uacpi_object **Ret)
{
    uacpi_object *argv[4] = { 0 };
    uacpi_object_array args, empty = { 0 };
    uacpi_data_view view;
    uacpi_status ust;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    ULONG i;

    view.bytes  = (uacpi_u8 *)&PCI_ROOT_BUS_DSM_UUID;
    view.length = sizeof(GUID);
    argv[0] = uacpi_object_create_buffer(view);
    argv[1] = uacpi_object_create_integer(1);         // revision
    argv[2] = uacpi_object_create_integer(Function);
    argv[3] = uacpi_object_create_package(empty);     // no fn-specific args
    for (i = 0; i < 4; i++)
    {
        if (argv[i] == NULL)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto out;
        }
    }

    args.objects = argv;
    args.count   = 4;
    ust = uacpi_eval(Pdo->Node, "_DSM", &args, Ret);
    status = uacpi_likely_success(ust)
                 ? STATUS_SUCCESS
                 : (ust == UACPI_STATUS_NOT_FOUND) ? STATUS_NOT_IMPLEMENTED
                                                   : STATUS_UNSUCCESSFUL;
out:
    for (i = 0; i < 4; i++)
    {
        if (argv[i] != NULL)
        {
            uacpi_object_unref(argv[i]);
        }
    }
    return status;
}

static VOID
UacpipPciRootDsmEvaluate(PUACPI_PDO Pdo)
{
    uacpi_object *ret = NULL;
    uacpi_data_view view;
    NTSTATUS status;

    if (Pdo->PciRoot.DsmEvaluated)
    {
        return;
    }
    Pdo->PciRoot.DsmEvaluated = TRUE;
    Pdo->PciRoot.BusCapsFound = FALSE;

    // Function 0: supported-functions bitmap (buffer); need bit 4.
    status = UacpipPciRootDsmEval(Pdo, 0, &ret);
    if (!NT_SUCCESS(status) || ret == NULL ||
        uacpi_object_get_type(ret) != UACPI_OBJECT_BUFFER ||
        uacpi_unlikely_error(uacpi_object_get_buffer(ret, &view)) ||
        view.length == 0 || (view.bytes[0] & 0x10) == 0)
        {
        if (ret != NULL)
        {
            uacpi_object_unref(ret);
        }
        return;
    }
    uacpi_object_unref(ret);
    ret = NULL;

    // Function 4: Package with the record buffer; malformed data is ignored.
    status = UacpipPciRootDsmEval(Pdo, 4, &ret);
    if (NT_SUCCESS(status) && ret != NULL &&
        uacpi_object_get_type(ret) == UACPI_OBJECT_PACKAGE)
        {
        uacpi_object_array pkg;
        uacpi_size i;
        if (uacpi_likely_success(uacpi_object_get_package(ret, &pkg)))
        {
            for (i = 0; i < pkg.count; i++)
            {
                if (uacpi_object_get_type(pkg.objects[i]) != UACPI_OBJECT_BUFFER ||
                    uacpi_unlikely_error(
                        uacpi_object_get_buffer(pkg.objects[i], &view)))
                        {
                    continue;
                }
                if (view.length >= 0x18 &&
                    *(uacpi_u16 *)view.bytes == 1 &&              // Type
                    *(uacpi_u16 *)(view.bytes + 2) == 0 &&        // Reserved
                    *(uacpi_u32 *)(view.bytes + 4) >= 0x18 &&     // Length
                    *(uacpi_u16 *)(view.bytes + 8) == 1 &&        // CapId
                    *(uacpi_u16 *)(view.bytes + 10) >= 0x10) {    // CapLen
                    Pdo->PciRoot.CurrentSpeedAndMode =
                        *(uacpi_u32 *)(view.bytes + 12);
                    Pdo->PciRoot.SupportedSpeedsAndModes =
                        *(uacpi_u32 *)(view.bytes + 16);
                    Pdo->PciRoot.BusCapAttributes =
                        *(uacpi_u32 *)(view.bytes + 20);
                    Pdo->PciRoot.BusCapsFound = TRUE;
                    UacpiTrace("[acpi] %s: _DSM fn4 caps cur=%u sup=0x%X attr=0x%X\n",
                              Pdo->Name, Pdo->PciRoot.CurrentSpeedAndMode,
                              Pdo->PciRoot.SupportedSpeedsAndModes,
                              Pdo->PciRoot.BusCapAttributes);
                    break;
                }
            }
        }
    }
    if (ret != NULL)
    {
        uacpi_object_unref(ret);
    }
}

// pci.sys's copy ends at OscControlGranted on every release (Vista, Win8, Win10);
// WDK 10.0.28000 appends Cxl* members, so never write past it.
#define UACPI_PCI_ROOT_BUS_HW_CAP_LENGTH \
    RTL_SIZEOF_THROUGH_FIELD(PCI_ROOT_BUS_HARDWARE_CAPABILITY, OscControlGranted)
C_ASSERT(UACPI_PCI_ROOT_BUS_HW_CAP_LENGTH == 0x24);

// Called by pci.sys at PASSIVE_LEVEL, so the lazy AML evaluation is safe.
static VOID __stdcall
UacpiIfPciRootBusCapability(PVOID Context, PPCI_ROOT_BUS_HARDWARE_CAPABILITY Cap)
{
    // Context is the UACPI_PDO, not a DEVICE_OBJECT.
    PUACPI_PDO pdo = (PUACPI_PDO)Context;

    RtlZeroMemory(Cap, UACPI_PCI_ROOT_BUS_HW_CAP_LENGTH);

    UacpipPciRootOscNegotiate(pdo);
    UacpipPciRootDsmEvaluate(pdo);

    Cap->OscFeatureSupport.u.AsULONG = UACPI_PCI_OSC_SUPPORT;
    Cap->OscControlRequest.u.AsULONG = UACPI_PCI_OSC_CONTROL_REQ;
    Cap->OscControlGranted.u.AsULONG = pdo->PciRoot.OscControlGranted;

    Cap->SecondaryInterface = pdo->PciRoot.IsExpress ? PciExpress
                                                     : PciConventional;
    if (Cap->SecondaryInterface != PciExpress && pdo->PciRoot.BusCapsFound)
    {
        Cap->BusCapabilitiesFound = TRUE;
        Cap->CurrentSpeedAndMode = pdo->PciRoot.CurrentSpeedAndMode;
        Cap->SupportedSpeedsAndModes = pdo->PciRoot.SupportedSpeedsAndModes;
        if (pdo->PciRoot.BusCapAttributes & 0x4)
        {
            Cap->DeviceIDMessagingCapable = TRUE;
        }
        if (pdo->PciRoot.BusCapAttributes & 0x1)
        {
            Cap->SecondaryBusWidth = BusWidth64Bits;
        }
    }
}

// ExpressWakeControl: EnableWake clears PM1_EN PCIEXP_WAKE_DIS (bit 14).
static KSPIN_LOCK UacpiPm1EnLock;
static USHORT     UacpiPm1aEnPort;   // pm1?_evt_blk + pm1_evt_len/2 (0 = absent)
static USHORT     UacpiPm1bEnPort;

static VOID __stdcall
UacpiIfPciExpressWakeControl(PVOID Context, BOOLEAN EnableWake)
{
    KIRQL irql;
    USHORT en;

    UNREFERENCED_PARAMETER(Context);

    KeAcquireSpinLock(&UacpiPm1EnLock, &irql);
    if (UacpiPm1aEnPort != 0)
    {
        en = READ_PORT_USHORT((PUSHORT)(ULONG_PTR)UacpiPm1aEnPort);
        en = EnableWake ? (USHORT)(en & 0xBFFFu) : (USHORT)(en | 0x4000u);
        WRITE_PORT_USHORT((PUSHORT)(ULONG_PTR)UacpiPm1aEnPort, en);
    }
    if (UacpiPm1bEnPort != 0)
    {
        en = READ_PORT_USHORT((PUSHORT)(ULONG_PTR)UacpiPm1bEnPort);
        en = EnableWake ? (USHORT)(en & 0xBFFFu) : (USHORT)(en | 0x4000u);
        WRITE_PORT_USHORT((PUSHORT)(ULONG_PTR)UacpiPm1bEnPort, en);
    }
    KeReleaseSpinLock(&UacpiPm1EnLock, irql);
}

static NTSTATUS
UacpiBuildPciBusInterface(PUACPI_PDO Pdo, PIO_STACK_LOCATION sp)
{
    PPCI_BUS_INTERFACE_STANDARD bi =
        (PPCI_BUS_INTERFACE_STANDARD)sp->Parameters.QueryInterface.Interface;
    ULONG size = sp->Parameters.QueryInterface.Size;
    struct acpi_fadt *fadt = NULL;
    BOOLEAN expressWake = FALSE;
    uacpi_u64 bbn = 0;
    // The v1 GUID ends at ExpressWakeControl; _STANDARD2 adds PrepareMultistageResume.
    // WDK 10.0.28000 declares only the v2 shape, so size by GUID, not sizeof.
    BOOLEAN isV2 = IsEqualGUID(sp->Parameters.QueryInterface.InterfaceType,
                               &GUID_PCI_BUS_INTERFACE_STANDARD2);

    // Through LineToPin is the minimum.
    if (size < (ULONG)FIELD_OFFSET(PCI_BUS_INTERFACE_STANDARD, RootBusCapability))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (sp->Parameters.QueryInterface.Version > (isV2 ? 2u : 1u))
    {
        return STATUS_NOINTERFACE;
    }

    if (uacpi_unlikely_error(
            uacpi_eval_simple_integer(Pdo->Node, "_BBN", &bbn)))
            {
        bbn = 0;                    // no _BBN => bus 0 (ACPI spec default)
    }
    Pdo->PciRootBaseBus = (UCHAR)bbn;

    // PNP0A08 in _HID or _CID marks a PCIe root.
    if (_stricmp(Pdo->Hid, "PNP0A08") == 0)
    {
        Pdo->PciRoot.IsExpress = TRUE;
    }
    else
    {
        static const uacpi_char *const expressIds[] = { "PNP0A08", UACPI_NULL };
        Pdo->PciRoot.IsExpress =
            (uacpi_device_matches_pnp_id(Pdo->Node, expressIds) == UACPI_TRUE);
    }

    // Negotiate _OSC once, at PASSIVE_LEVEL.
    UacpipPciRootOscNegotiate(Pdo);

    // FADT PCIEXP_WAK gates ExpressWakeControl; also save the PM1_EN ports.
    if (uacpi_likely_success(uacpi_table_fadt(&fadt)) && fadt != NULL)
    {
        if (fadt->flags & (1u << 14))
        {
            expressWake = TRUE;
        }
        KeInitializeSpinLock(&UacpiPm1EnLock);
        if (fadt->pm1a_evt_blk != 0 && fadt->pm1_evt_len >= 4)
        {
            UacpiPm1aEnPort = (USHORT)(fadt->pm1a_evt_blk + fadt->pm1_evt_len / 2);
        }
        if (fadt->pm1b_evt_blk != 0 && fadt->pm1_evt_len >= 4)
        {
            UacpiPm1bEnPort = (USHORT)(fadt->pm1b_evt_blk + fadt->pm1_evt_len / 2);
        }
    }

    bi->Size    = FIELD_OFFSET(PCI_BUS_INTERFACE_STANDARD, RootBusCapability);
    bi->Version = isV2 ? 2 : 1;
    bi->Context = Pdo;                // PUACPI_PDO, not Common.Self
    bi->InterfaceReference   = UacpiIfRef;
    bi->InterfaceDereference = UacpiIfDeref;
    bi->ReadConfig  = UacpiIfPciReadConfig;
    bi->WriteConfig = UacpiIfPciWriteConfig;
    bi->PinToLine   = UacpiIfPciPinToLine;
    bi->LineToPin   = UacpiIfPciLineToPin;
    if (size >= (ULONG)FIELD_OFFSET(PCI_BUS_INTERFACE_STANDARD, ExpressWakeControl))
    {
        bi->Size = FIELD_OFFSET(PCI_BUS_INTERFACE_STANDARD, ExpressWakeControl);
        bi->RootBusCapability = UacpiIfPciRootBusCapability;
    }
    if (size >= (ULONG)PCI_BUS_INTERFACE_STANDARD_VERSION_1_LENGTH)
    {
        // Filled only when FADT PCIEXP_WAK is set; NULL means unimplemented.
        if (expressWake)
        {
            bi->Size = (USHORT)PCI_BUS_INTERFACE_STANDARD_VERSION_1_LENGTH;
            bi->ExpressWakeControl = UacpiIfPciExpressWakeControl;
        }
        else
        {
            bi->ExpressWakeControl = NULL;
        }
    }
    if (isV2 && size >= (ULONG)sizeof(PCI_BUS_INTERFACE_STANDARD))
    {
        // Unimplemented; Size stops short of it and pci.sys NULL-checks it.
        bi->PrepareMultistageResume = NULL;
    }
    UacpiTrace("[acpi] provided PCI_BUS_INTERFACE_STANDARD (base bus %u%s) to %s\n",
              Pdo->PciRootBaseBus,
              Pdo->PciRoot.IsExpress ? ", express" : "",
              Pdo->Name);
    return STATUS_SUCCESS;
}

// Synchronous PnP IRP to the top of Target's stack; starts NOT_SUPPORTED.
static NTSTATUS
UacpiSendSynchronousPnpIrp(PDEVICE_OBJECT Target, PIO_STACK_LOCATION TopStack)
{
    PDEVICE_OBJECT top;
    PIRP irp;
    PIO_STACK_LOCATION next;
    IO_STATUS_BLOCK iosb;
    KEVENT event;
    NTSTATUS status;

    KeInitializeEvent(&event, SynchronizationEvent, FALSE);
    top = IoGetAttachedDeviceReference(Target);
    irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, top, NULL, 0, NULL, &event, &iosb);
    if (irp == NULL)
    {
        ObDereferenceObject(top);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    irp->IoStatus.Status      = STATUS_NOT_SUPPORTED;
    irp->IoStatus.Information = 0;
    next = IoGetNextIrpStackLocation(irp);
    *next = *TopStack;                 // same major/minor/parameters
    next->CompletionRoutine = NULL;
    next->Context           = NULL;
    next->Control           = 0;

    status = IoCallDriver(top, irp);
    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    ObDereferenceObject(top);
    return status;
}

// Parent device object: the enumerating filter, else the root FDO (*IsRoot).
static PDEVICE_OBJECT
UacpipPdoParentDeviceObject(PUACPI_PDO Pdo, PBOOLEAN IsRoot)
{
    PUACPI_FDO fdo = Pdo->Parent;
    PDEVICE_OBJECT parent = NULL;
    PLIST_ENTRY e;

    *IsRoot = TRUE;
    if (fdo == NULL)
    {
        return NULL;
    }
    if (Pdo->ParentNode != NULL)
    {
        ExAcquireFastMutex(&fdo->ChildLock);
        for (e = fdo->Filters.Flink; e != &fdo->Filters; e = e->Flink)
        {
            PUACPI_FILTER f = CONTAINING_RECORD(e, UACPI_FILTER, Link);
            if (f->Node == Pdo->ParentNode)
            {
                parent  = f->Common.Self;
                *IsRoot = FALSE;
                break;
            }
        }
        ExReleaseFastMutex(&fdo->ChildLock);
    }
    if (parent == NULL)
    {
        parent = fdo->Common.Self;
    }
    return parent;
}

// QUERY_INTERFACE dispatch

// GUID_DEVICE_RESET_INTERFACE_STANDARD; not declared in this DDK.
#ifndef DEVICE_RESET_INTERFACE_VERSION
#define DEVICE_RESET_INTERFACE_VERSION 1

typedef enum _DEVICE_RESET_TYPE
{
    FunctionLevelDeviceReset = 0,
    PlatformLevelDeviceReset
} DEVICE_RESET_TYPE, *PDEVICE_RESET_TYPE;

typedef NTSTATUS (NTAPI DEVICE_RESET_HANDLER)(
    _In_ PVOID InterfaceContext,
    _In_ DEVICE_RESET_TYPE ResetType,
    _In_ ULONG Flags,
    _In_opt_ PVOID ResetParameters);
typedef DEVICE_RESET_HANDLER *PDEVICE_RESET_HANDLER;

typedef struct _DEVICE_RESET_INTERFACE_STANDARD
{
    USHORT                 Size;
    USHORT                 Version;
    PVOID                  Context;
    PINTERFACE_REFERENCE   InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;
    PDEVICE_RESET_HANDLER  DeviceReset;
    ULONG                  SupportedResetTypes;
    PVOID                  Reserved;
} DEVICE_RESET_INTERFACE_STANDARD, *PDEVICE_RESET_INTERFACE_STANDARD;
#endif

// { 649fdf26-3bc0-4813-ad24-7e0c1eda3fa3 }
static const GUID UacpiGuidDeviceResetInterfaceStandard =
{ 0x649fdf26, 0x3bc0, 0x4813,
  { 0xad, 0x24, 0x7e, 0x0c, 0x1e, 0xda, 0x3f, 0xa3 } };

// Platform-level reset runs _RST on the device's node.
static NTSTATUS NTAPI
UacpiDeviceResetHandler(PVOID InterfaceContext, DEVICE_RESET_TYPE ResetType,
                       ULONG Flags, PVOID ResetParameters)
{
    uacpi_namespace_node *node = (uacpi_namespace_node *)InterfaceContext;
    uacpi_status st;

    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(ResetParameters);

    if (node == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (ResetType != PlatformLevelDeviceReset)
    {
        // A function-level reset belongs to the bus driver, not to firmware
        return STATUS_NOT_SUPPORTED;
    }

    st = uacpi_eval_simple(node, "_RST", UACPI_NULL);
    UacpiTrace("[acpi] reset: _RST -> %d\n", (int)st);
    return uacpi_likely_success(st) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

static BOOLEAN
UacpipNodeHasReset(uacpi_namespace_node *Node)
{
    uacpi_namespace_node *rst = UACPI_NULL;

    return (BOOLEAN)(Node != NULL &&
                     uacpi_likely_success(
                         uacpi_namespace_node_find(Node, "_RST", &rst)) &&
                     rst != UACPI_NULL);
}

NTSTATUS
UacpiBuildDeviceResetInterface(uacpi_namespace_node *Node, const char *TraceName,
                              PIO_STACK_LOCATION sp)
{
    PDEVICE_RESET_INTERFACE_STANDARD out;

    if (!IsEqualGUID(sp->Parameters.QueryInterface.InterfaceType,
                     &UacpiGuidDeviceResetInterfaceStandard))
    {
        return STATUS_NOT_SUPPORTED;
    }
    if (!UacpipNodeHasReset(Node))
    {
        // Nothing firmware can reset here; let whoever else is asked answer
        return STATUS_NOT_SUPPORTED;
    }
    if (sp->Parameters.QueryInterface.Size <
        FIELD_OFFSET(DEVICE_RESET_INTERFACE_STANDARD, SupportedResetTypes))
        {
        return STATUS_BUFFER_TOO_SMALL;
    }

    out = (PDEVICE_RESET_INTERFACE_STANDARD)sp->Parameters.QueryInterface.Interface;
    RtlZeroMemory(out, sp->Parameters.QueryInterface.Size);
    out->Size                 = sp->Parameters.QueryInterface.Size;
    out->Version              = DEVICE_RESET_INTERFACE_VERSION;
    out->Context              = Node;
    out->InterfaceReference   = UacpiIfRef;
    out->InterfaceDereference = UacpiIfDeref;
    out->DeviceReset          = UacpiDeviceResetHandler;
    if (sp->Parameters.QueryInterface.Size >=
        RTL_SIZEOF_THROUGH_FIELD(DEVICE_RESET_INTERFACE_STANDARD, SupportedResetTypes))
        {
        out->SupportedResetTypes = 1u << PlatformLevelDeviceReset;
    }

    UacpiTrace("[acpi] %s: DEVICE_RESET_INTERFACE_STANDARD provided (_RST)\n",
              TraceName);
    return STATUS_SUCCESS;
}

NTSTATUS
UacpiBuildAcpiInterface(PDEVICE_OBJECT Context, const char *TraceName,
                       PIO_STACK_LOCATION sp)
{
    const GUID *guid = sp->Parameters.QueryInterface.InterfaceType;
    PINTERFACE  out  = sp->Parameters.QueryInterface.Interface;

    UNREFERENCED_PARAMETER(out);

    // v1: Context is typed PDEVICE_OBJECT and must be the PDO device object.
    if (IsEqualGUID(guid, &GUID_ACPI_INTERFACE_STANDARD))
    {

        PACPI_INTERFACE_STANDARD ai = (PACPI_INTERFACE_STANDARD)out;

        if (sp->Parameters.QueryInterface.Size < sizeof(ACPI_INTERFACE_STANDARD))
        {
            return STATUS_BUFFER_TOO_SMALL;
        }
        ai->Size    = sizeof(ACPI_INTERFACE_STANDARD);
        ai->Version = 1;
        ai->Context = Context;
        ai->InterfaceReference   = UacpiIfRef;
        ai->InterfaceDereference = UacpiIfDeref;
        ai->GpeConnectVector     = UacpiIfGpeConnectVector;
        ai->GpeDisconnectVector  = UacpiIfGpeDisconnectVector;
        ai->GpeEnableEvent       = UacpiIfGpeEnableEvent;
        ai->GpeDisableEvent      = UacpiIfGpeDisableEvent;
        ai->GpeClearStatus       = UacpiIfGpeClearStatus;
        ai->RegisterForDeviceNotifications   = UacpiIfRegisterNotify;
        ai->UnregisterForDeviceNotifications = UacpiIfUnregisterNotify;
        UacpiTrace("[acpi] provided ACPI_INTERFACE_STANDARD to %s\n", TraceName);
        return STATUS_SUCCESS;
    }

    // v2: same Context; only two slots need v2 wrappers.
    if (IsEqualGUID(guid, &GUID_ACPI_INTERFACE_STANDARD2))
    {

        PACPI_INTERFACE_STANDARD2 ai2 = (PACPI_INTERFACE_STANDARD2)out;

        if (sp->Parameters.QueryInterface.Size < sizeof(ACPI_INTERFACE_STANDARD2))
        {
            return STATUS_BUFFER_TOO_SMALL;
        }
        ai2->Size    = sizeof(ACPI_INTERFACE_STANDARD2);
        ai2->Version = 2;
        ai2->Context = Context;
        ai2->InterfaceReference   = UacpiIfRef;
        ai2->InterfaceDereference = UacpiIfDeref;
        ai2->GpeConnectVector     = (PGPE_CONNECT_VECTOR2)UacpiIfGpeConnectVector;
        ai2->GpeDisconnectVector  = UacpiIfGpeDisconnectVector2;
        ai2->GpeEnableEvent       = (PGPE_ENABLE_EVENT2)UacpiIfGpeEnableEvent;
        ai2->GpeDisableEvent      = (PGPE_DISABLE_EVENT2)UacpiIfGpeDisableEvent;
        ai2->GpeClearStatus       = (PGPE_CLEAR_STATUS2)UacpiIfGpeClearStatus;
        ai2->RegisterForDeviceNotifications =
            (PREGISTER_FOR_DEVICE_NOTIFICATIONS2)UacpiIfRegisterNotify;
        ai2->UnregisterForDeviceNotifications = UacpiIfUnregisterNotify2;
        UacpiTrace("[acpi] provided ACPI_INTERFACE_STANDARD2 to %s\n", TraceName);
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
UacpiPdoQueryInterface(PUACPI_PDO Pdo, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    const GUID *guid = sp->Parameters.QueryInterface.InterfaceType;

    // Forward to the parent stack; x64 IoGetDmaAdapter needs an answer here.
    if (IsEqualGUID(guid, &GUID_BUS_INTERFACE_STANDARD))
    {
        BOOLEAN isRoot = TRUE;
        PDEVICE_OBJECT parent = UacpipPdoParentDeviceObject(Pdo, &isRoot);
        NTSTATUS st = STATUS_NOINTERFACE;

        if (parent != NULL)
        {
            IO_STACK_LOCATION copy = *sp;
            if (isRoot)
            {
                copy.Parameters.QueryInterface.InterfaceSpecificData = Pdo->Common.Self;
            }
            st = UacpiSendSynchronousPnpIrp(parent, &copy);
        }
        UacpiTrace("[acpi] %s: BUS_INTERFACE_STANDARD query forwarded to %s -> 0x%X\n",
                  Pdo->Name, isRoot ? "root stack (HAL PDO)" : "filter stack", st);
        Irp->IoStatus.Status = st;
        return st;
    }
    // Shared with the filter QI path.
    if (IsEqualGUID(guid, &GUID_ACPI_INTERFACE_STANDARD) ||
        IsEqualGUID(guid, &GUID_ACPI_INTERFACE_STANDARD2))
        {
        return UacpiBuildAcpiInterface(Pdo->Common.Self, Pdo->Name, sp);
    }

    // PCI config and root-bus interface, PCI roots only.
    if ((IsEqualGUID(guid, &GUID_PCI_BUS_INTERFACE_STANDARD) ||
         IsEqualGUID(guid, &GUID_PCI_BUS_INTERFACE_STANDARD2)) &&
        UacpiHidIsPciRoot(Pdo->Hid))
        {
        return UacpiBuildPciBusInterface(Pdo, sp);
    }

    // The interrupt translator and arbiter are served by fdo.c and filter.c.
    if (IsEqualGUID(guid, &GUID_ARBITER_INTERFACE_STANDARD))
    {
        // Memory, I/O and bus number arbiters only (resarb.c).
        NTSTATUS st = UacpiQueryResArbiter(Pdo, sp);
        if (st != STATUS_NOT_SUPPORTED)
        {
            return st;
        }
    }

    // Not ours; leave the IRP status unchanged.
    return Irp->IoStatus.Status;
}
