/*
 * PROJECT:     ReactHypervTest
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     A machine with nothing in it, so that a firmware can be watched
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The other half of this program drives real hardware through the contract it
 * expects. This half is the machine that hardware would be put into, built
 * down to the smallest thing that will hold a firmware: memory, one processor,
 * and somewhere for the firmware to be.
 *
 * There is deliberately no hardware in it at all. A firmware coming up on a
 * machine with nothing in it says out loud, by what it reaches for, what it
 * expects a machine to have; and that is the list of what has to be there
 * before it will do anything else. Watching it fail is the point.
 */

#include <windows.h>
#include <winhvplatform.h>
#include <winhvemulation.h>
#include <stdio.h>
#include <string.h>

namespace hv
{

/* WHERE THE PLATFORM IS ******************************************************/

static HRESULT (WINAPI *CreatePartition)(WHV_PARTITION_HANDLE *);
static HRESULT (WINAPI *DeletePartition)(WHV_PARTITION_HANDLE);
static HRESULT (WINAPI *SetupPartition)(WHV_PARTITION_HANDLE);
static HRESULT (WINAPI *SetPartitionProperty)(WHV_PARTITION_HANDLE,
                                              WHV_PARTITION_PROPERTY_CODE,
                                              const VOID *, UINT32);
static HRESULT (WINAPI *MapGpaRange)(WHV_PARTITION_HANDLE, VOID *,
                                     WHV_GUEST_PHYSICAL_ADDRESS, UINT64,
                                     WHV_MAP_GPA_RANGE_FLAGS);
static HRESULT (WINAPI *EmulatorCreate)(const WHV_EMULATOR_CALLBACKS *,
                                       WHV_EMULATOR_HANDLE *);
static HRESULT (WINAPI *EmulatorMmio)(WHV_EMULATOR_HANDLE, VOID *,
                                      const WHV_VP_EXIT_CONTEXT *,
                                      const WHV_MEMORY_ACCESS_CONTEXT *,
                                      WHV_EMULATOR_STATUS *);
static HRESULT (WINAPI *EmulatorIo)(WHV_EMULATOR_HANDLE, VOID *,
                                    const WHV_VP_EXIT_CONTEXT *,
                                    const WHV_X64_IO_PORT_ACCESS_CONTEXT *,
                                    WHV_EMULATOR_STATUS *);
static HRESULT (WINAPI *CancelRun)(WHV_PARTITION_HANDLE, UINT32, UINT32);
static HRESULT (WINAPI *TranslateGva)(WHV_PARTITION_HANDLE, UINT32, UINT64,
                                      WHV_TRANSLATE_GVA_FLAGS,
                                      WHV_TRANSLATE_GVA_RESULT *,
                                      WHV_GUEST_PHYSICAL_ADDRESS *);
static HRESULT (WINAPI *UnmapGpaRange)(WHV_PARTITION_HANDLE,
                                       WHV_GUEST_PHYSICAL_ADDRESS, UINT64);
static HRESULT (WINAPI *CreateVirtualProcessor)(WHV_PARTITION_HANDLE, UINT32,
                                                UINT32);
static HRESULT (WINAPI *DeleteVirtualProcessor)(WHV_PARTITION_HANDLE, UINT32);
static HRESULT (WINAPI *RunVirtualProcessor)(WHV_PARTITION_HANDLE, UINT32,
                                             VOID *, UINT32);
static HRESULT (WINAPI *SetRegisters)(WHV_PARTITION_HANDLE, UINT32,
                                      const WHV_REGISTER_NAME *, UINT32,
                                      const WHV_REGISTER_VALUE *);
static HRESULT (WINAPI *GetRegisters)(WHV_PARTITION_HANDLE, UINT32,
                                      const WHV_REGISTER_NAME *, UINT32,
                                      WHV_REGISTER_VALUE *);

static bool FindPlatform()
{
    HMODULE Library = LoadLibraryA("WinHvPlatform.dll");

    if (Library == nullptr)
        return false;

    /*
     * Named one at a time rather than worked out from the name here, because
     * two of these are not called what the thing they do is called.
     */
#define BIND(Name, Called) \
    *reinterpret_cast<void **>(&Name) = \
        reinterpret_cast<void *>(GetProcAddress(Library, Called)); \
    if (Name == nullptr) \
    { \
        printf("the platform library has no %s\n", Called); \
        return false; \
    }

    BIND(CreatePartition, "WHvCreatePartition");
    BIND(DeletePartition, "WHvDeletePartition");
    BIND(SetupPartition, "WHvSetupPartition");
    BIND(SetPartitionProperty, "WHvSetPartitionProperty");
    BIND(MapGpaRange, "WHvMapGpaRange");
    BIND(UnmapGpaRange, "WHvUnmapGpaRange");
    BIND(TranslateGva, "WHvTranslateGva");
    BIND(CreateVirtualProcessor, "WHvCreateVirtualProcessor");
    BIND(DeleteVirtualProcessor, "WHvDeleteVirtualProcessor");
    BIND(RunVirtualProcessor, "WHvRunVirtualProcessor");
    BIND(CancelRun, "WHvCancelRunVirtualProcessor");
    BIND(SetRegisters, "WHvSetVirtualProcessorRegisters");
    BIND(GetRegisters, "WHvGetVirtualProcessorRegisters");

    /*
     * The thing that reads an instruction and carries it out, which lives in a
     * library of its own. A processor stopped on a place in memory is not told
     * what it was doing there, so without this there is no way to know what to
     * hand back or where to put it.
     */
    const HMODULE Emulation = LoadLibraryA("WinHvEmulation.dll");

    if (Emulation == nullptr)
    {
        printf("this machine cannot work out what an instruction was doing\n");
        return false;
    }

    Library = Emulation;

    BIND(EmulatorCreate, "WHvEmulatorCreateEmulator");
    BIND(EmulatorMmio, "WHvEmulatorTryMmioEmulation");
    BIND(EmulatorIo, "WHvEmulatorTryIoEmulation");
#undef BIND

    return true;
}

/* WHAT THE MACHINE IS ********************************************************/

/* As much memory as a firmware of this kind expects to find below it */
#define RUNNER_MEMORY   0x20000000ull

/* And how far past the top of it is still there to be landed on */
#define RUNNER_SLACK    0x01000000ull

/* And where everything stops, which is where a firmware answers itself from */
#define RUNNER_TOP      0x100000000ull

/*
 * Where a processor is when it comes out of reset: the top of the first
 * megabyte, reached through a segment whose hidden base is the top of
 * everything rather than what its number says.
 */
#define RESET_SEGMENT   0xF000
#define RESET_OFFSET    0xFFF0
#define RESET_BASE      0xFFFF0000ull

/*
 * What it was holding when it stopped. A value that was worked out rather than
 * read from somewhere is still in a register, so this is what says where an
 * address the firmware jumped to came from.
 */
/*
 * What is under the stack pointer. The firmware is running its own code with
 * its own stack in the memory it was given, so the addresses on it are the run
 * of calls that led to wherever it stopped, and every one of them is a place
 * in the image that can be looked at.
 */
static void SayStack(const void *Ram, ULONG64 Extent, ULONG64 Rsp)
{
    if ((Rsp + 64) > Extent)
        return;

    const auto *Frame =
        reinterpret_cast<const ULONG *>((const UCHAR *)Ram + Rsp);

    printf("  stack at %08llx:", (unsigned long long)Rsp);

    for (ULONG Index = 0; Index < 16; Index++)
    {
        if ((Index % 8) == 0)
            printf("\n   ");

        printf(" %08lx", Frame[Index]);
    }

    printf("\n");
}

static void SayRegisters(WHV_PARTITION_HANDLE Partition, ULONG64 *Rsp)
{
    static const WHV_REGISTER_NAME Wanted[] =
    {
        WHvX64RegisterRax, WHvX64RegisterRbx, WHvX64RegisterRcx,
        WHvX64RegisterRdx, WHvX64RegisterRsi, WHvX64RegisterRdi,
        WHvX64RegisterRsp, WHvX64RegisterRbp, WHvX64RegisterCr0,
        WHvX64RegisterCr3, WHvX64RegisterCr4
    };

    static const char *const Called[] =
    {
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
        "rsp", "rbp", "cr0", "cr3", "cr4"
    };

    WHV_REGISTER_VALUE Held[ARRAYSIZE(Wanted)] = {};

    if (FAILED(GetRegisters(Partition, 0, Wanted, ARRAYSIZE(Wanted), Held)))
        return;

    for (ULONG Index = 0; Index < ARRAYSIZE(Wanted); Index++)
    {
        printf("  %s %016llx%s", Called[Index],
               (unsigned long long)Held[Index].Reg64,
               ((Index % 3) == 2) ? "\n" : "");
    }

    printf("\n");

    if (Rsp != nullptr)
        *Rsp = Held[6].Reg64;
}

/* A MACHINE SOMETHING CAN BE PUT INTO ****************************************/

/*
 * The same machine as below, taken apart so that the other half of this program
 * can put real hardware into it. What that half has is the contract; what this
 * half has is the processor and the memory, and neither needs to know how the
 * other works.
 */
static WHV_PARTITION_HANDLE TheMachine = nullptr;

/* Whether the faults a guest takes stop it, which is for watching one go wrong */
static bool TheWatchFaults = false;

void Faults(bool Watching) { TheWatchFaults = Watching; }
static UCHAR *TheMemory = nullptr;
static ULONG64 TheExtent = 0;

bool Open(ULONG64 Ram)
{
    if (!FindPlatform())
    {
        printf("this machine cannot make machines\n");
        return false;
    }

    if (FAILED(CreatePartition(&TheMachine)))
        return false;

    WHV_PARTITION_PROPERTY Property = {};

    Property.ProcessorCount = 1;
    SetPartitionProperty(TheMachine, WHvPartitionPropertyCodeProcessorCount,
                         &Property, sizeof(Property));

    /*
     * The faults a guest takes are its own business and never stop the
     * processor, which is right until a guest starts taking one over and over.
     * Asking for these to stop it as well is the only way to see that at all,
     * and they are the ones a guest going wrong takes.
     */
    if (TheWatchFaults)
    {
        WHV_PARTITION_PROPERTY Taken = {};

        Taken.ExceptionExitBitmap =
            (1u << 6) | (1u << 13) | (1u << 14) | (1u << 8);

        SetPartitionProperty(TheMachine,
                             WHvPartitionPropertyCodeExceptionExitBitmap,
                             &Taken, sizeof(Taken));
    }

    if (FAILED(SetupPartition(TheMachine)))
        return false;

    TheMemory = static_cast<UCHAR *>(VirtualAlloc(nullptr, (SIZE_T)Ram,
                                                  MEM_COMMIT | MEM_RESERVE,
                                                  PAGE_READWRITE));

    if (TheMemory == nullptr)
        return false;

    const auto Whole = static_cast<WHV_MAP_GPA_RANGE_FLAGS>(
        WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite |
        WHvMapGpaRangeFlagExecute);

    if (FAILED(MapGpaRange(TheMachine, TheMemory, 0, Ram, Whole)))
        return false;

    TheExtent = Ram;
    return FAILED(CreateVirtualProcessor(TheMachine, 0, 0)) ? false : true;
}

/*
 * Made to come back out, from another thread.
 *
 * A processor let go of does not have to stop. A guest in a loop that reaches
 * for nothing outside itself runs forever without the machine hearing anything
 * of it, and whoever let it go is inside the call the whole time: no count of
 * stops bounds that, because there are none. So something else has to reach in.
 */
bool Interrupt()
{
    if ((TheMachine == nullptr) || (CancelRun == nullptr))
        return false;

    return SUCCEEDED(CancelRun(TheMachine, 0, 0));
}

/* How much memory there is, for anything that walks the whole of it */
ULONG64 Extent() { return TheExtent; }

/* Where a device that answers for memory of its own has its window taken out */
bool Hollow(ULONG64 Where, ULONG64 Length)
{
    if ((TheMachine == nullptr) || (Where + Length > TheExtent))
        return false;

    return SUCCEEDED(UnmapGpaRange(TheMachine, Where, Length));
}

/*
 * A device's own memory, put where the guest will find it. What was mapped
 * there before has to be taken out first, which is the caller's business
 * because only it knows whether it meant to cover something.
 */
bool Fill(ULONG64 Where, ULONG64 Length, void *Backing, bool ReadOnly)
{
    if ((TheMachine == nullptr) || (Backing == nullptr))
        return false;

    ULONG Allowed = WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagExecute;

    if (!ReadOnly)
        Allowed |= WHvMapGpaRangeFlagWrite;

    return SUCCEEDED(MapGpaRange(TheMachine, Backing, Where, Length,
                                 (WHV_MAP_GPA_RANGE_FLAGS)Allowed));
}

/* Where a guest address really is, asked of the processor holding the tables */
bool Reachable(ULONG64 Address, ULONG64 *Physical)
{
    WHV_TRANSLATE_GVA_RESULT Went = {};
    WHV_GUEST_PHYSICAL_ADDRESS Found = 0;

    if ((TheMachine == nullptr) || (TranslateGva == nullptr))
        return false;

    const HRESULT Status = TranslateGva(TheMachine, 0, Address,
                                        WHvTranslateGvaFlagValidateRead,
                                        &Went, &Found);

    if (FAILED(Status) || (Went.ResultCode != WHvTranslateGvaResultSuccess))
        return false;

    *Physical = Found;
    return true;
}

bool Place(const void *Image, ULONG Length, ULONG64 Where)
{
    if ((TheMemory == nullptr) || ((Where + Length) > TheExtent))
        return false;

    memcpy(TheMemory + Where, Image, Length);
    return true;
}

/*
 * Put where a processor of this kind is when it comes out of reset, except that
 * the segment it starts in is given rather than assumed: a firmware that lives
 * in the last of the first megabyte is reached through a base that says so,
 * and is not the same as one that answers from the top of everything.
 */
bool Start(USHORT Selector, ULONG64 Base, ULONG64 Rip)
{
    static const WHV_REGISTER_NAME Named[] =
    {
        WHvX64RegisterCs, WHvX64RegisterRip, WHvX64RegisterRflags
    };

    WHV_REGISTER_VALUE Values[ARRAYSIZE(Named)] = {};

    Values[0].Segment.Base = Base;
    Values[0].Segment.Limit = 0xFFFF;
    Values[0].Segment.Selector = Selector;
    Values[0].Segment.Attributes = 0x009B;

    Values[1].Reg64 = Rip;
    Values[2].Reg64 = 0x0002;

    return SUCCEEDED(SetRegisters(TheMachine, 0, Named, ARRAYSIZE(Named),
                                  Values));
}

bool Step(WHV_RUN_VP_EXIT_CONTEXT *Exit)
{
    return SUCCEEDED(RunVirtualProcessor(TheMachine, 0, Exit, sizeof(*Exit)));
}

/* Several at once, which is how the instruction reader asks for them */
bool Many(const WHV_REGISTER_NAME *Names, ULONG Count,
          WHV_REGISTER_VALUE *Values)
{
    return SUCCEEDED(GetRegisters(TheMachine, 0, Names, Count, Values));
}

bool Put(const WHV_REGISTER_NAME *Names, ULONG Count,
         const WHV_REGISTER_VALUE *Values)
{
    return SUCCEEDED(SetRegisters(TheMachine, 0, Names, Count, Values));
}

/* And a page of it, which the reader wants in its own shape */
bool Walk(ULONG64 Address, WHV_TRANSLATE_GVA_FLAGS Flags,
          WHV_TRANSLATE_GVA_RESULT_CODE *Result,
          WHV_GUEST_PHYSICAL_ADDRESS *Physical)
{
    WHV_TRANSLATE_GVA_RESULT Went = {};

    if (FAILED(TranslateGva(TheMachine, 0, Address, Flags, &Went, Physical)))
        return false;

    *Result = Went.ResultCode;
    return true;
}

bool Reader(const WHV_EMULATOR_CALLBACKS *Ways, WHV_EMULATOR_HANDLE *Made)
{
    return SUCCEEDED(EmulatorCreate(Ways, Made));
}

bool Mmio(WHV_EMULATOR_HANDLE Made, const WHV_VP_EXIT_CONTEXT *Where,
          const WHV_MEMORY_ACCESS_CONTEXT *What, WHV_EMULATOR_STATUS *How)
{
    return SUCCEEDED(EmulatorMmio(Made, nullptr, Where, What, How));
}

/*
 * A port access the same way, which is what a run of them needs. How far the
 * pointers move and where they wrap is the instruction's business and depends
 * on how wide the addresses are where it is running, so it is not worked out
 * here.
 */
bool Io(WHV_EMULATOR_HANDLE Made, const WHV_VP_EXIT_CONTEXT *Where,
        const WHV_X64_IO_PORT_ACCESS_CONTEXT *What, WHV_EMULATOR_STATUS *How)
{
    return SUCCEEDED(EmulatorIo(Made, nullptr, Where, What, How));
}

bool Poke(WHV_REGISTER_NAME Which, ULONG64 Value)
{
    WHV_REGISTER_VALUE Held = {};

    Held.Reg64 = Value;

    return SUCCEEDED(SetRegisters(TheMachine, 0, &Which, 1, &Held));
}

ULONG64 Peek(WHV_REGISTER_NAME Which)
{
    WHV_REGISTER_VALUE Held = {};

    if (FAILED(GetRegisters(TheMachine, 0, &Which, 1, &Held)))
        return 0;

    return Held.Reg64;
}

void *Guest(ULONG64 Where, ULONG Length)
{
    if ((TheMemory == nullptr) || ((Where + Length) > TheExtent))
        return nullptr;

    return TheMemory + Where;
}

void Tell()
{
    if (TheMachine != nullptr)
        SayRegisters(TheMachine, nullptr);
}

void Close()
{
    if (TheMachine == nullptr)
        return;

    DeleteVirtualProcessor(TheMachine, 0);
    DeletePartition(TheMachine);
    TheMachine = nullptr;
}

static void SayExit(const WHV_RUN_VP_EXIT_CONTEXT &Exit, ULONG64 Count)
{
    printf("%6llu  %04x:%016llx  ",
           (unsigned long long)Count,
           Exit.VpContext.Cs.Selector,
           (unsigned long long)Exit.VpContext.Rip);

    switch (Exit.ExitReason)
    {
        case WHvRunVpExitReasonX64IoPortAccess:
            printf("port %04x %s width %u\n",
                   Exit.IoPortAccess.PortNumber,
                   Exit.IoPortAccess.AccessInfo.IsWrite ? "written" : "read",
                   Exit.IoPortAccess.AccessInfo.AccessSize);
            break;

        case WHvRunVpExitReasonMemoryAccess:
        {
            const char *What = "read";

            if (Exit.MemoryAccess.AccessInfo.AccessType == WHvMemoryAccessWrite)
                What = "written";
            else if (Exit.MemoryAccess.AccessInfo.AccessType ==
                     WHvMemoryAccessExecute)
            {
                What = "run from";
            }

            printf("memory %012llx %s\n",
                   (unsigned long long)Exit.MemoryAccess.Gpa, What);
            break;
        }

        case WHvRunVpExitReasonX64MsrAccess:
            printf("register %08x %s\n", Exit.MsrAccess.MsrNumber,
                   Exit.MsrAccess.AccessInfo.IsWrite ? "written" : "read");
            break;

        case WHvRunVpExitReasonX64Cpuid:
            printf("asked what the processor is, %08llx\n",
                   (unsigned long long)Exit.CpuidAccess.Rax);
            break;

        case WHvRunVpExitReasonX64Halt:
            printf("stopped of its own accord\n");
            break;

        case WHvRunVpExitReasonUnrecoverableException:
            printf("gave up\n");
            break;

        case WHvRunVpExitReasonX64InterruptWindow:
            printf("ready for an interrupt\n");
            break;

        default:
            printf("exit of kind %u\n", Exit.ExitReason);
            break;
    }
}

/**
 * @brief
 * Puts a firmware into a machine with nothing in it and watches it come up.
 *
 * @remarks
 * The image is laid against the top of everything, because that is where a
 * processor looks first and where a firmware of any age answers itself from.
 * Anything it reaches for that is not there is said out loud rather than
 * answered, which is the whole point of running it here.
 */
int Run(const char *Path, ULONG Steps, ULONG64 Ram)
{
    if (!FindPlatform())
    {
        printf("the platform library is not here, so there is no machine\n");
        return 1;
    }

    const HANDLE File = CreateFileA(Path, GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);

    if (File == INVALID_HANDLE_VALUE)
    {
        printf("no firmware at %s\n", Path);
        return 1;
    }

    LARGE_INTEGER Size = {};

    GetFileSizeEx(File, &Size);

    const ULONG64 Length = (ULONG64)Size.QuadPart;

    printf("firmware %s, %llu bytes\n", Path, (unsigned long long)Length);

    void *Rom = VirtualAlloc(nullptr, (SIZE_T)Length, MEM_COMMIT | MEM_RESERVE,
                             PAGE_READWRITE);
    DWORD Read = 0;

    if ((Rom == nullptr) ||
        !ReadFile(File, Rom, (DWORD)Length, &Read, nullptr) ||
        (Read != Length))
    {
        printf("it would not read\n");
        CloseHandle(File);
        return 1;
    }

    CloseHandle(File);

    WHV_PARTITION_HANDLE Partition = nullptr;

    if (FAILED(CreatePartition(&Partition)))
    {
        printf("a machine could not be made\n");
        return 1;
    }

    WHV_PARTITION_PROPERTY Property = {};

    Property.ProcessorCount = 1;
    SetPartitionProperty(Partition, WHvPartitionPropertyCodeProcessorCount,
                         &Property, sizeof(Property));

    if (FAILED(SetupPartition(Partition)))
    {
        printf("the machine would not be set up\n");
        return 1;
    }

    /*
     * The memory it has, and the firmware above everything it has.
     *
     * More is mapped than is said to be there. A firmware of this kind works
     * out where the top is and then puts the memory it keeps for good at that
     * address, so the top itself is landed on rather than stopped at, and a
     * machine whose last byte is the last byte is one it falls off.
     */
    const ULONG64 Extent = (Ram != 0) ? Ram : RUNNER_MEMORY;
    const ULONG64 Mapped = Extent + RUNNER_SLACK;

    void *Below = VirtualAlloc(nullptr, (SIZE_T)Mapped,
                               MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    const auto Whole = static_cast<WHV_MAP_GPA_RANGE_FLAGS>(
        WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite |
        WHvMapGpaRangeFlagExecute);
    /*
     * The firmware is given as much say over where it sits as over anything
     * else. On a board it would be read only and the scratch it uses early on
     * would be somewhere else; here there is nowhere else yet, and a firmware
     * stopped on its first write says nothing about what it would have done.
     */
    const auto Fixed = static_cast<WHV_MAP_GPA_RANGE_FLAGS>(
        WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite |
        WHvMapGpaRangeFlagExecute);

    if ((Below == nullptr) ||
        FAILED(MapGpaRange(Partition, Below, 0, Mapped, Whole)))
    {
        printf("the machine has no memory\n");
        return 1;
    }

    printf("memory 000000000000 to %012llx, and %012llx past it\n",
           (unsigned long long)(Extent - 1),
           (unsigned long long)RUNNER_SLACK);

    const ULONG64 Where = RUNNER_TOP - Length;

    if (FAILED(MapGpaRange(Partition, Rom, Where, Length, Fixed)))
    {
        printf("the firmware would not go at %012llx\n",
               (unsigned long long)Where);
        return 1;
    }

    /*
     * And nowhere else. A firmware of this age runs from up here and nothing
     * else: putting a copy of it down in the first megabyte as well, the way
     * an older one is answered, only gives a wrong address somewhere to land.
     */
    printf("firmware %012llx to %012llx\n",
           (unsigned long long)Where, (unsigned long long)(RUNNER_TOP - 1));

    if (FAILED(CreateVirtualProcessor(Partition, 0, 0)))
    {
        printf("the machine has no processor\n");
        return 1;
    }

    /* Out of reset, the way a processor of this kind always comes out of it */
    WHV_REGISTER_NAME Names[2] = { WHvX64RegisterCs, WHvX64RegisterRip };
    WHV_REGISTER_VALUE Values[2] = {};

    Values[0].Segment.Base = RESET_BASE;
    Values[0].Segment.Limit = 0xFFFF;
    Values[0].Segment.Selector = RESET_SEGMENT;
    Values[0].Segment.SegmentType = 0x0B;
    Values[0].Segment.NonSystemSegment = 1;
    Values[0].Segment.Present = 1;
    Values[1].Reg64 = RESET_OFFSET;

    SetRegisters(Partition, 0, Names, 2, Values);

    printf("running from %04x:%04x, which is %012llx\n\n",
           RESET_SEGMENT, RESET_OFFSET,
           (unsigned long long)(RESET_BASE + RESET_OFFSET));

    /*
     * Every stop is said out loud. Nothing is answered: a port read comes back
     * as whatever was already in the register and a write goes nowhere, which
     * is exactly what a machine with nothing in it does.
     */
    WHV_RUN_VP_EXIT_CONTEXT Exit = {};
    ULONG64 Count = 0;
    ULONG64 Ports = 0;
    ULONG64 Memory = 0;

    while (Count < Steps)
    {
        if (FAILED(RunVirtualProcessor(Partition, 0, &Exit, sizeof(Exit))))
        {
            printf("the processor would not run\n");
            break;
        }

        Count++;

        /* The first of each kind, and then every hundredth, or it is a flood */
        if ((Count <= 40) || ((Count % 500) == 0))
            SayExit(Exit, Count);

        if (Exit.ExitReason == WHvRunVpExitReasonX64IoPortAccess)
            Ports++;
        else if (Exit.ExitReason == WHvRunVpExitReasonMemoryAccess)
            Memory++;

        if ((Exit.ExitReason == WHvRunVpExitReasonX64Halt) ||
            (Exit.ExitReason == WHvRunVpExitReasonUnrecoverableException) ||
            (Exit.ExitReason == WHvRunVpExitReasonNone))
        {
            break;
        }

        /*
         * Past the instruction that stopped, because nothing here carries it
         * out. A firmware that wanted an answer gets none and moves on, which
         * is how far it gets before it needs one it cannot do without.
         */
        const WHV_REGISTER_NAME Onward = WHvX64RegisterRip;
        WHV_REGISTER_VALUE Next = {};

        Next.Reg64 = Exit.VpContext.Rip + Exit.VpContext.InstructionLength;

        if (Exit.VpContext.InstructionLength == 0)
            break;

        SetRegisters(Partition, 0, &Onward, 1, &Next);
    }

    printf("\nstopped after %llu, at %04x:%016llx\n",
           (unsigned long long)Count,
           Exit.VpContext.Cs.Selector,
           (unsigned long long)Exit.VpContext.Rip);
    printf("  it reached for %llu port(s) and %llu place(s) in memory\n",
           (unsigned long long)Ports, (unsigned long long)Memory);

    ULONG64 Stack = 0;

    SayRegisters(Partition, &Stack);
    SayStack(Below, Mapped, Stack);

    DeleteVirtualProcessor(Partition, 0);
    DeletePartition(Partition);
    return 0;
}

} /* namespace hv */
