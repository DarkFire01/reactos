/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The manager's end of the virtual device contract
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "rtvmm.h"

namespace rtvm
{

/*
 * Reference counting for something the manager owns outright.
 *
 * Every interface here is a face of the manager, which outlives all of them, so
 * counting is kept only because the contract says the calls are there. Nothing
 * is freed when a count reaches zero, because nothing was allocated to begin
 * with.
 */
class Permanent
{
public:
    ULONG Hold() noexcept { return (ULONG)InterlockedIncrement(&m_Count); }
    ULONG Drop() noexcept { return (ULONG)InterlockedDecrement(&m_Count); }

private:
    volatile LONG m_Count = 1;
};

/*
 * A device written against the older shape of this project, made to look like
 * one written against this one.
 *
 * It exists so that the machine keeps running while the devices are moved over
 * one at a time. Each one that becomes a real device takes its adapter with it,
 * and when the last one has gone so does this.
 */
class LegacyPortAdapter : public IVndIoPortHandler,
                          public IVndMmioHandler,
                          private Permanent
{
public:
    explicit LegacyPortAdapter(RTVM_DEVICE *Device) noexcept : m_Device(Device) {}

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override;
    STDMETHODIMP_(ULONG) AddRef() override { return Hold(); }
    STDMETHODIMP_(ULONG) Release() override { return Drop(); }

    STDMETHODIMP NotifyUnregistered() override { return S_OK; }

    STDMETHODIMP NotifyIoPortRead(USHORT Port, USHORT Width, ULONG *Value) override;
    STDMETHODIMP NotifyIoPortWrite(USHORT Port, USHORT Width, ULONG Value) override;

    STDMETHODIMP NotifyMmioRead(ULONG64 Address, ULONG Length,
                                void *Buffer) override;
    STDMETHODIMP NotifyMmioWrite(ULONG64 Address, ULONG Length,
                                 const void *Buffer) override;

    RTVM_DEVICE *Device() const noexcept { return m_Device; }

private:
    RTVM_DEVICE *m_Device;
};

class VdevHost;

/*
 * What a device is handed for a range it reserved, and gives back to let the
 * range go. One is made for each reservation and lives until the device lets
 * go of it, which is after it has revoked and so after the bus is clear.
 */
class Reservation : public IVndRegistration
{
public:
    Reservation(Bus &On, IVndIoPortHandler *Ports) noexcept
        : m_Bus(On), m_Ports(Ports) {}
    Reservation(Bus &On, IVndMmioHandler *Memory) noexcept
        : m_Bus(On), m_Memory(Memory) {}

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP Revoke() override;

private:
    ~Reservation() = default;

    Bus &m_Bus;
    IVndIoPortHandler *m_Ports = nullptr;
    IVndMmioHandler *m_Memory = nullptr;
    volatile LONG m_Count = 1;
};

/*
 * Where a device asks for the ports and the memory it answers for. Everything
 * registered through it lands on the machine's bus.
 */
class EmulationServices : public IVmAmd64EmulationServices, private Permanent
{
public:
    explicit EmulationServices(VdevHost &Owner) noexcept : m_Owner(Owner) {}

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override;
    STDMETHODIMP_(ULONG) AddRef() override { return Hold(); }
    STDMETHODIMP_(ULONG) Release() override { return Drop(); }

    STDMETHODIMP RegisterMmioHandler(ULONG64 FirstPage, ULONG64 PageCount,
                                     IVndMmioHandler *Handler, BOOL Enabled,
                                     IVndRegistration **Registration) override;
    STDMETHODIMP RegisterMbHandler() override { return E_NOTIMPL; }
    STDMETHODIMP RegisterApicEoiHandler() override { return E_NOTIMPL; }
    STDMETHODIMP RegisterIoPortHandler(USHORT FirstPort, USHORT LastPort,
                                       ULONG Widths, IVndIoPortHandler *Handler,
                                       ULONG Flags,
                                       IVndRegistration **Registration) override;
    STDMETHODIMP RegisterMsrHandler() override { return E_NOTIMPL; }
    STDMETHODIMP RegisterExceptionHandler() override { return E_NOTIMPL; }

private:
    VdevHost &m_Owner;
};

/*
 * The processors, as a device with something for them sees them.
 *
 * The interrupt controller offers a vector and is told, when it next asks,
 * whether it was taken. Nothing here decides whether the guest is willing: that
 * is the machine's to know and it knows it only at the moment it runs.
 */
class ProcessorServices : public IVmProcessorServices, private Permanent
{
public:
    explicit ProcessorServices(VdevHost &Owner) noexcept : m_Owner(Owner) {}

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override;
    STDMETHODIMP_(ULONG) AddRef() override { return Hold(); }
    STDMETHODIMP_(ULONG) Release() override { return Drop(); }

    STDMETHODIMP GetVirtualProcessorCount(ULONG *Count) override;
    STDMETHODIMP SetVirtualProcessorState() override { return E_NOTIMPL; }
    STDMETHODIMP GetVirtualProcessorState() override { return E_NOTIMPL; }

    STDMETHODIMP AssertVirtualProcessorInterrupt(ULONG64 Delivery,
                                                 ULONG64 Reserved,
                                                 ULONG Vector) override;
    STDMETHODIMP ClearVirtualProcessorInterrupt() override;

    STDMETHODIMP ConfigureInterceptThrottlingExclusion() override { return E_NOTIMPL; }
    STDMETHODIMP StopAllVirtualProcessors() override { return E_NOTIMPL; }
    STDMETHODIMP StartAllVirtualProcessors() override { return E_NOTIMPL; }

private:
    VdevHost &m_Owner;
};

/*
 * The guest's memory, for a device that reaches into it rather than waiting to
 * be faulted into. Only the two that read and write it are answered: the rest
 * build the memory a guest has, and that is the manager's to do.
 */
class GuestMemoryAccess : public IVmGuestMemoryAccess,
                          public IRtvmApertureServices,
                          private Permanent
{
public:
    explicit GuestMemoryAccess(VdevHost &Owner) noexcept : m_Owner(Owner) {}

    /* Ours, standing in for the reference call that builds a device's memory */
    STDMETHODIMP CreateAperture(ULONG64 Base, ULONG64 Length,
                                void **Where) override;

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override;
    STDMETHODIMP_(ULONG) AddRef() override { return Hold(); }
    STDMETHODIMP_(ULONG) Release() override { return Drop(); }

    STDMETHODIMP CreateDeviceMemoryBlock() override { return E_NOTIMPL; }
    STDMETHODIMP CreateRamGpaRange() override { return E_NOTIMPL; }
    STDMETHODIMP CreateRamApertureFromByteRange() override { return E_NOTIMPL; }
    STDMETHODIMP CreateSectionBackedGpaRange() override { return E_NOTIMPL; }
    STDMETHODIMP CreateDaxFileBackedGpaRange() override { return E_NOTIMPL; }
    STDMETHODIMP RegisterForVtl2Access() override { return E_NOTIMPL; }

    STDMETHODIMP ReadRamBytes(ULONG64 Address, void *Buffer,
                              ULONG Length) override;
    STDMETHODIMP WriteRamBytes(ULONG64 Address, const void *Buffer,
                               ULONG Length) override;
    STDMETHODIMP ReadRamBytesEx() override { return E_NOTIMPL; }
    STDMETHODIMP WriteRamBytesEx() override { return E_NOTIMPL; }

    STDMETHODIMP CreateNotificationWithHandler() override { return E_NOTIMPL; }
    STDMETHODIMP GetHclErrorPageLocations() override { return E_NOTIMPL; }

    STDMETHODIMP TranslateGvaToGpa(ULONG64 Address, ULONG64 *Physical) override;

    STDMETHODIMP CreateMemoryBlockPageAperture() override { return E_NOTIMPL; }
    STDMETHODIMP DestroyAperture() override { return E_NOTIMPL; }
    STDMETHODIMP RegisterForEmulationOnMemoryWrite() override { return E_NOTIMPL; }
    STDMETHODIMP UnregisterEmulationOnMemoryWrite() override { return E_NOTIMPL; }

private:
    VdevHost &m_Owner;
};

/*
 * How a device reaches everything else. A device names the services it wants
 * and is given them one at a time through here.
 */
class ServiceAccess : public IVmServiceAccess, private Permanent
{
public:
    explicit ServiceAccess(VdevHost &Owner) noexcept : m_Owner(Owner) {}

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override;
    STDMETHODIMP_(ULONG) AddRef() override { return Hold(); }
    STDMETHODIMP_(ULONG) Release() override { return Drop(); }

    STDMETHODIMP GetService(REFIID Service, void **Object) override;

private:
    VdevHost &m_Owner;
};

/* As many device libraries and devices as one machine is ever given */
constexpr ULONG MaximumLibraries = 16;
constexpr ULONG MaximumVdevs = 32;

/*
 * Loads device libraries and brings the devices in them up.
 *
 * A library is a class server: it is asked for a class object by identifier and
 * that class object makes the device. Nothing about a library is reached by
 * name except the one export that hands out class objects, which is what lets
 * a library built elsewhere be loaded here without knowing anything about it.
 */
class VdevHost : public IUnknown, private Permanent
{
public:
    explicit VdevHost(Machine &Owner);
    ~VdevHost();

    VdevHost(const VdevHost &) = delete;
    VdevHost &operator=(const VdevHost &) = delete;

    /* The unknown a device is handed, and asks for its service access through */
    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override;
    STDMETHODIMP_(ULONG) AddRef() override { return Hold(); }
    STDMETHODIMP_(ULONG) Release() override { return Drop(); }

    bool Load(const char *FileName);

    /* Makes one of a kind the loaded libraries offer, and brings it up */
    bool Create(REFCLSID Class, const char *Name,
                const char *Settings = nullptr);

    bool PowerOnAll();
    void PowerOffAll();
    void ResetAll();

    Machine &Owner() noexcept { return m_Machine; }
    EmulationServices &Emulation() noexcept { return m_Emulation; }
    ProcessorServices &Processors() noexcept { return m_Processors; }
    GuestMemoryAccess &GuestMemory() noexcept { return m_Memory; }

    /* The ones every other device leans on, once they have come up */
    IVmPicService *Interrupts() const noexcept { return m_Interrupts; }
    IVmDmaController *Transfers() const noexcept { return m_Transfers; }
    IVmIoApic *Lines() const noexcept { return m_Lines; }
    IVideoVdev *Screen() const noexcept { return m_Screen; }
    ServiceAccess &Services() noexcept { return m_Services; }

    /* Whatever a device asked for that the manager has, or nothing */
    HRESULT FindService(REFIID Service, void **Object);

private:
    struct LoadedVdev
    {
        IVirtualDevice *Device;
        CHAR Name[32];
    };

    Machine &m_Machine;
    EmulationServices m_Emulation;
    ProcessorServices m_Processors;
    GuestMemoryAccess m_Memory;
    ServiceAccess m_Services;

    Array<HMODULE, MaximumLibraries> m_Libraries;
    Array<LoadedVdev, MaximumVdevs> m_Vdevs;

    /* What a device published for other devices to be given */
    Array<IUnknown *, MaximumVdevs> m_Published;
    Array<GUID, MaximumVdevs> m_PublishedAs;

    /* Kept apart because everything that raises a line goes through it */
    IVmPicService *m_Interrupts = nullptr;
    IVmDmaController *m_Transfers = nullptr;
    IVmIoApic *m_Lines = nullptr;
    IVideoVdev *m_Screen = nullptr;
};

} /* namespace rtvm */
