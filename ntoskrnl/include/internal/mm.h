#pragma once

#include <internal/arch/mm.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TYPES *********************************************************************/

struct _EPROCESS;

extern PMMSUPPORT MmKernelAddressSpace;
extern PFN_COUNT MiFreeSwapPages;
extern PFN_COUNT MiUsedSwapPages;
extern PFN_COUNT MmNumberOfPhysicalPages;
extern UCHAR MmDisablePagingExecutive;
extern PFN_NUMBER MmLowestPhysicalPage;
extern PFN_NUMBER MmHighestPhysicalPage;
extern PFN_NUMBER MmAvailablePages;
extern PFN_NUMBER MmResidentAvailablePages;
extern ULONG MmThrottleTop;
extern ULONG MmThrottleBottom;

extern LIST_ENTRY MmLoadedUserImageList;

extern KMUTANT MmSystemLoadLock;

extern ULONG MmNumberOfPagingFiles;

extern SIZE_T MmTotalNonPagedPoolQuota;
extern SIZE_T MmTotalPagedPoolQuota;

extern PVOID MmUnloadedDrivers;
extern PVOID MmLastUnloadedDrivers;
extern PVOID MmTriageActionTaken;
extern PVOID KernelVerifier;
extern MM_DRIVER_VERIFIER_DATA MmVerifierData;

extern SIZE_T MmTotalCommitLimit;
extern SIZE_T MmTotalCommittedPages;
extern SIZE_T MmSharedCommit;
extern SIZE_T MmDriverCommit;
extern SIZE_T MmProcessCommit;
extern SIZE_T MmPagedPoolCommit;
extern SIZE_T MmPeakCommitment;
extern SIZE_T MmtotalCommitLimitMaximum;

extern PVOID MiDebugMapping; // internal
extern PMMPTE MmDebugPte; // internal

extern KSPIN_LOCK MmPfnLock;

struct _KTRAP_FRAME;
struct _EPROCESS;

//
// Pool Quota values
//
#define MI_QUOTA_NON_PAGED_NEEDED_PAGES             64
#define MI_NON_PAGED_QUOTA_MIN_RESIDENT_PAGES       200
#define MI_CHARGE_PAGED_POOL_QUOTA                  0x80000
#define MI_CHARGE_NON_PAGED_POOL_QUOTA              0x10000

//
// Special IRQL value (found in assertions)
//
#define MM_NOIRQL ((KIRQL)0xFFFFFFFF)

//
// MmDbgCopyMemory Flags
//
#define MMDBG_COPY_WRITE            0x00000001
#define MMDBG_COPY_PHYSICAL         0x00000002
#define MMDBG_COPY_UNSAFE           0x00000004
#define MMDBG_COPY_CACHED           0x00000008
#define MMDBG_COPY_UNCACHED         0x00000010
#define MMDBG_COPY_WRITE_COMBINED   0x00000020

//
// Maximum chunk size per copy
//
#define MMDBG_COPY_MAX_SIZE         0x8

/* Although Microsoft says this isn't hardcoded anymore,
   they won't be able to change it. Stuff depends on it */
#define MM_VIRTMEM_GRANULARITY              (64 * 1024)

#define PAGED_POOL_MASK                     1
#define MUST_SUCCEED_POOL_MASK              2
#define CACHE_ALIGNED_POOL_MASK             4
#define QUOTA_POOL_MASK                     8
#define SESSION_POOL_MASK                   32
#define VERIFIER_POOL_MASK                  64

#define MAX_PAGING_FILES                    (16)

/* PAGE_ROUND_UP and PAGE_ROUND_DOWN equivalent, with support for 64-bit-only data types */
#define PAGE_ROUND_UP_64(x) \
    (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

#define PAGE_ROUND_DOWN_64(x) \
    ((x) & ~(PAGE_SIZE - 1))

#ifdef _M_AMD64
#define InterlockedCompareExchangePte(PointerPte, Exchange, Comperand) \
    InterlockedCompareExchange64((PLONG64)(PointerPte), Exchange, Comperand)

#define InterlockedExchangePte(PointerPte, Value) \
    InterlockedExchange64((PLONG64)(PointerPte), Value)
#else
#define InterlockedCompareExchangePte(PointerPte, Exchange, Comperand) \
    InterlockedCompareExchange((PLONG)(PointerPte), Exchange, Comperand)

#define InterlockedExchangePte(PointerPte, Value) \
    InterlockedExchange((PLONG)(PointerPte), Value)
#endif

#if MI_TRACE_PFNS
extern ULONG MI_PFN_CURRENT_USAGE;
extern CHAR MI_PFN_CURRENT_PROCESS_NAME[16];
#define MI_SET_USAGE(x)     MI_PFN_CURRENT_USAGE = x
#define MI_SET_PROCESS2(x)  memcpy(MI_PFN_CURRENT_PROCESS_NAME, x, min(sizeof(x), sizeof(MI_PFN_CURRENT_PROCESS_NAME)))
FORCEINLINE
void
MI_SET_PROCESS(PEPROCESS Process)
{
    if (!Process)
        MI_SET_PROCESS2("Kernel");
    else if (Process == (PEPROCESS)1)
        MI_SET_PROCESS2("Hydra");
    else
        MI_SET_PROCESS2(Process->ImageFileName);
}

FORCEINLINE
void
MI_SET_PROCESS_USTR(PUNICODE_STRING ustr)
{
    PWSTR pos, strEnd;
    ULONG i;

    if (!ustr->Buffer || ustr->Length == 0)
    {
        MI_PFN_CURRENT_PROCESS_NAME[0] = 0;
        return;
    }

    pos = strEnd = &ustr->Buffer[ustr->Length / sizeof(WCHAR)];
    while ((*pos != L'\\') && (pos >  ustr->Buffer))
        pos--;

    if (*pos == L'\\')
        pos++;

    for (i = 0; i < sizeof(MI_PFN_CURRENT_PROCESS_NAME) && pos <= strEnd; i++, pos++)
        MI_PFN_CURRENT_PROCESS_NAME[i] = (CHAR)*pos;
}
#else
#define MI_SET_USAGE(x)
#define MI_SET_PROCESS(x)
#define MI_SET_PROCESS2(x)
#endif

typedef enum _MI_PFN_USAGES
{
    MI_USAGE_NOT_SET = 0,
    MI_USAGE_PAGED_POOL,
    MI_USAGE_NONPAGED_POOL,
    MI_USAGE_NONPAGED_POOL_EXPANSION,
    MI_USAGE_KERNEL_STACK,
    MI_USAGE_KERNEL_STACK_EXPANSION,
    MI_USAGE_SYSTEM_PTE,
    MI_USAGE_VAD,
    MI_USAGE_PEB_TEB,
    MI_USAGE_SECTION,
    MI_USAGE_PAGE_TABLE,
    MI_USAGE_PAGE_DIRECTORY,
    MI_USAGE_LEGACY_PAGE_DIRECTORY,
    MI_USAGE_DRIVER_PAGE,
    MI_USAGE_CONTINOUS_ALLOCATION,
    MI_USAGE_MDL,
    MI_USAGE_DEMAND_ZERO,
    MI_USAGE_ZERO_LOOP,
    MI_USAGE_CACHE,
    MI_USAGE_PFN_DATABASE,
    MI_USAGE_BOOT_DRIVER,
    MI_USAGE_INIT_MEMORY,
    MI_USAGE_PAGE_FILE,
    MI_USAGE_COW,
    MI_USAGE_WSLE,
    MI_USAGE_FREE_PAGE
} MI_PFN_USAGES;

//
// These two mappings are actually used by Windows itself, based on the ASSERTS
//
#define StartOfAllocation ReadInProgress
#define EndOfAllocation WriteInProgress

typedef struct _MMPFNENTRY
{
    USHORT Modified:1;
    USHORT ReadInProgress:1;                 // StartOfAllocation
    USHORT WriteInProgress:1;                // EndOfAllocation
    USHORT PrototypePte:1;
    USHORT PageColor:4;
    USHORT PageLocation:3;
    USHORT RemovalRequested:1;
    USHORT CacheAttribute:2;
    USHORT Rom:1;
    USHORT ParityError:1;
} MMPFNENTRY;

#ifdef _WIN64
#define MI_PTE_FRAME_BITS 57
#else
#define MI_PTE_FRAME_BITS 25
#endif

// Mm internal
typedef struct _MMPFN
{
    union
    {
        PFN_NUMBER Flink;
        ULONG WsIndex;
        PKEVENT Event;
        NTSTATUS ReadStatus;
        SINGLE_LIST_ENTRY NextStackPfn;
    } u1;
    PMMPTE PteAddress;
    union
    {
        PFN_NUMBER Blink;
        ULONG_PTR ShareCount;
    } u2;
    union
    {
        struct
        {
            USHORT ReferenceCount;
            MMPFNENTRY e1;
        };
        struct
        {
            USHORT ReferenceCount;
            USHORT ShortFlags;
        } e2;
    } u3;
#ifdef _WIN64
    ULONG UsedPageTableEntries;
#endif
    union
    {
        MMPTE OriginalPte;
        LONG AweReferenceCount;
    };
    union
    {
        ULONG_PTR EntireFrame;
        struct
        {
            ULONG_PTR PteFrame : MI_PTE_FRAME_BITS;
            ULONG_PTR InPageError:1;
            ULONG_PTR VerifierAllocation:1;
            ULONG_PTR AweAllocation:1;
            ULONG_PTR Priority:3;
        };
    } u4;
#if MI_TRACE_PFNS
    MI_PFN_USAGES PfnUsage;
    CHAR ProcessName[16];
#define MI_SET_PFN_PROCESS_NAME(pfn, x) memcpy(pfn->ProcessName, x, min(sizeof(x), sizeof(pfn->ProcessName)))
    PVOID CallSite;
#endif

    // HACK until WS lists are supported
    MMWSLE Wsle;
} MMPFN, *PMMPFN;

extern PMMPFN MmPfnDatabase;

typedef struct _MMPFNLIST
{
    PFN_NUMBER Total;
    MMLISTS ListName;
    PFN_NUMBER Flink;
    PFN_NUMBER Blink;
} MMPFNLIST, *PMMPFNLIST;

extern MMPFNLIST MmZeroedPageListHead;
extern MMPFNLIST MmFreePageListHead;
extern MMPFNLIST MmStandbyPageListHead;
extern MMPFNLIST MmModifiedPageListHead;
extern MMPFNLIST MmModifiedNoWritePageListHead;

// Mm internal
/* Entry describing free pool memory */
typedef struct _MMFREE_POOL_ENTRY
{
    LIST_ENTRY List;
    PFN_COUNT Size;
    ULONG Signature;
    struct _MMFREE_POOL_ENTRY *Owner;
} MMFREE_POOL_ENTRY, *PMMFREE_POOL_ENTRY;

/* Signature of a freed block */
#define MM_FREE_POOL_SIGNATURE 'ARM3'

/* Paged pool information */
typedef struct _MM_PAGED_POOL_INFO
{
    PRTL_BITMAP PagedPoolAllocationMap;
    PRTL_BITMAP EndOfPagedPoolBitmap;
    PMMPTE FirstPteForPagedPool;
    PMMPTE LastPteForPagedPool;
    PMMPDE NextPdeForPagedPoolExpansion;
    ULONG PagedPoolHint;
    SIZE_T PagedPoolCommit;
    SIZE_T AllocatedPagedPool;
} MM_PAGED_POOL_INFO, *PMM_PAGED_POOL_INFO;

/* Page file information */
typedef struct _MMPAGING_FILE
{
    PFN_NUMBER Size;
    PFN_NUMBER MaximumSize;
    PFN_NUMBER MinimumSize;
    PFN_NUMBER FreeSpace;
    PFN_NUMBER CurrentUsage;
    PFILE_OBJECT FileObject;
    UNICODE_STRING PageFileName;
    PRTL_BITMAP Bitmap;
    HANDLE FileHandle;
}
MMPAGING_FILE, *PMMPAGING_FILE;

extern PMMPAGING_FILE MmPagingFile[MAX_PAGING_FILES];

//
// Mm copy support for Kd
//
NTSTATUS
NTAPI
MmDbgCopyMemory(
    IN ULONG64 Address,
    IN PVOID Buffer,
    IN ULONG Size,
    IN ULONG Flags
);

//
// Determines if a given address is a session address
//
BOOLEAN
NTAPI
MmIsSessionAddress(
    IN PVOID Address
);

ULONG
NTAPI
MmGetSessionId(
    IN PEPROCESS Process
);

ULONG
NTAPI
MmGetSessionIdEx(
    IN PEPROCESS Process
);

/* npool.c *******************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
MiInitializeNonPagedPool(VOID);

PVOID
NTAPI
MiAllocatePoolPages(
    IN POOL_TYPE PoolType,
    IN SIZE_T SizeInBytes
);

POOL_TYPE
NTAPI
MmDeterminePoolType(
    IN PVOID VirtualAddress
);

ULONG
NTAPI
MiFreePoolPages(
    IN PVOID StartingAddress
);

/* pool.c *******************************************************************/

_Requires_lock_held_(PspQuotaLock)
BOOLEAN
NTAPI
MmRaisePoolQuota(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T CurrentMaxQuota,
    _Out_ PSIZE_T NewMaxQuota
);

_Requires_lock_held_(PspQuotaLock)
VOID
NTAPI
MmReturnPoolQuota(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T QuotaToReturn
);

/* mminit.c ******************************************************************/

CODE_SEG("INIT")
BOOLEAN
NTAPI
MmInitSystem(IN ULONG Phase,
             IN PLOADER_PARAMETER_BLOCK LoaderBlock);


/* pagefile.c ****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
MmInitPagingFile(VOID);

BOOLEAN
NTAPI
MmIsFileObjectAPagingFile(PFILE_OBJECT FileObject);

VOID
NTAPI
MmShowOutOfSpaceMessagePagingFile(VOID);

NTSTATUS
NTAPI
MiReadPageFile(
    _In_ PFN_NUMBER Page,
    _In_ ULONG PageFileIndex,
    _In_ ULONG_PTR PageFileOffset);

/* process.c ****************************************************************/

NTSTATUS
NTAPI
MmInitializeProcessAddressSpace(
    IN PEPROCESS Process,
    IN PEPROCESS Clone OPTIONAL,
    IN PVOID Section OPTIONAL,
    IN OUT PULONG Flags,
    IN POBJECT_NAME_INFORMATION *AuditName OPTIONAL
);

NTSTATUS
NTAPI
MmCreatePeb(
    IN PEPROCESS Process,
    IN PINITIAL_PEB InitialPeb,
    OUT PPEB *BasePeb
);

NTSTATUS
NTAPI
MmCreateTeb(
    IN PEPROCESS Process,
    IN PCLIENT_ID ClientId,
    IN PINITIAL_TEB InitialTeb,
    OUT PTEB* BaseTeb
);

VOID
NTAPI
MmDeleteTeb(
    struct _EPROCESS *Process,
    PTEB Teb
);

VOID
NTAPI
MmCleanProcessAddressSpace(IN PEPROCESS Process);

VOID
NTAPI
MmDeleteProcessAddressSpace(IN PEPROCESS Process);

ULONG
NTAPI
MmGetSessionLocaleId(VOID);

NTSTATUS
NTAPI
MmSetMemoryPriorityProcess(
    IN PEPROCESS Process,
    IN UCHAR MemoryPriority
);

/* special.c *****************************************************************/

VOID
NTAPI
MiInitializeSpecialPool(VOID);

BOOLEAN
NTAPI
MmUseSpecialPool(
    IN SIZE_T NumberOfBytes,
    IN ULONG Tag);

BOOLEAN
NTAPI
MmIsSpecialPoolAddress(
    IN PVOID P);

BOOLEAN
NTAPI
MmIsSpecialPoolAddressFree(
    IN PVOID P);

PVOID
NTAPI
MmAllocateSpecialPool(
    IN SIZE_T NumberOfBytes,
    IN ULONG Tag,
    IN POOL_TYPE PoolType,
    IN ULONG SpecialType);

VOID
NTAPI
MmFreeSpecialPool(
    IN PVOID P);

/* mm.c **********************************************************************/

NTSTATUS
NTAPI
MmAccessFault(
    IN ULONG FaultCode,
    IN PVOID Address,
    IN KPROCESSOR_MODE Mode,
    IN PVOID TrapInformation
);

/* process.c *****************************************************************/

PVOID
NTAPI
MmCreateKernelStack(BOOLEAN GuiStack, UCHAR Node);

VOID
NTAPI
MmDeleteKernelStack(PVOID Stack,
                    BOOLEAN GuiStack);

/* freelist.c **********************************************************/
_IRQL_raises_(DISPATCH_LEVEL)
_IRQL_requires_max_(DISPATCH_LEVEL)
_Requires_lock_not_held_(MmPfnLock)
_Acquires_lock_(MmPfnLock)
_IRQL_saves_
FORCEINLINE
KIRQL
MiAcquirePfnLock(VOID)
{
    return KeAcquireQueuedSpinLock(LockQueuePfnLock);
}

_Requires_lock_held_(MmPfnLock)
_Releases_lock_(MmPfnLock)
_IRQL_requires_(DISPATCH_LEVEL)
FORCEINLINE
VOID
MiReleasePfnLock(
    _In_ _IRQL_restores_ KIRQL OldIrql)
{
    KeReleaseQueuedSpinLock(LockQueuePfnLock, OldIrql);
}

_IRQL_requires_min_(DISPATCH_LEVEL)
_Requires_lock_not_held_(MmPfnLock)
_Acquires_lock_(MmPfnLock)
FORCEINLINE
VOID
MiAcquirePfnLockAtDpcLevel(VOID)
{
    PKSPIN_LOCK_QUEUE LockQueue;

    ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);
    LockQueue = &KeGetCurrentPrcb()->LockQueue[LockQueuePfnLock];
    KeAcquireQueuedSpinLockAtDpcLevel(LockQueue);
}

_Requires_lock_held_(MmPfnLock)
_Releases_lock_(MmPfnLock)
_IRQL_requires_min_(DISPATCH_LEVEL)
FORCEINLINE
VOID
MiReleasePfnLockFromDpcLevel(VOID)
{
    PKSPIN_LOCK_QUEUE LockQueue;

    LockQueue = &KeGetCurrentPrcb()->LockQueue[LockQueuePfnLock];
    KeReleaseQueuedSpinLockFromDpcLevel(LockQueue);
    ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);
}

#define MI_ASSERT_PFN_LOCK_HELD() NT_ASSERT((KeGetCurrentIrql() >= DISPATCH_LEVEL) && (MmPfnLock != 0))

FORCEINLINE
PMMPFN
MiGetPfnEntry(IN PFN_NUMBER Pfn)
{
    PMMPFN Page;
    extern RTL_BITMAP MiPfnBitMap;

    /* Make sure the PFN number is valid */
    if (Pfn > MmHighestPhysicalPage) return NULL;

    /* Make sure this page actually has a PFN entry */
    if ((MiPfnBitMap.Buffer) && !(RtlTestBit(&MiPfnBitMap, (ULONG)Pfn))) return NULL;

    /* Get the entry */
    Page = &MmPfnDatabase[Pfn];

    /* Return it */
    return Page;
};

FORCEINLINE
PFN_NUMBER
MiGetPfnEntryIndex(IN PMMPFN Pfn1)
{
    //
    // This will return the Page Frame Number (PFN) from the MMPFN
    //
    return Pfn1 - MmPfnDatabase;
}

VOID
NTAPI
MmDumpArmPfnDatabase(
   IN BOOLEAN StatusOnly
);

VOID
NTAPI
MmZeroPageThread(
    VOID
);

/* hypermap.c *****************************************************************/
PVOID
NTAPI
MiMapPageInHyperSpace(IN PEPROCESS Process,
                      IN PFN_NUMBER Page,
                      IN PKIRQL OldIrql);

VOID
NTAPI
MiUnmapPageInHyperSpace(IN PEPROCESS Process,
                        IN PVOID Address,
                        IN KIRQL OldIrql);

PVOID
NTAPI
MiMapPagesInZeroSpace(IN PMMPFN Pfn1,
                      IN PFN_NUMBER NumberOfPages);

VOID
NTAPI
MiUnmapPagesInZeroSpace(IN PVOID VirtualAddress,
                        IN PFN_NUMBER NumberOfPages);

/* i386/page.c *********************************************************/

BOOLEAN
NTAPI
MmCreateProcessAddressSpace(
    IN ULONG MinWs,
    IN PEPROCESS Dest,
    IN PULONG_PTR DirectoryTableBase
);

CODE_SEG("INIT")
NTSTATUS
NTAPI
MmInitializeHandBuiltProcess(
    IN PEPROCESS Process,
    IN PULONG_PTR DirectoryTableBase
);

CODE_SEG("INIT")
NTSTATUS
NTAPI
MmInitializeHandBuiltProcess2(
    IN PEPROCESS Process
);

NTSTATUS
NTAPI
MmSetExecuteOptions(IN ULONG ExecuteOptions);

NTSTATUS
NTAPI
MmGetExecuteOptions(IN PULONG ExecuteOptions);

/* arch/procsup.c ************************************************************/

BOOLEAN
MiArchCreateProcessAddressSpace(
    _In_ PEPROCESS Process,
    _In_ PULONG_PTR DirectoryTableBase);

/* section.c *****************************************************************/

VOID
NTAPI
MmGetImageInformation(
    OUT PSECTION_IMAGE_INFORMATION ImageInformation
);

PFILE_OBJECT
NTAPI
MmGetFileObjectForSection(
    IN PVOID Section
);
NTSTATUS
NTAPI
MmGetFileNameForAddress(
    IN PVOID Address,
    OUT PUNICODE_STRING ModuleName
);

NTSTATUS
NTAPI
MmGetFileNameForSection(
    IN PVOID Section,
    OUT POBJECT_NAME_INFORMATION *ModuleName
);

CODE_SEG("INIT")
NTSTATUS
NTAPI
MmInitSectionImplementation(VOID);

CODE_SEG("INIT")
NTSTATUS
NTAPI
MmCreatePhysicalMemorySection(VOID);

/* Exported from NT 6.2 onward. We keep it internal. */
NTSTATUS
NTAPI
MmMapViewInSystemSpaceEx(
    _In_ PVOID Section,
    _Outptr_result_bytebuffer_ (*ViewSize) PVOID *MappedBase,
    _Inout_ PSIZE_T ViewSize,
    _Inout_ PLARGE_INTEGER SectionOffset,
    _In_ ULONG_PTR Flags);

BOOLEAN
NTAPI
MmIsDataSectionResident(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer,
    _In_ LONGLONG Offset,
    _In_ ULONG Length);

NTSTATUS
NTAPI
MmMakeSegmentDirty(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer,
    _In_ LONGLONG Offset,
    _In_ ULONG Length);

VOID
NTAPI
MmCaptureDirtyPages(
    _In_ PVOID BaseAddress,
    _In_ SIZE_T Length);

NTSTATUS
NTAPI
MmFlushSegment(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer,
    _In_opt_ PLARGE_INTEGER Offset,
    _In_ ULONG Length,
    _Out_opt_ PIO_STATUS_BLOCK Iosb);

NTSTATUS
NTAPI
MmZeroDataSection(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer,
    _In_ LONGLONG Offset,
    _In_ ULONG Length);

NTSTATUS
NTAPI
MmMakeDataSectionResident(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer,
    _In_ LONGLONG Offset,
    _In_ ULONG Length);

BOOLEAN
NTAPI
MmPurgeSegment(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer,
    _In_opt_ PLARGE_INTEGER Offset,
    _In_ ULONG Length);

NTSTATUS
NTAPI
MmExtendSection(
    _In_ PVOID Section,
    _Inout_ PLARGE_INTEGER NewSize);

/* sysldr.c ******************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
MiReloadBootLoadedDrivers(
    IN PLOADER_PARAMETER_BLOCK LoaderBlock
);

CODE_SEG("INIT")
BOOLEAN
NTAPI
MiInitializeLoadedModuleList(
    IN PLOADER_PARAMETER_BLOCK LoaderBlock
);

BOOLEAN
NTAPI
MmChangeKernelResourceSectionProtection(IN ULONG_PTR ProtectionMask);

VOID
NTAPI
MmMakeKernelResourceSectionWritable(VOID);

NTSTATUS
NTAPI
MmLoadSystemImage(
    IN PUNICODE_STRING FileName,
    IN PUNICODE_STRING NamePrefix OPTIONAL,
    IN PUNICODE_STRING LoadedName OPTIONAL,
    IN ULONG Flags,
    OUT PVOID *ModuleObject,
    OUT PVOID *ImageBaseAddress
);

NTSTATUS
NTAPI
MmUnloadSystemImage(
    IN PVOID ImageHandle
);

#ifdef CONFIG_SMP
BOOLEAN
NTAPI
MmVerifyImageIsOkForMpUse(
    _In_ PVOID BaseAddress);
#endif // CONFIG_SMP

NTSTATUS
NTAPI
MmCheckSystemImage(
    _In_ HANDLE ImageHandle);

NTSTATUS
NTAPI
MmCallDllInitialize(
    _In_ PLDR_DATA_TABLE_ENTRY LdrEntry,
    _In_ PLIST_ENTRY ModuleListHead);

VOID
NTAPI
MmFreeDriverInitialization(
    IN PLDR_DATA_TABLE_ENTRY LdrEntry);

/* ReactOS-only, used by psmgr.c PspLookupSystemDllEntryPoint() as well */
NTSTATUS
NTAPI
RtlpFindExportedRoutineByName(
    _In_ PVOID ImageBase,
    _In_ PCSTR ExportName,
    _Out_ PVOID* Function,
    _Out_opt_ PBOOLEAN IsForwarder,
    _In_ NTSTATUS NotFoundStatus);

/* Exported from NT 10.0 onward. We keep it internal. */
PVOID
NTAPI
RtlFindExportedRoutineByName(
    _In_ PVOID ImageBase,
    _In_ PCSTR ExportName);

/* procsup.c *****************************************************************/

NTSTATUS
NTAPI
MmGrowKernelStack(
    IN PVOID StackPointer
);


FORCEINLINE
VOID
MmLockAddressSpace(PMMSUPPORT AddressSpace)
{
    ASSERT(!PsGetCurrentThread()->OwnsProcessWorkingSetExclusive);
    ASSERT(!PsGetCurrentThread()->OwnsProcessWorkingSetShared);
    ASSERT(!PsGetCurrentThread()->OwnsSystemWorkingSetExclusive);
    ASSERT(!PsGetCurrentThread()->OwnsSystemWorkingSetShared);
    ASSERT(!PsGetCurrentThread()->OwnsSessionWorkingSetExclusive);
    ASSERT(!PsGetCurrentThread()->OwnsSessionWorkingSetShared);
    KeAcquireGuardedMutex(&CONTAINING_RECORD(AddressSpace, EPROCESS, Vm)->AddressCreationLock);
}

FORCEINLINE
VOID
MmUnlockAddressSpace(PMMSUPPORT AddressSpace)
{
    KeReleaseGuardedMutex(&CONTAINING_RECORD(AddressSpace, EPROCESS, Vm)->AddressCreationLock);
}

FORCEINLINE
PEPROCESS
MmGetAddressSpaceOwner(IN PMMSUPPORT AddressSpace)
{
    if (AddressSpace == MmKernelAddressSpace) return NULL;
    return CONTAINING_RECORD(AddressSpace, EPROCESS, Vm);
}

FORCEINLINE
PMMSUPPORT
MmGetCurrentAddressSpace(VOID)
{
    return &((PEPROCESS)KeGetCurrentThread()->ApcState.Process)->Vm;
}

FORCEINLINE
PMMSUPPORT
MmGetKernelAddressSpace(VOID)
{
    return MmKernelAddressSpace;
}


/* expool.c ******************************************************************/

VOID
NTAPI
ExpCheckPoolAllocation(
    PVOID P,
    POOL_TYPE PoolType,
    ULONG Tag);

VOID
NTAPI
ExReturnPoolQuota(
    IN PVOID P);


/* mmsup.c *****************************************************************/

NTSTATUS
NTAPI
MmAdjustWorkingSetSize(
    IN SIZE_T WorkingSetMinimumInBytes,
    IN SIZE_T WorkingSetMaximumInBytes,
    IN ULONG SystemCache,
    IN BOOLEAN IncreaseOkay);


/* session.c *****************************************************************/

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
NTAPI
MmAttachSession(
    _Inout_ PVOID SessionEntry,
    _Out_ PKAPC_STATE ApcState);

_IRQL_requires_max_(APC_LEVEL)
VOID
NTAPI
MmDetachSession(
    _Inout_ PVOID SessionEntry,
    _Out_ PKAPC_STATE ApcState);

VOID
NTAPI
MmQuitNextSession(
    _Inout_ PVOID SessionEntry);

PVOID
NTAPI
MmGetSessionById(
    _In_ ULONG SessionId);

_IRQL_requires_max_(APC_LEVEL)
VOID
NTAPI
MmSetSessionLocaleId(
    _In_ LCID LocaleId);

/* shutdown.c *****************************************************************/

VOID
MmShutdownSystem(IN ULONG Phase);

/* virtual.c *****************************************************************/

NTSTATUS
NTAPI
MmCopyVirtualMemory(IN PEPROCESS SourceProcess,
                    IN PVOID SourceAddress,
                    IN PEPROCESS TargetProcess,
                    OUT PVOID TargetAddress,
                    IN SIZE_T BufferSize,
                    IN KPROCESSOR_MODE PreviousMode,
                    OUT PSIZE_T ReturnSize);

/* wslist.cpp ****************************************************************/
extern KEVENT MmWorkingSetManagerEvent;

_Requires_exclusive_lock_held_(WorkingSet->WorkingSetMutex)
VOID
NTAPI
MiInitializeWorkingSetList(_Inout_ PMMSUPPORT WorkingSet);

_Requires_exclusive_lock_held_(Vm->WorkingSetMutex)
VOID
NTAPI
MiInsertInWorkingSetList(
    _Inout_ PMMSUPPORT Vm,
    _In_ PVOID Address,
    _In_ ULONG Protection);

_Requires_exclusive_lock_held_(Vm->WorkingSetMutex)
VOID
NTAPI
MiRemoveFromWorkingSetList(
    _Inout_ PMMSUPPORT Vm,
    _In_ PVOID Address);

VOID
NTAPI
MiAddValidPageToWorkingSet(
    _In_ PVOID Address,
    _In_ ULONG Protection);

VOID
NTAPI
MiRemoveSharedPageFromWorkingSet(
    _In_ PVOID Address,
    _In_ PMMPFN Pfn);

_Requires_exclusive_lock_held_(WorkingSet->WorkingSetMutex)
VOID
NTAPI
MiShrinkWorkingSetList(
    _Inout_ PMMSUPPORT WorkingSet);

VOID
NTAPI
MmWorkingSetManager(VOID);

#ifdef __cplusplus
} // extern "C"

namespace ntoskrnl
{
using MiPfnLockGuard = const KiQueuedSpinLockGuard<LockQueuePfnLock>;
} // namespace ntoskrnl

#endif
