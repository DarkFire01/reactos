/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     One virtual machine, from its memory up to the loop that runs it
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "rtvmm.h"

#include <winhvplatform.h>

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
    HRESULT (WINAPI *MapGpaRange)(WHV_PARTITION_HANDLE, VOID *, WHV_GUEST_PHYSICAL_ADDRESS, UINT64, WHV_MAP_GPA_RANGE_FLAGS);
    HRESULT (WINAPI *CreateVirtualProcessor)(WHV_PARTITION_HANDLE, UINT32, UINT32);
    HRESULT (WINAPI *DeleteVirtualProcessor)(WHV_PARTITION_HANDLE, UINT32);
    HRESULT (WINAPI *RunVirtualProcessor)(WHV_PARTITION_HANDLE, UINT32, VOID *, UINT32);
    HRESULT (WINAPI *CancelRunVirtualProcessor)(WHV_PARTITION_HANDLE, UINT32, UINT32);
    HRESULT (WINAPI *SetRegisters)(WHV_PARTITION_HANDLE, UINT32, const WHV_REGISTER_NAME *, UINT32, const WHV_REGISTER_VALUE *);
    HRESULT (WINAPI *GetRegisters)(WHV_PARTITION_HANDLE, UINT32, const WHV_REGISTER_NAME *, UINT32, WHV_REGISTER_VALUE *);
}

/*
 * Where a machine that starts the old way finds its firmware. The last sixty
 * four kilobytes below a megabyte are the firmware's, and the processor comes
 * out of reset sixteen bytes from the top of them.
 */
constexpr ULONG64 FirmwareBase = 0x000F0000;
constexpr ULONG64 FirmwareSize = 0x00010000;
constexpr USHORT ResetSegment = 0xF000;
constexpr USHORT ResetOffset = 0xFFF0;

/* Where the manager leaves the firmware a note about the machine */
constexpr ULONG64 MachineDescription = 0x00000500;

Machine::~Machine()
{
    /* The devices go before the partition they were answering for */
    m_Devices.Reset();

    if (m_Partition != nullptr)
    {
        for (ULONG Index = 0; Index < m_ProcessorCount; Index++)
            platform::DeleteVirtualProcessor(m_Partition, Index);

        platform::DeletePartition(m_Partition);
        m_Partition = nullptr;
    }
}

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

#undef BIND

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

    const auto Flags = static_cast<WHV_MAP_GPA_RANGE_FLAGS>(
        WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute);

    Result = platform::MapGpaRange(Partition, m_Memory.Base(), 0,
                                   m_Memory.Size(), Flags);
    if (FAILED(Result))
    {
        Log(RtvmLogError, "the memory would not map, %08lx\n", Result);
        return false;
    }

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

    if (!GetFileSizeEx(File.Get(), &Size) || (Size.QuadPart == 0) ||
        (static_cast<ULONG64>(Size.QuadPart) > FirmwareSize))
    {
        Log(RtvmLogError, "%s is %lld bytes, which is not a firmware image\n",
            Path, static_cast<long long>(Size.QuadPart));
        return false;
    }

    const ULONG64 Length = static_cast<ULONG64>(Size.QuadPart);
    const ULONG64 Base = (FirmwareBase + FirmwareSize) - Length;
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

    Code.Base = FirmwareBase;
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

    if (!BindPlatform())
        return false;

    if (!m_Bus.Initialize())
    {
        Log(RtvmLogError, "no room for the port table\n");
        return false;
    }

    if (!CreatePartition(Config))
        return false;

    m_Pic.Reset();
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
        "rtvmstorage.dll",
        "rtvmchipset.dll"
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

    if (!LoadFirmware(Config.FirmwarePath.Get()))
        return false;

    if (!DescribeMachine(Config))
        return false;

    for (ULONG Index = 0; Index < m_ProcessorCount; Index++)
    {
        if (!PrepareProcessor(Index))
            return false;
    }

    return true;
}

void Machine::SetInterruptLine(ULONG Line, bool Asserted)
{
    m_Pic.SetLine(Line, Asserted);

    if (m_Pic.Pending())
        InterlockedExchange(&m_InterruptPending, 1);
}

bool Machine::ReadGuest(ULONG64 Address, void *Buffer, ULONG Length)
{
    return m_Memory.Read(Address, Buffer, Length);
}

bool Machine::WriteGuest(ULONG64 Address, const void *Buffer, ULONG Length)
{
    return m_Memory.Write(Address, Buffer, Length);
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
     * Reads land through the extra segment and writes come out of the data
     * one, and in real mode the segment is the base the processor is using
     * rather than the selector, so the exit's own copy is what counts.
     */
    ULONG64 Address = IsWrite
                    ? (Exit.IoPortAccess.Ds.Base + (Exit.IoPortAccess.Rsi & 0xFFFF))
                    : (Exit.IoPortAccess.Es.Base + (Exit.IoPortAccess.Rdi & 0xFFFF));

    ULONG64 Moved = 0;

    while (Moved < Count)
    {
        if (IsWrite)
        {
            ULONG Value = 0;

            if (!m_Memory.Read(Address, &Value, Width))
                break;

            if (Pic::Owns(Port))
                m_Pic.WritePort(Port, Value);
            else
                m_Bus.WritePort(Port, Width, Value);
        }
        else
        {
            const ULONG Value = Pic::Owns(Port)
                              ? m_Pic.ReadPort(Port)
                              : m_Bus.ReadPort(Port, Width);

            if (!m_Memory.Write(Address, &Value, Width))
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
    if (InterlockedCompareExchange(&m_InterruptPending, 0, 0) == 0)
        return;

    const WHV_REGISTER_NAME Name = WHvX64RegisterRflags;
    WHV_REGISTER_VALUE Value = {};

    if (FAILED(platform::GetRegisters(m_Partition, Index, &Name, 1, &Value)))
        return;

    /* Not while the guest has them off. It will be offered again */
    if ((Value.Reg64 & 0x200) == 0)
        return;

    const int Vector = m_Pic.Acknowledge();

    if (Vector < 0)
    {
        InterlockedExchange(&m_InterruptPending, 0);
        return;
    }

    const WHV_REGISTER_NAME Interrupt = WHvRegisterPendingInterruption;
    WHV_REGISTER_VALUE Pending = {};

    Pending.PendingInterruption.InterruptionPending = 1;
    Pending.PendingInterruption.InterruptionType = WHvX64PendingInterrupt;
    Pending.PendingInterruption.InterruptionVector = static_cast<UINT32>(Vector);

    platform::SetRegisters(m_Partition, Index, &Interrupt, 1, &Pending);

    if (!m_Pic.Pending())
        InterlockedExchange(&m_InterruptPending, 0);
}

StopReason Machine::RunProcessor(ULONG Index)
{
    WHV_RUN_VP_EXIT_CONTEXT Exit = {};

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
                    const ULONG Value = static_cast<ULONG>(Exit.IoPortAccess.Rax);

                    if (Pic::Owns(Port))
                        m_Pic.WritePort(Port, Value);
                    else
                        m_Bus.WritePort(Port, Width, Value);
                }
                else
                {
                    const ULONG Value = Pic::Owns(Port)
                                      ? m_Pic.ReadPort(Port)
                                      : m_Bus.ReadPort(Port, Width);

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
                break;
            }

            case WHvRunVpExitReasonMemoryAccess:
            {
                const ULONG64 Address = Exit.MemoryAccess.Gpa;

                if (!m_Bus.MemoryClaimed(Address))
                {
                    Log(RtvmLogWarning,
                        "processor %lu touched %012llx, where there is nothing\n",
                        Index, static_cast<unsigned long long>(Address));
                }

                /*
                 * Carrying out the faulting instruction is what belongs here.
                 * Until a device sits behind a window, stepping over it is
                 * honest: nothing claimed the address, so nothing was going to
                 * answer for it either way.
                 */
                StepOver(Index, Exit.VpContext.Rip, Exit.VpContext.InstructionLength);
                break;
            }

            case WHvRunVpExitReasonX64Halt:
                /*
                 * Waiting for something to happen. If a line is already up the
                 * loop goes straight back round and delivers it; otherwise this
                 * is where the machine sits while it is idle.
                 */
                if (InterlockedCompareExchange(&m_InterruptPending, 0, 0) == 0)
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
            {
                /* Nothing here has model specific registers worth the name */
                const WHV_REGISTER_NAME Names[] =
                {
                    WHvX64RegisterRax, WHvX64RegisterRdx, WHvX64RegisterRip
                };
                WHV_REGISTER_VALUE Values[RTL_NUMBER_OF(Names)] = {};

                Values[2].Reg64 = Exit.VpContext.Rip + Exit.VpContext.InstructionLength;

                platform::SetRegisters(m_Partition, Index, Names,
                                       RTL_NUMBER_OF(Names), Values);
                break;
            }

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

StopReason Machine::Run()
{
    Log(RtvmLogInfo, "running\n");

    /* One processor for now. The rest are made and wait to be started */
    const StopReason Reason = RunProcessor(0);

    m_Devices->StopAll();
    return Reason;
}

} /* namespace rtvm */
