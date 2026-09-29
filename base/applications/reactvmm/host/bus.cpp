/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Turning a guest access into a call on whatever answers for it
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "rtvmm.h"

#include <string.h>

namespace rtvm
{

/* MEMORY *********************************************************************/

Memory::~Memory()
{
    if (m_Base != nullptr)
        VirtualFree(m_Base, 0, MEM_RELEASE);
}

bool Memory::Allocate(ULONG64 Size)
{
    m_Base = VirtualAlloc(nullptr,
                          static_cast<SIZE_T>(Size),
                          MEM_COMMIT | MEM_RESERVE,
                          PAGE_READWRITE);
    if (m_Base == nullptr)
        return false;

    m_Size = Size;
    return true;
}

void *Memory::At(ULONG64 Address, ULONG64 Length) const
{
    if ((m_Base == nullptr) || (Address >= m_Size))
        return nullptr;

    /* Written this way so that a length near the top cannot wrap past it */
    if (Length > (m_Size - Address))
        return nullptr;

    return static_cast<UCHAR *>(m_Base) + Address;
}

bool Memory::Read(ULONG64 Address, void *Buffer, ULONG Length) const
{
    void *Source = At(Address, Length);

    if (Source == nullptr)
        return false;

    memcpy(Buffer, Source, Length);
    return true;
}

bool Memory::Write(ULONG64 Address, const void *Buffer, ULONG Length)
{
    void *Target = At(Address, Length);

    if (Target == nullptr)
        return false;

    memcpy(Target, Buffer, Length);
    return true;
}

/* THE BUS ********************************************************************/

Bus::~Bus()
{
    if (m_Ports != nullptr)
        VirtualFree(m_Ports, 0, MEM_RELEASE);
}

bool Bus::Initialize()
{
    /*
     * One entry per port address. Committed rather than searched because every
     * port access in the machine goes through it, and half a megabyte of
     * zeroes is cheaper than deciding which pages are worth having.
     */
    m_Ports = static_cast<IVndIoPortHandler **>(
        VirtualAlloc(nullptr,
                     PortCount * sizeof(*m_Ports),
                     MEM_COMMIT | MEM_RESERVE,
                     PAGE_READWRITE));

    return m_Ports != nullptr;
}

bool Bus::ClaimPorts(IVndIoPortHandler *Handler, USHORT First, USHORT Count)
{
    const ULONG Last = static_cast<ULONG>(First) + Count;

    if ((m_Ports == nullptr) || (Count == 0) || (Last > PortCount))
        return false;

    /* Nothing is written until the whole range is known to be free */
    for (ULONG Port = First; Port < Last; Port++)
    {
        if (m_Ports[Port] != nullptr)
            return false;
    }

    for (ULONG Port = First; Port < Last; Port++)
        m_Ports[Port] = Handler;

    return true;
}

bool Bus::ClaimMemory(RTVM_DEVICE *Device, ULONG64 Base, ULONG64 Length)
{
    if (Length == 0)
        return false;

    for (const MemoryRange &Range : m_Memory)
    {
        if ((Base < (Range.Base + Range.Length)) && (Range.Base < (Base + Length)))
            return false;
    }

    return m_Memory.Add({ Base, Length, Device });
}

void Bus::ForgetPorts(IVndIoPortHandler *Handler)
{
    if (m_Ports == nullptr)
        return;

    for (ULONG Port = 0; Port < PortCount; Port++)
    {
        if (m_Ports[Port] == Handler)
            m_Ports[Port] = nullptr;
    }
}

void Bus::Forget(RTVM_DEVICE *Device)
{

    Array<MemoryRange, MaximumMemoryRanges> Kept;

    for (const MemoryRange &Range : m_Memory)
    {
        if (Range.Device != Device)
            Kept.Add(Range);
    }

    m_Memory = Kept;
}

ULONG Bus::ReadPort(USHORT Port, ULONG Width)
{
    if (m_Ports == nullptr)
        return Floating;

    IVndIoPortHandler *Handler = m_Ports[Port];

    if (Handler == nullptr)
        return Floating;

    ULONG Value = Floating;

    if (FAILED(Handler->NotifyIoPortRead(Port, Width, &Value)))
        return Floating;

    return Value;
}

void Bus::WritePort(USHORT Port, ULONG Width, ULONG Value)
{
    if (m_Ports == nullptr)
        return;

    IVndIoPortHandler *Handler = m_Ports[Port];

    if (Handler != nullptr)
        Handler->NotifyIoPortWrite(Port, Width, Value);
}

bool Bus::MemoryClaimed(ULONG64 Address) const
{
    for (const MemoryRange &Range : m_Memory)
    {
        if ((Address >= Range.Base) && (Address < (Range.Base + Range.Length)))
            return true;
    }

    return false;
}

bool Bus::ReadMemory(ULONG64 Address, ULONG Width, void *Buffer)
{
    for (const MemoryRange &Range : m_Memory)
    {
        if ((Address < Range.Base) || (Address >= (Range.Base + Range.Length)))
            continue;

        RTVM_DEVICE *Device = Range.Device;

        if ((Device->Vtable == nullptr) || (Device->Vtable->MemoryRead == nullptr))
            break;

        return Device->Vtable->MemoryRead(Device, Address, Width, Buffer) == RtvmOk;
    }

    /* Nothing there, and a read of nothing is all ones */
    memset(Buffer, 0xFF, Width);
    return false;
}

bool Bus::WriteMemory(ULONG64 Address, ULONG Width, const void *Buffer)
{
    for (const MemoryRange &Range : m_Memory)
    {
        if ((Address < Range.Base) || (Address >= (Range.Base + Range.Length)))
            continue;

        RTVM_DEVICE *Device = Range.Device;

        if ((Device->Vtable == nullptr) || (Device->Vtable->MemoryWrite == nullptr))
            break;

        return Device->Vtable->MemoryWrite(Device, Address, Width, Buffer) == RtvmOk;
    }

    return false;
}

} /* namespace rtvm */
