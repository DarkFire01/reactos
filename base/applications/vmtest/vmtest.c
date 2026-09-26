/*
 * PROJECT:     ReactOS Hypervisor Platform Test
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The smallest virtual machine that proves a partition works
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Written against the Windows Hypervisor Platform, which is the documented way
 * to ask a hypervisor for a partition. The same source runs on Windows against
 * Hyper-V, so when it prints the same thing here the partition is what works
 * and not the test.
 *
 * The guest is twenty bytes of 16 bit code that writes a string to a port and
 * halts, so every exit this has to handle is one a hypervisor cannot avoid
 * implementing: memory, a port, and a halt.
 */

/* INCLUDES *******************************************************************/

#include <windows.h>
#include <winhvplatform.h>

#include <stdio.h>
#include <stdarg.h>

/* GLOBALS ********************************************************************/

/*
 * The platform is bound at run time rather than linked, so this runs on a
 * system that has no hypervisor platform and says so instead of failing to
 * start.
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

static PFN_WHV_GET_CAPABILITY VmTestGetCapability;
static PFN_WHV_CREATE_PARTITION VmTestCreatePartition;
static PFN_WHV_DELETE_PARTITION VmTestDeletePartition;
static PFN_WHV_SETUP_PARTITION VmTestSetupPartition;
static PFN_WHV_SET_PARTITION_PROPERTY VmTestSetPartitionProperty;
static PFN_WHV_MAP_GPA_RANGE VmTestMapGpaRange;
static PFN_WHV_CREATE_VP VmTestCreateVirtualProcessor;
static PFN_WHV_DELETE_VP VmTestDeleteVirtualProcessor;
static PFN_WHV_RUN_VP VmTestRunVirtualProcessor;
static PFN_WHV_SET_VP_REGISTERS VmTestSetVirtualProcessorRegisters;

#define VMTEST_GUEST_RAM_SIZE   0x100000
#define VMTEST_CODE_ADDRESS     0x1000
#define VMTEST_MESSAGE_ADDRESS  0x1020
#define VMTEST_OUTPUT_PORT      0xE9

static const UCHAR VmTestGuestCode[] =
{
    /* 1000 */ 0xBE, 0x20, 0x10,    /* mov si, 1020h            */
    /* 1003 */ 0xAC,                /* lodsb                    */
    /* 1004 */ 0x3C, 0x00,          /* cmp al, 0                */
    /* 1006 */ 0x74, 0x04,          /* je 100Ch                 */
    /* 1008 */ 0xE6, 0xE9,          /* out 0E9h, al             */
    /* 100A */ 0xEB, 0xF7,          /* jmp 1003h                */
    /* 100C */ 0xF4,                /* hlt                      */
    /* 100D */ 0xEB, 0xFE           /* jmp 100Dh                */
};

static const CHAR VmTestGuestMessage[] = "Hello from inside the partition\n";

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Writes a line to the console and to the debugger.
 *
 * @remarks
 * A machine being tested headlessly has no console worth reading, so the
 * debug log is where the result is actually collected from.
 */
static
VOID
__cdecl
VmTestPrint(
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

/**
 * @brief
 * Binds one routine of the platform.
 *
 * @return
 * FALSE when the platform does not offer it, which no client can work around.
 */
static
BOOLEAN
VmTestBindOne(
    _In_ HMODULE Platform,
    _In_z_ PCSTR Name,
    _Out_ FARPROC *Routine)
{
    *Routine = GetProcAddress(Platform, Name);

    if (*Routine == NULL)
    {
        VmTestPrint("  %s is missing from the platform\n", Name);
        return FALSE;
    }

    return TRUE;
}

#define VMTEST_BIND(Target, Name) \
    if (!VmTestBindOne(Platform, Name, (FARPROC *)&Target)) \
        return FALSE

/**
 * @brief
 * Finds the hypervisor platform and binds what this test calls.
 */
static
BOOLEAN
VmTestBindPlatform(VOID)
{
    HMODULE Platform = LoadLibraryW(L"WinHvPlatform.dll");

    if (Platform == NULL)
    {
        VmTestPrint("  no WinHvPlatform.dll on this system\n");
        return FALSE;
    }

    VMTEST_BIND(VmTestGetCapability, "WHvGetCapability");
    VMTEST_BIND(VmTestCreatePartition, "WHvCreatePartition");
    VMTEST_BIND(VmTestDeletePartition, "WHvDeletePartition");
    VMTEST_BIND(VmTestSetupPartition, "WHvSetupPartition");
    VMTEST_BIND(VmTestSetPartitionProperty, "WHvSetPartitionProperty");
    VMTEST_BIND(VmTestMapGpaRange, "WHvMapGpaRange");
    VMTEST_BIND(VmTestCreateVirtualProcessor, "WHvCreateVirtualProcessor");
    VMTEST_BIND(VmTestDeleteVirtualProcessor, "WHvDeleteVirtualProcessor");
    VMTEST_BIND(VmTestRunVirtualProcessor, "WHvRunVirtualProcessor");
    VMTEST_BIND(VmTestSetVirtualProcessorRegisters, "WHvSetVirtualProcessorRegisters");

    return TRUE;
}

/**
 * @brief
 * Reports the result of one step.
 */
static
VOID
VmTestReport(
    _In_z_ PCSTR What,
    _In_ HRESULT Result)
{
    VmTestPrint("  %-34s %s (%08lx)\n",
                What,
                SUCCEEDED(Result) ? "ok" : "FAILED",
                (ULONG)Result);
}

/**
 * @brief
 * Loads one segment register with a flat real mode segment.
 *
 * @param[in] Code
 * Whether the segment is the one instructions are fetched from.
 */
static
HRESULT
VmTestSetRealModeSegment(
    _In_ WHV_PARTITION_HANDLE Partition,
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

    return VmTestSetVirtualProcessorRegisters(Partition, 0, &Name, 1, &Value);
}

int
__cdecl
main(void)
{
    WHV_CAPABILITY Capability;
    WHV_PARTITION_HANDLE Partition = NULL;
    WHV_RUN_VP_EXIT_CONTEXT Exit;
    WHV_REGISTER_NAME Names[2];
    WHV_REGISTER_VALUE Values[2];
    UINT32 Written = 0;
    UINT32 ProcessorCount = 1;
    PVOID GuestRam = NULL;
    HRESULT Result;
    ULONG Exits = 0;

    VmTestPrint("ReactOS hypervisor platform test\n\n");

    if (!VmTestBindPlatform())
        return 1;

    Result = VmTestGetCapability(WHvCapabilityCodeHypervisorPresent,
                                 &Capability,
                                 sizeof(Capability),
                                 &Written);
    VmTestReport("hypervisor present", Result);
    if (FAILED(Result) || !Capability.HypervisorPresent)
    {
        VmTestPrint("\nNo hypervisor platform here. On Windows that is the\n"
                    "Windows Hypervisor Platform feature, on ReactOS it is ReactV.\n");
        return 1;
    }

    Result = VmTestCreatePartition(&Partition);
    VmTestReport("create partition", Result);
    if (FAILED(Result))
        return 1;

    Result = VmTestSetPartitionProperty(Partition,
                                        WHvPartitionPropertyCodeProcessorCount,
                                        &ProcessorCount,
                                        sizeof(ProcessorCount));
    VmTestReport("one virtual processor", Result);

    Result = VmTestSetupPartition(Partition);
    VmTestReport("set up partition", Result);
    if (FAILED(Result))
        goto Cleanup;

    /* A megabyte of guest memory, which is more than this guest will touch */
    GuestRam = VirtualAlloc(NULL,
                            VMTEST_GUEST_RAM_SIZE,
                            MEM_COMMIT | MEM_RESERVE,
                            PAGE_READWRITE);
    if (GuestRam == NULL)
    {
        VmTestPrint("  out of memory for the guest\n");
        goto Cleanup;
    }

    CopyMemory((PUCHAR)GuestRam + VMTEST_CODE_ADDRESS,
               VmTestGuestCode,
               sizeof(VmTestGuestCode));
    CopyMemory((PUCHAR)GuestRam + VMTEST_MESSAGE_ADDRESS,
               VmTestGuestMessage,
               sizeof(VmTestGuestMessage));

    Result = VmTestMapGpaRange(Partition,
                               GuestRam,
                               0,
                               VMTEST_GUEST_RAM_SIZE,
                               WHvMapGpaRangeFlagRead |
                               WHvMapGpaRangeFlagWrite |
                               WHvMapGpaRangeFlagExecute);
    VmTestReport("map guest memory at zero", Result);
    if (FAILED(Result))
        goto Cleanup;

    Result = VmTestCreateVirtualProcessor(Partition, 0, 0);
    VmTestReport("create virtual processor", Result);
    if (FAILED(Result))
        goto Cleanup;

    /* Point it at the code instead of the reset vector it starts on */
    Result = VmTestSetRealModeSegment(Partition, WHvX64RegisterCs, TRUE);
    if (SUCCEEDED(Result))
        Result = VmTestSetRealModeSegment(Partition, WHvX64RegisterDs, FALSE);
    if (SUCCEEDED(Result))
        Result = VmTestSetRealModeSegment(Partition, WHvX64RegisterEs, FALSE);
    if (SUCCEEDED(Result))
        Result = VmTestSetRealModeSegment(Partition, WHvX64RegisterSs, FALSE);
    VmTestReport("flat real mode segments", Result);

    Names[0] = WHvX64RegisterRip;
    Values[0].Reg64 = VMTEST_CODE_ADDRESS;
    Names[1] = WHvX64RegisterRflags;
    Values[1].Reg64 = 2;

    Result = VmTestSetVirtualProcessorRegisters(Partition, 0, Names, 2, Values);
    VmTestReport("set the starting point", Result);
    if (FAILED(Result))
        goto Cleanup;

    VmTestPrint("\nrunning, the guest speaks through port %02x:\n\n  ",
                VMTEST_OUTPUT_PORT);

    for (;;)
    {
        Result = VmTestRunVirtualProcessor(Partition, 0, &Exit, sizeof(Exit));
        if (FAILED(Result))
        {
            VmTestPrint("\n\n  run failed, %08lx\n", (ULONG)Result);
            goto Cleanup;
        }

        Exits++;

        if (Exit.ExitReason == WHvRunVpExitReasonX64IoPortAccess)
        {
            if (Exit.IoPortAccess.PortNumber == VMTEST_OUTPUT_PORT)
                VmTestPrint("%c", (CHAR)(Exit.IoPortAccess.Rax & 0xFF));

            /* The hypervisor stopped on the instruction, so step over it */
            Names[0] = WHvX64RegisterRip;
            Values[0].Reg64 = Exit.VpContext.Rip + Exit.VpContext.InstructionLength;

            Result = VmTestSetVirtualProcessorRegisters(Partition, 0, Names, 1, Values);
            if (FAILED(Result))
            {
                VmTestPrint("\n\n  could not step over the port access, %08lx\n",
                            (ULONG)Result);
                goto Cleanup;
            }

            continue;
        }

        if (Exit.ExitReason == WHvRunVpExitReasonX64Halt)
        {
            VmTestPrint("\n\n  the guest halted after %lu exits\n", Exits);
            break;
        }

        VmTestPrint("\n\n  unexpected exit %lu at rip %I64x\n",
                    (ULONG)Exit.ExitReason,
                    Exit.VpContext.Rip);
        break;
    }

    VmTestDeleteVirtualProcessor(Partition, 0);

Cleanup:

    if (GuestRam != NULL)
        VirtualFree(GuestRam, 0, MEM_RELEASE);

    if (Partition != NULL)
        VmTestDeletePartition(Partition);

    return 0;
}

/* EOF */
