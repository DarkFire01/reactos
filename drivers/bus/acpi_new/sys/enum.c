/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Namespace enumeration into PDOs and device IDs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"
#include <uacpi/internal/namespace.h>   // uacpi_namespace_node_get_object_typed

// Set to 1 before FDO start to hoist every _HID device into the root relations.
int UacpiFlatEnumEnabled = 0;
int UacpiEnumDiagEnabled = 1;
int UacpiEnumDiagDelaySeconds = 2;

// Legacy Processor() ID string: "<VendorIdentifier> - <Identifier>" from
// CentralProcessor\0, cut before " Stepping". Empty means no processor PDO.
static CHAR UacpiProcessorString[128];

#if (NTDDI_VERSION >= NTDDI_WIN10)
// Win10 appends the CPUID brand string to the device ID; Vista does not.
static CHAR UacpiProcessorBrand[80];

#define UACPI_SYSTEM_PROCESSOR_BRAND_STRING 105   // SystemProcessorBrandString
NTSYSAPI NTSTATUS NTAPI ZwQuerySystemInformation(ULONG SystemInformationClass,
                                                 PVOID SystemInformation,
                                                 ULONG SystemInformationLength,
                                                 PULONG ReturnLength);
#endif

// Instance ID is the ACPI processor ID: "%2x" from Win8 (Win8 DP, Win10), "%2d"
// before it (the only two-wide format in Vista's acpi.sys).
#if (NTDDI_VERSION >= NTDDI_WIN8)
#define UACPI_PROCESSOR_INSTANCE_FORMAT "%2x"
#else
#define UACPI_PROCESSOR_INSTANCE_FORMAT "%2d"
#endif

// Read one REG_SZ into an ASCII buffer; FALSE if absent or not a string.
static BOOLEAN
UacpipReadRegSzAscii(HANDLE Key, PCWSTR Name, PCHAR Out, ULONG OutSize)
{
    UNICODE_STRING name;
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 256];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    ULONG len = 0, i, chars;
    PCWCH src;

    RtlInitUnicodeString(&name, Name);
    if (!NT_SUCCESS(ZwQueryValueKey(Key, &name, KeyValuePartialInformation,
                                    info, sizeof(buffer), &len)) ||
        info->Type != REG_SZ)
    {
        return FALSE;
    }
    src = (PCWCH)info->Data;
    chars = info->DataLength / sizeof(WCHAR);
    for (i = 0; i < chars && i + 1 < OutSize && src[i] != L'\0'; i++)
    {
        Out[i] = (src[i] < 0x80) ? (CHAR)src[i] : '?';
    }
    Out[i] = '\0';
    return TRUE;
}

VOID
UacpiInitProcessorString(VOID)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    CHAR vendor[64], identifier[96];
    PCHAR stepping;

    PAGED_CODE();

    UacpiProcessorString[0] = '\0';
    RtlInitUnicodeString(&path,
        L"\\Registry\\Machine\\Hardware\\Description\\System\\CentralProcessor\\0");
    InitializeObjectAttributes(&oa, &path,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa)))
    {
        UacpiTrace("[acpi] processor: CentralProcessor\\0 not found - no processor devices\n");
        return;
    }
    if (UacpipReadRegSzAscii(key, L"Identifier", identifier, sizeof(identifier)) &&
        UacpipReadRegSzAscii(key, L"VendorIdentifier", vendor, sizeof(vendor)))
    {
        stepping = strstr(identifier, "Stepping");
        if (stepping != NULL && stepping > identifier)
        {
            stepping[-1] = '\0';   // drop " Stepping N"
        }
        RtlStringCbPrintfA(UacpiProcessorString, sizeof(UacpiProcessorString),
                           "%s - %s", vendor, identifier);
    }
    ZwClose(key);

#if (NTDDI_VERSION >= NTDDI_WIN10)
    {
        ULONG size = 0, i;

        RtlZeroMemory(UacpiProcessorBrand, sizeof(UacpiProcessorBrand));
        if (NT_SUCCESS(ZwQuerySystemInformation(UACPI_SYSTEM_PROCESSOR_BRAND_STRING,
                                                UacpiProcessorBrand,
                                                sizeof(UacpiProcessorBrand) - 1, &size)))
        {
            // Non-printable characters and commas become spaces, as in the reference.
            for (i = 0; UacpiProcessorBrand[i] != '\0'; i++)
            {
                if ((UCHAR)(UacpiProcessorBrand[i] - 0x20) > 0x5F ||
                    UacpiProcessorBrand[i] == ',')
                {
                    UacpiProcessorBrand[i] = ' ';
                }
            }
        }
        else
        {
            UacpiProcessorBrand[0] = '\0';
        }
    }
#endif

    UacpiTrace("[acpi] processor: ID string \"%s\"\n", UacpiProcessorString);
}

// Wide-string builders (PnP frees the returned buffer with ExFreePool).

// Build a single REG_SZ (NUL-terminated) wide string from an ASCII source.
static PWSTR
UacpiWideDup(const char *ascii)
{
    SIZE_T len = strlen(ascii);
    PWSTR out = (PWSTR)ExAllocatePoolWithTag(PagedPool,
                                             (len + 1) * sizeof(WCHAR),
                                             UACPI_POOL_TAG);
    SIZE_T i;
    if (out == NULL)
    {
        return NULL;
    }
    for (i = 0; i < len; i++)
    {
        out[i] = (WCHAR)(UCHAR)ascii[i];
    }
    out[len] = L'\0';
    return out;
}

// Build a REG_MULTI_SZ wide string from 'count' ASCII entries.
static PWSTR
UacpiWideMultiSz(const char **entries, ULONG count)
{
    SIZE_T total = 1;   // final terminating NUL
    ULONG k;
    PWSTR out, p;

    for (k = 0; k < count; k++)
    {
        total += strlen(entries[k]) + 1;
    }
    out = (PWSTR)ExAllocatePoolWithTag(PagedPool, total * sizeof(WCHAR),
                                       UACPI_POOL_TAG);
    if (out == NULL)
    {
        return NULL;
    }
    p = out;
    for (k = 0; k < count; k++)
    {
        const char *s = entries[k];
        while (*s)
        {
            *p++ = (WCHAR)(UCHAR)*s++;
        }
        *p++ = L'\0';
    }
    *p = L'\0';         // list terminator
    return out;
}

// Small namespace helpers.

static BOOLEAN
UacpiNodeHasAdr(uacpi_namespace_node *node, uacpi_u64 *adr)
{
    uacpi_u64 v = 0;
    if (uacpi_likely_success(uacpi_eval_simple_integer(node, "_ADR", &v)))
    {
        if (adr != NULL)
        {
            *adr = v;
        }
        return TRUE;
    }
    return FALSE;
}

// Returns the node's _HID into hidBuf ("" if none).
static BOOLEAN
UacpiNodeGetHid(uacpi_namespace_node *node, char *hidBuf, SIZE_T hidBufSize)
{
    uacpi_id_string *hid = NULL;

    hidBuf[0] = '\0';
    if (uacpi_likely_success(uacpi_eval_hid(node, &hid)) && hid != NULL)
    {
        RtlStringCbCopyA(hidBuf, hidBufSize, hid->value);
        uacpi_free_id_string(hid);
        return TRUE;
    }
    return FALSE;
}

BOOLEAN
UacpiNodeIsPresentEx(uacpi_namespace_node *node)
{
    // uACPI returns all-ones when _STA is absent => present.
    uacpi_u32 sta = UACPI_STA_PRESENT | UACPI_STA_FUNCTIONING;
    (void)uacpi_eval_sta(node, &sta);
    return (sta & UACPI_STA_PRESENT) != 0;
}

// PCI interrupt links (PNP0C0F) are never devnodes; irqarb.c routes them.
static BOOLEAN
UacpiHidExcludedFromEnum(const char *hid)
{
    return _stricmp(hid, "PNP0C0F") == 0;
}

// ACPI0010 processor container: gets a PDO, and its children are enumerated.
static
BOOLEAN
UacpiHidIsContainer(const char *hid)
{
    return _stricmp(hid, "ACPI0010") == 0;
}

BOOLEAN
UacpiHidIsPciRoot(const char *hid)
{
    return _stricmp(hid, "PNP0A03") == 0 || _stricmp(hid, "PNP0A08") == 0;
}

// Must match UacpiBuildChildPdosForNode; resarb.c skips these nodes' _CRS.
BOOLEAN
UacpiNodeWillBecomePdo(uacpi_namespace_node *node)
{
    char hid[16];

    if (!UacpiNodeIsPresentEx(node))
    {
        return FALSE;
    }
    if (!UacpiNodeGetHid(node, hid, sizeof(hid)))
    {
        return FALSE;   // no _HID: _ADR filter candidate, or nothing at all
    }
    return !UacpiHidExcludedFromEnum(hid);
}

// Get-or-create; a second PDO for the same node bugchecks 0xCA.
static PUACPI_PDO
UacpiGetOrCreatePdo(PUACPI_FDO Fdo, uacpi_namespace_node *node,
                   uacpi_namespace_node *parentNode, BOOLEAN thermal)
{
    PDRIVER_OBJECT drv = Fdo->Common.Self->DriverObject;
    PDEVICE_OBJECT pdoDevice;
    PUACPI_PDO pdo;
    uacpi_object_name name;
    uacpi_u64 adr = 0;
    uacpi_object_type type;
    BOOLEAN isProcessor = FALSE;
    NTSTATUS status;

    pdo = UacpiFindPdoByNode(Fdo, node);
    if (pdo != NULL)
    {
        pdo->Present = TRUE;
        return pdo;
    }

    // Legacy Processor(): acpi.sys builds no device without the ID string.
    if (!thermal &&
        uacpi_likely_success(uacpi_namespace_node_type(node, &type)) &&
        type == UACPI_OBJECT_PROCESSOR)
    {
        if (UacpiProcessorString[0] == '\0')
        {
            return NULL;
        }
        isProcessor = TRUE;
    }

    status = IoCreateDevice(drv, sizeof(UACPI_PDO), NULL, FILE_DEVICE_ACPI,
                            FILE_AUTOGENERATED_DEVICE_NAME | FILE_DEVICE_SECURE_OPEN,
                            FALSE, &pdoDevice);
    if (!NT_SUCCESS(status))
    {
        return NULL;
    }

    pdo = (PUACPI_PDO)pdoDevice->DeviceExtension;
    RtlZeroMemory(pdo, sizeof(*pdo));
    pdo->Common.Type = UacpiExtPdo;
    pdo->Common.Self = pdoDevice;
    pdo->Parent = Fdo;
    pdo->Node = node;
    pdo->ParentNode = parentNode;
    pdo->Present = TRUE;
    pdo->IsThermalZone = thermal;
    pdo->IsProcessor = isProcessor;
    UacpiWakeInit(&pdo->Wake, node);
    KeInitializeSpinLock(&pdo->Thermal.Lock);
    InitializeListHead(&pdo->Thermal.IrpQueue);

    name = uacpi_namespace_node_name(node);
    pdo->Name[0] = name.text[0];
    pdo->Name[1] = name.text[1];
    pdo->Name[2] = name.text[2];
    pdo->Name[3] = name.text[3];
    pdo->Name[4] = '\0';

    // Instance ID: absolute path without separators; unique across the tree.
    {
        const uacpi_char *path = uacpi_namespace_node_generate_absolute_path(node);
        if (path != NULL)
        {
            const uacpi_char *s = path;
            ULONG w = 0;
            for (; *s && w < sizeof(pdo->Instance) - 1; s++)
            {
                if (*s != '\\' && *s != '.')
                {
                    pdo->Instance[w++] = *s;
                }
            }
            pdo->Instance[w] = '\0';
            uacpi_free_absolute_path(path);
        }
        else
        {
            RtlStringCbCopyA(pdo->Instance, sizeof(pdo->Instance), pdo->Name);
        }
    }

    // Processors: the ACPI processor ID, formatted as acpi.sys does.
    if (isProcessor)
    {
        uacpi_object *obj =
            uacpi_namespace_node_get_object_typed(node, UACPI_OBJECT_PROCESSOR_BIT);
        uacpi_processor_info info;

        if (obj != NULL &&
            uacpi_likely_success(uacpi_object_get_processor_info(obj, &info)))
        {
            RtlStringCbPrintfA(pdo->Instance, sizeof(pdo->Instance),
                               UACPI_PROCESSOR_INSTANCE_FORMAT, info.id);
        }
    }

    if (thermal)
    {
        // Thermal zones have no _HID; the well-known PnP ID lets thermal.sys bind.
        RtlStringCbCopyA(pdo->Hid, sizeof(pdo->Hid), "ThermalZone");
    }
    else
    {
        (void)UacpiNodeGetHid(node, pdo->Hid, sizeof(pdo->Hid));
    }
    if (UacpiNodeHasAdr(node, &adr))
    {
        pdo->HasAdr = TRUE;
        pdo->Adr = adr;
    }

    // Tag power/sleep/lid buttons for GUID_DEVICE_SYS_BUTTON (drvs/button.c).
    (void)UacpiButtonClassify(pdo);

    pdoDevice->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    pdoDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    ExAcquireFastMutex(&Fdo->ChildLock);
    InsertTailList(&Fdo->Children, &pdo->Link);
    ExReleaseFastMutex(&Fdo->ChildLock);

    UacpiTrace("[acpi] PDO %s  HID=%s  ADR=%s0x%I64X\n",
              pdo->Name, pdo->Hid[0] ? pdo->Hid : "(none)",
              pdo->HasAdr ? "" : "(none) ", pdo->Adr);
    return pdo;
}

// Create or refresh PDOs for the direct children of 'parent'.
VOID
UacpiBuildChildPdosForNode(PUACPI_FDO Fdo, uacpi_namespace_node *parent)
{
    uacpi_namespace_node *child = NULL;

    if (parent == NULL)
    {
        return;
    }

    while (uacpi_likely_success(uacpi_namespace_node_next_typed(
               parent, &child,
               UACPI_OBJECT_DEVICE_BIT | UACPI_OBJECT_THERMAL_ZONE_BIT |
               UACPI_OBJECT_PROCESSOR_BIT)) &&
           child != NULL)
           {

        uacpi_object_type type;
        char hid[16];

        if (uacpi_unlikely_error(uacpi_namespace_node_type(child, &type)))
        {
            continue;
        }

        if (type == UACPI_OBJECT_THERMAL_ZONE)
        {
            UacpiGetOrCreatePdo(Fdo, child, parent, TRUE);
            continue;
        }

        if (!UacpiNodeIsPresentEx(child))
        {
            PUACPI_PDO existing = UacpiFindPdoByNode(Fdo, child);
            if (existing != NULL)
            {
                existing->Present = FALSE;
            }
            continue;
        }

        // Legacy Processor objects: IDs come from CentralProcessor\0 (see above).
        if (type == UACPI_OBJECT_PROCESSOR)
        {
            UacpiGetOrCreatePdo(Fdo, child, parent, FALSE);
            continue;
        }

        // _HID wins over _ADR; only _ADR-without-_HID is a filter candidate.
        if (UacpiNodeGetHid(child, hid, sizeof(hid)))
        {
            if (UacpiHidExcludedFromEnum(hid))
            {
                continue;
            }

            UacpiGetOrCreatePdo(Fdo, child, parent, FALSE);

            // Processors sit below the container; descend into it.
            if (UacpiHidIsContainer(hid))
            {
                UacpiBuildChildPdosForNode(Fdo, child);
            }
            continue;
        }

        // No _HID: _ADR nodes go to UacpiDetectFilterDevices.
        continue;
    }
}

// Merge present child PDOs into the IRP's relations, deduped and referenced.
NTSTATUS
UacpiMergeChildRelations(PUACPI_FDO Fdo, uacpi_namespace_node *parent,
                        uacpi_namespace_node *parent2, PIRP Irp)
{
    PDEVICE_RELATIONS oldRel = (PDEVICE_RELATIONS)Irp->IoStatus.Information;
    PDEVICE_RELATIONS newRel;
    PLIST_ENTRY entry;
    ULONG oldCount = (oldRel != NULL) ? oldRel->Count : 0;
    ULONG ours = 0, i, k;
    SIZE_T size;

    ExAcquireFastMutex(&Fdo->ChildLock);

    for (entry = Fdo->Children.Flink; entry != &Fdo->Children; entry = entry->Flink)
    {
        PUACPI_PDO pdo = CONTAINING_RECORD(entry, UACPI_PDO, Link);
        if (pdo->Present &&
            (pdo->ParentNode == parent || (parent2 != NULL && pdo->ParentNode == parent2)))
            {
            ours++;
        }
    }

    size = FIELD_OFFSET(DEVICE_RELATIONS, Objects) +
           (SIZE_T)(oldCount + ours) * sizeof(PDEVICE_OBJECT);
    if (size < sizeof(DEVICE_RELATIONS))
    {
        size = sizeof(DEVICE_RELATIONS);
    }
    newRel = (PDEVICE_RELATIONS)ExAllocatePoolWithTag(PagedPool, size, UACPI_POOL_TAG);
    if (newRel == NULL)
    {
        ExReleaseFastMutex(&Fdo->ChildLock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (i = 0; i < oldCount; i++)
    {
        newRel->Objects[i] = oldRel->Objects[i];
    }
    newRel->Count = oldCount;

    for (entry = Fdo->Children.Flink; entry != &Fdo->Children; entry = entry->Flink)
    {
        PUACPI_PDO pdo = CONTAINING_RECORD(entry, UACPI_PDO, Link);
        BOOLEAN already = FALSE;

        if (!pdo->Present ||
            (pdo->ParentNode != parent && (parent2 == NULL || pdo->ParentNode != parent2)))
            {
            continue;
        }
        for (k = 0; k < oldCount; k++)
        {
            if (newRel->Objects[k] == pdo->Common.Self)
            {
                already = TRUE;
                break;
            }
        }
        if (already)
        {
            continue;
        }
        ObReferenceObject(pdo->Common.Self);
        newRel->Objects[newRel->Count++] = pdo->Common.Self;
        pdo->Reported = TRUE;
    }

    ExReleaseFastMutex(&Fdo->ChildLock);

    if (oldRel != NULL)
    {
        ExFreePool(oldRel);
    }
    Irp->IoStatus.Information = (ULONG_PTR)newRel;
    return STATUS_SUCCESS;
}

// Flat mode: hoist _HID devices into the root. Never recurse in the callback.
typedef struct _ACPI_FLAT_CTX
{
    PUACPI_FDO             Fdo;
    uacpi_namespace_node *Sb;           // ParentNode for every hoisted PDO
    uacpi_u32             PciRootDepth; // 0 = not inside a PCI-root subtree
} UACPI_FLAT_CTX, *PUACPI_FLAT_CTX;

static uacpi_iteration_decision
UacpiFlatWalkCb(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    PUACPI_FLAT_CTX Ctx = (PUACPI_FLAT_CTX)user;
    uacpi_object_type type;
    char hid[16];
    BOOLEAN hasHid, inPciSubtree;

    if (Ctx->PciRootDepth != 0 && depth <= Ctx->PciRootDepth)
    {
        Ctx->PciRootDepth = 0;          // left the PCI root's subtree
    }
    inPciSubtree = (Ctx->PciRootDepth != 0);

    if (uacpi_unlikely_error(uacpi_namespace_node_type(node, &type)))
    {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    if (type == UACPI_OBJECT_THERMAL_ZONE)
    {
        UacpiGetOrCreatePdo(Ctx->Fdo, node, Ctx->Sb, TRUE);
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    if (type != UACPI_OBJECT_DEVICE)
    {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    if (!UacpiNodeIsPresentEx(node))
    {
        return UACPI_ITERATION_DECISION_NEXT_PEER;
    }

    hasHid = UacpiNodeGetHid(node, hid, sizeof(hid));
    if (hasHid && UacpiHidExcludedFromEnum(hid))
    {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    if (inPciSubtree)
    {
        if (UacpiNodeHasAdr(node, NULL) || !hasHid)
        {
            // PCI-enumerated or nothing to expose; descend for nested devices.
            return UACPI_ITERATION_DECISION_CONTINUE;
        }
    }

    if (UacpiGetOrCreatePdo(Ctx->Fdo, node, Ctx->Sb, FALSE) == NULL)
    {
        return UACPI_ITERATION_DECISION_NEXT_PEER;
    }
    if (hasHid && UacpiHidIsPciRoot(hid))
    {
        Ctx->PciRootDepth = depth;      // entering the PCI root's subtree
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}


// Namespace inventory
static uacpi_iteration_decision
UacpiEnumDiagCb(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    PUACPI_FDO Fdo = (PUACPI_FDO)user;
    uacpi_object_type type;
    uacpi_u32 sta = UACPI_STA_PRESENT | UACPI_STA_FUNCTIONING;
    uacpi_status staStatus;
    const char *verdict;
    char hid[16];
    uacpi_u64 adr = 0;
    BOOLEAN hasHid, hasAdr;
    const uacpi_char *path;

    if (uacpi_unlikely_error(uacpi_namespace_node_type(node, &type)))
    {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    if (type != UACPI_OBJECT_DEVICE &&
        type != UACPI_OBJECT_THERMAL_ZONE &&
        type != UACPI_OBJECT_PROCESSOR)
    {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    staStatus = uacpi_eval_sta(node, &sta);
    hasHid = UacpiNodeGetHid(node, hid, sizeof(hid));
    hasAdr = UacpiNodeHasAdr(node, &adr);

    if (type == UACPI_OBJECT_THERMAL_ZONE)
    {
        verdict = "thermal zone";
    }
    else if (UacpiFindPdoByNode(Fdo, node) != NULL)
    {
        verdict = "PDO";
    }
    else if ((sta & UACPI_STA_PRESENT) == 0)
    {
        verdict = "skipped: _STA reports not present";
    }
    else if (!hasHid)
    {
        verdict = hasAdr ? "skipped: no _HID, addressed by its parent bus"
                         : "skipped: no _HID and no _ADR";
    }
    else if (UacpiHidExcludedFromEnum(hid))
    {
        verdict = "skipped: _HID excluded from enumeration";
    }
    else if (UacpiHidIsContainer(hid))
    {
        verdict = "container: gets a PDO, and is enumerated through";
    }
    else
    {
        // Below a PCI root: surfaced later by the parent function's filter.
        verdict = "not yet: below a PCI root, surfaced when its filter runs";
    }

    path = uacpi_namespace_node_generate_absolute_path(node);
    UacpiTrace("[acpi] ns: %-28s depth %u HID=%-9s ADR=%s _STA=%s0x%X -> %s\n",
              path != UACPI_NULL ? path : "?",
              depth,
              hasHid ? hid : "-",
              hasAdr ? "yes" : "-",
              uacpi_unlikely_error(staStatus) ? "(absent) " : "",
              sta,
              verdict);
    if (path != UACPI_NULL)
    {
        uacpi_free_absolute_path(path);
    }

    return UACPI_ITERATION_DECISION_CONTINUE;
}

VOID
UacpiEnumDiagDump(PUACPI_FDO Fdo, BOOLEAN Settled)
{

    uacpi_namespace_node *root;

    PAGED_CODE();

    if (!UacpiEnumDiagEnabled)
    {
        return;
    }

    root = uacpi_namespace_root();
    if (root == UACPI_NULL)
    {
        return;
    }

    UacpiTrace("[acpi] ns: ==== namespace inventory ====\n");
    UacpiTrace("[acpi] ns: (%s)\n",
              Settled ? "settled - these verdicts are final"
                      : "first pass - the PCI subtree has not been walked yet");
    uacpi_namespace_for_each_child_simple(root, UacpiEnumDiagCb, Fdo);
    UacpiTrace("[acpi] ns: ==== end ====\n");
}


// Settled dump: debounce timer, pushed out again by every enumeration pass.
static KTIMER      UacpiEnumDiagTimer;
static KDPC        UacpiEnumDiagDpc;
static WORK_QUEUE_ITEM UacpiEnumDiagWork;
// Keeps the work item from being queued twice (bugcheck 0x139).
#define UACPI_ENUMDIAG_IDLE     0    /* never armed */
#define UACPI_ENUMDIAG_ARMED    1    /* timer pending */
#define UACPI_ENUMDIAG_QUEUED   2    /* work item in flight, do not queue again */
#define UACPI_ENUMDIAG_DONE     3    /* dumped; this is a one-shot */

static LONG        UacpiEnumDiagState;
static PUACPI_FDO  UacpiEnumDiagFdo;

static VOID
NTAPI
UacpiEnumDiagWorker(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    if (UacpiEnumDiagFdo != NULL)
    {
        UacpiEnumDiagDump(UacpiEnumDiagFdo, TRUE);
    }

    InterlockedExchange(&UacpiEnumDiagState, UACPI_ENUMDIAG_DONE);
}

static VOID
NTAPI
UacpiEnumDiagDpcRoutine(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    // At most one queued work item.
    if (InterlockedCompareExchange(&UacpiEnumDiagState,
                                   UACPI_ENUMDIAG_QUEUED,
                                   UACPI_ENUMDIAG_ARMED) != UACPI_ENUMDIAG_ARMED)
    {
        return;
    }

    /* _STA evaluation is AML, so it needs PASSIVE_LEVEL */
#pragma warning(suppress: 4996)
    ExQueueWorkItem(&UacpiEnumDiagWork, DelayedWorkQueue);
}

// Schedule the settled namespace inventory.
VOID
UacpiEnumDiagArm(PUACPI_FDO Fdo)
{
    LARGE_INTEGER Due;
    LONG State;

    if (!UacpiEnumDiagEnabled || Fdo == NULL)
    {
        return;
    }

    // Do not re-arm once the work item is queued or done.
    State = InterlockedCompareExchange(&UacpiEnumDiagState,
                                       UACPI_ENUMDIAG_ARMED,
                                       UACPI_ENUMDIAG_IDLE);
    if (State == UACPI_ENUMDIAG_QUEUED || State == UACPI_ENUMDIAG_DONE)
    {
        return;
    }

    UacpiEnumDiagFdo = Fdo;
    Due.QuadPart = -((LONGLONG)UacpiEnumDiagDelaySeconds * 10 * 1000 * 1000);

    if (State == UACPI_ENUMDIAG_ARMED)
    {
        /* Already waiting: push it out, this pass may not be the last */
        (void)KeSetTimer(&UacpiEnumDiagTimer, Due, &UacpiEnumDiagDpc);
        return;
    }

#pragma warning(suppress: 4996)
    ExInitializeWorkItem(&UacpiEnumDiagWork, UacpiEnumDiagWorker, NULL);
    KeInitializeDpc(&UacpiEnumDiagDpc, UacpiEnumDiagDpcRoutine, NULL);
    KeInitializeTimer(&UacpiEnumDiagTimer);

    (void)KeSetTimer(&UacpiEnumDiagTimer, Due, &UacpiEnumDiagDpc);
    UacpiTrace("[acpi] ns: settled inventory follows %u second(s) after the last pass\n",
              (ULONG)UacpiEnumDiagDelaySeconds);
}

// Initial enumeration (FDO start).
NTSTATUS
UacpiEnumerateNamespace(PUACPI_FDO Fdo)
{
    uacpi_namespace_node *sb, *tz;

    PAGED_CODE();

    if (!Fdo->UacpiUp)
    {
        return STATUS_UNSUCCESSFUL;
    }
    sb = uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_SB);
    if (sb == NULL)
    {
        UacpiTrace("[acpi] \\_SB not found\n");
        return STATUS_UNSUCCESSFUL;
    }
    tz = uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_TZ);

    if (UacpiFlatEnumEnabled)
    {
        UACPI_FLAT_CTX ctx;
        ctx.Fdo = Fdo;
        ctx.Sb = sb;
        ctx.PciRootDepth = 0;
        uacpi_namespace_for_each_child_simple(sb, UacpiFlatWalkCb, &ctx);
        if (tz != NULL)
        {
            ctx.Sb = tz;
            ctx.PciRootDepth = 0;
            uacpi_namespace_for_each_child_simple(tz, UacpiFlatWalkCb, &ctx);
        }
    }
    else
    {
        UacpiBuildChildPdosForNode(Fdo, sb);
        UacpiBuildChildPdosForNode(Fdo, tz);
    }

    UacpiEnumDiagDump(Fdo, FALSE);
    UacpiEnumDiagArm(Fdo);
    return STATUS_SUCCESS;
}

// Root FDO BusRelations: refresh and merge the \_SB and \_TZ children.
NTSTATUS
UacpiBuildBusRelations(PUACPI_FDO Fdo, PIRP Irp)
{
    uacpi_namespace_node *sb =
        uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_SB);
    uacpi_namespace_node *tz =
        uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_TZ);
    NTSTATUS status;
    ULONG count = 0;

    if (!UacpiFlatEnumEnabled)
    {
        UacpiBuildChildPdosForNode(Fdo, sb);
        UacpiBuildChildPdosForNode(Fdo, tz);
    }
    status = UacpiMergeChildRelations(Fdo, sb, tz, Irp);
    if (NT_SUCCESS(status) && Irp->IoStatus.Information != 0)
    {
        count = ((PDEVICE_RELATIONS)Irp->IoStatus.Information)->Count;
    }
    UacpiTrace("[acpi] BusRelations: %u PDO(s)\n", count);
    return status;
}

// Fixed IDs for no-_HID nodes; an ACPI\<node name> ID matches no INF.
static const char *
UacpipNoHidDeviceId(PUACPI_PDO Pdo)
{
    uacpi_object_type type;

    if (Pdo->IsThermalZone)
    {
        return "ACPI\\ThermalZone";
    }
    if (Pdo->Node != NULL &&
        uacpi_likely_success(uacpi_namespace_node_type(Pdo->Node, &type)) &&
        type == UACPI_OBJECT_PROCESSOR)
        {
        return "ACPI\\Processor";
    }
    return NULL;   // no literal applies; fall back to the node name
}

// Legacy Processor IDs (ACPIGetProcessorIDWide). Spaces stay; PnP makes them '_'.
static NTSTATUS
UacpipProcessorQueryId(PUACPI_PDO Pdo, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    CHAR s[sizeof(UacpiProcessorString)];
    CHAR hw[6][sizeof(UacpiProcessorString) + 8];
    const char *ids[6];
    PCHAR model, family;
    PWSTR out = NULL;
    ULONG i;

    switch (sp->Parameters.QueryId.IdType)
    {
    case BusQueryDeviceID:
#if (NTDDI_VERSION >= NTDDI_WIN10)
        if (UacpiProcessorBrand[0] != '\0')
        {
            CHAR dev[sizeof(UacpiProcessorString) + sizeof(UacpiProcessorBrand) + 8];

            RtlStringCbPrintfA(dev, sizeof(dev), "ACPI\\%s - %s",
                               UacpiProcessorString, UacpiProcessorBrand);
            out = UacpiWideDup(dev);
            break;
        }
#endif
        RtlStringCbPrintfA(hw[0], sizeof(hw[0]), "ACPI\\%s", UacpiProcessorString);
        out = UacpiWideDup(hw[0]);
        break;

    case BusQueryHardwareIDs:
        // ACPI\ and * forms of the full string, then cut before Model, then Family.
        RtlStringCbCopyA(s, sizeof(s), UacpiProcessorString);
        model  = strstr(s, "Model");
        family = strstr(s, "Family");
        if (model == NULL || family == NULL || model == s || family == s)
        {
            return STATUS_UNSUCCESSFUL;
        }
        RtlStringCbPrintfA(hw[0], sizeof(hw[0]), "ACPI\\%s", s);
        RtlStringCbPrintfA(hw[1], sizeof(hw[1]), "*%s", s);
        model[-1] = '\0';
        RtlStringCbPrintfA(hw[2], sizeof(hw[2]), "ACPI\\%s", s);
        RtlStringCbPrintfA(hw[3], sizeof(hw[3]), "*%s", s);
        family[-1] = '\0';
        RtlStringCbPrintfA(hw[4], sizeof(hw[4]), "ACPI\\%s", s);
        RtlStringCbPrintfA(hw[5], sizeof(hw[5]), "*%s", s);
        for (i = 0; i < 6; i++)
        {
            ids[i] = hw[i];
        }
        out = UacpiWideMultiSz(ids, 6);
        break;

    case BusQueryCompatibleIDs:
        ids[0] = "ACPI\\Processor";
        out = UacpiWideMultiSz(ids, 1);
        break;

    case BusQueryInstanceID:
        out = UacpiWideDup(Pdo->Instance);
        break;

    default:
        return Irp->IoStatus.Status;
    }

    if (out == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Irp->IoStatus.Information = (ULONG_PTR)out;
    return STATUS_SUCCESS;
}

NTSTATUS
UacpiPdoQueryId(PUACPI_PDO Pdo, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    char buf[64];
    const char *ids[2];
    PWSTR out = NULL;

    if (Pdo->IsProcessor)
    {
        return UacpipProcessorQueryId(Pdo, Irp);
    }

    switch (sp->Parameters.QueryId.IdType)
    {
    case BusQueryDeviceID:
    {
        const char *literal;

        if (Pdo->Hid[0])
        {
            RtlStringCbPrintfA(buf, sizeof(buf), "ACPI\\%s", Pdo->Hid);
        }
        else if ((literal = UacpipNoHidDeviceId(Pdo)) != NULL)
        {
            RtlStringCbCopyA(buf, sizeof(buf), literal);
        }
        else
        {
            RtlStringCbPrintfA(buf, sizeof(buf), "ACPI\\%s", Pdo->Name);
        }
        out = UacpiWideDup(buf);
        break;
    }

    case BusQueryHardwareIDs:
        if (Pdo->Hid[0])
        {
            char compat[64];
            RtlStringCbPrintfA(buf, sizeof(buf), "ACPI\\%s", Pdo->Hid);
            RtlStringCbPrintfA(compat, sizeof(compat), "*%s", Pdo->Hid);
            ids[0] = buf;
            ids[1] = compat;
            out = UacpiWideMultiSz(ids, 2);
        }
        else
        {
            const char *literal = UacpipNoHidDeviceId(Pdo);

            if (literal != NULL)
            {
                RtlStringCbCopyA(buf, sizeof(buf), literal);
            }
            else
            {
                RtlStringCbPrintfA(buf, sizeof(buf), "ACPI\\%s", Pdo->Name);
            }
            ids[0] = buf;
            out = UacpiWideMultiSz(ids, 1);
        }
        break;

    case BusQueryCompatibleIDs:
    {
        // *<HID>, then ACPI\<CID> and *<CID> for each _CID.
        char cbuf[16][40];
        const char *clist[16];
        ULONG n = 0;
        uacpi_pnp_id_list *cids = NULL;

        if (Pdo->Hid[0])
        {
            RtlStringCbPrintfA(cbuf[n], sizeof(cbuf[0]), "*%s", Pdo->Hid);
            clist[n] = cbuf[n];
            n++;
        }
        if (Pdo->Node != NULL &&
            uacpi_likely_success(uacpi_eval_cid(Pdo->Node, &cids)) && cids != NULL)
            {
            ULONG i;
            for (i = 0; i < cids->num_ids && n < RTL_NUMBER_OF(cbuf) - 1; i++)
            {
                RtlStringCbPrintfA(cbuf[n], sizeof(cbuf[0]), "ACPI\\%s",
                                   cids->ids[i].value);
                clist[n] = cbuf[n];
                n++;
                RtlStringCbPrintfA(cbuf[n], sizeof(cbuf[0]), "*%s", cids->ids[i].value);
                clist[n] = cbuf[n];
                n++;
            }
            uacpi_free_pnp_id_list(cids);
        }
        out = (n > 0) ? UacpiWideMultiSz(clist, n) : NULL;
        break;
    }

    case BusQueryInstanceID:
        // Namespace path built at PDO creation; unique across the tree.
        out = UacpiWideDup(Pdo->Instance);
        break;

    default:
        return Irp->IoStatus.Status;   // leave as-is (e.g. STATUS_NOT_SUPPORTED)
    }

    if (out == NULL && sp->Parameters.QueryId.IdType != BusQueryCompatibleIDs)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Irp->IoStatus.Information = (ULONG_PTR)out;
    return STATUS_SUCCESS;
}
