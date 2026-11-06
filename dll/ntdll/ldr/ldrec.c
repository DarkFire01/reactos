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
    PLDR_DATA_TABLE_ENTRY LdrEntry;
    PLIST_ENTRY ListHead, Entry;
    ULONG_PTR Base;

    if (!Ldr)
        return FALSE;

    /* FIXME: Use a code bitmap, this does not see code outside of images */
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
 * Tells whether an address inside an image is ARM64EC code, as opposed to ARM64 or x64 code.
 */
BOOLEAN
NTAPI
LdrpIsArm64EcCode(
    _In_ PVOID ImageBase,
    _In_ PVOID Address)
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
            return (Range[i].StartOffset & 3) == IMAGE_CHPE_RANGE_ARM64EC;
    }

    return FALSE;
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
 * Points the emulator dispatch slots of an ARM64EC image at the emulator and the loader call check.
 */
VOID
NTAPI
LdrpUpdateHybridMetadata(
    _In_ PVOID ImageBase)
{
    PIMAGE_ARM64EC_METADATA Metadata = LdrpGetArm64EcMetadata(ImageBase);
    PUCHAR Base = ImageBase;

    if (!Metadata || !LdrpEmulatorBase)
        return;

    if (Metadata->__os_arm64x_dispatch_call_no_redirect)
        LdrpWriteImagePointer(Base + Metadata->__os_arm64x_dispatch_call_no_redirect, LdrpExitToX64);
    if (Metadata->__os_arm64x_dispatch_ret)
        LdrpWriteImagePointer(Base + Metadata->__os_arm64x_dispatch_ret, LdrpRetToEntryThunk);
    if (Metadata->__os_arm64x_dispatch_fptr)
        LdrpWriteImagePointer(Base + Metadata->__os_arm64x_dispatch_fptr, LdrpDispatchJump);
    if (Metadata->__os_arm64x_dispatch_call)
        LdrpWriteImagePointer(Base + Metadata->__os_arm64x_dispatch_call, (PVOID)LdrpEcCallCheck);
    if (Metadata->__os_arm64x_dispatch_icall)
        LdrpWriteImagePointer(Base + Metadata->__os_arm64x_dispatch_icall, (PVOID)LdrpEcCallCheck);
    if (Metadata->__os_arm64x_dispatch_icall_cfg)
        LdrpWriteImagePointer(Base + Metadata->__os_arm64x_dispatch_icall_cfg, (PVOID)LdrpEcCallCheck);
}

#endif /* _M_ARM64 || _M_ARM64EC */

/* Process start up only runs in the ARM64 half */
#ifdef _M_ARM64

/**
 * @brief
 * Picks the emulator, the default value of the Wow64\amd64 key overrides xtajit64.dll.
 */
static
NTSTATUS
LdrpLoadEmulator(VOID)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING ValueName = RTL_CONSTANT_STRING(L"");
    UNICODE_STRING DllName = LdrpDefaultEmulator;
    UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + MAX_PATH * sizeof(WCHAR)];
    PKEY_VALUE_PARTIAL_INFORMATION Value = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    HANDLE KeyHandle;
    ULONG ResultLength;
    NTSTATUS Status;

    InitializeObjectAttributes(&ObjectAttributes, &LdrpEmulatorKey, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(NtOpenKey(&KeyHandle, KEY_QUERY_VALUE, &ObjectAttributes)))
    {
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
            RtlInitUnicodeString(&DllName, (PCWSTR)Value->Data);
        }
    }

    Status = LdrLoadDll(NULL, NULL, &DllName, &LdrpEmulatorBase);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("LDR: Unable to load the x64 emulator %wZ, Status=0x%08lx\n", &DllName, Status);
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

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Gives the current thread the CHPE CPU area the emulator keeps its state in.
 */
static
NTSTATUS
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

    /* FIXME: TEB->ChpeV2CpuAreaInfo once the TEB has it, ARM64 has no VDM */
    NtCurrentTeb()->Vdm = CpuArea;

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
    NTSTATUS Status;

    /* The emulator is native code, it binds to the ARM64 view of ntdll before that changes */
    Status = LdrpLoadEmulator();
    if (!NT_SUCCESS(Status))
        return Status;

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

    /* From here on x64 and ARM64EC code import the ARM64EC view of every ARM64X image */
    LdrpX64Process = TRUE;
    LdrpApplyArm64XView(LdrpNtDllDataTableEntry->DllBase);
    LdrpUpdateHybridMetadata(LdrpNtDllDataTableEntry->DllBase);

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
    PCHPE_V2_CPU_AREA_INFO CpuArea = NtCurrentTeb()->Vdm;
    PARM64EC_NT_CONTEXT Guest = CpuArea->ContextAmd64;
    PPEB Peb = NtCurrentPeb();
    PIMAGE_NT_HEADERS NtHeader = RtlImageNtHeader(Peb->ImageBaseAddress);
    ULONG64 Stack;

    /* An x64 call left the return address on an aligned stack, the entry point returning ends the process */
    Stack = (Context->Sp & ~15ULL) - 8;
    *(PULONG64)Stack = 0;

    RtlZeroMemory(Guest, sizeof(*Guest));
    /* x64 CONTEXT_ALL */
    Guest->ContextFlags = 0x0010001F;
    Guest->Sp = Stack;
    Guest->X0 = (ULONG64)Peb;
    Guest->Pc = (ULONG64)Peb->ImageBaseAddress + NtHeader->OptionalHeader.AddressOfEntryPoint;
    Guest->AMD64_EFlags = 0x202;
    Guest->AMD64_SegCs = 0x33;
    Guest->AMD64_SegSs = 0x2B;
    Guest->AMD64_MxCsr = 0x1F80;
    Guest->AMD64_MxCsr_copy = 0x1F80;

    DPRINT1("LDR: Starting x64 image %wZ at %p\n", &LdrpImageEntry->BaseDllName, (PVOID)Guest->Pc);
    LdrpBeginSimulation();

    /* BeginSimulation does not come back */
    NtTerminateProcess(NtCurrentProcess(), STATUS_INTERNAL_ERROR);
    for (;;);
}

#endif /* _M_ARM64 */

/* EOF */
