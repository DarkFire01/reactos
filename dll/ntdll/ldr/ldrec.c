/*
 * PROJECT:     ReactOS NT User Mode Library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64EC and ARM64X loader support
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/**
 * @brief
 * Returns the CHPE metadata of an ARM64X or ARM64EC image, if it has one.
 */
PIMAGE_ARM64EC_METADATA
NTAPI
LdrpGetArm64EcMetadata(
    _In_ PVOID ImageBase)
{
    PIMAGE_LOAD_CONFIG_DIRECTORY64 LoadConfig;
    ULONG Size;

    LoadConfig = RtlImageDirectoryEntryToData(ImageBase,
                                              TRUE,
                                              IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG,
                                              &Size);
    if (!LoadConfig)
        return NULL;

    if (Size < RTL_SIZEOF_THROUGH_FIELD(IMAGE_LOAD_CONFIG_DIRECTORY64, CHPEMetadataPointer) ||
        LoadConfig->Size < RTL_SIZEOF_THROUGH_FIELD(IMAGE_LOAD_CONFIG_DIRECTORY64, CHPEMetadataPointer))
    {
        return NULL;
    }

    return (PIMAGE_ARM64EC_METADATA)(ULONG_PTR)LoadConfig->CHPEMetadataPointer;
}

/**
 * @brief
 * Tells whether an address inside an image is ARM64 or ARM64EC code.
 */
static
BOOLEAN
LdrpIsEcCodeInImage(
    _In_ PVOID ImageBase,
    _In_ ULONG Rva)
{
    PIMAGE_ARM64EC_METADATA Metadata;
    PIMAGE_NT_HEADERS NtHeader;
    PIMAGE_CHPE_RANGE_ENTRY Range;
    ULONG i, Start;

    Metadata = LdrpGetArm64EcMetadata(ImageBase);
    if (!Metadata)
    {
        /* Without a code map the whole image is of its machine type */
        NtHeader = RtlImageNtHeader(ImageBase);
        return NtHeader && NtHeader->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64;
    }

    Range = (PIMAGE_CHPE_RANGE_ENTRY)((ULONG_PTR)ImageBase + Metadata->CodeMap);
    for (i = 0; i < Metadata->CodeMapCount; i++)
    {
        Start = Range[i].StartOffset & ~3UL;
        if (Rva >= Start && Rva - Start < Range[i].Length)
            return (Range[i].StartOffset & 3) != IMAGE_CHPE_RANGE_AMD64;
    }

    return FALSE;
}

/**
 * @brief
 * Tells whether code at an address runs natively, as ARM64 or ARM64EC, or has to be emulated as x64.
 *
 * @param[in] CodePointer
 * The code address.
 *
 * @return
 * TRUE for ARM64 and ARM64EC code, FALSE for x64 code.
 */
BOOLEAN
NTAPI
RtlIsEcCode(
    _In_ ULONG64 CodePointer)
{
    PPEB_LDR_DATA Ldr = NtCurrentPeb()->Ldr;
    PULONG64 Bitmap = NtCurrentPeb()->EcCodeBitMap;
    PLDR_DATA_TABLE_ENTRY LdrEntry;
    PLIST_ENTRY ListHead, Entry;
    ULONG_PTR Base, Page;

    /* x64 processes have the bitmap, one bit for each page of ARM64EC code */
    if (Bitmap)
    {
        Page = (ULONG_PTR)CodePointer >> PAGE_SHIFT;
        return (Bitmap[Page / 64] >> (Page % 64)) & 1;
    }

    if (!Ldr)
        return FALSE;

    ListHead = &Ldr->InMemoryOrderModuleList;
    for (Entry = ListHead->Flink; Entry != ListHead; Entry = Entry->Flink)
    {
        LdrEntry = CONTAINING_RECORD(Entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        Base = (ULONG_PTR)LdrEntry->DllBase;

        if (CodePointer >= Base && CodePointer - Base < LdrEntry->SizeOfImage)
            return LdrpIsEcCodeInImage(LdrEntry->DllBase, (ULONG)(CodePointer - Base));
    }

    return FALSE;
}

#if defined(_M_ARM64) || defined(_M_ARM64EC)

/* GLOBALS ******************************************************************/

/* The main image is x64, ARM64X images get their ARM64EC view */
BOOLEAN LdrpX64Process;

extern PLDR_DATA_TABLE_ENTRY LdrpImageEntry;
extern PLDR_DATA_TABLE_ENTRY LdrpNtDllDataTableEntry;

static UNICODE_STRING LdrpEmulatorKey = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\Software\\Microsoft\\Wow64\\amd64");
static UNICODE_STRING LdrpDefaultEmulator = RTL_CONSTANT_STRING(L"xtajit64.dll");

static PVOID LdrpEmulatorBase;
static PVOID LdrpExitToX64;
static PVOID LdrpRetToEntryThunk;
static PVOID LdrpDispatchJump;
static VOID (NTAPI *LdrpBeginSimulation)(VOID);
static NTSTATUS (NTAPI *LdrpEmulatorProcessInit)(VOID);
static NTSTATUS (NTAPI *LdrpEmulatorThreadInit)(VOID);
static NTSTATUS (NTAPI *LdrpEmulatorResetToConsistentState)(PEXCEPTION_RECORD, PVOID, PARM64_NT_CONTEXT);
static NTSTATUS (NTAPI *LdrpEmulatorNotifyMapViewOfSection)(PVOID, PVOID, PVOID, SIZE_T, ULONG, ULONG);
static VOID (NTAPI *LdrpEmulatorNotifyUnmapViewOfSection)(PVOID, BOOL, NTSTATUS);
static VOID (NTAPI *LdrpEmulatorNotifyMemoryAlloc)(PVOID, SIZE_T, ULONG, ULONG, BOOL, NTSTATUS);
static VOID (NTAPI *LdrpEmulatorNotifyMemoryFree)(PVOID, SIZE_T, ULONG, BOOL, NTSTATUS);
static VOID (NTAPI *LdrpEmulatorNotifyMemoryProtect)(PVOID, SIZE_T, ULONG, BOOL, NTSTATUS);

/* ldrecasm.s */
VOID
LdrpEcCallCheck(VOID);

/**
 * @brief
 * Writes a pointer into a read only part of an image.
 */
static
VOID
LdrpWriteImagePointer(
    _In_ PVOID Address,
    _In_ PVOID Value)
{
    PVOID ProtectBase = Address;
    SIZE_T ProtectSize = sizeof(PVOID);
    ULONG OldProtect;

    if (!NT_SUCCESS(NtProtectVirtualMemory(NtCurrentProcess(), &ProtectBase, &ProtectSize, PAGE_READWRITE, &OldProtect)))
        return;

    *(PVOID *)Address = Value;
    NtProtectVirtualMemory(NtCurrentProcess(), &ProtectBase, &ProtectSize, OldProtect, &OldProtect);
}

/**
 * @brief
 * Applies one block of ARM64X fixups to a page.
 */
static
VOID
LdrpApplyArm64XBlock(
    _In_ PUCHAR Page,
    _In_ PUSHORT Entry,
    _In_ PUSHORT End)
{
    PVOID ProtectBase = Page;
    SIZE_T ProtectSize = PAGE_SIZE;
    ULONG OldProtect, Size, Delta;
    PUCHAR Target;
    USHORT Type, Meta;

    if (!NT_SUCCESS(NtProtectVirtualMemory(NtCurrentProcess(), &ProtectBase, &ProtectSize, PAGE_READWRITE, &OldProtect)))
        return;

    /* A zero entry pads the block */
    while (Entry < End && *Entry)
    {
        Target = Page + (*Entry & 0xFFF);
        Type = (*Entry >> 12) & 3;
        Meta = *Entry >> 14;
        Entry++;

        switch (Type)
        {
            case IMAGE_DVRT_ARM64X_FIXUP_TYPE_ZEROFILL:
                RtlZeroMemory(Target, (SIZE_T)1 << Meta);
                break;

            case IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE:
                Size = 1U << Meta;
                RtlCopyMemory(Target, Entry, Size);
                Entry += Size / sizeof(USHORT);
                break;

            case IMAGE_DVRT_ARM64X_FIXUP_TYPE_DELTA:
                /* Meta bit 1 picks a scale of 8 over 4, bit 0 makes it negative */
                Delta = (ULONG)*Entry++ * ((Meta & 2) ? 8 : 4);
                if (Meta & 1)
                    Delta = 0 - Delta;
                *(PULONG)Target += Delta;
                break;

            default:
                break;
        }
    }

    NtProtectVirtualMemory(NtCurrentProcess(), &ProtectBase, &ProtectSize, OldProtect, &OldProtect);
}

/**
 * @brief
 * Switches an ARM64X image to its ARM64EC view with the fixups in its load config.
 *
 * @param[in] ImageBase
 * The mapped image, before base relocations are applied.
 */
VOID
NTAPI
LdrpApplyArm64XView(
    _In_ PVOID ImageBase)
{
    PIMAGE_NT_HEADERS NtHeader = RtlImageNtHeader(ImageBase);
    PIMAGE_LOAD_CONFIG_DIRECTORY64 LoadConfig;
    PIMAGE_SECTION_HEADER Section;
    PIMAGE_DYNAMIC_RELOCATION_TABLE Table;
    PIMAGE_DYNAMIC_RELOCATION64 Relocation;
    PIMAGE_BASE_RELOCATION Block;
    PUCHAR Cursor, End, BlockEnd;
    ULONG Size;

    if (!NtHeader || NtHeader->FileHeader.Machine != IMAGE_FILE_MACHINE_ARM64)
        return;

    LoadConfig = RtlImageDirectoryEntryToData(ImageBase, TRUE, IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG, &Size);
    if (!LoadConfig ||
        Size < RTL_SIZEOF_THROUGH_FIELD(IMAGE_LOAD_CONFIG_DIRECTORY64, DynamicValueRelocTableSection) ||
        LoadConfig->Size < RTL_SIZEOF_THROUGH_FIELD(IMAGE_LOAD_CONFIG_DIRECTORY64, DynamicValueRelocTableSection) ||
        !LoadConfig->DynamicValueRelocTableSection ||
        LoadConfig->DynamicValueRelocTableSection > NtHeader->FileHeader.NumberOfSections)
    {
        return;
    }

    Section = IMAGE_FIRST_SECTION(NtHeader) + LoadConfig->DynamicValueRelocTableSection - 1;
    if (LoadConfig->DynamicValueRelocTableOffset >= Section->Misc.VirtualSize)
        return;

    Table = (PIMAGE_DYNAMIC_RELOCATION_TABLE)((PUCHAR)ImageBase + Section->VirtualAddress +
                                              LoadConfig->DynamicValueRelocTableOffset);
    if (Table->Version != 1)
    {
        DPRINT1("LDR: Dynamic relocation table version %lu is not supported\n", Table->Version);
        return;
    }

    Cursor = (PUCHAR)(Table + 1);
    End = Cursor + Table->Size;
    while (Cursor + sizeof(*Relocation) <= End)
    {
        Relocation = (PIMAGE_DYNAMIC_RELOCATION64)Cursor;
        Cursor += sizeof(*Relocation) + Relocation->BaseRelocSize;

        if (Relocation->Symbol != IMAGE_DYNAMIC_RELOCATION_ARM64X)
            continue;

        Block = (PIMAGE_BASE_RELOCATION)(Relocation + 1);
        BlockEnd = (PUCHAR)Block + Relocation->BaseRelocSize;
        while ((PUCHAR)Block + sizeof(*Block) <= BlockEnd && Block->SizeOfBlock)
        {
            LdrpApplyArm64XBlock((PUCHAR)ImageBase + Block->VirtualAddress,
                                 (PUSHORT)(Block + 1),
                                 (PUSHORT)((PUCHAR)Block + Block->SizeOfBlock));
            Block = (PIMAGE_BASE_RELOCATION)((PUCHAR)Block + Block->SizeOfBlock);
        }
        break;
    }
}

/**
 * @brief
 * Tells whether an address inside an image is code of the given code map range type.
 */
static
BOOLEAN
LdrpIsCodeOfType(
    _In_ PVOID ImageBase,
    _In_ PVOID Address,
    _In_ ULONG Type)
{
    PIMAGE_ARM64EC_METADATA Metadata = LdrpGetArm64EcMetadata(ImageBase);
    PIMAGE_CHPE_RANGE_ENTRY Range;
    ULONG_PTR Rva = (ULONG_PTR)Address - (ULONG_PTR)ImageBase;
    ULONG i, Start;

    if (!Metadata)
        return FALSE;

    Range = (PIMAGE_CHPE_RANGE_ENTRY)((ULONG_PTR)ImageBase + Metadata->CodeMap);
    for (i = 0; i < Metadata->CodeMapCount; i++)
    {
        Start = Range[i].StartOffset & ~3UL;
        if (Rva >= Start && Rva - Start < Range[i].Length)
            return (Range[i].StartOffset & 3) == Type;
    }

    return FALSE;
}

/**
 * @brief
 * Tells whether an address inside an image is ARM64EC code, as opposed to ARM64 or x64 code.
 */
BOOLEAN
NTAPI
LdrpIsArm64EcCode(
    _In_ PVOID ImageBase,
    _In_ PVOID Address)
{
    return LdrpIsCodeOfType(ImageBase, Address, IMAGE_CHPE_RANGE_ARM64EC);
}

/**
 * @brief
 * Tells whether an address inside an ARM64X image is code of its ARM64 half.
 */
BOOLEAN
NTAPI
LdrpIsArm64Code(
    _In_ PVOID ImageBase,
    _In_ PVOID Address)
{
    return LdrpIsCodeOfType(ImageBase, Address, IMAGE_CHPE_RANGE_ARM64);
}

/**
 * @brief
 * Follows the x64 fast forward thunk of an ARM64EC function to the function.
 *
 * @return
 * The ARM64EC function, or the address itself when it is not such a thunk.
 */
PVOID
NTAPI
LdrpResolveFastForward(
    _In_ PVOID Address)
{
    /* mov rax, rsp / mov [rax+20h], rbx / push rbp / pop rbp / jmp rel32 */
    static const UCHAR Thunk[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x5D, 0xE9 };
    PUCHAR Code = Address;
    LONG Offset;

    if (((ULONG_PTR)Code & 15) || RtlCompareMemory(Code, Thunk, sizeof(Thunk)) != sizeof(Thunk))
        return Address;

    RtlCopyMemory(&Offset, Code + sizeof(Thunk), sizeof(Offset));
    /* The image may not be in the module list yet, the thunk alone identifies it */
    return Code + sizeof(Thunk) + sizeof(Offset) + Offset;
}

/**
 * @brief
 * Sets or clears the code bitmap bits of the pages a range of code touches.
 */
static
VOID
LdrpSetEcCodeBits(
    _In_ ULONG_PTR Start,
    _In_ SIZE_T Length,
    _In_ BOOLEAN Set)
{
    PULONG64 Bitmap = NtCurrentPeb()->EcCodeBitMap;
    ULONG_PTR Page, Last;

    if (!Bitmap || !Length)
        return;

    Last = (Start + Length - 1) >> PAGE_SHIFT;
    for (Page = Start >> PAGE_SHIFT; Page <= Last; Page++)
    {
        if (Set)
            InterlockedOr64((PLONG64)&Bitmap[Page / 64], 1LL << (Page % 64));
        else
            InterlockedAnd64((PLONG64)&Bitmap[Page / 64], ~(1LL << (Page % 64)));
    }
}

/**
 * @brief
 * Sets or clears the code bitmap bits of the ARM64EC code in an image.
 */
static
VOID
LdrpMarkEcCode(
    _In_ PVOID ImageBase,
    _In_ BOOLEAN Set)
{
    PIMAGE_ARM64EC_METADATA Metadata = LdrpGetArm64EcMetadata(ImageBase);
    PIMAGE_CHPE_RANGE_ENTRY Range;
    ULONG i;

    if (!Metadata)
        return;

    /* Only ARM64EC code has the entry thunks x64 callers go through, plain ARM64 code stays out */
    Range = (PIMAGE_CHPE_RANGE_ENTRY)((ULONG_PTR)ImageBase + Metadata->CodeMap);
    for (i = 0; i < Metadata->CodeMapCount; i++)
    {
        if ((Range[i].StartOffset & 3) == IMAGE_CHPE_RANGE_ARM64EC)
            LdrpSetEcCodeBits((ULONG_PTR)ImageBase + (Range[i].StartOffset & ~3UL), Range[i].Length, Set);
    }
}

/**
 * @brief
 * Points the emulator dispatch slots of an ARM64EC image at the emulator and the loader call check,
 * and marks its ARM64EC code in the code bitmap.
 */
VOID
NTAPI
LdrpUpdateHybridMetadata(
    _In_ PVOID ImageBase)
{
    PIMAGE_ARM64EC_METADATA Metadata = LdrpGetArm64EcMetadata(ImageBase);
    PUCHAR Base = ImageBase;

    if (!Metadata)
        return;

    LdrpMarkEcCode(ImageBase, TRUE);

    if (Metadata->__os_arm64x_dispatch_call)
        LdrpWriteImagePointer(Base + Metadata->__os_arm64x_dispatch_call, (PVOID)LdrpEcCallCheck);
    if (Metadata->__os_arm64x_dispatch_icall)
        LdrpWriteImagePointer(Base + Metadata->__os_arm64x_dispatch_icall, (PVOID)LdrpEcCallCheck);
    if (Metadata->__os_arm64x_dispatch_icall_cfg)
        LdrpWriteImagePointer(Base + Metadata->__os_arm64x_dispatch_icall_cfg, (PVOID)LdrpEcCallCheck);

    /* The emulator image itself maps before its entry points are known */
    if (!LdrpEmulatorBase)
        return;

    if (Metadata->__os_arm64x_dispatch_call_no_redirect)
        LdrpWriteImagePointer(Base + Metadata->__os_arm64x_dispatch_call_no_redirect, LdrpExitToX64);
    if (Metadata->__os_arm64x_dispatch_ret)
        LdrpWriteImagePointer(Base + Metadata->__os_arm64x_dispatch_ret, LdrpRetToEntryThunk);
    if (Metadata->__os_arm64x_dispatch_fptr)
        LdrpWriteImagePointer(Base + Metadata->__os_arm64x_dispatch_fptr, LdrpDispatchJump);
}

/**
 * @brief
 * Marks the thread as inside an emulator notification, so memory calls the emulator makes from it are not reported back.
 *
 * @return
 * The CPU area to clear the mark on, or NULL when the notification must be skipped.
 */
static
PCHPE_V2_CPU_AREA_INFO
LdrpEnterEmulatorCallback(
    _In_opt_ PVOID Notify)
{
    PCHPE_V2_CPU_AREA_INFO CpuArea = NtCurrentTeb()->ChpeV2CpuAreaInfo;

    /* Threads the emulator does not run have no CPU area, it cannot take notifications on them */
    if (!Notify || !CpuArea || CpuArea->InSyscallCallback)
        return NULL;

    CpuArea->InSyscallCallback = TRUE;
    return CpuArea;
}

/**
 * @brief
 * Tells the emulator about an image view the loader mapped, so it knows the code in it.
 */
VOID
NTAPI
LdrpNotifyImageMapped(
    _In_ PVOID ViewBase,
    _In_ SIZE_T ViewSize)
{
    PCHPE_V2_CPU_AREA_INFO CpuArea = LdrpEnterEmulatorCallback((PVOID)LdrpEmulatorNotifyMapViewOfSection);

    if (!CpuArea)
        return;

    LdrpEmulatorNotifyMapViewOfSection(NULL, ViewBase, NULL, ViewSize, 0, PAGE_EXECUTE_WRITECOPY);
    CpuArea->InSyscallCallback = FALSE;
}

/**
 * @brief
 * Unmaps an image view of an x64 process, taking it out of the code bitmap and the emulator.
 */
NTSTATUS
NTAPI
LdrpUnmapImageView(
    _In_ PVOID ViewBase)
{
    PCHPE_V2_CPU_AREA_INFO CpuArea;
    NTSTATUS Status;

    LdrpMarkEcCode(ViewBase, FALSE);

    CpuArea = LdrpEnterEmulatorCallback((PVOID)LdrpEmulatorNotifyUnmapViewOfSection);
    if (!CpuArea)
        return NtUnmapViewOfSection(NtCurrentProcess(), ViewBase);

    LdrpEmulatorNotifyUnmapViewOfSection(ViewBase, FALSE, STATUS_SUCCESS);
    Status = NtUnmapViewOfSection(NtCurrentProcess(), ViewBase);
    LdrpEmulatorNotifyUnmapViewOfSection(ViewBase, TRUE, Status);

    CpuArea->InSyscallCallback = FALSE;
    return Status;
}

/**
 * @brief
 * Unpacks the x64 state an ARM64 context holds in the ARM64EC register mapping.
 */
static
VOID
LdrpUnpackX64Context(
    _In_ PARM64_NT_CONTEXT Native,
    _Inout_ PARM64EC_NT_CONTEXT Guest)
{
    ULONG EFlags;

    Guest->X8 = Native->X8;
    Guest->X0 = Native->X0;
    Guest->X1 = Native->X1;
    Guest->X27 = Native->X27;
    Guest->Sp = Native->Sp;
    Guest->Fp = Native->Fp;
    Guest->X25 = Native->X25;
    Guest->X26 = Native->X26;
    Guest->X2 = Native->X2;
    Guest->X3 = Native->X3;
    Guest->X4 = Native->X4;
    Guest->X5 = Native->X5;
    Guest->X19 = Native->X19;
    Guest->X20 = Native->X20;
    Guest->X21 = Native->X21;
    Guest->X22 = Native->X22;
    Guest->Pc = Native->Pc;

    /* The MMX registers, with their top words kept in x16 and x17 */
    Guest->Lr = Native->Lr;
    Guest->X6 = Native->X6;
    Guest->X7 = Native->X7;
    Guest->X9 = Native->X9;
    Guest->X10 = Native->X10;
    Guest->X11 = Native->X11;
    Guest->X12 = Native->X12;
    Guest->X15 = Native->X15;
    Guest->X16_0 = (USHORT)Native->X16;
    Guest->X16_1 = (USHORT)(Native->X16 >> 16);
    Guest->X16_2 = (USHORT)(Native->X16 >> 32);
    Guest->X16_3 = (USHORT)(Native->X16 >> 48);
    Guest->X17_0 = (USHORT)Native->X17;
    Guest->X17_1 = (USHORT)(Native->X17 >> 16);
    Guest->X17_2 = (USHORT)(Native->X17 >> 32);
    Guest->X17_3 = (USHORT)(Native->X17 >> 48);
    RtlCopyMemory(Guest->V, Native->V, sizeof(Guest->V));

    /* NZCV and the single step bit carry SF, ZF, CF, OF and TF, the other flags stay */
    EFlags = Guest->AMD64_EFlags & ~(0x0001 | 0x0040 | 0x0080 | 0x0100 | 0x0800);
    if (Native->Cpsr & (1UL << 29))
        EFlags |= 0x0001;
    if (Native->Cpsr & (1UL << 30))
        EFlags |= 0x0040;
    if (Native->Cpsr & (1UL << 31))
        EFlags |= 0x0080;
    if (Native->Cpsr & (1UL << 21))
        EFlags |= 0x0100;
    if (Native->Cpsr & (1UL << 28))
        EFlags |= 0x0800;
    Guest->AMD64_EFlags = EFlags;
}

/**
 * @brief
 * Lets the emulator restore its state before an exception in an x64 process is dispatched.
 * It does not come back when the exception was its own.
 */
VOID
NTAPI
LdrpResetEmulatorState(
    _In_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PARM64_NT_CONTEXT Context)
{
    PCHPE_V2_CPU_AREA_INFO CpuArea = NtCurrentTeb()->ChpeV2CpuAreaInfo;
    ARM64EC_NT_CONTEXT Guest;

    /* Threads the emulator does not run have no CPU area */
    if (!LdrpEmulatorResetToConsistentState || !CpuArea)
        return;

    Guest = *CpuArea->ContextAmd64;
    LdrpUnpackX64Context(Context, &Guest);
    LdrpEmulatorResetToConsistentState(ExceptionRecord, &Guest, Context);
}

#ifdef _M_ARM64EC

/* ntdllffs.S */
extern ULONG_PTR __arm64ec_syscall_ffs[];
extern ULONG __arm64ec_syscall_ffs_size;

typedef ULONG_PTR (NTAPI *PLDRP_SYSTEM_SERVICE)(ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                                                ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                                                ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                                                ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR);

/**
 * @brief
 * Runs a system call x64 code made with the syscall instruction, x64 register numbering.
 */
static
ULONG_PTR
LdrpEmulateSystemCall(
    _In_ PARM64EC_NT_CONTEXT Guest)
{
    PLDRP_SYSTEM_SERVICE Service;
    PULONG_PTR Stack;

    if (Guest->X8 >= __arm64ec_syscall_ffs_size)
        return (ULONG_PTR)STATUS_INVALID_SYSTEM_SERVICE;

    /* r10 holds the first argument, the fifth and up follow the return address and the home space */
    Service = (PLDRP_SYSTEM_SERVICE)__arm64ec_syscall_ffs[Guest->X8];
    Stack = (PULONG_PTR)(Guest->Sp + 0x28);
    return Service(Guest->X4, Guest->X1, Guest->X2, Guest->X3,
                   Stack[0], Stack[1], Stack[2], Stack[3], Stack[4], Stack[5],
                   Stack[6], Stack[7], Stack[8], Stack[9], Stack[10], Stack[11]);
}

/**
 * @brief
 * Called by KiUserExceptionDispatcher when the emulator raises an exception for x64 code.
 */
DECLSPEC_NORETURN
VOID
NTAPI
LdrpDispatchEmulatorException(
    _In_ PARM64_NT_CONTEXT Context,
    _In_ PEXCEPTION_RECORD ExceptionRecord)
{
    PCHPE_V2_CPU_AREA_INFO CpuArea = NtCurrentTeb()->ChpeV2CpuAreaInfo;
    PARM64EC_NT_CONTEXT Guest = CpuArea->ContextAmd64;

    LdrpUnpackX64Context(Context, Guest);

    if (ExceptionRecord->ExceptionCode == STATUS_EMULATION_SYSCALL)
    {
        Guest->X8 = LdrpEmulateSystemCall(Guest);
        LdrpBeginSimulation();
    }

    /* FIXME: Dispatch exceptions to x64 handlers */
    DPRINT1("LDR: Exception 0x%08lx in x64 code at %p is not dispatched\n",
            ExceptionRecord->ExceptionCode, (PVOID)Guest->Pc);
    NtTerminateProcess(NtCurrentProcess(), ExceptionRecord->ExceptionCode);
    for (;;);
}

/**
 * @brief
 * Starts a memory call of the current process that the emulator hears about.
 *
 * @return
 * The CPU area of the thread, or NULL when the call is not reported.
 */
static
PCHPE_V2_CPU_AREA_INFO
LdrpBeginNotifiedMemoryCall(
    _In_ HANDLE ProcessHandle,
    _In_opt_ PVOID Notify)
{
    if (ProcessHandle != NtCurrentProcess())
        return NULL;

    return LdrpEnterEmulatorCallback(Notify);
}

/**
 * @brief
 * NtAllocateVirtualMemory of the ARM64EC view, the emulator tracks executable memory through it.
 */
NTSTATUS
NTAPI
LdrpEcAllocateVirtualMemory(
    _In_ HANDLE ProcessHandle,
    _Inout_ PVOID *BaseAddress,
    _In_ ULONG_PTR ZeroBits,
    _Inout_ PSIZE_T RegionSize,
    _In_ ULONG AllocationType,
    _In_ ULONG Protect)
{
    PCHPE_V2_CPU_AREA_INFO CpuArea = LdrpBeginNotifiedMemoryCall(ProcessHandle, (PVOID)LdrpEmulatorNotifyMemoryAlloc);
    NTSTATUS Status;

    if (!CpuArea)
        return NtAllocateVirtualMemory(ProcessHandle, BaseAddress, ZeroBits, RegionSize, AllocationType, Protect);

    LdrpEmulatorNotifyMemoryAlloc(*BaseAddress, *RegionSize, AllocationType, Protect, FALSE, STATUS_SUCCESS);
    Status = NtAllocateVirtualMemory(ProcessHandle, BaseAddress, ZeroBits, RegionSize, AllocationType, Protect);
    LdrpEmulatorNotifyMemoryAlloc(*BaseAddress, *RegionSize, AllocationType, Protect, TRUE, Status);

    CpuArea->InSyscallCallback = FALSE;
    return Status;
}

/**
 * @brief
 * NtFreeVirtualMemory of the ARM64EC view, the emulator drops code it translated from freed memory.
 */
NTSTATUS
NTAPI
LdrpEcFreeVirtualMemory(
    _In_ HANDLE ProcessHandle,
    _Inout_ PVOID *BaseAddress,
    _Inout_ PSIZE_T RegionSize,
    _In_ ULONG FreeType)
{
    PCHPE_V2_CPU_AREA_INFO CpuArea = LdrpBeginNotifiedMemoryCall(ProcessHandle, (PVOID)LdrpEmulatorNotifyMemoryFree);
    NTSTATUS Status;

    if (!CpuArea)
        return NtFreeVirtualMemory(ProcessHandle, BaseAddress, RegionSize, FreeType);

    LdrpEmulatorNotifyMemoryFree(*BaseAddress, *RegionSize, FreeType, FALSE, STATUS_SUCCESS);
    Status = NtFreeVirtualMemory(ProcessHandle, BaseAddress, RegionSize, FreeType);
    LdrpEmulatorNotifyMemoryFree(*BaseAddress, *RegionSize, FreeType, TRUE, Status);

    CpuArea->InSyscallCallback = FALSE;
    return Status;
}

/**
 * @brief
 * NtProtectVirtualMemory of the ARM64EC view, the emulator follows memory becoming executable or writable.
 */
NTSTATUS
NTAPI
LdrpEcProtectVirtualMemory(
    _In_ HANDLE ProcessHandle,
    _Inout_ PVOID *BaseAddress,
    _Inout_ PSIZE_T NumberOfBytesToProtect,
    _In_ ULONG NewAccessProtection,
    _Out_ PULONG OldAccessProtection)
{
    PCHPE_V2_CPU_AREA_INFO CpuArea = LdrpBeginNotifiedMemoryCall(ProcessHandle, (PVOID)LdrpEmulatorNotifyMemoryProtect);
    NTSTATUS Status;

    if (!CpuArea)
        return NtProtectVirtualMemory(ProcessHandle, BaseAddress, NumberOfBytesToProtect, NewAccessProtection, OldAccessProtection);

    LdrpEmulatorNotifyMemoryProtect(*BaseAddress, *NumberOfBytesToProtect, NewAccessProtection, FALSE, STATUS_SUCCESS);
    Status = NtProtectVirtualMemory(ProcessHandle, BaseAddress, NumberOfBytesToProtect, NewAccessProtection, OldAccessProtection);
    LdrpEmulatorNotifyMemoryProtect(*BaseAddress, *NumberOfBytesToProtect, NewAccessProtection, TRUE, Status);

    CpuArea->InSyscallCallback = FALSE;
    return Status;
}

/**
 * @brief
 * Runs the emulator notifications other processes queued for this one.
 */
NTSTATUS
NTAPI
ProcessPendingCrossProcessEmulatorWork(VOID)
{
    /* Nothing queues emulator work across processes yet */
    return STATUS_SUCCESS;
}

#endif /* _M_ARM64EC */

#endif /* _M_ARM64 || _M_ARM64EC */

/* Process start up only runs in the ARM64 half */
#ifdef _M_ARM64

/**
 * @brief
 * Picks the emulator, the default value of the Wow64\amd64 key overrides xtajit64.dll.
 */
static
VOID
LdrpGetEmulatorName(
    _Out_ PUNICODE_STRING DllName)
{
    static UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + MAX_PATH * sizeof(WCHAR)];
    PKEY_VALUE_PARTIAL_INFORMATION Value = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    UNICODE_STRING ValueName = RTL_CONSTANT_STRING(L"");
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE KeyHandle;
    ULONG ResultLength;
    NTSTATUS Status;

    *DllName = LdrpDefaultEmulator;

    InitializeObjectAttributes(&ObjectAttributes, &LdrpEmulatorKey, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(NtOpenKey(&KeyHandle, KEY_QUERY_VALUE, &ObjectAttributes)))
        return;

    Status = NtQueryValueKey(KeyHandle,
                             &ValueName,
                             KeyValuePartialInformation,
                             Value,
                             sizeof(Buffer) - sizeof(WCHAR),
                             &ResultLength);
    NtClose(KeyHandle);

    if (NT_SUCCESS(Status) &&
        (Value->Type == REG_SZ || Value->Type == REG_EXPAND_SZ) &&
        Value->DataLength > sizeof(WCHAR))
    {
        ((PWCHAR)Value->Data)[Value->DataLength / sizeof(WCHAR)] = UNICODE_NULL;
        RtlInitUnicodeString(DllName, (PCWSTR)Value->Data);
    }
}

/**
 * @brief
 * Tells whether the emulator is a plain ARM64 image, which has to bind to the ARM64 view of ntdll.
 *
 * @param[in] DllName
 * A file name in System32 or a full path.
 */
static
BOOLEAN
LdrpIsNativeEmulator(
    _In_ PUNICODE_STRING DllName)
{
    SECTION_IMAGE_INFORMATION ImageInfo;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    UNICODE_STRING Path;
    WCHAR Buffer[MAX_PATH];
    HANDLE File, Section;
    BOOLEAN Native = FALSE;
    NTSTATUS Status;

    if (wcschr(DllName->Buffer, L'\\'))
    {
        if (!RtlDosPathNameToNtPathName_U(DllName->Buffer, &Path, NULL, NULL))
            return FALSE;
    }
    else
    {
        RtlInitEmptyUnicodeString(&Path, Buffer, sizeof(Buffer));
        RtlAppendUnicodeToString(&Path, L"\\SystemRoot\\System32\\");
        RtlAppendUnicodeStringToString(&Path, DllName);
    }

    InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&File,
                        SYNCHRONIZE | FILE_EXECUTE | FILE_READ_DATA,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (Path.Buffer != Buffer)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Path.Buffer);
    if (!NT_SUCCESS(Status))
        return FALSE;

    Status = NtCreateSection(&Section,
                             SECTION_QUERY | SECTION_MAP_READ | SECTION_MAP_EXECUTE,
                             NULL,
                             NULL,
                             PAGE_EXECUTE,
                             SEC_IMAGE,
                             File);
    NtClose(File);
    if (!NT_SUCCESS(Status))
        return FALSE;

    if (NT_SUCCESS(NtQuerySection(Section, SectionImageInformation, &ImageInfo, sizeof(ImageInfo), NULL)))
        Native = (ImageInfo.Machine == IMAGE_FILE_MACHINE_ARM64);
    NtClose(Section);

    return Native;
}

/**
 * @brief
 * Loads the emulator and finds its entry points.
 */
static
NTSTATUS
LdrpLoadEmulator(
    _In_ PUNICODE_STRING DllName)
{
    NTSTATUS Status;

    Status = LdrLoadDll(NULL, NULL, DllName, &LdrpEmulatorBase);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("LDR: Unable to load the x64 emulator %wZ, Status=0x%08lx\n", DllName, Status);
        return Status;
    }

#define GET_EMULATOR_EXPORT(Name, Target)                                                      \
    {                                                                                          \
        ANSI_STRING ExportName = RTL_CONSTANT_STRING(Name);                                    \
        Status = LdrGetProcedureAddress(LdrpEmulatorBase, &ExportName, 0, (PVOID *)&(Target)); \
        if (!NT_SUCCESS(Status))                                                               \
        {                                                                                      \
            DPRINT1("LDR: The x64 emulator has no %s\n", Name);                                \
            return Status;                                                                     \
        }                                                                                      \
    }

    GET_EMULATOR_EXPORT("ExitToX64", LdrpExitToX64);
    GET_EMULATOR_EXPORT("RetToEntryThunk", LdrpRetToEntryThunk);
    GET_EMULATOR_EXPORT("DispatchJump", LdrpDispatchJump);
    GET_EMULATOR_EXPORT("BeginSimulation", LdrpBeginSimulation);
    GET_EMULATOR_EXPORT("ProcessInit", LdrpEmulatorProcessInit);
    GET_EMULATOR_EXPORT("ThreadInit", LdrpEmulatorThreadInit);

#undef GET_EMULATOR_EXPORT

    /* The exception hook and the image notifications are optional, the interpreter has none of them */
    {
        ANSI_STRING ResetName = RTL_CONSTANT_STRING("ResetToConsistentState");
        ANSI_STRING MapName = RTL_CONSTANT_STRING("NotifyMapViewOfSection");
        ANSI_STRING UnmapName = RTL_CONSTANT_STRING("NotifyUnmapViewOfSection");

        if (!NT_SUCCESS(LdrGetProcedureAddress(LdrpEmulatorBase, &ResetName, 0, (PVOID *)&LdrpEmulatorResetToConsistentState)))
            LdrpEmulatorResetToConsistentState = NULL;

        if (!NT_SUCCESS(LdrGetProcedureAddress(LdrpEmulatorBase, &MapName, 0, (PVOID *)&LdrpEmulatorNotifyMapViewOfSection)) ||
            !NT_SUCCESS(LdrGetProcedureAddress(LdrpEmulatorBase, &UnmapName, 0, (PVOID *)&LdrpEmulatorNotifyUnmapViewOfSection)))
        {
            LdrpEmulatorNotifyMapViewOfSection = NULL;
            LdrpEmulatorNotifyUnmapViewOfSection = NULL;
        }
    }

    /* Function exports of an ARM64EC emulator lead to their x64 entries, native code calls the ARM64EC code */
    LdrpEmulatorProcessInit = LdrpResolveFastForward(LdrpEmulatorProcessInit);
    LdrpEmulatorThreadInit = LdrpResolveFastForward(LdrpEmulatorThreadInit);
    if (LdrpEmulatorResetToConsistentState)
        LdrpEmulatorResetToConsistentState = LdrpResolveFastForward(LdrpEmulatorResetToConsistentState);
    if (LdrpEmulatorNotifyMapViewOfSection)
    {
        LdrpEmulatorNotifyMapViewOfSection = LdrpResolveFastForward(LdrpEmulatorNotifyMapViewOfSection);
        LdrpEmulatorNotifyUnmapViewOfSection = LdrpResolveFastForward(LdrpEmulatorNotifyUnmapViewOfSection);
    }

    /* The memory notifications come as a set too */
    {
        ANSI_STRING AllocName = RTL_CONSTANT_STRING("NotifyMemoryAlloc");
        ANSI_STRING FreeName = RTL_CONSTANT_STRING("NotifyMemoryFree");
        ANSI_STRING ProtectName = RTL_CONSTANT_STRING("NotifyMemoryProtect");

        if (NT_SUCCESS(LdrGetProcedureAddress(LdrpEmulatorBase, &AllocName, 0, (PVOID *)&LdrpEmulatorNotifyMemoryAlloc)) &&
            NT_SUCCESS(LdrGetProcedureAddress(LdrpEmulatorBase, &FreeName, 0, (PVOID *)&LdrpEmulatorNotifyMemoryFree)) &&
            NT_SUCCESS(LdrGetProcedureAddress(LdrpEmulatorBase, &ProtectName, 0, (PVOID *)&LdrpEmulatorNotifyMemoryProtect)))
        {
            LdrpEmulatorNotifyMemoryAlloc = LdrpResolveFastForward(LdrpEmulatorNotifyMemoryAlloc);
            LdrpEmulatorNotifyMemoryFree = LdrpResolveFastForward(LdrpEmulatorNotifyMemoryFree);
            LdrpEmulatorNotifyMemoryProtect = LdrpResolveFastForward(LdrpEmulatorNotifyMemoryProtect);
        }
        else
        {
            LdrpEmulatorNotifyMemoryAlloc = NULL;
            LdrpEmulatorNotifyMemoryFree = NULL;
            LdrpEmulatorNotifyMemoryProtect = NULL;
        }
    }

    return STATUS_SUCCESS;
}

/* ldrecasm.s */
ULONG_PTR
LdrpCallX64(
    _In_ PVOID ExitToX64,
    _In_ PVOID Target,
    _In_ ULONG_PTR Arg1,
    _In_ ULONG_PTR Arg2,
    _In_ ULONG_PTR Arg3);

/**
 * @brief
 * Runs an x64 DLL entry point or TLS callback in the emulator.
 */
ULONG_PTR
NTAPI
LdrpCallX64Routine(
    _In_ PVOID Routine,
    _In_ PVOID BaseAddress,
    _In_ ULONG Reason,
    _In_opt_ PVOID Context)
{
    return LdrpCallX64(LdrpExitToX64, Routine, (ULONG_PTR)BaseAddress, Reason, (ULONG_PTR)Context);
}

/**
 * @brief
 * Gives the current thread the CHPE CPU area the emulator keeps its state in.
 */
NTSTATUS
NTAPI
LdrpInitializeX64Thread(VOID)
{
    PCHPE_V2_CPU_AREA_INFO CpuArea;

    CpuArea = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*CpuArea));
    if (!CpuArea)
        return STATUS_NO_MEMORY;

    /* The x64 CONTEXT wants 16 byte alignment, the process heap gives it */
    CpuArea->ContextAmd64 = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ARM64EC_NT_CONTEXT));
    if (!CpuArea->ContextAmd64)
        return STATUS_NO_MEMORY;

    NtCurrentTeb()->ChpeV2CpuAreaInfo = CpuArea;

    return LdrpEmulatorThreadInit();
}

/**
 * @brief
 * Sets up a process whose main image is x64: the emulator, the thread state and the ARM64EC view of ntdll.
 */
NTSTATUS
NTAPI
LdrpInitializeX64Process(VOID)
{
    SYSTEM_BASIC_INFORMATION BasicInfo;
    WCHAR ObjectDirectory[64];
    UNICODE_STRING DllName;
    PVOID Bitmap = NULL;
    BOOLEAN Native;
    SIZE_T Size;
    NTSTATUS Status;

    /* CSR maps its shared section at the address it has in CSRSS, top down, so it goes in before the emulator */
    if (NtCurrentPeb()->SessionId)
        _snwprintf(ObjectDirectory, RTL_NUMBER_OF(ObjectDirectory), L"\\Sessions\\%lu\\Windows", NtCurrentPeb()->SessionId);
    else
        wcscpy(ObjectDirectory, L"\\Windows");
    Status = CsrpConnectToServer(ObjectDirectory);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("LDR: Unable to connect to CSR, Status=0x%08lx\n", Status);
        return Status;
    }

    /* One bit for each page of the user address space, kept high and out of the way of fixed shared views */
    Status = NtQuerySystemInformation(SystemBasicInformation, &BasicInfo, sizeof(BasicInfo), NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    Size = (BasicInfo.MaximumUserModeAddress >> PAGE_SHIFT) / 8 + 1;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Bitmap, 0, &Size, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("LDR: Unable to allocate the ARM64EC code bitmap, Status=0x%08lx\n", Status);
        return Status;
    }
    NtCurrentPeb()->EcCodeBitMap = Bitmap;

    /* x64 code the loader runs returns to LdrpCallX64, see ldrecasm.s */
    LdrpSetEcCodeBits((ULONG_PTR)LdrpCallX64, sizeof(ULONG), TRUE);

    /* A native emulator binds to the ARM64 view of ntdll, an ARM64EC one like FEX to the ARM64EC view */
    LdrpGetEmulatorName(&DllName);
    Native = LdrpIsNativeEmulator(&DllName);
    if (Native)
    {
        Status = LdrpLoadEmulator(&DllName);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* From here on x64 and ARM64EC code import the ARM64EC view of every ARM64X image */
    LdrpX64Process = TRUE;
    LdrpApplyArm64XView(LdrpNtDllDataTableEntry->DllBase);
    LdrpUpdateHybridMetadata(LdrpNtDllDataTableEntry->DllBase);

    if (!Native)
    {
        Status = LdrpLoadEmulator(&DllName);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* Images mapped before the emulator entry points were known get them now */
    LdrpUpdateHybridMetadata(LdrpNtDllDataTableEntry->DllBase);
    LdrpUpdateHybridMetadata(LdrpEmulatorBase);

    Status = LdrpEmulatorProcessInit();
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("LDR: x64 emulator process init failed, Status=0x%08lx\n", Status);
        return Status;
    }

    Status = LdrpInitializeX64Thread();
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("LDR: x64 emulator thread init failed, Status=0x%08lx\n", Status);
        return Status;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Starts the x64 main image in the emulator. The initial native context only provides the stack.
 */
DECLSPEC_NORETURN
VOID
NTAPI
LdrpStartX64Process(
    _In_ PCONTEXT Context)
{
    PCHPE_V2_CPU_AREA_INFO CpuArea = NtCurrentTeb()->ChpeV2CpuAreaInfo;
    PARM64EC_NT_CONTEXT Guest = CpuArea->ContextAmd64;
    PPEB Peb = NtCurrentPeb();
    PIMAGE_NT_HEADERS NtHeader = RtlImageNtHeader(Peb->ImageBaseAddress);
    ULONG64 Stack;

    /* An x64 call left the return address on an aligned stack, the entry point returning ends the process */
    Stack = (Context->Sp & ~15ULL) - 8;
    *(PULONG64)Stack = 0;

    /* The emulator thread init set up segments, flags and the FPU, the entry point gets the PEB in rcx */
    Guest->Sp = Stack;
    Guest->X0 = (ULONG64)Peb;
    Guest->Pc = (ULONG64)Peb->ImageBaseAddress + NtHeader->OptionalHeader.AddressOfEntryPoint;

    DPRINT1("LDR: Starting x64 image %wZ at %p\n", &LdrpImageEntry->BaseDllName, (PVOID)Guest->Pc);
    LdrpBeginSimulation();

    /* BeginSimulation does not come back */
    NtTerminateProcess(NtCurrentProcess(), STATUS_INTERNAL_ERROR);
    for (;;);
}

#endif /* _M_ARM64 */

/* EOF */
