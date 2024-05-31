
#include <freeldr.h>
#include <debug.h>
DBG_DEFAULT_CHANNEL(WINDOWS);
#include <internal/arm/mm.h>
//#include <internal/arm/intrin_i.h>
#include "../../winldr.h"

PLOADER_PARAMETER_BLOCK PubLoaderBlockVA;
KERNEL_ENTRY_POINT PubKiSystemStartup;

extern PVOID OsLoaderBase;
extern SIZE_T OsLoaderSize;

PHARDWARE_PTE Ttbr1_addr;
PHARDWARE_PTE Ttbr0_addr;

#define UINT32 u32
#define UINT64 u64
#define UINT8 u8

// signed types
typedef CHAR i8;
typedef INT16 i16;
typedef INT32 i32;
typedef INT64 i64;

// unsigned types
typedef UINT8 u8;
typedef UINT16 u16;
typedef UINT32 u32;
typedef UINT64 u64;

typedef size_t ptr_t;
#define ADDR_TO_PTR(addr) ((void*)((ptr_t)(addr)))

#if defined(__x86_64__) || defined(__aarch64__)
typedef i64 ssize_t;
#elif defined(__i386__)
typedef i32 ssize_t;
#else
#error unknown architecture
#endif

enum page_type {
    // 4K pages
    PAGE_TYPE_NORMAL = 0,

    // 2/4M pages
    PAGE_TYPE_HUGE = 1,
};

struct page_mapping_spec {
    struct page_table *pt;

    u64 virtual_base;
    u64 physical_base;

    size_t count;
    enum page_type type;
    BOOLEAN critical;
};

struct page_table {
    void *root;
    void (*write_slot)(void*, u64);
    u64 (*read_slot)(void*);
    u64 max_table_address;
    u64 entry_address_mask;
    u8 table_width_shift;
    u8 levels;
    u8 entry_width;
    u8 base_shift;
};

static inline ptr_t pt_get_root(struct page_table *pt)
{
    return (ptr_t)pt->root;
}

static inline size_t page_shift(struct page_table *pt)
{
    return pt->base_shift;
}

static inline size_t huge_page_shift(struct page_table *pt)
{
    return page_shift(pt) + pt->table_width_shift;
}

static inline size_t huge_page_size(struct page_table *pt)
{
    return 1ul << huge_page_shift(pt);
}

static inline size_t page_size(struct page_table *pt)
{
    return 1ul << pt->base_shift;
}


static u8 g_current_el;
static u64 g_ips_bits;
u32 g_aarch64_access_flag_mask;

BOOLEAN handover_flags_map[32] = { 0 };


#define BIT_MASK(start_bit, end_bit) \
    (((1ull << ((end_bit) - (start_bit))) - 1) << start_bit)



    #define ALIGN_UP_MASK(x, mask)   (((x) + (mask)) & ~(mask))
#undef ALIGN_UP
#define ALIGN_UP(x, val)         ALIGN_UP_MASK(x, (typeof(x))(val) - 1)

#define ALIGN_DOWN_MASK(x, mask) ((x) & ~(mask))
#undef ALIGN_DOWN
#define ALIGN_DOWN(x, val)       ALIGN_DOWN_MASK(x, (typeof(x))(val) - 1)

#define IS_ALIGNED_MASK(x, mask) (((x) & (mask)) == 0)
#define IS_ALIGNED(x, val)       IS_ALIGNED_MASK(x, (typeof(x))(val) - 1)
#undef PAGE_ROUND_DOWN
#undef PAGE_ROUND_UP
#define PAGE_ROUND_UP(size)   ALIGN_UP(size, PAGE_SIZE)
#define PAGE_ROUND_DOWN(size) ALIGN_DOWN(size, PAGE_SIZE)


static u64 get_feature_bits(u64 features, u64 first_bit, u64 last_bit)
{
    return (features & BIT_MASK(first_bit, last_bit + 1)) >> first_bit;
}
#define HO_AARCH64_52_BIT_IA_BIT 28
#define HO_AARCH64_52_BIT_IA (1 << HO_AARCH64_52_BIT_IA_BIT)

#define MMFR0_PARange_START 0
#define MMFR0_PARange_END 3
#define MMFR0_PARange_32BITS 0b0000
#define MMFR0_PARange_36BITS 0b0001
#define MMFR0_PARange_40BITS 0b0010
#define MMFR0_PARange_42BITS 0b0011
#define MMFR0_PARange_44BITS 0b0100
#define MMFR0_PARange_48BITS 0b0101
#define MMFR0_PARange_52BITS 0b0110

#define MMFR0_TGran4_START 28
#define MMFR0_TGran4_END 31
#define MMFR0_TGran4_SUPPORTED 0b0000
#define MMFR0_TGran4_SUPPORTED_52_BIT 0b0001
#define MMFT0_TGran4_UNSUPPORTED 0b1111

#define MMFR1_HFDBS_START 0
#define MMFR1_HFDBS_END 4

#define MMFR1_VH_START 8
#define MMFR1_VH_END 11
#define MMFR1_VH_PRESENT 0b0001
#define PAGE_PRESENT   (1ull << 0)

/*
 * This is supposed to be an index into the APTable, but it's located at
 * different offsets depending on whether this is a table or a block
 * descriptor. We currently don't have such abstraction, so just hardcode
 * this to zero.
*/
#define PAGE_READWRITE (0)

#define PAGE_AARCH64_BLOCK_OR_PAGE_DESCRIPTOR (0ull << 1)
#define PAGE_AARCH64_TABLE_DESCRIPTOR (1ull << 1)
#define PAGE_AARCH64_ACCESS_FLAG (1ull << 10)

#define PAGE_NORMAL (PAGE_AARCH64_TABLE_DESCRIPTOR | \
                     g_aarch64_access_flag_mask)
#define PAGE_HUGE (PAGE_AARCH64_BLOCK_OR_PAGE_DESCRIPTOR | \
                   g_aarch64_access_flag_mask)


struct handover_info_aarch64 {
    u64 arg0, arg1;
    u64 entrypoint;
    u64 stack;
    u64 direct_map_base;

    // Same for all ELs
    u64 ttbr0, ttbr1;
    u64 mair, tcr;
    u64 sctlr;

    BOOLEAN unmap_lower_half;
};


/*
 * Generic handover info structure:
 * entrypoint -> address of the kernel binary entry, possibly higher half
 * stack -> address of the top of the kernel stack, possibly higher half
 * arg0, arg1 -> arguments to pass to the kernel binary entrypoint
 * direct_map_base -> base address in the higher half that direct maps at least
 *                    'handover_get_minimum_map_length()' amount of
 *                    physical memory.
 * pt -> page table that will be switched to before handing control to kernel
 * flags -> flags that describe the expected system state before 'entrypoint'
 *          is invoked, some are arch-specific.
 *
 * Page table is expected to contain at least two mappings, where both linearly
 * map physical ram from address zero:
 * 0x0000...0000 -> handover_get_minimum_map_length()
 * AND
 * direct_map_base -> handover_get_minimum_map_length()
 */
struct handover_info {
    u64 entrypoint;
    u64 stack;
    u64 arg0, arg1;
    u64 direct_map_base;

    struct page_table pt;
/*
 * If set, unmaps the first table or handover_get_minimum_map_length()
 * worth of pages from the page table root, whichever one is bigger.
 */
#define HO_HIGHER_HALF_ONLY_BIT 0
#define HO_HIGHER_HALF_ONLY (1 << HO_HIGHER_HALF_ONLY_BIT)

/*
 * Arch-specific flags are described in <arch>/include/handover_flags.h
 */
    u32 flags;
};


u64 ultra_higher_half_base(u32 flags)
{
    return 0xFFFFFFFF80000000;
}

void kernel_handover_aarch64(struct handover_info_aarch64 *hia);

u32 current_el(void);
u64 read_id_aa64mmfr0_el1(void);
u64 read_id_aa64mmfr1_el1(void);

u64 read_hcr_el2(void);
void write_hcr_el2(u64);

void initialize_flags_map(void)
{
    u64 mmfr0, mmfr1, tgran4, parange;
    u8 parange_bits;
    BOOLEAN has_vhe, has_hafdbs;

    g_current_el = current_el();
    TRACE("running at EL%u\n", current_el());

    mmfr0 = read_id_aa64mmfr0_el1();

    tgran4 = get_feature_bits(mmfr0, MMFR0_TGran4_START, MMFR0_TGran4_END);
    switch (tgran4) { 
    case MMFR0_TGran4_SUPPORTED_52_BIT:
    TRACE("52-bit IA w/ 4K granule is supported\n");
        handover_flags_map[HO_AARCH64_52_BIT_IA_BIT] = TRUE;
    case MMFR0_TGran4_SUPPORTED:
        break;
    case MMFT0_TGran4_UNSUPPORTED:
    TRACE("CPU doesn't support 4K translation granule\n");
    default:
        TRACE("TGran4 %d\n", tgran4);
    }

    parange = get_feature_bits(mmfr0, MMFR0_PARange_START, MMFR0_PARange_END);
    switch (parange) {
    case MMFR0_PARange_32BITS:
        parange_bits = 32;
        break;
    case MMFR0_PARange_36BITS:
        parange_bits = 36;
        break;
    case MMFR0_PARange_40BITS:
        parange_bits = 40;
        break;
    case MMFR0_PARange_42BITS:
        parange_bits = 42;
        break;
    case MMFR0_PARange_44BITS:
        parange_bits = 44;
        break;
    case MMFR0_PARange_48BITS:
        parange_bits = 48;
        break;
    case MMFR0_PARange_52BITS:
        parange_bits = 52;
        break;
    default:
    TRACE("PARange %d\n", parange);
    }


    g_ips_bits = parange << 32;

    mmfr1 = read_id_aa64mmfr1_el1();
    /*
     * We cannot provide proper higher half mappings in EL2 if FEAT_VHE is not
     * supported since TTBR1_EL2 is not accessible.
     *
     * There are multiple ways to solve this:
     * - Just drop down to EL1 and load TTBR1_EL1. Sure, this works. However,
     *   this forces the loader to take responsibility for having set up every
     *   system register correctly and doing full hardware feature detect prior
     *   to dropping down to EL1 as the actual kernel won't be able to do it on
     *   its own since it has no access to EL2 registers after handoff. No, we
     *   are not doing this.
     * - Just split the TTBR0_EL2 address space in half and consider its upper
     *   half "the upper half". This requires the kernel to be linked
     *   specifically for that scenario, which is not acceptable. So not an
     *   option either.
     * - Just don't configure any registers and rely on the hardware to having
     *   set them up correctly beforehand. Yeah, no.
     */
    has_vhe = get_feature_bits(
        mmfr1,
        MMFR1_VH_START,
        MMFR1_VH_END
    ) == MMFR1_VH_PRESENT;

    if (!has_vhe && g_current_el == 2)
        TRACE("EL2 boot is not supported without FEAT_VHE support\n");

    has_hafdbs = get_feature_bits(mmfr1, MMFR1_HFDBS_START, MMFR1_HFDBS_END);
    TRACE("Hardware Access flag management: %s\n", has_hafdbs ? "yes" : "no");
    if (!has_hafdbs)
        g_aarch64_access_flag_mask = PAGE_AARCH64_ACCESS_FLAG;
}



#define KB 1024
#define MB (KB * KB)
#define GB (KB*KB*KB)

u64 handover_get_minimum_map_length(u64 direct_map_base, u32 flags)
{
    return 4ull * GB;
}

u64 handover_get_max_pt_address(u64 direct_map_base, u32 flags)
{

    // No known limitations
    return 0xFFFFFFFFFFFFFFFF;
}

void handover_prepare_for(struct handover_info *hi)
{

}

#define MMFR0_PARange_START 0
#define MMFR0_PARange_END 3
#define MMFR0_PARange_32BITS 0b0000
#define MMFR0_PARange_36BITS 0b0001
#define MMFR0_PARange_40BITS 0b0010
#define MMFR0_PARange_42BITS 0b0011
#define MMFR0_PARange_44BITS 0b0100
#define MMFR0_PARange_48BITS 0b0101
#define MMFR0_PARange_52BITS 0b0110

#define MMFR0_TGran4_START 28
#define MMFR0_TGran4_END 31
#define MMFR0_TGran4_SUPPORTED 0b0000
#define MMFR0_TGran4_SUPPORTED_52_BIT 0b0001
#define MMFT0_TGran4_UNSUPPORTED 0b1111

#define MMFR1_HFDBS_START 0
#define MMFR1_HFDBS_END 4

#define MMFR1_VH_START 8
#define MMFR1_VH_END 11
#define MMFR1_VH_PRESENT 0b0001


#define NORMAL_NON_CACHEABLE 0b00
#define OUTER_SHAREABLE 0b10

#define TCR_DS (1ull << 59)
#define TCR_HA (1ull << 39)
#define TCR_TG1_4K_GRANULE (0b10 << 30)
#define TCR_TG0_4K_GRANULE (0b00 << 14)
#define TCR_SH1_SHIFT 28
#define TCR_ORGN1_SHIFT 26
#define TCR_IRGN1_SHIFT 24
#define TCR_SH0_SHIFT 12
#define TCR_ORGN0_SHIFT 10
#define TCR_IRGN0_SHIFT 8

#define TCR_T1SZ_SHIFT 16
#define TCR_T0SZ_SHIFT 0

static u64 build_tcr()
{
    u64 ret = 0;
    u32 tsz;

    if (g_aarch64_access_flag_mask != PAGE_AARCH64_ACCESS_FLAG)
        ret |= TCR_HA;

    ret |= g_ips_bits;

    ret |= NORMAL_NON_CACHEABLE << TCR_IRGN0_SHIFT;
    ret |= NORMAL_NON_CACHEABLE << TCR_ORGN0_SHIFT;
    ret |= OUTER_SHAREABLE << TCR_SH0_SHIFT;
    ret |= TCR_TG1_4K_GRANULE;

    if (0) {
        tsz = 64 - 52;

        /*
         * NOTE: We enable DS simply for the sake of having access to 52-bit
         *       input addresses, we don't actually support the custom PA
         *       format where the upper bits of the address are actually stored
         *       in the lower bits of a PTE, so we rely on those bits to always
         *       be equal to zero, this can obviously break in the future.
         * TODO: add an abstraction for this and implement it properly.
         */
        ret |= TCR_DS;
    } else {
        tsz = 64 - 48;
    }

    ret |= tsz << TCR_T1SZ_SHIFT;

    ret |= NORMAL_NON_CACHEABLE << TCR_IRGN1_SHIFT;
    ret |= NORMAL_NON_CACHEABLE << TCR_ORGN1_SHIFT;
    ret |= OUTER_SHAREABLE << TCR_SH1_SHIFT;

    ret |= TCR_TG0_4K_GRANULE;
    ret |= tsz << TCR_T0SZ_SHIFT;

    return ret;
}

#define HCR_E2H (1ull << 34)
#define HCR_TGE (1ull << 27)
#define SCTLR_SA (1 << 3)
#define SCTLR_M (1 << 0)

#define MAIR_NON_CACHEABLE 0b0100
#define MAIR_I_SHIFT 0
#define MAIR_O_SHIFT 4

enum pt_type {
    PT_TYPE_AARCH64_4K_GRANULE_48_BIT = 4,
    PT_TYPE_AARCH64_4K_GRANULE_52_BIT = 5,
};

enum pt_constraint {
    PT_CONSTRAINT_AT_LEAST,
    PT_CONSTRAINT_EXACTLY,
    PT_CONSTRAINT_MAX,
};

u32 read_u32(void *ptr) { return *(u32*)ptr; }
u64 read_u32_zero_extend(void *ptr) { return read_u32(ptr); }

u64 read_u64(void *ptr) { return *(u64*)ptr; }

void write_u32(void *ptr, u32 val)
{
    u32 *dword = ptr;
    *dword = val;
}

void write_u32_u64(void *ptr, u64 val)
{
    write_u32(ptr, (u32)val);
}

void write_u32_checked_u64(void *ptr, u64 val)
{
  //  BUG_ON(val > 0xFFFFFFFF);
    write_u32_u64(ptr, val);
}

void write_u64(void *ptr, u64 val)
{
    u64 *qword = ptr;
    *qword = val;
}


static inline size_t pt_depth(enum pt_type pt)
{
    return (size_t)pt;
}

static inline BOOLEAN pt_is_huge_page(u64 entry)
{
    return (entry & PAGE_AARCH64_TABLE_DESCRIPTOR) == 0;
}

#define ALLOCATOR_DEFAULT_CEILING (4ull * GB)
#define ALLOCATOR_DEFAULT_ALLOC_TYPE MEMORY_TYPE_LOADER_RECLAIMABLE

// ALLOCATE_CEILING is implicit if ALLOCATE_PRECISE is not set
#define ALLOCATE_PRECISE  (1 << 0)
#define ALLOCATE_CRITICAL (1 << 1)
#define ALLOCATE_STACK    (1 << 2)

struct allocation_spec {
    union {
        u64 addr;
        u64 ceiling;
    };

    size_t pages;

    u32 flags;
    u32 type;
};


void *pt_get_table_page(u64 max_address)
{
    struct allocation_spec spec = {
        .ceiling = max_address,
        .pages = 1,
    };

    if (!spec.ceiling || spec.ceiling > (4ull * GB))
        spec.ceiling = 4ull * GB;

    return ADDR_TO_PTR(MmAllocateMemoryWithType(PAGE_SIZE, LoaderFirmwarePermanent));
}

/* FUNCTIONS **************************************************************/

static u8 unified_pt_depth(enum pt_type type)
{
    return pt_depth(type) + 1;
}

void page_table_init(struct page_table *pt, enum pt_type type,
                     u64 max_table_address)
{
    pt->root = pt_get_table_page(max_table_address);


    pt->levels = unified_pt_depth(type);
    pt->base_shift = PAGE_SHIFT;
    pt->max_table_address = max_table_address;

    // We currently don't support 52-bit OA, so this is the mask
    pt->entry_address_mask = ~(BIT_MASK(48, 64) | BIT_MASK(0, PAGE_SHIFT));

    RtlZeroMemory(pt->root, PAGE_SIZE);

    pt->entry_width = 8;
    pt->table_width_shift = 9;
    pt->write_slot = write_u64;
    pt->read_slot = read_u64;
}

#define LOOKUP_LEVEL_MINUS_1 4
#define LOOKUP_LEVEL_MINUS_1_WIDTH_SHIFT 4

u8 pt_table_width_shift_for_level(struct page_table *pt, size_t idx)
{
    if (pt->levels == unified_pt_depth(PT_TYPE_AARCH64_4K_GRANULE_52_BIT) &&
        idx == LOOKUP_LEVEL_MINUS_1)
        return LOOKUP_LEVEL_MINUS_1_WIDTH_SHIFT;

    return pt->table_width_shift;
}


BOOLEAN ultra_configure_pt_type(struct handover_info *hi, u8 pt_levels,
                             enum pt_constraint constraint,
                             enum pt_type *out_type)
{
    enum pt_type type = PT_TYPE_AARCH64_4K_GRANULE_48_BIT;

    if ((pt_levels == 5 || constraint == PT_CONSTRAINT_AT_LEAST) &&
         1)
    {
        hi->flags |= HO_AARCH64_52_BIT_IA;
        type = PT_TYPE_AARCH64_4K_GRANULE_52_BIT;
    }

    if (pt_levels == 5 && type != PT_TYPE_AARCH64_4K_GRANULE_52_BIT &&
        constraint != PT_CONSTRAINT_MAX)
        return FALSE;

    *out_type = type;
    return TRUE;
}

static size_t get_level_bit_offset(struct page_table *pt, size_t idx)
{
    return pt->base_shift + (pt->table_width_shift * idx);
}


static size_t get_level_index(struct page_table *pt, u64 virtual_address,
                              size_t level)
{
    u8 width_shift;
    size_t table_width_mask;

    width_shift = pt_table_width_shift_for_level(pt, level);

    table_width_mask = (1 << width_shift) - 1;
    u64 table_selector = virtual_address >> get_level_bit_offset(pt, level);

    return table_selector & table_width_mask;
}


u64 pt_get_root_pte_at(struct page_table *pt, u64 virtual_address)
{
    size_t idx;
    u64 ret = 0;

    idx = get_level_index(pt, virtual_address, pt->levels - 1);
    memcpy(&ret, (PVOID)((ULONG_PTR)pt->root + idx * pt->entry_width), pt->entry_width);

    return (u64)(ret & pt->entry_address_mask);
}



VOID
WinLdrSetupMachineDependent(
    PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    TRACE("\n\nWinLdrSetupMachineDependent: Entry\n");


    struct handover_info_aarch64 hia = (struct handover_info_aarch64) {
        .arg0 = 0,
        .arg1 = 0,
        .direct_map_base = (u64)ultra_higher_half_base(0),
        .entrypoint = (u64)NULL,
        .stack =  (u64)MmAllocateMemoryWithType(PAGE_SIZE * 4, LoaderFirmwarePermanent),
        .unmap_lower_half = HO_HIGHER_HALF_ONLY,
    };
    initialize_flags_map();
    handover_flags_map[HO_HIGHER_HALF_ONLY_BIT] = TRUE;


    hia.ttbr0 = pt_get_root_pte_at(MmAllocateMemoryWithType(PAGE_SIZE, LoaderFirmwarePermanent), 0x0000000000000000);
    hia.ttbr1 = pt_get_root_pte_at(MmAllocateMemoryWithType(PAGE_SIZE, LoaderFirmwarePermanent), (u64)ultra_higher_half_base(0));
    

    /*
     * Enable E2H if running at EL2 to enable TTBR1_EL2
     * TGE is enabled for sanity reasons
     */
    if (g_current_el == 2) {
        // NOTE: VHE support is verified during initialization
        u64 hcr;

        hcr = read_hcr_el2();
        hcr |= HCR_E2H | HCR_TGE;
        write_hcr_el2(hcr);
    }


        // Just play it safe
        hia.mair = (MAIR_NON_CACHEABLE << MAIR_O_SHIFT) |
        (MAIR_NON_CACHEABLE << MAIR_I_SHIFT);
        hia.tcr = build_tcr();

// Cache disabled, stack alignment checking enabled, MMU enabled
    hia.sctlr = SCTLR_SA | SCTLR_M;

    kernel_handover_aarch64(&hia);
    for(;;)
    {

    }
    /* Create page tables */
}



























































































































 
static
BOOLEAN
WinLdrMapSpecialPages()
{
    TRACE("WinLdrMapSpecialPages: Entry\n");
    /* TODO: Map in the kernel CPTs */
    return TRUE;
}


BOOLEAN
MempIsPageMapped(PVOID VirtualAddress)
{
    return TRUE;
}


static
PHARDWARE_PTE
MempGetOrCreatePageDir(PHARDWARE_PTE PdeBase, ULONG Index)
{
    return 0;
}

static
BOOLEAN
MempMapSinglePage(ULONG64 VirtualAddress, ULONG64 PhysicalAddress)
{
    return TRUE;
}

static
PFN_NUMBER
MempMapRangeOfPages(ULONG64 VirtualAddress, ULONG64 PhysicalAddress, PFN_NUMBER cPages, BOOLEAN IsTtbr1)
{
    PFN_NUMBER i;

    for (i = 0; i < cPages; i++)
    {
        if (!MempMapSinglePage(VirtualAddress, PhysicalAddress))
        {
            ERR("Failed to map page %ld from %p to %p\n",
                    i, (PVOID)VirtualAddress, (PVOID)PhysicalAddress);
            return i;
        }
        VirtualAddress += PAGE_SIZE;
        PhysicalAddress += PAGE_SIZE;
    }
    return i;
}


BOOLEAN
MempSetupPaging(IN PFN_NUMBER StartPage,
                IN PFN_NUMBER NumberOfPages,
                IN BOOLEAN KernelMapping)
{
    TRACE(">>> MempSetupPaging(0x%lx, %ld, %p)\n",
            StartPage, NumberOfPages, StartPage * PAGE_SIZE + KSEG0_BASE);

    /* map this */
    return TRUE;
}

void write_ttbr1_el1(ULONG_PTR Tbbr1Addr);
void write_ttbr0_el1(ULONG_PTR Tbbr0Addr);
void SmashTlb();
UINT64 GetSCTLREL1();
void SetSCTLREL1(UINT64 Value);
void SetSCTLREL1_hold(UINT64 Value);\

static
BOOLEAN
MempAllocatePageTables(VOID)
{
    TRACE(">>> MempAllocatePageTables\n");


    return TRUE;
}
void TriggerIsb();
void HoldForArmTests();
void  write_tcr(UINT64 tcr);
UINT64  read_tcr();
/* ************************************************************************/
#define MMUBIT 1;
#define DISABLE_MMU_BIT 0
VOID
WinLdrSetProcessorContext(VOID)
{

    TRACE("WinLdrSetProcessorContext: Entry\n");

   // TRACE("Paging enabled\n");
    for(;;)
    {

    }
    /* enable paging */
}
extern KERNEL_ENTRY_POINT PubKiSystemStartup;
extern PLOADER_PARAMETER_BLOCK PubLoaderBlock;
VOID
ContinueExecution()
{
    TRACE("test");
    (*PubKiSystemStartup)(PubLoaderBlock);
    for(;;)
    {

    }
}

VOID
MempUnmapPage(IN PFN_NUMBER Page)
{
    TRACE("Unmapping %X\n", Page);
    return;
}

VOID
MempDump(VOID)
{
    return;
}
