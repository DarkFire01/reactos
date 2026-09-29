/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Loading hardware modules and making devices out of them
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "vdevhost.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace rtvm
{

/* WHAT A DEVICE CALLS BACK THROUGH *******************************************/

static RTVM_STATUS RTVMAPI
HostClaimPortRange(PVOID Context, PRTVM_DEVICE Device, USHORT First, USHORT Count)
{
    auto *Owner = static_cast<Machine *>(Context);

    if ((Device == nullptr) || (Count == 0))
        return RtvmBadParameter;

    LegacyPortAdapter *Stand = Owner->Devices().AdapterFor(Device);

    if (Stand == nullptr)
        return RtvmFailed;

    return Owner->SystemBus().ClaimPorts(Stand, First, Count) ? RtvmOk : RtvmInUse;
}

static RTVM_STATUS RTVMAPI
HostClaimMemoryRange(PVOID Context, PRTVM_DEVICE Device, ULONG64 Base, ULONG64 Length)
{
    auto *Owner = static_cast<Machine *>(Context);

    if ((Device == nullptr) || (Length == 0))
        return RtvmBadParameter;

    LegacyPortAdapter *Stand = Owner->Devices().AdapterFor(Device);

    if (Stand == nullptr)
        return RtvmFailed;

    return Owner->SystemBus().ClaimMemory(Stand, Base, Length) ? RtvmOk : RtvmInUse;
}

static RTVM_STATUS RTVMAPI
HostSetInterruptLine(PVOID Context, ULONG Line, BOOLEAN Asserted)
{
    auto *Owner = static_cast<Machine *>(Context);

    if (Line > 15)
        return RtvmBadParameter;

    Owner->SetInterruptLine(Line, Asserted != FALSE);
    return RtvmOk;
}

static RTVM_STATUS RTVMAPI
HostReadGuestMemory(PVOID Context, ULONG64 Address, PVOID Buffer, ULONG Length)
{
    auto *Owner = static_cast<Machine *>(Context);

    return Owner->ReadGuest(Address, Buffer, Length) ? RtvmOk : RtvmBadParameter;
}

static RTVM_STATUS RTVMAPI
HostWriteGuestMemory(PVOID Context, ULONG64 Address, const VOID *Buffer, ULONG Length)
{
    auto *Owner = static_cast<Machine *>(Context);

    return Owner->WriteGuest(Address, Buffer, Length) ? RtvmOk : RtvmBadParameter;
}

static RTVM_STATUS RTVMAPI
HostRequestChannel(PVOID Context, ULONG Channel, ULONG Length,
                   PULONG Direction, PULONG64 Address, PULONG Count)
{
    auto *Owner = static_cast<Machine *>(Context);

    if ((Direction == nullptr) || (Address == nullptr) || (Count == nullptr))
        return RtvmBadParameter;

    IVmDmaController *Channels = Owner->Channels();

    if (Channels == nullptr)
        return RtvmNotSupported;

    ULONG Result = 0;

    return SUCCEEDED(Channels->RequestDma(Channel, 0.0, Length, Direction,
                                          Address, Count, &Result))
         ? RtvmOk
         : RtvmNotClaimed;
}

static RTVM_STATUS RTVMAPI
HostChannelFinished(PVOID Context, ULONG Channel)
{
    auto *Owner = static_cast<Machine *>(Context);
    IVmDmaController *Channels = Owner->Channels();

    if (Channels == nullptr)
        return RtvmNotSupported;

    return SUCCEEDED(Channels->ReportDmaComplete(Channel)) ? RtvmOk : RtvmFailed;
}

static RTVM_STATUS RTVMAPI
HostSetTimer(PVOID Context, PRTVM_DEVICE Device, ULONG64 Nanoseconds)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Nanoseconds);

    /* Nothing asks for one yet, and answering a lie would be worse */
    return RtvmNotSupported;
}

static VOID RTVMAPI
HostLog(PVOID Context, RTVM_LOG_LEVEL Level, PCSTR Format, ...)
{
    char Line[512];
    va_list Arguments;

    UNREFERENCED_PARAMETER(Context);

    va_start(Arguments, Format);
    vsnprintf(Line, sizeof(Line), Format, Arguments);
    va_end(Arguments);

    Log(Level, "%s", Line);
}

/* THE HOST *******************************************************************/

DeviceHost::DeviceHost(Machine &Owner)
    : m_Machine(Owner)
{
    m_Interface.Size = sizeof(m_Interface);
    m_Interface.Context = &m_Machine;
    m_Interface.ClaimPortRange = HostClaimPortRange;
    m_Interface.ClaimMemoryRange = HostClaimMemoryRange;
    m_Interface.SetInterruptLine = HostSetInterruptLine;
    m_Interface.ReadGuestMemory = HostReadGuestMemory;
    m_Interface.WriteGuestMemory = HostWriteGuestMemory;
    m_Interface.SetTimer = HostSetTimer;
    m_Interface.RequestChannel = HostRequestChannel;
    m_Interface.ChannelFinished = HostChannelFinished;
    m_Interface.Log = HostLog;
}

DeviceHost::~DeviceHost()
{
    StopAll();

    /* Destroyed newest first, so that nothing is pulled out from under another */
    RTVM_DEVICE *Device = nullptr;

    while (m_Devices.Take(Device))
    {
        LegacyPortAdapter *Stand = AdapterFor(Device);

        if (Stand != nullptr)
        {
            m_Machine.SystemBus().ForgetPorts(Stand);
            m_Machine.SystemBus().ForgetMemory(Stand);
        }

        if ((Device->Vtable != nullptr) && (Device->Vtable->Destroy != nullptr))
            Device->Vtable->Destroy(Device);
    }

    /*
     * The libraries stay loaded. Every device is gone by now, but a module is
     * free to have left a thread or a callback behind, and unloading under one
     * of those costs more than the handle does.
     */
    m_Modules.Clear();
}

bool DeviceHost::Load(const char *FileName)
{
    if (m_Modules.Full())
    {
        Log(RtvmLogError, "no room for another module\n");
        return false;
    }

    UniqueLibrary Library(LoadLibraryA(FileName));

    if (!Library)
    {
        Log(RtvmLogError, "%s would not load, error %lu\n", FileName, GetLastError());
        return false;
    }

    auto Entry = reinterpret_cast<PFN_RTVM_DEVICE_MODULE_ENTRY>(
        reinterpret_cast<void *>(
            GetProcAddress(Library.Get(), RTVM_DEVICE_MODULE_ENTRY_NAME)));

    if (Entry == nullptr)
    {
        Log(RtvmLogError, "%s has no %s, so it is not a hardware module\n",
            FileName, RTVM_DEVICE_MODULE_ENTRY_NAME);
        return false;
    }

    const RTVM_DEVICE_MODULE *Info = Entry(RTVM_DEVICE_ABI_VERSION);

    if (Info == nullptr)
    {
        Log(RtvmLogError, "%s does not speak version %u\n",
            FileName, RTVM_DEVICE_ABI_VERSION);
        return false;
    }

    Log(RtvmLogTrace, "%s offers %lu kind(s)\n", Info->ModuleName, Info->ClassCount);

    for (ULONG Index = 0; Index < Info->ClassCount; Index++)
    {
        Log(RtvmLogTrace, "    %s, %s\n",
            Info->Classes[Index].Name,
            Info->Classes[Index].Description);
    }

    LoadedModule Loaded;

    Loaded.Handle = Library.Release();
    Loaded.Info = Info;

    return m_Modules.Add(Loaded);
}

bool DeviceHost::Create(const char *ClassName, const char *Parameters)
{
    if (m_Devices.Full())
    {
        Log(RtvmLogError, "no room for another device\n");
        return false;
    }

    for (const LoadedModule &Loaded : m_Modules)
    {
        const RTVM_DEVICE_MODULE *Info = Loaded.Info;

        for (ULONG Index = 0; Index < Info->ClassCount; Index++)
        {
            const RTVM_DEVICE_CLASS &Class = Info->Classes[Index];

            if (_stricmp(Class.Name, ClassName) != 0)
                continue;

            RTVM_DEVICE *Device = nullptr;
            const char *Arguments = ((Parameters != nullptr) && (*Parameters != '\0'))
                                  ? Parameters
                                  : nullptr;

            if (Class.Create(&m_Interface, Arguments, &Device) != RtvmOk)
            {
                Log(RtvmLogError, "a %s could not be made\n", ClassName);
                return false;
            }

            /* The module fills in its own half, the manager fills in the rest */
            Device->Host = &m_Interface;

            LegacyPortAdapter *Stand = new LegacyPortAdapter(Device);

            if ((Stand == nullptr) || !m_Adapters.Add(Stand))
            {
                Log(RtvmLogError, "nothing to stand in for %s on the bus\n",
                    ClassName);
                return false;
            }

            return m_Devices.Add(Device);
        }
    }

    Log(RtvmLogError, "no loaded module offers a %s\n", ClassName);
    return false;
}

bool DeviceHost::StartAll()
{
    for (RTVM_DEVICE *Device : m_Devices)
    {
        if ((Device->Vtable == nullptr) || (Device->Vtable->Start == nullptr))
            continue;

        if (Device->Vtable->Start(Device) != RtvmOk)
        {
            Log(RtvmLogError, "%s would not start\n", Device->Name);
            return false;
        }
    }

    return true;
}

void DeviceHost::ResetAll()
{
    for (RTVM_DEVICE *Device : m_Devices)
    {
        if ((Device->Vtable != nullptr) && (Device->Vtable->Reset != nullptr))
            Device->Vtable->Reset(Device);
    }
}

LegacyPortAdapter *DeviceHost::AdapterFor(RTVM_DEVICE *Device) const
{
    for (ULONG Index = 0; Index < m_Adapters.Count(); Index++)
    {
        if (m_Adapters[Index]->Device() == Device)
            return m_Adapters[Index];
    }

    return nullptr;
}

void DeviceHost::PostInput(RTVM_INPUT_KIND Kind, ULONG Value)
{
    for (RTVM_DEVICE *Device : m_Devices)
    {
        if (RTVM_CARRIES(Device->Vtable, RTVM_DEVICE_VTABLE, Input))
            Device->Vtable->Input(Device, Kind, Value);
    }
}

void DeviceHost::StopAll()
{
    /* Backwards, so that a device is stopped before whatever it leans on */
    for (ULONG Index = m_Devices.Count(); Index > 0; Index--)
    {
        RTVM_DEVICE *Device = m_Devices[Index - 1];

        if ((Device->Vtable != nullptr) && (Device->Vtable->Stop != nullptr))
            Device->Vtable->Stop(Device);
    }
}

} /* namespace rtvm */
