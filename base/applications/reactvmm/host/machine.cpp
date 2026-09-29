/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     One virtual machine, from its memory up to the loop that runs it
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "vdevhost.h"
#include "acpi.h"

#include <winhvplatform.h>
#include <winhvemulation.h>

#include <stdio.h>
#include <string.h>

namespace rtvm
{

/*
 * The platform is bound at run time, so a manager built here still starts
 * where the library is a different one, and says so rather than not loading.
 */
namespace platform
{
    HRESULT (WINAPI *GetCapability)(WHV_CAPABILITY_CODE, VOID *, UINT32, UINT32 *);
    HRESULT (WINAPI *CreatePartition)(WHV_PARTITION_HANDLE *);
    HRESULT (WINAPI *DeletePartition)(WHV_PARTITION_HANDLE);
    HRESULT (WINAPI *SetupPartition)(WHV_PARTITION_HANDLE);
    HRESULT (WINAPI *SetPartitionProperty)(WHV_PARTITION_HANDLE, WHV_PARTITION_PROPERTY_CODE, const VOID *, UINT32);

    /* Handing one to a processor's own controller, when there is one */
    HRESULT (WINAPI *RequestInterrupt)(WHV_PARTITION_HANDLE, const WHV_INTERRUPT_CONTROL *, UINT32);
    HRESULT (WINAPI *MapGpaRange)(WHV_PARTITION_HANDLE, VOID *, WHV_GUEST_PHYSICAL_ADDRESS, UINT64, WHV_MAP_GPA_RANGE_FLAGS);
    HRESULT (WINAPI *CreateVirtualProcessor)(WHV_PARTITION_HANDLE, UINT32, UINT32);
    HRESULT (WINAPI *DeleteVirtualProcessor)(WHV_PARTITION_HANDLE, UINT32);
    HRESULT (WINAPI *RunVirtualProcessor)(WHV_PARTITION_HANDLE, UINT32, VOID *, UINT32);
    HRESULT (WINAPI *CancelRunVirtualProcessor)(WHV_PARTITION_HANDLE, UINT32, UINT32);
    HRESULT (WINAPI *SetRegisters)(WHV_PARTITION_HANDLE, UINT32, const WHV_REGISTER_NAME *, UINT32, const WHV_REGISTER_VALUE *);
    HRESULT (WINAPI *GetRegisters)(WHV_PARTITION_HANDLE, UINT32, const WHV_REGISTER_NAME *, UINT32, WHV_REGISTER_VALUE *);
    HRESULT (WINAPI *TranslateGva)(WHV_PARTITION_HANDLE, UINT32, WHV_GUEST_VIRTUAL_ADDRESS, WHV_TRANSLATE_GVA_FLAGS, WHV_TRANSLATE_GVA_RESULT *, WHV_GUEST_PHYSICAL_ADDRESS *);

    /*
     * Carrying out the instruction that faulted on a window a device answers
     * for. The processor says which instruction it was and nothing about what
     * it meant, so something has to read it, and that something is here rather
     * than in this project.
     */
    HRESULT (WINAPI *CreateEmulator)(const WHV_EMULATOR_CALLBACKS *, WHV_EMULATOR_HANDLE *);
    HRESULT (WINAPI *DestroyEmulator)(WHV_EMULATOR_HANDLE);
    HRESULT (WINAPI *TryMmioEmulation)(WHV_EMULATOR_HANDLE, VOID *, const WHV_VP_EXIT_CONTEXT *, const WHV_MEMORY_ACCESS_CONTEXT *, WHV_EMULATOR_STATUS *);
}

/*
 * Where a machine that starts the old way finds its firmware. The last sixty
 * four kilobytes below a megabyte are the firmware's, and the processor comes
 * out of reset sixteen bytes from the top of them.
 */
/*
 * Where a firmware image is put, and the most of one this machine has room
 * for. The processor comes out of reset at the top of this, so an image is
 * always laid against the end of it rather than the start: what matters is
 * that the last sixteen bytes of the image are the last sixteen bytes here.
 *
 * Ours is a single segment and wants only the last sixty four thousand of
 * this. One written for a machine of this kind and not for this one is
 * usually twice that, and brings its own answers to everything this manager
 * would otherwise have had to describe.
 */
constexpr ULONG64 FirmwareBase = 0x000E0000;
constexpr ULONG64 FirmwareSize = 0x00020000;

/* Past this much of one, a firmware is taken to be somebody else's */
constexpr ULONG64 OwnFirmwareSize = 0x00010000;
/*
 * Where the processor starts, which is the top of the first megabyte however
 * much of a firmware is laid out below it. The segment it starts in is not
 * where the image begins: an image twice this long reaches back past it.
 */
/* Where addressing stops, which is where a firmware answers itself from */
constexpr ULONG64 TopOfMemory = 0x100000000ull;

constexpr ULONG64 ResetBase = 0x000F0000;
constexpr USHORT ResetSegment = 0xF000;
constexpr USHORT ResetOffset = 0xFFF0;

/* Where the manager leaves the firmware a note about the machine */
constexpr ULONG64 MachineDescription = 0x00000500;

/* How many things nothing answers for are worth naming, the rest being counted */
constexpr ULONG HolesToName = 8;

/* And how much port traffic is worth saying out loud before it is only noise */
constexpr ULONG WatchLimit = 20000;

Machine::Machine()
{
    InitializeCriticalSection(&m_ChipLock);
}

Machine::~Machine()
{
    if ((m_Emulator != nullptr) && (platform::DestroyEmulator != nullptr))
        platform::DestroyEmulator(m_Emulator);

    /* The devices go before the partition they were answering for */
    m_Devices.Reset();

    if (m_Partition != nullptr)
    {
        for (ULONG Index = 0; Index < m_ProcessorCount; Index++)
            platform::DeleteVirtualProcessor(m_Partition, Index);

        platform::DeletePartition(m_Partition);
        m_Partition = nullptr;
    }

    /* After the partition, which was the only thing still reaching into them */
    for (const Aperture &One : m_Apertures)
    {
        if (One.Where != nullptr)
            VirtualFree(One.Where, 0, MEM_RELEASE);
    }

    DeleteCriticalSection(&m_ChipLock);
}


/* CARRYING OUT THE INSTRUCTION THAT FAULTED **********************************/

/*
 * What the instruction reader is given so it can reach the machine, and which
 * of its processors faulted.
 */
struct Faulted
{
    Machine *Owner;
    ULONG Index;
};

static HRESULT CALLBACK EmulatedMemory(VOID *Context,
                                       WHV_EMULATOR_MEMORY_ACCESS_INFO *Access)
{
    Machine &Owner = *static_cast<Faulted *>(Context)->Owner;


    /*
     * Direction is which way the instruction meant to move it: nothing for a
     * read, because whatever is there is being asked, and something for a
     * write.
     *
     * Both halves of an instruction come through here, not only the one that
     * faulted, because what reads the instruction cannot reach the machine's
     * memory by itself. One that moves a run of bytes from memory into a
     * device window asks here for the memory as well, and answering only for
     * the device leaves it carrying nothing.
     */
    if (Access->Direction == 0)
    {
        if (!Owner.SystemBus().ReadMemory(Access->GpaAddress,
                                          Access->AccessSize,
                                          Access->Data) &&
            !Owner.MemoryBlock().Read(Access->GpaAddress, Access->Data,
                                      Access->AccessSize))
        {
            /* Nothing is there, and a bus with nothing on it reads as ones */
            memset(Access->Data, 0xFF, Access->AccessSize);
        }

        Owner.WatchMemory(Access->GpaAddress, Access->AccessSize, false,
                          Access->Data);
        return S_OK;
    }

    Owner.WatchMemory(Access->GpaAddress, Access->AccessSize, true,
                      Access->Data);

    if (Owner.SystemBus().WriteMemory(Access->GpaAddress, Access->AccessSize,
                                      Access->Data))
    {
        return S_OK;
    }

    /* And a write into a hole is swallowed, rather than refused */
    Owner.MemoryBlock().Write(Access->GpaAddress, Access->Data,
                              Access->AccessSize);
    return S_OK;
}

static HRESULT CALLBACK EmulatedIoPort(VOID *Context,
                                       WHV_EMULATOR_IO_ACCESS_INFO *Access)
{
    auto *Where = static_cast<Faulted *>(Context);

    if (Access->Direction == 0)
    {
        Access->Data = Where->Owner->ReadPort(Access->Port, Access->AccessSize);
        return S_OK;
    }

    Where->Owner->WritePort(Access->Port, Access->AccessSize, Access->Data);
    return S_OK;
}

static HRESULT CALLBACK EmulatedGetRegisters(VOID *Context,
                                             const WHV_REGISTER_NAME *Names,
                                             UINT32 Count,
                                             WHV_REGISTER_VALUE *Values)
{
    auto *Where = static_cast<Faulted *>(Context);

    return Where->Owner->ReadRegisters(Where->Index, Names, Count, Values);
}

static HRESULT CALLBACK EmulatedSetRegisters(VOID *Context,
                                             const WHV_REGISTER_NAME *Names,
                                             UINT32 Count,
                                             const WHV_REGISTER_VALUE *Values)
{
    auto *Where = static_cast<Faulted *>(Context);

    return Where->Owner->WriteRegisters(Where->Index, Names, Count, Values);
}

static HRESULT CALLBACK EmulatedTranslate(VOID *Context,
                                          WHV_GUEST_VIRTUAL_ADDRESS Gva,
                                          WHV_TRANSLATE_GVA_FLAGS Flags,
                                          WHV_TRANSLATE_GVA_RESULT_CODE *Result,
                                          WHV_GUEST_PHYSICAL_ADDRESS *Gpa)
{
    auto *Where = static_cast<Faulted *>(Context);

    return Where->Owner->Translate(Where->Index, Gva, Flags, Result, Gpa);
}

static const WHV_EMULATOR_CALLBACKS EmulatorCallbacks =
{
    sizeof(EmulatorCallbacks),
    0,
    EmulatedIoPort,
    EmulatedMemory,
    EmulatedGetRegisters,
    EmulatedSetRegisters,
    EmulatedTranslate
};

bool Machine::BindPlatform()
{
    static UniqueLibrary Library;

    Library.Reset(LoadLibraryW(L"WinHvPlatform.dll"));

    if (!Library)
    {
        Log(RtvmLogError, "there is no hypervisor platform here\n");
        return false;
    }

    const HMODULE Handle = Library.Get();
    bool Complete = true;

    auto Bind = [&](void **Target, const char *Name, bool Required)
    {
        *Target = reinterpret_cast<void *>(GetProcAddress(Handle, Name));

        if ((*Target == nullptr) && Required)
        {
            Log(RtvmLogError, "the platform has no %s\n", Name);
            Complete = false;
        }
    };

#define BIND(member, name, required) \
    Bind(reinterpret_cast<void **>(&platform::member), name, required)

    BIND(GetCapability, "WHvGetCapability", true);
    BIND(CreatePartition, "WHvCreatePartition", true);
    BIND(DeletePartition, "WHvDeletePartition", true);
    BIND(SetupPartition, "WHvSetupPartition", true);
    BIND(SetPartitionProperty, "WHvSetPartitionProperty", true);
    BIND(MapGpaRange, "WHvMapGpaRange", true);
    BIND(CreateVirtualProcessor, "WHvCreateVirtualProcessor", true);
    BIND(DeleteVirtualProcessor, "WHvDeleteVirtualProcessor", true);
    BIND(RunVirtualProcessor, "WHvRunVirtualProcessor", true);
    BIND(SetRegisters, "WHvSetVirtualProcessorRegisters", true);
    BIND(GetRegisters, "WHvGetVirtualProcessorRegisters", true);

    /* Only needed to stop a processor early, so its absence is not fatal */
    BIND(CancelRunVirtualProcessor, "WHvCancelRunVirtualProcessor", false);
    BIND(RequestInterrupt, "WHvRequestInterrupt", false);
    BIND(TranslateGva, "WHvTranslateGva", false);

#undef BIND

    /*
     * The part that reads instructions lives in a library of its own. A machine
     * whose devices all answer for ports rather than memory never needs it, so
     * not having it is only a reason to refuse a device that wants a window.
     */
    HMODULE Emulator = LoadLibraryA("WinHvEmulation.dll");

    if (Emulator != nullptr)
    {
        platform::CreateEmulator = reinterpret_cast<decltype(platform::CreateEmulator)>(
            reinterpret_cast<void *>(GetProcAddress(Emulator, "WHvEmulatorCreateEmulator")));
        platform::DestroyEmulator = reinterpret_cast<decltype(platform::DestroyEmulator)>(
            reinterpret_cast<void *>(GetProcAddress(Emulator, "WHvEmulatorDestroyEmulator")));
        platform::TryMmioEmulation = reinterpret_cast<decltype(platform::TryMmioEmulation)>(
            reinterpret_cast<void *>(GetProcAddress(Emulator, "WHvEmulatorTryMmioEmulation")));
    }
    else
    {
        Log(RtvmLogWarning,
            "no instruction reader, so nothing may answer for a window\n");
    }

    return Complete;
}

bool Machine::CreatePartition(const Configuration &Config)
{
    WHV_CAPABILITY Capability = {};
    UINT32 Written = 0;

    HRESULT Result = platform::GetCapability(WHvCapabilityCodeHypervisorPresent,
                                             &Capability, sizeof(Capability),
                                             &Written);
    if (FAILED(Result) || !Capability.HypervisorPresent)
    {
        Log(RtvmLogError, "the hypervisor is not running\n");
        return false;
    }

    WHV_PARTITION_HANDLE Partition = nullptr;

    Result = platform::CreatePartition(&Partition);
    if (FAILED(Result))
    {
        Log(RtvmLogError, "no partition, %08lx\n", Result);
        return false;
    }

    m_Partition = Partition;

    WHV_PARTITION_PROPERTY Property = {};

    Property.ProcessorCount = Config.ProcessorCount;
    Result = platform::SetPartitionProperty(Partition,
                                            WHvPartitionPropertyCodeProcessorCount,
                                            &Property, sizeof(Property));
    if (FAILED(Result))
    {
        Log(RtvmLogError, "the partition would not take %lu processor(s), %08lx\n",
            Config.ProcessorCount, Result);
        return false;
    }

    m_ProcessorCount = Config.ProcessorCount;

    /*
     * A machine of this shape reaches its hardware by port, so those exits are
     * the ones that matter. Asking for the other two is not refused where they
     * are already always taken, so the answer is not checked.
     */
    Property = {};
    Property.ExtendedVmExits.X64CpuidExit = 1;
    Property.ExtendedVmExits.X64MsrExit = 1;
    platform::SetPartitionProperty(Partition,
                                   WHvPartitionPropertyCodeExtendedVmExits,
                                   &Property, sizeof(Property));

    /*
     * Each processor's own controller, answered for by the hypervisor rather
     * than here. A system written since the pair of controllers stopped being
     * enough programs one whether or not anything has said it is there, so a
     * machine without one is a machine such a system stops on.
     */
    Property = {};
    Property.LocalApicEmulationMode = WHvX64LocalApicEmulationModeXApic;

    if (SUCCEEDED(platform::SetPartitionProperty(
            Partition, WHvPartitionPropertyCodeLocalApicEmulationMode,
            &Property, sizeof(Property))))
    {
        m_HaveLocalApic = true;
    }
    else
    {
        Log(RtvmLogWarning,
            "this hypervisor answers for no local controller, so the pair of "
            "them is all there is\n");
    }

    Result = platform::SetupPartition(Partition);
    if (FAILED(Result))
    {
        Log(RtvmLogError, "the partition would not come up, %08lx\n", Result);
        return false;
    }

    if (!m_Memory.Allocate(Config.MemorySize))
    {
        Log(RtvmLogError, "no room for %llu bytes of guest memory\n",
            static_cast<unsigned long long>(Config.MemorySize));
        return false;
    }

    /*
     * The memory is allocated here but not mapped. Mapping waits until the
     * devices have said which windows they answer for, because a window that
     * is mapped as memory is one the guest writes straight into and the device
     * behind it never hears about.
     */
    Log(RtvmLogInfo, "%llu MB of memory, %lu processor(s)\n",
        static_cast<unsigned long long>(Config.MemorySize / (1024 * 1024)),
        Config.ProcessorCount);

    return true;
}

/**
 * @brief
 * Puts the firmware where the processor will come out of reset into it.
 *
 * @remarks
 * The image is placed so that its end sits at the top of the first megabyte,
 * which is what puts the entry point under the reset vector. An image shorter
 * than the window is allowed and simply starts further up.
 */
bool Machine::LoadFirmware(const char *Path)
{
    UniqueFile File(CreateFileA(Path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));

    if (!File)
    {
        Log(RtvmLogError, "no firmware at %s\n", Path);
        return false;
    }

    LARGE_INTEGER Size = {};

    if (!GetFileSizeEx(File.Get(), &Size) || (Size.QuadPart == 0))
    {
        Log(RtvmLogError, "%s is %lld bytes, which is not a firmware image\n",
            Path, static_cast<long long>(Size.QuadPart));
        return false;
    }

    ULONG64 Whole = static_cast<ULONG64>(Size.QuadPart);

    /*
     * An image bigger than there is room for is one built for a board that
     * answers it at the top of everything as well as down here. Only the end
     * of it is reachable at this address, and the end of it is the part with
     * the place the processor starts in, so the front is skipped.
     */
    if (Whole > FirmwareSize)
    {
        LARGE_INTEGER Skip = {};

        Skip.QuadPart = static_cast<LONGLONG>(Whole - FirmwareSize);

        if (!SetFilePointerEx(File.Get(), Skip, nullptr, FILE_BEGIN))
        {
            Log(RtvmLogError, "%s would not be read from the end\n", Path);
            return false;
        }

        Whole = FirmwareSize;
    }

    const ULONG64 Length = Whole;
    const ULONG64 Base = (FirmwareBase + FirmwareSize) - Length;

    /* Anything this big describes the machine itself and needs no help */
    m_OwnFirmware = (Length <= OwnFirmwareSize);
    void *Target = m_Memory.At(Base, Length);

    if (Target == nullptr)
    {
        Log(RtvmLogError, "the machine is too small to hold its own firmware\n");
        return false;
    }

    DWORD Read = 0;

    if (!ReadFile(File.Get(), Target, static_cast<DWORD>(Length), &Read, nullptr) ||
        (Read != Length))
    {
        Log(RtvmLogError, "%s would not read\n", Path);
        return false;
    }

    Log(RtvmLogInfo, "firmware %s, %llu bytes at %05llx\n",
        Path,
        static_cast<unsigned long long>(Length),
        static_cast<unsigned long long>(Base));

    /*
     * And the same bytes again at the very top of what can be addressed.
     *
     * A board of this kind answers its firmware in two places: down here,
     * where a processor still in the mode it starts in can reach it, and up
     * there, where the part of it that has already moved on expects to find
     * itself. Ours has only ever wanted the first. One written for a board
     * rather than for this manager reads itself back out of the second, and
     * finding nothing there is the end of it.
     */
    if (!m_OwnFirmware && (platform::MapGpaRange != nullptr))
    {
        const ULONG64 High = TopOfMemory - Length;
        const auto Flags = static_cast<WHV_MAP_GPA_RANGE_FLAGS>(
            WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagExecute);

        if (FAILED(platform::MapGpaRange(m_Partition, Target, High, Length,
                                         Flags)))
        {
            Log(RtvmLogWarning,
                "the firmware could not also be put at %012llx\n",
                static_cast<unsigned long long>(High));
        }
        else
        {
            Log(RtvmLogTrace, "firmware again at %012llx\n",
                static_cast<unsigned long long>(High));
        }
    }

    return true;
}

/**
 * @brief
 * Puts one processor into the state it would be in coming out of reset.
 *
 * @remarks
 * Real mode, but the code segment's hidden base is the one the processor
 * actually uses and it does not match the selector. That is what lets sixteen
 * bit firmware at the top of the first megabyte be reached through a selector
 * which could not otherwise address it, and getting it wrong is the usual
 * reason a machine of this shape never executes an instruction.
 */
bool Machine::PrepareProcessor(ULONG Index)
{
    HRESULT Result = platform::CreateVirtualProcessor(m_Partition, Index, 0);

    if (FAILED(Result))
    {
        Log(RtvmLogError, "processor %lu would not be made, %08lx\n", Index, Result);
        return false;
    }

    /* Only the first one runs from reset, the rest wait to be started */
    if (Index != 0)
        return true;

    const WHV_REGISTER_NAME Names[] =
    {
        WHvX64RegisterCs, WHvX64RegisterDs, WHvX64RegisterEs,
        WHvX64RegisterFs, WHvX64RegisterGs, WHvX64RegisterSs,
        WHvX64RegisterRip, WHvX64RegisterRflags,
        WHvX64RegisterCr0, WHvX64RegisterRsp
    };

    WHV_REGISTER_VALUE Values[RTL_NUMBER_OF(Names)] = {};

    WHV_X64_SEGMENT_REGISTER Code = {};

    Code.Base = ResetBase;
    Code.Limit = 0xFFFF;
    Code.Selector = ResetSegment;
    Code.SegmentType = 0x0B;
    Code.NonSystemSegment = 1;
    Code.Present = 1;

    WHV_X64_SEGMENT_REGISTER Data = {};

    Data.Base = 0;
    Data.Limit = 0xFFFF;
    Data.Selector = 0;
    Data.SegmentType = 0x03;
    Data.NonSystemSegment = 1;
    Data.Present = 1;

    Values[0].Segment = Code;
    Values[1].Segment = Data;
    Values[2].Segment = Data;
    Values[3].Segment = Data;
    Values[4].Segment = Data;
    Values[5].Segment = Data;
    Values[6].Reg64 = ResetOffset;

    /* Bit one is always set, and interrupts are off until firmware says so */
    Values[7].Reg64 = 0x0002;

    /* Real mode, and the two bits the processor holds down out of reset */
    Values[8].Reg64 = 0x60000010;

    /* Somewhere to put a return address before firmware sets up its own */
    Values[9].Reg64 = 0x7000;

    Result = platform::SetRegisters(m_Partition, Index, Names,
                                    RTL_NUMBER_OF(Names), Values);
    if (FAILED(Result))
    {
        Log(RtvmLogError, "processor %lu would not take its reset state, %08lx\n",
            Index, Result);
        return false;
    }

    return true;
}

/**
 * @brief
 * Leaves the firmware a description of the machine it woke up in.
 *
 * @remarks
 * Firmware on real hardware reads the board to find out what it is running
 * on. There is no board here, so the manager writes down what it built and the
 * firmware reads it from a fixed place.
 *
 * It goes at five hundred because that is the one part of low memory nothing
 * else has ever claimed: the interrupt table is below it and the data area is
 * above it, and it is free again the moment the firmware has read it.
 */
/**
 * @brief
 * Maps the guest's memory, leaving a hole wherever a device answers.
 *
 * @remarks
 * A window that is mapped as memory is one the guest writes straight into, and
 * the device behind it is never told. So the ranges devices claimed are left
 * unmapped: an access to one of them has nowhere to land and comes back to the
 * manager as an exit, which is what gets it to the device.
 *
 * The claims are put in order first, because they are made in whatever order
 * the devices were started and the gaps between them have to be walked from
 * the bottom up.
 */
/**
 * @brief
 * Gives a device a piece of memory and the guest somewhere to reach it.
 *
 * @remarks
 * Not a window a device answers for: the guest reads and writes it without
 * this manager hearing about any of it, which is the whole point. A screen is
 * written far too often for anything else, and a device that asked to be told
 * about every pixel would be told about several million a second.
 */
bool Machine::CreateAperture(ULONG64 Base, ULONG64 Length, void **Where)
{
    if ((Where == nullptr) || (Length == 0))
        return false;

    *Where = nullptr;

    /* Above everything the guest was given, so that nothing is taken away */
    if (Base < m_Memory.Size())
        return false;

    if (m_Apertures.Full() || (m_Partition == nullptr))
        return false;

    void *Block = VirtualAlloc(nullptr, static_cast<SIZE_T>(Length),
                               MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (Block == nullptr)
        return false;

    const auto Flags = static_cast<WHV_MAP_GPA_RANGE_FLAGS>(
        WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite);

    if (FAILED(platform::MapGpaRange(m_Partition, Block, Base, Length, Flags)))
    {
        Log(RtvmLogError, "%012llx for %llx would not be given out\n",
            static_cast<unsigned long long>(Base),
            static_cast<unsigned long long>(Length));
        VirtualFree(Block, 0, MEM_RELEASE);
        return false;
    }

    if (!m_Apertures.Add({ Block, Base, Length }))
    {
        VirtualFree(Block, 0, MEM_RELEASE);
        return false;
    }

    Log(RtvmLogTrace, "aperture %012llx to %012llx\n",
        static_cast<unsigned long long>(Base),
        static_cast<unsigned long long>(Base + Length - 1));

    *Where = Block;
    return true;
}

bool Machine::MapMemory()
{
    struct Hole
    {
        ULONG64 Base;
        ULONG64 Length;
    };

    Array<Hole, MaximumMemoryRanges> Holes;

    for (ULONG Index = 0; Index < m_Bus.ClaimedCount(); Index++)
    {
        Hole One = {};

        m_Bus.ClaimedAt(Index, One.Base, One.Length);

        /* Anything past the end of memory is not a hole in it */
        if (One.Base >= m_Memory.Size())
            continue;

        Holes.Add(One);
    }

    /* In order, by where they start */
    for (ULONG Outer = 0; Outer + 1 < Holes.Count(); Outer++)
    {
        for (ULONG Inner = 0; Inner + 1 < Holes.Count() - Outer; Inner++)
        {
            if (Holes[Inner].Base > Holes[Inner + 1].Base)
            {
                const Hole Swap = Holes[Inner];

                Holes[Inner] = Holes[Inner + 1];
                Holes[Inner + 1] = Swap;
            }
        }
    }

    const auto Flags = static_cast<WHV_MAP_GPA_RANGE_FLAGS>(
        WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute);

    ULONG64 Next = 0;

    for (ULONG Index = 0; Index <= Holes.Count(); Index++)
    {
        const ULONG64 Until = (Index < Holes.Count()) ? Holes[Index].Base
                                                      : m_Memory.Size();

        if (Until > Next)
        {
            const ULONG64 Length = Until - Next;
            void *Where = m_Memory.At(Next, Length);

            if (Where == nullptr)
                return false;

            const HRESULT Result = platform::MapGpaRange(m_Partition, Where,
                                                         Next, Length, Flags);
            if (FAILED(Result))
            {
                Log(RtvmLogError, "%llx for %llx would not map, %08lx\n",
                    static_cast<unsigned long long>(Next),
                    static_cast<unsigned long long>(Length),
                    Result);
                return false;
            }

            Log(RtvmLogTrace, "memory %012llx to %012llx\n",
                static_cast<unsigned long long>(Next),
                static_cast<unsigned long long>(Next + Length - 1));
        }

        if (Index < Holes.Count())
        {
            const ULONG64 End = Holes[Index].Base + Holes[Index].Length;

            Log(RtvmLogTrace, "device %012llx to %012llx\n",
                static_cast<unsigned long long>(Holes[Index].Base),
                static_cast<unsigned long long>(End - 1));

            /* Overlapping claims were refused, so this only ever moves forward */
            if (End > Next)
                Next = End;
        }
    }

    return true;
}

bool Machine::DescribeMachine(const Configuration &Config)
{
    struct Description
    {
        ULONG Magic;
        ULONG Version;
        ULONG64 MemorySize;
        ULONG ProcessorCount;
        ULONG Reserved;
    };

    Description Written = {};

    /* Spells the manager's name, so the firmware can tell it was written */
    Written.Magic = 0x4D565452;
    Written.Version = 1;
    Written.MemorySize = Config.MemorySize;
    Written.ProcessorCount = Config.ProcessorCount;

    if (!m_Memory.Write(MachineDescription, &Written, sizeof(Written)))
    {
        Log(RtvmLogError, "the machine is too small to describe itself\n");
        return false;
    }

    return true;
}

bool Machine::Build(const Configuration &Config)
{
    SetLogLevel(Config.LogLevel);

    /*
     * Counted from when it starts running rather than from here. Building it
     * can wait on something outside, and a machine told to run for a minute
     * should get its minute however long it waited to be let go.
     */
    m_Seconds = Config.RunSeconds;

    m_WatchFirst = Config.WatchFirst;
    m_WatchLast = Config.WatchLast;
    m_SeeFirst = Config.SeeFirst;
    m_SeeLast = Config.SeeLast;

    if (!BindPlatform())
        return false;

    if (!m_Bus.Initialize())
    {
        Log(RtvmLogError, "no room for the port table\n");
        return false;
    }

    if (!CreatePartition(Config))
        return false;


    /*
     * The devices that come out of a class server. They are brought up before
     * the older ones, because the older ones raise lines on the interrupt
     * controller and it has to be there first.
     */
    /*
     * The instruction reader, made before any device asks for a window, so
     * that one asking is either given it or refused rather than given a
     * window nothing can answer for.
     */
    if (platform::CreateEmulator != nullptr)
    {
        WHV_EMULATOR_HANDLE Made = nullptr;

        if (SUCCEEDED(platform::CreateEmulator(&EmulatorCallbacks, &Made)))
            m_Emulator = Made;
        else
            Log(RtvmLogWarning, "the instruction reader would not start\n");
    }

    m_Vdevs.Reset(new VdevHost(*this));

    if (!m_Vdevs)
        return false;

    if (!m_Vdevs->Load("rtvmemulateddevices.dll"))
        return false;

    if (!m_Vdevs->Create(CLSID_PicDevice, "interrupt controller"))
        return false;

    if (!m_Vdevs->Create(CLSID_DmaControllerDevice, "transfer controller"))
        return false;

    /*
     * The bus before anything that wants a place on it, because a device asks
     * for that place while it is reserving what else it answers for.
     */
    if (!m_Vdevs->Create(CLSID_PciBusDevice, "bus"))
        return false;

    if (!m_Vdevs->Load("rtvmchipsetdevices.dll"))
        return false;

    if (!m_Vdevs->Create(CLSID_IoApicDevice, "line router"))
        return false;

    /* Last of these, because its line has to have somewhere to go */
    if (!m_Vdevs->Create(CLSID_PitDevice, "interval timer"))
        return false;

    if (!m_Vdevs->Create(CLSID_VideoS3Device, "display"))
        return false;

    /*
     * The disks, which are one controller and not one device each. What hangs
     * off it came in on the command line as a request for a kind called after
     * the controller rather than after any one drive.
     */
    for (const DeviceRequest &Request : Config.Requests)
    {
        const char *Text = Request.Get();

        if (strncmp(Text, "ide:", 4) != 0)
            continue;

        if (!m_Vdevs->Create(CLSID_IdeControllerDevice, "disk controller",
                             Text + 4))
        {
            return false;
        }
    }

    m_Devices.Reset(new DeviceHost(*this));

    if (!m_Devices)
        return false;

    /*
     * Every module is loaded before any device is made, so that a device may
     * be asked for by kind without caring which module happens to have it.
     */
    static const char *const Modules[] =
    {
        "rtvmserial.dll",
        "rtvmchipset.dll",
        "rtvmkeyboard.dll",
        "rtvmfloppy.dll"
    };

    for (const char *Name : Modules)
    {
        if (!m_Devices->Load(Name))
            Log(RtvmLogWarning, "%s is not here, so its hardware is not\n", Name);
    }

    for (const DeviceRequest &Request : Config.Requests)
    {
        Text<64> ClassName;
        const char *Text = Request.Get();
        const char *Colon = strchr(Text, ':');

        /* Already made, by the kind that comes out of a class server */
        if (strncmp(Text, "ide:", 4) == 0)
            continue;

        if (!ClassName.SetUpTo(Text, ':'))
        {
            Log(RtvmLogError, "%s does not name a kind\n", Text);
            return false;
        }

        if (!m_Devices->Create(ClassName.Get(),
                               (Colon != nullptr) ? (Colon + 1) : nullptr))
        {
            return false;
        }
    }

    if (!m_Devices->StartAll())
        return false;

    if (!m_Vdevs->PowerOnAll())
        return false;

    /* Now that every window is claimed, the rest of it becomes memory */
    if (!MapMemory())
        return false;

    if (!LoadFirmware(Config.FirmwarePath.Get()))
        return false;

    if (!DescribeMachine(Config))
        return false;

    /*
     * And the tables it describes itself with, which a system of any age
     * looks for before it will start at all.
     */
    /*
     * A firmware of somebody else's builds these for itself and puts them
     * where this manager would have, so writing ours over it would be writing
     * over the firmware that is about to run.
     */
    if (m_OwnFirmware && !DescribeWithTables(m_Memory, m_ProcessorCount))
    {
        Log(RtvmLogError, "the tables would not go where they are looked for\n");
        return false;
    }

    for (ULONG Index = 0; Index < m_ProcessorCount; Index++)
    {
        if (!PrepareProcessor(Index))
            return false;
    }

    return true;
}

/*
 * The two chips the board has of its own answer before the bus does, because
 * nothing loadable is allowed to take their addresses away from them.
 */
/* Everything on the bus was reserved by a device and reaches that device */
/*
 * Whether a port is one the operator asked to be told about, and how many
 * they have been told about already. A device that is not being talked to at
 * all looks exactly like one that is being talked to and answering wrongly,
 * and this is the only thing that tells the two apart.
 */
/*
 * The same for a window rather than a port, which is the only way to see a
 * device reached through memory being talked to.
 */
void Machine::WatchMemory(ULONG64 Address, ULONG Width, bool Writing,
                          const void *Data)
{
    if ((m_SeeLast < m_SeeFirst) || (Address < m_SeeFirst) ||
        (Address > m_SeeLast) || (m_Seen >= WatchLimit))
    {
        return;
    }

    ULONG64 Value = 0;

    memcpy(&Value, Data, (Width > sizeof(Value)) ? sizeof(Value) : Width);
    m_Seen++;

    Log(RtvmLogInfo, "    %012llx %s %0*llx\n",
        static_cast<unsigned long long>(Address),
        Writing ? "<-" : "->", (int)(Width * 2),
        static_cast<unsigned long long>(Value));
}

bool Machine::Watched(USHORT Port)
{
    if ((m_WatchLast < m_WatchFirst) || (Port < m_WatchFirst) ||
        (Port > m_WatchLast) || (m_Watched >= WatchLimit))
    {
        return false;
    }

    m_Watched++;
    return true;
}

void Machine::WritePort(USHORT Port, ULONG Width, ULONG Value)
{
    if (Watched(Port))
        Log(RtvmLogInfo, "    %04x <- %0*lx\n", Port, Width * 2, Value);

    m_Bus.WritePort(Port, Width, Value);
}

ULONG Machine::ReadPort(USHORT Port, ULONG Width)
{
    const ULONG Value = m_Bus.ReadPort(Port, Width);

    if (Watched(Port))
        Log(RtvmLogInfo, "    %04x -> %0*lx\n", Port, Width * 2, Value);

    return Value;
}

void Machine::SetInterruptLine(ULONG Line, bool Asserted)
{
    /*
     * Not straight at the interrupt controller. Where a line goes is not the
     * manager's to decide and not the raising device's either, so it goes to
     * the one thing that does decide.
     */
    IVmIoApic *Router = m_Vdevs->Lines();

    if (Asserted)
        Router->AssertIrq((UCHAR)Line, VDEV_IRQ_SOURCE_ONLY);
    else
        Router->DeassertIrq((UCHAR)Line, VDEV_IRQ_SOURCE_ONLY);

}

bool Machine::ReadGuest(ULONG64 Address, void *Buffer, ULONG Length)
{
    return m_Memory.Read(Address, Buffer, Length);
}

bool Machine::WriteGuest(ULONG64 Address, const void *Buffer, ULONG Length)
{
    return m_Memory.Write(Address, Buffer, Length);
}


void Machine::PostInput(RTVM_INPUT_KIND Kind, ULONG Value)
{
    if (m_Devices)
        m_Devices->PostInput(Kind, Value);
}

HRESULT Machine::ReadRegisters(ULONG Index, const WHV_REGISTER_NAME *Names,
                               ULONG Count, WHV_REGISTER_VALUE *Values)
{
    return platform::GetRegisters(m_Partition, Index, Names, Count, Values);
}

HRESULT Machine::WriteRegisters(ULONG Index, const WHV_REGISTER_NAME *Names,
                                ULONG Count, const WHV_REGISTER_VALUE *Values)
{
    return platform::SetRegisters(m_Partition, Index, Names, Count, Values);
}

HRESULT Machine::Translate(ULONG Index, ULONG64 Gva, ULONG Flags,
                           WHV_TRANSLATE_GVA_RESULT_CODE *Result, ULONG64 *Gpa)
{
    if (platform::TranslateGva == nullptr)
        return E_NOTIMPL;

    WHV_TRANSLATE_GVA_RESULT Answer = {};
    const HRESULT Status = platform::TranslateGva(m_Partition, Index, Gva,
                                                  (WHV_TRANSLATE_GVA_FLAGS)Flags,
                                                  &Answer, Gpa);

    if (Result != nullptr)
        *Result = Answer.ResultCode;

    return Status;
}

/**
 * @brief
 * Carries out the instruction that faulted on a window a device answers for.
 *
 * @remarks
 * The processor says which instruction it was and nothing about what it meant:
 * there is no register holding what a write was going to write. Reading the
 * instruction is the only way to find out, and that is what this hands off.
 */
bool Machine::EmulateAccess(ULONG Index, const WHV_RUN_VP_EXIT_CONTEXT &Exit)
{
    if ((m_Emulator == nullptr) || (platform::TryMmioEmulation == nullptr))
        return false;

    Faulted Where = { this, Index };
    WHV_EMULATOR_STATUS Status = {};

    const HRESULT Result = platform::TryMmioEmulation(m_Emulator, &Where,
                                                      &Exit.VpContext,
                                                      &Exit.MemoryAccess,
                                                      &Status);

    if (FAILED(Result) || !Status.EmulationSuccessful)
    {
        Log(RtvmLogWarning,
            "%04x:%08llx would not be read, %08lx, why %08lx\n",
            Exit.VpContext.Cs.Selector, Exit.VpContext.Rip,
            Result, Status.AsUINT32);
        return false;
    }

    return true;
}

IVmDmaController *Machine::Channels() const
{
    return m_Vdevs ? m_Vdevs->Transfers() : nullptr;
}

IVideoVdev *Machine::Screen() const
{
    return m_Vdevs ? m_Vdevs->Screen() : nullptr;
}

ULONG Machine::DeviceCount() const
{
    return m_Devices ? m_Devices->Count() : 0;
}

const char *Machine::DeviceName(ULONG Index) const
{
    return m_Devices->NameAt(Index);
}

void Machine::Snapshot(MachineStatus &Status) const
{
    Status.Rip = m_LastRip;
    Status.Cs = (USHORT)m_LastCs;
    Status.Delivered = m_Delivered;
    Status.Refused = m_Refused;
    Status.Running = (m_Running != 0);

    /*
     * Worked out from what was delivered rather than from what was raised.
     * A device raises its line on the router, which is a device too, so the
     * manager no longer sees every line that moves; what it does see is every
     * vector that went in.
     *
     * Which line a vector came from is only knowable by where the firmware put
     * the two chips' bases. A guest that moved them lights the wrong lamp,
     * which is a smaller price than lighting none.
     */
    for (ULONG Line = 0; Line < RTL_NUMBER_OF(Status.LineCount); Line++)
    {
        const ULONG Vector = (Line < 8) ? (0x08 + Line) : (0x70 + (Line - 8));

        Status.LineCount[Line] = m_VectorCount[Vector];
    }
}

void Machine::Stop()
{
    InterlockedExchange(&m_Stopping, 1);

    if (platform::CancelRunVirtualProcessor != nullptr)
    {
        for (ULONG Index = 0; Index < m_ProcessorCount; Index++)
            platform::CancelRunVirtualProcessor(m_Partition, Index, 0);
    }
}

/**
 * @brief
 * Carries out a string port access, the whole run of it at once.
 *
 * @remarks
 * This is how sectors move. A disk read is one `rep insw` of two hundred and
 * fifty six words, and a machine that only answers a word at a time would take
 * an exit for every one of them.
 *
 * The count, the pointer and the direction all come out of the exit, and all
 * three go back changed, because the instruction is being carried out here
 * rather than re-executed.
 */
/**
 * @brief
 * Where an address a guest instruction was using is in this machine's memory.
 *
 * @remarks
 * A guest that has never built tables of its own is already saying where in
 * memory it means, and asking would be answered with the same number. One that
 * has is saying where in its own arrangement of it, and only the processor
 * knows how to turn that into the other.
 */
bool Machine::Reachable(ULONG Index, ULONG64 Address, bool Writing,
                        ULONG64 *Where)
{
    WHV_TRANSLATE_GVA_RESULT_CODE Result = WHvTranslateGvaResultSuccess;
    const ULONG Flags = Writing ? WHvTranslateGvaFlagValidateWrite
                                : WHvTranslateGvaFlagValidateRead;

    if (FAILED(Translate(Index, Address, Flags, &Result, Where)) ||
        (Result != WHvTranslateGvaResultSuccess))
    {
        return false;
    }

    return true;
}

void Machine::StringPort(ULONG Index, const WHV_RUN_VP_EXIT_CONTEXT &Exit)
{
    const USHORT Port = Exit.IoPortAccess.PortNumber;
    const ULONG Width = Exit.IoPortAccess.AccessInfo.AccessSize;
    const bool IsWrite = Exit.IoPortAccess.AccessInfo.IsWrite != 0;
    const bool Repeated = Exit.IoPortAccess.AccessInfo.RepPrefix != 0;

    /* Without a repeat prefix the instruction moves exactly one */
    ULONG64 Count = Repeated ? (Exit.IoPortAccess.Rcx & 0xFFFF) : 1;

    /* The direction flag says which way the pointer walks */
    const bool Backwards = (Exit.VpContext.Rflags & 0x400) != 0;
    const LONG64 Step = Backwards ? -static_cast<LONG64>(Width)
                                  :  static_cast<LONG64>(Width);

    /*
     * How much of the pointer counts. A guest running the way this machine
     * comes out of reset carries sixteen bits of it and the rest is somebody
     * else's; one that has moved on carries all of it, and taking only the
     * low half would leave the pointer somewhere near the bottom of memory
     * and the transfer landing on whatever is there.
     */
    const bool Wide = (Exit.VpContext.Cs.Long != 0) ||
                      (Exit.VpContext.Cs.Default != 0);
    const ULONG64 Reach = Wide ? 0xFFFFFFFFull : 0xFFFFull;

    /*
     * Reads land through the extra segment and writes come out of the data
     * one, and the segment is the base the processor is using rather than the
     * selector, so the exit's own copy is what counts.
     */
    ULONG64 Address = IsWrite
                    ? (Exit.IoPortAccess.Ds.Base + (Exit.IoPortAccess.Rsi & Reach))
                    : (Exit.IoPortAccess.Es.Base + (Exit.IoPortAccess.Rdi & Reach));

    ULONG64 Moved = 0;

    /*
     * Where the last page asked about turned out to be. A run of these stays
     * in one page for most of its length, and asking again for every word
     * would cost more than the exit this is here to avoid.
     */
    ULONG64 Page = ~0ull;
    ULONG64 Behind = 0;

    while (Moved < Count)
    {
        ULONG64 Where = 0;

        /* A run that reaches the end of a page carries on in another */
        if ((Address & ~(VDEV_PAGE_SIZE - 1ull)) != Page)
        {
            if (!Reachable(Index, Address, IsWrite, &Where))
                break;

            Page = Address & ~(VDEV_PAGE_SIZE - 1ull);
            Behind = Where - Address;
        }
        else
        {
            Where = Address + Behind;
        }

        /* One that steps over the edge in the middle of a word is refused */
        if (((Address & (VDEV_PAGE_SIZE - 1ull)) + Width) > VDEV_PAGE_SIZE)
            break;

        if (IsWrite)
        {
            ULONG Value = 0;

            if (!m_Memory.Read(Where, &Value, Width))
                break;

            WritePort(Port, Width, Value);
        }
        else
        {
            ULONG Value;

            Value = ReadPort(Port, Width);

            if (!m_Memory.Write(Where, &Value, Width))
                break;
        }

        Address += Step;
        Moved++;
    }

    /* What the instruction would have left behind, now that it has been done */
    const WHV_REGISTER_NAME Names[] =
    {
        WHvX64RegisterRcx, WHvX64RegisterRsi, WHvX64RegisterRdi, WHvX64RegisterRip
    };
    WHV_REGISTER_VALUE Values[RTL_NUMBER_OF(Names)] = {};

    Values[0].Reg64 = Repeated ? (Exit.IoPortAccess.Rcx - Moved)
                               : Exit.IoPortAccess.Rcx;

    Values[1].Reg64 = IsWrite
                    ? (Exit.IoPortAccess.Rsi + (Step * static_cast<LONG64>(Moved)))
                    : Exit.IoPortAccess.Rsi;

    Values[2].Reg64 = IsWrite
                    ? Exit.IoPortAccess.Rdi
                    : (Exit.IoPortAccess.Rdi + (Step * static_cast<LONG64>(Moved)));

    Values[3].Reg64 = Exit.VpContext.Rip + Exit.VpContext.InstructionLength;

    platform::SetRegisters(m_Partition, Index, Names, RTL_NUMBER_OF(Names), Values);
}

/* Past the instruction that caused the exit, which the platform leaves to us */
void Machine::StepOver(ULONG Index, ULONG64 Rip, ULONG Length)
{
    const WHV_REGISTER_NAME Name = WHvX64RegisterRip;
    WHV_REGISTER_VALUE Value = {};

    Value.Reg64 = Rip + Length;
    platform::SetRegisters(m_Partition, Index, &Name, 1, &Value);
}

/**
 * @brief
 * Hands the processor whatever the controller has, when it will take it.
 */
void Machine::DeliverInterrupt(ULONG Index)
{
    /*
     * Asked of the controller every time rather than remembered. A remembered
     * answer goes stale the moment the guest says an interrupt is finished:
     * nothing moved a line, so nothing would set the flag again, and every
     * interrupt still latched would wait forever.
     */
    if (!Pending())
        return;

    const WHV_REGISTER_NAME Name = WHvX64RegisterRflags;
    WHV_REGISTER_VALUE Value = {};

    if (FAILED(platform::GetRegisters(m_Partition, Index, &Name, 1, &Value)))
        return;

    /*
     * Not while the guest has them off. Rather than look again at whatever
     * moment the next exit happens to be, the processor is asked to come back
     * the moment it would accept one.
     *
     * Sampling was not enough. A guest sitting in a firmware call polls with
     * interrupts off for almost the whole call, because the gate it came
     * through cleared them, so nearly every exit is seen at a moment when
     * nothing can be delivered and the interrupt waits for a coincidence.
     */
    if ((Value.Reg64 & 0x200) == 0)
    {
        RequestInterruptWindow(Index, true);
        return;
    }

    const ULONG Offered = m_Offered;

    if (Offered == VDEV_NO_VECTOR)
        return;

    const int Vector = (int)Offered;

    /*
     * Taken here and not offered again. The controller is told only when it
     * next asks, which is on the write that ends the interrupt, and until then
     * there is nothing left for the processor to be given.
     */
    InterlockedExchange(&m_Taken, 1);
    InterlockedExchange((volatile LONG *)&m_Offered, (LONG)VDEV_NO_VECTOR);

    const WHV_REGISTER_NAME Interrupt = WHvRegisterPendingInterruption;
    WHV_REGISTER_VALUE Pending = {};

    Pending.PendingInterruption.InterruptionPending = 1;
    Pending.PendingInterruption.InterruptionType = WHvX64PendingInterrupt;
    Pending.PendingInterruption.InterruptionVector = static_cast<UINT32>(Vector);

    platform::SetRegisters(m_Partition, Index, &Interrupt, 1, &Pending);

    m_Delivered++;

    if (Vector < (int)RTL_NUMBER_OF(m_VectorCount))
        m_VectorCount[Vector]++;

    /* Taken, so there is nothing left to be told about for now */
    RequestInterruptWindow(Index, false);
}

/**
 * @brief
 * Asks the processor to stop as soon as it would accept an interrupt, or stops
 * asking.
 *
 * @remarks
 * Set while something is owed and the guest has interrupts off. The processor
 * then comes back with an interrupt window exit at the first instruction
 * boundary where one could be taken, which is the only way to deliver into a
 * guest that spends its time inside calls made through a gate.
 */
void Machine::RequestInterruptWindow(ULONG Index, bool Wanted)
{
    if (Wanted == m_WindowWanted)
        return;

    const WHV_REGISTER_NAME Name = WHvX64RegisterDeliverabilityNotifications;
    WHV_REGISTER_VALUE Value = {};

    Value.DeliverabilityNotifications.InterruptNotification = Wanted ? 1 : 0;

    if (SUCCEEDED(platform::SetRegisters(m_Partition, Index, &Name, 1, &Value)))
        m_WindowWanted = Wanted;
}

/**
 * @brief
 * Says where a processor is and what it is holding.
 *
 * @remarks
 * For when a machine has stopped making progress. Where it is spinning is the
 * whole question, and without this the only answer available is that it is not
 * getting anywhere.
 */
void Machine::ReportProcessor(ULONG Index)
{
    const WHV_REGISTER_NAME Names[] =
    {
        WHvX64RegisterCs, WHvX64RegisterRip, WHvX64RegisterRflags,
        WHvX64RegisterRax, WHvX64RegisterRbx, WHvX64RegisterRcx,
        WHvX64RegisterRdx, WHvX64RegisterSs, WHvX64RegisterRsp
    };

    WHV_REGISTER_VALUE Values[RTL_NUMBER_OF(Names)] = {};

    if (FAILED(platform::GetRegisters(m_Partition, Index, Names,
                                      RTL_NUMBER_OF(Names), Values)))
    {
        return;
    }

    /*
     * Whole registers, not the low halves of them. A guest that has gone into
     * protected mode is past the point where the low half means anything, and
     * one narrowed to it reads as an address the guest has never been at.
     */
    Log(RtvmLogInfo, "    processor %lu at %04x:%08llx, flags %04llx%s\n",
        Index,
        Values[0].Segment.Selector,
        static_cast<unsigned long long>(Values[1].Reg64),
        static_cast<unsigned long long>(Values[2].Reg64 & 0xFFFF),
        ((Values[2].Reg64 & 0x200) != 0) ? "" : ", interrupts off");

    Log(RtvmLogInfo, "    ax %08llx bx %08llx cx %08llx dx %08llx, stack %04x:%08llx\n",
        static_cast<unsigned long long>(Values[3].Reg64),
        static_cast<unsigned long long>(Values[4].Reg64),
        static_cast<unsigned long long>(Values[5].Reg64),
        static_cast<unsigned long long>(Values[6].Reg64),
        Values[7].Segment.Selector,
        static_cast<unsigned long long>(Values[8].Reg64));

    /*
     * The instruction it is sitting on, which usually settles what it is
     * doing. A guest that has turned paging on is not where it says it is,
     * so the address is put through its own tables first: reading the number
     * straight gives whatever happens to sit at that place in memory, which
     * is never the instruction and usually nothing at all.
     */
    ULONG64 Where = Values[0].Segment.Base + Values[1].Reg64;
    WHV_TRANSLATE_GVA_RESULT_CODE Result = WHvTranslateGvaResultSuccess;
    ULONG64 Behind = 0;

    if (SUCCEEDED(Translate(Index, Where, WHvTranslateGvaFlagValidateRead,
                            &Result, &Behind)) &&
        (Result == WHvTranslateGvaResultSuccess))
    {
        Where = Behind;
    }
    UCHAR Code[8] = {};

    if (m_Memory.Read(Where, Code, sizeof(Code)))
    {
        Log(RtvmLogInfo, "    code %02x %02x %02x %02x %02x %02x %02x %02x\n",
            Code[0], Code[1], Code[2], Code[3],
            Code[4], Code[5], Code[6], Code[7]);
    }
}

/**
 * @brief
 * Writes out a run of the machine's memory as it stands.
 *
 * @remarks
 * Taken straight rather than through a processor, so it is what is there and
 * not what one of them can reach. A guest that has gone into protected mode
 * with paging off reads the same either way, which is when this is of use.
 *
 * A window a device answers for is asked of the device, because the memory
 * behind one of those has not been written to since the device took it over.
 */
bool Machine::DumpMemory(const char *Path, ULONG64 Base, ULONG Length)
{
    if ((Path == nullptr) || (Length == 0))
        return false;

    UniqueFile Output(CreateFileA(Path, GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr));

    if (!Output)
    {
        Log(RtvmLogError, "%s could not be written, %lu\n", Path,
            GetLastError());
        return false;
    }

    /* A page at a time, so that nothing has to be held twice over */
    UCHAR Page[VDEV_PAGE_SIZE];
    ULONG Done = 0;

    while (Done < Length)
    {
        const ULONG Part = min((ULONG)sizeof(Page), Length - Done);
        DWORD Written = 0;

        if (m_Bus.MemoryClaimed(Base + Done))
        {
            for (ULONG Byte = 0; Byte < Part; Byte++)
            {
                if (!m_Bus.ReadMemory(Base + Done + Byte, 1, &Page[Byte]))
                    Page[Byte] = 0xFF;
            }
        }
        else if (!m_Memory.Read(Base + Done, Page, Part))
        {
            Log(RtvmLogError, "the machine has nothing at %llx\n",
                static_cast<unsigned long long>(Base + Done));
            return false;
        }

        if (!WriteFile(Output.Get(), Page, Part, &Written, nullptr) ||
            (Written != Part))
        {
            Log(RtvmLogError, "%s was only part written\n", Path);
            return false;
        }

        Done += Part;
    }

    Log(RtvmLogInfo, "%lx bytes from %llx are in %s\n", Length,
        static_cast<unsigned long long>(Base), Path);

    return true;
}

/* THE REGISTERS THAT ARE NOT REGISTERS ***************************************/

/* The handful a machine of this kind is asked for by name */
#define MSR_TIME_STAMP          0x00000010
#define MSR_APIC_BASE           0x0000001B
#define MSR_RANGE_KINDS         0x000000FE
#define MSR_MISC_ENABLE         0x000001A0
#define MSR_RANGE_DEFAULT       0x000002FF
#define MSR_PAGE_KINDS          0x00000277

/* Where the processor's own controller answers, and which one is the first */
#define APIC_DEFAULT_BASE       0xFEE00000
#define APIC_IS_FIRST           0x00000100
#define APIC_ENABLED            0x00000800

/* How many kinds of range the processor can be told about, and none set */
#define RANGE_KIND_COUNT        8

/* What the eight parts of the page table's own kinds come up as */
#define PAGE_KINDS_AT_RESET     0x0007040600070406ull

/**
 * @brief
 * Answers one of the registers a processor is asked for by number.
 *
 * @remarks
 * Answering every one of them with nothing is worse than not answering at
 * all. The first of them counts how long the processor has been running, and
 * a guest working out how fast it goes by reading that twice would find it had
 * not moved: the same standing still that a clock which never ticks and a
 * counter which never counts down both cause, and just as hard to see.
 */
void Machine::AnswerMsr(ULONG Index, const WHV_RUN_VP_EXIT_CONTEXT &Exit)
{
    const ULONG Which = Exit.MsrAccess.MsrNumber;
    const bool Writing = (Exit.MsrAccess.AccessInfo.AsUINT32 & 1) != 0;

    WHV_REGISTER_NAME Names[3];
    WHV_REGISTER_VALUE Values[3] = {};
    ULONG Count = 0;
    ULONG64 Shown = 0;

    if (Writing)
    {
        const ULONG64 Value = ((Exit.MsrAccess.Rdx & 0xFFFFFFFF) << 32) |
                              (Exit.MsrAccess.Rax & 0xFFFFFFFF);

        Shown = Value;

        /* The only one worth taking, because a guest may set where it counts from */
        if (Which == MSR_TIME_STAMP)
        {
            Names[Count] = WHvX64RegisterTsc;
            Values[Count].Reg64 = Value;
            Count++;
        }
        else if (m_Holes < HolesToName)
        {
            Log(RtvmLogTrace, "processor %lu set %08lx, which is not kept\n",
                Index, Which);
        }
    }
    else
    {
        ULONG64 Value = 0;

        switch (Which)
        {
            case MSR_TIME_STAMP:
            {
                /*
                 * The same count the processor's own instruction for reading
                 * it gives back, which is the hypervisor's to keep and not
                 * this. Asking anything else would have the two disagree, and
                 * a guest that read the count one way and then the other
                 * would see it jump.
                 */
                WHV_REGISTER_NAME Name = WHvX64RegisterTsc;
                WHV_REGISTER_VALUE Held = {};
                const HRESULT Got = platform::GetRegisters(m_Partition, Index,
                                                           &Name, 1, &Held);

                if (SUCCEEDED(Got))
                {
                    Value = Held.Reg64;
                    break;
                }

                /*
                 * Said out loud rather than answered with nothing. A count
                 * that reads as zero every time is a processor that never
                 * runs, and everything that measures itself against one waits
                 * forever without saying what it is waiting for.
                 */
                if (m_Holes < HolesToName)
                {
                    Log(RtvmLogWarning,
                        "the count of how long the processor has run cannot "
                        "be read, %08lx\n", Got);
                    m_Holes++;
                }

                break;
            }

            case MSR_APIC_BASE:
                Value = APIC_DEFAULT_BASE | APIC_ENABLED;

                /* Only one of them is the one the machine started on */
                if (Index == 0)
                    Value |= APIC_IS_FIRST;

                break;

            case MSR_RANGE_KINDS:
                /* As many as can be described, and none of them fixed */
                Value = RANGE_KIND_COUNT;
                break;

            case MSR_PAGE_KINDS:
                Value = PAGE_KINDS_AT_RESET;
                break;

            case MSR_RANGE_DEFAULT:
            case MSR_MISC_ENABLE:
            default:
                Value = 0;
                break;
        }

        Shown = Value;

        Names[Count] = WHvX64RegisterRax;
        Values[Count].Reg64 = Value & 0xFFFFFFFF;
        Count++;

        Names[Count] = WHvX64RegisterRdx;
        Values[Count].Reg64 = (Value >> 32) & 0xFFFFFFFF;
        Count++;
    }

    /*
     * The first few said out loud. Which of these a guest asks for, and what
     * it was told, is the only way to tell a register answered wrongly from
     * one the hypervisor was answering all along.
     */
    if (m_Watched < WatchLimit)
    {
        Log(RtvmLogTrace, "    msr %08lx %s %016llx\n", Which,
            Writing ? "<-" : "->",
            static_cast<unsigned long long>(Shown));
        m_Watched++;
    }

    /* And on past it either way, because the instruction has been answered */
    Names[Count] = WHvX64RegisterRip;
    Values[Count].Reg64 = Exit.VpContext.Rip + Exit.VpContext.InstructionLength;
    Count++;

    platform::SetRegisters(m_Partition, Index, Names, Count, Values);
}

/**
 * @brief
 * Hands one to the controller a processor has of its own.
 *
 * @remarks
 * Not the same thing as holding up the line the old pair share. That line is
 * one wire into one processor and whatever is on it waits its turn; this says
 * which processor and which vector, and the hypervisor puts it where that
 * processor will find it.
 */
bool Machine::RequestVector(ULONG Destination, ULONG Vector, bool Lowest,
                            bool Logical, bool Level)
{
    if (!m_HaveLocalApic || (platform::RequestInterrupt == nullptr))
        return false;

    WHV_INTERRUPT_CONTROL Asking = {};

    Asking.Type = Lowest ? WHvX64InterruptTypeLowestPriority
                         : WHvX64InterruptTypeFixed;
    Asking.DestinationMode = Logical
                           ? WHvX64InterruptDestinationModeLogical
                           : WHvX64InterruptDestinationModePhysical;
    Asking.TriggerMode = Level ? WHvX64InterruptTriggerModeLevel
                               : WHvX64InterruptTriggerModeEdge;
    Asking.Destination = Destination;
    Asking.Vector = Vector;

    const bool Given = SUCCEEDED(platform::RequestInterrupt(m_Partition,
                                                            &Asking,
                                                            sizeof(Asking)));
    if (Given)
        m_Handed++;
    else
        m_NotHanded++;

    return Given;
}

/* Whether the controller has anything for a processor that will take it */
bool Machine::Pending()
{
    return m_Offered != VDEV_NO_VECTOR;
}

/* What a controller has for the processors, until it says otherwise */
void Machine::OfferVector(ULONG Vector)
{
    InterlockedExchange((volatile LONG *)&m_Offered, (LONG)Vector);
    InterlockedExchange(&m_Taken, 0);
}

bool Machine::VectorWasTaken()
{
    return InterlockedExchange(&m_Taken, 0) != 0;
}

StopReason Machine::RunProcessor(ULONG Index)
{
    /* How many accesses to nothing are tolerated before the machine is stopped */
    constexpr ULONG StrayLimit = 64;

    WHV_RUN_VP_EXIT_CONTEXT Exit = {};
    ULONG Stray = 0;

    while (InterlockedCompareExchange(&m_Stopping, 0, 0) == 0)
    {
        DeliverInterrupt(Index);

        const HRESULT Result = platform::RunVirtualProcessor(m_Partition, Index,
                                                             &Exit, sizeof(Exit));
        if (FAILED(Result))
        {
            Log(RtvmLogError, "processor %lu would not run, %08lx\n", Index, Result);
            return StopReason::Refused;
        }

        /* Left behind for the panel, which has no other way to see it move */
        m_LastRip = Exit.VpContext.Rip;
        m_LastCs = Exit.VpContext.Cs.Selector;

        switch (Exit.ExitReason)
        {
            case WHvRunVpExitReasonX64IoPortAccess:
            {
                const USHORT Port = Exit.IoPortAccess.PortNumber;
                const ULONG Width = Exit.IoPortAccess.AccessInfo.AccessSize;

                if (Exit.IoPortAccess.AccessInfo.StringOp)
                {
                    StringPort(Index, Exit);
                    break;
                }

                if (Exit.IoPortAccess.AccessInfo.IsWrite)
                {
                    /*
                     * Only as much of it as the instruction meant to write.
                     * The rest of the register is whatever the guest happened
                     * to leave there, and a device that takes it whole acts on
                     * bytes nobody sent it.
                     */
                    const ULONG Mask = (Width >= 4)
                                     ? 0xFFFFFFFFu
                                     : ((1u << (Width * 8)) - 1);

                    WritePort(Port, Width,
                              static_cast<ULONG>(Exit.IoPortAccess.Rax) & Mask);
                }
                else
                {
                    ULONG Value;

                    Value = ReadPort(Port, Width);

                    /*
                     * Only the bytes the access asked for are replaced, which
                     * is what the instruction does to the register itself.
                     */
                    const ULONG64 Mask = (Width >= 4) ? 0xFFFFFFFFull
                                                      : ((1ull << (Width * 8)) - 1);

                    const WHV_REGISTER_NAME Name = WHvX64RegisterRax;
                    WHV_REGISTER_VALUE Set = {};

                    Set.Reg64 = (Exit.IoPortAccess.Rax & ~Mask) | (Value & Mask);
                    platform::SetRegisters(m_Partition, Index, &Name, 1, &Set);
                }

                StepOver(Index, Exit.VpContext.Rip, Exit.VpContext.InstructionLength);
                Stray = 0;
                break;
            }

            case WHvRunVpExitReasonMemoryAccess:
            {
                const ULONG64 Address = Exit.MemoryAccess.Gpa;

                /*
                 * A memory access exit does not always say how long the
                 * instruction was: the length comes with the bytes instead.
                 * Stepping by a length of zero moves nothing, and a processor
                 * that faults on the same instruction forever looks exactly
                 * like a machine that has hung, with nothing said about why.
                 */
                ULONG Length = Exit.VpContext.InstructionLength;

                if (Length == 0)
                    Length = Exit.MemoryAccess.InstructionByteCount;

                /*
                 * Carried out rather than skipped, whether or not a device
                 * answers for the window. A write that never happened and a
                 * read that gave back nothing are both worse than not having
                 * the device at all, and a window nothing answers for is not a
                 * failure either: a bus with nothing on it reads as ones and
                 * lets a write go, and a guest touching a hole and carrying on
                 * is the whole reason it does.
                 */
                /*
                 * Said out loud for the first few, because a guest writing
                 * into a hole loses what it wrote and reads it back as ones,
                 * and nothing about what happens afterwards points at where.
                 */
                if (!m_Bus.MemoryClaimed(Address))
                {
                    if (m_Holes < HolesToName)
                    {
                        Log(RtvmLogWarning,
                            "nothing answers for %012llx, reached from "
                            "%04x:%08llx\n",
                            static_cast<unsigned long long>(Address),
                            Exit.VpContext.Cs.Selector,
                            static_cast<unsigned long long>(Exit.VpContext.Rip));
                    }

                    m_Holes++;
                }

                if (EmulateAccess(Index, Exit))
                {
                    Stray = 0;
                    break;
                }

                /*
                 * It could not be carried out at all, which is what a
                 * processor fetching from a hole looks like: stepping over an
                 * instruction that was never read gets nowhere, and it would
                 * do the same again at once. So this is counted and given up
                 * on rather than spun on.
                 */
                if (Stray == 0)
                {
                    Log(RtvmLogError,
                        "processor %lu touched %012llx at %04x:%08llx, "
                        "and could not be told what is there\n",
                        Index,
                        static_cast<unsigned long long>(Address),
                        Exit.VpContext.Cs.Selector,
                        static_cast<unsigned long long>(Exit.VpContext.Rip));
                }

                Stray++;

                if (Stray > StrayLimit)
                {
                    Log(RtvmLogError,
                        "processor %lu is getting nowhere, so it is stopped\n",
                        Index);
                    return StopReason::Refused;
                }

                StepOver(Index, Exit.VpContext.Rip, Length);
                break;
            }

            case WHvRunVpExitReasonX64Halt:
                /*
                 * Waiting for something to happen. If a line is already up the
                 * loop goes straight back round and delivers it; otherwise this
                 * is where the machine sits while it is idle.
                 */
                if (!Pending())
                    Sleep(1);
                break;

            case WHvRunVpExitReasonX64Cpuid:
            {
                /* Answer as the processor would, since nothing here differs yet */
                const WHV_REGISTER_NAME Names[] =
                {
                    WHvX64RegisterRax, WHvX64RegisterRbx,
                    WHvX64RegisterRcx, WHvX64RegisterRdx,
                    WHvX64RegisterRip
                };
                WHV_REGISTER_VALUE Values[RTL_NUMBER_OF(Names)] = {};

                Values[0].Reg64 = Exit.CpuidAccess.DefaultResultRax;
                Values[1].Reg64 = Exit.CpuidAccess.DefaultResultRbx;
                Values[2].Reg64 = Exit.CpuidAccess.DefaultResultRcx;
                Values[3].Reg64 = Exit.CpuidAccess.DefaultResultRdx;
                Values[4].Reg64 = Exit.VpContext.Rip + Exit.VpContext.InstructionLength;

                platform::SetRegisters(m_Partition, Index, Names,
                                       RTL_NUMBER_OF(Names), Values);
                break;
            }

            case WHvRunVpExitReasonX64MsrAccess:
                AnswerMsr(Index, Exit);
                Stray = 0;
                break;

            case WHvRunVpExitReasonX64InterruptWindow:
                /*
                 * The moment that was asked for. Nothing else to do here: the
                 * top of the loop delivers, and the guest will now take it.
                 */
                Stray = 0;
                break;

            case WHvRunVpExitReasonCanceled:
                return StopReason::Cancelled;

            case WHvRunVpExitReasonUnrecoverableException:
                Log(RtvmLogError, "processor %lu gave up at %04x:%08llx\n",
                    Index,
                    Exit.VpContext.Cs.Selector,
                    static_cast<unsigned long long>(Exit.VpContext.Rip));
                return StopReason::TripleFault;

            default:
                Log(RtvmLogWarning,
                    "processor %lu stopped for reason %u at %04x:%08llx\n",
                    Index,
                    static_cast<unsigned>(Exit.ExitReason),
                    Exit.VpContext.Cs.Selector,
                    static_cast<unsigned long long>(Exit.VpContext.Rip));
                return StopReason::Refused;
        }
    }

    return StopReason::Shutdown;
}

/**
 * @brief
 * Stops the machine once the time it was given is up.
 *
 * @remarks
 * The loop that runs a processor only looks at the clock between two of its
 * turns, and a guest that has stopped coming back never gives it one: a
 * machine told to run for a minute would then run until something killed it,
 * taking the summary of what its hardware did with it.
 */
DWORD WINAPI Machine::Watching(LPVOID Parameter)
{
    auto *Self = static_cast<Machine *>(Parameter);

    while (InterlockedCompareExchange(&Self->m_Stopping, 0, 0) == 0)
    {
        if (GetTickCount() >= Self->m_Deadline)
        {
            Log(RtvmLogInfo, "the time it was given is up\n");
            Self->ReportProcessor(0);
            Self->Stop();
            break;
        }

        Sleep(100);
    }

    return 0;
}

StopReason Machine::Run()
{
    Log(RtvmLogInfo, "running\n");

    InterlockedExchange(&m_Running, 1);

    HANDLE Watchdog = nullptr;

    if (m_Seconds != 0)
    {
        m_Deadline = GetTickCount() + (m_Seconds * 1000);
        Watchdog = CreateThread(nullptr, 0, Watching, this, 0, nullptr);
    }

    /* One processor for now. The rest are made and wait to be started */
    const StopReason Reason = RunProcessor(0);

    InterlockedExchange(&m_Stopping, 1);

    if (Watchdog != nullptr)
    {
        WaitForSingleObject(Watchdog, 2000);
        CloseHandle(Watchdog);
    }

    InterlockedExchange(&m_Running, 0);

    m_Devices->StopAll();

    /* What the hardware actually did, which is worth knowing either way */
    Log(RtvmLogInfo, "%lu interrupt(s) taken\n", m_Delivered);

    /*
     * And what went the other way, to the controller each processor has of
     * its own. None of that is counted above, because it never passes through
     * the pair of chips and nothing here sees it arrive.
     */
    if ((m_Handed != 0) || (m_NotHanded != 0))
    {
        Log(RtvmLogInfo, "%lu handed to a local controller, %lu refused\n",
            m_Handed, m_NotHanded);
    }


    /* And what was put in, which is not always one for one with the above */
    for (ULONG Vector = 0; Vector < RTL_NUMBER_OF(m_VectorCount); Vector++)
    {
        if (m_VectorCount[Vector] != 0)
        {
            Log(RtvmLogInfo, "    vector %02lx taken %lu time(s)\n",
                Vector, m_VectorCount[Vector]);
        }
    }

    return Reason;
}

} /* namespace rtvm */
