/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The manager's own declarations
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <windows.h>
#include <winhvplatform.h>

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

    bool ClaimPorts(RTVM_DEVICE *Device, USHORT First, USHORT Count);
    bool ClaimMemory(RTVM_DEVICE *Device, ULONG64 Base, ULONG64 Length);
    void Forget(RTVM_DEVICE *Device);

    ULONG ReadPort(USHORT Port, ULONG Width);
    void WritePort(USHORT Port, ULONG Width, ULONG Value);

    bool ReadMemory(ULONG64 Address, ULONG Width, void *Buffer);
    bool WriteMemory(ULONG64 Address, ULONG Width, const void *Buffer);

    bool MemoryClaimed(ULONG64 Address) const;

private:
    struct MemoryRange
    {
        ULONG64 Base = 0;
        ULONG64 Length = 0;
        RTVM_DEVICE *Device = nullptr;
    };

    RTVM_DEVICE **m_Ports = nullptr;
    Array<MemoryRange, MaximumMemoryRanges> m_Memory;
};

/*
 * The interrupt controller, as the pair of chips a PC has. It is the manager's
 * own rather than a loadable module: firmware cannot come up without it, and a
 * machine that cannot deliver an interrupt is not worth starting.
 */
class Pic
{
public:
    void Reset();

    /* Whether this is one of the four addresses the pair answers at */
    static constexpr bool Owns(USHORT Port) noexcept
    {
        return (Port == 0x20) || (Port == 0x21) || (Port == 0xA0) || (Port == 0xA1);
    }

    void SetLine(ULONG Line, bool Asserted);

    /* The vector to deliver, or -1 when nothing is pending */
    int Acknowledge();
    bool Pending() const;

    ULONG ReadPort(USHORT Port);
    void WritePort(USHORT Port, ULONG Value);

private:
    struct Chip
    {
        UCHAR Request;
        UCHAR Service;
        UCHAR Mask;
        UCHAR Base;
        UCHAR InitStep;
        bool Cascade;
        bool AutoEnd;
        bool ReadService;
    };

    int HighestPending(const Chip &Chip) const;

    Chip m_Chip[2] = {};
};

class Machine;

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

    bool StartAll();
    void ResetAll();
    void StopAll();

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
 * One virtual machine: its memory, its bus, its devices and its processors.
 * The partition underneath is reached through the platform library, which is
 * bound at run time so that a manager built here still starts where that
 * library is a different one.
 */
class Machine
{
public:
    Machine() = default;
    ~Machine();

    Machine(const Machine &) = delete;
    Machine &operator=(const Machine &) = delete;

    bool Build(const Configuration &Config);
    StopReason Run();
    void Stop();

    Memory &MemoryBlock() noexcept { return m_Memory; }
    Bus &SystemBus() noexcept { return m_Bus; }
    Pic &Controller() noexcept { return m_Pic; }

    /* Called through the device interface, which is why these are public */
    void SetInterruptLine(ULONG Line, bool Asserted);
    bool ReadGuest(ULONG64 Address, void *Buffer, ULONG Length);
    bool WriteGuest(ULONG64 Address, const void *Buffer, ULONG Length);

private:
    bool BindPlatform();
    bool CreatePartition(const Configuration &Config);
    bool LoadFirmware(const char *Path);
    bool PrepareProcessor(ULONG Index);
    StopReason RunProcessor(ULONG Index);

    void DeliverInterrupt(ULONG Index);
    void StringPort(ULONG Index, const WHV_RUN_VP_EXIT_CONTEXT &Exit);
    void StepOver(ULONG Index, ULONG64 Rip, ULONG Length);

    Memory m_Memory;
    Bus m_Bus;
    Pic m_Pic;
    Owned<DeviceHost> m_Devices;

    void *m_Partition = nullptr;
    ULONG m_ProcessorCount = 1;
    volatile LONG m_Stopping = 0;

    /* Set while a line is up and nothing has taken the vector yet */
    volatile LONG m_InterruptPending = 0;
};

/* Reads the command line into a configuration. False means it said why */
bool ParseCommandLine(int argc, char **argv, Configuration &Config);
void PrintUsage();

} /* namespace rtvm */
