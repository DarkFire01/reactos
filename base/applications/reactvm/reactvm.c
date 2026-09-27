/*
 * PROJECT:     ReactOS Virtual Machine Monitor
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     A machine built out of the hypervisor platform, and what it proves
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * This is a virtual machine monitor of our own rather than a test of one call
 * at a time: it makes a machine, puts a program in it, runs it, answers what
 * the guest asks for, and says afterwards whether the guest saw what it should
 * have. Every device the guest can reach is here, because a hypervisor has
 * none.
 *
 * The programs are sixteen bit, hand assembled, and listed one instruction per
 * line so that what the guest executes can be read. They are deliberately
 * small: what is being proved is the machinery under them.
 */

/* INCLUDES *******************************************************************/

#include <windows.h>
#include <winhvplatform.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* GLOBALS ********************************************************************/

/*
 * The platform is bound at run time, so this runs on a system without one and
 * says so rather than failing to start.
 */

typedef HRESULT (WINAPI *PFN_WHV_GET_CAPABILITY)(WHV_CAPABILITY_CODE, VOID *, UINT32, UINT32 *);
typedef HRESULT (WINAPI *PFN_WHV_CREATE_PARTITION)(WHV_PARTITION_HANDLE *);
typedef HRESULT (WINAPI *PFN_WHV_DELETE_PARTITION)(WHV_PARTITION_HANDLE);
typedef HRESULT (WINAPI *PFN_WHV_SETUP_PARTITION)(WHV_PARTITION_HANDLE);
typedef HRESULT (WINAPI *PFN_WHV_SET_PARTITION_PROPERTY)(WHV_PARTITION_HANDLE, WHV_PARTITION_PROPERTY_CODE, const VOID *, UINT32);
typedef HRESULT (WINAPI *PFN_WHV_MAP_GPA_RANGE)(WHV_PARTITION_HANDLE, VOID *, WHV_GUEST_PHYSICAL_ADDRESS, UINT64, WHV_MAP_GPA_RANGE_FLAGS);
typedef HRESULT (WINAPI *PFN_WHV_CREATE_VP)(WHV_PARTITION_HANDLE, UINT32, UINT32);
typedef HRESULT (WINAPI *PFN_WHV_DELETE_VP)(WHV_PARTITION_HANDLE, UINT32);
typedef HRESULT (WINAPI *PFN_WHV_RUN_VP)(WHV_PARTITION_HANDLE, UINT32, VOID *, UINT32);
typedef HRESULT (WINAPI *PFN_WHV_SET_VP_REGISTERS)(WHV_PARTITION_HANDLE, UINT32, const WHV_REGISTER_NAME *, UINT32, const WHV_REGISTER_VALUE *);
typedef HRESULT (WINAPI *PFN_WHV_GET_VP_REGISTERS)(WHV_PARTITION_HANDLE, UINT32, const WHV_REGISTER_NAME *, UINT32, WHV_REGISTER_VALUE *);

static PFN_WHV_GET_CAPABILITY VmGetCapability;
static PFN_WHV_CREATE_PARTITION VmCreatePartition;
static PFN_WHV_DELETE_PARTITION VmDeletePartition;
static PFN_WHV_SETUP_PARTITION VmSetupPartition;
static PFN_WHV_SET_PARTITION_PROPERTY VmSetPartitionProperty;
static PFN_WHV_MAP_GPA_RANGE VmMapGpaRange;
static PFN_WHV_CREATE_VP VmCreateVirtualProcessor;
static PFN_WHV_DELETE_VP VmDeleteVirtualProcessor;
static PFN_WHV_RUN_VP VmRunVirtualProcessor;
static PFN_WHV_SET_VP_REGISTERS VmSetRegisters;
static PFN_WHV_GET_VP_REGISTERS VmGetRegisters;

/* The machine: a megabyte of memory, a console port, and nothing else yet */
#define VM_RAM_SIZE         0x100000
#define VM_CODE_BASE        0x1000
#define VM_RESULT_BASE      0x2000
#define VM_CONSOLE_PORT     0xE9

/* How many exits one program may take before it is called a runaway */
#define VM_EXIT_LIMIT       4096

/* What the guest is expected to find out, per the interface it asks through */
#define VM_HV_LEAF_VENDOR   0x40000000
#define VM_HV_LEAF_INTERFACE 0x40000001
#define VM_HV_LEAF_SPARE    0x40000005
#define VM_HV_VENDOR_EBX    0x7263694D  /* 'rciM' */
#define VM_HV_VENDOR_ECX    0x666F736F  /* 'foso' */
#define VM_HV_VENDOR_EDX    0x76482074  /* 'vH t' */
#define VM_HV_INTERFACE     0x31237648  /* '1#vH' */
#define VM_CPUID_HYPERVISOR 0x80000000

/* What this monitor answers a guest with, chosen to be nothing a processor has */
#define VM_TEST_MSR         0x4B4F5201
#define VM_TEST_MSR_LOW     0xFEEDFACE
#define VM_TEST_MSR_HIGH    0x0BADC0DE
#define VM_TEST_CPUID_EAX   0x11111111
#define VM_TEST_CPUID_EBX   0x22222222
#define VM_TEST_CPUID_ECX   0x33333333
#define VM_TEST_CPUID_EDX   0x44444444

/*
 * One virtual machine: the partition, the memory behind it, and what the
 * monitor learned while it ran.
 */
typedef struct _VM
{
    WHV_PARTITION_HANDLE Partition;
    PUCHAR Ram;
    ULONG Exits;
    /* What the guest asked for and this monitor answered */
    ULONG CpuidExits;
    ULONG MsrReads;
    ULONG MsrWrites;
    ULONG MsrNumber;
    ULONG64 MsrValue;
    /* Whatever the guest wrote to the console port, in order */
    CHAR Console[256];
    ULONG ConsoleLength;
} VM, *PVM;

/*
 * mov eax, <leaf> / cpuid / store all four registers at 2000h / halt. The leaf
 * is patched in, so one program serves every question worth asking.
 */
#define VM_CPUID_PROGRAM_LEAF 2

static const UCHAR VmCpuidProgram[] =
{
    0x66, 0xB8, 0x00, 0x00, 0x00, 0x00,     /* mov eax, <leaf>          */
    0x0F, 0xA2,                             /* cpuid                    */
    0x66, 0xA3, 0x00, 0x20,                 /* mov [2000h], eax         */
    0x66, 0x89, 0x1E, 0x04, 0x20,           /* mov [2004h], ebx         */
    0x66, 0x89, 0x0E, 0x08, 0x20,           /* mov [2008h], ecx         */
    0x66, 0x89, 0x16, 0x0C, 0x20,           /* mov [200Ch], edx         */
    0xF4                                    /* hlt                      */
};

/* mov ecx, <msr> / rdmsr / store eax and edx / halt */
#define VM_RDMSR_PROGRAM_MSR 2

static const UCHAR VmRdmsrProgram[] =
{
    0x66, 0xB9, 0x00, 0x00, 0x00, 0x00,     /* mov ecx, <msr>           */
    0x0F, 0x32,                             /* rdmsr                    */
    0x66, 0xA3, 0x00, 0x20,                 /* mov [2000h], eax         */
    0x66, 0x89, 0x16, 0x04, 0x20,           /* mov [2004h], edx         */
    0xF4                                    /* hlt                      */
};

/* mov ecx, <msr> / mov eax, <low> / mov edx, <high> / wrmsr / halt */
#define VM_WRMSR_PROGRAM_MSR  2
#define VM_WRMSR_PROGRAM_LOW  8
#define VM_WRMSR_PROGRAM_HIGH 14

static const UCHAR VmWrmsrProgram[] =
{
    0x66, 0xB9, 0x00, 0x00, 0x00, 0x00,     /* mov ecx, <msr>           */
    0x66, 0xB8, 0x00, 0x00, 0x00, 0x00,     /* mov eax, <low>           */
    0x66, 0xBA, 0x00, 0x00, 0x00, 0x00,     /* mov edx, <high>          */
    0x0F, 0x30,                             /* wrmsr                    */
    0xF4                                    /* hlt                      */
};

/*
 * mov si, 1030h / lodsb / stop at the zero / out to the console / halt. The
 * string sits just past the program.
 */
#define VM_PRINT_PROGRAM_TEXT 0x30

static const UCHAR VmPrintProgram[] =
{
    0xBE, 0x30, 0x10,                       /* mov si, 1030h            */
    0xAC,                                   /* lodsb                    */
    0x3C, 0x00,                             /* cmp al, 0                */
    0x74, 0x04,                             /* je +4                    */
    0xE6, 0xE9,                             /* out 0E9h, al             */
    0xEB, 0xF7,                             /* jmp lodsb                */
    0xF4                                    /* hlt                      */
};

/* FUNCTIONS ******************************************************************/

static
VOID
__cdecl
VmPrint(
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...)
{
    CHAR Line[512];
    va_list Arguments;

    va_start(Arguments, Format);
    _vsnprintf(Line, sizeof(Line) - 1, Format, Arguments);
    va_end(Arguments);

    Line[sizeof(Line) - 1] = ANSI_NULL;

    fputs(Line, stdout);
    OutputDebugStringA(Line);
}

/* How many checks were made and how many of them held */
static ULONG VmChecks;
static ULONG VmFailures;

static
VOID
VmCheck(
    _In_z_ PCSTR What,
    _In_ BOOLEAN Held)
{
    VmChecks++;

    if (!Held)
        VmFailures++;

    VmPrint("  %-44s %s\n", What, Held ? "ok" : "FAILED");
}

static
VOID
VmCheckValue(
    _In_z_ PCSTR What,
    _In_ ULONG64 Wanted,
    _In_ ULONG64 Got)
{
    VmChecks++;

    if (Wanted == Got)
    {
        VmPrint("  %-44s ok\n", What);
        return;
    }

    VmFailures++;
    VmPrint("  %-44s FAILED, wanted %I64x got %I64x\n", What, Wanted, Got);
}

static
BOOLEAN
VmBindOne(
    _In_ HMODULE Platform,
    _In_z_ PCSTR Name,
    _Out_ FARPROC *Routine)
{
    *Routine = GetProcAddress(Platform, Name);

    if (*Routine == NULL)
    {
        VmPrint("  %s is missing from the platform\n", Name);
        return FALSE;
    }

    return TRUE;
}

#define VM_BIND(Target, Name) \
    if (!VmBindOne(Platform, Name, (FARPROC *)&Target)) \
        return FALSE

static
BOOLEAN
VmBindPlatform(VOID)
{
    HMODULE Platform = LoadLibraryW(L"WinHvPlatform.dll");

    if (Platform == NULL)
    {
        VmPrint("  no WinHvPlatform.dll on this system\n");
        return FALSE;
    }

    VM_BIND(VmGetCapability, "WHvGetCapability");
    VM_BIND(VmCreatePartition, "WHvCreatePartition");
    VM_BIND(VmDeletePartition, "WHvDeletePartition");
    VM_BIND(VmSetupPartition, "WHvSetupPartition");
    VM_BIND(VmSetPartitionProperty, "WHvSetPartitionProperty");
    VM_BIND(VmMapGpaRange, "WHvMapGpaRange");
    VM_BIND(VmCreateVirtualProcessor, "WHvCreateVirtualProcessor");
    VM_BIND(VmDeleteVirtualProcessor, "WHvDeleteVirtualProcessor");
    VM_BIND(VmRunVirtualProcessor, "WHvRunVirtualProcessor");
    VM_BIND(VmSetRegisters, "WHvSetVirtualProcessorRegisters");
    VM_BIND(VmGetRegisters, "WHvGetVirtualProcessorRegisters");

    return TRUE;
}

/**
 * @brief
 * Loads one segment register with a segment that covers the first megabyte.
 */
static
HRESULT
VmSetRealModeSegment(
    _In_ PVM Vm,
    _In_ WHV_REGISTER_NAME Name,
    _In_ BOOLEAN Code)
{
    WHV_REGISTER_VALUE Value;

    ZeroMemory(&Value, sizeof(Value));

    Value.Segment.Base = 0;
    Value.Segment.Limit = 0xFFFF;
    Value.Segment.Selector = 0;
    Value.Segment.SegmentType = Code ? 0xB : 0x3;
    Value.Segment.NonSystemSegment = 1;
    Value.Segment.Present = 1;

    return VmSetRegisters(Vm->Partition, 0, &Name, 1, &Value);
}

static
HRESULT
VmSetRegister(
    _In_ PVM Vm,
    _In_ WHV_REGISTER_NAME Name,
    _In_ ULONG64 Value)
{
    WHV_REGISTER_VALUE Register;

    ZeroMemory(&Register, sizeof(Register));
    Register.Reg64 = Value;

    return VmSetRegisters(Vm->Partition, 0, &Name, 1, &Register);
}

/**
 * @brief
 * Builds a machine with memory and one processor, in real mode, ready to run
 * from the address programs are loaded at.
 *
 * @param[in] CpuidLeaves
 * Which CPUID leaves this monitor wants to answer itself. The hypervisor
 * answers the rest, and the guest cannot tell which did.
 *
 * @param[in] MsrExits
 * Whether this monitor wants to answer the guest's model specific registers.
 */
static
BOOLEAN
VmCreate(
    _Out_ PVM Vm,
    _In_reads_opt_(LeafCount) const ULONG *CpuidLeaves,
    _In_ ULONG LeafCount,
    _In_ BOOLEAN MsrExits)
{
    WHV_EXTENDED_VM_EXITS Extended;
    UINT32 ProcessorCount = 1;
    HRESULT Result;

    ZeroMemory(Vm, sizeof(*Vm));

    Result = VmCreatePartition(&Vm->Partition);
    if (FAILED(Result))
    {
        VmPrint("  could not create a partition, %08lx\n", (ULONG)Result);
        return FALSE;
    }

    Result = VmSetPartitionProperty(Vm->Partition,
                                    WHvPartitionPropertyCodeProcessorCount,
                                    &ProcessorCount,
                                    sizeof(ProcessorCount));
    if (FAILED(Result))
    {
        VmPrint("  could not ask for one processor, %08lx\n", (ULONG)Result);
        return FALSE;
    }

    /*
     * Which kinds of exit this monitor wants at all. The list of CPUID leaves
     * is only looked at once the partition has been told that the monitor
     * wants CPUID exits in the first place.
     */
    Extended.AsUINT64 = 0;
    Extended.X64CpuidExit = (LeafCount != 0) ? 1 : 0;
    Extended.X64MsrExit = MsrExits ? 1 : 0;

    if (Extended.AsUINT64 != 0)
    {
        Result = VmSetPartitionProperty(Vm->Partition,
                                        WHvPartitionPropertyCodeExtendedVmExits,
                                        &Extended,
                                        sizeof(Extended));
        if (FAILED(Result))
        {
            VmPrint("  could not ask for the exits wanted, %08lx\n", (ULONG)Result);
            return FALSE;
        }
    }

    if (LeafCount != 0)
    {
        Result = VmSetPartitionProperty(Vm->Partition,
                                        WHvPartitionPropertyCodeCpuidExitList,
                                        CpuidLeaves,
                                        LeafCount * sizeof(*CpuidLeaves));
        if (FAILED(Result))
        {
            VmPrint("  could not ask about cpuid leaves, %08lx\n", (ULONG)Result);
            return FALSE;
        }
    }

    Result = VmSetupPartition(Vm->Partition);
    if (FAILED(Result))
    {
        VmPrint("  could not set the partition up, %08lx\n", (ULONG)Result);
        return FALSE;
    }

    Vm->Ram = VirtualAlloc(NULL,
                           VM_RAM_SIZE,
                           MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
    if (Vm->Ram == NULL)
    {
        VmPrint("  out of memory for the guest\n");
        return FALSE;
    }

    Result = VmMapGpaRange(Vm->Partition,
                           Vm->Ram,
                           0,
                           VM_RAM_SIZE,
                           WHvMapGpaRangeFlagRead |
                           WHvMapGpaRangeFlagWrite |
                           WHvMapGpaRangeFlagExecute);
    if (FAILED(Result))
    {
        VmPrint("  could not give the guest its memory, %08lx\n", (ULONG)Result);
        return FALSE;
    }

    Result = VmCreateVirtualProcessor(Vm->Partition, 0, 0);
    if (FAILED(Result))
    {
        VmPrint("  could not create a processor, %08lx\n", (ULONG)Result);
        return FALSE;
    }

    Result = VmSetRealModeSegment(Vm, WHvX64RegisterCs, TRUE);
    if (SUCCEEDED(Result))
        Result = VmSetRealModeSegment(Vm, WHvX64RegisterDs, FALSE);
    if (SUCCEEDED(Result))
        Result = VmSetRealModeSegment(Vm, WHvX64RegisterEs, FALSE);
    if (SUCCEEDED(Result))
        Result = VmSetRealModeSegment(Vm, WHvX64RegisterSs, FALSE);
    if (SUCCEEDED(Result))
        Result = VmSetRegister(Vm, WHvX64RegisterRflags, 2);
    if (SUCCEEDED(Result))
        Result = VmSetRegister(Vm, WHvX64RegisterRip, VM_CODE_BASE);

    if (FAILED(Result))
    {
        VmPrint("  could not put the processor in real mode, %08lx\n", (ULONG)Result);
        return FALSE;
    }

    return TRUE;
}

static
VOID
VmDestroy(
    _In_ PVM Vm)
{
    if (Vm->Partition != NULL)
    {
        VmDeleteVirtualProcessor(Vm->Partition, 0);
        VmDeletePartition(Vm->Partition);
        Vm->Partition = NULL;
    }

    if (Vm->Ram != NULL)
    {
        VirtualFree(Vm->Ram, 0, MEM_RELEASE);
        Vm->Ram = NULL;
    }
}

/**
 * @brief
 * Puts a program in the machine and the processor at the start of it.
 *
 * @remarks
 * The machine is reused from one program to the next, because building one
 * costs the root a great deal more than running one.
 */
static
BOOLEAN
VmLoad(
    _In_ PVM Vm,
    _In_reads_bytes_(Length) const UCHAR *Program,
    _In_ ULONG Length)
{
    HRESULT Result;

    ZeroMemory(Vm->Ram + VM_CODE_BASE, 0x1000);
    ZeroMemory(Vm->Ram + VM_RESULT_BASE, 0x1000);
    CopyMemory(Vm->Ram + VM_CODE_BASE, Program, Length);

    Vm->Exits = 0;
    Vm->CpuidExits = 0;
    Vm->MsrReads = 0;
    Vm->MsrWrites = 0;
    Vm->MsrNumber = 0;
    Vm->MsrValue = 0;
    Vm->ConsoleLength = 0;

    Result = VmSetRegister(Vm, WHvX64RegisterRip, VM_CODE_BASE);
    if (FAILED(Result))
    {
        VmPrint("  could not put the processor at the start, %08lx\n", (ULONG)Result);
        return FALSE;
    }

    return TRUE;
}

/* Patches a four byte immediate a program was assembled with room for */
static
VOID
VmPatch(
    _In_ PVM Vm,
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    Vm->Ram[VM_CODE_BASE + Offset + 0] = (UCHAR)Value;
    Vm->Ram[VM_CODE_BASE + Offset + 1] = (UCHAR)(Value >> 8);
    Vm->Ram[VM_CODE_BASE + Offset + 2] = (UCHAR)(Value >> 16);
    Vm->Ram[VM_CODE_BASE + Offset + 3] = (UCHAR)(Value >> 24);
}

static
ULONG
VmResult(
    _In_ PVM Vm,
    _In_ ULONG Index)
{
    const UCHAR *At = Vm->Ram + VM_RESULT_BASE + (Index * sizeof(ULONG));

    return (ULONG)At[0] | ((ULONG)At[1] << 8) | ((ULONG)At[2] << 16) |
           ((ULONG)At[3] << 24);
}

/* Puts the processor on the instruction after the one it stopped on */
static
HRESULT
VmStepOver(
    _In_ PVM Vm,
    _In_ const WHV_RUN_VP_EXIT_CONTEXT *Exit)
{
    if (Exit->VpContext.InstructionLength == 0)
    {
        VmPrint("  nothing said how long the instruction at %I64x is\n",
                Exit->VpContext.Rip);
        return E_FAIL;
    }

    return VmSetRegister(Vm,
                         WHvX64RegisterRip,
                         Exit->VpContext.Rip + Exit->VpContext.InstructionLength);
}

/**
 * @brief
 * Answers what the guest asked about a CPUID leaf this monitor took for
 * itself, which is what the guest then sees in place of the default.
 */
static
HRESULT
VmAnswerCpuid(
    _In_ PVM Vm,
    _In_ const WHV_RUN_VP_EXIT_CONTEXT *Exit)
{
    WHV_REGISTER_NAME Names[4];
    WHV_REGISTER_VALUE Values[4];
    HRESULT Result;

    Vm->CpuidExits++;

    Names[0] = WHvX64RegisterRax;
    Names[1] = WHvX64RegisterRbx;
    Names[2] = WHvX64RegisterRcx;
    Names[3] = WHvX64RegisterRdx;

    ZeroMemory(Values, sizeof(Values));
    Values[0].Reg64 = VM_TEST_CPUID_EAX;
    Values[1].Reg64 = VM_TEST_CPUID_EBX;
    Values[2].Reg64 = VM_TEST_CPUID_ECX;
    Values[3].Reg64 = VM_TEST_CPUID_EDX;

    Result = VmSetRegisters(Vm->Partition, 0, Names, 4, Values);
    if (FAILED(Result))
        return Result;

    return VmStepOver(Vm, Exit);
}

/**
 * @brief
 * Answers the guest's model specific register access, which on a machine with
 * no processor behind it is whatever this monitor decides it is.
 */
static
HRESULT
VmAnswerMsr(
    _In_ PVM Vm,
    _In_ const WHV_RUN_VP_EXIT_CONTEXT *Exit)
{
    WHV_REGISTER_NAME Names[2];
    WHV_REGISTER_VALUE Values[2];
    HRESULT Result;

    Vm->MsrNumber = Exit->MsrAccess.MsrNumber;

    if (Exit->MsrAccess.AccessInfo.IsWrite)
    {
        Vm->MsrWrites++;
        Vm->MsrValue = ((Exit->MsrAccess.Rdx & 0xFFFFFFFF) << 32) |
                       (Exit->MsrAccess.Rax & 0xFFFFFFFF);

        return VmStepOver(Vm, Exit);
    }

    Vm->MsrReads++;

    Names[0] = WHvX64RegisterRax;
    Names[1] = WHvX64RegisterRdx;

    ZeroMemory(Values, sizeof(Values));
    Values[0].Reg64 = VM_TEST_MSR_LOW;
    Values[1].Reg64 = VM_TEST_MSR_HIGH;

    Result = VmSetRegisters(Vm->Partition, 0, Names, 2, Values);
    if (FAILED(Result))
        return Result;

    return VmStepOver(Vm, Exit);
}

/**
 * @brief
 * Runs the guest until it halts, answering everything it asks for on the way.
 *
 * @return
 * FALSE when the guest stopped for something this monitor has no answer for,
 * which is reported as it happens.
 */
static
BOOLEAN
VmRun(
    _In_ PVM Vm)
{
    WHV_RUN_VP_EXIT_CONTEXT Exit;
    HRESULT Result;

    for (;;)
    {
        if (Vm->Exits >= VM_EXIT_LIMIT)
        {
            VmPrint("  the guest is not getting anywhere, %lu exits\n", Vm->Exits);
            return FALSE;
        }

        Result = VmRunVirtualProcessor(Vm->Partition, 0, &Exit, sizeof(Exit));
        if (FAILED(Result))
        {
            VmPrint("  the processor could not be run, %08lx\n", (ULONG)Result);
            return FALSE;
        }

        Vm->Exits++;

        switch (Exit.ExitReason)
        {
            case WHvRunVpExitReasonX64Halt:
                return TRUE;

            case WHvRunVpExitReasonX64IoPortAccess:
                if ((Exit.IoPortAccess.PortNumber == VM_CONSOLE_PORT) &&
                    (Vm->ConsoleLength < (sizeof(Vm->Console) - 1)))
                {
                    Vm->Console[Vm->ConsoleLength++] = (CHAR)Exit.IoPortAccess.Rax;
                }

                Result = VmStepOver(Vm, &Exit);
                break;

            case WHvRunVpExitReasonX64Cpuid:
                Result = VmAnswerCpuid(Vm, &Exit);
                break;

            case WHvRunVpExitReasonX64MsrAccess:
                Result = VmAnswerMsr(Vm, &Exit);
                break;

            default:
                VmPrint("  the guest stopped for %lu at %I64x, which is unanswered\n",
                        (ULONG)Exit.ExitReason,
                        Exit.VpContext.Rip);
                return FALSE;
        }

        if (FAILED(Result))
            return FALSE;
    }
}

/* THE MACHINE IN PIECES ******************************************************/

/* Loads the CPUID program, asks it about one leaf, and runs it */
static
BOOLEAN
VmAskCpuid(
    _In_ PVM Vm,
    _In_ ULONG Leaf)
{
    if (!VmLoad(Vm, VmCpuidProgram, sizeof(VmCpuidProgram)))
        return FALSE;

    VmPatch(Vm, VM_CPUID_PROGRAM_LEAF, Leaf);

    return VmRun(Vm);
}

/**
 * @brief
 * What a guest gets told when the monitor takes no interest in the question:
 * whatever the hypervisor says on its own, which is what it must get right
 * without being asked.
 */
static
VOID
VmStageWhatTheHypervisorSays(VOID)
{
    static const CHAR Text[] = "the machine is running";
    VM Vm;

    VmPrint("\nthe machine, with the monitor keeping out of it\n");

    if (!VmCreate(&Vm, NULL, 0, FALSE))
        goto Done;

    /* A line out of a port and a halt, which is every machine there has to be */
    if (!VmLoad(&Vm, VmPrintProgram, sizeof(VmPrintProgram)))
        goto Done;

    CopyMemory(Vm.Ram + VM_CODE_BASE + VM_PRINT_PROGRAM_TEXT, Text, sizeof(Text));

    if (!VmRun(&Vm))
        goto Done;

    Vm.Console[Vm.ConsoleLength] = ANSI_NULL;

    VmCheck("the guest wrote what it meant to write",
            strcmp(Vm.Console, Text) == 0);

    if (!VmAskCpuid(&Vm, VM_HV_LEAF_VENDOR))
        goto Done;

    VmCheckValue("the interface names itself", VM_HV_VENDOR_EBX, VmResult(&Vm, 1));
    VmCheckValue("and goes on naming itself", VM_HV_VENDOR_ECX, VmResult(&Vm, 2));
    VmCheckValue("and finishes naming itself", VM_HV_VENDOR_EDX, VmResult(&Vm, 3));
    VmCheck("it offers at least the interface leaf",
            VmResult(&Vm, 0) >= VM_HV_LEAF_INTERFACE);
    VmCheck("the monitor was not troubled with any of it", Vm.CpuidExits == 0);

    if (!VmAskCpuid(&Vm, VM_HV_LEAF_INTERFACE))
        goto Done;

    VmCheckValue("the interface signature", VM_HV_INTERFACE, VmResult(&Vm, 0));

    if (!VmAskCpuid(&Vm, 1))
        goto Done;

    VmCheck("the processor says a hypervisor is present",
            (VmResult(&Vm, 2) & VM_CPUID_HYPERVISOR) != 0);

Done:
    VmDestroy(&Vm);
}

/**
 * @brief
 * A leaf the monitor took for itself, which is the whole point of a monitor:
 * what the guest is told is not what the machine would have said. The leaves
 * it did not take have to keep working without it.
 */
static
VOID
VmStageWhatTheMonitorSays(VOID)
{
    ULONG Leaves[1];
    VM Vm;

    VmPrint("\nthe monitor answers one cpuid leaf and leaves the rest\n");

    Leaves[0] = VM_HV_LEAF_SPARE;

    if (!VmCreate(&Vm, Leaves, 1, FALSE))
        goto Done;

    if (!VmAskCpuid(&Vm, VM_HV_LEAF_SPARE))
        goto Done;

    VmCheckValue("the monitor was asked once", 1, Vm.CpuidExits);
    VmCheckValue("the guest saw what the monitor said in eax",
                 VM_TEST_CPUID_EAX, VmResult(&Vm, 0));
    VmCheckValue("and in ebx", VM_TEST_CPUID_EBX, VmResult(&Vm, 1));
    VmCheckValue("and in ecx", VM_TEST_CPUID_ECX, VmResult(&Vm, 2));
    VmCheckValue("and in edx", VM_TEST_CPUID_EDX, VmResult(&Vm, 3));

    if (!VmAskCpuid(&Vm, VM_HV_LEAF_VENDOR))
        goto Done;

    VmCheck("a leaf it did not ask about was left to the hypervisor",
            Vm.CpuidExits == 0);
    VmCheckValue("which still names itself", VM_HV_VENDOR_EBX, VmResult(&Vm, 1));

Done:
    VmDestroy(&Vm);
}

/**
 * @brief
 * A guest reading and writing a register no processor has, which only reaches
 * the monitor because the monitor asked for it.
 */
static
VOID
VmStageMonitorAnswersMsr(VOID)
{
    VM Vm;

    VmPrint("\nthe monitor answers a register no processor has\n");

    if (!VmCreate(&Vm, NULL, 0, TRUE))
        goto Done;

    if (!VmLoad(&Vm, VmRdmsrProgram, sizeof(VmRdmsrProgram)))
        goto Done;

    VmPatch(&Vm, VM_RDMSR_PROGRAM_MSR, VM_TEST_MSR);

    if (!VmRun(&Vm))
        goto Done;

    VmCheckValue("the monitor was asked to read it", 1, Vm.MsrReads);
    VmCheckValue("and told which one", VM_TEST_MSR, Vm.MsrNumber);
    VmCheckValue("the guest read the low half back",
                 VM_TEST_MSR_LOW, VmResult(&Vm, 0));
    VmCheckValue("and the high half", VM_TEST_MSR_HIGH, VmResult(&Vm, 1));

    if (!VmLoad(&Vm, VmWrmsrProgram, sizeof(VmWrmsrProgram)))
        goto Done;

    VmPatch(&Vm, VM_WRMSR_PROGRAM_MSR, VM_TEST_MSR);
    VmPatch(&Vm, VM_WRMSR_PROGRAM_LOW, VM_TEST_MSR_LOW);
    VmPatch(&Vm, VM_WRMSR_PROGRAM_HIGH, VM_TEST_MSR_HIGH);

    if (!VmRun(&Vm))
        goto Done;

    VmCheckValue("the monitor was asked to write it", 1, Vm.MsrWrites);
    VmCheckValue("with the value the guest put there",
                 ((ULONG64)VM_TEST_MSR_HIGH << 32) | VM_TEST_MSR_LOW,
                 Vm.MsrValue);

Done:
    VmDestroy(&Vm);
}

int
__cdecl
main(void)
{
    WHV_CAPABILITY Capability;
    UINT32 Written = 0;
    HRESULT Result;

    VmPrint("ReactOS virtual machine monitor\n");

    if (!VmBindPlatform())
        return 1;

    Result = VmGetCapability(WHvCapabilityCodeHypervisorPresent,
                             &Capability,
                             sizeof(Capability),
                             &Written);
    if (FAILED(Result) || !Capability.HypervisorPresent)
    {
        VmPrint("\nNo hypervisor platform here, so there is no machine to build.\n");
        return 1;
    }

    VmStageWhatTheHypervisorSays();
    VmStageWhatTheMonitorSays();
    VmStageMonitorAnswersMsr();

    VmPrint("\n%lu checks, %lu of them failed\n", VmChecks, VmFailures);

    return (VmFailures == 0) ? 0 : 1;
}

/* EOF */
