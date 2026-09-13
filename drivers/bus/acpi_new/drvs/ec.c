/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Embedded Controller (PNP0C09): address space, queries, PDO
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"
#include <uacpi/opregion.h>
#include <uacpi/utilities.h>
#include <uacpi/namespace.h>
#include <uacpi/event.h>
#include <uacpi/tables.h>   // uacpi_table_find_by_signature (ECDT)
#include <uacpi/acpi.h>     // struct acpi_ecdt, ACPI_ECDT_SIGNATURE

// EC command bytes / status bits (ACPI spec 12).
#define EC_CMD_READ    0x80
#define EC_CMD_WRITE   0x81
#define EC_CMD_QUERY   0x84
#define EC_STS_OBF     0x01   // output buffer full (EC->host data ready)
#define EC_STS_IBF     0x02   // input buffer full  (host->EC busy)
#define EC_STS_SCI_EVT 0x20   // EC has a pending SCI query event

#define EC_SPACE_SIZE  256    // one byte of address
#define EC_TIMEOUT_US  10000  // per handshake step
#define EC_POLL_US     10

// The length acpi.sys demands of a registration: 0x10 on x86, 0x20 on x64.
C_ASSERT(sizeof(ACPI_EC_QUERY_REGISTRATION) == (sizeof(PVOID) == 8 ? 32 : 16));

// Linked at bring-up (PASSIVE, serialized by PnP), walked from the query drain.
static PUACPI_EC  UacpiEcList;
static KSPIN_LOCK UacpiEcListLock;

// Spin until (status & mask) == want, or the timeout (us) elapses.
static BOOLEAN
UacpiEcWait(PUACPI_EC Ec, UCHAR mask, UCHAR want, ULONG timeoutUs)
{
    ULONG waited = 0;
    for (;;)
    {
        UCHAR sts = READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)Ec->CmdPort);
        if ((sts & mask) == want)
        {
            return TRUE;
        }
        if (waited >= timeoutUs)
        {
            return FALSE;
        }
        KeStallExecutionProcessor(EC_POLL_US);
        waited += EC_POLL_US;
    }
}

// _GLK, for callers that reach the controller from outside the interpreter.
// uACPI takes the global lock itself across a field transaction whose Lock rule
// asks for it, and that lock is not recursive, so the region handler must not.
static BOOLEAN
UacpiEcGlobalLockAcquire(PUACPI_EC Ec, uacpi_u32 *Seq)
{
    *Seq = 0;
    if (!Ec->GlobalLock)
    {
        return TRUE;
    }
    return (BOOLEAN)(uacpi_acquire_global_lock(0xFFFF, Seq) == UACPI_STATUS_OK);
}

static VOID
UacpiEcGlobalLockRelease(PUACPI_EC Ec, uacpi_u32 Seq)
{
    if (Ec->GlobalLock)
    {
        (void)uacpi_release_global_lock(Seq);
    }
}

static BOOLEAN
UacpiEcReadByte(PUACPI_EC Ec, UCHAR addr, PUCHAR value)
{
    BOOLEAN ok;

    ExAcquireFastMutex(&Ec->Lock);
    ok = UacpiEcWait(Ec, EC_STS_IBF, 0, EC_TIMEOUT_US);
    if (ok)
    {
        WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)Ec->CmdPort, EC_CMD_READ);
        ok = UacpiEcWait(Ec, EC_STS_IBF, 0, EC_TIMEOUT_US);
    }
    if (ok)
    {
        WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)Ec->DataPort, addr);
        ok = UacpiEcWait(Ec, EC_STS_OBF, EC_STS_OBF, EC_TIMEOUT_US);
    }
    if (ok)
    {
        *value = READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)Ec->DataPort);
    }
    ExReleaseFastMutex(&Ec->Lock);

    if (!ok)
    {
        UacpiTrace("[acpi] EC: read 0x%02X timed out\n", addr);
    }
    return ok;
}

static BOOLEAN
UacpiEcWriteByte(PUACPI_EC Ec, UCHAR addr, UCHAR value)
{
    BOOLEAN ok;

    ExAcquireFastMutex(&Ec->Lock);
    ok = UacpiEcWait(Ec, EC_STS_IBF, 0, EC_TIMEOUT_US);
    if (ok)
    {
        WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)Ec->CmdPort, EC_CMD_WRITE);
        ok = UacpiEcWait(Ec, EC_STS_IBF, 0, EC_TIMEOUT_US);
    }
    if (ok)
    {
        WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)Ec->DataPort, addr);
        ok = UacpiEcWait(Ec, EC_STS_IBF, 0, EC_TIMEOUT_US);
    }
    if (ok)
    {
        WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)Ec->DataPort, value);
    }
    ExReleaseFastMutex(&Ec->Lock);

    if (!ok)
    {
        UacpiTrace("[acpi] EC: write 0x%02X timed out\n", addr);
    }
    return ok;
}

// One locked EC QUERY; returns the query byte, 0 if none. Run _Qxx unlocked.
static UCHAR
UacpiEcQueryOne(PUACPI_EC Ec)
{
    uacpi_u32 seq;
    UCHAR sts, q = 0;

    // A query is ours, not a field transaction: nothing above holds _GLK.
    if (!UacpiEcGlobalLockAcquire(Ec, &seq))
    {
        return 0;
    }
    ExAcquireFastMutex(&Ec->Lock);
    sts = READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)Ec->CmdPort);
    if (sts & EC_STS_SCI_EVT)
    {
        if (UacpiEcWait(Ec, EC_STS_IBF, 0, EC_TIMEOUT_US))
        {
            WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)Ec->CmdPort, EC_CMD_QUERY);
            if (UacpiEcWait(Ec, EC_STS_OBF, EC_STS_OBF, EC_TIMEOUT_US))
            {
                q = READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)Ec->DataPort);
            }
        }
    }
    ExReleaseFastMutex(&Ec->Lock);
    UacpiEcGlobalLockRelease(Ec, seq);
    return q;
}

// Drain pending queries. As in acpi.sys, a driver that claimed the code gets
// the callback and _Qxx does not run.
static VOID
UacpiEcDrainQueries(PUACPI_EC Ec)
{
    ULONG guard;

    for (guard = 0; guard < UACPI_EC_QUERY_CODES; guard++)
    {
        PACPI_EC_QUERY_ROUTINE routine;
        PVOID context;
        KIRQL irql;
        char  name[8];
        UCHAR q = UacpiEcQueryOne(Ec);

        if (q == 0)
        {
            return;
        }

        KeAcquireSpinLock(&Ec->QueryLock, &irql);
        routine = Ec->QueryRoutine[q];
        context = Ec->QueryContext[q];
        KeReleaseSpinLock(&Ec->QueryLock, irql);

        if (routine != NULL)
        {
            UacpiTrace("[acpi] EC: query 0x%02X -> handler\n", q);
            routine(q, context);
            continue;
        }

        RtlStringCbPrintfA(name, sizeof(name), "_Q%02X", q);
        UacpiTrace("[acpi] EC: query 0x%02X -> %s\n", q, name);
        if (Ec->Node != NULL)
        {
            (void)uacpi_eval(Ec->Node, name, NULL, NULL);
        }
    }

    UacpiTrace("[acpi] EC: query drain hit its bound\n");
}

// Runs at PASSIVE_LEVEL: safe to take the fast mutex and evaluate _Qxx (AML).
static VOID
NTAPI
UacpiEcWorkRoutine(PVOID Context)
{
    PUACPI_EC ec = (PUACPI_EC)Context;

    // Clear the flag first so a GPE during the drain queues another pass.
    InterlockedExchange(&ec->WorkQueued, 0);
    UacpiEcDrainQueries(ec);
}

// DISPATCH_LEVEL: queues the PASSIVE drain the SCI ISR cannot queue itself.
_Function_class_(KDEFERRED_ROUTINE)
static VOID
NTAPI
UacpiEcGpeDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    PUACPI_EC ec = (PUACPI_EC)Context;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    // Deprecated API; acceptable since the driver never unloads.
#pragma warning(suppress: 4996)
    ExQueueWorkItem(&ec->Work, DelayedWorkQueue);
}

// EC GPE handler, called from the SCI ISR. Queue the drain to PASSIVE via a DPC.
static uacpi_interrupt_ret
UacpiEcGpeHandler(uacpi_handle ctx, uacpi_namespace_node *gpe_device, uacpi_u16 idx)
{
    PUACPI_EC ec = (PUACPI_EC)ctx;

    UNREFERENCED_PARAMETER(gpe_device);
    UNREFERENCED_PARAMETER(idx);
    if (InterlockedExchange(&ec->WorkQueued, 1) == 0)
    {
        KeInsertQueueDpc(&ec->Dpc, NULL, NULL);
    }
    return UACPI_GPE_REENABLE;
}

static uacpi_status
UacpiEcRegionHandler(uacpi_region_op op, uacpi_handle op_data)
{
    switch (op)
    {
    case UACPI_REGION_OP_ATTACH:
    {
        uacpi_region_attach_data *a = (uacpi_region_attach_data *)op_data;
        a->out_region_context = a->handler_context;
        return UACPI_STATUS_OK;
    }
    case UACPI_REGION_OP_DETACH:
        return UACPI_STATUS_OK;

    case UACPI_REGION_OP_READ:
    {
        uacpi_region_rw_data *rw = (uacpi_region_rw_data *)op_data;
        PUACPI_EC ec = (PUACPI_EC)rw->handler_context;
        uacpi_u8 width = rw->byte_width ? rw->byte_width : 1;
        uacpi_u8 i;

        // Access is byte-granular; a wider AML field assembles little-endian.
        rw->value = 0;
        for (i = 0; i < width; i++)
        {
            UCHAR v;
            if (rw->offset + i >= EC_SPACE_SIZE ||
                !UacpiEcReadByte(ec, (UCHAR)(rw->offset + i), &v))
            {
                return UACPI_STATUS_AML_BAD_ENCODING;
            }
            rw->value |= (uacpi_u64)v << (i * 8);
        }
        return UACPI_STATUS_OK;
    }
    case UACPI_REGION_OP_WRITE:
    {
        uacpi_region_rw_data *rw = (uacpi_region_rw_data *)op_data;
        PUACPI_EC ec = (PUACPI_EC)rw->handler_context;
        uacpi_u8 width = rw->byte_width ? rw->byte_width : 1;
        uacpi_u8 i;

        for (i = 0; i < width; i++)
        {
            if (rw->offset + i >= EC_SPACE_SIZE ||
                !UacpiEcWriteByte(ec, (UCHAR)(rw->offset + i),
                                  (UCHAR)(rw->value >> (i * 8))))
            {
                return UACPI_STATUS_AML_BAD_ENCODING;
            }
        }
        return UACPI_STATUS_OK;
    }
    default:
        return UACPI_STATUS_UNIMPLEMENTED;
    }
}

// Extract the EC's two I/O ports from _CRS (data first, command/status second).
static BOOLEAN
UacpiEcReadPorts(uacpi_namespace_node *node, PUACPI_EC Ec)
{
    uacpi_resources *res = NULL;
    uacpi_resource *r;
    USHORT ports[2];
    ULONG n = 0;

    if (uacpi_unlikely_error(uacpi_get_current_resources(node, &res)) || res == NULL)
    {
        return FALSE;
    }
    for (r = res->entries; r->type != UACPI_RESOURCE_TYPE_END_TAG && n < 2;
         r = UACPI_NEXT_RESOURCE(r))
         {
        if (r->type == UACPI_RESOURCE_TYPE_IO)
        {
            ports[n++] = r->io.minimum;
        }
        else if (r->type == UACPI_RESOURCE_TYPE_FIXED_IO)
        {
            ports[n++] = r->fixed_io.address;
        }
    }
    uacpi_free_resources(res);
    if (n < 2)
    {
        return FALSE;
    }
    Ec->DataPort = ports[0];
    Ec->CmdPort  = ports[1];
    return TRUE;
}

// Read the query GPE from _GPE: a \_GPE index or {gpe_device, index}.
static BOOLEAN
UacpiEcReadGpe(uacpi_namespace_node *node, PUACPI_EC Ec)
{
    uacpi_object *ret = NULL;
    uacpi_u64 idx = 0;
    BOOLEAN ok = FALSE;

    if (uacpi_unlikely_error(uacpi_eval(node, "_GPE", NULL, &ret)) || ret == NULL)
    {
        UacpiTrace("[acpi] EC: no _GPE - queries will not be delivered\n");
        return FALSE;
    }
    if (uacpi_object_get_type(ret) == UACPI_OBJECT_INTEGER)
    {
        if (uacpi_likely_success(uacpi_object_get_integer(ret, &idx)))
        {
            Ec->GpeDevice = UACPI_NULL;   // FADT \_GPE block
            Ec->GpeIdx    = (uacpi_u16)idx;
            ok = TRUE;
        }
    }
    else if (uacpi_object_get_type(ret) == UACPI_OBJECT_PACKAGE)
    {
        uacpi_object_array pkg;
        if (uacpi_likely_success(uacpi_object_get_package(ret, &pkg)) &&
            pkg.count >= 2 &&
            uacpi_likely_success(uacpi_object_get_integer(pkg.objects[1], &idx)))
            {
            // GPE block device in [0] ignored; index treated as a \_GPE bit.
            Ec->GpeDevice = UACPI_NULL;
            Ec->GpeIdx    = (uacpi_u16)idx;
            ok = TRUE;
        }
    }
    uacpi_object_unref(ret);
    return ok;
}

static VOID
UacpiEcReadGlk(uacpi_namespace_node *node, PUACPI_EC Ec)
{
    uacpi_u64 glk = 0;

    if (uacpi_likely_success(uacpi_eval_simple_integer(node, "_GLK", &glk)) &&
        glk != 0)
    {
        Ec->GlobalLock = TRUE;
        UacpiTrace("[acpi] EC: _GLK set\n");
    }
}

static PUACPI_EC
UacpiEcAllocate(uacpi_namespace_node *node)
{
    PUACPI_EC ec = (PUACPI_EC)ExAllocatePoolWithTag(NonPagedPool, sizeof(*ec),
                                                    UACPI_POOL_TAG);
    if (ec == NULL)
    {
        return NULL;
    }
    RtlZeroMemory(ec, sizeof(*ec));
    ec->Node = node;
    ExInitializeFastMutex(&ec->Lock);
    KeInitializeSpinLock(&ec->QueryLock);
#pragma warning(suppress: 4996)
    ExInitializeWorkItem(&ec->Work, UacpiEcWorkRoutine, ec);
    KeInitializeDpc(&ec->Dpc, UacpiEcGpeDpc, ec);
    return ec;
}

static PUACPI_EC
UacpiEcFindByNode(uacpi_namespace_node *node)
{
    PUACPI_EC ec;
    KIRQL irql;

    KeAcquireSpinLock(&UacpiEcListLock, &irql);
    for (ec = UacpiEcList; ec != NULL; ec = ec->Next)
    {
        if (ec->Node == node)
        {
            break;
        }
    }
    KeReleaseSpinLock(&UacpiEcListLock, irql);
    return ec;
}

// Ports must already be set. The GPE is optional: without one the EC still
// serves AML reads, it just delivers no events.
static BOOLEAN
UacpiEcInstall(PUACPI_EC Ec, BOOLEAN HaveGpe)
{
    KIRQL irql;

    if (uacpi_unlikely_error(uacpi_install_address_space_handler(
            Ec->Node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,
            UacpiEcRegionHandler, Ec)))
            {
        UacpiTrace("[acpi] EC: install_address_space_handler failed\n");
        return FALSE;
    }
    Ec->RegionOn = TRUE;

    KeAcquireSpinLock(&UacpiEcListLock, &irql);
    Ec->Next = UacpiEcList;
    UacpiEcList = Ec;
    KeReleaseSpinLock(&UacpiEcListLock, irql);

    UacpiTrace("[acpi] EC installed: data=0x%X cmd=0x%X%s\n",
               Ec->DataPort, Ec->CmdPort, Ec->FromEcdt ? " (ECDT)" : "");

    if (HaveGpe)
    {
        if (uacpi_likely_success(uacpi_install_gpe_handler(
                Ec->GpeDevice, Ec->GpeIdx, UACPI_GPE_TRIGGERING_EDGE,
                UacpiEcGpeHandler, Ec)))
                {
            (void)uacpi_enable_gpe(Ec->GpeDevice, Ec->GpeIdx);
            UacpiTrace("[acpi] EC: query GPE 0x%02X connected\n", Ec->GpeIdx);
            UacpiEcDrainQueries(Ec);   // service any query pending at boot
        }
        else
        {
            UacpiTrace("[acpi] EC: install_gpe_handler(0x%02X) failed\n", Ec->GpeIdx);
        }
    }
    return TRUE;
}

BOOLEAN
UacpiEcIsEcNode(uacpi_namespace_node *Node)
{
    static const uacpi_char *const ec_hids[] = { "PNP0C09", UACPI_NULL };

    return (BOOLEAN)(Node != NULL && uacpi_device_matches_pnp_id(Node, ec_hids));
}

// Bring one namespace EC up from its own _CRS / _GPE / _GLK.
static PUACPI_EC
UacpiEcBringUpNode(uacpi_namespace_node *node)
{
    PUACPI_EC ec = UacpiEcFindByNode(node);
    BOOLEAN haveGpe;

    if (ec != NULL)
    {
        return ec;   // already up (ECDT, or an earlier pass)
    }

    ec = UacpiEcAllocate(node);
    if (ec == NULL)
    {
        return NULL;
    }
    if (!UacpiEcReadPorts(node, ec))
    {
        UacpiTrace("[acpi] EC found but _CRS ports unreadable\n");
        ExFreePoolWithTag(ec, UACPI_POOL_TAG);
        return NULL;
    }
    UacpiEcReadGlk(node, ec);
    haveGpe = UacpiEcReadGpe(node, ec);

    if (!UacpiEcInstall(ec, haveGpe))
    {
        ExFreePoolWithTag(ec, UACPI_POOL_TAG);
        return NULL;
    }
    return ec;
}

// Boot EC from the ECDT: ports, GPE and namespace path all come from the table.
static BOOLEAN
UacpiEcInitFromEcdt(void)
{
    uacpi_table tbl;
    struct acpi_ecdt *ecdt;
    uacpi_namespace_node *node = NULL;
    PUACPI_EC ec;
    BOOLEAN ok = FALSE;

    if (uacpi_unlikely_error(
            uacpi_table_find_by_signature(ACPI_ECDT_SIGNATURE, &tbl)) ||
        tbl.ptr == NULL)
        {
        return FALSE;
    }
    ecdt = (struct acpi_ecdt *)tbl.ptr;

    if (uacpi_unlikely_error(uacpi_namespace_node_find(
            uacpi_namespace_root(), ecdt->ec_id, &node)) || node == NULL)
            {
        UacpiTrace("[acpi] EC(ECDT): device '%s' not found in namespace\n",
                   ecdt->ec_id);
        goto done;
    }
    if (UacpiEcFindByNode(node) != NULL)
    {
        ok = TRUE;
        goto done;
    }

    ec = UacpiEcAllocate(node);
    if (ec == NULL)
    {
        goto done;
    }
    ec->CmdPort   = (USHORT)ecdt->ec_control.address;   // command/status port
    ec->DataPort  = (USHORT)ecdt->ec_data.address;      // data port
    ec->GpeDevice = UACPI_NULL;                         // FADT \_GPE block
    ec->GpeIdx    = ecdt->gpe_bit;
    ec->FromEcdt  = TRUE;

    if (!UacpiEcInstall(ec, TRUE))
    {
        ExFreePoolWithTag(ec, UACPI_POOL_TAG);
        goto done;
    }
    UacpiTrace("[acpi] EC(ECDT) id=%s gpe=0x%02X\n", ecdt->ec_id, ec->GpeIdx);
    ok = TRUE;

done:
    uacpi_table_unref(&tbl);
    return ok;
}

static uacpi_iteration_decision
UacpiEcMatchCb(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    UNREFERENCED_PARAMETER(user);
    UNREFERENCED_PARAMETER(depth);

    // Match on _HID/_CID only: EC _STA may itself need the region handler.
    if (!UacpiEcIsEcNode(node))
    {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    UacpiTrace("[acpi] EC: PNP0C09 matched, reading _CRS ports\n");
    if (UacpiEcBringUpNode(node) != NULL)
    {
        return UACPI_ITERATION_DECISION_BREAK;   // one boot EC is enough
    }
    return UACPI_ITERATION_DECISION_CONTINUE;    // keep looking
}

// From glue.c, before uacpi_namespace_initialize(). The ECDT names the boot EC
// for exactly this moment; without one, scan for PNP0C09 instead, since _INI
// and _REG run AML that reads EC fields. The rest come up at their own START.
VOID
UacpiEcInitialize(void)
{
    KeInitializeSpinLock(&UacpiEcListLock);

    if (!UacpiEcInitFromEcdt())
    {
        (void)uacpi_namespace_for_each_child_simple(uacpi_namespace_root(),
                                                    UacpiEcMatchCb, NULL);
    }
    if (UacpiEcList == NULL)
    {
        UacpiTrace("[acpi] no Embedded Controller present\n");
    }
}

// PNP0C09 START: adopt the controller already up on this node, or install it.
// acpi.sys draws the same line between its ECDT context and a device it starts.
VOID
UacpiEcStart(PUACPI_PDO Pdo)
{
    PUACPI_EC ec;

    if (Pdo->Node == NULL || !UacpiEcIsEcNode(Pdo->Node))
    {
        return;
    }

    ec = UacpiEcBringUpNode(Pdo->Node);
    if (ec == NULL)
    {
        UacpiTrace("[acpi] EC: %s could not be started\n", Pdo->Name);
        return;
    }

    ec->Pdo = Pdo;
    Pdo->Ec = ec;
    UacpiTrace("[acpi] EC: %s started (%s)\n", Pdo->Name,
               ec->FromEcdt ? "ECDT" : "_CRS");
}

// Only the device link is dropped: AML keeps reading EC fields for as long as
// the namespace is loaded.
VOID
UacpiEcRemove(PUACPI_PDO Pdo)
{
    PUACPI_EC ec = Pdo->Ec;

    if (ec == NULL)
    {
        return;
    }
    ec->Pdo = NULL;
    Pdo->Ec = NULL;
    UacpiTrace("[acpi] EC: %s removed\n", Pdo->Name);
}

// IRP_MJ_READ / IRP_MJ_WRITE: ByteOffset is the EC address, Length the count.
// Polled PIO under the transaction mutex, so PASSIVE_LEVEL only; acpi.sys
// queues these instead and takes them at any IRQL.
NTSTATUS
UacpiEcReadWrite(PUACPI_PDO Pdo, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PUACPI_EC ec = Pdo->Ec;
    PUCHAR buf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    BOOLEAN write = (BOOLEAN)(sp->MajorFunction == IRP_MJ_WRITE);
    ULONG length = sp->Parameters.Read.Length;
    ULONG offset = sp->Parameters.Read.ByteOffset.LowPart;
    uacpi_u32 seq;
    ULONG i;

    if (ec == NULL || !ec->RegionOn)
    {
        return UacpiCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return UacpiCompleteIrp(Irp, STATUS_INVALID_DEVICE_STATE, 0);
    }
    if (length == 0)
    {
        return UacpiCompleteIrp(Irp, STATUS_SUCCESS, 0);
    }
    // The address space is 256 bytes.
    if (buf == NULL || sp->Parameters.Read.ByteOffset.HighPart != 0 ||
        offset >= EC_SPACE_SIZE || length > EC_SPACE_SIZE - offset)
    {
        return UacpiCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
    }

    // Held across the transfer, so firmware sees a multi-byte access whole.
    if (!UacpiEcGlobalLockAcquire(ec, &seq))
    {
        return UacpiCompleteIrp(Irp, STATUS_DEVICE_BUSY, 0);
    }
    for (i = 0; i < length; i++)
    {
        BOOLEAN ok = write
            ? UacpiEcWriteByte(ec, (UCHAR)(offset + i), buf[i])
            : UacpiEcReadByte(ec, (UCHAR)(offset + i), &buf[i]);

        if (!ok)
        {
            UacpiEcGlobalLockRelease(ec, seq);
            return UacpiCompleteIrp(Irp, STATUS_IO_TIMEOUT, i);   // short
        }
    }
    UacpiEcGlobalLockRelease(ec, seq);
    return UacpiCompleteIrp(Irp, STATUS_SUCCESS, length);
}

// Register / unregister a callback for one query code. METHOD_NEITHER hands us
// the caller's own pointer, so kernel callers only.
NTSTATUS
UacpiEcDeviceControl(PUACPI_PDO Pdo, PIRP Irp, PBOOLEAN Handled)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
    PACPI_EC_QUERY_REGISTRATION reg;
    PUACPI_EC ec = Pdo->Ec;
    NTSTATUS status;
    KIRQL irql;

    *Handled = FALSE;
    if (code != IOCTL_ACPI_EC_REGISTER_QUERY_HANDLER &&
        code != IOCTL_ACPI_EC_UNREGISTER_QUERY_HANDLER)
    {
        return STATUS_NOT_SUPPORTED;
    }
    *Handled = TRUE;

    if (ec == NULL)
    {
        return UacpiCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
    }
    if (Irp->RequestorMode != KernelMode)
    {
        return UacpiCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
    if (sp->Parameters.DeviceIoControl.Type3InputBuffer == NULL ||
        sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(*reg))
    {
        return UacpiCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    }
    reg = (PACPI_EC_QUERY_REGISTRATION)sp->Parameters.DeviceIoControl.Type3InputBuffer;

    KeAcquireSpinLock(&ec->QueryLock, &irql);
    if (code == IOCTL_ACPI_EC_REGISTER_QUERY_HANDLER)
    {
        if (reg->Handler == NULL || ec->QueryRoutine[reg->QueryCode] != NULL)
        {
            status = STATUS_UNSUCCESSFUL;   // already claimed
        }
        else
        {
            ec->QueryRoutine[reg->QueryCode] = (PACPI_EC_QUERY_ROUTINE)reg->Handler;
            ec->QueryContext[reg->QueryCode] = reg->Context;
            reg->Cookie = (ULONG_PTR)reg->QueryCode + 1;
            status = STATUS_SUCCESS;
        }
    }
    else
    {
        if (ec->QueryRoutine[reg->QueryCode] == NULL)
        {
            status = STATUS_UNSUCCESSFUL;
        }
        else
        {
            ec->QueryRoutine[reg->QueryCode] = NULL;
            ec->QueryContext[reg->QueryCode] = NULL;
            status = STATUS_SUCCESS;
        }
    }
    KeReleaseSpinLock(&ec->QueryLock, irql);

    UacpiTrace("[acpi] EC: %s handler for _Q%02X -> 0x%X\n",
               code == IOCTL_ACPI_EC_REGISTER_QUERY_HANDLER ? "register"
                                                            : "unregister",
               reg->QueryCode, status);

    return UacpiCompleteIrp(Irp, status, 0);
}
