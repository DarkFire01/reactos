/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     uACPI kernel API on ntoskrnl
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include <ntddk.h>

#include <uacpi/kernel_api.h>
#include <uacpi/status.h>
#include <uacpi/log.h>
#include <uacpi/tables.h>   // uacpi_table_find_by_signature (MCFG -> ECAM)
#include <uacpi/acpi.h>     // struct acpi_mcfg / acpi_mcfg_allocation

#include <arc/arc.h>                    // CONFIGURATION_COMPONENT_DATA
#include <reactos/drivers/acpi/acpi.h>  // ACPI_BIOS_MULTI_NODE

// Loader block access for the "ACPI BIOS" node.
extern NTSYSAPI PLOADER_PARAMETER_BLOCK KeLoaderBlock;
extern NTSYSAPI
PCONFIGURATION_COMPONENT_DATA
NTAPI
KeFindConfigurationNextEntry(
    _In_ PCONFIGURATION_COMPONENT_DATA Child,
    _In_ CONFIGURATION_CLASS Class,
    _In_ CONFIGURATION_TYPE Type,
    _In_opt_ PULONG ComponentKey,
    _Inout_ PCONFIGURATION_COMPONENT_DATA *NextLink);

#include "uacpi_host.h"

#define UACPI_HOST_TAG   'HpcA'   // 'AcpH'

int UacpiHostVerbose = 1;

// Logging

static ULONG
UacpiHostDpfltrLevel(uacpi_log_level level)
{
    if (UacpiHostVerbose)
    {
        return DPFLTR_ERROR_LEVEL;   // force-visible during bring-up
    }
    switch (level)
    {
    case UACPI_LOG_ERROR: return DPFLTR_ERROR_LEVEL;
    case UACPI_LOG_WARN:  return DPFLTR_WARNING_LEVEL;
    default:              return DPFLTR_INFO_LEVEL;   // INFO/TRACE/DEBUG
    }
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *str)
{
    // uACPI already terminates the string with UACPI_END_OF_LOG_MSG ("\n").
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, UacpiHostDpfltrLevel(level), "[uacpi] %s", str);
}

// RSDP discovery

static PHYSICAL_ADDRESS UacpiHostRsdpOverridePa; // .QuadPart == 0 => not set

VOID UacpiHostRsdpOverride(PHYSICAL_ADDRESS RsdpPhysical)
{
    UacpiHostRsdpOverridePa = RsdpPhysical;
}

// Check the "RSD PTR " signature and the ACPI 1.0 checksum.
static BOOLEAN
UacpiHostRsdpValid(const UCHAR *p)
{
    static const UCHAR sig[8] = { 'R','S','D',' ','P','T','R',' ' };
    UCHAR sum = 0;
    ULONG i;

    for (i = 0; i < 8; i++)
    {
        if (p[i] != sig[i])
        {
            return FALSE;
        }
    }
    for (i = 0; i < 20; i++)   // ACPI 1.0 checksum covers bytes [0,20)
    {
        sum = (UCHAR)(sum + p[i]);
    }
    return sum == 0;
}

// Scan [start, start+len) on 16-byte boundaries. Returns the RSDP or 0.
static uacpi_phys_addr
UacpiHostScanForRsdp(ULONG_PTR start, ULONG len)
{
    PHYSICAL_ADDRESS pa;
    UCHAR *va;
    uacpi_phys_addr found = 0;
    ULONG off;

    pa.QuadPart = start;
    va = (UCHAR *)MmMapIoSpace(pa, len, MmCached);
    if (va == NULL)
    {
        return 0;
    }
    for (off = 0; off + 20 <= len; off += 16)
    {
        if (UacpiHostRsdpValid(va + off))
        {
            found = (uacpi_phys_addr)(start + off);
            break;
        }
    }
    MmUnmapIoSpace(va, len);
    return found;
}

// RSDP built around the root table in the loader's "ACPI BIOS" node.
static struct acpi_rsdp *UacpiHostLoaderRsdp;

static PACPI_BIOS_MULTI_NODE
UacpiHostFindLoaderAcpiNode(VOID)
{
    PCONFIGURATION_COMPONENT_DATA root, entry, next = NULL;
    PCM_PARTIAL_RESOURCE_LIST list;

    if (KeLoaderBlock == NULL || KeLoaderBlock->ConfigurationRoot == NULL)
    {
        return NULL;
    }

    // The ConfigurationRoot offset is NTDDI-gated in arc.h and unverified for
    // Win10 1507; only walk it if it looks like the ARC system node.
    root = KeLoaderBlock->ConfigurationRoot;
    if (!MmIsAddressValid(root) || !MmIsAddressValid((PUCHAR)(root + 1) - 1) ||
        root->Parent != NULL || root->ComponentEntry.Class != SystemClass)
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[acpi] host: loader ConfigurationRoot %p is not the ARC system node\n",
               root);
        return NULL;
    }

    for (entry = KeFindConfigurationNextEntry(root,
                                              AdapterClass, MultiFunctionAdapter,
                                              NULL, &next);
         entry != NULL;
         entry = KeFindConfigurationNextEntry(root,
                                              AdapterClass, MultiFunctionAdapter,
                                              NULL, &next))
    {
        if (entry->ComponentEntry.Identifier != NULL &&
            _stricmp(entry->ComponentEntry.Identifier, "ACPI BIOS") == 0)
        {
            break;
        }
        next = entry;
    }

    if (entry == NULL)
    {
        return NULL;
    }

    // The node follows a one-entry DeviceSpecific resource list.
    list = (PCM_PARTIAL_RESOURCE_LIST)entry->ConfigurationData;
    if (list == NULL ||
        entry->ComponentEntry.ConfigurationDataLength <
            FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors[1]) +
            FIELD_OFFSET(ACPI_BIOS_MULTI_NODE, E820Entry) ||
        list->Count < 1 ||
        list->PartialDescriptors[0].Type != CmResourceTypeDeviceSpecific)
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[acpi] host: loader ACPI BIOS node has no device-specific data\n");
        return NULL;
    }

    return (PACPI_BIOS_MULTI_NODE)(list + 1);
}

static UCHAR
UacpiHostChecksum(const UCHAR *buf, ULONG len)
{
    UCHAR sum = 0;
    ULONG i;

    for (i = 0; i < len; i++)
    {
        sum = (UCHAR)(sum + buf[i]);
    }
    return (UCHAR)(0 - sum);
}

// Returns the RSDP for the loader's root table, or 0. Built once and kept.
static uacpi_phys_addr
UacpiHostRsdpFromLoader(VOID)
{
    PACPI_BIOS_MULTI_NODE node;
    struct acpi_sdt_hdr *root;
    struct acpi_rsdp *rsdp;

    if (UacpiHostLoaderRsdp != NULL)
    {
        return (uacpi_phys_addr)MmGetPhysicalAddress(UacpiHostLoaderRsdp).QuadPart;
    }

    node = UacpiHostFindLoaderAcpiNode();
    if (node == NULL || node->RsdtAddress.QuadPart == 0)
    {
        return 0;
    }

    // The root table signature decides the RSDP revision.
    root = (struct acpi_sdt_hdr *)MmMapIoSpace(node->RsdtAddress, sizeof(*root),
                                               MmNonCached);
    if (root == NULL)
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[acpi] host: cannot map loader root table at %010I64X\n",
                  node->RsdtAddress.QuadPart);
        return 0;
    }

    rsdp = (struct acpi_rsdp *)ExAllocatePoolWithTag(NonPagedPool, sizeof(*rsdp),
                                                     UACPI_HOST_TAG);
    if (rsdp == NULL)
    {
        MmUnmapIoSpace(root, sizeof(*root));
        return 0;
    }
    RtlZeroMemory(rsdp, sizeof(*rsdp));
    RtlCopyMemory(rsdp->signature, "RSD PTR ", sizeof(rsdp->signature));
    RtlCopyMemory(rsdp->oemid, "ROS   ", sizeof(rsdp->oemid));

    if (RtlEqualMemory(root->signature, "XSDT", 4))
    {
        rsdp->revision = 2;
        rsdp->length = sizeof(*rsdp);
        rsdp->xsdt_addr = (uacpi_u64)node->RsdtAddress.QuadPart;
    }
    else if (RtlEqualMemory(root->signature, "RSDT", 4) &&
             node->RsdtAddress.QuadPart <= MAXULONG)
    {
        rsdp->revision = 0;
        rsdp->rsdt_addr = node->RsdtAddress.LowPart;
    }
    else
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[acpi] host: loader root table at %010I64X is '%.4s', "
                  "not a root table this can describe\n",
                  node->RsdtAddress.QuadPart, root->signature);
        MmUnmapIoSpace(root, sizeof(*root));
        ExFreePoolWithTag(rsdp, UACPI_HOST_TAG);
        return 0;
    }
    MmUnmapIoSpace(root, sizeof(*root));

    // Revision 2 adds a checksum over the whole structure.
    rsdp->checksum = UacpiHostChecksum((const UCHAR *)rsdp, 20);
    if (rsdp->revision >= 2)
    {
        rsdp->extended_checksum = UacpiHostChecksum((const UCHAR *)rsdp,
                                                    sizeof(*rsdp));
    }

    UacpiHostLoaderRsdp = rsdp;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[acpi] host: RSDP built for the loader's %s at %010I64X\n",
              rsdp->revision >= 2 ? "XSDT" : "RSDT", node->RsdtAddress.QuadPart);
    return (uacpi_phys_addr)MmGetPhysicalAddress(rsdp).QuadPart;
}

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address)
{
    uacpi_phys_addr rsdp;
    PHYSICAL_ADDRESS pa;
    UCHAR *va;
    ULONG_PTR ebda;

    if (UacpiHostRsdpOverridePa.QuadPart != 0)
    {
        *out_rsdp_address = (uacpi_phys_addr)UacpiHostRsdpOverridePa.QuadPart;
        return UACPI_STATUS_OK;
    }

    // 0) The loader's root table. The only source on UEFI.
    rsdp = UacpiHostRsdpFromLoader();
    if (rsdp != 0)
    {
        *out_rsdp_address = rsdp;
        return UACPI_STATUS_OK;
    }

    // 1) The 1 KB EBDA. Its segment is the word at physical 0x40E.
    pa.QuadPart = 0x40E;
    va = (UCHAR *)MmMapIoSpace(pa, sizeof(USHORT), MmCached);
    if (va != NULL)
    {
        ebda = ((ULONG_PTR)(*(USHORT *)va)) << 4;
        MmUnmapIoSpace(va, sizeof(USHORT));
        if (ebda >= 0x400 && ebda < 0xA0000)
        {
            rsdp = UacpiHostScanForRsdp(ebda, 0x400);
            if (rsdp != 0)
            {
                *out_rsdp_address = rsdp;
                return UACPI_STATUS_OK;
            }
        }
    }

    // 2) The BIOS read-only region 0xE0000 to 0xFFFFF.
    rsdp = UacpiHostScanForRsdp(0xE0000, 0x20000);
    if (rsdp != 0)
    {
        *out_rsdp_address = rsdp;
        return UACPI_STATUS_OK;
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[acpi] host: no RSDP from the loader and none in low memory\n");
    return UACPI_STATUS_NOT_FOUND;
}

// Physical memory mapping

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len)
{
    PHYSICAL_ADDRESS pa;
    ULONG_PTR pageBase = (ULONG_PTR)addr & ~((ULONG_PTR)PAGE_SIZE - 1);
    ULONG_PTR offset   = (ULONG_PTR)addr - pageBase;
    SIZE_T    mapLen   = ROUND_TO_PAGES(offset + len);
    UCHAR    *va;

    pa.QuadPart = (LONGLONG)pageBase;
    va = (UCHAR *)MmMapIoSpace(pa, mapLen, MmCached);
    if (va == NULL)
    {
        return UACPI_MAP_FAILED;
    }
    return va + offset;   // page-aligned VA + the preserved intra-page offset
}

void uacpi_kernel_unmap(void *addr, uacpi_size len)
{
    ULONG_PTR va       = (ULONG_PTR)addr;
    ULONG_PTR pageBase = va & ~((ULONG_PTR)PAGE_SIZE - 1);
    ULONG_PTR offset   = va - pageBase;
    SIZE_T    mapLen   = ROUND_TO_PAGES(offset + len);

    // Same page-aligned VA and length that uacpi_kernel_map mapped.
    MmUnmapIoSpace((void *)pageBase, mapLen);
}

// Allocation

void *uacpi_kernel_alloc(uacpi_size size)
{
    return ExAllocatePoolWithTag(NonPagedPool, size, UACPI_HOST_TAG);
}

void uacpi_kernel_free(void *mem)
{
    if (mem != NULL)   // NULL is allowed; ExFreePool rejects it
    {
        ExFreePoolWithTag(mem, UACPI_HOST_TAG);
    }
}

// PCI configuration space

// uacpi_pci_address does not fit in a handle on x86, so allocate one.

typedef struct _ACPI_HOST_PCI_DEVICE
{
    uacpi_pci_address addr;
    PUCHAR            EcamVa;    // mapped 4 KB config page (ECAM), NULL if type-1
} UACPI_HOST_PCI_DEVICE, *PUACPI_HOST_PCI_DEVICE;

// ECAM windows from MCFG. Without one, type-1 reaches segment 0, bytes 0-255.
typedef struct _ACPI_ECAM_ALLOC
{
    uacpi_u64 Base;
    uacpi_u16 Segment;
    uacpi_u8  StartBus;
    uacpi_u8  EndBus;
} UACPI_ECAM_ALLOC;

#define UACPI_ECAM_MAX 16
static UACPI_ECAM_ALLOC UacpiEcam[UACPI_ECAM_MAX];
static ULONG           UacpiEcamCount;
static BOOLEAN         UacpiEcamParsed;

static void
UacpiHostParseMcfgOnce(void)
{
    uacpi_table tbl;
    struct acpi_mcfg *mcfg;
    ULONG total, i;

    if (UacpiEcamParsed)
    {
        return;
    }
    UacpiEcamParsed = TRUE;   // parse once

    if (uacpi_unlikely_error(uacpi_table_find_by_signature("MCFG", &tbl)) ||
        tbl.ptr == NULL)
        {
        return;   // no MCFG => no ECAM (type-1 only)
    }
    mcfg = (struct acpi_mcfg *)tbl.ptr;
    if (mcfg->hdr.length > sizeof(struct acpi_mcfg))
    {
        total = (mcfg->hdr.length - (ULONG)sizeof(struct acpi_mcfg)) /
                (ULONG)sizeof(struct acpi_mcfg_allocation);
        for (i = 0; i < total && UacpiEcamCount < UACPI_ECAM_MAX; i++)
        {
            UacpiEcam[UacpiEcamCount].Base     = mcfg->entries[i].address;
            UacpiEcam[UacpiEcamCount].Segment  = mcfg->entries[i].segment;
            UacpiEcam[UacpiEcamCount].StartBus = mcfg->entries[i].start_bus;
            UacpiEcam[UacpiEcamCount].EndBus   = mcfg->entries[i].end_bus;
            UacpiEcamCount++;
        }
    }
    uacpi_table_unref(&tbl);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "[uacpi] ECAM: %u MCFG segment window(s)\n", UacpiEcamCount);
}

// Physical address of the function's 4 KB config page, or 0 without ECAM.
static uacpi_u64
UacpiHostEcamPagephys(const uacpi_pci_address *a)
{
    ULONG i;

    UacpiHostParseMcfgOnce();
    for (i = 0; i < UacpiEcamCount; i++)
    {
        if (UacpiEcam[i].Segment == a->segment &&
            a->bus >= UacpiEcam[i].StartBus && a->bus <= UacpiEcam[i].EndBus)
            {
            return UacpiEcam[i].Base +
                   ((uacpi_u64)(a->bus - UacpiEcam[i].StartBus) << 20) +
                   ((uacpi_u64)a->device << 15) +
                   ((uacpi_u64)a->function << 12);
        }
    }
    return 0;
}

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address,
                                          uacpi_handle *out_handle)
{
    PUACPI_HOST_PCI_DEVICE dev =
        (PUACPI_HOST_PCI_DEVICE)ExAllocatePoolWithTag(NonPagedPool, sizeof(*dev),
                                                     UACPI_HOST_TAG);
    uacpi_u64 page;

    if (dev == NULL)
    {
        return UACPI_STATUS_OUT_OF_MEMORY;
    }
    dev->addr   = address;
    dev->EcamVa = NULL;

    // MmMapIoSpace needs PASSIVE_LEVEL; otherwise use type-1.
    page = UacpiHostEcamPagephys(&address);
    if (page != 0 && KeGetCurrentIrql() == PASSIVE_LEVEL)
    {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = (LONGLONG)page;
        dev->EcamVa = (PUCHAR)MmMapIoSpace(pa, PAGE_SIZE, MmNonCached);
    }

    *out_handle = dev;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle)
{
    PUACPI_HOST_PCI_DEVICE dev = (PUACPI_HOST_PCI_DEVICE)handle;
    if (dev != NULL)
    {
        if (dev->EcamVa != NULL)
        {
            MmUnmapIoSpace(dev->EcamVa, PAGE_SIZE);
        }
        ExFreePoolWithTag(dev, UACPI_HOST_TAG);
    }
}

static ULONG
UacpiHostPciSlot(const uacpi_pci_address *a)
{
    PCI_SLOT_NUMBER slot;
    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber   = a->device;
    slot.u.bits.FunctionNumber = a->function;
    return slot.u.AsULONG;
}

// Unreachable or absent config space reads as all 0xFF.
static uacpi_status
UacpiHostPciRead(uacpi_handle handle, uacpi_size offset, void *value, ULONG width)
{
    PUACPI_HOST_PCI_DEVICE dev = (PUACPI_HOST_PCI_DEVICE)handle;
    ULONG got;

    // ECAM
    if (dev->EcamVa != NULL && (offset + width) <= PAGE_SIZE)
    {
        volatile PUCHAR p = dev->EcamVa + offset;
        switch (width)
        {
        case 1: *(uacpi_u8  *)value = READ_REGISTER_UCHAR(p);                 break;
        case 2: *(uacpi_u16 *)value = READ_REGISTER_USHORT((PUSHORT)p);       break;
        case 4: *(uacpi_u32 *)value = READ_REGISTER_ULONG((PULONG)p);         break;
        default: RtlFillMemory(value, width, 0xFF);                           break;
        }
        return UACPI_STATUS_OK;
    }

    // Type-1: segment 0, bytes 0-255 only.
    if (dev->addr.segment != 0 || (offset + width) > 256)
    {
        RtlFillMemory(value, width, 0xFF);
        return UACPI_STATUS_OK;
    }
    got = HalGetBusDataByOffset(PCIConfiguration, dev->addr.bus,
                                UacpiHostPciSlot(&dev->addr), value,
                                (ULONG)offset, width);
    if (got != width)
    {
        RtlFillMemory(value, width, 0xFF);
    }
    return UACPI_STATUS_OK;
}

static uacpi_status
UacpiHostPciWrite(uacpi_handle handle, uacpi_size offset, void *value, ULONG width)
{
    PUACPI_HOST_PCI_DEVICE dev = (PUACPI_HOST_PCI_DEVICE)handle;

    if (dev->EcamVa != NULL && (offset + width) <= PAGE_SIZE)
    {
        volatile PUCHAR p = dev->EcamVa + offset;
        switch (width)
        {
        case 1: WRITE_REGISTER_UCHAR(p, *(uacpi_u8 *)value);                  break;
        case 2: WRITE_REGISTER_USHORT((PUSHORT)p, *(uacpi_u16 *)value);       break;
        case 4: WRITE_REGISTER_ULONG((PULONG)p, *(uacpi_u32 *)value);         break;
        default: break;
        }
        return UACPI_STATUS_OK;
    }

    if (dev->addr.segment != 0 || (offset + width) > 256)
    {
        return UACPI_STATUS_OK;   // unreachable without an ECAM window
    }
    HalSetBusDataByOffset(PCIConfiguration, dev->addr.bus,
                          UacpiHostPciSlot(&dev->addr), value,
                          (ULONG)offset, width);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle d, uacpi_size o, uacpi_u8 *v)
{ return UacpiHostPciRead(d, o, v, 1); }
uacpi_status uacpi_kernel_pci_read16(uacpi_handle d, uacpi_size o, uacpi_u16 *v)
{ return UacpiHostPciRead(d, o, v, 2); }
uacpi_status uacpi_kernel_pci_read32(uacpi_handle d, uacpi_size o, uacpi_u32 *v)
{ return UacpiHostPciRead(d, o, v, 4); }

uacpi_status uacpi_kernel_pci_write8(uacpi_handle d, uacpi_size o, uacpi_u8 v)
{ return UacpiHostPciWrite(d, o, &v, 1); }
uacpi_status uacpi_kernel_pci_write16(uacpi_handle d, uacpi_size o, uacpi_u16 v)
{ return UacpiHostPciWrite(d, o, &v, 2); }
uacpi_status uacpi_kernel_pci_write32(uacpi_handle d, uacpi_size o, uacpi_u32 v)
{ return UacpiHostPciWrite(d, o, &v, 4); }

// System I/O

// The handle is the port base.

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len,
                                 uacpi_handle *out_handle)
{
    UNREFERENCED_PARAMETER(len);
    *out_handle = (uacpi_handle)(ULONG_PTR)base;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle)
{
    UNREFERENCED_PARAMETER(handle);
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size o, uacpi_u8 *v)
{ *v = READ_PORT_UCHAR((PUCHAR)((ULONG_PTR)h + o)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size o, uacpi_u16 *v)
{ *v = READ_PORT_USHORT((PUSHORT)((ULONG_PTR)h + o)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size o, uacpi_u32 *v)
{ *v = READ_PORT_ULONG((PULONG)((ULONG_PTR)h + o)); return UACPI_STATUS_OK; }

uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size o, uacpi_u8 v)
{ WRITE_PORT_UCHAR((PUCHAR)((ULONG_PTR)h + o), v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size o, uacpi_u16 v)
{ WRITE_PORT_USHORT((PUSHORT)((ULONG_PTR)h + o), v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size o, uacpi_u32 v)
{ WRITE_PORT_ULONG((PULONG)((ULONG_PTR)h + o), v); return UACPI_STATUS_OK; }

// Timing

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void)
{
    LARGE_INTEGER freq;
    LARGE_INTEGER ctr = KeQueryPerformanceCounter(&freq);
    ULONGLONG c = (ULONGLONG)ctr.QuadPart;
    ULONGLONG f = (ULONGLONG)freq.QuadPart;
    ULONGLONG secs, rem;

    if (f == 0)
    {
        return 0;
    }
    // ns = c * 1e9 / f, split to avoid 64-bit overflow (rem < f).
    secs = c / f;
    rem  = c % f;
    return secs * 1000000000ULL + (rem * 1000000000ULL) / f;
}

void uacpi_kernel_stall(uacpi_u8 usec)
{
    KeStallExecutionProcessor(usec);
}

void uacpi_kernel_sleep(uacpi_u64 msec)
{
    LARGE_INTEGER interval;
    interval.QuadPart = -(LONGLONG)(msec * 10000ULL);   // relative, 100ns units
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

// Log waits above APC_LEVEL. AML must run at PASSIVE_LEVEL.
static VOID
UacpiHostWarnIrql(const char *what, uacpi_u16 ms)
{
    KIRQL irql = KeGetCurrentIrql();

    if (irql > APC_LEVEL)
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[uacpi] ILLEGAL: %s at IRQL %u (timeout %u) - AML requires "
                   "PASSIVE_LEVEL; the caller is the bug\n",
                   what, (ULONG)irql, (ULONG)ms);
    }
}

// Millisecond timeout for KeWaitForSingleObject. 0xFFFF is infinite (NULL).
static PLARGE_INTEGER
UacpiHostTimeout(uacpi_u16 ms, LARGE_INTEGER *storage)
{
    if (ms == 0xFFFF)
    {
        return NULL;                             // infinite
    }
    storage->QuadPart = -(LONGLONG)((ULONGLONG)ms * 10000ULL);   // 0 => immediate
    return storage;
}

// Mutex  (binary, non-recursive, any-thread release => KSEMAPHORE(1,1))

uacpi_handle uacpi_kernel_create_mutex(void)
{
    PKSEMAPHORE sem = (PKSEMAPHORE)ExAllocatePoolWithTag(NonPagedPool,
                                                         sizeof(KSEMAPHORE),
                                                         UACPI_HOST_TAG);
    if (sem != NULL)
    {
        KeInitializeSemaphore(sem, 1, 1);
    }
    return sem;
}

void uacpi_kernel_free_mutex(uacpi_handle h)
{
    if (h != NULL)
    {
        ExFreePoolWithTag(h, UACPI_HOST_TAG);
    }
}

// Seconds between reports while an infinite mutex wait is stuck.
#define UACPI_MUTEX_WAIT_REPORT_SECONDS 5

// Mutex owners for the stuck report. A full table leaves the owner unknown.
#define UACPI_MUTEX_OWNER_MAX 64

static struct
{
    volatile PVOID Handle;
    volatile PVOID Owner;
} UacpiHostMutexOwners[UACPI_MUTEX_OWNER_MAX];

static VOID
UacpiHostMutexSetOwner(uacpi_handle h, PVOID owner)
{
    ULONG i;

    for (i = 0; i < UACPI_MUTEX_OWNER_MAX; i++)
    {
        if (UacpiHostMutexOwners[i].Handle == h)
        {
            UacpiHostMutexOwners[i].Owner = owner;
            return;
        }
    }
    if (owner == NULL)
    {
        return;                     // not tracked
    }
    for (i = 0; i < UACPI_MUTEX_OWNER_MAX; i++)
    {
        if (InterlockedCompareExchangePointer(&UacpiHostMutexOwners[i].Handle,
                                              h, NULL) == NULL)
        {
            UacpiHostMutexOwners[i].Owner = owner;
            return;
        }
    }
}

static PVOID
UacpiHostMutexGetOwner(uacpi_handle h)
{
    ULONG i;

    for (i = 0; i < UACPI_MUTEX_OWNER_MAX; i++)
    {
        if (UacpiHostMutexOwners[i].Handle == h)
        {
            return UacpiHostMutexOwners[i].Owner;
        }
    }
    return NULL;
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle h, uacpi_u16 timeout)
{
    LARGE_INTEGER to;
    NTSTATUS st;

    UacpiHostWarnIrql("acquire_mutex", timeout);

    // Infinite waits log every UACPI_MUTEX_WAIT_REPORT_SECONDS and keep waiting.
    if (timeout == 0xFFFF)
    {
        ULONG waited = 0;

        // Try without a timer first.
        to.QuadPart = 0;
        st = KeWaitForSingleObject((PKSEMAPHORE)h, Executive, KernelMode,
                                   FALSE, &to);
        if (st != STATUS_TIMEOUT)
        {
            goto done;
        }

        for (;;)
        {
            to.QuadPart =
                -((LONGLONG)UACPI_MUTEX_WAIT_REPORT_SECONDS * 10 * 1000 * 1000);
            st = KeWaitForSingleObject((PKSEMAPHORE)h, Executive, KernelMode,
                                       FALSE, &to);
            if (st != STATUS_TIMEOUT)
            {
                break;
            }

            waited += UACPI_MUTEX_WAIT_REPORT_SECONDS;
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "[uacpi] STUCK: mutex %p not acquired after %lu s "
                       "(waiter thread %p, irql %u; held by thread %p) - "
                       "still waiting\n",
                       h, waited, PsGetCurrentThread(),
                       (ULONG)KeGetCurrentIrql(),
                       UacpiHostMutexGetOwner(h));
        }
    }
    else
    {
        st = KeWaitForSingleObject((PKSEMAPHORE)h, Executive, KernelMode,
                                   FALSE, UacpiHostTimeout(timeout, &to));
    }

done:
    if (st == STATUS_SUCCESS)
    {
        UacpiHostMutexSetOwner(h, PsGetCurrentThread());
        return UACPI_STATUS_OK;
    }
    if (st == STATUS_TIMEOUT)
    {
        return UACPI_STATUS_TIMEOUT;
    }
    return UACPI_STATUS_INTERNAL_ERROR;
}

void uacpi_kernel_release_mutex(uacpi_handle h)
{
    UacpiHostMutexSetOwner(h, NULL);
    KeReleaseSemaphore((PKSEMAPHORE)h, IO_NO_INCREMENT, 1, FALSE);
}

// Event  (counting semaphore => KSEMAPHORE(0, MAXLONG))

uacpi_handle uacpi_kernel_create_event(void)
{
    PKSEMAPHORE sem = (PKSEMAPHORE)ExAllocatePoolWithTag(NonPagedPool,
                                                         sizeof(KSEMAPHORE),
                                                         UACPI_HOST_TAG);
    if (sem != NULL)
    {
        KeInitializeSemaphore(sem, 0, MAXLONG);
    }
    return sem;
}

void uacpi_kernel_free_event(uacpi_handle h)
{
    if (h != NULL)
    {
        ExFreePoolWithTag(h, UACPI_HOST_TAG);
    }
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle h, uacpi_u16 timeout)
{
    LARGE_INTEGER to;
    NTSTATUS st;

    UacpiHostWarnIrql("wait_for_event", timeout);
    st = KeWaitForSingleObject((PKSEMAPHORE)h, Executive, KernelMode,
                                        FALSE, UacpiHostTimeout(timeout, &to));
    return (st == STATUS_SUCCESS) ? UACPI_TRUE : UACPI_FALSE;
}

void uacpi_kernel_reset_event(uacpi_handle h)
{
    // No NT "drain" primitive: consume the count with non-blocking waits.
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    while (KeWaitForSingleObject((PKSEMAPHORE)h, Executive, KernelMode, FALSE,
                                 &zero) == STATUS_SUCCESS)
                                 {
        /* keep draining */
    }
}

// Thread id / interrupt flag

uacpi_thread_id uacpi_kernel_get_thread_id(void)
{
    // A KTHREAD pointer is unique and never (void*)-1.
    return (uacpi_thread_id)KeGetCurrentThread();
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void)
{
    KIRQL old;
    KeRaiseIrql(HIGH_LEVEL, &old);   // masks all device interrupts on this CPU
    return (uacpi_interrupt_state)old;
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state)
{
    KeLowerIrql((KIRQL)state);
}

// Spinlock  (may be used in interrupt context)

uacpi_handle uacpi_kernel_create_spinlock(void)
{
    PKSPIN_LOCK lock = (PKSPIN_LOCK)ExAllocatePoolWithTag(NonPagedPool,
                                                          sizeof(KSPIN_LOCK),
                                                          UACPI_HOST_TAG);
    if (lock != NULL)
    {
        KeInitializeSpinLock(lock);
    }
    return lock;
}

void uacpi_kernel_free_spinlock(uacpi_handle h)
{
    if (h != NULL)
    {
        ExFreePoolWithTag(h, UACPI_HOST_TAG);
    }
}

// Firmware requests (AML Breakpoint / Fatal)

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req)
{
    switch (req->type)
    {
    case UACPI_FIRMWARE_REQUEST_TYPE_BREAKPOINT:
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[uacpi] AML Breakpoint (ctx %p)\n", req->breakpoint.ctx);
        break;
    case UACPI_FIRMWARE_REQUEST_TYPE_FATAL:
        // Log only; do not bugcheck.
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[uacpi] AML Fatal: type=0x%X code=0x%X arg=0x%I64X\n",
                   req->fatal.type, req->fatal.code, req->fatal.arg);
        break;
    default:
        break;
    }
    return UACPI_STATUS_OK;
}

// SCI, deferred work, spinlocks and signal_event are in uacpi_isr.c.
