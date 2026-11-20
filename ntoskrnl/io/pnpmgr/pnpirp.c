/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shortcuts for sending different IRP_MJ_PNP requests
 * COPYRIGHT:   Copyright 2010 Sir Richard <sir_richard@svn.reactos.org>
 *              Copyright 2020 Victor Perevertkin <victor.perevertkin@reactos.org>
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* The interrupt mask the debug output reports: EFLAGS on x86, DAIF on ARM64 */
#if defined(_M_ARM64)
#define IopReadInterruptFlags() ((ULONG_PTR)_ReadStatusReg(ARM64_SYSREG(3, 3, 4, 2, 1)))
#else
#define IopReadInterruptFlags() __readeflags()
#endif

/* The user IOSB is not written for an error that was not pended, so the result is taken here */
static
NTSTATUS
NTAPI
IopSynchronousCallCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    *(PIO_STATUS_BLOCK)Context = Irp->IoStatus;
    return STATUS_CONTINUE_COMPLETION;
}

/*
 * PnP watchdog, for bringing up binary drivers without a kernel debugger: every
 * synchronous PnP request is tracked, and one still running after a while is
 * reported with its device and the code addresses found on the waiting thread's
 * stack, as module+offset. One stuck request blocks all device enumeration after it.
 */
#define PNP_WATCH_SLOTS         16
#define PNP_WATCH_REPORT_AFTER  (15LL * 10000000)   /* 15 s */
#define PNP_WATCH_PERIOD        (-5LL * 10000000)   /* 5 s */

typedef struct _PNP_WATCH_SLOT
{
    PETHREAD volatile Thread;
    PDEVICE_OBJECT DeviceObject;
    UCHAR MajorFunction;
    UCHAR MinorFunction;
    BOOLEAN Reported;
    LARGE_INTEGER StartTime;
} PNP_WATCH_SLOT, *PPNP_WATCH_SLOT;

static PNP_WATCH_SLOT PnpWatchSlots[PNP_WATCH_SLOTS];
static LONG PnpWatchStarted;

/* The loaded module a code address on a stack falls into. PsLoadedModuleResource must be held */
static PLDR_DATA_TABLE_ENTRY
PnpWatchFindModule(
    _In_ ULONG_PTR Value)
{
    PLIST_ENTRY Entry;
    PLDR_DATA_TABLE_ENTRY LdrEntry;

    for (Entry = PsLoadedModuleList.Flink; Entry != &PsLoadedModuleList; Entry = Entry->Flink)
    {
        LdrEntry = CONTAINING_RECORD(Entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
        if (Value > (ULONG_PTR)LdrEntry->DllBase + PAGE_SIZE &&
            Value < (ULONG_PTR)LdrEntry->DllBase + LdrEntry->SizeOfImage)
        {
            return LdrEntry;
        }
    }
    return NULL;
}

/* Whether a stack value points into an executable section, so data pointers are not taken for frames */
static BOOLEAN
PnpWatchIsCode(
    _In_ PLDR_DATA_TABLE_ENTRY LdrEntry,
    _In_ ULONG_PTR Value)
{
    PIMAGE_NT_HEADERS NtHeaders;
    PIMAGE_SECTION_HEADER Section;
    ULONG_PTR Rva = Value - (ULONG_PTR)LdrEntry->DllBase;
    ULONG i;

    /* A session image such as cdd.dll is not mapped in the System process the watch runs in */
    if (!MmIsAddressValid(LdrEntry->DllBase))
        return TRUE;

    NtHeaders = RtlImageNtHeader(LdrEntry->DllBase);
    if (!NtHeaders)
        return TRUE;

    Section = IMAGE_FIRST_SECTION(NtHeaders);
    for (i = 0; i < NtHeaders->FileHeader.NumberOfSections; i++, Section++)
    {
        if (Rva >= Section->VirtualAddress &&
            Rva < Section->VirtualAddress + max(Section->Misc.VirtualSize, Section->SizeOfRawData))
        {
            return (Section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        }
    }
    return FALSE;
}

/* Only the watchdog thread scans stacks, one at a time */
static ULONG_PTR PnpWatchStackCopy[KERNEL_LARGE_STACK_SIZE / sizeof(ULONG_PTR)];

/*
 * Copy a thread's stack from its saved stack pointer to its base, page by page while the pages
 * are there. The thread may exit and the reaper unmap its stack at any time, so the scan works
 * on the copy, and the window is one page copy, not a module list walk per slot.
 */
static ULONG
PnpWatchCopyStack(
    _In_ PULONG_PTR Sp,
    _In_ PULONG_PTR Base)
{
    PUCHAR Source = (PUCHAR)Sp, End = (PUCHAR)Base;
    ULONG_PTR Chunk, Copied = 0;

    while (Source < End)
    {
        Chunk = min((ULONG_PTR)(End - Source), PAGE_SIZE - ((ULONG_PTR)Source & (PAGE_SIZE - 1)));
        if (!MmIsAddressValid(Source))
            break;
        RtlCopyMemory((PUCHAR)PnpWatchStackCopy + Copied, Source, Chunk);
        Copied += Chunk;
        Source += Chunk;
    }

    return (ULONG)(Copied / sizeof(ULONG_PTR));
}

/*
 * Print the code addresses found between a blocked thread's saved stack pointer and its stack
 * base. With Filter, print nothing unless one of them lies in one of the named modules, among
 * the first FilterDepth code addresses from the top when FilterDepth is not 0. Returns whether
 * something was printed.
 */
static BOOLEAN
PnpWatchScanStack(
    _In_ PKTHREAD Thread,
    _In_opt_ const PCWSTR *Filter,
    _In_ ULONG FilterCount,
    _In_ ULONG FilterDepth)
{
    PEX_RUNDOWN_REF Rundown = &((PETHREAD)Thread)->RundownProtect;
    PULONG_PTR Sp, Base, p, End;
    PLDR_DATA_TABLE_ENTRY LdrEntry;
    UNICODE_STRING Name;
    BOOLEAN Match = (Filter == NULL);
    ULONG Hits = 0, Depth = 0, i;

    /* PspExitThread waits for this before KeTerminateThread, so the reaper cannot free the stack */
    if (!ExAcquireRundownProtection(Rundown))
        return FALSE;

    Sp = Thread->KernelStack;
    Base = Thread->InitialStack;
    if (Thread->State == Terminated ||
        !Sp || !Base || Sp >= Base || (ULONG_PTR)(Base - Sp) > KERNEL_LARGE_STACK_SIZE / sizeof(ULONG_PTR))
    {
        ExReleaseRundownProtection(Rundown);
        return FALSE;
    }

    End = PnpWatchStackCopy + PnpWatchCopyStack(Sp, Base);
    ExReleaseRundownProtection(Rundown);

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&PsLoadedModuleResource, TRUE);
    for (p = PnpWatchStackCopy; p < End && !Match; p++)
    {
        LdrEntry = PnpWatchFindModule(*p);
        if (!LdrEntry)
            continue;
        if (FilterDepth && ++Depth > FilterDepth)
            break;
        for (i = 0; i < FilterCount; i++)
        {
            RtlInitUnicodeString(&Name, Filter[i]);
            if (RtlEqualUnicodeString(&Name, &LdrEntry->BaseDllName, TRUE))
                Match = TRUE;
        }
    }
    if (Match)
    {
        if (Filter)
        {
            DbgPrint("PNPWATCH: thread %p of %s, state %u, wait reason %u\n",
                     Thread, PsGetThreadProcess((PETHREAD)Thread)->ImageFileName,
                     Thread->State, Thread->WaitReason);
        }
        /* Code addresses only, three to a line, so the innermost ones stay on a phone screen */
        for (p = PnpWatchStackCopy; p < End && Hits < 24; p++)
        {
            LdrEntry = PnpWatchFindModule(*p);
            if (!LdrEntry || !PnpWatchIsCode(LdrEntry, *p))
                continue;

            DbgPrint("%s%wZ+0x%Ix%s",
                     (Hits % 3) ? " " : "PNPWATCH: ",
                     &LdrEntry->BaseDllName,
                     *p - (ULONG_PTR)LdrEntry->DllBase,
                     (Hits % 3 == 2) ? "\n" : "");
            Hits++;
        }
        if (Hits % 3)
            DbgPrint("\n");
    }
    ExReleaseResourceLite(&PsLoadedModuleResource);
    KeLeaveCriticalRegion();
    return Match;
}

static const PCWSTR PnpWatchDisplayModules[] =
{
    L"qcdxkm850.sys", L"nvlddmkm.sys", L"dxgkrnl.sys", L"dxgmms2.sys", L"cdd.dll"
};

/*
 * qcdxkm850.sys of 2018-10-01 logs into three rings of 40 byte records in its image, an event
 * code at +36 and five arguments from +16. Nothing else reports why its start failed.
 */
#define PNP_WATCH_QCDX_IMAGE_SIZE   0x415000
#define PNP_WATCH_QCDX_RECORD       40
#define PNP_WATCH_QCDX_SHOWN        48

typedef struct _PNP_WATCH_QCDX_RING
{
    ULONG RecordsRva;
    ULONG CapacityRva;
    ULONG WrittenRva;
    PCSTR Name;
} PNP_WATCH_QCDX_RING;

static const PNP_WATCH_QCDX_RING PnpWatchQcdxRings[] =
{
    { 0x190B70, 0x190B88, 0x190BC4, "error" },
    { 0x190BF0, 0x190BB0, 0x190BE0, "warning" },
    { 0x190BD0, 0x190BE4, 0x190AE8, "info" }
};

/* SDM845 Adreno 630 clock controller and GMU, read when qcdxkm850.sys gives up */
#define PNP_WATCH_GPUCC_BASE        0x05090000
#define PNP_WATCH_GPUCC_SIZE        0x9000
#define PNP_WATCH_GMU_BASE          0x05060000
#define PNP_WATCH_GMU_SIZE          0x3F000
#define PNP_WATCH_GDSC_ON           0x80000000
#define PNP_WATCH_BRANCH_OFF        0x80000000

typedef struct _PNP_WATCH_GPU_REG
{
    ULONG Offset;
    PCSTR Name;
} PNP_WATCH_GPU_REG;

static const PNP_WATCH_GPU_REG PnpWatchGpuccRegs[] =
{
    { 0x106C, "cx gdscr" },
    { 0x100C, "gx gdscr" },
    { 0x1098, "gmu cbcr" },
    { 0x109C, "cxo cbcr" }
};

/* GMU offsets are relative to 0x05060000 */
static const PNP_WATCH_GPU_REG PnpWatchGmuRegs[] =
{
    { 0x1E070, "fw init result" },
    { 0x1E068, "fw busy" },
    { 0x12FE0, "boot slumber option" },
    { 0x12FE4, "gx vote" },
    { 0x12FE8, "mx vote" },
    { 0x1E648, "gmu2host info" },
    { 0x1E658, "host2gmu raw" },
    { 0x1E340, "sptprac status" },
    { 0x1E3B0, "rpmh power state" },
    { 0x2EC14, "ao host irq status" },
    { 0x2EC20, "rscc control ack" },
    { 0x2EC30, "cx busy status" },
    { 0x1E000, "cm3 sysreset" },
    { 0x1E004, "cm3 boot config" },
    { 0x1E0B4, "cm3 cfg" },
    { 0x1E220, "always on count" },
    { 0x1E300, "ifpc ctrl" },
    { 0x1E3A0, "rpmh ctrl" },
    { 0x1E600, "hfi ctrl status" },
    { 0x1E604, "hfi version" },
    { 0x1E608, "hfi sfr addr" },
    { 0x1E714, "general 0" },
    { 0x1E718, "general 1" },
    { 0x1E72C, "general 6" },
    { 0x1E730, "general 7" },
    { 0x1E7C0, "lm handshake" },
    { 0x2EC0C, "ao irq enable" },
    { 0x2EC18, "ao host irq mask" },
    { 0x2EC1C, "rscc control req" },
    { 0x2EC40, "ahb fence ctrl" },
    { 0x2EC44, "ahb fence range 0" },
    { 0x2EC4C, "ahb fence status" },
    { 0x1E610, "hfi qtbl info" },
    { 0x1E614, "hfi qtbl addr" },
    { 0x1E400, "log buffer msg" },
    { 0x1E404, "log buffer resp" },
    { 0x1E64C, "gmu2host irq mask" },
    /* RSC block inside the GMU, its sequencer is loaded by the host on a630 */
    { 0x2D010, "rscc status0" },
    { 0x2D020, "rscc pdc seq start" },
    { 0x2D024, "rscc pdc match lo" },
    { 0x2D028, "rscc pdc match hi" },
    { 0x2D02C, "rscc pdc slave id" },
    { 0x2D404, "rscc seq busy" },
    { 0x2D600, "rscc seq mem 0" },
    { 0x2D610, "rscc seq mem 4" },
    { 0x2DFB8, "rscc tcs1 status" },
    { 0x2E4F8, "rscc tcs3 status" }
};

/* GPU power domain controller in AOSS, programmed by the host on a630 */
#define PNP_WATCH_GPU_PDC_BASE      0x0B280000
#define PNP_WATCH_GPU_PDC_SIZE      0x6000
#define PNP_WATCH_GPU_PDC_SEQ_BASE  0x0B480000
#define PNP_WATCH_GPU_PDC_SEQ_SIZE  0x1000

static const PNP_WATCH_GPU_REG PnpWatchGpuPdcRegs[] =
{
    { 0x4500, "pdc enable" },
    { 0x4520, "pdc seq start" },
    { 0x55C8, "pdc tcs1 control" },
    { 0x55CC, "pdc tcs1 enable bank" },
    { 0x55D4, "pdc tcs1 cmd0 msgid" },
    { 0x55D8, "pdc tcs1 cmd0 addr" },
    { 0x55DC, "pdc tcs1 cmd0 data" },
    { 0x55F8, "pdc tcs1 cmd2 addr" },
    { 0x575C, "pdc tcs3 enable bank" },
    { 0x5768, "pdc tcs3 cmd0 addr" },
    { 0x576C, "pdc tcs3 cmd0 data" }
};

static const PNP_WATCH_GPU_REG PnpWatchGpuPdcSeqRegs[] =
{
    { 0x0, "pdc seq mem 0" },
    { 0x4, "pdc seq mem 1" },
    { 0x10, "pdc seq mem 4" }
};

/* Adreno SMMU (MMU1), the GMU reaches its HFI queues and log through stream 5 */
#define PNP_WATCH_GPU_SMMU_BASE     0x05040000
#define PNP_WATCH_GPU_SMMU_SIZE     0x10000
#define PNP_WATCH_GPU_SMMU_SLOTS    8
#define PNP_WATCH_GPU_SMMU_GR1      0x1000
#define PNP_WATCH_GPU_SMMU_CB       0x8000
#define PNP_WATCH_GPU_SMMU_CB_SIZE  0x1000

static VOID
PnpWatchDumpGpuSmmu(VOID)
{
    PHYSICAL_ADDRESS Physical;
    PUCHAR Va, Cb;
    ULONG i;

    Physical.QuadPart = PNP_WATCH_GPU_SMMU_BASE;
    Va = MmMapIoSpace(Physical, PNP_WATCH_GPU_SMMU_SIZE, MmNonCached);
    if (!Va)
    {
        DbgPrint("PNPWATCH: cannot map the GPU SMMU\n");
        return;
    }

    DbgPrint("PNPWATCH: gpu smmu cr0 %08lx id0 %08lx id1 %08lx gfsr %08lx gfsynr0 %08lx\n",
             READ_REGISTER_ULONG((PULONG)(Va + 0x0)),
             READ_REGISTER_ULONG((PULONG)(Va + 0x20)),
             READ_REGISTER_ULONG((PULONG)(Va + 0x24)),
             READ_REGISTER_ULONG((PULONG)(Va + 0x48)),
             READ_REGISTER_ULONG((PULONG)(Va + 0x50)));

    for (i = 0; i < PNP_WATCH_GPU_SMMU_SLOTS; i++)
    {
        DbgPrint("PNPWATCH: gpu smmu smr%lu %08lx s2cr %08lx cbar %08lx\n",
                 i,
                 READ_REGISTER_ULONG((PULONG)(Va + 0x800 + i * sizeof(ULONG))),
                 READ_REGISTER_ULONG((PULONG)(Va + 0xC00 + i * sizeof(ULONG))),
                 READ_REGISTER_ULONG((PULONG)(Va + PNP_WATCH_GPU_SMMU_GR1 + i * sizeof(ULONG))));
    }

    for (i = 0; i < PNP_WATCH_GPU_SMMU_SLOTS; i++)
    {
        Cb = Va + PNP_WATCH_GPU_SMMU_CB + i * PNP_WATCH_GPU_SMMU_CB_SIZE;
        DbgPrint("PNPWATCH: gpu smmu cb%lu sctlr %08lx fsr %08lx far %08lx%08lx fsynr0 %08lx\n",
                 i,
                 READ_REGISTER_ULONG((PULONG)(Cb + 0x0)),
                 READ_REGISTER_ULONG((PULONG)(Cb + 0x58)),
                 READ_REGISTER_ULONG((PULONG)(Cb + 0x64)),
                 READ_REGISTER_ULONG((PULONG)(Cb + 0x60)),
                 READ_REGISTER_ULONG((PULONG)(Cb + 0x68)));
    }

    MmUnmapIoSpace(Va, PNP_WATCH_GPU_SMMU_SIZE);
}

static ULONG
PnpWatchDumpGpuBlock(
    _In_ ULONG Base,
    _In_ ULONG Size,
    _In_reads_(Count) PNP_WATCH_GPU_REG const *Regs,
    _In_ ULONG Count,
    _Out_writes_(Count) PULONG Values)
{
    PHYSICAL_ADDRESS Physical;
    PUCHAR Va;
    ULONG i;

    Physical.QuadPart = Base;
    Va = MmMapIoSpace(Physical, Size, MmNonCached);
    if (!Va)
    {
        DbgPrint("PNPWATCH: cannot map GPU registers at %lx\n", Base);
        return 0;
    }

    for (i = 0; i < Count; i++)
    {
        Values[i] = READ_REGISTER_ULONG((PULONG)(Va + Regs[i].Offset));
        DbgPrint("PNPWATCH: gpu %s %08lx\n", Regs[i].Name, Values[i]);
    }

    MmUnmapIoSpace(Va, Size);
    return Count;
}

static VOID
PnpWatchDumpGpuState(VOID)
{
    ULONG Gpucc[RTL_NUMBER_OF(PnpWatchGpuccRegs)];
    ULONG Gmu[RTL_NUMBER_OF(PnpWatchGmuRegs)];
    ULONG Pdc[RTL_NUMBER_OF(PnpWatchGpuPdcRegs)];
    ULONG PdcSeq[RTL_NUMBER_OF(PnpWatchGpuPdcSeqRegs)];

    if (!PnpWatchDumpGpuBlock(PNP_WATCH_GPUCC_BASE, PNP_WATCH_GPUCC_SIZE,
                              PnpWatchGpuccRegs, RTL_NUMBER_OF(PnpWatchGpuccRegs), Gpucc))
    {
        return;
    }

    /* The PDC sits in the always on domain, it answers whatever state the GPU is in */
    PnpWatchDumpGpuBlock(PNP_WATCH_GPU_PDC_BASE, PNP_WATCH_GPU_PDC_SIZE,
                         PnpWatchGpuPdcRegs, RTL_NUMBER_OF(PnpWatchGpuPdcRegs), Pdc);
    PnpWatchDumpGpuBlock(PNP_WATCH_GPU_PDC_SEQ_BASE, PNP_WATCH_GPU_PDC_SEQ_SIZE,
                         PnpWatchGpuPdcSeqRegs, RTL_NUMBER_OF(PnpWatchGpuPdcSeqRegs), PdcSeq);

    /* A GMU without CX power or its clock stalls the bus instead of answering */
    if (!(Gpucc[0] & PNP_WATCH_GDSC_ON) || (Gpucc[2] & PNP_WATCH_BRANCH_OFF))
    {
        DbgPrint("PNPWATCH: GMU is not powered, its registers are not read\n");
        return;
    }

    PnpWatchDumpGpuBlock(PNP_WATCH_GMU_BASE, PNP_WATCH_GMU_SIZE,
                         PnpWatchGmuRegs, RTL_NUMBER_OF(PnpWatchGmuRegs), Gmu);

    /* Last, a powered down SMMU stalls the bus instead of answering */
    PnpWatchDumpGpuSmmu();
}

VOID
PnpWatchDumpQcdxLog(VOID)
{
    UNICODE_STRING QcdxName = RTL_CONSTANT_STRING(L"qcdxkm850.sys");
    PLDR_DATA_TABLE_ENTRY LdrEntry, Qcdx = NULL;
    PNP_WATCH_QCDX_RING const *Ring;
    PULONG Record;
    PUCHAR Base, Records;
    ULONG Capacity, Written, i, r;
    PLIST_ENTRY Entry;

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&PsLoadedModuleResource, TRUE);
    for (Entry = PsLoadedModuleList.Flink; Entry != &PsLoadedModuleList; Entry = Entry->Flink)
    {
        LdrEntry = CONTAINING_RECORD(Entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
        if (RtlEqualUnicodeString(&LdrEntry->BaseDllName, &QcdxName, TRUE))
        {
            Qcdx = LdrEntry;
            break;
        }
    }

    if (!Qcdx)
        DbgPrint("PNPWATCH: qcdxkm850.sys is not loaded\n");
    else if (Qcdx->SizeOfImage != PNP_WATCH_QCDX_IMAGE_SIZE)
        DbgPrint("PNPWATCH: qcdxkm850.sys is another build, its log is not read\n");
    else
    {
        Base = Qcdx->DllBase;
        for (r = 0; r < RTL_NUMBER_OF(PnpWatchQcdxRings); r++)
        {
            Ring = &PnpWatchQcdxRings[r];
            Records = *(PUCHAR *)(Base + Ring->RecordsRva);
            Capacity = *(PULONG)(Base + Ring->CapacityRva);
            Written = *(PULONG)(Base + Ring->WrittenRva);
            DbgPrint("PNPWATCH: qcdx %s log, %lu records\n", Ring->Name, Written);
            if (!Records || !Capacity)
                continue;

            for (i = Written - min(Written, min(Capacity, PNP_WATCH_QCDX_SHOWN)); i < Written; i++)
            {
                Record = (PULONG)(Records + (i % Capacity) * PNP_WATCH_QCDX_RECORD);
                if (!MmIsAddressValid(Record))
                    break;

                DbgPrint("PNPWATCH: qcdx %s %lu: code %lu, %lx %lx %lx %lx %lx\n",
                         Ring->Name, Record[2], Record[9],
                         Record[4], Record[5], Record[6], Record[7], Record[8]);
            }
        }
    }

    ExReleaseResourceLite(&PsLoadedModuleResource);
    KeLeaveCriticalRegion();

    if (Qcdx)
        PnpWatchDumpGpuState();
}

/* Display drivers being brought up: show every thread that is somewhere inside them */
static VOID
PnpWatchDumpDisplayThreads(VOID)
{
    PEPROCESS Process;
    PETHREAD Thread;

    PnpWatchDumpQcdxLog();
    DbgPrint("PNPWATCH: threads in the display drivers at %I64d s\n", KeQueryInterruptTime() / 10000000);
    for (Process = PsGetNextProcess(NULL); Process; Process = PsGetNextProcess(Process))
    {
        for (Thread = PsGetNextProcessThread(Process, NULL); Thread; Thread = PsGetNextProcessThread(Process, Thread))
        {
            if (Thread != PsGetCurrentThread())
                PnpWatchScanStack(&Thread->Tcb, PnpWatchDisplayModules, RTL_NUMBER_OF(PnpWatchDisplayModules), 0);
        }
    }
}

/*
 * A GUI thread that waits inside the display drivers this long is reported once per boot, with
 * its stack. A mode change, a present or a full screen switch that never returns holds the USER
 * lock, so the desktop freezes or the screen stays dark and nothing else reaches the log. Only
 * the innermost code addresses count, so a thread that merely went through the display drivers
 * earlier is not taken for one stuck in them. Once per thread, not once per wait: a worker that
 * idles in cdd.dll (boot 19 had a csrss.exe thread waiting there at 45, 75 and 150 s) would
 * otherwise use up the reports long before the mode change they are meant for.
 */
#define PNP_WATCH_GUI_STALL_SECONDS 10
#define PNP_WATCH_GUI_STALL_DEPTH   8
#define PNP_WATCH_GUI_STALL_REPORTS 32

static PKTHREAD PnpWatchGuiStalls[PNP_WATCH_GUI_STALL_REPORTS];
static ULONG PnpWatchGuiStallReports;

static BOOLEAN
PnpWatchIsGuiStallReported(
    _In_ PKTHREAD Thread)
{
    ULONG i;

    for (i = 0; i < PnpWatchGuiStallReports; i++)
    {
        if (PnpWatchGuiStalls[i] == Thread)
            return TRUE;
    }
    return FALSE;
}

static VOID
PnpWatchDumpStalledGuiThreads(VOID)
{
    ULONG Limit = (ULONG)(PNP_WATCH_GUI_STALL_SECONDS * 10000000LL / KeMaximumIncrement);
    PEPROCESS Process;
    PETHREAD Thread;
    ULONG Waited;
    BOOLEAN NewStall = FALSE;
    static ULONG FullDumps;
    static LONGLONG LastFullDump;

    if (PnpWatchGuiStallReports >= PNP_WATCH_GUI_STALL_REPORTS)
        return;

    for (Process = PsGetNextProcess(NULL); Process; Process = PsGetNextProcess(Process))
    {
        for (Thread = PsGetNextProcessThread(Process, NULL); Thread; Thread = PsGetNextProcessThread(Process, Thread))
        {
            /* Push locks and guarded mutexes wait in KeWaitForGate, which leaves the thread GateWait */
            if (Thread == PsGetCurrentThread() || !Thread->Tcb.Win32Thread ||
                (Thread->Tcb.State != Waiting && Thread->Tcb.State != GateWait))
            {
                continue;
            }

            Waited = KeTickCount.LowPart - Thread->Tcb.WaitTime;
            if (Waited < Limit || PnpWatchGuiStallReports >= PNP_WATCH_GUI_STALL_REPORTS ||
                PnpWatchIsGuiStallReported(&Thread->Tcb))
            {
                continue;
            }

            if (PnpWatchScanStack(&Thread->Tcb, PnpWatchDisplayModules, RTL_NUMBER_OF(PnpWatchDisplayModules), PNP_WATCH_GUI_STALL_DEPTH))
            {
                DbgPrint("PNPWATCH: GUI thread %p has waited %I64u s inside the display drivers\n",
                         Thread, (ULONG64)Waited * KeMaximumIncrement / 10000000);
                PnpWatchGuiStalls[PnpWatchGuiStallReports++] = &Thread->Tcb;
                NewStall = TRUE;
            }
        }
    }

    /* One stuck thread rarely shows who it waits for: when the screen stops
     * updating, show everything that is inside the display drivers too, at most
     * once a minute and five times per boot */
    if (NewStall && FullDumps < 5 &&
        (LastFullDump == 0 || KeQueryInterruptTime() - LastFullDump >= 60LL * 10000000))
    {
        FullDumps++;
        LastFullDump = KeQueryInterruptTime();
        PnpWatchDumpDisplayThreads();
    }
}

/* GMU bring-up timeline: every register change from the moment qcdx loads */
#define PNP_WATCH_GPU_TRACE_TIME    (60LL * 10000000)
#define PNP_WATCH_GPU_TRACE_LINES   800
#define PNP_WATCH_GPU_TRACE_WAIT    (-50LL * 10000)

typedef enum _PNP_WATCH_GPU_BLOCK
{
    PnpWatchGpuGpucc,
    PnpWatchGpuGmu,
    PnpWatchGpuPdc,
    PnpWatchGpuBlockCount
} PNP_WATCH_GPU_BLOCK;

typedef struct _PNP_WATCH_GPU_TRACE_REG
{
    PNP_WATCH_GPU_BLOCK Block;
    ULONG Offset;
    PCSTR Name;
} PNP_WATCH_GPU_TRACE_REG;

static const PNP_WATCH_GPU_TRACE_REG PnpWatchGpuTraceRegs[] =
{
    { PnpWatchGpuGpucc, 0x106C, "cx gdscr" },
    { PnpWatchGpuGpucc, 0x100C, "gx gdscr" },
    { PnpWatchGpuGpucc, 0x1098, "gmu cbcr" },
    { PnpWatchGpuPdc, 0x4500, "pdc enable" },
    { PnpWatchGpuGmu, 0x1E000, "cm3 sysreset" },
    { PnpWatchGpuGmu, 0x1E070, "fw init result" },
    { PnpWatchGpuGmu, 0x1E068, "fw busy" },
    { PnpWatchGpuGmu, 0x12FE0, "boot slumber option" },
    { PnpWatchGpuGmu, 0x12FE4, "gx vote" },
    { PnpWatchGpuGmu, 0x12FE8, "mx vote" },
    { PnpWatchGpuGmu, 0x1E658, "host2gmu raw" },
    { PnpWatchGpuGmu, 0x1E648, "gmu2host info" },
    { PnpWatchGpuGmu, 0x1E340, "sptprac status" },
    { PnpWatchGpuGmu, 0x1E3A0, "rpmh ctrl" },
    { PnpWatchGpuGmu, 0x1E3B0, "rpmh power state" },
    { PnpWatchGpuGmu, 0x1E600, "hfi ctrl status" },
    { PnpWatchGpuGmu, 0x1E714, "general 0" },
    { PnpWatchGpuGmu, 0x1E730, "general 7" },
    { PnpWatchGpuGmu, 0x2EC14, "ao host irq status" },
    { PnpWatchGpuGmu, 0x2EC1C, "rscc control req" },
    { PnpWatchGpuGmu, 0x2EC20, "rscc control ack" },
    { PnpWatchGpuGmu, 0x2EC30, "cx busy status" },
    { PnpWatchGpuGmu, 0x2EC4C, "ahb fence status" },
    { PnpWatchGpuGmu, 0x2D010, "rscc status0" },
    { PnpWatchGpuGmu, 0x2D404, "rscc seq busy" },
    { PnpWatchGpuGmu, 0x2DD18, "rscc tcs0 status" },
    { PnpWatchGpuGmu, 0x2DFB8, "rscc tcs1 status" },
    { PnpWatchGpuGmu, 0x2E258, "rscc tcs2 status" },
    { PnpWatchGpuGmu, 0x2E4F8, "rscc tcs3 status" }
};

static LONG PnpWatchGpuTraceStarted;

static BOOLEAN
PnpWatchIsQcdxLoaded(VOID)
{
    UNICODE_STRING QcdxName = RTL_CONSTANT_STRING(L"qcdxkm850.sys");
    PLDR_DATA_TABLE_ENTRY LdrEntry;
    PLIST_ENTRY Entry;
    BOOLEAN Found = FALSE;

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&PsLoadedModuleResource, TRUE);
    for (Entry = PsLoadedModuleList.Flink; Entry != &PsLoadedModuleList; Entry = Entry->Flink)
    {
        LdrEntry = CONTAINING_RECORD(Entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
        if (RtlEqualUnicodeString(&LdrEntry->BaseDllName, &QcdxName, TRUE))
        {
            Found = TRUE;
            break;
        }
    }
    ExReleaseResourceLite(&PsLoadedModuleResource);
    KeLeaveCriticalRegion();

    return Found;
}

/*
 * Polls the GPU clock controller, GMU, RSC and PDC while qcdx brings the GMU up and
 * prints what changed. GMU registers are only read while CX is powered and the GMU
 * clock runs, a gated block stalls the bus instead of answering.
 */
static VOID
NTAPI
PnpWatchGpuTraceThread(
    _In_ PVOID Context)
{
    static const ULONG Bases[PnpWatchGpuBlockCount] =
        { PNP_WATCH_GPUCC_BASE, PNP_WATCH_GMU_BASE, PNP_WATCH_GPU_PDC_BASE };
    static const ULONG Sizes[PnpWatchGpuBlockCount] =
        { PNP_WATCH_GPUCC_SIZE, PNP_WATCH_GMU_SIZE, PNP_WATCH_GPU_PDC_SIZE };
    ULONG Values[RTL_NUMBER_OF(PnpWatchGpuTraceRegs)];
    BOOLEAN Known[RTL_NUMBER_OF(PnpWatchGpuTraceRegs)];
    PUCHAR Va[PnpWatchGpuBlockCount];
    PHYSICAL_ADDRESS Physical;
    LARGE_INTEGER Wait, Start, Now, Frequency;
    ULONG Lines = 0, Loops = 0, Index, Value, Block;
    BOOLEAN GmuOn, WasGmuOn = FALSE;
    LONGLONG Micro;

    UNREFERENCED_PARAMETER(Context);

    Wait.QuadPart = PNP_WATCH_GPU_TRACE_WAIT;
    while (!PnpWatchIsQcdxLoaded())
        KeDelayExecutionThread(KernelMode, FALSE, &Wait);

    for (Block = 0; Block < PnpWatchGpuBlockCount; Block++)
    {
        Physical.QuadPart = Bases[Block];
        Va[Block] = MmMapIoSpace(Physical, Sizes[Block], MmNonCached);
        if (!Va[Block])
        {
            DbgPrint("GPUTRACE: cannot map block %lu\n", Block);
            return;
        }
    }

    RtlZeroMemory(Known, sizeof(Known));
    Start = KeQueryPerformanceCounter(&Frequency);
    DbgPrint("GPUTRACE: qcdx loaded, tracing for %I64d s\n", PNP_WATCH_GPU_TRACE_TIME / 10000000);

    for (;;)
    {
        Now = KeQueryPerformanceCounter(NULL);
        Micro = (Now.QuadPart - Start.QuadPart) * 1000000 / Frequency.QuadPart;
        if (Micro * 10 >= PNP_WATCH_GPU_TRACE_TIME || Lines >= PNP_WATCH_GPU_TRACE_LINES)
            break;

        GmuOn = FALSE;
        for (Index = 0; Index < RTL_NUMBER_OF(PnpWatchGpuTraceRegs); Index++)
        {
            const PNP_WATCH_GPU_TRACE_REG *Reg = &PnpWatchGpuTraceRegs[Index];

            /* The clock controller entries come first and decide whether the GMU can be read */
            if (Reg->Block == PnpWatchGpuGmu && !GmuOn)
            {
                Known[Index] = FALSE;
                continue;
            }

            Value = READ_REGISTER_ULONG((PULONG)(Va[Reg->Block] + Reg->Offset));
            if (!Known[Index] || Values[Index] != Value)
            {
                DbgPrint("GPUTRACE: %7I64d.%03I64d ms %-20s %08lx -> %08lx\n",
                         Micro / 1000, Micro % 1000, Reg->Name, Known[Index] ? Values[Index] : 0, Value);
                Lines++;
            }
            Values[Index] = Value;
            Known[Index] = TRUE;

            if (Index == 2)
                GmuOn = (Values[0] & PNP_WATCH_GDSC_ON) && !(Values[2] & PNP_WATCH_BRANCH_OFF);
        }

        if (GmuOn != WasGmuOn)
        {
            DbgPrint("GPUTRACE: %7I64d.%03I64d ms GMU %s\n", Micro / 1000, Micro % 1000,
                     GmuOn ? "readable" : "unpowered");
            WasGmuOn = GmuOn;
            Lines++;
        }

        KeStallExecutionProcessor(200);
        if (++Loops % 25 == 0)
            ZwYieldExecution();
    }

    DbgPrint("GPUTRACE: done after %lu lines\n", Lines);
    for (Block = 0; Block < PnpWatchGpuBlockCount; Block++)
        MmUnmapIoSpace(Va[Block], Sizes[Block]);
}

static VOID
NTAPI
PnpWatchThread(
    _In_ PVOID Context)
{
    LARGE_INTEGER Period, Now;
    PPNP_WATCH_SLOT Slot;
    PDEVICE_NODE DeviceNode;
    PETHREAD Thread;
    ULONG i;
    static const ULONG ThreadDumpTimes[] = { 45, 75, 150, 300 };   /* seconds since boot */
    ULONG NextThreadDump = 0;

    UNREFERENCED_PARAMETER(Context);
    Period.QuadPart = PNP_WATCH_PERIOD;

    for (;;)
    {
        KeDelayExecutionThread(KernelMode, FALSE, &Period);
        /* Not the system time, which boot moves by the time zone bias */
        Now.QuadPart = KeQueryInterruptTime();

        if (NextThreadDump < RTL_NUMBER_OF(ThreadDumpTimes) &&
            Now.QuadPart >= ThreadDumpTimes[NextThreadDump] * 10000000LL)
        {
            NextThreadDump++;
            PnpWatchDumpDisplayThreads();
        }

        PnpWatchDumpStalledGuiThreads();

        for (i = 0; i < PNP_WATCH_SLOTS; i++)
        {
            Slot = &PnpWatchSlots[i];
            Thread = Slot->Thread;
            if (!Thread || Slot->Reported || Now.QuadPart - Slot->StartTime.QuadPart < PNP_WATCH_REPORT_AFTER)
                continue;

            /* Reported once per request; the slot is reused only after the request returns */
            Slot->Reported = TRUE;
            DeviceNode = IopGetDeviceNode(Slot->DeviceObject);
            DbgPrint("PNPWATCH: IRP %02x/%02x to %wZ still running after %I64d s, thread %p\n",
                     Slot->MajorFunction, Slot->MinorFunction,
                     DeviceNode ? &DeviceNode->InstancePath : NULL,
                     (Now.QuadPart - Slot->StartTime.QuadPart) / 10000000, Thread);
            PnpWatchScanStack(&Thread->Tcb, NULL, 0, 0);
        }
    }
}

static PPNP_WATCH_SLOT
PnpWatchBegin(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIO_STACK_LOCATION IoStackLocation)
{
    HANDLE ThreadHandle;
    ULONG i;

    if (!InterlockedCompareExchange(&PnpWatchStarted, 1, 0))
    {
        if (NT_SUCCESS(PsCreateSystemThread(&ThreadHandle, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                            PnpWatchThread, NULL)))
        {
            ZwClose(ThreadHandle);
        }
    }

    if (!InterlockedCompareExchange(&PnpWatchGpuTraceStarted, 1, 0))
    {
        if (NT_SUCCESS(PsCreateSystemThread(&ThreadHandle, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                            PnpWatchGpuTraceThread, NULL)))
        {
            ZwClose(ThreadHandle);
        }
    }

    for (i = 0; i < PNP_WATCH_SLOTS; i++)
    {
        if (InterlockedCompareExchangePointer((PVOID *)&PnpWatchSlots[i].Thread, PsGetCurrentThread(), NULL) == NULL)
        {
            PnpWatchSlots[i].DeviceObject = DeviceObject;
            PnpWatchSlots[i].MajorFunction = IoStackLocation->MajorFunction;
            PnpWatchSlots[i].MinorFunction = IoStackLocation->MinorFunction;
            PnpWatchSlots[i].Reported = FALSE;
            PnpWatchSlots[i].StartTime.QuadPart = KeQueryInterruptTime();
            return &PnpWatchSlots[i];
        }
    }
    return NULL;
}

static VOID
PnpWatchEnd(
    _In_opt_ PPNP_WATCH_SLOT Slot,
    _In_ NTSTATUS Status)
{
    PDEVICE_NODE DeviceNode;

    if (!Slot)
        return;

    if (Slot->Reported)
    {
        DeviceNode = IopGetDeviceNode(Slot->DeviceObject);
        DbgPrint("PNPWATCH: IRP %02x/%02x to %wZ finished late with 0x%lx\n",
                 Slot->MajorFunction, Slot->MinorFunction,
                 DeviceNode ? &DeviceNode->InstancePath : NULL, Status);
    }
    InterlockedExchangePointer((PVOID *)&Slot->Thread, NULL);
}

NTSTATUS
IopSynchronousCall(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIO_STACK_LOCATION IoStackLocation,
    _Out_ PVOID *Information)
{
    PIRP Irp;
    PIO_STACK_LOCATION IrpStack;
    IO_STATUS_BLOCK IoStatusBlock;
    IO_STATUS_BLOCK Result;
    KEVENT Event;
    NTSTATUS Status;
    PDEVICE_OBJECT TopDeviceObject;
    PPNP_WATCH_SLOT WatchSlot;
    PAGED_CODE();

    WatchSlot = PnpWatchBegin(DeviceObject, IoStackLocation);

    /* Call the top of the device stack */
    TopDeviceObject = IoGetAttachedDeviceReference(DeviceObject);

    /* Allocate an IRP */
    Irp = IoAllocateIrp(TopDeviceObject->StackSize, FALSE);
    if (!Irp)
    {
        ObDereferenceObject(TopDeviceObject);
        PnpWatchEnd(WatchSlot, STATUS_INSUFFICIENT_RESOURCES);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Initialize to failure */
    Irp->IoStatus.Status = IoStatusBlock.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = IoStatusBlock.Information = 0;

    /* Special case for IRP_MN_FILTER_RESOURCE_REQUIREMENTS */
    if ((IoStackLocation->MajorFunction == IRP_MJ_PNP) &&
        (IoStackLocation->MinorFunction == IRP_MN_FILTER_RESOURCE_REQUIREMENTS))
    {
        /* Copy the resource requirements list into the IOSB */
        Irp->IoStatus.Information =
        IoStatusBlock.Information = (ULONG_PTR)IoStackLocation->Parameters.FilterResourceRequirements.IoResourceRequirementList;
    }

    Result = IoStatusBlock;

    /* Initialize the event */
    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

    /* Set them up */
    Irp->UserIosb = &IoStatusBlock;
    Irp->UserEvent = &Event;

    /* Queue the IRP */
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    IoQueueThreadIrp(Irp);

    /* Copy-in the stack */
    IrpStack = IoGetNextIrpStackLocation(Irp);
    *IrpStack = *IoStackLocation;
    IoSetCompletionRoutine(Irp, IopSynchronousCallCompletion, &Result, TRUE, TRUE, TRUE);

    /* Call the driver */
    Status = IoCallDriver(TopDeviceObject, Irp);
    /* Otherwise we may get stuck here or have IoStatusBlock not populated */
    ASSERT(!KeAreAllApcsDisabled());
#if DBG
    /*
     * A request completed in this thread has run its completion APC by now. Other kernel APCs
     * can stay queued while the thread is in a critical region, as file systems are, so only
     * this IRP's own APC counts. Its address is compared, the IRP may already be freed.
     */
    if (Status != STATUS_PENDING)
    {
        PLIST_ENTRY ApcListHead = &KeGetCurrentThread()->ApcState.ApcListHead[KernelMode];
        PLIST_ENTRY ApcEntry;

        for (ApcEntry = ApcListHead->Flink; ApcEntry != ApcListHead; ApcEntry = ApcEntry->Flink)
        {
            if (ApcEntry != &Irp->Tail.Apc.ApcListEntry)
                continue;

            DPRINT1("Completion APC of IRP %p still queued: Irql %u Pending %u KernelApcDisable %d SpecialApcDisable %d Flags 0x%Ix\n",
                    Irp,
                    KeGetCurrentIrql(),
                    KeGetCurrentThread()->ApcState.KernelApcPending,
                    KeGetCurrentThread()->KernelApcDisable,
                    KeGetCurrentThread()->SpecialApcDisable,
                    IopReadInterruptFlags());
            DbgBreakPoint();
            break;
        }
    }
#endif
    if (Status == STATUS_PENDING)
    {
        /* Wait for it */
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }

    Status = Result.Status;

    /* Remove the reference */
    ObDereferenceObject(TopDeviceObject);
    PnpWatchEnd(WatchSlot, Status);

    /* Return the information */
    *Information = (PVOID)Result.Information;
    return Status;
}

// IRP_MN_START_DEVICE (0x00)
NTSTATUS
PiIrpStartDevice(
    _In_ PDEVICE_NODE DeviceNode)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeResourcesAssigned);

    /* A device added before its plug-in registered gets the offer here */
    PopFxPrepareDevice(DeviceNode->PhysicalDeviceObject, TRUE);

    PVOID info;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_START_DEVICE,
        .Parameters.StartDevice.AllocatedResources = DeviceNode->ResourceList,
        .Parameters.StartDevice.AllocatedResourcesTranslated = DeviceNode->ResourceListTranslated
    };

    // Vista+ does an asynchronous call
    NTSTATUS status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, &info);
    DeviceNode->CompletionStatus = status;
    if (!NT_SUCCESS(status))
        DPRINT1("Start %wZ -> 0x%lx\n", &DeviceNode->InstancePath, status);
    return status;
}

// IRP_MN_STOP_DEVICE (0x04)
NTSTATUS
PiIrpStopDevice(
    _In_ PDEVICE_NODE DeviceNode)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeQueryStopped);

    PVOID info;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_STOP_DEVICE
    };

    DPRINT1("Stopping %wZ\n", &DeviceNode->InstancePath);

    // Drivers should never fail a IRP_MN_STOP_DEVICE request
    NTSTATUS status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, &info);
    ASSERT(NT_SUCCESS(status));
    return status;
}

// IRP_MN_QUERY_STOP_DEVICE (0x05)
NTSTATUS
PiIrpQueryStopDevice(
    _In_ PDEVICE_NODE DeviceNode)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeStarted);

    PVOID info;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_STOP_DEVICE
    };

    NTSTATUS status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, &info);
    DeviceNode->CompletionStatus = status;
    return status;
}

// IRP_MN_CANCEL_STOP_DEVICE (0x06)
NTSTATUS
PiIrpCancelStopDevice(
    _In_ PDEVICE_NODE DeviceNode)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeQueryStopped);

    PVOID info;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_CANCEL_STOP_DEVICE
    };

    // in fact we don't care which status is returned here
    NTSTATUS status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, &info);
    ASSERT(NT_SUCCESS(status));
    return status;
}

// IRP_MN_QUERY_DEVICE_RELATIONS (0x07)
NTSTATUS
PiIrpQueryDeviceRelations(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ DEVICE_RELATION_TYPE Type)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeStarted);

    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_DEVICE_RELATIONS,
        .Parameters.QueryDeviceRelations.Type = Type
    };

    // Vista+ does an asynchronous call
    NTSTATUS status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject,
                                         &stack,
                                         (PVOID)&DeviceNode->OverUsed1.PendingDeviceRelations);
    DeviceNode->CompletionStatus = status;
    return status;
}

// IRP_MN_QUERY_RESOURCES (0x0A)
NTSTATUS
PiIrpQueryResources(
    _In_ PDEVICE_NODE DeviceNode,
    _Out_ PCM_RESOURCE_LIST *Resources)
{
    PAGED_CODE();

    ASSERT(DeviceNode);

    ULONG_PTR longRes;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_RESOURCES
    };

    NTSTATUS status;
    status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, (PVOID)&longRes);
    if (NT_SUCCESS(status))
    {
        *Resources = (PVOID)longRes;
    }

    return status;
}

// IRP_MN_QUERY_RESOURCE_REQUIREMENTS (0x0B)
NTSTATUS
PiIrpQueryResourceRequirements(
    _In_ PDEVICE_NODE DeviceNode,
    _Out_ PIO_RESOURCE_REQUIREMENTS_LIST *Resources)
{
    PAGED_CODE();

    ASSERT(DeviceNode);

    ULONG_PTR longRes;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_RESOURCE_REQUIREMENTS
    };

    NTSTATUS status;
    status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, (PVOID)&longRes);
    if (NT_SUCCESS(status))
    {
        *Resources = (PVOID)longRes;
    }

    return status;
}

// IRP_MN_QUERY_DEVICE_TEXT (0x0C)
NTSTATUS
PiIrpQueryDeviceText(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ LCID LocaleId,
    _In_ DEVICE_TEXT_TYPE Type,
    _Out_ PWSTR *DeviceText)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeUninitialized);

    ULONG_PTR longText;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_DEVICE_TEXT,
        .Parameters.QueryDeviceText.DeviceTextType = Type,
        .Parameters.QueryDeviceText.LocaleId = LocaleId
    };

    NTSTATUS status;
    status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, (PVOID)&longText);
    if (NT_SUCCESS(status))
    {
        *DeviceText = (PVOID)longText;
    }

    return status;
}

// IRP_MN_QUERY_PNP_DEVICE_STATE (0x14)
NTSTATUS
PiIrpQueryPnPDeviceState(
    _In_ PDEVICE_NODE DeviceNode,
    _Out_ PPNP_DEVICE_STATE DeviceState)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeStartPostWork ||
           DeviceNode->State == DeviceNodeStarted);

    ULONG_PTR longState;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_PNP_DEVICE_STATE
    };

    NTSTATUS status;
    status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, (PVOID)&longState);
    if (NT_SUCCESS(status))
    {
        *DeviceState = longState;
    }

    return status;
}
