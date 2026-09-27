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
typedef HRESULT (WINAPI *PFN_WHV_TRANSLATE_GVA)(WHV_PARTITION_HANDLE, UINT32, WHV_GUEST_VIRTUAL_ADDRESS, WHV_TRANSLATE_GVA_FLAGS, WHV_TRANSLATE_GVA_RESULT *, WHV_GUEST_PHYSICAL_ADDRESS *);
typedef HRESULT (WINAPI *PFN_WHV_POST_SYNIC_MESSAGE)(WHV_PARTITION_HANDLE, UINT32, UINT32, const VOID *, UINT32);
typedef HRESULT (WINAPI *PFN_WHV_GET_XSAVE_STATE)(WHV_PARTITION_HANDLE, UINT32, VOID *, UINT32, UINT32 *);

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
static PFN_WHV_TRANSLATE_GVA VmTranslateGva;

/* Not every platform has these, so they are bound without insisting on them */
static PFN_WHV_POST_SYNIC_MESSAGE VmPostSynicMessage;
static PFN_WHV_GET_XSAVE_STATE VmGetXsaveState;

/* The machine: a megabyte of memory, a console port, and nothing else yet */
#define VM_RAM_SIZE         0x100000
#define VM_CODE_BASE        0x1000
#define VM_RESULT_BASE      0x2000
#define VM_STACK_TOP        0x8000
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
    /* Whether the monitor waits to be told the guest could take an interrupt */
    BOOLEAN AskForWindow;
    /* Whether a zero byte out of the console port means the guest is waiting */
    BOOLEAN Interrupting;
    ULONG Windows;
    ULONG LastPortInfo;
    ULONG Injections;
    ULONG StringWrites;
    ULONG Hypercalls;
    ULONG64 HypercallControl;
    /* What the guest writes when it is ready to be posted a message */
    CHAR ReadyLetter;
    const UCHAR *Message;
    HRESULT MessageResult;
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
 * Two programs that wait for an interrupt. Both tell the monitor they are
 * ready by writing to the console port, then spin on a flag the handler sets,
 * so neither of them halts until the interrupt has been taken. The difference
 * is where interrupts are enabled: the first is already open when the monitor
 * hears from it, the second is not, which is what makes the monitor ask to be
 * told when the processor could take one.
 */
static const UCHAR VmInterruptProgram[] =
{
    0xFB,                                   /* sti                      */
    0xE6, 0xE9,                             /* out 0E9h, al             */
    0xA0, 0x10, 0x20,                       /* mov al, [2010h]          */
    0x3C, 0x00,                             /* cmp al, 0                */
    0x75, 0x04,                             /* jne the halt             */
    0xE6, 0x80,                             /* out 80h, al              */
    0xEB, 0xF5,                             /* jmp back to the load     */
    0xF4                                    /* hlt                      */
};

static const UCHAR VmWindowProgram[] =
{
    0xE6, 0xE9,                             /* out 0E9h, al             */
    0xFB,                                   /* sti                      */
    0xA0, 0x10, 0x20,                       /* mov al, [2010h]          */
    0x3C, 0x00,                             /* cmp al, 0                */
    0x75, 0x04,                             /* jne the halt             */
    0xE6, 0x80,                             /* out 80h, al              */
    0xEB, 0xF5,                             /* jmp back to the load     */
    0xF4                                    /* hlt                      */
};

/*
 * The port the wait loop touches every time round, which is what keeps the
 * monitor in the picture while the guest is waiting. A guest that spins on
 * memory alone never comes out, and a machine has nothing to say about it.
 */
#define VM_YIELD_PORT       0x80

/* Where a guest's own messages go, and which interrupt it asked for them on */
#define VM_SYNIC_MESSAGE_PAGE   0x3000
#define VM_SYNIC_SINT           0
#define VM_SYNIC_VECTOR         0x33
#define VM_SYNIC_LETTER         'M'
#define VM_SYNIC_READY          'R'

/*
 * A whole string out of one instruction, which is what a real driver writes to
 * a port. The monitor is told how many and from where, and does the rest.
 */
#define VM_STRING_PROGRAM_TEXT 0x30
#define VM_STRING_PROGRAM_COUNT 4

static const UCHAR VmStringProgram[] =
{
    0xBE, 0x30, 0x10,                       /* mov si, 1030h            */
    0xB9, 0x00, 0x00,                       /* mov cx, <count>          */
    0xBA, 0xE9, 0x00,                       /* mov dx, 0E9h             */
    0xF3, 0x6E,                             /* rep outsb                */
    0xF4                                    /* hlt                      */
};

/*
 * A guest asking the hypervisor for something. The processor of this machine
 * takes the call directly, which a real guest would reach through the page the
 * hypervisor writes for it.
 */
#define VM_HYPERCALL_PROGRAM_CODE 2
#define VM_HYPERCALL_CONTROL    0x1234
#define VM_HYPERCALL_ANSWER     'H'

static const UCHAR VmHypercallProgram[] =
{
    0x66, 0xB9, 0x00, 0x00, 0x00, 0x00,     /* mov ecx, <control>       */
    0x0F, 0x01, 0xD9,                       /* vmmcall                  */
    0xE6, 0xE9,                             /* out 0E9h, al             */
    0xF4                                    /* hlt                      */
};

/* One vector register into another, to prove the guest has its own */
static const UCHAR VmVectorProgram[] =
{
    0x0F, 0x28, 0xC8,                       /* movaps xmm1, xmm0        */
    0xF4                                    /* hlt                      */
};

/* What the guest is given in its first vector register, and where it puts it */
#define VM_VECTOR_LOW       0x1122334455667788ULL
#define VM_VECTOR_HIGH      0x99AABBCCDDEEFF00ULL
#define VM_VECTOR_SSE_CR4   0x00000600

/*
 * What both of them run when the interrupt arrives: it says so on the console,
 * leaves a word behind for the monitor to find, and sets the flag the program
 * is waiting on.
 */
#define VM_HANDLER_ADDRESS  0x1100
#define VM_HANDLER_VECTOR   0x33
#define VM_HANDLER_MARK     0xDEADBEEF
#define VM_HANDLER_LETTER   'B'

static const UCHAR VmInterruptHandler[] =
{
    0xB0, 0x42,                             /* mov al, 42h              */
    0xE6, 0xE9,                             /* out 0E9h, al             */
    0x66, 0xB8, 0xEF, 0xBE, 0xAD, 0xDE,     /* mov eax, 0DEADBEEFh      */
    0x66, 0xA3, 0x00, 0x20,                 /* mov [2000h], eax         */
    0xC6, 0x06, 0x10, 0x20, 0x01,           /* mov byte [2010h], 1      */
    0xCF                                    /* iret                     */
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
    VM_BIND(VmTranslateGva, "WHvTranslateGva");

    VmPostSynicMessage = (PFN_WHV_POST_SYNIC_MESSAGE)
        GetProcAddress(Platform, "WHvPostVirtualProcessorSynicMessage");

    VmGetXsaveState = (PFN_WHV_GET_XSAVE_STATE)
        GetProcAddress(Platform, "WHvGetVirtualProcessorXsaveState");

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
VmCreateWith(
    _Out_ PVM Vm,
    _In_reads_opt_(LeafCount) const ULONG *CpuidLeaves,
    _In_ ULONG LeafCount,
    _In_ BOOLEAN MsrExits,
    _In_ BOOLEAN HypercallExits,
    _In_ UINT32 ProcessorCount)
{
    WHV_EXTENDED_VM_EXITS Extended;
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
    Extended.HypercallExit = HypercallExits ? 1 : 0;

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
    if (SUCCEEDED(Result))
        Result = VmSetRegister(Vm, WHvX64RegisterRsp, VM_STACK_TOP);

    if (FAILED(Result))
    {
        VmPrint("  could not put the processor in real mode, %08lx\n", (ULONG)Result);
        return FALSE;
    }

    return TRUE;
}

static
BOOLEAN
VmCreate(
    _Out_ PVM Vm,
    _In_reads_opt_(LeafCount) const ULONG *CpuidLeaves,
    _In_ ULONG LeafCount,
    _In_ BOOLEAN MsrExits)
{
    return VmCreateWith(Vm, CpuidLeaves, LeafCount, MsrExits, FALSE, 1);
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
    Vm->Windows = 0;
    Vm->Injections = 0;
    Vm->StringWrites = 0;
    Vm->Hypercalls = 0;
    Vm->HypercallControl = 0;
    Vm->MessageResult = E_PENDING;
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

/* Patches a two byte immediate, which is what a sixteen bit register takes */
static
VOID
VmPatchWord(
    _In_ PVM Vm,
    _In_ ULONG Offset,
    _In_ USHORT Value)
{
    Vm->Ram[VM_CODE_BASE + Offset + 0] = (UCHAR)Value;
    Vm->Ram[VM_CODE_BASE + Offset + 1] = (UCHAR)(Value >> 8);
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
 * Carries out a string write to the console port, which the guest asked for
 * with one instruction and the monitor has to do a byte at a time.
 *
 * @remarks
 * The count and the source are the guest's own registers, and the monitor moves
 * them on itself, because the instruction stopped before any of it happened.
 */
static
HRESULT
VmWriteStringToConsole(
    _In_ PVM Vm,
    _In_ const WHV_RUN_VP_EXIT_CONTEXT *Exit)
{
    WHV_REGISTER_NAME Names[2];
    WHV_REGISTER_VALUE Values[2];
    ULONG64 Source;
    ULONG64 Count;
    ULONG64 Index;
    HRESULT Result;

    Source = Exit->IoPortAccess.Ds.Base + Exit->IoPortAccess.Rsi;
    Count = Exit->IoPortAccess.AccessInfo.RepPrefix ? (Exit->IoPortAccess.Rcx & 0xFFFF)
                                                    : 1;

    Vm->StringWrites++;

    for (Index = 0; Index < Count; Index++)
    {
        if ((Source + Index) >= VM_RAM_SIZE)
            break;

        if (Vm->ConsoleLength < (sizeof(Vm->Console) - 1))
            Vm->Console[Vm->ConsoleLength++] = (CHAR)Vm->Ram[Source + Index];
    }

    /* The guest is left where the instruction would have left it */
    Names[0] = WHvX64RegisterRsi;
    Names[1] = WHvX64RegisterRcx;

    ZeroMemory(Values, sizeof(Values));
    Values[0].Reg64 = Exit->IoPortAccess.Rsi + Count;
    Values[1].Reg64 = Exit->IoPortAccess.AccessInfo.RepPrefix ? 0
                                                              : Exit->IoPortAccess.Rcx;

    Result = VmSetRegisters(Vm->Partition, 0, Names, 2, Values);
    if (FAILED(Result))
        return Result;

    return VmStepOver(Vm, Exit);
}

/* Gives the guest the interrupt this monitor has been holding for it */
static
HRESULT
VmGiveInterrupt(
    _In_ PVM Vm)
{
    WHV_X64_PENDING_INTERRUPTION_REGISTER Pending;
    WHV_REGISTER_NAME Name = WHvRegisterPendingInterruption;
    WHV_REGISTER_VALUE Value;

    Pending.AsUINT64 = 0;
    Pending.InterruptionPending = 1;
    Pending.InterruptionType = WHvX64PendingInterrupt;
    Pending.InterruptionVector = VM_HANDLER_VECTOR;

    ZeroMemory(&Value, sizeof(Value));
    Value.Reg64 = Pending.AsUINT64;

    Vm->Injections++;

    return VmSetRegisters(Vm->Partition, 0, &Name, 1, &Value);
}

/* Asks to be told the moment the guest could take one */
static
HRESULT
VmAskForWindow(
    _In_ PVM Vm)
{
    WHV_X64_DELIVERABILITY_NOTIFICATIONS_REGISTER Wanted;
    WHV_REGISTER_NAME Name = WHvX64RegisterDeliverabilityNotifications;
    WHV_REGISTER_VALUE Value;

    Wanted.AsUINT64 = 0;
    Wanted.InterruptNotification = 1;

    ZeroMemory(&Value, sizeof(Value));
    Value.Reg64 = Wanted.AsUINT64;

    return VmSetRegisters(Vm->Partition, 0, &Name, 1, &Value);
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
            Vm->Console[Vm->ConsoleLength] = ANSI_NULL;

            VmPrint("  the guest is not getting anywhere: %lu exits, rip %I64x, "
                    "flag %lx, windows %lu, given %lu, console '%s'\n",
                    Vm->Exits,
                    Exit.VpContext.Rip,
                    VmResult(Vm, 4),
                    Vm->Windows,
                    Vm->Injections,
                    Vm->Console);
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
                if (Exit.IoPortAccess.PortNumber != VM_CONSOLE_PORT)
                {
                    Result = VmStepOver(Vm, &Exit);
                    break;
                }

                /*
                 * A zero is the guest saying it is ready for the interrupt
                 * rather than anything worth printing, so the monitor either
                 * gives it one or asks to be told when it could take one.
                 */
                Vm->LastPortInfo = Exit.IoPortAccess.AccessInfo.AsUINT32;

                if (Vm->Interrupting && ((Exit.IoPortAccess.Rax & 0xFF) == 0))
                {
                    Result = VmStepOver(Vm, &Exit);

                    if (SUCCEEDED(Result) && (Vm->Injections == 0))
                    {
                        Result = Vm->AskForWindow ? VmAskForWindow(Vm)
                                                  : VmGiveInterrupt(Vm);
                    }

                    break;
                }

                /*
                 * A whole string in one instruction, which the monitor has to
                 * carry out itself: the guest said where from and how many, and
                 * nothing has moved yet.
                 */
                if ((Vm->Message != NULL) &&
                    ((CHAR)(Exit.IoPortAccess.Rax & 0xFF) == Vm->ReadyLetter))
                {
                    Vm->MessageResult = VmPostSynicMessage(Vm->Partition,
                                                           0,
                                                           VM_SYNIC_SINT,
                                                           Vm->Message,
                                                           256);

                    if (Vm->ConsoleLength < (sizeof(Vm->Console) - 1))
                    {
                        Vm->Console[Vm->ConsoleLength++] =
                            (CHAR)Exit.IoPortAccess.Rax;
                    }

                    Result = VmStepOver(Vm, &Exit);
                    break;
                }

                if (Exit.IoPortAccess.AccessInfo.StringOp)
                {
                    Result = VmWriteStringToConsole(Vm, &Exit);
                    break;
                }

                if (Vm->ConsoleLength < (sizeof(Vm->Console) - 1))
                    Vm->Console[Vm->ConsoleLength++] = (CHAR)Exit.IoPortAccess.Rax;

                Result = VmStepOver(Vm, &Exit);
                break;

            case WHvRunVpExitReasonHypercall:
                Vm->Hypercalls++;
                Vm->HypercallControl = Exit.Hypercall.Rcx;

                Result = VmSetRegister(Vm, WHvX64RegisterRax, VM_HYPERCALL_ANSWER);
                if (SUCCEEDED(Result))
                    Result = VmStepOver(Vm, &Exit);

                break;

            case WHvRunVpExitReasonX64InterruptWindow:
                /* The processor could take one now, so it gets one */
                Vm->Windows++;
                Result = VmGiveInterrupt(Vm);
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

/**
 * @brief
 * A guest that waits for an interrupt and a monitor that gives it one.
 *
 * @param[in] AskForWindow
 * Whether the guest has interrupts closed when the monitor first hears from
 * it, so that the monitor has to ask to be told when they open.
 */
static
VOID
VmStageInterrupt(
    _In_ BOOLEAN AskForWindow)
{
    PUCHAR Entry;
    VM Vm;

    VmPrint(AskForWindow ? "\nthe monitor waits for the guest to open up\n"
                         : "\nthe monitor gives the guest an interrupt\n");

    if (!VmCreate(&Vm, NULL, 0, FALSE))
        goto Done;

    Vm.AskForWindow = AskForWindow;
    Vm.Interrupting = TRUE;

    if (!VmLoad(&Vm,
                AskForWindow ? VmWindowProgram : VmInterruptProgram,
                AskForWindow ? sizeof(VmWindowProgram) : sizeof(VmInterruptProgram)))
    {
        goto Done;
    }

    CopyMemory(Vm.Ram + VM_HANDLER_ADDRESS,
               VmInterruptHandler,
               sizeof(VmInterruptHandler));

    /* Where a real mode processor looks for the handler, offset then segment */
    Entry = Vm.Ram + (VM_HANDLER_VECTOR * 4);
    Entry[0] = (UCHAR)VM_HANDLER_ADDRESS;
    Entry[1] = (UCHAR)(VM_HANDLER_ADDRESS >> 8);
    Entry[2] = 0;
    Entry[3] = 0;

    if (!VmRun(&Vm))
        goto Done;

    Vm.Console[Vm.ConsoleLength] = ANSI_NULL;

    if (AskForWindow)
        VmCheckValue("the monitor was told once that it could", 1, Vm.Windows);
    else
        VmCheck("the monitor did not have to wait to be told", Vm.Windows == 0);

    VmCheckValue("the interrupt was given once", 1, Vm.Injections);
    VmCheck("the handler ran", Vm.Console[0] == VM_HANDLER_LETTER);
    VmCheckValue("and left its mark behind", VM_HANDLER_MARK, VmResult(&Vm, 0));
    VmCheckValue("and the guest carried on where it left off", 1, VmResult(&Vm, 4));

Done:
    VmDestroy(&Vm);
}

/*
 * Where the guest's own tables go when the monitor builds them for it, and
 * what they are made to say. The addresses are picked to be far from anything
 * else the machine uses.
 */
#define VM_PAGE_DIRECTORY   0x4000
#define VM_PAGE_TABLE       0x5000
#define VM_MAPPED_PAGE      0x7000
#define VM_MAPPED_VIRTUAL   0x00401000
#define VM_ABSENT_VIRTUAL   0x00402000
#define VM_READ_ONLY_VIRTUAL 0x00403000
#define VM_PTE_PRESENT      0x001
#define VM_PTE_WRITE        0x002
#define VM_PTE_USER         0x004

/* Writes one four byte entry into a table the monitor is building */
static
VOID
VmPutEntry(
    _In_ PVM Vm,
    _In_ ULONG TableGpa,
    _In_ ULONG Index,
    _In_ ULONG Entry)
{
    PUCHAR At = Vm->Ram + TableGpa + (Index * sizeof(ULONG));

    At[0] = (UCHAR)Entry;
    At[1] = (UCHAR)(Entry >> 8);
    At[2] = (UCHAR)(Entry >> 16);
    At[3] = (UCHAR)(Entry >> 24);
}

/* Asks where one of the guest's own addresses ends up */
static
BOOLEAN
VmFollow(
    _In_ PVM Vm,
    _In_ ULONG64 Virtual,
    _In_ WHV_TRANSLATE_GVA_FLAGS Flags,
    _Out_ WHV_TRANSLATE_GVA_RESULT *Result,
    _Out_ WHV_GUEST_PHYSICAL_ADDRESS *Physical)
{
    HRESULT Status;

    Result->ResultCode = WHvTranslateGvaResultIntercept;
    *Physical = 0;

    Status = VmTranslateGva(Vm->Partition, 0, Virtual, Flags, Result, Physical);
    if (FAILED(Status))
    {
        VmPrint("  the address could not be followed at all, %08lx\n", (ULONG)Status);
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief
 * A guest with tables of its own, and a hypervisor asked to follow them.
 *
 * @remarks
 * The guest never runs. What is being proved is that the core reads the
 * guest's tables out of the guest's own memory, which is the only way a root
 * can make sense of an address its guest used.
 */
static
VOID
VmStageFollowGuestAddresses(VOID)
{
    WHV_TRANSLATE_GVA_RESULT Result;
    WHV_GUEST_PHYSICAL_ADDRESS Physical;
    VM Vm;

    VmPrint("\nthe core follows the guest's own tables\n");

    if (!VmCreate(&Vm, NULL, 0, FALSE))
        goto Done;

    /* With no paging an address is already physical, table or no table */
    if (!VmFollow(&Vm, 0x1234, WHvTranslateGvaFlagValidateRead, &Result, &Physical))
        goto Done;

    VmCheckValue("without paging it is the same address",
                 WHvTranslateGvaResultSuccess, Result.ResultCode);
    VmCheckValue("and the same page", 0x1234, Physical);

    /*
     * Two levels of the guest's own tables: one virtual page is mapped for
     * writing, one is mapped without it, and one is not mapped at all.
     */
    ZeroMemory(Vm.Ram + VM_PAGE_DIRECTORY, 0x1000);
    ZeroMemory(Vm.Ram + VM_PAGE_TABLE, 0x1000);

    VmPutEntry(&Vm, VM_PAGE_DIRECTORY, VM_MAPPED_VIRTUAL >> 22,
               VM_PAGE_TABLE | VM_PTE_PRESENT | VM_PTE_WRITE | VM_PTE_USER);
    VmPutEntry(&Vm, VM_PAGE_TABLE, (VM_MAPPED_VIRTUAL >> 12) & 0x3FF,
               VM_MAPPED_PAGE | VM_PTE_PRESENT | VM_PTE_WRITE | VM_PTE_USER);
    VmPutEntry(&Vm, VM_PAGE_TABLE, (VM_READ_ONLY_VIRTUAL >> 12) & 0x3FF,
               VM_MAPPED_PAGE | VM_PTE_PRESENT | VM_PTE_USER);

    if (FAILED(VmSetRegister(&Vm, WHvX64RegisterCr3, VM_PAGE_DIRECTORY)))
        goto Done;

    if (FAILED(VmSetRegister(&Vm, WHvX64RegisterCr4, 0)))
        goto Done;

    /* Protected mode with paging on, which is what makes the tables count */
    if (FAILED(VmSetRegister(&Vm, WHvX64RegisterCr0, 0x80000011)))
        goto Done;

    if (!VmFollow(&Vm, VM_MAPPED_VIRTUAL + 0x234, WHvTranslateGvaFlagValidateWrite,
                  &Result, &Physical))
    {
        goto Done;
    }

    VmCheckValue("a mapped address is followed",
                 WHvTranslateGvaResultSuccess, Result.ResultCode);
    VmCheckValue("to the page the guest said, offset and all",
                 VM_MAPPED_PAGE + 0x234, Physical);

    if (!VmFollow(&Vm, VM_ABSENT_VIRTUAL, WHvTranslateGvaFlagValidateRead,
                  &Result, &Physical))
    {
        goto Done;
    }

    VmCheckValue("an address the guest never mapped is refused",
                 WHvTranslateGvaResultPageNotPresent, Result.ResultCode);

    if (!VmFollow(&Vm, VM_READ_ONLY_VIRTUAL, WHvTranslateGvaFlagValidateWrite,
                  &Result, &Physical))
    {
        goto Done;
    }

    VmCheckValue("writing where the guest may only read is refused",
                 WHvTranslateGvaResultPrivilegeViolation, Result.ResultCode);

    if (!VmFollow(&Vm, VM_READ_ONLY_VIRTUAL, WHvTranslateGvaFlagValidateRead,
                  &Result, &Physical))
    {
        goto Done;
    }

    VmCheckValue("reading it is not", WHvTranslateGvaResultSuccess, Result.ResultCode);

Done:
    VmDestroy(&Vm);
}

/**
 * @brief
 * A guest that writes a whole string with one instruction, and a monitor that
 * has to carry it out.
 */
static
VOID
VmStageStringPort(VOID)
{
    static const CHAR Text[] = "a string at once";
    VM Vm;

    VmPrint("\nthe guest writes a string with one instruction\n");

    if (!VmCreate(&Vm, NULL, 0, FALSE))
        goto Done;

    if (!VmLoad(&Vm, VmStringProgram, sizeof(VmStringProgram)))
        goto Done;

    /* Without the terminator, because the count is what says where it ends */
    VmPatchWord(&Vm, VM_STRING_PROGRAM_COUNT, sizeof(Text) - 1);
    CopyMemory(Vm.Ram + VM_CODE_BASE + VM_STRING_PROGRAM_TEXT, Text, sizeof(Text));

    if (!VmRun(&Vm))
        goto Done;

    Vm.Console[Vm.ConsoleLength] = ANSI_NULL;

    VmCheckValue("the monitor was asked once for the whole string",
                 1, Vm.StringWrites);

    if (Vm.StringWrites != 1)
        VmPrint("    the port access said %08lx\n", Vm.LastPortInfo);

    VmCheck("and the string came out of it", strcmp(Vm.Console, Text) == 0);

    if (strcmp(Vm.Console, Text) != 0)
        VmPrint("    it wrote '%s'\n", Vm.Console);

Done:
    VmDestroy(&Vm);
}

/**
 * @brief
 * A guest with vector registers of its own, which it may use without touching
 * anyone else's.
 */
static
VOID
VmStageVectorRegisters(VOID)
{
    WHV_REGISTER_NAME Name;
    WHV_REGISTER_VALUE Value;
    VM Vm;

    VmPrint("\nthe guest has its own vector registers\n");

    if (!VmCreate(&Vm, NULL, 0, FALSE))
        goto Done;

    if (!VmLoad(&Vm, VmVectorProgram, sizeof(VmVectorProgram)))
        goto Done;

    /* The instruction the guest runs is refused outright without this */
    if (FAILED(VmSetRegister(&Vm, WHvX64RegisterCr4, VM_VECTOR_SSE_CR4)))
        goto Done;

    Name = WHvX64RegisterXmm0;
    ZeroMemory(&Value, sizeof(Value));
    Value.Reg128.Low64 = VM_VECTOR_LOW;
    Value.Reg128.High64 = VM_VECTOR_HIGH;

    if (FAILED(VmSetRegisters(Vm.Partition, 0, &Name, 1, &Value)))
    {
        VmPrint("  the first vector register could not be written\n");
        goto Done;
    }

    ZeroMemory(&Value, sizeof(Value));

    if (FAILED(VmGetRegisters(Vm.Partition, 0, &Name, 1, &Value)))
    {
        VmPrint("  the first vector register could not be read\n");
        goto Done;
    }

    VmCheckValue("what was put in one comes back out", VM_VECTOR_LOW, Value.Reg128.Low64);
    VmCheckValue("both halves of it", VM_VECTOR_HIGH, Value.Reg128.High64);

    if (!VmRun(&Vm))
        goto Done;

    Name = (WHV_REGISTER_NAME)(WHvX64RegisterXmm0 + 1);
    ZeroMemory(&Value, sizeof(Value));

    if (FAILED(VmGetRegisters(Vm.Partition, 0, &Name, 1, &Value)))
    {
        VmPrint("  the second vector register could not be read\n");
        goto Done;
    }

    VmCheckValue("the guest moved it into another itself",
                 VM_VECTOR_LOW, Value.Reg128.Low64);
    VmCheckValue("all of it", VM_VECTOR_HIGH, Value.Reg128.High64);

    if (VmGetXsaveState != NULL)
    {
        static UCHAR Saved[0x1000];
        UINT32 Written = 0;
        HRESULT Result;

        Result = VmGetXsaveState(Vm.Partition, 0, Saved, sizeof(Saved), &Written);

        VmCheckValue("the whole saved state can be read", S_OK, (ULONG)Result);

        if (SUCCEEDED(Result))
        {
            /* The vector registers sit where the processor puts them */
            VmCheckValue("and the first register is in it",
                         VM_VECTOR_LOW,
                         *(const ULONG64 *)(Saved + 0xA0));
        }
        else
        {
            VmPrint("    reading it said %08lx, %lu bytes\n", (ULONG)Result, Written);
        }
    }

Done:
    VmDestroy(&Vm);
}

/* THE SYNTHETIC CONTROLLER ****************************************************/

/*
 * A guest that turns its own synthetic interrupt controller on, says which
 * interrupt it wants messages on, and then waits. The monitor posts it a
 * message, the handler reads it out of the page the guest asked for, and says
 * on the console what it found.
 */

static const UCHAR VmSynicProgram[] =
{
    /*
     * Touching the page first, because the root puts a page in only when
     * something wants it, and what wants this one is the message that has not
     * been posted yet.
     */
    0xC6, 0x06, 0x00, 0x30, 0x00,           /* mov byte [3000h], 0      */
    0x66, 0xB9, 0x83, 0x00, 0x00, 0x40,     /* mov ecx, 40000083h       */
    0x66, 0xB8, 0x01, 0x30, 0x00, 0x00,     /* mov eax, 3001h           */
    0x66, 0x31, 0xD2,                       /* xor edx, edx             */
    0x0F, 0x30,                             /* wrmsr                    */
    0x66, 0xB9, 0x90, 0x00, 0x00, 0x40,     /* mov ecx, 40000090h       */
    0x66, 0xB8, 0x33, 0x00, 0x00, 0x00,     /* mov eax, 33h             */
    0x0F, 0x30,                             /* wrmsr                    */
    0x66, 0xB9, 0x80, 0x00, 0x00, 0x40,     /* mov ecx, 40000080h       */
    0x66, 0xB8, 0x01, 0x00, 0x00, 0x00,     /* mov eax, 1               */
    0x0F, 0x30,                             /* wrmsr                    */
    0xFB,                                   /* sti                      */
    0xB0, 0x52,                             /* mov al, 52h              */
    0xE6, 0xE9,                             /* out 0E9h, al             */
    0xA0, 0x10, 0x20,                       /* mov al, [2010h]          */
    0x3C, 0x00,                             /* cmp al, 0                */
    0x75, 0x04,                             /* jne the halt             */
    0xE6, 0x80,                             /* out 80h, al              */
    0xEB, 0xF5,                             /* jmp back to the load     */
    0xF4                                    /* hlt                      */
};

/* What the guest runs when the message arrives */
static const UCHAR VmSynicHandler[] =
{
    0xA0, 0x10, 0x30,                       /* mov al, [3010h]          */
    0xE6, 0xE9,                             /* out 0E9h, al             */
    0xC6, 0x06, 0x10, 0x20, 0x01,           /* mov byte [2010h], 1      */
    0x66, 0xB9, 0x84, 0x00, 0x00, 0x40,     /* mov ecx, 40000084h       */
    0x66, 0x31, 0xC0,                       /* xor eax, eax             */
    0x66, 0x31, 0xD2,                       /* xor edx, edx             */
    0x0F, 0x30,                             /* wrmsr                    */
    0xCF                                    /* iret                     */
};

/* MORE THAN ONE PROCESSOR *****************************************************/

/*
 * Each processor says which one it is on the console, leaves a word of its own
 * behind, and halts. The letter, the word and where it goes are patched in, so
 * the two of them run the same program over different addresses.
 */
#define VM_SECOND_CODE_BASE     0x1200
#define VM_SECOND_RESULT        0x2020
#define VM_PROCESSOR_LETTER     1
#define VM_PROCESSOR_MARK       6
#define VM_PROCESSOR_WHERE      12

static const UCHAR VmProcessorProgram[] =
{
    0xB0, 0x00,                             /* mov al, <letter>         */
    0xE6, 0xE9,                             /* out 0E9h, al             */
    0x66, 0xB8, 0x00, 0x00, 0x00, 0x00,     /* mov eax, <mark>          */
    0x66, 0xA3, 0x00, 0x20,                 /* mov [<where>], eax       */
    0xF4                                    /* hlt                      */
};

#define VM_FIRST_MARK           0x11111111
#define VM_SECOND_MARK          0x22222222

/* What one processor of the machine is doing, and what the monitor saw of it */
typedef struct _VM_PROCESSOR
{
    PVM Vm;
    UINT32 Index;
    ULONG Exits;
    CHAR Letter;
    BOOLEAN Halted;
    HRESULT Result;
} VM_PROCESSOR, *PVM_PROCESSOR;

static
HRESULT
VmSetRegisterOn(
    _In_ PVM Vm,
    _In_ UINT32 VpIndex,
    _In_ WHV_REGISTER_NAME Name,
    _In_ ULONG64 Value)
{
    WHV_REGISTER_VALUE Register;

    ZeroMemory(&Register, sizeof(Register));
    Register.Reg64 = Value;

    return VmSetRegisters(Vm->Partition, VpIndex, &Name, 1, &Register);
}

/* Gives one processor a flat real mode segment, whichever processor it is */
static
HRESULT
VmSetRealModeSegmentOn(
    _In_ PVM Vm,
    _In_ UINT32 VpIndex,
    _In_ WHV_REGISTER_NAME Name,
    _In_ BOOLEAN Code)
{
    WHV_REGISTER_VALUE Value;

    ZeroMemory(&Value, sizeof(Value));

    Value.Segment.Limit = 0xFFFF;
    Value.Segment.SegmentType = Code ? 0xB : 0x3;
    Value.Segment.NonSystemSegment = 1;
    Value.Segment.Present = 1;

    return VmSetRegisters(Vm->Partition, VpIndex, &Name, 1, &Value);
}

/**
 * @brief
 * Runs one processor of the machine until it halts.
 *
 * @remarks
 * One of these runs on a thread of its own, because a processor is run by
 * whoever is willing to give up a thread to it, and two of them are meant to
 * be able to run at the same time.
 */
static
DWORD
WINAPI
VmRunProcessor(
    _In_ LPVOID Context)
{
    PVM_PROCESSOR Processor = Context;
    WHV_RUN_VP_EXIT_CONTEXT Exit;
    PVM Vm = Processor->Vm;

    for (;;)
    {
        if (Processor->Exits >= VM_EXIT_LIMIT)
        {
            Processor->Result = E_ABORT;
            return 0;
        }

        Processor->Result = VmRunVirtualProcessor(Vm->Partition,
                                                 Processor->Index,
                                                 &Exit,
                                                 sizeof(Exit));
        if (FAILED(Processor->Result))
            return 0;

        Processor->Exits++;

        if (Exit.ExitReason == WHvRunVpExitReasonX64Halt)
        {
            Processor->Halted = TRUE;
            return 0;
        }

        if (Exit.ExitReason == WHvRunVpExitReasonX64IoPortAccess)
        {
            if (Exit.IoPortAccess.PortNumber == VM_CONSOLE_PORT)
                Processor->Letter = (CHAR)Exit.IoPortAccess.Rax;

            Processor->Result = VmSetRegisterOn(Vm,
                                                Processor->Index,
                                                WHvX64RegisterRip,
                                                Exit.VpContext.Rip +
                                                    Exit.VpContext.InstructionLength);
            if (FAILED(Processor->Result))
                return 0;

            continue;
        }

        VmPrint("  processor %lu stopped for %lu at %I64x\n",
                Processor->Index,
                (ULONG)Exit.ExitReason,
                Exit.VpContext.Rip);

        Processor->Result = E_UNEXPECTED;
        return 0;
    }
}

/* Puts one processor of the machine at the start of its own program */
static
BOOLEAN
VmStartProcessor(
    _In_ PVM Vm,
    _In_ UINT32 VpIndex,
    _In_ ULONG CodeBase,
    _In_ CHAR Letter,
    _In_ ULONG Mark,
    _In_ ULONG Where)
{
    PUCHAR At = Vm->Ram + CodeBase;
    HRESULT Result;

    CopyMemory(At, VmProcessorProgram, sizeof(VmProcessorProgram));

    At[VM_PROCESSOR_LETTER] = (UCHAR)Letter;
    At[VM_PROCESSOR_MARK + 0] = (UCHAR)Mark;
    At[VM_PROCESSOR_MARK + 1] = (UCHAR)(Mark >> 8);
    At[VM_PROCESSOR_MARK + 2] = (UCHAR)(Mark >> 16);
    At[VM_PROCESSOR_MARK + 3] = (UCHAR)(Mark >> 24);
    At[VM_PROCESSOR_WHERE + 0] = (UCHAR)Where;
    At[VM_PROCESSOR_WHERE + 1] = (UCHAR)(Where >> 8);

    Result = VmSetRealModeSegmentOn(Vm, VpIndex, WHvX64RegisterCs, TRUE);
    if (SUCCEEDED(Result))
        Result = VmSetRealModeSegmentOn(Vm, VpIndex, WHvX64RegisterDs, FALSE);
    if (SUCCEEDED(Result))
        Result = VmSetRealModeSegmentOn(Vm, VpIndex, WHvX64RegisterSs, FALSE);
    if (SUCCEEDED(Result))
        Result = VmSetRegisterOn(Vm, VpIndex, WHvX64RegisterRflags, 2);
    if (SUCCEEDED(Result))
        Result = VmSetRegisterOn(Vm, VpIndex, WHvX64RegisterRsp, VM_STACK_TOP);
    if (SUCCEEDED(Result))
        Result = VmSetRegisterOn(Vm, VpIndex, WHvX64RegisterRip, CodeBase);

    if (FAILED(Result))
    {
        VmPrint("  processor %lu could not be started, %08lx\n",
                VpIndex,
                (ULONG)Result);
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief
 * A machine with two processors, each running its own program at the same time.
 */
static
VOID
VmStageTwoProcessors(VOID)
{
    VM_PROCESSOR First;
    VM_PROCESSOR Second;
    HANDLE Thread;
    VM Vm;

    VmPrint("\nthe machine has two processors\n");

    if (!VmCreateWith(&Vm, NULL, 0, FALSE, FALSE, 2))
        goto Done;

    ZeroMemory(Vm.Ram + VM_RESULT_BASE, 0x1000);

    if (FAILED(VmCreateVirtualProcessor(Vm.Partition, 1, 0)))
    {
        VmPrint("  a second processor could not be made\n");
        goto Done;
    }

    if (!VmStartProcessor(&Vm, 0, VM_CODE_BASE, 'A', VM_FIRST_MARK, VM_RESULT_BASE))
        goto Done;

    if (!VmStartProcessor(&Vm, 1, VM_SECOND_CODE_BASE, 'B', VM_SECOND_MARK,
                          VM_SECOND_RESULT))
    {
        goto Done;
    }

    ZeroMemory(&First, sizeof(First));
    ZeroMemory(&Second, sizeof(Second));

    First.Vm = &Vm;
    First.Index = 0;
    Second.Vm = &Vm;
    Second.Index = 1;

    Thread = CreateThread(NULL, 0, VmRunProcessor, &Second, 0, NULL);
    if (Thread == NULL)
    {
        VmPrint("  the second processor has no thread to run on\n");
        goto Done;
    }

    VmRunProcessor(&First);

    WaitForSingleObject(Thread, INFINITE);
    CloseHandle(Thread);

    VmCheck("the first processor halted", First.Halted && SUCCEEDED(First.Result));
    VmCheck("the second processor halted", Second.Halted && SUCCEEDED(Second.Result));
    VmCheckValue("each said which one it was", 'A', First.Letter);
    VmCheckValue("and so did the other", 'B', Second.Letter);
    VmCheckValue("the first left its own word behind",
                 VM_FIRST_MARK, VmResult(&Vm, 0));
    VmCheckValue("and the second left its own",
                 VM_SECOND_MARK, VmResult(&Vm, (VM_SECOND_RESULT - VM_RESULT_BASE) / 4));

    VmDeleteVirtualProcessor(Vm.Partition, 1);

Done:
    VmDestroy(&Vm);
}

/**
 * @brief
 * A guest with a synthetic interrupt controller of its own, and a monitor that
 * posts it a message through it.
 */
static
VOID
VmStageSynicMessage(VOID)
{
    UCHAR Message[256];
    PUCHAR Entry;
    VM Vm;

    VmPrint("\nthe monitor posts the guest a message\n");

    if (VmPostSynicMessage == NULL)
    {
        VmPrint("  the platform cannot post one\n");
        return;
    }

    if (!VmCreate(&Vm, NULL, 0, FALSE))
        goto Done;

    if (!VmLoad(&Vm, VmSynicProgram, sizeof(VmSynicProgram)))
        goto Done;

    CopyMemory(Vm.Ram + VM_HANDLER_ADDRESS, VmSynicHandler, sizeof(VmSynicHandler));
    ZeroMemory(Vm.Ram + VM_SYNIC_MESSAGE_PAGE, 0x1000);

    Entry = Vm.Ram + (VM_SYNIC_VECTOR * 4);
    Entry[0] = (UCHAR)VM_HANDLER_ADDRESS;
    Entry[1] = (UCHAR)(VM_HANDLER_ADDRESS >> 8);
    Entry[2] = 0;
    Entry[3] = 0;

    /*
     * The guest says it is ready by writing a letter, and the monitor answers
     * with the message rather than with an interrupt of its own.
     */
    Vm.ReadyLetter = VM_SYNIC_READY;

    ZeroMemory(Message, sizeof(Message));
    Message[0] = 1;                                 /* any type but zero */
    Message[4] = 16;                                /* how much payload  */
    Message[16] = VM_SYNIC_LETTER;

    Vm.Message = Message;

    if (!VmRun(&Vm))
        goto Done;

    Vm.Console[Vm.ConsoleLength] = ANSI_NULL;

    VmCheckValue("the message was taken", S_OK, Vm.MessageResult);
    VmCheck("the guest read it out of its own page",
            Vm.Console[1] == VM_SYNIC_LETTER);
    VmCheckValue("and carried on afterwards", 1, VmResult(&Vm, 4));

    if (Vm.Console[1] != VM_SYNIC_LETTER)
        VmPrint("    the console says '%s'\n", Vm.Console);

    /* The slot is emptied by the guest, which is what says it is finished */
    VmCheckValue("the guest emptied the slot it read",
                 0, *(const ULONG *)(Vm.Ram + VM_SYNIC_MESSAGE_PAGE));

Done:
    VmDestroy(&Vm);
}

/**
 * @brief
 * A guest that asks the hypervisor for something, and a monitor that answers in
 * the hypervisor's place.
 */
static
VOID
VmStageGuestHypercall(VOID)
{
    VM Vm;

    VmPrint("\nthe guest asks the hypervisor, and the monitor answers\n");

    if (!VmCreateWith(&Vm, NULL, 0, FALSE, TRUE, 1))
        goto Done;

    if (!VmLoad(&Vm, VmHypercallProgram, sizeof(VmHypercallProgram)))
        goto Done;

    VmPatch(&Vm, VM_HYPERCALL_PROGRAM_CODE, VM_HYPERCALL_CONTROL);

    if (!VmRun(&Vm))
        goto Done;

    Vm.Console[Vm.ConsoleLength] = ANSI_NULL;

    VmCheckValue("the monitor was asked once", 1, Vm.Hypercalls);
    VmCheckValue("and told what the guest asked for",
                 VM_HYPERCALL_CONTROL, Vm.HypercallControl);
    VmCheckValue("the guest read the answer back",
                 VM_HYPERCALL_ANSWER, (ULONG)(UCHAR)Vm.Console[0]);

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
    VmStageInterrupt(FALSE);
    VmStageInterrupt(TRUE);
    VmStageFollowGuestAddresses();
    VmStageStringPort();
    VmStageVectorRegisters();
    VmStageTwoProcessors();
    VmStageSynicMessage();
    VmStageGuestHypercall();

    VmPrint("\n%lu checks, %lu of them failed\n", VmChecks, VmFailures);

    return (VmFailures == 0) ? 0 : 1;
}

/* EOF */
