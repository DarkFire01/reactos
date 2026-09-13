/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ACPI IRQ (GSIV) arbiter
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"
#include <wdmguid.h>        // GUID_ARBITER_INTERFACE_STANDARD, GUID_BUS_INTERFACE_STANDARD
#include <limits.h>         // LONG_MAX
#include "arbiter.h"        // ARBITER_INSTANCE and the Arb* library
#include <uacpi/utilities.h>
#include <uacpi/tables.h>   // uacpi_table_fadt, for the SCI GSI
#include <uacpi/acpi.h>     // struct acpi_fadt

// PCI configuration-space offset of the Interrupt Pin register (type-0 header).
#define UACPI_PCI_CFG_INTERRUPT_PIN 0x3D

// Runtime gate (default on), checked at QUERY_INTERFACE time.
int UacpiIrqArbEnabled = 1;
// Verbose Test/Rollback and _PRT traces. Registry: Parameters\IrqArbVerbose.
int UacpiIrqArbVerbose = 1;

static ARBITER_INSTANCE UacpiIrqArbiter;
static BOOLEAN          UacpiIrqArbUp = FALSE;

#ifndef CM_RESOURCE_INTERRUPT_MESSAGE
#define CM_RESOURCE_INTERRUPT_MESSAGE 0x0002
#endif
// Message-interrupt window; any placed Start at or above BASE is MSI.
#define UACPI_MSI_GSIV_BASE 0xFFF00000ull
#define UACPI_MSI_GSIV_LAST 0xFFFFFFFEull

// Message count per placement, keyed on its message-window slot.
#define UACPI_MSI_RANGE_DATA_MAX 64

typedef struct _UACPI_MSI_RANGE_DATA
{
    ULONGLONG Start;        // message-window slot the device was placed on
    ULONG     MessageCount; // messages behind the placement
} UACPI_MSI_RANGE_DATA;

static UACPI_MSI_RANGE_DATA UacpiMsiRangeData[UACPI_MSI_RANGE_DATA_MAX];
static ULONG UacpiMsiRangeDataCount;

// Record the message count for a placement. Called from PackResource.
static VOID
UacpipMessageCountRecord(ULONGLONG Start, ULONG Count)
{
    ULONG i;

    for (i = 0; i < UacpiMsiRangeDataCount; i++)
    {
        if (UacpiMsiRangeData[i].Start == Start)
        {
            UacpiMsiRangeData[i].MessageCount = Count;   // re-placed: replace
            return;
        }
    }
    if (UacpiMsiRangeDataCount < UACPI_MSI_RANGE_DATA_MAX)
    {
        UacpiMsiRangeData[UacpiMsiRangeDataCount].Start        = Start;
        UacpiMsiRangeData[UacpiMsiRangeDataCount].MessageCount = Count;
        UacpiMsiRangeDataCount++;
        return;
    }
    UacpiTrace("[acpi] irqarb: WARN - message range-data table full (%u), "
              "count for slot 0x%I64X not recorded\n",
              UACPI_MSI_RANGE_DATA_MAX, Start);
}

// The count recorded for a placement, or STATUS_NOT_FOUND.
static NTSTATUS
UacpipMessageCountFromAllocation(ULONGLONG Start, PULONG Count)
{
    ULONG i;

    *Count = 0;

    for (i = 0; i < UacpiMsiRangeDataCount; i++)
    {
        if (UacpiMsiRangeData[i].Start == Start)
        {
            *Count = UacpiMsiRangeData[i].MessageCount;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_NOT_FOUND;
}

// Message count from the requirement span, 1..32; allocator handles alignment.
static ULONG
UacpipMessageCountFromRequirement(PIO_RESOURCE_DESCRIPTOR Descriptor)
{
    ULONGLONG span;

    if (Descriptor->u.Interrupt.MaximumVector < Descriptor->u.Interrupt.MinimumVector)
    {
        return 1;
    }
    span = (ULONGLONG)Descriptor->u.Interrupt.MaximumVector -
           Descriptor->u.Interrupt.MinimumVector + 1;
    if (span == 0 || span > 32)
    {
        return 1;
    }
    return (ULONG)span;
}

static NTSTATUS NTAPI
UacpiIrqUnpackRequirement(PIO_RESOURCE_DESCRIPTOR Descriptor, PULONGLONG Minimum,
                         PULONGLONG Maximum, PULONGLONG Length, PULONGLONG Alignment)
{
    if (Descriptor == NULL || Descriptor->Type != CmResourceTypeInterrupt)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (Descriptor->Flags & CM_RESOURCE_INTERRUPT_MESSAGE)
    {
        // MSI/MSI-X: one slot in the message window. IDT vectors come later.
        ULONG count = UacpipMessageCountFromRequirement(Descriptor);
        ULONGLONG span = 0;

        // PIC model cannot deliver MSI; an empty window falls back to INTx.
        if (g_AcpiInterruptModel != 1)
        {
            UacpiTrace("[acpi] irqreq: MESSAGE declined, interrupt model is PIC "
                      "- falling back to the wired line\n");
            *Minimum   = 1;
            *Maximum   = 0;
            *Length    = 1;
            *Alignment = 1;
            return STATUS_SUCCESS;
        }

        if (Descriptor->u.Interrupt.MaximumVector >= Descriptor->u.Interrupt.MinimumVector)
        {
            span = (ULONGLONG)Descriptor->u.Interrupt.MaximumVector -
                   Descriptor->u.Interrupt.MinimumVector + 1;
        }
        // Span is for the trace only; PackResource records the count.
        UacpiTrace("[acpi] irqreq: MESSAGE flags 0x%X share %u min 0x%X max 0x%X "
                  "span %u -> one slot, message count %u%s\n",
                  Descriptor->Flags, Descriptor->ShareDisposition,
                  Descriptor->u.Interrupt.MinimumVector,
                  Descriptor->u.Interrupt.MaximumVector,
                  (ULONG)span, count,
                  (span > 1 && count != (ULONG)span) ? "  <== MALFORMED SPAN" : "");
        *Minimum   = UACPI_MSI_GSIV_BASE;
        *Maximum   = UACPI_MSI_GSIV_LAST;
        *Length    = 1;
        *Alignment = 1;
        return STATUS_SUCCESS;
    }
    *Minimum   = Descriptor->u.Interrupt.MinimumVector;
    *Maximum   = Descriptor->u.Interrupt.MaximumVector;
    *Length    = 1;
    *Alignment = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
UacpiIrqPackResource(PIO_RESOURCE_DESCRIPTOR Requirement, ULONGLONG Start,
                    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor)
{
    if (Requirement == NULL || Descriptor == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    Descriptor->Type             = CmResourceTypeInterrupt;
    Descriptor->ShareDisposition = Requirement->ShareDisposition;
    Descriptor->Flags            = Requirement->Flags;
    if (Start >= UACPI_MSI_GSIV_BASE)
    {
        // Raw message form; the kernel resolves the real vector later.
        Descriptor->Flags |= CM_RESOURCE_INTERRUPT_MESSAGE;
        // Level: low USHORT is Group, high USHORT is MessageCount.
        {
            // First callback holding both the count and the granted slot.
            ULONG count = UacpipMessageCountFromRequirement(Requirement);

            UacpipMessageCountRecord(Start, count);

            Descriptor->u.Interrupt.Level = (count << 16);   // Group 0
        }
        Descriptor->u.Interrupt.Vector   = (ULONG)Start;
        // Keep the requested affinity policy; -1 when none was given.
        {
            KAFFINITY targeted = (KAFFINITY)Requirement->u.Interrupt.TargetedProcessors;

            Descriptor->u.Interrupt.Affinity =
                (targeted != 0) ? targeted : (KAFFINITY)-1;
        }
    }
    else
    {
        // Raw line form: Level/Vector carry the GSIV.
        Descriptor->u.Interrupt.Level    = (ULONG)Start;
        Descriptor->u.Interrupt.Vector   = (ULONG)Start;
        Descriptor->u.Interrupt.Affinity = (KAFFINITY)-1;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
UacpiIrqUnpackResource(PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor,
                      PULONGLONG Start, PULONGLONG Length)
{
    if (Descriptor == NULL || Descriptor->Type != CmResourceTypeInterrupt)
    {
        return STATUS_INVALID_PARAMETER;
    }
    // Vector is the GSIV or message slot; a message spans MessageCount slots.
    *Start  = Descriptor->u.Interrupt.Vector;
    *Length = 1;
    if ((Descriptor->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) ||
        Descriptor->u.Interrupt.Vector >= UACPI_MSI_GSIV_BASE)
        {
        ULONG count = (ULONG)(Descriptor->u.Interrupt.Level >> 16);
        if (count != 0 && count <= 32)
        {
            *Length = count;
        }
    }
    return STATUS_SUCCESS;
}

static INT32 NTAPI
UacpiIrqScoreRequirement(PIO_RESOURCE_DESCRIPTOR Descriptor)
{
    ULONG min, max, score;

    // Lower scores go first. Clamp at 0xFFFF; +5 if it can reach above 15.
    if (Descriptor == NULL || Descriptor->Type != CmResourceTypeInterrupt)
    {
        return 0xFFFF;
    }
    min = Descriptor->u.Interrupt.MinimumVector;
    max = Descriptor->u.Interrupt.MaximumVector;

    score = max - min + 1;
    if ((max - min) == (ULONG)-1 || score > 0xFFFF)
    {
        score = 0xFFFF;      // empty range wraps to 0; treat as least constrained
    }
    if (max >= 0x10)
    {
        score += 5;
    }
    return (LONG)score;
}

// _PRT resolution: PDO -> routed GSIV.

// Find the PCI-root child PDO whose _BBN matches Bus (absent _BBN counts as 0).
// Returns the namespace node (nodes are stable for the driver's lifetime).
static uacpi_namespace_node *
UacpipFindPciRootNode(ULONG Bus)
{
    uacpi_namespace_node *candidates[8];
    ULONG count = 0, i;
    PLIST_ENTRY e;

    if (g_AcpiFdo == NULL)
    {
        return NULL;
    }

    ExAcquireFastMutex(&g_AcpiFdo->ChildLock);
    for (e = g_AcpiFdo->Children.Flink;
         e != &g_AcpiFdo->Children && count < RTL_NUMBER_OF(candidates);
         e = e->Flink)
         {
        PUACPI_PDO pdo = CONTAINING_RECORD(e, UACPI_PDO, Link);
        if (_stricmp(pdo->Hid, "PNP0A03") == 0 || _stricmp(pdo->Hid, "PNP0A08") == 0)
        {
            candidates[count++] = pdo->Node;
        }
    }
    ExReleaseFastMutex(&g_AcpiFdo->ChildLock);

    // Evaluate _BBN outside the child lock (AML evaluation).
    for (i = 0; i < count; i++)
    {
        uacpi_u64 bbn = 0;
        if (uacpi_unlikely_error(
                uacpi_eval_simple_integer(candidates[i], "_BBN", &bbn)))
                {
            bbn = 0;   // no _BBN => bus 0 (ACPI spec default)
        }
        if ((ULONG)bbn == Bus)
        {
            return candidates[i];
        }
    }
    return NULL;
}

// PCI type-0/type-1 header offsets used by the bridge-aware _PRT walk.
#define UACPI_PCI_CFG_HEADER_TYPE     0x0E   // bit7 multifn, bits0-6: 1 = PCI-PCI bridge
#define UACPI_PCI_CFG_SECONDARY_BUS   0x19   // (bridge) downstream bus number

// Read PCI config space through the PDO's BUS_INTERFACE_STANDARD.
static NTSTATUS
UacpipReadPciConfig(PDEVICE_OBJECT Pdo, ULONG Offset, PVOID Buffer, ULONG Length)
{
    BUS_INTERFACE_STANDARD bif;
    PDEVICE_OBJECT target;
    PIO_STACK_LOCATION sp;
    IO_STATUS_BLOCK iosb;
    KEVENT event;
    PIRP irp;
    NTSTATUS status;

    RtlZeroMemory(&bif, sizeof(bif));
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    target = IoGetAttachedDeviceReference(Pdo);

    irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, target, NULL, 0, NULL,
                                       &event, &iosb);
    if (irp == NULL)
    {
        ObDereferenceObject(target);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    sp = IoGetNextIrpStackLocation(irp);
    sp->MajorFunction = IRP_MJ_PNP;
    sp->MinorFunction = IRP_MN_QUERY_INTERFACE;
    sp->Parameters.QueryInterface.InterfaceType          = &GUID_BUS_INTERFACE_STANDARD;
    sp->Parameters.QueryInterface.Size                   = sizeof(bif);
    sp->Parameters.QueryInterface.Version                = 1;
    sp->Parameters.QueryInterface.Interface              = (PINTERFACE)&bif;
    sp->Parameters.QueryInterface.InterfaceSpecificData  = NULL;

    status = IoCallDriver(target, irp);
    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    ObDereferenceObject(target);

    if (!NT_SUCCESS(status) || bif.GetBusData == NULL)
    {
        return NT_SUCCESS(status) ? STATUS_NOT_SUPPORTED : status;
    }

    if (bif.GetBusData(bif.Context, PCI_WHICHSPACE_CONFIG, Buffer, Offset, Length)
            != Length)
            {
        status = STATUS_UNSUCCESSFUL;
    }
    else
    {
        status = STATUS_SUCCESS;
    }

    if (bif.InterfaceDereference != NULL)
    {
        bif.InterfaceDereference(bif.Context);
    }
    return status;
}

// Config DWORD via the HAL by bus/slot; no IRP. Always a full DWORD read.
static NTSTATUS
UacpipHalReadConfigDword(ULONG Bus, ULONG Slot, ULONG Offset, PULONG Value)
{
    ULONG got;

    *Value = 0;
    // Deprecated, used on purpose: the bus interface needs an IRP.
#pragma warning(suppress: 4996)
    got = HalGetBusDataByOffset(PCIConfiguration, Bus, Slot, Value, Offset, sizeof(ULONG));
    return (got == sizeof(ULONG)) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

// (BaseClass << 8 | SubClass) of a PDO, or 0xFFFF if unreadable.
ULONG
UacpiReadPciClassCode(PDEVICE_OBJECT Pdo)
{
    ULONG dw = 0;   // config dword at 0x08: [31:24]=BaseClass [23:16]=SubClass
    if (Pdo == NULL ||
        !NT_SUCCESS(UacpipReadPciConfig(Pdo, 0x08, &dw, sizeof(dw))))
        {
        return 0xFFFF;
    }
    return (((dw >> 24) & 0xFF) << 8) | ((dw >> 16) & 0xFF);
}

// Find the ACPI-filtered PCI-PCI bridge whose secondary bus is Bus.
static uacpi_namespace_node *
UacpipFindBridgeNodeForBus(ULONG Bus, PDEVICE_OBJECT *BridgePdo)
{
    PLIST_ENTRY e;
    struct { uacpi_namespace_node *Node; PDEVICE_OBJECT Pdo; } cand[32];
    ULONG count = 0, i;
    uacpi_namespace_node *found = NULL;

    *BridgePdo = NULL;
    if (g_AcpiFdo == NULL)
    {
        return NULL;
    }

    // Snapshot the filter (bridge) list under the lock; read config outside it.
    ExAcquireFastMutex(&g_AcpiFdo->ChildLock);
    for (e = g_AcpiFdo->Filters.Flink;
         e != &g_AcpiFdo->Filters && count < RTL_NUMBER_OF(cand);
         e = e->Flink)
         {
        PUACPI_FILTER f = CONTAINING_RECORD(e, UACPI_FILTER, Link);
        if (f->ForeignPdo != NULL && f->Node != NULL)
        {
            cand[count].Node = f->Node;
            cand[count].Pdo  = f->ForeignPdo;
            count++;
        }
    }
    ExReleaseFastMutex(&g_AcpiFdo->ChildLock);

    for (i = 0; i < count; i++)
    {
        UCHAR headerType = 0, secondary = 0;

        if (!NT_SUCCESS(UacpipReadPciConfig(cand[i].Pdo, UACPI_PCI_CFG_HEADER_TYPE,
                                           &headerType, 1)))
                                           {
            continue;
        }
        if ((headerType & 0x7F) != 1)   // not a PCI-PCI bridge (type-1) header
        {
            continue;
        }
        if (!NT_SUCCESS(UacpipReadPciConfig(cand[i].Pdo, UACPI_PCI_CFG_SECONDARY_BUS,
                                           &secondary, 1)))
                                           {
            continue;
        }
        if ((ULONG)secondary == Bus)
        {
            found = cand[i].Node;
            *BridgePdo = cand[i].Pdo;
            break;
        }
    }
    return found;
}

// PCI interrupt links (PNP0C0F): decided once on first use, _SRS'd, cached.
#define UACPI_IRQ_LINK_MAX 32
#define UACPI_IRQ_LINK_NONE ((ULONG)-1)

typedef struct _ACPI_IRQ_LINK
{
    uacpi_namespace_node *Node;
    ULONG                 Gsiv;      // UACPI_IRQ_LINK_NONE until decided
} UACPI_IRQ_LINK, *PUACPI_IRQ_LINK;

static UACPI_IRQ_LINK UacpiIrqLinks[UACPI_IRQ_LINK_MAX];

// Most links declare well under this many candidates in _PRS.
#define UACPI_LINK_CAND_MAX 16

// Rotates each link's starting index into its _PRS list to spread links.
static ULONG UacpiIrqPciAlternativeRotation;

// PCIDeviceExclusionMask: lines barred for PCI links. GSIV < 16 only.
static USHORT UacpiIrqPciExclusionMask;

// SCI GSIV, set at init. Never steer a PCI link onto it.
static ULONG UacpiIrqSciGsiv = (ULONG)-1;

static VOID
UacpipIrqPolicyConfigure(VOID)
{
    UNICODE_STRING path;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    ULONG len = 0;

    RtlInitUnicodeString(&path,
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\ACPI\\Parameters");
    InitializeObjectAttributes(&oa, &path,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa)))
    {
        return;
    }

    RtlInitUnicodeString(&name, L"PCIDeviceExclusionMask");
    if (NT_SUCCESS(ZwQueryValueKey(key, &name, KeyValuePartialInformation,
                                   info, sizeof(buffer), &len)) &&
        info->Type == REG_DWORD && info->DataLength >= sizeof(USHORT))
        {

        UacpiIrqPciExclusionMask = *(PUSHORT)info->Data;
        UacpiTrace("[acpi] irqarb: PCI link exclusion mask 0x%04X\n",
                  UacpiIrqPciExclusionMask);
    }

    ZwClose(key);
}

// The Index-th interrupt descriptor of the link's _CRS, verbatim.
static BOOLEAN UacpipLinkNodeGsivRaw(uacpi_namespace_node *Link, ULONG Index,
                                    PULONG Gsiv);

// Find or create this link's cache slot. NULL when the table is full.
static PUACPI_IRQ_LINK
UacpipLinkSlot(uacpi_namespace_node *Node, BOOLEAN Create)
{
    ULONG i;

    for (i = 0; i < UACPI_IRQ_LINK_MAX; i++)
    {
        if (UacpiIrqLinks[i].Node == Node)
        {
            return &UacpiIrqLinks[i];
        }
    }
    if (!Create)
    {
        return NULL;
    }
    for (i = 0; i < UACPI_IRQ_LINK_MAX; i++)
    {
        if (UacpiIrqLinks[i].Node == NULL)
        {
            UacpiIrqLinks[i].Node = Node;
            UacpiIrqLinks[i].Gsiv = UACPI_IRQ_LINK_NONE;
            return &UacpiIrqLinks[i];
        }
    }
    return NULL;
}

// The link's _PRS candidates, in declaration order.
static ULONG
UacpipLinkCandidates(uacpi_namespace_node *Link, PULONG Cand, ULONG Max)
{
    uacpi_resources *res = NULL;
    uacpi_resource *r;
    ULONG count = 0;

    if (uacpi_unlikely_error(uacpi_get_possible_resources(Link, &res)) || res == NULL)
    {
        return 0;
    }
    for (r = res->entries; r->type != UACPI_RESOURCE_TYPE_END_TAG;
         r = UACPI_NEXT_RESOURCE(r))
         {
        ULONG n, i;
        BOOLEAN ext = (BOOLEAN)(r->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ);

        if (r->type == UACPI_RESOURCE_TYPE_IRQ)
        {
            n = r->irq.num_irqs;
        }
        else if (ext)
        {
            n = r->extended_irq.num_irqs;
        }
        else
        {
            continue;
        }
        for (i = 0; i < n && count < Max; i++)
        {
            Cand[count++] = ext ? r->extended_irq.irqs[i] : r->irq.irqs[i];
        }
        break;      // a link declares one IRQ resource; it is the choice set
    }
    uacpi_free_resources(res);
    return count;
}

// Program the link with _SRS, using its own _CRS as the template.
static NTSTATUS
UacpipLinkProgram(uacpi_namespace_node *Link, ULONG Gsiv)
{
    uacpi_resources *res = NULL;
    uacpi_resource *r;
    uacpi_status ust;

    if (uacpi_unlikely_error(uacpi_get_current_resources(Link, &res)) || res == NULL)
    {
        return STATUS_NOT_FOUND;
    }
    for (r = res->entries; r->type != UACPI_RESOURCE_TYPE_END_TAG;
         r = UACPI_NEXT_RESOURCE(r))
         {
        if (r->type == UACPI_RESOURCE_TYPE_IRQ && r->irq.num_irqs >= 1)
        {
            r->irq.num_irqs = 1;
            r->irq.irqs[0] = (uacpi_u8)Gsiv;
            break;
        }
        if (r->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ &&
            r->extended_irq.num_irqs >= 1)
            {
            r->extended_irq.num_irqs = 1;
            r->extended_irq.irqs[0] = Gsiv;
            break;
        }
    }
    ust = uacpi_set_resources(Link, res);
    uacpi_free_resources(res);
    return uacpi_likely_success(ust) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

// Is some other link already sitting on this line?
static BOOLEAN
UacpipGsivHeldByLink(ULONG Gsiv)
{
    ULONG i;

    for (i = 0; i < UACPI_IRQ_LINK_MAX; i++)
    {
        if (UacpiIrqLinks[i].Node != NULL && UacpiIrqLinks[i].Gsiv == Gsiv)
        {
            return TRUE;
        }
    }
    return FALSE;
}

// Pick the link's line by _PRS rotation. FALSE if there is no usable _PRS.
static BOOLEAN
UacpipLinkChoose(uacpi_namespace_node *Link, ULONG Current, PULONG Chosen)
{
    ULONG cand[UACPI_LINK_CAND_MAX];
    ULONG count, i, start, pass;

    UNREFERENCED_PARAMETER(Current);

    count = UacpipLinkCandidates(Link, cand, UACPI_LINK_CAND_MAX);
    if (count == 0)
    {
        return FALSE;
    }

    start = UacpiIrqPciAlternativeRotation;

    // Pass 0 skips lines held by other links; pass 1 is plain rotation.
    for (pass = 0; pass < 2; pass++)
    {
        for (i = 0; i < count; i++)
        {
            ULONG gsiv = cand[(start + i) % count];

            if (gsiv == UacpiIrqSciGsiv)
            {
                continue;       // never route PCI interrupts through the SCI
            }
            if (gsiv < 16 && (UacpiIrqPciExclusionMask & (1u << gsiv)) != 0)
            {
                continue;       // platform forbids this line for PCI
            }
            if (pass == 0 && UacpipGsivHeldByLink(gsiv))
            {
                continue;       // another link is already here
            }

            // Rotation advances in UacpiIrqGetNextAllocationRange, per range.
            *Chosen = gsiv;
            return TRUE;
        }
    }
    return FALSE;
}

// Decide, program and cache a link's line. Idempotent; also called by pdo.c.
BOOLEAN
UacpiIrqLinkDecide(uacpi_namespace_node *Node, PULONG Gsiv)
{
    PUACPI_IRQ_LINK slot = UacpipLinkSlot(Node, TRUE);
    ULONG chosen = 0, current;

    if (slot == NULL)
    {
        return FALSE;
    }
    if (slot->Gsiv != UACPI_IRQ_LINK_NONE)
    {
        *Gsiv = slot->Gsiv;
        return TRUE;
    }

    if (!UacpipLinkNodeGsivRaw(Node, 0, &current))
    {
        current = UACPI_IRQ_LINK_NONE;   // link currently reports nothing usable
    }

    if (!UacpipLinkChoose(Node, current, &chosen))
    {
        if (current == UACPI_IRQ_LINK_NONE)
        {
            return FALSE;
        }
        chosen = current;               // no _PRS: the firmware line stands
    }

    if (chosen != current)
    {
        if (NT_SUCCESS(UacpipLinkProgram(Node, chosen)))
        {
            UacpiTrace("[acpi] link: %p GSIV %u -> %u (rot %u)\n",
                      Node, current, chosen, UacpiIrqPciAlternativeRotation);
        }
        else
        {
            UacpiTrace("[acpi] link: %p _SRS to GSIV %u FAILED, staying on %u\n",
                      Node, chosen, current);
            if (current == UACPI_IRQ_LINK_NONE)
            {
                return FALSE;
            }
            chosen = current;
        }
    }

    slot->Gsiv = chosen;
    *Gsiv = chosen;
    return TRUE;
}

// Resume: re-run _SRS on every decided PCI link, PIC model only.
//
// A firmware reset on the way out of S1-S4 can revert each link to its power-on
// line. Under APIC routing the interrupts land in IO-APIC redirection entries
// the HAL restores, so there is nothing to do; under PIC routing the link
// registers are ours, and without this a resumed device's line is lost and a
// PIC-model box can hang on the first disk interrupt.
VOID
UacpiIrqLinksResume(VOID)
{
    ULONG i;

    if (g_AcpiInterruptModel != 0)
    {
        return;
    }
    for (i = 0; i < UACPI_IRQ_LINK_MAX; i++)
    {
        PUACPI_IRQ_LINK slot = &UacpiIrqLinks[i];

        if (slot->Node == NULL || slot->Gsiv == UACPI_IRQ_LINK_NONE)
        {
            continue;
        }
        if (NT_SUCCESS(UacpipLinkProgram(slot->Node, slot->Gsiv)))
        {
            UacpiTrace("[acpi] link: %p _SRS restored to GSIV %u on resume\n",
                      slot->Node, slot->Gsiv);
        }
        else
        {
            UacpiTrace("[acpi] link: %p _SRS restore to GSIV %u FAILED\n",
                      slot->Node, slot->Gsiv);
        }
    }
}

// Resolve a _PRT link reference to its GSIV, deciding the link if needed.
static NTSTATUS
UacpipLinkNodeGsiv(uacpi_namespace_node *Link, ULONG Index, PULONG Gsiv)
{
    // Index > 0 is not a steerable link; read it from _CRS.
    if (Index != 0)
    {
        return UacpipLinkNodeGsivRaw(Link, Index, Gsiv) ? STATUS_SUCCESS
                                                       : STATUS_NOT_FOUND;
    }
    return UacpiIrqLinkDecide(Link, Gsiv) ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

static BOOLEAN
UacpipLinkNodeGsivRaw(uacpi_namespace_node *Link, ULONG Index, PULONG Gsiv)
{
    uacpi_resources *res = NULL;
    uacpi_resource *r;
    ULONG n = 0;
    NTSTATUS status = STATUS_NOT_FOUND;

    if (uacpi_unlikely_error(uacpi_get_current_resources(Link, &res)) || res == NULL)
    {
        return FALSE;
    }

    for (r = res->entries; r->type != UACPI_RESOURCE_TYPE_END_TAG;
         r = UACPI_NEXT_RESOURCE(r))
         {
        if (r->type == UACPI_RESOURCE_TYPE_IRQ)
        {
            if (n++ == Index && r->irq.num_irqs > 0)
            {
                *Gsiv = r->irq.irqs[0];
                status = STATUS_SUCCESS;
                break;
            }
        }
        else if (r->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ)
        {
            if (n++ == Index && r->extended_irq.num_irqs > 0)
            {
                *Gsiv = r->extended_irq.irqs[0];
                status = STATUS_SUCCESS;
                break;
            }
        }
    }

    uacpi_free_resources(res);
    return (BOOLEAN)NT_SUCCESS(status);
}

// Look up (Device, Pin) in Node's _PRT. Pin is 1-based, _PRT pins 0-based.
static NTSTATUS
UacpipCrackPrt(uacpi_namespace_node *Node, ULONG Device, UCHAR Pin, PULONG Gsiv)
{
    uacpi_pci_routing_table *prt = NULL;
    NTSTATUS status = STATUS_NOT_FOUND;
    uacpi_size i;

    if (uacpi_unlikely_error(uacpi_get_pci_routing_table(Node, &prt)) || prt == NULL)
    {
        if (UacpiIrqArbVerbose)
        {
            UacpiTrace("[acpi] prtdump: node %p has NO _PRT (looking for dev %u INT%c)\n",
                      Node, Device, (char)('A' + (Pin - 1)));
        }
        return STATUS_NOT_FOUND;
    }
    if (UacpiIrqArbVerbose)
    {
        // Dump every entry for the target device.
        UacpiTrace("[acpi] prtdump: node %p _PRT has %u entries; matches for dev %u:\n",
                  Node, (ULONG)prt->num_entries, Device);
        for (i = 0; i < prt->num_entries; i++)
        {
            uacpi_pci_routing_table_entry *e = &prt->entries[i];
            if ((e->address >> 16) != Device)
            {
                continue;
            }
            UacpiTrace("[acpi] prtdump:   addr 0x%X pin INT%c source %p index %u\n",
                      (ULONG)e->address, (char)('A' + (e->pin & 3)),
                      e->source, (ULONG)e->index);
        }
    }
    for (i = 0; i < prt->num_entries; i++)
    {
        uacpi_pci_routing_table_entry *e = &prt->entries[i];

        // Address: device in the high word, function or 0xFFFF in the low word.
        if ((e->address >> 16) != Device)
        {
            continue;
        }
        if ((e->address & 0xFFFF) != 0xFFFF && (e->address & 0xFFFF) != 0)
        {
            // Only the all-functions form is matched.
            continue;
        }
        if (e->pin != (uacpi_u8)(Pin - 1))
        {
            continue;
        }
        if (e->source == NULL)
        {
            *Gsiv = e->index;                 // fixed GSIV entry
            status = STATUS_SUCCESS;
        }
        else
        {
            status = UacpipLinkNodeGsiv(e->source, e->index, Gsiv);   // link node _CRS
        }
        break;
    }
    uacpi_free_pci_routing_table(prt);
    return status;
}

// PDO to routed GSIV, walking up through PCI-PCI bridges.
static NTSTATUS
UacpipPrtResolveGsiv(PDEVICE_OBJECT DevicePdo, PULONG Gsiv)
{
    ULONG bus = 0, address = 0, length = 0;
    ULONG device;
    UCHAR pin = 0;
    NTSTATUS status;
    ULONG depth;

    // Device coordinates: bus + (device << 16 | function).
    status = IoGetDeviceProperty(DevicePdo, DevicePropertyBusNumber,
                                 sizeof(bus), &bus, &length);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    status = IoGetDeviceProperty(DevicePdo, DevicePropertyAddress,
                                 sizeof(address), &address, &length);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    device = address >> 16;

    // Pin via the HAL (dword 0x3C, bits 15:8); a bus interface IRP can fail.
    {
        ULONG pinDw = 0;
        ULONG slot = (device & 0x1F) | ((address & 0x7) << 5);

        if (!NT_SUCCESS(UacpipHalReadConfigDword(bus, slot, 0x3C, &pinDw)))
        {
            return STATUS_NOT_FOUND;
        }
        pin = (UCHAR)((pinDw >> 8) & 0xFF);
    }
    if (pin == 0 || pin > 4)
    {
        return STATUS_NOT_FOUND;   // no interrupt pin => nothing to route
    }

    if (UacpiIrqArbVerbose)
    {
        UacpiTrace("[acpi] prtwalk: PDO %p start bus %u dev %u func %u pin INT%c\n",
                  DevicePdo, bus, device, (address & 0xFFFF),
                  (char)('A' + (pin - 1)));
    }

    // Try the producing bridge's _PRT; on a miss transform the pin and climb.
    for (depth = 0; depth < 8; depth++)
    {
        uacpi_namespace_node *root = UacpipFindPciRootNode(bus);
        if (root != NULL)
        {
            status = UacpipCrackPrt(root, device, pin, Gsiv);   // host bridge _PRT
            if (UacpiIrqArbVerbose)
            {
                UacpiTrace("[acpi] prtwalk:   host-bridge bus %u _PRT(dev %u, INT%c) "
                          "-> 0x%X gsiv %u\n", bus, device, (char)('A' + (pin - 1)),
                          status, NT_SUCCESS(status) ? *Gsiv : 0);
            }
            break;
        }
        {
            PDEVICE_OBJECT bridgePdo = NULL;
            uacpi_namespace_node *bridge = UacpipFindBridgeNodeForBus(bus, &bridgePdo);
            ULONG bridgeAddr = 0;
            UCHAR oldPin = pin;

            if (bridge == NULL || bridgePdo == NULL)
            {
                if (UacpiIrqArbVerbose)
                {
                    UacpiTrace("[acpi] prtwalk:   no ACPI bridge produces bus %u "
                              "-> NOT_FOUND\n", bus);
                }
                return STATUS_NOT_FOUND;   // producer not an ACPI-known bridge
            }
            // This bridge's own _PRT may route its secondary-bus devices directly.
            status = UacpipCrackPrt(bridge, device, pin, Gsiv);
            if (NT_SUCCESS(status))
            {
                if (UacpiIrqArbVerbose)
                {
                    UacpiTrace("[acpi] prtwalk:   bridge(sec bus %u) _PRT(dev %u, INT%c) "
                              "-> gsiv %u\n", bus, device, (char)('A' + (pin - 1)), *Gsiv);
                }
                break;
            }
            // No entry: subclass 4 swizzles, subclass 7 uses the bridge pin.
            if (!NT_SUCCESS(IoGetDeviceProperty(bridgePdo, DevicePropertyBusNumber,
                                                sizeof(bus), &bus, &length)) ||
                !NT_SUCCESS(IoGetDeviceProperty(bridgePdo, DevicePropertyAddress,
                                                sizeof(bridgeAddr), &bridgeAddr, &length)))
                                                {
                return STATUS_NOT_FOUND;
            }
            {
                ULONG bridgeSlot = ((bridgeAddr >> 16) & 0x1F) | ((bridgeAddr & 0x7) << 5);
                ULONG classDw = 0;
                UCHAR subClass;

                if (!NT_SUCCESS(UacpipHalReadConfigDword(bus, bridgeSlot, 0x08, &classDw)))
                {
                    if (UacpiIrqArbVerbose)
                    {
                        UacpiTrace("[acpi] prtwalk:   bridge(sec bus %u) class read "
                                  "failed -> NOT_FOUND\n", bus);
                    }
                    return STATUS_NOT_FOUND;
                }
                subClass = (UCHAR)((classDw >> 16) & 0xFF);   // config 0x0A

                if (subClass == 4)
                {
                    pin = (UCHAR)(((device + (pin - 1)) & 3) + 1);      // swizzle
                }
                else if (subClass == 7)
                {
                    ULONG pinDw = 0;
                    UCHAR bridgePin;
                    if (!NT_SUCCESS(UacpipHalReadConfigDword(bus, bridgeSlot, 0x3C, &pinDw)))
                    {
                        return STATUS_NOT_FOUND;
                    }
                    bridgePin = (UCHAR)((pinDw >> 8) & 0xFF);           // InterruptPin @0x3D
                    if (bridgePin == 0 || bridgePin > 4)
                    {
                        return STATUS_NOT_FOUND;
                    }
                    pin = bridgePin;
                }
                else
                {
                    if (UacpiIrqArbVerbose)
                    {
                        UacpiTrace("[acpi] prtwalk:   bridge(sec bus %u) subclass 0x%02X "
                                  "not routable -> NOT_FOUND\n", bus, subClass);
                    }
                    return STATUS_NOT_FOUND;   // only subclasses 4 and 7 route
                }

                if (UacpiIrqArbVerbose)
                {
                    UacpiTrace("[acpi] prtwalk:   no entry on bridge(sec bus)->subclass %u "
                              "%s dev %u INT%c => INT%c, climb to bus %u dev %u\n",
                              subClass, (subClass == 7) ? "bridge-pin" : "swizzle",
                              device, (char)('A' + (oldPin - 1)), (char)('A' + (pin - 1)),
                              bus, (ULONG)(bridgeAddr >> 16));
                }
            }
            device = bridgeAddr >> 16;
        }
    }

    if (NT_SUCCESS(status))
    {
        // _PRT-routed lines are level/active-low.
        UacpiIrqLibNoteLevelGsiv(*Gsiv);
    }
    return status;
}

// WorkSpace: bits 0..7 cursor; 8..31 _PRT (0 unresolved, 1 none, GSIV + 2).
#define UACPI_WS_STATE(ws)        ((ULONG)((ws) & 0xFF))
#define UACPI_WS_PRT(ws)          ((ULONG)((ULONG_PTR)(ws) >> 8))
#define UACPI_WS_SET_STATE(ws, v) \
    ((ws) = (((ws) & ~(ULONG_PTR)0xFF) | (ULONG_PTR)(v)))
#define UACPI_WS_SET_PRT(ws, v)   \
    ((ws) = (((ws) & (ULONG_PTR)0xFF) | ((ULONG_PTR)(v) << 8)))

// GetNextAllocationRange cursor values.
#define UACPI_NEXT_INITIAL       0   // pick a disposition
#define UACPI_NEXT_LINK_PREF     1   // the link's already-decided line
#define UACPI_NEXT_STACK_UP      2   // stacking policy, unused
#define UACPI_NEXT_BOOT_CONFIG   3   // the firmware's boot assignment
#define UACPI_NEXT_ALTERNATIVES  4   // walk the alternatives

// Resolve and cache the routed GSIV in WorkSpace. No clamp; skipped for boot.
static NTSTATUS NTAPI
UacpiIrqPreprocessEntry(PARBITER_INSTANCE Arbiter, PARBITER_ALLOCATION_STATE State)
{
    ULONG gsiv;

    UNREFERENCED_PARAMETER(Arbiter);

    if (State->Entry == NULL || State->Entry->PhysicalDeviceObject == NULL)
    {
        return STATUS_SUCCESS;
    }
    if (State->Flags & ARBITER_STATE_FLAG_BOOT)
    {
        // Boot reservation stays on the firmware IRQ.
        return STATUS_SUCCESS;
    }

    if (UACPI_WS_PRT(State->WorkSpace) == 0)
    {
        ULONG_PTR ws = State->WorkSpace;

        UACPI_WS_SET_PRT(ws,
            NT_SUCCESS(UacpipPrtResolveGsiv(State->Entry->PhysicalDeviceObject, &gsiv))
                ? ((ULONG_PTR)gsiv + 2) : 1);
        State->WorkSpace = ws;

        if (UACPI_WS_PRT(ws) >= 2)
        {
            UacpiTrace("[acpi] irqarb: PDO %p _PRT-routed to GSIV %u\n",
                      State->Entry->PhysicalDeviceObject,
                      UACPI_WS_PRT(ws) - 2);
        }
    }
    return STATUS_SUCCESS;
}

// GetNextAllocationRange: routed line, then boot config, then the library.

// The range this PDO already holds from its boot configuration.
static BOOLEAN
UacpipFindBootConfig(PARBITER_INSTANCE Arbiter, PARBITER_ALLOCATION_STATE State,
                    PULONG Irq)
{
    RTL_RANGE_LIST_ITERATOR it;
    PRTL_RANGE range;

    if (Arbiter->Allocation == NULL || State->Entry == NULL)
    {
        return FALSE;
    }
    if (!NT_SUCCESS(RtlGetFirstRange(Arbiter->Allocation, &it, &range)))
    {
        return FALSE;
    }
    while (range != NULL)
    {
        // Our arbiter library marks boot ranges ARBITER_RANGE_BOOT_ALLOCATED.
        if ((range->Attributes & ARBITER_RANGE_BOOT_ALLOCATED) != 0 &&
            range->Owner == State->Entry->PhysicalDeviceObject)
            {
            *Irq = (ULONG)range->Start;
            return TRUE;
        }
        if (!NT_SUCCESS(RtlGetNextRange(&it, &range, TRUE)))
        {
            break;
        }
    }
    return FALSE;
}

// Which alternative can hold this line?
static BOOLEAN
UacpipFindIrqInAlternatives(PARBITER_ALLOCATION_STATE State, ULONG Irq,
                           PULONG Index)
{
    ULONG i;

    for (i = 0; i < State->AlternativeCount; i++)
    {
        if (State->Alternatives[i].Minimum <= (ULONGLONG)Irq &&
            State->Alternatives[i].Maximum >= (ULONGLONG)Irq)
            {
            *Index = i;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN NTAPI
UacpiIrqGetNextAllocationRange(PARBITER_INSTANCE Arbiter,
                              PARBITER_ALLOCATION_STATE State)
{
    PARBITER_ALTERNATIVE alt;
    ULONG_PTR ws;
    ULONG preferred = 0, index = 0;
    BOOLEAN havePreferred;

    alt = (State->CurrentAlternative != NULL) ? State->CurrentAlternative
                                              : State->Alternatives;

    // Message requirements have no preference; use the library walk.
    if (State->AlternativeCount == 0 || alt == NULL || alt->Descriptor == NULL ||
        (alt->Descriptor->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) != 0)
        {
        return ArbiterLibGetNextAllocationRange(Arbiter, State);
    }

    // Preferences apply only to devices routed through a _PRT.
    ws = State->WorkSpace;
    if (UACPI_WS_PRT(ws) < 2)
    {
        return ArbiterLibGetNextAllocationRange(Arbiter, State);
    }

    while (UACPI_WS_STATE(ws) < UACPI_NEXT_ALTERNATIVES)
    {
        havePreferred = FALSE;

        if (UACPI_WS_STATE(ws) == UACPI_NEXT_INITIAL)
        {
            // Default disposition; UACPI_NEXT_STACK_UP is unreachable.
            UACPI_WS_SET_STATE(ws, UACPI_NEXT_LINK_PREF);
            continue;
        }

        if (UACPI_WS_STATE(ws) == UACPI_NEXT_LINK_PREF)
        {
            UACPI_WS_SET_STATE(ws, UACPI_NEXT_BOOT_CONFIG);
            preferred = UACPI_WS_PRT(ws) - 2;
            havePreferred = TRUE;
        }
        else
        {
            UACPI_WS_SET_STATE(ws, UACPI_NEXT_ALTERNATIVES);
            havePreferred = UacpipFindBootConfig(Arbiter, State, &preferred);
        }

        if (havePreferred &&
            UacpipFindIrqInAlternatives(State, preferred, &index))
            {
            State->CurrentMinimum     = preferred;
            State->CurrentMaximum     = preferred;
            State->CurrentAlternative = &State->Alternatives[index];
            State->WorkSpace          = ws;
            UacpiIrqPciAlternativeRotation++;
            return TRUE;
        }
    }

    // Preferences exhausted; the library walk keeps its priority state.
    State->WorkSpace = ws;
    if (!ArbiterLibGetNextAllocationRange(Arbiter, State))
    {
        return FALSE;
    }
    UacpiIrqPciAlternativeRotation++;
    return TRUE;
}

// Record level triggering from the LATCHED bit, before commit.
static VOID
UacpipNotePlacedGsivTriggering(PARBITER_ALLOCATION_STATE State)
{
    PIO_RESOURCE_DESCRIPTOR desc =
        (State->CurrentAlternative != NULL) ? State->CurrentAlternative->Descriptor : NULL;

    if (desc != NULL && State->Start < UACPI_MSI_GSIV_BASE &&
        !(desc->Flags & CM_RESOURCE_INTERRUPT_LATCHED) &&
        !UacpiIrqLibGsivForcedEdge((ULONG)State->Start))   // a MADT ISO edge wins
        {
        UacpiIrqLibNoteLevelGsiv((ULONG)State->Start);   // level-sensitive/active-low
    }
}

static BOOLEAN NTAPI
UacpiIrqFindSuitableRange(PARBITER_INSTANCE Arbiter, PARBITER_ALLOCATION_STATE State)
{
    // MSI window: no _PRT applies; generic first fit.
    if (State->CurrentMinimum >= UACPI_MSI_GSIV_BASE)
    {
        return ArbiterLibFindSuitableRange(Arbiter, State);
    }
    if (UACPI_WS_PRT(State->WorkSpace) >= 2)
    {
        ULONG gsiv = UACPI_WS_PRT(State->WorkSpace) - 2;

        // The routed GSIV must fall in the offered window.
        if (State->CurrentMinimum <= gsiv && gsiv <= State->CurrentMaximum)
        {
            State->Start = gsiv;
            State->End   = gsiv;
            UacpipNotePlacedGsivTriggering(State);   // level/edge from the descriptor
            return TRUE;
        }
        return FALSE;
    }

    if (ArbiterLibFindSuitableRange(Arbiter, State))   // ISA / unconstrained
    {
        UacpipNotePlacedGsivTriggering(State);
        return TRUE;
    }
    return FALSE;
}

// The FDO; its own reservations are skipped at commit.
static PDEVICE_OBJECT UacpiIrqArbFdoSelf;

// DIAG: tell a failed placement apart from a later rollback.
static NTSTATUS NTAPI
UacpiIrqTestAllocation(PARBITER_INSTANCE Arbiter,
                      PARBITER_TEST_ALLOCATION_PARAMETERS Parameters)
{
    NTSTATUS status = ArbiterLibTestAllocation(Arbiter, Parameters);
    // Trace passes only; misses are routine probes.
    if (UacpiIrqArbVerbose && NT_SUCCESS(status))
    {
        UacpiTrace("[acpi] irqarb: DIAG TestAllocation PASS (placement ok)\n");
    }
    return status;
}

static NTSTATUS NTAPI
UacpiIrqRollbackAllocation(PARBITER_INSTANCE Arbiter)
{
    if (UacpiIrqArbVerbose)
    {
        UacpiTrace("[acpi] irqarb: DIAG RollbackAllocation (external conflict)\n");
    }
    return ArbiterLibRollbackAllocation(Arbiter);
}

// Last connection data written per owner; unchanged ranges are skipped.
typedef struct _ACPI_IRQ_WRITTEN
{
    PDEVICE_OBJECT Owner;
    ULONG          Start;
    ULONG          Length;
} UACPI_IRQ_WRITTEN;
#define UACPI_IRQ_WRITTEN_MAX 256
static UACPI_IRQ_WRITTEN UacpiIrqWritten[UACPI_IRQ_WRITTEN_MAX];
static ULONG            UacpiIrqWrittenCount;

// TRUE if Owner's assignment differs from its last successful write.
static BOOLEAN
UacpipConnectionDataChanged(PDEVICE_OBJECT Owner, ULONG Start, ULONG Length)
{
    ULONG i;

    // No side effect; the cache is updated only after a successful write.
    for (i = 0; i < UacpiIrqWrittenCount; i++)
    {
        if (UacpiIrqWritten[i].Owner == Owner)
        {
            return (UacpiIrqWritten[i].Start != Start) ||
                   (UacpiIrqWritten[i].Length != Length);
        }
    }
    return TRUE;   // not yet written
}

// Record a successful write. One row per device.
static VOID
UacpipConnectionDataRecord(PDEVICE_OBJECT Owner, ULONG Start, ULONG Length)
{
    ULONG i;

    for (i = 0; i < UacpiIrqWrittenCount; i++)
    {
        if (UacpiIrqWritten[i].Owner == Owner)
        {
            UacpiIrqWritten[i].Start  = Start;
            UacpiIrqWritten[i].Length = Length;
            return;
        }
    }
    if (UacpiIrqWrittenCount < UACPI_IRQ_WRITTEN_MAX)
    {
        UacpiIrqWritten[UacpiIrqWrittenCount].Owner  = Owner;
        UacpiIrqWritten[UacpiIrqWrittenCount].Start  = Start;
        UacpiIrqWritten[UacpiIrqWrittenCount].Length = Length;
        UacpiIrqWrittenCount++;
    }
}

static NTSTATUS NTAPI
UacpiIrqCommitAllocation(PARBITER_INSTANCE Arbiter)
{
    RTL_RANGE_LIST_ITERATOR iterator;
    PRTL_RANGE range;
    NTSTATUS status;

    ULONG wrote = 0;

    status = ArbiterLibCommitAllocation(Arbiter);   // base: PossibleAllocation -> Allocation
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    if (NT_SUCCESS(RtlGetFirstRange(Arbiter->Allocation, &iterator, &range)))
    {
        do
        {
            PDEVICE_OBJECT owner = (PDEVICE_OBJECT)range->Owner;
            ULONG start  = (ULONG)range->Start;
            ULONG length;

            // Vectors needed: range length for a line, recorded count for MSI.
            if (range->Start >= UACPI_MSI_GSIV_BASE)
            {
                if (!NT_SUCCESS(UacpipMessageCountFromAllocation(range->Start,
                                                                 &length)) ||
                    length == 0)
                {
                    UacpiTrace("[acpi] irqarb: WARN - MESSAGE placement 0x%I64X "
                              "has no recorded count, assuming 1\n", range->Start);
                    length = 1;
                }
            }
            else
            {
                length = (ULONG)(range->End - range->Start + 1);
            }

            // Skip FDO reservations and ranges unchanged since the last write.
            if (owner != NULL && owner != UacpiIrqArbFdoSelf &&
                UacpipConnectionDataChanged(owner, start, length))
                {
                // Allocates `length` IDT vectors; failures are traced.
                NTSTATUS ws = UacpiIrqLibWriteConnectionData(owner, start, length);
                if (NT_SUCCESS(ws))
                {
                    UacpipConnectionDataRecord(owner, start, length);   // cache only on success
                    wrote++;
                }
                else
                {
                    // Leave the cache stale; retried next commit.
                    UacpiTrace("[acpi] irqarb: WARN - connection-data write for PDO %p "
                              "GSIV/msg %u failed 0x%X (will retry next commit)\n",
                              owner, start, ws);
                }
            }
        } while (NT_SUCCESS(RtlGetNextRange(&iterator, &range, TRUE)));
    }
    if (wrote != 0)
    {
        UacpiTrace("[acpi] irqarb: commit wrote connection data for %u new/changed "
                  "device(s)\n", wrote);
    }
    return status;
}

// Instance lifecycle + QUERY_INTERFACE provisioning.
NTSTATUS
UacpiIrqArbiterInitialize(PUACPI_FDO Fdo)
{
    NTSTATUS status;

    if (UacpiIrqArbUp)
    {
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&UacpiIrqArbiter, sizeof(UacpiIrqArbiter));

    UacpipIrqPolicyConfigure();

    // Set callbacks first; the library only fills NULL slots.
    UacpiIrqArbiter.UnpackRequirement = UacpiIrqUnpackRequirement;
    UacpiIrqArbiter.PackResource      = UacpiIrqPackResource;
    UacpiIrqArbiter.UnpackResource    = UacpiIrqUnpackResource;
    UacpiIrqArbiter.ScoreRequirement  = UacpiIrqScoreRequirement;
    UacpiIrqArbiter.PreprocessEntry   = UacpiIrqPreprocessEntry;
    UacpiIrqArbiter.GetNextAllocationRange = UacpiIrqGetNextAllocationRange;
    UacpiIrqArbiter.FindSuitableRange = UacpiIrqFindSuitableRange;  // PCI _PRT derivation
    UacpiIrqArbiter.CommitAllocation  = UacpiIrqCommitAllocation;   // writes connection data
    UacpiIrqArbiter.TestAllocation    = UacpiIrqTestAllocation;     // DIAG
    UacpiIrqArbiter.RollbackAllocation = UacpiIrqRollbackAllocation; // DIAG
    UacpiIrqArbFdoSelf = Fdo->Common.Self;

    status = ArbiterLibInitializeInstance(&UacpiIrqArbiter, Fdo->Common.Self,
                                          CmResourceTypeInterrupt,
                                          // "Root" AllocationOrder, as resarb.c
                                          L"ACPI_IRQ", L"Root", NULL);
    if (!NT_SUCCESS(status))
    {
        UacpiTrace("[acpi] irqarb: ArbiterLibInitializeInstance failed 0x%X\n", status);
        return status;
    }

    // Pre-claim the SCI GSI (FADT sci_int) as shared, owned by the FDO.
    {
        struct acpi_fadt *fadt = NULL;
        ULONG sciGsi = (ULONG)-1;

        if (uacpi_likely_success(uacpi_table_fadt(&fadt)) && fadt != NULL)
        {
            sciGsi = fadt->sci_int;
        }
        // No fallback; the FADT is the only source of the SCI GSI.
        if (sciGsi != (ULONG)-1)
        {
            (void)RtlAddRange(UacpiIrqArbiter.Allocation,
                              sciGsi, sciGsi,
                              0, RTL_RANGE_LIST_ADD_SHARED, NULL, Fdo->Common.Self);
            UacpiIrqLibNoteLevelGsiv(sciGsi);   // the SCI is level/active-low
            UacpiIrqSciGsiv = sciGsi;           // and off-limits to PCI links
        }

        // PIC model: claim GSIV 16 and up, stopping below the MSI window.
        if (g_AcpiInterruptModel == 0)
        {
            (void)RtlAddRange(UacpiIrqArbiter.Allocation,
                              16, 0xFFEFFFFFull,
                              0, 0 /* exclusive */, NULL, Fdo->Common.Self);
        }

        UacpiIrqArbUp = TRUE;
        UacpiMsiDiagArm();
        UacpiTrace("[acpi] irqarb: IRQ arbiter up (%s model, SCI GSI %u %s)\n",
                  g_AcpiInterruptModel == 1 ? "APIC" : "PIC",
                  (sciGsi != (ULONG)-1) ? sciGsi : 0,
                  (sciGsi != (ULONG)-1) ? "reserved shared" : "unknown");
    }
    return STATUS_SUCCESS;
}

static VOID NTAPI UacpiIrqArbRef(PVOID c)   { UNREFERENCED_PARAMETER(c); }
static VOID NTAPI UacpiIrqArbDeref(PVOID c) { UNREFERENCED_PARAMETER(c); }

// Fill an ARBITER_INTERFACE for interrupt arbiter queries, else NOT_SUPPORTED.
NTSTATUS
UacpiQueryIrqArbiter(PIO_STACK_LOCATION sp)
{
    PARBITER_INTERFACE ai = (PARBITER_INTERFACE)sp->Parameters.QueryInterface.Interface;

    if (!UacpiIrqArbEnabled || !UacpiIrqArbUp)
    {
        return STATUS_NOT_SUPPORTED;
    }
    if (!IsEqualGUID(sp->Parameters.QueryInterface.InterfaceType,
                     &GUID_ARBITER_INTERFACE_STANDARD))
                     {
        return STATUS_NOT_SUPPORTED;   // not an arbiter query at all
    }
    if ((ULONG_PTR)sp->Parameters.QueryInterface.InterfaceSpecificData !=
        CmResourceTypeInterrupt)
        {
        return STATUS_NOT_SUPPORTED;   // we only arbitrate interrupts
    }
    if (sp->Parameters.QueryInterface.Size < sizeof(ARBITER_INTERFACE))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    ai->Size                 = sizeof(ARBITER_INTERFACE);
    ai->Version              = 1;
    ai->Context              = &UacpiIrqArbiter;
    ai->InterfaceReference   = UacpiIrqArbRef;
    ai->InterfaceDereference = UacpiIrqArbDeref;
    ai->ArbiterHandler       = ArbiterLibHandler;
    ai->Flags                = 0;      // full arbiter (not ARBITER_PARTIAL)

    UacpiTrace("[acpi] irqarb: provided ARBITER_INTERFACE (interrupt)\n");
    return STATUS_SUCCESS;
}

// MSI/MSI-X readback diagnostic, run once from a timer after devices start.
int UacpiMsiDiagEnabled = 1;
int UacpiMsiDiagDelaySeconds = 30;

static KTIMER           UacpiMsiDiagTimer;
static KDPC             UacpiMsiDiagDpc;
static WORK_QUEUE_ITEM  UacpiMsiDiagWork;
static LONG             UacpiMsiDiagArmed;

// Dump the MSI-X table through a private read-only mapping.
static VOID
UacpipDumpMsiXTable(PDEVICE_OBJECT Pdo, const char *Name, ULONG Bir,
                   ULONG TableOffset, ULONG Entries)
{
    PHYSICAL_ADDRESS phys;
    ULONG   barLo = 0, barHi = 0;
    ULONG   i, shown, unmasked = 0, programmed = 0;
    ULONG   bytes;
    PUCHAR  base;

    if (Entries == 0 || Entries > 256 || Bir > 5)
    {
        return;
    }
    if (!NT_SUCCESS(UacpipReadPciConfig(Pdo, 0x10 + Bir * 4, &barLo, sizeof(barLo))))
    {
        return;
    }
    if (barLo & 1)
    {
        UacpiTrace("[acpi] msidiag: %s     table BAR%u is I/O space - not readable here\n",
                  Name, Bir);
        return;
    }
    if (((barLo >> 1) & 3) == 2)   // 64-bit BAR: high half follows
    {
        (void)UacpipReadPciConfig(Pdo, 0x10 + (Bir + 1) * 4, &barHi, sizeof(barHi));
    }

    phys.LowPart  = (barLo & ~0xFul) + TableOffset;
    phys.HighPart = (LONG)barHi;
    if (phys.LowPart == TableOffset && barHi == 0)
    {
        UacpiTrace("[acpi] msidiag: %s     table BAR%u not assigned\n", Name, Bir);
        return;
    }

    bytes = Entries * 16;
    base = (PUCHAR)MmMapIoSpace(phys, bytes, MmNonCached);
    if (base == NULL)
    {
        UacpiTrace("[acpi] msidiag: %s     table map of 0x%08X%08X (%u bytes) failed\n",
                  Name, (ULONG)phys.HighPart, phys.LowPart, bytes);
        return;
    }

    UacpiTrace("[acpi] msidiag: %s     MSI-X table @ 0x%08X%08X, %u entries\n",
              Name, (ULONG)phys.HighPart, phys.LowPart, Entries);

    shown = (Entries > 32) ? 32 : Entries;
    for (i = 0; i < Entries; i++)
    {
        ULONG addrLo = READ_REGISTER_ULONG((PULONG)(base + i * 16 + 0));
        ULONG addrHi = READ_REGISTER_ULONG((PULONG)(base + i * 16 + 4));
        ULONG data   = READ_REGISTER_ULONG((PULONG)(base + i * 16 + 8));
        ULONG ctl    = READ_REGISTER_ULONG((PULONG)(base + i * 16 + 12));

        if (addrLo != 0 || addrHi != 0)
        {
            programmed++;
        }
        if ((ctl & 1) == 0)
        {
            unmasked++;
        }
        if (i < shown)
        {
            UacpiTrace("[acpi] msidiag: %s       [%02u] addr 0x%08X%08X data 0x%04X "
                      "(vec 0x%02X) ctl 0x%X %s\n",
                      Name, i, addrHi, addrLo, (USHORT)data, (ULONG)(data & 0xFF),
                      ctl, (ctl & 1) ? "masked" : "UNMASKED");
        }
    }
    UacpiTrace("[acpi] msidiag: %s     summary: %u/%u entries programmed, %u unmasked\n",
              Name, programmed, Entries, unmasked);

    MmUnmapIoSpace(base, bytes);
}

// Decode a function's capability list. PASSIVE_LEVEL only.
static VOID
UacpipDumpMsiState(PDEVICE_OBJECT Pdo, const char *Name)
{
    ULONG   ids = 0, cmdsts = 0, cls = 0, capdw = 0, dw = 0;
    ULONG   guard;
    UCHAR   cap;
    BOOLEAN found = FALSE;

    if (Pdo == NULL)
    {
        return;
    }
    if (!NT_SUCCESS(UacpipReadPciConfig(Pdo, 0x00, &ids, sizeof(ids))))
    {
        UacpiTrace("[acpi] msidiag: %s PDO %p config space unreadable\n", Name, Pdo);
        return;
    }
    if ((USHORT)ids == 0xFFFF)
    {
        return;                      // function gone
    }
    (void)UacpipReadPciConfig(Pdo, 0x04, &cmdsts, sizeof(cmdsts));
    (void)UacpipReadPciConfig(Pdo, 0x08, &cls, sizeof(cls));

    UacpiTrace("[acpi] msidiag: %s PDO %p %04X:%04X class 0x%04X cmd 0x%04X sts 0x%04X "
              "INTx %s, INTx-status %s\n",
              Name, Pdo, (USHORT)ids, (USHORT)(ids >> 16),
              (ULONG)((((cls >> 24) & 0xFF) << 8) | ((cls >> 16) & 0xFF)),
              (USHORT)cmdsts, (USHORT)(cmdsts >> 16),
              (cmdsts & 0x400) ? "DISABLED (message mode)" : "enabled (line mode)",
              ((cmdsts >> 16) & 0x08) ? "ASSERTED" : "clear");

    if (((cmdsts >> 16) & 0x10) == 0)   // status bit 4: capability list
    {
        UacpiTrace("[acpi] msidiag: %s no capability list\n", Name);
        return;
    }
    if (!NT_SUCCESS(UacpipReadPciConfig(Pdo, 0x34, &dw, sizeof(dw))))
    {
        return;
    }

    cap = (UCHAR)(dw & 0xFC);
    for (guard = 0; cap >= 0x40 && guard < 48; guard++)
    {
        UCHAR id, next;

        if (!NT_SUCCESS(UacpipReadPciConfig(Pdo, cap, &capdw, sizeof(capdw))))
        {
            break;
        }
        id   = (UCHAR)(capdw & 0xFF);
        next = (UCHAR)((capdw >> 8) & 0xFF);

        if (id == 0x05)   // MSI
        {
            USHORT  ctl    = (USHORT)(capdw >> 16);
            ULONG   addrLo = 0, addrHi = 0, data = 0;
            BOOLEAN is64   = (ctl & 0x80) != 0;

            (void)UacpipReadPciConfig(Pdo, cap + 4, &addrLo, sizeof(addrLo));
            if (is64)
            {
                (void)UacpipReadPciConfig(Pdo, cap + 8,  &addrHi, sizeof(addrHi));
                (void)UacpipReadPciConfig(Pdo, cap + 12, &data,   sizeof(data));
            }
            else
            {
                (void)UacpipReadPciConfig(Pdo, cap + 8,  &data,   sizeof(data));
            }
            UacpiTrace("[acpi] msidiag: %s   MSI cap@0x%02X ctl 0x%04X ENABLE %u "
                      "capable %u granted %u %s addr 0x%08X%08X data 0x%04X "
                      "(vector 0x%02X)\n",
                      Name, cap, ctl, (ULONG)(ctl & 1),
                      (ULONG)(1u << ((ctl >> 1) & 7)),
                      (ULONG)(1u << ((ctl >> 4) & 7)),
                      is64 ? "64-bit" : "32-bit", addrHi, addrLo,
                      (USHORT)data, (ULONG)(data & 0xFF));
            found = TRUE;
        }
        else if (id == 0x11)   // MSI-X
        {
            USHORT ctl = (USHORT)(capdw >> 16);
            ULONG  tbl = 0, pba = 0;

            (void)UacpipReadPciConfig(Pdo, cap + 4, &tbl, sizeof(tbl));
            (void)UacpipReadPciConfig(Pdo, cap + 8, &pba, sizeof(pba));
            UacpiTrace("[acpi] msidiag: %s   MSI-X cap@0x%02X ctl 0x%04X ENABLE %u "
                      "func-mask %u size %u table BIR %u off 0x%X pba BIR %u off 0x%X\n",
                      Name, cap, ctl, (ULONG)((ctl >> 15) & 1),
                      (ULONG)((ctl >> 14) & 1), (ULONG)((ctl & 0x7FF) + 1),
                      (ULONG)(tbl & 7), (ULONG)(tbl & ~7u),
                      (ULONG)(pba & 7), (ULONG)(pba & ~7u));
            UacpipDumpMsiXTable(Pdo, Name, tbl & 7, tbl & ~7u,
                               (ULONG)((ctl & 0x7FF) + 1));
            found = TRUE;
        }
        cap = (UCHAR)(next & 0xFC);
    }
    if (!found)
    {
        UacpiTrace("[acpi] msidiag: %s   no MSI/MSI-X capability\n", Name);
    }
}

// DMA diagnostic: can the PDO's stack produce an adapter and common buffers?
static VOID
UacpipDumpDmaAdapter(PDEVICE_OBJECT Pdo, const char *Name)
{
    DEVICE_DESCRIPTION desc;
    PDMA_ADAPTER       adapter;
    ULONG              mapRegisters = 0;

    if (Pdo == NULL)
    {
        return;
    }
    RtlZeroMemory(&desc, sizeof(desc));
    desc.Version           = DEVICE_DESCRIPTION_VERSION;
    desc.InterfaceType     = PCIBus;
    desc.Master            = TRUE;
    desc.ScatterGather     = TRUE;
    desc.Dma32BitAddresses = FALSE;
    desc.Dma64BitAddresses = TRUE;
    desc.MaximumLength     = 0x10000;      // 64 KB, a typical controller ask
    desc.DmaChannel        = 0;
    desc.DmaWidth          = Width32Bits;
    desc.DmaSpeed          = Compatible;

    adapter = IoGetDmaAdapter(Pdo, &desc, &mapRegisters);
    if (adapter == NULL)
    {
        UacpiTrace("[acpi] dmadiag: %s PDO %p IoGetDmaAdapter returned NULL "
                  "<== device cannot DMA; START will fail INSUFFICIENT_RESOURCES\n",
                  Name, Pdo);
        return;
    }

    UacpiTrace("[acpi] dmadiag: %s PDO %p adapter %p map registers %u "
              "(64-bit master, s/g)\n",
              Name, Pdo, adapter, mapRegisters);

    // Allocate a ladder of common buffer sizes to find where it fails.
    if (adapter->DmaOperations != NULL &&
        adapter->DmaOperations->AllocateCommonBuffer != NULL)
        {
        static const ULONG sizes[] = { 0x1000, 0x10000, 0x40000, 0x100000 };
        ULONG s;

        for (s = 0; s < RTL_NUMBER_OF(sizes); s++)
        {
            PHYSICAL_ADDRESS logical;
            PVOID va;

            logical.QuadPart = 0;
            va = adapter->DmaOperations->AllocateCommonBuffer(
                     adapter, sizes[s], &logical, FALSE);
            if (va == NULL)
            {
                UacpiTrace("[acpi] dmadiag: %s   common buffer %6u bytes: FAILED "
                          "<== this is the START failure\n",
                          Name, sizes[s]);
                break;                    // no point asking for more
            }
            UacpiTrace("[acpi] dmadiag: %s   common buffer %6u bytes: ok "
                      "va %p logical 0x%08X%08X\n",
                      Name, sizes[s], va,
                      (ULONG)logical.HighPart, logical.LowPart);
            if (adapter->DmaOperations->FreeCommonBuffer != NULL)
            {
                adapter->DmaOperations->FreeCommonBuffer(
                    adapter, sizes[s], logical, va, FALSE);
            }
        }
    }

    if (adapter->DmaOperations != NULL &&
        adapter->DmaOperations->PutDmaAdapter != NULL)
        {
        adapter->DmaOperations->PutDmaAdapter(adapter);
    }
}

// PIC model: report committed GSIVs that are still masked in the 8259 IMRs.
static VOID
UacpipDumpLineState(VOID)
{
    UCHAR master, slave;
    ULONG i;

    if (g_AcpiInterruptModel != 0)
    {
        // APIC inputs are traced as they are unmasked.
        return;
    }

    master = READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)0x21);
    slave  = READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)0xA1);

    UacpiTrace("[acpi] irqdiag: ==== wired-line readback, PIC IMR "
              "master 0x%02X slave 0x%02X ====\n", master, slave);

    for (i = 0; i < UacpiIrqWrittenCount; i++)
    {
        ULONG gsiv = UacpiIrqWritten[i].Start;
        ULONG vector = 0, polarity = 0, mode = 0;
        KAFFINITY affinity = 0;
        KIRQL irql = 0;
        BOOLEAN masked;

        if ((ULONGLONG)gsiv >= UACPI_MSI_GSIV_BASE)
        {
            continue;               // a message run, covered by msidiag above
        }
        if (gsiv > 15)
        {
            continue;               // not an 8259 line
        }

        masked = (gsiv < 8) ? (BOOLEAN)((master >> gsiv) & 1)
                            : (BOOLEAN)((slave >> (gsiv - 8)) & 1);

        if (!NT_SUCCESS(UacpiIrqLibResolveVector(gsiv, &vector, &irql, &affinity,
                                                &polarity, &mode)))
                                                {
            vector = 0;
        }

        UacpiTrace("[acpi] irqdiag: GSIV %2u vector 0x%02X owner %p -> %s\n",
                  gsiv, vector, UacpiIrqWritten[i].Owner,
                  masked ? "MASKED (nothing connected)" : "unmasked");
    }

    UacpiTrace("[acpi] irqdiag: ==== end ====\n");
}

// Walk every foreign PDO this driver filters and dump its message state.
static VOID
NTAPI
UacpipMsiDiagWorker(PVOID Context)
{
    struct { PDEVICE_OBJECT Pdo; char Name[8]; } cand[48];
    ULONG count = 0, i;
    PLIST_ENTRY e;

    UNREFERENCED_PARAMETER(Context);

    if (g_AcpiFdo == NULL)
    {
        return;
    }

    // Snapshot with references held; the dump sends IRPs outside the lock.
    ExAcquireFastMutex(&g_AcpiFdo->ChildLock);
    for (e = g_AcpiFdo->Filters.Flink;
         e != &g_AcpiFdo->Filters && count < RTL_NUMBER_OF(cand);
         e = e->Flink)
         {
        PUACPI_FILTER f = CONTAINING_RECORD(e, UACPI_FILTER, Link);

        if (f->ForeignPdo == NULL)
        {
            continue;
        }
        ObReferenceObject(f->ForeignPdo);
        cand[count].Pdo = f->ForeignPdo;
        if (f->Node != NULL)
        {
            uacpi_object_name n = uacpi_namespace_node_name(f->Node);
            cand[count].Name[0] = n.text[0];
            cand[count].Name[1] = n.text[1];
            cand[count].Name[2] = n.text[2];
            cand[count].Name[3] = n.text[3];
            cand[count].Name[4] = 0;
        }
        else
        {
            cand[count].Name[0] = '?';
            cand[count].Name[1] = 0;
        }
        count++;
    }
    ExReleaseFastMutex(&g_AcpiFdo->ChildLock);

    UacpiTrace("[acpi] msidiag: ==== message-interrupt readback, %u filtered "
              "function(s) ====\n", count);
    for (i = 0; i < count; i++)
    {
        UacpipDumpMsiState(cand[i].Pdo, cand[i].Name);
        UacpipDumpDmaAdapter(cand[i].Pdo, cand[i].Name);
        ObDereferenceObject(cand[i].Pdo);
    }
    UacpiTrace("[acpi] msidiag: ==== end ====\n");

    UacpipDumpLineState();
}

static VOID
NTAPI
UacpipMsiDiagDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    // Config reads need PASSIVE_LEVEL; hop off the DPC.
#pragma warning(suppress: 4996)
    ExQueueWorkItem(&UacpiMsiDiagWork, DelayedWorkQueue);
}

VOID
UacpiMsiDiagArm(VOID)
{
    LARGE_INTEGER due;

    if (!UacpiMsiDiagEnabled)
    {
        return;
    }
    if (InterlockedCompareExchange(&UacpiMsiDiagArmed, 1, 0) != 0)
    {
        return;                                  // one-shot
    }
#pragma warning(suppress: 4996)
    ExInitializeWorkItem(&UacpiMsiDiagWork, UacpipMsiDiagWorker, NULL);
    KeInitializeDpc(&UacpiMsiDiagDpc, UacpipMsiDiagDpc, NULL);
    KeInitializeTimer(&UacpiMsiDiagTimer);

    due.QuadPart = -((LONGLONG)UacpiMsiDiagDelaySeconds * 10 * 1000 * 1000);
    (void)KeSetTimer(&UacpiMsiDiagTimer, due, &UacpiMsiDiagDpc);
    UacpiTrace("[acpi] msidiag: armed, dumping message state in %u second(s)\n",
              (ULONG)UacpiMsiDiagDelaySeconds);
}

VOID
UacpiMsiDiagDisarm(VOID)
{
    if (InterlockedCompareExchange(&UacpiMsiDiagArmed, 2, 1) == 1)
    {
        (void)KeCancelTimer(&UacpiMsiDiagTimer);
    }
}
