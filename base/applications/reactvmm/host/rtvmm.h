/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The manager's own declarations
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <windows.h>
#include <winhvplatform.h>

#include "vdev.h"
#include "rtvm_util.h"

extern "C" {
#include "rtvm_device.h"
}

namespace rtvm
{

/* As many of each as one machine may have, which is more than one needs */
constexpr ULONG MaximumModules = 16;
constexpr ULONG MaximumDevices = 32;
constexpr ULONG MaximumMemoryRanges = 32;
constexpr ULONG MaximumRequests = 16;

void Log(RTVM_LOG_LEVEL Level, const char *Format, ...);
void SetLogLevel(RTVM_LOG_LEVEL Level);

/*
 * Where the guest's memory lives. One block, mapped into the partition once,
 * and reachable from the manager for as long as the machine exists.
 */
class Memory
{
public:
    Memory() = default;
    ~Memory();

    Memory(const Memory &) = delete;
    Memory &operator=(const Memory &) = delete;

    bool Allocate(ULONG64 Size);

    bool Read(ULONG64 Address, void *Buffer, ULONG Length) const;
    bool Write(ULONG64 Address, const void *Buffer, ULONG Length);

    void *Base() const noexcept { return m_Base; }
    ULONG64 Size() const noexcept { return m_Size; }

    /* A pointer into the block, or nullptr when the range is not all there */
    void *At(ULONG64 Address, ULONG64 Length) const;

private:
    void *m_Base = nullptr;
    ULONG64 m_Size = 0;
};

/*
 * Everything the guest can address that is not memory.
 *
 * Ports are answered out of one entry per address rather than a search. There
 * are only sixty five thousand of them, the table costs half a megabyte, and
 * every port access in the machine goes through it, so it is worth the pages.
 */
class Bus
{
public:
    /* A port nobody claimed reads as all ones, the way an empty bus does */
    static constexpr ULONG Floating = 0xFFFFFFFFu;
    static constexpr ULONG PortCount = 0x10000;

    Bus() = default;
    ~Bus();

    Bus(const Bus &) = delete;
    Bus &operator=(const Bus &) = delete;

    bool Initialize();

    bool ClaimPorts(IVndIoPortHandler *Handler, USHORT First, USHORT Count);
    bool ClaimMemory(RTVM_DEVICE *Device, ULONG64 Base, ULONG64 Length);
    void ForgetPorts(IVndIoPortHandler *Handler);
    void Forget(RTVM_DEVICE *Device);

    ULONG ReadPort(USHORT Port, ULONG Width);
    void WritePort(USHORT Port, ULONG Width, ULONG Value);

    bool ReadMemory(ULONG64 Address, ULONG Width, void *Buffer);
    bool WriteMemory(ULONG64 Address, ULONG Width, const void *Buffer);

    bool MemoryClaimed(ULONG64 Address) const;

    /* So that the manager can leave a hole where each of these sits */
    ULONG ClaimedCount() const { return m_Memory.Count(); }
    void ClaimedAt(ULONG Index, ULONG64 &Base, ULONG64 &Length) const
    {
        Base = m_Memory[Index].Base;
        Length = m_Memory[Index].Length;
    }

private:
    struct MemoryRange
    {
        ULONG64 Base = 0;
        ULONG64 Length = 0;
        RTVM_DEVICE *Device = nullptr;
    };

    IVndIoPortHandler **m_Ports = nullptr;
    Array<MemoryRange, MaximumMemoryRanges> m_Memory;

};

class Machine;
class LegacyPortAdapter;
class VdevHost;

/*
 * Somewhere for the operator to look. A device that owns a display hands pages
 * up rather than drawing them, because where they are drawn is the manager's
 * business and there may be nowhere at all.
 */
class Display
{
public:
    virtual ~Display() = default;

    virtual void Present(const RTVM_TEXT_PAGE &Page) = 0;
};

/*
 * Loads modules and makes devices out of them. It also holds the interface the
 * devices call back through, because that interface has to reach the machine.
 */
class DeviceHost
{
public:
    explicit DeviceHost(Machine &Owner);
    ~DeviceHost();

    DeviceHost(const DeviceHost &) = delete;
    DeviceHost &operator=(const DeviceHost &) = delete;

    bool Load(const char *FileName);
    bool Create(const char *ClassName, const char *Parameters);

    /* What stands in for a device on the bus, made when the device was */
    LegacyPortAdapter *AdapterFor(RTVM_DEVICE *Device) const;

    bool StartAll();
    void ResetAll();
    void StopAll();

    /* Offered to every device that takes input, because any of them may want it */
    void PostInput(RTVM_INPUT_KIND Kind, ULONG Value);

    ULONG Count() const { return m_Devices.Count(); }
    const char *NameAt(ULONG Index) const { return m_Devices[Index]->Name; }

private:
    struct LoadedModule
    {
        HMODULE Handle = nullptr;
        const RTVM_DEVICE_MODULE *Info = nullptr;
    };

    Machine &m_Machine;
    RTVM_HOST_INTERFACE m_Interface = {};

    Array<LoadedModule, MaximumModules> m_Modules;
    Array<RTVM_DEVICE *, MaximumDevices> m_Devices;

    /* One per device, so that the bus sees every device the same way */
    Array<LegacyPortAdapter *, MaximumDevices> m_Adapters;
};

/* One piece of hardware as it was asked for, kind and settings together */
using DeviceRequest = Text<160>;

/* How the machine was asked to be built */
struct Configuration
{
    ULONG64 MemorySize = 128ull * 1024 * 1024;
    ULONG ProcessorCount = 1;
    Text<MAX_PATH> FirmwarePath;
    Array<DeviceRequest, MaximumRequests> Requests;
    RTVM_LOG_LEVEL LogLevel = RtvmLogInfo;

    /* How long to run for, or zero to run until something stops it */
    ULONG RunSeconds = 0;

    /* Whether the operator gets a window, or only the log */
    bool Window = false;

    /* Where to leave a picture of that window when it goes */
    Text<MAX_PATH> CapturePath;
};

/* Why the machine stopped */
enum class StopReason
{
    Halted,
    Shutdown,
    TripleFault,
    Refused,
    Cancelled
};

/*
 * What the machine is doing, read rather than followed. Nothing here is held
 * under a lock: a panel redrawing twenty times a second does not need a
 * consistent set, it needs a recent one.
 */
struct MachineStatus
{
    ULONG64 Rip;
    USHORT Cs;
    ULONG Delivered;
    ULONG Refused;
    ULONG LineCount[16];
    bool Running;
};

/*
 * One virtual machine: its memory, its bus, its devices and its processors.
 * The partition underneath is reached through the platform library, which is
 * bound at run time so that a manager built here still starts where that
 * library is a different one.
 */
class Machine
{
public:
    Machine();
    ~Machine();

    Machine(const Machine &) = delete;
    Machine &operator=(const Machine &) = delete;

    bool Build(const Configuration &Config);
    StopReason Run();
    void Stop();

    Memory &MemoryBlock() noexcept { return m_Memory; }
    Bus &SystemBus() noexcept { return m_Bus; }

    /* Reached from the device interface, to find what stands in for a device */
    DeviceHost &Devices() noexcept { return *m_Devices; }

    /* Where pages of text go from now on, or nullptr for nowhere */
    void Attach(Display *Screen) noexcept { m_Display = Screen; }

    /* Something the operator did, offered to whichever devices take input */
    void PostInput(RTVM_INPUT_KIND Kind, ULONG Value);

    void Snapshot(MachineStatus &Status) const;

    /* The names of the devices, for a panel that lists what the machine has */
    ULONG DeviceCount() const;
    const char *DeviceName(ULONG Index) const;

    /* Called through the device interface, which is why these are public */
    void SetInterruptLine(ULONG Line, bool Asserted);

    /* What a controller device has for the processors, and whether it went */
    void OfferVector(ULONG Vector);
    bool VectorWasTaken();
    bool ReadGuest(ULONG64 Address, void *Buffer, ULONG Length);
    bool WriteGuest(ULONG64 Address, const void *Buffer, ULONG Length);
    bool PresentText(const RTVM_TEXT_PAGE &Page);

    /* The transfer controller, once one has come up, or nothing */
    IVmDmaController *Channels() const;

    /* Where a port access goes, whichever exit brought it */
    void WritePort(USHORT Port, ULONG Width, ULONG Value);
    ULONG ReadPort(USHORT Port, ULONG Width);


private:
    bool BindPlatform();
    bool CreatePartition(const Configuration &Config);
    bool LoadFirmware(const char *Path);
    bool DescribeMachine(const Configuration &Config);
    bool MapMemory();
    bool PrepareProcessor(ULONG Index);
    StopReason RunProcessor(ULONG Index);

    void DeliverInterrupt(ULONG Index);

    /* Whether the controller has anything for a processor that will take it */
    bool Pending();

    /* Where a processor is, for when it has stopped getting anywhere */
    void ReportProcessor(ULONG Index);

    /* Ask to be told the moment the guest would accept an interrupt */
    void RequestInterruptWindow(ULONG Index, bool Wanted);
    void StringPort(ULONG Index, const WHV_RUN_VP_EXIT_CONTEXT &Exit);
    void StepOver(ULONG Index, ULONG64 Rip, ULONG Length);

    Memory m_Memory;
    Bus m_Bus;
    Owned<DeviceHost> m_Devices;
    Owned<VdevHost> m_Vdevs;

    /* What the controller device last offered, and whether it has been put in */
    volatile ULONG m_Offered = (ULONG)-1;
    volatile LONG m_Taken = 0;

    void *m_Partition = nullptr;
    ULONG m_ProcessorCount = 1;
    volatile LONG m_Stopping = 0;

    /* Whose it is to draw, set before the machine runs and not changed after */
    Display *m_Display = nullptr;

    /* Where the processor was the last time it came out, for the panel */
    volatile ULONG64 m_LastRip = 0;
    volatile LONG m_LastCs = 0;
    volatile LONG m_Running = 0;

    /*
     * The board's own chips are reached from the processor's thread and from
     * whichever thread a device keeps time on, so one is held while any of
     * them is touched.
     */
    CRITICAL_SECTION m_ChipLock = {};

    /* When to stop of its own accord, or zero to keep going */
    ULONG m_Deadline = 0;

    /* What the hardware did, reported when the machine stops */
    ULONG m_Delivered = 0;
    ULONG m_VectorCount[256] = {};
    ULONG m_Refused = 0;

    /* Whether the processor has been asked to stop when it can take one */
    bool m_WindowWanted = false;
};

/* Reads the command line into a configuration. False means it said why */
bool ParseCommandLine(int argc, char **argv, Configuration &Config);
void PrintUsage();

} /* namespace rtvm */
